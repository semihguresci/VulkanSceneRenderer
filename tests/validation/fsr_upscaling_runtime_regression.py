"""Exercise native and Vulkan FSR reconstruction through the viewer capture path."""

import argparse
import json
import os
from pathlib import Path
import statistics
import subprocess

import numpy as np
from PIL import Image

from gltf_extended_materials_runtime_regression import validation_log_error


ROOT = Path(__file__).resolve().parents[2]
RATIOS = {"native": 10, "quality": 15, "balanced": 17, "performance": 20}


def sequence(frames, samples, events=(), moving="Moving cube"):
    return {"schemaVersion": 1, "frames": frames, "sampleFrames": samples,
            "events": events, "objectName": moving,
            "camera": [{"frame": 1, "position": [0, 1.8, 6.5], "target": [0, .8, 0]},
                       {"frame": frames, "position": [.2, 1.8, 6.5], "target": [0, .8, 0]}],
            "objectTranslation": [{"frame": 1, "translation": [-1, 0, 0]},
                                  {"frame": frames - 2, "translation": [1, 0, 0]}]}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--renderer", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--model", type=Path, default=ROOT / "models/validation/taa_scene.gltf")
    parser.add_argument("--suite", choices=("smoke", "full"), default="smoke")
    parser.add_argument("--expect-unavailable", action="store_true",
                        help="Verify the normal renderer when the FSR SDK/runtime is unavailable")
    args = parser.parse_args()
    renderer, output, model = (p.resolve() for p in (args.renderer, args.output, args.model))
    output.mkdir(parents=True, exist_ok=False)
    (output / "vk_layer_settings.txt").write_text("khronos_validation.validate_sync = true\n")
    env = dict(os.environ, VK_LAYER_SETTINGS_PATH=str(output))
    results = {}

    def capture(label, mode, config, *, extent=(640, 360), technique="deferred-raster", display="lit", ui=False):
        sequence_path = output / f"{label}.sequence.json"
        sequence_path.write_text(json.dumps(config), encoding="utf-8")
        image = output / f"{label}.png"
        command = [str(renderer), "--hidden", "--validation", "--no-taa", "--msaa", "1",
                   "--width", str(extent[0]), "--height", str(extent[1]), "--model", str(model),
                   "--import-scale", "1", "--display-mode", display, "--render-technique", technique,
                   "--no-bloom", "--no-ddgi", "--no-ray-query", "--ray-shadows", "raster", "--sky", "plain",
                   "--environment-intensity", ".35", "--directional-intensity", "3", "--exposure", ".5",
                   "--fixed-dt", ".016666667", "--capture-sequence", str(sequence_path), "--screenshot", str(image)]
        if mode is not None:
            command.extend(("--fsr", mode))
        if not ui:
            command.extend(("--no-ui", "--screenshot-viewport"))
        run = subprocess.run(command, cwd=renderer.parent, env=env, capture_output=True, text=True,
                             errors="replace", timeout=240)
        log = run.stdout + run.stderr
        (output / f"{label}.log").write_text(log, encoding="utf-8")
        assert run.returncode == 0 and not validation_log_error(log), (label, log)

        states, images = {}, {}
        for frame in config["sampleFrames"]:
            name = label if frame == config["frames"] else f"{label}.frame-{frame:04d}"
            states[frame] = json.loads((output / f"{name}.telemetry.json").read_text(encoding="utf-8"))
            with Image.open(output / f"{name}.png") as readback:
                assert list(readback.size) == states[frame]["fsr"]["displayExtent"], states[frame]["fsr"]
                images[frame] = np.array(readback.convert("RGB"))
        timed = [p["gpuMs"] for state in states.values() for p in state["passes"]
                 if p["name"] == "TemporalResolve" and p["gpuTimed"]]
        results[label] = {"states": {frame: state["fsr"] for frame, state in states.items()},
                          "medianResolveGpuMs": statistics.median(timed) if timed else None,
                          "medianKnownGpuMs": statistics.median(state["gpuKnownMs"] for state in states.values()),
                          "temporalAllocationBytes": states[config["sampleFrames"][-1]]["taa"]["allocatedImageBytes"],
                          "command": command}
        print(f"{label}: {len(images)} Vulkan-validation-clean captures", flush=True)
        return states, images

    def check_mode(state, requested, effective, extent):
        fsr = state["fsr"]
        assert fsr["requested"] == requested and fsr["mode"] == effective, fsr
        assert fsr["active"] == (effective != "native") and fsr["displayExtent"] == list(extent), fsr
        assert fsr["renderExtent"] == [value * 10 // RATIOS[effective] for value in extent], fsr
        if fsr["active"]:
            assert fsr["status"] == "FSR 3.1.6 is ready (SDK 2.4.0)", fsr
            assert any(p["name"] == "TemporalResolve" and p["active"] for p in state["passes"]), fsr

    size = (640, 360)
    motion = sequence(24, [1, 8, 13, 16, 18, 24], [{"frame": 12, "exposure": .8}, {"frame": 15, "reset": True}])
    motion["camera"] = [motion["camera"][0],
        {"frame": 17, "position": [.2, 1.8, 6.5], "target": [0, .8, 0]},
        {"frame": 18, "position": [2, 2.5, -6], "target": [0, .8, 0]}]
    baseline, native = capture("native", None, motion, extent=size)
    requested, upscaled = capture("quality", "quality", motion, extent=size)
    expected = "native" if args.expect_unavailable else "quality"
    for frame, state in requested.items():
        check_mode(baseline[frame], "native", "native", size)
        check_mode(state, "quality", expected, size)
    if args.expect_unavailable:
        assert all(np.array_equal(native[frame], upscaled[frame]) for frame in native)
    else:
        compare = {frame: float(np.mean(np.abs(upscaled[frame].astype(float) - native[frame])))
                   for frame in native}
        assert max(compare.values()) < 25, compare
        assert not np.array_equal(upscaled[8], native[8]), "FSR output did not change with reconstruction"
        assert requested[16]["taa"]["epoch"] > requested[13]["taa"]["epoch"], requested[16]["taa"]
        assert requested[18]["taa"]["epoch"] > requested[16]["taa"]["epoch"], requested[18]["taa"]
        assert requested[18]["taa"]["resetReason"] == "camera cut", requested[18]["taa"]
        results["quality"]["nativeMae8Bit"] = compare

        switched_size = (720, 400)
        events = [{"frame": 4, "fsr": "balanced"}, {"frame": 7, "fsr": "performance"},
                  {"frame": 10, "fsr": "native"}, {"frame": 13, "fsr": "quality"},
                  {"frame": 15, "reload": str(model)},
                  {"frame": 16, "resize": list(switched_size)}, {"frame": 19, "samples": 4},
                  {"frame": 22, "samples": 1}]
        lifecycle = sequence(24, [1, 4, 7, 10, 13, 15, 17, 20, 24], events)
        switched, _ = capture("switch-resize-msaa", "quality", lifecycle, extent=size)
        expected_modes = ["quality", "balanced", "performance", "native", "quality", "quality", "quality", "native", "quality"]
        for (frame, state), mode in zip(switched.items(), expected_modes):
            check_mode(state, "quality" if frame == 20 else mode, mode, switched_size if frame > 16 else size)
        epochs = [state["taa"]["epoch"] for state in switched.values()]
        assert all(b > a for a, b in zip(epochs, epochs[1:])), epochs

        if args.suite == "full":
            larger = (1280, 720)
            forward = sequence(24, [8, 16, 24], moving="Glass front")
            _, transparent_native = capture("forward-native", None, forward, extent=larger, technique="forward-raster")
            state, transparent_fsr = capture("forward-performance", "performance", forward,
                                             extent=larger, technique="forward-raster")
            for frame, receipt in state.items():
                check_mode(receipt, "performance", "performance", larger)
                assert np.mean(np.abs(transparent_fsr[frame].astype(float) - transparent_native[frame])) < 25
            for name in ("taa-velocity", "taa-reactive"):
                state, diagnostic = capture(name, "quality", forward, display=name)
                check_mode(state[24], "quality", "quality", size)
                assert float(np.std(diagnostic[24])) > 10, name
            ui_state, _ = capture("ui-quality", "quality", motion, ui=True)
            check_mode(ui_state[24], "quality", "quality", size)
            ui_receipt = json.loads((output / "ui-quality.png.capture.json").read_text(encoding="utf-8"))
            assert ui_receipt["includesUi"], ui_receipt

    report = {"passed": True, "gpu": requested[24]["gpu"], "captures": results}
    (output / "results.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")


if __name__ == "__main__":
    main()
