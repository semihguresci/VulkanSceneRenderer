#!/usr/bin/env python3
"""Native DDGI integration checks; requires a built Vulkan renderer and ray-query GPU.

Run from any directory: python path/to/tests/visual-regression/ddgi/run_runtime.py
Relative --exe/--output paths resolve against the repository, not the caller's cwd.
The default eleven cases exercise both raster techniques, disabled/paused GI,
dynamic epochs, continuous motion, optional surface-budget isolation and a
one-probe update budget. All sequence frames are captured and checked.

--probe-memory-fallback adds a device-dependent 1 MiB allocation test: its CPU
payload fits, but driver/VMA image alignment must push actual allocations over
the budget. A device that fits these allocations will fail that optional check.
--surface-validation-only runs two optional surface-validation fallback cases
using a Cornell node with an invertible scale whose normal transform exceeds
float range. Raster GI must disable its AS; hard ray shadows retain compact AS.
--debug-only checks all four probe overlays in both raster techniques, compares
them with matched GI-only captures, and checks paused/unsupported diagnostics.
Logs, generated inputs, PNG/telemetry captures and runtime-results.json remain
under --output; no golden images or build files are modified.
"""

import argparse
import base64
from dataclasses import dataclass
from datetime import datetime, timezone
import hashlib
import json
import math
import os
from pathlib import Path
import re
import struct
import subprocess
import sys
import time


REPO_ROOT = Path(__file__).resolve().parents[3]
VALIDATION_ERROR = re.compile(
    r"VUID-|SYNC-HAZARD|\[error\]|\[critical\]|\bValidation Error\b|\bException\b",
    re.IGNORECASE,
)
DDGI_KEYS = (
    "supported", "enabled", "active", "status", "contentEpoch", "historyResets",
    "updateFrame", "updatedProbes", "tracedRays", "firstProbe", "nextProbe",
    "refreshRemainingProbes", "totalAllocatedBytes",
    "paused", "debugView", "debugProbeRadius",
)


@dataclass(frozen=True)
class Case:
    name: str
    technique: str = "deferred-raster"
    extra: tuple = ()
    frame_count: int = 40
    sequence: Path = None
    scene: Path = None
    kind: str = "active"
    probe_count: int = 343
    probes_per_frame: int = 128


def repository_path(value):
    path = Path(value).expanduser()
    return (path if path.is_absolute() else REPO_ROOT / path).resolve()


def write_json(path, value):
    path.write_text(json.dumps(value, indent=2) + "\n", encoding="utf-8")


def write_inputs(output):
    dynamic = output / "dynamic-sequence.json"
    write_json(dynamic, {
        "schemaVersion": 1, "frames": 60, "sampleFrames": list(range(1, 61)),
        "camera": [
            {"frame": 1, "position": [0, 1, 3.5], "target": [0, 1, 0]},
            {"frame": 20, "position": [0.1, 1, 3.5], "target": [0, 1, 0]},
        ],
        "objectNode": 0,
        "objectTranslation": [
            {"frame": 1, "translation": [0, 0, 0]},
            {"frame": 41, "translation": [0, 0, 0]},
            {"frame": 42, "translation": [0.05, 0, 0]},
            {"frame": 60, "translation": [0.05, 0, 0]},
        ],
        "events": [{"frame": 21, "areaLightPosition": [0.55, 1.7, 0.18]}],
    })
    continuous = output / "continuous-sequence.json"
    write_json(continuous, {
        "schemaVersion": 1, "frames": 30, "sampleFrames": list(range(1, 31)),
        "objectNode": 0,
        "objectTranslation": [
            {"frame": 1, "translation": [0, 0, 0]},
            {"frame": 16, "translation": [0.04, 0, 0]},
            {"frame": 30, "translation": [0.04, 0, 0]},
        ],
    })
    one_probe = output / "one-probe-sequence.json"
    write_json(one_probe, {
        "schemaVersion": 1, "frames": 4, "sampleFrames": [1, 2, 3, 4],
    })
    # Repeated indices overflow 1 MiB of optional surface metadata while the
    # compact opaque shadow scene remains cheap and the screen footprint tiny.
    surface = output / "large_surface.gltf"
    vertices = struct.pack("<9f", -0.005, 0.995, 0, 0.005, 0.995, 0, 0, 1.005, 0)
    indices = struct.pack("<3H", 0, 1, 2) * 5500
    payload = vertices + indices
    write_json(surface, {
        "asset": {"version": "2.0"},
        "buffers": [{"byteLength": len(payload), "uri":
                     "data:application/octet-stream;base64," +
                     base64.b64encode(payload).decode("ascii")}],
        "bufferViews": [
            {"buffer": 0, "byteOffset": 0, "byteLength": len(vertices), "target": 34962},
            {"buffer": 0, "byteOffset": len(vertices), "byteLength": len(indices), "target": 34963},
        ],
        "accessors": [
            {"bufferView": 0, "componentType": 5126, "count": 3, "type": "VEC3",
             "min": [-0.005, 0.995, 0], "max": [0.005, 1.005, 0]},
            {"bufferView": 1, "componentType": 5123, "count": 16500, "type": "SCALAR"},
        ],
        "meshes": [{"primitives": [{"attributes": {"POSITION": 0}, "indices": 1}]}],
        "nodes": [{"mesh": 0}], "scenes": [{"nodes": [0]}], "scene": 0,
    })
    return dynamic, continuous, one_probe, surface


def make_cases(inputs, include_memory):
    dynamic, continuous, one_probe, surface = inputs
    cases = [
        Case(f"cornell-{technique.split('-')[0]}-gi{intensity}", technique,
             ("--ddgi-intensity", intensity))
        for technique in ("deferred-raster", "forward-raster")
        for intensity in ("0", "1")
    ]
    cases += [
        Case("cornell-no-ray-query", extra=("--no-ray-query",), kind="unsupported"),
        Case("cornell-paused", extra=("--ddgi-ray-budget", "0"), kind="paused"),
        Case("cornell-dynamic", extra=("--ddgi-counts", "7", "8", "7", "--taa"),
             frame_count=60, sequence=dynamic, kind="dynamic", probe_count=392),
        Case("cornell-continuous", extra=("--ddgi-counts", "7", "8", "7", "--taa"),
             frame_count=30, sequence=continuous, kind="continuous", probe_count=392),
        Case("surface-budget-raster", extra=("--ddgi-surface-memory-mb", "1"),
             frame_count=4, scene=surface, kind="surface-raster"),
        Case("surface-budget-hard", extra=("--ddgi-surface-memory-mb", "1",
                                          "--ray-shadows", "hard"),
             frame_count=4, scene=surface, kind="surface-hard"),
        Case("cornell-one-probe-budget", extra=("--ddgi-probes-per-frame", "1",
                                               "--ddgi-ray-budget", "128"),
             frame_count=4, sequence=one_probe, probes_per_frame=1),
    ]
    if include_memory:
        # 125 probes / 64 updates have a CPU payload of 945,008 bytes. Actual
        # image allocations include device-dependent padding/alignment.
        cases.append(Case("cornell-probe-memory-fallback", extra=(
            "--ddgi-counts", "5", "5", "5", "--ddgi-probes-per-frame", "64",
            "--ddgi-ray-budget", "8192", "--ddgi-memory-mb", "1"),
            frame_count=4, kind="memory", probe_count=125, probes_per_frame=64))
    return cases


def surface_validation_cases(output):
    # The importer sanitizes nonfinite authored attributes, so use a finite,
    # affine and invertible instance transform that is valid for visibility
    # but cannot represent its inverse-transpose normal transform in float.
    scene = json.loads((REPO_ROOT / "models/validation/cornell_box_local_light.gltf").read_text(
        encoding="utf-8"))
    scene["nodes"][0]["scale"] = [1e-39, 1, 1]
    path = output / "invalid_normal_transform.gltf"
    write_json(path, scene)
    return [
        Case("surface-validation-raster", frame_count=4, scene=path,
             kind="invalid-raster"),
        Case("surface-validation-hard", extra=("--ray-shadows", "hard"),
             frame_count=4, scene=path, kind="invalid-hard"),
    ]


def debug_cases():
    cases = []
    for technique in ("deferred-raster", "forward-raster"):
        prefix = technique.split("-")[0]
        cases.append(Case(f"debug-{prefix}-off", technique, frame_count=6))
        for view in ("irradiance", "visibility", "classification", "relocation"):
            cases.append(Case(f"debug-{prefix}-{view}", technique,
                              ("--ddgi-debug", view), frame_count=6))
    cases += [
        Case("debug-paused", extra=("--ddgi-debug", "classification", "--ddgi-pause"),
             frame_count=4, kind="paused"),
        Case("debug-unsupported", extra=("--ddgi-debug", "classification", "--no-ray-query"),
             frame_count=4, kind="unsupported"),
    ]
    return cases


def check_debug_images(output):
    # Matched frame counts/settings make the overlay the only image difference.
    # This catches missing forward/deferred hooks without relying on goldens or
    # display-space brightness thresholds for irradiance.
    from PIL import Image
    import numpy as np
    comparisons = []
    for technique in ("deferred", "forward"):
        baseline = np.asarray(Image.open(output / f"debug-{technique}-off.png").convert("RGB"),
                              dtype=np.int16)
        for view in ("irradiance", "visibility", "classification", "relocation"):
            overlay = np.asarray(Image.open(output / f"debug-{technique}-{view}.png").convert("RGB"),
                                 dtype=np.int16)
            changed = int(np.count_nonzero(np.max(np.abs(overlay - baseline), axis=2) > 8))
            require(changed > 20, f"{technique} {view}: probe overlay did not change the image")
            comparisons.append({"technique": technique, "view": view, "changedPixels": changed})
    return comparisons


def command_for(case, exe, output):
    scene = case.scene or REPO_ROOT / "models/validation/cornell_box_local_light.gltf"
    command = [str(exe), "--model", str(scene),
        "--width", "400", "--height", "360", "--hidden", "--no-ui", "--validation",
        "--no-bloom", "--no-taa", "--msaa", "1", "--render-technique", case.technique,
        "--display-mode", "lit", "--camera-position", "0", "1", "3.5",
        "--camera-target", "0", "1", "0", "--camera-fov", "50", "--exposure", ".04",
        "--environment-intensity", "0", "--directional-intensity", "0", "--ddgi",
        "--ddgi-origin", "-1.05", "-.1", "-1.05", "--ddgi-spacing", ".35", ".35", ".35",
        "--ddgi-counts", "7", "7", "7", "--ddgi-rays", "128",
        "--ddgi-probes-per-frame", "128", "--ddgi-ray-budget", "16384",
        "--ray-shadows", "raster", "--area-lighting", "sampled", "--area-light-samples", "25",
        "--screenshot", str(output / f"{case.name}.png"),
        "--warmup-frames", str(case.frame_count - 1), "--capture-frame", str(case.frame_count),
        *case.extra]
    if case.sequence:
        command += ["--capture-sequence", str(case.sequence)]
    return command


def capture_paths(case, output):
    frames = range(1, case.frame_count + 1) if case.sequence else [case.frame_count]
    for frame in frames:
        stem = case.name if frame == case.frame_count else f"{case.name}.frame-{frame:04d}"
        yield frame, output / f"{stem}.png", output / f"{stem}.telemetry.json"


def compact_frame(frame, telemetry):
    result = {
        "frame": frame, "gpu": telemetry.get("gpu"), "technique": telemetry.get("technique"),
        "ddgi": {key: telemetry.get("ddgi", {}).get(key) for key in DDGI_KEYS},
        "rayShadows": {key: telemetry.get("rayShadows", {}).get(key)
                       for key in ("supported", "active", "mode", "generation", "historyResets", "build")},
        "taa": {key: telemetry.get("taa", {}).get(key)
                for key in ("enabled", "epoch", "submittedFrame", "resetReason")},
        "cameraPosition": telemetry.get("camera", {}).get("position"),
    }
    if "selectedProbes" in telemetry.get("ddgi", {}):
        result["ddgi"]["selectedProbes"] = telemetry["ddgi"]["selectedProbes"]
    return result


def require(condition, message):
    if not condition:
        raise AssertionError(message)


def validate_selected_probes(gi, probe_count, prefix=""):
    """Validate packed priority telemetry; absent arrays retain legacy checks."""
    if "selectedProbes" not in gi:
        return False
    selected = gi["selectedProbes"]
    require(isinstance(selected, list), prefix + "selectedProbes must be an array")
    require(len(selected) == gi["updatedProbes"], prefix + "selected probe count differs from update budget")
    require(all(type(index) is int and 0 <= index < probe_count for index in selected),
            prefix + "selected probe index outside packed volume")
    require(len(set(selected)) == len(selected), prefix + "duplicate selected probe")
    if selected:
        require(0 <= gi["firstProbe"] < probe_count and 0 <= gi["nextProbe"] < probe_count,
                prefix + "priority scheduling cursor outside packed volume")
        require(selected[0] == gi["firstProbe"], prefix + "first selected probe differs from fairness cursor")
    return True


def check_frames(case, frames):
    previous_cursor = None
    for sample in frames:
        frame, gi = sample["frame"], sample["ddgi"]
        prefix = f"frame {frame}: "
        require(gi["enabled"] is True, prefix + "DDGI was not enabled")
        priority = validate_selected_probes(gi, case.probe_count, prefix)
        if case.kind in ("unsupported", "paused", "surface-raster", "surface-hard", "memory",
                         "invalid-raster", "invalid-hard"):
            for key in ("updatedProbes", "tracedRays", "updateFrame", "nextProbe"):
                require(gi[key] == 0, prefix + f"{key} must remain zero: {gi[key]}")
            if case.kind == "paused":
                require(gi["active"] is True, prefix + "paused volume should remain active")
                require("paused" in gi["status"], prefix + "missing paused status")
            else:
                require(gi["active"] is False, prefix + "fallback GI must be inactive")
            if case.kind == "unsupported":
                require(gi["supported"] is False, prefix + "--no-ray-query did not disable support")
            elif case.kind.startswith("surface"):
                require("surface budget" in gi["status"], prefix + "missing surface-budget fallback reason")
                ray = sample["rayShadows"]
                require(ray["active"] == (case.kind == "surface-hard"),
                        prefix + "surface-budget failure changed the requested shadow route")
                if case.kind == "surface-hard":
                    require(ray["mode"] == 1 and ray["generation"] > 0,
                            prefix + "hard ray shadows did not retain their AS")
                    require(ray["build"] and ray["build"]["instances"] == 1,
                            prefix + "compact shadow scene is missing")
            elif case.kind.startswith("invalid"):
                require(gi["supported"] is True, prefix + "ray queries must remain supported")
                require("normal transform must be finite" in gi["status"],
                        prefix + "missing optional surface-validation fallback reason")
                require(gi["firstProbe"] == 0, prefix + "fallback must not schedule probe updates")
                ray = sample["rayShadows"]
                hard = case.kind == "invalid-hard"
                require(ray["active"] is hard and ray["mode"] == int(hard),
                        prefix + "surface-validation failure changed the requested shadow route")
                require(ray["generation"] == int(hard),
                        prefix + "surface-validation failure rebuilt or retained an unexpected AS")
                if hard:
                    require(ray["build"] and ray["build"]["instances"] == 11,
                            prefix + "compact Cornell shadow scene is missing")
            elif case.kind == "memory":
                require("actual VMA allocations exceed memory budget" in gi["status"],
                        prefix + "device did not exceed the optional 1 MiB actual allocation budget")
            continue
        require(gi["supported"] is True and gi["active"] is True,
                prefix + f"GI did not activate: {gi['status']}")
        expected_first = ((frame - 1) * case.probes_per_frame) % case.probe_count
        expected_next = (frame * case.probes_per_frame) % case.probe_count
        checks = [("updatedProbes", case.probes_per_frame),
                  ("tracedRays", case.probes_per_frame * 128), ("updateFrame", frame)]
        if not priority:
            checks += [("firstProbe", expected_first), ("nextProbe", expected_next)]
        elif previous_cursor is not None:
            require(gi["firstProbe"] == previous_cursor, prefix + "fairness cursor restarted between submitted frames")
        for key, expected in checks:
            require(gi[key] == expected, prefix + f"{key}: expected {expected}, got {gi[key]}")
        previous_cursor = gi["nextProbe"]
        expected_epoch = (1 if frame <= 20 else 2 if frame <= 41 else 3) if case.kind == "dynamic" else (
            min(frame, 16) if case.kind == "continuous" else 1)
        require(gi["contentEpoch"] == expected_epoch,
                prefix + f"contentEpoch: expected {expected_epoch}, got {gi['contentEpoch']}")
        if case.kind in ("dynamic", "continuous"):
            require(sample["taa"]["enabled"] is True, prefix + "TAA was not enabled")
            require(sample["taa"]["submittedFrame"] == frame, prefix + "incorrect TAA submitted frame")
            expected_generation = (1 if frame <= 41 else 2) if case.kind == "dynamic" else min(frame, 16)
            require(sample["rayShadows"]["generation"] == expected_generation,
                    prefix + "unexpected AS rebuild for constant camera/object keyframes")
            if priority:
                fresh_frames = math.ceil(case.probe_count / math.ceil(case.probes_per_frame / 2))
                last_change = (1 if frame <= 20 else 21 if frame <= 41 else 42) if case.kind == "dynamic" else min(frame, 16)
                if frame >= last_change + fresh_frames - 1:
                    require(gi["refreshRemainingProbes"] == 0,
                            prefix + "priority scheduler exceeded its fair fresh-sweep bound")
    if case.kind == "dynamic":
        # Allow a deliberate lighting reset at frame 21 while asserting that
        # camera motion and the object transform itself do not reset TAA.
        for group in (frames[:20], frames[40:]):
            require(len({sample["taa"]["epoch"] for sample in group}) == 1,
                    "TAA epoch changed during camera/object motion")
        require(frames[-1]["ddgi"]["refreshRemainingProbes"] == 0,
                "dynamic scene never completed its fresh probe sweep")
    if case.kind == "continuous":
        require(len({sample["taa"]["epoch"] for sample in frames}) == 1,
                "TAA epoch changed during continuous object motion")
        require(frames[-1]["ddgi"]["refreshRemainingProbes"] == 0,
                "continuous motion starved probes after motion stopped")


def capture(case, exe, output, environment, timeout):
    command = command_for(case, exe, output)
    record = {"name": case.name, "command": command,
              "commandLine": subprocess.list2cmdline(command), "cwd": str(exe.parent),
              "log": str(output / f"{case.name}.log"), "frames": [], "failures": []}
    # Remove only this case's exact outputs so an old capture cannot pass a new
    # run that failed before writing its screenshot or telemetry.
    paths = list(capture_paths(case, output))
    for _, png, telemetry_path in paths:
        for path in (png, telemetry_path):
            path.unlink(missing_ok=True)
    started = time.monotonic()
    try:
        run = subprocess.run(command, cwd=exe.parent, env=environment,
                             capture_output=True, text=True, encoding="utf-8",
                             errors="replace", timeout=timeout)
        log = run.stdout + run.stderr
        record["returnCode"] = run.returncode
    except subprocess.TimeoutExpired as error:
        def decoded(value):
            return value.decode("utf-8", errors="replace") if isinstance(value, bytes) else value or ""
        log = decoded(error.stdout) + decoded(error.stderr)
        record["returnCode"] = None
        record["failures"].append(f"renderer timed out after {timeout} seconds")
    Path(record["log"]).write_text(log, encoding="utf-8")
    record["durationSeconds"] = round(time.monotonic() - started, 3)
    record["validationErrors"] = [line for line in log.splitlines() if VALIDATION_ERROR.search(line)]
    if record["returnCode"] != 0:
        record["failures"].append(f"renderer exit code {record['returnCode']}")
    if record["validationErrors"]:
        record["failures"].append("renderer logged validation/runtime errors")
    for frame, png, telemetry_path in paths:
        try:
            require(png.is_file() and png.read_bytes()[:8] == b"\x89PNG\r\n\x1a\n",
                    f"frame {frame}: missing/invalid PNG {png}")
            telemetry = json.loads(telemetry_path.read_text(encoding="utf-8"))
            record["frames"].append(compact_frame(frame, telemetry))
        except (OSError, ValueError, AssertionError) as error:
            record["failures"].append(str(error))
    if len(record["frames"]) == len(paths):
        try:
            check_frames(case, record["frames"])
        except (AssertionError, KeyError, TypeError) as error:
            record["failures"].append(str(error))
    record["passed"] = not record["failures"]
    print(json.dumps({"case": case.name, "passed": record["passed"],
                      "failures": record["failures"],
                      "ddgi": record["frames"][-1]["ddgi"] if record["frames"] else None}), flush=True)
    return record


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--exe", default="out/build/visual-studio/Release/VulkanSceneRenderer.exe")
    parser.add_argument("--output", default="out/ddgi-validation")
    parser.add_argument("--timeout", type=float, default=180, help="maximum seconds per renderer case (default: 180)")
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument("--probe-memory-fallback", action="store_true",
                      help="add the driver-alignment-dependent 1 MiB actual-allocation fallback case")
    mode.add_argument("--surface-validation-only", action="store_true",
                      help="run only the two optional surface-validation Raster/Hard fallback cases")
    mode.add_argument("--debug-only", action="store_true",
                      help="run probe diagnostic views in both raster techniques")
    args = parser.parse_args()
    exe, output = repository_path(args.exe), repository_path(args.output)
    if not exe.is_file():
        parser.error(f"renderer executable does not exist: {exe}")
    if not (args.timeout > 0):
        parser.error("--timeout must be positive")
    output.mkdir(parents=True, exist_ok=True)
    (output / "vk_layer_settings.txt").write_text(
        "khronos_validation.validate_sync = true\n", encoding="utf-8")
    environment = os.environ.copy()
    environment["VK_LAYER_SETTINGS_PATH"] = str(output)
    cases = (debug_cases() if args.debug_only else surface_validation_cases(output)
             if args.surface_validation_only else
             make_cases(write_inputs(output), args.probe_memory_fallback))
    results_path = output / "runtime-results.json"
    results = {"schemaVersion": 1, "startedUtc": datetime.now(timezone.utc).isoformat(),
               "exe": str(exe), "exeSha256": hashlib.sha256(exe.read_bytes()).hexdigest(),
               "environmentOverrides": {"VK_LAYER_SETTINGS_PATH": str(output)},
               "probeMemoryFallbackIsDeviceDependent": args.probe_memory_fallback,
               "surfaceValidationOnly": args.surface_validation_only,
               "debugOnly": args.debug_only,
               "cases": []}
    for case in cases:
        results["cases"].append(capture(case, exe, output, environment, args.timeout))
        results["passed"] = all(record["passed"] for record in results["cases"])
        write_json(results_path, results)
    results["finishedUtc"] = datetime.now(timezone.utc).isoformat()
    if args.debug_only and results["passed"]:
        try:
            results["imageComparisons"] = check_debug_images(output)
        except (AssertionError, ImportError) as error:
            results["passed"] = False
            results["imageComparisonFailure"] = str(error)
    write_json(results_path, results)
    print(f"{'PASS' if results['passed'] else 'FAIL'}: {len(results['cases'])} cases; {results_path}")
    return 0 if results["passed"] else 1


if __name__ == "__main__":
    sys.exit(main())
