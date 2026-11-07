"""Real-tool generic native RK advection with exact ID-keyed payload checks."""
import argparse
from array import array
import json
import math
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest


sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "python"))
from container_openvdb.dense import DenseType, DenseVolume, GridClass, VectorType, write_dense_array


class NativePointAdvectionCli(unittest.TestCase):
    identity = (1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1)
    columns = ("id", "flag", "counter", "weight", "energy", "axis", "target", "label", "v", "pscale")
    groups = ("A", "B", "C", "empty")

    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="container-native-point-advection-cli-")
        self.directory = Path(self.temporary.name)
        self.originals = {}
        self.serial = 0

    def tearDown(self):
        try:
            for source, data in self.originals.items():
                self.assertEqual(source.read_bytes(), data, source)
            self.assertFalse(list(self.directory.glob(".container-*")))
        finally:
            self.temporary.cleanup()

    def enabled_only(self):
        if not self.enabled:
            self.skipTest("OpenVDB is disabled")

    def call(self, *args, succeeds=True):
        result = subprocess.run([str(self.tool), *map(str, args)], cwd=self.directory,
                                capture_output=True, text=True, encoding="utf-8", timeout=60)
        self.assertEqual(result.returncode == 0, succeeds,
                         f"{args!r}\n{result.stdout}\n{result.stderr}")
        return result

    def report(self, *args):
        return json.loads(self.call(*args).stdout)

    def protect(self, source):
        source = self.directory / source
        self.originals[source] = source.read_bytes()

    def write_json(self, name, document):
        (self.directory / name).write_text(json.dumps(document, ensure_ascii=False), encoding="utf-8")

    def export(self, command, source, *args):
        self.serial += 1
        output = self.directory / f"column-{self.serial}.json"
        self.call(command, source, *args, output)
        result = json.loads(output.read_text(encoding="utf-8"))
        self.assertEqual(result["traversal"], "leaf_origin_xyz_then_local_index")
        return result

    def fixture(self, name="points.vdb", grid="points"):
        positions = ((-16.25, 0, 0), (-1.125, .25, -.25), (0, 0, 0),
                     (.125, .25, 0), (9.25, 0, .5), (16.125, -.25, 0))
        self.write_json("points.json", [{"position": p, "velocity": [3+i, -4, 5], "radius": .25+.125*i}
                                        for i, p in enumerate(positions)])
        self.call("--points", "points.json", "raw.vdb", "--voxel-size", 1)
        self.write_json("edits.json", {"version": 1, "attributes": [
            {"op": "add", "name": "id", "type": "int64", "values": [2**63-1-i for i in range(6)]},
            {"op": "add", "name": "flag", "type": "bool", "values": [i % 2 == 0 for i in range(6)]},
            {"op": "add", "name": "counter", "type": "int32", "values": [-2**31+i for i in range(6)]},
            {"op": "add", "name": "weight", "type": "float", "values": [-0., .125, -.5, 2., 4., 8.]},
            {"op": "add", "name": "energy", "type": "double", "values": [-0., 1e-200, -1e200, 2., 4., 8.]},
            {"op": "add", "name": "axis", "type": "vec3f", "values": [[-0., i, -i] for i in range(6)]},
            {"op": "add", "name": "target", "type": "vec3d", "values": [[1e100, i+.125, -0.] for i in range(6)]},
            {"op": "add", "name": "label", "type": "string", "values": [f"record-{i}-é-\u0000" for i in range(6)]}],
            "groups": [{"op": "add", "name": "A", "values": [i % 2 == 0 for i in range(6)]},
                       {"op": "add", "name": "B", "values": [i < 5 for i in range(6)]},
                       {"op": "add", "name": "C", "values": [i == 2 for i in range(6)]},
                       {"op": "add", "name": "empty", "values": [False]}]})
        self.call("--edit-point-attributes", "raw.vdb", "edits.json", "edited.vdb", "--grid-name", grid)
        self.write_json("metadata.json", {"version": 1, "inputs": [{"path": "edited.vdb", "edits": [
            {"selector": grid, "metadata": [{"name": "artist", "operation": "set", "type": "string", "value": "raw-\u0000-世界"}]}]}],
            "metadata": [{"name": "scene", "operation": "set", "type": "int64", "value": 9007199254740993}]})
        self.call("--author-file", "metadata.json", name)
        for source in ("points.json", "raw.vdb", "edits.json", "edited.vdb", name):
            self.protect(source)
        return self.snapshot(name)

    @staticmethod
    def encoded(value, kind):
        if kind in ("float", "double"):
            return struct.pack("<f" if kind == "float" else "<d", value)
        if kind in ("vec3f", "vec3d"):
            return struct.pack("<3f" if kind == "vec3f" else "<3d", *value)
        return value

    def neighbors(self, source):
        return self.report("--point-neighbors", source, "--neighbor-query", 0, 0, 0, 128,
                           "--neighbor-index-voxel-size", 8)

    def snapshot(self, source):
        descriptions = self.export("--point-schema", source)
        columns = {name: self.export("--point-attribute", source, name) for name in self.columns}
        memberships = {name: self.export("--point-group", source, name)["values"] for name in self.groups}
        ids = columns["id"]["values"]
        self.assertTrue(all(type(value) is int and value > 2**53 for value in ids))
        self.assertEqual(len(set(ids)), len(ids))
        report = self.neighbors(source)
        hits = report["queries"][0]["hits"]
        self.assertEqual(sorted(hit["reference"]["ordinal"] for hit in hits), list(range(len(ids))))
        points = {}
        for hit in hits:
            index = hit["reference"]["ordinal"]
            points[ids[index]] = {"world": hit["positionWorld"],
                "payload": {name: self.encoded(column["values"][index], column["type"])
                            for name, column in columns.items()},
                "groups": {name: membership[index] for name, membership in memberships.items()}}
        return {"points": points, "ids": ids, "schema": descriptions, "neighbors": report}

    def compare(self, before, after, matrix=identity, predicate=lambda point: True):
        self.assertEqual(set(before["points"]), set(after["points"]))
        self.assertEqual(before["schema"]["attributes"], after["schema"]["attributes"])
        self.assertEqual(before["schema"]["groups"], after["schema"]["groups"])
        for key, old in before["points"].items():
            new = after["points"][key]
            self.assertEqual(old["payload"], new["payload"], key)
            self.assertEqual(old["groups"], new["groups"], key)
            expected = [math.fsum([matrix[4*r+3], *(matrix[4*r+c]*old["world"][c] for c in range(3))])
                        for r in range(3)] if predicate(old) else old["world"]
            for actual, target in zip(new["world"], expected):
                self.assertAlmostEqual(actual, target, delta=2e-5)
        for field in ("gridMetadata", "fileMetadata", "descriptorMetadata"):
            # Native writer statistics/name may change. Custom metadata stays.
            for name in ("artist", "scene"):
                old = [m for m in before["neighbors"][field] if m["name"] == name]
                new = [m for m in after["neighbors"][field] if m["name"] == name]
                self.assertEqual(old, new)

    def velocity(self, vector=(.5, -.25, .125), name="velocity.vdb", grid="velocity"):
        self.call("--velocity", name, "--vector", *vector, "--grid-name", grid)
        self.protect(name)

    def dense_velocity(self, name, function, *, dtype=DenseType.VEC3_FLOAT64, half=3,
                       background=(0, 0, 0), tag=VectorType.CONTRAVARIANT_RELATIVE,
                       world=True, grid="velocity", linear=(1, 0, 0, 0, 1, 0, 0, 0, 1),
                       translation=(0, 0, 0), grid_class=GridClass.UNKNOWN):
        values = array(dtype.typecode)
        for z in range(-half, half + 1):
            for y in range(-half, half + 1):
                for x in range(-half, half + 1):
                    values.extend(function(x, y, z))
        volume = DenseVolume(dtype, (2 * half + 1,) * 3, values,
            bytearray([1]) * ((2 * half + 1) ** 3), origin=(-half,) * 3,
            background=array(dtype.typecode, background), grid_name=grid,
            vector_type=tag, world_space=world, grid_class=grid_class,
            linear=linear, translation=translation)
        encoded = self.directory / (name + ".cvarr")
        write_dense_array(encoded, volume)
        self.call("--dense-import", encoded, name)
        self.protect(name)

    def single(self, position=(.25, 0, 0), name="single.vdb"):
        self.write_json("single.json", [{"position": position, "velocity": [4, 5, 6], "radius": .5}])
        self.call("--points", "single.json", name, "--voxel-size", 1)
        self.protect(name)

    def single_world(self, source):
        hits = self.neighbors(source)["queries"][0]["hits"]
        self.assertEqual(len(hits), 1)
        return hits[0]["positionWorld"]

    def assert_report(self, report, count=6):
        self.assertEqual(report["schemaVersion"], 1)
        self.assertEqual(report["operation"], "advect-native-points")
        for field in ("inputCount", "selectedCount", "outputCount", "inputActiveCount", "outputActiveCount",
                      "inactiveRecordsActivated", "outputVoxelCount", "workUpperBound", "nativeWorkUpperBound",
                      "formattingWorkUpperBound", "executedSteps", "velocitySampleCount"):
            self.assertIs(type(report[field]), int, field)
            self.assertGreaterEqual(report[field], 0)
        self.assertEqual(report["inputCount"], count)
        self.assertEqual(report["outputCount"], count)
        self.assertEqual(report["schema"]["pointCount"], count)
        self.assertEqual(report["workUpperBound"], report["nativeWorkUpperBound"] + report["formattingWorkUpperBound"])
        for field in ("noOp", "archivedSchema", "integrationSkipped", "velocityGridNameValidUtf8", "velocityGridNameTruncated"):
            self.assertIs(type(report[field]), bool)
        for field in ("maxPositionErrorVoxels", "maxPositionErrorWorld", "maxUnselectedPositionErrorVoxels",
                      "maximumWorldSpeedBound", "maximumStepTravelVelocityVoxels", "maximumTravelVelocityVoxels",
                      "maximumWorldTravel", "maximumEndpointArithmeticEnvelopeWorld"):
            self.assertTrue(math.isfinite(report[field]) and report[field] >= 0, field)
        self.assertLessEqual(report["maxPositionErrorVoxels"], 1e-4)
        self.assertIn("Only P", report["payloadSemantics"])
        self.assertIn("RK endpoints", report["errorSemantics"])
        self.assertTrue(report["diagnostics"])
        self.assertTrue(report["motionDiagnostics"])
        self.assertNotIn("references", report)

    def test_help_strict_arity_duplicates_ranges_and_irrelevant_options(self):
        help_text = self.call("--help").stdout
        for flag in ("--advect-native-points", "--point-integration", "--velocity-layout",
                     "--max-point-step-travel", "--max-point-travel", "--max-point-advection-work",
                     "--max-point-advection-grids", "--max-point-advection-metadata"):
            self.assertIn(flag, help_text)
        base = ("--advect-native-points", "missing", "missing-velocity", "bad.vdb")
        cases = [("--advect-native-points",), ("--advect-native-points", "a", "b"),
                 ("--advect-native-points", "", "b", "bad.vdb"),
                 ("--advect-native-points", "a", "-h", "bad.vdb"),
                 ("--advect-native-points", "a", "b", "--help")]
        for flags in (("--time-step", "nan"), ("--time-step", 3601), ("--steps", 0), ("--steps", 65),
                      ("--steps", " -1"), ("--point-integration", "rk3"), ("--velocity-layout", "local"),
                      ("--point-position-error", 0), ("--point-position-error", .251),
                      ("--max-point-step-travel", 16.1), ("--max-point-travel", 257),
                      ("--max-point-advection-work", 0), ("--max-point-advection-work", 10000000001),
                      ("--max-point-advection-grids", 4097), ("--max-point-advection-metadata", 0),
                      ("--max-points", 2**63), ("--max-attributes", 1025), ("--max-groups", 4097),
                      ("--point-target-transform", *self.identity[:15]),
                      ("--point-target-transform", *self.identity[:15], 0),
                      ("--point-target-transform", *self.identity, 2),
                      ("--point-target-transform", "inf", *self.identity[1:]),
                      ("--grid", "one", "--input-grid", "two"), ("--point-grid", "one", "--grid", "two"),
                      ("--velocity-grid", "a", "--velocity-grid", "b"),
                      ("--time-step", 1, "--time-step", 2),
                      ("--any-group", "A", "--any-group", "A"), ("--all-group", ""),
                      ("--allow-point-activity-merge", "--allow-point-activity-merge"),
                      ("--point-world-transform", *self.identity), ("--voxel-size", .5),
                      ("--half-width", 3), ("--max-triangles", 1), ("--max-edits", 1),
                      ("--update-velocity", "true"), ("--grid", "-h")):
            cases.append((*base, *flags))
        for args in cases:
            with self.subTest(args=args):
                self.call(*args, succeeds=False)
                self.assertFalse((self.directory / "bad.vdb").exists())
        self.assertEqual(list(self.directory.iterdir()), [])

    def test_disabled_reports_availability_before_paths_options_and_report(self):
        if self.enabled:
            self.skipTest("OpenVDB is enabled")
        for flags in ((), ("--max-input-bytes", 1), ("--steps", 0), ("--any-group", "missing")):
            result = self.call("--advect-native-points", "不存在", "不存在流", "bad.vdb", *flags, succeeds=False)
            self.assertIn("ENABLE_OPENVDB=ON", result.stderr)
        self.assertEqual(list(self.directory.iterdir()), [])

    def test_selected_signed_constant_flow_preserves_exact_columns_groups_and_metadata(self):
        self.enabled_only()
        before = self.fixture()
        self.velocity()
        report = self.report("--advect-native-points", "points.vdb", "velocity.vdb", "moved.vdb",
            "--time-step", -2, "--steps", 4, "--point-integration", "rk4", "--velocity-layout", "world",
            "--any-group", "A", "--all-group", "B", "--exclude-group", "C",
            "--point-grid", "descriptor:points", "--velocity-grid", "velocity")
        self.assert_report(report)
        self.assertEqual(report["selectedCount"], 2)
        self.assertEqual(report["velocitySampleCount"], 32)
        self.assertFalse(report["integrationSkipped"])
        self.assertFalse(report["noOp"])
        matrix = (1, 0, 0, -1, 0, 1, 0, .5, 0, 0, 1, -.25, 0, 0, 0, 1)
        self.compare(before, self.snapshot("moved.vdb"), matrix,
                     lambda p: p["groups"]["A"] and p["groups"]["B"] and not p["groups"]["C"])

    def test_independent_affine_linear_field_rk_polynomials(self):
        self.enabled_only()
        self.single()
        self.dense_velocity("linear.vdb", lambda x, y, z: (.25 * (.5*x - 1), 0, 0), half=5,
                            linear=(.5, 0, 0, 0, .5, 0, 0, 0, .5), translation=(-1, .5, -.25))
        for mode in ("rk2", "rk4"):
            output = mode + ".vdb"
            report = self.report("--advect-native-points", "single.vdb", "linear.vdb", output,
                                 "--time-step", 1, "--steps", 4, "--point-integration", mode)
            self.assert_report(report, 1)
            a = .25 / 4
            polynomial = 1 + a + a*a/2 + (a**3/6 + a**4/24 if mode == "rk4" else 0)
            self.assertAlmostEqual(self.single_world(output)[0], .25 * polynomial**4, delta=3e-8)

    def test_all_five_world_tags_and_native_types_are_raw_physical_xyz(self):
        self.enabled_only()
        self.single((0, 0, 0))
        for dtype in (DenseType.VEC3_FLOAT32, DenseType.VEC3_FLOAT64):
            for tag in VectorType:
                name = f"tag-{int(dtype)}-{int(tag)}.vdb"
                self.dense_velocity(name, lambda x, y, z: (.25, .5, -.125), half=0,
                    background=(.25, .5, -.125), dtype=dtype, tag=tag,
                    linear=(-2, 0, 0, 0, 2, 0, 0, 0, 2), translation=(10, 2, -3))
                output = "out-" + name
                report = self.report("--advect-native-points", "single.vdb", name, output, "--time-step", 2)
                self.assert_report(report, 1)
                self.assertEqual(self.single_world(output), [.5, 1, -.25])
                self.assertIn("WORLD XYZ", report["velocitySemantics"])

    def test_native_mac_linear_half_face_oracle_in_rotated_reflected_frame(self):
        self.enabled_only()
        self.single((1, -2, 3))
        self.dense_velocity("mac-raw.vdb", lambda x, y, z: (.25*(x-.5)+.5, .5*(y-.5)-.25, .125*(z-.5)+.25), half=4,
            world=False, tag=VectorType.CONTRAVARIANT_RELATIVE, grid_class=GridClass.STAGGERED,
            linear=(0, -2, 0, 2, 0, 0, 0, 0, -2), translation=(1, -2, 3))
        self.write_json("mac-meta.json", {"version": 1, "inputs": [{"path": "mac-raw.vdb", "edits": [
            {"selector": "velocity", "metadata": [
                {"name": "container:mac_layout", "operation": "set", "type": "string", "value": "negative_faces"},
                {"name": "container:velocity_units", "operation": "set", "type": "string", "value": "voxels_per_second"}]}]}]})
        self.call("--author-file", "mac-meta.json", "mac.vdb")
        self.protect("mac.vdb")
        report = self.report("--advect-native-points", "single.vdb", "mac.vdb", "out.vdb",
                             "--time-step", .5, "--velocity-layout", "native-mac")
        self.assert_report(report, 1)
        q = (.265625, -.140625, .12890625)
        expected = (1 - 2*q[1], -2 + 2*q[0], 3 - 2*q[2])
        for actual, target in zip(self.single_world("out.vdb"), expected):
            self.assertAlmostEqual(actual, target, delta=2e-7)
        self.assertIn("negative-face", report["velocitySemantics"])

    def test_skipped_integration_is_distinct_from_target_repartition_and_archives(self):
        self.enabled_only()
        before = self.fixture()
        self.velocity()
        target = (0, -.5, 0, 10, .5, 0, 0, -2, 0, 0, -.5, 3, 0, 0, 0, 1)
        report = self.report("--advect-native-points", "points.vdb", "velocity.vdb", "mapped.vdb",
                             "--time-step", 0, "--point-target-transform", *target)
        self.assert_report(report)
        self.assertTrue(report["integrationSkipped"])
        self.assertFalse(report["noOp"])
        self.assertEqual(report["velocitySampleCount"], 0)
        self.compare(before, self.snapshot("mapped.vdb"))
        self.assertEqual(self.report("--inspect", "mapped.vdb")["grids"][0]["indexToWorld"], list(target))
        empty_selection = self.report("--advect-native-points", "points.vdb", "velocity.vdb", "none.vdb", "--any-group", "empty")
        self.assertTrue(empty_selection["integrationSkipped"])
        self.assertTrue(empty_selection["noOp"])
        self.call("--select-points", "points.vdb", "archive.vdb", "--any-group", "empty")
        original = self.neighbors("archive.vdb")
        archived = self.report("--advect-native-points", "archive.vdb", "velocity.vdb", "archive-copy.vdb")
        self.assert_report(archived, 0)
        self.assertTrue(archived["integrationSkipped"] and archived["noOp"] and archived["archivedSchema"])
        copied = self.neighbors("archive-copy.vdb")
        persistent = lambda rows: [m for m in rows if not m["name"].startswith("file_")]
        self.assertEqual(persistent(original["gridMetadata"]), persistent(copied["gridMetadata"]))
        self.assertEqual(original["schema"], copied["schema"])

    def test_float_subnormal_flow_and_invalid_native_name_report_remain_loss_aware(self):
        self.enabled_only()
        self.single((0, 0, 0))
        speed = struct.unpack("<f", struct.pack("<I", 1))[0]
        self.dense_velocity("tiny.vdb", lambda x, y, z: (speed, 0, 0), half=0,
                            background=(speed, 0, 0), dtype=DenseType.VEC3_FLOAT32)
        target = (1e-6, 0, 0, 0, 0, 1e-6, 0, 0, 0, 0, 1e-6, 0, 0, 0, 0, 1)
        report = self.report("--advect-native-points", "single.vdb", "tiny.vdb", "tiny-out.vdb",
                             "--time-step", 1, "--point-target-transform", *target)
        self.assert_report(report, 1)
        self.assertEqual(self.report("--inspect", "tiny-out.vdb")["grids"][0]["indexToWorld"], list(target))
        self.protect("tiny-out.vdb")
        # Read the original tiny native map directly through both repaired
        # routes. No re-encoding can hide a lost subnormal endpoint.
        native = self.report("--inspect", "tiny-out.vdb")["grids"][0]
        self.assertEqual(native["indexToWorld"], list(target))
        self.assertEqual(native["recorded"]["indexBounds"], {"minimum": [0, 0, 0], "maximum": [0, 0, 0]})
        offsets = self.export("--point-attribute", "tiny-out.vdb", "P")["values"]
        self.assertEqual(len(offsets), 1)
        actual = 1e-6 * offsets[0][0]  # Exact tiny map and verified voxel zero.
        self.assertGreater(actual, 0)
        self.assertAlmostEqual(actual, speed, delta=speed*1e-5)
        self.assertEqual(offsets[0][1:], [0, 0])
        world = self.single_world("tiny-out.vdb")
        self.assertGreater(world[0], 0)
        self.assertAlmostEqual(world[0], speed, delta=speed*1e-5)
        self.assertEqual(world[1:], [0, 0])
        self.dense_velocity("binary.vdb", lambda x, y, z: (.5, 0, 0), half=0,
                            background=(.5, 0, 0), grid="\udcff")
        report = self.report("--advect-native-points", "single.vdb", "binary.vdb", "binary-out.vdb", "--time-step", 1)
        self.assertIsNone(report["velocityGridName"])
        self.assertFalse(report["velocityGridNameValidUtf8"])
        self.assertEqual(report["velocityGridNameBytesHex"], "ff")
        self.assertEqual(report["velocityGridNameByteCount"], 1)
        self.assertEqual(self.single_world("binary-out.vdb"), [.5, 0, 0])

    def test_unicode_literal_paths_exclusive_outputs_limits_and_incompatible_sources(self):
        self.enabled_only()
        self.fixture("入力-é.vdb", "粒子")
        self.velocity(name="速度.vdb", grid="流れ")
        parent = self.directory / "移流-é"
        parent.mkdir()
        output = parent / "出力.vdb"
        result = self.report("--advect-native-points", "入力-é.vdb", "速度.vdb", output,
            "--time-step", 0, "--input-grid", "粒子", "--velocity-grid", "流れ", "--grid-name", "移流")
        self.assert_report(result)
        self.assertEqual(result["velocityGridName"], "流れ")
        completed = output.read_bytes()
        self.call("--advect-native-points", "入力-é.vdb", "速度.vdb", output, succeeds=False)
        self.assertEqual(output.read_bytes(), completed)
        self.call("--advect-native-points", "入力-é.vdb", "速度.vdb", "入力-é.vdb", succeeds=False)
        (self.directory / "-h").write_bytes((self.directory / "入力-é.vdb").read_bytes())
        literal = self.report("--advect-native-points", "./-h", "速度.vdb", "literal.vdb", "--time-step", 0)
        self.assertTrue(literal["integrationSkipped"] and literal["noOp"])
        for flags in (("--max-input-bytes", 1024), ("--max-voxels", 1), ("--max-points", 1),
                      ("--max-attributes", 1), ("--max-groups", 1), ("--max-string-bytes", 1),
                      ("--max-point-advection-work", 1), ("--max-point-advection-grids", 1),
                      ("--max-point-advection-metadata", 1), ("--max-point-step-travel", .001),
                      ("--max-point-travel", .001), ("--point-grid", "missing"),
                      ("--velocity-grid", "missing"), ("--any-group", "missing"),
                      ("--velocity-layout", "native-mac"),
                      ("--point-target-transform", 1, 0, 0, 0, 0, 2, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1)):
            with self.subTest(flags=flags):
                self.call("--advect-native-points", "入力-é.vdb", "速度.vdb", "bad.vdb", *flags, succeeds=False)
                self.assertFalse((self.directory / "bad.vdb").exists())
        self.call("--sphere", "scalar.vdb", "--voxel-size", .25)
        self.call("--advect-native-points", "入力-é.vdb", "scalar.vdb", "bad.vdb", succeeds=False)
        self.assertFalse((self.directory / "bad.vdb").exists())


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--tool", required=True, type=Path)
    parser.add_argument("--enabled", required=True, choices=("0", "1"))
    args, remaining = parser.parse_known_args()
    NativePointAdvectionCli.tool = args.tool.resolve(strict=True)
    NativePointAdvectionCli.enabled = args.enabled == "1"
    unittest.main(argv=[__file__, *remaining])

