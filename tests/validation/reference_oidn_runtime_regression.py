"""Validate reference denoising quality, raw EXR identity and viewport history on the renderer."""

import argparse
import json
import os
from pathlib import Path
import subprocess

os.environ.setdefault("OPENCV_IO_ENABLE_OPENEXR", "1")
import cv2
import numpy as np
from PIL import Image

from display_capture_test_support import validation_log_error
from reference_transmission_runtime_regression import fixture as transmission_fixture


def capture_state(stem, suffix):
    image = Path(f"{stem}{suffix}")
    metadata = json.loads(Path(f"{image}.capture.json").read_text(encoding="utf-8"))
    state = json.loads(Path(f"{stem}.telemetry.json").read_text(encoding="utf-8"))["referencePath"]
    assert state["active"] and not metadata["referencePath"]["hasDiagnostics"], state
    return state, metadata


def read_capture(stem, suffix):
    image = Path(f"{stem}{suffix}")
    state, metadata = capture_state(stem, suffix)
    if suffix == ".exr":
        pixels = cv2.imdecode(np.frombuffer(image.read_bytes(), np.uint8), cv2.IMREAD_UNCHANGED)
        assert pixels.dtype == np.float32 and np.isfinite(pixels).all(), image
    else:
        with Image.open(image) as decoded:
            pixels = np.asarray(decoded.convert("RGB"), dtype=np.float32) / 255
    return pixels, state, metadata


def frame_stem(stem, frame, frames):
    return stem if frame == frames else stem.with_name(f"{stem.name}.frame-{frame:04d}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--renderer", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--backend", choices=("oidn", "optix"), default="oidn")
    parser.add_argument("--model", type=Path, help="Cornell-box fixture for a staged executable without assets")
    parser.add_argument("--expect-unavailable", action="store_true")
    args = parser.parse_args()
    renderer, output = args.renderer.resolve(strict=True), args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    (output / "vk_layer_settings.txt").write_text("khronos_validation.validate_sync = true\n", encoding="utf-8")
    model = (args.model or renderer.parent / "models" / "validation" / "cornell_box_local_light.gltf").resolve(strict=True)
    common = [str(renderer), "--model", str(model), "--hidden", "--no-ui", "--validation",
              "--width", "256", "--height", "192", "--no-taa", "--no-bloom", "--msaa", "1",
              "--render-technique", "path-tracing", "--display-mode", "lit", "--path-bounces", "5",
              "--path-seed", "17", "--environment-intensity", "0", "--directional-intensity", "0",
              "--camera-position", "0", "1", "4", "--camera-target", "0", "1", "0",
              "--camera-fov", "42", "--exposure", ".25"]
    environment = dict(os.environ, VK_LAYER_SETTINGS_PATH=str(output))
    backend = args.backend
    label = "OptiX" if backend == "optix" else "Vulkan OIDN"
    display_key = f"{backend}DisplaySamples"

    def select_backend(quality):
        return ["--path-optix"] if backend == "optix" and quality != "off" else ["--path-oidn", quality]

    def run(stem, arguments):
        log = stem.with_suffix(".log")
        with log.open("w", encoding="utf-8") as stream:
            subprocess.run(common + arguments, cwd=renderer.parent, stdout=stream,
                           stderr=subprocess.STDOUT, check=True, timeout=240, env=environment)
        assert not validation_log_error(log.read_text(encoding="utf-8", errors="replace")), log

    def capture(name, samples=4, quality="off", suffix=".png", extra=()):
        stem = output / name
        batch = min(16, samples)
        lag = int(backend == "optix" and quality != "off")
        run(stem, ["--path-spp", str(batch), "--path-samples", str(samples), *select_backend(quality),
                   "--capture-frame", str(samples // batch + lag), "--warmup-frames", "0",
                   "--screenshot", str(stem.with_suffix(suffix)), *extra])
        pixels, state, metadata = read_capture(stem, suffix)
        assert state["attemptedSamples"] == samples, state
        return pixels, state[backend], metadata

    def capture_sequence(name, frames, suffix, arguments, **sequence):
        stem = output / name
        config = stem.with_suffix(".json")
        config.write_text(json.dumps({"schemaVersion": 1, "frames": frames,
                                     "sampleFrames": list(range(1, frames + 1)), **sequence}), encoding="utf-8")
        run(stem, [*arguments, "--capture-sequence", str(config), "--screenshot", str(stem.with_suffix(suffix))])
        return stem

    def check_backend_switches():
        frames, batch, limit = 18, 8, 64
        if backend == "oidn":
            changes = {5: "high", 10: "balanced", 11: "off", 13: "high"}
            events = [{"frame": frame, "referenceOidn": quality} for frame, quality in changes.items()]
        else:
            changes = {5: "high", 11: "off", 13: "high"}
            events = [{"frame": frame, "referenceOptix": quality != "off"} for frame, quality in changes.items()]
        arguments = ["--path-spp", str(batch), "--path-samples", str(limit),
                     "--path-oidn", "off", "--path-denoise-interval", str(batch)]
        control = capture_sequence("switch-control", frames, ".exr", arguments)
        switched = capture_sequence("switch-backend", frames, ".exr", arguments, events=events)

        history = []
        quality = "off"
        for frame in range(1, frames + 1):
            quality = changes.get(frame, quality)
            expected_samples = min(frame * batch, limit)
            control_frame = frame_stem(control, frame, frames)
            switched_frame = frame_stem(switched, frame, frames)
            control_state, _ = capture_state(control_frame, ".exr")
            state, metadata = capture_state(switched_frame, ".exr")
            assert control_state["attemptedSamples"] == state["attemptedSamples"] == expected_samples, (frame, state)
            assert control_state["historyResets"] == state["historyResets"] == 1, (frame, state)
            assert metadata["source"] == "path-trace-mean", (frame, metadata)
            reference = metadata["referencePath"]
            assert reference["attemptedSamplesPerPixel"] == expected_samples and reference["historyResets"] == 1, reference
            assert reference["sampleLimitReached"] == (expected_samples == limit), (frame, reference)
            assert reference[display_key] == 0, (frame, reference)
            assert Path(f"{control_frame}.exr").read_bytes() == Path(f"{switched_frame}.exr").read_bytes(), frame

            denoising = state[backend]
            if backend == "oidn":
                assert denoising["quality"] == quality, (frame, denoising)
            else:
                assert denoising["requested"] == (quality != "off"), (frame, denoising)
            displayed = denoising["displayedSamples"]
            guides = denoising["guideSamples"]
            if quality == "off":
                assert displayed == guides == 0 and denoising["status"] in ("", "off"), (frame, denoising)
            elif args.expect_unavailable:
                assert displayed == guides == 0 and denoising["status"] not in ("", "off", "ready"), (frame, denoising)
            else:
                assert 0 < guides <= expected_samples and 0 <= displayed <= guides, (frame, denoising)
                if guides < expected_samples or (backend == "optix" and frame in changes):
                    assert displayed == 0, (frame, denoising)
                # Enable again at the sample cap, when guide recovery must not add new raw samples.
                if frame in (9, 10, 17, 18):
                    assert displayed == guides == limit, (frame, denoising)
            history.append({"frame": frame, "attemptedSamples": expected_samples,
                            "historyResets": state["historyResets"], backend: denoising})

        return {"rawExrByteExact": True, "frames": history}

    noisy, _, _ = capture("noisy")
    filtered, denoiser, metadata = capture("filtered", quality="high")
    if args.expect_unavailable:
        assert denoiser["displayedSamples"] == 0 and denoiser["status"] not in ("off", "ready"), denoiser
        np.testing.assert_array_equal(noisy, filtered)
        report = {"passed": True, "rawDisplayPixelExact": True, backend: denoiser,
                  "backendSwitches": check_backend_switches()}
        (output / "results.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
        print(f"PASS unavailable {label} preserves raw display and accumulated samples across live switches")
        return

    assert denoiser["displayedSamples"] == 4 and denoiser["allocatedBytes"] > 0 and denoiser["lastGpuMs"] > 0, denoiser
    assert metadata["referencePath"][display_key] == 4, metadata
    raw, _, _ = capture("raw", suffix=".exr")
    guided_raw, _, raw_metadata = capture("guided-raw", quality="high", suffix=".exr")
    assert raw_metadata["source"] == "path-trace-mean" and raw_metadata["referencePath"][display_key] == 0
    np.testing.assert_array_equal(raw, guided_raw)
    converged, _, _ = capture("converged", samples=256)
    noisy_error = float(np.sqrt(np.mean(np.square(noisy - converged))))
    filtered_error = float(np.sqrt(np.mean(np.square(filtered - converged))))
    assert filtered_error < noisy_error, (filtered_error, noisy_error)

    glass_model = output / "transmission.gltf"
    transmission_fixture(glass_model, "smooth")
    glass, glass_denoiser, _ = capture("glass", samples=1, quality="balanced", extra=(
        "--model", str(glass_model), "--camera-position", "0", "0", "0", "--camera-target", "0", "0", "-1"))
    center = glass[16:-16, 16:-16]
    assert glass_denoiser["displayedSamples"] == 1 and float(center.mean()) > .05
    assert np.max(np.std(center, axis=(0, 1))) < .02

    transform = {"node": {"sourceNode": 0}, "matrix": [1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, .2, 0, 0, 1]}
    if backend == "optix":
        move_frame, resize_frame, scene_frame = 4, 7, 10
        expected_samples = (0, 0, 8, 0, 0, 8, 0, 0, 8, 0, 0, 8, 8, 16)
    else:
        move_frame, resize_frame, scene_frame = 3, 5, 8
        expected_samples = (0, 8, 0, 8, 0, 8, 8, 0, 8)
    frames = len(expected_samples)
    stem = capture_sequence("history", frames, ".png",
        ["--path-spp", "4", "--path-samples", "16", *select_backend("fast"), "--path-denoise-interval", "8"],
        camera=[{"frame": move_frame - 1, "position": [0, 1, 4], "target": [0, 1, 0]},
                {"frame": move_frame, "position": [.3, 1, 4], "target": [0, 1, 0]}],
        events=[{"frame": resize_frame, "resize": [288, 208]}, {"frame": scene_frame, "nodeTransform": transform}])
    states = []
    last_reset = 1
    prior_resets = 0
    for frame, expected in enumerate(expected_samples, 1):
        pixels, state, _ = read_capture(frame_stem(stem, frame, frames), ".png")
        assert pixels.shape[:2] == ((192, 256) if frame < resize_frame else (208, 288)), (frame, pixels.shape)
        if frame in (move_frame, resize_frame, scene_frame):
            last_reset = frame
        if frame in (move_frame, scene_frame):
            assert state["historyResets"] == prior_resets + 1, (frame, state)
        assert state["attemptedSamples"] == min(16, 4 * (frame - last_reset + 1)), (frame, state)
        assert state[backend]["displayedSamples"] == expected, (frame, state)
        prior_resets = state["historyResets"]
        states.append(state[backend])
    report = {"noisyRmse": noisy_error, "filteredRmse": filtered_error, backend: denoiser,
              "glass": glass_denoiser, "history": states, "backendSwitches": check_backend_switches()}
    (output / "results.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(f"PASS {label}, exact raw EXRs and history resets (RMSE {noisy_error:.5f} to {filtered_error:.5f})")


if __name__ == "__main__":
    main()
