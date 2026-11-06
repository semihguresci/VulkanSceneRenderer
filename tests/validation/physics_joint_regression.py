"""Opt-in fixed/hinge/slider integration across primary and fragmented BIM bodies.

Creates small local fixtures and executes deterministic creation, motor reversal,
reset, removal, recreation, body replacement, endpoint detach and model unload.
"""
import argparse
import hashlib
import json
import math
import os
from pathlib import Path

from physics_auxiliary_regression import PRODUCT_GUID, matrix, write_fixtures

SAMPLES = [1, 3, 120, 240, 242, 244, 246, 248, 300, 302, 304]
TYPES = {"fixed": 0, "hinge": 1, "slider": 2}


def fixtures(directory, provider):
    floor, auxiliary = write_fixtures(directory)
    scene = json.loads(floor.read_text(encoding="utf-8"))
    scene["nodes"] = [{"name": "Joint static anchor", "mesh": 0,
                        "translation": [0, 4, 0], "scale": [.4, .4, .4]}]
    if provider == "primary":
        scene["nodes"].append({"name": "Joint dynamic bar", "mesh": 0,
                               "translation": [0, 3, 0], "scale": [3, 1, 1]})
    scene["nodes"].append({"name": "Joint visual floor", "mesh": 0,
                           "translation": [0, -.25, 0], "scale": [12, .5, 8]})
    scene["scenes"][0]["nodes"] = list(range(len(scene["nodes"])))
    source = Path(directory) / f"physics-joints-{provider}.gltf"
    source.write_text(json.dumps(scene), encoding="utf-8")
    return source, auxiliary["bim"] if provider == "bim" else None


def joint_settings(kind, provider):
    joint = {"type": kind, "bodyA": {"node": 1},
             "bodyB": {"node": 3} if provider == "primary" else {"bimObject": 0},
             "anchor": [0, 3, 0], "collideConnected": False}
    if kind != "fixed":
        joint.update({"axis": [0, 0, 1] if kind == "hinge" else [1, 0, 0],
                      "limits": [-.5, .5] if kind == "hinge" else [-.75, .75],
                      "motor": {"mode": "velocity", "targetVelocity": 1 if kind == "hinge" else .8,
                                "maxForceOrTorque": 1000, "frequencyHz": 2, "dampingRatio": 1}})
    return joint


def sequence(kind, provider):
    target = {"physicsNode": 3} if provider == "primary" else {"bimSelectedObject": 0}
    events = [{"frame": 1, "physicsNode": 1, "physicsAttach": "static"},
              {"frame": 2, **target, "physicsAttach": "dynamic"},
              {"frame": 3, "physicsJointCreate": joint_settings(kind, provider), "physicsAction": "play"}]
    if kind != "fixed":
        events.append({"frame": 121, "physicsJointMotorVelocity": -1 if kind == "hinge" else -.8})
    events += [{"frame": 241, "physicsAction": "reset"},
               {"frame": 243, "physicsJointAction": "remove"},
               {"frame": 245, "physicsJointCreate": joint_settings(kind, provider)},
               {"frame": 247, **target, "physicsAttach": "dynamic"},
               {"frame": 249, "physicsAction": "play"},
               {"frame": 301, **target, "physicsAction": "detach"},
               {"frame": 303, "guiUnload": True}]
    if kind != "fixed":
        events[-3]["physicsJointMotorVelocity"] = 1 if kind == "hinge" else .8
    return {"schemaVersion": 1, "frames": 304, "sampleFrames": SAMPLES,
            "camera": [{"frame": 1, "position": [7, 6, 12], "target": [0, 2.5, 0]},
                       {"frame": 304, "position": [7, 6, 12], "target": [0, 2.5, 0]}], "events": events}


def check(record):
    import numpy as np
    assert not record.get("validationErrors"), "Joint capture contains Vulkan/runtime validation errors"
    samples = {sample["frame"]: sample for sample in record["samples"]}
    assert sorted(samples) == SAMPLES and len(samples) == len(record["samples"]), "Incomplete joint captures"
    states = {frame: sample["telemetry"]["physics"] for frame, sample in samples.items()}
    for state in states.values():
        assert state["available"] and not state.get("error"), "Joint physics backend failed"
        assert state.get("droppedSeconds", 0) == 0, "Joint capture dropped fixed simulation time"
    kind, provider = record["joint"], record["provider"]
    dynamic_node = 3
    if provider == "bim":
        elements = states[3]["auxiliaryElements"]
        assert len(elements) == 1 and len(elements[0]["objects"]) == 2, "Joint must bind the whole fragmented BIM product"
        assert all(obj["guid"] == PRODUCT_GUID for obj in elements[0]["objects"]), "Joint included an unrelated BIM product"
        dynamic_node = elements[0]["node"]

    def body(frame, node):
        return next(value for value in states[frame]["bodies"] if value["node"] == node)

    def joint(frame):
        state = states[frame]
        assert state["jointCount"] == len(state["joints"]) == 1, f"Joint absent at frame {frame}"
        result = state["joints"][0]
        assert result["type"] in (kind, TYPES[kind]), "Wrong native joint type"
        assert result["nodeA"] == 1 and result["nodeB"] == dynamic_node, "Joint connected the wrong bodies"
        return result

    initial = joint(3)
    initial_proxy = matrix(body(3, dynamic_node)["sceneWorldMatrix"])
    initial_bim = {obj["objectIndex"]: obj for obj in states[3].get("auxiliaryElements", [{}])[0].get("objects", [])} \
        if provider == "bim" else {}
    reference = np.eye(4)
    reference[:3, 3] = (0, 3, 0)
    coordinates = {}
    for frame in (3, 120, 240, 242, 246, 248, 300):
        recorded_joint = joint(frame)
        assert states[frame]["bodyCount"] == 2, "Mechanism body count changed"
        anchor, moving = body(frame, 1), body(frame, dynamic_node)
        assert anchor["motion"] == 0 and moving["motion"] == 2
        assert np.allclose(anchor["position"], [0, 4, 0], atol=1e-5, rtol=0), "Joint moved its static anchor"
        visual = matrix(moving["sceneWorldMatrix"])
        assert np.allclose(visual[:3, 3], moving["position"], atol=1e-4, rtol=0), "Joint body and scene pose disagree"
        if provider == "primary":
            assert np.allclose(matrix(moving["renderWorldMatrix"]), visual, atol=1e-5, rtol=0), "Joint primary mesh transform is stale"
            rotation = visual[:3, :3] / np.asarray([3, 1, 1])[None, :]
        else:
            rotation = visual[:3, :3]
            group = next(element for element in states[frame]["auxiliaryElements"] if element["node"] == dynamic_node)
            assert {obj["objectIndex"] for obj in group["objects"]} == initial_bim.keys(), "Joint lost a product fragment"
            delta = visual @ np.linalg.inv(initial_proxy)
            authored_delta = visual @ np.linalg.inv(reference)
            for obj in group["objects"]:
                original = initial_bim[obj["objectIndex"]]
                assert (obj["guid"], obj["sourceId"]) == (original["guid"], original["sourceId"]), "Joint changed BIM identity"
                model = matrix(obj["model"])
                assert np.allclose(model, delta @ matrix(original["model"]), atol=3e-4, rtol=0), "Joint BIM rendered transform is stale"
                normals = matrix(obj["normalMatrix"], columns=3)[:3, :3]
                assert np.allclose(normals, np.linalg.inv(model[:3, :3]).T, atol=3e-4, rtol=0), "Joint rotation left stale BIM normals"
                # Frame 3 has already simulated one step. Use the independently
                # authored cube geometry rather than rotate its captured AABB,
                # which would compound AABB expansion at later hinge angles.
                assert obj["objectIndex"] in (0, 1), "Unexpected fixture mesh ordering"
                center_x = -1 if obj["objectIndex"] == 0 else 1
                corners = np.array([[x, y, z, 1] for x in (center_x - .5, center_x + .5)
                                    for y in (2.5, 3.5) for z in (-.5, .5)])
                transformed = (authored_delta @ corners.T).T[:, :3]
                assert np.allclose(obj["boundsMin"], transformed.min(axis=0), atol=5e-4, rtol=0), "Joint rotation left stale BIM minimum bounds"
                assert np.allclose(obj["boundsMax"], transformed.max(axis=0), atol=5e-4, rtol=0), "Joint rotation left stale BIM maximum bounds"
        angle = math.atan2(float(rotation[1, 0]), float(rotation[0, 0]))
        if kind == "hinge":
            assert np.allclose(moving["position"], [0, 3, 0], atol=.025, rtol=0), "Hinge anchor drifted"
            assert np.allclose(rotation[:, 2], [0, 0, 1], atol=.025, rtol=0), "Hinge gained a forbidden rotation"
            coordinate, limit = angle, .5
        else:
            assert np.allclose(rotation, np.eye(3), atol=.02, rtol=0), "Fixed/slider joint allowed body rotation"
            assert abs(moving["position"][1] - 3) < .025 and abs(moving["position"][2]) < .025, "Joint lost transverse position lock"
            coordinate, limit = float(moving["position"][0]), .75 if kind == "slider" else 0
        if kind == "fixed":
            assert abs(coordinate) < .025, "Fixed joint lost relative translation"
        else:
            assert abs(coordinate) <= limit + .05, "Joint exceeded its motion limits"
            assert abs(abs(float(recorded_joint["coordinate"])) - abs(coordinate)) < .04, "Joint coordinate differs from rendered motion"
        coordinates[frame] = coordinate
    if kind != "fixed":
        limit = .5 if kind == "hinge" else .75
        assert abs(coordinates[120]) > limit - .08, "Velocity motor did not reach its first stop"
        assert abs(coordinates[240]) > limit - .08 and coordinates[120] * coordinates[240] < 0, "Motor reversal did not reach the opposite stop"
        assert abs(coordinates[300]) > limit - .08, "Joint motor failed after body replacement"
    assert not states[242]["playing"] and abs(coordinates[242]) < .025, "Reset did not restore and pause the mechanism"
    assert joint(242)["id"] == initial["id"], "Reset changed the stable scene joint identity"
    assert states[244]["jointCount"] == 0 and not states[244]["joints"], "Explicit joint removal failed"
    recreated = joint(246)["id"]
    assert recreated != initial["id"] and joint(248)["id"] == recreated, "Body replacement retired or recycled the scene joint"
    assert states[302]["jointCount"] == 0 and states[302]["bodyCount"] == 1, "Endpoint detach left a dangling constraint"
    assert states[304]["jointCount"] == states[304]["bodyCount"] == 0, "Unload retained a mechanism"
    if record["mode"] == "rays-taa":
        assert samples[240]["telemetry"]["rayShadows"]["active"] and samples[240]["telemetry"]["taa"]["enabled"], "Joint ray/TAA path was not active"
    hashes = [hashlib.sha256(Path(samples[frame]["png"]).read_bytes()).hexdigest() for frame in (120, 240)]
    if kind != "fixed":
        assert len(set(hashes)) == 2, "Moving joint did not change rendered pixels"
    return {"passed": True, "captures": len(samples), "independentCoordinates": coordinates,
            "resetRemovalReplaceDetachUnload": True, "imageSha256": hashes}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--exe", type=Path)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--require-opt-in", action="store_true")
    parser.add_argument("--check-existing", action="store_true")
    parser.add_argument("--joint", choices=tuple(TYPES))
    parser.add_argument("--provider", choices=("primary", "bim"))
    parser.add_argument("--technique", choices=("deferred-raster", "forward-raster"))
    parser.add_argument("--include-ray-taa", action="store_true", help="Add two rotating BIM hinge cases to exercise ray/TAA transforms")
    args = parser.parse_args()
    if args.require_opt_in and os.environ.get("CONTAINER_RUN_GPU_PHYSICS") != "1":
        print("Set CONTAINER_RUN_GPU_PHYSICS=1 to run native joint captures")
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
        cases = [(kind, provider, technique, "raster")
                 for kind in ((args.joint,) if args.joint else TYPES)
                 for provider in ((args.provider,) if args.provider else ("primary", "bim"))
                 for technique in ((args.technique,) if args.technique else ("deferred-raster", "forward-raster"))]
        if args.include_ray_taa:
            cases += [(kind, provider, technique, "rays-taa") for kind, provider, technique, _ in cases
                      if kind == "hinge" and provider == "bim"]
        results = {"schemaVersion": 1, "passed": False, "expectedCases": cases, "cases": [],
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
            for kind, provider, technique, mode in cases:
                source, auxiliary = fixtures(output / "fixtures" / provider, provider)
                name = f"physics-joint-{kind}-{provider}-{technique}-{mode}"
                extra = ["--directional-intensity", "2", "--environment-intensity", "1"]
                if auxiliary:
                    extra += ["--bim-model", str(auxiliary)]
                record = run_lifecycle_capture(exe, output, name, source, sequence(kind, provider), technique, mode, extra=extra)
                record.update({"joint": kind, "provider": provider})
                results["cases"].append(record)
                record["checks"] = check(record)
                print(json.dumps({"case": name, **record["checks"]}), flush=True)
                report.write_text(json.dumps(results, indent=2), encoding="utf-8")
            assert hashlib.sha256(exe.read_bytes()).hexdigest() == results["executableSha256"], "Executable changed during joint captures"
        else:
            cases = {(record["joint"], record["provider"], record["technique"], record["mode"]): record for record in results["cases"]}
            expected = {tuple(case) for case in results["expectedCases"]}
            assert expected and len(cases) == len(results["cases"]) == len(expected) and set(cases) == expected, "Incomplete/duplicate joint captures"
            for record in cases.values():
                for sample in record["samples"]:
                    local = output / Path(sample["png"]).name
                    if local.is_file():
                        sample["png"] = str(local)
                record["checks"] = check(record)
        results["passed"] = True
    except Exception as error:
        results["failure"] = str(error)
        print(f"Joint validation failed: {error}", flush=True)
    report.write_text(json.dumps(results, indent=2), encoding="utf-8")
    return 0 if results["passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
