"""Native reference-path checks for opt-in glTF retroreflection and thin scatter."""

import argparse
import hashlib
import json
import math
import os
from pathlib import Path
import subprocess

os.environ.setdefault("OPENCV_IO_ENABLE_OPENEXR", "1")
import cv2
import numpy as np

from gltf_accessor_runtime_regression import Asset, load_asset, mesh
from gltf_extended_materials_runtime_regression import validation_log_error


RETRO = "KHR_materials_retroreflection"
SCATTER = "KHR_materials_scatter"
LEGACY = "KHR_materials_volume_scatter"
FLAG = "--experimental-gltf-materials"


def panel(path, mode):
    asset = Asset()
    mesh(asset, asset.accessor("VEC3", [[-2, -2, 0], [2, -2, 0], [2, 2, 0], [-2, 2, 0]]),
         asset.accessor("VEC3", [[0, 0, 1]] * 4),
         asset.accessor("VEC2", [[0, 0], [1, 0], [1, 1], [0, 1]]))
    material = {"doubleSided": True, "pbrMetallicRoughness": {"baseColorFactor": [.7, .4, .15, 1]}}
    pbr = material["pbrMetallicRoughness"]
    extensions = material["extensions"] = {}
    if mode.startswith("retro"):
        pbr.update(metallicFactor=1, roughnessFactor=.45)
        extensions[RETRO] = {"retroreflectionFactor": 0 if mode == "retro-zero" else 128 / 255}
        if mode == "retro-texture":
            extensions[RETRO] = {"retroreflectionFactor": 1, "retroreflectionTexture": {
                "index": asset.image(np.array([[[128, 0, 255, 0]]], dtype=np.uint8))}}
    else:
        pbr.update(metallicFactor=0, roughnessFactor=0)
        extensions["KHR_materials_specular"] = {"specularFactor": 0}
        extensions["KHR_materials_transmission"] = {"transmissionFactor": 1}
        color_field = "multiscatterColor" if mode == "scatter-legacy" else "multiscatterColorFactor"
        scatter = {"scatterAnisotropy": .6, color_field: [.25, .7, .5],
                   "scatterStrengthTexture": {"index": asset.image(np.array([[[255, 0, 255, 128]]], dtype=np.uint8))},
                   "multiscatterColorTexture": {"index": asset.image(np.array([[[128, 64, 192, 0]]], dtype=np.uint8))}}
        if mode != "scatter-legacy":
            scatter["scatterStrengthFactor"] = 1
        extensions[LEGACY if mode == "scatter-legacy" else SCATTER] = scatter
    asset.model["materials"] = [material]
    asset.model["extensionsUsed"] = list(extensions)
    asset.write(path)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--renderer", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--generate-only", action="store_true")
    args = parser.parse_args()
    renderer, output = args.renderer.resolve(), args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    (output / "vk_layer_settings.txt").write_text("khronos_validation.validate_sync = true\n", encoding="utf-8")
    models = {}
    for mode in ("retro-zero", "retro-factor", "retro-texture", "scatter-current", "scatter-legacy"):
        models[mode] = output / (mode + ".gltf")
        panel(models[mode], mode)
    if args.generate_only:
        return

    evidence = {}

    def capture(name, model, *, retro=False, enabled=True, back=False, save=None):
        image = output / (name + ".exr")
        camera = ("2.4", "0", "3.2") if retro else ("0", "0", "4")
        direction = ("-.6", "0", "-.8") if retro else ("0", "0", "1" if back else "-1")
        command = [str(renderer), "--model", str(model), "--import-scale", "1", "--hidden", "--no-ui",
                   "--validation", "--width", "192", "--height", "144", "--no-bloom", "--render-technique", "path-tracing",
                   "--display-mode", "lit", "--path-spp", "16", "--path-samples", "16", "--path-bounces", "1",
                   "--environment-intensity", "0", "--camera-position", *camera, "--camera-target", "0", "0", "0",
                   "--camera-fov", "45", "--directional-intensity", "2", "--directional-direction", *direction,
                   "--directional-color", "1", "1", "1", "--warmup-frames", "0", "--capture-frame", "1",
                   "--screenshot", str(image)]
        if enabled:
            command.append(FLAG)
        if save:
            command.extend(("--save-scene", str(save)))
        log_path = output / (name + ".log")
        with log_path.open("w", encoding="utf-8") as log:
            subprocess.run(command, cwd=renderer.parent, stdout=log, stderr=subprocess.STDOUT, check=True,
                           timeout=180, env=dict(os.environ, VK_LAYER_SETTINGS_PATH=str(output)))
        assert not validation_log_error(log_path.read_text(encoding="utf-8", errors="replace")), log_path
        metadata = json.loads(Path(f"{image}.capture.json").read_text(encoding="utf-8"))
        assert metadata["source"] == "path-trace-mean" and not metadata["referencePath"]["hasDiagnostics"], metadata
        assert metadata["referencePath"]["attemptedSamplesPerPixel"] == 16, metadata
        pixels = cv2.imdecode(np.frombuffer(image.read_bytes(), np.uint8), cv2.IMREAD_UNCHANGED)
        assert pixels.shape == (144, 192, 3) and np.isfinite(pixels).all(), image
        center = pixels[71:73, 95:97, ::-1].mean(axis=(0, 1))
        evidence[name] = {"command": command, "center": center.tolist(), "metadata": metadata,
                          "pixelSha256": hashlib.sha256(pixels.tobytes()).hexdigest()}
        return center

    baseline = capture("retro-disabled", models["retro-texture"], retro=True, enabled=False)
    zero = capture("retro-zero", models["retro-zero"], retro=True)
    factor = capture("retro-factor", models["retro-factor"], retro=True)
    textured = capture("retro-texture", models["retro-texture"], retro=True)
    np.testing.assert_array_equal(zero, baseline)
    assert evidence["retro-zero"]["pixelSha256"] == evidence["retro-disabled"]["pixelSha256"]
    np.testing.assert_allclose(textured, factor, rtol=1e-5, atol=1e-7)
    assert np.all(textured > baseline * 4), (textured, baseline)

    saved = output / "scatter-portable.glb"
    front = capture("scatter-front", models["scatter-current"], save=saved)
    back = capture("scatter-back", models["scatter-current"], back=True)
    legacy = capture("scatter-legacy", models["scatter-legacy"], back=True)
    archived, _ = load_asset(saved)
    material_index = archived["meshes"][0]["primitives"][0]["material"]
    archive_scatter = archived["materials"][material_index]["extensions"][SCATTER]
    assert archive_scatter["scatterStrengthTexture"]["index"] != archive_scatter["multiscatterColorTexture"]["index"]
    restored = capture("scatter-reloaded", saved, back=True)
    np.testing.assert_array_equal(legacy, back)
    np.testing.assert_array_equal(restored, back)
    srgb = np.array([128, 64, 192]) / 255
    linear = ((srgb + .055) / 1.055) ** 2.4
    albedo = np.array([.25, .7, .5]) * linear
    strength = 128 / 255
    np.testing.assert_allclose(front, 2 * albedo * strength * .2 / math.pi, atol=2e-6, rtol=.007)
    np.testing.assert_allclose(back, 2 * albedo * strength * .8 / math.pi, atol=2e-6, rtol=.007)
    (output / "results.json").write_text(json.dumps(evidence, indent=2), encoding="utf-8")
    print("PASS opt-in retroreflection, thin scatter, legacy aliases and portable textures", flush=True)


if __name__ == "__main__":
    main()
