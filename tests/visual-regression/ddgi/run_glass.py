#!/usr/bin/env python3
"""Native DDGI thin-glass traversal comparisons with matched GI-zero controls.

The camera sees the shaded wall from the cutaway front without looking through
the divider. Only secondary DDGI rays enable this opt-in transport policy;
primary raster/ray shadows retain their existing conservative material policy.
Every image pair keeps the field active and changes only primary GI intensity.
"""
from __future__ import annotations

import argparse
import copy
from datetime import datetime, timezone
import hashlib
import json
import math
import os
from pathlib import Path
import subprocess
import time

import numpy as np

from quality_fixtures import box, gltf_scene, rectangle
from run_material_quality import material_scene, runtime_shader_hashes
from run_quality import command_for as quality_command, receiver_metrics
from run_runtime import VALIDATION_ERROR, repository_path, write_json


VARIANTS = ("open-control", "glass-pane", "opaque-behind", "layer-cap",
            "thick-unsupported")
TECHNIQUES = ("deferred-raster", "forward-raster")
FRAMES = 160


def require(condition, message):
    if not condition:
        raise AssertionError(message)


def glass_scene(variant, width=400, height=360):
    require(variant in VARIANTS + ("tinted-pane",), f"Unknown glass fixture: {variant}")
    require(width >= 160 and height >= 160, "Glass fixture resolution must be at least 160")
    scene = copy.deepcopy(material_scene(
        "open-control" if variant == "open-control" else "sheet-double", width, height))
    scene["name"] = variant
    # Keep every probe outside all nine tightly spaced panes. A probe in the
    # stack could relay radiance across several shorter paths rather than test
    # the per-ray crossing cap. No intentional geometry/probe coincidences.
    scene["volume"] = dict(scene["volume"], origin=[-3.25, -.05, -2], counts=[14, 7, 7])
    if variant == "layer-cap":
        scene["quads"] = [q for q in scene["quads"] if q["material"] != "partition"]
        for layer in range(9):
            scene["quads"].append(rectangle(f"glass-layer-{layer}",
                [(layer - 4) * .01, 1.4, -.5], [0, 1.4, 0], [0, 0, 1.5], "partition"))
    if variant == "opaque-behind":
        # Closed opaque solid has its own white material, not the glass
        # partition material. A transmitted closest ray must stop at this wall.
        scene["quads"] += box("opaque-behind-glass", [.12, 0, -2], [.15, 2.8, 1], "white")
    scene["glassPolicy"] = {
        "explicitTransmission": variant != "open-control", "opacity": 1.0,
        "roughness": .05, "metallic": 0.0,
        "declaredThickness": .1 if variant == "thick-unsupported" else 0.0,
        "sheetCount": 9 if variant == "layer-cap" else 0 if variant == "open-control" else 1,
        "opaqueTerminalWall": variant == "opaque-behind",
        "tint": [.25, .6, 1] if variant == "tinted-pane" else [1, 1, 1],
        "model": "bounded straight-through smooth thin dielectric secondary transport",
    }
    return scene


def write_glass_fixtures(output, width=400, height=360, include_tint=False):
    output = Path(output)
    output.mkdir(parents=True, exist_ok=True)
    scenes = []
    for variant in VARIANTS + (("tinted-pane",) if include_tint else ()):
        scene = glass_scene(variant, width, height)
        gltf = gltf_scene(scene)
        for material in gltf["materials"]:
            if material["name"] == "partition":
                material["doubleSided"] = True
                material["alphaMode"] = "OPAQUE"
                material["pbrMetallicRoughness"] = {
                    "baseColorFactor": scene["glassPolicy"]["tint"] + [1],
                    "metallicFactor": 0, "roughnessFactor": .05}
                material["extensions"] = {"KHR_materials_transmission": {"transmissionFactor": 1},
                    "KHR_materials_volume": {"thicknessFactor": scene["glassPolicy"]["declaredThickness"]}}
        gltf["extensionsUsed"] += ["KHR_materials_transmission", "KHR_materials_volume"]
        path = output / f"glass-{variant}.gltf"
        write_json(path, gltf)
        scene["gltf"] = path.name
        scene["gltfSha256"] = hashlib.sha256(path.read_bytes()).hexdigest()
        scenes.append(scene)
    manifest = {"schemaVersion": 1, "fixtures": scenes}
    write_json(output / "glass-fixture-manifest.json", manifest)
    return manifest


def case_name(variant, technique, thin_glass, primary_indirect):
    return (f"{technique.split('-')[0]}-{variant}-"
            f"{'transmit' if thin_glass else 'block'}-gi{int(primary_indirect)}")


def command_for(exe, output, scene, name, frames, *, technique="deferred-raster",
                thin_glass=True, primary_indirect=True):
    require(technique in TECHNIQUES, "Unsupported glass capture technique")
    require(1 <= frames <= 10000, "Glass capture frames must be in [1,10000]")
    command = quality_command(exe, output, scene, name, frames,
                              technique=technique, enabled=primary_indirect)
    if thin_glass:
        command.append("--ddgi-thin-glass")
    return command


def capture(exe, output, environment, scene, frames, timeout, *, technique,
            thin_glass, primary_indirect):
    name = case_name(scene["name"], technique, thin_glass, primary_indirect)
    command = command_for(exe, output, scene, name, frames, technique=technique,
                          thin_glass=thin_glass, primary_indirect=primary_indirect)
    png, telemetry_path = output / f"{name}.png", output / f"{name}.telemetry.json"
    png.unlink(missing_ok=True)
    telemetry_path.unlink(missing_ok=True)
    result = {"name": name, "fixture": scene["name"], "technique": technique,
              "thinGlass": thin_glass, "primaryIndirect": primary_indirect,
              "command": command, "cwd": str(exe.parent), "failures": []}
    started = time.monotonic()
    try:
        process = subprocess.run(command, cwd=exe.parent, env=environment, capture_output=True,
            text=True, encoding="utf-8", errors="replace", timeout=timeout)
        log, code = process.stdout + process.stderr, process.returncode
    except subprocess.TimeoutExpired as error:
        log, code = str(error), None
    (output / f"{name}.log").write_text(log, encoding="utf-8")
    result.update(returnCode=code, durationSeconds=round(time.monotonic() - started, 3),
        validationErrors=[line for line in log.splitlines() if VALIDATION_ERROR.search(line)])
    if code != 0 or result["validationErrors"]:
        result["failures"].append("Renderer failed or logged validation/runtime errors")
    try:
        telemetry = json.loads(telemetry_path.read_text(encoding="utf-8"))
        gi = telemetry["ddgi"]
        require(gi["active"] is True, f"Inactive DDGI glass capture: {gi['status']}")
        require(gi["settings"]["thinGlassTransmission"] is thin_glass,
                "Thin-glass opt-in flag did not reach runtime settings")
        require(gi["settings"]["intensity"] == int(primary_indirect),
                "Matched primary-indirect control did not reach runtime settings")
        require(gi["updateFrame"] == frames and gi["refreshRemainingProbes"] == 0,
                "Static glass field did not complete its bounded freshness sweep")
        result.update(png=png.name, gpu=telemetry["gpu"], ddgi=gi,
                      receivers=receiver_metrics(png, scene, telemetry["camera"]))
    except (OSError, ValueError, KeyError, AssertionError) as error:
        result["failures"].append(str(error))
    result["passed"] = not result["failures"]
    print(json.dumps({"case": name, "passed": result["passed"],
                      "failures": result["failures"]}), flush=True)
    return result


def compare_glass(cases, techniques=TECHNIQUES):
    records = {}
    for case in cases:
        key = (case["technique"], case["fixture"], case["thinGlass"], case["primaryIndirect"])
        require(key not in records, "Duplicate glass comparison capture")
        require(case["passed"], f"Failed glass capture: {case['name']}")
        records[key] = case
        for receiver in case["receivers"].values():
            require(math.isfinite(receiver["estimatedLuminance"]) and
                    np.isfinite(receiver["estimatedRadiance"]).all(), "Nonfinite glass receiver metric")
            require(receiver.get("saturatedChannelFraction", 0) < .01,
                    "Saturated glass receiver cannot support HDR comparison")

    def delta(technique, variant, enabled):
        pair = [records.get((technique, variant, enabled, state)) for state in (True, False)]
        require(all(pair), f"Missing matched GI1/GI0 glass pair: {technique}/{variant}/{enabled}")
        on, off = (case["receivers"]["shaded-back"] for case in pair)
        rgb = np.asarray(on["estimatedRadiance"]) - off["estimatedRadiance"]
        return {"isolatedIndirectLuminance": on["estimatedLuminance"] - off["estimatedLuminance"],
                "isolatedIndirectRadiance": rgb.tolist(),
                "totalLuminance": on["estimatedLuminance"],
                "zeroPrimaryLuminance": off["estimatedLuminance"]}

    result = {}
    for technique in techniques:
        measured = {variant: delta(technique, variant, True) for variant in VARIANTS}
        measured["glass-disabled"] = delta(technique, "glass-pane", False)
        if (technique, "tinted-pane", True, True) in records:
            measured["tinted-pane"] = delta(technique, "tinted-pane", True)
        open_indirect = measured["open-control"]["isolatedIndirectLuminance"]
        require(open_indirect > .005, "Open control has no measurable indirect illumination")
        pane = measured["glass-pane"]["isolatedIndirectLuminance"]
        disabled = measured["glass-disabled"]["isolatedIndirectLuminance"]
        require(pane > max(.003, .03 * open_indirect) and pane > disabled + .003,
                "Enabling thin glass did not measurably transmit indirect illumination")
        # Quantized HDR reconstruction, coarse probe interpolation and finite
        # convergence preclude an exact zero-leak image assertion.
        black_limit = max(.002, .05 * open_indirect)
        for variant in ("glass-disabled", "opaque-behind", "layer-cap", "thick-unsupported"):
            require(abs(measured[variant]["isolatedIndirectLuminance"]) <= black_limit,
                    f"{variant}: unsupported/terminal geometry leaked indirect illumination")
        for value in measured.values():
            value["isolatedIndirectToOpenRatio"] = value["isolatedIndirectLuminance"] / open_indirect
        result[technique] = {"receivers": measured, "blockedIndirectLimit": black_limit,
            "policy": "Signed GI1-GI0 estimates isolate primary DDGI; alpha remains one. "
                      "Nine sheets cluster between probe columns to test per-ray traversal exhaustion."}
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--exe", default="out/build/visual-studio/Release/VulkanSceneRenderer.exe")
    parser.add_argument("--output", default="out/ddgi-glass-validation")
    parser.add_argument("--frames", type=int, default=FRAMES)
    parser.add_argument("--timeout", type=float, default=180)
    parser.add_argument("--width", type=int, default=400)
    parser.add_argument("--height", type=int, default=360)
    parser.add_argument("--technique", choices=("both",) + TECHNIQUES, default="both")
    parser.add_argument("--include-tint", action="store_true")
    args = parser.parse_args()
    exe, output = repository_path(args.exe), repository_path(args.output)
    if not exe.is_file() or not 1 <= args.frames <= 10000 or not math.isfinite(args.timeout) or args.timeout <= 0:
        parser.error("Need existing executable, frames in [1,10000], finite positive timeout")
    if args.width < 160 or args.height < 160:
        parser.error("Glass fixture dimensions must be at least 160")
    output.mkdir(parents=True, exist_ok=True)
    manifest = write_glass_fixtures(output, args.width, args.height, args.include_tint)
    environment = os.environ.copy()
    environment["VK_LAYER_SETTINGS_PATH"] = str(output)
    (output / "vk_layer_settings.txt").write_text("khronos_validation.validate_sync = true\n")
    result = {"schemaVersion": 1, "startedUtc": datetime.now(timezone.utc).isoformat(),
        "exeSha256": hashlib.sha256(exe.read_bytes()).hexdigest(),
        "runtimeShaderSha256": runtime_shader_hashes(exe), "cases": [],
        "measurementLimits": ["Quantized ACES/sRGB PNG estimates are not float radiance readbacks.",
            "Straight-through thin sheets omit refraction, caustics and reflected specular energy.",
            "Every field is warm and active in both primary-intensity captures; camera/exposure/light match."]}
    techniques = TECHNIQUES if args.technique == "both" else (args.technique,)
    for technique in techniques:
        for scene in manifest["fixtures"]:
            flags = (False, True) if scene["name"] == "glass-pane" else (True,)
            for thin_glass in flags:
                for primary in (True, False):
                    result["cases"].append(capture(exe, output, environment, scene,
                        args.frames, args.timeout, technique=technique,
                        thin_glass=thin_glass, primary_indirect=primary))
                    write_json(output / "glass-results.json", result)
    result["comparisonFailures"] = []
    try:
        result["comparisons"] = compare_glass(result["cases"], techniques)
    except (AssertionError, KeyError, ValueError) as error:
        result["comparisonFailures"].append(str(error))
    result["passed"] = all(case["passed"] for case in result["cases"]) and not result["comparisonFailures"]
    write_json(output / "glass-results.json", result)
    print(json.dumps({"passed": result["passed"], "cases": len(result["cases"]),
                      "comparisonFailures": result["comparisonFailures"]}), flush=True)
    return 0 if result["passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
