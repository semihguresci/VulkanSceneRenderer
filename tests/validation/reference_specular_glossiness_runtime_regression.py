"""Validate authored specular/glossiness factors and textures in the Vulkan reference viewport."""

from __future__ import annotations

import argparse
import copy
import hashlib
import json
import math
import os
from pathlib import Path
import subprocess
import sys

os.environ.setdefault("OPENCV_IO_ENABLE_OPENEXR", "1")
import cv2
import numpy as np

from gltf_accessor_runtime_regression import Asset, decode_accessor, load_asset
from gltf_extended_materials_runtime_regression import validation_log_error
from gltf_merge_runtime_regression import image_pixels, portable_copy, texture_info_contract


WIDTH, HEIGHT, SAMPLES, INTENSITY = 192, 144, 13, 2
SG = "KHR_materials_pbrSpecularGlossiness"
POINTER = "KHR_animation_pointer"
TRANSFORM = "KHR_texture_transform"
DIFFUSE = [.6, .4, .2, 1]
SPECULAR = [.2, .3, .4]
GLOSSINESS = .65
END_DIFFUSE = [.3, .8, .2, 1]
END_SPECULAR = [.35, .1, .25]
END_GLOSSINESS = .2
DIFFUSE_TEXEL = [128, 96, 192, 255]
SPECULAR_TEXEL = [96, 160, 64, 128]
MAPPING = dict(offset=[.1, .2], scale=[1.6, .8], rotation=.2, texCoord=1)
UV = [[0, 0], [1, 0], [1, 1], [0, 1]]


def srgb(values):
    values = np.asarray(values, dtype=float) / 255
    return np.where(values <= .04045, values / 12.92, ((values + .055) / 1.055) ** 2.4)


def factors(profile):
    if profile in ("metallic-roughness", "diffuse-only"):
        return [.2, .4, .6, 1], [0, 0, 0], .25
    if profile == "factors":
        return [.2, .4, .6, 1], [.1, .2, .3], .25
    if profile == "texture-control":
        diffuse = np.asarray(DIFFUSE[:3]) * srgb(DIFFUSE_TEXEL[:3])
        specular = np.asarray(SPECULAR) * srgb(SPECULAR_TEXEL[:3])
        return [*diffuse, 1], specular.tolist(), GLOSSINESS * SPECULAR_TEXEL[3] / 255
    if profile == "alpha-wrong":
        return DIFFUSE, SPECULAR, GLOSSINESS * float(srgb([SPECULAR_TEXEL[3]])[0]) / (SPECULAR_TEXEL[3] / 255)
    if profile == "posed":
        return ((np.asarray(DIFFUSE) + END_DIFFUSE) * .5).tolist(), (
            (np.asarray(SPECULAR) + END_SPECULAR) * .5).tolist(), (GLOSSINESS + END_GLOSSINESS) * .5
    if profile == "edited":
        return DIFFUSE, [.1, .7, .2], .2
    return DIFFUSE, SPECULAR, GLOSSINESS


def textures(patterned):
    diffuse = np.tile(DIFFUSE_TEXEL, (4, 4, 1)).astype(np.uint8)
    specular = np.tile(SPECULAR_TEXEL, (4, 4, 1)).astype(np.uint8)
    if patterned:
        diffuse[:2, :2, :3] = [48, 192, 80]
        diffuse[2:, 2:, :3] = [224, 64, 128]
        specular[:2, :2] = [160, 64, 128, 64]
        specular[2:, 2:] = [64, 96, 192, 192]
    return diffuse, specular


def transformed_uv():
    angle = MAPPING["rotation"]
    rotate = np.array([[math.cos(angle), -math.sin(angle)],
                       [math.sin(angle), math.cos(angle)]])
    return (np.asarray(UV) * MAPPING["scale"]) @ rotate.T + MAPPING["offset"]


def fixture(path, profile):
    asset = Asset()
    baked = profile == "baked-uv"
    attrs = dict(
        POSITION=asset.accessor("VEC3", [[-8, -8, 0], [8, -8, 0], [8, 8, 0], [-8, 8, 0]]),
        NORMAL=asset.accessor("VEC3", [[0, 0, 1]] * 4),
        TANGENT=asset.accessor("VEC4", [[1, 0, 0, 1]] * 4),
        TEXCOORD_0=asset.accessor("VEC2", transformed_uv() if baked else [[.1, .1]] * 4),
        TEXCOORD_1=asset.accessor("VEC2", UV))
    asset.model["meshes"] = [dict(name="Specular glossiness surface", primitives=[dict(
        attributes=attrs, indices=asset.accessor("SCALAR", [0, 1, 2, 0, 2, 3], 5123, indices=True),
        material=0)])]
    diffuse, specular, glossiness = factors(profile)
    # Dormant MR values deliberately disagree with the authored SG workflow.
    material = dict(name="Specular glossiness panel", pbrMetallicRoughness=dict(
        baseColorFactor=[.9, .02, .01, 1], metallicFactor=1, roughnessFactor=.95))
    if profile == "metallic-roughness":
        material["pbrMetallicRoughness"].update(baseColorFactor=diffuse, metallicFactor=0, roughnessFactor=1)
        material["extensions"] = {"KHR_materials_specular": dict(specularFactor=0)}
    else:
        sg = dict(diffuseFactor=diffuse, specularFactor=specular, glossinessFactor=glossiness,
                  extras=dict(authoredWorkflow="retained specular glossiness"))
        textured = profile not in ("diffuse-only", "factors", "texture-control")
        if textured:
            patterned = profile in ("uv1", "baked-uv", "wrong-uv", "edited", "animated", "posed")
            diffuse_pixels, specular_pixels = textures(patterned)
            for field, pixels in (("diffuseTexture", diffuse_pixels), ("specularGlossinessTexture", specular_pixels)):
                texture = asset.image(pixels)
                asset.model["samplers"][texture].update(minFilter=9728, magFilter=9728)
                info = dict(index=texture, texCoord=0)
                if patterned and profile not in ("baked-uv", "wrong-uv"):
                    info["extensions"] = {TRANSFORM: copy.deepcopy(MAPPING)}
                sg[field] = info
        material["extensions"] = {SG: sg}
    asset.model["materials"] = [material]
    asset.model["extensionsUsed"] = sorted(material["extensions"])
    if profile in ("uv1", "edited", "animated", "posed"):
        asset.model["extensionsUsed"].append(TRANSFORM)
    if profile == "animated":
        input_keys = asset.accessor("SCALAR", [0, 1])
        outputs = [asset.accessor(kind, values) for kind, values in (
            ("VEC4", [DIFFUSE, END_DIFFUSE]), ("VEC3", [SPECULAR, END_SPECULAR]),
            ("SCALAR", [GLOSSINESS, END_GLOSSINESS]))]
        asset.model["animations"] = [dict(name="Authored SG factors", extras=dict(curve="keep authored keys"),
            samplers=[dict(input=input_keys, output=output, interpolation="LINEAR") for output in outputs],
            channels=[dict(sampler=i, target=dict(path="pointer", extensions={
                POINTER: dict(pointer=f"/materials/0/extensions/{SG}/{field}")}))
                for i, field in enumerate(("diffuseFactor", "specularFactor", "glossinessFactor"))])]
        asset.model["extensionsUsed"].append(POINTER)
    asset.model["cameras"] = [dict(type="orthographic", orthographic=dict(xmag=2, ymag=1.5, znear=.1, zfar=100))]
    asset.model["nodes"].append(dict(name="Reference orthographic camera", camera=0, translation=[0, 0, 4]))
    asset.model["scenes"][0]["nodes"].append(1)
    asset.write(path)


def inspect_export(path):
    model, buffers = load_asset(path)
    material_index, material = next((index, row) for index, row in enumerate(model["materials"])
                                   if row.get("name") == "Specular glossiness panel")
    sg = material["extensions"][SG]
    for field, expected in zip(("diffuseFactor", "specularFactor", "glossinessFactor"), factors("posed")):
        np.testing.assert_allclose(sg[field], expected, rtol=0, atol=1e-6)
    assert sg.get("extras") == dict(authoredWorkflow="retained specular glossiness"), sg
    for field, pixels in zip(("diffuseTexture", "specularGlossinessTexture"), textures(True)):
        info = sg[field]
        expected_info = dict(index=info["index"], texCoord=0, extensions={TRANSFORM: MAPPING})
        assert texture_info_contract(info) == texture_info_contract(expected_info), info
        texture = model["textures"][info["index"]]
        np.testing.assert_array_equal(image_pixels(path, model, buffers, texture["source"]), pixels)
    clip, = model["animations"]
    assert len(clip["channels"]) == 3 and clip.get("extras") == dict(curve="keep authored keys"), clip
    for channel, field, samples in zip(clip["channels"], ("diffuseFactor", "specularFactor", "glossinessFactor"),
            ([DIFFUSE, END_DIFFUSE], [SPECULAR, END_SPECULAR], [GLOSSINESS, END_GLOSSINESS])):
        assert channel["target"]["extensions"][POINTER]["pointer"] == f"/materials/{material_index}/extensions/{SG}/{field}"
        sampler = clip["samplers"][channel["sampler"]]
        assert sampler.get("interpolation", "LINEAR") == "LINEAR", sampler
        np.testing.assert_array_equal(np.asarray(decode_accessor(path, sampler["input"])).reshape(-1), [0, 1])
        np.testing.assert_array_equal(np.asarray(decode_accessor(path, sampler["output"])).reshape(-1),
                                      np.asarray(samples, dtype=np.float32).reshape(-1).astype(float))
    return dict(path=str(path), sha256=hashlib.sha256(path.read_bytes()).hexdigest(),
                authoredFactorsPreserved=True, texturesAndTransformsPreserved=True, animationKeysPreserved=True)


def changed(first, second):
    delta = np.abs(first - second)
    assert np.count_nonzero(delta.max(axis=2) > .001) > WIDTH * HEIGHT // 10, float(delta.max())
    return dict(maximumAbsoluteDelta=float(delta.max()), meanAbsoluteDelta=float(delta.mean()))


def texture_interval(actual, literal):
    diffuse_factor = np.asarray(DIFFUSE[:3])
    specular_factor = np.asarray(SPECULAR)
    diffuse_min = diffuse_factor * srgb(np.clip(np.asarray(DIFFUSE_TEXEL[:3]) - .5, 0, 255))
    diffuse_max = diffuse_factor * srgb(np.clip(np.asarray(DIFFUSE_TEXEL[:3]) + .5, 0, 255))
    specular_min = specular_factor * srgb(np.clip(np.asarray(SPECULAR_TEXEL[:3]) - .5, 0, 255))
    specular_max = specular_factor * srgb(np.clip(np.asarray(SPECULAR_TEXEL[:3]) + .5, 0, 255))
    glossiness = GLOSSINESS * SPECULAR_TEXEL[3] / 255
    specular_scale = 1 / (4 * math.pi * (1 - glossiness) ** 4)
    # The GPU upload suite bounds sampled sRGB by half one authored source
    # code. Propagate that independent interval through each normal-incidence
    # SG term. Alpha is linear UNORM and receives no sRGB interval.
    lower = INTENSITY * ((1 - specular_max) * diffuse_min * (1 - specular_max.max()) / math.pi +
                         specular_min * specular_scale)
    upper = INTENSITY * ((1 - specular_min) * diffuse_max * (1 - specular_min.max()) / math.pi +
                         specular_max * specular_scale)
    diffuse, specular, _ = factors("texture-control")
    diffuse, specular = np.asarray(diffuse[:3]), np.asarray(specular)
    exact = INTENSITY * ((1 - specular) * diffuse * (1 - specular.max()) / math.pi +
                         specular * specular_scale)
    np.testing.assert_allclose(literal, np.broadcast_to(exact, literal.shape), rtol=0, atol=3e-6)
    assert (actual >= lower - 3e-6).all() and (actual <= upper + 3e-6).all(), (
        actual.min(axis=(0, 1)), actual.max(axis=(0, 1)), lower, upper)
    return dict(name="sg-srgb-source-code-interval", sourceCodeRadius=.5,
                radianceLower=lower.tolist(), radianceUpper=upper.tolist(),
                maximumCpuEotfRadianceDelta=float(np.abs(actual - literal).max()),
                literalAnalyticMaximumDelta=float(np.abs(literal - exact).max()))


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
    runtime = [renderer, *renderer.parent.glob("*.dll"), *(renderer.parent / "spv_shaders").glob("*.spv")]
    runtime_hashes = {str(path): hashlib.sha256(path.read_bytes()).hexdigest() for path in runtime}
    helpers = {Path(__file__).resolve()}
    helpers.update(Path(module.__file__).resolve() for module in tuple(sys.modules.values())
                   if getattr(module, "__file__", None) and
                   Path(module.__file__).resolve().parent == Path(__file__).resolve().parent)
    helper_hashes = {str(path): hashlib.sha256(path.read_bytes()).hexdigest() for path in helpers}
    report = dict(passed=False, rendererSha256=runtime_hashes[str(renderer)], runtimeSha256=runtime_hashes,
                  helperSha256=helper_hashes, captures=0, runs=[], exports=[], imageChecks=[])
    models = {}
    for profile in ("metallic-roughness", "diffuse-only", "factors", "textured", "texture-control",
                    "alpha-wrong", "uv1", "baked-uv", "wrong-uv", "edited", "animated", "posed"):
        models[profile] = sources / f"{profile}.gltf"
        fixture(models[profile], profile)
    input_hashes = {path.name: hashlib.sha256(path.read_bytes()).hexdigest() for path in models.values()}
    report["fixtureSha256"] = input_hashes

    def verify_runtime():
        current = [renderer, *renderer.parent.glob("*.dll"), *(renderer.parent / "spv_shaders").glob("*.spv")]
        assert {str(path) for path in current} == set(runtime_hashes), "Runtime inventory changed"
        assert all(hashlib.sha256(Path(path).read_bytes()).hexdigest() == digest
                   for path, digest in {**runtime_hashes, **helper_hashes}.items()), "Runtime or helper changed"

    def capture(name, model, *, frames=(4,), events=(), save=None, animated=False):
        verify_runtime()
        base = output / name
        source, buffers = load_asset(model)
        buffer_hashes = [hashlib.sha256(data).hexdigest() for data in buffers]
        camera_node, = (index for index, node in enumerate(source["nodes"])
                        if node.get("name") == "Reference orthographic camera")
        sequence = base.with_suffix(".sequence.json")
        sequence.write_text(json.dumps(dict(schemaVersion=1, frames=max(frames),
            sampleFrames=list(frames), events=list(events))), encoding="utf-8")
        command = [
            str(renderer), "--hidden", "--validation", "--no-ui", "--screenshot-viewport",
            "--width", str(WIDTH), "--height", str(HEIGHT), "--msaa", "1", "--fixed-dt", ".016666667",
            "--model", str(model), "--gltf-camera-node", str(camera_node), "--import-scale", "1",
            "--render-technique", "path-tracing", "--environment-intensity", "0",
            "--directional-intensity", str(INTENSITY), "--directional-direction", "0", "0", "-1",
            "--directional-color", "1", "1", "1", "--no-bloom", "--no-taa", "--display-mode", "lit",
            "--exposure", "1", "--path-spp", "4", "--path-samples", str(SAMPLES), "--path-bounces", "2",
            "--capture-sequence", str(sequence), "--screenshot", str(base.with_suffix(".exr"))]
        if animated:
            command += ["--animation-clip", "0", "--animation-time", ".5", "--animation-paused"]
        if save is not None:
            save.parent.mkdir(parents=True, exist_ok=True)
            command += ["--save-scene", str(save)]
        log = base.with_suffix(".log")
        model_hash = hashlib.sha256(model.read_bytes()).hexdigest()
        with log.open("w", encoding="utf-8") as stream:
            process = subprocess.run(command, cwd=renderer.parent, stdout=stream, stderr=subprocess.STDOUT,
                timeout=240, env=dict(os.environ, VK_LAYER_SETTINGS_PATH=str(output)),
                creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
        assert process.returncode == 0 and not validation_log_error(log.read_text(encoding="utf-8", errors="replace")), log
        verify_runtime()
        assert hashlib.sha256(model.read_bytes()).hexdigest() == model_hash, "Scene input changed during capture"
        _, current_buffers = load_asset(model)
        assert [hashlib.sha256(data).hexdigest() for data in current_buffers] == buffer_hashes, "Scene buffer changed"
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
            if animated:
                player = state["gltfAnimation"]
                assert player["selectedClip"] == 0 and player["time"] == .5 and not player["playing"], player
            samples.append(dict(pixels=pixels[:, :, ::-1], path=reference, image=str(image)))
            report["captures"] += 1
        report["runs"].append(dict(name=name, log=str(log), command=command, modelSha256=model_hash,
            bufferSha256=buffer_hashes,
            samples=[dict(image=sample["image"], referencePath=sample["path"]) for sample in samples]))
        return samples

    try:
        images = {}
        for profile in ("metallic-roughness", "diffuse-only", "factors", "textured", "texture-control",
                        "alpha-wrong", "uv1", "baked-uv", "wrong-uv", "edited", "posed"):
            images[profile] = capture(profile, models[profile])[0]["pixels"]
        diffuse = np.asarray([.2, .4, .6])
        np.testing.assert_allclose(images["metallic-roughness"], np.broadcast_to(INTENSITY * diffuse / math.pi,
            images["metallic-roughness"].shape), rtol=0, atol=2e-6)
        np.testing.assert_array_equal(images["diffuse-only"], images["metallic-roughness"])
        specular = np.asarray([.1, .2, .3])
        analytic = INTENSITY * ((1 - specular) * diffuse * (1 - specular.max()) / math.pi +
                                specular / (4 * math.pi * .75 ** 4))
        np.testing.assert_allclose(images["factors"], np.broadcast_to(analytic, images["factors"].shape), rtol=0, atol=3e-6)
        report["imageChecks"].append(texture_interval(images["textured"], images["texture-control"]))
        report["imageChecks"].append(dict(name="packed-alpha-is-linear", **changed(images["textured"], images["alpha-wrong"])))
        np.testing.assert_array_equal(images["uv1"], images["baked-uv"])
        report["imageChecks"].append(dict(name="uv1-transform-visible", **changed(images["uv1"], images["wrong-uv"])))
        animated = capture("animated", models["animated"], animated=True)[0]["pixels"]
        np.testing.assert_allclose(animated, images["posed"], rtol=2e-6, atol=3e-7)
        history = capture("material-history", models["uv1"], frames=(4, 8, 12, 16), events=(
            dict(frame=5, materialEdit=dict(sourceMaterial=0, values=[
                dict(property=f"extensions/{SG}/specularFactor", value=[.1, .7, .2]),
                dict(property=f"extensions/{SG}/glossinessFactor", value=.2)])),
            dict(frame=9, sceneHistory="undo"), dict(frame=13, sceneHistory="redo")))
        assert [sample["path"]["historyResets"] for sample in history] == [1, 2, 3, 4], history
        for index in (0, 2):
            np.testing.assert_array_equal(history[index]["pixels"], images["uv1"])
        for index in (1, 3):
            np.testing.assert_array_equal(history[index]["pixels"], images["edited"])
        portable = []
        for extension in ("gltf", "glb"):
            saved = output / "exports" / extension / f"specular-glossiness.{extension}"
            saved_pixels = capture(f"save-{extension}", models["animated"], save=saved, animated=True)[0]["pixels"]
            np.testing.assert_array_equal(saved_pixels, animated)
            report["exports"].append(inspect_export(saved))
            copied, _ = portable_copy(saved, output / "portable" / extension)
            portable.append((extension, copied))
        sources.rename(output / "sources-removed")
        for extension, model in portable:
            restored = capture(f"reload-{extension}", model, animated=True)[0]["pixels"]
            np.testing.assert_array_equal(restored, animated)
        verify_runtime()
        assert all(hashlib.sha256((output / "sources-removed" / name).read_bytes()).hexdigest() == digest
                   for name, digest in input_hashes.items()), "Original scene fixtures changed"
        assert report["captures"] == 20 and len(report["runs"]) == 17 and len(report["exports"]) == 2, report
        report["passed"] = True
    finally:
        (output / "results.json").write_text(json.dumps(report, indent=2), encoding="utf-8")
    print("PASS authored SG factors, color and alpha textures, UV1, animation, history and portable raw reload", flush=True)


if __name__ == "__main__":
    main()
