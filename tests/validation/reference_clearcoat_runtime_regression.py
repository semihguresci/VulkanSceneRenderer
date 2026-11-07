"""Validate authored clearcoat, raw reference history and portable scene reload."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess

os.environ.setdefault("OPENCV_IO_ENABLE_OPENEXR", "1")
import cv2
import numpy as np

from gltf_accessor_runtime_regression import Asset, load_asset
from gltf_extended_materials_runtime_regression import validation_log_error
from gltf_merge_runtime_regression import image_pixels, portable_copy


WIDTH, HEIGHT, SAMPLES = 192, 144, 13
CLEARCOAT = "KHR_materials_clearcoat"
COAT_TEXEL = [128, 229, 51, 255]
ROUGHNESS_TEXEL = [217, 96, 37, 255]
NORMAL_TEXEL = [204, 128, 230, 255]


def fixture(path, profile):
    asset = Asset()
    primitive = dict(attributes=dict(
        POSITION=asset.accessor("VEC3", [[-8, -8, 0], [8, -8, 0], [8, 8, 0], [-8, 8, 0]]),
        NORMAL=asset.accessor("VEC3", [[0, 0, 1]] * 4),
        TANGENT=asset.accessor("VEC4", [[1, 0, 0, 1]] * 4),
        TEXCOORD_0=asset.accessor("VEC2", [[0, 0], [1, 0], [1, 1], [0, 1]])),
        indices=asset.accessor("SCALAR", [0, 1, 2, 0, 2, 3], 5123, indices=True), material=0)
    asset.model["meshes"] = [dict(name="Clearcoat surface", primitives=[primitive])]
    material = dict(name="Clearcoat panel", pbrMetallicRoughness=dict(
        baseColorFactor=[.2, .1, .05, 1], metallicFactor=0, roughnessFactor=.7),
        extensions={"KHR_materials_specular": dict(specularFactor=0)})
    if profile != "uncoated":
        coat = dict(clearcoatFactor=COAT_TEXEL[0] / 255, clearcoatRoughnessFactor=ROUGHNESS_TEXEL[1] / 255)
        if profile != "constant":
            coat.update(clearcoatFactor=1, clearcoatRoughnessFactor=1,
                clearcoatTexture=dict(index=asset.image(np.tile(COAT_TEXEL, (2, 2, 1)).astype(np.uint8))),
                clearcoatRoughnessTexture=dict(index=asset.image(np.tile(ROUGHNESS_TEXEL, (2, 2, 1)).astype(np.uint8))))
        if profile in ("normal-zero", "normal-active"):
            coat["clearcoatNormalTexture"] = dict(
                index=asset.image(np.tile(NORMAL_TEXEL, (2, 2, 1)).astype(np.uint8)),
                scale=0 if profile == "normal-zero" else 1)
        material["extensions"][CLEARCOAT] = coat
    asset.model["materials"] = [material]
    asset.model["extensionsUsed"] = sorted(material["extensions"])
    asset.write(path)


def inspect_export(path):
    model, buffers = load_asset(path)
    material, = (row for row in model["materials"] if row.get("name") == "Clearcoat panel")
    coat = material["extensions"][CLEARCOAT]
    assert coat["clearcoatFactor"] == coat["clearcoatRoughnessFactor"] == 1, coat
    assert coat["clearcoatNormalTexture"].get("scale", 1) == 1, coat
    for field, texel in (("clearcoatTexture", COAT_TEXEL),
                         ("clearcoatRoughnessTexture", ROUGHNESS_TEXEL),
                         ("clearcoatNormalTexture", NORMAL_TEXEL)):
        texture = model["textures"][coat[field]["index"]]
        np.testing.assert_array_equal(image_pixels(path, model, buffers, texture["source"]),
                                      np.tile(texel, (2, 2, 1)).astype(np.uint8))
    return dict(path=str(path), sha256=hashlib.sha256(path.read_bytes()).hexdigest(),
                clearcoatTexturesPreserved=True)


def changed(first, second):
    delta = np.abs(first - second)
    assert np.count_nonzero(delta.max(axis=2) > .001) > WIDTH * HEIGHT // 4, float(delta.max())
    return dict(maximumAbsoluteDelta=float(delta.max()), meanAbsoluteDelta=float(delta.mean()))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--renderer", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    renderer, output = args.renderer.resolve(), args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    sources = output / "sources"
    sources.mkdir()
    (output / "vk_layer_settings.txt").write_text("khronos_validation.validate_sync = true\n", encoding="utf-8")
    renderer_hash = hashlib.sha256(renderer.read_bytes()).hexdigest()
    runtime_inputs = [renderer, *renderer.parent.glob("*.dll"),
                      *(renderer.parent / "spv_shaders").glob("*.spv")]
    runtime_hashes = {str(path): hashlib.sha256(path.read_bytes()).hexdigest() for path in runtime_inputs}
    report = dict(passed=False, rendererSha256=renderer_hash, runtimeSha256=runtime_hashes,
                  captures=0, runs=[], exports=[], imageChecks=[])
    fixtures = {}
    for profile in ("uncoated", "constant", "textured", "normal-zero", "normal-active"):
        fixtures[profile] = sources / f"{profile}.gltf"
        fixture(fixtures[profile], profile)

    def capture(name, model, *, frames=(4,), events=(), flags=(), save=None):
        base = output / name
        sequence = base.with_suffix(".sequence.json")
        sequence.write_text(json.dumps(dict(schemaVersion=1, frames=max(frames),
            sampleFrames=list(frames), events=list(events))), encoding="utf-8")
        command = [
            str(renderer), "--hidden", "--validation", "--no-ui", "--screenshot-viewport",
            "--width", str(WIDTH), "--height", str(HEIGHT), "--msaa", "1", "--fixed-dt", ".016666667",
            "--model", str(model), "--import-scale", "1", "--render-technique", "path-tracing",
            "--camera-position", "0", "0", "4", "--camera-target", "0", "0", "0",
            "--camera-fov", "45", "--environment-intensity", "0", "--directional-intensity", "2",
            "--directional-direction", "0", "0", "-1", "--directional-color", "1", "1", "1",
            "--no-bloom", "--no-taa", "--display-mode", "lit", "--exposure", "1",
            "--path-spp", "4", "--path-samples", str(SAMPLES), "--path-bounces", "2",
            "--capture-sequence", str(sequence), "--screenshot", str(base.with_suffix(".exr")), *flags]
        if save is not None:
            save.parent.mkdir(parents=True, exist_ok=True)
            command += ["--save-scene", str(save)]
        log = base.with_suffix(".log")
        with log.open("w", encoding="utf-8") as stream:
            process = subprocess.run(command, cwd=renderer.parent, stdout=stream, stderr=subprocess.STDOUT,
                timeout=240, env=dict(os.environ, VK_LAYER_SETTINGS_PATH=str(output)),
                creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
        assert process.returncode == 0 and not validation_log_error(log.read_text(encoding="utf-8", errors="replace")), log
        samples = []
        for frame in frames:
            stem = base if frame == max(frames) else Path(f"{base}.frame-{frame:04d}")
            image = Path(f"{stem}.exr")
            state = json.loads(Path(f"{stem}.telemetry.json").read_text(encoding="utf-8"))
            metadata = json.loads(Path(f"{image}.capture.json").read_text(encoding="utf-8"))
            pixels = cv2.imdecode(np.frombuffer(image.read_bytes(), dtype=np.uint8), cv2.IMREAD_UNCHANGED)
            assert pixels is not None and pixels.dtype == np.float32 and pixels.shape == (HEIGHT, WIDTH, 3), image
            assert np.isfinite(pixels).all() and (pixels >= 0).all(), image
            reference = state["referencePath"]
            assert state["technique"] == "path-tracing" and reference["active"], reference
            assert reference["attemptedSamples"] == SAMPLES, reference
            assert reference["unsupportedSamples"] == reference["rejectedSamples"] == 0, reference
            assert metadata["source"] == "path-trace-mean" and metadata["colorSpace"] == "linear-sRGB", metadata
            assert metadata["referencePath"]["attemptedSamplesPerPixel"] == SAMPLES, metadata
            samples.append(dict(pixels=pixels[:, :, ::-1], path=reference, image=str(image),
                gpuPasses=state.get("passes", [])))
            report["captures"] += 1
        report["runs"].append(dict(name=name, log=str(log), command=command,
            samples=[dict(image=s["image"], referencePath=s["path"], gpuPasses=s["gpuPasses"])
                     for s in samples]))
        return samples

    try:
        plain = capture("uncoated", fixtures["uncoated"])[0]["pixels"]
        constant = capture("constant", fixtures["constant"])[0]["pixels"]
        textured = capture("textured", fixtures["textured"])[0]["pixels"]
        report["imageChecks"].append(dict(name="clearcoat-visible", **changed(plain, constant)))
        np.testing.assert_array_equal(textured, constant)
        normal_zero = capture("normal-zero", fixtures["normal-zero"])[0]["pixels"]
        np.testing.assert_array_equal(normal_zero, textured)
        normal = capture("normal-active", fixtures["normal-active"])[0]["pixels"]
        report["imageChecks"].append(dict(name="clearcoat-normal-visible", **changed(textured, normal)))
        display_changed = capture("display-independent", fixtures["normal-active"],
            flags=("--exposure", ".125", "--tone-map", "reinhard"))[0]["pixels"]
        np.testing.assert_array_equal(display_changed, normal)
        history = capture("material-history", fixtures["normal-active"], frames=(4, 8, 12, 16), events=(
            dict(frame=5, materialEdit=dict(sourceMaterial=0, values=[
                dict(property="extensions/KHR_materials_clearcoat/clearcoatFactor", value=0)])),
            dict(frame=9, sceneHistory="undo"), dict(frame=13, sceneHistory="redo")))
        assert [sample["path"]["historyResets"] for sample in history] == [1, 2, 3, 4], history
        np.testing.assert_array_equal(history[0]["pixels"], normal)
        np.testing.assert_array_equal(history[0]["pixels"], history[2]["pixels"])
        np.testing.assert_array_equal(history[1]["pixels"], plain)
        np.testing.assert_array_equal(history[1]["pixels"], history[3]["pixels"])
        portable = []
        for extension in ("gltf", "glb"):
            saved = output / "exports" / extension / f"clearcoat.{extension}"
            saved_pixels = capture(f"save-{extension}", fixtures["normal-active"], save=saved)[0]["pixels"]
            np.testing.assert_array_equal(saved_pixels, normal)
            report["exports"].append(inspect_export(saved))
            copied, _ = portable_copy(saved, output / "portable" / extension)
            portable.append((extension, copied))
        sources.rename(output / "sources-removed")
        for extension, model in portable:
            restored = capture(f"reload-{extension}", model)[0]["pixels"]
            np.testing.assert_array_equal(restored, normal)
        assert hashlib.sha256(renderer.read_bytes()).hexdigest() == renderer_hash
        assert all(hashlib.sha256(Path(path).read_bytes()).hexdigest() == digest
                   for path, digest in runtime_hashes.items())
        report["passed"] = True
    finally:
        (output / "results.json").write_text(json.dumps(report, indent=2), encoding="utf-8")
    print("PASS authored clearcoat factors, textures, normal, history and portable raw EXR reload", flush=True)


if __name__ == "__main__":
    main()
