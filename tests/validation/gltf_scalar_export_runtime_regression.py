"""Compare Vulkan reloads of the scalar export fixture with literal PNG controls."""

import argparse
import base64
import io
import json
import os
from pathlib import Path
import subprocess

from PIL import Image, ImageChops

from gltf_extended_materials_runtime_regression import validation_log_error


SLOTS = {
    "specular": {"specularTexture": 3},
    "transmission": {"transmissionTexture": 0},
    "clearcoat": {"clearcoatTexture": 0, "clearcoatRoughnessTexture": 1},
    "volume": {"thicknessTexture": 1},
    "sheen": {"sheenRoughnessTexture": 3},
    "iridescence": {"iridescenceTexture": 0, "iridescenceThicknessTexture": 1},
    "diffuse_transmission": {"diffuseTransmissionTexture": 3},
}


def png_uri(pixel):
    encoded = io.BytesIO()
    Image.new("RGBA", (1, 1), tuple(pixel)).save(encoded, format="PNG")
    return "data:image/png;base64," + base64.b64encode(encoded.getvalue()).decode("ascii")


def controls(source):
    model = json.loads(source.read_text(encoding="utf-8"))
    material = next(m for m in model["materials"] if "KHR_materials_transmission" in m.get("extensions", {}))
    extensions = material["extensions"]
    original = [63, 47, 109, 255]
    occlusion = material["occlusionTexture"]["index"]
    model["images"][model["textures"][occlusion]["source"]] = {"uri": png_uri(original)}
    for extension, fields in SLOTS.items():
        for field, channel in fields.items():
            texture = extensions["KHR_materials_" + extension][field]["index"]
            pixel = original.copy()
            pixel[channel] = original[2]
            model["images"][model["textures"][texture]["source"]] = {"uri": png_uri(pixel)}

    literal = source.with_name("literal.gltf")
    literal.write_text(json.dumps(model), encoding="utf-8")
    extensions["KHR_materials_transmission"]["transmissionTexture"]["index"] = occlusion
    wrong_channel = source.with_name("wrong-channel.gltf")
    wrong_channel.write_text(json.dumps(model), encoding="utf-8")
    return literal, wrong_channel


def capture(renderer, output, technique, source):
    target = output / f"{technique}-{source.stem}-{source.suffix[1:]}"
    screenshot = target.with_suffix(".png")
    screenshot.unlink(missing_ok=True)
    log = target.with_suffix(".log")
    command = [
        str(renderer), "--hidden", "--no-ui", "--validation", "--no-bloom", "--no-taa",
        "--width", "320", "--height", "240", "--msaa", "1", "--warmup-frames", "12",
        "--capture-frame", "13", "--fixed-dt", ".016666667", "--render-technique", technique,
        "--display-mode", "lit", "--exposure", ".35", "--camera-position", ".65", ".65", "4",
        "--camera-target", ".65", ".65", "0", "--camera-fov", "45", "--import-scale", "1",
        "--environment-intensity", "1", "--directional-intensity", "3",
        "--directional-direction", "0", "0", "-1", "--no-ray-query", "--ray-shadows", "raster",
        "--model", str(source), "--screenshot", str(screenshot),
    ]
    with log.open("w", encoding="utf-8") as stream:
        process = subprocess.run(command, cwd=renderer.parent, stdout=stream, stderr=subprocess.STDOUT,
                                 timeout=120, env=dict(os.environ, VK_LAYER_SETTINGS_PATH=str(output)))
    if process.returncode or validation_log_error(log.read_text(encoding="utf-8", errors="replace")):
        raise RuntimeError(f"Vulkan capture failed, see {log}")
    with Image.open(screenshot) as image:
        return image.convert("RGB")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("renderer", type=Path)
    parser.add_argument("fixture_directory", type=Path)
    args = parser.parse_args()
    fixture = args.fixture_directory.resolve()
    gltf = fixture / "transmission.gltf"
    glb = fixture / "transmission.glb"
    literal, wrong = controls(gltf)
    for technique in ("deferred-raster", "forward-raster"):
        expected = capture(args.renderer.resolve(), fixture, technique, literal)
        for source in (gltf, glb):
            actual = capture(args.renderer.resolve(), fixture, technique, source)
            if ImageChops.difference(expected, actual).getbbox():
                raise RuntimeError(f"{technique} {source.suffix} differs from the literal fixture")
        wrong_image = capture(args.renderer.resolve(), fixture, technique, wrong)
        red, green, blue = ImageChops.difference(expected, wrong_image).split()
        unchanged = ImageChops.lighter(ImageChops.lighter(red, green), blue).histogram()[0]
        changed = expected.width * expected.height - unchanged
        if not changed:
            raise RuntimeError(f"{technique} wrong-channel control did not change the render")
        print(f"{technique}: glTF and GLB match exactly; wrong channel changes {changed} pixels")


if __name__ == "__main__":
    main()
