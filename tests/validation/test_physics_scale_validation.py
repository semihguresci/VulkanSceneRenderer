"""CPU failure checks for native collider growth and source-unit scale evidence."""
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

import physics_scale_regression as runner


def synthetic(directory, case, technique="deferred-raster", *, resolves=True, applies_scale=True):
    import numpy as np
    _, _, _, specification = runner.fixture(case, Path(directory) / case)
    record = {"fixture": specification, "technique": technique, "mode": "raster", "validationErrors": [], "samples": []}
    for frame in runner.SAMPLE_FRAMES[case]:
        live = frame < specification["clearFrame"]
        unloaded = frame >= specification["unloadFrame"]
        bodies, elements = [], []
        for name, geometry in specification["primary"].items():
            if not live or case == "static-growth" and frame == 1 and name != specification["animatedName"]:
                continue
            model = runner.matrix(geometry["authoredModel"])
            if name == specification["animatedName"] and applies_scale:
                model[:3, :3] = model[:3, :3] @ np.diag(runner.relative_scale(case, frame))
            bodies.append({"node": geometry["node"], "motion": 0 if case == "static-growth" and name == specification["animatedName"] else 2,
                "position": model[:3, 3].tolist(), "sceneWorldMatrix": runner.encode(model), "renderWorldMatrix": runner.encode(model)})
        if case == "static-growth" and live and frame != 1:
            y = 0 if frame <= 90 or not resolves else -.3 if frame == 92 else -.98
            proxy = runner.translation([4, y, 0])
            model = proxy @ np.linalg.inv(runner.translation([4, 0, 0])) @ runner.matrix(runner.BIM_ROTATION)
            vertices = runner.transform_vertices(model, specification["bimGeometry"]["0"]["vertices"])
            normal = np.zeros((4, 3))
            normal[:3, :] = np.linalg.inv(model[:3, :3]).T
            obj = {"objectIndex": 0, "guid": runner.GUID, "sourceId": "stable-source",
                "model": runner.encode(model), "normalMatrix": runner.encode(normal),
                "boundsMin": vertices.min(axis=0).tolist(), "boundsMax": vertices.max(axis=0).tolist()}
            bodies.append({"node": 50, "motion": 2, "position": [4, y, 0], "sceneWorldMatrix": runner.encode(proxy), "renderWorldMatrix": []})
            elements.append({"node": 50, "productId": 7, "objects": [obj]})
        state = {"available": True, "playing": live and (case == "unit-scale" or frame > 1),
            "error": "", "droppedSeconds": 0, "jointCount": 0, "bodyCount": len(bodies),
            "activeBodyCount": (2 if frame == 4 else 1 if frame == 92 else 0) if case == "static-growth" else 0,
            "totalSteps": frame if live else 0, "bodies": bodies, "auxiliaryElements": elements}
        scene = {"primaryObjectCount": 0 if unloaded else len(specification["primary"]),
                 "auxiliaryObjectCount": 0 if unloaded or case == "unit-scale" else 1}
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
                result = subprocess.run([sys.executable, "-S", "-B", str(Path(runner.__file__)), *arguments],
                    capture_output=True, text=True, env=environment)
                self.assertEqual(result.returncode, status, result.stderr)
                self.assertFalse(output.exists())

    def test_missing_executable_fails_before_output_creation(self):
        with tempfile.TemporaryDirectory() as temporary:
            output = Path(temporary) / "not-created"
            result = subprocess.run([sys.executable, "-S", "-B", str(Path(runner.__file__)), "--output", str(output)],
                capture_output=True, text=True)
            self.assertEqual(result.returncode, 2, result.stderr)
            self.assertFalse(output.exists())


class ScaleValidationTests(unittest.TestCase):
    def setUp(self):
        if importlib.util.find_spec("numpy") is None:
            self.skipTest("numpy unavailable")
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)

    def record(self, case="static-growth", **options):
        return synthetic(self.temporary.name, case, **options)

    @staticmethod
    def state(record, frame):
        return next(sample["telemetry"]["physics"] for sample in record["samples"] if sample["frame"] == frame)

    def test_valid_complete_four_case_matrix(self):
        cases = [self.record(case, technique=technique) for case in runner.CASES for technique in runner.TECHNIQUES]
        for record in cases:
            with self.subTest(case=record["fixture"]["case"], technique=record["technique"]):
                self.assertTrue(runner.check(record)["passed"])
        results = {"expectedCases": [[case, technique] for case in runner.CASES for technique in runner.TECHNIQUES], "cases": cases}
        self.assertEqual(len(list(runner.check_matrix(results))), 4)

    def test_unit_geometry_and_named_scale_track_target_the_source_parent(self):
        _, auxiliary, sequence, specification = runner.fixture("unit-scale", Path(self.temporary.name) / "unit")
        geometry = specification["primary"][specification["animatedName"]]
        self.assertIsNone(auxiliary)
        self.assertEqual(sequence["objectNode"], 0)
        self.assertEqual(sequence["objectName"], specification["animatedName"])
        self.assertEqual(geometry["node"], 1)
        self.assertEqual(max(vertex[0] for vertex in geometry["vertices"]), 10000)
        self.assertEqual([key["frame"] for key in sequence["objectScale"]], [1, 3, 4, 40])
        self.assertEqual(sequence["objectScale"][-1]["scale"], [1.005] * 3)
        self.assertGreater(sequence["objectScale"][-1]["frame"], specification["unloadFrame"])

    def test_growth_must_begin_with_sleep_and_wake_only_the_new_overlap(self):
        record = self.record()
        self.state(record, 90)["activeBodyCount"] = 1
        with self.assertRaisesRegex(AssertionError, "had not settled"):
            runner.check(record)
        for count in (0, 2):
            record = self.record()
            self.state(record, 92)["activeBodyCount"] = count
            with self.assertRaisesRegex(AssertionError, "wake only the new overlap"):
                runner.check(record)

    def test_old_embedded_sleeping_pose_cannot_pass_with_consistent_render_geometry(self):
        with self.assertRaisesRegex(AssertionError, "did not resolve the sleeping overlap"):
            runner.check(self.record(resolves=False))

    def test_small_authored_scale_cannot_be_overwritten_by_simulation(self):
        with self.assertRaisesRegex(AssertionError, "Authored scale did not persist"):
            runner.check(self.record("unit-scale", applies_scale=False))

    def test_remote_sleeping_primary_stays_at_its_authored_pose(self):
        record = self.record()
        remote_node = record["fixture"]["primary"]["Remote sleeping primary cube"]["node"]
        remote = next(body for body in self.state(record, 120)["bodies"] if body["node"] == remote_node)
        remote["position"][0] += 1
        remote["sceneWorldMatrix"][12] += 1
        remote["renderWorldMatrix"][12] += 1
        with self.assertRaisesRegex(AssertionError, "Authored scale did not persist"):
            runner.check(record)

    def test_bim_product_identity_and_geometry_publication_must_stay_stable(self):
        record = self.record()
        self.state(record, 150)["auxiliaryElements"][0]["productId"] += 1
        with self.assertRaisesRegex(AssertionError, "changed BIM product identity"):
            runner.check(record)
        record = self.record()
        record["samples"][4]["telemetry"]["scene"]["auxiliaryObjectCount"] += 1
        with self.assertRaisesRegex(AssertionError, "changed imported geometry counts"):
            runner.check(record)

    def test_rendered_matrices_bounds_and_normals_are_checked_independently(self):
        record = self.record()
        self.state(record, 120)["auxiliaryElements"][0]["objects"][0]["boundsMin"][1] += .3
        with self.assertRaisesRegex(AssertionError, "bounds are stale"):
            runner.check(record)
        record = self.record()
        self.state(record, 120)["auxiliaryElements"][0]["objects"][0]["normalMatrix"][0] = 0
        with self.assertRaisesRegex(AssertionError, "normals are stale"):
            runner.check(record)
        record = self.record("unit-scale")
        self.state(record, 8)["bodies"][0]["renderWorldMatrix"][0] = .001
        with self.assertRaisesRegex(AssertionError, "rendered scale transform is stale"):
            runner.check(record)

    def test_consistently_shifted_bim_geometry_cannot_disagree_with_physics(self):
        record = self.record()
        for frame in (120, 150):
            state = self.state(record, frame)
            element = state["auxiliaryElements"][0]
            body = next(body for body in state["bodies"] if body["node"] == element["node"])
            # Keep rendered placement, bounds and proxy mutually consistent,
            # but move them away from the real physical position. A geometric
            # separation check alone would accept this unrelated placement.
            body["sceneWorldMatrix"][12] += 100
            obj = element["objects"][0]
            obj["model"][12] += 100
            obj["boundsMin"][0] += 100
            obj["boundsMax"][0] += 100
        with self.assertRaisesRegex(AssertionError, "proxy disagrees with its physical position"):
            runner.check(record)

    def test_incomplete_nonfinite_or_wrong_motion_body_records_fail(self):
        record = self.record("unit-scale")
        self.state(record, 8)["bodies"][0]["renderWorldMatrix"] = [1] * 15
        with self.assertRaisesRegex(AssertionError, "Invalid captured physics matrix"):
            runner.check(record)
        record = self.record("unit-scale")
        self.state(record, 8)["bodies"][0]["sceneWorldMatrix"][0] = float("nan")
        with self.assertRaisesRegex(AssertionError, "Invalid captured physics matrix"):
            runner.check(record)
        record = self.record()
        self.state(record, 120)["bodies"][0]["motion"] = 2
        with self.assertRaisesRegex(AssertionError, "motion type changed"):
            runner.check(record)

    def test_playback_and_fixed_steps_must_continue_after_scale_edit(self):
        record = self.record()
        self.state(record, 120)["playing"] = False
        with self.assertRaisesRegex(AssertionError, "unexpectedly paused"):
            runner.check(record)
        record = self.record("unit-scale")
        self.state(record, 30)["totalSteps"] = self.state(record, 8)["totalSteps"]
        with self.assertRaisesRegex(AssertionError, "interrupted fixed-step playback"):
            runner.check(record)

    def test_clear_and_unload_retire_state_and_only_unload_removes_geometry(self):
        record = self.record()
        self.state(record, 154)["bodyCount"] = 1
        with self.assertRaisesRegex(AssertionError, "Clear retained"):
            runner.check(record)
        record = self.record("unit-scale")
        record["samples"][-1]["telemetry"]["scene"]["primaryObjectCount"] = 1
        with self.assertRaisesRegex(AssertionError, "Unload retained model geometry"):
            runner.check(record)

    def test_incomplete_duplicate_samples_and_unchanged_images_fail(self):
        record = self.record()
        record["samples"].append(copy.deepcopy(record["samples"][0]))
        with self.assertRaisesRegex(AssertionError, "Incomplete scale captures"):
            runner.check(record)
        record = self.record("unit-scale")
        images = {sample["frame"]: sample["png"] for sample in record["samples"]}
        Path(images[30]).write_bytes(Path(images[3]).read_bytes())
        with self.assertRaisesRegex(AssertionError, "did not change captured PNGs"):
            runner.check(record)

    def test_matrix_rejects_missing_duplicate_and_unknown_cases(self):
        record = self.record()
        expected = [["static-growth", "deferred-raster"]]
        for cases in ([], [record, copy.deepcopy(record)]):
            with self.assertRaisesRegex(AssertionError, "Incomplete scale case matrix"):
                runner.check_matrix({"expectedCases": expected, "cases": cases})
        for invalid in (expected * 2, [["unknown", "deferred-raster"]]):
            with self.assertRaisesRegex(AssertionError, "Unknown or duplicate"):
                runner.check_matrix({"expectedCases": invalid, "cases": [record]})

    def test_check_existing_revalidates_and_clears_a_previous_pass(self):
        directory = Path(self.temporary.name)
        record = self.record()
        report = directory / "results.json"
        record["checks"] = {"passed": True}
        report.write_text(json.dumps({"passed": True, "expectedCases": [["static-growth", "deferred-raster"]], "cases": [record]}))
        arguments = [str(Path(runner.__file__)), "--output", str(directory), "--check-existing"]
        with mock.patch.object(sys, "argv", arguments):
            self.assertEqual(runner.main(), 0)
        saved = json.loads(report.read_text())
        self.state(saved["cases"][0], 92)["activeBodyCount"] = 0
        report.write_text(json.dumps(saved))
        with mock.patch.object(sys, "argv", arguments):
            self.assertEqual(runner.main(), 1)
        saved = json.loads(report.read_text())
        self.assertFalse(saved["passed"])
        self.assertNotIn("checks", saved["cases"][0])


if __name__ == "__main__":
    unittest.main()
