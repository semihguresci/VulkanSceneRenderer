# Bounded Draco decoding worker

`GltfDracoWorker.h` exposes the decoder-free client. The official pinned Draco
library is linked only into `ContainerDracoWorker.exe`, beside the application.
An explicit absolute executable path supports private tests; launch never uses a
shell or searches `PATH`. Omitting the optional worker reports unavailable
decoding. Platforms without an equivalent enforced launcher reject bounded
Draco decoding explicitly; there is no in-process fallback.

On Windows the client creates a hidden, suspended child, assigns a Job Object,
then resumes it. The job limits process committed memory, CPU time and the
active process count to one, and terminates children when closed. A wall timeout
terminates the child and retires its handles. The worker independently verifies
the applied job limits before reading its request. Windows documents the
committed-memory limit in
[JOBOBJECT_EXTENDED_LIMIT_INFORMATION](https://learn.microsoft.com/en-us/windows/win32/api/winnt/ns-winnt-jobobject_extended_limit_information).

These controls bound decoder resource use. A Job Object does not change the
user's security token and is not a security sandbox for arbitrary executable
code. The bundled executable is trusted; the compressed bitstream is untrusted.

## API and budgets

`DecodeDracoInWorker` takes compressed bytes, requested unique attribute IDs and
component counts, `DracoDecodeLimits`, and optional worker/temp-root paths.
Finite input/output/count/process-memory/time limits are mandatory. Defaults are
64 MiB compressed input, 256 MiB complete output, 8 million points/faces,
512 MiB process committed memory, and a 30-second wall/CPU limit. The glTF
orchestrator supplies tighter declared point/face/output limits and checks exact
accessor counts before publishing decoded buffers.

The result owns uint32 triangle indices and densely mapped per-point attribute
bytes, with each attribute's unique ID, scalar type, component count and
normalized flag. Face indices remain Draco point indices. The worker resolves
`mapped_index()` for each point and uses raw `GetMappedValue`, preserving exact
integers such as 16,777,217 and uint32 maximum. It verifies tight raw strides and
buffer bounds first: Draco's raw getter copies the entire attribute byte stride.
The caller validates the declared glTF representation and performs any numeric
conversion. The worker applies no coordinate conversion.

## Protocol version 1

All integers and raw scalar components are little-endian. Requests have a
72-byte header, 8 bytes per requested attribute, then compressed bytes. Responses
have a 48-byte header, uint32 triangle indices, then a 24-byte header and dense
raw values per attribute. `maxOutputBytes` includes every header and index.
Failure responses contain at most 1 KiB of diagnostic text within that budget.
Version 1 accepts at most 1,024 requested attributes with 1–16 components each;
requests exceeding those explicit limits fail.

The parent checks the response file length before allocating arrays. It streams
directly into final arrays, validates scalar types, component counts, unique IDs,
normalized flags, exact raw lengths, index ranges, and trailing bytes. It does
not allocate a second complete response buffer. A failed decode, timeout or
invalid response throws before returning a result, so the caller can preserve
the current model.

Each call creates an owned temporary child directory containing `request.bin`
and `response.bin`. Cleanup retains/checks the directory identity and refuses to
traverse a changed directory or reparse point. It removes only these known files
and the empty directory, or unlinks a redirected directory link itself. It never
recursively deletes the temporary root.

## Validation

`gltf_draco_worker_tests` checks protocol corruption/truncation, response budgets,
exact typed values, hidden/job-limited launch, actual memory-allocation denial,
wall timeout, abnormal exit, oversized/malformed output and temporary cleanup.
A test-only probe attempts to redirect the owned directory to a sibling junction
target; victim files must survive. Official encoder fixtures test mapped RGBA and
exact unsigned attributes, malformed streams and count/output/attribute limits.
The compression orchestration suite separately validates decoded glTF accessors
and model-level failure behavior.
