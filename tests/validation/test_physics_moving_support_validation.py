"""CPU checks for cross-provider kinematic contact evidence."""
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

import physics_moving_support_regression as runner


def synthetic(directory, provider, technique="deferred-raster", *, carries=True, auxiliary_count=2):
    import numpy as np
    _, _, _, fixture = runner.fixture(provider, Path(directory) / provider)
    base = runner.matrix(runner.BIM_ROTATION)
    original = fixture["bimGeometry"]
    vertices = np.concatenate([runner.transform_vertices(base, part["vertices"]) for part in original.values()])
    center = (vertices.min(axis=0) + vertices.max(axis=0))/2
    record = {"fixture": fixture, "technique": technique, "mode": "raster", "validationErrors": [], "samples": []}
    for frame in runner.SAMPLE_FRAMES:
        active, both = frame < 181, 1 < frame < 181
        support_y = runner.lift_height(frame)
        rider_y = 2.99 if frame == 3 else .75 + (support_y if carries else 0)
        primary_support = provider == "gltf"
        primary_y = support_y if primary_support else rider_y
        bim_y = rider_y if primary_support else support_y
        primary_model = runner.matrix(fixture["primary"]["authoredModel"])
        primary_model[1, 3] = primary_y
        primary = {"node": fixture["primary"]["node"], "motion": 1 if primary_support else 2,
                   "position": [0, primary_y, 0], "sceneWorldMatrix": runner.encode(primary_model),
                   "renderWorldMatrix": runner.encode(primary_model)}
        proxy = runner.translation([0, bim_y, 0])
        model = proxy @ np.linalg.inv(runner.translation(center)) @ base
        normal = np.zeros((4, 3))
        normal[:3, :] = np.linalg.inv(model[:3, :3]).T
        fragments = []
        for index, part in original.items():
            world = runner.transform_vertices(model, part["vertices"])
            fragments.append({"objectIndex": int(index), "guid": part["guid"], "sourceId": "stable-source",
                "model": runner.encode(model), "normalMatrix": runner.encode(normal),
                "boundsMin": world.min(axis=0).tolist(), "boundsMax": world.max(axis=0).tolist()})
        auxiliary = {"node": 50, "motion": 2 if primary_support else 1, "position": [0, bim_y, 0],
                     "sceneWorldMatrix": runner.encode(proxy), "renderWorldMatrix": []}
        element = {"node": 50, "productId": 7, "objects": fragments}
        bodies = ([primary, auxiliary] if both else [primary] if primary_support else [auxiliary]) if active else []
        elements = [element] if active and (both or not primary_support) else []
        physics = {"available": True, "playing": 1 < frame < 181, "error": "", "droppedSeconds": 0,
                   "totalSteps": frame-1 if active else 0, "jointCount": 0, "bodyCount": len(bodies),
                   "bodies": bodies, "auxiliaryElements": elements}
        png = Path(directory) / f"{provider}-{technique}-{frame}.png"
        png.write_bytes(f"lift frame {frame}".encode())
        scene = {"primaryObjectCount": 0 if frame == 190 else 1,
                 "auxiliaryObjectCount": 0 if frame == 190 else auxiliary_count}
        record["samples"].append({"frame": frame, "png": str(png), "telemetry": {"physics": physics, "scene": scene}})
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


class MovingSupportTests(unittest.TestCase):
    def setUp(self):
        if importlib.util.find_spec("numpy") is None:
            self.skipTest("NumPy required for independent geometry checks")
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)

    def state(self, record, frame):
        return next(sample for sample in record["samples"] if sample["frame"] == frame)["telemetry"]["physics"]

    def test_fixture_assigns_kinematic_motion_only_to_support_provider(self):
        for provider in runner.PROVIDERS:
            with self.subTest(provider=provider):
                source, auxiliary, sequence, specification = runner.fixture(provider, Path(self.temporary.name) / provider)
                self.assertTrue(source.is_file() and auxiliary.is_file())
                self.assertEqual(sequence["sampleFrames"], runner.SAMPLE_FRAMES)
                support, rider = sequence["events"][:2]
                self.assertEqual(support["physicsAttach"], "kinematic")
                self.assertEqual(rider["physicsAttach"], "dynamic")
                self.assertIn("physicsNode" if provider == "gltf" else "bimSelectedObject", support)
                self.assertIn("bimSelectedObject" if provider == "gltf" else "physicsNode", rider)
                self.assertEqual("objectTranslation" in sequence, provider == "gltf")
                self.assertEqual("bimTranslation" in sequence, provider == "bim")
                if provider == "gltf":
                    self.assertEqual(sequence["objectNode"], 0)
                    self.assertEqual(sequence["objectName"], json.loads(source.read_text())["nodes"][0]["name"])
                    self.assertEqual(specification["animatedPrimaryNode"], 0)
                    self.assertEqual(support["physicsNode"], 1)
                    self.assertNotEqual(sequence["objectNode"], support["physicsNode"])
                self.assertEqual({part["guid"] for part in specification["bimGeometry"].values()}, {runner.GUID})
                self.assertEqual(sequence["events"][-2]["physicsAction"], "clear")
                self.assertTrue(sequence["events"][-1]["guiUnload"])

    def test_healthy_four_case_matrix_passes(self):
        results = {"expectedCases": [], "cases": []}
        for provider in runner.PROVIDERS:
            for technique in runner.TECHNIQUES:
                record = synthetic(self.temporary.name, provider, technique)
                self.assertTrue(runner.check(record)["passed"])
                results["expectedCases"].append([provider, technique])
                results["cases"].append(record)
        self.assertEqual(len(list(runner.check_matrix(results))), 4)

    def test_overlay_objects_preserve_imported_geometry_count(self):
        record = synthetic(self.temporary.name, "bim", auxiliary_count=5)
        self.assertTrue(runner.check(record)["passed"])
        record["samples"][3]["telemetry"]["scene"]["auxiliaryObjectCount"] = 2
        with self.assertRaisesRegex(AssertionError, "changed imported geometry"):
            runner.check(record)

    def test_bim_lift_must_not_pause_the_primary_dynamic_rider(self):
        record = synthetic(self.temporary.name, "bim")
        self.state(record, 120)["playing"] = False
        with self.assertRaisesRegex(AssertionError, "paused the simulation"):
            runner.check(record)

    def test_dynamic_body_must_be_carried_by_other_provider_contact(self):
        for provider in runner.PROVIDERS:
            with self.subTest(provider=provider):
                record = synthetic(self.temporary.name, provider, carries=False)
                with self.assertRaisesRegex(AssertionError, "did not follow moving support"):
                    runner.check(record)

    def test_kinematic_support_must_follow_authored_height(self):
        record = synthetic(self.temporary.name, "gltf")
        support = self.state(record, 120)["bodies"][0]
        support["position"][1] = 0
        support["sceneWorldMatrix"][13] = support["renderWorldMatrix"][13] = 0
        with self.assertRaisesRegex(AssertionError, "missed authored lift height"):
            runner.check(record)

    def test_rendered_primary_pose_must_match_physics_scene(self):
        record = synthetic(self.temporary.name, "bim")
        self.state(record, 150)["bodies"][0]["renderWorldMatrix"][13] -= 1.5
        with self.assertRaisesRegex(AssertionError, "Primary rendered transform is stale"):
            runner.check(record)

    def test_primary_rider_preserves_authored_mesh_scale(self):
        record = synthetic(self.temporary.name, "bim")
        rider = self.state(record, 150)["bodies"][0]
        rider["sceneWorldMatrix"][0] = rider["renderWorldMatrix"][0] = 2
        with self.assertRaisesRegex(AssertionError, "lost authored mesh scale"):
            runner.check(record)

    def test_bim_bounds_use_original_fragment_geometry(self):
        record = synthetic(self.temporary.name, "bim")
        self.state(record, 150)["auxiliaryElements"][0]["objects"][0]["boundsMax"][1] -= 1.5
        with self.assertRaisesRegex(AssertionError, "bounds disagree with original geometry"):
            runner.check(record)

    def test_bim_normals_and_fragment_membership_stay_current(self):
        record = synthetic(self.temporary.name, "gltf")
        self.state(record, 120)["auxiliaryElements"][0]["objects"][0]["normalMatrix"][0] = 0
        with self.assertRaisesRegex(AssertionError, "normal matrix is stale"):
            runner.check(record)
        record = synthetic(self.temporary.name, "gltf")
        self.state(record, 120)["auxiliaryElements"][0]["objects"].pop()
        with self.assertRaisesRegex(AssertionError, "split into separate bindings"):
            runner.check(record)

    def test_fragment_identity_and_fixed_step_playback_stay_stable(self):
        record = synthetic(self.temporary.name, "bim")
        self.state(record, 150)["auxiliaryElements"][0]["productId"] += 1
        with self.assertRaisesRegex(AssertionError, "product identity"):
            runner.check(record)
        record = synthetic(self.temporary.name, "bim")
        self.state(record, 180)["totalSteps"] = self.state(record, 90)["totalSteps"] + 1
        with self.assertRaisesRegex(AssertionError, "interrupted fixed-step playback"):
            runner.check(record)

    def test_clear_and_unload_retire_bodies_but_only_unload_removes_geometry(self):
        record = synthetic(self.temporary.name, "gltf")
        self.state(record, 184)["bodyCount"] = 1
        with self.assertRaisesRegex(AssertionError, "Clear did not retire"):
            runner.check(record)
        record = synthetic(self.temporary.name, "gltf")
        record["samples"][-1]["telemetry"]["scene"]["primaryObjectCount"] = 1
        with self.assertRaisesRegex(AssertionError, "Unload retained"):
            runner.check(record)

    def test_repeated_png_cannot_claim_visible_motion(self):
        record = synthetic(self.temporary.name, "bim")
        images = {sample["frame"]: sample["png"] for sample in record["samples"]}
        Path(images[150]).write_bytes(Path(images[90]).read_bytes())
        with self.assertRaisesRegex(AssertionError, "did not change captured PNGs"):
            runner.check(record)

    def test_case_matrix_rejects_missing_duplicate_and_unknown_records(self):
        record = synthetic(self.temporary.name, "gltf")
        expected = [["gltf", "deferred-raster"]]
        self.assertEqual(len(list(runner.check_matrix({"expectedCases": expected, "cases": [record]}))), 1)
        for cases in ([], [record, copy.deepcopy(record)]):
            with self.assertRaisesRegex(AssertionError, "Incomplete"):
                runner.check_matrix({"expectedCases": expected, "cases": cases})
        with self.assertRaisesRegex(AssertionError, "Unknown"):
            runner.check_matrix({"expectedCases": [["usd", "deferred-raster"]], "cases": [record]})

    def test_check_existing_revalidates_and_clears_previous_pass(self):
        directory = Path(self.temporary.name)
        record = synthetic(directory, "bim")
        record["checks"] = {"passed": True}
        report = directory / "results.json"
        report.write_text(json.dumps({"passed": True, "expectedCases": [["bim", "deferred-raster"]], "cases": [record]}))
        arguments = [str(Path(runner.__file__)), "--output", str(directory), "--check-existing"]
        with mock.patch.object(sys, "argv", arguments):
            self.assertEqual(runner.main(), 0)
        results = json.loads(report.read_text())
        self.assertTrue(results["passed"])
        self.state(results["cases"][0], 120)["playing"] = False
        report.write_text(json.dumps(results))
        with mock.patch.object(sys, "argv", arguments):
            self.assertEqual(runner.main(), 1)
        results = json.loads(report.read_text())
        self.assertFalse(results["passed"])
        self.assertNotIn("checks", results["cases"][0])


if __name__ == "__main__":
    unittest.main()
