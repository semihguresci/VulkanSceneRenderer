#!/usr/bin/env python3
"""Profile probe diagnostics and retain a warmed BIM field across camera motion."""

from __future__ import annotations

import argparse
from pathlib import Path
import hashlib
import math
import os

import numpy as np
from PIL import Image

from bim_leakage_fixtures import write_fixtures
from run_bim_leakage import capture_args, compare_pair, pair_sequence
from run_material_quality import runtime_shader_hashes
from run_performance import statistic
from run_quality import COARSE_GRID, FINE_GRID, capture
from run_runtime import repository_path, require, write_json


TECHNIQUES = ("deferred-raster", "forward-raster")
DEBUG_VIEWS = {"classification": 3, "relocation": 4, "visibility": 2, "visibility-spread": 5}
TIMED_PASSES = ("DdgiTrace", "DdgiUpdate")


def physical_coverage(scene, grid):
    spacing, counts = grid
    origin = scene["volume"]["origin"]
    receiver_margin = {
        receiver["name"]: min(min((position - start) / cell, count - 1 - (position - start) / cell)
                              for position, start, cell, count in zip(receiver["position"], origin, spacing, counts))
        for receiver in scene["receivers"]
    }
    return {
        "spacingMetres": spacing,
        "receiverMarginInCells": receiver_margin,
        "outsideReceivers": [name for name, margin in receiver_margin.items() if margin < -1e-7],
        "wallThicknessInCells": scene["feature"]["wallThicknessMetres"] / spacing[0],
        "gapWidthInCells": scene["feature"]["gapWidthMetres"] / spacing[2],
    }


def profile_frames(frames):
    samples = {name: [] for name in TIMED_PASSES}
    for frame in frames:
        require(frame["ddgi"]["refreshRemainingProbes"] == 0, "Profiling an incomplete fresh sweep")
        passes = {entry["name"]: entry["gpuMs"] for entry in frame["passes"] if entry["gpuTimed"]}
        for name in samples:
            value = passes[name]
            require(math.isfinite(value) and value > 0, f"Missing GPU timing for {name}")
            samples[name].append(value)
    return {name: {"rawGpuMs": values, **statistic(values)} for name, values in samples.items()}


def display_difference(output, left, right):
    a, b = (np.asarray(Image.open(output / frame["png"]).convert("RGB"), dtype=np.int16)
            for frame in (left, right))
    require(a.shape == b.shape, "Diagnostic captures have different resolutions")
    return np.max(np.abs(a - b), axis=2)


def changed_pixels(output, left, right):
    return int(np.count_nonzero(display_difference(output, left, right) > 8))


def retained_state(gi):
    return tuple(gi[key] for key in ("historyResets", "contentEpoch", "totalAllocatedBytes"))


def motion_sequence(camera, warm):
    def key(frame, dx):
        return {"frame": frame, **{name: [point[0] + dx, *point[1:]]
                                  for name, point in camera.items() if name in ("position", "target")}}

    return {"schemaVersion": 1, "frames": warm + 5,
            "sampleFrames": list(range(warm, warm + 6)),
            "events": [{"frame": warm + 1, "ddgiPaused": True},
                       {"frame": warm + 4, "ddgiPaused": False}],
            "camera": [key(1, 0), key(warm + 1, 0), key(warm + 2, .25),
                       key(warm + 3, 0), key(warm + 5, 0)]}


def check_motion(frames, output):
    warm, held, moved, returned, resumed, final = frames
    baseline = warm["ddgi"]
    require(baseline["refreshRemainingProbes"] == 0, "Motion started before initial probe coverage")
    for sample in frames:
        gi = sample["ddgi"]
        require(retained_state(gi) == retained_state(baseline) and gi["refreshRemainingProbes"] == 0,
                "Camera motion or pause/resume invalidated the probe field")
        require(sample["rayShadows"]["generation"] == warm["rayShadows"]["generation"],
                "Camera-only motion rebuilt the ray scene")
    for sample in (held, moved, returned):
        gi = sample["ddgi"]
        require(gi["paused"] and gi["updatedProbes"] == 0 and gi["updateFrame"] == baseline["updateFrame"] and
                gi["schedulingCursor"] == baseline["schedulingCursor"], "Paused history advanced during motion")
    require(abs(moved["camera"]["position"][0] - held["camera"]["position"][0] - .25) < 1e-4 and
            held["camera"] == returned["camera"], "Motion or camera restoration did not apply")
    maximum_difference = int(display_difference(output, held, returned).max())
    require(maximum_difference == 0, "Returning to the fixed camera changed the paused lighting image")
    require(not resumed["ddgi"]["paused"] and not final["ddgi"]["paused"] and
            final["ddgi"]["updateFrame"] == baseline["updateFrame"] + 2, "Probe updates did not resume")
    return {"movedCameraXMetres": .25, "restoredMaximumRgb8Difference": maximum_difference,
            "retainedUpdateFrame": baseline["updateFrame"], "resumedUpdateFrame": final["ddgi"]["updateFrame"]}


def capture_sequence(exe, output, environment, scene, name, sequence, technique, timeout, fine, extra=()):
    sequence_path = output / f"{name}.sequence.json"
    write_json(sequence_path, sequence)
    base = [*capture_args(scene, output, "opaque"), "--fixed-dt", str(1 / 60)]
    record = capture(exe, output, environment, scene, name, sequence["frames"], timeout,
                     technique=technique, coarse=not fine, fine=fine, sequence=sequence_path,
                     extra=(*base, *extra))
    require(record["passed"], f"{name}: {record['failures']}")
    return record


def run_diagnostics(exe, output, environment, scene, technique, warm, timeout, fine):
    name = technique.split("-")[0]
    results = {"technique": technique, "cases": []}

    def run(case, sequence, extra=()):
        record = capture_sequence(exe, output, environment, scene, f"{name}-{case}", sequence,
                                  technique, timeout, fine, extra)
        results["cases"].append(record)
        return record

    timed = {"schemaVersion": 1, "frames": warm, "sampleFrames": [warm - 4, warm - 2, warm]}
    base_frames = run("debug-off", timed)["frames"]
    results["gpuTimings"] = {"off": profile_frames(base_frames)}
    images = {}
    for view, mode in DEBUG_VIEWS.items():
        frames = run(view, timed, ("--ddgi-debug", view))["frames"]
        require(all(frame["ddgi"]["debugView"] == mode for frame in frames), f"{view} did not apply")
        results["gpuTimings"][view] = profile_frames(frames)
        changed = changed_pixels(output, base_frames[-1], frames[-1])
        require(changed > 20, f"{view} did not draw probe markers")
        images[view] = frames[-1]
        results.setdefault("overlayPixelsOverEightCodes", {})[view] = changed
    spread_difference = changed_pixels(output, images["visibility"], images["visibility-spread"])
    require(spread_difference > 20,
            "Visibility mean and standard deviation overlays are indistinguishable")
    results["visibilitySpreadPixelsOverEightCodesFromMean"] = spread_difference

    biases = {"default": (), "receiver-bias": ("--ddgi-normal-bias", ".09", "--ddgi-view-bias", ".03"),
              "visibility-bias": ("--ddgi-visibility-bias", ".15")}
    results["biasSignedIndirectLuminance"] = {}
    grid = "dense" if fine else "coarse"
    for label, flags in biases.items():
        pair = run(label, pair_sequence(warm), flags)
        row = compare_pair(pair, scene, technique, grid, "opaque", warm)
        results["biasSignedIndirectLuminance"][label] = {
            name: receiver["signedIndirectLuminance"] for name, receiver in row["receivers"].items()}
    results["motion"] = check_motion(run("motion", motion_sequence(scene["camera"], warm))["frames"], output)
    return results


def run_unit_sensitivity(exe, output, environment, scenes, warm, timeout):
    results = {}
    technique = "deferred-raster"
    for label, offset in (("on-receiver-wall", 0.), ("off-receiver-wall", -.05)):
        rows = {}
        cases = []
        for scene in scenes:
            origin = list(scene["volume"]["origin"])
            origin[2] += offset
            shifted = {**scene, "volume": {**scene["volume"], "origin": origin}}
            units = scene["sourceUnits"]
            record = capture_sequence(exe, output, environment, shifted, f"{label}-{units}",
                                      pair_sequence(warm), technique, timeout, False)
            cases.append(record)
            rows[units] = compare_pair(record, shifted, technique, "coarse", "opaque", warm)
        results[label] = {"originOffsetZMetres": offset, "cases": cases, "rows": rows,
                          "millimetreMinusMetreLuminance": {
                              name: rows["millimetre"]["receivers"][name]["signedIndirectLuminance"] -
                                    rows["metre"]["receivers"][name]["signedIndirectLuminance"]
                              for name in rows["metre"]["receivers"]}}
    return results


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--exe", required=True)
    parser.add_argument("--output", default="out/ddgi-bim-leakage-diagnostics")
    parser.add_argument("--technique", choices=("both", *TECHNIQUES), default="both")
    parser.add_argument("--grid", choices=("coarse", "dense"), default="coarse")
    parser.add_argument("--warm-frames", type=int, default=48)
    parser.add_argument("--timeout", type=float, default=180)
    args = parser.parse_args()
    exe, output = repository_path(args.exe), repository_path(args.output)
    fine = args.grid == "dense"
    if (not exe.is_file() or not (22 if fine else 8) <= args.warm_frames <= 9995 or
            not math.isfinite(args.timeout) or args.timeout <= 0):
        parser.error("An existing executable, enough warm frames for a fresh sweep, and a positive timeout are required")
    output.mkdir(parents=True, exist_ok=True)
    fixtures = write_fixtures(output)["fixtures"]
    scene = next(row for row in fixtures
                 if row["variant"] == "gap" and row["sourceUnits"] == "metre")
    (output / "vk_layer_settings.txt").write_text("khronos_validation.validate_sync = true\n", encoding="utf-8")
    environment = dict(os.environ, VK_LAYER_SETTINGS_PATH=str(output))
    environment.pop("VK_INSTANCE_LAYERS", None)
    environment.pop("GFXRECON_CAPTURE_FILE", None)
    result = {"schemaVersion": 1, "exeSha256": hashlib.sha256(exe.read_bytes()).hexdigest(),
              "shaderSha256": runtime_shader_hashes(exe), "fixture": scene["name"], "grid": args.grid,
              "coverage": physical_coverage(scene, FINE_GRID if fine else COARSE_GRID), "cases": [], "failures": [],
              "timingScope": "Aggregate trace and probe-update GPU pass samples from validation-enabled captures"}
    techniques = TECHNIQUES if args.technique == "both" else (args.technique,)
    for technique in techniques:
        try:
            result["cases"].append(run_diagnostics(exe, output, environment, scene, technique,
                                                    args.warm_frames, args.timeout, fine))
        except (AssertionError, KeyError, OSError, ValueError) as error:
            result["failures"].append(f"{technique}: {error}")
    if not fine and TECHNIQUES[0] in techniques:
        try:
            result["unitSensitivity"] = run_unit_sensitivity(exe, output, environment,
                [row for row in fixtures if row["variant"] == "open"], args.warm_frames, args.timeout)
        except (AssertionError, KeyError, OSError, ValueError) as error:
            result["failures"].append(f"unit sensitivity: {error}")
    result["passed"] = not result["failures"]
    write_json(output / "bim-leakage-diagnostics.json", result)
    for failure in result["failures"]:
        print(failure, flush=True)
    return 0 if result["passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
