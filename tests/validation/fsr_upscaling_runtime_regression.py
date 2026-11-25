"""Exercise native and Vulkan FSR reconstruction through the viewer capture path."""

import argparse
import json
import os
from pathlib import Path
import re
import statistics
import struct
import subprocess

import numpy as np
from PIL import Image

from gltf_extended_materials_runtime_regression import validation_log_error
from graphics_concurrency_benchmark import file_identity, validation_environment


ROOT = Path(__file__).resolve().parents[2]
RATIOS = {"native": 10, "quality": 15, "balanced": 17, "performance": 20}
SDK = "2.4.0"
X64_PE_MACHINE = 0x8664
RUNTIME_HASHES = {
    "amd_fidelityfx_loader_vk.dll": "38a56a01fb3565c0757d9f5c1e4e69de4b65e5f2e5769b323b200dc1f142bab9",
    "amd_fidelityfx_upscaler_vk.dll": "4a7e895af6e0b615d40a84d59eb8c2b1054ad91cdd0c739567940270d51bdab7",
}
FALLBACK_REASONS = {
    "sdk-disabled": {"Built without the Vulkan FSR SDK"},
    "missing-runtime": {f"The pinned FSR SDK {SDK} loader/upscaler DLLs are unavailable beside the executable"},
    "unsupported-device": {
        "GPU lacks the required Vulkan FSR features",
        "GPU lacks the required Vulkan FSR image formats",
        "GPU lacks the exact image formats required by the analytical FSR provider",
    },
}
VALIDATION_INSERTED = re.compile(r'Insert instance layer "VK_LAYER_KHRONOS_validation" \(([^\r\n]+)\)')
VALIDATION_SETTINGS = ("khronos_validation.validate_sync = true\n"
                       "khronos_validation.duplicate_message_limit = 1000\n")


def sequence(frames, samples, events=(), moving="Moving cube"):
    return {"schemaVersion": 1, "frames": frames, "sampleFrames": samples,
            "events": events, "objectName": moving,
            "camera": [{"frame": 1, "position": [0, 1.8, 6.5], "target": [0, .8, 0]},
                       {"frame": frames, "position": [.2, 1.8, 6.5], "target": [0, .8, 0]}],
            "objectTranslation": [{"frame": 1, "translation": [-1, 0, 0]},
                                  {"frame": frames - 2, "translation": [1, 0, 0]}]}


def pe_machine(path):
    with path.open("rb") as stream:
        if stream.read(2) != b"MZ":
            return None
        stream.seek(0x3c)
        stream.seek(int.from_bytes(stream.read(4), "little"))
        header = stream.read(6)
    if len(header) != 6 or header[:4] != b"PE\0\0":
        return None
    return struct.unpack_from("<H", header, 4)[0]


def runtime_receipt(renderer, enabled):
    runtime = {name: file_identity(renderer.parent / name) if (renderer.parent / name).is_file() else None
               for name in RUNTIME_HASHES}
    shader = renderer.parent / "spv_shaders/fsr_inputs.comp.spv"
    machine = pe_machine(renderer)
    for name, info in runtime.items():
        if info:
            info["peMachine"] = pe_machine(Path(info["path"]))
        if enabled:
            assert info and info["sha256"] == RUNTIME_HASHES[name] and info["peMachine"] == X64_PE_MACHINE, (name, info)
    if enabled:
        assert machine == X64_PE_MACHINE and shader.is_file(), (renderer, machine, shader)
    return {"renderer": {**file_identity(renderer), "peMachine": machine}, "sdkPin": SDK, "backend": "Vulkan",
            "signedRuntime": runtime,
            "inputShader": file_identity(shader) if shader.is_file() else None}


def taa_switch_sequence():
    return {"schemaVersion": 1, "frames": 20, "sampleFrames": [4, 8, 12, 16, 20],
            "camera": [{"frame": 1, "position": [0, 1.8, 6.5], "target": [0, .8, 0]}],
            "events": [{"frame": 1, "taa": True}, {"frame": 5, "fsr": "quality"}, {"frame": 9, "taa": False},
                       {"frame": 13, "fsr": "native"}, {"frame": 17, "taa": True}]}


def temporal_resolve(state):
    return any(p["name"] == "TemporalResolve" and p["active"] for p in state["passes"])


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--renderer", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--model", type=Path, default=ROOT / "models/validation/taa_scene.gltf")
    parser.add_argument("--suite", choices=("smoke", "full"), default="smoke")
    fallback = parser.add_mutually_exclusive_group()
    fallback.add_argument("--expect-unavailable", action="store_true",
                          help="Verify the normal renderer when the FSR SDK/runtime is unavailable")
    fallback.add_argument("--expect-fallback", choices=FALLBACK_REASONS, help="Require this exact native fallback cause")
    parser.add_argument("--expect-provider", default="3.1.6", help="Provider version returned by the signed SDK")
    parser.add_argument("--validation-manifest", type=Path, help="Use and identify this Vulkan validation layer")
    args = parser.parse_args()
    unavailable = args.expect_unavailable or args.expect_fallback is not None
    renderer, output, model = (p.resolve() for p in (args.renderer, args.output, args.model))
    runtime = runtime_receipt(renderer, enabled=not unavailable)
    if args.expect_fallback == "missing-runtime":
        assert any(info is None for info in runtime["signedRuntime"].values()), runtime
    output.mkdir(parents=True, exist_ok=False)
    env = dict(os.environ, VK_LAYER_SETTINGS_PATH=str(output), VK_LOADER_DEBUG="layer")
    validation = validation_environment(args.validation_manifest, output, env) if args.validation_manifest else None
    (output / "vk_layer_settings.txt").write_text(VALIDATION_SETTINGS, encoding="ascii")
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
        inserted = VALIDATION_INSERTED.search(log)
        assert inserted, f"Vulkan validation layer was not confirmed for {label}"
        layer = file_identity(renderer.parent / Path(inserted[1]))
        if validation:
            assert layer == validation["library"], (label, layer, validation["library"])

        states, images = {}, {}
        for frame in config["sampleFrames"]:
            name = label if frame == config["frames"] else f"{label}.frame-{frame:04d}"
            states[frame] = json.loads((output / f"{name}.telemetry.json").read_text(encoding="utf-8"))
            assert states[frame]["validationEnabled"], (label, frame)
            with Image.open(output / f"{name}.png") as readback:
                assert list(readback.size) == states[frame]["fsr"]["displayExtent"], states[frame]["fsr"]
                images[frame] = np.array(readback.convert("RGB"))
        timed = [p["gpuMs"] for state in states.values() for p in state["passes"]
                 if p["name"] == "TemporalResolve" and p["gpuTimed"]]
        results[label] = {"states": {frame: state["fsr"] for frame, state in states.items()},
                          "temporalStates": {frame: {"taaConfigured": state["taa"]["enabled"],
                                                     "resolveActive": temporal_resolve(state),
                                                     "epoch": state["taa"]["epoch"],
                                                     "jitterUv": state["camera"]["jitterUv"]}
                                             for frame, state in states.items()},
                          "validationLayer": layer,
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
            assert fsr["status"] == f"FSR {args.expect_provider} is ready (SDK {SDK})", fsr
            assert temporal_resolve(state), fsr

    def check_fallback(state):
        assert state["fsr"]["status"], state["fsr"]
        if args.expect_fallback:
            assert state["fsr"]["status"] in FALLBACK_REASONS[args.expect_fallback], state["fsr"]

    size = (640, 360)
    motion = sequence(24, [1, 8, 13, 16, 18, 24], [{"frame": 12, "exposure": .8}, {"frame": 15, "reset": True}])
    motion["camera"] = [motion["camera"][0],
        {"frame": 17, "position": [.2, 1.8, 6.5], "target": [0, .8, 0]},
        {"frame": 18, "position": [2, 2.5, -6], "target": [0, .8, 0]}]
    baseline, native = capture("native", None, motion, extent=size)
    requested, upscaled = capture("quality", "quality", motion, extent=size)
    expected = "native" if unavailable else "quality"
    for frame, state in requested.items():
        check_mode(baseline[frame], "native", "native", size)
        check_mode(state, "quality", expected, size)
    if unavailable:
        for state in requested.values():
            check_fallback(state)
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
            for frame, frame_state in state.items():
                check_mode(frame_state, "performance", "performance", larger)
                assert np.mean(np.abs(transparent_fsr[frame].astype(float) - transparent_native[frame])) < 25
            for name in ("taa-velocity", "taa-reactive"):
                state, diagnostic = capture(name, "quality", forward, display=name)
                check_mode(state[24], "quality", "quality", size)
                assert float(np.std(diagnostic[24])) > 10, name
            ui_state, _ = capture("ui-quality", "quality", motion, ui=True)
            check_mode(ui_state[24], "quality", "quality", size)
            ui_receipt = json.loads((output / "ui-quality.png.capture.json").read_text(encoding="utf-8"))
            assert ui_receipt["includesUi"], ui_receipt

    taa_states, taa_images = capture("taa-fsr-switch", None, taa_switch_sequence(), extent=size)
    for frame, state in taa_states.items():
        quality_requested = frame in (8, 12)
        configured_taa = frame in (4, 8, 20)
        fsr_on = quality_requested and not unavailable
        temporal_on = configured_taa or fsr_on
        check_mode(state, "quality" if quality_requested else "native", "quality" if fsr_on else "native", size)
        if quality_requested and unavailable:
            check_fallback(state)
        assert state["taa"]["enabled"] == configured_taa and temporal_resolve(state) == temporal_on, state["taa"]
        assert any(state["camera"]["jitterUv"][:2]) == temporal_on, state["camera"]
    if not unavailable:
        assert taa_states[4]["taa"]["epoch"] < taa_states[8]["taa"]["epoch"] == taa_states[12]["taa"]["epoch"]
        assert taa_states[12]["taa"]["epoch"] < taa_states[16]["taa"]["epoch"]
        assert taa_states[8]["taa"]["allocatedImageBytes"] == taa_states[12]["taa"]["allocatedImageBytes"]
    assert not np.array_equal(taa_images[16], taa_images[20]), "Native TAA did not change the reconstruction"

    assert runtime == runtime_receipt(renderer, enabled=not unavailable), "The installed runtime changed during the captures"
    device = {key: requested[24][key] for key in ("gpu", "vendorId", "deviceId", "driverVersion", "vulkanApiVersion")}
    report = {"passed": True, "gpu": device["gpu"], "device": device, "runtime": runtime,
              "validationManifest": validation["manifest"] if validation else None,
              "validationSettings": VALIDATION_SETTINGS.splitlines(),
              "activeProvider": None if unavailable else args.expect_provider,
              "expectedFallback": args.expect_fallback or ("unspecified" if unavailable else None), "captures": results}
    (output / "results.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")


if __name__ == "__main__":
    main()
