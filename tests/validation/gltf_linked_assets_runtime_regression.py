"""Validate linked glTF placement, aliases, picks, deformation, editing and export."""

import argparse
import base64
import copy
import hashlib
import json
from pathlib import Path

import numpy as np
from PIL import Image

from gltf_accessor_runtime_regression import Asset, load_asset, mesh, textured_material
from gltf_material_variants_runtime_regression import run


XMP = "KHR_xmp_json_ld"
LARGE_IDENTIFIER = 2**54 + 3
PLACEMENT_TRANSLATIONS = ((-.75, -.25, 0), (.75, .125, 0))
PLACEMENT_SCALES = ((1, 1, 1), (1, 1.25, 1))
BIND_POSITIONS = np.array([[-.25, -.25, 0], [.25, -.25, 0], [.25, .25, 0], [-.25, .25, 0]])
POSE_TIMES = (.25, 1.)


def packet(label):
    return {"@context": {"xmp": "http://ns.adobe.com/xap/1.0/"}, "xmp:Label": label}


def attach_packet(record, index):
    record.setdefault("extensions", {})[XMP] = {"packet": index}


def write_json(path, value):
    path.write_text(json.dumps(value, indent=2), encoding="utf-8")


def pixels(path):
    with Image.open(path) as image:
        return np.array(image.convert("RGB"))


def fixture(output):
    folder = output / "linked-inputs"
    leaf_dir = folder / "parts"
    leaf_dir.mkdir(parents=True)
    leaf_path = leaf_dir / "Sample panel.gltf"
    asset = Asset()
    mesh(asset, asset.accessor("VEC3", [[-.3, -.28, 0], [.3, -.28, 0], [.3, .28, 0], [-.3, .28, 0]]),
         asset.accessor("VEC3", [[0, 0, 1]] * 4), asset.accessor("VEC2", [[0, 0], [1, 0], [1, 1], [0, 1]]))
    textured_material(asset)
    asset.model["nodes"][0]["name"] = "Aliased textured panel"
    reference = output / "ordinary.gltf"
    positions = [[-.75, 0, 0], [.75, 0, 0]]
    asset.model["nodes"] = [{"name": "Reference panel", "mesh": 0, "translation": position} for position in positions]
    asset.model["scenes"][0]["nodes"] = [0, 1]
    asset.write(reference)

    leaf = copy.deepcopy(asset.model)
    leaf["nodes"] = [{"name": "Aliased textured panel", "mesh": 0}]
    leaf["scenes"][0]["nodes"] = [0]
    leaf.setdefault("extensionsUsed", []).append(XMP)
    leaf.setdefault("extensions", {})[XMP] = {"packets": [
        {**packet("Linked asset"), "xmp:Identifier": LARGE_IDENTIFIER},
        packet("Linked scene"), packet("Linked mesh"),
    ]}
    for index, record in enumerate((leaf["asset"], leaf["scenes"][0], leaf["meshes"][0])):
        attach_packet(record, index)
    for table, actual, alias in (("buffers", "Shared mesh.bin", "virtual%20mesh.bin"),
                                 ("images", "Shared image.png", "virtual%20image.png")):
        record = leaf[table][0]
        (folder / actual).write_bytes(base64.b64decode(record["uri"].split(",", 1)[1]))
        record["uri"] = alias
    write_json(leaf_path, leaf)
    primary = folder / "linked.gltf"
    manifest = {
        "asset": {"version": "2.1", "minVersion": "2.1"},
        "files": [
            {"uri": "parts/Sample%20panel.gltf", "mimeType": "model/gltf+json",
             "aliases": [{"alias": "virtual%20mesh.bin", "file": 1}, {"alias": "virtual%20image.png", "file": 2}]},
            {"uri": "Shared%20mesh.bin", "mimeType": "application/octet-stream"},
            {"uri": "Shared%20image.png", "mimeType": "image/png"},
        ],
        "externalAssets": [{"file": 0}],
        "nodes": [{"name": f"Placement {i}", "externalAsset": 0, "translation": position}
                  for i, position in enumerate(positions)],
        "scenes": [{"nodes": [0, 1]}], "scene": 0,
    }
    manifest["extensionsUsed"] = [XMP]
    manifest["extensions"] = {XMP: {"packets": [packet("Primary asset"), packet("First placement")]}}
    attach_packet(manifest["asset"], 0)
    attach_packet(manifest["nodes"][0], 1)
    write_json(primary, manifest)
    return primary, reference


def verify_picks(state):
    instancing = state["gltfInstancing"]
    assert instancing["sourceVertexCount"] == 4 and instancing["sourceIndexCount"] == 6, instancing
    assert instancing["drawCount"] == 2, instancing
    picks = state["gltfNodeFlags"]["probes"]
    assert len(picks) == 2 and len({pick["sourceNodeIndex"] for pick in picks}) == 2, picks
    for pick in picks:
        assert pick["projected"] and pick["hit"] and pick["nearestHitGraphNode"] == pick["graphNode"], pick
        assert pick["resolvedSelection"] == pick["graphNode"], pick


NESTED_CARRIER = "Nested panel carrier"
# Centers at ±.75 project here with the runner's 45-degree FOV, z=4, and 640x360 viewport.
NESTED_PICK_PIXELS = ([239, 180], [401, 180])


def nested_fixture(output):
    primary, baked = fixture(output)
    model = json.loads(primary.read_text(encoding="utf-8"))
    old_buffer = primary.parent / "Shared mesh.bin"
    old_buffer.rename(old_buffer.with_name("Shared #mesh.bin"))
    model["files"][1]["uri"] = "Shared%20%23mesh.bin"
    nested_files = copy.deepcopy(model["files"])
    nested_files[0]["uri"] = "./Sample%20panel.gltf"
    for file in nested_files[1:]:
        file["uri"] = "../" + file["uri"]
    for alias in nested_files[0]["aliases"]:
        alias["alias"] = "./" + alias["alias"]
    write_json(primary.parent / "parts" / "Nested source.gltf", {
        "asset": {"version": "2.1", "minVersion": "2.1"},
        "files": nested_files, "externalAssets": [{"file": 0}],
        "nodes": [{"name": NESTED_CARRIER, "externalAsset": 0, "translation": [.25, 0, 0]}],
        "scenes": [{"nodes": [0]}], "scene": 0,
    })
    model["externalAssets"].append({"file": len(model["files"])})
    model["files"].append({"uri": "parts/Nested%20source.gltf", "mimeType": "model/gltf+json"})
    model["nodes"][1].update(externalAsset=1, translation=[.5, 0, 0])
    write_json(primary, model)

    # Use separate meshes with literal world positions and an embedded texture
    # so the control shares neither transform composition nor alias resolution.
    asset = Asset()
    normals = asset.accessor("VEC3", [[0, 0, 1]] * 4)
    uv = asset.accessor("VEC2", [[0, 0], [1, 0], [1, 1], [0, 1]])
    left = mesh(asset, asset.accessor("VEC3", [[-1.05, -.28, 0], [-.45, -.28, 0],
                                               [-.45, .28, 0], [-1.05, .28, 0]]), normals, uv)
    right = copy.deepcopy(left)
    right["attributes"]["POSITION"] = asset.accessor("VEC3", [[.45, -.28, 0], [1.05, -.28, 0],
                                                              [1.05, .28, 0], [.45, .28, 0]])
    asset.model["meshes"].append({"primitives": [right]})
    asset.model["nodes"] = [{"name": f"Placement {i}", "mesh": i} for i in range(2)]
    asset.model["scenes"][0]["nodes"] = [0, 1]
    textured_material(asset)
    asset.write(baked)

    sequence = output / "nested-selection.json"
    events = []
    for frame, pixel in zip((3, 6), NESTED_PICK_PIXELS, strict=True):
        events.extend(({"frame": frame - 2, "viewportPointer": {"action": "select", "pixel": pixel}},
                       {"frame": frame, "depthProbe": pixel}))
    write_json(sequence, {"schemaVersion": 1, "frames": 6, "sampleFrames": [3, 6], "events": events})
    return primary, baked, sequence


def verify_nested_picks(rows, *, linked, baked):
    hits = []
    for panel, (row, pixel) in enumerate(zip(rows, NESTED_PICK_PIXELS, strict=True)):
        state = row["state"]
        geometry = state["gltfInstancing"]
        assert (geometry["drawCount"], geometry["sourceVertexCount"], geometry["sourceIndexCount"]) == (
            (2, 8, 12) if baked else (2, 4, 6)), geometry
        tables = state["gltfMerge"]["sourceTables"]
        assert tables["meshes"] == (2 if baked else 1), tables
        assert all(tables[kind] == 1 for kind in ("buffers", "images", "textures", "samplers")), tables
        if linked:
            assert tables["materials"] == 1, tables

        probe = state["depthProbe"]
        assert probe["pixel"] == pixel and probe["opaque"] > 0, probe
        interaction = probe["sceneInteraction"]
        raw_id = interaction["rawGpuId"]
        assert type(raw_id) is int and raw_id > 0 and interaction["gpu"] is not None, interaction
        hit = interaction["gpu"]["hit"]
        assert hit is not None and hit == interaction["cpu"]["hit"], interaction
        assert hit == interaction["gpu"]["selection"] == interaction["cpu"]["selection"], interaction
        assert state["gui"]["selectedMeshNodeId"] == hit["graphNode"], state["gui"]
        assert hit["sourceNode"] >= 0 and hit["sourcePrimitive"] == 0 and hit["sourceInstance"] == -1, hit

        flags = state["gltfNodeFlags"]
        assert not flags["truncated"], flags
        nodes = {node["graphNode"]: node for node in flags["nodes"]}
        assert nodes[hit["graphNode"]]["linkedReadOnly"] is linked, hit
        ancestry = []
        current = hit["graphNode"]
        while current != -1:
            node = nodes[current]
            ancestry.append(node["name"])
            current = node["parentGraphNode"]
        assert f"Placement {panel}" in ancestry and f"Placement {1 - panel}" not in ancestry, ancestry
        assert (NESTED_CARRIER in ancestry) == (panel == 1 and not baked), ancestry

        bindings = state["gltfMaterialBindings"]
        assert not bindings["drawsTruncated"] and not bindings["materialsTruncated"], bindings
        draw, = (draw for draw in bindings["draws"] if draw["graphNode"] == hit["graphNode"])
        material, = (material for material in bindings["materials"]
                     if material["runtimeMaterial"] == draw["runtimeMaterial"])
        assert material["active"] and material["authored"]["pbrMetallicRoughness"]["baseColorTexture"]["index"] == 0, material
        hits.append({**hit, "pixel": pixel, "rawGpuId": raw_id, "ancestry": ancestry,
                     "runtimeMaterial": draw["runtimeMaterial"], "firstIndex": draw["firstIndex"]})
    for key in ("rawGpuId", "graphNode", "sourceNode"):
        assert hits[0][key] != hits[1][key], hits
    for key in ("sourceMesh", "firstIndex"):
        assert (hits[0][key] == hits[1][key]) == (not baked), hits
    assert hits[0]["runtimeMaterial"] == hits[1]["runtimeMaterial"], hits
    return hits


def run_nested_linked(renderer, output, records, common):
    primary, baked, sequence = nested_fixture(output / "nested")
    companion = primary.with_name("nested-companion.gltf")
    techniques = ("deferred-raster", "forward-raster")
    portable = {technique: output / f"nested-portable.{suffix}"
                for technique, suffix in zip(techniques, ("gltf", "glb"), strict=True)}
    input_hashes = {path.relative_to(primary.parent): hashlib.sha256(path.read_bytes()).hexdigest()
                    for path in primary.parent.rglob("*") if path.is_file()}
    references, checks = {}, []
    options = (*common, "--no-ray-query")
    linked_options = (*options, "--experimental-gltf-linked-assets")

    def capture(source, technique, case, *, save=None, extra=options, linked=False):
        rows = run(renderer, output, source, f"nested-{technique}-{case}", technique, "lit", records,
                   sequence=sequence, save=save, extra=extra, require_validation=True)
        hits = verify_nested_picks(rows, linked=linked, baked=case == "baked")
        frames = [pixels(Path(row["image"])) for row in rows]
        if case == "baked":
            for frame, (x, y) in zip(frames, NESTED_PICK_PIXELS, strict=True):
                region = frame[y - 16:y + 16, x - 18:x + 18]
                assert len(np.unique(region.reshape(-1, 3), axis=0)) > 2, "Aliased texture control is uniform"
            references[technique] = frames
        else:
            for actual, expected in zip(frames, references[technique], strict=True):
                np.testing.assert_array_equal(actual, expected)
        checks.extend({"technique": technique, "case": case, **hit} for hit in hits)

    for technique in techniques:
        capture(baked, technique, "baked")
        write_companion = technique == techniques[0]
        capture(primary, technique, "linked", save=None if write_companion else portable[technique], linked=True,
                extra=(*linked_options, "--save-linked-scene", str(companion)) if write_companion else linked_options)
        assert json.loads(companion.read_text(encoding="utf-8")) == json.loads(primary.read_text(encoding="utf-8"))
        capture(companion, technique, "companion", save=portable[technique] if write_companion else None,
                extra=linked_options, linked=True)
        model, _ = load_asset(portable[technique])
        assert model["asset"]["version"] == "2.0" and "files" not in model and "externalAssets" not in model
        assert len(model["meshes"]) == len(model["textures"]) == len(model["images"]) == 1

    offline = primary.parent.with_name("nested-inputs-offline")
    primary.parent.rename(offline)
    assert not primary.exists() and not companion.exists()
    assert all(hashlib.sha256((offline / path).read_bytes()).hexdigest() == digest
               for path, digest in input_hashes.items())
    for technique in techniques:
        capture(portable[technique], technique, "source-free")
    assert len(checks) == 16, checks
    return checks


def verify_portable_xmp(model):
    packets = model["extensions"][XMP]["packets"]
    assert [entry["xmp:Label"] for entry in packets] == [
        "Primary asset", "First placement", "Linked asset", "Linked scene", "Linked mesh",
    ]
    assert packets[2]["xmp:Identifier"] == LARGE_IDENTIFIER
    assert model["asset"]["extensions"][XMP]["packet"] == 0
    assert model["meshes"][0]["extensions"][XMP]["packet"] == 4
    placement = next(node for node in model["nodes"]
                     if node.get("name") in ("Placement 0", "Edited placement"))
    assert placement["extensions"][XMP]["packet"] == 1
    origins = [(node, node.get("extras", {}).get("containerLinkedSource"))
               for node in model["nodes"]]
    assert sum(origin is not None and origin["node"] >= 0 for _, origin in origins) == 2
    metadata = sorted((origin["scope"], node["extensions"][XMP]["packet"])
                      for node, origin in origins if origin is not None and origin["node"] == -1)
    assert metadata == [("asset", 2), ("asset", 2), ("scene", 3), ("scene", 3)]


def animated_fixture(output):
    folder = output / "animated-inputs"
    folder.mkdir()

    def panel(positions):
        asset = Asset()
        primitive = mesh(asset, asset.accessor("VEC3", positions),
                         asset.accessor("VEC3", [[0, 0, 1]] * 4),
                         asset.accessor("VEC2", [[0, 0], [1, 0], [1, 1], [0, 1]]))
        textured_material(asset)
        return asset, primitive

    asset, primitive = panel(BIND_POSITIONS)
    primitive["attributes"]["JOINTS_0"] = asset.accessor("VEC4", [[0, 0, 0, 0]] * 4, 5121)
    primitive["attributes"]["WEIGHTS_0"] = asset.accessor("VEC4", [[1, 0, 0, 0]] * 4)
    primitive["targets"] = [{"POSITION": asset.accessor("VEC3", [[.25, 0, 0]] * 4)}]
    asset.model["meshes"][0]["weights"] = [0]
    asset.model["nodes"] = [
        {"name": "Child root", "children": [1, 2]},
        {"name": "Skinned panel", "mesh": 0, "skin": 0, "translation": [.125, 0, 0]},
        {"name": "Skeleton root", "translation": [0, .125, 0], "children": [3]},
        {"name": "Animated joint"},
    ]
    inverse_bind = np.eye(4)
    inverse_bind[1, 3] = -.125
    asset.model["skins"] = [{"skeleton": 2, "joints": [3],
                            "inverseBindMatrices": asset.accessor("MAT4", inverse_bind.T)}]
    times = asset.accessor("SCALAR", [0, 1])
    asset.model["animations"] = [{
        "name": "Child pose",
        "samplers": [
            {"input": times, "output": asset.accessor("VEC3", [[0, 0, 0], [0, .5, 0]])},
            {"input": times, "output": asset.accessor("SCALAR", [0, 1])},
        ],
        "channels": [
            {"sampler": 0, "target": {"node": 3, "path": "translation"}},
            {"sampler": 1, "target": {"node": 1, "path": "weights"}},
        ],
    }]
    asset.write(folder / "Animated panel.gltf")
    placements = [{"name": f"Animated placement {i}", "translation": translation, "scale": scale}
                  for i, (translation, scale) in enumerate(zip(PLACEMENT_TRANSLATIONS, PLACEMENT_SCALES))]
    primary = folder / "linked.gltf"
    write_json(primary, {
        "asset": {"version": "2.1", "minVersion": "2.1"},
        "files": [{"uri": "Animated%20panel.gltf", "mimeType": "model/gltf+json"}],
        "externalAssets": [{"file": 0}],
        "nodes": [{**placement, "externalAsset": 0} for placement in placements],
        "scenes": [{"nodes": [0, 1]}], "scene": 0,
    })
    controls = []
    for i, time in enumerate(POSE_TIMES):
        # The bind offset cancels the skeleton root translation. Morph adds
        # X and the animated joint adds Y before the parent placement.
        baked, _ = panel(BIND_POSITIONS + [time * .25, time * .5, 0])
        baked.model["nodes"] = [{**placement, "mesh": 0} for placement in placements]
        baked.model["scenes"][0]["nodes"] = [0, 1]
        controls.append(output / f"animated-baked-pose-{i}.gltf")
        baked.write(controls[-1])
    sequence = output / "linked-pose-sequence.json"
    write_json(sequence, {"schemaVersion": 1, "frames": 16, "sampleFrames": [8, 16], "events": [
        {"frame": 1, "animationClip": 0, "animationTime": POSE_TIMES[0], "animationPlaying": False},
        {"frame": 9, "animationTime": POSE_TIMES[1]},
    ]})
    return primary, controls, sequence


def verify_animated_pose(state, time):
    animation = state["gltfAnimation"]
    assert animation["clipCount"] == 1 and animation["selectedClip"] == 0 and not animation["playing"], animation
    assert animation["diagnostics"] == 0 and abs(animation["time"] - time) < 1e-6, animation
    deformed = state["gltfDeformation"]
    assert deformed["active"] and deformed["gpuReady"] and deformed["currentBoundToGpu"], deformed
    # The shared source quad has four vertices. Its missing authored tangents
    # require one output vertex per triangle corner in each deformed range.
    assert deformed["sourceVertexCount"] == 4 and deformed["expandedVertexCount"] == 16, deformed
    assert deformed["expandedIndexCount"] == 18 and deformed["instanceCount"] == 2, deformed
    instances = sorted(deformed["instances"], key=lambda instance: instance["modelColumns"][3][0])
    assert len(instances) == len({instance["sourceNodeIndex"] for instance in instances}) == 2, instances
    assert sorted(instance["vertexOffset"] for instance in instances) == [4, 10], instances
    instancing = state["gltfInstancing"]
    assert instancing["drawCount"] == 2 and instancing["sourceIndexCount"] == 6, instancing
    local = BIND_POSITIONS + [time * .25 - .125, time * .5, 0]
    for instance, translation, scale in zip(instances, PLACEMENT_TRANSLATIONS, PLACEMENT_SCALES):
        assert instance["boundsValid"] and instance["vertexCount"] == instance["indexCount"] == 6, instance
        assert instance["pick"]["hit"] and instance["pick"]["hitNode"] == instance["graphNode"], instance
        model = np.diag([*scale, 1.])
        model[:3, 3] = np.array(translation) + [.125, 0, 0]
        for key, expected in (("modelColumns", model.T), ("firstPosition", local[0]),
                              ("boundsMin", local.min(axis=0)), ("boundsMax", local.max(axis=0)),
                              ("weights", [time])):
            np.testing.assert_allclose(instance[key], expected, atol=2e-5, rtol=0, err_msg=key)


def run_animated_linked(renderer, output, records, common):
    primary, controls, sequence = animated_fixture(output)
    saved = output / "animated-portable.glb"
    techniques = ("deferred-raster", "forward-raster")
    linked_options = (*common, "--experimental-gltf-linked-assets")
    references = {}
    for technique in techniques:
        original = run(renderer, output, primary, f"animated-linked-{technique}", technique, "lit", records,
                       sequence=sequence, save=saved if technique == techniques[0] else None,
                       extra=linked_options)
        assert len(original) == len(POSE_TIMES), original
        expected = []
        for i, (record, control, time) in enumerate(zip(original, controls, POSE_TIMES)):
            baked = run(renderer, output, control, f"animated-control-{technique}-{i}",
                        technique, "lit", records, extra=common)[0]
            verify_animated_pose(record["state"], time)
            expected.append(pixels(Path(baked["image"])))
            np.testing.assert_array_equal(pixels(Path(record["image"])), expected[-1])
        assert not np.array_equal(*expected), "Both linked poses rendered identically"
        references[technique] = expected

    model, _ = load_asset(saved)
    assert model["asset"]["version"] == "2.0" and "files" not in model and "externalAssets" not in model
    skins = model["skins"]
    owners = {i: node for i, node in enumerate(model["nodes"]) if "skin" in node}
    assert len(owners) == len(skins) == 2
    assert len({node["mesh"] for node in owners.values()}) == 1
    assert len({skin["inverseBindMatrices"] for skin in skins}) == 1
    joints = {joint for skin in skins for joint in skin["joints"]}
    assert len(joints) == 2
    animation, = model["animations"]
    for path, targets in (("translation", joints), ("weights", set(owners))):
        channels = [channel for channel in animation["channels"] if channel["target"]["path"] == path]
        assert len(channels) == 2 and {channel["target"]["node"] for channel in channels} == targets
        assert len({animation["samplers"][channel["sampler"]]["output"] for channel in channels}) == 1

    companion = primary.with_name("animated-companion.gltf")
    original_manifest = primary.read_bytes()
    technique = techniques[0]
    unposed = run(renderer, output, primary, "animated-companion-save", technique, "lit", records,
                  extra=(*linked_options, "--save-linked-scene", str(companion)))[0]
    assert unposed["state"]["gltfAnimation"]["selectedClip"] == -1
    assert primary.read_bytes() == original_manifest
    assert json.loads(companion.read_text(encoding="utf-8")) == json.loads(original_manifest)
    endpoint = POSE_TIMES[-1]
    restored = run(renderer, output, companion, "animated-companion-reload", technique, "lit", records,
                   extra=(*linked_options, "--animation-clip", "0", "--animation-time", str(endpoint),
                          "--animation-paused"))[0]
    verify_animated_pose(restored["state"], endpoint)
    np.testing.assert_array_equal(pixels(Path(restored["image"])), references[technique][-1])

    primary.parent.rename(output / "animated-original-inputs-removed")
    for technique in techniques:
        restored = run(renderer, output, saved, f"animated-reloaded-{technique}", technique, "lit", records,
                       sequence=sequence, extra=common)
        assert len(restored) == len(POSE_TIMES), restored
        for record, expected, time in zip(restored, references[technique], POSE_TIMES):
            verify_animated_pose(record["state"], time)
            np.testing.assert_array_equal(pixels(Path(record["image"])), expected)


EDITABLE_PLACEMENTS = ("A left", "A right", "B middle")
EDITABLE_PAYLOADS = ("Edited A left", "Edited A right", "Panel B")
EDITED_A_COLOR = [.13, .44, .93, 1.]
READ_ONLY_DIAGNOSTIC = "linked asset is read-only"


def editable_fixture(output, *, animated=False):
    folder = output / ("editable-animated-inputs" if animated else "editable-linked-inputs")
    folder.mkdir()
    for label, color in (("A", [.92, .1, .05, 1.]), ("B", [.05, .85, .1, 1.])):
        asset = Asset()
        mesh(asset, asset.accessor("VEC3", BIND_POSITIONS), asset.accessor("VEC3", [[0, 0, 1]] * 4),
             asset.accessor("VEC2", [[0, 0], [1, 0], [1, 1], [0, 1]]))
        asset.model["nodes"][0]["name"] = f"Panel {label}"
        asset.model["extensionsUsed"] = ["KHR_materials_unlit"]
        asset.model["materials"] = [{"name": f"Paint {label}", "extensions": {"KHR_materials_unlit": {}},
                                     "pbrMetallicRoughness": {"baseColorFactor": color}}]
        if animated:
            shift = .5 if label == "A" else -.5
            asset.model["animations"] = [{"name": f"Panel {label} shift", "samplers": [
                {"input": asset.accessor("SCALAR", [0, 1]),
                 "output": asset.accessor("VEC3", [[0, 0, 0], [0, shift, 0]])}],
                "channels": [{"sampler": 0, "target": {"node": 0, "path": "translation"}}]}]
        path = folder / f"Panel {label}.gltf"
        asset.write(path)
        if label == "B":
            # The remaining link owns a real alias dependency for companion save/reload tests.
            (folder / "Panel B.bin").write_bytes(asset.data)
            asset.model["buffers"][0]["uri"] = "virtual%20panel.bin"
            write_json(path, asset.model)
    manifest = folder / "scene.gltf"
    write_json(manifest, {
        "asset": {"version": "2.1", "minVersion": "2.1"},
        "files": [{"uri": "Panel%20A.gltf", "mimeType": "model/gltf+json"},
                  {"uri": "Panel%20B.gltf", "mimeType": "model/gltf+json",
                   "aliases": [{"alias": "virtual%20panel.bin", "file": 2}]},
                  {"uri": "Panel%20B.bin", "mimeType": "application/octet-stream"}],
        "externalAssets": [{"file": 0}, {"file": 1}],
        "nodes": [{"name": name, "externalAsset": int(index == 2), "translation": [x, 0, 0]}
                  for index, (name, x) in enumerate(zip(EDITABLE_PLACEMENTS, (-1.4, 1.4, 0)))],
        "scenes": [{"nodes": [0, 1, 2]}], "scene": 0,
    })
    return manifest


def linked_panel_nodes(state, placements, names, locks):
    instancing = state["gltfInstancing"]
    assert instancing["drawCount"] == 3 and instancing["sourceVertexCount"] == 8, instancing
    assert instancing["sourceIndexCount"] == 12, instancing
    all_nodes = {node["graphNode"]: node for node in state["gltfNodeFlags"]["nodes"]}
    nodes = [node for node in all_nodes.values() if node["active"] and node["sourceIndex"] >= 0]
    probes = state["gltfNodeFlags"]["probes"]

    def below(graph_node, ancestor):
        while graph_node != -1:
            if graph_node == ancestor:
                return True
            graph_node = all_nodes[graph_node]["parentGraphNode"]
        return False

    result = []
    for placement, name, locked in zip(placements, names, locks):
        carrier, = (node for node in nodes if node["name"] == placement)
        payload, = (node for node in nodes if node["name"] == name and node["parentGraphNode"] == carrier["graphNode"])
        assert carrier["linkedReadOnly"] is False and payload["linkedReadOnly"] is locked, (carrier, payload)
        probe, = (probe for probe in probes if below(probe["graphNode"], payload["graphNode"]))
        assert probe["projected"] and probe["hit"] and probe["nearestHitGraphNode"] == probe["graphNode"], probe
        result.append(payload["sourceIndex"])
    return result


def node_authoring(frame, action, source, name=None):
    request = {"action": action, "sourceNode": source}
    if name is not None:
        request["name"] = name
    return {"frame": frame, "nodeAuthoring": request}


def node_transform(frame, source, y):
    return {"frame": frame, "nodeTransform": {"node": {"sourceNode": source},
                                              "matrix": [1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, y, 0, 1]}}


def paint_edit(frame, source):
    return {"frame": frame, "materialEdit": {"sourceMaterial": source, "values": [
        {"property": "pbrMetallicRoughness/baseColorFactor", "value": EDITED_A_COLOR}]}}


def expect_linked_rejection(renderer, output, primary, prefix, records, options, events):
    sequence = output / f"{prefix}.json"
    write_json(sequence, {"schemaVersion": 1, "frames": 5, "sampleFrames": [5], "events": events})
    try:
        run(renderer, output, primary, prefix, "deferred-raster", "lit", records, sequence=sequence, extra=options)
    except RuntimeError:
        log = (output / f"{prefix}.log").read_text(encoding="utf-8", errors="replace")
        assert READ_ONLY_DIAGNOSTIC in log and "Make Editable" in log, log
    else:
        raise AssertionError(f"{prefix} accepted an edit to linked content")


def run_editable_merge(renderer, output, linked, records, options, incoming_count, left_payload):
    primary = output / "editable-merge-primary.gltf"
    ordinary = json.loads((linked.parent / "Panel A.gltf").read_text(encoding="utf-8"))
    ordinary["nodes"][0].update(name="Ordinary primary panel", translation=[0, 1.05, 0])
    write_json(primary, ordinary)

    # Import/Merge appends the canonical incoming nodes, a placement parent,
    # then the complete selected-scene clone in incoming source order.
    parent_source = len(ordinary["nodes"]) + incoming_count
    placed_left = parent_source + 1 + left_payload
    parent_name = "Imported linked panels"
    sequence = output / "editable-merge-sequence.json"
    write_json(sequence, {"schemaVersion": 1, "frames": 10, "sampleFrames": [2, 4, 6, 8, 10], "events": [
        {"frame": 3, "merge": {"path": str(linked), "parentName": parent_name}, "guiWaitForLoad": True},
        {"frame": 5, "sceneHistory": "undo"},
        {"frame": 7, "sceneHistory": "redo"},
        node_authoring(9, "make-editable", placed_left),
    ]})
    captures = run(renderer, output, primary, "editable-merge", "deferred-raster", "lit", records,
                   sequence=sequence, extra=options)
    assert len(captures) == 5, captures
    states = [capture["state"] for capture in captures]
    baseline = states[0]["gltfMerge"]
    committed = states[1]["gltfMerge"]
    assert committed["outcome"] == "committed" and committed["parentName"] == parent_name, committed
    assert committed["sourceReadCount"] == baseline["sourceReadCount"] + 1, committed
    expectations = ((None, (0, 0)), ((True, True, True), (1, 0)), (None, (0, 1)),
                    ((True, True, True), (1, 0)), ((False, False, True), (0, 0)))
    for state, (locks, cursor) in zip(states, expectations):
        merge, flags = state["gltfMerge"], state["gltfNodeFlags"]
        assert not flags["truncated"] and merge["graphIdentity"] == baseline["graphIdentity"], merge
        history = state["gltfMaterialVariants"]
        assert (history.get("undoCount", 0), history.get("redoCount", 0)) == cursor, history
        nodes = [node for node in flags["nodes"] if node["sourceIndex"] >= 0]
        retained, = (node for node in nodes if node["name"] == "Ordinary primary panel")
        assert retained["sourceIndex"] == 0 and retained["active"] and not retained["linkedReadOnly"], retained
        if locks is None:
            assert nodes == [retained] and merge["sourceTables"] == baseline["sourceTables"], merge
            assert state["gltfScenes"]["activeRenderableCount"] == 1
            continue

        parent, = (node for node in nodes if node["name"] == parent_name)
        assert parent["sourceIndex"] == parent_source and parent["active"] and not parent["linkedReadOnly"], parent
        assert merge["sourceTables"]["nodes"] == parent_source + 1 + incoming_count, merge
        assert state["gltfScenes"]["activeRenderableCount"] == 4
        assert merge["sourceReadCount"] == committed["sourceReadCount"], merge
        for name, panel, locked in zip(EDITABLE_PLACEMENTS, ("Panel A", "Panel A", "Panel B"), locks):
            canonical, = (node for node in nodes if node["name"] == name and not node["active"])
            placed, = (node for node in nodes if node["name"] == name and node["active"])
            assert canonical["parentGraphNode"] != parent["graphNode"], canonical
            assert placed["parentGraphNode"] == parent["graphNode"], placed
            for carrier in (canonical, placed):
                assert not carrier["linkedReadOnly"], carrier
                payload, = (node for node in nodes if node["name"] == panel and
                            node["parentGraphNode"] == carrier["graphNode"])
                assert payload["active"] is carrier["active"] and payload["linkedReadOnly"] is locked, payload
                if name == EDITABLE_PLACEMENTS[0] and carrier["active"]:
                    assert payload["sourceIndex"] == placed_left, payload

    before, merged, undone, redone, detached = (pixels(Path(capture["image"])) for capture in captures)
    assert not np.array_equal(before, merged), "Linked Import/Merge produced no visible panels"
    np.testing.assert_array_equal(before, undone)
    np.testing.assert_array_equal(merged, redone)
    np.testing.assert_array_equal(redone, detached)


def payload_mesh(model, name):
    pending = [node for node in model["nodes"] if node.get("name") == name]
    assert len(pending) == 1, name
    meshes = []
    while pending:
        node = pending.pop()
        if "mesh" in node:
            meshes.append(node["mesh"])
        pending.extend(model["nodes"][child] for child in node.get("children", []))
    mesh_index, = meshes
    return mesh_index


def verify_edited_a_material(model):
    a_mesh = payload_mesh(model, EDITABLE_PAYLOADS[0])
    assert payload_mesh(model, EDITABLE_PAYLOADS[1]) == a_mesh
    material = model["materials"][model["meshes"][a_mesh]["primitives"][0]["material"]]
    np.testing.assert_allclose(material["pbrMetallicRoughness"]["baseColorFactor"], EDITED_A_COLOR, rtol=0, atol=1e-6)
    return a_mesh


def verify_hybrid_companion(renderer, output, primary, companion, sequence, records, options, placements, references):
    model = json.loads(companion.read_text(encoding="utf-8"))
    assert model["asset"]["version"] == "2.1" and model["asset"]["minVersion"] == "2.1", model["asset"]
    verify_edited_a_material(model)
    for name in placements[:2]:
        carrier, = (node for node in model["nodes"] if node.get("name") == name)
        assert "externalAsset" not in carrier and carrier.get("children"), carrier
    retained, = (node for node in model["nodes"] if node.get("name") == placements[2])
    assert "mesh" not in retained and not retained.get("children"), retained
    assert not any(node.get("name") == "Panel B" for node in model["nodes"]), model["nodes"]
    assert len(model["externalAssets"]) == 1 and len(model["files"]) == 2, model["files"]
    b_file = model["files"][model["externalAssets"][retained["externalAsset"]]["file"]]
    alias, = b_file["aliases"]
    assert b_file["uri"] == "Panel%20B.gltf" and alias["alias"] == "virtual%20panel.bin", b_file
    assert model["files"][alias["file"]]["uri"] == "Panel%20B.bin", model["files"]

    b_source = primary.parent / "Panel B.gltf"
    original_b = b_source.read_bytes()
    prefix = "editable-hybrid-protect-b"
    try:
        run(renderer, output, primary, prefix, "deferred-raster", "lit", records, sequence=sequence,
            extra=(*options, "--save-linked-scene", str(b_source)))
    except RuntimeError:
        log = (output / f"{prefix}.log").read_text(encoding="utf-8", errors="replace")
        assert "cannot replace a linked input file" in log, log
    else:
        raise AssertionError("Hybrid save replaced its retained linked source")
    assert b_source.read_bytes() == original_b

    (primary.parent / "Panel A.gltf").rename(output / "editable-detached-A.gltf")
    for technique, expected in references.items():
        restored = run(renderer, output, companion, f"editable-hybrid-reloaded-{technique}", technique, "lit", records,
                       extra=options)[0]
        linked_panel_nodes(restored["state"], placements, EDITABLE_PAYLOADS, (False, False, True))
        np.testing.assert_array_equal(pixels(Path(restored["image"])), expected)

    b_buffer = primary.parent / "Panel B.bin"
    absent = output / "editable-retained-B.bin"
    companion_bytes = companion.read_bytes()
    b_buffer.rename(absent)
    try:
        prefix = "editable-hybrid-missing-b"
        try:
            run(renderer, output, companion, prefix, "deferred-raster", "lit", records, extra=options)
        except RuntimeError:
            log = (output / f"{prefix}.log").read_text(encoding="utf-8", errors="replace")
            assert "missing linked file 'Panel B.bin'" in log, log
        else:
            raise AssertionError("Hybrid reload ignored the missing retained B alias target")
    finally:
        absent.rename(b_buffer)
    assert companion.read_bytes() == companion_bytes


def run_hybrid_animation(renderer, output, records, options):
    primary = editable_fixture(output, animated=True)
    companion = primary.with_name("animated-companion.gltf")
    pose_events = [{"frame": 5, "animationClip": 0, "animationTime": .75, "animationPlaying": False},
                   {"frame": 7, "animationClip": 1, "animationTime": .75},
                   {"frame": 9, "animationClip": -1}]
    sequence = {"schemaVersion": 1, "frames": 10, "sampleFrames": [4, 6, 8, 10]}
    save_sequence = output / "editable-hybrid-animation-save.json"
    write_json(save_sequence, {**sequence, "events": [node_authoring(2, "make-editable", 0), *pose_events]})
    live = run(renderer, output, primary, "editable-hybrid-animation-save", "deferred-raster", "lit", records,
               sequence=save_sequence, extra=(*options, "--save-linked-scene", str(companion)))

    model = json.loads(companion.read_text(encoding="utf-8"))
    clip, = model["animations"]
    assert clip["name"] == "Panel A shift", clip
    targets = [channel["target"] for channel in clip["channels"]]
    assert len(targets) == len({target["node"] for target in targets}) == 2, targets
    assert all(target["path"] == "translation" and model["nodes"][target["node"]]["name"] == "Panel A"
               for target in targets), targets

    (primary.parent / "Panel A.gltf").rename(output / "editable-detached-animated-A.gltf")
    reload_sequence = output / "editable-hybrid-animation-reload.json"
    write_json(reload_sequence, {**sequence, "events": pose_events})
    restored = run(renderer, output, companion, "editable-hybrid-animation-reload", "deferred-raster", "lit", records,
                   sequence=reload_sequence, extra=options)
    for captures in (live, restored):
        assert len(captures) == 4, captures
        for capture, selected in zip(captures, (-1, 0, 1, -1)):
            state = capture["state"]
            left, right, middle = linked_panel_nodes(state, EDITABLE_PLACEMENTS, ("Panel A", "Panel A", "Panel B"),
                                                     (False, False, True))
            animation = state["gltfAnimation"]
            assert animation["clipCount"] == 2 and animation["selectedClip"] == selected, animation
            assert animation["diagnostics"] == 0 and not animation["playing"], animation
            nodes = {node["sourceIndex"]: node for node in animation["nodes"]}
            expected = ({left, right} if selected == 0 else {middle} if selected == 1 else set())
            assert set(nodes) == expected, animation
            for node in nodes.values():
                assert node["worldMatrix"][13] == (.375 if selected == 0 else -.375), node

    originals = [pixels(Path(capture["image"])) for capture in live]
    assert not np.array_equal(originals[0], originals[1]), "Detached A clip did not move the panels"
    assert not np.array_equal(originals[0], originals[2]), "Retained B clip did not move the panel"
    np.testing.assert_array_equal(originals[0], originals[3])
    for capture, expected in zip(restored, originals):
        np.testing.assert_array_equal(pixels(Path(capture["image"])), expected)


def run_editable_linked(renderer, output, records, common):
    primary = editable_fixture(output)
    inputs = {path: hashlib.sha256(path.read_bytes()).hexdigest() for path in primary.parent.iterdir()}
    linked_options = (*common, "--experimental-gltf-linked-assets")
    techniques = ("deferred-raster", "forward-raster")
    baseline = run(renderer, output, primary, "editable-baseline", techniques[0], "lit", records,
                   extra=linked_options)[0]
    original_names = ("Panel A", "Panel A", "Panel B")
    left, right, middle = linked_panel_nodes(baseline["state"], EDITABLE_PLACEMENTS, original_names, (True, True, True))
    detach = node_authoring(4, "make-editable", left)
    for prefix, events in (
        ("editable-deny-linked-rename", [node_authoring(2, "rename", left, "Denied")]),
        ("editable-deny-linked-transform", [node_transform(2, right, .35)]),
        ("editable-deny-linked-material", [paint_edit(2, 0)]),
        ("editable-deny-independent-source", [detach, node_authoring(5, "rename", middle, "Denied")]),
    ):
        expect_linked_rejection(renderer, output, primary, prefix, records, linked_options, events)

    portable = output / "editable-portable.glb"
    companion = primary.with_name("editable-companion.gltf")
    sequence = output / "editable-sequence.json"
    edited_placements = (*EDITABLE_PLACEMENTS[:2], "Edited B placement")
    write_json(sequence, {"schemaVersion": 1, "frames": 8, "sampleFrames": [2, 3, 4, 6, 7, 8], "events": [
        node_authoring(3, "rename", 2, edited_placements[2]),
        detach,
        node_authoring(5, "rename", left, EDITABLE_PAYLOADS[0]),
        node_authoring(6, "rename", right, EDITABLE_PAYLOADS[1]),
        node_transform(7, left, .375),
        paint_edit(8, 0),
    ]})
    references = {}
    for technique in techniques:
        save_portable = technique == techniques[0]
        captures = run(renderer, output, primary, f"editable-live-{technique}", technique, "lit", records,
                       sequence=sequence, save=portable if save_portable else None,
                       extra=linked_options if save_portable else (*linked_options, "--save-linked-scene", str(companion)))
        snapshots = (
            (EDITABLE_PLACEMENTS, original_names, (True, True, True), 0),
            (edited_placements, original_names, (True, True, True), 1),
            (edited_placements, original_names, (False, False, True), 0),
            (edited_placements, EDITABLE_PAYLOADS, (False, False, True), 2),
            (edited_placements, EDITABLE_PAYLOADS, (False, False, True), 3),
            (edited_placements, EDITABLE_PAYLOADS, (False, False, True), 4),
        )
        assert len(captures) == len(snapshots), captures
        for capture, (placements, names, locks, undo) in zip(captures, snapshots):
            state = capture["state"]
            assert linked_panel_nodes(state, placements, names, locks) == [left, right, middle], state
            history = state["gltfMaterialVariants"]
            assert (history.get("undoCount", 0), history.get("redoCount", 0)) == (undo, 0), history

        images = [pixels(Path(capture["image"])) for capture in captures]
        for unchanged in images[1:4]:
            np.testing.assert_array_equal(unchanged, images[0])
        left_end, right_start = images[0].shape[1] * 3 // 8, images[0].shape[1] * 5 // 8
        assert not np.array_equal(images[3][:, :left_end], images[4][:, :left_end]), "A left did not move"
        np.testing.assert_array_equal(images[3][:, left_end:], images[4][:, left_end:])
        assert not np.array_equal(images[4][:, :left_end], images[5][:, :left_end]), "A left paint did not change"
        assert not np.array_equal(images[4][:, right_start:], images[5][:, right_start:]), "A right did not share the paint"
        np.testing.assert_array_equal(images[4][:, left_end:right_start], images[5][:, left_end:right_start])
        references[technique] = images[-1]

    run_editable_merge(renderer, output, primary, records, linked_options,
                       baseline["state"]["gltfMerge"]["sourceTables"]["nodes"], left)
    assert all(hashlib.sha256(path.read_bytes()).hexdigest() == digest for path, digest in inputs.items())
    model, _ = load_asset(portable)
    assert model["asset"]["version"] == "2.0" and "files" not in model and "externalAssets" not in model
    a_mesh = verify_edited_a_material(model)
    assert payload_mesh(model, EDITABLE_PAYLOADS[2]) != a_mesh
    verify_hybrid_companion(renderer, output, primary, companion, sequence, records, linked_options,
                            edited_placements, references)
    primary.parent.rename(output / "editable-original-inputs-removed")
    for technique in techniques:
        restored = run(renderer, output, portable, f"editable-portable-reloaded-{technique}", technique, "lit", records,
                       extra=common)[0]
        linked_panel_nodes(restored["state"], edited_placements, EDITABLE_PAYLOADS, (False, False, False))
        np.testing.assert_array_equal(pixels(Path(restored["image"])), references[technique])
    run_hybrid_animation(renderer, output, records, linked_options)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--renderer", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    only = parser.add_mutually_exclusive_group()
    only.add_argument("--animated-only", action="store_true")
    only.add_argument("--editable-only", action="store_true")
    only.add_argument("--nested-only", action="store_true")
    args = parser.parse_args()
    renderer, output = args.renderer.resolve(), args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    renderer_digest = hashlib.sha256(renderer.read_bytes()).hexdigest()
    (output / "vk_layer_settings.txt").write_text("khronos_validation.validate_sync = true\n", encoding="utf-8")
    records = []
    common = ("--import-scale", "1", "--sky", "plain")
    if args.nested_only:
        nested_checks = run_nested_linked(renderer, output, records, common)
        assert hashlib.sha256(renderer.read_bytes()).hexdigest() == renderer_digest
        write_json(output / "results.json", {"passed": True, "rendererSha256": renderer_digest,
                   "captures": records, "nestedChecks": nested_checks})
        print("PASS nested linked geometry, aliases, GPU picks and source-free reloads", flush=True)
        return
    if args.animated_only or args.editable_only:
        regression = run_animated_linked if args.animated_only else run_editable_linked
        regression(renderer, output, records, common)
        assert hashlib.sha256(renderer.read_bytes()).hexdigest() == renderer_digest
        write_json(output / "results.json", {"passed": True, "rendererSha256": renderer_digest, "captures": records})
        print("PASS animated linked placements and companion save" if args.animated_only else
              "PASS linked ownership, Make Editable, shared edits, hybrid and portable reloads", flush=True)
        return
    primary, ordinary = fixture(output)
    saved = output / "portable.glb"
    references = {}
    for technique in ("deferred-raster", "forward-raster"):
        linked = run(renderer, output, primary, f"linked-{technique}", technique, "lit", records,
                     save=saved if technique == "deferred-raster" else None,
                     extra=(*common, "--experimental-gltf-linked-assets"))[0]
        independent = run(renderer, output, ordinary, f"reference-{technique}", technique, "lit", records, extra=common)[0]
        verify_picks(linked["state"])
        references[technique] = pixels(Path(independent["image"]))
        np.testing.assert_array_equal(pixels(Path(linked["image"])), references[technique])

    source_hashes = {path: hashlib.sha256(path.read_bytes()).hexdigest()
                     for path in primary.parent.rglob("*") if path.is_file()}
    linked_saved = primary.with_name("companion.gltf")
    edited_portable = output / "edited-portable.glb"
    edited_portable.write_bytes(b"Unrelated portable output may be replaced")
    sequence = output / "placement-edits.json"
    matrix = [1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, -.75, .375, 0, 1]
    write_json(sequence, {"schemaVersion": 1, "frames": 4, "sampleFrames": [4], "events": [
        {"frame": 2, "nodeAuthoring": {"action": "rename", "sourceNode": 0, "name": "Edited placement"}},
        {"frame": 3, "nodeTransform": {"node": {"sourceNode": 0}, "matrix": matrix}},
    ]})
    authored = json.loads(primary.read_text(encoding="utf-8"))
    linked_options = (*common, "--experimental-gltf-linked-assets")
    edited = {}
    for technique in references:
        keep_links = technique == "deferred-raster"
        control = run(renderer, output, primary, f"edited-{technique}", technique, "lit", records,
                      sequence=sequence, save=None if keep_links else edited_portable,
                      extra=(*linked_options, "--save-linked-scene", str(linked_saved)) if keep_links else linked_options)[0]
        edited[technique] = pixels(Path(control["image"]))
        assert not np.array_equal(edited[technique], references[technique]), "placement edit did not change the capture"
    companion = json.loads(linked_saved.read_text(encoding="utf-8"))
    assert companion["files"] == authored["files"] and companion["externalAssets"] == authored["externalAssets"]
    assert companion["nodes"][0] == {"name": "Edited placement", "externalAsset": 0, "matrix": matrix,
                                      "extensions": {XMP: {"packet": 1}}}
    assert companion["extensions"] == authored["extensions"] and companion["asset"] == authored["asset"]
    assert "meshes" not in companion and "buffers" not in companion
    assert all(hashlib.sha256(path.read_bytes()).hexdigest() == digest for path, digest in source_hashes.items())
    for technique in references:
        restored = run(renderer, output, linked_saved, f"linked-reloaded-{technique}", technique, "lit", records,
                       extra=linked_options)[0]
        verify_picks(restored["state"])
        np.testing.assert_array_equal(pixels(Path(restored["image"])), edited[technique])

    def rejected(prefix, expected, extra, sequence=None):
        try:
            run(renderer, output, primary, prefix, "deferred-raster", "lit", records,
                sequence=sequence, extra=(*linked_options, *extra))
        except RuntimeError:
            log = (output / f"{prefix}.log").read_text(encoding="utf-8", errors="replace")
            assert expected in log, log
        else:
            raise AssertionError(f"Expected {prefix} to reject export")

    rejected("protect-source", "cannot replace a linked input file", ("--save-scene", str(primary)))
    rejected_sequence = output / "payload-edit.json"
    write_json(rejected_sequence, {"schemaVersion": 1, "frames": 3, "sampleFrames": [3], "events": [
        {"frame": 2, "nodeAuthoring": {"action": "rename", "sourceNode": 2, "name": "Payload edit"}},
    ]})
    companion_bytes = linked_saved.read_bytes()
    rejected("protect-companion", READ_ONLY_DIAGNOSTIC,
             ("--save-linked-scene", str(linked_saved)), rejected_sequence)
    assert linked_saved.read_bytes() == companion_bytes
    assert all(hashlib.sha256(path.read_bytes()).hexdigest() == digest for path, digest in source_hashes.items())

    model, _ = load_asset(saved)
    assert model["asset"]["version"] == "2.0" and "externalAssets" not in model and "files" not in model
    verify_portable_xmp(model)
    verify_portable_xmp(load_asset(edited_portable)[0])
    primary.parent.rename(output / "original-inputs-removed")
    for technique in references:
        restored = run(renderer, output, saved, f"reloaded-{technique}", technique, "lit", records, extra=common)[0]
        verify_picks(restored["state"])
        np.testing.assert_array_equal(pixels(Path(restored["image"])), references[technique])
        flattened = run(renderer, output, edited_portable, f"edited-flattened-{technique}", technique,
                        "lit", records, extra=common)[0]
        verify_picks(flattened["state"])
        np.testing.assert_array_equal(pixels(Path(flattened["image"])), edited[technique])
    run_animated_linked(renderer, output, records, common)
    run_editable_linked(renderer, output, records, common)
    nested_checks = run_nested_linked(renderer, output, records, common)
    assert hashlib.sha256(renderer.read_bytes()).hexdigest() == renderer_digest
    write_json(output / "results.json", {"passed": True, "rendererSha256": renderer_digest,
               "captures": records, "nestedChecks": nested_checks})
    print("PASS linked geometry, picks, animation, reference preservation and pixel-exact portable reload", flush=True)


if __name__ == "__main__":
    main()
