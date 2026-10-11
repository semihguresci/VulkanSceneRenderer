#!/usr/bin/env python3
"""Native DDGI preset, packed-volume and priority scheduling regression checks.

Requires a built renderer and ray-query GPU. Generated inputs and captures stay
under --output; archived DDGI evidence is not modified. Glass has a separate
ray-traversal/lighting harness in run_glass.py.
"""
from __future__ import annotations
import argparse
import copy
import hashlib
import json
import math
import os
from pathlib import Path
import subprocess
from quality_fixtures import build_scene, gltf_scene
from run_runtime import repository_path, write_json, VALIDATION_ERROR


def require(condition, message):
    if not condition:
        raise AssertionError(message)


def check_batch(gi, count, budget, rays):
    selected = gi["selectedProbes"]
    require(gi["updatedProbes"] == len(selected) <= min(count, budget), "probe budget exceeded")
    require(len(selected) == len(set(selected)), "duplicate submitted probe")
    require(all(type(index) is int and 0 <= index < count for index in selected), "invalid packed index")
    require(gi["tracedRays"] == len(selected) * rays, "incomplete probe ray set")
    require(0 <= gi["schedulingCursor"] < count, "invalid fairness cursor")


def check_motion(samples, count, budget, last_change=10):
    require(samples and samples[-1]["frame"] >= last_change + math.ceil(count / math.ceil(budget / 2)),
            "capture ends before starvation bound")
    after = [sample for sample in samples if sample["frame"] > last_change]
    covered = set()
    previous = None
    for sample in samples:
        gi = sample["ddgi"]
        check_batch(gi, count, budget, 32)
        require(gi["active"], "packed volume inactive")
        if previous and sample["frame"] > last_change:
            require(gi["contentEpoch"] == previous["contentEpoch"], "stopped geometry keeps invalidating")
            require(gi["refreshRemainingProbes"] <= previous["refreshRemainingProbes"], "freshness regressed")
        previous = gi
    for sample in after[:math.ceil(count / math.ceil(budget / 2))]:
        covered.update(sample["ddgi"]["selectedProbes"])
    require(len(covered) == count, "priority updates starved probes after geometry stopped")
    require(after[-1]["ddgi"]["refreshRemainingProbes"] == 0, "fresh sweep never completed")
    require(samples[-1]["ddgi"]["contentEpoch"] > samples[0]["ddgi"]["contentEpoch"], "geometry did not refresh transport")


def run_capture(exe, output, name, arguments, environment, timeout, sequence=None, frames=6):
    png = output / f"{name}.png"
    sample_frames = json.loads(sequence.read_text())["sampleFrames"] if sequence else [frames]
    for frame in sample_frames:
        stem = name if frame == frames else f"{name}.frame-{frame:04d}"
        (output / f"{stem}.png").unlink(missing_ok=True)
        (output / f"{stem}.telemetry.json").unlink(missing_ok=True)
    command = [str(exe), *arguments, "--hidden", "--no-ui", "--validation", "--no-bloom",
               "--no-taa", "--msaa", "1", "--width", "400", "--height", "360",
               "--screenshot", str(png), "--warmup-frames", str(frames - 1), "--capture-frame", str(frames)]
    if sequence:
        command += ["--capture-sequence", str(sequence)]
    run = subprocess.run(command, cwd=exe.parent, env=environment, capture_output=True,
                         text=True, encoding="utf-8", errors="replace", timeout=timeout)
    log = run.stdout + run.stderr
    (output / f"{name}.log").write_text(log, encoding="utf-8")
    require(run.returncode == 0, f"{name}: renderer failed: {run.returncode}")
    require(not VALIDATION_ERROR.search(log), f"{name}: validation/runtime error")
    samples = []
    for frame in sample_frames:
        stem = name if frame == frames else f"{name}.frame-{frame:04d}"
        capture = output / f"{stem}.png"
        require(capture.read_bytes()[:8] == b"\x89PNG\r\n\x1a\n", f"{name}: missing PNG")
        telemetry = json.loads((output / f"{stem}.telemetry.json").read_text(encoding="utf-8"))
        samples.append({"frame": frame, "ddgi": telemetry["ddgi"]})
    record = {"name": name, "command": command, "samples": samples, "passed": True}
    print(json.dumps({"case": name, "passed": True}), flush=True)
    return record


def invalid_capture(exe, output, name, arguments, expected, environment, timeout):
    run = subprocess.run([str(exe), *arguments, "--hidden", "--no-ui"], cwd=exe.parent, env=environment,
                         capture_output=True, text=True, encoding="utf-8", errors="replace", timeout=timeout)
    log = run.stdout + run.stderr
    (output / f"{name}.log").write_text(log, encoding="utf-8")
    require(run.returncode != 0 and expected.lower() in log.lower(), f"{name}: invalid input not rejected")
    require("GPU pass profiler" not in log, f"{name}: rejection happened after GPU initialization")
    print(json.dumps({"case": name, "passed": True}), flush=True)
    return {"name": name, "passed": True, "returnCode": run.returncode, "expectedError": expected}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--exe", default="out/build/visual-studio/Release/VulkanSceneRenderer.exe")
    parser.add_argument("--output", default="out/ddgi-features-validation/native")
    parser.add_argument("--timeout", type=float, default=90)
    args = parser.parse_args()
    exe, output = repository_path(args.exe), repository_path(args.output)
    output.mkdir(parents=True, exist_ok=True)
    environment = os.environ.copy()
    environment.pop("GFXRECON_CAPTURE_FILE", None)
    environment.pop("VK_INSTANCE_LAYERS", None)
    scene = output / "scene.gltf"
    write_json(scene, gltf_scene(build_scene("doorway", 400, 360)))
    preset = output / "scene-preset.json"
    preset.unlink(missing_ok=True)
    base = ["--model", str(scene), "--environment-intensity", "0", "--directional-intensity", "0"]
    records, failures = [], []
    try:
        seed = run_capture(exe, output, "fit-save", [*base, "--ddgi", "--ddgi-fit-scene", "--ddgi-fit-probes", "64",
                "--ddgi-rays", "32", "--ddgi-probes-per-frame", "24", "--ddgi-ray-budget", "768",
                "--ddgi-save-preset", str(preset)], environment, args.timeout)
        records.append(seed)
        require(seed["samples"][-1]["ddgi"]["active"] and seed["samples"][-1]["ddgi"]["updatedProbes"] > 0,
                "fitted field fell back instead of updating")
        document = json.loads(preset.read_text(encoding="utf-8"))
        require(document["settings"]["volume"]["counts"] == seed["samples"][-1]["ddgi"]["settings"]["counts"],
                "saved startup placeholder instead of fitted volume")
        for technique in ("deferred-raster", "forward-raster"):
            for order in ("before", "after"):
                flags = ["--ddgi-intensity", ".37", "--ddgi-preset", str(preset)] if order == "before" else ["--ddgi-preset", str(preset), "--ddgi-intensity", ".37"]
                record = run_capture(exe, output, f"preset-{technique}-{order}",
                    [*base, *flags, "--render-technique", technique], environment, args.timeout)
                require(abs(record["samples"][-1]["ddgi"]["settings"]["intensity"] - .37) < 1e-6, "preset override precedence")
                require(record["samples"][-1]["ddgi"]["active"] and record["samples"][-1]["ddgi"]["updatedProbes"] > 0,
                        "loaded preset fell back instead of updating")
                records.append(record)
        detail = copy.deepcopy(document["settings"]["volume"])
        detail["origin"] = [origin + spacing * (count - 1) * .25 for origin, spacing, count in
                            zip(detail["origin"], detail["spacing"], detail["counts"])]
        detail["spacing"] = [spacing * .5 for spacing in detail["spacing"]]
        document["settings"]["additionalVolumes"] = [detail]
        multi = output / "multi-preset.json"
        write_json(multi, document)
        count = math.prod(detail["counts"]) + math.prod(document["settings"]["volume"]["counts"])
        for technique in ("deferred-raster", "forward-raster"):
            record = run_capture(exe, output, f"multi-{technique}", [*base, "--ddgi-preset", str(multi),
                "--render-technique", technique, "--ddgi-debug", "classification"], environment, args.timeout, frames=18)
            check_batch(record["samples"][-1]["ddgi"], count, 24, 32)
            require(record["samples"][-1]["ddgi"]["active"] and record["samples"][-1]["ddgi"]["updatedProbes"] > 0,
                    "packed field fell back instead of updating")
            require(len(record["samples"][-1]["ddgi"]["settings"]["additionalVolumes"]) == 1, "detail volume missing")
            records.append(record)
        sequence = output / "motion.json"
        write_json(sequence, {"schemaVersion": 1, "frames": 32, "sampleFrames": list(range(1, 33)),
            "objectName": "Partition", "objectTranslation": [{"frame": 1, "translation": [0, 0, 0]},
            {"frame": 4, "translation": [0, 0, 0]}, {"frame": 10, "translation": [.8, 0, 0]},
            {"frame": 32, "translation": [.8, 0, 0]}]})
        motion = run_capture(exe, output, "multi-motion", [*base, "--ddgi-preset", str(multi)],
                             environment, args.timeout, sequence=sequence, frames=32)
        check_motion(motion["samples"], count, 24)
        records.append(motion)
        for name, flags in (("disabled", ["--no-ddgi"]), ("unsupported", ["--no-ray-query"]), ("paused", ["--ddgi-pause"])):
            record = run_capture(exe, output, f"multi-{name}", [*base, "--ddgi-preset", str(multi), *flags], environment, args.timeout)
            require(record["samples"][-1]["ddgi"]["updatedProbes"] == 0, f"{name} submitted updates")
            records.append(record)
        for name, flags in (("mismatched-scale", [*base, "--import-scale", "2"]),
                            ("mismatched-model", ["--model", str(scene.parent / "other.gltf")])):
            records.append(invalid_capture(exe, output, name, [*flags, "--ddgi-preset", str(preset)], "do not match", environment, args.timeout))
        for name, patch, expected in (("invalid-version", lambda value: value.update(version=99), "unsupported version"),
                                     ("invalid-volume", lambda value: value["settings"]["volume"].update(counts=[0, 4, 4]), "counts must be nonzero"),
                                     ("invalid-combined-cap", lambda value: value["settings"].update(payloadBudgetBytes=1), "exceeds maxPayloadBytes")):
            invalid = copy.deepcopy(document)
            patch(invalid)
            path = output / f"{name}.json"
            write_json(path, invalid)
            records.append(invalid_capture(exe, output, name, [*base, "--ddgi-preset", str(path)], expected, environment, args.timeout))
    except (AssertionError, OSError, ValueError, KeyError, subprocess.TimeoutExpired) as error:
        failures.append(str(error))
    result = {"schemaVersion": 1, "exeSha256": hashlib.sha256(exe.read_bytes()).hexdigest(),
              "cases": records, "failures": failures, "passed": not failures}
    write_json(output / "feature-results.json", result)
    print(json.dumps({"passed": result["passed"], "cases": len(records), "failures": failures}), flush=True)
    return 0 if result["passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
