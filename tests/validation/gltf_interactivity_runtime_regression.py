"""Validate authored behavior graph publication in both Vulkan raster techniques."""

import argparse
import json
from pathlib import Path

import numpy as np
from PIL import Image

from gltf_animation_runtime_regression import write_fixture
from gltf_material_variants_runtime_regression import run


def config(*items):
    return {"value": list(items)}


def constant(type_index, *items):
    return {"type": type_index, "value": list(items)}


def branch(index):
    return {"node": index}


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


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--renderer", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--generate-only", action="store_true")
    parser.add_argument("--portability", action="store_true", help="Also validate save/reload and live merge")
    args = parser.parse_args()
    renderer, output = args.renderer.resolve(), args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    (output / "vk_layer_settings.txt").write_text("khronos_validation.validate_sync = true\n", encoding="utf-8")
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
