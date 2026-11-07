"""Compare authored glTF opacity micromaps with the sampled-alpha renderer."""

import argparse
import json
import os
from pathlib import Path
import struct
import subprocess

os.environ.setdefault("OPENCV_IO_ENABLE_OPENEXR", "1")
import cv2
import numpy as np
from PIL import Image

from gltf_accessor_runtime_regression import Asset, load_asset
from gltf_extended_materials_runtime_regression import validation_log_error


EXTENSION = "EXT_mesh_opacity_micromap"
FLAG = "--experimental-opacity-micromaps"


def quad(left, right, low, high, z):
    return [[left, low, z], [right, low, z], [right, high, z], [left, high, z]]


def fixture(path, malformed=False):
    asset = Asset()
    source = asset.model
    texture = asset.image(np.array([[[180, 90, 40, 0], [180, 90, 40, 255]]], dtype=np.uint8))

    def primitive(positions, coordinates, material):
        indices = [i + first for first in range(0, len(positions), 4) for i in (0, 1, 2, 0, 2, 3)]
        return {"attributes": {"POSITION": asset.accessor("VEC3", positions),
                               "NORMAL": asset.accessor("VEC3", [[0, 0, 1]] * len(positions)),
                               "TEXCOORD_0": asset.accessor("VEC2", coordinates)},
                "indices": asset.accessor("SCALAR", indices, 5123, indices=True), "material": material}

    receiver = primitive(quad(-2.5, 2.5, -2, 2, 0), [[.5, .5]] * 4, 0)
    blocker = primitive(quad(-.9, -.25, -.55, .55, 1) + quad(.25, .9, -.55, .55, 1),
                        [[.25, .5]] * 4 + [[.75, .5]] * 4, 1)
    data = asset.view(bytes((0, 1)))
    triangles = asset.view(struct.pack("<IHHIHH", 0, 0, 1, 1, 0, 1))
    lookup = asset.accessor("SCALAR", [0, 0, 1, 1], 5125)
    blocker["extensions"] = {EXTENSION: {"micromap": (1 << 32) if malformed else 0,
                                          "micromapIndices": lookup}}
    source["extensions"] = {EXTENSION: {"micromaps": [{"data": data, "triangles": triangles,
        "usageCounts": [2], "usageLevels": [0], "usageFormats": [1]}]}}
    source["meshes"] = [{"primitives": [receiver]}, {"primitives": [blocker]}]
    source["nodes"] = [{"mesh": 0, "name": "Receiver"}, {"mesh": 1, "name": "Baked cutout"},
                       {"camera": 0, "translation": [0, 0, 4]}]
    source["scenes"] = [{"nodes": [0, 1, 2]}]
    source["cameras"] = [{"type": "orthographic", "orthographic": {"xmag": 2, "ymag": 1.5,
                                                                      "znear": .1, "zfar": 20}}]
    source["materials"] = [
        {"pbrMetallicRoughness": {"baseColorFactor": [.65, .7, .75, 1], "metallicFactor": 0}},
        {"alphaMode": "MASK", "alphaCutoff": .5, "doubleSided": True,
         "pbrMetallicRoughness": {"baseColorFactor": [1, 1, 1, 1], "baseColorTexture": {"index": texture},
                                 "metallicFactor": 0}},
    ]
    source["extensionsUsed"] = [EXTENSION]
    asset.write(path)


def pixels(path):
    if path.suffix == ".exr":
        values = cv2.imdecode(np.frombuffer(path.read_bytes(), np.uint8), cv2.IMREAD_UNCHANGED)
        assert values is not None and values.dtype == np.float32 and np.isfinite(values).all(), path
        return values
    with Image.open(path) as image:
        return np.array(image.convert("RGB"))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--renderer", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--generate-only", action="store_true")
    args = parser.parse_args()
    renderer, output = args.renderer.resolve(), args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    (output / "vk_layer_settings.txt").write_text("khronos_validation.validate_sync = true\n", encoding="utf-8")
    model, malformed = output / "cutout.gltf", output / "malformed.gltf"
    fixture(model)
    fixture(malformed, malformed=True)
    if args.generate_only:
        return

    evidence = {}

    def capture(name, source, *, enable, technique="path-tracing", events=(), save=None, samples=1, camera_node=2):
        count = max((event["frame"] for event in events), default=1)
        stem = output / name
        suffix = ".exr" if technique == "path-tracing" else ".png"
        image = stem.with_suffix(suffix)
        sequence = stem.with_suffix(".sequence.json")
        sequence.write_text(json.dumps({"schemaVersion": 1, "frames": count, "sampleFrames": list(range(1, count + 1)),
                                        "events": events}), encoding="utf-8")
        command = [str(renderer), "--model", str(source), "--import-scale", "1", "--hidden", "--no-ui", "--validation",
                   "--width", "256", "--height", "192", "--msaa", str(samples), "--no-taa", "--no-bloom", "--no-ddgi",
                   "--gltf-camera-node", str(camera_node), "--render-technique", technique, "--display-mode", "lit",
                   "--ray-shadows", "hard", "--no-ray-shadow-denoise", "--environment-intensity", "0",
                   "--directional-intensity", "2", "--directional-direction", "1", "0", "-1",
                   "--directional-color", "1", "1", "1", "--path-spp", "8", "--path-samples", "8", "--path-bounces", "1",
                   "--capture-sequence", str(sequence), "--screenshot", str(image)]
        if enable:
            command.append(FLAG)
        if save is not None:
            command += ["--save-scene", str(save)]
        log = stem.with_suffix(".log")
        with log.open("w", encoding="utf-8") as stream:
            subprocess.run(command, cwd=renderer.parent, stdout=stream, stderr=subprocess.STDOUT, check=True,
                           timeout=240, env=dict(os.environ, VK_LAYER_SETTINGS_PATH=str(output)))
        text = log.read_text(encoding="utf-8", errors="replace")
        assert not validation_log_error(text), log
        images, states = [], []
        for frame in range(1, count + 1):
            current = stem if frame == count else output / f"{name}.frame-{frame:04d}"
            images.append(pixels(Path(f"{current}{suffix}")))
            telemetry = json.loads(Path(f"{current}.telemetry.json").read_text(encoding="utf-8"))
            states.append(telemetry["rayShadows"])
            if technique == "path-tracing":
                path = telemetry["referencePath"]
                assert path["attemptedSamples"] == 8 and not path["unsupportedSamples"] and not path["rejectedSamples"], path
        evidence[name] = {"rayShadows": states, "command": command}
        return images, states, text

    events = [{"frame": 2, "materialEdit": {"sourceMaterial": 1, "values": [{"property": "alphaCutoff", "value": 0}]}},
              {"frame": 3, "sceneHistory": "undo"}, {"frame": 4, "sectionPlane": [0, 0, -1, .5]}]
    baseline, _, _ = capture("reference-baseline", model, enable=False, events=events)
    saved = output / "portable.glb"
    accelerated, states, _ = capture("reference-micromaps", model, enable=True, events=events, save=saved)
    assert states[0]["opacityMicromaps"]["deviceEnabled"], states[0]
    assert [state["build"]["opacityMicromapBlas"] for state in states] == [1, 0, 1, 1], states
    assert states[0]["build"]["opacityMicromapBytes"] > 0, states[0]
    for raw, accelerated_image in zip(baseline, accelerated, strict=True):
        np.testing.assert_array_equal(raw, accelerated_image)
    assert np.count_nonzero(accelerated[0] != accelerated[1]) > 100
    assert np.count_nonzero(accelerated[0] != accelerated[3]) > 100
    np.testing.assert_array_equal(accelerated[0], accelerated[2])

    fallback, bad_states, bad_log = capture("malformed-fallback", malformed, enable=True)
    assert "opacity_micromap_fallback" in bad_log and bad_states[0]["build"]["opacityMicromapBlas"] == 0
    np.testing.assert_array_equal(fallback[0], baseline[0])
    portable, _ = load_asset(saved)
    assert EXTENSION not in portable.get("extensionsUsed", []) and EXTENSION not in portable.get("extensions", {})
    assert all(EXTENSION not in prim.get("extensions", {}) for mesh in portable["meshes"] for prim in mesh["primitives"])
    saved_camera, = (index for index, node in enumerate(portable["nodes"]) if "camera" in node)
    reloaded, reload_states, _ = capture("portable-reload", saved, enable=True, camera_node=saved_camera)
    assert reload_states[0]["build"]["opacityMicromapBlas"] == 0
    np.testing.assert_array_equal(reloaded[0], baseline[0])

    for technique, samples, count in (("deferred-raster", 1, 1), ("forward-raster", 4, 0)):
        controls = [{"frame": 2, "sectionPlane": [0, 0, -1, .5]}]
        raw, _, _ = capture(f"{technique}-baseline", model, enable=False, technique=technique, events=controls, samples=samples)
        accelerated, states, _ = capture(f"{technique}-micromaps", model, enable=True, technique=technique, events=controls, samples=samples)
        assert [state["build"]["opacityMicromapBlas"] for state in states] == [count, count], states
        assert np.count_nonzero(raw[0] != raw[1]) > 100
        for reference, actual in zip(raw, accelerated, strict=True):
            np.testing.assert_array_equal(reference, actual)

    (output / "results.json").write_text(json.dumps(evidence, indent=2) + "\n", encoding="utf-8")
    print("PASS authored opacity micromaps, alpha-edit invalidation, clip parity, and portable fallback", flush=True)


if __name__ == "__main__":
    main()
