"""Actual-tool exact typed sparse active-region queries and bounded JSON."""
import argparse
from array import array
import json
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "python"))
from container_openvdb.dense import DenseType, DenseVolume, VectorType, write_dense_array


class RegionQueriesCli(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="container-region-queries-cli-")
        self.directory = Path(self.temporary.name)
        self.originals = {}

    def tearDown(self):
        try:
            for path, original in self.originals.items():
                self.assertEqual(path.read_bytes(), original, path)
        finally:
            self.temporary.cleanup()

    def call(self, *args, succeeds=True):
        result = subprocess.run([str(self.tool), *map(str, args)], cwd=self.directory,
            capture_output=True, text=True, encoding="utf-8", timeout=60)
        self.assertEqual(result.returncode == 0, succeeds,
                         f"{args!r}\n{result.stdout}\n{result.stderr}")
        if not succeeds:
            self.assertEqual(result.stdout, "", "failed query emitted partial report")
        return result

    def report(self, *args):
        result = json.loads(self.call("--query-active-regions", *args).stdout)
        self.assertEqual(result["schemaVersion"], 1)
        self.assertEqual(result["operation"], "query-active-regions")
        self.assertTrue(result["countsComplete"])
        for name in ("sourceActiveVoxels", "sourceActiveRecords", "sourceActiveTiles",
                     "sourceActiveTileVoxels", "sourceLeafNodes", "sourceLowerNodes",
                     "sourceUpperNodes", "sourceRootEntries", "storedLeafVoxels",
                     "storedInternalSlots", "nodeAndRootEntryCount", "nativeBytes",
                     "nativeWorkspaceUpperBound", "reportReservation", "workspaceUpperBound",
                     "nativeWorkUpperBound", "formattingWorkUpperBound", "workUpperBound"):
            self.assertIs(type(result[name]), int, name)
            self.assertGreaterEqual(result[name], 0)
        self.assertEqual(result["workUpperBound"],
                         result["nativeWorkUpperBound"] + result["formattingWorkUpperBound"])
        self.assertEqual(result["workspaceUpperBound"],
                         result["nativeWorkspaceUpperBound"] + result["reportReservation"])
        self.assertEqual(result["traversal"], "native_depth_first_child_table_order")
        for query in result["queries"]:
            self.assertTrue(query["countsComplete"])
            self.assertIs(type(query["recordsTruncated"]), bool)
            for name in ("matchedRecordCount", "matchedVoxelCount", "returnedVoxelCount",
                         "omittedRecordCount", "omittedVoxelCount"):
                self.assertIs(type(query[name]), int)
            self.assertEqual(query["matchedRecordCount"], len(query["records"]) + query["omittedRecordCount"])
            self.assertEqual(query["matchedVoxelCount"], query["returnedVoxelCount"] + query["omittedVoxelCount"])
        return result

    def enabled_only(self):
        if not self.enabled:
            self.skipTest("OpenVDB is disabled")

    def fixture(self, name="native.vdb"):
        self.enabled_only()
        output = self.directory / name
        result = subprocess.run([str(self.generator), "--region", str(output)],
            capture_output=True, text=True, encoding="utf-8", timeout=60)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(json.loads(result.stdout)["grids"], 10)
        self.originals[output] = output.read_bytes()
        return output

    def test_help_strict_arity_indices_duplicates_caps_and_irrelevant_options(self):
        help_text = self.call("--help").stdout
        for flag in ("--query-active-regions", "--region", "--counts-only",
                     "--max-region-work", "--max-region-nodes", "--max-region-active-voxels",
                     "--max-region-grids", "--max-region-queries", "--max-region-metadata",
                     "--max-region-records", "--max-region-total-records", "--region-report-bytes"):
            self.assertIn(flag, help_text)
        cases = [("--query-active-regions",), ("--query-active-regions", ""),
                 ("--query-active-regions", "-h"), ("--query-active-regions", "--region")]
        flags = [("--region", 0, 0, 0, 1, 1), ("--region", 0, 0, 0, 1, 1, "-h"),
                 ("--region", 0, 0, 0, 1, 1, "1.0"),
                 ("--region", "+0", 0, 0, 1, 1, 1),
                 ("--region", -2147483649, 0, 0, 1, 1, 1),
                 ("--region", 0, 0, 0, 2147483648, 1, 1),
                 ("--region", 1, 0, 0, 0, 1, 1),
                 ("--grid", "a", "--input-grid", "b"),
                 ("--counts-only", "--counts-only"),
                 ("--counts-only", "--max-region-records", 0),
                 ("--counts-only", "--max-region-total-records", 0),
                 ("--max-region-work", 0), ("--max-region-work", 10000000001),
                 ("--max-input-bytes", 17179869185),
                 ("--max-region-nodes", 1000001), ("--max-region-grids", 4097),
                 ("--max-region-queries", 0), ("--max-region-queries", 4097),
                 ("--max-region-metadata", 16777217), ("--max-region-records", 1000001),
                 ("--max-region-total-records", "18446744073709551616"),
                 ("--max-region-active-voxels", " 1"), ("--max-voxels", "+1"),
                 ("--region-report-bytes", 16777217), ("--grid", "-h"),
                 ("--max-region-nodes", 2, "--max-region-nodes", 3),
                 ("--grid-name", "output"), ("--output", "out.json"),
                 ("--voxel-size", .5), ("--radius", 1)]
        cases += [("--query-active-regions", "missing", *flag) for flag in flags]
        for case in cases:
            with self.subTest(case=case):
                failure = self.call(*case, succeeds=False)
                self.assertNotIn("ENABLE_OPENVDB=ON", failure.stderr,
                                 "invalid syntax reached runtime availability")
                if len(case) <= 2:
                    expected = "requires nonempty INPUT"
                else:
                    operands = case[2:]
                    first = operands[0]
                    if first == "--region":
                        if len(operands) < 7:
                            expected = "missing value"
                        elif operands[-1] == "-h":
                            expected = "missing nonempty"
                        elif operands == ("--region", 1, 0, 0, 0, 1, 1):
                            expected = "ordered inclusive"
                        else:
                            expected = "Int32"
                    elif first == "--grid" and operands[-1] == "-h":
                        expected = "missing nonempty"
                    elif first == "--counts-only" and len(operands) > 1 and operands[1] != "--counts-only":
                        expected = "cannot be combined"
                    elif first == "--grid" or operands.count(first) > 1:
                        expected = "duplicate"
                    elif first in ("--grid-name", "--output", "--voxel-size", "--radius"):
                        expected = "unknown/incompatible"
                    elif any(str(v).startswith((" ", "+")) for v in operands[1:]):
                        expected = "unsigned decimal"
                    else:
                        expected = "count exceeds range"
                self.assertIn(expected, failure.stderr)
        self.assertEqual(list(self.directory.iterdir()), [])

    def test_disabled_availability_precedes_paths_reports_and_tiny_positive_caps(self):
        if self.enabled:
            self.skipTest("OpenVDB is enabled")
        for flags in ((), ("--max-input-bytes", 1), ("--max-region-work", 1),
                      ("--counts-only", "--max-region-metadata", 1)):
            with self.subTest(flags=flags):
                result = self.call("--query-active-regions", "不存在/missing.vdb", *flags, succeeds=False)
                self.assertIn("ENABLE_OPENVDB=ON", result.stderr)
        self.assertEqual(list(self.directory.iterdir()), [])

    def test_all_nine_native_types_exact_bits_activity_and_raw_vector_values(self):
        source = self.fixture()
        expected = {
            "bool": {"type": "bool", "value": False},
            "mask": {"type": "mask", "value": True},
            "int32": {"type": "int32", "value": -2147483648},
            "int64": {"type": "int64", "value": 9007199254740993},
            "float": {"type": "float32", "bits": "7f812345"},
            "double": {"type": "float64", "bits": "fff0000000001234"},
            "vec3i": {"type": "vec3i", "value": [-2147483648, 0, 2147483647]},
            "vec3f": {"type": "vec3f", "bits": ["80000000", "7f812345", "c0000000"]},
            "vec3d": {"type": "vec3d", "bits": ["8000000000000000", "7ff0000000001234", "c000000000000000"]},
        }
        for grid, value in expected.items():
            with self.subTest(grid=grid):
                report = self.report(source, "--grid", grid, "--region", -2, 1, 2, 0, 3, 4)
                self.assertEqual(report["nativeType"], value["type"])
                self.assertEqual(report["sourceActiveVoxels"], 1)
                self.assertEqual(report["sourceActiveTiles"], 0)
                self.assertEqual(report["indexToWorld"], [.5, 0, 0, 0, 0, .5, 0, 0, 0, 0, .5, 0, 0, 0, 0, 1])
                query = report["queries"][0]
                self.assertEqual(query["matchedVoxelCount"], 1)
                self.assertFalse(query["recordsTruncated"])
                record, = query["records"]
                self.assertEqual(record["value"], value)
                self.assertEqual(record["sourceLevel"], 0)
                self.assertFalse(record["tile"])
                self.assertEqual(record["clipped"], {"minimum": [-1, 2, 3], "maximum": [-1, 2, 3]})
                if grid.startswith("vec"):
                    self.assertFalse(report["inWorldSpace"])
        self.assertEqual(self.report(source, "--grid", "bool")["background"], {"type": "bool", "value": True})

    def test_compact_root_tiles_have_independent_clipped_uint64_multiplicity(self):
        source = self.fixture()
        report = self.report(source, "--grid", "tiles", "--max-voxels", 1,
            "--max-region-nodes", 3, "--region", 4095, 2, 3, 4097, 3, 4,
            "--region", 0, 0, 0, 4095, 4095, 4095)
        self.assertEqual(report["sourceActiveVoxels"], 2**37)
        self.assertEqual(report["sourceActiveTileVoxels"], 2**37)
        self.assertEqual(report["sourceActiveTiles"], 2)
        self.assertEqual(report["sourceRootEntries"], 2)
        self.assertEqual(report["sourceLeafNodes"], 0)
        self.assertEqual(report["storedLeafVoxels"], 0)
        self.assertEqual(report["nodeAndRootEntryCount"], 3)
        clipped, full = report["queries"]
        self.assertEqual(clipped["matchedVoxelCount"], 3*2*2)
        self.assertEqual(sorted(r["voxelCount"] for r in clipped["records"]), [4, 8])
        self.assertEqual(full["matchedVoxelCount"], 4096**3)
        self.assertEqual(full["records"][0]["voxelCount"], 2**36)
        self.assertEqual({r["value"]["value"] for r in clipped["records"]}, {2**53+1, 2**63-1})
        self.assertTrue(all(r["tile"] and r["sourceLevel"] == 3 for r in clipped["records"]))

    def test_prefix_quotas_counts_only_empty_batch_and_int32_endpoint_boxes(self):
        source = self.fixture()
        regions = ("--region", -1, -1, -1, -1, -1, -1,
                   "--region", -2147483648, -2147483648, -2147483648, 2147483647, 2147483647, 2147483647,
                   "--region", 0, 0, 0, 8191, 4095, 4095)
        full = self.report(source, "--grid", "tiles", *regions)
        limited = self.report(source, "--grid", "tiles", *regions,
            "--max-region-records", 1, "--max-region-total-records", 1)
        self.assertEqual([len(q["records"]) for q in limited["queries"]], [0, 1, 0])
        self.assertEqual([q["matchedVoxelCount"] for q in limited["queries"]], [0, 2**37, 2**37])
        self.assertEqual(limited["queries"][1]["records"][0], full["queries"][1]["records"][0])
        self.assertTrue(limited["queries"][2]["recordsTruncated"])
        counts = self.report(source, "--grid", "tiles", *regions, "--counts-only")
        self.assertTrue(all(not q["records"] for q in counts["queries"]))
        self.assertEqual(counts["queries"][1]["omittedVoxelCount"], 2**37)
        empty = self.report(source, "--grid", "tiles")
        self.assertEqual(empty["queries"], [])
        self.assertEqual(empty["sourceActiveVoxels"], 2**37)

    def test_binary_metadata_native_lengths_exact_hex_and_explicit_prefix_truncation(self):
        source = self.fixture()
        full = self.report(source, "--grid", "int64")
        for catalog in (full["gridMetadata"], full["fileMetadata"]):
            binary, = [m for m in catalog if m["name"]["text"] == "binary"]
            payload = binary["serializedValue"]
            expected = struct.pack("=I", 5) + b"raw\x00\xff"
            self.assertEqual(payload["byteCount"], len(expected))
            self.assertEqual(payload["bytesHex"], expected.hex())
            self.assertFalse(payload["truncated"])
        exact, = [m for m in full["fileMetadata"] if m["name"]["text"] == "exact_id"]
        self.assertEqual(exact["serializedValue"]["bytesHex"], (struct.pack("=I", 8)+struct.pack("=q", 2**53+1)).hex())
        small = self.report(source, "--grid", "int64", "--region-report-bytes", 3)
        self.assertEqual(small["reportBytePrefixLimit"], 3)
        self.assertTrue(small["gridName"]["truncated"])
        self.assertIsNone(small["gridName"]["text"])
        self.assertEqual(small["gridName"]["bytesHex"], b"int".hex())
        self.assertEqual(small["serializedTransform"]["byteCount"], full["serializedTransform"]["byteCount"])
        self.assertEqual(small["serializedTransform"]["bytesHex"], full["serializedTransform"]["bytesHex"][:6])
        zero = self.report(source, "--grid", "int64", "--region-report-bytes", 0)
        self.assertEqual(zero["serializedTransform"]["prefixBytes"], 0)
        self.assertEqual(zero["serializedTransform"]["bytesHex"], "")
        self.assertTrue(zero["serializedTransform"]["truncated"])

    def test_independent_dense_float_signed_zero_map_and_binary_native_name(self):
        self.enabled_only()
        for dtype in DenseType:
            values = array(dtype.typecode, [-0., 2., -3.] if dtype.components == 3 else [-0.])
            volume = DenseVolume(dtype, (1, 1, 1), values, bytearray([1]), origin=(-2, 1, 0),
                linear=(.5, .25, 0, 0, .5, 0, 0, 0, -.5), translation=(2, -1, .25),
                grid_name="\udcff", vector_type=VectorType.CONTRAVARIANT_ABSOLUTE, world_space=False)
            encoded = self.directory / f"dense-{int(dtype)}.cvarr"
            source = self.directory / f"dense-{int(dtype)}.vdb"
            write_dense_array(encoded, volume)
            self.call("--dense-import", encoded, source)
            self.originals[source] = source.read_bytes()
            report = self.report(source, "--region", -2, 1, 0, -2, 1, 0)
            self.assertIsNone(report["gridName"]["text"])
            self.assertFalse(report["gridName"]["validUtf8"])
            self.assertEqual(report["gridName"]["bytesHex"], "ff")
            self.assertEqual(report["indexToWorld"], [.5, .25, 0, 2, 0, .5, 0, -1, 0, 0, -.5, .25, 0, 0, 0, 1])
            actual = report["queries"][0]["records"][0]["value"]["bits"]
            code = "I" if dtype.width == 4 else "Q"
            expected = [f"{struct.unpack('='+code, struct.pack('='+dtype.typecode, v))[0]:0{dtype.width*2}x}" for v in values]
            self.assertEqual(actual, expected if dtype.components == 3 else expected[0])

    def test_unicode_sources_literal_h_generator_exclusivity_and_shared_limits(self):
        source = self.fixture("入力-é.vdb")
        original = source.read_bytes()
        failed = subprocess.run([str(self.generator), "--region", str(source)],
            capture_output=True, text=True, encoding="utf-8", timeout=60)
        self.assertNotEqual(failed.returncode, 0)
        self.assertEqual(source.read_bytes(), original)
        self.report(source, "--input-grid", "int64", "--counts-only")
        literal = self.directory / "-h"
        literal.write_bytes(original)
        self.originals[literal] = original
        self.assertEqual(self.report("./-h", "--grid", "tiles")["sourceActiveVoxels"], 2**37)
        for flags in (("--max-input-bytes", 1024), ("--max-region-work", 1),
                      ("--max-voxels", 1), ("--max-region-nodes", 1),
                      ("--max-region-grids", 1),
                      ("--max-region-metadata", 1),
                      ("--max-region-queries", 1, "--region", 0, 0, 0, 0, 0, 0,
                       "--region", 0, 0, 0, 1, 1, 1)):
            with self.subTest(flags=flags):
                self.call("--query-active-regions", source, "--grid", "int64", *flags, succeeds=False)
        # The active-volume cap must cover even a census-only query of a tile.
        self.call("--query-active-regions", source, "--grid", "tiles",
                  "--max-region-active-voxels", 2**37-1, succeeds=False)
        self.call("--query-active-regions", source, succeeds=False)
        self.call("--query-active-regions", source, "--grid", "missing", succeeds=False)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--tool", required=True, type=Path)
    parser.add_argument("--generator", type=Path)
    parser.add_argument("--enabled", required=True, choices=("0", "1"))
    args, remaining = parser.parse_known_args()
    RegionQueriesCli.tool = args.tool.resolve(strict=True)
    RegionQueriesCli.enabled = args.enabled == "1"
    if RegionQueriesCli.enabled and not args.generator:
        parser.error("--generator is required for enabled real native query fixtures")
    RegionQueriesCli.generator = args.generator.resolve(strict=True) if args.generator else None
    unittest.main(argv=[__file__, *remaining])
