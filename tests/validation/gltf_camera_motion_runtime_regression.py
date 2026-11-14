"""Exercise camera motion editing, sampled poses, history, and portable exports."""

import argparse
import json
import math
import os
from pathlib import Path
import subprocess

import numpy as np
from PIL import Image

from display_capture_test_support import validation_log_error
from gltf_accessor_runtime_regression import Asset, decode_accessor, load_asset


WIDTH, HEIGHT = 320, 240
FIXED_DT = .125
CAMERA = "Parented camera"
PARENT = "Camera mount"
MARKER = "Independent animated marker"
OLD_CLIP = "Original marker motion"
PARENT_POSITION = np.array([1.25, .5, -.75])
ANIMATED_PARENT_POSITION = [3.75, .8, -1.25]
PARENT_YAW = 32.
REST_TRANSLATION = [.4, .15, 5.75]
REST_YAW = -12.
A = ([-1.5, 1., 6.], -15.)
B = ([.4, .5, 5.3], 7.)
C = ([1.5, .85, 7.2], 25.)
UPDATE = ([-.6, .9, 5.4], 11.)
ORIGINAL = ((0., A), (1., B), (2., C))
UPDATED = ((0., A), (1., UPDATE), (2., C))
MOVED = ((0., A), (1.25, UPDATE), (2., C))
DELETED = ((0., A), (2., C))


def yaw_rotation(degrees):
    angle = math.radians(degrees)
    s, c = math.sin(angle), math.cos(angle)
    return np.array([[c, 0, s], [0, 1, 0], [-s, 0, c]])


def yaw_quaternion(degrees):
    half = math.radians(degrees) / 2
    return np.array([0., math.sin(half), 0., math.cos(half)])


def navigation(frame, pose):
    position, yaw = pose
    target = position + yaw_rotation(yaw) @ np.array([0., 0., -1.])
    return {"frame": frame, "position": position, "target": target.tolist()}


def sample(keys, time):
    if time <= keys[0][0]:
        return keys[0][1]
    for (left, (p0, yaw0)), (right, (p1, yaw1)) in zip(keys, keys[1:]):
        if time <= right:
            weight = (time - left) / (right - left)
            # All authored rotations use one axis and less than 180° separation.
            # Quaternion slerp therefore interpolates their angles linearly.
            return (1 - weight) * np.asarray(p0) + weight * np.asarray(p1), (1 - weight) * yaw0 + weight * yaw1
    return keys[-1][1]


def camera_rest_matrix():
    matrix = np.eye(4)
    matrix[:3, :3] = yaw_rotation(REST_YAW)
    matrix[:3, 3] = REST_TRANSLATION
    return matrix.ravel(order="F")


def fixture(path, *, matrix_camera=False):
    asset = Asset()
    model = asset.model
    model["nodes"] = [
        {"name": PARENT, "translation": PARENT_POSITION.tolist(),
         "rotation": yaw_quaternion(PARENT_YAW).tolist(), "children": [1]},
        {"name": CAMERA, "camera": 0, "translation": REST_TRANSLATION,
         "rotation": yaw_quaternion(REST_YAW).tolist()},
        {"name": MARKER, "mesh": 0},
    ]
    if matrix_camera:
        camera = model["nodes"][1]
        del camera["translation"], camera["rotation"]
        camera["matrix"] = camera_rest_matrix().tolist()
    model["cameras"] = [{"name": CAMERA, "type": "perspective", "perspective": {"yfov": .82, "znear": .1, "zfar": 100.}}]
    model["scenes"] = [{"nodes": [0, 2]}]
    model["materials"] = [{"extensions": {"KHR_materials_unlit": {}},
                          "pbrMetallicRoughness": {"baseColorFactor": [.85, .27, .06, 1.]}}]
    model["extensionsUsed"] = ["KHR_materials_unlit"]
    positions = [[-1.9, -1.25, 0.], [1.15, -.9, 0.], [.6, 1.2, 0.]]
    model["meshes"] = [{"primitives": [{"attributes": {
        "POSITION": asset.accessor("VEC3", positions),
        "NORMAL": asset.accessor("VEC3", [[0., 0., 1.]] * 3),
    }, "indices": asset.accessor("SCALAR", [0, 1, 2], 5123, indices=True), "material": 0}]}]
    model["animations"] = [{
        "name": OLD_CLIP,
        "samplers": [{"input": asset.accessor("SCALAR", [0., 1.5]), "interpolation": "STEP",
                      "output": asset.accessor("VEC3", [[0., 0., 0.], [.6, .1, 0.]])}],
        "channels": [{"sampler": 0, "target": {"node": 2, "path": "translation"}}],
    }]
    if matrix_camera:
        original = model["animations"][0]
        original["samplers"].append({"input": original["samplers"][0]["input"], "interpolation": "STEP",
                                     "output": asset.accessor("VEC3", [PARENT_POSITION, ANIMATED_PARENT_POSITION])})
        original["channels"].append({"sampler": 1, "target": {"node": 0, "path": "translation"}})
    asset.write(path)


def save_json(path, data):
    path.write_text(json.dumps(data, indent=2) + "\n", encoding="utf-8")


def edit_sequence(path, *, active_save=False):
    events, camera, captures, expected = [], [], [], {}
    tick = 0

    def step(action=None, *, pose=None, label=None, keys=None, cursor=None, time=None, image=False, playing=False):
        nonlocal tick
        tick += 1
        if action is not None:
            events.append({"frame": tick, **action})
        if pose is not None:
            camera.append(navigation(tick, pose))
        if label is not None:
            expected[label] = (tick, keys, cursor, time, playing)
            if image:
                captures.append(tick)

    def motion(operation, **fields):
        return {"cameraMotion": {"sourceNode": 1, "operation": operation, **fields}}

    def seek(time, label, keys, cursor, *, image=False):
        step({"animationTime": time}, label=label, keys=keys, cursor=cursor, time=time, image=image)

    if active_save:
        step({"animationClip": 0, "animationTime": 1.5, "animationPlaying": False})
    step(motion("create"), pose=A, label="created", keys=((0., A),), cursor=(1, 0))
    if active_save:
        step({"cameraSelection": {"sourceNode": 1}})
        seek(0., "animated-parent-key", ((0., A),), (1, 0))
        step({"cameraSelection": {"viewport": True}})
    step(motion("capture", clip=1, time=1.), pose=B, label="second", keys=ORIGINAL[:2], cursor=(2, 0))
    step(motion("capture", clip=1, time=2.), pose=C, label="third", keys=ORIGINAL, cursor=(3, 0))
    step({"cameraSelection": {"sourceNode": 1}})
    seek(.5, "first-scrub", ORIGINAL, (3, 0), image=True)
    seek(1.5, "second-scrub", ORIGINAL, (3, 0), image=True)
    step({"animationTime": 0.})
    step({"animationPlaying": True})
    step(label="playing-first", keys=ORIGINAL, cursor=(3, 0), playing=True)
    step(label="playing-next", keys=ORIGINAL, cursor=(3, 0), playing=True)
    step({"animationPlaying": False}, label="paused", keys=ORIGINAL, cursor=(3, 0))
    step({"cameraSelection": {"viewport": True}})
    step(motion("capture", clip=1, time=1.), pose=UPDATE)
    step({"cameraSelection": {"sourceNode": 1}})
    seek(1., "updated", UPDATED, (4, 0))
    step({"sceneHistory": "undo"})
    seek(1., "undo-update", ORIGINAL, (3, 1))
    step({"sceneHistory": "redo"})
    seek(1., "redo-update", UPDATED, (4, 0))
    step(motion("move", clip=1, time=1., newTime=1.25))
    seek(1., "moved-interior", MOVED, (5, 0))
    step({"sceneHistory": "undo"})
    seek(1., "undo-move", UPDATED, (4, 1))
    step({"sceneHistory": "redo"})
    seek(1.25, "redo-move", MOVED, (5, 0))
    step(motion("delete", clip=1, time=1.25))
    seek(1.25, "deleted", DELETED, (6, 0))
    step({"sceneHistory": "undo"})
    seek(1.25, "undo-delete", MOVED, (5, 1))
    step({"sceneHistory": "redo"})
    seek(1.25, "redo-delete", DELETED, (6, 0))
    for _ in range(5):
        step({"sceneHistory": "undo"})
    step({"sceneHistory": "undo"}, label="undo-create", cursor=(0, 6))
    step({"sceneHistory": "redo"}, label="redo-create", keys=((0., A),), cursor=(1, 5))
    for _ in range(4):
        step({"sceneHistory": "redo"})
    seek(0., "saved-start", MOVED, (5, 1), image=True)
    seek(.625, "saved-between-a", MOVED, (5, 1), image=True)
    seek(1.25, "saved-middle", MOVED, (5, 1), image=True)
    seek(1.625, "saved-between-b", MOVED, (5, 1), image=True)
    seek(2., "saved-end", MOVED, (5, 1), image=True)
    if active_save:
        captures = [tick]
        telemetry_frames = [
            expected[label][0] for label in ("created", "animated-parent-key", "undo-create", "redo-create")
        ] + [tick]
    else:
        step({"animationClip": -1}, label="saved-rest", keys=MOVED, cursor=(5, 1))
        telemetry_frames = [entry[0] for entry in expected.values()]
    save_json(path, {"schemaVersion": 1, "frames": tick, "sampleFrames": captures,
                     "telemetryFrames": telemetry_frames, "camera": camera, "events": events})
    return expected


def assert_quaternion(actual, expected):
    actual = np.asarray(actual)
    np.testing.assert_allclose(np.linalg.norm(actual), 1., atol=1e-5, rtol=0)
    np.testing.assert_allclose(abs(actual @ expected), 1., atol=1e-6, rtol=0)


def check_pose(state, source_node, expected_pose, *, world_only=False):
    position, yaw = expected_pose
    basis = yaw_rotation(yaw)
    world = np.eye(4)
    world[:3, :3] = basis
    world[:3, 3] = position
    camera = state["gltfCameraAuthoring"]
    assert camera["selectedSourceNode"] == source_node, camera
    np.testing.assert_allclose(np.asarray(camera["view"]).reshape((4, 4), order="F"),
                               np.linalg.inv(world), atol=8e-5, rtol=0)
    if world_only:
        return
    nodes = state["gltfAnimation"]["nodes"]
    node, = (entry for entry in nodes if entry["sourceIndex"] == source_node)
    np.testing.assert_allclose(np.asarray(node["worldMatrix"]).reshape((4, 4), order="F"), world, atol=8e-5, rtol=0)
    local_position = yaw_rotation(PARENT_YAW).T @ (position - PARENT_POSITION)
    np.testing.assert_allclose(node["translation"], local_position, atol=8e-5, rtol=0)
    assert_quaternion(node["rotation"], yaw_quaternion(yaw - PARENT_YAW))


def capture_path(base, frame, end):
    return base if frame == end else base.with_name(f"{base.stem}.frame-{frame:04d}{base.suffix}")


def state_at(base, frame, end):
    state = json.loads(capture_path(base, frame, end).with_suffix(".telemetry.json").read_bytes())
    assert state["validationEnabled"] and state["technique"] == "deferred-raster", state["build"]
    assert state["resolution"] == [WIDTH, HEIGHT]
    return state


def run(renderer, model, script, base, *, save=None):
    cmd = [str(renderer), "--model", str(model), "--import-scale", "1",
           "--hidden", "--no-ui", "--validation", "--msaa", "1", "--no-taa", "--no-bloom",
           "--no-ray-query", "--no-ddgi", "--render-technique", "deferred-raster",
           "--display-mode", "lit", "--width", str(WIDTH), "--height", str(HEIGHT),
           "--fixed-dt", str(FIXED_DT), "--sky", "plain", "--sky-horizon", "0", "0", "0",
           "--environment-intensity", "0", "--capture-sequence", str(script),
           "--screenshot-viewport", "--screenshot", str(base)]
    if save is not None:
        cmd.extend(("--save-scene", str(save)))
    log = base.with_suffix(".log")
    with log.open("w", encoding="utf-8") as stream:
        result = subprocess.run(cmd, cwd=renderer.parent, stdout=stream, stderr=subprocess.STDOUT,
                                env=dict(os.environ, VK_LAYER_SETTINGS_PATH=str(base.parent)), timeout=180)
    assert result.returncode == 0 and not validation_log_error(log.read_text(encoding="utf-8", errors="replace")), log


def check_edit_sequence(base, expected):
    end = max(step[0] for step in expected.values())
    playback = {}
    for label, (frame, keys, cursor, time, playing) in expected.items():
        state = state_at(base, frame, end)
        animation = state["gltfAnimation"]
        assert animation["available"] and animation["diagnostics"] == 0, (label, animation)
        assert animation["clipCount"] == (1 if keys is None else 2), (label, animation)
        motion = animation["cameraMotion"]
        assert len(motion) == (0 if keys is None else 1), (label, motion)
        if keys is not None:
            entry, = motion
            assert (entry["clip"], entry["sourceNode"]) == (1, 1), (label, entry)
            assert entry["keyTimes"] == [key[0] for key in keys], (label, entry)
        assert state["sceneEditHistory"] == dict(zip(("undo", "redo"), cursor)), (label, state["sceneEditHistory"])
        assert animation["playing"] is playing, label
        if time is not None or playing or label == "paused":
            assert animation["selectedClip"] == 1, (label, animation)
            measured = animation["time"]
            if time is not None:
                np.testing.assert_allclose(measured, time, rtol=0, atol=1e-7)
            check_pose(state, 1, sample(keys, measured))
            playback[label] = measured
    assert 0. < playback["playing-first"] < 1.
    assert playback["playing-next"] - playback["playing-first"] == FIXED_DT
    assert playback["paused"] == playback["playing-next"]


def check_matrix_promotion(base, expected):
    end = expected["saved-end"][0]
    for label in ("created", "undo-create", "redo-create"):
        state = state_at(base, expected[label][0], end)
        camera, = (node for node in state["gltfCameraPoses"]["nodes"] if node["sourceNode"] == 1)
        if label == "undo-create":
            for role in ("authored", "evaluated", "rest"):
                assert camera[role]["translation"] == camera[role]["rotation"] == [], (label, role)
                np.testing.assert_allclose(camera[role]["matrix"], camera_rest_matrix(), atol=1e-7, rtol=0)
        else:
            for role in ("authored", "rest"):
                assert camera[role]["matrix"] == [], (label, role)
                np.testing.assert_allclose(camera[role]["translation"], REST_TRANSLATION, atol=1e-6, rtol=0)
                assert_quaternion(camera[role]["rotation"], yaw_quaternion(REST_YAW))


def check_export(path, *, active_save=False):
    model, _ = load_asset(path)
    names = [node.get("name", "") for node in model["nodes"]]
    camera, parent, marker = map(names.index, (CAMERA, PARENT, MARKER))
    assert camera in model["nodes"][parent]["children"]
    default = model["nodes"][camera]
    translation, yaw = REST_TRANSLATION, REST_YAW
    if active_save:
        translation = yaw_rotation(PARENT_YAW).T @ (C[0] - PARENT_POSITION)
        yaw = C[1] - PARENT_YAW
    np.testing.assert_allclose(default["translation"], translation, rtol=0, atol=8e-5)
    assert_quaternion(default["rotation"], yaw_quaternion(yaw))
    retained, motion = model["animations"]
    assert retained["name"] == OLD_CLIP
    assert retained["channels"][0] == {"sampler": 0, "target": {"node": marker, "path": "translation"}}
    sampler = retained["samplers"][0]
    assert sampler["interpolation"] == "STEP"
    np.testing.assert_array_equal(decode_accessor(path, sampler["input"]).ravel(), [0., 1.5])
    np.testing.assert_allclose(decode_accessor(path, sampler["output"]), [[0., 0., 0.], [.6, .1, 0.]], atol=1e-7, rtol=0)
    if active_save:
        assert retained["channels"][1] == {"sampler": 1, "target": {"node": parent, "path": "translation"}}
        sampler = retained["samplers"][1]
        assert sampler["interpolation"] == "STEP"
        np.testing.assert_array_equal(decode_accessor(path, sampler["input"]).ravel(), [0., 1.5])
        np.testing.assert_allclose(decode_accessor(path, sampler["output"]),
                                   [PARENT_POSITION, ANIMATED_PARENT_POSITION], atol=1e-7, rtol=0)
    channels = {channel["target"]["path"]: channel for channel in motion["channels"][:2]}
    assert set(channels) == {"translation", "rotation"}, channels
    for kind, channel in channels.items():
        assert channel["target"]["node"] == camera
        sampler = motion["samplers"][channel["sampler"]]
        assert sampler.get("interpolation", "LINEAR") == "LINEAR"
        for field, shape in (("input", "SCALAR"), ("output", "VEC3" if kind == "translation" else "VEC4")):
            accessor = model["accessors"][sampler[field]]
            assert accessor["type"] == shape and accessor["componentType"] == 5126 and accessor["count"] == 3, accessor
        np.testing.assert_array_equal(decode_accessor(path, sampler["input"]).ravel(), [time for time, _ in MOVED])
        for row, (_, (position, yaw)) in zip(decode_accessor(path, sampler["output"]), MOVED, strict=True):
            if kind == "rotation":
                assert_quaternion(row, yaw_quaternion(yaw - PARENT_YAW))
            else:
                np.testing.assert_allclose(row, yaw_rotation(PARENT_YAW).T @ (position - PARENT_POSITION), atol=8e-5, rtol=0)

    def check_restore(clip, channel, targets):
        target = channel["target"]
        identity = target["node"], target["path"]
        value = targets.pop(identity)
        sampler = clip["samplers"][channel["sampler"]]
        assert sampler["interpolation"] == "STEP"
        np.testing.assert_array_equal(decode_accessor(path, sampler["input"]).ravel(), [0.])
        row, = decode_accessor(path, sampler["output"])
        if identity[1] == "rotation":
            assert_quaternion(row, value)
        else:
            np.testing.assert_allclose(row, value, atol=8e-5, rtol=0)

    # Portable export can add one-key restores for properties owned by another
    # clip. Verify their values while separately checking untouched originals.
    camera_rest = {(camera, "translation"): REST_TRANSLATION, (camera, "rotation"): yaw_quaternion(REST_YAW)}
    for channel in retained["channels"][2 if active_save else 1:]:
        check_restore(retained, channel, camera_rest)
    marker_rest = {(marker, "translation"): [0., 0., 0.]}
    if active_save:
        marker_rest[parent, "translation"] = PARENT_POSITION
    for channel in motion["channels"][2:]:
        check_restore(motion, channel, marker_rest)
    if active_save:
        assert not camera_rest, "The other clip lost its camera rest when saving an active camera pose"
    return camera, marker


def read_image(path):
    with Image.open(path) as image:
        assert image.size == (WIDTH, HEIGHT), path
        return np.asarray(image.convert("RGB"))


def reopen(renderer, model, output, authored, expected, *, active_save=False):
    camera, marker = check_export(model, active_save=active_save)
    prefix = f"reopened-{model.suffix[1:]}"
    script, image = output / f"{prefix}.json", output / f"{prefix}.png"
    labels = [label for label in expected if label.startswith("saved-") and expected[label][3] is not None]
    events = [
        {"frame": 1, "cameraSelection": {"sourceNode": camera}},
        {"frame": 2, "animationClip": 0, "animationTime": 1.5},
        {"frame": 3, "animationClip": 1, "animationTime": expected[labels[0]][3]},
        *[{"frame": 3 + index, "animationTime": expected[label][3]} for index, label in enumerate(labels[1:], 1)],
    ]
    end = events[-1]["frame"]
    save_json(script, {"schemaVersion": 1, "frames": end,
                       "sampleFrames": list(range(3, end + 1)), "telemetryFrames": [2], "events": events})
    run(renderer, model, script, image)
    original_state = state_at(image, 2, end)
    original = original_state["gltfAnimation"]
    assert original["clipCount"] == 2 and original["selectedClip"] == 0 and original["time"] == 1.5
    node, = (entry for entry in original["nodes"] if entry["sourceIndex"] == marker)
    np.testing.assert_allclose(node["translation"], [.6, .1, 0.], atol=1e-7, rtol=0)
    origin = np.asarray(ANIMATED_PARENT_POSITION) if active_save else PARENT_POSITION
    rest_world = origin + yaw_rotation(PARENT_YAW) @ REST_TRANSLATION
    check_pose(original_state, camera, (rest_world, PARENT_YAW + REST_YAW), world_only=True)
    authored_end = max(entry[0] for entry in expected.values())
    last_pixels = None
    changed = False
    for frame, label in enumerate(labels, 3):
        state = state_at(image, frame, end)
        animation = state["gltfAnimation"]
        time = expected[label][3]
        assert animation["clipCount"] == 2 and animation["selectedClip"] == 1 and animation["time"] == time
        check_pose(state, camera, sample(MOVED, time))
        pixels = read_image(capture_path(image, frame, end))
        expected_pixels = read_image(capture_path(authored, expected[label][0], authored_end))
        np.testing.assert_array_equal(pixels, expected_pixels, err_msg=f"{model.suffix} reopened pose at {time}s")
        changed |= last_pixels is not None and not np.array_equal(last_pixels, pixels)
        last_pixels = pixels
    assert changed, f"Camera motion did not change any presented pixels after reopening {model}"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("renderer", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--generate-only", action="store_true")
    args = parser.parse_args()
    renderer, output = args.renderer.resolve(), args.output.resolve()
    output.mkdir(parents=True)
    (output / "vk_layer_settings.txt").write_text("khronos_validation.validate_sync = true\n", encoding="ascii")
    source = output / "fixture.gltf"
    matrix_source = output / "matrix-fixture.gltf"
    fixture(source)
    fixture(matrix_source, matrix_camera=True)
    all_edits = output / "edit.json"
    save_glb = output / "edit-glb.json"
    expected = edit_sequence(all_edits)
    active_expected = edit_sequence(save_glb, active_save=True)
    if args.generate_only:
        return
    authored = output / "authored.png"
    written_gltf, written_glb = output / "written.gltf", output / "written.glb"
    run(renderer, source, all_edits, authored, save=written_gltf)
    check_edit_sequence(authored, expected)
    active_image = output / "authored-active.png"
    run(renderer, matrix_source, save_glb, active_image, save=written_glb)
    check_matrix_promotion(active_image, active_expected)
    active_frame, _, _, active_time, _ = active_expected["saved-end"]
    parent_key = state_at(active_image, active_expected["animated-parent-key"][0], active_frame)
    assert parent_key["gltfAnimation"]["selectedClip"] == 1 and parent_key["gltfAnimation"]["time"] == 0.
    check_pose(parent_key, 1, A)
    active = state_at(active_image, active_frame, active_frame)
    assert active["gltfAnimation"]["selectedClip"] == 1 and active["gltfAnimation"]["time"] == active_time
    check_pose(active, 1, C)
    reopen(renderer, written_gltf, output, authored, expected)
    reopen(renderer, written_glb, output, authored, expected, active_save=True)
    first_frame = expected["created"][0]
    last_frame = expected["saved-rest"][0]
    state = state_at(authored, first_frame, last_frame)
    save_json(output / "results.json", {"passed": True, "build": state["build"], "gpu": state["gpu"],
                "validatedEditSamples": len(expected), "roundtripFormats": ["gltf", "glb"]})
    print("PASS parented camera key capture, interpolation, playback, update, move, delete, and undo/redo", flush=True)
    print("PASS matrix-authored camera promotes to TRS and undo restores its original matrix", flush=True)
    print("PASS glTF and GLB preserve both clips and reopen with pixel-identical camera poses", flush=True)


if __name__ == "__main__":
    main()
