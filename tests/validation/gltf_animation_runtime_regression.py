"""Opt-in native glTF animation/pointer publication checks in both raster paths.

Creates its own glTF buffers, captures independently specified paused times,
checks Vulkan/synchronization validation, and verifies semantic telemetry plus
visible pixel changes. Requires a Vulkan display/runtime and Pillow.
"""
from __future__ import annotations

import argparse
import base64
import hashlib
import json
import math
import os
from pathlib import Path
import re
import struct
import subprocess
import zlib


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


def pattern_png():
    """Embedded asymmetric RGB atlas; a fractional offset cannot wrap to identity."""
    rows = bytearray()
    for y in range(16):
        rows.append(0)  # PNG filter: None.
        for x in range(16):
            color = (240, 35, 20) if x < 5 else ((25, 200, 60) if y < 6 else (30, 55, 235))
            if 9 <= x < 13 and 8 <= y < 12:
                color = (245, 210, 20)
            rows.extend(color)
    def chunk(kind, content):
        return struct.pack(">I", len(content)) + kind + content + struct.pack(">I", zlib.crc32(kind + content))
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", 16, 16, 8, 2, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(bytes(rows))) + chunk(b"IEND", b""))


def write_fixture(path, texture_motion=False):
    """A flat-normal cube with independent TRS/material/light/camera clips."""
    data = bytearray()
    views, accessors = [], []

    def accessor(kind, values, component=5126):
        while len(data) % 4:
            data.append(0)
        offset = len(data)
        flat = [x for value in values for x in value] if isinstance(values[0], tuple) else values
        data.extend(struct.pack("<" + ("f" if component == 5126 else "H") * len(flat), *flat))
        view = len(views)
        views.append({"buffer": 0, "byteOffset": offset, "byteLength": len(data) - offset})
        result = {"bufferView": view, "componentType": component, "count": len(values), "type": kind}
        if kind == "SCALAR" and component == 5126:
            result.update(min=[min(values)], max=[max(values)])
        elif kind == "VEC3":
            result.update(min=[min(v[i] for v in values) for i in range(3)],
                          max=[max(v[i] for v in values) for i in range(3)])
        accessors.append(result)
        return len(accessors) - 1

    faces = [
        ((0, 0, 1), [(-.5, -.5, .5), (.5, -.5, .5), (.5, .5, .5), (-.5, .5, .5)]),
        ((0, 0, -1), [(.5, -.5, -.5), (-.5, -.5, -.5), (-.5, .5, -.5), (.5, .5, -.5)]),
        ((1, 0, 0), [(.5, -.5, .5), (.5, -.5, -.5), (.5, .5, -.5), (.5, .5, .5)]),
        ((-1, 0, 0), [(-.5, -.5, -.5), (-.5, -.5, .5), (-.5, .5, .5), (-.5, .5, -.5)]),
        ((0, 1, 0), [(-.5, .5, .5), (.5, .5, .5), (.5, .5, -.5), (-.5, .5, -.5)]),
        ((0, -1, 0), [(-.5, -.5, -.5), (.5, -.5, -.5), (.5, -.5, .5), (-.5, -.5, .5)]),
    ]
    positions = accessor("VEC3", [v for _, vertices in faces for v in vertices])
    normals = accessor("VEC3", [normal for normal, vertices in faces for _ in vertices])
    indices = accessor("SCALAR", [face * 4 + i for face in range(6) for i in (0, 1, 2, 0, 2, 3)], 5123)
    times = accessor("SCALAR", [0., 1.])
    translation = accessor("VEC3", [(-.6, 0, 0), (.6, 0, 0)])
    color = accessor("VEC4", [(1., .02, .02, 1.), (.02, .02, 1., 1.)])
    intensity = accessor("SCALAR", [2., 80.])
    yfov = accessor("SCALAR", [.5, 1.])
    if texture_motion:
        uv = accessor("VEC2", [(0., 1.), (1., 1.), (1., 0.), (0., 0.)] * 6)
        offset = accessor("VEC2", [(.05, .05), (.42, .2)])

    def animation(name, output, path, *, node=None, pointer=None):
        target = {"path": path}
        if node is not None:
            target["node"] = node
        if pointer:
            target["extensions"] = {"KHR_animation_pointer": {"pointer": pointer}}
        return {"name": name, "samplers": [{"input": times, "output": output, "interpolation": "LINEAR"}],
                "channels": [{"sampler": 0, "target": target}]}

    model = {
        "asset": {"version": "2.0", "generator": "Container authored animation regression fixture"},
        "extensionsUsed": ["KHR_animation_pointer", "KHR_lights_punctual"],
        "extensions": {"KHR_lights_punctual": {"lights": [
            {"type": "point", "color": [1, 1, 1], "intensity": 30, "range": 10}]}},
        "scenes": [{"nodes": [0, 1, 2]}], "scene": 0,
        "nodes": [{"name": "Animated cube", "mesh": 0},
                  {"name": "Animated light", "translation": [2, 2, 3],
                   "extensions": {"KHR_lights_punctual": {"light": 0}}},
                  {"name": "Authored camera", "translation": [0, 0, 3], "camera": 0}],
        "cameras": [{"type": "perspective", "perspective": {"yfov": .5, "znear": .1, "zfar": 100}}],
        "materials": [{"name": "Animated material", "pbrMetallicRoughness": {
            "baseColorFactor": [.8, .8, .8, 1], "metallicFactor": 0, "roughnessFactor": .6}}],
        "meshes": [{"primitives": [{"attributes": {"POSITION": positions, "NORMAL": normals},
                                     "indices": indices, "material": 0}]}],
        "animations": [animation("Translation", translation, "translation", node=0),
                       animation("Material color", color, "pointer", pointer="/materials/0/pbrMetallicRoughness/baseColorFactor"),
                       animation("Light intensity", intensity, "pointer", pointer="/extensions/KHR_lights_punctual/lights/0/intensity"),
                       animation("Camera field of view", yfov, "pointer", pointer="/cameras/0/perspective/yfov")],
        "bufferViews": views, "accessors": accessors,
        "buffers": [{"byteLength": len(data), "uri": "data:application/octet-stream;base64," + base64.b64encode(data).decode()}],
    }
    if texture_motion:
        model["extensionsUsed"].extend(["KHR_texture_transform", "KHR_materials_unlit"])
        model["images"] = [{"uri": "data:image/png;base64," + base64.b64encode(pattern_png()).decode()}]
        model["samplers"] = [{"magFilter": 9729, "minFilter": 9729, "wrapS": 33071, "wrapT": 33071}]
        model["textures"] = [{"source": 0, "sampler": 0}]
        material = model["materials"][0]
        material["extensions"] = {"KHR_materials_unlit": {}}
        pbr = material["pbrMetallicRoughness"]
        pbr["baseColorFactor"] = [1, 1, 1, 1]
        pbr["baseColorTexture"] = {"index": 0, "texCoord": 0, "extensions": {"KHR_texture_transform": {
            "offset": [.05, .05], "scale": [.6, .65], "texCoord": 0, "extras": {"authored": "preserve"}}}}
        model["meshes"][0]["primitives"][0]["attributes"]["TEXCOORD_0"] = uv
        model["animations"].append(animation("Visible texture motion", offset, "pointer",
            pointer="/materials/0/pbrMetallicRoughness/baseColorTexture/extensions/KHR_texture_transform/offset"))
    path.write_text(json.dumps(model, indent=2), encoding="utf-8")


def finite_json(value):
    if isinstance(value, float):
        return math.isfinite(value)
    if isinstance(value, dict):
        return all(finite_json(child) for child in value.values())
    if isinstance(value, list):
        return all(finite_json(child) for child in value)
    return True


def capture(renderer, output, fixture, technique, clip, time, timeout, directional_intensity=0):
    name = f"{technique}-clip{clip}-time{time}"
    image = output / f"{name}.png"
    telemetry = image.with_suffix(".telemetry.json")
    image.unlink(missing_ok=True)
    telemetry.unlink(missing_ok=True)
    command = [str(renderer), "--hidden", "--no-ui", "--validation", "--width", "640", "--height", "360",
               "--msaa", "1", "--warmup-frames", "3", "--capture-frame", "4", "--fixed-dt", "0.016666667",
               "--display-mode", "lit", "--no-bloom", "--environment-intensity", "0",
               "--directional-intensity", str(directional_intensity), "--exposure", "1", "--camera-position", "0", "0", "3",
               "--camera-target", "0", "0", "0", "--camera-fov", "60", "--render-technique", technique,
               "--model", str(fixture), "--animation-clip", str(clip), "--animation-time", str(time),
               "--animation-paused", "--screenshot", str(image)]
    if clip == 3:
        command.extend(["--gltf-camera-node", "2"])
    log = output / f"{name}.log"
    with log.open("w", encoding="utf-8") as stream:
        process = subprocess.run(command, cwd=renderer.parent, stdout=stream, stderr=subprocess.STDOUT,
                                 timeout=timeout, env=dict(os.environ, VK_LAYER_SETTINGS_PATH=str(output)),
                                 creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
    text = log.read_text(encoding="utf-8", errors="replace")
    require(process.returncode == 0, f"Native animation process failed ({process.returncode}): {log}")
    require(not re.search(r"VUID-|SYNC-HAZARD|Validation Error", text, re.IGNORECASE), f"Vulkan validation failed: {log}")
    require(image.is_file() and telemetry.is_file(), f"Missing fresh native animation capture: {image}")
    state = json.loads(telemetry.read_text(encoding="utf-8"))
    require(finite_json(state), f"Nonfinite native telemetry: {telemetry}")
    require(state["technique"] == technique, "Wrong raster technique captured")
    animation = state["gltfAnimation"]
    require(animation["available"] and animation["clipCount"] == (5 if clip == 4 else 4) and animation["selectedClip"] == clip,
            f"Wrong animation clip/availability: {telemetry}")
    require(abs(animation["time"] - time) < 1e-6 and not animation["playing"], "Configured paused seek did not hold")
    require(animation["diagnostics"] == 0 and len(animation["targets"]) == 1, "Fixture animation channel was not supported")
    require(state["scene"]["primaryObjectCount"] >= 1, "No primary geometry was rendered")
    return {"image": str(image), "telemetry": str(telemetry), "log": str(log), "command": command, "state": state}


def check_pair(first, second, clip):
    from PIL import Image, ImageChops, ImageStat
    with Image.open(first["image"]) as image:
        before = image.convert("RGB")
    with Image.open(second["image"]) as image:
        after = image.convert("RGB")
    require(before.size == after.size == (640, 360), "Wrong native capture dimensions")
    difference = ImageChops.difference(before, after)
    channels = difference.split()
    maximum = ImageChops.lighter(ImageChops.lighter(channels[0], channels[1]), channels[2])
    histogram = maximum.histogram()
    changed = sum(histogram[8:]) / (before.width * before.height)
    mean_absolute = sum(ImageStat.Stat(difference).mean) / 3
    require(changed >= .002 and mean_absolute >= .1,
            f"Clip {clip} did not visibly change rendered pixels: changed={changed:.4%}, MAE={mean_absolute:.4f}")
    left, right = first["state"]["gltfAnimation"], second["state"]["gltfAnimation"]
    if clip == 0:
        for state, expected in ((left, -.6), (right, .6)):
            node = next(node for node in state["nodes"] if node["sourceIndex"] == 0)
            require(abs(node["translation"][0] - expected) < 1e-6, "Source animation translation is stale")
            require(abs(node["localMatrix"][12] - expected) < 1e-6 and abs(node["worldMatrix"][12] - expected) < 1e-6,
                    "Animated source translation was not published to the runtime graph")
    elif clip == 1:
        for state, expected in ((left, (1, .02, .02, 1)), (right, (.02, .02, 1, 1))):
            material = next(material for material in state["materials"] if material["sourceIndex"] == 0)
            require(max(abs(a - b) for a, b in zip(material["baseColor"], expected)) < 1e-6,
                    "Animated material values were not published to the existing runtime material")
        red, blue = ImageStat.Stat(before).mean, ImageStat.Stat(after).mean
        require(red[0] > red[2] + .5 and blue[2] > blue[0] + .5, "Material capture did not change from red to blue")
    elif clip == 2:
        for state, expected in ((left, 2), (right, 80)):
            require(state["pointLights"] and abs(state["pointLights"][0]["intensity"] - expected) < 1e-6,
                    "Animated intensity was not published to runtime punctual lights")
        first_mean, second_mean = sum(ImageStat.Stat(before).mean), sum(ImageStat.Stat(after).mean)
        require(second_mean > first_mean + .5, "Brighter authored light did not increase rendered brightness")
    elif clip == 3:
        for state, fov in ((left, .5), (right, 1)):
            require(state["selectedCameraNode"] == 2, "Authored camera node was not selected")
            require(abs(state["cameraProjection"][5] - 1 / math.tan(fov / 2)) < 1e-5,
                    "Animated yfov did not reach the camera projection")
    elif clip == 4:
        for state, expected in ((left, (.05, .05)), (right, (.42, .2))):
            target = state["targets"][0]
            require(target["property"] == "pbrMetallicRoughness/baseColorTexture/extensions/KHR_texture_transform/offset",
                    "Texture transform pointer was omitted or redirected")
            require(max(abs(a - b) for a, b in zip(target["value"], expected)) < 1e-6,
                    "Texture offset did not sample the configured time")
        require(left["nodes"] == right["nodes"], "Texture-motion fixture changed geometry")
    return {"passed": True, "changedPixelFraction": changed, "meanAbsolutePixelError": mean_absolute,
            "imageSha256": [hashlib.sha256(Path(record["image"]).read_bytes()).hexdigest() for record in (first, second)]}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--renderer", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--technique", action="append", choices=("deferred-raster", "forward-raster"))
    parser.add_argument("--clip", type=int, action="append", choices=range(5),
                        help="Restrict to one or more clips; default runs all five (4 is embedded-PNG texture motion)")
    parser.add_argument("--directional-intensity", type=float, default=0,
                        help="Optional directional light for authored-camera cascade validation")
    parser.add_argument("--timeout", type=float, default=120)
    args = parser.parse_args()
    try:
        import PIL  # noqa: F401
    except ImportError:
        print("SKIP: native glTF animation validation requires Pillow")
        return 77
    renderer, output = args.renderer.resolve(), args.output.resolve()
    require(math.isfinite(args.directional_intensity) and args.directional_intensity >= 0,
            "Directional intensity must be finite and nonnegative")
    require(renderer.is_file(), f"Renderer executable does not exist: {renderer}")
    output.mkdir(parents=True, exist_ok=True)
    fixture = output / "native-animation.gltf"
    write_fixture(fixture)
    texture_fixture = output / "native-texture-animation.gltf"
    write_fixture(texture_fixture, texture_motion=True)
    (output / "vk_layer_settings.txt").write_text("khronos_validation.validate_sync = true\n", encoding="utf-8")
    results = {"schemaVersion": 1, "passed": False, "renderer": str(renderer),
               "executableSha256": hashlib.sha256(renderer.read_bytes()).hexdigest(),
               "clips": args.clip or [0, 1, 2, 3, 4], "directionalIntensity": args.directional_intensity, "cases": []}
    report = output / "results.json"
    report.write_text(json.dumps(results, indent=2), encoding="utf-8")
    try:
        for technique in args.technique or ("deferred-raster", "forward-raster"):
            for clip in args.clip or range(5):
                pair = {"technique": technique, "clip": clip, "samples": []}
                results["cases"].append(pair)
                for time in (0, 1):
                    pair["samples"].append(capture(renderer, output, texture_fixture if clip == 4 else fixture, technique, clip, time, args.timeout,
                                                   args.directional_intensity))
                    report.write_text(json.dumps(results, indent=2), encoding="utf-8")
                pair["checks"] = check_pair(*pair["samples"], clip)
                print(json.dumps({"technique": technique, "clip": clip, **pair["checks"]}), flush=True)
                report.write_text(json.dumps(results, indent=2), encoding="utf-8")
        require(hashlib.sha256(renderer.read_bytes()).hexdigest() == results["executableSha256"],
                "Renderer executable changed during native validation")
        results["passed"] = True
    except Exception as error:
        results["failure"] = str(error)
        raise
    finally:
        report.write_text(json.dumps(results, indent=2), encoding="utf-8")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
