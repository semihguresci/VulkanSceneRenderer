"""Exercise the Tekla demo's CPU contracts without native tools or Vulkan."""

from copy import deepcopy
import hashlib
import json
import math
from pathlib import Path
import re
import struct
import sys
import tempfile
import unittest
from unittest.mock import patch


sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools"))
import bim_tekla_robotics_demo as demo
from bim_robotics_camera_exr import camera_pixel, compare_camera_frames
from bim_robotics_replay_usd import write_replay_stage


def sample(tick, position, yaw):
    return {"tick": tick, "base": {"position": position, "yaw": yaw},
            "obstruction": "none", "blockingEntityId": ""}


def report_with_history(history):
    return {"schema": "container.bim-robotics-simulation", "version": 1,
            "coordinateConvention": "ROS REP-103", "ticksPerSecond": 60, "history": history}


def tiny_camera_exr(second_row=None, pixel_types=None):
    """Create two one-pixel scanlines with FLOAT and UINT sensor channels."""
    formats = {"B": 2, "G": 2, "InstanceID": 0, "R": 2, "SemanticID": 0, "Z": 2}
    formats.update(pixel_types or {})
    first = {"B": 0., "G": 1., "InstanceID": 11441, "R": .25, "SemanticID": 5, "Z": 0.}

    def attribute(name, kind, value):
        return name.encode("ascii") + b"\0" + kind.encode("ascii") + b"\0" + struct.pack("<I", len(value)) + value

    channels = b"".join(name.encode("ascii") + b"\0" + struct.pack("<iB3xii", kind, 0, 1, 1)
                        for name, kind in formats.items()) + b"\0"
    window = struct.pack("<4i", 0, 0, 0, 1)
    header = (struct.pack("<II", 20000630, 2) + attribute("channels", "chlist", channels) +
              attribute("compression", "compression", b"\0") + attribute("dataWindow", "box2i", window) +
              attribute("displayWindow", "box2i", window) + attribute("lineOrder", "lineOrder", b"\0") + b"\0")
    rows = []
    for y, pixel in enumerate((first, {**first, **(second_row or {})})):
        data = b"".join(struct.pack("<I" if kind == 0 else "<f", pixel[name]) for name, kind in formats.items())
        rows.append(struct.pack("<iI", y, len(data)) + data)
    first_offset = len(header) + 16
    return header + struct.pack("<QQ", first_offset, first_offset + len(rows[0])) + b"".join(rows)


class TeklaDemoTests(unittest.TestCase):
    def setUp(self):
        directory = tempfile.TemporaryDirectory(prefix="tekla-demo-tests-")
        self.addCleanup(directory.cleanup)
        self.work = Path(directory.name)

    def test_authored_drive_schedule_and_camera_cadence(self):
        demo.write_inputs(self.work)
        scenario = demo.read_object(self.work / "scenario.json")
        self.assertEqual((scenario["ticks"], scenario["ticksPerSecond"]), (570, 60))
        self.assertEqual(scenario["initialFeet"], [6.5, -20.75, 12.45])
        self.assertEqual(scenario["initialYaw"], 0)
        self.assertEqual([(c["tick"], c["linear"], c["angular"]) for c in scenario["commands"]], [
            (0, .4, 0), (150, 0, math.pi / 4), (270, .4, 0),
            (420, 0, -math.pi / 4), (540, 0, 0),
        ])
        self.assertEqual(scenario["lidar"]["azimuthSamples"], 96)
        self.assertEqual(scenario["lidar"]["elevationAngles"], [0])
        self.assertEqual(scenario["lidar"]["scansPerSecondMilliHz"], 10_000)
        self.assertEqual(scenario["driveSensors"]["samplesPerSecondMilliHz"], 20_000)
        self.assertEqual(scenario["seed"], scenario["lidar"]["seed"])
        self.assertEqual(scenario["seed"], scenario["driveSensors"]["seed"])
        camera = demo.read_object(self.work / "camera.json")
        self.assertEqual(camera["timing"], {"ticksPerSecond": 60, "framesPerSecondMilliHz": 2000,
                                            "firstTick": 0, "lastTick": 570})
        self.assertEqual((camera["intrinsics"]["width"], camera["intrinsics"]["height"]), (320, 240))

    def test_motion_requires_matching_source_committed_ticks_and_goal_pose(self):
        source = {"byteSize": 3, "sha256": hashlib.sha256(b"BIM").hexdigest()}
        history = []
        # An analytical trace exercises the verifier separately from the native physics run.
        for tick in range(571):
            east = min(tick, 150) / 150
            north = min(max(tick - 270, 0), 150) / 150
            left = min(max(tick - 150, 0), 120) / 120
            right = min(max(tick - 420, 0), 120) / 120
            history.append(sample(tick, [6.5 + east, -20.75 + north, 12.45], (left - right) * math.pi / 2))
        report = {**report_with_history(history), "sceneSource": {"layers": [source]},
                  "sourceNamespace": demo.NAMESPACE, "seed": demo.SEED, "state": deepcopy(history[-1])}

        self.assertEqual(demo.check_motion(report, source)[-1], [7.5, -19.75, 12.45])
        with self.assertRaisesRegex(RuntimeError, "pinned source"):
            demo.check_motion(report, {**source, "byteSize": 4})
        changed = deepcopy(report)
        del changed["history"][1]
        with self.assertRaisesRegex(RuntimeError, "committed tick"):
            demo.check_motion(changed, source)
        changed = deepcopy(report)
        changed["history"][172].update(obstruction="blocked", blockingEntityId="fixture-column")
        with self.assertRaisesRegex(RuntimeError, "blocked at tick 172"):
            demo.check_motion(changed, source)
        changed = deepcopy(report)
        changed["history"][420]["base"]["yaw"] = 0
        with self.assertRaisesRegex(RuntimeError, "inspection heading"):
            demo.check_motion(changed, source)

    def test_route_requires_every_moving_sweep_and_a_blocked_inspection_control(self):
        start, corner, goal = [0, 0, 1], [1, 0, 1], [1, 1, 1]
        points = [start, corner, corner, goal, goal]
        expected = [{"from": start, "to": corner}, {"from": corner, "to": goal},
                    {"from": goal, "to": [3, 1, 1]}]
        target = f"bim/{len(demo.NAMESPACE)}:{demo.NAMESPACE}/guid/22:{demo.COLUMN_GUID}"
        passed = {"reachable": True, "reason": "none", "supportSamples": [{"entityId": "fixture-slab"}]}
        results = [{**walk, **passed} for walk in expected[:2]]
        results.append({**expected[-1], "reachable": False, "reason": "blocked",
                        "blockingHit": {"entityId": target}})
        model = self.work / "fixture.ifc"

        def check(records):
            def invoke(label, tool, arguments):
                self.assertEqual((label, tool), ("check-route", "BimRoboticsCollision"))
                source, report, namespace, queries = arguments
                self.assertEqual((source, namespace), (model, demo.NAMESPACE))
                query = demo.read_object(queries)
                self.assertEqual(query["version"], 1)
                self.assertEqual(query["walks"], expected)
                demo.write_json(report, {"walks": records})

            return demo.check_route(self.work, invoke, model, points)

        self.assertEqual(check(results), 2)
        with self.assertRaisesRegex(RuntimeError, "missed a moving segment"):
            check(results[:-1])
        with self.assertRaisesRegex(RuntimeError, "Segment 0 failed"):
            check([{**results[0], "supportSamples": []}, *results[1:]])
        with self.assertRaisesRegex(RuntimeError, "Segment 0 failed"):
            check([results[1], results[0], results[2]])
        for bad_control in ({"reachable": True},
                            {"blockingHit": {"entityId": "fixture-sliding-door"}}):
            with self.subTest(control=bad_control):
                with self.assertRaisesRegex(RuntimeError, "blocked control"):
                    check([*results[:2], {**results[-1], **bad_control}])

    def test_inspection_requires_final_forward_beam_identity_and_range(self):
        target = f"bim/{len(demo.NAMESPACE)}:{demo.NAMESPACE}/guid/22:{demo.COLUMN_GUID}"
        no_hit = {"status": "no-hit", "entityId": None, "rangeMeters": None}
        hit = {"status": "valid", "entityId": target, "rangeMeters": 1.5}
        scan = {"tick": 570, "returns": [no_hit for _ in range(96)]}
        scan["returns"][48] = hit
        report = {"lidar": {"scans": [scan], "azimuthSamples": 96, "raysPerScan": 96,
                            "azimuthStart": -math.pi, "azimuthSpan": math.tau, "elevationAngles": [0]}}
        self.assertEqual(demo.check_column_observation(report),
                         {"guid": demo.COLUMN_GUID, "sourceId": "#14449", **hit})

        scan["tick"] = 569
        with self.assertRaisesRegex(RuntimeError, "scheduled final LiDAR scan"):
            demo.check_column_observation(report)
        scan["tick"] = 570
        scan["returns"].pop()
        with self.assertRaisesRegex(RuntimeError, "scheduled final LiDAR scan"):
            demo.check_column_observation(report)
        scan["returns"].append(no_hit)
        scan["returns"][47] = hit
        for bad_ray in (no_hit, {**hit, "entityId": "fixture-column"}, {**hit, "rangeMeters": 1.55}):
            with self.subTest(ray=bad_ray):
                scan["returns"][48] = bad_ray
                with self.assertRaisesRegex(RuntimeError, "final forward ray"):
                    demo.check_column_observation(report)

    def test_camera_verifier_requires_all_scheduled_ticks_and_semantic_label(self):
        ticks = list(range(0, 571, 30))
        demo.write_json(self.work / "dataset.json", {"frameCount": len(ticks)})
        label_path = self.work / "labels.json"
        labels = {"sourceNamespace": demo.NAMESPACE,
                  "instances": [{"guid": demo.COLUMN_GUID, "occurrenceId": "#14449"}]}
        demo.write_json(label_path, labels)
        frames = []
        for index, tick in enumerate(ticks):
            frame = self.work / f"frame-{index:08d}.json"
            frames.append(frame)
            demo.write_json(frame, {"tick": tick})
            # Only the wrapper's byte-hash comparison runs on these synthetic payloads.
            frame.with_suffix(".exr").write_bytes(str(tick).encode("ascii"))
        hashes, metadata, manifest = demo.check_camera_capture(self.work)
        self.assertEqual([frame["tick"] for frame in metadata], ticks)
        self.assertEqual(len(set(hashes)), manifest["frameCount"])
        demo.write_json(frames[-1], {"tick": 569})
        with self.assertRaisesRegex(RuntimeError, "missed a scheduled tick"):
            demo.check_camera_capture(self.work)
        demo.write_json(frames[-1], {"tick": 570})
        labels["instances"][0]["occurrenceId"] = "#14450"
        demo.write_json(label_path, labels)
        with self.assertRaisesRegex(RuntimeError, "column occurrence"):
            demo.check_camera_capture(self.work)
        for frame in frames:
            frame.with_suffix(".exr").write_bytes(b"unchanged")
        with self.assertRaisesRegex(RuntimeError, "changing observations"):
            demo.check_camera_capture(self.work)

    def test_exr_accepts_small_finite_rgb_deltas_and_reports_the_maximum(self):
        reference, candidate = self.work / "reference.exr", self.work / "candidate.exr"
        reference.write_bytes(tiny_camera_exr())
        candidate.write_bytes(tiny_camera_exr({"B": .000005, "G": 1.0009}))
        self.assertAlmostEqual(compare_camera_frames(reference, candidate, 1, 2), .0009, places=7)
        candidate.write_bytes(tiny_camera_exr({"B": .000011, "G": 1.0009}))
        with self.assertRaisesRegex(ValueError, r"B differs.+row 1, column 0"):
            compare_camera_frames(reference, candidate, 1, 2)
        candidate.write_bytes(tiny_camera_exr({"G": 1.0011}))
        with self.assertRaisesRegex(ValueError, r"G differs.+row 1, column 0"):
            compare_camera_frames(reference, candidate, 1, 2)

    def test_exr_depth_and_ids_remain_byte_exact_even_when_rgb_is_close(self):
        reference, candidate = self.work / "reference.exr", self.work / "candidate.exr"
        reference.write_bytes(tiny_camera_exr({"InstanceID": 4_000_000_001}))
        for channel, value in (("Z", -0.), ("Z", .000001), ("InstanceID", 4_000_000_002), ("SemanticID", 6)):
            with self.subTest(channel=channel, value=value):
                pixel = {"InstanceID": 4_000_000_001, "G": 1.0009}
                pixel[channel] = value
                candidate.write_bytes(tiny_camera_exr(pixel))
                with self.assertRaisesRegex(ValueError, rf"{channel} differs.+row 1, column 0"):
                    compare_camera_frames(reference, candidate, 1, 2)

    def test_exr_pixel_preserves_unsigned_labels_without_float_conversion(self):
        frame = self.work / "typed.exr"
        frame.write_bytes(tiny_camera_exr({"InstanceID": 4_000_000_001, "SemanticID": 3_000_000_003, "Z": 1.375}))
        first = camera_pixel(frame, 1, 2, 0, 0)
        second = camera_pixel(frame, 1, 2, 0, 1)
        self.assertEqual((first["InstanceID"], first["SemanticID"]), (11441, 5))
        self.assertEqual((second["InstanceID"], second["SemanticID"]), (4_000_000_001, 3_000_000_003))
        self.assertIs(type(second["InstanceID"]), int)
        self.assertIs(type(second["SemanticID"]), int)
        self.assertEqual(second["Z"], 1.375)
        self.assertIs(type(second["Z"]), float)

    def test_exr_rejects_nonfinite_rgb_and_malformed_scanline_files(self):
        frame = self.work / "malformed.exr"
        valid = tiny_camera_exr()
        invalid_row = bytearray(valid)
        # The last chunk contains an eight-byte header and six four-byte samples.
        struct.pack_into("<i", invalid_row, len(valid) - (8 + 6 * 4), 0)
        malformed = [
            (tiny_camera_exr({"R": math.nan}), r"nonfinite EXR R at row 1, column 0"),
            (tiny_camera_exr(pixel_types={"SemanticID": 2}), r"SemanticID must use UINT"),
            (invalid_row, r"invalid EXR row 1"),
            (valid[:-1], r"EXR scanline payload"),
        ]
        for data, diagnostic in malformed:
            with self.subTest(diagnostic=diagnostic):
                frame.write_bytes(data)
                with self.assertRaisesRegex(ValueError, diagnostic):
                    compare_camera_frames(frame, frame, 1, 2)

    def test_wrong_model_fails_before_invoking_native_tools_or_creating_output(self):
        model = self.work / "wrong.ifc"
        model.write_bytes(b"small unrelated fixture")
        output = self.work / "output"
        with patch.object(demo, "run_stage") as stage:
            with self.assertRaisesRegex(RuntimeError, "pinned Tekla House IFC"):
                demo.run_demo(output, self.work, model, 10, cpu_only=True)
        stage.assert_not_called()
        self.assertFalse(output.exists())

    def test_usda_wrapper_uses_relative_bim_layer_and_unwraps_yaw_both_ways(self):
        scene = self.work / "inspection scene.usda"
        scene_bytes = b'#usda 1.0\n\ndef Xform "World" {}\n'
        scene.write_bytes(scene_bytes)
        replay = self.work / "replay.usda"
        for signs in (1, -1):
            with self.subTest(direction=signs):
                history = [sample(tick, [tick, 2, 3], math.radians(signs * yaw))
                           for tick, yaw in enumerate((179, -179, -178))]
                robot_id = 'Robot "Δ"'
                write_replay_stage(replay, scene.name, {**report_with_history(history), "robotId": robot_id})
                stage = replay.read_text(encoding="utf-8")
                self.assertTrue(stage.startswith("#usda 1.0\n"))
                self.assertIn(f"subLayers = [@{scene.name}@]", stage)
                self.assertNotIn(str(self.work), stage)
                self.assertIn('upAxis = "Z"', stage)
                self.assertIn("metersPerUnit = 1", stage)
                self.assertIn("endTimeCode = 2", stage)
                self.assertIn("timeCodesPerSecond = 60", stage)
                self.assertIn('def Xform "ReplayRobot"', stage)
                self.assertIn("2: (2.0, 2.0, 3.0)", stage)
                encoded_id = re.search(r"robotics:robotId = (.+)", stage).group(1)
                self.assertEqual(json.loads(encoded_id), robot_id)
                rotation_text = re.search(r"xformOp:rotateZ\.timeSamples = \{([^}]+)\}", stage).group(1)
                rotations = [(int(tick), float(degrees))
                             for tick, degrees in re.findall(r"(\d+): ([^,\n]+)", rotation_text)]
                self.assertEqual([tick for tick, _ in rotations], [0, 1, 2])
                for (_, measured), expected in zip(rotations, (179, 181, 182)):
                    self.assertAlmostEqual(measured, signs * expected)
        self.assertEqual(scene.read_bytes(), scene_bytes)

    def test_usda_validation_keeps_existing_output_on_malformed_input(self):
        replay = self.work / "replay.usda"
        previous = b"# previous replay\n"
        replay.write_bytes(previous)
        valid = report_with_history([sample(0, [0, 0, 1], 0)])
        invalid_reports = [
            {**valid, "schema": "wrong-schema"},
            {**valid, "version": True},
            {**valid, "ticksPerSecond": False},
            {**valid, "history": []},
            {**valid, "history": [sample(1, [0, 0, 1], 0)]},
            {**valid, "history": [sample(0, [0, 1], 0)]},
            {**valid, "history": [sample(0, [0, 0, math.nan], 0)]},
            {**valid, "history": [sample(0, [0, 0, 1], math.inf)]},
            {**valid, "robotId": 1},
        ]
        for report in invalid_reports:
            with self.subTest(report=report):
                with self.assertRaises(ValueError):
                    write_replay_stage(replay, "scene.usda", report)
        for unsafe in ("../scene.usda", "folder\\scene.usda", "C:scene.usda", "scene@extra.usda", replay.name):
            with self.subTest(scene=unsafe):
                with self.assertRaises(ValueError):
                    write_replay_stage(replay, unsafe, valid)
        self.assertEqual(list(self.work.iterdir()), [replay])
        self.assertEqual(replay.read_bytes(), previous)


if __name__ == "__main__":
    unittest.main()
