"""Real-tool native P deformation/repartition with exact ID-keyed payload checks."""
import argparse
import json
import math
from pathlib import Path
import struct
import subprocess
import tempfile
import unittest


class PointMotionCli(unittest.TestCase):
    identity = (1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1)
    columns = ("id", "flag", "counter", "weight", "energy", "axis", "target", "label", "v", "pscale")
    groups = ("A", "B", "C", "empty")

    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="container-point-motion-cli-")
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

    def assert_report(self, report, count=6):
        self.assertEqual(report["schemaVersion"], 1)
        self.assertEqual(report["operation"], "move-native-points")
        for field in ("inputCount", "selectedCount", "outputCount", "inputActiveCount", "outputActiveCount",
                      "inactiveRecordsActivated", "outputVoxelCount", "workUpperBound"):
            self.assertIs(type(report[field]), int, field)
        self.assertEqual(report["inputCount"], count)
        self.assertEqual(report["outputCount"], count)
        self.assertEqual(report["inputActiveCount"], count)
        self.assertEqual(report["outputActiveCount"], count)
        self.assertEqual(report["inactiveRecordsActivated"], 0)
        self.assertEqual(report["schema"]["pointCount"], count)
        self.assertTrue(all(math.isfinite(report[key]) and report[key] >= 0 for key in
                            ("maxPositionErrorVoxels", "maxPositionErrorWorld", "maxUnselectedPositionErrorVoxels")))
        self.assertLessEqual(report["maxPositionErrorVoxels"], 1e-4)
        self.assertIs(type(report["noOp"]), bool)
        self.assertIs(type(report["archivedSchema"]), bool)
        self.assertIn("Only P", report["payloadSemantics"])
        self.assertNotIn("references", report)
        self.assertTrue(report["diagnostics"])

    def test_help_strict_arity_empty_duplicate_and_irrelevant_options(self):
        help_text = self.call("--help").stdout
        for flag in ("--move-native-points", "--point-world-transform", "--point-target-transform",
                     "--point-position-error", "--allow-point-activity-merge", "--max-point-motion-work",
                     "--max-point-motion-grids", "--max-point-motion-metadata"):
            self.assertIn(flag, help_text)
        base = ("--move-native-points", "missing", "bad.vdb")
        cases = [("--move-native-points",), ("--move-native-points", "missing"),
                 ("--move-native-points", "", "bad.vdb"), ("--move-native-points", "missing", ""),
                 ("--move-native-points", "-h", "bad.vdb"), ("--move-native-points", "missing", "-h"),
                 ("--move-native-points", "--help", "bad.vdb")]
        for flags in (("--point-world-transform", *self.identity[:15]),
                      ("--point-target-transform", *self.identity, 2),
                      ("--point-target-transform", *self.identity[:15], 0),
                      ("--point-world-transform", "nan", *self.identity[1:]),
                      ("--point-world-transform", *self.identity, "--point-world-transform", *self.identity),
                      ("--grid", "one", "--input-grid", "two"), ("--grid", ""), ("--grid", "-h"),
                      ("--any-group", "A", "--any-group", "A"), ("--all-group", ""),
                      ("--point-position-error", 0), ("--point-position-error", .251),
                      ("--point-position-error", "inf"), ("--max-point-motion-work", 0),
                      ("--max-point-motion-work", " -1"), ("--max-point-motion-work", 10000000001),
                      ("--max-point-motion-grids", 4097), ("--max-point-motion-metadata", 0),
                      ("--max-attributes", 1025), ("--max-groups", 4097), ("--max-points", 2**63),
                      ("--max-string-bytes", 0), ("--allow-point-activity-merge", "--allow-point-activity-merge"),
                      ("--voxel-size", .1), ("--half-width", 3), ("--max-triangles", 1),
                      ("--max-edits", 1), ("--vector", 1, 2, 3), ("--point-position-error", .01, "extra")):
            cases.append((*base, *flags))
        for args in cases:
            with self.subTest(args=args):
                self.call(*args, succeeds=False)
                self.assertFalse((self.directory / "bad.vdb").exists())
        self.assertEqual(list(self.directory.iterdir()), [])

    def test_disabled_valid_route_reports_availability_without_source_access(self):
        if self.enabled:
            self.skipTest("OpenVDB is enabled")
        for flags in ((), ("--point-world-transform", *self.identity),
                      ("--any-group", "missing", "--allow-point-activity-merge")):
            result = self.call("--move-native-points", "不存在.vdb", "bad.vdb", *flags, succeeds=False)
            self.assertIn("ENABLE_OPENVDB=ON", result.stderr)
        self.assertEqual(list(self.directory.iterdir()), [])

    def test_exact_noop_and_explicit_identity_preserve_native_payload_columns(self):
        self.enabled_only()
        before = self.fixture()
        report = self.report("--move-native-points", "points.vdb", "copy.vdb",
                             "--point-world-transform", *self.identity, "--allow-point-activity-merge")
        self.assert_report(report)
        self.assertTrue(report["noOp"])
        self.assertEqual(report["selectedCount"], 6)
        self.assertEqual(report["maxPositionErrorVoxels"], 0)
        self.compare(before, self.snapshot("copy.vdb"))

    def test_group_world_affine_reorders_records_but_preserves_exact_ids_and_payload(self):
        self.enabled_only()
        before = self.fixture()
        matrix = (-1, .5, 0, 5, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1)
        report = self.report("--move-native-points", "points.vdb", "moved.vdb",
                             "--point-world-transform", *matrix, "--any-group", "A",
                             "--all-group", "B", "--exclude-group", "C", "--input-grid", "descriptor:points")
        self.assert_report(report)
        self.assertEqual(report["selectedCount"], 2)
        self.assertFalse(report["noOp"])
        after = self.snapshot("moved.vdb")
        self.assertNotEqual(before["ids"], after["ids"])
        self.compare(before, after, matrix, lambda p: p["groups"]["A"] and p["groups"]["B"] and not p["groups"]["C"])

    def test_target_map_repartitions_unselected_world_points_without_value_semantic_changes(self):
        self.enabled_only()
        before = self.fixture()
        target = (0, -.5, 0, 10, .5, 0, 0, -2, 0, 0, -.5, 3, 0, 0, 0, 1)
        report = self.report("--move-native-points", "points.vdb", "mapped.vdb",
                             "--point-target-transform", *target, "--any-group", "empty")
        self.assert_report(report)
        self.assertEqual(report["selectedCount"], 0)
        self.assertFalse(report["noOp"])
        self.compare(before, self.snapshot("mapped.vdb"))
        grid = self.report("--inspect", "mapped.vdb")["grids"][0]
        self.assertEqual(grid["indexToWorld"], list(target))
        self.assertEqual(grid["valueType"], "ptdataidx32")

    def test_singular_world_deformation_can_collapse_points_without_deleting_payloads(self):
        self.enabled_only()
        before = self.fixture()
        collapse = (0, 0, 0, 1, 0, 0, 0, 2, 0, 0, 0, 3, 0, 0, 0, 1)
        report = self.report("--move-native-points", "points.vdb", "collapsed.vdb",
                             "--point-world-transform", *collapse)
        self.assert_report(report)
        self.assertEqual(report["outputVoxelCount"], 1)
        self.compare(before, self.snapshot("collapsed.vdb"), collapse)

    def test_empty_native_schema_archive_remains_a_zero_record_noop(self):
        self.enabled_only()
        self.fixture()
        self.call("--select-points", "points.vdb", "empty.vdb", "--any-group", "empty")
        before = self.neighbors("empty.vdb")
        self.assertTrue(before["archivedSchema"])
        self.protect("empty.vdb")
        result = self.report("--move-native-points", "empty.vdb", "empty-copy.vdb")
        self.assert_report(result, count=0)
        self.assertTrue(result["noOp"])
        self.assertTrue(result["archivedSchema"])
        after = self.neighbors("empty-copy.vdb")
        self.assertEqual(before["schema"], after["schema"])
        # Native writer compression/statistics are regenerated. Custom keys,
        # including the three opaque archive entries, must remain exact.
        persistent = lambda rows: [m for m in rows if not m["name"].startswith("file_")]
        self.assertEqual(persistent(before["gridMetadata"]), persistent(after["gridMetadata"]))
        self.assertEqual(before["fileMetadata"], after["fileMetadata"])

    def test_unicode_literal_dash_h_exclusive_outputs_limits_and_invalid_maps(self):
        self.enabled_only()
        self.fixture(name="入力-é.vdb", grid="粒子")
        parent = self.directory / "変形-é"
        parent.mkdir()
        output = parent / "出力.vdb"
        result = self.report("--move-native-points", "入力-é.vdb", output, "--input-grid", "粒子", "--grid-name", "移動")
        self.assert_report(result)
        self.assertEqual(result["gridName"], "移動")
        self.assertEqual(self.report("--point-neighbors", output, "--neighbor-buckets")["schema"]["gridName"], "移動")
        previous = output.read_bytes()
        self.call("--move-native-points", "入力-é.vdb", output, succeeds=False)
        self.assertEqual(output.read_bytes(), previous)
        (self.directory / "-h").write_bytes((self.directory / "入力-é.vdb").read_bytes())
        literal = self.report("--move-native-points", "./-h", "literal.vdb")
        self.assertTrue(literal["noOp"])
        for flags in (("--max-input-bytes", 1024), ("--max-voxels", 1), ("--max-points", 1),
                      ("--max-attributes", 1), ("--max-groups", 1), ("--max-string-bytes", 1),
                      ("--max-point-motion-work", 1), ("--max-point-motion-metadata", 1),
                      ("--grid", "missing"), ("--any-group", "missing"),
                      ("--point-target-transform", 1, 0, 0, 0, 0, 2, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1),
                      ("--point-target-transform", 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1)):
            with self.subTest(flags=flags):
                self.call("--move-native-points", "入力-é.vdb", "bad.vdb", *flags, succeeds=False)
                self.assertFalse((self.directory / "bad.vdb").exists())
        self.call("--sphere", "scalar.vdb", "--voxel-size", .25)
        self.call("--move-native-points", "scalar.vdb", "bad.vdb", succeeds=False)
        self.assertFalse((self.directory / "bad.vdb").exists())


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--tool", required=True, type=Path)
    parser.add_argument("--enabled", required=True, choices=("0", "1"))
    args, remaining = parser.parse_known_args()
    PointMotionCli.tool = args.tool.resolve(strict=True)
    PointMotionCli.enabled = args.enabled == "1"
    unittest.main(argv=[__file__, *remaining])
