"""CPU checks for auxiliary physics fixtures and captured-data assertions."""
import copy
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

SCRIPT = Path(__file__).with_name("physics_auxiliary_regression.py")
spec = importlib.util.spec_from_file_location("physics_auxiliary_regression", SCRIPT)
runner = importlib.util.module_from_spec(spec)
spec.loader.exec_module(runner)


def transform(y=0):
    return [1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, y, 0, 1]


def synthetic(directory, *, source_id_collision=False):
    normals = [1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0]

    def group(guid, node, product, height):
        indices = (0, 1) if guid == runner.PRODUCT_GUID else (2,)
        shift = height - 3 if guid == runner.PRODUCT_GUID else 0
        objects = [{"objectIndex": index, "guid": guid, "sourceId": f"/fragment{index}",
                    "model": transform(shift), "normalMatrix": normals.copy(),
                    "boundsMin": [(-1, 1, 4)[index] - .5, 2.5 + shift, -.5],
                    "boundsMax": [(-1, 1, 4)[index] + .5, 3.5 + shift, .5]} for index in indices]
        return {"node": node, "productId": product, "objects": objects}, {
            "node": node, "motion": 2 if guid == runner.PRODUCT_GUID else 0,
            "position": [0, height, 0], "sceneWorldMatrix": transform(height)}

    record = {"format": "usd", "technique": "deferred-raster", "mode": "rays-taa",
              "validationErrors": [], "samples": []}
    for frame in runner.SAMPLE_FRAMES:
        bodies, elements = [], []
        if frame in (1, 3, 30, 180, 182, 184):
            bodies.append({"node": 1, "motion": 0, "position": [0, -.25, 0]})
        if frame in (3, 30, 180, 182, 184):
            other, body = group(runner.OTHER_GUID, 10, 2, 3)
            elements.append(other)
            bodies.append(body)
        if frame in (3, 30, 180, 182, 190):
            height = {3: 3, 30: 2, 180: .5, 182: 3, 190: 2.99}[frame]
            product, body = group(runner.PRODUCT_GUID, 11, 1, height)
            elements.append(product)
            bodies.append(body)
        state = {"available": True, "playing": frame in (3, 30, 180, 190),
                 "bodyCount": len(bodies), "bodies": bodies, "auxiliaryElements": elements,
                 "droppedSeconds": 0, "totalSteps": max(0, frame - 2)}
        png = directory / f"synthetic-{frame}.png"
        # The native runner independently decodes PNGs. This CPU assertion test
        # supplies distinct bytes solely for its changing-image hash guard.
        png.write_bytes(f"frame {frame}".encode())
        record["samples"].append({"frame": frame, "png": str(png), "telemetry": {
            "physics": state, "scene": {"auxiliaryObjectCount": 0 if frame == 200 else 3},
            "rayShadows": {"active": True}, "taa": {"enabled": True}}})
    if source_id_collision:
        record["format"] = "bim"
        record["sourceIdCollision"] = True
        for sample in record["samples"]:
            for element in sample["telemetry"]["physics"]["auxiliaryElements"]:
                for obj in element["objects"]:
                    obj["sourceId"] = runner.COLLIDING_SOURCE_ID
                    if element["productId"] == 1:
                        obj["guid"] = ""
    return record


class FixtureTests(unittest.TestCase):
    def test_generated_product_has_two_fragments_and_unrelated_identity(self):
        with tempfile.TemporaryDirectory() as temporary:
            floor, sources = runner.write_fixtures(temporary)
            gltf = json.loads(floor.read_text())
            self.assertEqual(gltf["nodes"][0]["mesh"], 0)
            self.assertEqual(gltf["nodes"][0]["scale"], [12, .5, 8])
            bim = json.loads(sources["bim"].read_text())
            self.assertEqual([element["guid"] for element in bim["elements"]],
                             [runner.PRODUCT_GUID, runner.PRODUCT_GUID, runner.OTHER_GUID])
            self.assertEqual(len(bim["meshes"]), 3)
            self.assertEqual(sources["usd"].read_text().count(f'string guid = "{runner.PRODUCT_GUID}"'), 2)

    def test_source_id_collision_fixture_keeps_guid_precedence(self):
        with tempfile.TemporaryDirectory() as temporary:
            _, sources = runner.write_fixtures(temporary, source_id_collision=True)
            elements = json.loads(sources["bim"].read_text())["elements"]
            self.assertEqual([element["guid"] for element in elements], ["", "", runner.OTHER_GUID])
            self.assertEqual([element["sourceId"] for element in elements], [runner.COLLIDING_SOURCE_ID] * 3)

    def test_source_id_collision_option_requires_explicit_bim_format(self):
        with tempfile.TemporaryDirectory() as temporary:
            output = Path(temporary) / "not-created"
            for arguments in ([], ["--format", "usd"]):
                result = subprocess.run([sys.executable, "-S", "-B", str(SCRIPT), "--output", str(output),
                                         "--source-id-collision", *arguments], capture_output=True, text=True)
                self.assertEqual(result.returncode, 2)
                self.assertIn("requires --format bim", result.stderr)
                self.assertFalse(output.exists())

    def test_cube_faces_are_outward_and_non_degenerate(self):
        points, normals, indices = runner.cube()
        self.assertEqual((len(points), len(normals), len(indices)), (24, 24, 36))
        for first in range(0, len(indices), 3):
            a, b, c = [points[index] for index in indices[first:first + 3]]
            ab, ac = [b[i] - a[i] for i in range(3)], [c[i] - a[i] for i in range(3)]
            cross = [ab[1] * ac[2] - ab[2] * ac[1], ab[2] * ac[0] - ab[0] * ac[2],
                     ab[0] * ac[1] - ab[1] * ac[0]]
            self.assertGreater(sum(cross[i] * normals[indices[first]][i] for i in range(3)), 0)

    def test_sequence_targets_primary_floor_and_auxiliary_product(self):
        sequence = runner.sequence(Path("fixture.bim"))
        self.assertEqual(sequence["sampleFrames"], runner.SAMPLE_FRAMES)
        self.assertEqual(sequence["events"][0]["physicsNode"], 1)
        self.assertEqual(sequence["events"][2]["physicsAttach"], "dynamic")
        self.assertEqual(sequence["events"][2]["physicsAction"], "play")

    def test_opt_in_skip_and_help_need_only_standard_library(self):
        with tempfile.TemporaryDirectory() as temporary:
            output = Path(temporary) / "not-created-on-skip"
            environment = dict(os.environ)
            environment.pop("CONTAINER_RUN_GPU_PHYSICS", None)
            skipped = subprocess.run([sys.executable, "-S", "-B", str(SCRIPT), "--output", str(output),
                                      "--require-opt-in"], capture_output=True, text=True, env=environment)
            self.assertEqual(skipped.returncode, 77, skipped.stderr)
            self.assertFalse(output.exists())
            help_result = subprocess.run([sys.executable, "-S", "-B", str(SCRIPT), "--help"],
                                         capture_output=True, text=True, env=environment)
            self.assertEqual(help_result.returncode, 0, help_result.stderr)

    def test_generated_sequence_matches_capture_schema(self):
        if importlib.util.find_spec("jsonschema") is None:
            self.skipTest("jsonschema not installed")
        import jsonschema
        schema = json.loads((SCRIPT.parents[1] / "fixtures/rendering/temporal_capture.schema.json").read_text())
        jsonschema.validate(runner.sequence(Path("fixture.bim")), schema)
        invalid = runner.sequence(Path("fixture.bim"))
        invalid["events"][2]["physicsAction"] = "detach"
        with self.assertRaises(jsonschema.ValidationError):
            jsonschema.validate(invalid, schema)


class CapturedStateTests(unittest.TestCase):
    def setUp(self):
        if importlib.util.find_spec("numpy") is None:
            self.skipTest("NumPy required for captured matrix assertions")
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.record = synthetic(Path(self.temporary.name))

    def sample(self, frame):
        return next(sample for sample in self.record["samples"] if sample["frame"] == frame)

    def product(self, frame):
        return next(group for group in self.sample(frame)["telemetry"]["physics"]["auxiliaryElements"]
                    if group["productId"] == 1)

    def test_healthy_rigid_product_lifecycle_passes(self):
        self.assertTrue(runner.check(self.record)["passed"])

    def test_source_id_collision_retains_distinct_guid_product(self):
        self.record = synthetic(Path(self.temporary.name), source_id_collision=True)
        self.assertTrue(runner.check(self.record)["sourceIdCollisionGuard"])

    def test_source_id_collision_must_not_merge_guid_bearing_fragment(self):
        self.record = synthetic(Path(self.temporary.name), source_id_collision=True)
        other = self.sample(30)["telemetry"]["physics"]["auxiliaryElements"][0]["objects"][0]
        self.product(30)["objects"].append(copy.deepcopy(other))
        with self.assertRaisesRegex(AssertionError, "merged unrelated products"):
            runner.check(self.record)

    def test_source_id_collision_flag_requires_matching_capture_identity(self):
        self.record["format"] = "bim"
        self.record["sourceIdCollision"] = True
        with self.assertRaisesRegex(AssertionError, "Missing/duplicate attached product"):
            runner.check(self.record)

    def test_importer_floorplan_overlay_objects_preserve_mesh_checks(self):
        for sample in self.record["samples"]:
            if sample["frame"] != 200:
                sample["telemetry"]["scene"]["auxiliaryObjectCount"] = 5
        self.assertTrue(runner.check(self.record)["passed"])

    def test_changed_geometry_count_after_detach_fails(self):
        self.sample(184)["telemetry"]["scene"]["auxiliaryObjectCount"] = 2
        with self.assertRaisesRegex(AssertionError, "changed imported geometry count"):
            runner.check(self.record)

    def test_one_stale_fragment_model_fails(self):
        self.product(30)["objects"][1]["model"] = transform()
        with self.assertRaisesRegex(AssertionError, "Rendered fragment"):
            runner.check(self.record)

    def test_stale_bounds_fail(self):
        self.product(30)["objects"][0]["boundsMin"][1] = 2.5
        with self.assertRaisesRegex(AssertionError, "minimum bound"):
            runner.check(self.record)

    def test_stale_normal_transform_fails(self):
        self.product(30)["objects"][0]["normalMatrix"][0] = 2
        with self.assertRaisesRegex(AssertionError, "normal transform"):
            runner.check(self.record)

    def test_changed_source_identity_fails(self):
        self.product(30)["objects"][0]["sourceId"] = "/different-product"
        with self.assertRaisesRegex(AssertionError, "source identity"):
            runner.check(self.record)

    def test_unrelated_product_movement_fails(self):
        other = self.sample(30)["telemetry"]["physics"]["auxiliaryElements"][0]
        other["objects"][0]["model"] = transform(1)
        with self.assertRaisesRegex(AssertionError, "Unrelated product"):
            runner.check(self.record)

    def test_reload_retained_body_fails(self):
        self.sample(188)["telemetry"]["physics"]["bodyCount"] = 1
        with self.assertRaisesRegex(AssertionError, "Reload retained"):
            runner.check(self.record)

    def test_disabled_ray_route_and_validation_errors_fail(self):
        original = copy.deepcopy(self.record)
        self.sample(180)["telemetry"]["rayShadows"]["active"] = False
        with self.assertRaisesRegex(AssertionError, "Ray-query"):
            runner.check(self.record)
        original["validationErrors"] = ["SYNC-HAZARD"]
        with self.assertRaisesRegex(AssertionError, "validation errors"):
            runner.check(original)


if __name__ == "__main__":
    unittest.main()
