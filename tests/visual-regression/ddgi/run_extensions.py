#!/usr/bin/env python3
"""Native automatic detail placement, scrolling and async DDGI integration checks.

Requires a Release renderer and ray-query GPU. Async checks require the actual
second-queue route unless --allow-async-fallback is requested for other hardware.
Generated inputs/results stay in --output. Run sequentially with other GPU tests.
"""
from __future__ import annotations
import argparse
import hashlib
import json
import math
import os
from pathlib import Path
import subprocess
from quality_fixtures import build_scene, gltf_scene
from run_features import check_batch, require, run_capture
from run_material_quality import runtime_shader_hashes
from run_runtime import repository_path, write_json


def check_detail(gi, minimum, maximum, budget=512):
    require(gi["active"] and len(gi["settings"]["additionalVolumes"]) == 1,
            "selected element detail was not fitted")
    settings = gi["settings"]
    require(settings["origin"] == [-4., -2., -2.5] and settings["spacing"] == [1., 1., 1.] and
            settings["counts"] == [9, 7, 13], "detail fit replaced primary grid")
    volume = settings["additionalVolumes"][0]
    detail_count = math.prod(volume["counts"])
    require(detail_count <= budget and min(volume["spacing"]) > 0, "detail budget/resolution invalid")
    for axis in range(3):
        origin, spacing, count = volume["origin"][axis], volume["spacing"][axis], volume["counts"][axis]
        require(abs(origin + spacing - minimum[axis]) <= 1e-5,
                "detail fit did not tightly pad the selected element minimum")
        require(maximum[axis] - 1e-5 <= origin + spacing * (count - 2) <= maximum[axis] + spacing + 1e-5,
                "detail fit did not tightly pad the selected element maximum")
    check_batch(gi, 819 + detail_count, 64, 32)
    require(any(index >= 819 for index in gi["selectedProbes"]), "fitted detail probes were never submitted")
    require(gi["updateFrame"] > 0 and gi["updatedProbes"] > 0 and gi["refreshRemainingProbes"] == 0,
            "fitted detail never finished updating")


def check_scroll(samples, count=819, allow_async_fallback=False, asynchronous=False):
    require(bool(samples), "missing scrolling captures")
    baseline = samples[0]["ddgi"]
    require(baseline["scroll"]["count"] == 0 and baseline["scroll"]["origin"] == [-4., -2., -2.5],
            "camera-follow moved before the scripted camera motion")
    previous_count = 0
    moved = False
    for sample in samples:
        gi = sample["ddgi"]
        require(gi["active"], "scrolling field inactive")
        check_batch(gi, count, 64, 32)
        require(gi["historyResets"] == baseline["historyResets"], "cell move rebuilt all history")
        require(gi["totalAllocatedBytes"] == baseline["totalAllocatedBytes"], "cell move reallocated resources")
        require(gi["scroll"]["count"] >= previous_count, "scroll count regressed")
        require(gi["scroll"]["count"] == (0 if sample["frame"] <= 34 else 1),
                "scrolling did not track the scripted cell crossing")
        previous_count = gi["scroll"]["count"]
        require(gi["settings"]["origin"] == [-4., -2., -2.5], "scrolling overwrote saved configuration")
        if gi["scroll"]["count"]:
            moved = True
            require(gi["scroll"]["preservedProbes"] == 728, "single-cell overlap was not preserved")
            require(gi["scroll"]["origin"] == [-3., -2., -2.5], "runtime volume did not snap one cell")
        if asynchronous:
            require(gi["asyncActive"] or allow_async_fallback, "async route fell back on capable test hardware")
    require(moved and samples[-1]["ddgi"]["scroll"]["count"] == 1, "scrolling failed or kept drifting")
    require(samples[-1]["ddgi"]["refreshRemainingProbes"] == 0, "exposed cells never refreshed")


def check_async(samples, allow_fallback=False):
    require(bool(samples), "missing async captures")
    submitted = published = 0
    for sample in samples:
        gi = sample["ddgi"]
        require(gi["active"] and (gi["asyncActive"] or allow_fallback), "async route inactive or unexpectedly fell back")
        require(gi["asyncSubmittedUpdates"] >= submitted and gi["asyncPublishedUpdates"] >= published,
                "async timeline counter regressed")
        require(gi["asyncPublishedUpdates"] <= gi["asyncSubmittedUpdates"], "published unsubmitted compute work")
        submitted, published = gi["asyncSubmittedUpdates"], gi["asyncPublishedUpdates"]
        check_batch(gi, 819, 64, 32)
    if samples[-1]["ddgi"]["asyncActive"]:
        require(submitted > 5 and published > 5, "compute jobs did not submit and publish")
    paused = [sample["ddgi"] for sample in samples if 34 <= sample["frame"] <= 40]
    require(paused and all(gi["paused"] and gi["updatedProbes"] == 0 for gi in paused), "pause still submitted updates")
    require(len({gi["updateFrame"] for gi in paused}) == 1, "pause advanced submitted updates")
    final = samples[-1]["ddgi"]
    require(not final["paused"] and final["updateFrame"] > paused[-1]["updateFrame"],
            "async updates did not resume")
    if final["asyncActive"]:
        require(final["asyncSubmittedUpdates"] > paused[-1]["asyncSubmittedUpdates"] and
                final["asyncPublishedUpdates"] > paused[-1]["asyncPublishedUpdates"],
                "resume did not submit and publish new compute work")
    require(final["refreshRemainingProbes"] == 0, "resumed field never finished refreshing")


def check_async_geometry(samples, allow_fallback=False):
    require(bool(samples), "missing async geometry captures")
    settled = [sample["ddgi"] for sample in samples if sample["frame"] >= 20]
    require(settled, "missing settled geometry captures")
    for sample in samples:
        gi = sample["ddgi"]
        require(gi["active"] and (gi["asyncActive"] or allow_fallback), "async geometry field inactive")
        check_batch(gi, 819, 64, 32)
        require(gi["asyncPublishedUpdates"] <= gi["asyncSubmittedUpdates"], "geometry published unsubmitted work")
    require(samples[-1]["ddgi"]["contentEpoch"] > samples[0]["ddgi"]["contentEpoch"],
            "moving geometry did not refresh transport")
    require(len({gi["contentEpoch"] for gi in settled}) == 1, "settled geometry kept invalidating")
    require(all(right["refreshRemainingProbes"] <= left["refreshRemainingProbes"]
                for left, right in zip(settled, settled[1:])), "settled geometry freshness regressed")
    require(settled[-1]["refreshRemainingProbes"] == 0, "async geometry never finished refreshing")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--exe", default="out/build/visual-studio/Release/VulkanSceneRenderer.exe")
    parser.add_argument("--output", default="out/ddgi-extensions-validation/native")
    parser.add_argument("--timeout", type=float, default=180)
    parser.add_argument("--allow-async-fallback", action="store_true")
    args = parser.parse_args()
    exe, output = repository_path(args.exe), repository_path(args.output)
    output.mkdir(parents=True, exist_ok=True)
    environment = os.environ.copy()
    environment.pop("GFXRECON_CAPTURE_FILE", None)
    environment.pop("VK_INSTANCE_LAYERS", None)
    environment["VK_LAYER_SETTINGS_PATH"] = str(output)
    (output / "vk_layer_settings.txt").write_text("khronos_validation.validate_sync = true\n")
    scene = output / "scene.gltf"
    write_json(scene, gltf_scene(build_scene("sealed-thin", 400, 360)))
    base = ["--model", str(scene), "--ddgi", "--environment-intensity", "0", "--directional-intensity", "0",
            "--ddgi-origin", "-4", "-2", "-2.5", "--ddgi-spacing", "1", "1", "1",
            "--ddgi-counts", "9", "7", "13", "--ddgi-rays", "32", "--ddgi-probes-per-frame", "64",
            "--ddgi-ray-budget", "2048", "--camera-position", "0", "1", "3.5", "--camera-target", "0", "1", "0"]
    scroll = output / "scroll.json"
    sample_frames = [1, 32, 33, 34, 35, 36, 37, 38, 40, 64]
    write_json(scroll, {"schemaVersion": 1, "frames": 64, "sampleFrames": sample_frames,
        "camera": [{"frame": 1, "position": [0, 1, 3.5], "target": [0, 1, 0]},
                   {"frame": 32, "position": [0, 1, 3.5], "target": [0, 1, 0]},
                   {"frame": 38, "position": [1.1, 1, 3.5], "target": [1.1, 1, 0]},
                   {"frame": 64, "position": [1.1, 1, 3.5], "target": [1.1, 1, 0]}]})
    lifecycle = output / "async.json"
    write_json(lifecycle, {"schemaVersion": 1, "frames": 64,
        "sampleFrames": [1, 16, 32, 33, 34, 36, 38, 40, 41, 48, 64],
        "events": [{"frame": 33, "ddgiPaused": True}, {"frame": 41, "ddgiPaused": False}]})
    geometry = output / "geometry.json"
    write_json(geometry, {"schemaVersion": 1, "frames": 64,
        "sampleFrames": [1, 10, 20, 21, 32, 48, 64], "objectName": "Partition",
        "objectTranslation": [{"frame": 1, "translation": [0, 0, 0]},
                              {"frame": 3, "translation": [0, 0, 0]},
                              {"frame": 10, "translation": [.35, 0, 0]},
                              {"frame": 64, "translation": [.35, 0, 0]}]})
    records, failures = [], []
    try:
        for technique in ("deferred-raster", "forward-raster"):
            route = [*base, "--render-technique", technique]
            detail = run_capture(exe, output, f"detail-{technique}",
                [*route, "--ddgi-detail-node", "Partition", "--ddgi-detail-probes", "512"],
                environment, args.timeout, frames=48)
            gi = detail["samples"][-1]["ddgi"]
            check_detail(gi, [-.015, 0., -2.], [.015, 2.8, 1.])
            records.append(detail)
            for async_mode in (False, True):
                record = run_capture(exe, output, f"scroll-{'async' if async_mode else 'sync'}-{technique}",
                    [*route, "--ddgi-camera-follow", *(["--ddgi-async"] if async_mode else [])],
                    environment, args.timeout, sequence=scroll, frames=64)
                check_scroll(record["samples"], allow_async_fallback=args.allow_async_fallback, asynchronous=async_mode)
                records.append(record)
            record = run_capture(exe, output, f"async-lifecycle-{technique}", [*route, "--ddgi-async"],
                environment, args.timeout, sequence=lifecycle, frames=64)
            check_async(record["samples"], args.allow_async_fallback)
            records.append(record)
            record = run_capture(exe, output, f"async-geometry-{technique}", [*route, "--ddgi-async"],
                environment, args.timeout, sequence=geometry, frames=64)
            check_async_geometry(record["samples"], args.allow_async_fallback)
            records.append(record)
        # Exercise auxiliary-provider selection separately from glTF subtrees.
        # Both fixtures contain the same cube in world-space coordinates.
        for extension, selected in (("bim", "taa-0002"), ("usda", "mesh2")):
            fixture = repository_path(f"models/validation/taa_equivalent.{extension}")
            record = run_capture(exe, output, f"detail-provider-{extension}",
                [*base, "--model", str(fixture), "--ddgi-detail-node", selected,
                 "--ddgi-detail-probes", "512"], environment, args.timeout, frames=48)
            gi = record["samples"][-1]["ddgi"]
            check_detail(gi, [-.45, 0., -.45], [.45, 1.2, .45])
            records.append(record)
        for name, flags, expected in (("detail-orphan-budget", ["--ddgi-detail-probes", "64"], "requires --ddgi-detail-node"),
                                     ("detail-invalid-budget", ["--ddgi-detail-node", "Partition", "--ddgi-detail-probes", "26"], "at least 27"),
                                     ("detail-missing-node", [*base, "--ddgi-detail-node", "missing room"], "missing or ambiguous")):
            run = subprocess.run([str(exe), *flags, "--hidden", "--no-ui"], cwd=exe.parent, env=environment,
                                 capture_output=True, text=True, encoding="utf-8", errors="replace", timeout=args.timeout)
            log = run.stdout + run.stderr
            (output / f"{name}.log").write_text(log, encoding="utf-8")
            require(run.returncode != 0 and expected in log, f"{name}: failed for an unexpected reason")
            records.append({"name": name, "passed": True, "expectedError": expected, "returnCode": run.returncode})
    except (AssertionError, OSError, ValueError, KeyError, subprocess.TimeoutExpired) as error:
        failures.append(str(error))
    result = {"passed": not failures, "cases": records, "failures": failures,
              "exeSha256": hashlib.sha256(exe.read_bytes()).hexdigest(),
              "runtimeShaderSha256": runtime_shader_hashes(exe)}
    write_json(output / "extension-results.json", result)
    print(json.dumps({"passed": result["passed"], "cases": len(records), "failures": failures}), flush=True)
    return 0 if result["passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
