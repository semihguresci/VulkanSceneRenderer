"""Benchmark Native, TAA and Vulkan FSR against same-frame scene controls."""

import argparse
import json
import os
from pathlib import Path
import subprocess

import numpy as np
from PIL import Image

from display_capture_test_support import validation_log_error
from fsr_benchmark_scenes import benchmark_scene, benchmark_sequence, compare_images, scene_identity
from fsr_upscaling_runtime_regression import (
    RATIOS, SDK, VALIDATION_INSERTED, VALIDATION_SETTINGS, runtime_receipt, temporal_resolve,
)
from graphics_concurrency_benchmark import file_identity, require, validation_environment
from large_scene_asset_benchmark import capture_paths, frame_timings, verify_visible_image


MODES = ("native", "taa", "quality", "balanced", "performance")
DEVICE_FIELDS = ("gpu", "vendorId", "deviceId", "driverVersion", "vulkanApiVersion")


def parse_extent(value):
    try:
        width, height = (int(component) for component in value.lower().split("x"))
    except ValueError:
        raise argparse.ArgumentTypeError("Use WIDTHxHEIGHT, for example 1920x1080") from None
    if width < 4 or height < 4:
        raise argparse.ArgumentTypeError("Both display dimensions must be at least four pixels")
    return width, height


def record_state(state, scene, extent, mode, args):
    label = f"{scene['name']} {extent} {mode}"
    fsr = state["fsr"]
    quality = "native" if mode == "taa" else mode
    active = quality != "native"
    render = [axis * 10 // RATIOS[quality] for axis in extent]
    require(state["technique"] == args.technique and state["msaaSamples"] == 1, f"Wrong raster profile: {label}")
    require(state["resolution"] == fsr["displayExtent"] == list(extent) and fsr["renderExtent"] == render,
            f"Wrong render/display extent: {label}: {fsr}")
    require(fsr["requested"] == fsr["mode"] == quality and fsr["active"] == active,
            f"Wrong effective upscaler: {label}: {fsr}")
    require(state["taa"]["enabled"] == (mode == "taa") and not state["dlss"]["active"] and
            temporal_resolve(state) == (mode != "native"), f"Wrong temporal resolve policy: {label}")
    require(state["serializedConcurrency"], f"Expected the same serialized graphics policy: {label}")
    require(state["validationEnabled"] == (args.validation_manifest is not None), f"Unexpected Vulkan layer profile: {label}")

    sdk_bytes = fsr["sdkAllocatedBytes"]
    aliasable_bytes = fsr["sdkAliasableBytes"]
    if active:
        require(fsr["status"] == f"FSR {args.expect_provider} is ready (SDK {SDK})", f"Wrong FSR provider: {label}")
        if args.expect_provider == "3.1.6" or sdk_bytes is not None:
            require(type(sdk_bytes) is int and sdk_bytes > 0 and type(aliasable_bytes) is int and
                    0 <= aliasable_bytes <= sdk_bytes, f"Invalid live SDK allocation: {label}: {fsr}")
    else:
        require(sdk_bytes is None and aliasable_bytes is None, f"Unexpected SDK allocation on a native frame: {label}")

    scene_state = state["scene"]
    objects = scene_state[f"{scene['provider']}ObjectCount"]
    require(objects >= scene["minimumObjectCount"], f"The representative scene did not load: {label}: {objects}")
    vma = state["shaderReload"]
    temporal_bytes = state["taa"]["allocatedImageBytes"]
    require(0 < vma["vmaDeviceLocalBytes"] <= vma["vmaBytes"] and vma["vmaAllocations"] > 0,
            f"Missing VMA allocation snapshot: {label}")
    require((temporal_bytes == 0 if mode == "native" else 0 < temporal_bytes <= vma["vmaBytes"]),
            f"Invalid engine temporal allocation: {label}: {temporal_bytes}")
    return {"camera": state["camera"]["unjitteredViewProjColumns"], "exposure": state["exposure"],
            "objectCount": objects, "epoch": state["taa"]["epoch"], "build": state["build"],
            "renderExtent": render, "displayExtent": list(extent), "fsr": fsr,
            "vmaBytes": vma["vmaBytes"], "vmaDeviceLocalBytes": vma["vmaDeviceLocalBytes"],
            "vmaAllocationCount": vma["vmaAllocations"], "temporalImageBytes": temporal_bytes,
            "adapterTelemetry": state.get("gpuHardware")}


def run_case(args, scene, extent, mode, env, validation):
    output = args.output / scene["name"] / f"{extent[0]}x{extent[1]}" / mode
    output.mkdir(parents=True)
    image = output / "scene.png"
    sequence = output / "sequence.json"
    # Leave four ordinary submissions after the steady interval to retire
    # queries before the final capture and telemetry snapshot.
    frames = args.warmup + args.samples + 5
    plan = benchmark_sequence(scene, frames, (args.warmup, frames), (), mode == "taa")
    sequence.write_text(json.dumps(plan, indent=2) + "\n", encoding="utf-8")
    command = [str(args.renderer), *scene["arguments"], "--hidden", "--no-ui", "--no-taa",
               "--validation" if validation else "--no-validation", "--width", str(extent[0]), "--height", str(extent[1]),
               "--render-technique", args.technique, "--display-mode", "lit", "--msaa", "1", "--serialize-graphics",
               "--geometry-lod-error", "0", "--geometry-upload-kb", "0", "--no-bloom", "--no-ddgi", "--no-ray-query",
               "--ray-shadows", "raster", "--gpu-hardware-telemetry", "--fixed-dt", ".016666667",
               "--fsr", "native" if mode == "taa" else mode, "--capture-sequence", str(sequence),
               "--screenshot-viewport", "--screenshot", str(image)]
    log_path = output / "runtime.log"
    print(f"{scene['name']} {extent[0]}x{extent[1]} {mode}", flush=True)
    with log_path.open("w", encoding="utf-8") as log:
        run = subprocess.run(command, cwd=args.renderer.parent, env=env, stdout=log, stderr=subprocess.STDOUT, timeout=300)
    messages = log_path.read_text(encoding="utf-8", errors="replace")
    require(run.returncode == 0 and not validation_log_error(messages), f"Renderer failed. See {log_path}")
    if validation:
        match = VALIDATION_INSERTED.search(messages)
        require(match and file_identity(args.renderer.parent / Path(match[1])) == validation["library"],
                f"The pinned validation layer was not used. See {log_path}")

    captures = {}
    images = {}
    for frame in plan["sampleFrames"]:
        screenshot, telemetry = capture_paths(image, frame, plan)
        state = json.loads(telemetry.read_bytes())
        capture = record_state(state, scene, extent, mode, args)
        capture["image"] = verify_visible_image(screenshot, extent)
        capture["telemetry"] = str(telemetry)
        metadata = json.loads(Path(f"{screenshot}.capture.json").read_bytes())
        require(metadata["source"] == "viewport" and not metadata["includesUi"], f"Unexpected UI in {screenshot}")
        with Image.open(screenshot) as pixels:
            images[frame] = np.array(pixels.convert("RGB"))
        captures[frame] = capture

    require(len({capture["epoch"] for capture in captures.values()}) == 1, f"History reset during camera motion: {log_path}")
    timing = frame_timings(state, args.warmup + args.samples + 2, args.samples)
    rows = [row for row in state["frameHistory"] if row["frameIndex"] in timing["frameIndices"]]
    serials = [row["gpuRecordingSerial"] for row in rows if row["gpuFrameIndex"] == row["frameIndex"]]
    require(timing["matchedGpuFrameCount"] == args.samples and len(serials) == args.samples and
            all(type(serial) is int for serial in serials) and serials == sorted(set(serials)) and
            timing["gpuGraphicsMs"]["frameIndices"] == timing["frameIndices"] and
            timing["gpuGraphicsMs"]["stats"]["median"] > 0, f"Missing or duplicated GPU timestamps: {log_path}")
    device = {field: state[field] for field in DEVICE_FIELDS}
    print(f"  Median graphics {timing['gpuGraphicsMs']['stats']['median']:.3f} ms", flush=True)
    return {"command": command, "captures": captures, "timing": timing, "device": device}, images


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--renderer", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--sponza", required=True, type=Path, help="Original Sponza.gltf")
    parser.add_argument("--gltf", required=True, type=Path, help="Original ABeautifulGame.gltf")
    parser.add_argument("--bim", required=True, type=Path, help="Native Tekla House IFCX or IFC")
    parser.add_argument("--extent", action="append", type=parse_extent, help="Presentation size, repeatable. Defaults to 1080p and 1440p.")
    parser.add_argument("--technique", choices=("deferred-raster", "forward-raster"), default="deferred-raster")
    parser.add_argument("--warmup", type=int, default=48)
    parser.add_argument("--samples", type=int, default=32)
    parser.add_argument("--expect-provider", default="3.1.6")
    parser.add_argument("--validation-manifest", type=Path, help="Enable synchronization validation with this manifest")
    args = parser.parse_args()
    if args.warmup < 1 or not 1 <= args.samples <= 235:
        parser.error("Use at least one warm-up frame and one to 235 samples within the telemetry history")
    args.renderer, args.output = args.renderer.resolve(), args.output.resolve()
    extents = args.extent or [(1920, 1080), (2560, 1440)]
    if len(set(extents)) != len(extents):
        parser.error("Presentation sizes must be unique")
    scenes = [benchmark_scene(name, path) for name, path in (("sponza", args.sponza), ("chess", args.gltf), ("tekla", args.bim))]
    inputs = {scene["name"]: scene_identity(scene) for scene in scenes}
    runtime = runtime_receipt(args.renderer, enabled=True)
    args.output.mkdir(parents=True, exist_ok=False)
    env = dict(os.environ, VK_LOADER_LAYERS_DISABLE="~implicit~")
    for variable in ("VK_INSTANCE_LAYERS", "VK_LOADER_LAYERS_ENABLE", "VK_ADD_LAYER_PATH"):
        env.pop(variable, None)
    validation = validation_environment(args.validation_manifest, args.output, env) if args.validation_manifest else None
    if validation:
        (args.output / "vk_layer_settings.txt").write_text(VALIDATION_SETTINGS, encoding="ascii")
    report = {"runtime": runtime, "inputs": inputs, "scenes": scenes, "warmupFrames": args.warmup,
              "steadySamplesPerCase": args.samples, "fixedDeltaSeconds": 1 / 60, "technique": args.technique,
              "serializedGraphics": True, "validation": validation, "cases": {}}

    for scene in scenes:
        for extent in extents:
            reference, native_images = run_case(args, scene, extent, "native", env, validation)
            require(report.setdefault("device", reference["device"]) == reference["device"], "Physical GPU changed")
            modes = {"native": reference}
            report["cases"][f"{scene['name']}-{extent[0]}x{extent[1]}"] = modes
            for mode in MODES[1:]:
                result, images = run_case(args, scene, extent, mode, env, validation)
                require(result["device"] == report["device"], "Physical GPU changed")
                for frame, capture in result["captures"].items():
                    native = reference["captures"][frame]
                    require(all(capture[key] == native[key] for key in ("camera", "exposure", "objectCount", "build")),
                            f"Nonmatching native reference: {scene['name']} {extent} {mode} frame {frame}")
                    capture["nativeComparison"] = compare_images(native_images[frame], images[frame])
                require(any(capture["nativeComparison"]["changedPixelCount"] > 0 for capture in result["captures"].values()),
                        f"Reconstruction produced an exact native match at every sample: {scene['name']} {extent} {mode}")
                modes[mode] = result
    require(runtime == runtime_receipt(args.renderer, enabled=True), "The renderer or FSR runtime changed")
    require(inputs == {scene["name"]: scene_identity(scene) for scene in scenes}, "A benchmark source asset changed")
    report["passed"] = True
    (args.output / "results.json").write_text(json.dumps(report, indent=2, allow_nan=False) + "\n", encoding="utf-8")


if __name__ == "__main__":
    main()
