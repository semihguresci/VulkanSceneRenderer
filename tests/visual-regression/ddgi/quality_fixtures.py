#!/usr/bin/env python3
"""Generate deterministic architectural scenes and an independent transport description.

No renderer or probe implementation is imported. The JSON sidecar records the
same opaque axis-aligned rectangles used to construct glTF triangles, so a CPU
ray reference can intersect exact planes rather than reverse engineer captures.
"""

from __future__ import annotations

import argparse
import base64
import hashlib
import json
from pathlib import Path
import struct

import numpy as np


VARIANTS = ("doorway", "sealed-thin", "sealed-thick", "open", "doorway-neutral")
CAMERA = {"position": [0.0, 1.65, 7.0], "target": [0.0, 1.15, -0.5], "fov": 50.0}
VOLUME = {"origin": [-3.0, -0.05, -2.0], "spacing": [0.5, 0.5, 0.5],
          "counts": [13, 7, 7], "maxDistance": 12.0}
MATERIALS = {
    "white": [0.7, 0.7, 0.7],
    "partition": [0.65, 0.65, 0.65],
    "red": [0.65, 0.075, 0.04],
}


def rectangle(name, center, u, v, material="white"):
    """u and v are half-extent vectors; cross(u,v) defines the front normal."""
    center, u, v = (np.asarray(value, dtype=np.float64) for value in (center, u, v))
    normal = np.cross(u, v)
    normal /= np.linalg.norm(normal)
    corners = [center - u - v, center + u - v,
               center + u + v, center - u + v]
    return {"name": name, "center": center.tolist(), "u": u.tolist(),
            "v": v.tolist(), "normal": normal.tolist(),
            "corners": [point.tolist() for point in corners], "material": material,
            "albedo": MATERIALS[material]}


def box(name, low, high, material="partition"):
    low, high = np.asarray(low), np.asarray(high)
    center, half = (low + high) * 0.5, (high - low) * 0.5
    faces = []
    for axis in range(3):
        # Cyclic axes make cross(u,v) point along +axis.
        a, b = (axis + 1) % 3, (axis + 2) % 3
        for sign in (-1, 1):
            position = center.copy()
            position[axis] += sign * half[axis]
            u, v = np.zeros(3), np.zeros(3)
            u[a], v[b] = half[a], half[b] * sign
            faces.append(rectangle(f"{name}-{axis}-{sign:+d}", position, u, v, material))
    return faces


def project(point, camera=CAMERA, width=960, height=640):
    """World to upper-left pixels, using RH view and the negative scene viewport."""
    eye, target = np.asarray(camera["position"]), np.asarray(camera["target"])
    forward = target - eye
    forward /= np.linalg.norm(forward)
    right = np.cross(forward, [0.0, 1.0, 0.0])
    right /= np.linalg.norm(right)
    up = np.cross(right, forward)
    delta = np.asarray(point) - eye
    depth = np.dot(delta, forward)
    if depth <= 0:
        raise ValueError("Receiver must be in front of the camera")
    focal = 1.0 / np.tan(np.deg2rad(camera["fov"]) * 0.5)
    ndc_x = np.dot(delta, right) * focal / (depth * width / height)
    ndc_y = np.dot(delta, up) * focal / depth
    return [float((ndc_x * 0.5 + 0.5) * width),
            float((0.5 - ndc_y * 0.5) * height)]


def receiver(name, position, normal, width, height):
    pixel = project(position, width=width, height=height)
    x, y = (int(round(value)) for value in pixel)
    return {"name": name, "position": position, "normal": normal,
            "albedo": MATERIALS["white"], "centerPixel": pixel,
            "pixelRoi": [x - 6, y - 6, x + 6, y + 6],
            "referenceRegion": "center point; screen ROI is a separate spatial average"}


def build_scene(variant="doorway", width=960, height=640):
    if variant not in VARIANTS:
        raise ValueError(f"Unknown architectural variant: {variant}")
    quads = [
        rectangle("floor", [0, 0, -0.5], [3, 0, 0], [0, 0, -1.5]),
        rectangle("ceiling", [0, 2.8, -0.5], [3, 0, 0], [0, 0, 1.5]),
        rectangle("back", [0, 1.4, -2], [3, 0, 0], [0, 1.4, 0]),
        rectangle("left-bounce-wall", [-3, 1.4, -0.5], [0, 1.4, 0], [0, 0, 1.5],
                  "white" if variant == "doorway-neutral" else "red"),
        rectangle("right-wall", [3, 1.4, -0.5], [0, 1.4, 0], [0, 0, -1.5]),
    ]
    if variant.startswith("sealed"):
        thickness = 0.03 if variant == "sealed-thin" else 0.24
        quads += box("sealed-partition", [-thickness * 0.5, 0, -2],
                     [thickness * 0.5, 2.8, 1])
    elif variant.startswith("doorway"):
        thickness = 0.06
        # All three solids meet at edges; the aperture is z[-.55,.25], y[0,2.1].
        quads += box("door-left", [-thickness * 0.5, 0, -2],
                     [thickness * 0.5, 2.8, -0.55])
        quads += box("door-right", [-thickness * 0.5, 0, 0.25],
                     [thickness * 0.5, 2.8, 1])
        quads += box("door-header", [-thickness * 0.5, 2.1, -0.55],
                     [thickness * 0.5, 2.8, 0.25])
    else:
        thickness = 0.0
    return {"schemaVersion": 1, "name": variant, "units": "metres",
            "description": "Two cutaway chambers with one ceiling rectangle; black environment",
            "camera": CAMERA, "captureSize": [width, height], "volume": VOLUME,
            "partitionThickness": thickness, "partitionNode": 2 if thickness > 0 else None,
            "light": {"position": [-1.5, 2.6, -0.6], "normal": [0, -1, 0],
                      "u": [0.4, 0, 0], "v": [0, 0, -0.4],
                      "radiance": [100.0, 100.0, 100.0], "range": 0.0},
            "quads": quads,
            "receivers": [receiver("lit-back", [-1.6, 0.8, -2], [0, 0, 1], width, height),
                          receiver("shaded-back", [1.6, 0.8, -2], [0, 0, 1], width, height),
                          receiver("lit-floor", [-1.6, 0, -0.8], [0, 1, 0], width, height),
                          receiver("shaded-floor", [1.6, 0, -0.8], [0, 1, 0], width, height)],
            "referencePolicy": {"model": "opaque single-sided Lambertian, exact quad geometry",
                                "environmentRadiance": [0, 0, 0],
                                "rendererDifference": "GPU PBR includes Fresnel energy attenuation; reference is diffuse only",
                                "sealedInvariant": "shaded room has exactly zero source transport at every bounce"}}


def gltf_scene(scene):
    payload = bytearray()
    views, accessors, meshes = [], [], []

    def append_view(data, target):
        while len(payload) % 4:
            payload.append(0)
        index = len(views)
        views.append({"buffer": 0, "byteOffset": len(payload), "byteLength": len(data),
                      "target": target})
        payload.extend(data)
        return index

    groups = [("Architectural shell", [quad for quad in scene["quads"]
                                        if quad["material"] != "partition"])]
    partition = [quad for quad in scene["quads"] if quad["material"] == "partition"]
    if partition:
        groups.append(("Partition", partition))
    for name, group in groups:
        primitives = []
        for material_index, material in enumerate(MATERIALS):
            quads = [quad for quad in group if quad["material"] == material]
            if not quads:
                continue
            positions = np.asarray([point for quad in quads for point in quad["corners"]])
            normals = [quad["normal"] for quad in quads for _ in range(4)]
            indices = [base + index for base in range(0, len(positions), 4)
                       for index in (0, 1, 2, 0, 2, 3)]
            pos_view = append_view(struct.pack("<" + "f" * positions.size, *positions.ravel()), 34962)
            normal_view = append_view(struct.pack("<" + "f" * (len(normals) * 3),
                                                 *(value for normal in normals for value in normal)), 34962)
            index_view = append_view(struct.pack("<" + "H" * len(indices), *indices), 34963)
            first = len(accessors)
            accessors += [{"bufferView": pos_view, "componentType": 5126, "count": len(positions),
                           "type": "VEC3", "min": positions.min(axis=0).tolist(),
                           "max": positions.max(axis=0).tolist()},
                          {"bufferView": normal_view, "componentType": 5126, "count": len(normals),
                           "type": "VEC3"},
                          {"bufferView": index_view, "componentType": 5123, "count": len(indices),
                           "type": "SCALAR"}]
            primitives.append({"attributes": {"POSITION": first, "NORMAL": first + 1},
                               "indices": first + 2, "mode": 4, "material": material_index})
        meshes.append({"name": name, "primitives": primitives})
    materials = [{"name": name, "doubleSided": False,
                  "pbrMetallicRoughness": {"baseColorFactor": color + [1],
                                           "metallicFactor": 0, "roughnessFactor": 1}}
                 for name, color in MATERIALS.items()]
    nodes = [{"name": "Architectural shell", "mesh": 0},
             {"name": "Ceiling rectangle", "translation": scene["light"]["position"],
              "rotation": [-0.7071067811865476, 0, 0, 0.7071067811865476],
              "extensions": {"KHR_lights_punctual": {"light": 0}}}]
    if partition:
        nodes.append({"name": "Partition", "mesh": 1})
    return {"asset": {"version": "2.0", "generator": "Container DDGI architecture quality fixture"},
            "extensionsUsed": ["KHR_lights_punctual"],
            "extensions": {"KHR_lights_punctual": {"lights": [
                {"name": "Lit chamber ceiling rectangle", "type": "point", "color": [1, 1, 1],
                 "intensity": 100.0,
                 "extras": {"areaLight": {"shape": "rect", "width": 0.8, "height": 0.8}}}]}},
            "buffers": [{"byteLength": len(payload), "uri": "data:application/octet-stream;base64," +
                         base64.b64encode(payload).decode("ascii")}],
            "bufferViews": views, "accessors": accessors, "materials": materials,
            "meshes": meshes, "nodes": nodes,
            "scenes": [{"name": scene["name"], "nodes": list(range(len(nodes)))}], "scene": 0}


def write_fixtures(output, width=960, height=640):
    output = Path(output).resolve()
    output.mkdir(parents=True, exist_ok=True)
    records = []
    for variant in VARIANTS:
        scene = build_scene(variant, width, height)
        path = output / f"architecture-{variant}.gltf"
        path.write_text(json.dumps(gltf_scene(scene), indent=2) + "\n", encoding="utf-8")
        scene["gltf"] = path.name
        scene["gltfSha256"] = hashlib.sha256(path.read_bytes()).hexdigest()
        records.append(scene)
    manifest = {"schemaVersion": 1, "fixtures": records,
                "suggestedCli": ["--camera-position", *map(str, CAMERA["position"]),
                                 "--camera-target", *map(str, CAMERA["target"]),
                                 "--camera-fov", str(CAMERA["fov"]),
                                 "--environment-intensity", "0", "--directional-intensity", "0",
                                 "--exposure", ".04", "--ddgi", "--ddgi-origin",
                                 *map(str, VOLUME["origin"]), "--ddgi-spacing",
                                 *map(str, VOLUME["spacing"]), "--ddgi-counts",
                                 *map(str, VOLUME["counts"]), "--ddgi-max-distance", "12"]}
    (output / "fixture-manifest.json").write_text(json.dumps(manifest, indent=2) + "\n",
                                                 encoding="utf-8")
    return manifest


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--width", type=int, default=960)
    parser.add_argument("--height", type=int, default=640)
    args = parser.parse_args()
    write_fixtures(args.output, args.width, args.height)
    print(f"Generated {len(VARIANTS)} architectural fixtures in {args.output.resolve()}")


if __name__ == "__main__":
    main()
