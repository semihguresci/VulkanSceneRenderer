#!/usr/bin/env python3
"""Native one-shot DDGI scene-fitting checks; run after building the renderer.

Twelve small captures exercise Cornell import scales and both raster techniques,
equivalent glTF/BIM/USD triangle scenes, disabled/capability fallback, and an
explicit live refit after moving an architectural partition. This checks setup
and lifetime contracts, not lighting quality or performance. Parser-only cases
verify conflicting manual settings fail before Vulkan initialization.

Outputs default to out/ddgi-volume-fit-validation and do not replace prior DDGI
captures. Relative paths resolve against the repository, not the caller's cwd.
"""
from __future__ import annotations

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

from run_runtime import REPO_ROOT, VALIDATION_ERROR, repository_path, require, write_json


@dataclass(frozen=True)
class FitCase:
    name: str
    scene: Path
    bounds: tuple
    technique: str = "deferred-raster"
    scale: float = 1.0
    probe_budget: int = 512
    default_budget: bool = False
    enabled: bool = True
    unsupported: bool = False
    frames: int = 6
    sequence: Path | None = None
    moved_bounds: tuple | None = None
    memory_mb: int = 64


def identity():
    return [[float(row == column) for column in range(4)] for row in range(4)]


def multiply(a, b):
    return [[sum(a[row][k] * b[k][column] for k in range(4))
             for column in range(4)] for row in range(4)]


def node_matrix(node):
    if "matrix" in node:
        # glTF arrays store matrix columns, independently of Python row lists.
        return [[node["matrix"][column * 4 + row] for column in range(4)] for row in range(4)]
    x, y, z, w = node.get("rotation", [0, 0, 0, 1])
    sx, sy, sz = node.get("scale", [1, 1, 1])
    tx, ty, tz = node.get("translation", [0, 0, 0])
    return [[(1 - 2 * (y * y + z * z)) * sx, 2 * (x * y - z * w) * sy,
             2 * (x * z + y * w) * sz, tx],
            [2 * (x * y + z * w) * sx, (1 - 2 * (x * x + z * z)) * sy,
             2 * (y * z - x * w) * sz, ty],
            [2 * (x * z - y * w) * sx, 2 * (y * z + x * w) * sy,
             (1 - 2 * (x * x + y * y)) * sz, tz], [0, 0, 0, 1]]


def gltf_triangle_bounds(path, scale=1.0, moved_node=None, translation=None):
    """Independent source bounds from active nodes and referenced triangle vertices.

    The deterministic fixtures use dense float32 POSITION accessors. Reject
    unsupported layouts instead of quietly using authored accessor min/max.
    """
    path = Path(path)
    scene = json.loads(path.read_text(encoding="utf-8"))
    buffers = []
    for buffer in scene["buffers"]:
        uri = buffer["uri"]
        buffers.append(base64.b64decode(uri.split(",", 1)[1]) if uri.startswith("data:")
                       else (path.parent / uri).read_bytes())

    def accessor(index):
        value = scene["accessors"][index]
        require("sparse" not in value, "fit fixture checker does not support sparse accessors")
        view = scene["bufferViews"][value["bufferView"]]
        components = {"SCALAR": 1, "VEC3": 3}[value["type"]]
        code = {5121: "B", 5123: "H", 5125: "I", 5126: "f"}[value["componentType"]]
        fmt = "<" + code * components
        stride = view.get("byteStride", struct.calcsize(fmt))
        offset = view.get("byteOffset", 0) + value.get("byteOffset", 0)
        return [struct.unpack_from(fmt, buffers[view["buffer"]], offset + item * stride)
                for item in range(value["count"])]

    points = []

    def visit(index, parent):
        node = dict(scene["nodes"][index])
        if moved_node is not None and node.get("name") == moved_node:
            require("matrix" not in node, "moved fit fixture must use node TRS")
            node["translation"] = translation
        world = multiply(parent, node_matrix(node))
        if "mesh" in node:
            for primitive in scene["meshes"][node["mesh"]]["primitives"]:
                if primitive.get("mode", 4) != 4:
                    continue
                position_accessor = scene["accessors"][primitive["attributes"]["POSITION"]]
                require(position_accessor["componentType"] == 5126 and position_accessor["type"] == "VEC3",
                        "fit fixture POSITION must be dense float32 VEC3")
                positions = accessor(primitive["attributes"]["POSITION"])
                indices = [item[0] for item in accessor(primitive["indices"])] if "indices" in primitive else range(len(positions))
                require(len(indices) % 3 == 0, "fit fixture triangle indices must be complete")
                for vertex in set(indices):
                    require(0 <= vertex < len(positions), "fit fixture index out of bounds")
                    point = (*positions[vertex], 1)
                    points.append([scale * sum(world[row][column] * point[column] for column in range(4))
                                   for row in range(3)])
        for child in node.get("children", []):
            visit(child, world)

    for root in scene["scenes"][scene.get("scene", 0)]["nodes"]:
        visit(root, identity())
    require(points, "fit fixture has no referenced triangle positions")
    return (tuple(min(point[axis] for point in points) for axis in range(3)),
            tuple(max(point[axis] for point in points) for axis in range(3)))


def write_live_fixture(output):
    # Reuse the existing deterministic architectural generator, not its manual
    # probe volume. Translating the partition beyond the shell changes bounds.
    from quality_fixtures import build_scene, gltf_scene
    scene = output / "architecture-fit-doorway.gltf"
    write_json(scene, gltf_scene(build_scene("doorway", width=400, height=360)))
    sequence = output / "volume-fit-sequence.json"
    write_json(sequence, {"schemaVersion": 1, "frames": 8,
        "sampleFrames": [1, 3, 4, 5, 6, 8], "objectName": "Partition",
        "objectTranslation": [{"frame": 1, "translation": [0, 0, 0]},
                              {"frame": 3, "translation": [0, 0, 0]},
                              {"frame": 4, "translation": [5, 0, 0]},
                              {"frame": 8, "translation": [5, 0, 0]}],
        # JSON Schema integers include exact integral floats; exercise the
        # runtime parser as well as the integer budgets supplied by the CLI.
        "events": [{"frame": 4, "ddgiFitProbes": 512.0}]})
    return scene, sequence


def make_cases(output):
    cornell = REPO_ROOT / "models/validation/cornell_box_local_light.gltf"
    cases = []
    for scale in (.001, 1.0, 1000.0):
        for technique in ("deferred-raster", "forward-raster"):
            default = scale == 1 and technique == "deferred-raster"
            tight_memory = scale == 1 and technique == "forward-raster"
            cases.append(FitCase(f"fit-cornell-{technique.split('-')[0]}-scale-{scale:g}",
                cornell, gltf_triangle_bounds(cornell, scale), technique, scale,
                2048 if default else 64 if tight_memory else 512,
                default_budget=default, memory_mb=1 if tight_memory else 64))
    equivalent = REPO_ROOT / "models/validation/taa_equivalent.gltf"
    equivalent_bounds = gltf_triangle_bounds(equivalent)
    for extension in ("gltf", "bim", "usda"):
        # These three existing fixtures author the same world-space triangles.
        cases.append(FitCase(f"fit-equivalent-{extension}", equivalent.with_suffix("." + extension),
                             equivalent_bounds))
    cornell_bounds = gltf_triangle_bounds(cornell)
    cases.append(FitCase("fit-no-ray-query", cornell, cornell_bounds, unsupported=True))
    cases.append(FitCase("fit-disabled", cornell, cornell_bounds, enabled=False))
    scene, sequence = write_live_fixture(output)
    cases.append(FitCase("fit-live-partition", scene, gltf_triangle_bounds(scene),
                         frames=8, sequence=sequence,
                         moved_bounds=gltf_triangle_bounds(scene, moved_node="Partition", translation=[5, 0, 0])))
    return cases


def command_for(case, exe, output):
    command = [str(exe), "--model", str(case.scene), "--import-scale", str(case.scale),
        "--width", "400", "--height", "360", "--hidden", "--no-ui", "--validation",
        "--no-bloom", "--no-taa", "--msaa", "1", "--display-mode", "lit",
        "--render-technique", case.technique, "--exposure", ".04",
        "--environment-intensity", "0", "--directional-intensity", "0",
        "--ray-shadows", "raster", "--ddgi" if case.enabled else "--no-ddgi",
        "--ddgi-fit-scene", "--ddgi-rays", "64", "--ddgi-probes-per-frame", "256",
        "--ddgi-ray-budget", "16384", "--screenshot", str(output / f"{case.name}.png"),
        "--warmup-frames", str(case.frames - 1), "--capture-frame", str(case.frames)]
    if not case.default_budget:
        command += ["--ddgi-fit-probes", str(case.probe_budget)]
    if case.memory_mb != 64:
        command += ["--ddgi-memory-mb", str(case.memory_mb)]
    if case.unsupported:
        command += ["--no-ray-query"]
    if case.sequence:
        command += ["--capture-sequence", str(case.sequence)]
    return command


def capture_paths(case, output):
    frames = json.loads(case.sequence.read_text(encoding="utf-8"))["sampleFrames"] if case.sequence else [case.frames]
    for frame in frames:
        stem = case.name if frame == case.frames else f"{case.name}.frame-{frame:04d}"
        yield frame, output / f"{stem}.png", output / f"{stem}.telemetry.json"


def finite_vector(value, label):
    require(isinstance(value, list) and len(value) == 3 and
            all(isinstance(item, (int, float)) and not isinstance(item, bool) and math.isfinite(item) for item in value),
            f"{label} must contain three finite numbers")
    return value


def check_fit(gi, expected_bounds, budget, enabled, unsupported=False,
              payload_budget=64 * 1024 * 1024):
    require(gi["enabled"] is enabled, "fitting changed the requested enabled state")
    fit, settings = gi["sceneFit"], gi["settings"]
    require(fit["applied"] is True, "fit did not apply: " + str(fit.get("status")))
    require(isinstance(fit["status"], str) and bool(fit["status"].strip()), "fit status is missing")
    require(fit["probeBudget"] == budget, "incorrect requested probe budget")
    low, high = expected_bounds
    tolerance = max(1e-9, math.dist(low, high) * 2e-6)
    for reported, expected in ((finite_vector(fit["boundsMin"], "boundsMin"), low),
                               (finite_vector(fit["boundsMax"], "boundsMax"), high)):
        require(all(abs(a - b) <= tolerance for a, b in zip(reported, expected)),
                f"fit source world bounds differ: {reported} != {expected}")
    origin = finite_vector(settings["origin"], "origin")
    spacing = finite_vector(settings["spacing"], "spacing")
    require(all(value > 0 for value in spacing), "fit spacing must remain positive")
    require(all(math.isclose(value, spacing[0], rel_tol=1e-6) for value in spacing),
            "fit spacing is not isotropic")
    counts = settings["counts"]
    require(isinstance(counts, list) and len(counts) == 3 and
            all(isinstance(value, int) and not isinstance(value, bool) and value >= 3 for value in counts),
            "fitted probe counts must be integer dimensions >=3")
    count = math.prod(counts)
    require(27 <= count <= budget and fit["probeCount"] == count, "fitted probe count exceeds cap or differs from telemetry")
    maximum = [origin[axis] + spacing[axis] * (counts[axis] - 1) for axis in range(3)]
    require(all(origin[axis] <= low[axis] + tolerance and maximum[axis] >= high[axis] - tolerance for axis in range(3)),
            "fitted volume does not cover current source bounds")
    require(all(origin[axis] < low[axis] and maximum[axis] > high[axis] for axis in range(3)),
            "fitted volume is missing outward padding")
    diagonal = math.dist(origin, maximum)
    require(math.isfinite(settings["maxDistance"]) and settings["maxDistance"] >= diagonal * (1 - 2e-6),
            "fit trace distance does not cover padded volume diagonal")
    for key, ratio in (("normalBias", .06), ("viewBias", .02), ("visibilityBias", .10)):
        require(math.isclose(settings[key], spacing[0] * ratio, rel_tol=2e-6, abs_tol=spacing[0] * 1e-8),
                f"fit did not scale {key} with its cell size")
    require(math.isclose(gi["debugProbeRadius"], spacing[0] * .12, rel_tol=2e-6, abs_tol=spacing[0] * 1e-8),
            "fit did not scale probe diagnostic radius")
    require(isinstance(fit["payloadBytes"], int) and count * 48 <= fit["payloadBytes"] <= payload_budget,
            "fit CPU payload estimate is invalid or exceeds configured budget")
    if unsupported or not enabled:
        require(gi["active"] is False, "unsupported/disabled fitted field became active")
        require(all(gi[key] == 0 for key in ("updateFrame", "updatedProbes", "tracedRays")),
                "unsupported/disabled fitted field scheduled updates")
        if unsupported:
            require(gi["supported"] is False, "no-ray-query capability fallback failed")
    else:
        require(gi["supported"] is True and gi["active"] is True, "fitted GI is inactive: " + str(gi["status"]))
        require(gi["updatedProbes"] > 0 and gi["tracedRays"] == gi["updatedProbes"] * 64,
                "fitted field did not schedule complete bounded ray sets")
    return {"probeCount": count, "spacing": spacing[0], "paddedMin": origin,
            "paddedMax": maximum, "payloadBytes": fit["payloadBytes"]}


def volume_key(gi):
    settings = gi["settings"]
    return tuple(tuple(settings[key]) for key in ("origin", "spacing", "counts"))


def check_frames(case, frames):
    require(bool(frames), "capture has no frames")
    reports = []
    if not case.sequence:
        for sample in frames:
            reports.append(check_fit(sample["ddgi"], case.bounds, case.probe_budget,
                                     case.enabled, case.unsupported,
                                     case.memory_mb * 1024 * 1024))
            if case.memory_mb == 1:
                require(sample["ddgi"]["sceneFit"]["probeCount"] == 64,
                        "tight-memory 3D fit did not use the minimum padded 4x4x4 volume")
        return reports
    by_frame = {sample["frame"]: sample["ddgi"] for sample in frames}
    require({1, 3, 4, 5, 6, 8} <= set(by_frame), "live fit checkpoints are missing")
    initial = volume_key(by_frame[1])
    require(volume_key(by_frame[3]) == initial,
            "one-shot fitted volume changed without a new request")
    require(volume_key(by_frame[4]) != initial, "same-frame live refit kept the stale volume")
    for frame in (1, 3):
        reports.append(check_fit(by_frame[frame], case.bounds, case.probe_budget, True))
    for frame in (4, 5, 6, 8):
        reports.append(check_fit(by_frame[frame], case.moved_bounds, case.probe_budget, True))
    require(by_frame[4]["updateFrame"] == 1 and by_frame[4]["firstProbe"] == 0,
            "changed live-fit lattice did not restart newly allocated history")
    for frame in (5, 6, 8):
        require(volume_key(by_frame[frame]) == volume_key(by_frame[4]), "fit repeated without a request")
        require(by_frame[frame]["historyResets"] == by_frame[4]["historyResets"],
                "stable scene repeatedly reset history after live fitting")
        require(by_frame[frame]["updateFrame"] == frame - 3, "live-fit history did not advance after its single rebuild")
    return reports


def decoded(value):
    return value.decode("utf-8", errors="replace") if isinstance(value, bytes) else value or ""


def capture(case, exe, output, environment, timeout):
    command = command_for(case, exe, output)
    record = {"name": case.name, "command": command, "commandLine": subprocess.list2cmdline(command),
              "cwd": str(exe.parent), "log": str(output / f"{case.name}.log"), "frames": [], "failures": []}
    paths = list(capture_paths(case, output))
    for _, png, telemetry in paths:
        png.unlink(missing_ok=True)
        telemetry.unlink(missing_ok=True)
    started = time.monotonic()
    try:
        run = subprocess.run(command, cwd=exe.parent, env=environment, capture_output=True,
                             text=True, encoding="utf-8", errors="replace", timeout=timeout)
        log, code = run.stdout + run.stderr, run.returncode
    except subprocess.TimeoutExpired as error:
        log, code = decoded(error.stdout) + decoded(error.stderr), None
        record["failures"].append(f"renderer timed out after {timeout} seconds")
    Path(record["log"]).write_text(log, encoding="utf-8")
    record.update(returnCode=code, durationSeconds=round(time.monotonic() - started, 3),
                  validationErrors=[line for line in log.splitlines() if VALIDATION_ERROR.search(line)])
    if code != 0 or record["validationErrors"]:
        record["failures"].append("renderer failed or logged validation/runtime errors")
    for frame, png, telemetry_path in paths:
        try:
            require(png.is_file() and png.read_bytes()[:8] == b"\x89PNG\r\n\x1a\n", f"frame {frame}: missing/invalid PNG")
            telemetry = json.loads(telemetry_path.read_text(encoding="utf-8"))
            record["frames"].append({"frame": frame, "png": png.name, "gpu": telemetry.get("gpu"),
                                     "technique": telemetry.get("technique"), "ddgi": telemetry["ddgi"]})
        except (OSError, ValueError, KeyError, AssertionError) as error:
            record["failures"].append(str(error))
    if len(record["frames"]) == len(paths):
        try:
            record["fitChecks"] = check_frames(case, record["frames"])
        except (AssertionError, KeyError, TypeError, ValueError) as error:
            record["failures"].append(str(error))
    record["passed"] = not record["failures"]
    print(json.dumps({"case": case.name, "passed": record["passed"], "failures": record["failures"]}), flush=True)
    return record


def invalid_cli_cases():
    manual = (("origin", ("--ddgi-origin", "0", "0", "0")),
              ("spacing", ("--ddgi-spacing", "1", "1", "1")),
              ("counts", ("--ddgi-counts", "3", "3", "3")),
              ("distance", ("--ddgi-max-distance", "20")),
              ("normal-bias", ("--ddgi-normal-bias", ".03")),
              ("view-bias", ("--ddgi-view-bias", ".01")),
              ("visibility-bias", ("--ddgi-visibility-bias", ".05")),
              ("radius", ("--ddgi-probe-radius", ".06")))
    cases = [("fit-conflict-" + name, ("--ddgi-fit-scene", *flags), "cannot be combined") for name, flags in manual]
    return cases + [("fit-conflict-origin-reversed", ("--ddgi-origin", "0", "0", "0", "--ddgi-fit-scene"), "cannot be combined"),
                    ("fit-budget-too-small", ("--ddgi-fit-scene", "--ddgi-fit-probes", "26"), "at least 27"),
                    ("fit-budget-without-fit", ("--ddgi-fit-probes", "512"), "requires --ddgi-fit-scene")]


VULKAN_STARTED = re.compile(r"Vulkan 1\.\d|Initializing Vulkan renderer|Ray queries:|\[HDR\]", re.IGNORECASE)


def check_cli_failure(code, log, expected):
    require(code is not None and code != 0, "invalid fit options did not fail")
    require(expected in log, "invalid fit options did not report the expected configuration error")
    require(not VULKAN_STARTED.search(log), "invalid fit options initialized Vulkan before failing")
    require(not re.search(r"VUID-|SYNC-HAZARD", log), "parser-only rejection logged Vulkan validation errors")


def capture_cli_failure(name, flags, expected, exe, output, environment, timeout):
    command = [str(exe), "--hidden", "--no-ui", *flags]
    record = {"name": name, "command": command, "expectedError": expected, "failures": []}
    try:
        run = subprocess.run(command, cwd=exe.parent, env=environment, capture_output=True,
                             text=True, encoding="utf-8", errors="replace", timeout=timeout)
        log, code = run.stdout + run.stderr, run.returncode
    except subprocess.TimeoutExpired as error:
        log, code = decoded(error.stdout) + decoded(error.stderr), None
    (output / f"{name}.log").write_text(log, encoding="utf-8")
    record["returnCode"] = code
    try:
        check_cli_failure(code, log, expected)
    except AssertionError as error:
        record["failures"].append(str(error))
    record["passed"] = not record["failures"]
    print(json.dumps({"case": name, "passed": record["passed"], "failures": record["failures"]}), flush=True)
    return record


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--exe", default="out/build/visual-studio/Release/VulkanSceneRenderer.exe")
    parser.add_argument("--output", default="out/ddgi-volume-fit-validation")
    parser.add_argument("--timeout", type=float, default=180)
    parser.add_argument("--parser-only", action="store_true")
    args = parser.parse_args()
    exe, output = repository_path(args.exe), repository_path(args.output)
    if not exe.is_file():
        parser.error(f"renderer executable does not exist: {exe}")
    if not math.isfinite(args.timeout) or args.timeout <= 0:
        parser.error("--timeout must be finite and positive")
    output.mkdir(parents=True, exist_ok=True)
    (output / "vk_layer_settings.txt").write_text("khronos_validation.validate_sync = true\n", encoding="utf-8")
    environment = os.environ.copy()
    environment["VK_LAYER_SETTINGS_PATH"] = str(output)
    results = {"schemaVersion": 1, "startedUtc": datetime.now(timezone.utc).isoformat(),
               "exe": str(exe), "exeSha256": hashlib.sha256(exe.read_bytes()).hexdigest(),
               "environmentOverrides": {"VK_LAYER_SETTINGS_PATH": str(output)},
               "nativeCases": [], "parserCases": []}
    path = output / "volume-fit-results.json"
    for name, flags, expected in invalid_cli_cases():
        results["parserCases"].append(capture_cli_failure(name, flags, expected, exe, output, environment, args.timeout))
        write_json(path, results)
    if not args.parser_only:
        for case in make_cases(output):
            results["nativeCases"].append(capture(case, exe, output, environment, args.timeout))
            write_json(path, results)
    results["passed"] = all(case["passed"] for case in results["parserCases"] + results["nativeCases"])
    results["finishedUtc"] = datetime.now(timezone.utc).isoformat()
    write_json(path, results)
    print(f"{'PASS' if results['passed'] else 'FAIL'}: {len(results['nativeCases'])} native + {len(results['parserCases'])} parser cases; {path}")
    return 0 if results["passed"] else 1


if __name__ == "__main__":
    sys.exit(main())
