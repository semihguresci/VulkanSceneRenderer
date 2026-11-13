"""Exercise live swapchain format changes with raster post-processing and ImGui."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import tempfile

from display_capture_test_support import emitter_fixture, validation_log_error


TECHNIQUES = ("deferred-raster", "forward-raster")
START = (640, 480)
WIDE = (672, 480)
NARROW = (608, 456)
FORMATS = {"bgra8-srgb": 50, "rgba8-srgb": 43, "rgba8-unorm": 37, "bgra8-unorm": 44}
SRGB_NONLINEAR = 0
PHASES = (
    ("startup", 3, START, "bgra8-srgb", 0, 1, {}),
    ("extent-only", 5, WIDE, "bgra8-srgb", 1, 1, {"resize": WIDE}),
    ("rgba-srgb", 7, WIDE, "rgba8-srgb", 2, 2, {"swapchainFormat": "rgba8-srgb"}),
    ("rgba-extent-only", 9, NARROW, "rgba8-srgb", 3, 2, {"resize": NARROW}),
    ("rgba-unorm", 11, NARROW, "rgba8-unorm", 4, 3, {"swapchainFormat": "rgba8-unorm"}),
    ("bgra-unorm", 13, NARROW, "bgra8-unorm", 5, 4, {"swapchainFormat": "bgra8-unorm"}),
    ("restored", 15, START, "bgra8-srgb", 6, 5, {"swapchainFormat": "bgra8-srgb", "resize": START}),
)
VALIDATION = "VK_LAYER_KHRONOS_validation"
LOADER_LAYER = re.compile(r'Insert(?:ed)? (instance|device) layer "([^"]+)" \(([^\r\n]+)\)')


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


def sha(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def write_inputs(output, phases=PHASES):
    model = output / "emitter.gltf"
    emitter_fixture(model, {
        "emissiveFactor": [.5, .25, .125],
        "extensions": {"KHR_materials_emissive_strength": {"emissiveStrength": 8}},
        "pbrMetallicRoughness": {"baseColorFactor": [0, 0, 0, 1]},
    })
    sequence = output / "sequence.json"
    sequence.write_text(json.dumps({
        "schemaVersion": 1,
        "frames": phases[-1][1],
        "sampleFrames": [frame for _, frame, *_ in phases],
        "events": [{"frame": frame, **event} for _, frame, _, _, _, _, event in phases if event],
    }, indent=2) + "\n", encoding="utf-8")
    return model, sequence


def command_for(renderer, model, sequence, image, technique, viewport):
    command = [
        str(renderer), "--hidden", "--validation", "--model", str(model), "--import-scale", "1",
        "--width", str(START[0]), "--height", str(START[1]), "--msaa", "1", "--no-taa", "--no-bloom",
        "--render-technique", technique, "--display-mode", "lit", "--exposure", "1", "--tone-map", "reinhard",
        "--no-ray-query", "--no-ddgi", "--ray-shadows", "raster", "--camera-fov", "45",
        "--camera-position", "0", "0", "4", "--camera-target", "0", "0", "0",
        "--sky", "plain", "--sky-horizon", "0", "0", "0", "--environment-intensity", "0",
        "--bounce-intensity", "0", "--directional-intensity", "0", "--fixed-dt", ".016666667",
        "--capture-sequence", str(sequence), "--screenshot", str(image),
    ]
    if viewport:
        command.append("--screenshot-viewport")
    return command


def validation_files(directory):
    if directory is None:
        return None
    manifest = directory / "VkLayer_khronos_validation.json"
    layer = json.loads(manifest.read_text(encoding="utf-8-sig"))["layer"]
    require(layer["name"] == VALIDATION, f"Invalid validation manifest {manifest}")
    library = (directory / layer["library_path"]).resolve(strict=True)
    return {"library": str(library), "apiVersion": layer["api_version"],
            "manifestSha256": sha(manifest), "librarySha256": sha(library)}


def capture(renderer, model, sequence, output, technique, viewport, env, validation):
    scope = "viewport" if viewport else "window"
    image = output / f"{technique}-{scope}.png"
    log = image.with_suffix(".log")
    command = command_for(renderer, model, sequence, image, technique, viewport)
    with log.open("w", encoding="utf-8") as stream:
        result = subprocess.run(command, cwd=renderer.parent, env=env, stdout=stream, stderr=subprocess.STDOUT,
                                timeout=180, creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
    text = log.read_text(encoding="utf-8", errors="replace")
    require(result.returncode == 0 and not validation_log_error(text), f"Vulkan capture failed. See {log}")
    loaded = {kind: Path(library).resolve() for kind, name, library in LOADER_LAYER.findall(text)
              if name == VALIDATION}
    require(set(loaded) == {"instance", "device"}, f"Validation was not loaded on the instance and device. See {log}")
    if validation is not None:
        require(all(path == Path(validation["library"]) for path in loaded.values()),
                f"Unexpected validation layer library. See {log}")
    return {"technique": technique, "scope": scope, "command": command, "log": str(log),
            "exitCode": result.returncode, "validationLibraries": {kind: str(path) for kind, path in loaded.items()},
            "captures": []}, image


def expected_emitter():
    # Linear emission (4, 2, 1), manual exposure 1, per-channel Reinhard and IEC sRGB encoding.
    mapped = [radiance / (1 + radiance) for radiance in (4, 2, 1)]
    return tuple(round(255 * (1.055 * value ** (1 / 2.4) - .055)) for value in mapped)


def check_capture(base, technique, viewport, phase, final_frame):
    from PIL import Image

    name, frame, extent, format_name, recreates, generation, _ = phase
    image = base if frame == final_frame else base.with_name(f"{base.stem}.frame-{frame:04d}.png")
    state = json.loads(image.with_suffix(".telemetry.json").read_bytes())
    metadata = json.loads(Path(f"{image}.capture.json").read_bytes())
    require(state["validationEnabled"] and state["technique"] == technique and state["resolution"] == list(extent),
            f"Wrong renderer state for {image}")
    require(any(item["name"] == "PostProcess" and item["active"] for item in state["passes"]),
            f"Post-process did not run for {image}")
    require(state["swapchainFormat"] == FORMATS[format_name] and state["swapchainColorSpace"] == SRGB_NONLINEAR,
            f"Expected {format_name} with sRGB nonlinear color space for {image}")
    require(state["swapchainSupportsTransferSrc"], f"The swapchain does not support PNG readback for {image}")
    require(state["swapchainRecreates"] == recreates and state["presentationPipelineGeneration"] == generation and
            state["guiPipelineGeneration"] == generation,
            f"Expected recreate count {recreates} and pipeline generations {generation} for {image}, got "
            f"{state['swapchainRecreates']}, {state['presentationPipelineGeneration']}, {state['guiPipelineGeneration']}")
    require((metadata["width"], metadata["height"]) == extent and metadata["colorSpace"] == "sRGB" and
            metadata["source"] == ("viewport" if viewport else "window") and metadata["includesUi"] == (not viewport) and
            metadata["exposureMode"] == "manual" and metadata["manualExposure"] == 1 and metadata["toneMapper"] == 1,
            f"Wrong capture metadata for {image}: {metadata}")
    with Image.open(image) as png:
        require(png.format == "PNG" and png.size == extent, f"Invalid PNG or extent for {image}")
        rgb = png.convert("RGB")
    width, height = extent
    x, y = width // 2, height // 2
    if viewport:
        expected = expected_emitter()
        require(all(max(abs(actual - target) for actual, target in zip(pixel, expected, strict=True)) <= 2
                    for pixel in rgb.crop((x - 8, y - 8, x + 8, y + 8)).getdata()),
                f"Expected sRGB RGB {expected} near center of {image}, got {rgb.getpixel((x, y))}")
    return {"phase": name, "frame": frame, "image": str(image), "extent": list(extent),
            "revision": state["build"]["revision"], "swapchainFormat": state["swapchainFormat"],
            "swapchainColorSpace": state["swapchainColorSpace"], "swapchainRecreates": state["swapchainRecreates"],
            "swapchainImages": state["swapchainImages"], "swapchainSupportsTransferSrc": state["swapchainSupportsTransferSrc"],
            "presentationPipelineGeneration": state["presentationPipelineGeneration"],
            "guiPipelineGeneration": state["guiPipelineGeneration"],
            "centerRgb": rgb.getpixel((x, y)), "decodedSha256": hashlib.sha256(rgb.tobytes()).hexdigest(),
            "metadata": metadata}, rgb


def check_ui(viewport, window, image):
    from PIL import ImageChops

    difference = ImageChops.difference(viewport, window)
    red, green, blue = difference.split()
    strongest = ImageChops.lighter(red, ImageChops.lighter(green, blue))
    changed = sum(strongest.histogram()[9:])
    require(changed >= viewport.width * viewport.height // 100,
            f"Expected visible ImGui drawing in {image}, observed only {changed} changed pixels")
    return changed


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--renderer", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--technique", choices=TECHNIQUES, action="append")
    parser.add_argument("--managed-output", action="store_true", help="write to a fresh run directory under --output")
    parser.add_argument("--validation-layer-dir", type=Path, help="directory of a specific Khronos validation layer")
    args = parser.parse_args()
    if os.environ.get("CONTAINER_RUN_GPU_SWAPCHAIN_FORMAT") != "1":
        print("SKIP: set CONTAINER_RUN_GPU_SWAPCHAIN_FORMAT=1 to run GPU captures")
        return 77

    renderer, output = args.renderer.resolve(strict=True), args.output.resolve()
    if args.managed_output:
        output.mkdir(parents=True, exist_ok=True)
        output = Path(tempfile.mkdtemp(prefix="run-", dir=output))
        print(f"Captures: {output}", flush=True)
    else:
        output.mkdir(parents=True)
    (output / "vk_layer_settings.txt").write_text("khronos_validation.validate_sync = true\n", encoding="utf-8")
    env = dict(os.environ, VK_LAYER_SETTINGS_PATH=str(output), VK_LOADER_DEBUG="layer")
    layer_dir = args.validation_layer_dir.resolve(strict=True) if args.validation_layer_dir else None
    validation = validation_files(layer_dir)
    if layer_dir is not None:
        env["VK_LAYER_PATH"] = str(layer_dir)
    model, sequence = write_inputs(output)
    renderer_hash = sha(renderer)
    report = {"executableSha256": renderer_hash, "fixture": str(model), "sequence": str(sequence),
              "validationLayer": validation, "expectedEmitterRgb": expected_emitter(), "passed": False, "runs": []}
    try:
        for technique in args.technique or TECHNIQUES:
            viewport_images = {}
            for viewport in (True, False):
                row, base = capture(renderer, model, sequence, output, technique, viewport, env, validation)
                report["runs"].append(row)
                for phase in PHASES:
                    item, rgb = check_capture(base, technique, viewport, phase, PHASES[-1][1])
                    row["captures"].append(item)
                    if viewport:
                        viewport_images[item["phase"]] = rgb
                    else:
                        item["uiChangedPixels"] = check_ui(viewport_images[item["phase"]], rgb, item["image"])
                if viewport:
                    require(row["captures"][0]["decodedSha256"] == row["captures"][-1]["decodedSha256"],
                            f"The restored viewport differs from the original in {technique}")
            print(f"{technique}: four advertised formats, resize-only reuse, sRGB PNG and ImGui verified", flush=True)
        require(sha(renderer) == renderer_hash, "Renderer executable changed during capture")
        report["passed"] = True
    finally:
        (output / "results.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
