#!/usr/bin/env python3
"""DDGI material-policy diagnostics and live enable/pause/fallback contracts.

The cutaway architecture is identical to quality_fixtures.py. Only the divider
geometry and its glTF doubleSided flag differ. Leakage is reported against an
open-room control rather than imposing a zero-leak assumption on thin sheets
or on double-sided solids, which cannot supply enclosing-backface evidence.
"""

from __future__ import annotations

import argparse
from datetime import datetime, timezone
import hashlib
import json
import math
import os
from pathlib import Path
import subprocess
import time

import numpy as np

from quality_fixtures import build_scene, gltf_scene, rectangle
from run_quality import command_for as quality_command, receiver_metrics
from run_runtime import repository_path, write_json, VALIDATION_ERROR, validate_selected_probes


VARIANTS = ("solid-single", "solid-double", "sheet-single", "sheet-double",
            "doorway-single", "doorway-double", "open-control")
LIVE_FRAMES = 24
LIVE_EVENTS = ({"frame": 3, "ddgiEnabled": True},
               {"frame": 9, "ddgiPaused": True},
               {"frame": 12, "ddgiPaused": False},
               {"frame": 16, "ddgiEnabled": False},
               {"frame": 19, "ddgiEnabled": True})


def require(condition, message):
    if not condition:
        raise AssertionError(message)


def runtime_shader_hashes(exe):
    """Bind captures to the standalone SPIR-V assets loaded beside the exe."""
    shader_directory = Path(exe).parent / "spv_shaders"
    shaders = sorted(shader_directory.glob("*.spv"))
    require(bool(shaders), "Renderer runtime has no standalone SPIR-V assets")
    return {path.relative_to(Path(exe).parent).as_posix():
            hashlib.sha256(path.read_bytes()).hexdigest() for path in shaders}


def material_scene(variant, width=960, height=640):
    require(variant in VARIANTS, f"Unknown material fixture: {variant}")
    base = ("open" if variant == "open-control" else
            "doorway" if variant.startswith("doorway") else "sealed-thin")
    scene = build_scene(base, width, height)
    scene["name"] = variant
    double_sided = variant.endswith("-double")
    if variant.startswith("sheet"):
        scene["quads"] = [q for q in scene["quads"] if q["material"] != "partition"]
        # One zero-thickness sheet; cross(u,v) is +X. Rays approaching from the
        # lit -X room encounter its back face if the material is single-sided.
        scene["quads"].append(rectangle("single-sheet", [0, 1.4, -.5],
                                            [0, 1.4, 0], [0, 0, 1.5], "partition"))
        scene["partitionThickness"] = 0.0
        scene["partitionNode"] = 2
    scene["materialPolicy"] = {
        "partitionDoubleSided": double_sided, "shellDoubleSided": False,
        "alphaMode": "OPAQUE", "partitionGeometry":
            "none" if base == "open" else "single +X sheet" if variant.startswith("sheet")
            else "closed box" if base == "sealed-thin" else "three closed boxes around doorway",
        "classificationLimit": "Double-sided hits do not count as enclosing backfaces; "
                               "a single sheet has no well-defined enclosed interior.",
    }
    scene["referencePolicy"]["model"] = "diagnostic material-policy comparison; no CPU reference computed"
    scene["referencePolicy"].pop("sealedInvariant", None)
    return scene


def write_material_fixtures(output, width=960, height=640):
    output = Path(output)
    output.mkdir(parents=True, exist_ok=True)
    scenes = []
    for variant in VARIANTS:
        scene = material_scene(variant, width, height)
        gltf = gltf_scene(scene)
        for material in gltf["materials"]:
            if material["name"] == "partition":
                material["doubleSided"] = scene["materialPolicy"]["partitionDoubleSided"]
        path = output / f"material-{variant}.gltf"
        write_json(path, gltf)
        scene["gltf"] = path.name
        scene["gltfSha256"] = hashlib.sha256(path.read_bytes()).hexdigest()
        scenes.append(scene)
    manifest = {"schemaVersion": 1, "fixtures": scenes}
    write_json(output / "material-fixture-manifest.json", manifest)
    return manifest


def live_sequence():
    return {"schemaVersion": 1, "frames": LIVE_FRAMES,
            "sampleFrames": list(range(1, LIVE_FRAMES + 1)),
            "events": [dict(event) for event in LIVE_EVENTS]}


def command_for(exe, output, scene, name, frames, *, technique="deferred-raster",
                live=False, unsupported=False, primary_indirect=True):
    sequence = output / f"{name}-sequence.json" if live else None
    command = quality_command(exe, output, scene, name, frames,
                              technique=technique, sequence=sequence,
                              enabled=primary_indirect)
    if live:
        command[command.index("--ddgi")] = "--no-ddgi"
    if unsupported:
        command.append("--no-ray-query")
    return command


def check_live_frames(samples):
    require([s["frame"] for s in samples] == list(range(1, LIVE_FRAMES + 1)),
            "Live-control sequence must capture every frame")
    expected_update, cursor = 0, 0
    first_active_bytes = None
    previous = None
    for sample in samples:
        frame, gi = sample["frame"], sample["ddgi"]
        prefix = f"Live frame {frame}: "
        enabled = 3 <= frame < 16 or frame >= 19
        paused = 9 <= frame < 12
        require(gi["supported"] is True, prefix + "ray-query support is required")
        require(gi["enabled"] is enabled and gi["paused"] is paused,
                prefix + "enable/pause event did not apply, including false values")
        require(gi["active"] is enabled, prefix + "active state disagrees with enable")
        updating = enabled and not paused
        probes = math.prod(gi["settings"]["counts"])
        priority = validate_selected_probes(gi, probes, prefix)
        if updating:
            expected_update += 1
            require(gi["updatedProbes"] == 256 and gi["tracedRays"] == 32768,
                    prefix + "enabled field did not submit its update budget")
            require(gi["firstProbe"] == cursor, prefix + "pause/disable restarted scheduling cursor")
            cursor = gi["nextProbe"] if priority else (cursor + 256) % probes
            require(gi["nextProbe"] == cursor, prefix + "incorrect next scheduling cursor")
        else:
            for key in ("updatedProbes", "tracedRays", "firstProbe", "nextProbe"):
                require(gi[key] == 0, prefix + f"inactive batch must publish zero {key}")
        require(gi["schedulingCursor"] == cursor,
                prefix + "committed scheduling cursor was not retained")
        require(gi["updateFrame"] == expected_update,
                prefix + "pause/disable lost submitted history or incremented update frame")
        if frame == 3:
            first_active_bytes = gi["totalAllocatedBytes"]
            require(first_active_bytes > samples[0]["ddgi"]["totalAllocatedBytes"],
                    prefix + "enabling from fallback did not allocate full probe resources")
        if frame >= 3:
            require(gi["totalAllocatedBytes"] == first_active_bytes,
                    prefix + "pause/disable/re-enable replaced probe allocations")
        if paused:
            for key in ("contentEpoch", "historyResets", "refreshRemainingProbes"):
                require(gi[key] == previous[key], prefix + f"pause changed {key}")
            require("paused" in gi["status"].lower(), prefix + "missing retained-history pause status")
        if previous and frame > 3:
            require(gi["contentEpoch"] >= previous["contentEpoch"] and
                    gi["historyResets"] >= previous["historyResets"],
                    prefix + "re-enable reset retained epoch/history counters")
        previous = gi
    return {"updatedFrames": expected_update, "pausedFrames": [9, 10, 11],
            "disabledFrames": [1, 2, 16, 17, 18], "finalUpdateFrame": expected_update,
            "finalSchedulingCursor": cursor,
            "retainedProbeAllocationBytes": first_active_bytes,
            "note": "A scene-provider surface rebuild can advance epoch on enable; "
                    "pause and the retained scheduler/history do not reset."}


def check_unsupported_frames(samples):
    require(bool(samples), "Missing unsupported-device capture")
    for sample in samples:
        gi = sample["ddgi"]
        require(gi["enabled"] is True and gi["supported"] is False and gi["active"] is False,
                "Capability fallback must retain request and disable active GI")
        for key in ("updatedProbes", "tracedRays", "updateFrame", "firstProbe", "nextProbe",
                    "schedulingCursor"):
            require(gi[key] == 0, f"Capability fallback must keep {key} zero")
        require("ray quer" in gi["status"].lower() or "ray-query" in gi["status"].lower(),
                "Missing ray-query capability fallback reason")
        require(sample["rayShadows"]["active"] is False,
                "Unsupported ray queries must disable active ray shadows")
    return {"capabilityFallback": "requested DDGI/ray shadows disabled; raster fallback remains rendered"}


def capture(exe, output, environment, scene, name, frames, timeout, **options):
    live = options.get("live", False)
    if live:
        write_json(output / f"{name}-sequence.json", live_sequence())
    command = command_for(exe, output, scene, name, frames, **options)
    sample_frames = list(range(1, frames + 1)) if live else [frames]
    paths = []
    for frame in sample_frames:
        stem = name if frame == frames else f"{name}.frame-{frame:04d}"
        png, telemetry = output / f"{stem}.png", output / f"{stem}.telemetry.json"
        png.unlink(missing_ok=True)
        telemetry.unlink(missing_ok=True)
        paths.append((frame, png, telemetry))
    record = {"name": name, "fixture": scene["name"], "command": command,
              "cwd": str(exe.parent), "materialPolicy": scene["materialPolicy"],
              "frames": [], "failures": []}
    started = time.monotonic()
    try:
        run = subprocess.run(command, cwd=exe.parent, env=environment, capture_output=True,
                             text=True, encoding="utf-8", errors="replace", timeout=timeout)
        log, code = run.stdout + run.stderr, run.returncode
    except subprocess.TimeoutExpired as error:
        log, code = str(error), None
    (output / f"{name}.log").write_text(log, encoding="utf-8")
    record.update(returnCode=code, durationSeconds=round(time.monotonic() - started, 3),
                  validationErrors=[line for line in log.splitlines() if VALIDATION_ERROR.search(line)])
    if code != 0 or record["validationErrors"]:
        record["failures"].append("Renderer failed or logged validation/runtime errors")
    for frame, png, telemetry_path in paths:
        try:
            telemetry = json.loads(telemetry_path.read_text(encoding="utf-8"))
            gi = telemetry["ddgi"]
            if not live and not options.get("unsupported"):
                require(gi["active"] is True, f"Inactive material diagnostic: {gi['status']}")
            record["frames"].append({"frame": frame, "png": png.name, "gpu": telemetry["gpu"],
                "ddgi": gi, "rayShadows": telemetry["rayShadows"],
                "camera": telemetry["camera"],
                "receivers": receiver_metrics(png, scene, telemetry["camera"])})
        except (OSError, ValueError, KeyError, AssertionError) as error:
            record["failures"].append(str(error))
    try:
        if live:
            record["liveChecks"] = check_live_frames(record["frames"])
        if options.get("unsupported"):
            record["fallbackChecks"] = check_unsupported_frames(record["frames"])
    except (AssertionError, KeyError) as error:
        record["failures"].append(str(error))
    record["passed"] = not record["failures"]
    print(json.dumps({"case": name, "passed": record["passed"],
                      "failures": record["failures"]}), flush=True)
    return record


def compare_materials(cases):
    successful = {case["fixture"]: case["frames"][-1]["receivers"] for case in cases
                  if case["passed"] and case["frames"] and case["name"].startswith("material-")}
    require(set(VARIANTS) <= set(successful), "Missing successful material diagnostic captures")
    denominator = successful["open-control"]["shaded-back"]["estimatedLuminance"]
    require(math.isfinite(denominator) and denominator > .005,
            "Material comparison needs an illuminated open control")
    ratios = {name: successful[name]["shaded-back"]["estimatedLuminance"] / denominator
              for name in VARIANTS if not name.startswith("doorway")}
    for name, values in successful.items():
        for receiver in values.values():
            require(math.isfinite(receiver["estimatedLuminance"]) and
                    np.isfinite(receiver["estimatedRadiance"]).all(),
                    f"Nonfinite receiver measurement in {name}")
    differences = {}
    for family in ("solid", "sheet", "doorway"):
        single, double = successful[f"{family}-single"], successful[f"{family}-double"]
        differences[family] = {name: {
            "doubleMinusSingleRadianceEstimate": (np.array(double[name]["estimatedRadiance"]) -
                                                   np.array(single[name]["estimatedRadiance"])).tolist(),
            "doubleMinusSingleLuminanceEstimate": double[name]["estimatedLuminance"] - single[name]["estimatedLuminance"]}
            for name in single}
    controls = {case["fixture"]: case["frames"][-1]["receivers"] for case in cases
                if case["passed"] and case["frames"] and case["name"].startswith("primary-off-")}
    require({"sheet-single", "sheet-double"} <= set(controls),
            "Missing successful sheet zero-primary-indirect controls")
    sheet_comparisons = {}
    for name in ("sheet-single", "sheet-double"):
        total = successful[name]["shaded-back"]["estimatedLuminance"]
        direct = controls[name]["shaded-back"]["estimatedLuminance"]
        require(math.isfinite(direct), f"Nonfinite sheet control measurement in {name}")
        sheet_comparisons[name] = {
            "totalLuminanceEstimate": total, "directOnlyLuminanceEstimate": direct,
            "isolatedIndirectLuminanceDeltaEstimate": total - direct,
            "totalToOpenLuminanceRatio": total / denominator,
            "directOnlyToOpenLuminanceRatio": direct / denominator,
            "isolatedIndirectToOpenLuminanceRatio": (total - direct) / denominator,
        }
    return {"openControlLuminance": denominator, "shadedBackToOpenLuminanceRatios": ratios,
            "doubleSidedDifferences": differences, "sheetControlComparisons": sheet_comparisons,
            "policy": "Diagnostics expose material limitations; no leak threshold is asserted for "
                      "double-sided solids or sheets. Geometry, exposure and probe budgets are matched. "
                      "Sheet controls keep a warmed field active and set only primary indirect intensity to zero. "
                      "Direct-only estimates can include legacy ambient where probe coverage is unavailable; "
                      "the signed pair difference isolates the primary DDGI contribution."}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--exe", default="out/build/visual-studio/Release/VulkanSceneRenderer.exe")
    parser.add_argument("--output", default="out/ddgi-material-validation")
    parser.add_argument("--frames", type=int, default=240)
    parser.add_argument("--timeout", type=float, default=180)
    parser.add_argument("--static-only", action="store_true")
    args = parser.parse_args()
    exe, output = repository_path(args.exe), repository_path(args.output)
    if not exe.is_file() or not 1 <= args.frames <= 10000 or args.timeout <= 0:
        parser.error("Need existing executable, frames in [1,10000], positive timeout")
    output.mkdir(parents=True, exist_ok=True)
    manifest = write_material_fixtures(output)
    environment = os.environ.copy()
    environment["VK_LAYER_SETTINGS_PATH"] = str(output)
    (output / "vk_layer_settings.txt").write_text("khronos_validation.validate_sync = true\n")
    results = {"schemaVersion": 1, "startedUtc": datetime.now(timezone.utc).isoformat(),
        "exeSha256": hashlib.sha256(exe.read_bytes()).hexdigest(),
        "runtimeShaderSha256": runtime_shader_hashes(exe), "cases": [],
        "measurementLimits": ["Estimated HDR decoded from quantized ACES/sRGB PNG; saturated channels cannot be inverted exactly.",
            "Only divider doubleSided flags differ within solid/sheet/doorway pairs; shell stays single-sided.",
            "Single sheets lack enclosed interiors; double-sided solids do not supply backface classification evidence.",
            "Static diagnostics report response rather than asserting a zero-leak material guarantee."]}
    for scene in manifest["fixtures"]:
        results["cases"].append(capture(exe, output, environment, scene,
            f"material-{scene['name']}", args.frames, args.timeout))
        write_json(output / "material-quality-results.json", results)
    for scene in manifest["fixtures"]:
        if scene["name"] in ("sheet-single", "sheet-double"):
            results["cases"].append(capture(exe, output, environment, scene,
                f"primary-off-{scene['name']}", args.frames, args.timeout,
                primary_indirect=False))
            write_json(output / "material-quality-results.json", results)
    results["comparisonFailures"] = []
    try:
        results["comparisons"] = compare_materials(results["cases"])
    except (AssertionError, KeyError) as error:
        results["comparisonFailures"].append(str(error))
    if not args.static_only:
        scene = next(s for s in manifest["fixtures"] if s["name"] == "doorway-single")
        for technique in ("deferred-raster", "forward-raster"):
            results["cases"].append(capture(exe, output, environment, scene,
                f"live-{technique.split('-')[0]}", LIVE_FRAMES, args.timeout,
                technique=technique, live=True))
            write_json(output / "material-quality-results.json", results)
        results["cases"].append(capture(exe, output, environment, scene,
            "unsupported-capability", 4, args.timeout, unsupported=True))
    results["passed"] = all(c["passed"] for c in results["cases"]) and not results["comparisonFailures"]
    results["finishedUtc"] = datetime.now(timezone.utc).isoformat()
    write_json(output / "material-quality-results.json", results)
    print(f"{'PASS' if results['passed'] else 'FAIL'}: {len(results['cases'])} material/live-control cases")
    return 0 if results["passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
