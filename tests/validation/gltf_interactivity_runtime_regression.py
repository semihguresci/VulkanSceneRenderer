"""Validate authored behavior graph publication in both Vulkan raster techniques."""

import argparse
import json
from pathlib import Path

import numpy as np
from PIL import Image

from gltf_accessor_runtime_regression import load_asset
from gltf_animation_runtime_regression import write_fixture
from gltf_material_variants_runtime_regression import run
from gltf_node_flags_runtime_regression import ANIMATED_MESH, ANIMATED_PARENT, FLAGS, POINTER, animated_fixture


BOOLEAN_SAMPLES = {
    1: (True, (True, True, True)), 2: (True, (False, True, True)),
    3: (False, (True, True, True)), 5: (True, (True, False, True)),
    6: (True, (True, True, False)), 7: (False, (True, True, True)),
}


def config(*items):
    return {"value": list(items)}


def constant(type_index, *items):
    return {"type": type_index, "value": list(items)}


def branch(index, socket=None):
    return {"node": index} if socket is None else {"node": index, "socket": socket}


def fixture(output):
    path = output / "behavior.gltf"
    write_fixture(path)
    asset = json.loads(path.read_text(encoding="utf-8"))
    asset["extensionsUsed"].append("KHR_materials_unlit")
    asset["materials"][0]["extensions"] = {"KHR_materials_unlit": {}}
    reference = output / "rest.gltf"
    reference.write_text(json.dumps(asset, indent=2), encoding="utf-8")
    asset["extensionsUsed"].append("KHR_interactivity")
    asset["extensionsRequired"] = ["KHR_interactivity"]
    target = {"pointer": config("/materials/0/pbrMetallicRoughness/baseColorFactor"), "type": config(0)}
    clip = {"animation": constant(1, "/animations/0")}
    asset["extensions"]["KHR_interactivity"] = {"graph": 0, "graphs": [{
        "types": [{"signature": "float4"}, {"signature": "ref"}],
        "events": [{"id": "stop"}, {"id": "recolor"}],
        "declarations": [{"op": operation} for operation in (
            "event/onStart", "pointer/set", "animation/start", "event/receive", "animation/stop")],
        "nodes": [
            {"declaration": 0, "flows": {"out": branch(1)}},
            {"declaration": 1, "configuration": target,
             "values": {"value": constant(0, 1, .02, .02, 1)}, "flows": {"out": branch(2)}},
            {"declaration": 2, "values": clip},
            {"declaration": 3, "configuration": {"event": config(0)}, "flows": {"out": branch(4)}},
            {"declaration": 4, "values": clip},
            {"declaration": 3, "configuration": {"event": config(1)}, "flows": {"out": branch(6)}},
            {"declaration": 1, "configuration": target, "values": {"value": constant(0, .02, 1, .02, 1)}},
        ],
    }]}
    path.write_text(json.dumps(asset, indent=2), encoding="utf-8")
    sequence = output / "behavior-sequence.json"
    sequence.write_text(json.dumps({
        "schemaVersion": 1, "frames": 31, "sampleFrames": [1, 12, 16, 23, 29],
        "events": [
            {"frame": 14, "interactivityEvent": "stop"},
            {"frame": 18, "interactivityEvent": "recolor"},
            {"frame": 27, "interactivityReset": True},
        ],
    }), encoding="utf-8")
    return path, reference, sequence


def pixels(record):
    with Image.open(record["image"]) as image:
        return np.asarray(image.convert("RGB"), dtype=np.int16)


def center_rgb(image):
    y, x = (extent // 2 for extent in image.shape[:2])
    return np.median(image[y-4:y+4, x-4:x+4], axis=(0, 1)).astype(int).tolist()


def color_mask(image, channel):
    return (image[:, :, channel] > 64) & (image[:, :, channel] - image[:, :, 2] > 32)


def check_portability(renderer, output, graph, reference, sequence, saved, technique, cohort, captures, extra):
    reloaded = run(renderer, output, saved, f"{technique}-reloaded", technique, "lit", captures,
                   sequence=sequence, extra=extra)
    assert len(reloaded) == len(cohort)
    for actual, expected in zip(reloaded, cohort):
        assert actual["state"]["gltfInteractivity"] == expected["state"]["gltfInteractivity"]
        np.testing.assert_array_equal(pixels(actual), pixels(expected))

    base = output / "merge-base.gltf"
    base_document = json.loads(reference.read_text(encoding="utf-8"))
    base_document["nodes"][0]["translation"] = [10, 0, 0]
    base.write_text(json.dumps(base_document), encoding="utf-8")
    merge_sequence = output / "merge-sequence.json"
    merge_sequence.write_text(json.dumps({
        "schemaVersion": 1, "frames": 39, "sampleFrames": [1, 8, 15, 22, 26, 30, 36],
        "events": [
            {"frame": 4, "merge": {"path": str(graph), "parentName": "Behavior placement"}, "guiWaitForLoad": True},
            {"frame": 17, "interactivityEvent": "stop"},
            {"frame": 20, "interactivityEvent": "recolor"},
            {"frame": 28, "sceneHistory": "undo"},
            {"frame": 34, "sceneHistory": "redo"},
        ],
    }), encoding="utf-8")
    merged = run(renderer, output, base, f"{technique}-merged", technique, "lit", captures,
                 sequence=merge_sequence, extra=extra)
    assert len(merged) == 7
    states = [record["state"]["gltfInteractivity"] for record in merged]
    assert [state["available"] for state in states] == [False, True, True, True, True, False, True], states
    assert all(not state["diagnostic"] for state in states), states
    assert [states[i]["activeClips"] for i in (1, 2, 3, 4, 6)] == [1, 1, 0, 0, 1], states
    first, moved = [color_mask(pixels(merged[i]), 0) for i in (1, 2)]
    assert first.sum() > 100 and moved.sum() > 100
    assert np.where(moved)[1].mean() > np.where(first)[1].mean() + 5
    stopped = color_mask(pixels(merged[3]), 1)
    assert stopped.sum() > 100
    np.testing.assert_array_equal(stopped, color_mask(pixels(merged[4]), 1))
    assert color_mask(pixels(merged[5]), 1).sum() == 0
    assert color_mask(pixels(merged[6]), 0).sum() > 100
    return {"reloaded": len(reloaded), "mergeStates": states}


def boolean_fixture(output):
    path = output / "boolean-behavior.gltf"
    animated_fixture(path)
    model = json.loads(path.read_text(encoding="utf-8"))
    del model["animations"]
    model["extensionsUsed"].remove(POINTER)
    model["extensionsUsed"].append("KHR_interactivity")
    model["extensionsRequired"] = ["KHR_interactivity"]
    del model["nodes"][1]["extensions"][FLAGS["visible"]]["visible"]

    def target(member):
        return {"pointer": config(f"/nodes/1/extensions/{FLAGS[member]}/{member}"), "type": config(0)}

    def receive(event, destination):
        return {"declaration": 0, "configuration": {"event": config(event)}, "flows": {"out": branch(destination)}}

    def write(member, value, **flows):
        node = {"declaration": 3, "configuration": target(member), "values": {"value": value}}
        if flows:
            node["flows"] = flows
        return node

    model["extensions"]["KHR_interactivity"] = {"graph": 0, "graphs": [{
        "types": [{"signature": "bool"}],
        "events": [{"id": name} for name in ("hide", "blockSelection", "blockHover")],
        "declarations": [{"op": op} for op in ("event/receive", "pointer/get", "flow/branch", "pointer/set")],
        "nodes": [
            receive(0, 2), {"declaration": 1, "configuration": target("visible")},
            {"declaration": 2, "values": {"condition": branch(1)}, "flows": {"true": branch(3)}},
            write("visible", constant(0, False)),
            receive(1, 5), write("selectable", constant(0, False)),
            receive(2, 7), write("selectable", branch(1), out=branch(8)),
            write("hoverable", constant(0, False)),
        ],
    }]}
    path.write_text(json.dumps(model, indent=2), encoding="utf-8")
    sequence = output / "boolean-behavior-sequence.json"
    sequence.write_text(json.dumps({"schemaVersion": 1, "frames": max(BOOLEAN_SAMPLES),
        "sampleFrames": list(BOOLEAN_SAMPLES), "events": [
            {"frame": 1, "interactivityPlaying": True}, {"frame": 2, "interactivityEvent": "hide"},
            {"frame": 3, "interactivityReset": True}, {"frame": 4, "interactivityPlaying": True},
            {"frame": 5, "interactivityEvent": "blockSelection"}, {"frame": 6, "interactivityEvent": "blockHover"},
            {"frame": 7, "interactivityReset": True},
        ]}), encoding="utf-8")
    return path, sequence


def inspect_boolean_behavior(record, playing, expected):
    state = record["state"]
    graph = state["gltfInteractivity"]
    assert graph["available"] and not graph["diagnostic"], graph
    assert graph["playing"] is playing and graph["started"] is playing, graph
    flags = state["gltfNodeFlags"]
    assert not flags["truncated"], flags
    rows = {node["graphNode"]: node for node in flags["nodes"]}
    source = {node["name"]: node for node in flags["nodes"] if node["sourceIndex"] >= 0}
    parent, child = source[ANIMATED_PARENT], source[ANIMATED_MESH]
    assert child["parentGraphNode"] == parent["graphNode"], (parent, child)
    for name, value in zip(FLAGS, expected, strict=True):
        key = name.capitalize()
        assert parent["local"+key] is value and child["local"+key] is True and child["effective"+key] is value, (name, parent, child)
    visible, selectable, hoverable = expected
    assert state["gltfScenes"]["pointLightCount"] == int(visible), state["gltfScenes"]
    assert state["gltfScenes"]["editHistoryDepth"] == 0, state["gltfScenes"]

    def below_child(probe):
        node = probe["graphNode"]
        while node in rows:
            if node == child["graphNode"]:
                return True
            node = rows[node]["parentGraphNode"]
        return False

    probes = [probe for probe in flags["probes"] if below_child(probe)]
    if visible:
        assert len(probes) == 1 and probes[0]["hit"], probes
        probe = probes[0]
        hit = probe["nearestHitGraphNode"]
        assert hit == probe["graphNode"], probe
        assert probe["resolvedSelection"] == (hit if selectable else -1), probe
        assert probe["resolvedHover"] == (hit if hoverable else -1), probe
    else:
        assert not any(probe["hit"] for probe in probes), probes


def run_boolean_behavior(renderer, output, *, generate_only, portability):
    source, sequence = boolean_fixture(output)
    if generate_only:
        return
    captures, results = [], {}
    for technique, suffix in (("deferred-raster", ".gltf"), ("forward-raster", ".glb")):
        destination = output / f"{technique}-boolean{suffix}" if portability else None
        cohort = run(renderer, output, source, f"{technique}-boolean-source", technique, "lit", captures,
                     sequence=sequence, save=destination)
        baseline = pixels(cohort[0])
        rows = []
        for (frame, (playing, expected)), record in zip(BOOLEAN_SAMPLES.items(), cohort, strict=True):
            inspect_boolean_behavior(record, playing, expected)
            changed = float(np.any(np.abs(pixels(record) - baseline) > 5, axis=2).mean())
            if expected[0]:
                np.testing.assert_array_equal(pixels(record), baseline)
            else:
                assert changed > .015, (frame, changed)
            rows.append({"frame": frame, "flags": expected, "changedPixelFraction": changed})
        results[technique] = {"samples": rows}
        if destination:
            saved, _ = load_asset(destination)
            parent, node = next((i, node) for i, node in enumerate(saved["nodes"]) if node.get("name") == ANIMATED_PARENT)
            visible = node["extensions"][FLAGS["visible"]]
            assert "visible" not in visible and visible["extras"]["authoringTag"] == "visible", visible
            graph = saved["extensions"]["KHR_interactivity"]["graphs"][0]
            pointers = {node["configuration"]["pointer"]["value"][0] for node in graph["nodes"]
                        if "pointer" in node.get("configuration", {})}
            assert pointers == {f"/nodes/{parent}/extensions/{extension}/{member}" for member, extension in FLAGS.items()}, pointers
            reloaded = run(renderer, output, destination, f"{technique}-boolean-reload", technique, "lit", captures,
                           sequence=sequence)
            for (playing, expected), before, after in zip(BOOLEAN_SAMPLES.values(), cohort, reloaded, strict=True):
                inspect_boolean_behavior(after, playing, expected)
                np.testing.assert_array_equal(pixels(before), pixels(after))
            results[technique]["portableFormat"] = suffix
        print("PASS Boolean behavior", technique, flush=True)
    (output / "results.json").write_text(json.dumps({"passed": True, "captures": captures, "results": results},
                                                     indent=2), encoding="utf-8")


def unlit_graph_fixture(path):
    write_fixture(path)
    model = json.loads(path.read_text(encoding="utf-8"))
    del model["animations"], model["cameras"]
    model["nodes"] = model["nodes"][:1]
    model["scenes"][0]["nodes"] = [0]
    model["extensions"] = {}
    model["extensionsUsed"] = ["KHR_materials_unlit", "KHR_interactivity"]
    model["extensionsRequired"] = ["KHR_interactivity"]
    material = model["materials"][0]
    material["extensions"] = {"KHR_materials_unlit": {}}
    return model


def scheduler_fixtures(output):
    delay_path = output / "scheduler-delay.gltf"
    model = unlit_graph_fixture(delay_path)
    red, green, blue = (1, .02, .02, 1), (.02, 1, .02, 1), (.02, .02, 1, 1)
    model["materials"][0]["pbrMetallicRoughness"]["baseColorFactor"] = red
    target = {"pointer": config("/materials/0/pbrMetallicRoughness/baseColorFactor"), "type": config(1)}
    operations = ("event/onStart", "event/receive", "flow/sequence", "flow/setDelay",
                  "flow/cancelDelay", "pointer/interpolate", "pointer/set")

    def node(operation, **members):
        return {"declaration": operations.index(operation), **members}

    def receive(event, destination, socket=None):
        return node("event/receive", configuration={"event": config(event)},
                    flows={"out": branch(destination, socket)})

    def delay(duration, done):
        return node("flow/setDelay", values={"duration": constant(0, duration)}, flows={"done": branch(done)})

    def write(color):
        return node("pointer/set", configuration=target, values={"value": constant(1, *color)})

    def save(path, nodes, event_names, sample_frames, events):
        model["extensions"]["KHR_interactivity"] = {"graph": 0, "graphs": [{
            "types": [{"signature": "float"}, {"signature": "float4"}],
            "events": [{"id": event} for event in event_names],
            "declarations": [{"op": operation} for operation in operations],
            "nodes": nodes,
        }]}
        path.write_text(json.dumps(model, indent=2), encoding="utf-8")
        sequence = path.with_suffix(".sequence.json")
        sequence.write_text(json.dumps({"schemaVersion": 1, "frames": sample_frames[-1],
                                        "sampleFrames": sample_frames, "events": events}, indent=2), encoding="utf-8")
        return path, sequence, sample_frames

    delays = save(delay_path, [
        node("event/onStart", flows={"out": branch(4)}),
        receive(0, 5), receive(1, 5, "cancel"), receive(2, 8),
        node("flow/sequence", flows={"a": branch(5), "b": branch(6), "c": branch(7)}),
        # The control paints green before any cancelled delay could repaint the cube.
        delay(.275, 9), delay(.375, 10), delay(.125, 11),
        node("flow/cancelDelay", values={"delay": branch(6, "lastDelay")}),
        write(red), write(blue), write(green),
    ], ("repeat", "cancelAll", "cancelOne"), (1, 4, 7, 8, 10), [
        {"frame": 2, "interactivityEvent": "repeat"},
        {"frame": 3, "interactivityEvent": "cancelAll"},
        {"frame": 4, "interactivityEvent": "cancelOne"},
    ])
    override = save(output / "scheduler-override.gltf", [
        node("event/onStart", flows={"out": branch(2)}),
        receive(0, 3),
        node("pointer/interpolate", configuration=target,
             values={"value": constant(1, *blue), "duration": constant(0, .35)}, flows={"done": branch(4)}),
        write(green), write(red),
    ], ("override",), (1, 3, 4, 9), [{"frame": 4, "interactivityEvent": "override"}])
    return {"delays": delays, "override": override}


def check_scheduler(case, frames, cohort):
    rows, images = [], []
    for frame, record in zip(frames, cohort, strict=True):
        state = record["state"]["gltfInteractivity"]
        assert state["available"] and state["playing"] and state["started"] and not state["diagnostic"], state
        assert abs(state["time"] - (frame - 1) * .05) < 1e-6, (frame, state)
        image = pixels(record)
        rows.append({"frame": frame, "graphTime": state["time"], "centerRgb": center_rgb(image)})
        images.append(image)

    green_frame = 1 if case == "delays" else 2
    for index, channel in ((0, 0), (green_frame, 1)):
        rgb = rows[index]["centerRgb"]
        assert rgb[channel] > 64 and rgb[channel] - max(rgb[:channel] + rgb[channel+1:]) > 32, rows
    if case == "override":
        assert rows[1]["centerRgb"][2] > rows[0]["centerRgb"][2] + 20, rows
    for image in images[green_frame+1:]:
        np.testing.assert_array_equal(image, images[green_frame])
    return rows


def run_scheduler(renderer, output, *, generate_only):
    fixtures = scheduler_fixtures(output)
    if generate_only:
        return
    captures, results, errors = [], {}, []
    extra = ("--import-scale", "1", "--sky", "plain", "--sky-horizon", "0", "0", "0", "--fixed-dt", ".05")
    for technique in ("deferred-raster", "forward-raster"):
        results[technique] = {}
        for case, (fixture_path, sequence, frames) in fixtures.items():
            try:
                cohort = run(renderer, output, fixture_path, f"{technique}-scheduler-{case}",
                             technique, "lit", captures, sequence=sequence, extra=extra, require_validation=True)
                results[technique][case] = {"samples": check_scheduler(case, frames, cohort)}
            except (AssertionError, RuntimeError) as error:
                results[technique][case] = {"error": str(error)}
                errors.append(f"{technique} {case}: {error}")
    report = {"passed": not errors, "captureCount": len(captures), "captures": captures, "results": results}
    (output / "results.json").write_text(json.dumps(report, indent=2), encoding="utf-8")
    if errors:
        raise AssertionError("\n".join(errors))
    print("PASS delay cancellation and explicit pointer writes in both raster paths", flush=True)


VIEWPORT_CENTER = (320, 180)
VIEWPORT_BACKGROUND = (24, 24)
VIEWPORT_ACTIONS = {
    2: ("hover", VIEWPORT_CENTER),
    3: ("hover", VIEWPORT_BACKGROUND),
    4: ("select", VIEWPORT_BACKGROUND),
    5: ("select", VIEWPORT_CENTER),
}
VIEWPORT_COLORS = {2: (0,), 3: (1,), 4: (2,), 5: (2,), 6: (0, 1)}


def viewport_events_fixture(output):
    path = output / "viewport-events.gltf"
    model = unlit_graph_fixture(path)
    model["extensionsUsed"].extend((FLAGS["selectable"], FLAGS["hoverable"]))
    model["nodes"][0]["extensions"] = {
        FLAGS["selectable"]: {"selectable": True},
        FLAGS["hoverable"]: {"hoverable": True},
    }
    model["materials"][0]["pbrMetallicRoughness"]["baseColorFactor"] = [1, .02, .02, 1]
    target = {"pointer": config("/materials/0/pbrMetallicRoughness/baseColorFactor"), "type": config(0)}

    def handler(declaration, destination):
        return {"declaration": declaration, "configuration": {"nodeIndex": config(0)},
                "flows": {"out": branch(destination)}}

    def write(red, green, blue):
        return {"declaration": 3, "configuration": target, "values": {"value": constant(0, red, green, blue, 1)}}

    model["extensions"]["KHR_interactivity"] = {"graph": 0, "graphs": [{
        "types": [{"signature": "float4"}, {"signature": "ref"}],
        "declarations": [
            {"op": "event/onHoverIn", "extension": FLAGS["hoverable"]},
            {"op": "event/onHoverOut", "extension": FLAGS["hoverable"]},
            {"op": "event/onSelect", "extension": FLAGS["selectable"]},
            {"op": "pointer/set"}, {"op": "ref/eq"}, {"op": "flow/branch"},
        ],
        "nodes": [
            handler(0, 1), write(.02, 1, .02),
            handler(1, 3), write(.02, .02, 1),
            handler(2, 6),
            {"declaration": 4, "values": {"a": branch(4, "selectedNode"), "b": constant(1, "/nodes/0")}},
            {"declaration": 5, "values": {"condition": branch(5)}, "flows": {"true": branch(7)}},
            write(1, 1, .02),
        ],
    }]}
    path.write_text(json.dumps(model, indent=2), encoding="utf-8")
    sequence = output / "viewport-events.sequence.json"
    sequence.write_text(json.dumps({"schemaVersion": 1, "frames": max(VIEWPORT_COLORS),
        "sampleFrames": list(VIEWPORT_COLORS), "events": [
            {"frame": frame, "viewportPointer": {"action": action, "pixel": pixel}, "depthProbe": pixel}
            for frame, (action, pixel) in VIEWPORT_ACTIONS.items()
        ]}, indent=2), encoding="utf-8")
    return path, sequence


def check_viewport_events(cohort):
    rows = []
    target_node = None
    for (frame, channels), record in zip(VIEWPORT_COLORS.items(), cohort, strict=True):
        state = record["state"]
        graph = state["gltfInteractivity"]
        assert graph["available"] and graph["playing"] and graph["started"] and not graph["diagnostic"], graph
        rgb = center_rgb(pixels(record))
        low = max(rgb[channel] for channel in range(3) if channel not in channels)
        assert all(rgb[channel] > 64 and rgb[channel] - low > 32 for channel in channels), (frame, rgb)
        selected = state["gui"]["selectedMeshNodeId"]
        row = {"frame": frame, "centerRgb": rgb, "selectedGraphNode": selected}
        if frame in VIEWPORT_ACTIONS:
            action, pixel = VIEWPORT_ACTIONS[frame]
            probe = state["depthProbe"]
            interaction = probe["sceneInteraction"]
            assert probe["pixel"] == list(pixel) and interaction["gpu"] == interaction["cpu"], probe
            hit = interaction["gpu"]["hit"]
            if pixel == VIEWPORT_CENTER:
                assert interaction["rawGpuId"] > 0 and hit["sourceNode"] == 0 and hit["sourceMesh"] == 0, probe
                assert interaction["gpu"]["hover"] == interaction["gpu"]["selection"] == hit, probe
                if target_node is None:
                    target_node = hit["graphNode"]
                assert hit["graphNode"] == target_node, probe
            else:
                assert interaction["rawGpuId"] == 0 and hit is None, probe
            row.update(pointerAction=action, pointerPixel=pixel, hit=hit, rawGpuId=interaction["rawGpuId"])
        if frame == 4:
            assert selected in (-1, 2**32 - 1), row
        else:
            # Scene initialization selects its first renderable node.
            assert selected == target_node, row
        rows.append(row)
    return rows


def run_viewport_events(renderer, output, *, generate_only):
    path, sequence = viewport_events_fixture(output)
    if generate_only:
        return
    captures, results = [], {}
    extra = ("--import-scale", "1", "--sky", "plain", "--sky-horizon", "0", "0", "0", "--screenshot-viewport")
    for technique in ("deferred-raster", "forward-raster"):
        cohort = run(renderer, output, path, f"{technique}-viewport-events", technique, "lit", captures,
                     sequence=sequence, extra=extra, require_validation=True)
        results[technique] = {"samples": check_viewport_events(cohort)}
    report = {"passed": True, "captureCount": len(captures), "captures": captures, "results": results}
    (output / "results.json").write_text(json.dumps(report, indent=2), encoding="utf-8")
    print("PASS viewport hover, background click, and selection behavior in both raster paths", flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--renderer", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--generate-only", action="store_true")
    parser.add_argument("--portability", action="store_true", help="Validate save/reload, with live merge in the standard suite")
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument("--boolean-only", action="store_true", help="Validate Boolean Object Model pointers")
    mode.add_argument("--scheduler-only", action="store_true", help="Validate delay and interpolation cancellation")
    mode.add_argument("--viewport-events-only", action="store_true", help="Validate hover and click behavior graph events")
    args = parser.parse_args()
    renderer, output = args.renderer.resolve(), args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    (output / "vk_layer_settings.txt").write_text("khronos_validation.validate_sync = true\n", encoding="utf-8")
    if args.viewport_events_only:
        run_viewport_events(renderer, output, generate_only=args.generate_only)
        return
    if args.scheduler_only:
        run_scheduler(renderer, output, generate_only=args.generate_only)
        return
    if args.boolean_only:
        run_boolean_behavior(renderer, output, generate_only=args.generate_only, portability=args.portability)
        return
    graph, reference, sequence = fixture(output)
    if args.generate_only:
        return
    captures, results = [], {}
    saved = output / "portable-behavior.glb" if args.portability else None
    for technique in ("deferred-raster", "forward-raster"):
        extra = ("--import-scale", "1", "--sky", "plain", "--sky-horizon", "0", "0", "0")
        rest = run(renderer, output, reference, f"{technique}-rest", technique, "lit", captures, extra=extra)[0]
        cohort = run(renderer, output, graph, f"{technique}-graph", technique, "lit", captures,
                     sequence=sequence, save=saved if technique == "deferred-raster" else None, extra=extra)
        assert len(cohort) == 5
        states = [record["state"]["gltfInteractivity"] for record in cohort]
        assert all(state["available"] and not state["diagnostic"] for state in states), states
        assert [state["activeClips"] for state in states] == [1, 1, 0, 0, 0], states
        assert states[-1]["playing"] is False and states[-1]["started"] is False, states
        masks = [color_mask(pixels(record), channel) for record, channel in zip(cohort, (0, 0, 0, 1))]
        assert all(mask.sum() > 100 for mask in masks)
        x = [float(np.where(mask)[1].mean()) for mask in masks]
        assert x[1] > x[0] + 5 and x[2] >= x[1], x
        np.testing.assert_array_equal(masks[2], masks[3])
        np.testing.assert_array_equal(pixels(cohort[-1]), pixels(rest))
        results[technique] = {"coloredCentroids": x, "states": states}
        if saved:
            results[technique]["portability"] = check_portability(
                renderer, output, graph, reference, sequence, saved, technique, cohort, captures, extra)
    (output / "results.json").write_text(json.dumps({"passed": True, "captures": captures, "results": results},
                                                     indent=2), encoding="utf-8")
    print("PASS authored behavior graph moves, recolors, stops, and restores in both raster paths", flush=True)


if __name__ == "__main__":
    main()
