"""Opt-in cross-provider kinematic lifts with real render/physics evidence.

A primary glTF platform lifts a fragmented BIM rider, then the inverse provider
arrangement exercises BIM root translation without pausing the simulation.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path

from physics_auxiliary_regression import cube, matrix
from physics_scene_diversity_regression import (
    BIM_ROTATION, bim_source_geometry, encode, transform_vertices, translation,
    write_gltf,
)

PROVIDERS = ("gltf", "bim")
TECHNIQUES = ("deferred-raster", "forward-raster")
SAMPLE_FRAMES = [1, 3, 90, 120, 150, 180, 184, 190]
GUID = "moving-support-fragmented-product"


def lift_height(frame):
    return max(0.0, min(1.5, (frame - 90) * 1.5 / 60))


def write_product(path, support):
    """Two adjacent mesh fragments form one box, in dotBIM's source Z-up."""
    points, _, indices = cube()
    center = [0, 0 if support else 3, 0]
    size = [4, .5, 4] if support else [1, 1, 1]
    meshes, elements = [], []
    for index in range(2):
        offset = (-.25 if index == 0 else .25) * size[0]
        world = [[point[0]*size[0]/2 + offset, point[1]*size[1] + center[1],
                  point[2]*size[2]] for point in points]
        source = [[x, -z, y] for x, y, z in world]
        meshes.append({"mesh_id": index, "coordinates": [v for point in source for v in point],
                       "indices": indices})
        elements.append({"mesh_id": index, "guid": GUID, "type": "IfcBuildingElementProxy",
            "vector": {"x": 0, "y": 0, "z": 0},
            "rotation": {"qx": 0, "qy": 0, "qz": 0, "qw": 1},
            "color": {"r": 70 if support else 220, "g": 150, "b": 210 if support else 65, "a": 255},
            "info": {"Name": "Moving BIM platform" if support else "BIM lift rider"}})
    path.write_text(json.dumps({"schema_version": "1.1.0", "meshes": meshes,
                                "elements": elements, "info": {}}), encoding="utf-8")


def fixture(provider, directory):
    if provider not in PROVIDERS:
        raise ValueError("Unknown moving-support provider")
    directory = Path(directory)
    directory.mkdir(parents=True, exist_ok=True)
    source, auxiliary = directory / "lift.gltf", directory / "fragmented-product.bim"
    primary_support = provider == "gltf"
    node = {"name": "Primary lift platform" if primary_support else "Primary lift rider", "mesh": 0,
            "translation": [0, 0 if primary_support else 3, 0],
            "scale": [4, .5, 4] if primary_support else [1, 1, 1]}
    geometry = write_gltf(source, [node], [0])
    primary = next(iter(geometry.values()))
    write_product(auxiliary, support=not primary_support)
    endpoint = {"physicsNode": primary["node"]} if primary_support else {"bimSelectedObject": 0}
    rider = {"bimSelectedObject": 0} if primary_support else {"physicsNode": primary["node"]}
    events = [{"frame": 1, **endpoint, "physicsAttach": "kinematic", "physicsSettings": {"friction": .6}},
              {"frame": 2, **rider, "physicsAttach": "dynamic", "physicsAction": "play",
               "physicsSettings": {"friction": .6, "restitution": 0}},
              {"frame": 181, "physicsAction": "clear"}, {"frame": 187, "guiUnload": True}]
    sequence = {"schemaVersion": 1, "frames": 190, "sampleFrames": SAMPLE_FRAMES.copy(),
        "camera": [{"frame": frame, "position": [6, 5, 8], "target": [0, 1, 0]} for frame in (1, 190)],
        "events": events}
    track = [{"frame": frame, "translation": [0, lift_height(frame), 0]} for frame in (1, 90, 150, 190)]
    if primary_support:
        # The imported source node owns its scale. Translate that parent so
        # world-space lift height is not multiplied by its .5 Y scale.
        # Name lookup must also suspend after unload: the empty replacement can
        # reuse node 0 for SceneRoot, but cannot retain this imported name.
        sequence.update({"objectNode": 0, "objectName": node["name"], "objectTranslation": track})
    else:
        # This model contains only the kinematic platform. The dynamic rider
        # belongs to the primary provider, so translating BIM never pauses it.
        sequence["bimTranslation"] = track
    specification = {"supportProvider": provider, "primary": primary,
                     "animatedPrimaryNode": 0 if primary_support else None,
                     "bimGeometry": bim_source_geometry(auxiliary), "sampleFrames": SAMPLE_FRAMES.copy(),
                     "supportHalfHeight": .25, "riderHalfHeight": .5, "liftHeight": 1.5}
    return source, auxiliary, sequence, specification


def check(record):
    import numpy as np
    assert not record.get("validationErrors"), "Moving-support capture recorded validation errors"
    specification = record["fixture"]
    provider = specification["supportProvider"]
    assert provider in PROVIDERS and record["technique"] in TECHNIQUES and record["mode"] == "raster"
    samples = {sample["frame"]: sample for sample in record["samples"]}
    assert sorted(samples) == SAMPLE_FRAMES and len(samples) == len(record["samples"]), "Incomplete moving-support captures"
    primary_node = specification["primary"]["node"]
    base = matrix(BIM_ROTATION)
    original = specification["bimGeometry"]
    all_vertices = np.concatenate([transform_vertices(base, part["vertices"]) for part in original.values()])
    original_center = (all_vertices.min(axis=0) + all_vertices.max(axis=0)) / 2
    identity, body_nodes = None, None
    auxiliary_count = samples[1]["telemetry"]["scene"]["auxiliaryObjectCount"]
    assert auxiliary_count >= 2, "Missing fragmented BIM geometry"
    riders = {}
    for frame, sample in samples.items():
        telemetry = sample["telemetry"]
        state, scene = telemetry["physics"], telemetry["scene"]
        assert state["available"] and not state.get("error"), f"Physics failed at frame {frame}"
        assert state.get("droppedSeconds", 0) == 0, "Moving-support simulation dropped time"
        assert state["jointCount"] == 0, "Lift unexpectedly depends on a joint"
        if frame == 190:
            assert state["bodyCount"] == 0 and scene["primaryObjectCount"] == scene["auxiliaryObjectCount"] == 0, "Unload retained lift bodies or geometry"
            continue
        assert scene["primaryObjectCount"] == 1 and scene["auxiliaryObjectCount"] == auxiliary_count, "Lift lifecycle changed imported geometry"
        if frame == 184:
            assert state["bodyCount"] == 0 and not state["playing"], "Clear did not retire lift bodies"
            assert not state.get("bodies") and not state.get("auxiliaryElements"), "Clear retained auxiliary bindings"
            continue
        expected_count = 1 if frame == 1 else 2
        assert state["bodyCount"] == len(state["bodies"]) == expected_count, "Lift body count changed"
        if frame != 1:
            assert state["playing"], "Authored kinematic movement paused the simulation"
        primary = next((body for body in state["bodies"] if body["node"] == primary_node), None)
        if primary is not None:
            model = matrix(primary["renderWorldMatrix"])
            assert np.allclose(model, matrix(primary["sceneWorldMatrix"]), atol=1e-5, rtol=0), "Primary rendered transform is stale"
            authored = matrix(specification["primary"]["authoredModel"])
            assert np.allclose(np.linalg.norm(model[:3, :3], axis=0), np.linalg.norm(authored[:3, :3], axis=0),
                               atol=3e-4, rtol=0), "Primary collider lost authored mesh scale"
            vertices = np.asarray(specification["primary"]["vertices"])
            center = (vertices.min(axis=0) + vertices.max(axis=0))/2
            assert np.allclose((model @ np.append(center, 1))[:3], primary["position"], atol=3e-4, rtol=0), "Primary collider disagrees with rendered geometry"
        elements = state.get("auxiliaryElements", [])
        if elements:
            assert len(elements) == 1 and len(elements[0]["objects"]) == 2, "Fragmented BIM product split into separate bindings"
            element = elements[0]
            assert {obj["objectIndex"] for obj in element["objects"]} == {0, 1}, "BIM product fragment membership changed"
            auxiliary_body = next(body for body in state["bodies"] if body["node"] == element["node"])
            expected_model = matrix(auxiliary_body["sceneWorldMatrix"]) @ np.linalg.inv(translation(original_center)) @ base
            semantic = (element["productId"], tuple((obj["objectIndex"], obj["guid"], obj["sourceId"]) for obj in element["objects"]))
            if identity is None:
                identity = semantic
            assert identity == semantic, "Moving support changed BIM product identity"
            for obj in element["objects"]:
                assert obj["guid"] == GUID
                model = matrix(obj["model"])
                assert np.allclose(model, expected_model, atol=4e-4, rtol=0), "BIM rendered fragments lost rigid placement"
                vertices = transform_vertices(model, original[str(obj["objectIndex"])]["vertices"])
                assert np.allclose(obj["boundsMin"], vertices.min(axis=0), atol=5e-4, rtol=0) and np.allclose(obj["boundsMax"], vertices.max(axis=0), atol=5e-4, rtol=0), "BIM bounds disagree with original geometry"
                assert np.allclose(matrix(obj["normalMatrix"], columns=3)[:3, :3], np.linalg.inv(model[:3, :3]).T, atol=3e-4, rtol=0), "BIM normal matrix is stale"
        else:
            assert provider == "gltf" and frame == 1, "Missing BIM lift binding"
            auxiliary_body = None
        support = primary if provider == "gltf" else auxiliary_body
        assert support is not None and support["motion"] == 1, "Support is not kinematic"
        assert np.allclose(support["position"], [0, lift_height(frame), 0], atol=3e-4, rtol=0), "Kinematic support missed authored lift height"
        if provider == "gltf":
            expected = matrix(specification["primary"]["authoredModel"])
            expected[1, 3] += lift_height(frame)
            assert np.allclose(matrix(support["renderWorldMatrix"]), expected, atol=3e-4, rtol=0), "Rendered support lost authored lift geometry"
        else:
            expected = translation([0, lift_height(frame), 0]) @ base
            assert all(np.allclose(matrix(obj["model"]), expected, atol=4e-4, rtol=0) for obj in elements[0]["objects"]), "Rendered support lost authored lift geometry"
        if frame != 1:
            rider = auxiliary_body if provider == "gltf" else primary
            assert rider is not None and rider["motion"] == 2, "Rider is not dynamic"
            nodes = (support["node"], rider["node"])
            if body_nodes is None:
                body_nodes = nodes
            assert body_nodes == nodes, "Lift replaced body bindings during motion"
            riders[frame] = rider
            assert np.allclose(rider["position"][::2], [0, 0], atol=.1, rtol=0), "Lift rider drifted off the support"
            if frame >= 90:
                expected_y = lift_height(frame) + specification["supportHalfHeight"] + specification["riderHalfHeight"]
                assert abs(rider["position"][1] - expected_y) < .12, "Dynamic rider did not follow moving support contact"
                assert rider["position"][1] > lift_height(frame) + .6, "Dynamic rider penetrated the support"
    assert 1.4 < riders[180]["position"][1] - riders[90]["position"][1] < 1.6, "Kinematic lift did not carry the other provider's dynamic body"
    before, after = samples[90]["telemetry"]["physics"], samples[180]["telemetry"]["physics"]
    assert after["totalSteps"] - before["totalSteps"] >= 89, "Moving support interrupted fixed-step playback"
    hashes = [hashlib.sha256(Path(samples[frame]["png"]).read_bytes()).hexdigest() for frame in (90, 150)]
    assert len(set(hashes)) == 2, "Visible lift motion did not change captured PNGs"
    return {"passed": True, "captures": len(samples), "crossProviderContact": True,
            "independentGeometryBounds": True, "imageSha256": hashes}


def check_matrix(results):
    expected = {tuple(case) for case in results["expectedCases"]}
    supported = {(provider, technique) for provider in PROVIDERS for technique in TECHNIQUES}
    cases = {(case["fixture"]["supportProvider"], case["technique"]): case for case in results["cases"]}
    assert expected and expected <= supported, "Unknown moving-support case matrix"
    assert set(cases) == expected and len(cases) == len(results["cases"]) == len(expected), "Incomplete moving-support case matrix"
    return cases.values()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--exe", type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--provider", choices=PROVIDERS, help="Provider of the kinematic platform")
    parser.add_argument("--technique", choices=TECHNIQUES)
    parser.add_argument("--require-opt-in", action="store_true")
    parser.add_argument("--check-existing", action="store_true")
    args = parser.parse_args()
    if args.require_opt_in and os.environ.get("CONTAINER_RUN_GPU_PHYSICS") != "1":
        print("Set CONTAINER_RUN_GPU_PHYSICS=1 for native moving-support captures")
        return 77
    if not args.check_existing and (args.exe is None or not args.exe.is_file()):
        parser.error("--exe must name an existing renderer when capturing")
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    report = output / "results.json"
    if args.check_existing:
        results = json.loads(report.read_text(encoding="utf-8"))
    else:
        exe = args.exe.resolve()
        selected = [(provider, technique) for provider in ((args.provider,) if args.provider else PROVIDERS)
                    for technique in ((args.technique,) if args.technique else TECHNIQUES)]
        results = {"schemaVersion": 1, "passed": False, "expectedCases": selected, "cases": [],
                   "executableSha256": hashlib.sha256(exe.read_bytes()).hexdigest()}
    results["passed"] = False
    results.pop("failure", None)
    for case in results["cases"]:
        case.pop("checks", None)
    report.write_text(json.dumps(results, indent=2), encoding="utf-8")
    try:
        if not args.check_existing:
            from gui_model_loading_regression import run_lifecycle_capture
            (output / "vk_layer_settings.txt").write_text("khronos_validation.validate_sync = true\n", encoding="utf-8")
            (output / "imgui.ini").write_text("[Window][Scene Controls]\nPos=0,0\nSize=350,500\nCollapsed=1\n\n"
                "[Window][Renderer Telemetry]\nPos=0,25\nSize=350,500\nCollapsed=1\n", encoding="utf-8")
            for provider, technique in selected:
                source, auxiliary, sequence, specification = fixture(provider, output / "fixtures" / provider)
                record = run_lifecycle_capture(exe, output, f"physics-moving-support-{provider}-{technique}", source,
                    sequence, technique, "raster", extra=["--bim-model", str(auxiliary), "--directional-intensity", "2", "--environment-intensity", "1"])
                record["fixture"] = specification
                results["cases"].append(record)
                record["checks"] = check(record)
                report.write_text(json.dumps(results, indent=2), encoding="utf-8")
                print(json.dumps({"provider": provider, "technique": technique, **record["checks"]}), flush=True)
            assert hashlib.sha256(exe.read_bytes()).hexdigest() == results["executableSha256"], "Executable changed during moving-support capture"
        for record in check_matrix(results):
            if args.check_existing:
                for sample in record["samples"]:
                    local = output / Path(sample["png"]).name
                    if local.is_file():
                        sample["png"] = str(local)
                record["checks"] = check(record)
        results["passed"] = True
    except Exception as error:
        results["failure"] = str(error)
        print(f"Moving-support validation failed: {error}", flush=True)
    report.write_text(json.dumps(results, indent=2), encoding="utf-8")
    return 0 if results["passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
