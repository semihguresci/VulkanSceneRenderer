"""Deterministic physics integration in varied glTF and BIM scenes.

Analytic checks use authored mesh vertices, parent transforms and mechanism
anchors independently of captured bounds. No downloaded assets are required.
"""
import argparse
import base64
import hashlib
import json
import math
import os
from pathlib import Path
import struct

from physics_auxiliary_regression import cube, matrix

CASES = ("nested-offcenter", "mirrored-parent", "ramp-contact", "bim-global-translation",
         "mixed-provider-slider", "repository-model-replacement", "ccd-thin-wall",
         "joint-during-kinematic-motion", "kinematic-resize-contact",
         "kinematic-replacement-contact", "dynamic-support-teleport", "contact-wake-chain")
KINEMATIC_CONTACT_CASES = ("kinematic-resize-contact", "kinematic-replacement-contact")
CONTACT_SAMPLE_FRAMES = [1, 2, 3, 4, 6, 8, 10, 12, 16]
SUPPORT_TELEPORT_SAMPLE_FRAMES = [3, 180, 182, 184, 210, 214, 218, 222, 226]
CONTACT_WAKE_CHAIN_SAMPLE_FRAMES = [7, 180, 182, 184, 210, 214, 218, 222, 226]
BIM_ROTATION = [1, 0, 0, 0, 0, 0, -1, 0, 0, 1, 0, 0, 0, 0, 0, 1]
# Jolt 5.6 PhysicsSettings/PhysicsSystem bound a linear cast's permitted
# penetration by min(mPenetrationSlop, mLinearCastMaxPenetration * innerRadius).
# This fixture's 10cm box inner radius makes the default allowance 2cm.
CCD_ALLOWED_PENETRATION = min(.02, .25 * .1)
CCD_STOP_MAXIMUM_X = -.025 - .1 + CCD_ALLOWED_PENETRATION + 2e-5


def parse_ccd_speed(value):
    """Accept a finite native float velocity while keeping one-step crossing."""
    try:
        speed = float(value)
    except (TypeError, ValueError):
        raise argparse.ArgumentTypeError("CCD speed must be a finite float32 value >=500") from None
    if not math.isfinite(speed) or not 500 <= speed <= float.fromhex("0x1.fffffep+127"):
        raise argparse.ArgumentTypeError("CCD speed must be a finite float32 value >=500")
    return struct.unpack("<f", struct.pack("<f", speed))[0]


def encode(value):
    return value.T.reshape(-1).tolist()


def translation(value):
    import numpy as np
    result = np.eye(4)
    result[:3, 3] = value
    return result


def node_matrix(node):
    import numpy as np
    if "matrix" in node:
        return matrix(node["matrix"])
    result = translation(node.get("translation", [0, 0, 0]))
    x, y, z, w = node.get("rotation", [0, 0, 0, 1])
    rotation = np.array([[1-2*y*y-2*z*z, 2*x*y-2*z*w, 2*x*z+2*y*w],
                         [2*x*y+2*z*w, 1-2*x*x-2*z*z, 2*y*z-2*x*w],
                         [2*x*z-2*y*w, 2*y*z+2*x*w, 1-2*x*x-2*y*y]])
    result[:3, :3] = rotation @ np.diag(node.get("scale", [1, 1, 1]))
    return result


def graph_leaves(document, meshes):
    """Derive SceneManager's source-node/primitive-node order from the glTF."""
    import numpy as np
    result, next_node = {}, 0

    def visit(index, parent):
        nonlocal next_node
        node = document["nodes"][index]
        next_node += 1  # Source nodes are retained as nonrenderable parents.
        world = parent @ node_matrix(node)
        if "mesh" in node:
            for primitive in document["meshes"][node["mesh"]]["primitives"]:
                result[node["name"]] = {"node": next_node, "vertices": meshes[primitive["attributes"]["POSITION"]],
                                         "authoredModel": encode(world)}
                next_node += 1
        for child in node.get("children", []):
            visit(child, world)

    for index in document["scenes"][document.get("scene", 0)]["nodes"]:
        visit(index, np.eye(4))
    return result


def write_gltf(path, nodes, roots, mesh_vertices=None):
    points, normals, indices = cube()
    mesh_vertices = mesh_vertices or [points]
    data = bytearray()
    views, accessors, meshes, vertex_accessors = [], [], [], {}
    for vertices in mesh_vertices:
        attributes = {}
        for name, values in (("POSITION", vertices), ("NORMAL", normals)):
            packed = struct.pack("<72f", *(component for point in values for component in point))
            views.append({"buffer": 0, "byteOffset": len(data), "byteLength": len(packed)})
            accessor = {"bufferView": len(views)-1, "componentType": 5126, "count": 24, "type": "VEC3"}
            if name == "POSITION":
                accessor.update({"min": [min(point[i] for point in values) for i in range(3)],
                                 "max": [max(point[i] for point in values) for i in range(3)]})
                vertex_accessors[len(accessors)] = vertices
            attributes[name] = len(accessors)
            accessors.append(accessor)
            data.extend(packed)
        packed = struct.pack("<36H", *indices)
        views.append({"buffer": 0, "byteOffset": len(data), "byteLength": len(packed)})
        accessors.append({"bufferView": len(views)-1, "componentType": 5123, "count": 36, "type": "SCALAR"})
        data.extend(packed)
        meshes.append({"primitives": [{"attributes": attributes, "indices": len(accessors)-1, "material": 0}]})
    document = {"asset": {"version": "2.0"}, "scene": 0, "scenes": [{"nodes": roots}],
                "nodes": nodes, "meshes": meshes, "materials": [{"pbrMetallicRoughness": {
                    "baseColorFactor": [.65, .4, .2, 1], "roughnessFactor": .8, "metallicFactor": 0}}],
                "buffers": [{"byteLength": len(data), "uri": "data:application/octet-stream;base64," + base64.b64encode(data).decode()}],
                "bufferViews": views, "accessors": accessors}
    path.write_text(json.dumps(document), encoding="utf-8")
    return graph_leaves(document, vertex_accessors)


def gltf_source_geometry(path):
    document = json.loads(path.read_text(encoding="utf-8"))
    buffers = [base64.b64decode(buffer["uri"].split(",", 1)[1]) for buffer in document["buffers"]]
    meshes = {}
    for mesh in document["meshes"]:
        for primitive in mesh["primitives"]:
            index = primitive["attributes"]["POSITION"]
            accessor = document["accessors"][index]
            assert accessor["componentType"] == 5126 and accessor["type"] == "VEC3"
            view = document["bufferViews"][accessor["bufferView"]]
            offset = view.get("byteOffset", 0) + accessor.get("byteOffset", 0)
            stride = view.get("byteStride", 12)
            meshes[index] = [struct.unpack_from("<3f", buffers[view["buffer"]], offset + vertex * stride)
                             for vertex in range(accessor["count"])]
    return graph_leaves(document, meshes)


def bim_source_geometry(path):
    document = json.loads(path.read_text(encoding="utf-8"))
    meshes = {mesh["mesh_id"]: mesh["coordinates"] for mesh in document["meshes"]}
    return {str(index): {"vertices": [coordinates[i:i+3] for i in range(0, len(coordinates), 3)],
                         "guid": element["guid"]}
            for index, element in enumerate(document["elements"])
            for coordinates in [meshes[element["mesh_id"]]]}


def write_bim(path):
    points, _, indices = cube()
    # An asymmetric L-shaped product and an unrelated tetrahedral product.
    pieces = [([-.75, 3, 0], [2.5, .5, .6]), ([-1.75, 3.75, 0], [.5, 1.5, .6])]
    meshes, elements = [], []
    for index in range(3):
        if index < 2:
            center, scale = pieces[index]
            world = [[point[axis]*scale[axis] + center[axis] for axis in range(3)] for point in points]
            faces = indices
        else:
            world = [[3.5, .5, -.5], [4.5, .5, -.5], [4, .5, .5], [4, 1.5, 0]]
            faces = [0, 1, 2, 0, 3, 1, 1, 3, 2, 2, 3, 0]
        source = [[x, -z, y] for x, y, z in world]
        meshes.append({"mesh_id": index, "coordinates": [component for point in source for component in point], "indices": faces})
        elements.append({"mesh_id": index, "guid": "diversity-L" if index < 2 else "diversity-tetrahedron",
            "type": "IfcBuildingElementProxy", "vector": {"x": 0, "y": 0, "z": 0},
            "rotation": {"qx": 0, "qy": 0, "qz": 0, "qw": 1},
            "color": {"r": 190, "g": 120, "b": 60 if index < 2 else 220, "a": 255},
            "info": {"Name": "L assembly" if index < 2 else "Tetrahedron"}})
    path.write_text(json.dumps({"schema_version": "1.1.0", "meshes": meshes, "elements": elements, "info": {}}), encoding="utf-8")


def kinematic_contact_fixture(case, directory):
    """A pending support target must survive its collider rebuild."""
    resize = case == "kinematic-resize-contact"
    source = directory / (case + ".gltf")
    name = "Resized kinematic source" if resize else "Pushed primary cube"
    specs = write_gltf(source, [{"name": name, "mesh": 0,
                                "translation": [-2, 0, 0] if resize else [0, 0, 0]}], [0])
    primary = specs[name]["node"]
    auxiliary = directory / (case + ".bim")
    points, _, indices = cube()
    center = 0 if resize else -2
    guid = "resize-pushed-cube" if resize else "replacement-kinematic-support"
    auxiliary.write_text(json.dumps({"schema_version": "1.1.0", "info": {},
        "meshes": [{"mesh_id": 0, "coordinates": [component for x, y, z in points
                    for component in (x + center, -z, y)], "indices": indices}],
        "elements": [{"mesh_id": 0, "guid": guid, "type": "IfcBuildingElementProxy",
            "vector": {"x": 0, "y": 0, "z": 0},
            "rotation": {"qx": 0, "qy": 0, "qz": 0, "qw": 1},
            "color": {"r": 60, "g": 170, "b": 220, "a": 255},
            "info": {"Name": "Pushed BIM cube" if resize else "Replaced BIM support"}}]}), encoding="utf-8")
    options = {"case": case, "primaryNode": primary, "bimGuid": guid,
        "primaryGeometry": {str(spec["node"]): spec for spec in specs.values()},
        "bimGeometry": bim_source_geometry(auxiliary), "fixedRenderDeltaSeconds": 4 / 60,
        "contactCenter": 3.1 if resize else 3.0}
    if resize:
        events = [{"frame": 1, "physicsNode": primary, "physicsAttach": "kinematic",
                   "physicsSettings": {"friction": 0, "gravityFactor": 0}},
                  {"frame": 2, "bimSelectedObject": 0, "physicsAttach": "dynamic", "physicsAction": "play",
                   "physicsSettings": {"friction": 0, "gravityFactor": 0}}]
    else:
        events = [{"frame": 1, "bimSelectedObject": 0, "physicsAttach": "kinematic",
                   "physicsSettings": {"friction": .25, "gravityFactor": 0}},
                  {"frame": 2, "physicsNode": primary, "physicsAttach": "dynamic", "physicsAction": "play",
                   "physicsSettings": {"friction": 0, "gravityFactor": 0}},
                  {"frame": 4, "bimSelectedObject": 0, "physicsAttach": "kinematic",
                   "physicsSettings": {"friction": 0, "gravityFactor": 0}}]
    events += [{"frame": 7, "physicsAction": "reset"},
               {"frame": 9, "bimSelectedObject": 0, "physicsAction": "detach"},
               {"frame": 11, "physicsNode": primary, "physicsAction": "detach"},
               {"frame": 15, "guiUnload": True}]
    sequence = {"schemaVersion": 1, "frames": 16, "sampleFrames": CONTACT_SAMPLE_FRAMES.copy(),
        "camera": [{"frame": frame, "position": [6, 4, 10], "target": [1, 0, 0]} for frame in (1, 16)],
        "events": events}
    if resize:
        sequence.update({"objectNode": 0, "objectName": name,
            "objectTranslation": [{"frame": frame, "translation": [0, 0, 0] if frame <= 3 else [4, 0, 0]}
                                  for frame in (1, 3, 4, 16)],
            "objectScale": [{"frame": frame, "scale": [1, 1, 1] if frame <= 3 else [1.2, 1.2, 1.2]}
                            for frame in (1, 3, 4, 16)]})
    else:
        sequence["bimTranslation"] = [{"frame": frame, "translation": [0, 0, 0] if frame <= 3 else [4, 0, 0]}
                                      for frame in (1, 3, 4, 16)]
    return source, auxiliary, sequence, options


def dynamic_support_teleport_fixture(directory):
    source = directory / "dynamic-support-teleport.gltf"
    name = "Teleported dynamic glTF support"
    specs = write_gltf(source, [
        {"name": "Floor", "mesh": 0, "translation": [0, -.25, 0], "scale": [32, .5, 10]},
        {"name": name, "mesh": 0, "translation": [0, .5, 0]}], [0, 1])
    auxiliary = directory / "dynamic-support-rider.bim"
    points, _, indices = cube()
    guid = "dynamic-support-rider"
    auxiliary.write_text(json.dumps({"schema_version": "1.1.0", "info": {},
        "meshes": [{"mesh_id": 0, "coordinates": [component for x, y, z in points
                    for component in (x, -z, y + 1.5)], "indices": indices}],
        "elements": [{"mesh_id": 0, "guid": guid, "type": "IfcBuildingElementProxy",
            "vector": {"x": 0, "y": 0, "z": 0},
            "rotation": {"qx": 0, "qy": 0, "qz": 0, "qw": 1},
            "color": {"r": 60, "g": 170, "b": 220, "a": 255},
            "info": {"Name": "Dynamic support BIM rider"}}]}), encoding="utf-8")
    options = {"case": "dynamic-support-teleport", "floorNode": specs["Floor"]["node"],
        "supportNode": specs[name]["node"], "bimGuid": guid,
        "primaryGeometry": {str(spec["node"]): spec for spec in specs.values()},
        "bimGeometry": bim_source_geometry(auxiliary)}
    sequence = {"schemaVersion": 1, "frames": 226, "sampleFrames": SUPPORT_TELEPORT_SAMPLE_FRAMES.copy(),
        "camera": [{"frame": frame, "position": [12, 7, 14], "target": [4, 0, 0]} for frame in (1, 226)],
        "objectNode": 0, "objectName": name,
        "objectTranslation": [{"frame": frame, "translation": [0, 0, 0] if frame <= 181 else [10, 0, 0]}
                              for frame in (1, 181, 182, 226)],
        "events": [
            {"frame": 1, "physicsNode": options["floorNode"], "physicsAttach": "static"},
            {"frame": 2, "physicsNode": options["supportNode"], "physicsAttach": "dynamic",
             "physicsSettings": {"gravityFactor": 1, "friction": .5}},
            {"frame": 3, "bimSelectedObject": 0, "physicsAttach": "dynamic", "physicsAction": "play",
             "physicsSettings": {"gravityFactor": 1, "friction": .5}},
            {"frame": 181, "physicsAction": "pause"}, {"frame": 183, "physicsAction": "play"},
            {"frame": 211, "physicsAction": "reset"},
            {"frame": 215, "bimSelectedObject": 0, "physicsAction": "detach"},
            {"frame": 219, "physicsNode": options["supportNode"], "physicsAction": "detach"},
            {"frame": 225, "guiUnload": True}]}
    return source, auxiliary, sequence, options


def contact_wake_chain_fixture(directory):
    source = directory / "contact-wake-chain.gltf"
    heights = [.5, 1.7, 2.9]
    specs = write_gltf(source, [
        {"name": f"Contact chain link {index}", "mesh": 0, "translation": [0, height, 0]}
        for index, height in enumerate(heights)], [0, 1, 2])
    nodes = [specs[f"Contact chain link {index}"]["node"] for index in range(3)]
    auxiliary = directory / "contact-chain-platform.bim"
    points, _, indices = cube()
    guid = "contact-chain-platform"
    auxiliary.write_text(json.dumps({"schema_version": "1.1.0", "info": {},
        "meshes": [{"mesh_id": 0, "coordinates": [component for x, y, z in points
                    for component in (4 * x, -4 * z, y - .5)], "indices": indices}],
        "elements": [{"mesh_id": 0, "guid": guid, "type": "IfcBuildingElementProxy",
            "vector": {"x": 0, "y": 0, "z": 0},
            "rotation": {"qx": 0, "qy": 0, "qz": 0, "qw": 1},
            "color": {"r": 60, "g": 170, "b": 220, "a": 255},
            "info": {"Name": "Moved contact chain platform"}}]}), encoding="utf-8")
    events = [{"frame": 1, "bimSelectedObject": 0, "physicsAttach": "static"}]
    events += [{"frame": index + 2, "physicsNode": node, "physicsAttach": "dynamic",
                "physicsSettings": {"gravityFactor": 1}} for index, node in enumerate(nodes)]
    events += [{"frame": index + 5, "physicsJointCreate": {"type": "fixed",
                "bodyA": {"node": nodes[index]}, "bodyB": {"node": nodes[index + 1]},
                "anchor": [0, (heights[index] + heights[index + 1]) / 2, 0], "axis": [0, 1, 0]}}
               for index in range(2)]
    events += [{"frame": 7, "physicsAction": "play"}, {"frame": 181, "physicsAction": "pause"},
               {"frame": 183, "physicsAction": "play"}, {"frame": 211, "physicsAction": "reset"},
               {"frame": 215, "physicsNode": nodes[1], "physicsAction": "detach"},
               {"frame": 219, "bimSelectedObject": 0, "physicsAction": "detach"},
               {"frame": 225, "guiUnload": True}]
    sequence = {"schemaVersion": 1, "frames": 226, "sampleFrames": CONTACT_WAKE_CHAIN_SAMPLE_FRAMES.copy(),
        "camera": [{"frame": frame, "position": [11, 7, 15], "target": [3, 1, 0]} for frame in (1, 226)],
        "bimTranslation": [{"frame": frame, "translation": [0, 0, 0] if frame <= 181 else [10, 0, 0]}
                           for frame in (1, 181, 182, 226)], "events": events}
    options = {"case": "contact-wake-chain", "chainNodes": nodes, "chainHeights": heights,
        "bimGuid": guid, "primaryGeometry": {str(spec["node"]): spec for spec in specs.values()},
        "bimGeometry": bim_source_geometry(auxiliary)}
    return source, auxiliary, sequence, options


def fixture(case, directory, ccd_speed=500):
    speed = parse_ccd_speed(ccd_speed)
    directory = Path(directory)
    directory.mkdir(parents=True, exist_ok=True)
    if case in KINEMATIC_CONTACT_CASES:
        return kinematic_contact_fixture(case, directory)
    if case == "dynamic-support-teleport":
        return dynamic_support_teleport_fixture(directory)
    if case == "contact-wake-chain":
        return contact_wake_chain_fixture(directory)
    floor = {"name": "Floor", "mesh": 0, "translation": [0, -.25, 0], "scale": [16, .5, 10]}
    source = directory / (case + ".gltf")
    auxiliary = None
    options = {"case": case, "primaryGeometry": {}, "bimGeometry": {}}
    if case in ("nested-offcenter", "mirrored-parent"):
        points, _, _ = cube()
        offcenter = [[x+2, y, z] for x, y, z in points]
        mirrored = case == "mirrored-parent"
        parent = {"name": "Rotated parent", "translation": [0, 4, 0], "rotation": [0, math.sin(.35/2), 0, math.cos(.35/2)],
                  "scale": [-1.5 if mirrored else 1.5, 1.5, 1.5], "children": [2]}
        child = {"name": "Offcenter body", "mesh": 1, "rotation": [0, math.sin(-.15/2), 0, math.cos(-.15/2)], "scale": [.8, 1.2, .6]}
        specs = write_gltf(source, [floor, parent, child], [0, 1], [points, offcenter])
        options["dynamic"] = specs["Offcenter body"]["node"]
        options["settledHeight"] = .9
        samples = [1, 3, 30, 180, 184, 188, 200]
        events = [{"frame": 1, "physicsNode": specs["Floor"]["node"], "physicsAttach": "static"},
                  {"frame": 2, "physicsNode": options["dynamic"], "physicsAttach": "dynamic", "physicsAction": "play"},
                  {"frame": 181, "physicsAction": "reset"}, {"frame": 185, "physicsNode": options["dynamic"], "physicsAction": "detach"},
                  {"frame": 197, "guiUnload": True}]
    elif case == "ramp-contact":
        ramp = {"name": "Ramp", "mesh": 0, "translation": [0, 2, 0], "rotation": [0, 0, math.sin(-.65/2), math.cos(-.65/2)], "scale": [8, .4, 6]}
        body = {"name": "Ramp body", "mesh": 0, "translation": [-1, 5, 0]}
        specs = write_gltf(source, [floor, ramp, body], [0, 1, 2])
        options["dynamic"] = specs["Ramp body"]["node"]
        samples = [1, 4, 60, 180, 200]
        events = [{"frame": 1, "physicsNode": specs["Floor"]["node"], "physicsAttach": "static"},
                  {"frame": 2, "physicsNode": specs["Ramp"]["node"], "physicsAttach": "static"},
                  {"frame": 3, "physicsNode": options["dynamic"], "physicsAttach": "dynamic", "physicsAction": "play"},
                  {"frame": 197, "guiUnload": True}]
    elif case in ("bim-global-translation", "mixed-provider-slider"):
        auxiliary = directory / "asymmetric-L-and-tetrahedron.bim"
        write_bim(auxiliary)
        options["bimGeometry"] = bim_source_geometry(auxiliary)
        if case == "bim-global-translation":
            specs = write_gltf(source, [floor], [0])
            samples = [1, 4, 30, 90, 92, 180, 184, 194, 200]
            events = [{"frame": 1, "physicsNode": specs["Floor"]["node"], "physicsAttach": "static"},
                      {"frame": 2, "bimSelectedObject": 2, "physicsAttach": "static"},
                      {"frame": 3, "bimSelectedObject": 0, "physicsAttach": "dynamic", "physicsAction": "play"},
                      {"frame": 181, "physicsAction": "reset"}, {"frame": 191, "guiReload": str(auxiliary)},
                      {"frame": 197, "guiUnload": True}]
            options["bimShift"] = [5, 0, -1]
        else:
            anchor = {"name": "Mixed anchor", "mesh": 0, "translation": [1.25, 4.625, -1], "scale": [.4, .4, .4]}
            specs = write_gltf(source, [anchor, floor], [0, 1])
            samples = [1, 3, 120, 144, 148, 240, 244, 256]
            joint = {"type": "slider", "bodyA": {"node": specs["Mixed anchor"]["node"]}, "bodyB": {"bimObject": 0},
                     "anchor": [1.25, 3.625, -1], "axis": [1, 0, 0], "limits": [-.75, .75],
                     "motor": {"mode": "velocity", "targetVelocity": .8, "maxForceOrTorque": 1000}}
            events = [{"frame": 1, "physicsNode": specs["Mixed anchor"]["node"], "physicsAttach": "static"},
                      {"frame": 2, "bimSelectedObject": 0, "physicsAttach": "dynamic", "physicsSettings": {"gravityFactor": 0}},
                      {"frame": 3, "physicsJointCreate": joint, "physicsAction": "play"},
                      {"frame": 141, "physicsAction": "reset"},
                      {"frame": 145, "bimSelectedObject": 0, "physicsAttach": "dynamic", "physicsSettings": {"gravityFactor": 0}},
                      {"frame": 149, "physicsAction": "play"}, {"frame": 241, "physicsAction": "detach"},
                      {"frame": 253, "guiUnload": True}]
            options["bimShift"] = [2, 0, -1]
    elif case == "ccd-thin-wall":
        wall = {"name": "Thin CCD wall", "mesh": 0, "translation": [0, 2, 0], "scale": [.05, 4, 4]}
        fast = {"name": "Fast CCD body", "mesh": 0, "translation": [-5, 2, 0], "scale": [.2, .2, .2]}
        specs = write_gltf(source, [wall, fast], [0, 1])
        options["dynamic"] = specs["Fast CCD body"]["node"]
        options["initialVelocity"] = [speed, 0, 0]
        samples = [1, 2, 3, 5, 16, 20]
        events = [{"frame": 1, "physicsNode": specs["Thin CCD wall"]["node"], "physicsAttach": "static"},
                  {"frame": 2, "physicsNode": options["dynamic"], "physicsAttach": "dynamic",
                   "physicsSettings": {"gravityFactor": 0, "useCCD": True, "linearVelocity": options["initialVelocity"]}},
                  {"frame": 3, "physicsAction": "play"}, {"frame": 17, "guiUnload": True}]
    elif case == "joint-during-kinematic-motion":
        pusher_name = "Joint creation animated kinematic source"
        specs = write_gltf(source, [
            {"name": pusher_name, "mesh": 0, "translation": [-2, 0, 0]},
            {"name": "Unrelated fixed joint body", "mesh": 0, "translation": [20, 0, 0]}], [0, 1])
        auxiliary = directory / "kinematic-pushed-cube.bim"
        points, _, indices = cube()
        auxiliary.write_text(json.dumps({"schema_version": "1.1.0", "info": {},
            "meshes": [{"mesh_id": 0, "coordinates": [component for x, y, z in points
                        for component in (x, -z, y)], "indices": indices}],
            "elements": [{"mesh_id": 0, "guid": "joint-creation-pushed-cube", "type": "IfcBuildingElementProxy",
                "vector": {"x": 0, "y": 0, "z": 0}, "rotation": {"qx": 0, "qy": 0, "qz": 0, "qw": 1},
                "color": {"r": 60, "g": 170, "b": 220, "a": 255}, "info": {"Name": "Pushed BIM cube"}}]}), encoding="utf-8")
        options.update({"pusher": specs[pusher_name]["node"],
            "jointBody": specs["Unrelated fixed joint body"]["node"], "animatedName": pusher_name,
            "pushedGuid": "joint-creation-pushed-cube", "bimGeometry": bim_source_geometry(auxiliary),
            "fixedRenderDeltaSeconds": 4 / 60})
        samples = [1, 2, 3, 4, 6, 8, 10, 12, 16]
        joint = {"type": "fixed", "bodyA": {"world": True}, "bodyB": {"node": options["jointBody"]},
                 "anchor": [20, 0, 0], "axis": [0, 1, 0]}
        events = [{"frame": 1, "physicsNode": options["pusher"], "physicsAttach": "kinematic",
                   "physicsSettings": {"friction": 0}},
                  {"frame": 2, "bimSelectedObject": 0, "physicsAttach": "dynamic", "physicsAction": "play",
                   "physicsSettings": {"friction": 0, "gravityFactor": 0}},
                  {"frame": 3, "physicsNode": options["jointBody"], "physicsAttach": "dynamic",
                   "physicsSettings": {"gravityFactor": 0}},
                  {"frame": 4, "physicsJointCreate": joint}, {"frame": 7, "physicsAction": "reset"},
                  {"frame": 9, "bimSelectedObject": 0, "physicsAction": "detach"},
                  {"frame": 11, "physicsNode": options["jointBody"], "physicsAction": "detach"},
                  {"frame": 15, "guiUnload": True}]
    else:
        root = Path(__file__).resolve().parents[2]
        source = root / "models/validation/taa_scene.gltf"
        replacement = root / "models/validation/taa_equivalent.bim"
        specs = gltf_source_geometry(source)
        options["bimGeometry"] = bim_source_geometry(replacement)
        options["sourceSha256"] = hashlib.sha256(source.read_bytes()).hexdigest()
        options["replacementSha256"] = hashlib.sha256(replacement.read_bytes()).hexdigest()
        options["dynamic"] = specs["Moving cube"]["node"]
        joint = {"type": "slider", "bodyA": {"node": specs["Floor"]["node"]}, "bodyB": {"node": options["dynamic"]},
                 "anchor": [0, .6, 0], "axis": [0, 1, 0], "limits": [0, 1.5],
                 "motor": {"mode": "velocity", "targetVelocity": 1, "maxForceOrTorque": 1000}}
        samples = [1, 3, 120, 122, 126, 240, 256]
        events = [{"frame": 1, "physicsNode": specs["Floor"]["node"], "physicsAttach": "static"},
                  {"frame": 2, "physicsNode": options["dynamic"], "physicsAttach": "dynamic"},
                  {"frame": 3, "physicsJointCreate": joint, "physicsAction": "play"},
                  {"frame": 121, "guiReload": str(replacement)},
                  {"frame": 123, "bimSelectedObject": 0, "physicsAttach": "static"},
                  {"frame": 124, "bimSelectedObject": 2, "physicsAttach": "dynamic"},
                  {"frame": 125, "physicsJointCreate": {**joint, "bodyA": {"bimObject": 0}, "bodyB": {"bimObject": 2}}, "physicsAction": "play"},
                  {"frame": 253, "guiUnload": True}]
    options["primaryGeometry"] = {str(spec["node"]): spec for spec in specs.values()}
    count = samples[-1]
    sequence = {"schemaVersion": 1, "frames": count, "sampleFrames": samples,
                "camera": [{"frame": 1, "position": [10, 8, 15], "target": [2, 2, 0]},
                           {"frame": count, "position": [10, 8, 15], "target": [2, 2, 0]}], "events": events}
    if case == "ccd-thin-wall":
        sequence["camera"] = [{"frame": frame, "position": [-6, 4, 9], "target": [-2, 2, 0]} for frame in (1, count)]
    if case == "joint-during-kinematic-motion":
        sequence["camera"] = [{"frame": frame, "position": [6, 4, 10], "target": [1, 0, 0]} for frame in (1, count)]
        sequence.update({"objectNode": 0, "objectName": options["animatedName"],
            "objectTranslation": [{"frame": frame, "translation": [0, 0, 0] if frame <= 3 else [4, 0, 0]}
                                  for frame in (1, 3, 4, count)]})
    if case == "bim-global-translation":
        sequence["bimTranslation"] = [{"frame": 1, "translation": [0, 0, 0]}, {"frame": 90, "translation": [0, 0, 0]},
                                      {"frame": 91, "translation": options["bimShift"]}, {"frame": count, "translation": options["bimShift"]}]
    if case == "mixed-provider-slider":
        sequence["bimTranslation"] = [{"frame": 1, "translation": options["bimShift"]}]
    return source, auxiliary, sequence, options


def transform_vertices(transform, vertices):
    import numpy as np
    return (transform @ np.column_stack((vertices, np.ones(len(vertices)))).T).T[:, :3]


def check_kinematic_contact(record, samples):
    import numpy as np
    specification = record["fixture"]
    resize = specification["case"] == "kinematic-resize-contact"
    primary_node = specification["primaryNode"]

    def primary(frame):
        return next(body for body in samples[frame]["physics"]["bodies"] if body["node"] == primary_node)

    def auxiliary(frame):
        state = samples[frame]["physics"]
        element = next(element for element in state["auxiliaryElements"]
                       if any(obj["guid"] == specification["bimGuid"] for obj in element["objects"]))
        return next(body for body in state["bodies"] if body["node"] == element["node"]), element

    def support(frame):
        return primary(frame) if resize else auxiliary(frame)[0]

    def pushed(frame):
        return auxiliary(frame)[0] if resize else primary(frame)

    # Check the reproduced bug before newer identity telemetry. The old native
    # executable must fail for lost contact, rather than missing a new field.
    first_contact = pushed(4)["position"][0]
    assert abs(first_contact - specification["contactCenter"]) < .3, "Kinematic rebuild skipped the pending contact sweep"
    assert pushed(6)["position"][0] > first_contact + 1, "Pushed body lost its contact velocity after rebuild"
    assert record["sampleFrames"] == CONTACT_SAMPLE_FRAMES, "Unexpected kinematic-contact sample frames"
    assert specification["fixedRenderDeltaSeconds"] == 4 / 60, "Kinematic contact requires four physics ticks per frame"
    auxiliary_count = samples[1]["scene"]["auxiliaryObjectCount"]
    assert auxiliary_count >= len(specification["bimGeometry"]), "Kinematic contact is missing imported BIM geometry"
    initial_auxiliary_frame = 2 if resize else 1
    auxiliary_node = auxiliary(initial_auxiliary_frame)[0]["node"]
    product_identity = auxiliary(initial_auxiliary_frame)[1]["productId"]
    for frame, telemetry in samples.items():
        if frame >= 15:
            continue
        state, scene = telemetry["physics"], telemetry["scene"]
        assert scene["primaryObjectCount"] == 1 and scene["auxiliaryObjectCount"] == auxiliary_count, "Kinematic contact changed imported geometry counts"
        primary_live = frame < 11 and (resize or frame >= 2)
        auxiliary_live = frame < 9 and (not resize or frame >= 2)
        expected_nodes = ({primary_node} if primary_live else set()) | ({auxiliary_node} if auxiliary_live else set())
        assert {body["node"] for body in state["bodies"]} == expected_nodes and state["bodyCount"] == len(state["bodies"]) == len(expected_nodes), "Kinematic contact bindings were not retired correctly"
        assert state["jointCount"] == len(state["joints"]) == 0, "Kinematic contact unexpectedly created a joint"
        assert len(state["auxiliaryElements"]) == int(auxiliary_live), "Kinematic contact retained detached BIM publication"
        if primary_live:
            primary_body = primary(frame)
            assert len(primary_body["renderWorldMatrix"]) == 16, "Kinematic contact is missing the primary rendered transform"
            physical_model = node_matrix({"translation": primary_body["position"], "rotation": primary_body["rotation"]})
            if resize and frame >= 4:
                physical_model[:3, :3] *= 1.2
            assert np.allclose(matrix(primary_body["sceneWorldMatrix"]), physical_model, atol=3e-4, rtol=0), "Kinematic contact primary proxy disagrees with its body pose"
        if auxiliary_live:
            aux_body, element = auxiliary(frame)
            assert aux_body["node"] == auxiliary_node and element["productId"] == product_identity, "Kinematic rebuild changed BIM product membership"
            assert {obj["objectIndex"] for obj in element["objects"]} == {int(index) for index in specification["bimGeometry"]}, "Kinematic rebuild changed BIM fragment membership"
            physical_proxy = node_matrix({"translation": aux_body["position"], "rotation": aux_body["rotation"]})
            assert np.allclose(matrix(aux_body["sceneWorldMatrix"]), physical_proxy, atol=3e-4, rtol=0), "Kinematic contact BIM proxy disagrees with its body pose"
        expected_playing = 2 <= frame < 7
        assert state["playing"] == expected_playing, "Kinematic contact Play/reset state is wrong"
        assert state["substeps"] == (4 if expected_playing else 0), "Kinematic contact did not run four fixed ticks"
        assert state["totalSteps"] == (4 * (frame - 1) if frame < 7 else 0), "Kinematic rebuild changed fixed-step accumulation"
        if frame < 9:
            current_support = support(frame)
            expected_center = [-2 if frame < 4 else 2, 0, 0]
            expected_model = translation(expected_center)
            if resize and frame >= 4:
                expected_model[:3, :3] *= 1.2
            assert current_support["motion"] == 1 and current_support["settings"]["gravityFactor"] == 0
            expected_friction = .25 if not resize and frame < 4 else 0
            assert current_support["settings"]["friction"] == expected_friction, "Kinematic replacement settings were not propagated"
            assert np.allclose(current_support["position"], expected_center, atol=1e-4, rtol=0), "Kinematic support did not reach its authored target"
            assert np.allclose(matrix(current_support["sceneWorldMatrix"]), expected_model, atol=2e-4, rtol=0), "Kinematic support scale or reset target is wrong"
        if 2 <= frame < 9:
            current_pushed = pushed(frame)
            assert current_pushed["motion"] == 2 and current_pushed["settings"]["gravityFactor"] == 0 and current_pushed["settings"]["friction"] == 0, "Pushed body settings were not propagated"
            assert np.allclose(current_pushed["position"][1:], [0, 0], atol=4e-4, rtol=0), "Kinematic contact produced transverse drift"
    for frame in (2, 3, 8):
        assert np.allclose(pushed(frame)["position"], [0, 0, 0], atol=1e-4, rtol=0), "Pushed body authored/reset pose changed"
    support_ids = [support(frame)["id"] for frame in (1, 3, 4, 6, 8)]
    pushed_ids = [pushed(frame)["id"] for frame in (2, 3, 4, 6, 8)]
    assert all(identity > 0 for identity in support_ids + pushed_ids), "Kinematic contact is missing live body identities"
    assert support_ids[0] == support_ids[1] and support_ids[1] != support_ids[2] == support_ids[3] and support_ids[3] != support_ids[4], "Kinematic rebuild/reset did not replace the support body handle"
    assert len(set(pushed_ids[:4])) == 1 and pushed_ids[3] != pushed_ids[4], "Kinematic support replacement unexpectedly rebuilt the pushed body"


def check_dynamic_support_teleport(record, samples):
    import numpy as np
    specification = record["fixture"]

    def primary(frame, node):
        return next(body for body in samples[frame]["physics"]["bodies"] if body["node"] == node)

    def rider(frame):
        state = samples[frame]["physics"]
        element = next(element for element in state["auxiliaryElements"]
                       if any(obj["guid"] == specification["bimGuid"] for obj in element["objects"]))
        return next(body for body in state["bodies"] if body["node"] == element["node"]), element

    settled = rider(180)[0]["position"]
    fallen = rider(210)[0]["position"]
    # Keep the observed missing wake as the first case-specific failure on the
    # old native executable, independent of additional lifecycle telemetry.
    assert settled[1] - fallen[1] > .7 and abs(fallen[1] - .5) < .08, "Dynamic support teleport left its former rider floating"
    assert abs(settled[1] - 1.5) < .03, "Dynamic support rider never rested on its support"
    assert rider(184)[0]["position"][1] < settled[1] - .001, "Dynamic support rider missed the first resumed ticks"
    assert record["sampleFrames"] == SUPPORT_TELEPORT_SAMPLE_FRAMES, "Unexpected dynamic-support sample frames"
    assert samples[180]["physics"]["activeBodyCount"] == 0, "Dynamic support stack did not sleep before its edit"
    assert samples[182]["physics"]["activeBodyCount"] == 2, "Dynamic support edit did not wake its former contact"
    assert np.allclose(rider(182)[0]["position"], settled, atol=1e-5, rtol=0), "Paused support edit advanced its former rider"
    rider_node = rider(3)[0]["node"]
    product_identity = rider(3)[1]["productId"]
    auxiliary_count = samples[3]["scene"]["auxiliaryObjectCount"]
    assert auxiliary_count >= len(specification["bimGeometry"]), "Dynamic support fixture is missing BIM geometry"
    for frame, telemetry in samples.items():
        if frame >= 225:
            continue
        state, scene = telemetry["physics"], telemetry["scene"]
        expected_nodes = {specification["floorNode"]}
        if frame < 219:
            expected_nodes.add(specification["supportNode"])
        if frame < 215:
            expected_nodes.add(rider_node)
        assert {body["node"] for body in state["bodies"]} == expected_nodes and state["bodyCount"] == len(state["bodies"]) == len(expected_nodes), "Dynamic support bindings were not retired correctly"
        assert state["jointCount"] == len(state["joints"]) == 0
        assert scene["primaryObjectCount"] == 2 and scene["auxiliaryObjectCount"] == auxiliary_count, "Dynamic support changed imported geometry counts"
        assert len(state["auxiliaryElements"]) == int(frame < 215), "Dynamic support retained detached BIM publication"
        for node in expected_nodes - {rider_node}:
            assert len(primary(frame, node)["renderWorldMatrix"]) == 16, "Dynamic support is missing a primary rendered transform"
        if frame < 219:
            support = primary(frame, specification["supportNode"])
            assert support["motion"] == 2 and support["settings"]["gravityFactor"] == 1
            assert abs(support["position"][0] - (0 if frame < 182 else 10)) < .01, "Dynamic support teleport/reset lost the authored source parent"
            assert abs(support["position"][1] - .5) < .04
        if frame < 215:
            current, element = rider(frame)
            assert current["motion"] == 2 and current["settings"]["gravityFactor"] == 1
            assert current["node"] == rider_node and element["productId"] == product_identity, "Dynamic support changed BIM product membership"
            assert {obj["objectIndex"] for obj in element["objects"]} == {int(index) for index in specification["bimGeometry"]}
            pose = node_matrix({"translation": current["position"], "rotation": current["rotation"]})
            assert np.allclose(matrix(current["sceneWorldMatrix"]), pose, atol=3e-4, rtol=0), "Dynamic support BIM proxy disagrees with its body pose"
            assert np.allclose(current["position"][::2], [0, 0], atol=.01, rtol=0), "Dynamic support rider drifted sideways"
        playing = frame <= 180 or 183 <= frame <= 210
        expected_steps = frame - 2 if frame <= 180 else 178 if frame <= 182 else frame - 4 if frame <= 210 else 0
        assert state["playing"] == playing and state["substeps"] == int(playing), "Dynamic support pause/resume timing is wrong"
        assert state["totalSteps"] == expected_steps, "Dynamic support lost fixed-step accumulation"
    assert np.allclose(rider(214)[0]["position"], [0, 1.5, 0], atol=1e-4, rtol=0), "Dynamic support reset lost the rider's authored pose"


def check_contact_wake_chain(record, samples):
    import numpy as np
    specification = record["fixture"]
    nodes = specification["chainNodes"]

    def primary(frame, node):
        return next(body for body in samples[frame]["physics"]["bodies"] if body["node"] == node)

    def platform(frame):
        state = samples[frame]["physics"]
        element = next(element for element in state["auxiliaryElements"]
                       if any(obj["guid"] == specification["bimGuid"] for obj in element["objects"]))
        return next(body for body in state["bodies"] if body["node"] == element["node"]), element

    # The old neighborhood wake activates only the bottom AABB hit. Require
    # its remote welded links before the first resumed solver snapshot.
    assert samples[182]["physics"]["activeBodyCount"] == 3, "Contact neighborhood failed to wake every dynamic chain link"
    assert samples[180]["physics"]["activeBodyCount"] == 0, "Contact chain never slept on its platform"
    assert record["sampleFrames"] == CONTACT_WAKE_CHAIN_SAMPLE_FRAMES, "Unexpected contact-chain sample frames"
    for frame, ticks in ((184, 2), (210, 28)):
        expected_drop = -9.81 / 60**2 * ticks * (ticks + 1) / 2
        offsets = [np.asarray(primary(frame, node)["position"]) - primary(180, node)["position"] for node in nodes]
        assert all(abs(offset[1] - expected_drop) < .01 and np.linalg.norm(offset[[0, 2]]) < .01 for offset in offsets), "Contact chain did not fall uniformly on the resumed ticks"
        assert np.max(np.ptp(np.asarray(offsets), axis=0)) < .002, "Contact wake left a sleeping link behind its mechanism"
    platform_node = platform(7)[0]["node"]
    product_identity = platform(7)[1]["productId"]
    auxiliary_count = samples[7]["scene"]["auxiliaryObjectCount"]
    assert auxiliary_count >= len(specification["bimGeometry"]), "Contact chain is missing its imported platform"
    initial_joints = {(joint["nodeA"], joint["nodeB"]): joint["id"] for joint in samples[7]["physics"]["joints"]}
    expected_pairs = set(zip(nodes, nodes[1:]))
    assert set(initial_joints) == expected_pairs, "Contact chain has the wrong fixed joints"
    for frame, telemetry in samples.items():
        if frame >= 225:
            continue
        state, scene = telemetry["physics"], telemetry["scene"]
        expected_nodes = set(nodes if frame < 215 else (nodes[0], nodes[2]))
        if frame < 219:
            expected_nodes.add(platform_node)
        assert {body["node"] for body in state["bodies"]} == expected_nodes and state["bodyCount"] == len(state["bodies"]) == len(expected_nodes), "Contact chain bindings were not retired correctly"
        assert scene["primaryObjectCount"] == 3 and scene["auxiliaryObjectCount"] == auxiliary_count, "Contact chain changed imported geometry counts"
        assert len(state["auxiliaryElements"]) == int(frame < 219), "Contact chain retained detached platform publication"
        for node in expected_nodes - {platform_node}:
            body = primary(frame, node)
            assert body["motion"] == 2 and body["settings"]["gravityFactor"] == 1
            assert len(body["renderWorldMatrix"]) == 16, "Contact chain is missing a primary rendered transform"
            pose = node_matrix({"translation": body["position"], "rotation": body["rotation"]})
            assert np.allclose(matrix(body["sceneWorldMatrix"]), pose, atol=3e-4, rtol=0), "Contact chain rendered proxy disagrees with its physical pose"
            if frame in (182, 214):
                expected = primary(180, node)["position"] if frame == 182 else [0, specification["chainHeights"][nodes.index(node)], 0]
                assert np.allclose(body["position"], expected, atol=1e-4, rtol=0), "Contact chain paused edit or reset changed a link pose"
        if frame < 219:
            body, element = platform(frame)
            expected_center = [0 if frame < 182 else 10, -.5, 0]
            assert body["motion"] == 0 and body["node"] == platform_node and element["productId"] == product_identity
            assert {obj["objectIndex"] for obj in element["objects"]} == {int(index) for index in specification["bimGeometry"]}
            assert np.allclose(body["position"], expected_center, atol=1e-4, rtol=0), "Contact platform translation/reset changed its authored pose"
            assert np.allclose(matrix(body["sceneWorldMatrix"]), translation(expected_center), atol=1e-4, rtol=0), "Contact platform proxy disagrees with its physical pose"
        expected_joint_count = 2 if frame < 215 else 0
        assert state["jointCount"] == len(state["joints"]) == expected_joint_count, "Middle-link detach did not retire both fixed joints"
        assert {(joint["nodeA"], joint["nodeB"]) for joint in state["joints"]} == (expected_pairs if frame < 215 else set()), "Contact chain changed its fixed-joint membership"
        for joint in state["joints"]:
            pair = (joint["nodeA"], joint["nodeB"])
            assert joint["type"] == "fixed" and pair in initial_joints and joint["id"] == initial_joints[pair], "Contact chain reset changed its fixed-joint identity"
            first, second = (primary(frame, node) for node in pair)
            first_pose = node_matrix({"translation": first["position"], "rotation": first["rotation"]})
            second_pose = node_matrix({"translation": second["position"], "rotation": second["rotation"]})
            a = first_pose @ np.array([0, .6, 0, 1])
            b = second_pose @ np.array([0, -.6, 0, 1])
            assert np.linalg.norm(a - b) < .004, "Contact chain fixed anchors separated"
        playing = frame <= 180 or 183 <= frame <= 210
        expected_steps = frame - 6 if frame <= 180 else 174 if frame <= 182 else frame - 8 if frame <= 210 else 0
        assert state["playing"] == playing and state["substeps"] == int(playing), "Contact chain pause/resume timing is wrong"
        assert state["totalSteps"] == expected_steps, "Contact chain lost fixed-step accumulation"
    for node in nodes:
        assert primary(180, node)["id"] == primary(182, node)["id"] == primary(210, node)["id"], "Contact wake unexpectedly replaced a body"
        assert primary(214, node)["id"] != primary(210, node)["id"], "Contact chain Reset retained an obsolete body handle"


def check(record):
    import numpy as np
    assert not record.get("validationErrors"), "Scene diversity capture recorded validation errors"
    samples = {sample["frame"]: sample["telemetry"] for sample in record["samples"]}
    fixture = record["fixture"]
    case = fixture["case"]
    assert sorted(samples) == record["sampleFrames"] and len(samples) == len(record["samples"]), "Incomplete diversity captures"
    base_bim = matrix(BIM_ROTATION)
    for frame, telemetry in samples.items():
        state = telemetry["physics"]
        assert state["available"] and not state.get("error"), f"Physics failed at frame {frame}"
        assert state.get("droppedSeconds", 0) == 0
        for body in state.get("bodies", []):
            scene = matrix(body["sceneWorldMatrix"])
            if body["renderWorldMatrix"]:
                rendered = matrix(body["renderWorldMatrix"])
                assert np.allclose(rendered, scene, atol=1e-5, rtol=0), "Primary rendered transform is stale"
                spec = fixture["primaryGeometry"][str(body["node"])]
                vertices = np.asarray(spec["vertices"], dtype=float)
                center = (vertices.min(axis=0) + vertices.max(axis=0)) / 2
                expected = (rendered @ np.append(center, 1))[:3]
                assert np.allclose(expected, body["position"], atol=3e-4, rtol=0), "Off-center collider disagrees with rendered mesh center"
                if body["motion"] == 0:
                    assert np.allclose(rendered, matrix(spec["authoredModel"]), atol=1e-5, rtol=0), "Static primary scene moved"
        for element in state.get("auxiliaryElements", []):
            objects = element["objects"]
            original = [fixture["bimGeometry"][str(obj["objectIndex"])] for obj in objects]
            world = np.concatenate([transform_vertices(base_bim, spec["vertices"]) for spec in original])
            center = (world.min(axis=0) + world.max(axis=0)) / 2
            body = next(body for body in state["bodies"] if body["node"] == element["node"])
            expected_model = matrix(body["sceneWorldMatrix"]) @ np.linalg.inv(translation(center)) @ base_bim
            for obj, spec in zip(objects, original):
                assert obj["guid"] == spec["guid"], "Physics changed semantic product identity"
                model = matrix(obj["model"])
                assert np.allclose(model, expected_model, atol=4e-4, rtol=0), "BIM fragments lost rigid product placement"
                actual_vertices = transform_vertices(model, spec["vertices"])
                assert np.allclose(obj["boundsMin"], actual_vertices.min(axis=0), atol=5e-4, rtol=0), "BIM minimum bounds disagree with original geometry"
                assert np.allclose(obj["boundsMax"], actual_vertices.max(axis=0), atol=5e-4, rtol=0), "BIM maximum bounds disagree with original geometry"
                assert np.allclose(matrix(obj["normalMatrix"], columns=3)[:3, :3], np.linalg.inv(model[:3, :3]).T,
                                   atol=3e-4, rtol=0), "BIM normal matrix is stale"

    def body(frame, node=None, guid=None):
        state = samples[frame]["physics"]
        if guid is not None:
            element = next(element for element in state["auxiliaryElements"] if any(obj["guid"] == guid for obj in element["objects"]))
            node = element["node"]
        return next(body for body in state["bodies"] if body["node"] == node)

    if case in ("nested-offcenter", "mirrored-parent"):
        first, settled = body(3, fixture["dynamic"]), body(180, fixture["dynamic"])
        assert first["position"][1] - settled["position"][1] > 2, "Nested off-center body did not fall"
        assert abs(settled["position"][1] - fixture["settledHeight"]) < .1, "Scaled collider settled at the wrong floor height"
        reset = body(184, fixture["dynamic"])
        assert abs(reset["position"][1] - 4) < 1e-4 and not samples[184]["physics"]["playing"], "Nested body reset lost the parent transform"
        determinant = np.linalg.det(matrix(settled["renderWorldMatrix"])[:3, :3])
        assert (determinant < 0) == (case == "mirrored-parent"), "Physics lost mirrored scale handedness"
        assert samples[188]["physics"]["bodyCount"] == 1, "Detach retained the nested dynamic body"
    elif case == "ramp-contact":
        first, final = body(4, fixture["dynamic"]), body(180, fixture["dynamic"])
        assert final["position"][0] > first["position"][0] + 1, "Ramp did not produce downslope motion"
        assert .35 < final["position"][1] < 1, "Ramp body failed to contact the floor"
        assert samples[180]["physics"]["bodyCount"] == 3
    elif case == "bim-global-translation":
        first, settled = body(4, guid="diversity-L"), body(180, guid="diversity-L")
        assert first["position"][1] - settled["position"][1] > 2
        assert abs(settled["position"][1] - .875) < .12, "Asymmetric BIM product missed floor contact"
        old_other, moved_other = body(90, guid="diversity-tetrahedron"), body(92, guid="diversity-tetrahedron")
        assert np.allclose(np.asarray(moved_other["position"])-old_other["position"], fixture["bimShift"], atol=2e-4, rtol=0), "Global translation missed the static BIM body"
        reset = body(184, guid="diversity-L")
        assert np.allclose(reset["position"], [4.25, 3.625, -1], atol=2e-4, rtol=0), "Global translation was lost on reset"
        assert samples[194]["physics"]["bodyCount"] == 0, "BIM replacement retained old bindings"
    elif case == "mixed-provider-slider":
        for frame in (120, 240):
            moving = body(frame, guid="diversity-L")
            assert 1.85 < moving["position"][0] < 2.1, "Mixed-provider slider motor missed its stop"
            assert np.allclose(moving["position"][1:], [3.625, -1], atol=.03, rtol=0), "Mixed joint lost transverse lock"
            assert moving["settings"]["gravityFactor"] == 0, "Mechanism body settings were not propagated"
        initial_joint = samples[120]["physics"]["joints"][0]["id"]
        assert samples[144]["physics"]["joints"][0]["id"] == samples[148]["physics"]["joints"][0]["id"] == initial_joint, "Reset/replacement lost mixed joint identity"
        assert np.allclose(body(144, guid="diversity-L")["position"], [1.25, 3.625, -1], atol=2e-4, rtol=0)
        assert samples[244]["physics"]["jointCount"] == 0
    elif case == "joint-during-kinematic-motion":
        assert record["sampleFrames"] == [1, 2, 3, 4, 6, 8, 10, 12, 16], "Unexpected joint-motion sample frames"
        assert fixture["fixedRenderDeltaSeconds"] == 4 / 60, "Joint motion requires four physics ticks per frame"
        # BIM telemetry includes the importer's generated edge/point overlays,
        # so preserve its loaded baseline rather than assuming one draw object.
        auxiliary_count = samples[1]["scene"]["auxiliaryObjectCount"]
        assert auxiliary_count >= len(fixture["bimGeometry"]), "Joint motion is missing imported BIM geometry"
        joint_identity = None
        for frame, telemetry in samples.items():
            state, scene = telemetry["physics"], telemetry["scene"]
            if frame >= 15:
                continue
            assert scene["primaryObjectCount"] == 2 and scene["auxiliaryObjectCount"] == auxiliary_count, "Joint motion changed imported geometry counts"
            expected_nodes = {fixture["pusher"]}
            if 3 <= frame < 11:
                expected_nodes.add(fixture["jointBody"])
            pushed = body(frame, guid=fixture["pushedGuid"]) if 2 <= frame < 9 else None
            if pushed:
                expected_nodes.add(pushed["node"])
                assert pushed["motion"] == 2 and pushed["settings"]["gravityFactor"] == 0 and pushed["settings"]["friction"] == 0, "Pushed BIM settings were not propagated"
                # This scene has translation-only motion. Bind the proxy's full
                # basis and translation to the actual backend center, rather
                # than allowing consistently stale BIM models/bounds to pass.
                assert np.allclose(matrix(pushed["sceneWorldMatrix"]), translation(pushed["position"]), atol=3e-4, rtol=0), "BIM proxy disagrees with the pushed body pose"
                assert np.allclose(pushed["position"][1:], [0, 0], atol=3e-4, rtol=0), "Kinematic contact produced transverse drift"
            assert {item["node"] for item in state["bodies"]} == expected_nodes and state["bodyCount"] == len(state["bodies"]) == len(expected_nodes), "Joint-motion bindings were not retired correctly"
            assert len(state["auxiliaryElements"]) == int(pushed is not None), "Detach retained auxiliary publication"
            pusher = body(frame, fixture["pusher"])
            assert len(pusher["renderWorldMatrix"]) == 16, "Joint-motion primary render matrix is missing"
            expected_pusher = [-2 if frame < 4 else 2, 0, 0]
            assert pusher["motion"] == 1 and pusher["settings"]["friction"] == 0, "Kinematic pusher settings were not propagated"
            assert np.allclose(pusher["position"], expected_pusher, atol=1e-4, rtol=0) and np.allclose(matrix(pusher["sceneWorldMatrix"]), translation(expected_pusher), atol=1e-4, rtol=0), "Kinematic sweep did not reach its authored target"
            if fixture["jointBody"] in expected_nodes:
                remote = body(frame, fixture["jointBody"])
                assert len(remote["renderWorldMatrix"]) == 16, "Joint-motion primary render matrix is missing"
                assert remote["motion"] == 2 and remote["settings"]["gravityFactor"] == 0
                assert np.allclose(matrix(remote["sceneWorldMatrix"]), translation([20, 0, 0]), atol=1e-4, rtol=0), "Unrelated joint body moved"
            expected_playing = 2 <= frame < 7
            assert state["playing"] == expected_playing, "Joint-motion Play/reset state is wrong"
            assert state["substeps"] == (4 if expected_playing else 0), "Joint motion did not run four fixed ticks"
            assert state["totalSteps"] == (4 * (frame - 1) if frame < 7 else 0), "Joint creation changed fixed-step accumulation"
            joint_live = 4 <= frame < 11
            assert state["jointCount"] == len(state["joints"]) == int(joint_live), "Joint lifecycle did not match endpoint retirement"
            if joint_live:
                joint = state["joints"][0]
                assert joint["type"] == "fixed" and joint["nodeA"] is None and joint["nodeB"] == fixture["jointBody"], "Joint creation used the moving collision bodies"
                assert np.allclose(joint["settings"]["anchor"], [20, 0, 0], atol=1e-4, rtol=0), "Unrelated joint anchor changed"
                if joint_identity is None:
                    joint_identity = joint["id"]
                assert joint["id"] == joint_identity, "Reset changed the unrelated joint identity"
        for frame in (2, 3, 8):
            assert np.allclose(body(frame, guid=fixture["pushedGuid"])["position"], [0, 0, 0], atol=1e-4, rtol=0), "Pushed BIM authored/reset pose changed"
        pushed_after = body(4, guid=fixture["pushedGuid"])
        assert 2.7 < pushed_after["position"][0] < 3.3, "Joint creation skipped the kinematic contact sweep"
        assert body(6, guid=fixture["pushedGuid"])["position"][0] > pushed_after["position"][0] + 1, "Pushed BIM body lost its contact velocity"
    elif case in KINEMATIC_CONTACT_CASES:
        check_kinematic_contact(record, samples)
    elif case == "dynamic-support-teleport":
        check_dynamic_support_teleport(record, samples)
    elif case == "contact-wake-chain":
        check_contact_wake_chain(record, samples)
    elif case == "ccd-thin-wall":
        initial = body(2, fixture["dynamic"])
        assert np.allclose(initial["position"], [-5, 2, 0], atol=1e-5, rtol=0)
        settings = initial["settings"]
        assert settings["useCCD"] and settings["gravityFactor"] == 0, "CCD/gravity settings were not propagated"
        assert settings["linearVelocity"] == fixture.get("initialVelocity", [500, 0, 0]), "Fast-body velocity was not propagated"
        for frame in (3, 5, 16):
            stopped = body(frame, fixture["dynamic"])
            # One fixed step would move at least 8.33m without CCD and miss
            # the 5cm wall. Check the actual known near face plus 10cm body half.
            assert -.3 < stopped["position"][0] <= CCD_STOP_MAXIMUM_X, "CCD body tunneled through the thin wall"
            assert np.allclose(stopped["position"][1:], [2, 0], atol=1e-4, rtol=0), "Zero-gravity CCD path drifted"
    else:
        assert body(120, fixture["dynamic"])["position"][1] > 1.9, "Repository glTF body did not move through the joint"
        assert samples[122]["physics"]["bodyCount"] == samples[122]["physics"]["jointCount"] == 0, "glTF to BIM replacement retained old physics"
        assert samples[122]["scene"]["primaryObjectCount"] == 0 and samples[122]["scene"]["auxiliaryObjectCount"] >= 15
        assert body(240, guid="taa-0002")["position"][1] > 1.9, "Repository BIM body did not move through the joint"
    final = samples[record["sampleFrames"][-1]]
    assert final["physics"]["bodyCount"] == final["physics"]["jointCount"] == 0, "Unload left physics in a diverse scene"
    assert final["scene"]["primaryObjectCount"] == final["scene"]["auxiliaryObjectCount"] == 0
    images = {sample["frame"]: sample["png"] for sample in record["samples"]}
    pair = (180, 210) if case in ("dynamic-support-teleport", "contact-wake-chain") else (3, 4) if case == "joint-during-kinematic-motion" or case in KINEMATIC_CONTACT_CASES else (2, 3) if case == "ccd-thin-wall" else (3, 180) if case in ("nested-offcenter", "mirrored-parent") else (4, 180) if case in ("ramp-contact", "bim-global-translation") else (3, 120)
    image_data = [Path(images[frame]).read_bytes() for frame in pair]
    assert all(image_data), "Diversity capture PNG is empty"
    hashes = [hashlib.sha256(data).hexdigest() for data in image_data]
    assert len(set(hashes)) == 2, "Visible scene motion did not change captured PNGs"
    if record["mode"] == "rays-taa":
        active = samples[120]
        assert active["rayShadows"]["active"] and active["taa"]["enabled"], "Diversity ray/TAA route was not active"
    return {"passed": True, "captures": len(samples), "independentGeometryBounds": True,
            "sceneCondition": case, "imageSha256": hashes}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--exe", type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--require-opt-in", action="store_true")
    parser.add_argument("--check-existing", action="store_true")
    parser.add_argument("--scene", "--case", dest="case", choices=CASES)
    parser.add_argument("--technique", choices=("deferred-raster", "forward-raster"))
    parser.add_argument("--include-ray-taa", action="store_true")
    parser.add_argument("--ccd-speed", type=parse_ccd_speed, default=500,
                        help="Initial CCD fixture speed (finite float32 >=500; default: 500)")
    args = parser.parse_args()
    if args.require_opt_in and os.environ.get("CONTAINER_RUN_GPU_PHYSICS") != "1":
        print("Set CONTAINER_RUN_GPU_PHYSICS=1 for native scene diversity captures")
        return 77
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    report = output / "results.json"
    if args.check_existing:
        results = json.loads(report.read_text(encoding="utf-8"))
    else:
        if args.exe is None:
            parser.error("--exe is required when capturing")
        exe = args.exe.resolve()
        selected = [(case, technique, "raster") for case in ((args.case,) if args.case else CASES)
                    for technique in ((args.technique,) if args.technique else ("deferred-raster", "forward-raster"))]
        if args.include_ray_taa:
            # Two additional conditions, distributed across the raster techniques.
            for case, technique in (("mixed-provider-slider", "deferred-raster"), ("repository-model-replacement", "forward-raster")):
                if (not args.case or args.case == case) and (not args.technique or args.technique == technique):
                    selected.append((case, technique, "rays-taa"))
        results = {"schemaVersion": 1, "passed": False, "expectedCases": selected, "cases": [],
                   "executableSha256": hashlib.sha256(exe.read_bytes()).hexdigest()}
    results["passed"] = False
    results.pop("failure", None)
    report.write_text(json.dumps(results, indent=2), encoding="utf-8")
    try:
        if not args.check_existing:
            from gui_model_loading_regression import run_lifecycle_capture
            (output / "vk_layer_settings.txt").write_text("khronos_validation.validate_sync = true\n", encoding="utf-8")
            (output / "imgui.ini").write_text("[Window][Scene Controls]\nPos=0,0\nSize=350,500\nCollapsed=1\n\n"
                "[Window][Renderer Telemetry]\nPos=0,25\nSize=350,500\nCollapsed=1\n", encoding="utf-8")
            for case, technique, mode in selected:
                source, auxiliary, sequence, specification = fixture(case, output / "fixtures" / case, ccd_speed=args.ccd_speed)
                extra = ["--directional-intensity", "2", "--environment-intensity", "1"]
                if auxiliary:
                    extra += ["--bim-model", str(auxiliary)]
                if case == "joint-during-kinematic-motion" or case in KINEMATIC_CONTACT_CASES:
                    # The shared lifecycle helper specifies 1/60 earlier;
                    # the CLI's last value wins to exercise four-tick batches.
                    extra += ["--fixed-dt", str(specification["fixedRenderDeltaSeconds"])]
                record = run_lifecycle_capture(exe, output, f"physics-diversity-{case}-{technique}-{mode}", source, sequence, technique, mode, extra=extra)
                record.update({"fixture": specification, "sampleFrames": sequence["sampleFrames"]})
                results["cases"].append(record)
                record["checks"] = check(record)
                report.write_text(json.dumps(results, indent=2), encoding="utf-8")
                print(json.dumps({"case": case, "technique": technique, "mode": mode, **record["checks"]}), flush=True)
            assert hashlib.sha256(exe.read_bytes()).hexdigest() == results["executableSha256"], "Executable changed during diversity capture"
        else:
            cases = {(record["fixture"]["case"], record["technique"], record["mode"]): record for record in results["cases"]}
            expected = {tuple(case) for case in results["expectedCases"]}
            supported = {(case, technique, "raster") for case in CASES
                         for technique in ("deferred-raster", "forward-raster")} | {
                         ("mixed-provider-slider", "deferred-raster", "rays-taa"),
                         ("repository-model-replacement", "forward-raster", "rays-taa")}
            assert expected <= supported, "Unknown diversity case matrix"
            assert expected and len(cases) == len(results["cases"]) == len(expected) and set(cases) == expected, "Incomplete diversity case matrix"
            for record in cases.values():
                for sample in record["samples"]:
                    local = output / Path(sample["png"]).name
                    if local.is_file():
                        sample["png"] = str(local)
                record["checks"] = check(record)
        results["passed"] = True
    except Exception as error:
        results["failure"] = str(error)
        print(f"Scene diversity validation failed: {error}", flush=True)
    report.write_text(json.dumps(results, indent=2), encoding="utf-8")
    return 0 if results["passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
