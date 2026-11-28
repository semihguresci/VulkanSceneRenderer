"""Collect Release DDGI timings on the adapter identified by the matrix probe."""

from dataclasses import asdict
from datetime import datetime, timezone
import hashlib
import json
import math
from pathlib import Path
import platform

import run_performance as perf


_ADAPTER_IDS = ("vendorId", "deviceId")
_ADAPTER_FIELDS = (*_ADAPTER_IDS, "gpu", "driverVersion", "vulkanApiVersion")
_LAYER_OVERRIDES = ("VK_INSTANCE_LAYERS", "VK_LOADER_LAYERS_ENABLE", "VK_LAYER_SETTINGS_PATH")
_DRIVER_OVERRIDES = ("VK_DRIVER_FILES", "VK_ICD_FILENAMES", "VK_ADD_DRIVER_FILES", "VK_LOADER_DRIVERS_SELECT",
                     "VK_LOADER_DRIVERS_DISABLE", "VK_LOADER_DEVICE_SELECT", "MESA_VK_DEVICE_SELECT", "DRI_PRIME")


def _now():
    return datetime.now(timezone.utc).isoformat()


def _sha256(path):
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def _inputs(exe, scene):
    directory = exe.parent / "spv_shaders"
    shaders = {path.relative_to(directory).as_posix(): _sha256(path)
               for path in sorted(directory.rglob("*.spv")) if path.is_file()}
    if not shaders:
        raise ValueError(f"Missing staged Vulkan shaders at {directory}")
    return {"exe": str(exe), "exeSha256": _sha256(exe), "scene": str(scene), "sceneSha256": _sha256(scene),
            "shaderDirectory": str(directory), "shaderSha256": shaders}


def _verify_capture(path, expected, previous):
    try:
        frame = json.loads(path.read_bytes())
        if frame["validationEnabled"] is not False:
            raise ValueError("renderer enabled Vulkan validation in the timing lane")
        build = frame["build"]
        if build["configuration"] != "Release":
            raise ValueError(f"expected a Release renderer, got {build['configuration']!r}")
        device = {field: frame[field] for field in _ADAPTER_FIELDS}
        for field, requested in expected.items():
            actual = device[field]
            if type(actual) is not type(requested) or actual != requested:
                raise ValueError(f"adapter {field} is {actual!r}, expected {requested!r}")
        identity = {"adapter": device, "build": build}
        if previous is not None and previous != identity:
            raise ValueError("adapter, driver, or build identity changed between timing captures")
        return identity
    except (KeyError, OSError, TypeError, ValueError) as error:
        raise ValueError(f"{path}: {error}") from error


def run_timing(exe: Path, scene: Path, output: Path, environment: dict, expected_adapter: dict,
               *, timeout: float = 300, config: perf.Config | None = None) -> dict:
    """Run the warmed performance sweep and return its reports and capture evidence."""
    config = perf.Config() if config is None else config
    perf.validate_config(config)
    if config.validation is not False:
        raise ValueError("The Release timing lane requires validation disabled")
    if not math.isfinite(timeout) or timeout <= 0:
        raise ValueError("The timing timeout must be positive and finite")
    if any(type(expected_adapter.get(field)) is not int for field in _ADAPTER_IDS):
        raise ValueError("Timing requires a startup-proven Vulkan vendorId and deviceId")
    expected = {field: expected_adapter[field] for field in _ADAPTER_FIELDS
                if expected_adapter.get(field) is not None}

    exe, scene = (Path(path).resolve(strict=True) for path in (exe, scene))
    inputs = _inputs(exe, scene)
    output = Path(output).resolve()
    output.mkdir(parents=True)
    env = dict(environment)
    for key in _LAYER_OVERRIDES:
        env.pop(key, None)

    raw_path = output / "performance-results.json"
    summary_path = output / "timing-summary.json"
    cases = perf.make_cases(perf.TECHNIQUES, config)
    report = {
        "schemaVersion": 1,
        "startedUtc": _now(),
        **inputs,
        "host": platform.platform(),
        "expectedAdapter": expected,
        "config": asdict(config),
        "fixedTimestepSeconds": 1 / 60,
        "sampling": {
            "probeCount": perf.PROBE_COUNT,
            "raysPerProbe": perf.RAYS_PER_PROBE,
            "probesPerFrameByMode": perf.BUDGETS,
            "rayRotationSeed": "deterministic shader hash of probe index and DDGI update frame",
        },
        "percentileMethod": "linear interpolation at (sampleCount-1)*0.95",
        "measurementClass": "repeated native capture GPU pass timing",
        "sustainedSampling": config.repeats >= 3 and config.samples >= 90,
        "limitations": perf.LIMITATIONS,
        "driverOverrides": {key: env[key] for key in _DRIVER_OVERRIDES if key in env},
        "cases": [],
        "captures": [],
        "failures": [],
    }
    identity = None
    for case in cases:
        try:
            record = perf.capture(case, config, exe, scene, output, env, timeout)
        except (AssertionError, KeyError, OSError, TypeError, ValueError) as error:
            report["failures"].append(f"{case.name}: {error}")
            break
        captures = [path for _, _, path in perf.paths_for(case, config, output) if path.is_file()]
        record["captureTelemetry"] = [str(path) for path in captures]
        report["captures"].extend(record["captureTelemetry"])
        report["cases"].append(record)
        try:
            for path in captures:
                identity = _verify_capture(path, expected, identity)
        except ValueError as error:
            record["failures"].append(str(error))
            record["passed"] = False
        if not record["passed"]:
            report["failures"].extend(f"{case.name}: {failure}" for failure in record["failures"])
        perf.write_json(raw_path, report)
        if not record["passed"]:
            break

    report["observedIdentity"] = identity
    try:
        if _inputs(exe, scene) != inputs:
            raise ValueError("Executable, staged shaders, or scene changed during timing")
        report["modeSummaries"] = perf.mode_summaries(report["cases"])
        report["lightingRouteComparisons"] = perf.matched_comparisons(report["cases"])
        report["lightingRouteDeltaMs"] = {
            technique: perf.statistic(values)
            for technique in perf.TECHNIQUES
            if (values := [pair["lightingRouteDeltaMs"] for pair in report["lightingRouteComparisons"]
                           if pair["technique"] == technique])
        }
    except (AssertionError, KeyError, OSError, TypeError, ValueError) as error:
        report["failures"].append(str(error))
    passed = len(report["cases"]) == len(cases) and identity is not None and not report["failures"]
    report.update(passed=passed, finishedUtc=_now())
    perf.write_json(raw_path, report)

    summary = {key: report[key] for key in (
        "schemaVersion", "startedUtc", "finishedUtc", "config", "expectedAdapter", "observedIdentity",
        "measurementClass", "sustainedSampling", "limitations", "captures", "failures", "passed")}
    for key in ("modeSummaries", "lightingRouteComparisons", "lightingRouteDeltaMs"):
        summary[key] = report.get(key, [] if key != "lightingRouteDeltaMs" else {})
    summary.update(rawSamples=str(raw_path), summary=str(summary_path), completedCases=len(report["cases"]),
                   plannedCases=len(cases))
    perf.write_json(summary_path, summary)
    return summary
