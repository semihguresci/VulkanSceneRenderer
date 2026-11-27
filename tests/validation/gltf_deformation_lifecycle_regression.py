"""Check deformation reloads and raster playback across graphics frame slots."""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path

from gltf_deformation_runtime_regression import (
    check_pixels, check_telemetry, evaluate, finite_json, spec_for, write_fixture,
)
from gltf_playback_lifecycle_regression import difference, read_json, run, write_json
from gltf_animation_runtime_regression import require


def run_lifecycle(renderer, output):
    skin = spec_for("skin")
    source, missing, static, rest, morph = (output / f"{name}.gltf" for name in
        ("skinned", "invalid-skin-joint", "baked", "baked-rest", "required-morph-pointer"))
    write_fixture(source, skin)
    write_fixture(static, skin, evaluate(skin, 1))
    write_fixture(rest, skin, evaluate(skin, 0))
    invalid = read_json(source)
    invalid["skins"][0]["joints"][-1] = len(invalid["nodes"])
    write_json(missing, invalid)

    morph_spec = spec_for("morph")
    write_fixture(morph, morph_spec)
    required = read_json(morph)
    required["extensionsUsed"] = required["extensionsRequired"] = ["KHR_animation_pointer"]
    required["animations"][0]["channels"][0]["target"] = {
        "path": "pointer", "extensions": {"KHR_animation_pointer": {"pointer": "/nodes/0/weights"}}}
    write_json(morph, required)

    sequence = output / "deformation-lifecycle.json"
    write_json(sequence, {"schemaVersion": 1, "frames": 6, "sampleFrames": [2, 3, 4, 5, 6],
        "camera": [{"frame": 1, "position": skin["camera"], "target": skin["target"]}],
        "events": [{"frame": 1, "viewportPointer": {"action": "select", "pixel": [8, 8]}},
                   {"frame": 3, "reload": str(missing)}, {"frame": 4, "reload": str(static)},
                   {"frame": 5, "reload": str(source)}, {"frame": 6, "reload": str(rest)}]})
    cases = []
    display = ["--display-mode", "lit", "--environment-intensity", ".35",
               "--directional-intensity", "3", "--camera-fov", "45", "--screenshot-viewport"]
    for technique in ("deferred-raster", "forward-raster"):
        image, log, record = run(renderer, output, source, technique, "skin-reload",
            [*display, "--capture-sequence", str(sequence), "--animation-clip", "0", "--animation-time", "1",
             "--animation-paused"])
        require(missing.name in log and "joint node is out of range" in log,
                "Invalid joint reload did not produce a contextual diagnostic")
        frames = [image.with_name(f"{image.stem}.frame-{i:04d}.png") for i in (2, 3, 4, 5)] + [image]
        states = [read_json(path.with_suffix(".telemetry.json")) for path in frames]
        require(all(state["camera"] == states[0]["camera"] for state in states),
                "Reload changed the reference camera")
        for index in (0, 1):
            check_telemetry({"state": states[index]}, evaluate(skin, 1), skin)
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
        print("PASS", technique, "invalid joint rollback, static replacement, required morph pointer", flush=True)
    return cases


def check_playback_policy(state, slots, frames):
    require(finite_json(state) and state["validationEnabled"] and state["framesInFlight"] == slots,
            "Playback did not use the requested frame slots with validation enabled")
    require(not state["taa"]["enabled"] and not state["rayShadows"]["supported"]
            and not state["ddgi"]["enabled"], "Playback did not use the isolated raster configuration")
    history = state["frameHistory"]
    require([row["frameIndex"] for row in history] == list(range(frames)),
            "Playback history omitted or repeated a submission")
    require(all(row["cpuPhasesMs"]["Screenshot"] == 0 for row in history[:-1])
            and history[-1]["cpuPhasesMs"]["Screenshot"] > 0,
            "Playback captured a frame before its terminal screenshot")
    policies = {(row["serializedConcurrency"], row["concurrencyReason"]) for row in history}
    require(policies == {(False, "per-frame resources")},
            f"Playback requires the current-frame fence policy, got {sorted(policies)} "
            f"with GPU timing source {state['gpuProfiler']['source']}")
    pending = [row["graphicsFramesInFlight"] for row in history[:-1]
               if row["graphicsFramesInFlight"] is not None]
    require(all(type(count) is int and 0 <= count <= slots for count in pending),
            "Playback reported an invalid pending graphics frame count")
    require(not state["gpuProfiler"]["available"] or len(pending) == frames - 1,
            "The active profiler omitted pending graphics counts during playback")
    return dict(steadyFrameIndices=[row["frameIndex"] for row in history[:-1]],
                serialized=False, reason="per-frame resources", peakPending=max(pending, default=None))


def run_playback(renderer, output, slots):
    # The first submission displays time zero. Subsequent frames advance by an
    # exactly representable timestep, exercising repeated slot reuse at 0.75 s.
    frames = 49
    timestep = 1 / 64
    terminal_time = (frames - 1) * timestep
    spec = spec_for("combined")
    expected = evaluate(spec, terminal_time)
    source, baked = output / "playback-combined.gltf", output / "playback-combined-baked.gltf"
    write_fixture(source, spec)
    write_fixture(baked, spec, expected)
    display = ["--display-mode", "lit", "--width", "640", "--height", "360",
               "--environment-intensity", ".35", "--directional-intensity", "3",
               "--camera-position", *map(str, spec["camera"]),
               "--camera-target", *map(str, spec["target"]), "--camera-fov", "45",
               "--screenshot-viewport", "--no-ray-query", "--no-ddgi", "--ray-shadows", "raster",
               "--fixed-dt", str(timestep), "--warmup-frames", str(frames - 1), "--capture-frame", str(frames)]
    cases = []
    for technique in ("deferred-raster", "forward-raster"):
        first_dynamic_image = None
        for count in slots:
            name = f"playback-slots-{count}"
            records = {}
            for mode, model in (("animated", source), ("baked", baked)):
                flags = [*display, "--frames-in-flight", str(count)]
                if mode == "animated":
                    flags += ["--animation-clip", "0", "--animation-time", "0"]
                image, log, record = run(renderer, output, model, technique, f"{name}-{mode}", flags)
                require('Insert instance layer "VK_LAYER_KHRONOS_validation"' in log,
                        "Playback did not load the Khronos validation layer")
                state = read_json(image.with_suffix(".telemetry.json"))
                policy = check_playback_policy(state, count, frames)
                require(not list(output.glob(f"{image.stem}.frame-*.png")),
                        "Playback produced an unexpected intermediate capture")
                records[mode] = dict(record, state=state, policy=policy)
            dynamic, reference = records["animated"], records["baked"]
            state = dynamic["state"]
            animation = state["gltfAnimation"]
            require(animation["selectedClip"] == 0 and animation["playing"] and animation["diagnostics"] == 0
                    and abs(animation["time"] - terminal_time) < 1e-6,
                    f"Playback did not reach its independently specified pose: {animation}")
            check_telemetry(dynamic, expected, spec, moving=True)
            require(state["gltfDeformation"]["maxPositionDelta"] > 0
                    and not state["gltfDeformation"]["historyReset"],
                    "Playback did not retain changing current and previous deformation streams")
            require(not reference["state"]["gltfDeformation"]["active"], "The baked control unexpectedly deformed")
            for field in ("build", "gpu", "driverVersion", "resolution", "camera"):
                require(state[field] == reference["state"][field], f"Playback and baked control differed in {field}")
            if first_dynamic_image is None:
                first_dynamic_image = dynamic["image"]
            require(difference(first_dynamic_image, dynamic["image"]) == 0,
                    "The terminal deformation image changed with the graphics slot count")
            cases.append(dict(technique=technique, slots=count, submittedFrames=frames,
                              terminalTime=terminal_time, **check_pixels(dynamic, reference, expected),
                              animated={key: value for key, value in dynamic.items() if key != "state"},
                              baked={key: value for key, value in reference.items() if key != "state"}))
            print("PASS", technique, f"skin+morph playback with {count} graphics slots", flush=True)
    return cases


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--renderer", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--suite", choices=("all", "lifecycle", "playback"), default="all")
    parser.add_argument("--slots", type=int, choices=(1, 2, 3), action="append",
                        help="graphics slot counts for the playback cohort (default 1, 2, 3)")
    args = parser.parse_args()
    renderer, output = args.renderer.resolve(strict=True), args.output.resolve()
    output.mkdir(parents=True)
    (output / "vk_layer_settings.txt").write_text("khronos_validation.validate_sync = true\n", encoding="utf-8")
    os.environ["VK_LOADER_DEBUG"] = "layer"
    initial = hashlib.sha256(renderer.read_bytes()).hexdigest()
    report = dict(passed=False, rendererSha256=initial, suite=args.suite, captures=0, cases=[], playback=[])
    try:
        if args.suite in ("all", "lifecycle"):
            report["cases"] = run_lifecycle(renderer, output)
            report["captures"] += 12
        if args.suite in ("all", "playback"):
            report["playback"] = run_playback(renderer, output, dict.fromkeys(args.slots or (1, 2, 3)))
            report["captures"] += 2 * len(report["playback"])
        require(hashlib.sha256(renderer.read_bytes()).hexdigest() == initial, "Renderer changed")
        report["passed"] = True
    finally:
        write_json(output / "results.json", report)


if __name__ == "__main__":
    main()
