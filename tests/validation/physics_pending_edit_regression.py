"""Opt-in native same-frame Play and unrelated Detach transform regressions."""
import argparse
import hashlib
import json
import os
from pathlib import Path

from physics_auxiliary_regression import cube, matrix
from physics_scene_diversity_regression import BIM_ROTATION, bim_source_geometry, encode, transform_vertices, translation, write_gltf

CASES = ("pending-play", "pending-detach")
TECHNIQUES = ("deferred-raster", "forward-raster")
SAMPLE_FRAMES = [1, 2, 7, 8, 9, 10, 11, 20, 24, 30]
GUID = "pending-edit-unrelated-bim-product"
PHYSICS_DT = 1 / 60
TICKS_PER_FRAME = 4
# Jolt 5.6 BodyCreationSettings::mLinearDamping defaults to .05. Its dynamic
# MotionProperties::ApplyForceTorqueAndDragInternal multiplies velocity by (1-d*dt)
# before each position integration. This prediction is independent of poses.
LINEAR_DAMPING = .05
EDIT = [4, 2, 0]


def displacement(prior_ticks, ticks):
    decay = 1 - LINEAR_DAMPING * PHYSICS_DT
    return PHYSICS_DT * decay ** (prior_ticks + 1) * (1 - decay ** ticks) / (1 - decay)


def expected_ticks(case, frame):
    resume = 9 if case == "pending-play" else 10
    return TICKS_PER_FRAME * (min(frame, 7) + max(0, frame - resume + 1))


def write_unrelated_product(path):
    points, _, indices = cube()
    source = [[x - 4, -z, y + 1] for x, y, z in points]
    path.write_text(json.dumps({"schema_version": "1.1.0", "info": {},
        "meshes": [{"mesh_id": 0, "coordinates": [value for point in source for value in point], "indices": indices}],
        "elements": [{"mesh_id": 0, "guid": GUID, "type": "IfcBuildingElementProxy",
            "vector": {"x": 0, "y": 0, "z": 0}, "rotation": {"qx": 0, "qy": 0, "qz": 0, "qw": 1},
            "color": {"r": 60, "g": 170, "b": 220, "a": 255}, "info": {"Name": "Unrelated detachable product"}}]}), encoding="utf-8")


def fixture(case, directory):
    if case not in CASES:
        raise ValueError("Unknown pending-edit condition")
    directory = Path(directory)
    directory.mkdir(parents=True, exist_ok=True)
    source, auxiliary = directory / f"{case}.gltf", None
    name = "Pending-edit animated source parent"
    geometry = write_gltf(source, [{"name": name, "mesh": 0, "translation": [0, 1, 0]}], [0])[name]
    events = [{"frame": 1, "physicsNode": geometry["node"], "physicsAttach": "dynamic", "physicsAction": "play",
               "physicsSettings": {"gravityFactor": 0, "linearVelocity": [1, 0, 0]}},
              {"frame": 8, "physicsAction": "pause"}]
    if case == "pending-play":
        events.append({"frame": 9, "physicsAction": "play"})
    else:
        auxiliary = directory / "unrelated-product.bim"
        write_unrelated_product(auxiliary)
        events += [{"frame": 2, "bimSelectedObject": 0, "physicsAttach": "dynamic",
                    "physicsSettings": {"gravityFactor": 0, "linearVelocity": [0, 0, 1]}},
                   {"frame": 9, "bimSelectedObject": 0, "physicsAction": "detach"},
                   {"frame": 10, "physicsAction": "play"},
                   {"frame": 11, "bimSelectedObject": 0, "physicsAttach": "static"}]
    events += [{"frame": 23, "physicsAction": "clear"}, {"frame": 27, "guiUnload": True}]
    events.sort(key=lambda event: event["frame"])
    sequence = {"schemaVersion": 1, "frames": 30, "sampleFrames": SAMPLE_FRAMES.copy(),
        "camera": [{"frame": frame, "position": [10, 8, 14], "target": [1, 2, 0]} for frame in (1, 30)],
        "objectNode": 0, "objectName": name,
        "objectTranslation": [{"frame": frame, "translation": [0, 0, 0] if frame < 9 else EDIT.copy()}
                              for frame in (1, 8, 9, 30)], "events": events}
    specification = {"case": case, "primary": geometry, "animatedName": name, "animatedPrimaryNode": 0,
        "bimGeometry": bim_source_geometry(auxiliary) if auxiliary else {},
        "clearFrame": 23, "unloadFrame": 27, "sampleFrames": SAMPLE_FRAMES.copy(),
        "ticksPerFrame": TICKS_PER_FRAME, "fixedRenderDeltaSeconds": TICKS_PER_FRAME * PHYSICS_DT}
    return source, auxiliary, sequence, specification


def check(record):
    import numpy as np
    assert not record.get("validationErrors"), "Pending-edit capture recorded validation errors"
    specification = record["fixture"]
    case = specification["case"]
    assert case in CASES and record["technique"] in TECHNIQUES and record["mode"] == "raster", "Unknown pending-edit capture"
    samples = {sample["frame"]: sample for sample in record["samples"]}
    assert sorted(samples) == SAMPLE_FRAMES and len(samples) == len(record["samples"]), "Incomplete pending-edit captures"
    primary_node = specification["primary"]["node"]
    states = {frame: sample["telemetry"]["physics"] for frame, sample in samples.items()}
    paused_primary = next(body for body in states[8]["bodies"] if body["node"] == primary_node)
    paused_position = np.asarray(paused_primary["position"], dtype=float)
    assert np.isfinite(paused_position).all() and paused_position.shape == (3,), "Invalid paused primary pose"
    auxiliary_count = samples[1]["telemetry"]["scene"]["auxiliaryObjectCount"]
    assert (auxiliary_count >= 1 if case == "pending-detach" else auxiliary_count == 0), "Unexpected pending-edit auxiliary geometry"
    identity = None
    for frame, sample in samples.items():
        state, scene = states[frame], sample["telemetry"]["scene"]
        assert state["available"] and not state.get("error"), f"Physics failed at frame {frame}"
        assert state.get("droppedSeconds", 0) == 0 and state["jointCount"] == 0, "Unexpected pending-edit simulation timing or joints"
        if frame >= specification["unloadFrame"]:
            assert state["bodyCount"] == 0 and not state.get("bodies") and not state.get("auxiliaryElements"), "Unload retained pending-edit bindings"
            assert scene["primaryObjectCount"] == scene["auxiliaryObjectCount"] == 0, "Unload retained pending-edit geometry"
            continue
        assert scene["primaryObjectCount"] == 1 and scene["auxiliaryObjectCount"] == auxiliary_count, "Pending edits changed imported geometry counts"
        if frame >= specification["clearFrame"]:
            assert state["bodyCount"] == 0 and not state["playing"] and not state.get("bodies") and not state.get("auxiliaryElements"), "Clear retained pending-edit bindings"
            continue
        auxiliary_bound = case == "pending-detach" and (2 <= frame <= 8 or frame >= 11)
        expected_count = 2 if auxiliary_bound else 1
        assert state["bodyCount"] == len(state["bodies"]) == expected_count, "Pending-edit body count changed"
        expected_playing = frame != 8 and not (case == "pending-detach" and frame == 9)
        assert state["playing"] == expected_playing, "Pending-edit playback state is incorrect"
        assert state["totalSteps"] == expected_ticks(case, frame), "Pending-edit fixed-step batch was interrupted"
        body = next(body for body in state["bodies"] if body["node"] == primary_node)
        assert body["motion"] == 2, "Primary pending-edit body is not dynamic"
        if frame <= 8:
            expected_position = np.array([displacement(0, state["totalSteps"]), 1, 0])
        else:
            ticks = state["totalSteps"] - states[8]["totalSteps"]
            expected_position = paused_position + np.asarray(EDIT) + [displacement(states[8]["totalSteps"], ticks), 0, 0]
        assert np.allclose(body["position"], expected_position, atol=3e-4, rtol=0), "Pending world edit or retained velocity integration was lost/reapplied"
        scene_model, rendered = matrix(body["sceneWorldMatrix"]), matrix(body["renderWorldMatrix"])
        expected_model = matrix(specification["primary"]["authoredModel"])
        expected_model[:3, 3] = expected_position
        assert np.allclose(scene_model, expected_model, atol=3e-4, rtol=0), "Primary scene transform lost the pending edit"
        assert np.allclose(rendered, scene_model, atol=1e-5, rtol=0), "Primary rendered pending-edit transform is stale"
        points = transform_vertices(rendered, specification["primary"]["vertices"])
        assert np.allclose((points.min(axis=0) + points.max(axis=0)) / 2, body["position"], atol=3e-4, rtol=0), "Primary geometry disagrees with its physical center"
        assert np.allclose(points.max(axis=0) - points.min(axis=0), [1, 1, 1], atol=3e-4, rtol=0), "Pending edit changed primary geometry size"
        elements = state.get("auxiliaryElements", [])
        if not auxiliary_bound:
            assert not elements, "Detached/unattached BIM binding remained published"
            continue
        assert len(elements) == 1 and len(elements[0]["objects"]) == 1, "Missing or split unrelated BIM product"
        element = elements[0]
        auxiliary = next(body for body in state["bodies"] if body["node"] == element["node"])
        expected_aux_position = [-4, 1, displacement(0, state["totalSteps"] - TICKS_PER_FRAME) if frame <= 8 else 0]
        assert auxiliary["motion"] == (2 if frame <= 8 else 0), "Unrelated BIM motion type changed"
        assert np.allclose(auxiliary["position"], expected_aux_position, atol=3e-4, rtol=0), "BIM detach did not restore its authored placement"
        proxy = matrix(auxiliary["sceneWorldMatrix"])
        assert np.allclose(proxy[:3, 3], auxiliary["position"], atol=3e-4, rtol=0), "BIM proxy disagrees with its physical position"
        assert np.allclose(proxy, translation(expected_aux_position), atol=3e-4, rtol=0), "BIM proxy lost its expected translation-only pose"
        obj = element["objects"][0]
        semantic = (element["productId"], obj["objectIndex"], obj["guid"], obj["sourceId"])
        if identity is None:
            identity = semantic
        assert identity == semantic and obj["objectIndex"] == 0 and obj["guid"] == GUID, "Detach changed BIM product identity"
        model = matrix(obj["model"])
        from_proxy = proxy @ np.linalg.inv(translation([-4, 1, 0])) @ matrix(BIM_ROTATION)
        assert np.allclose(model, from_proxy, atol=3e-4, rtol=0), "BIM rendered model disagrees with its complete proxy transform"
        expected = translation(np.asarray(expected_aux_position) - [-4, 1, 0]) @ matrix(BIM_ROTATION)
        assert np.allclose(model, expected, atol=3e-4, rtol=0), "BIM rendered model ignored authored restoration"
        vertices = transform_vertices(model, specification["bimGeometry"]["0"]["vertices"])
        assert np.allclose(obj["boundsMin"], vertices.min(axis=0), atol=3e-4, rtol=0) and np.allclose(obj["boundsMax"], vertices.max(axis=0), atol=3e-4, rtol=0), "BIM pending-edit bounds are stale"
        assert np.allclose(matrix(obj["normalMatrix"], columns=3)[:3, :3], np.linalg.inv(model[:3, :3]).T, atol=3e-4, rtol=0), "BIM pending-edit normals are stale"
    assert states[7]["totalSteps"] == states[8]["totalSteps"], "Pause advanced the pending-edit scene"
    hashes = []
    for frame in (7, 9, 20):
        contents = Path(samples[frame]["png"]).read_bytes()
        assert contents, "Empty pending-edit image"
        hashes.append(hashlib.sha256(contents).hexdigest())
    assert len(set(hashes)) == 3, "Pending edit/motion did not change captured PNGs"
    return {"passed": True, "captures": len(samples), "independentVelocityPrediction": True,
            "ticksPerPlayingFrame": TICKS_PER_FRAME, "imageSha256": hashes}


def check_matrix(results):
    expected = {tuple(case) for case in results["expectedCases"]}
    supported = {(case, technique) for case in CASES for technique in TECHNIQUES}
    actual = {(record["fixture"]["case"], record["technique"]): record for record in results["cases"]}
    assert expected and expected <= supported and len(expected) == len(results["expectedCases"]), "Unknown or duplicate pending-edit case matrix"
    assert set(actual) == expected and len(actual) == len(results["cases"]), "Incomplete pending-edit case matrix"
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
        print("Set CONTAINER_RUN_GPU_PHYSICS=1 for native pending-edit captures")
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
                # The shared helper puts its 1/60 argument first; the existing
                # sequential CLI parser deliberately applies this later value.
                extra = ["--fixed-dt", str(TICKS_PER_FRAME * PHYSICS_DT), "--directional-intensity", "2", "--environment-intensity", "1"]
                if auxiliary:
                    extra += ["--bim-model", str(auxiliary)]
                record = run_lifecycle_capture(exe, output, f"physics-pending-edit-{case}-{technique}", source,
                    sequence, technique, "raster", extra=extra)
                record["fixture"] = specification
                results["cases"].append(record)
                record["checks"] = check(record)
                report.write_text(json.dumps(results, indent=2), encoding="utf-8")
                print(json.dumps({"case": case, "technique": technique, **record["checks"]}), flush=True)
            assert hashlib.sha256(exe.read_bytes()).hexdigest() == results["executableSha256"], "Executable changed during pending-edit capture"
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
        print(f"Pending-edit validation failed: {error}", flush=True)
    report.write_text(json.dumps(results, indent=2), encoding="utf-8")
    return 0 if results["passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
