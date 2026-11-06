"""Opt-in native collider-growth and small authored-unit scale regressions."""
import argparse
import hashlib
import json
import os
from pathlib import Path

from physics_auxiliary_regression import cube, matrix
from physics_scene_diversity_regression import (
    BIM_ROTATION, bim_source_geometry, encode, transform_vertices, translation, write_gltf,
)

CASES = ("static-growth", "unit-scale")
TECHNIQUES = ("deferred-raster", "forward-raster")
SAMPLE_FRAMES = {"static-growth": [1, 4, 90, 92, 120, 150, 154, 160],
                 "unit-scale": [1, 3, 4, 8, 30, 34, 40]}
GUID = "physics-static-growth-sleeper"


def relative_scale(case, frame):
    if case == "static-growth":
        return [10, 1, 4] if frame >= 91 else [1, 1, 1]
    return [1.005] * 3 if frame >= 4 else [1, 1, 1]


def write_sleeper(path):
    points, _, indices = cube()
    source = [[x + 4, -z, y] for x, y, z in points]
    path.write_text(json.dumps({"schema_version": "1.1.0", "info": {},
        "meshes": [{"mesh_id": 0, "coordinates": [v for point in source for v in point], "indices": indices}],
        "elements": [{"mesh_id": 0, "guid": GUID, "type": "IfcBuildingElementProxy",
            "vector": {"x": 0, "y": 0, "z": 0},
            "rotation": {"qx": 0, "qy": 0, "qz": 0, "qw": 1},
            "color": {"r": 230, "g": 120, "b": 60, "a": 255},
            "info": {"Name": "Sleeping BIM cube outside old bounds"}}]}), encoding="utf-8")


def fixture(case, directory):
    if case not in CASES:
        raise ValueError("Unknown physics scale case")
    directory = Path(directory)
    directory.mkdir(parents=True, exist_ok=True)
    source = directory / f"{case}.gltf"
    auxiliary = None
    name = "Growing static obstacle" if case == "static-growth" else "Small source-unit dynamic cube"
    node = {"name": name, "mesh": 0}
    points = None
    if case == "static-growth":
        nodes = [node, {"name": "Remote sleeping primary cube", "mesh": 0, "translation": [20, 0, 0]}]
        roots = [0, 1]
        frames, clear, unload = 160, 153, 157
        scale_frames = (1, 90, 91, frames)
        camera, target = [23, 18, 32], [8, 0, 0]
    else:
        node["scale"] = [.001] * 3
        points = [[[component * 20000 for component in point] for point in cube()[0]]]
        nodes, roots = [node], [0]
        frames, clear, unload = 40, 31, 37
        scale_frames = (1, 3, 4, frames)
        camera, target = [32, 26, 42], [0, 0, 0]
    primary = write_gltf(source, nodes, roots, points)
    animated = primary[name]
    if case == "static-growth":
        auxiliary = directory / "sleeping-product.bim"
        write_sleeper(auxiliary)
        remote = primary["Remote sleeping primary cube"]
        events = [{"frame": 1, "physicsNode": animated["node"], "physicsAttach": "static"},
            {"frame": 2, "physicsNode": remote["node"], "physicsAttach": "dynamic",
             "physicsSettings": {"gravityFactor": 0}},
            {"frame": 3, "bimSelectedObject": 0, "physicsAttach": "dynamic", "physicsAction": "play",
             "physicsSettings": {"gravityFactor": 0}}]
    else:
        events = [{"frame": 1, "physicsNode": animated["node"], "physicsAttach": "dynamic",
                   "physicsAction": "play", "physicsSettings": {"gravityFactor": 0}}]
    events += [{"frame": clear, "physicsAction": "clear"}, {"frame": unload, "guiUnload": True}]
    sequence = {"schemaVersion": 1, "frames": frames, "sampleFrames": SAMPLE_FRAMES[case].copy(),
        "camera": [{"frame": frame, "position": camera, "target": target} for frame in (1, frames)],
        "objectNode": 0, "objectName": name,
        "objectScale": [{"frame": frame, "scale": relative_scale(case, frame)} for frame in scale_frames],
        "events": events}
    specification = {"case": case, "primary": primary, "animatedName": name, "animatedPrimaryNode": 0,
        "bimGeometry": bim_source_geometry(auxiliary) if auxiliary else {},
        "sampleFrames": SAMPLE_FRAMES[case].copy(), "clearFrame": clear, "unloadFrame": unload}
    return source, auxiliary, sequence, specification


def check(record):
    import numpy as np
    assert not record.get("validationErrors"), "Scale capture recorded validation errors"
    specification = record["fixture"]
    case = specification["case"]
    assert case in CASES and record["technique"] in TECHNIQUES and record["mode"] == "raster", "Unknown scale capture"
    samples = {sample["frame"]: sample for sample in record["samples"]}
    assert sorted(samples) == SAMPLE_FRAMES[case] and len(samples) == len(record["samples"]), "Incomplete scale captures"
    geometry = specification["primary"]
    animated = geometry[specification["animatedName"]]
    initial_auxiliary_count = samples[1]["telemetry"]["scene"]["auxiliaryObjectCount"]
    assert initial_auxiliary_count >= 1 if case == "static-growth" else initial_auxiliary_count == 0, "Unexpected auxiliary geometry"
    identity = None
    affected = {}
    for frame, sample in samples.items():
        state, scene = sample["telemetry"]["physics"], sample["telemetry"]["scene"]
        assert state["available"] and not state.get("error"), f"Physics failed at frame {frame}"
        assert state.get("droppedSeconds", 0) == 0 and state["jointCount"] == 0, "Unexpected scale simulation timing or joints"
        if frame >= specification["unloadFrame"]:
            assert state["bodyCount"] == 0 and not state.get("bodies") and not state.get("auxiliaryElements"), "Unload retained physics bindings"
            assert scene["primaryObjectCount"] == scene["auxiliaryObjectCount"] == 0, "Unload retained model geometry"
            continue
        assert scene["primaryObjectCount"] == len(geometry) and scene["auxiliaryObjectCount"] == initial_auxiliary_count, "Scale edits changed imported geometry counts"
        if frame >= specification["clearFrame"]:
            assert state["bodyCount"] == 0 and not state["playing"] and not state.get("bodies") and not state.get("auxiliaryElements"), "Clear retained physics bindings"
            continue
        expected_count = 1 if case == "unit-scale" or frame == 1 else 3
        assert state["bodyCount"] == len(state["bodies"]) == expected_count, "Scale body count changed"
        assert state["playing"] or case == "static-growth" and frame == 1, "Scale playback unexpectedly paused"
        for name, original in geometry.items():
            body = next((body for body in state["bodies"] if body["node"] == original["node"]), None)
            if body is None:
                assert case == "static-growth" and frame == 1 and name != specification["animatedName"], "Missing primary scale body"
                continue
            model = matrix(body["renderWorldMatrix"])
            assert np.allclose(model, matrix(body["sceneWorldMatrix"]), atol=1e-6, rtol=0), "Primary rendered scale transform is stale"
            expected = matrix(original["authoredModel"])
            if name == specification["animatedName"]:
                expected[:3, :3] = expected[:3, :3] @ np.diag(relative_scale(case, frame))
            tolerance = 2e-8 if case == "unit-scale" else 2e-4
            assert np.allclose(model, expected, atol=tolerance, rtol=0), "Authored scale did not persist through simulation"
            vertices = np.asarray(original["vertices"])
            center = (vertices.min(axis=0) + vertices.max(axis=0)) / 2
            assert np.allclose((model @ np.append(center, 1))[:3], body["position"], atol=2e-4, rtol=0), "Primary scaled geometry disagrees with its collider"
            expected_motion = 0 if case == "static-growth" and name == specification["animatedName"] else 2
            assert body["motion"] == expected_motion, "Scale fixture motion type changed"
        if case == "unit-scale":
            assert not state.get("auxiliaryElements"), "Unit-scale fixture acquired auxiliary bindings"
            if frame >= 4:
                points = transform_vertices(matrix(next(body for body in state["bodies"] if body["node"] == animated["node"])["renderWorldMatrix"]), animated["vertices"])
                assert np.allclose(points.max(axis=0) - points.min(axis=0), [20.1] * 3, atol=4e-4, rtol=0), "Source-unit scale produced incorrect world extents"
            continue
        if frame == 1:
            assert not state.get("auxiliaryElements"), "BIM body attached before its event"
            continue
        elements = state.get("auxiliaryElements", [])
        assert len(elements) == 1 and len(elements[0]["objects"]) == 1, "Missing or split BIM scale product"
        element = elements[0]
        body = next(body for body in state["bodies"] if body["node"] == element["node"])
        assert body["motion"] == 2, "Affected BIM body is not dynamic"
        proxy = matrix(body["sceneWorldMatrix"])
        assert np.allclose(proxy[:3, 3], body["position"], atol=3e-4, rtol=0), "BIM rendered proxy disagrees with its physical position"
        affected[frame] = body
        obj = element["objects"][0]
        semantic = (element["productId"], obj["objectIndex"], obj["guid"], obj["sourceId"])
        if identity is None:
            identity = semantic
        assert identity == semantic and obj["objectIndex"] == 0 and obj["guid"] == GUID, "Scale edit changed BIM product identity"
        expected = proxy @ np.linalg.inv(translation([4, 0, 0])) @ matrix(BIM_ROTATION)
        actual = matrix(obj["model"])
        assert np.allclose(actual, expected, atol=3e-4, rtol=0), "BIM rendered geometry ignored contact motion"
        points = transform_vertices(actual, specification["bimGeometry"]["0"]["vertices"])
        assert np.allclose(obj["boundsMin"], points.min(axis=0), atol=3e-4, rtol=0) and np.allclose(obj["boundsMax"], points.max(axis=0), atol=3e-4, rtol=0), "BIM scale-contact bounds are stale"
        assert np.allclose(matrix(obj["normalMatrix"], columns=3)[:3, :3], np.linalg.inv(actual[:3, :3]).T, atol=3e-4, rtol=0), "BIM scale-contact normals are stale"
        if frame <= 90:
            assert np.allclose(body["position"], [4, 0, 0], atol=2e-4, rtol=0), "BIM sleeper moved before collider growth"
        if frame in (120, 150):
            assert abs(body["position"][1]) > .9, "Growing static collider did not resolve the sleeping overlap"
            overlap = np.minimum(points.max(axis=0), [5, .5, 2]) - np.maximum(points.min(axis=0), [-5, -.5, -2])
            assert np.any(overlap <= .035), "BIM body still penetrates the grown collider"
    if case == "static-growth":
        before, after = samples[90]["telemetry"]["physics"], samples[150]["telemetry"]["physics"]
        assert before["activeBodyCount"] == 0, "Growth fixture had not settled to sleep"
        assert samples[92]["telemetry"]["physics"]["activeBodyCount"] == 1, "Static growth did not wake only the new overlap"
        assert after["totalSteps"] - before["totalSteps"] >= 59, "Scale edit interrupted fixed-step playback"
        image_frames = (90, 150)
    else:
        assert samples[30]["telemetry"]["physics"]["totalSteps"] - samples[8]["telemetry"]["physics"]["totalSteps"] >= 21, "Unit-scale edit interrupted fixed-step playback"
        image_frames = (3, 30)
    hashes = [hashlib.sha256(Path(samples[frame]["png"]).read_bytes()).hexdigest() for frame in image_frames]
    assert len(set(hashes)) == 2, "Visible scale change did not change captured PNGs"
    return {"passed": True, "captures": len(samples), "independentGeometryChecks": True, "imageSha256": hashes}


def check_matrix(results):
    expected = {tuple(case) for case in results["expectedCases"]}
    supported = {(case, technique) for case in CASES for technique in TECHNIQUES}
    actual = {(record["fixture"]["case"], record["technique"]): record for record in results["cases"]}
    assert expected and expected <= supported and len(expected) == len(results["expectedCases"]), "Unknown or duplicate scale case matrix"
    assert set(actual) == expected and len(actual) == len(results["cases"]), "Incomplete scale case matrix"
    return actual.values()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--exe", type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--case", choices=CASES)
    parser.add_argument("--technique", choices=TECHNIQUES)
    parser.add_argument("--require-opt-in", action="store_true")
    parser.add_argument("--check-existing", action="store_true")
    args = parser.parse_args()
    if args.require_opt_in and os.environ.get("CONTAINER_RUN_GPU_PHYSICS") != "1":
        print("Set CONTAINER_RUN_GPU_PHYSICS=1 for native physics scale captures")
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
        selected = [(case, technique) for case in ((args.case,) if args.case else CASES)
                    for technique in ((args.technique,) if args.technique else TECHNIQUES)]
        results = {"schemaVersion": 1, "passed": False, "expectedCases": selected, "cases": [],
                   "executableSha256": hashlib.sha256(exe.read_bytes()).hexdigest()}
    results["passed"] = False
    results.pop("failure", None)
    for record in results["cases"]:
        record.pop("checks", None)
    report.write_text(json.dumps(results, indent=2), encoding="utf-8")
    try:
        if not args.check_existing:
            from gui_model_loading_regression import run_lifecycle_capture
            (output / "vk_layer_settings.txt").write_text("khronos_validation.validate_sync = true\n", encoding="utf-8")
            (output / "imgui.ini").write_text("[Window][Scene Controls]\nPos=0,0\nSize=350,500\nCollapsed=1\n\n"
                "[Window][Renderer Telemetry]\nPos=0,25\nSize=350,500\nCollapsed=1\n", encoding="utf-8")
            for case, technique in selected:
                source, auxiliary, sequence, specification = fixture(case, output / "fixtures" / case)
                extra = ["--directional-intensity", "2", "--environment-intensity", "1"]
                if auxiliary:
                    extra += ["--bim-model", str(auxiliary)]
                record = run_lifecycle_capture(exe, output, f"physics-scale-{case}-{technique}", source,
                    sequence, technique, "raster", extra=extra)
                record["fixture"] = specification
                results["cases"].append(record)
                record["checks"] = check(record)
                report.write_text(json.dumps(results, indent=2), encoding="utf-8")
                print(json.dumps({"case": case, "technique": technique, **record["checks"]}), flush=True)
            assert hashlib.sha256(exe.read_bytes()).hexdigest() == results["executableSha256"], "Executable changed during scale capture"
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
        print(f"Physics scale validation failed: {error}", flush=True)
    report.write_text(json.dumps(results, indent=2), encoding="utf-8")
    return 0 if results["passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
