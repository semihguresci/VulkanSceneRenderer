"""Exercise Vulkan DLSS Super Resolution and its native fallback through the viewer."""

import argparse
import json
import os
from pathlib import Path
import statistics
import subprocess

import numpy as np
from PIL import Image

from fsr_upscaling_runtime_regression import sequence
from gltf_extended_materials_runtime_regression import validation_log_error


ROOT = Path(__file__).resolve().parents[2]
EXTENT = (640, 360)
RESIZED = (720, 400)
GLASS_EXTENT = (1280, 720)


def check_mode(state, requested, effective, extent):
    dlss = state["dlss"]
    assert dlss["requested"] == requested and dlss["mode"] == effective, dlss
    assert dlss["active"] == (effective != "native") and dlss["displayExtent"] == list(extent), dlss
    if dlss["active"]:
        assert all(0 < source < target for source, target in zip(dlss["renderExtent"], extent)), dlss
        assert not state["fsr"]["active"], state["fsr"]
        assert any(p["name"] == "TemporalResolve" and p["active"] for p in state["passes"]), dlss
    elif not state["fsr"]["active"]:
        assert dlss["renderExtent"] == list(extent), dlss
        if requested != "native":
            assert dlss["status"], dlss


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--renderer", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--model", type=Path, default=ROOT / "models/validation/taa_scene.gltf")
    parser.add_argument("--expect-unavailable", action="store_true", help="Require bit-identical native fallback")
    parser.add_argument("--expect-fsr", action="store_true", help="Require active FSR during the backend switch")
    args = parser.parse_args()
    renderer, output, model = (path.resolve() for path in (args.renderer, args.output, args.model))
    output.mkdir(parents=True, exist_ok=False)
    (output / "vk_layer_settings.txt").write_text("khronos_validation.validate_sync = true\n", encoding="utf-8")
    env = dict(os.environ, VK_LAYER_SETTINGS_PATH=str(output))
    captures = {}

    def capture(label, mode, config, *, technique="deferred-raster", extent=EXTENT):
        sequence_path = output / f"{label}.sequence.json"
        sequence_path.write_text(json.dumps(config), encoding="utf-8")
        image = output / f"{label}.png"
        command = [str(renderer), "--hidden", "--validation", "--no-ui", "--screenshot-viewport", "--no-taa",
                   "--msaa", "1", "--width", str(extent[0]), "--height", str(extent[1]), "--model", str(model),
                   "--import-scale", "1", "--display-mode", "lit", "--render-technique", technique,
                   "--no-bloom", "--no-ddgi", "--no-ray-query", "--ray-shadows", "raster", "--sky", "plain",
                   "--environment-intensity", ".35", "--directional-intensity", "3", "--exposure", ".5",
                   "--fixed-dt", ".016666667", "--capture-sequence", str(sequence_path), "--screenshot", str(image)]
        if mode is not None:
            command.extend(("--dlss", mode))
        log = output / f"{label}.log"
        with log.open("w", encoding="utf-8") as stream:
            run = subprocess.run(command, cwd=renderer.parent, env=env, stdout=stream, stderr=subprocess.STDOUT, timeout=240)
        log_text = log.read_text(encoding="utf-8", errors="replace")
        assert run.returncode == 0 and not validation_log_error(log_text), log
        assert "Undefined-Value-StorageImage-FormatMismatch-ImageView" not in log_text, log

        states, images, frames = {}, {}, {}
        for frame in config["sampleFrames"]:
            name = label if frame == config["frames"] else f"{label}.frame-{frame:04d}"
            receipt = output / f"{name}.telemetry.json"
            frame_image = output / f"{name}.png"
            state = json.loads(receipt.read_text(encoding="utf-8"))
            states[frame] = state
            with Image.open(frame_image) as readback:
                assert list(readback.size) == state["dlss"]["displayExtent"], state["dlss"]
                images[frame] = np.asarray(readback.convert("RGB"))
            assert float(np.std(images[frame])) > 8, frame_image
            frames[frame] = {"state": state["dlss"], "fsr": state["fsr"], "image": str(frame_image),
                             "telemetry": str(receipt), "historyEpoch": state["taa"]["epoch"]}
        timed = [p["gpuMs"] for state in states.values() for p in state["passes"]
                 if p["name"] == "TemporalResolve" and p["gpuTimed"]]
        captures[label] = {"frames": frames, "medianResolveGpuMs": statistics.median(timed) if timed else None,
                           "medianKnownGpuMs": statistics.median(state["gpuKnownMs"] for state in states.values()),
                           "temporalAllocationBytes": states[config["sampleFrames"][-1]]["taa"]["allocatedImageBytes"],
                           "command": command, "log": str(log)}
        print(f"{label}: {len(frames)} Vulkan-validation-clean captures", flush=True)
        return states, images

    motion = sequence(24, [1, 8, 13, 16, 18, 24], [{"frame": 12, "exposure": .8}, {"frame": 15, "reset": True}])
    motion["camera"] = [motion["camera"][0],
                        {"frame": 17, "position": [.2, 1.8, 6.5], "target": [0, .8, 0]},
                        {"frame": 18, "position": [2, 2.5, -6], "target": [0, .8, 0]}]
    native_states, native = capture("deferred-native", None, motion)
    quality, rebuilt = capture("deferred-quality", "quality", motion)
    for frame in quality:
        check_mode(native_states[frame], "native", "native", EXTENT)
        check_mode(quality[frame], "quality", "native" if args.expect_unavailable else "quality", EXTENT)

    glass = sequence(24, [8, 16, 24], moving="Glass front")
    glass["camera"] = glass["camera"][:1]
    native_forward, native_glass = capture("forward-native", "native", glass, technique="forward-raster", extent=GLASS_EXTENT)
    forward, rebuilt_glass = capture("forward-performance", "performance", glass,
                                     technique="forward-raster", extent=GLASS_EXTENT)
    for frame, state in forward.items():
        check_mode(native_forward[frame], "native", "native", GLASS_EXTENT)
        check_mode(state, "performance", "native" if args.expect_unavailable else "performance", GLASS_EXTENT)
        assert any(p["name"] == "OitResolve" and p["active"] for p in state["passes"]), state["passes"]

    for label, normal, candidate in (("deferred-quality", native, rebuilt), ("forward-performance", native_glass, rebuilt_glass)):
        if args.expect_unavailable:
            assert all(np.array_equal(normal[frame], candidate[frame]) for frame in normal), label
            captures[label]["nativePixelExact"] = True
        else:
            mae = {frame: float(np.mean(np.abs(candidate[frame].astype(float) - normal[frame]))) for frame in normal}
            assert max(mae.values()) < 25 and any(error > 0 for error in mae.values()), (label, mae)
            captures[label]["nativeMae8Bit"] = mae

    # Only the glass moves. The interior of the native change mask isolates
    # the layer's appearance/disocclusion from subpixel edges and the sky.
    glass_delta = native_glass[24].astype(np.float32) - native_glass[8]
    changed = np.max(np.abs(glass_delta), axis=2) >= 10
    interior = changed[2:-2, 2:-2] & changed[2:-2, :-4] & changed[2:-2, 4:] & changed[:-4, 2:-2] & changed[4:, 2:-2]
    assert np.count_nonzero(interior) > 256, "Moving glass did not cover enough interior pixels"
    rebuilt_delta = rebuilt_glass[24].astype(np.float32) - rebuilt_glass[8]
    glass_error = float(np.mean(np.abs((rebuilt_delta - glass_delta)[2:-2, 2:-2][interior])))
    glass_signal = float(np.mean(np.abs(glass_delta[2:-2, 2:-2][interior])))
    assert glass_error < .55 * glass_signal, (glass_error, glass_signal)
    captures["forward-performance"]["glassMotion"] = {"pixels": int(np.count_nonzero(interior)),
                                                       "nativeDelta8Bit": glass_signal, "deltaMae8Bit": glass_error}

    if not args.expect_unavailable:
        assert quality[16]["taa"]["epoch"] > quality[13]["taa"]["epoch"], quality[16]["taa"]
        assert quality[18]["taa"]["epoch"] > quality[16]["taa"]["epoch"], quality[18]["taa"]
        assert quality[18]["taa"]["resetReason"] == "camera cut", quality[18]["taa"]
        events = [{"frame": 4, "dlss": "balanced"}, {"frame": 7, "dlss": "performance"},
                  {"frame": 10, "dlss": "native"}, {"frame": 13, "dlss": "quality"},
                  {"frame": 15, "reload": str(model)}, {"frame": 16, "resize": list(RESIZED)},
                  {"frame": 19, "samples": 4}, {"frame": 22, "samples": 1},
                  {"frame": 25, "fsr": "quality"}, {"frame": 28, "dlss": "balanced"}]
        samples = [1, 4, 7, 10, 13, 15, 17, 20, 24, 26, 30]
        switched, _ = capture("switch-resize-msaa", "quality", sequence(30, samples, events))
        requested = ("quality", "balanced", "performance", "native", "quality", "quality", "quality", "quality",
                     "quality", "native", "balanced")
        for (frame, state), mode in zip(switched.items(), requested):
            check_mode(state, mode, "native" if frame == 20 else mode, RESIZED if frame > 16 else EXTENT)
        epochs = [state["taa"]["epoch"] for state in switched.values()]
        assert all(second > first for first, second in zip(epochs, epochs[1:])), epochs
        sizes = [switched[frame]["dlss"]["renderExtent"] for frame in (1, 4, 7)]
        assert all(quality_dim > balanced > performance for quality_dim, balanced, performance in zip(*sizes)), sizes
        fsr = switched[26]["fsr"]
        assert fsr["requested"] == "quality", fsr
        assert fsr["active"] or (not args.expect_fsr and fsr["status"]), fsr

    report = {"passed": True, "gpu": quality[24]["gpu"], "sdkFallbackExpected": args.expect_unavailable, "captures": captures}
    (output / "results.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")


if __name__ == "__main__":
    main()
