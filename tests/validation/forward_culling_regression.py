"""Opt-in pixel regression for opaque scene and BIM culling routes."""

import argparse
import base64
import json
import os
from pathlib import Path
import struct
import subprocess

try:
    from PIL import Image
except ImportError:
    print("SKIP: forward culling regression requires Pillow")
    raise SystemExit(77)


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

    def accessor(values, component, kind):
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

    position = accessor(positions, 5126, "VEC3")
    ifcx = {"data": []}
    for index, color in enumerate(colors):
        reversed_face = index >= 2
        sx, x = (-1 if index == 1 else 1), -3 + 2 * index
        indices = back if reversed_face else front
        material = {"doubleSided": index == 2,
                    "pbrMetallicRoughness": {"baseColorFactor": color,
                                            "metallicFactor": 0, "roughnessFactor": .8}}
        gltf["materials"].append(material)
        normal = accessor([[0, 0, -1 if reversed_face else 1]] * 4, 5126, "VEC3")
        gltf["meshes"].append({"primitives": [{"attributes": {"POSITION": position,
                                                               "NORMAL": normal},
                                               "indices": accessor(indices, 5125, "SCALAR"),
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
    gltf["buffers"] = [{"byteLength": len(data),
                        "uri": "data:application/octet-stream;base64," +
                               base64.b64encode(data).decode()}]
    models = []
    for extension, doc in [("gltf", gltf), ("ifcx", ifcx)]:
        path = output / ("culling." + extension)
        path.write_text(json.dumps(doc), encoding="utf-8")
        models.append(path)
    return models


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--exe", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    if os.environ.get("CONTAINER_RUN_GPU_FORWARD_CULLING") != "1":
        print("SKIP: set CONTAINER_RUN_GPU_FORWARD_CULLING=1 to run GPU captures")
        return 77
    exe, output = args.exe.resolve(), args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    (output / "vk_layer_settings.txt").write_text(
        "khronos_validation.validate_sync = true\n", encoding="utf-8")
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
