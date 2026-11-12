# Local renderer MCP

The renderer can accept Model Context Protocol requests while its viewport is
running. Set a loopback port and a bearer token before launching. For example,
from PowerShell in the repository directory:

```powershell
$env:CONTAINER_MCP_PORT = '8765'
$env:CONTAINER_MCP_TOKEN = python -c "import secrets; print(secrets.token_hex(24))"
$env:CONTAINER_MCP_ROOT = (Get-Location).Path
& .\out\build\feature-parity\VulkanSceneRenderer.exe
```

Point a Streamable HTTP MCP client at `http://127.0.0.1:8765/mcp` and configure
its `Authorization` header as `Bearer <token>`. The server accepts one JSON-RPC
message per HTTP POST and returns JSON directly. Use
`Accept: application/json, text/event-stream` and
`Content-Type: application/json`. Protocol negotiation supports 2025-11-25,
2025-06-18 and 2025-03-26.

The port is off by default. The listener binds to IPv4 loopback, requires its
token and rejects browser Origin headers. The application reads its endpoint
configuration at startup. Configure CMake with `-DENABLE_RENDERER_MCP=OFF` to
build the renderer without the listener. Scripted screenshot, capture-sequence
and path sample series launches keep the endpoint disabled.
[MCP transport specification](https://modelcontextprotocol.io/specification/2025-11-25/basic/transports)

| Tool | Operation |
| --- | --- |
| `renderer_get_settings` | Read the active technique and lighting settings. |
| `renderer_set_lighting` | Set environment and directional intensity in `[0,16]`, bounce intensity in `[0,2]`, or environment yaw in `[-180,180]` degrees. |
| `renderer_get_status` | Read the active model and model, environment, capture and shader reload status. |
| `renderer_recent_logs` | Read up to 32 recent renderer and Vulkan events. |
| `renderer_load_model` | Queue a self-contained glTF or GLB and an optional import scale. |
| `renderer_load_environment` | Queue an HDR or EXR environment. |
| `renderer_reload_shaders` | Queue the existing asynchronous Slang shader reload. |
| `renderer_capture` | Queue a PNG or scene-linear EXR with an optional viewport-only PNG crop. |
| `renderer_list_gpu_timers` | List pass names from the last retired GPU recording. |
| `renderer_start_gpu_measurement` | Start a bounded measurement for one of those pass names. |
| `renderer_poll_gpu_measurement` | Check progress or retrieve completed statistics by ID. |
| `renderer_cancel_gpu_measurement` | Cancel that measurement by ID. |

File tools require `CONTAINER_MCP_ROOT`. Input files and capture destinations
resolve beneath that directory. Model imports accept embedded buffers and
data URIs within a 512 MiB input budget. Capture requests require an existing
directory and unused image and sidecar names. Each capture file is published
atomically with exclusive destination creation.
Queued operations report their progress separately through
`renderer_get_status`. Lighting edits update the same runtime settings and
scene-default policy used by the editor.

Lighting ranges come from the same metadata used by the editor sliders, CLI
parser and lighting manager. Every supplied value must be finite and within its
inclusive range. A request with any invalid field rejects before any lighting
change is queued. The CLI applies these checks to `--environment-intensity`,
`--directional-intensity`, `--bounce-intensity` and `--environment-yaw`.
CLI values seed the session. Accepted editor and MCP edits apply to subsequent
rendered frames and persist when a different model is loaded. Scene presets
continue to set fields left at their automatic defaults.

Recent-log capture starts when the authenticated listener is active. The
64-entry ring keeps the timestamp, source, severity and a fixed event
description for each entry. File paths and original log payloads stay in the
application's ordinary console log.

## Measurements

Start a session with a pass name and optional `warmupFrames`, `samples` and
`timeoutMs`. Defaults are 8 warmup frames, 30 samples and 10 seconds. The maximum
is 240 warmup frames, 240 samples and 30 seconds. Each session uses previously
unseen retired recording serials after warmup. Polling the endpoint cannot
duplicate a frame. A pass that does not report a timing in a later frame adds
no sample.

Completed sessions report the count, mean, median, minimum, maximum and
population standard deviation in milliseconds. These measure the selected
Vulkan pass under the current scene and device workload. Keep the viewport
settings stable during the measurement. A session can time out if the renderer
stops submitting frames or its timer becomes unavailable.

The network worker validates messages and gives each command a two-second
response deadline. The render thread accepts commands at its frame-retirement
point. A request that expires before acceptance reports that the renderer did
not accept it. If the renderer has already accepted a command, the timeout
reports acceptance and the queued operation can finish after the connection
closes. A connection that closes before acceptance releases the pending slot.
Measurements have explicit cancel and timeout states and retain their last
result until another session starts.

Run the external regression against a configured Release build:

```powershell
python -B tests/validation/renderer_mcp_runtime_regression.py --exe out/build/feature-parity/VulkanSceneRenderer.exe --output out/mcp-regression --shader-reload
```
