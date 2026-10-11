#!/usr/bin/env python3
"""Measure fine room-volume leakage reduction over a coarse architectural grid.

The sealed partition is a closed 3 cm, single-sided solid. These checks make no
zero-leak claim for open sheets or double-sided BIM topology. Native captures
use fixed exposure and matched open-room controls in both raster techniques.
"""
from __future__ import annotations

import argparse
import copy
from datetime import datetime, timezone
import hashlib
import json
import math
import os

from quality_fixtures import write_fixtures
from run_quality import capture
from run_runtime import repository_path, require, validate_selected_probes, write_json

COARSE = {"origin": [-3., 0., -2.], "spacing": [.75, .75, .75], "counts": [9, 5, 5]}
DETAIL = {"origin": [-.25, -.25, -2.25], "spacing": [.25, .25, .25], "counts": [15, 15, 15]}
BLEND_CELLS = 1.
TECHNIQUES = ("deferred-raster", "forward-raster")
ROOM_MINIMUM = [.015, 0., -2.]
ROOM_MAXIMUM = [3., 2.8, 1.]
MIN_COARSE_LEAK_RATIO = .005
MAX_DETAIL_LEAK_RATIO = .01
MAX_RETAINED_LEAK_FRACTION = .5
MIN_LIT_SIDE_RETAINED_FRACTION = .8


def check_detail_padding(detail=DETAIL, minimum=ROOM_MINIMUM, maximum=ROOM_MAXIMUM,
                         blend_cells=BLEND_CELLS):
    require(blend_cells >= 0 and math.isfinite(blend_cells), "Invalid blend band")
    for axis in range(3):
        spacing = detail["spacing"][axis]
        endpoint = detail["origin"][axis] + spacing * (detail["counts"][axis] - 1)
        require(minimum[axis] - detail["origin"][axis] >= spacing * blend_cells - 1e-12 and
                endpoint - maximum[axis] >= spacing * blend_cells - 1e-12,
                "Detail room volume must pad all receiver surfaces beyond its blend band")


def detail_preset(seed):
    result = copy.deepcopy(seed)
    result["settings"]["additionalVolumes"] = [copy.deepcopy(DETAIL)]
    result["settings"]["volumeBlendCells"] = BLEND_CELLS
    return result


def prepared_scene(scene):
    result = copy.deepcopy(scene)
    result["volume"] = {**copy.deepcopy(COARSE), "maxDistance": 12.}
    return result


def check_capture(record, detail):
    require(record["passed"] and bool(record["frames"]),
            record["name"] + ": missing passing native capture")
    gi = record["frames"][-1]["ddgi"]
    require(gi["active"] and gi["supported"], "Probe field inactive")
    settings = gi["settings"]
    for key in ("origin", "spacing", "counts"):
        require(settings[key] == COARSE[key], "Coarse primary lattice changed")
    require(settings["additionalVolumes"] == ([DETAIL] if detail else []), "Detail volume did not apply")
    count = math.prod(COARSE["counts"]) + (math.prod(DETAIL["counts"]) if detail else 0)
    require(validate_selected_probes(gi, count), "Native priority telemetry is missing")
    require(gi["updatedProbes"] == min(count, 256) and
            gi["tracedRays"] == gi["updatedProbes"] * 128, "Combined frame budget changed")
    require(gi["refreshRemainingProbes"] == 0, "Initial combined fresh sweep did not finish")
    require(gi["totalAllocatedBytes"] <= 64 * 1024 * 1024, "Combined allocation exceeds payload ceiling")


def compare(records):
    by_name = {record["name"]: record["frames"][-1]["receivers"]
               for record in records if record["passed"] and record["frames"]}
    result = {}
    for technique in TECHNIQUES:
        names = [f"{technique}-{scene}-{mode}" for scene in ("sealed-thin", "open")
                 for mode in ("coarse", "detail")]
        require(all(name in by_name for name in names), "Missing matched volume captures")
        coarse, detail = (by_name[f"{technique}-sealed-thin-{mode}"] for mode in ("coarse", "detail"))
        opened, open_detail = (by_name[f"{technique}-open-{mode}"] for mode in ("coarse", "detail"))
        control = opened["shaded-back"]["estimatedLuminance"]
        require(control > .005, "Open control receiver is not illuminated")
        require(open_detail["shaded-back"]["estimatedLuminance"] >= control * .5,
                "Detail field suppressed legitimate open-room illumination")
        coarse_ratio = coarse["shaded-back"]["estimatedLuminance"] / control
        detail_ratio = detail["shaded-back"]["estimatedLuminance"] / control
        require(coarse_ratio > MIN_COARSE_LEAK_RATIO, "Coarse control has no measurable leakage to improve")
        require(detail_ratio <= MAX_DETAIL_LEAK_RATIO and
                detail_ratio <= coarse_ratio * MAX_RETAINED_LEAK_FRACTION,
                "Fine room volume did not measurably reduce coarse-grid leakage")
        lit = coarse["lit-back"]["estimatedLuminance"]
        require(lit > .005 and detail["lit-back"]["estimatedLuminance"] >= lit * MIN_LIT_SIDE_RETAINED_FRACTION,
                "Leakage reduction also darkened the legitimately lit chamber")
        result[technique] = {
            "openControlLuminance": control, "coarseLeakRatio": coarse_ratio,
            "detailLeakRatio": detail_ratio, "leakReductionFraction": 1. - detail_ratio / coarse_ratio,
            "litSideRetainedFraction": detail["lit-back"]["estimatedLuminance"] / lit,
            "limits": {"maximumDetailLeakRatio": MAX_DETAIL_LEAK_RATIO,
                       "minimumLeakReductionFraction": 1. - MAX_RETAINED_LEAK_FRACTION,
                       "minimumLitSideRetainedFraction": MIN_LIT_SIDE_RETAINED_FRACTION}}
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--exe", default="out/build/visual-studio/Release/VulkanSceneRenderer.exe")
    parser.add_argument("--output", default="out/ddgi-features-validation/volume-quality")
    parser.add_argument("--frames", type=int, default=160)
    parser.add_argument("--timeout", type=float, default=180)
    args = parser.parse_args()
    exe, output = repository_path(args.exe), repository_path(args.output)
    if not exe.is_file() or not 64 <= args.frames <= 10000 or args.timeout <= 0:
        parser.error("Need an existing executable, frames in [64,10000], and positive timeout")
    check_detail_padding()
    output.mkdir(parents=True, exist_ok=True)
    scenes = {scene["name"]: prepared_scene(scene) for scene in write_fixtures(output)["fixtures"]}
    environment = os.environ.copy()
    environment.pop("GFXRECON_CAPTURE_FILE", None)
    environment.pop("VK_INSTANCE_LAYERS", None)
    environment["VK_LAYER_SETTINGS_PATH"] = str(output)
    (output / "vk_layer_settings.txt").write_text("khronos_validation.validate_sync = true\n", encoding="utf-8")
    results = {"schemaVersion": 1, "startedUtc": datetime.now(timezone.utc).isoformat(),
               "exeSha256": hashlib.sha256(exe.read_bytes()).hexdigest(), "cases": [], "failures": [],
               "measurementLimits": ["Inverse ACES estimates from quantized sRGB receiver patches",
                   "Closed single-sided 3 cm wall, not a general guarantee for thin/open/double-sided BIM sheets",
                   "Detail volume pads shaded room receiver surfaces beyond its one-cell blend band"]}
    presets = {}
    for technique in TECHNIQUES:
        for variant in ("sealed-thin", "open"):
            scene = scenes[variant]
            seed = output / f"{variant}-coarse-preset.json"
            name = f"{technique}-{variant}-coarse"
            options = {"technique": technique}
            if technique == TECHNIQUES[0]:
                seed.unlink(missing_ok=True)
                options["save_preset"] = seed
            else:
                options["preset"] = seed
            coarse = capture(exe, output, environment, scene, name, args.frames, args.timeout, **options)
            results["cases"].append(coarse)
            try:
                check_capture(coarse, False)
                if variant not in presets:
                    document = json.loads(seed.read_text(encoding="utf-8"))
                    detail = output / f"{variant}-detail-preset.json"
                    write_json(detail, detail_preset(document))
                    presets[variant] = detail
                detail = capture(exe, output, environment, scene, f"{technique}-{variant}-detail",
                                 args.frames, args.timeout, technique=technique, preset=presets[variant])
                results["cases"].append(detail)
                check_capture(detail, True)
            except (AssertionError, KeyError, OSError, ValueError) as error:
                results["failures"].append(f"{technique}/{variant}: {error}")
            write_json(output / "volume-quality-results.json", results)
    try:
        results["comparisons"] = compare(results["cases"])
    except (AssertionError, KeyError) as error:
        results["failures"].append(str(error))
    results["passed"] = not results["failures"] and all(case["passed"] for case in results["cases"])
    results["finishedUtc"] = datetime.now(timezone.utc).isoformat()
    write_json(output / "volume-quality-results.json", results)
    print(f"{'PASS' if results['passed'] else 'FAIL'}: {len(results['cases'])} room-volume quality captures")
    return 0 if results["passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
