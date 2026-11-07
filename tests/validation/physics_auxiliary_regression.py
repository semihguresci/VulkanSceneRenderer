"""Opt-in deterministic BIM/USD product physics and model lifecycle checks.

Creates its own tiny glTF floor and auxiliary fixtures. Two spatially separated
mesh fragments share a GUID and must move as one rigid body; a third product
remains static. Exercises both raster techniques with raster and ray/TAA paths.
"""
import argparse
import base64
import hashlib
import json
import os
from pathlib import Path
import struct

PRODUCT_GUID = "physics-fragmented-product"
OTHER_GUID = "physics-unrelated-product"
COLLIDING_SOURCE_ID = "shared"
SAMPLE_FRAMES = [1, 3, 30, 180, 182, 184, 188, 190, 200]


def cube():
    """Independent outward-wound face vertices, normals and indexed triangles."""
    faces = [
        ([(1, -1, -1), (1, 1, -1), (1, 1, 1), (1, -1, 1)], (1, 0, 0)),
        ([(-1, -1, 1), (-1, 1, 1), (-1, 1, -1), (-1, -1, -1)], (-1, 0, 0)),
        ([(-1, 1, -1), (-1, 1, 1), (1, 1, 1), (1, 1, -1)], (0, 1, 0)),
        ([(-1, -1, 1), (-1, -1, -1), (1, -1, -1), (1, -1, 1)], (0, -1, 0)),
        ([(-1, -1, 1), (1, -1, 1), (1, 1, 1), (-1, 1, 1)], (0, 0, 1)),
        ([(1, -1, -1), (-1, -1, -1), (-1, 1, -1), (1, 1, -1)], (0, 0, -1))]
    points, normals, indices = [], [], []
    for face, normal in faces:
        offset = len(points)
        points.extend(tuple(component * .5 for component in point) for point in face)
        normals.extend([normal] * 4)
        indices.extend(offset + index for index in (0, 1, 2, 0, 2, 3))
    return points, normals, indices


def write_fixtures(directory, *, source_id_collision=False):
    directory = Path(directory)
    directory.mkdir(parents=True, exist_ok=True)
    points, normals, indices = cube()
    positions = struct.pack("<72f", *(value for point in points for value in point))
    normal_bytes = struct.pack("<72f", *(value for point in normals for value in point))
    index_bytes = struct.pack("<36H", *indices)
    data = positions + normal_bytes + index_bytes
    floor = directory / "physics-floor.gltf"
    floor.write_text(json.dumps({
        "asset": {"version": "2.0"}, "scene": 0, "scenes": [{"nodes": [0]}],
        "nodes": [{"name": "Physics auxiliary floor", "mesh": 0,
                   "translation": [0, -.25, 0], "scale": [12, .5, 8]}],
        "meshes": [{"primitives": [{"attributes": {"POSITION": 0, "NORMAL": 1},
                                     "indices": 2, "material": 0}]}],
        "materials": [{"pbrMetallicRoughness": {"baseColorFactor": [.6, .6, .6, 1],
                                                "metallicFactor": 0, "roughnessFactor": .8}}],
        "buffers": [{"byteLength": len(data), "uri": "data:application/octet-stream;base64," +
                     base64.b64encode(data).decode("ascii")}],
        "bufferViews": [{"buffer": 0, "byteOffset": 0, "byteLength": len(positions)},
                        {"buffer": 0, "byteOffset": len(positions), "byteLength": len(normal_bytes)},
                        {"buffer": 0, "byteOffset": len(positions + normal_bytes), "byteLength": len(index_bytes)}],
        "accessors": [{"bufferView": 0, "componentType": 5126, "count": 24, "type": "VEC3",
                       "min": [-.5, -.5, -.5], "max": [.5, .5, .5]},
                      {"bufferView": 1, "componentType": 5126, "count": 24, "type": "VEC3"},
                      {"bufferView": 2, "componentType": 5123, "count": 36, "type": "SCALAR"}]
    }), encoding="utf-8")
    centers = [(-1, 3, 0), (1, 3, 0), (4, 3, 0)]
    meshes, elements, usd = [], [], ['#usda 1.0', '(upAxis = "Y" metersPerUnit = 1)', 'def Xform "PhysicsFixture" {']
    for index, center in enumerate(centers):
        guid = PRODUCT_GUID if index < 2 else OTHER_GUID
        world_points = [tuple(point[axis] + center[axis] for axis in range(3)) for point in points]
        # DotBIM uses +Z up, +Y forward. Invert the renderer's documented import
        # rotation, keeping exactly the same world geometry as the Y-up USD.
        source_points = [(x, -z, y) for x, y, z in world_points]
        meshes.append({"mesh_id": index, "coordinates": [value for point in source_points for value in point],
                       "indices": indices})
        elements.append({"mesh_id": index,
                         "guid": "" if source_id_collision and index < 2 else guid,
                         "type": "IfcBuildingElementProxy",
                         "vector": {"x": 0, "y": 0, "z": 0},
                         "rotation": {"qx": 0, "qy": 0, "qz": 0, "qw": 1},
                         "color": {"r": 190, "g": 100 if index < 2 else 180, "b": 65, "a": 255},
                         "info": {"Name": "Fragmented product" if index < 2 else "Unrelated product"}})
        if source_id_collision:
            # Product A uses source identity; product B has its own GUID and
            # happens to reuse A's source label. The GUID must take precedence.
            elements[-1]["sourceId"] = COLLIDING_SOURCE_ID
        coordinates = ", ".join("(" + ", ".join(str(value) for value in point) + ")" for point in world_points)
        usd += [f'def Mesh "fragment{index}" (customData = {{ string guid = "{guid}" }}) {{',
                f'point3f[] points = [{coordinates}]',
                'int[] faceVertexCounts = [' + ','.join(['3'] * 12) + ']',
                'int[] faceVertexIndices = [' + ','.join(map(str, indices)) + ']',
                'uniform token subdivisionScheme = "none"',
                'color3f[] primvars:displayColor = [(0.75, 0.4, 0.25)]',
                'bool doubleSided = false', '}']
    usd.append('}')
    bim = directory / "physics-fragments.bim"
    bim.write_text(json.dumps({"schema_version": "1.1.0", "meshes": meshes,
                               "elements": elements, "info": {"Description": "Rigid product fixture"}}), encoding="utf-8")
    usda = directory / "physics-fragments.usda"
    usda.write_text("\n".join(usd) + "\n", encoding="utf-8")
    return floor, {"bim": bim, "usd": usda}


def sequence(auxiliary):
    return {"schemaVersion": 1, "frames": 200, "sampleFrames": SAMPLE_FRAMES,
            "camera": [{"frame": 1, "position": [7, 6, 12], "target": [1, 1.5, 0]},
                       {"frame": 200, "position": [7, 6, 12], "target": [1, 1.5, 0]}],
            "events": [
                {"frame": 1, "physicsNode": 1, "physicsAttach": "static"},
                {"frame": 2, "bimSelectedObject": 2, "physicsAttach": "static"},
                {"frame": 3, "bimSelectedObject": 0, "physicsAttach": "dynamic", "physicsAction": "play"},
                {"frame": 181, "physicsAction": "reset"},
                {"frame": 183, "physicsAction": "detach"},
                {"frame": 185, "guiReload": str(auxiliary)},
                {"frame": 189, "bimSelectedObject": 0, "physicsAttach": "dynamic", "physicsAction": "play"},
                {"frame": 193, "physicsAction": "clear"},
                {"frame": 197, "guiUnload": True}]}


def matrix(value, *, columns=4):
    import numpy as np
    result = np.asarray(value, dtype=float)
    assert result.size == columns * 4 and np.isfinite(result).all(), "Invalid captured physics matrix"
    return result.reshape(columns, 4).T


def check(record):
    import numpy as np
    collision = record.get("sourceIdCollision", False)
    assert not collision or record["format"] == "bim", "Source-ID collision fixture requires DotBIM"
    product_guid = "" if collision else PRODUCT_GUID
    assert not record.get("validationErrors"), "Capture recorded Vulkan/runtime validation errors"
    samples = {sample["frame"]: sample for sample in record["samples"]}
    assert sorted(samples) == SAMPLE_FRAMES and len(samples) == len(record["samples"]), "Incomplete auxiliary captures"
    states = {frame: sample["telemetry"]["physics"] for frame, sample in samples.items()}
    for state in states.values():
        assert state["available"], "Physics backend was unavailable"
        assert not state.get("error"), "Physics reported a runtime error"
        assert state.get("droppedSeconds", 0) == 0, "Deterministic physics dropped simulation time"

    def group(frame, guid):
        matches = [entry for entry in states[frame]["auxiliaryElements"]
                   if any(obj["guid"] == guid for obj in entry["objects"])]
        assert len(matches) == 1, f"Missing/duplicate attached product {guid} at frame {frame}"
        entry = matches[0]
        assert all(obj["guid"] == guid for obj in entry["objects"]), "Physics merged unrelated products"
        if collision:
            assert all(obj["sourceId"] == COLLIDING_SOURCE_ID for obj in entry["objects"]), \
                "Source-ID collision fixture identity was not preserved"
        body = next(body for body in states[frame]["bodies"] if body["node"] == entry["node"])
        return entry, body

    initial_group, initial_body = group(3, product_guid)
    assert len(initial_group["objects"]) == 2, "Both same-product fragments must share one body"
    initial_objects = {obj["objectIndex"]: obj for obj in initial_group["objects"]}
    initial_proxy = matrix(initial_body["sceneWorldMatrix"])
    # Importers may append floor-plan overlay geometry. Assert preservation of
    # the captured publication count while checking the three fixture mesh
    # identities independently through the two product bindings below.
    initial_auxiliary_count = samples[1]["telemetry"]["scene"]["auxiliaryObjectCount"]
    assert initial_auxiliary_count >= 3, "Auxiliary fixture mesh geometry is incomplete"
    other_initial, _ = group(3, OTHER_GUID)
    assert len(other_initial["objects"]) == 1
    other_initial = other_initial["objects"][0]
    assert len(states[3]["bodies"]) == states[3]["bodyCount"] == 3, "Expected primary floor and two auxiliary products"
    assert 2.9 < initial_body["position"][1] < 3.1
    assert states[3]["totalSteps"] < states[30]["totalSteps"] < states[180]["totalSteps"]
    for frame in (3, 30, 180, 182):
        entry, body = group(frame, product_guid)
        assert entry["node"] == initial_group["node"] and entry["productId"] == initial_group["productId"]
        assert body["motion"] == 2
        delta = matrix(body["sceneWorldMatrix"]) @ np.linalg.inv(initial_proxy)
        objects = {obj["objectIndex"]: obj for obj in entry["objects"]}
        assert objects.keys() == initial_objects.keys(), "Product fragments changed identity"
        for index, obj in objects.items():
            original = initial_objects[index]
            assert (obj["guid"], obj["sourceId"]) == (original["guid"], original["sourceId"]), "Physics changed BIM source identity"
            expected = delta @ matrix(original["model"])
            actual = matrix(obj["model"])
            assert np.allclose(actual, expected, atol=2e-4, rtol=0), "Rendered fragment ignored the rigid product transform"
            # This fixture has only translation during simulation. Both exact
            # world AABBs and inverse-transpose normals must stay in agreement.
            assert np.allclose(delta[:3, :3], np.eye(3), atol=2e-4, rtol=0), "Unexpected fixture rotation"
            shift = delta[:3, 3]
            assert np.allclose(obj["boundsMin"], np.asarray(original["boundsMin"]) + shift, atol=3e-4, rtol=0), "Stale fragment minimum bound"
            assert np.allclose(obj["boundsMax"], np.asarray(original["boundsMax"]) + shift, atol=3e-4, rtol=0), "Stale fragment maximum bound"
            normals = matrix(obj["normalMatrix"], columns=3)[:3, :3]
            assert np.allclose(normals, np.linalg.inv(actual[:3, :3]).T, atol=3e-4, rtol=0), "Stale normal transform"
        unrelated, other_body = group(frame, OTHER_GUID)
        assert other_body["motion"] == 0 and len(unrelated["objects"]) == 1
        other = unrelated["objects"][0]
        for key in ("model", "normalMatrix", "boundsMin", "boundsMax", "guid", "sourceId"):
            assert other[key] == other_initial[key], f"Unrelated product {key} changed"
    _, falling = group(30, product_guid)
    _, settled = group(180, product_guid)
    assert falling["position"][1] < initial_body["position"][1] - .5, "Auxiliary meshes did not fall"
    assert .44 < settled["position"][1] < .65, "Auxiliary product did not collide with the primary floor"
    assert not states[182]["playing"], "Reset must pause physics"
    _, reset_body = group(182, product_guid)
    assert abs(reset_body["position"][1] - 3) < 1e-4, "Reset did not restore the imported placement"
    assert states[184]["bodyCount"] == 2 and len(states[184]["auxiliaryElements"]) == 1, "Detach retained the selected product body"
    for frame in (3, 30, 180, 182, 184, 188, 190):
        assert samples[frame]["telemetry"]["scene"]["auxiliaryObjectCount"] == initial_auxiliary_count, \
            f"Physics attachment/lifecycle changed imported geometry count at frame {frame}"
    assert states[188]["bodyCount"] == 0 and not states[188]["auxiliaryElements"], "Reload retained stale physics identities"
    reattached, reattached_body = group(190, product_guid)
    assert len(reattached["objects"]) == 2 and states[190]["bodyCount"] == 1
    assert states[190]["playing"] and 2.9 < reattached_body["position"][1] < 3.1
    assert states[200]["bodyCount"] == 0 and not states[200]["auxiliaryElements"], "Unload retained auxiliary physics"
    assert samples[200]["telemetry"]["scene"]["auxiliaryObjectCount"] == 0
    if record["mode"] == "rays-taa":
        assert samples[180]["telemetry"]["rayShadows"]["active"], "Ray-query path inactive"
        assert samples[180]["telemetry"]["taa"]["enabled"], "Temporal path inactive"
    hashes = [hashlib.sha256(Path(samples[frame]["png"]).read_bytes()).hexdigest() for frame in (3, 30, 180)]
    assert len(set(hashes)) == 3, "Physics moved telemetry without changing the rendered image"
    return {"passed": True, "captures": len(samples), "fragmentCount": 2,
            "settledHeight": settled["position"][1], "unrelatedProductUnchanged": True,
            "resetDetachReloadUnload": True, "sourceIdCollisionGuard": collision, "imageSha256": hashes}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--exe", type=Path)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--require-opt-in", action="store_true")
    parser.add_argument("--check-existing", action="store_true")
    parser.add_argument("--format", choices=("bim", "usd"), help="Limit a native probe to one auxiliary format")
    parser.add_argument("--technique", choices=("deferred-raster", "forward-raster"))
    parser.add_argument("--mode", choices=("raster", "rays-taa"))
    parser.add_argument("--source-id-collision", action="store_true",
                        help="Require --format bim; two GUID-less fragments share the GUID-bearing third product's source ID")
    args = parser.parse_args()
    if args.source_id_collision and args.format != "bim":
        parser.error("--source-id-collision requires --format bim")
    if args.require_opt_in and os.environ.get("CONTAINER_RUN_GPU_PHYSICS") != "1":
        print("Set CONTAINER_RUN_GPU_PHYSICS=1 to run native auxiliary physics captures")
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
        assert exe.is_file(), "Build the application first"
        selected_cases = [(format_name, technique, mode)
                          for format_name in ((args.format,) if args.format else ("bim", "usd"))
                          for technique in ((args.technique,) if args.technique else ("deferred-raster", "forward-raster"))
                          for mode in ((args.mode,) if args.mode else ("raster", "rays-taa"))]
        results = {"schemaVersion": 1, "passed": False, "cases": [],
                   "expectedCases": selected_cases,
                   "sourceIdCollision": args.source_id_collision,
                   "executableSha256": hashlib.sha256(exe.read_bytes()).hexdigest()}
    results["passed"] = False
    results.pop("failure", None)
    report.write_text(json.dumps(results, indent=2), encoding="utf-8")
    try:
        if not args.check_existing:
            from gui_model_loading_regression import run_lifecycle_capture
            floor, sources = write_fixtures(output / "fixtures", source_id_collision=args.source_id_collision)
            (output / "vk_layer_settings.txt").write_text("khronos_validation.validate_sync = true\n", encoding="utf-8")
            (output / "imgui.ini").write_text(
                "[Window][Scene Controls]\nPos=0,0\nSize=350,500\nCollapsed=1\n\n"
                "[Window][Renderer Telemetry]\nPos=0,25\nSize=350,500\nCollapsed=1\n", encoding="utf-8")
            for format_name, technique, mode in selected_cases:
                source = sources[format_name]
                name = f"physics-aux-{format_name}-{technique}-{mode}"
                if args.source_id_collision:
                    name += "-source-id-collision"
                record = run_lifecycle_capture(exe, output, name, floor, sequence(source), technique, mode,
                    extra=("--bim-model", str(source), "--directional-intensity", "2", "--environment-intensity", "1"))
                record["format"] = format_name
                record["sourceIdCollision"] = args.source_id_collision
                results["cases"].append(record)
                record["checks"] = check(record)
                report.write_text(json.dumps(results, indent=2), encoding="utf-8")
                print(json.dumps({"case": name, **record["checks"]}), flush=True)
            assert hashlib.sha256(exe.read_bytes()).hexdigest() == results["executableSha256"], "Executable changed during captures"
        else:
            cases = {(record["format"], record["technique"], record["mode"]): record for record in results["cases"]}
            supported = {(format_name, technique, mode) for format_name in ("bim", "usd")
                         for technique in ("deferred-raster", "forward-raster") for mode in ("raster", "rays-taa")}
            expected = {tuple(case) for case in results.get("expectedCases", supported)}
            assert expected and expected <= supported, "Unexpected auxiliary case matrix"
            assert len(cases) == len(results["cases"]) == len(expected), "Incomplete/duplicate auxiliary cases"
            assert set(cases) == expected, "Unexpected auxiliary case matrix"
            for record in cases.values():
                for sample in record["samples"]:
                    local = output / Path(sample["png"]).name
                    if local.is_file():
                        sample["png"] = str(local)
                record["checks"] = check(record)
        results["passed"] = True
    except Exception as error:
        results["failure"] = str(error)
        print(f"Auxiliary physics validation failed: {error}", flush=True)
    report.write_text(json.dumps(results, indent=2), encoding="utf-8")
    return 0 if results["passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
