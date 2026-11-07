"""CPU failure checks for native pending Play/Detach edit evidence."""
import copy
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

import physics_pending_edit_regression as runner


def synthetic(directory, case, technique="deferred-raster", *, defect=None):
    import numpy as np
    _, _, _, specification = runner.fixture(case, Path(directory) / case)
    record = {"fixture": specification, "technique": technique, "mode": "raster", "validationErrors": [], "samples": []}
    prior = runner.expected_ticks(case, 8)
    paused = np.array([runner.displacement(0, prior), 1, 0])
    for frame in runner.SAMPLE_FRAMES:
        live, unloaded = frame < 23, frame >= 27
        ticks = runner.expected_ticks(case, frame) if live else 0
        position = np.array([runner.displacement(0, ticks), 1, 0]) if frame <= 8 else paused + np.asarray(runner.EDIT) + [runner.displacement(prior, ticks-prior), 0, 0]
        if frame >= 9 and live:
            if defect == "lost-edit":
                position -= runner.EDIT
            elif defect == "reapplied-every-tick" and ticks > prior:
                position = paused + runner.EDIT + np.array([runner.displacement(prior, 1), 0, 0])
            elif defect == "lost-velocity" and ticks > prior:
                position = paused + runner.EDIT
            elif defect == "lost-child-offset":
                position -= [paused[0], 0, 0]
        model = runner.matrix(specification["primary"]["authoredModel"])
        model[:3, 3] = position
        bodies = [{"node": specification["primary"]["node"], "motion": 2,
            "position": position.tolist(), "sceneWorldMatrix": runner.encode(model), "renderWorldMatrix": runner.encode(model)}] if live else []
        elements = []
        if case == "pending-detach" and live and (2 <= frame <= 8 or frame >= 11):
            z = runner.displacement(0, ticks-runner.TICKS_PER_FRAME) if frame <= 8 else 0
            if frame >= 11 and defect == "unrestored-bim":
                z = runner.displacement(0, prior-runner.TICKS_PER_FRAME)
            proxy = runner.translation([-4, 1, z])
            auxiliary_model = runner.translation([0, 0, z]) @ runner.matrix(runner.BIM_ROTATION)
            vertices = runner.transform_vertices(auxiliary_model, specification["bimGeometry"]["0"]["vertices"])
            normal = np.zeros((4, 3))
            normal[:3, :] = np.linalg.inv(auxiliary_model[:3, :3]).T
            obj = {"objectIndex": 0, "guid": runner.GUID, "sourceId": "stable-source",
                "model": runner.encode(auxiliary_model), "normalMatrix": runner.encode(normal),
                "boundsMin": vertices.min(axis=0).tolist(), "boundsMax": vertices.max(axis=0).tolist()}
            bodies.append({"node": 50, "motion": 2 if frame <= 8 else 0, "position": [-4, 1, z],
                "sceneWorldMatrix": runner.encode(proxy), "renderWorldMatrix": []})
            elements.append({"node": 50, "productId": 7, "objects": [obj]})
        state = {"available": True, "playing": live and frame != 8 and not (case == "pending-detach" and frame == 9),
            "error": "", "droppedSeconds": 0, "jointCount": 0, "totalSteps": ticks,
            "bodyCount": len(bodies), "bodies": bodies, "auxiliaryElements": elements}
        scene = {"primaryObjectCount": 0 if unloaded else 1,
                 "auxiliaryObjectCount": 0 if unloaded or case == "pending-play" else 1}
        png = Path(directory) / f"{case}-{technique}-{frame}.png"
        png.write_bytes(f"{case} frame {frame}".encode())
        record["samples"].append({"frame": frame, "png": str(png), "telemetry": {"physics": state, "scene": scene}})
    return record


class StandardLibraryTests(unittest.TestCase):
    def test_help_and_opt_in_skip_do_not_create_output(self):
        with tempfile.TemporaryDirectory() as temporary:
            output = Path(temporary) / "not-created"
            environment = dict(os.environ)
            environment.pop("CONTAINER_RUN_GPU_PHYSICS", None)
            for arguments, status in ((["--help"], 0), (["--output", str(output), "--require-opt-in"], 77)):
                process = subprocess.run([sys.executable, "-S", "-B", str(Path(runner.__file__)), *arguments],
                    capture_output=True, text=True, env=environment)
                self.assertEqual(process.returncode, status, process.stderr)
                self.assertFalse(output.exists())

    def test_missing_executable_fails_before_output_creation(self):
        with tempfile.TemporaryDirectory() as temporary:
            output = Path(temporary) / "not-created"
            process = subprocess.run([sys.executable, "-S", "-B", str(Path(runner.__file__)), "--output", str(output)],
                capture_output=True, text=True)
            self.assertEqual(process.returncode, 2, process.stderr)
            self.assertFalse(output.exists())


class PendingEditTests(unittest.TestCase):
    def setUp(self):
        if importlib.util.find_spec("numpy") is None:
            self.skipTest("numpy unavailable")
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)

    def record(self, case="pending-play", **options):
        return synthetic(self.temporary.name, case, **options)

    @staticmethod
    def state(record, frame):
        return next(sample["telemetry"]["physics"] for sample in record["samples"] if sample["frame"] == frame)

    def test_valid_complete_four_case_matrix(self):
        records = [self.record(case, technique=technique) for case in runner.CASES for technique in runner.TECHNIQUES]
        for record in records:
            with self.subTest(case=record["fixture"]["case"], technique=record["technique"]):
                self.assertTrue(runner.check(record)["passed"])
        self.assertEqual(len(list(runner.check_matrix({"expectedCases": [[case, technique] for case in runner.CASES for technique in runner.TECHNIQUES], "cases": records}))), 4)

    def test_fixture_uses_adjacent_named_parent_jump_and_bim_reattachment(self):
        _, auxiliary, sequence, fixture = runner.fixture("pending-detach", Path(self.temporary.name) / "fixture")
        self.assertTrue(auxiliary.is_file())
        self.assertEqual(sequence["objectNode"], 0)
        self.assertEqual(fixture["primary"]["node"], 1)
        self.assertEqual(sequence["objectName"], fixture["animatedName"])
        self.assertEqual([key["frame"] for key in sequence["objectTranslation"]], [1, 8, 9, 30])
        self.assertEqual(sequence["objectTranslation"][2]["translation"], [4, 2, 0])
        self.assertEqual(next(event for event in sequence["events"] if event["frame"] == 9)["physicsAction"], "detach")
        self.assertEqual(next(event for event in sequence["events"] if event["frame"] == 11)["physicsAttach"], "static")
        self.assertEqual(fixture["ticksPerFrame"], 4)

    def test_lost_same_frame_edit_and_child_offset_fail_with_coherent_geometry(self):
        for case in runner.CASES:
            for defect in ("lost-edit", "lost-child-offset"):
                with self.subTest(case=case, defect=defect):
                    with self.assertRaisesRegex(AssertionError, "world edit or retained velocity"):
                        runner.check(self.record(case, defect=defect))

    def test_repeated_first_batch_teleports_and_lost_velocity_are_rejected(self):
        for defect in ("reapplied-every-tick", "lost-velocity"):
            with self.subTest(defect=defect):
                with self.assertRaisesRegex(AssertionError, "lost/reapplied"):
                    runner.check(self.record(defect=defect))

    def test_detach_must_restore_bim_authored_geometry_after_actual_motion(self):
        with self.assertRaisesRegex(AssertionError, "detach did not restore"):
            runner.check(self.record("pending-detach", defect="unrestored-bim"))

    def test_physics_fixed_steps_and_pause_states_must_match_event_batch(self):
        record = self.record()
        self.state(record, 9)["totalSteps"] -= 3
        with self.assertRaisesRegex(AssertionError, "fixed-step batch was interrupted"):
            runner.check(record)
        record = self.record("pending-detach")
        self.state(record, 9)["playing"] = True
        with self.assertRaisesRegex(AssertionError, "playback state is incorrect"):
            runner.check(record)

    def test_primary_rendered_matrix_cannot_lag_valid_physics_pose(self):
        record = self.record()
        self.state(record, 9)["bodies"][0]["renderWorldMatrix"][12] -= 4
        with self.assertRaisesRegex(AssertionError, "rendered pending-edit transform is stale"):
            runner.check(record)

    def test_bim_proxy_bounds_and_normal_consistency_is_checked(self):
        record = self.record("pending-detach")
        self.state(record, 11)["bodies"][1]["sceneWorldMatrix"][12] += 1
        with self.assertRaisesRegex(AssertionError, "proxy disagrees with its physical position"):
            runner.check(record)
        record = self.record("pending-detach")
        self.state(record, 11)["auxiliaryElements"][0]["objects"][0]["boundsMin"][0] += .2
        with self.assertRaisesRegex(AssertionError, "bounds are stale"):
            runner.check(record)
        record = self.record("pending-detach")
        self.state(record, 11)["auxiliaryElements"][0]["objects"][0]["normalMatrix"][0] = 0
        with self.assertRaisesRegex(AssertionError, "normals are stale"):
            runner.check(record)

    def test_identity_and_body_counts_must_survive_detach_reattach(self):
        record = self.record("pending-detach")
        self.state(record, 11)["auxiliaryElements"][0]["productId"] += 1
        with self.assertRaisesRegex(AssertionError, "Detach changed BIM product identity"):
            runner.check(record)
        record = self.record("pending-detach")
        self.state(record, 9)["bodyCount"] = 2
        with self.assertRaisesRegex(AssertionError, "body count changed"):
            runner.check(record)

    def test_wrong_bim_proxy_basis_cannot_pass_with_unchanged_position_and_geometry(self):
        record = self.record("pending-detach")
        self.state(record, 11)["bodies"][1]["sceneWorldMatrix"][0] = 2
        with self.assertRaisesRegex(AssertionError, "proxy lost its expected translation-only pose"):
            runner.check(record)

    def test_incomplete_nonfinite_matrices_and_runtime_errors_fail(self):
        record = self.record()
        self.state(record, 9)["bodies"][0]["renderWorldMatrix"] = [1] * 15
        with self.assertRaisesRegex(AssertionError, "Invalid captured physics matrix"):
            runner.check(record)
        record = self.record()
        self.state(record, 9)["bodies"][0]["sceneWorldMatrix"][0] = float("nan")
        with self.assertRaisesRegex(AssertionError, "Invalid captured physics matrix"):
            runner.check(record)
        record = self.record()
        self.state(record, 20)["error"] = "runtime failure"
        with self.assertRaisesRegex(AssertionError, "Physics failed"):
            runner.check(record)

    def test_clear_unload_and_geometry_publication_are_checked(self):
        record = self.record()
        self.state(record, 24)["bodyCount"] = 1
        with self.assertRaisesRegex(AssertionError, "Clear retained"):
            runner.check(record)
        record = self.record("pending-detach")
        record["samples"][-1]["telemetry"]["scene"]["auxiliaryObjectCount"] = 1
        with self.assertRaisesRegex(AssertionError, "Unload retained pending-edit geometry"):
            runner.check(record)

    def test_missing_duplicate_and_empty_or_unchanged_images_fail(self):
        record = self.record()
        record["samples"].append(copy.deepcopy(record["samples"][0]))
        with self.assertRaisesRegex(AssertionError, "Incomplete pending-edit captures"):
            runner.check(record)
        for contents in (b"", b"unchanged"):
            record = self.record()
            for sample in record["samples"]:
                if sample["frame"] in (7, 9, 20):
                    Path(sample["png"]).write_bytes(contents)
            with self.assertRaisesRegex(AssertionError, "Empty pending-edit image|did not change captured PNGs"):
                runner.check(record)

    def test_matrix_rejects_missing_duplicate_and_unknown_conditions(self):
        record = self.record()
        expected = [["pending-play", "deferred-raster"]]
        for records in ([], [record, copy.deepcopy(record)]):
            with self.assertRaisesRegex(AssertionError, "Incomplete pending-edit case matrix"):
                runner.check_matrix({"expectedCases": expected, "cases": records})
        for invalid in (expected * 2, [["unknown", "deferred-raster"]]):
            with self.assertRaisesRegex(AssertionError, "Unknown or duplicate"):
                runner.check_matrix({"expectedCases": invalid, "cases": [record]})

    def test_check_existing_revalidates_and_clears_previous_pass(self):
        directory = Path(self.temporary.name)
        record = self.record()
        record["checks"] = {"passed": True}
        report = directory / "results.json"
        report.write_text(json.dumps({"passed": True, "expectedCases": [["pending-play", "deferred-raster"]], "cases": [record]}))
        arguments = [str(Path(runner.__file__)), "--output", str(directory), "--check-existing"]
        with mock.patch.object(sys, "argv", arguments):
            self.assertEqual(runner.main(), 0)
        saved = json.loads(report.read_text())
        self.state(saved["cases"][0], 9)["playing"] = False
        report.write_text(json.dumps(saved))
        with mock.patch.object(sys, "argv", arguments):
            self.assertEqual(runner.main(), 1)
        saved = json.loads(report.read_text())
        self.assertFalse(saved["passed"])
        self.assertNotIn("checks", saved["cases"][0])


if __name__ == "__main__":
    unittest.main()
