"""Check posed geometry across failed reloads and required morph pointers."""
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path

from gltf_deformation_runtime_regression import check_telemetry, evaluate, spec_for, write_fixture
from gltf_playback_lifecycle_regression import difference, read_json, run, write_json
from gltf_animation_runtime_regression import require


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--renderer", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    renderer = args.renderer.resolve(strict=True)
    output = args.output.resolve()
    output.mkdir(parents=True)
    (output / "vk_layer_settings.txt").write_text("khronos_validation.validate_sync = true\n", encoding="utf-8")

    skin = spec_for("skin")
    source, missing, static, rest, morph = (output / f"{name}.gltf" for name in
        ("skinned", "missing-selected-joint", "baked", "baked-rest", "required-morph-pointer"))
    write_fixture(source, skin)
    write_fixture(static, skin, evaluate(skin, 1))
    write_fixture(rest, skin, evaluate(skin, 0))
    invalid = read_json(source)
    joint = invalid["skins"][0]["joints"][-1]
    for node in invalid["nodes"]:
        if joint in node.get("children", []):
            node["children"].remove(joint)
    for scene in invalid["scenes"]:
        if joint in scene["nodes"]:
            scene["nodes"].remove(joint)
    write_json(missing, invalid)

    morph_spec = spec_for("morph")
    write_fixture(morph, morph_spec)
    required = read_json(morph)
    required["extensionsUsed"] = required["extensionsRequired"] = ["KHR_animation_pointer"]
    required["animations"][0]["channels"][0]["target"] = {
        "path": "pointer", "extensions": {"KHR_animation_pointer": {"pointer": "/nodes/0/weights"}}}
    write_json(morph, required)

    sequence = output / "deformation-lifecycle.json"
    write_json(sequence, {"schemaVersion": 1, "frames": 5, "sampleFrames": [1, 2, 3, 4, 5],
        "camera": [{"frame": 1, "position": skin["camera"], "target": skin["target"]}],
        "events": [{"frame": 2, "reload": str(missing)}, {"frame": 3, "reload": str(static)},
                   {"frame": 4, "reload": str(source)}, {"frame": 5, "reload": str(rest)}]})
    initial = hashlib.sha256(renderer.read_bytes()).hexdigest()
    cases = []
    display = ["--display-mode", "lit", "--environment-intensity", ".35",
               "--directional-intensity", "3", "--camera-fov", "45", "--screenshot-viewport"]
    for technique in ("deferred-raster", "forward-raster"):
        image, log, record = run(renderer, output, source, technique, "skin-reload",
            [*display, "--capture-sequence", str(sequence), "--animation-clip", "0", "--animation-time", "1",
             "--animation-paused"])
        require(missing.name in log and "required joint runtime node is missing" in log,
                "Missing joint reload did not produce a contextual diagnostic")
        frames = [image.with_name(f"{image.stem}.frame-{i:04d}.png") for i in (1, 2, 3, 4)] + [image]
        states = [read_json(path.with_suffix(".telemetry.json")) for path in frames]
        require(all(state["camera"] == states[0]["camera"] for state in states),
                "Reload changed the reference camera")
        for index in (0, 1):
            check_telemetry({"state": states[index]}, evaluate(skin, 1), skin, moving=index == 0)
            require(states[index]["gltfAnimation"]["selectedClip"] == 0 and
                    states[index]["gltfAnimation"]["time"] == 1, "Rejected reload reset the pose")
        require(difference(frames[0], frames[1]) == 0, "Failed joint reload changed pixels")
        require(not states[2]["gltfDeformation"]["active"], "Static replacement retained deformation")
        require(difference(frames[0], frames[2]) == 0, "Static baked replacement disagreed with the skin")
        require(states[3]["gltfDeformation"]["active"] and states[3]["gltfAnimation"]["selectedClip"] == -1,
                "Reloading the original skin did not rebuild its authored rest pose")
        check_telemetry({"state": states[3]}, evaluate(skin, 0), skin, moving=True)
        require(states[3]["gltfDeformation"]["historyReset"], "Reload retained the previous asset's pose history")
        require(not states[4]["gltfDeformation"]["active"], "Baked rest reference retained deformation")
        require(difference(frames[3], frames[4]) == 0, "Reloaded skin disagreed with the baked rest pose")
        motion = difference(frames[0], frames[3])
        require(motion > 1, f"Rest pose did not visibly replace the animated pose: {motion}")
        cases.append(dict(record, captures=[str(path) for path in frames], restPixelDifference=motion))

        shot, _, record = run(renderer, output, morph, technique, "required-morph",
            [*display, "--animation-clip", "0", "--animation-time", "1", "--animation-paused", "--capture-frame", "4",
             "--camera-position", *map(str, morph_spec["camera"]), "--camera-target", *map(str, morph_spec["target"])])
        state = read_json(shot.with_suffix(".telemetry.json"))
        check_telemetry({"state": state}, evaluate(morph_spec, 1), morph_spec)
        require(state["gltfAnimation"]["diagnostics"] == 0, "Required morph pointer was omitted")
        cases.append(record)
        print("PASS", technique, "missing joint rollback, static replacement, required morph pointer", flush=True)
    require(hashlib.sha256(renderer.read_bytes()).hexdigest() == initial, "Renderer changed")
    write_json(output / "results.json", dict(passed=True, rendererSha256=initial, captures=12, cases=cases))


if __name__ == "__main__":
    main()
