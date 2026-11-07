"""Exercise glTF playback and reloads through a native fixed-step capture."""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess

from PIL import Image, ImageChops, ImageStat

from gltf_animation_runtime_regression import finite_json, require, write_fixture


def write_json(path, document):
    path.write_text(json.dumps(document, indent=2) + "\n", encoding="utf-8")


def read_json(path):
    return json.loads(path.read_text(encoding="utf-8"))


def difference(first, second):
    with Image.open(first) as a, Image.open(second) as b:
        return sum(ImageStat.Stat(ImageChops.difference(a.convert("RGB"), b.convert("RGB"))).mean) / 3


def run(renderer, output, model, technique, name, args, *, reject=None):
    image = output / f"{technique}-{name}.png"
    log = image.with_suffix(".log")
    command = [
        str(renderer), "--model", str(model), "--hidden", "--no-ui", "--validation",
        "--width", "480", "--height", "270", "--msaa", "1", "--no-taa", "--no-bloom",
        "--environment-intensity", "0", "--directional-intensity", "0",
        "--camera-position", "0", "0", "3", "--camera-target", "0", "0", "0",
        "--exposure", "1", "--render-technique", technique, "--fixed-dt", ".25",
        "--screenshot", str(image), *args,
    ]
    with log.open("x", encoding="utf-8") as stream:
        result = subprocess.run(command, cwd=renderer.parent, stdout=stream, stderr=subprocess.STDOUT,
                                env=dict(os.environ, VK_LAYER_SETTINGS_PATH=str(output)), timeout=120,
                                creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
    text = log.read_text(encoding="utf-8", errors="replace")
    require(not re.search(r"VUID-|SYNC-HAZARD|Validation Error", text, re.I), f"Vulkan error in {log}")
    if reject:
        require(result.returncode != 0 and reject in text and model.name in text and not image.exists(),
                f"Required animation rejection was not diagnostic: {log}")
    else:
        require(result.returncode == 0 and image.is_file(), f"Missing successful native capture: {log}")
    return image, text, dict(command=command, log=str(log), image=str(image))


def pose(image, clip, time, playing, *, camera, count=4, translation=None):
    state = read_json(image.with_suffix(".telemetry.json"))
    require(finite_json(state) and state["scene"]["primaryObjectCount"] > 0, f"Invalid native scene at {image}")
    animation = state["gltfAnimation"]
    require(animation["available"] == bool(count) and animation["clipCount"] == count, f"Wrong animation asset at {image}")
    require(animation["selectedClip"] == clip and animation["selectedCameraNode"] == camera
            and animation["playing"] == playing and abs(animation["time"] - time) < 1e-6,
            f"Unexpected animation or camera state at {image}: {animation}")
    if translation is not None:
        require(len(animation["nodes"]) == 1, "Expected one active translation target")
        node = animation["nodes"][0]
        for actual in (node["translation"][0], node["localMatrix"][12], node["worldMatrix"][12]):
            require(abs(actual - translation) < 1e-6, f"Source/graph pose mismatch at {image}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--renderer", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    renderer, output = args.renderer.resolve(), args.output.resolve()
    output.mkdir(parents=True)
    (output / "vk_layer_settings.txt").write_text("khronos_validation.validate_sync = true\n", encoding="utf-8")
    source, malformed, replacement, unsupported = (output / f"{name}.gltf" for name in
        ("animated-required", "malformed-animation", "static-replacement", "unsupported-required-pointer"))
    write_fixture(source)
    model = read_json(source)
    model["extensionsRequired"] = ["KHR_animation_pointer"]
    write_json(source, model)

    bad = read_json(source)
    bad["animations"][0]["samplers"][0]["input"] = 0  # POSITION is a VEC3, not a time accessor.
    write_json(malformed, bad)
    bad = read_json(source)
    bad["animations"][1]["channels"][0]["target"]["extensions"]["KHR_animation_pointer"]["pointer"] = (
        "/materials/0/extras/unavailable")
    write_json(unsupported, bad)
    static = read_json(source)
    static.pop("animations")
    static.pop("extensionsRequired")
    static["extensionsUsed"].remove("KHR_animation_pointer")
    static["nodes"][2].pop("camera")
    static.pop("cameras")
    write_json(replacement, static)

    sequence = output / "playback.json"
    write_json(sequence, {"schemaVersion": 1, "frames": 9, "sampleFrames": list(range(1, 10)), "events": [
        {"frame": 1, "animationClip": 0, "animationTime": 0, "animationPlaying": False},
        {"frame": 2, "animationTime": 1},
        {"frame": 3, "reload": str(malformed)},
        {"frame": 4, "animationClip": 1, "animationTime": 1},
        {"frame": 5, "reload": str(replacement)},
        {"frame": 6, "reload": str(source)},
        {"frame": 7, "animationClip": 0, "animationTime": .75, "animationPlaying": True},
    ]})
    report = dict(passed=False, rendererSha256=hashlib.sha256(renderer.read_bytes()).hexdigest(), cases=[])
    for technique in ("deferred-raster", "forward-raster"):
        image, log, record = run(renderer, output, source, technique, "lifecycle",
                                 ["--capture-sequence", str(sequence), "--animation-clip", "0",
                                  "--animation-paused", "--gltf-camera-node", "2"])
        require(malformed.name in log and "animation 0 channel 0" in log, "Malformed timeline diagnostic is missing")
        frames = [image.with_name(f"{image.stem}.frame-{frame:04d}.png") for frame in range(1, 9)] + [image]
        for shot, (clip, time, playing, camera, count, translation) in zip(frames, [
                (0, 0, False, 2, 4, -.6), (0, 1, False, 2, 4, .6), (0, 1, False, 2, 4, .6),
                (1, 1, False, 2, 4, None), (-1, 0, False, -1, 0, None), (-1, 0, False, -1, 4, None),
                (0, .75, True, -1, 4, .3), (0, 0, True, -1, 4, -.6), (0, .25, True, -1, 4, -.3)]):
            pose(shot, clip, time, playing, camera=camera, count=count, translation=translation)
        movement = difference(frames[0], frames[1])
        require(movement > 1, "Seeking produced no visible motion")
        require(difference(frames[1], frames[2]) == 0, "Rejected reload changed the paused image")
        require(difference(frames[3], frames[4]) > 1, "Valid scene replacement produced no visible change")
        record.update(captures=[str(frame) for frame in frames], seekPixelDifference=movement)
        report["cases"].append(record)
        clamped, _, record = run(renderer, output, source, technique, "reverse-clamp",
                                 ["--animation-clip", "0", "--animation-time", ".5", "--animation-speed", "-1",
                                  "--animation-no-loop", "--capture-frame", "4"])
        pose(clamped, 0, 0, False, camera=-1, translation=-.6)
        report["cases"].append(record)
        _, _, record = run(renderer, output, unsupported, technique, "required-target",
                           ["--capture-frame", "1"], reject="required KHR_animation_pointer")
        report["cases"].append(record)
        print("PASS", technique, "seek, failed reload, replacement, loop, reverse clamp, required target", flush=True)
    require(hashlib.sha256(renderer.read_bytes()).hexdigest() == report["rendererSha256"], "Renderer changed")
    report["passed"] = True
    write_json(output / "results.json", report)


if __name__ == "__main__":
    main()
