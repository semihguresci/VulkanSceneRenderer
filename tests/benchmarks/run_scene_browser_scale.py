"""Run isolated scene-browser scale cases and preserve timings and memory evidence.

Each executable must support ``<count> --case <name> --verify-large --fixture
<compact|rich>``. Fixtures
run sequentially in separate processes so an allocation failure cannot discard
earlier results or keep another large fixture resident. This is an opt-in
benchmark runner, not a CTest timing gate.
"""

import argparse
import ctypes
from ctypes import wintypes
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import platform
import subprocess
import time


CASES = (
    "hierarchy_expanded",
    "hierarchy_collapsed",
    "hierarchy_panel_closed",
    "bim_empty_query",
    "bim_panel_closed",
    "bim_filtered_query",
)


def utc_now():
    return datetime.now(timezone.utc).isoformat()


class ProcessMemory:
    """OS process peaks plus explicitly sampled private-memory high water marks."""

    def __init__(self, process):
        self.process = process
        self.handle = None
        self.peak_working_set = None
        self.peak_private_commit = None
        self.peak_commit = None
        self.samples = 0
        self.error = None
        self.backend = "unavailable"
        if os.name == "nt":
            class Counters(ctypes.Structure):
                _fields_ = [("cb", wintypes.DWORD),
                            ("PageFaultCount", wintypes.DWORD)] + [
                    (name, ctypes.c_size_t) for name in (
                        "PeakWorkingSetSize", "WorkingSetSize",
                        "QuotaPeakPagedPoolUsage", "QuotaPagedPoolUsage",
                        "QuotaPeakNonPagedPoolUsage", "QuotaNonPagedPoolUsage",
                        "PagefileUsage", "PeakPagefileUsage", "PrivateUsage")]

            self.counters_type = Counters
            self.kernel = ctypes.WinDLL("kernel32", use_last_error=True)
            self.psapi = ctypes.WinDLL("psapi", use_last_error=True)
            self.kernel.OpenProcess.argtypes = [wintypes.DWORD, wintypes.BOOL,
                                               wintypes.DWORD]
            self.kernel.OpenProcess.restype = wintypes.HANDLE
            self.kernel.CloseHandle.argtypes = [wintypes.HANDLE]
            self.kernel.CloseHandle.restype = wintypes.BOOL
            self.psapi.GetProcessMemoryInfo.argtypes = [
                wintypes.HANDLE, ctypes.POINTER(Counters), wintypes.DWORD]
            self.psapi.GetProcessMemoryInfo.restype = wintypes.BOOL
            self.handle = self.kernel.OpenProcess(0x0410, False, process.pid)
            if self.handle:
                self.backend = "Windows GetProcessMemoryInfo"
            else:
                self.error = f"OpenProcess failed: {ctypes.get_last_error()}"
        elif platform.system() == "Linux":
            self.backend = "Linux /proc/<pid>/status (resident memory only)"

    def sample(self):
        if self.handle:
            counters = self.counters_type()
            counters.cb = ctypes.sizeof(counters)
            if self.psapi.GetProcessMemoryInfo(self.handle,
                                              ctypes.byref(counters), counters.cb):
                self.samples += 1
                self.peak_working_set = max(self.peak_working_set or 0,
                                            counters.PeakWorkingSetSize)
                self.peak_private_commit = max(self.peak_private_commit or 0,
                                              counters.PrivateUsage)
                self.peak_commit = max(self.peak_commit or 0,
                                       counters.PeakPagefileUsage)
        elif self.backend.startswith("Linux"):
            try:
                fields = {}
                for line in Path(f"/proc/{self.process.pid}/status").read_text().splitlines():
                    key, _, value = line.partition(":")
                    if key in ("VmHWM", "VmRSS"):
                        fields[key] = int(value.split()[0]) * 1024
                if fields:
                    self.samples += 1
                    self.peak_working_set = max(self.peak_working_set or 0,
                                                *fields.values())
            except (OSError, ValueError):
                pass  # The process may have exited between poll and sample.

    def close(self):
        if self.handle:
            self.kernel.CloseHandle(self.handle)
            self.handle = None

    def result(self):
        return {
            "backend": self.backend,
            "successful_samples": self.samples,
            "peak_working_set_bytes": self.peak_working_set,
            "os_peak_process_commit_bytes": self.peak_commit,
            "sampled_peak_private_commit_bytes": self.peak_private_commit,
            "private_commit_note": (
                "Maximum sampled PrivateUsage; a brief peak between samples may be missed."
                if os.name == "nt" else
                "Private committed memory is unavailable on this platform."),
            "error": self.error,
        }


def system_memory():
    if os.name != "nt":
        return {"backend": "unavailable"}

    class PerformanceInformation(ctypes.Structure):
        _fields_ = [("cb", wintypes.DWORD)] + [
            (name, ctypes.c_size_t) for name in (
                "CommitTotal", "CommitLimit", "CommitPeak", "PhysicalTotal",
                "PhysicalAvailable", "SystemCache", "KernelTotal", "KernelPaged",
                "KernelNonpaged", "PageSize")] + [
            (name, wintypes.DWORD) for name in
            ("HandleCount", "ProcessCount", "ThreadCount")]

    api = ctypes.WinDLL("psapi", use_last_error=True)
    api.GetPerformanceInfo.argtypes = [ctypes.POINTER(PerformanceInformation),
                                      wintypes.DWORD]
    api.GetPerformanceInfo.restype = wintypes.BOOL
    information = PerformanceInformation()
    information.cb = ctypes.sizeof(information)
    if not api.GetPerformanceInfo(ctypes.byref(information), information.cb):
        return {"backend": "Windows GetPerformanceInfo",
                "error": ctypes.get_last_error()}
    page_size = information.PageSize
    return {
        "backend": "Windows GetPerformanceInfo",
        "commit_total_bytes": information.CommitTotal * page_size,
        "commit_limit_bytes": information.CommitLimit * page_size,
        "available_commit_bytes": (information.CommitLimit - information.CommitTotal) * page_size,
        "physical_total_bytes": information.PhysicalTotal * page_size,
        "physical_available_bytes": information.PhysicalAvailable * page_size,
    }


def write_summary(path, summary):
    temporary = path.with_suffix(path.suffix + ".tmp")
    temporary.write_text(json.dumps(summary, indent=2) + "\n", encoding="utf-8")
    temporary.replace(path)


def file_sha256(path):
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def decode_benchmark_result(text, count, case, fixture="compact", variant="after"):
    """Reject stale/wrong matrix cells even if the child returned success."""
    try:
        payload = json.loads(text)
        cases = payload.get("cases", [])
        if (not isinstance(cases, list) or len(cases) != 1 or
                not isinstance(cases[0], dict) or
                cases[0].get("name") != case or
                cases[0].get("elements") != count):
            raise ValueError("Expected exactly the requested case and entity count")
        if payload.get("fixture_profile") != fixture:
            raise ValueError("Benchmark fixture profile differs from the requested fixture")
        allowed_variants = ("before", "before_packed", "before_interning") if variant == "before" else ("after",)
        if payload.get("variant") not in allowed_variants:
            raise ValueError("Benchmark variant differs from the requested executable role")
        legacy = payload["variant"] == "before"
        expected_flags = {
            "verification_requested": True,
            "validation_passed": True,
            "row_interaction_applicable": case in (
                "hierarchy_expanded", "bim_empty_query", "bim_filtered_query"),
            "middle_row_verified": case in ("hierarchy_expanded", "bim_empty_query"),
            "final_row_verified": case in (
                "hierarchy_expanded", "bim_empty_query", "bim_filtered_query"),
            "search_fields_verified": case == "bim_empty_query",
            "cache_reuse_verified": not legacy,
            "unload_reset_verified": case == "bim_empty_query" and not legacy,
        }
        failures = [name for name, expected in expected_flags.items()
                    if cases[0].get(name) is not expected]
        if failures:
            return {"status": "verification_failed", "benchmark": payload,
                    "error": "Requested large-fixture verification flags differ: " +
                    ", ".join(failures)}
        return {"status": "ok", "benchmark": payload}
    except (ValueError, TypeError, AttributeError) as error:
        return {"status": "invalid_json_result", "error": str(error)}


def run_case(executable, variant, count, case, fixture, logs, timeout, interval):
    stem = f"{variant}-{fixture}-{count}-{case}"
    stdout_path = logs / f"{stem}.stdout.json"
    stderr_path = logs / f"{stem}.stderr.log"
    command = [str(executable), str(count), "--case", case, "--verify-large",
               "--fixture", fixture]
    result = {
        "variant": variant, "entity_count": count, "case": case,
        "fixture": fixture,
        "command": command, "started_utc": utc_now(),
        "stdout_path": str(stdout_path), "stderr_path": str(stderr_path),
        "system_memory_before": system_memory(),
    }
    started = time.monotonic()
    process = None
    memory = None
    status = "running"
    try:
        with stdout_path.open("wb") as stdout, stderr_path.open("wb") as stderr:
            process = subprocess.Popen(command, cwd=executable.parent,
                                       stdout=stdout, stderr=stderr)
            memory = ProcessMemory(process)
            while process.poll() is None:
                memory.sample()
                elapsed = time.monotonic() - started
                if elapsed >= timeout:
                    status = "timeout"
                    process.kill()
                    process.wait()
                    break
                time.sleep(min(interval, timeout - elapsed))
    except KeyboardInterrupt:
        status = "interrupted"
        if process is not None and process.poll() is None:
            process.kill()
            process.wait()
    except OSError as error:
        status = "launch_error"
        result["error"] = str(error)
    finally:
        if process is not None and process.poll() is None:
            process.kill()
            process.wait()
        if memory is not None:
            result["process_memory"] = memory.result()
            memory.close()
        result["elapsed_seconds"] = time.monotonic() - started
        result["finished_utc"] = utc_now()
        result["exit_code"] = None if process is None else process.returncode
        result["system_memory_after"] = system_memory()

    stderr_text = (stderr_path.read_text(encoding="utf-8", errors="replace")
                   if stderr_path.is_file() else "")
    exit_code = result["exit_code"]
    exit_code_unsigned = None if exit_code is None else exit_code & 0xFFFFFFFF
    result["exit_code_hex"] = (None if exit_code_unsigned is None else
                               f"0x{exit_code_unsigned:08X}")
    oom_markers = ("bad_alloc", "out of memory", "cannot allocate memory")
    result["allocation_failure_evidence"] = [
        marker for marker in oom_markers if marker in stderr_text.lower()]
    if exit_code_unsigned in (0xC0000017, 0xC000009A, 0xC000012D):
        result["allocation_failure_evidence"].append(
            f"Windows memory/resource NTSTATUS {result['exit_code_hex']}")
    if status == "running":
        status = "ok" if exit_code == 0 else "nonzero_exit"
        if status == "ok":
            try:
                decoded = decode_benchmark_result(
                    stdout_path.read_text(encoding="utf-8"), count, case, fixture, variant)
                status = decoded.pop("status")
                result.update(decoded)
            except (OSError, UnicodeError) as error:
                status = "invalid_json_result"
                result["error"] = str(error)
    result["status"] = status
    return result


def bounded_timeout(value):
    timeout = float(value)
    if not 0.1 <= timeout <= 3600:
        raise argparse.ArgumentTypeError("timeout must be between 0.1 and 3600 seconds")
    return timeout


def entity_count(value):
    count = int(value.replace(",", "").replace("_", ""))
    if not 100 <= count <= 10_000_000:
        raise argparse.ArgumentTypeError("entity count must be between 100 and 10000000")
    return count


def main(argv=None):
    root = Path(__file__).resolve().parents[2]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--exe", type=Path,
                        help="Benchmark executable (required when running)")
    parser.add_argument("--require-opt-in", action="store_true",
                        help="Return 77 unless CONTAINER_RUN_LARGE_SCENE_BROWSER_TESTS=1")
    parser.add_argument("--baseline-exe", type=Path,
                        help="Optional matching-source baseline; must support the same CLI")
    parser.add_argument("--counts", nargs="+", type=entity_count,
                        default=[100_000, 1_000_000, 10_000_000])
    parser.add_argument("--cases", nargs="+", choices=CASES, default=list(CASES))
    parser.add_argument("--fixture", choices=("compact", "rich"), default="compact",
                        help="Metadata text fixture (default: compact)")
    parser.add_argument("--output", type=Path,
                        default=root / "out/scene-browser-validation/scale/results.json")
    parser.add_argument("--timeout", type=bounded_timeout, default=600.0,
                        help="Per-case deadline in seconds, 0.1 to 3600 (default: 600)")
    parser.add_argument("--sample-interval", type=float, default=0.05,
                        help="Process-memory sample interval in seconds (default: 0.05)")
    args = parser.parse_args(argv)
    if (args.require_opt_in and
            os.environ.get("CONTAINER_RUN_LARGE_SCENE_BROWSER_TESTS") != "1"):
        print("Skipped large scene-browser tests; set "
              "CONTAINER_RUN_LARGE_SCENE_BROWSER_TESTS=1 to run.", flush=True)
        return 77
    if args.exe is None:
        parser.error("--exe is required when running benchmarks")
    if not 0.01 <= args.sample_interval <= 5:
        parser.error("--sample-interval must be between 0.01 and 5 seconds")
    variants = [("after", args.exe.resolve())]
    if args.baseline_exe is not None:
        variants.insert(0, ("before", args.baseline_exe.resolve()))
    for _, executable in variants:
        if not executable.is_file():
            parser.error(f"Executable not found: {executable}")
    output = args.output.resolve()
    output.parent.mkdir(parents=True, exist_ok=True)
    logs = output.parent / f"{output.stem}-logs"
    logs.mkdir(parents=True, exist_ok=True)
    summary = {
        "schema_version": 1, "started_utc": utc_now(), "complete": False,
        "platform": platform.platform(), "python_version": platform.python_version(),
        "logical_cpu_count": os.cpu_count(), "counts": args.counts,
        "cases": args.cases, "fixture": args.fixture,
        "timeout_seconds_per_case": args.timeout,
        "memory_sample_interval_seconds": args.sample_interval,
        "isolation": "One case and entity count per sequential child process",
        "verification_requested": True,
        "executables": {variant: {"path": str(path), "sha256":
            file_sha256(path)}
            for variant, path in variants},
        "results": [],
    }
    write_summary(output, summary)
    for count in args.counts:
        for case in args.cases:
            for variant, executable in variants:
                print(f"{variant}: {case}, {count:,} entities", flush=True)
                result = run_case(executable, variant, count, case, args.fixture, logs,
                                  args.timeout, args.sample_interval)
                summary["results"].append(result)
                write_summary(output, summary)
                print(f"  {result['status']} ({result['elapsed_seconds']:.2f} s)", flush=True)
                if result["status"] == "interrupted":
                    return 130
    summary["complete"] = True
    summary["finished_utc"] = utc_now()
    summary["all_passed"] = all(result["status"] == "ok" for result in summary["results"])
    write_summary(output, summary)
    print(f"Saved {output}", flush=True)
    return 0 if summary["all_passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
