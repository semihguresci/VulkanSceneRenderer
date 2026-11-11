#!/usr/bin/env python3
"""Compare architectural DDGI leakage with a frozen active probe field."""

from __future__ import annotations

import argparse
from datetime import datetime, timezone
import hashlib
import math
import os
from pathlib import Path

import numpy as np

from reference import reference_receivers
from run_material_quality import runtime_shader_hashes
from run_quality import COARSE_GRID, EXPOSURE, FINE_GRID, capture, exr_decoder
from run_runtime import repository_path, require, write_json


TECHNIQUES = ("deferred-raster", "forward-raster")
GRIDS = {"coarse": COARSE_GRID, "dense": FINE_GRID}
SOURCE_UNITS = ("metre", "millimetre")
VARIANTS = ("open", "sheet", "sealed", "gap", "overlap", "mixed")
RECEIVER_NAMES = ("lit-back", "shaded-back", "lit-floor", "shaded-floor")
MIN_OPEN_RESPONSE = .005
MAX_DENSE_SEALED_LEAK = .01
MIN_DENSE_SEALED_LIT_FRACTION = .25
MAX_RASTER_DELTA = .05


def warm_frames(grid, visits):
    probes = math.prod(GRIDS[grid][1])
    return math.ceil(visits * probes / min(256, probes))


def pair_sequence(warm):
    return {"schemaVersion": 1, "frames": warm + 2,
            "sampleFrames": [warm + 1, warm + 2],
            "events": [{"frame": warm + 1, "ddgiPaused": True, "ddgiIntensity": 1},
                       {"frame": warm + 2, "ddgiIntensity": 0}]}


def capture_args(scene, output, policy):
    args = ["--tone-map", "aces", "--ddgi-hysteresis", "0", "--no-ddgi-camera-follow",
            "--no-ddgi-async", "--no-ddgi-dielectric",
            "--ddgi-thin-glass" if policy == "thin-glass" else "--no-ddgi-thin-glass"]
    if scene.get("bim"):
        args += ["--bim-model", str(output / scene["bim"]),
                 "--bim-import-scale", str(scene["bimImportScale"])]
    return args


def compare_receiver(on, off, *, linear=False):
    require(on["pixelRoi"] == off["pixelRoi"], "Receiver moved within the intensity pair")
    require(linear or on["saturatedChannelFraction"] == off["saturatedChannelFraction"] == 0,
            "Receiver contains clipped or near-clipped channels")
    interval_key = "luminanceFloatInterval" if linear else "luminanceQuantizationInterval"
    signed_interval_key = "signedIndirectFloatInterval" if linear else "signedIndirectQuantizationInterval"
    rgb_on, rgb_off = (np.asarray(sample["estimatedRadiance"]) for sample in (on, off))
    on_lum, off_lum = on["estimatedLuminance"], off["estimatedLuminance"]
    on_lo, on_hi = on[interval_key]
    off_lo, off_hi = off[interval_key]
    require(rgb_on.shape == rgb_off.shape == (3,) and np.isfinite([rgb_on, rgb_off]).all() and
            all(math.isfinite(value) for value in (on_lo, on_lum, on_hi, off_lo, off_lum, off_hi)) and
            0 <= on_lo <= on_lum <= on_hi and 0 <= off_lo <= off_lum <= off_hi,
            "Receiver has invalid radiance or measurement limits")
    return {"pixelRoi": on["pixelRoi"], "intensityOneLuminance": on_lum,
            "intensityZeroLuminance": off_lum, "signedIndirectLuminance": on_lum - off_lum,
            "signedIndirectRgb": (rgb_on - rgb_off).tolist(),
            signed_interval_key: [on_lo - off_hi, on_hi - off_lo]}


def indirect_interval(receiver):
    if "signedIndirectFloatInterval" in receiver:
        return receiver["signedIndirectFloatInterval"]
    return receiver["signedIndirectQuantizationInterval"]


def normalized_interval(numerator, denominator):
    if denominator[0] <= 0:
        return None
    endpoints = [value / scale for value in numerator for scale in denominator]
    return [min(endpoints), max(endpoints)]


def reference_interval(reference):
    estimate, radius = reference["indirectLuminance"], 2 * reference["luminanceStandardError"]
    return [max(0, estimate - radius), estimate + radius]


def check_provider(frame, scene):
    provider = frame["scene"]
    require(provider["primaryObjectCount"] > 0 and Path(provider["primary"]).name == scene["gltf"],
            "Primary architectural shell is missing")
    if scene.get("bim"):
        require(Path(provider["auxiliary"]).name == scene["bim"] and
                provider["auxiliaryObjectCount"] >= len(scene["bimElements"]) and
                math.isclose(provider["auxiliaryImportScale"], scene["bimImportScale"], rel_tol=1e-6),
                "Expected IFCX provider or import scale is missing")
        for edge in ("min", "max"):
            require(np.allclose(provider["auxiliaryBounds"][edge], scene["bimBounds"][edge],
                                rtol=0, atol=1e-5), f"IFCX world bounds changed at {edge}")


def check_capture_metadata(metadata, scene, *, linear=False):
    require(metadata and metadata["schemaVersion"] == 1 and metadata["includesUi"] is False and
            [metadata["width"], metadata["height"]] == scene["captureSize"],
            "Capture metadata does not confirm a scene-only image with the requested dimensions")
    if linear:
        require(metadata["source"] == "raster-scene-composite" and metadata["colorSpace"] == "linear-sRGB" and
                metadata["exposureApplied"] is False and metadata["toneMappingApplied"] is False,
                "Capture metadata does not confirm an ungraded scene-linear raster composite")
        return
    require(metadata["source"] == "window" and metadata["colorSpace"] == "sRGB" and
            metadata["displayMode"] == metadata["toneMapper"] == 0 and
            metadata["exposureMode"] == "manual" and math.isclose(metadata["manualExposure"], EXPOSURE, rel_tol=1e-6) and
            metadata["includesUi"] is False, "Capture metadata does not confirm fixed ACES exposure for sRGB")


def compare_pair(record, scene, technique, grid, policy, warm, *, linear=False):
    require(record["passed"], f"{record['name']}: native capture failed")
    require([frame["frame"] for frame in record["frames"]] == [warm + 1, warm + 2],
            "Missing frozen-field intensity samples")
    on, off = record["frames"]
    spacing, counts = GRIDS[grid]
    for frame, intensity in ((on, 1), (off, 0)):
        gi = frame["ddgi"]
        settings = gi["settings"]
        require(frame["technique"] == technique, "Requested raster technique is inactive")
        require(gi["active"] and gi["supported"] and gi["enabled"] and gi["paused"] and
                gi["updateFrame"] >= warm - 1 and gi["updateFrame"] > 0 and
                gi["refreshRemainingProbes"] == gi["updatedProbes"] == gi["tracedRays"] == 0,
                "Probe field is not active, warm and frozen")
        require(settings["intensity"] == intensity and settings["hysteresis"] == 0 and
                tuple(settings["counts"]) == counts and tuple(settings["spacing"]) == spacing and
                np.allclose(settings["origin"], scene["volume"]["origin"], rtol=0, atol=1e-6) and
                math.isclose(settings["maxDistance"], scene["volume"]["maxDistance"], rel_tol=1e-6) and
                not settings["additionalVolumes"] and not settings["cameraFollow"] and
                not settings["asyncCompute"] and not settings["dielectricTransmission"] and
                settings["thinGlassTransmission"] == (policy == "thin-glass"),
                "DDGI grid, intensity or transport policy differs from the fixture")
        require(frame["rayShadows"]["active"], "Hard ray shadows did not exercise scene visibility")
        lighting = frame["lighting"]
        require(lighting["directionalIntensity"] == lighting["environmentIntensity"] == 0 and
                lighting["areaLightCount"] == 1 and lighting["pointLightCount"] == 0,
                "Expected one rectangle emitter and zero environment or directional lighting")
        require(np.allclose(frame["camera"]["position"], scene["camera"]["position"], rtol=0, atol=1e-5),
                "Camera did not use the authored fixture position")
        check_capture_metadata(frame["capture"], scene, linear=linear)
        check_provider(frame, scene)

    frozen_keys = ("updateFrame", "historyResets", "contentEpoch", "schedulingCursor", "totalAllocatedBytes")
    require(all(on["ddgi"][key] == off["ddgi"][key] for key in frozen_keys) and
            on["rayShadows"]["generation"] == off["rayShadows"]["generation"],
            "History, updates, allocation or geometry changed during the intensity toggle")
    require({key: value for key, value in on["ddgi"]["settings"].items() if key != "intensity"} ==
            {key: value for key, value in off["ddgi"]["settings"].items() if key != "intensity"},
            "Intensity toggle changed another probe setting")
    require(on["camera"] == off["camera"] and on["scene"] == off["scene"] and
            on["lighting"] == off["lighting"] and on["capture"] == off["capture"],
            "Camera, scene, lighting or exposure changed within the pair")
    require(set(on["receivers"]) == set(off["receivers"]) == set(RECEIVER_NAMES),
            "Missing or mismatched receiver ROIs")
    return {"name": record["name"], "fixture": scene["name"], "variant": scene["variant"],
            "sourceUnits": scene["sourceUnits"], "technique": technique, "grid": grid, "policy": policy,
            "captureFormat": "scene-linear-exr" if linear else "display-png",
            "warmFrames": warm, "completedUpdates": on["ddgi"]["updateFrame"], "probeCount": math.prod(counts),
            "allocatedBytes": on["ddgi"]["totalAllocatedBytes"],
            "receivers": {name: compare_receiver(on["receivers"][name], off["receivers"][name], linear=linear)
                          for name in RECEIVER_NAMES}}


def compare_rows(rows, references):
    controls = {(row["technique"], row["grid"], row["sourceUnits"], row["captureFormat"]): row for row in rows
                if row["variant"] == "open" and row["policy"] == "opaque"}
    for row in rows:
        route = (row["technique"], row["grid"], row["sourceUnits"], row["captureFormat"])
        require(route in controls, f"{row['name']}: missing matched open control")
        control = controls[route]
        for name, receiver in row["receivers"].items():
            open_receiver = control["receivers"][name]
            open_lum = open_receiver["signedIndirectLuminance"]
            if name in ("lit-back", "shaded-back"):
                require(open_lum > MIN_OPEN_RESPONSE and indirect_interval(open_receiver)[0] > 0,
                        f"{control['name']}: dark open-control {name}")
            if open_lum > MIN_OPEN_RESPONSE:
                receiver["openControlSignedLuminance"] = open_lum
                receiver["signedFractionOfOpen"] = receiver["signedIndirectLuminance"] / open_lum
                receiver["measurementIntervalFractionOfOpen"] = ([1, 1] if row is control else
                    normalized_interval(indirect_interval(receiver), indirect_interval(open_receiver)))
            if row["variant"] in references:
                ref = references[row["variant"]][name]
                receiver["referenceIndirectLuminance"] = ref["indirectLuminance"]
                receiver["referenceStandardError"] = ref["luminanceStandardError"]
                receiver["signedDifferenceFromReference"] = (receiver["signedIndirectLuminance"] -
                                                               ref["indirectLuminance"])
                ref_open = references["open"][name]
                ref_open_lum = ref_open["indirectLuminance"]
                if ref_open_lum > MIN_OPEN_RESPONSE and "signedFractionOfOpen" in receiver:
                    ref_fraction = ref["indirectLuminance"] / ref_open_lum
                    signed_difference = receiver["signedFractionOfOpen"] - ref_fraction
                    receiver["referenceFractionOfOpen"] = ref_fraction
                    receiver["signedNormalizedDifferenceFromReference"] = signed_difference
                    receiver["falseDarkeningFractionOfOpen"] = max(0, -signed_difference)
                    receiver["excessFractionOfOpen"] = max(0, signed_difference)
                    cpu_interval = ([1, 1] if ref is ref_open else
                                    normalized_interval(reference_interval(ref), reference_interval(ref_open)))
                    gpu_interval = receiver["measurementIntervalFractionOfOpen"]
                    receiver["referenceFractionOfOpenTwoSeInterval"] = cpu_interval
                    if cpu_interval and gpu_interval:
                        receiver["signedNormalizedDifferenceInterval"] = [gpu_interval[0] - cpu_interval[1],
                                                                           gpu_interval[1] - cpu_interval[0]]

        if row["variant"] == "sealed" and row["grid"] == "dense":
            receivers = row["receivers"]
            shaded = receivers["shaded-back"]
            require(abs(shaded["signedFractionOfOpen"]) <= MAX_DENSE_SEALED_LEAK,
                    f"{row['name']}: dense sealed-wall leakage exceeded the fixture limit")
            require(receivers["lit-back"]["signedFractionOfOpen"] >= MIN_DENSE_SEALED_LIT_FRACTION,
                    f"{row['name']}: dense sealed-wall setup darkened the lit receiver")


def summarize_bounds(rows):
    groups = {}
    for row in rows:
        key = (row["variant"], row["policy"], row["grid"], row["captureFormat"])
        groups.setdefault(key, []).append(row)

    summary = []
    for (variant, policy, grid, capture_format), cases in groups.items():
        receivers = {}
        for name in ("lit-back", "shaded-back"):
            values = [row["receivers"][name] for row in cases]
            fractions = [value["signedFractionOfOpen"] for value in values]
            result = {"signedFractionOfOpenRange": [min(fractions), max(fractions)]}
            intervals = [value["measurementIntervalFractionOfOpen"] for value in values
                         if value["measurementIntervalFractionOfOpen"] is not None]
            if intervals:
                result["measurementEnvelopeFractionOfOpen"] = [min(bounds[0] for bounds in intervals),
                                                                 max(bounds[1] for bounds in intervals)]
            differences = [value["signedNormalizedDifferenceFromReference"] for value in values
                           if "signedNormalizedDifferenceFromReference" in value]
            if differences:
                result["signedNormalizedDifferenceFromReferenceRange"] = [min(differences), max(differences)]
                result["maximumFalseDarkeningFractionOfOpen"] = max(0, -min(differences))
                result["maximumExcessFractionOfOpen"] = max(0, max(differences))
            receivers[name] = result
        summary.append({"variant": variant, "policy": policy, "grid": grid, "routeCount": len(cases),
                        "captureFormat": capture_format, "receivers": receivers})
    return summary


def compare_axis(rows, axis, pair, *, parity=False):
    dimensions = ("variant", "sourceUnits", "technique", "grid", "policy", "captureFormat")
    key_fields = tuple(field for field in dimensions if field != axis)
    groups = {}
    for row in rows:
        key = tuple(row[field] for field in key_fields)
        group = groups.setdefault(key, {})
        require(row[axis] not in group, f"Duplicate matrix row: {row['name']}")
        group[row[axis]] = row

    comparisons = []
    for key, group in groups.items():
        if not all(value in group for value in pair):
            continue
        first, second = (group[value] for value in pair)
        receivers = {}
        for name in RECEIVER_NAMES:
            before, after = first["receivers"][name], second["receivers"][name]
            difference = after["signedIndirectLuminance"] - before["signedIndirectLuminance"]
            low_a, high_a = indirect_interval(before)
            low_b, high_b = indirect_interval(after)
            bounds = [low_b - high_a, high_b - low_a]
            receivers[name] = {"signedLuminanceDifference": difference,
                               "measurementInterval": bounds}
            if "signedFractionOfOpen" in before and "signedFractionOfOpen" in after:
                receivers[name]["signedDifferenceOfOpenFractions"] = (after["signedFractionOfOpen"] -
                                                                      before["signedFractionOfOpen"])
                before_ratio = before["measurementIntervalFractionOfOpen"]
                after_ratio = after["measurementIntervalFractionOfOpen"]
                if before_ratio and after_ratio:
                    receivers[name]["openNormalizedMeasurementInterval"] = [after_ratio[0] - before_ratio[1],
                                                                              after_ratio[1] - before_ratio[0]]
            if parity and name in ("lit-back", "shaded-back"):
                tolerance = MAX_RASTER_DELTA * max(before["openControlSignedLuminance"],
                                                    after["openControlSignedLuminance"])
                require(bounds[0] <= tolerance and bounds[1] >= -tolerance,
                        f"{axis} parity exceeded {MAX_RASTER_DELTA:.0%} of the open control at "
                        f"{name}: {first['name']} vs {second['name']}")
                receivers[name]["parityTolerance"] = tolerance
        comparisons.append({**dict(zip(key_fields, key)), "first": first["name"], "second": second["name"],
                            "receivers": receivers})
    return comparisons


def main():
    from bim_leakage_fixtures import write_fixtures

    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--exe", default="out/build/visual-studio/Release/VulkanSceneRenderer.exe")
    parser.add_argument("--output", default="out/ddgi-bim-leakage")
    parser.add_argument("--variant", action="append", choices=VARIANTS,
                        help="Select a fixture. Open controls are added automatically.")
    parser.add_argument("--technique", action="append", choices=TECHNIQUES)
    parser.add_argument("--grid", action="append", choices=GRIDS)
    parser.add_argument("--units", action="append", choices=SOURCE_UNITS)
    parser.add_argument("--warm-visits", type=int, default=12)
    parser.add_argument("--warm-frames", type=int, help="Override warmup frames across selected grids")
    parser.add_argument("--width", type=int, default=640)
    parser.add_argument("--height", type=int, default=426)
    parser.add_argument("--timeout", type=float, default=180)
    parser.add_argument("--reference-samples", type=int, default=4096)
    parser.add_argument("--skip-reference", action="store_true")
    parser.add_argument("--linear", action="store_true", help="Measure float scene-linear OpenEXR (requires OpenCV)")
    args = parser.parse_args()
    exe, output = repository_path(args.exe), repository_path(args.output)
    techniques = tuple(dict.fromkeys(args.technique or TECHNIQUES))
    grids = tuple(dict.fromkeys(args.grid or GRIDS))
    units = tuple(dict.fromkeys(args.units or SOURCE_UNITS))
    variants = set(args.variant or VARIANTS) | {"open"}
    warmup = {grid: (warm_frames(grid, args.warm_visits) if args.warm_frames is None else args.warm_frames)
              for grid in grids}
    if (not exe.is_file() or not math.isfinite(args.timeout) or args.timeout <= 0 or args.warm_visits < 1 or
            any(not 2 <= frames <= 9998 for frames in warmup.values()) or args.reference_samples < 2 or
            min(args.width, args.height) < 64):
        parser.error("Need an existing executable, positive timeout, at least 2 warm frames and reference samples, "
                     "and a capture size of at least 64 by 64")
    if args.linear:
        try:
            exr_decoder()
        except ValueError as error:
            parser.error(str(error))

    output.mkdir(parents=True, exist_ok=True)
    manifest = write_fixtures(output, width=args.width, height=args.height)
    environment = os.environ.copy()
    environment.pop("GFXRECON_CAPTURE_FILE", None)
    environment.pop("VK_INSTANCE_LAYERS", None)
    environment["VK_LAYER_SETTINGS_PATH"] = str(output)
    (output / "vk_layer_settings.txt").write_text("khronos_validation.validate_sync = true\n", encoding="utf-8")
    report_path = output / "bim-leakage-results.json"
    manifest_path = output / "bim-leakage-fixture-manifest.json"
    result = {"schemaVersion": 1, "startedUtc": datetime.now(timezone.utc).isoformat(),
              "exeSha256": hashlib.sha256(exe.read_bytes()).hexdigest(),
              "runtimeShaderSha256": runtime_shader_hashes(exe), "exposure": None if args.linear else EXPOSURE,
              "fixtureManifestSha256": hashlib.sha256(manifest_path.read_bytes()).hexdigest(),
              "captureFormat": "scene-linear-exr" if args.linear else "display-png",
              "warmFramesByGrid": warmup, "cases": [], "rows": [], "failures": [], "references": {},
              "comparisonPolicy": {"minimumOpenSignedLuminance": MIN_OPEN_RESPONSE,
                  "maximumDenseSealedShadedFractionOfOpen": MAX_DENSE_SEALED_LEAK,
                  "minimumDenseSealedLitFractionOfOpen": MIN_DENSE_SEALED_LIT_FRACTION,
                  "maximumRasterDifferenceFractionOfOpen": MAX_RASTER_DELTA},
              "measurementLimits": ["Signed intensity 1 minus intensity 0 is measured with one frozen active field.",
                  ("EXR intervals are singleton float measurements. Differences include numerical residuals."
                   if args.linear else "Display intervals cover RGB8 rounding after inverse ACES/sRGB."),
                  "Measurement intervals exclude spatial averaging and model bias.",
                  "CPU bounds use twice the estimated Monte Carlo standard error around diffuse point radiance. "
                  "These bounds exclude finite-depth truncation and GPU PBR/ROI differences.",
                  "Sheet and transmission policies have no independent radiometric reference in this suite.",
                  "Measured fixture bounds do not establish a general leak-free geometry guarantee."]}
    scenes = [scene for scene in manifest["fixtures"]
              if scene["variant"] in variants and scene["sourceUnits"] in units]
    expected_scenes = {(variant, source_units) for variant in variants for source_units in units}
    require(len(scenes) == len(expected_scenes) and
            {(scene["variant"], scene["sourceUnits"]) for scene in scenes} == expected_scenes,
            "Fixture manifest does not contain exactly one scene for each selected variant and source unit")
    for scene in scenes:
        variant = scene["variant"]
        if not args.skip_reference and scene["referencePolicy"]["supported"] and variant not in result["references"]:
            result["references"][variant] = reference_receivers(scene, samples=args.reference_samples, bounces=8)
        policies = ("opaque", "thin-glass") if variant == "mixed" else ("opaque",)
        for technique in techniques:
            for grid in grids:
                warm = warmup[grid]
                for policy in policies:
                    name = f"{scene['name']}-{technique}-{grid}-{policy}"
                    sequence = output / f"{name}-sequence.json"
                    write_json(sequence, pair_sequence(warm))
                    record = capture(exe, output, environment, scene, name, warm + 2, args.timeout,
                                     technique=technique, coarse=grid == "coarse", fine=grid == "dense",
                                     sequence=sequence, extra=capture_args(scene, output, policy), linear=args.linear)
                    result["cases"].append(record)
                    try:
                        result["rows"].append(compare_pair(record, scene, technique, grid, policy, warm,
                                                           linear=args.linear))
                    except (AssertionError, KeyError, TypeError, ValueError) as error:
                        result["failures"].append(f"{name}: {error}")
                    write_json(report_path, result)
    if not result["failures"]:
        try:
            compare_rows(result["rows"], result["references"])
            result["boundsByFixtureAndGrid"] = summarize_bounds(result["rows"])
            result["gridComparisons"] = compare_axis(result["rows"], "grid", ("coarse", "dense"))
            result["scaleComparisons"] = compare_axis(result["rows"], "sourceUnits", SOURCE_UNITS)
            result["rasterComparisons"] = compare_axis(result["rows"], "technique", TECHNIQUES, parity=True)
            result["materialPolicyComparisons"] = compare_axis(result["rows"], "policy", ("opaque", "thin-glass"))
        except (AssertionError, KeyError, TypeError, ValueError) as error:
            result["failures"].append(str(error))
    result["passed"] = bool(result["rows"]) and not result["failures"]
    result["finishedUtc"] = datetime.now(timezone.utc).isoformat()
    write_json(report_path, result)
    print(f"{'PASS' if result['passed'] else 'FAIL'}: {len(result['rows'])} BIM leakage pairs", flush=True)
    for failure in result["failures"]:
        print(failure, flush=True)
    return 0 if result["passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
