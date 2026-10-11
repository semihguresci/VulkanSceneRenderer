#!/usr/bin/env python3
"""Bounded DDGI dielectric transport: closed slabs, roughness and absorption.

Images use matched active GI1/GI0 fields. The shaded receiver is visible through
the front cutaway, so changes measure secondary transport rather than primary
glass rasterization. Contract GPU tests cover exact Snell and TIR numerics;
these captures cover actual production closest-hit traversal and termination.
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

from quality_fixtures import box, gltf_scene
import run_glass as thin
from run_material_quality import runtime_shader_hashes
from run_quality import receiver_metrics
from run_runtime import VALIDATION_ERROR, repository_path, write_json


VARIANTS = ("open-control", "thick-clear", "thick-rough", "thick-absorbing",
            "opaque-behind", "malformed-open", "nested-volumes")
TECHNIQUES = thin.TECHNIQUES


def dielectric_scene(variant, width=400, height=360):
    thin.require(variant in VARIANTS, f"Unknown dielectric fixture: {variant}")
    scene = thin.glass_scene("open-control" if variant == "open-control" else "glass-pane",
                             width, height)
    scene["name"] = variant
    if variant not in ("open-control", "malformed-open"):
        scene["quads"] = [quad for quad in scene["quads"] if quad["material"] != "partition"]
        scene["quads"] += box("glass-volume", [-.1, 0, -2], [.1, 2.8, 1], "partition")
    if variant == "opaque-behind":
        scene["quads"] += box("opaque-terminal", [.15, 0, -2], [.18, 2.8, 1], "white")
    if variant == "nested-volumes":
        scene["quads"] += box("nested-volume", [-.05, 0, -2], [.05, 2.8, 1], "partition")
    scene["dielectricPolicy"] = {
        "thicknessFactor": .2, "roughness": .45 if variant == "thick-rough" else 0,
        "ior": 1.5, "attenuationColor": [.001, .15, .8] if variant == "thick-absorbing" else [1, 1, 1],
        "attenuationDistance": .1 if variant == "thick-absorbing" else None,
        "closed": variant not in ("open-control", "malformed-open"),
        "unsupportedNesting": variant == "nested-volumes",
        "opaqueTerminal": variant == "opaque-behind",
    }
    return scene


def write_dielectric_fixtures(output, width=400, height=360):
    output = Path(output)
    output.mkdir(parents=True, exist_ok=True)
    scenes = []
    for variant in VARIANTS:
        scene = dielectric_scene(variant, width, height)
        document = gltf_scene(scene)
        policy = scene["dielectricPolicy"]
        for material in document["materials"]:
            if material["name"] != "partition":
                continue
            material["doubleSided"] = True
            material["alphaMode"] = "OPAQUE"
            material["pbrMetallicRoughness"] = {"baseColorFactor": [1, 1, 1, 1],
                "metallicFactor": 0, "roughnessFactor": policy["roughness"]}
            volume = {"thicknessFactor": policy["thicknessFactor"],
                      "attenuationColor": policy["attenuationColor"]}
            if policy["attenuationDistance"] is not None:
                volume["attenuationDistance"] = policy["attenuationDistance"]
            material["extensions"] = {"KHR_materials_transmission": {"transmissionFactor": 1},
                "KHR_materials_volume": volume, "KHR_materials_ior": {"ior": policy["ior"]}}
        document["extensionsUsed"] += ["KHR_materials_transmission", "KHR_materials_volume", "KHR_materials_ior"]
        path = output / f"dielectric-{variant}.gltf"
        write_json(path, document)
        scene["gltf"], scene["gltfSha256"] = path.name, hashlib.sha256(path.read_bytes()).hexdigest()
        scenes.append(scene)
    manifest = {"schemaVersion": 1, "fixtures": scenes}
    write_json(output / "dielectric-fixture-manifest.json", manifest)
    return manifest


def case_name(variant, technique, enabled, primary):
    return f"{technique.split('-')[0]}-{variant}-{'refract' if enabled else 'block'}-gi{int(primary)}"


def command_for(exe, output, scene, name, frames, *, technique="deferred-raster",
                dielectric=True, primary=True):
    command = thin.command_for(exe, output, scene, name, frames, technique=technique,
                               thin_glass=False, primary_indirect=primary)
    command.append("--ddgi-dielectric" if dielectric else "--no-ddgi-dielectric")
    return command


def capture(exe, output, environment, scene, frames, timeout, *, technique, dielectric, primary):
    name = case_name(scene["name"], technique, dielectric, primary)
    command = command_for(exe, output, scene, name, frames, technique=technique,
                          dielectric=dielectric, primary=primary)
    png, telemetry_path = output / f"{name}.png", output / f"{name}.telemetry.json"
    png.unlink(missing_ok=True)
    telemetry_path.unlink(missing_ok=True)
    record = {"name": name, "fixture": scene["name"], "technique": technique,
              "dielectric": dielectric, "primaryIndirect": primary,
              "command": command, "failures": []}
    started = time.monotonic()
    try:
        process = subprocess.run(command, cwd=exe.parent, env=environment, capture_output=True,
            text=True, encoding="utf-8", errors="replace", timeout=timeout)
        log, code = process.stdout + process.stderr, process.returncode
    except subprocess.TimeoutExpired as error:
        log, code = str(error), None
    (output / f"{name}.log").write_text(log, encoding="utf-8")
    record.update(returnCode=code, durationSeconds=round(time.monotonic() - started, 3),
                  validationErrors=[line for line in log.splitlines() if VALIDATION_ERROR.search(line)])
    if code != 0 or record["validationErrors"]:
        record["failures"].append("Renderer failed or logged validation/runtime errors")
    try:
        telemetry = json.loads(telemetry_path.read_text(encoding="utf-8"))
        gi = telemetry["ddgi"]
        thin.require(gi["active"] is True and gi["updateFrame"] == frames,
                     "Inactive or incomplete dielectric field")
        thin.require(gi["refreshRemainingProbes"] == 0, "Dielectric field is still stale")
        thin.require(gi["settings"]["dielectricTransmission"] is dielectric,
                     "Dielectric opt-in did not reach runtime settings")
        thin.require(gi["settings"]["thinGlassTransmission"] is False,
                     "Legacy thin opt-in confounds the dielectric capture")
        thin.require(gi["settings"]["intensity"] == int(primary), "GI pair intensity mismatched")
        record.update(png=png.name, gpu=telemetry["gpu"], ddgi=gi,
                      receivers=receiver_metrics(png, scene, telemetry["camera"]))
    except (OSError, ValueError, KeyError, AssertionError) as error:
        record["failures"].append(str(error))
    record["passed"] = not record["failures"]
    print(json.dumps({"case": name, "passed": record["passed"], "failures": record["failures"]}), flush=True)
    return record


def compare_dielectric(cases, techniques=TECHNIQUES):
    records = {}
    for case in cases:
        key = (case["technique"], case["fixture"], case["dielectric"], case["primaryIndirect"])
        thin.require(key not in records, "Duplicate dielectric comparison capture")
        thin.require(case["passed"], f"Failed dielectric capture: {case['name']}")
        metric = case["receivers"]["shaded-back"]
        thin.require(math.isfinite(metric["estimatedLuminance"]) and
                     np.isfinite(metric["estimatedRadiance"]).all(), "Nonfinite dielectric metric")
        thin.require(metric.get("saturatedChannelFraction", 0) < .01, "Saturated dielectric metric")
        records[key] = case

    def delta(technique, fixture, enabled=True):
        pair = [records.get((technique, fixture, enabled, primary)) for primary in (True, False)]
        thin.require(all(pair), f"Missing matched dielectric pair: {technique}/{fixture}/{enabled}")
        on, off = [case["receivers"]["shaded-back"] for case in pair]
        return {"indirectLuminance": on["estimatedLuminance"] - off["estimatedLuminance"],
                "indirectRadiance": (np.asarray(on["estimatedRadiance"]) - off["estimatedRadiance"]).tolist()}

    result = {}
    for technique in techniques:
        measured = {variant: delta(technique, variant) for variant in VARIANTS}
        measured["thick-disabled"] = delta(technique, "thick-clear", False)
        reference = measured["open-control"]["indirectLuminance"]
        clear = measured["thick-clear"]
        thin.require(reference > .005, "Open dielectric control has no measurable signal")
        thin.require(clear["indirectLuminance"] > max(.003, reference * .03) and
                     clear["indirectLuminance"] > measured["thick-disabled"]["indirectLuminance"] + .003,
                     "Closed clear volume did not refract measurable indirect illumination")
        thin.require(measured["thick-rough"]["indirectLuminance"] > max(.003, reference * .01),
                     "Rough volume did not transmit measurable indirect illumination")
        absorbing = measured["thick-absorbing"]["indirectRadiance"]
        clear_rgb = clear["indirectRadiance"]
        thin.require(absorbing[0] < max(.002, .65 * clear_rgb[0]) and
                     absorbing[2] > max(.002, 2 * absorbing[0]),
                     "Volume absorption did not selectively attenuate red transport")
        blocked_limit = max(.002, .05 * reference)
        for variant in ("thick-disabled", "opaque-behind", "malformed-open", "nested-volumes"):
            thin.require(abs(measured[variant]["indirectLuminance"]) <= blocked_limit,
                         f"{variant}: terminal or unsupported volume leaked illumination")
        result[technique] = {"receivers": measured, "blockedIndirectLimit": blocked_limit}
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--exe", default="out/build/visual-studio/Release/VulkanSceneRenderer.exe")
    parser.add_argument("--output", default="out/ddgi-dielectric-validation")
    parser.add_argument("--frames", type=int, default=160)
    parser.add_argument("--timeout", type=float, default=180)
    parser.add_argument("--width", type=int, default=400)
    parser.add_argument("--height", type=int, default=360)
    parser.add_argument("--technique", choices=("both",) + TECHNIQUES, default="both")
    args = parser.parse_args()
    exe, output = repository_path(args.exe), repository_path(args.output)
    if not exe.is_file() or not 1 <= args.frames <= 10000 or not math.isfinite(args.timeout) or args.timeout <= 0:
        parser.error("Need existing executable, frames in [1,10000], finite positive timeout")
    if args.width < 160 or args.height < 160:
        parser.error("Dielectric fixture resolution must be at least 160")
    output.mkdir(parents=True, exist_ok=True)
    manifest = write_dielectric_fixtures(output, args.width, args.height)
    environment = os.environ.copy()
    environment["VK_LAYER_SETTINGS_PATH"] = str(output)
    (output / "vk_layer_settings.txt").write_text("khronos_validation.validate_sync = true\n")
    result = {"schemaVersion": 1, "startedUtc": datetime.now(timezone.utc).isoformat(),
              "exeSha256": hashlib.sha256(exe.read_bytes()).hexdigest(),
              "runtimeShaderSha256": runtime_shader_hashes(exe), "cases": [],
              "measurementLimits": ["Quantized ACES/sRGB images estimate radiance, not float readbacks.",
                "One closed homogeneous volume is supported; no nested media or non-TIR reflected branch.",
                "Primary raster transport is unchanged; direct emitter connections remain conservative."]}
    techniques = TECHNIQUES if args.technique == "both" else (args.technique,)
    for technique in techniques:
        for scene in manifest["fixtures"]:
            enabled_options = (False, True) if scene["name"] == "thick-clear" else (True,)
            for enabled in enabled_options:
                for primary in (True, False):
                    result["cases"].append(capture(exe, output, environment, scene, args.frames,
                        args.timeout, technique=technique, dielectric=enabled, primary=primary))
                    write_json(output / "dielectric-results.json", result)
    result["comparisonFailures"] = []
    try:
        result["comparisons"] = compare_dielectric(result["cases"], techniques)
    except (AssertionError, KeyError, ValueError) as error:
        result["comparisonFailures"].append(str(error))
    result["passed"] = all(case["passed"] for case in result["cases"]) and not result["comparisonFailures"]
    write_json(output / "dielectric-results.json", result)
    print(json.dumps({"passed": result["passed"], "cases": len(result["cases"]),
                      "comparisonFailures": result["comparisonFailures"]}), flush=True)
    return 0 if result["passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
