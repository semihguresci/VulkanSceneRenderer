"""Opt-in pixel regression for opaque scene and BIM culling routes."""

import argparse
import base64
import json
import math
import os
from pathlib import Path
import re
import struct
import subprocess

try:
    from PIL import Image
except ImportError:
    print("SKIP: forward culling regression requires Pillow")
    raise SystemExit(77)


def accessor(gltf, data, values, component, kind):
    flat = [v for row in values for v in row] if kind == "VEC3" else values
    offset = len(data)
    data.extend(struct.pack("<" + ("f" if component == 5126 else "I") * len(flat), *flat))
    view = len(gltf["bufferViews"])
    gltf["bufferViews"].append({"buffer": 0, "byteOffset": offset,
                                "byteLength": len(data) - offset})
    result = {"bufferView": view, "componentType": component,
              "count": len(values), "type": kind}
    if kind == "VEC3":
        result.update(min=[min(row[a] for row in values) for a in range(3)],
                      max=[max(row[a] for row in values) for a in range(3)])
    gltf["accessors"].append(result)
    return len(gltf["accessors"]) - 1


def embed_buffer(gltf, data):
    gltf["buffers"] = [{"byteLength": len(data),
                        "uri": "data:application/octet-stream;base64," +
                               base64.b64encode(data).decode()}]


def fixtures(output):
    # A front face, a reflected front face, a visible double-sided back face,
    # and a single-sided back face that must stay hidden. Every color is unique.
    colors = [[.1, .9, .1, 1], [.9, .1, .1, 1],
              [.85, .1, .85, 1], [.1, .1, .9, 1]]
    positions = [[-.7, -1, 0], [.7, -1, 0], [.7, 1, 0], [-.7, 1, 0]]
    front, back = [0, 1, 2, 0, 2, 3], [0, 2, 1, 0, 3, 2]
    data = bytearray()
    gltf = {"asset": {"version": "2.0"}, "bufferViews": [], "accessors": [],
            "materials": [], "meshes": [], "nodes": [], "scene": 0,
            "scenes": [{"nodes": list(range(4))}]}

    position = accessor(gltf, data, positions, 5126, "VEC3")
    ifcx = {"data": []}
    for index, color in enumerate(colors):
        reversed_face = index >= 2
        sx, x = (-1 if index == 1 else 1), -3 + 2 * index
        indices = back if reversed_face else front
        material = {"doubleSided": index == 2,
                    "pbrMetallicRoughness": {"baseColorFactor": color,
                                            "metallicFactor": 0, "roughnessFactor": .8}}
        gltf["materials"].append(material)
        normal = accessor(gltf, data, [[0, 0, -1 if reversed_face else 1]] * 4, 5126, "VEC3")
        gltf["meshes"].append({"primitives": [{"attributes": {"POSITION": position,
                                                               "NORMAL": normal},
                                               "indices": accessor(gltf, data, indices, 5125, "SCALAR"),
                                               "material": index}]})
        gltf["nodes"].append({"mesh": index, "translation": [x, 0, 0], "scale": [sx, 1, 1]})
        # IFCX source axes are Z up; keep the same renderer-space geometry.
        ifcx["data"].append({"path": f"panel-{index}", "attributes": {
            "gltf::material": material,
            "usd::xformop": {"transform": [[sx, 0, 0, 0], [0, 1, 0, 0],
                                             [0, 0, 1, 0], [x, 0, 0, 1]]},
            "usd::usdgeom::mesh": {"points": [[p[0], 0, p[1]] for p in positions],
                                    "faceVertexCounts": [3, 3],
                                    "faceVertexIndices": indices}}})
    embed_buffer(gltf, data)
    models = []
    for extension, doc in [("gltf", gltf), ("ifcx", ifcx)]:
        path = output / ("culling." + extension)
        path.write_text(json.dumps(doc), encoding="utf-8")
        models.append(path)
    return models


HIZ_EXTENT = (1279, 719)
HIZ_CAMERA_FOV = 45
HIZ_EDGE_HALF_LENGTH = 9


def hiz_edge_fixtures(output):
    width, height = HIZ_EXTENT
    cx, cy = width // 2, height // 2
    half = HIZ_EDGE_HALF_LENGTH
    # Framebuffer-space rectangles, unprojected at fixed camera distances.
    # Only the rightmost column and bottom row reveal the rear panels.
    panels = [
        ("Occluder", (-1, -1, width - 1, height - 1), 1, [.4, .4, .4, 1]),
        ("Right edge", (width - 4, cy - half, width + 2, cy + half), 3, [1, 0, 0, 1]),
        ("Bottom edge", (cx - half, height - 4, cx + half, height + 2), 3, [0, 1, 0, 1]),
        ("Hidden", (cx - 8, cy - 8, cx + 8, cy + 8), 3, [0, 0, 1, 1]),
    ]
    data = bytearray()
    gltf = {"asset": {"version": "2.0"}, "bufferViews": [], "accessors": [],
            "materials": [], "meshes": [], "nodes": [], "scene": 0,
            "scenes": [{"nodes": list(range(len(panels)))}],
            "extensionsUsed": ["KHR_materials_unlit"]}
    indices = accessor(gltf, data, [0, 1, 2, 0, 2, 3], 5125, "SCALAR")
    normal = accessor(gltf, data, [[0, 0, 1]] * 4, 5126, "VEC3")
    for index, (name, (left, top, right, bottom), distance, color) in enumerate(panels):
        units_per_pixel = 2 * distance * math.tan(math.radians(HIZ_CAMERA_FOV / 2)) / height
        positions = [[(x - width / 2) * units_per_pixel,
                      (height / 2 - y) * units_per_pixel, 3 - distance]
                     for x, y in ((left, bottom), (right, bottom), (right, top), (left, top))]
        position = accessor(gltf, data, positions, 5126, "VEC3")
        gltf["materials"].append({"pbrMetallicRoughness": {"baseColorFactor": color},
                                  "extensions": {"KHR_materials_unlit": {}}})
        gltf["meshes"].append({"primitives": [{"attributes": {"POSITION": position, "NORMAL": normal},
                                               "indices": indices, "material": index}]})
        gltf["nodes"].append({"name": name, "mesh": index})
    embed_buffer(gltf, data)
    models = {}
    for route in ("gpu-occlusion", "explicit-draws"):
        if route == "explicit-draws":
            # Identical front-facing geometry, bypassing GPU occlusion via the
            # existing explicit double-sided opaque draw route.
            for material in gltf["materials"]:
                material["doubleSided"] = True
        path = output / f"hiz-odd-{route}.gltf"
        path.write_text(json.dumps(gltf), encoding="utf-8")
        models[route] = path
    return models


def hiz_edge_pixels(screenshot):
    width, height = HIZ_EXTENT
    cx, cy = width // 2, height // 2

    def dominant(pixel, channel):
        return pixel[channel] > 70 and all(pixel[channel] > value + 30
                                           for index, value in enumerate(pixel) if index != channel)

    with Image.open(screenshot) as source:
        assert source.size == HIZ_EXTENT, (screenshot, source.size)
        pixels = source.convert("RGB").load()
        return {
            "rightRed": sum(dominant(pixels[width - 1, y], 0) for y in range(height)),
            "bottomGreen": sum(dominant(pixels[x, height - 1], 1) for x in range(width)),
            "rightCoveredRed": sum(dominant(pixels[width - 2, y], 0) for y in range(height)),
            "bottomCoveredGreen": sum(dominant(pixels[x, height - 2], 1) for x in range(width)),
            "hiddenBlue": sum(dominant(pixels[x, y], 2) for x in range(cx - 8, cx + 8)
                              for y in range(cy - 8, cy + 8)),
            "occluderCenter": pixels[cx, cy],
            "clearCorner": pixels[width - 1, height - 1],
        }


def run_hiz_odd(exe, output, *, generate_only):
    models = hiz_edge_fixtures(output)
    width, height = HIZ_EXTENT
    # The renderer initially selects the first mesh and draws its yellow
    # outline over the two trailing edges. Click the empty corner to clear it.
    sequence = output / "hiz-odd.sequence.json"
    sequence.write_text(json.dumps({"schemaVersion": 1, "frames": 8, "sampleFrames": [8],
        "events": [{"frame": 2, "viewportPointer": {"action": "select", "pixel": [width - 1, height - 1]}}]}),
        encoding="utf-8")
    report = {"extent": HIZ_EXTENT, "fixtures": {route: str(path) for route, path in models.items()},
              "captureSequence": str(sequence), "captures": [], "passed": False}
    report_path = output / "results.json"
    if generate_only:
        report_path.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
        return 0

    expected_edge_pixels = 2 * HIZ_EDGE_HALF_LENGTH
    for technique in ("deferred-raster", "forward-raster"):
        for route, model in models.items():
            name = f"hiz-odd-{technique}-{route}"
            screenshot, logfile = output / f"{name}.png", output / f"{name}.log"
            telemetry = screenshot.with_suffix(".telemetry.json")
            screenshot.unlink(missing_ok=True)
            telemetry.unlink(missing_ok=True)
            command = [str(exe), "--model", str(model), "--import-scale", "1", "--hidden", "--no-ui",
                       "--validation", "--msaa", "1", "--width", str(width), "--height", str(height),
                       "--display-mode", "lit", "--no-bloom", "--no-taa", "--render-technique", technique,
                       "--sky", "plain", "--sky-horizon", "0", "0", "0", "--directional-intensity", "0",
                       "--environment-intensity", "0", "--bounce-intensity", "0", "--exposure", "1",
                       "--camera-position", "0", "0", "3", "--camera-target", "0", "0", "0",
                       "--camera-fov", str(HIZ_CAMERA_FOV), "--fixed-dt", ".016666667",
                       "--capture-sequence", str(sequence), "--screenshot-viewport", "--screenshot", str(screenshot)]
            with logfile.open("w", encoding="utf-8") as log:
                process = subprocess.run(command, cwd=exe.parent,
                    env=dict(os.environ, VK_LAYER_SETTINGS_PATH=str(output)),
                    stdout=log, stderr=subprocess.STDOUT, timeout=120)
            logtext = logfile.read_text(encoding="utf-8", errors="replace")
            validation_clean = not re.search(r"VUID-|SYNC-HAZARD|Validation Error", logtext, re.IGNORECASE)
            record = {"capture": name, "technique": technique, "route": route, "command": command,
                      "returnCode": process.returncode, "validationClean": validation_clean,
                      "image": str(screenshot), "telemetry": str(telemetry), "log": str(logfile), "passed": False}
            report["captures"].append(record)
            if process.returncode == 0 and screenshot.is_file() and telemetry.is_file():
                state = json.loads(telemetry.read_text(encoding="utf-8"))
                passes = {row["name"]: row for row in state["passes"]}
                culling_passes = {name: passes[name] for name in ("FrustumCull", "HiZGenerate", "OcclusionCull")}
                counts = hiz_edge_pixels(screenshot)
                record.update(passes=culling_passes, pixels=counts, revision=state["build"]["revision"],
                              selectedMeshNode=state["gui"]["selectedMeshNodeId"])
                route_ok = True
                if route == "gpu-occlusion":
                    route_ok = all(row["active"] and row["gpuTimed"] and row["gpuMs"] > 0
                                   for row in culling_passes.values())
                elif technique == "deferred-raster":
                    route_ok = not culling_passes["OcclusionCull"]["active"]
                pixels_ok = (counts["rightRed"] == counts["bottomGreen"] == expected_edge_pixels
                             and counts["rightCoveredRed"] == counts["bottomCoveredGreen"] == counts["hiddenBlue"] == 0
                             and min(counts["occluderCenter"]) > 70 and max(counts["clearCorner"]) < 10)
                record["passed"] = (validation_clean and route_ok and pixels_ok and state["validationEnabled"]
                                    and state["technique"] == technique and tuple(state["resolution"]) == HIZ_EXTENT
                                    and tuple(state["fsr"]["renderExtent"]) == HIZ_EXTENT
                                    and record["selectedMeshNode"] == 0xFFFFFFFF
                                    and state["scene"]["primaryObjectCount"] == 4)
                print(name, "PASS" if record["passed"] else "FAIL", counts, flush=True)
            else:
                print(name, "FAIL", logfile, flush=True)
            report_path.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    report["passed"] = all(record["passed"] for record in report["captures"])
    report_path.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    return 0 if report["passed"] else 1


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--exe", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--hiz-odd-only", action="store_true", help="check odd-sized Hi-Z with visible trailing-edge geometry")
    parser.add_argument("--generate-only", action="store_true", help="write the odd-sized Hi-Z glTF fixtures only")
    args = parser.parse_args()
    if args.generate_only and not args.hiz_odd_only:
        parser.error("--generate-only requires --hiz-odd-only")
    if not args.generate_only and os.environ.get("CONTAINER_RUN_GPU_FORWARD_CULLING") != "1":
        print("SKIP: set CONTAINER_RUN_GPU_FORWARD_CULLING=1 to run GPU captures")
        return 77
    exe, output = args.exe.resolve(), args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    (output / "vk_layer_settings.txt").write_text(
        "khronos_validation.validate_sync = true\n", encoding="utf-8")
    if args.hiz_odd_only:
        return run_hiz_odd(exe, output, generate_only=args.generate_only)
    results = []
    for model in fixtures(output):
        for technique in ["deferred-raster", "forward-raster"]:
            for taa in [False, True]:
                name = f"{model.suffix[1:]}-{technique}-taa{int(taa)}"
                screenshot, logfile = output / (name + ".png"), output / (name + ".log")
                command = [str(exe), "--model", str(model), "--hidden", "--no-ui",
                           "--validation", "--msaa", "1", "--width", "640", "--height", "360",
                           "--display-mode", "lit", "--no-bloom", "--render-technique", technique,
                           "--taa" if taa else "--no-taa", "--fixed-dt", "0.016666667",
                           "--directional-intensity", "2", "--environment-intensity", "0",
                           "--directional-direction", "0", "0", "-1",
                           "--exposure", ".3", "--camera-position", "0", "0", "9",
                           "--camera-target", "0", "0", "0", "--screenshot", str(screenshot)]
                with logfile.open("w", encoding="utf-8") as log:
                    process = subprocess.run(command, cwd=output,
                        env=dict(os.environ, VK_LAYER_SETTINGS_PATH=str(output)),
                        stdout=log, stderr=subprocess.STDOUT, timeout=120)
                logtext = logfile.read_text(encoding="utf-8", errors="replace")
                assert process.returncode == 0 and "VUID-" not in logtext and "SYNC-HAZARD" not in logtext, logfile
                with Image.open(screenshot) as image:
                    pixels = list(image.convert("RGB").getdata())
                counts = {
                    "front": sum(g > 70 and g > r + 30 and g > b + 30 for r, g, b in pixels),
                    "reflected": sum(r > 70 and r > g + 30 and r > b + 30 for r, g, b in pixels),
                    "backDoubleSided": sum(min(r, b) > 70 and min(r, b) > g + 30 for r, g, b in pixels),
                    "backSingleSided": sum(b > 70 and b > r + 30 and b > g + 30 for r, g, b in pixels)}
                assert all(counts[key] > 1000 for key in ["front", "reflected", "backDoubleSided"]), (name, counts)
                assert counts["backSingleSided"] < 10, (name, counts)
                if model.suffix == ".ifcx":
                    telemetry = json.loads(screenshot.with_suffix(".telemetry.json").read_text())
                    assert Path(telemetry["scene"]["auxiliary"]).resolve() == model
                    assert telemetry["scene"]["primary"] == ""
                results.append({"capture": name, "pixels": counts, "validationClean": True})
                print(name, counts, flush=True)
    (output / "results.json").write_text(json.dumps(results, indent=2) + "\n", encoding="utf-8")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
