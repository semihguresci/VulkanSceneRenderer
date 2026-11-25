"""Check signed geometry motion and the FSR dispatch on a fixed-step sequence."""

import numpy as np

from gltf_accessor_runtime_regression import Asset


DISPLAY = (640, 360)
FRAMES = (3, 4, 5, 6, 7, 8, 9, 12, 13)
CENTER = (320, 180)
OBJECT_SHIFT = (.06, .045, 0)
CAMERA_SHIFT = (.06, .04, 0)


def write_motion_fixture(path):
    asset = Asset()
    asset.model["extensionsUsed"] = ["KHR_materials_unlit"]
    asset.model["nodes"] = []
    for name, width, height, z, color in (
        ("Depth backdrop", 5, 3, 0, [.16, .19, .25, 1]),
        ("Motion probe", 1.1, .7, 2, [.65, .35, .18, 1]),
    ):
        index = len(asset.model["meshes"])
        positions = [(-width, -height, z), (width, -height, z), (width, height, z), (-width, height, z)]
        attributes = {"POSITION": asset.accessor("VEC3", positions),
                      "NORMAL": asset.accessor("VEC3", [(0, 0, 1)] * 4)}
        indices = asset.accessor("SCALAR", [0, 1, 2, 0, 2, 3], 5123, indices=True)
        asset.model["meshes"].append({"primitives": [{"attributes": attributes, "indices": indices, "material": index}]})
        asset.model["materials"].append({"pbrMetallicRoughness": {"baseColorFactor": color},
                                         "extensions": {"KHR_materials_unlit": {}}})
        asset.model["nodes"].append({"name": name, "mesh": index})
    asset.model["scenes"][0]["nodes"] = [0, 1]
    asset.write(path)


def motion_plan(sample_frames, exposure_frame):
    camera = [dict(frame=frame, position=[x, y, 6], target=[x, y, 0])
              for frame, (x, y, _) in ((1, (0, 0, 0)), (7, (0, 0, 0)), (8, CAMERA_SHIFT))]
    translations = [dict(frame=frame, translation=offset)
                    for frame, offset in ((1, (0, 0, 0)), (4, (0, 0, 0)), (5, OBJECT_SHIFT))]
    events = [{"frame": 3, "depthProbe": CENTER}, {"frame": 4, "depthProbe": [120, 180]},
              {"frame": exposure_frame, "exposure": .95},
              {"frame": 10, "skip": True}, {"frame": 11, "skip": True}]
    return {"schemaVersion": 1, "frames": FRAMES[-1], "sampleFrames": sample_frames,
            "camera": camera, "objectName": "Motion probe", "objectTranslation": translations,
            "events": sorted(events, key=lambda event: event["frame"])}


def project(state, point, *, jittered=False):
    key = "viewProjColumns" if jittered else "unjitteredViewProjColumns"
    vp = np.asarray(state["camera"][key], dtype=float).T
    clip = vp @ np.array([*point, 1.0])
    ndc = clip[:3] / clip[3]
    return np.array([.5 + .5 * ndc[0], .5 - .5 * ndc[1]]), ndc[2]


def halton(index, base):
    value = 0.0
    digit = 1.0
    while index:
        index, remainder = divmod(index, base)
        digit /= base
        value += remainder * digit
    return value


def motion_center(frame):
    return [0, 0, 2] if frame < 5 else [OBJECT_SHIFT[0], OBJECT_SHIFT[1], 2]


def velocity_patch(image):
    x, y = CENTER
    srgb = image[y - 6:y + 7, x - 6:x + 7].astype(float) / 255.0
    linear = np.where(srgb <= .04045, srgb / 12.92, ((srgb + .055) / 1.055) ** 2.4)
    # The velocity debug view encodes signed render pixels as .5 + motion / 16.
    assert np.max(np.abs(linear[:, :, 2] - .5)) < .008, "Motion probe contains invalid velocity"
    return np.median((linear[:, :, :2] - .5) * 16, axis=(0, 1))


def verify_motion(states, images, check_mode):
    stats = {}
    initial_epoch = states[FRAMES[0]]["taa"]["epoch"]
    for frame in FRAMES:
        state = states[frame]
        check_mode(state, "quality", "quality", DISPLAY)
        fsr = state["fsr"]
        scale = np.asarray(fsr["renderExtent"], dtype=float)
        np.testing.assert_array_equal(fsr["motionVectorScale"], scale)
        assert fsr["preExposure"] == 1, (frame, fsr)
        expected_elapsed = 50.0 if frame == 12 else 1000.0 / 60.0
        assert abs(fsr["frameTimeMs"] - expected_elapsed) < .03, (frame, fsr)
        assert state["taa"]["epoch"] == initial_epoch, (frame, state["taa"])
        expected_submissions = frame - (2 if frame >= 12 else 0)
        assert state["taa"]["submittedFrame"] == expected_submissions, (frame, state["taa"])
        phases = int(8 * (DISPLAY[0] / scale[0]) ** 2)
        index = (expected_submissions - 1) % phases + 1
        expected_jitter = [.5 - halton(index, 2), .5 - halton(index, 3)]
        np.testing.assert_allclose(fsr["jitterPixels"], expected_jitter, atol=1e-6)
        np.testing.assert_allclose(np.asarray(state["camera"]["jitterUv"][:2]) * scale,
                                   expected_jitter, atol=1e-6)
        raster_uv, raster_depth = project(state, motion_center(frame), jittered=True)
        stable_uv, stable_depth = project(state, motion_center(frame))
        np.testing.assert_allclose((raster_uv - stable_uv) * scale, expected_jitter, atol=1e-5)
        np.testing.assert_allclose(raster_depth, stable_depth, atol=1e-8)

        # The front plane is parallel to both camera views, so its centroid and
        # every interior probe pixel share the same unjittered displacement.
        expected_motion = np.zeros(2)
        if frame in (5, 8):
            prev, _ = project(states[frame - 1], motion_center(frame - 1))
            cur, _ = project(state, motion_center(frame))
            expected_motion = (prev - cur) * scale
        actual = velocity_patch(images[frame])
        np.testing.assert_allclose(actual, expected_motion, atol=.11, err_msg=f"Frame {frame} render-pixel velocity")
        stats[frame] = {"expectedMotionPixels": expected_motion.tolist(), "motionPixels": actual.tolist(),
                        "jitterPixels": fsr["jitterPixels"], "frameTimeMs": fsr["frameTimeMs"]}

    # Match the depth readback against independent finite reverse-Z depth
    # from the viewer's default physical near/far and the two authored planes.
    near, far = .05, 500.0
    for frame, distance in ((3, 4), (4, 6)):
        expected = near * (far - distance) / (distance * (far - near))
        depth = states[frame]["depthProbe"]["opaque"]
        np.testing.assert_allclose(depth, expected, atol=2e-6)
        stats[frame]["reverseZDepth"] = depth
    assert stats[3]["reverseZDepth"] > stats[4]["reverseZDepth"]
    return stats


def verify_fsr_temporal_inputs(output, capture, check_mode):
    fixture = output / "temporal-motion.gltf"
    write_motion_fixture(fixture)
    report = {}
    for technique in ("deferred-raster", "forward-raster"):
        variant = technique.split("-")[0]
        states, images = capture(f"temporal-velocity-{variant}", "quality", motion_plan(FRAMES, 9),
                                 technique=technique, display="taa-velocity", model_path=fixture)
        motion = verify_motion(states, images, check_mode)

        label = f"temporal-exposure-{variant}"
        step, changed = capture(label, "quality", motion_plan([8, 9, 13], 9),
                                technique=technique, model_path=fixture)
        control, reference = capture(f"{label}-control", "quality", motion_plan([8, 9, 13], 1),
                                     technique=technique, model_path=fixture)
        exposure = {}
        for frame, value in ((8, .5), (9, .95), (13, .95)):
            for state, expected in ((step[frame], value), (control[frame], .95)):
                check_mode(state, "quality", "quality", DISPLAY)
                assert state["exposure"]["mode"] == "manual"
                np.testing.assert_allclose(state["exposure"]["resolvedExposure"], expected, atol=1e-6)
                assert state["taa"]["epoch"] == step[8]["taa"]["epoch"]
            error = float(np.mean(np.abs(changed[frame].astype(np.int16) - reference[frame])))
            assert (error > 5 if frame == 8 else error < .1), (technique, frame, error)
            exposure[frame] = {"displayExposure": value, "constantExposureControlMae8Bit": error}
        report[technique] = {"velocity": motion, "exposure": exposure}
    return states[FRAMES[-1]], report
