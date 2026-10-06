"""Compare actual forward/deferred sky coverage, MSAA and temporal lifecycle."""
from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import subprocess
import sys

try:
    import numpy as np
    from PIL import Image
except ImportError:
    print("SKIP: forward sky captures require numpy and Pillow")
    raise SystemExit(77)


def pixels(path: Path):
    with Image.open(path) as source:
        return np.array(source.convert("RGB"), dtype=np.float64) / 255.0


def sky_mask(image):
    height, width = image.shape[:2]
    mask = np.zeros((height, width), dtype=bool)
    # These regions are outside the coverage fixture's floor, walls and objects.
    mask[:height // 10] = True
    mask[:height // 2, :width // 7] = True
    mask[:height // 2, -width // 7:] = True
    return mask


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--exe", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    if os.environ.get("CONTAINER_RUN_GPU_FORWARD_SKY") != "1":
        print("SKIP: set CONTAINER_RUN_GPU_FORWARD_SKY=1")
        return 77
    exe, output = args.exe.resolve(), args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    (output / "vk_layer_settings.txt").write_text(
        "khronos_validation.validate_sync = true\n", encoding="utf-8")
    model = exe.parent / "models/validation/taa_coverage.gltf"
    assert model.is_file(), model
    results = []

    def capture(name, technique, taa=False, msaa=1, intensity=1, fixture=model, sequence=None):
        screenshot, log = output / (name + ".png"), output / (name + ".log")
        for stale in [screenshot, *output.glob(name + ".frame-*.png")]:
            stale.unlink(missing_ok=True)
        command = [str(exe), "--model", str(fixture), "--hidden", "--no-ui",
                   "--validation", "--display-mode", "lit", "--no-bloom",
                   "--width", "640", "--height", "360", "--fixed-dt", "0.016666667",
                   "--camera-position", "0", "1.8", "6.5", "--camera-target", "0", ".8", "0",
                   "--environment-intensity", str(intensity), "--directional-intensity", "2",
                   "--exposure", ".25", "--msaa", str(msaa),
                   "--taa" if taa else "--no-taa", "--render-technique", technique,
                   "--screenshot", str(screenshot)]
        if sequence:
            command += ["--capture-sequence", str(sequence)]
        with log.open("w", encoding="utf-8") as stream:
            process = subprocess.run(command, cwd=exe.parent,
                env=dict(os.environ, VK_LAYER_SETTINGS_PATH=str(output)),
                stdout=stream, stderr=subprocess.STDOUT, timeout=180)
        messages = log.read_text(encoding="utf-8", errors="replace")
        assert process.returncode == 0 and "VUID-" not in messages and "SYNC-HAZARD" not in messages, log
        assert screenshot.is_file(), log
        return screenshot

    def compare(name, reference, candidate, whole=False):
        expected, actual = pixels(reference), pixels(candidate)
        assert expected.shape == actual.shape, name
        mask = np.ones(expected.shape[:2], dtype=bool) if whole else sky_mask(expected)
        error = np.abs(expected[mask] - actual[mask])
        mae, p99 = float(error.mean()), float(np.percentile(error, 99))
        # Sky sampling is shared; budgets allow only 8-bit quantization and a
        # small number of edge pixels in the isolated alpha/transparency probes.
        assert mae <= (0.005 if whole else 1.0 / 255), (name, mae, p99)
        assert p99 <= (0.03 if whole else 2.0 / 255), (name, mae, p99)
        results.append(dict(name=name, mae=mae, p99=p99, validationClean=True))
        print(name, "MAE", mae, "P99", p99, flush=True)

    forward = {}
    for taa in (False, True):
        reference = capture(f"deferred-taa{int(taa)}", "deferred-raster", taa)
        candidate = capture(f"forward-taa{int(taa)}", "forward-raster", taa)
        compare(f"sky-taa{int(taa)}", reference, candidate)
        assert pixels(candidate)[sky_mask(pixels(candidate))].mean() > 0.08, candidate
        forward[taa] = candidate
    msaa = capture("forward-msaa4", "forward-raster", msaa=4)
    compare("sky-msaa4", forward[False], msaa)
    # Interior wall colors stay covered and invariant under MSAA sky filling.
    one, four = pixels(forward[False]), pixels(msaa)
    wall = (slice(80, 115), slice(205, 265))
    assert np.abs(one[wall] - four[wall]).mean() <= 2.0 / 255, "sky overwrote opaque wall"
    for technique in ("deferred-raster", "forward-raster"):
        dark = pixels(capture("dark-" + technique, technique, intensity=0))
        assert dark[sky_mask(dark)].max() <= 2.0 / 255, "zero-intensity background is not dark"

    # Keep embedded buffers/images but isolate authored layers against the sky.
    original = json.loads(model.read_text(encoding="utf-8"))
    for label, names in (("mask", {"Alpha checker"}), ("glass", {"Glass front", "Glass back"})):
        fixture = json.loads(json.dumps(original))
        fixture["nodes"][0]["children"] = [i for i, node in enumerate(fixture["nodes"])
            if node.get("name") in names]
        if label == "mask":
            material = fixture["materials"][3]
            material.setdefault("extensions", {})["KHR_materials_unlit"] = {}
            fixture.setdefault("extensionsUsed", []).append("KHR_materials_unlit")
        path = output / (label + ".gltf")
        path.write_text(json.dumps(fixture), encoding="utf-8")
        reference = capture(label + "-deferred", "deferred-raster", fixture=path)
        candidate = capture(label + "-forward", "forward-raster", fixture=path)
        compare(label + "-coverage", reference, candidate, whole=True)
        if label == "mask":
            masked_msaa = capture("mask-msaa4", "forward-raster", msaa=4, fixture=path)
            compare("mask-msaa4", reference, masked_msaa, whole=True)

    sequence = output / "sky-lifecycle.json"
    sequence.write_text(json.dumps({"schemaVersion": 1, "frames": 24,
        "sampleFrames": [1, 6, 9, 12, 16, 24],
        "camera": [{"frame": 1, "position": [0, 1.8, 6.5], "target": [0, .8, 0]},
                   {"frame": 24, "position": [.2, 1.9, 6.5], "target": [.2, .9, 0]}],
        "events": [{"frame": 6, "resize": [800, 450]}, {"frame": 9, "resize": [640, 360]},
                   {"frame": 12, "technique": "forward-raster"},
                   {"frame": 16, "technique": "deferred-raster"}]}), encoding="utf-8")
    lifecycle = []
    for technique in ("deferred-raster", "forward-raster"):
        name = "lifecycle-" + technique
        final = capture(name, technique, taa=True, sequence=sequence)
        lifecycle.append(sorted(output.glob(name + ".frame-*.png")) + [final])
    assert len(lifecycle[0]) == len(lifecycle[1]) == 6
    for index, (reference, candidate) in enumerate(zip(*lifecycle)):
        compare(f"lifecycle-{index}", reference, candidate)

    # Reuse the temporal suite's existing coverage probe: jitter must not turn
    # static covered geometry into motion after the HDR sky draw is introduced.
    from temporal_regression import run_capture, linear
    for technique in ("deferred-raster", "forward-raster"):
        files = run_capture(exe, output, "velocity-" + technique,
            sequence="taa_static.json", model="taa_coverage.gltf",
            technique=technique, display="taa-velocity")
        with Image.open(files[-1]) as image:
            region = linear(image)[100:260, 200:440]
        stationary = float(np.mean(np.max(np.abs(region - .5), axis=-1) < .01))
        assert stationary >= .99, (technique, stationary)
        results.append(dict(name="static-coverage-" + technique, stationaryFraction=stationary,
                            validationClean=True))
    (output / "results.json").write_text(json.dumps(results, indent=2) + "\n", encoding="utf-8")
    return 0


if __name__ == "__main__":
    sys.exit(main())
