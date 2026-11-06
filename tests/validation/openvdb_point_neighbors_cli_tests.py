"""Real-tool native point index/atlas queries and explicit-map occupancy masks."""
import argparse
import json
import math
from pathlib import Path
import subprocess
import tempfile
import unittest


class PointNeighborsCli(unittest.TestCase):
    positions = (-9., -1., 0., 0., 1., 2., 9.)
    radii = (.25, .5, 1., 2., 4., .5, .125)

    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="container-point-neighbor-cli-")
        self.directory = Path(self.temporary.name)
        self.originals = {}

    def tearDown(self):
        try:
            for name, original in self.originals.items():
                self.assertEqual((self.directory / name).read_bytes(), original, name)
            self.assertFalse(list(self.directory.glob(".container-*")))
        finally:
            self.temporary.cleanup()

    def call(self, *args, succeeds=True):
        result = subprocess.run([str(self.tool), *map(str, args)], cwd=self.directory,
                                capture_output=True, text=True, encoding="utf-8", timeout=60)
        self.assertEqual(result.returncode == 0, succeeds,
                         f"{args!r}\n{result.stdout}\n{result.stderr}")
        return result

    def report(self, *args):
        return json.loads(self.call(*args).stdout)

    def enabled_only(self):
        if not self.enabled:
            self.skipTest("OpenVDB is disabled")

    def protect(self, name):
        self.originals[name] = (self.directory / name).read_bytes()

    def write_json(self, name, document):
        (self.directory / name).write_text(json.dumps(document, ensure_ascii=False), encoding="utf-8")

    def points(self):
        self.write_json("points.json", [{"position": [x, 0, 0]} for x in self.positions])
        self.call("--points", "points.json", "raw.vdb", "--voxel-size", 1)
        self.write_json("edits.json", {"version": 1, "attributes": [
            {"op": "add", "name": "id", "type": "int64", "values": [2**63 - 1 - i for i in range(7)]},
            {"op": "add", "name": "rad", "type": "double", "values": list(self.radii)}],
            "groups": [{"op": "add", "name": "A", "values": [i % 2 == 0 for i in range(7)]},
                       {"op": "add", "name": "B", "values": [i < 6 for i in range(7)]},
                       {"op": "add", "name": "C", "values": [i == 2 for i in range(7)]},
                       {"op": "add", "name": "empty", "values": [False]}]})
        self.call("--edit-point-attributes", "raw.vdb", "edits.json", "edited.vdb", "--grid-name", "points")
        self.write_json("metadata.json", {"version": 1, "inputs": [{"path": "edited.vdb", "edits": [
            {"selector": "points", "metadata": [{"name": "artist", "operation": "set", "type": "string", "value": "neighbor-\u0000-é"}]}]}],
            "metadata": [{"name": "scene", "operation": "set", "type": "int64", "value": 9007199254740993}]})
        self.call("--author-file", "metadata.json", "points.vdb")
        for name in ("points.json", "raw.vdb", "edits.json", "edited.vdb", "points.vdb"):
            self.protect(name)

    @staticmethod
    def query_flags(queries):
        return tuple(value for center, radius in queries for value in ("--neighbor-query", *center, radius))

    def neighbors(self, queries=(((0, 0, 0), 1),), *flags, source="points.vdb"):
        return self.report("--point-neighbors", source, *self.query_flags(queries), *flags)

    def assert_oracle(self, report, queries, selected=range(7), radii=None):
        self.assertEqual(len(report["queries"]), len(queries))
        for row, (center, radius) in zip(report["queries"], queries):
            expected = []
            for ordinal in selected:
                distance = math.hypot(self.positions[ordinal] - center[0], center[1], center[2])
                particle_radius = 0 if radii is None else radii[ordinal]
                if distance <= radius + particle_radius:
                    expected.append((distance, ordinal, particle_radius))
            expected.sort()
            self.assertEqual([h["reference"]["ordinal"] for h in row["hits"]], [i for d, i, r in expected])
            for hit, (distance, ordinal, particle_radius) in zip(row["hits"], expected):
                self.assertEqual(hit["positionWorld"], [self.positions[ordinal], 0, 0])
                self.assertEqual(hit["distanceWorld"], distance)
                self.assertEqual(hit["radiusWorld"], particle_radius)
                leaf = -16 if ordinal == 0 else -8 if ordinal == 1 else 0 if ordinal < 6 else 8
                local = 0 if ordinal in (0, 1, 6) else ordinal - 2
                self.assertEqual(hit["reference"]["leafOrigin"], [leaf, 0, 0])
                self.assertEqual(hit["reference"]["localIndex"], local)

    def test_help_strict_operands_duplicates_modes_and_caps(self):
        text = self.call("--help").stdout
        for option in ("--point-neighbors", "--neighbor-query", "--neighbor-mode", "--neighbor-buckets",
                       "--neighbor-index-voxel-size", "--neighbor-atlas-levels", "--neighbor-radius-attribute",
                       "--neighbor-radius", "--max-neighbors", "--max-neighbor-queries",
                       "--indexed-point-mask", "--indexed-origin", "--indexed-counts", "--indexed-count-name",
                       "--max-point-index-work", "--max-point-index-grids", "--max-point-index-metadata"):
            self.assertIn(option, text)
        base = ("--point-neighbors", "missing", "--neighbor-query", 0, 0, 0, 0)
        cases = [("--point-neighbors",), ("--point-neighbors", "-h"),
                 ("--point-neighbors", "missing", "--neighbor-query", 0, 0, 0),
                 ("--point-neighbors", "missing", "--neighbor-query", 0, 0, 0, -1),
                 ("--point-neighbors", "missing", "--neighbor-query", 0, 0, 0, "nan"),
                 ("--indexed-point-mask", "missing"), ("--indexed-point-mask", "missing", "-h"),
                 ("--indexed-point-mask", "missing", "bad.vdb", "--indexed-count-name", "counts"),
                 ("--indexed-point-mask", "missing", "bad.vdb", "--neighbor-mode", "particles"),
                 ("--indexed-point-mask", "missing", "bad.vdb", "--indexed-origin", 0, 0),
                 ("--indexed-point-mask", "missing", "bad.vdb", "--indexed-voxel-size", "1e-7")]
        for flags in (("--neighbor-mode", "bad"), ("--neighbor-radius", 1),
                      ("--neighbor-radius-attribute", "rad"), ("--neighbor-atlas-levels", 2),
                      ("--neighbor-mode", "particles", "--neighbor-radius", 1, "--neighbor-radius-attribute", "rad"),
                      ("--neighbor-mode", "particles", "--neighbor-atlas-levels", 33),
                      ("--neighbor-index-voxel-size", 0), ("--max-neighbors", 10000001),
                      ("--max-neighbor-queries", 0), ("--max-point-index-work", 0),
                      ("--max-point-index-grids", 4097), ("--max-point-index-metadata", 0),
                      ("--max-attributes", 1025), ("--max-groups", 4097), ("--max-string-bytes", 0),
                      ("--grid", "one", "--input-grid", "two"),
                      ("--neighbor-buckets", "--neighbor-buckets"),
                      ("--any-group", "A", "--any-group", "A"),
                      ("--grid", "-h"), ("--grid-name", "ignored"), ("--voxel-size", .1),
                      ("--max-edits", 1), ("--indexed-counts",)):
            cases.append((*base, *flags))
        for args in cases:
            with self.subTest(args=args):
                self.call(*args, succeeds=False)
                self.assertFalse((self.directory / "bad.vdb").exists())
        self.assertEqual(list(self.directory.iterdir()), [])

    def test_disabled_routes_fail_before_source_io(self):
        if self.enabled:
            self.skipTest("OpenVDB is enabled")
        for args in (("--point-neighbors", "不存在", "--neighbor-query", 0, 0, 0, 0),
                     ("--point-neighbors", "missing", "--neighbor-buckets", "--neighbor-mode", "particles"),
                     ("--indexed-point-mask", "missing", "bad.vdb", "--indexed-counts")):
            self.assertIn("ENABLE_OPENVDB=ON", self.call(*args, succeeds=False).stderr)
        self.assertEqual(list(self.directory.iterdir()), [])

    def test_centers_inclusive_ties_original_refs_schema_metadata_and_native_buckets(self):
        self.enabled_only(); self.points()
        queries = (((0, 0, 0), 1), ((0, 0, 0), 0), ((-9, 0, 0), 0))
        report = self.neighbors(queries, "--neighbor-buckets", "--neighbor-index-voxel-size", .5,
                                "--grid", "descriptor:points")
        self.assert_oracle(report, queries)
        self.assertEqual(report["schemaVersion"], 1)
        self.assertEqual(report["traversal"], "leaf_origin_xyz_then_local_index")
        self.assertEqual(report["schema"]["pointCount"], 7)
        self.assertEqual(report["selectedCount"], 7)
        self.assertEqual(report["schema"]["groups"], ["A", "B", "C", "empty"])
        self.assertEqual(next(a for a in report["schema"]["attributes"] if a["name"] == "id")["valueType"], "int64")
        self.assertEqual(next(m for m in report["gridMetadata"] if m["name"] == "artist")["value"], "neighbor-\u0000-é")
        self.assertEqual(next(m for m in report["fileMetadata"] if m["name"] == "scene")["value"], "9007199254740993")
        self.assertIsInstance(report["descriptorMetadata"], list)
        self.assertEqual(sum(level["pointCount"] for level in report["levels"]), 7)
        ordinals = []
        for bucket in report["buckets"]:
            members = [ref["ordinal"] for ref in bucket["points"]]
            self.assertEqual(members, sorted(members)); ordinals.extend(members)
        self.assertEqual(sorted(ordinals), list(range(7)))
        catalog = self.report("--point-neighbors", "points.vdb", "--neighbor-buckets")
        self.assertEqual(catalog["queries"], [])
        self.assertEqual(sum(len(b["points"]) for b in catalog["buckets"]), 7)
        (self.directory / "-h").write_bytes((self.directory / "points.vdb").read_bytes())
        self.assert_oracle(self.neighbors(queries, source="./-h"), queries)

    def test_particle_attribute_constant_radius_and_group_intersection_match_world_oracles(self):
        self.enabled_only(); self.points()
        queries = (((0, 0, 0), 0), ((.5, 0, 0), 0), ((0, 0, 0), 1))
        report = self.neighbors(queries, "--neighbor-mode", "particles", "--neighbor-radius-attribute", "rad",
                                "--neighbor-index-voxel-size", .5, "--neighbor-atlas-levels", 16, "--neighbor-buckets")
        self.assert_oracle(report, queries, radii=self.radii)
        self.assertEqual(sum(l["pointCount"] for l in report["levels"]), 7)
        self.assertGreater(len(report["levels"]), 1)
        constant = self.neighbors(queries, "--neighbor-mode", "particles", "--neighbor-radius", .5)
        self.assert_oracle(constant, queries, radii=[.5] * 7)
        selected = self.neighbors(queries, "--neighbor-mode", "particles", "--neighbor-radius-attribute", "rad",
                                  "--any-group", "A", "--all-group", "B", "--exclude-group", "C")
        self.assertEqual(selected["selectedCount"], 2)
        self.assert_oracle(selected, queries, selected=(0, 4), radii=self.radii)
        empty = self.neighbors(queries, "--any-group", "empty")
        self.assertEqual(empty["selectedCount"], 0)
        self.assertTrue(all(not q["hits"] for q in empty["queries"]))
        self.call("--select-points", "points.vdb", "empty.vdb", "--any-group", "empty")
        archived = self.neighbors(queries, source="empty.vdb")
        self.assertTrue(archived["archivedSchema"])
        self.assertEqual(archived["schema"]["pointCount"], 0)
        binary = [m for m in archived["gridMetadata"] if not m["valueValidUtf8"]]
        self.assertTrue(binary, "native empty archives retain binary String summaries")
        for item in binary:
            self.assertIsNone(item["value"])
            self.assertEqual(item["valueHexEncoding"], "metadata-summary-bytes")
            value = bytes.fromhex(item["valueHex"])
            self.assertTrue(value)
            self.assertEqual(len(value), item["nativeBytes"])
            with self.assertRaises(UnicodeDecodeError):
                value.decode("utf-8")

    def test_explicit_mask_map_exact_counts_group_selection_unicode_and_exclusive_output(self):
        self.enabled_only(); self.points()
        parent = self.directory / "近傍-é"; parent.mkdir()
        source = parent / "入力.vdb"; source.write_bytes((self.directory / "points.vdb").read_bytes())
        output = parent / "占有.vdb"
        report = self.report("--indexed-point-mask", source, output, "--indexed-voxel-size", 2,
                             "--indexed-origin", .5, 0, 0, "--indexed-counts", "--indexed-count-name", "tally",
                             "--grid-name", "occupied", "--input-grid", "points")
        self.assertEqual(report["inputCount"], 7)
        self.assertEqual(report["selectedCount"], 7)
        self.assertEqual(report["mask"]["activeVoxelCount"], 5)
        self.assertEqual(report["counts"]["gridName"], "tally")
        self.assertEqual(report["counts"]["activeVoxelCount"], 5)
        inspected = self.report("--inspect", output)
        by_name = {g["name"]: g for g in inspected["grids"]}
        self.assertEqual(by_name["occupied"]["valueType"], "bool")
        self.assertEqual(by_name["tally"]["valueType"], "int64")
        self.assertEqual(by_name["occupied"]["voxelSize"], [2, 2, 2])
        self.assertEqual(by_name["occupied"]["indexToWorld"][3], .5)
        # Exact native Int64 edits independently distinguish four count1 cells
        # from the one count3 cell. No lossy scalar conversion is involved.
        edited = self.report("--edit-topology", output, "count3.vdb", "--grid", "tally",
                             "--edit-operation", "deactivate-value", "--native-value", 1)
        self.assertEqual(edited["activeVoxelCount"], 1)
        last = self.report("--edit-topology", "count3.vdb", "count0.vdb", "--edit-operation", "deactivate-value", "--native-value", 3)
        self.assertEqual(last["activeVoxelCount"], 0)
        before = output.read_bytes()
        self.call("--indexed-point-mask", source, output, succeeds=False)
        self.assertEqual(output.read_bytes(), before)
        selected = self.report("--indexed-point-mask", "points.vdb", "selected.vdb", "--indexed-voxel-size", 2,
                               "--indexed-origin", .5, 0, 0, "--any-group", "A", "--all-group", "B", "--exclude-group", "C")
        self.assertEqual(selected["selectedCount"], 2)
        self.assertEqual(selected["mask"]["activeVoxelCount"], 2)
        self.assertIsNone(selected["counts"])

    def test_full_voxel_size_endpoints_preserve_world_query_and_output_maps(self):
        self.enabled_only(); self.points()
        query = (((0, 0, 0), 0),)
        for voxel in (1e-6, 1e6):
            for particles in (False, True):
                flags = ("--neighbor-mode", "particles", "--neighbor-radius", .25) if particles else ()
                report = self.neighbors(query, "--neighbor-index-voxel-size", voxel, *flags)
                self.assert_oracle(report, query, radii=[.25] * 7 if particles else None)
            result = self.report("--indexed-point-mask", "points.vdb", f"endpoint-{voxel}.vdb",
                                 "--indexed-voxel-size", voxel, "--indexed-counts", "--any-group", "C")
            self.assertEqual(result["selectedCount"], 1)
            self.assertEqual(result["mask"]["activeVoxelCount"], 1)
            grids = self.report("--inspect", f"endpoint-{voxel}.vdb")["grids"]
            self.assertEqual(len(grids), 2)
            for grid in grids:
                self.assertEqual(grid["voxelSize"], [voxel] * 3)

    def test_shared_caps_radius_semantics_selection_and_native_inputs_remain_intact(self):
        self.enabled_only(); self.points()
        queries = (((0, 0, 0), 1), ((0, 0, 0), 1))
        for flags in (("--max-neighbor-queries", 1), ("--max-neighbors", 1),
                      ("--max-point-index-work", 1), ("--max-point-index-metadata", 1),
                      ("--max-input-bytes", 1024), ("--max-voxels", 1), ("--max-points", 1),
                      ("--max-attributes", 1), ("--max-groups", 1), ("--any-group", "missing"),
                      ("--neighbor-mode", "particles", "--neighbor-radius-attribute", "P")):
            self.call("--point-neighbors", "points.vdb", *self.query_flags(queries), *flags, succeeds=False)
        for flags in (("--max-input-bytes", 1024), ("--max-voxels", 512, "--indexed-counts"),
                      ("--max-point-index-work", 1), ("--indexed-counts", "--indexed-count-name", "surface")):
            self.call("--indexed-point-mask", "points.vdb", "bad.vdb", *flags, succeeds=False)
            self.assertFalse((self.directory / "bad.vdb").exists())


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--tool", required=True, type=Path)
    parser.add_argument("--enabled", required=True, choices=("0", "1"))
    args, remaining = parser.parse_known_args()
    PointNeighborsCli.tool = args.tool.resolve(strict=True)
    PointNeighborsCli.enabled = args.enabled == "1"
    unittest.main(argv=[__file__, *remaining])
