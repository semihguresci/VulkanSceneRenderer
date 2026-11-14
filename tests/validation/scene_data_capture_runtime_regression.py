"""Verify deferred glTF RGB and compositing data exported from one Vulkan frame."""

import argparse
import base64
from io import BytesIO
import json
import math
import os
from pathlib import Path
import struct
import subprocess

os.environ.setdefault("OPENCV_IO_ENABLE_OPENEXR", "1")
import cv2
import numpy as np
from PIL import Image

from display_capture_test_support import validation_log_error
from gltf_accessor_runtime_regression import Asset


WIDTH, HEIGHT = 320, 240
FOV = 45
FOCAL_LENGTH = HEIGHT / (2 * math.tan(math.radians(FOV / 2)))
CAMERA = np.array([3.0, 0.0, 4.0])
FORWARD = np.array([-.6, 0.0, -.8])
RIGHT = np.array([.8, 0.0, -.6])
UP = np.array([0.0, 1.0, 0.0])
NORMAL_TEXEL = [204, 128, 230]
FRONT = (28, 100, 150, 172)
REAR = (56, 115, 119, 185)
INSTANCES = ((198, 115, 245, 150), (249, 173, 296, 208))
BLENDED = (45, 38, 110, 74)
CHANNELS = {"Z": 2, "N.X": 2, "N.Y": 2, "N.Z": 2, "ObjectID": 0}


def camera_point(x, y, depth):
    # World right/up become pixel right/up for this yawed camera. Image rows
    # increase downward because scene rasterization uses a flipped viewport.
    return (CAMERA + depth * FORWARD + (x - WIDTH / 2) * depth / FOCAL_LENGTH * RIGHT
            + (HEIGHT / 2 - y) * depth / FOCAL_LENGTH * UP)


def fixture(path):
    asset = Asset()

    def texture(mode, size, texels):
        image = Image.new(mode, size)
        image.putdata(texels)
        payload = BytesIO()
        image.save(payload, format="PNG")
        images = asset.model.setdefault("images", [])
        images.append({"uri": "data:image/png;base64," + base64.b64encode(payload.getvalue()).decode("ascii")})
        textures = asset.model.setdefault("textures", [])
        textures.append({"source": len(images) - 1, "sampler": 0})
        return len(textures) - 1

    def panel(name, bounds, depth, color, *, origin=None, alpha="OPAQUE", mapped=False):
        x0, y0, x1, y1 = bounds
        vertices = [camera_point(x, y, depth) for x, y in ((x0, y1), (x1, y1), (x1, y0), (x0, y0))]
        if origin is not None:
            vertices = [point - origin for point in vertices]
        pbr = {"baseColorFactor": [0, 0, 0, .5 if alpha == "BLEND" else 1], "metallicFactor": 0}
        material = {"emissiveFactor": color, "pbrMetallicRoughness": pbr, "alphaMode": alpha}
        if alpha == "MASK":
            pbr["baseColorTexture"] = {"index": texture("RGBA", (2, 1), [(255, 255, 255, 255), (255, 255, 255, 0)])}
            material["alphaCutoff"] = .5
        if mapped:
            material["normalTexture"] = {"index": texture("RGB", (1, 1), [tuple(NORMAL_TEXEL)])}
        materials, meshes = asset.model["materials"], asset.model["meshes"]
        primitive = {
            "attributes": {
                "POSITION": asset.accessor("VEC3", vertices),
                "NORMAL": asset.accessor("VEC3", [-FORWARD] * 4),
                "TANGENT": asset.accessor("VEC4", [[*RIGHT, 1]] * 4),
                "TEXCOORD_0": asset.accessor("VEC2", [[0, 1], [1, 1], [1, 0], [0, 0]]),
            },
            "indices": asset.accessor("SCALAR", [0, 1, 2, 0, 2, 3], 5123, indices=True),
            "material": len(materials),
        }
        node = {"name": name, "mesh": len(meshes)}
        materials.append(material)
        meshes.append({"primitives": [primitive]})
        return node

    asset.model["samplers"] = [{"magFilter": 9728, "minFilter": 9728, "wrapS": 33071, "wrapT": 33071}]
    front = panel("Non-selectable mask", FRONT, 3, [1, 0, 0], alpha="MASK", mapped=True)
    front["extensions"] = {"KHR_node_selectability": {"selectable": False}}
    rear = panel("Rear", REAR, 5, [0, 1, 0])
    # Both instances use one authored owner name, mesh, and primitive.
    instance_origin = camera_point(INSTANCES[0][0], INSTANCES[0][1], 4)
    instances = panel("Shared instance name", INSTANCES[0], 4, [0, 0, 1], origin=instance_origin)
    translations = [camera_point(x0, y0, 4) for x0, y0, _, _ in INSTANCES]
    instances["extensions"] = {"EXT_mesh_gpu_instancing": {
        "attributes": {"TRANSLATION": asset.accessor("VEC3", translations)},
    }}
    blended = panel("Blend-only surface", BLENDED, 2, [1, 0, 1], alpha="BLEND")
    asset.model["extensionsUsed"] = ["EXT_mesh_gpu_instancing", "KHR_node_selectability"]
    asset.model["extensionsRequired"] = ["EXT_mesh_gpu_instancing"]
    asset.model["nodes"] = [front, rear, instances, blended]
    asset.model["scenes"] = [{"nodes": list(range(4))}]
    asset.write(path)


def read_data_exr(path):
    # Decode the OpenEXR v2 single-part, uncompressed scanline layout directly.
    # Retain integer channel values even where a FLOAT could not represent them.
    def string(payload, position):
        end = payload.index(0, position)
        return payload[position:end].decode("ascii"), end + 1

    payload = path.read_bytes()
    assert struct.unpack_from("<II", payload) == (20000630, 2), path
    attributes = {}
    position = 8
    while payload[position]:
        name, position = string(payload, position)
        kind, position = string(payload, position)
        size, = struct.unpack_from("<I", payload, position)
        position += 4
        assert name not in attributes and size <= len(payload) - position, (path, name)
        attributes[name] = kind, payload[position:position + size]
        position += size
    assert attributes["compression"] == ("compression", b"\0"), path
    window = struct.pack("<iiii", 0, 0, WIDTH - 1, HEIGHT - 1)
    assert attributes["dataWindow"] == attributes["displayWindow"] == ("box2i", window), path
    kind, channel_list = attributes["channels"]
    assert kind == "chlist", path
    channels = {}
    cursor = 0
    while channel_list[cursor]:
        name, cursor = string(channel_list, cursor)
        pixel_type, _, x_sampling, y_sampling = struct.unpack_from("<iB3xii", channel_list, cursor)
        cursor += 16
        assert name not in channels and x_sampling == y_sampling == 1, (path, name)
        channels[name] = pixel_type
    assert cursor == len(channel_list) - 1 and channels == CHANNELS, (path, channels)

    values = {name: np.empty((HEIGHT, WIDTH), dtype="<u4" if pixel_type == 0 else "<f4")
              for name, pixel_type in channels.items()}
    offsets = struct.unpack_from(f"<{HEIGHT}Q", payload, position + 1)
    scanline_size = WIDTH * 4 * len(channels)
    first_offset = position + 1 + 8 * HEIGHT
    seen = set()
    for offset in offsets:
        assert first_offset <= offset <= len(payload) - 8 - scanline_size, (path, offset)
        row, size = struct.unpack_from("<iI", payload, offset)
        assert 0 <= row < HEIGHT and row not in seen and size == scanline_size, (path, row, size)
        seen.add(row)
        for plane, name in enumerate(channels):
            values[name][row, :] = np.frombuffer(payload, values[name].dtype, WIDTH, offset + 8 + plane * WIDTH * 4)
    assert len(seen) == HEIGHT
    return values


def read_rgb(path):
    metadata = json.loads(Path(f"{path}.capture.json").read_bytes())
    assert (metadata["width"], metadata["height"]) == (WIDTH, HEIGHT), metadata
    assert metadata["source"] == "raster-scene-composite" and metadata["colorSpace"] == "linear-sRGB", metadata
    assert not (metadata["includesUi"] or metadata["exposureApplied"] or metadata["toneMappingApplied"]), metadata
    bgr = cv2.imdecode(np.frombuffer(path.read_bytes(), dtype=np.uint8), cv2.IMREAD_UNCHANGED)
    assert bgr.shape == (HEIGHT, WIDTH, 3) and bgr.dtype == np.float32 and np.isfinite(bgr).all(), path
    return bgr[:, :, ::-1]


def rectangle(bounds):
    x0, y0, x1, y1 = bounds
    mask = np.zeros((HEIGHT, WIDTH), dtype=bool)
    mask[y0:y1, x0:x1] = True
    return mask


def verify(data, table, rgb, state):
    ids = data["ObjectID"]
    normals = np.stack([data[f"N.{component}"] for component in "XYZ"], axis=-1)
    assert (table["schemaVersion"], table["width"], table["height"]) == (1, WIDTH, HEIGHT), table
    assert table["backgroundId"] == 0 and table["depthUnits"] == "metres" and table["normalSpace"] == "world", table
    front = rectangle((FRONT[0], FRONT[1], (FRONT[0] + FRONT[2]) // 2, FRONT[3]))
    rear = rectangle(REAR) & ~front
    first, second = map(rectangle, INSTANCES)
    blended = rectangle(BLENDED)
    background = ~(front | rear | first | second)
    np.testing.assert_array_equal(ids == 0, background, err_msg="Opaque data coverage or alpha mask is wrong")
    np.testing.assert_array_equal(data["Z"][background], 0)
    np.testing.assert_array_equal(normals[background], 0)
    holes = rectangle(FRONT) & ~front
    assert np.any(holes & rear) and np.any(holes & background)
    np.testing.assert_array_equal(ids[blended], 0, err_msg="Data captured transparent picking IDs")

    tangent_normal = np.array(NORMAL_TEXEL, dtype=float) * (2 / 255) - 1
    mapped = np.stack([RIGHT, UP, -FORWARD], axis=1) @ (tangent_normal / np.linalg.norm(tangent_normal))
    spatial_checks = (
        (front, 3, mapped, 0, None), (rear, 5, -FORWARD, 1, None),
        (first, 4, -FORWARD, 2, 0), (second, 4, -FORWARD, 2, 1),
    )
    objects = table["objects"]
    assert all(type(row["id"]) is int and row["id"] > 0 for row in objects), objects
    lookup = {row["id"]: row for row in objects}
    assert len(lookup) == len(objects) == 4 and len({row["graphNode"] for row in objects}) == 4, objects
    seen = set()
    for region, depth, normal, node, instance in spatial_checks:
        observed, = np.unique(ids[region]).tolist()
        assert observed > 0 and observed not in seen and observed in lookup, observed
        seen.add(observed)
        entry = lookup[observed]
        assert (entry["sourceNode"], entry["sourceMesh"], entry["sourcePrimitive"], entry["sourceInstance"]) == (
            node, node, 0, instance), entry
        np.testing.assert_allclose(data["Z"][region], depth, atol=.002, rtol=0)
        np.testing.assert_allclose(normals[region], np.broadcast_to(normal, normals[region].shape), atol=.003, rtol=0)
    assert set(lookup) == seen and {row["name"] for row in objects if row["sourceNode"] == 2} == {"Shared instance name"}, objects
    front_id = int(ids[front].flat[0])
    front_node, = (node for node in state["gltfNodeFlags"]["nodes"] if node["graphNode"] == lookup[front_id]["graphNode"])
    assert front_node["effectiveVisible"] and not front_node["effectiveSelectable"], front_node

    r, g, b = np.moveaxis(rgb, 2, 0)
    color_checks = (
        ((r > .8) & (g < .04) & (b < .04), front, "red non-selectable front"),
        ((g > .8) & (r < .04) & (b < .04), rear, "green rear"),
        ((b > .8) & (r < .04) & (g < .04), first | second, "blue instances"),
        ((r > .08) & (b > .08) & (g < .04), blended, "magenta transparent surface"),
    )
    for actual, expected, label in color_checks:
        np.testing.assert_array_equal(actual, expected, err_msg=f"RGB coverage differs from data alignment for {label}")
    np.testing.assert_allclose(rgb[background & ~blended], 0, atol=.04, rtol=0)
    return sorted(lookup)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("renderer", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    renderer, output = args.renderer.resolve(), args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    (output / "vk_layer_settings.txt").write_text("khronos_validation.validate_sync = true\n", encoding="ascii")
    model = output / "compositing.gltf"
    fixture(model)

    def capture(name, *flags, invalid=False):
        path = output / name
        command = [
            str(renderer), "--model", str(model), "--import-scale", "1", "--hidden", "--no-ui", "--validation",
            "--width", str(WIDTH), "--height", str(HEIGHT), "--msaa", "1", "--no-taa", "--no-bloom",
            "--render-technique", "deferred-raster", "--display-mode", "lit", "--no-ray-query", "--no-ddgi",
            "--camera-position", *map(str, CAMERA), "--camera-target", "0", "0", "0", "--camera-fov", str(FOV),
            "--sky", "plain", "--sky-horizon", "0", "0", "0", "--environment-intensity", "0",
            "--directional-intensity", "0", "--bounce-intensity", "0",
            "--warmup-frames", "4", "--capture-frame", "5", "--screenshot", str(path), *flags,
        ]
        log = path.with_suffix(".log")
        with log.open("w", encoding="utf-8") as stream:
            result = subprocess.run(command, cwd=renderer.parent, stdout=stream, stderr=subprocess.STDOUT,
                                    timeout=150, env=dict(os.environ, VK_LAYER_SETTINGS_PATH=str(output)),
                                    creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
        contents = log.read_text(encoding="utf-8", errors="replace")
        assert not validation_log_error(contents), log
        if invalid:
            assert result.returncode != 0 and "--capture-data requires --screenshot FILE.exr" in contents, log
            assert not path.exists() and not path.with_suffix(".data.exr").exists(), log
            return path, None
        assert result.returncode == 0, log
        telemetry = json.loads(path.with_suffix(".telemetry.json").read_bytes())
        assert telemetry["technique"] == "deferred-raster" and telemetry["validationEnabled"], telemetry
        assert telemetry["resolution"] == [WIDTH, HEIGHT] and telemetry["msaaSamples"] == 1, telemetry
        assert not telemetry["taa"]["enabled"] and not telemetry["fsr"]["active"] and not telemetry["dlss"]["active"], telemetry
        return path, telemetry

    capture("invalid.png", "--capture-data", invalid=True)
    scene, state = capture("scene.exr", "--capture-data")
    ordinary, ordinary_state = capture("ordinary.exr")
    active_passes = {entry["name"] for entry in state["passes"] if entry["active"]}
    assert {"GBuffer", "TransparentPick", "OitResolve"} <= active_passes
    assert not ordinary.with_suffix(".data.exr").exists() and not ordinary.with_suffix(".data.json").exists()
    assert state["camera"]["unjitteredViewProjColumns"] == ordinary_state["camera"]["unjitteredViewProjColumns"]
    rgb = read_rgb(scene)
    np.testing.assert_array_equal(rgb, read_rgb(ordinary), err_msg="Requesting data changed the scene RGB capture")
    data = read_data_exr(scene.with_suffix(".data.exr"))
    lookup = json.loads(scene.with_suffix(".data.json").read_bytes())
    object_ids = verify(data, lookup, rgb, state)
    (output / "results.json").write_text(json.dumps({
        "passed": True, "build": state["build"], "gpu": state["gpu"],
        "channels": CHANNELS, "objectIds": object_ids,
    }, indent=2) + "\n", encoding="utf-8")
    print("PASS FLOAT depth and shading normals, UINT IDs, alpha-mask and instance identity, and exact RGB alignment",
          flush=True)


if __name__ == "__main__":
    main()
