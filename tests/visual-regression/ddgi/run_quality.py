#!/usr/bin/env python3
"""Architectural DDGI comparisons, fixed exposure, and independent diffuse references.

Captured RGB is decoded from sRGB and the renderer's ACES fit to estimate HDR
radiance. Quantized/saturated pixels and point-reference versus patch-average
differences limit this comparison; these are diagnostics, not image goldens.
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
from PIL import Image

from quality_fixtures import write_fixtures
from run_runtime import repository_path, write_json, VALIDATION_ERROR, validate_selected_probes

EXPOSURE = .04


def decode_radiance(rgb, exposure=EXPOSURE):
    srgb = np.asarray(rgb, dtype=np.float64) / 255.
    mapped = np.where(srgb <= .04045, srgb / 12.92,
                      ((srgb + .055) / 1.055) ** 2.4)
    # y=(2.51*x*x+.03*x)/(2.43*x*x+.59*x+.14), positive root.
    a, b, c = 2.51 - 2.43 * mapped, .03 - .59 * mapped, -.14 * mapped
    x = (-b + np.sqrt(b * b - 4 * a * c)) / (2 * a)
    return x / exposure


def receiver_metrics(png, scene, camera=None):
    image = np.asarray(Image.open(png).convert("RGB"))
    if list(image.shape[:2][::-1]) != scene["captureSize"]:
        raise AssertionError("Capture resolution differs from fixture ROI")
    result = {}
    for receiver in scene["receivers"]:
        left, top, right, bottom = receiver["pixelRoi"]
        if camera:
            matrix = np.array(camera["unjitteredViewProjColumns"]).T
            clip = matrix @ np.array(receiver["position"] + [1.])
            if clip[3] <= 0:
                raise AssertionError("Receiver is behind capture camera")
            ndc = clip[:2] / clip[3]
            x = int(round((ndc[0] * .5 + .5) * image.shape[1]))
            y = int(round((.5 - ndc[1] * .5) * image.shape[0]))
            left, top, right, bottom = x - 6, y - 6, x + 6, y + 6
        if not (0 <= left < right <= image.shape[1] and
                0 <= top < bottom <= image.shape[0]):
            raise AssertionError("Receiver ROI outside capture")
        pixels = image[top:bottom, left:right]
        hdr = decode_radiance(pixels)
        mean = hdr.mean(axis=(0, 1))
        result[receiver["name"]] = {
            "pixelRoi": [left, top, right, bottom], "meanRgb8": pixels.mean(axis=(0, 1)).tolist(),
            "estimatedRadiance": mean.tolist(),
            "estimatedLuminance": float(mean @ [.2126, .7152, .0722]),
            "saturatedChannelFraction": float((pixels >= 254).mean()),
        }
    return result


def command_for(exe, output, scene, name, frames, enabled=True, coarse=False,
                fine=False, technique="deferred-raster", sequence=None,
                preset=None, save_preset=None):
    volume = scene["volume"]
    spacing = [.75, .75, .75] if coarse else [.25, .25, .25] if fine else volume["spacing"]
    counts = [9, 5, 5] if coarse else [25, 13, 13] if fine else volume["counts"]
    command = [str(exe), "--model", str(output / scene["gltf"]),
        "--width", str(scene["captureSize"][0]), "--height", str(scene["captureSize"][1]),
        "--hidden", "--no-ui", "--validation", "--no-bloom", "--no-taa", "--msaa", "1",
        "--render-technique", technique, "--display-mode", "lit",
        "--camera-position", *map(str, scene["camera"]["position"]),
        "--camera-target", *map(str, scene["camera"]["target"]),
        "--camera-fov", str(scene["camera"]["fov"]), "--exposure", str(EXPOSURE),
        "--environment-intensity", "0", "--directional-intensity", "0",
        "--ray-shadows", "hard", "--area-lighting", "sampled", "--area-light-samples", "64",
        "--screenshot", str(output / f"{name}.png"),
        "--warmup-frames", str(frames - 1), "--capture-frame", str(frames)]
    # Keep the field active in both captures so coverage suppresses the same
    # legacy ambient/bounce approximation. Only primary indirect output differs.
    command += ["--ddgi", "--ddgi-intensity", "1" if enabled else "0",
            "--ddgi-origin", *map(str, volume["origin"]),
            "--ddgi-spacing", *map(str, spacing), "--ddgi-counts", *map(str, counts),
            "--ddgi-rays", "128", "--ddgi-probes-per-frame", "256",
            "--ddgi-ray-budget", "32768", "--ddgi-max-distance", str(volume["maxDistance"])]
    if sequence:
        command += ["--capture-sequence", str(sequence)]
    if preset:
        command += ["--ddgi-preset", str(preset)]
    if save_preset:
        command += ["--ddgi-save-preset", str(save_preset)]
    return command


def capture(exe, output, environment, scene, name, frames, timeout, **options):
    command = command_for(exe, output, scene, name, frames, **options)
    sample_frames = (json.loads(options["sequence"].read_text())["sampleFrames"]
                     if options.get("sequence") else [frames])
    record = {"name": name, "fixture": scene["name"], "command": command,
              "cwd": str(exe.parent), "frames": [], "failures": []}
    paths = []
    for frame in sample_frames:
        stem = name if frame == frames else f"{name}.frame-{frame:04d}"
        png, telemetry = output / f"{stem}.png", output / f"{stem}.telemetry.json"
        png.unlink(missing_ok=True)
        telemetry.unlink(missing_ok=True)
        paths.append((frame, png, telemetry))
    started = time.monotonic()
    try:
        run = subprocess.run(command, cwd=exe.parent, env=environment, capture_output=True,
                             text=True, encoding="utf-8", errors="replace", timeout=timeout)
        log, code = run.stdout + run.stderr, run.returncode
    except subprocess.TimeoutExpired as error:
        log = str(error)
        code = None
    (output / f"{name}.log").write_text(log, encoding="utf-8")
    record.update(returnCode=code, durationSeconds=round(time.monotonic() - started, 3),
                  validationErrors=[line for line in log.splitlines() if VALIDATION_ERROR.search(line)])
    if code != 0 or record["validationErrors"]:
        record["failures"].append("Renderer failed or logged validation/runtime errors")
    for frame, png, telemetry_path in paths:
        try:
            telemetry = json.loads(telemetry_path.read_text(encoding="utf-8"))
            gi = telemetry["ddgi"]
            if not gi["active"]:
                raise AssertionError(f"Inactive DDGI: {gi['status']}")
            record["frames"].append({"frame": frame, "png": png.name, "gpu": telemetry["gpu"],
                "ddgi": gi, "rayShadows": telemetry["rayShadows"],
                "camera": telemetry["camera"], "receivers": receiver_metrics(png, scene, telemetry["camera"])})
        except (OSError, ValueError, KeyError, AssertionError) as error:
            record["failures"].append(str(error))
    record["passed"] = not record["failures"]
    print(json.dumps({"case": name, "passed": record["passed"],
                      "failures": record["failures"]}), flush=True)
    return record


def compare(cases):
    by_name = {case["name"]: case["frames"][-1]["receivers"] for case in cases
               if case["passed"] and case["frames"]}
    comparisons = {}
    for variant in ("doorway", "sealed-thin", "sealed-thick", "open", "doorway-neutral"):
        on, off = by_name.get(f"{variant}-on"), by_name.get(f"{variant}-off")
        if on and off:
            comparisons[variant] = {
                key: {"indirectRgbEstimate": (np.array(on[key]["estimatedRadiance"]) -
                                              np.array(off[key]["estimatedRadiance"])).tolist(),
                      "indirectLuminanceEstimate": on[key]["estimatedLuminance"] - off[key]["estimatedLuminance"]}
                for key in on}
    if "open-on" in by_name:
        denominator = max(by_name["open-on"]["shaded-back"]["estimatedLuminance"], 1e-10)
        comparisons["openControlLuminance"] = by_name["open-on"]["shaded-back"]["estimatedLuminance"]
        for key in ("sealed-thin-on", "sealed-thick-on", "sealed-thin-coarse", "sealed-thin-fine"):
            if key in by_name:
                comparisons[key] = {"shadedBackToOpenLuminanceRatio":
                    by_name[key]["shaded-back"]["estimatedLuminance"] / denominator}
        if "sealed-thin-coarse" in by_name and "sealed-thin-coarse-off" in by_name:
            on, off = by_name["sealed-thin-coarse"], by_name["sealed-thin-coarse-off"]
            comparisons["sealed-thin-coarse"]["isolatedIndirectLuminanceEstimate"] = (
                on["shaded-back"]["estimatedLuminance"] - off["shaded-back"]["estimatedLuminance"])
            comparisons["sealed-thin-coarse"]["coverageFallbackLuminanceEstimate"] = off["shaded-back"]["estimatedLuminance"]
    return comparisons


def check_motion(record):
    priority = False
    fresh_frames = math.ceil(637 / 256)
    for sample in record["frames"]:
        frame, gi = sample["frame"], sample["ddgi"]
        epoch = 1 if frame <= 40 else 2 if frame <= 80 else min(frame - 78, 42)
        generation = 1 if frame <= 80 else min(frame - 79, 41)
        probes = math.prod(gi["settings"]["counts"])
        selected = validate_selected_probes(gi, probes, f"Motion frame {frame}: ")
        priority |= selected
        fresh_frames = math.ceil(probes / math.ceil(256 / 2)) if selected else math.ceil(probes / 256)
        checks = {"contentEpoch": epoch, "updateFrame": frame, "updatedProbes": 256,
                  "tracedRays": 32768}
        if not selected:
            checks.update(firstProbe=(frame - 1) * 256 % probes, nextProbe=frame * 256 % probes)
        for key, expected in checks.items():
            if gi[key] != expected:
                raise AssertionError(f"Motion frame {frame} {key}: {gi[key]} != {expected}")
        if sample["rayShadows"]["generation"] != generation:
            raise AssertionError(f"Motion frame {frame}: AS generation did not follow geometry")
        light_fresh = 41 + fresh_frames - 1
        geometry_fresh = 120 + fresh_frames - 1
        if (light_fresh <= frame <= 80 or frame >= geometry_fresh) and gi["refreshRemainingProbes"]:
            raise AssertionError(f"Motion frame {frame}: stopped scene never completed fresh sweep")
    frames = {sample["frame"]: sample for sample in record["frames"]}
    if not {40, 60, 80, 100, 124, 160} <= set(frames):
        raise AssertionError("Missing motion checkpoints")
    light_delta = (frames[60]["receivers"]["shaded-back"]["estimatedLuminance"] -
                   frames[40]["receivers"]["shaded-back"]["estimatedLuminance"])
    geometry_delta = (frames[100]["receivers"]["shaded-back"]["estimatedLuminance"] -
                      frames[80]["receivers"]["shaded-back"]["estimatedLuminance"])
    if abs(light_delta) < .005 or abs(geometry_delta) < .005:
        raise AssertionError("Motion events produced no measurable shaded receiver response")
    return {"freshSweepSubmittedFrames": fresh_frames,
            "lightResponseLuminanceDelta": light_delta,
            "geometryResponseLuminanceDelta": geometry_delta,
            "lastMotionFrame": 120, "checkedFreshAfterFrame": 124,
            "priorityScheduling": priority,
            "note": "Fresh visibility sweep completes before smoothed multibounce illumination converges"}


def check_quality(comparisons):
    if comparisons["openControlLuminance"] <= .005:
        raise AssertionError("Open control receiver was not illuminated")
    indirect = comparisons["doorway"]["shaded-back"]["indirectLuminanceEstimate"]
    colored = np.array(comparisons["doorway"]["lit-back"]["indirectRgbEstimate"])
    neutral = np.array(comparisons["doorway-neutral"]["lit-back"]["indirectRgbEstimate"])
    color_ratio = colored[0] / max(colored[1], 1e-10)
    neutral_ratio = neutral[0] / max(neutral[1], 1e-10)
    if indirect <= .005 or color_ratio <= neutral_ratio + .1:
        raise AssertionError("Architectural indirect illumination/color bleeding was not measurable")
    for name in ("sealed-thin-on", "sealed-thick-on", "sealed-thin-fine"):
        if comparisons[name]["shadedBackToOpenLuminanceRatio"] > .01:
            raise AssertionError(f"{name}: sealed-room response exceeds 1% of the open control")
    return {"doorwayShadedIndirectLuminance": indirect, "litBackIndirectRedGreenRatio": color_ratio,
            "neutralIndirectRedGreenRatio": neutral_ratio,
            "sealedNormalFineResponseLimit": .01,
            "coarseGridMeasuredLimitation": comparisons["sealed-thin-coarse"]}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--exe", default="out/build/visual-studio/Release/VulkanSceneRenderer.exe")
    parser.add_argument("--output", default="out/ddgi-quality-validation")
    parser.add_argument("--frames", type=int, default=240)
    parser.add_argument("--timeout", type=float, default=180)
    parser.add_argument("--reference-samples", type=int, default=16384)
    parser.add_argument("--skip-reference", action="store_true")
    parser.add_argument("--static-only", action="store_true")
    args = parser.parse_args()
    exe, output = repository_path(args.exe), repository_path(args.output)
    if not exe.is_file() or not 1 <= args.frames <= 10000 or args.timeout <= 0:
        parser.error("Need an existing executable, frames in [1,10000], positive timeout")
    output.mkdir(parents=True, exist_ok=True)
    manifest = write_fixtures(output)
    environment = os.environ.copy()
    environment["VK_LAYER_SETTINGS_PATH"] = str(output)
    (output / "vk_layer_settings.txt").write_text("khronos_validation.validate_sync = true\n")
    results = {"schemaVersion": 1, "startedUtc": datetime.now(timezone.utc).isoformat(),
        "exeSha256": hashlib.sha256(exe.read_bytes()).hexdigest(), "cases": [],
        "measurementLimits": ["Inverse ACES from quantized sRGB PNG; saturated pixels have no exact inverse",
            "GPU PBR versus CPU Lambertian; screen patch average versus center-point reference",
            "On/off pairs retain a warmed active field; primary indirect intensity is 1/0 to match ambient coverage",
            "Approximate single volume: leakage ratios are measured limitations, not zero-leak assertions"]}
    for scene in manifest["fixtures"]:
        for enabled in (False, True):
            results["cases"].append(capture(exe, output, environment, scene,
                f"{scene['name']}-{'on' if enabled else 'off'}", args.frames, args.timeout,
                enabled=enabled))
            write_json(output / "quality-results.json", results)
    by_variant = {scene["name"]: scene for scene in manifest["fixtures"]}
    for suffix, options in (("coarse", {"coarse": True}), ("fine", {"fine": True}),
                            ("forward", {"technique": "forward-raster"})):
        # Match average scheduled visits per probe across resolutions. A large
        # lattice otherwise gets much less hysteresis warmup at the same frame.
        target_visits = args.frames * 256 / math.prod(by_variant["sealed-thin"]["volume"]["counts"])
        probes = 225 if suffix == "coarse" else 4225 if suffix == "fine" else 637
        sweep_frames = math.ceil(target_visits * probes / min(probes, 256))
        results["cases"].append(capture(exe, output, environment, by_variant["sealed-thin"],
            f"sealed-thin-{suffix}", sweep_frames, args.timeout, **options))
        if suffix == "coarse":
            results["cases"].append(capture(exe, output, environment, by_variant["sealed-thin"],
                "sealed-thin-coarse-off", sweep_frames, args.timeout, enabled=False, coarse=True))
    results["comparisons"] = compare(results["cases"])
    results["qualityFailures"] = []
    try:
        results["qualityChecks"] = check_quality(results["comparisons"])
    except (AssertionError, KeyError) as error:
        results["qualityFailures"].append(str(error))
    if not args.static_only:
        scene = by_variant["doorway"]
        sequence = output / "motion-sequence.json"
        write_json(sequence, {"schemaVersion": 1, "frames": 160,
            "sampleFrames": [40, 41, 44, 60, 80, 81, 84, 100, 120, 124, 140, 160],
            "objectName": "Partition", "objectTranslation": [
                {"frame": 1, "translation": [0, 0, 0]},
                {"frame": 80, "translation": [0, 0, 0]},
                {"frame": 81, "translation": [.3, 0, 0]},
                {"frame": 100, "translation": [-.3, 0, 0]},
                {"frame": 120, "translation": [0, 0, 0]},
                {"frame": 160, "translation": [0, 0, 0]}],
            "camera": [{"frame": 1, "position": scene["camera"]["position"], "target": scene["camera"]["target"]},
                       {"frame": 20, "position": [.2, 1.65, 7], "target": scene["camera"]["target"]}],
            "events": [{"frame": 41, "areaLightPosition": [-.7, 2.6, -.6]}]})
        motion = capture(exe, output, environment, scene, "doorway-motion", 160, args.timeout,
                         sequence=sequence)
        try:
            motion["motionChecks"] = check_motion(motion)
        except (AssertionError, KeyError) as error:
            motion["failures"].append(str(error))
        motion["passed"] = not motion["failures"]
        results["cases"].append(motion)
    if not args.skip_reference:
        from reference import reference_receivers
        results["references"] = {}
        for scene in manifest["fixtures"]:
            results["references"][scene["name"]] = reference_receivers(
                scene, samples=args.reference_samples, bounces=8, seed=42)
            write_json(output / "quality-results.json", results)
    results["passed"] = all(case["passed"] for case in results["cases"]) and not results["qualityFailures"]
    results["finishedUtc"] = datetime.now(timezone.utc).isoformat()
    write_json(output / "quality-results.json", results)
    print(f"{'PASS' if results['passed'] else 'FAIL'}: {len(results['cases'])} quality cases")
    return 0 if results["passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
