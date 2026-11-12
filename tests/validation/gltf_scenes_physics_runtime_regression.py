"""Verify native physics and picking while switching between glTF scenes."""

import argparse
import json
from pathlib import Path

from light_editor_history_runtime import run, write_json
from physics_scene_diversity_regression import write_gltf


PRIMARY = "Original scene body"
OTHER = "Other scene box"
PIXEL = [320, 180]
INVALID_NODE = 2**32 - 1
TECHNIQUES = ("deferred-raster", "forward-raster")


def fixture(path):
    geometry = write_gltf(path, [dict(name=PRIMARY, mesh=0), dict(name=OTHER, mesh=0)], [0, 1])
    model = json.loads(path.read_text(encoding="utf-8"))
    model["scenes"] = [dict(name="Original", nodes=[0]), dict(name="Other", nodes=[1]),
                       dict(name="Empty", nodes=[])]
    model["extensionsUsed"] = ["KHR_materials_unlit"]
    model["materials"][0]["extensions"] = {"KHR_materials_unlit": {}}
    write_json(path, model)
    return geometry[PRIMARY]["node"]


def scenario(body_node):
    return [
        dict(frame=1, physicsAttach="dynamic", physicsNode=body_node, physicsAction="play",
             physicsSettings=dict(gravityFactor=0, linearVelocity=[1, 0, 0])),
        dict(frame=2, viewportPointer=dict(action="select", pixel=PIXEL), depthProbe=PIXEL),
        dict(frame=3, gltfScene=1),
        dict(frame=4, viewportPointer=dict(action="select", pixel=PIXEL), depthProbe=PIXEL),
        dict(frame=5, physicsAction="play"),
        dict(frame=6, physicsAction="step"),
        dict(frame=7, physicsAction="demo"),
        dict(frame=8, gltfScene=2),
        dict(frame=9, depthProbe=PIXEL),
        dict(frame=10, gltfScene=0),
        dict(frame=11, depthProbe=PIXEL),
        dict(frame=12, physicsAction="step"),
        dict(frame=13, physicsAction="play"),
        dict(frame=14, physicsAction="pause"),
        dict(frame=15, physicsAction="demo"),
        dict(frame=16, gltfScene=1),
        dict(frame=17, physicsAction="demo"),
        dict(frame=18, physicsAction="clear"),
        dict(frame=19, physicsAction="demo"),
        dict(frame=20, gltfScene=0),
    ]


def check_pick(state, source):
    interaction = state["depthProbe"]["sceneInteraction"]
    assert type(interaction["rawGpuId"]) is int and interaction["rawGpuId"] > 0, interaction
    assert interaction["cpu"] == interaction["gpu"], interaction
    hit = interaction["gpu"]["hit"]
    assert hit["sourceNode"] == source and hit["sourceMesh"] == 0, hit
    return hit["graphNode"]


def check(states, technique, body_node):
    physics = {frame: state["physics"] for frame, state in states.items()}
    bodies = {frame: {body["node"]: body for body in data["bodies"]} for frame, data in physics.items()}
    original = bodies[2][body_node]
    original_steps = physics[2]["totalSteps"]
    assert physics[2]["playing"] and original_steps > 0 and original["position"][0] > 0
    assert all(data["available"] and data["initialized"] for data in physics.values())
    assert all(states[frame]["gltfMerge"]["graphIdentity"] == states[2]["gltfMerge"]["graphIdentity"]
               for frame in states)
    assert all(bodies[frame][body_node]["id"] == original["id"] for frame in range(2, 18))

    for frame in range(3, 12):
        data = physics[frame]
        body = bodies[frame][body_node]
        assert not data["playing"] and data["bodyCount"] == 1 and data["totalSteps"] == original_steps, (frame, data)
        for field in ("position", "rotation", "settings", "sceneWorldMatrix"):
            assert body[field] == original[field], (frame, field, body, original)
        assert body["renderWorldMatrix"] == (original["renderWorldMatrix"] if frame >= 10 else []), (frame, body)
    for frame in (5, 6, 7):
        assert "belong to another glTF scene" in physics[frame]["error"], (frame, physics[frame])
        assert states[frame]["gltfMerge"]["graphNodeCount"] == states[4]["gltfMerge"]["graphNodeCount"]

    assert check_pick(states[2], 0) == body_node
    assert states[2]["gui"]["selectedMeshNodeId"] == body_node
    assert states[3]["gui"]["selectedMeshNodeId"] == INVALID_NODE
    other_node = check_pick(states[4], 1)
    assert other_node != body_node and states[4]["gui"]["selectedMeshNodeId"] == other_node
    assert states[9]["gltfScenes"]["selected"] == 2 and states[9]["gltfScenes"]["activeRenderableCount"] == 0
    assert states[9]["gui"]["selectedMeshNodeId"] == INVALID_NODE
    background = dict(hit=None, selection=None, hover=None)
    empty = states[9]["depthProbe"]["sceneInteraction"]
    assert empty["cpu"] == background, empty
    if technique == "deferred-raster":
        assert empty["rawGpuId"] == 0 and empty["gpu"] == background, empty
    else:
        assert empty["rawGpuId"] is None and empty["gpu"] is None, empty
    assert check_pick(states[11], 0) == body_node

    assert physics[12]["totalSteps"] == original_steps + 1 and not physics[12]["playing"]
    assert bodies[12][body_node]["position"][0] > original["position"][0]
    assert physics[13]["playing"] and physics[13]["totalSteps"] > physics[12]["totalSteps"]
    assert not physics[14]["playing"]
    assert all(not physics[frame]["error"] for frame in (12, 13, 14, 15))
    assert physics[15]["playing"] and physics[15]["bodyCount"] == 5
    demo = {body["name"] for node, body in bodies[15].items() if node != body_node}
    assert demo == {"Physics floor", "Physics box 1", "Physics box 2", "Physics box 3"}, demo
    assert states[15]["gltfScenes"]["activeRenderableCount"] == 5

    for frame in (16, 17):
        assert not physics[frame]["playing"] and physics[frame]["totalSteps"] == physics[15]["totalSteps"]
        assert physics[frame]["bodyCount"] == 5 and bodies[frame].keys() == bodies[15].keys()
        assert states[frame]["gltfScenes"]["selected"] == 1 and states[frame]["gltfScenes"]["activeRenderableCount"] == 1
        for node, body in bodies[frame].items():
            assert body["id"] == bodies[15][node]["id"] and body["position"] == bodies[15][node]["position"], (frame, body)
            assert body["renderWorldMatrix"] == [], (frame, body)
    assert "belong to another glTF scene" in physics[17]["error"]
    assert states[17]["gltfMerge"]["graphNodeCount"] == states[16]["gltfMerge"]["graphNodeCount"]

    # Clearing bodies leaves the first demo's geometry in A. A new demo in B
    # must create its own active geometry rather than reuse those nodes.
    assert not physics[18]["playing"] and not physics[18]["bodyCount"] and not physics[18]["error"]
    assert physics[19]["playing"] and physics[19]["bodyCount"] == 4 and not physics[19]["error"]
    assert {body["name"] for body in bodies[19].values()} == demo
    assert bodies[19].keys().isdisjoint(bodies[15])
    assert states[19]["gltfScenes"]["activeRenderableCount"] == 5
    assert states[19]["gltfMerge"]["graphNodeCount"] == states[18]["gltfMerge"]["graphNodeCount"] + 5
    assert not physics[20]["playing"] and physics[20]["totalSteps"] == physics[19]["totalSteps"]
    assert bodies[20].keys() == bodies[19].keys()
    assert all(body["renderWorldMatrix"] == [] for body in bodies[20].values())
    assert states[20]["gltfScenes"]["selected"] == 0 and states[20]["gltfScenes"]["activeRenderableCount"] == 5
    return dict(technique=technique, retainedBodyId=original["id"], pausedSteps=original_steps,
                resumedSteps=physics[13]["totalSteps"], successfulDemoScenes=[0, 1], captureCount=len(states))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--renderer", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--technique", action="append", choices=TECHNIQUES)
    args = parser.parse_args()
    renderer, output = args.renderer.resolve(strict=True), args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    (output / "vk_layer_settings.txt").write_text("khronos_validation.validate_sync = true\n", encoding="utf-8")
    source = output / "physics-scenes.gltf"
    body_node = fixture(source)
    report = dict(passed=False, checks=[])
    try:
        for technique in args.technique or TECHNIQUES:
            frames = run(renderer, output, technique, source, technique, list(range(2, 21)),
                         scenario(body_node), ("--no-ray-query",), fix_camera=True)
            states = {frame: state for frame, (_, state) in frames.items()}
            assert all(state["validationEnabled"] for state in states.values())
            report["checks"].append(check(states, technique, body_node))
            print(f"PASS {technique} multi-scene physics, demo guard and picking", flush=True)
        report["passed"] = True
    except Exception as error:
        report["failure"] = str(error)
        raise
    finally:
        write_json(output / "results.json", report)


if __name__ == "__main__":
    main()
