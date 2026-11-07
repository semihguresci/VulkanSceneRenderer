"""Real NanoVDB primitive CLI proof, including native-I/O-only builds.

The independent raw decoder below reads only the documented fixed Float/Double
breadth-first leaf layout for NanoVDB ABI32.6/32.7. It never calls VDB conversion
or uses the production sampler. Public native validation precedes each decode.
"""
import argparse
import itertools
import json
import math
from pathlib import Path
import struct
import subprocess
import tempfile
import unittest


class RawScalar:
    def __init__(self, path):
        self.data = path.read_bytes()
        if len(self.data) < 736 or self.data[:7] != b"NanoVDB":
            raise AssertionError("expected one actual raw NanoVDB grid")
        version = struct.unpack_from("<I", self.data, 16)[0]
        if version >> 21 != 32 or ((version >> 10) & 2047) not in (6, 7):
            raise AssertionError("independent decoder only covers ABI32.6/32.7")
        self.grid_class, self.kind = struct.unpack_from("<II", self.data, 632)
        if self.kind not in (1, 2):
            raise AssertionError("independent decoder requires native Float/Double")
        self.code, self.width = ("f", 4) if self.kind == 1 else ("d", 8)
        if struct.unpack_from("<Q", self.data, 32)[0] != len(self.data):
            raise AssertionError("raw single-grid byte length differs")
        tree = 672
        leaf_offset, _, _, root_offset = struct.unpack_from("<4q", self.data, tree)
        leaf_count = struct.unpack_from("<I", self.data, tree + 32)[0]
        root = tree + root_offset
        self.background = struct.unpack_from("<" + self.code, self.data,
                                             root + (28 if self.width == 4 else 32))[0]
        self.voxel_size = struct.unpack_from("<3d", self.data, 608)
        # LeafData has bbox+flags16B, mask64B, four statistics; mValues
        # starts at a32-byte boundary (96 for Float,128 for Double).
        self.values_offset = 96 if self.width == 4 else 128
        leaf_bytes = self.values_offset + 512 * self.width
        if leaf_count > len(self.data) // leaf_bytes:
            raise AssertionError("leaf count exceeds declared bytes")
        self.leaves = {}
        for index in range(leaf_count):
            offset = tree + leaf_offset + index * leaf_bytes
            if offset < 0 or offset + leaf_bytes > len(self.data):
                raise AssertionError("leaf range escapes file")
            minimum = struct.unpack_from("<3i", self.data, offset)
            origin = tuple(x & ~7 for x in minimum)
            if origin in self.leaves:
                raise AssertionError("duplicated leaf origin")
            self.leaves[origin] = offset

    def sample_leaf(self, point):
        origin = tuple(x & ~7 for x in point)
        if origin not in self.leaves:
            raise AssertionError(f"analytic near-interface point {point} has no native leaf")
        index = ((point[0] & 7) << 6) | ((point[1] & 7) << 3) | (point[2] & 7)
        leaf = self.leaves[origin]
        value = struct.unpack_from("<" + self.code, self.data,
                                   leaf + self.values_offset + self.width * index)[0]
        mask = struct.unpack_from("<Q", self.data, leaf + 16 + (index // 64) * 8)[0]
        return value, bool(mask & (1 << (index % 64)))


class NanoPrimitivesCli(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="container-nano-primitives-cli-")
        self.directory = Path(self.temporary.name)

    def tearDown(self):
        self.assertFalse(list(self.directory.glob(".container-*")), "private output leaked")
        self.temporary.cleanup()

    def call(self, *args, succeeds=True):
        result = subprocess.run([str(self.tool), *map(str, args)], cwd=self.directory,
                                capture_output=True, text=True, encoding="utf-8", timeout=90)
        self.assertEqual(result.returncode == 0, succeeds,
                         f"{args!r}\n{result.stdout}\n{result.stderr}")
        return result

    def report(self, *args):
        return json.loads(self.call(*args).stdout)

    def native_only(self):
        if not self.native:
            self.skipTest("native NanoVDB I/O is disabled")

    def primitive(self, shape="sphere", output="out.nvdb", *extra):
        return self.report("--nano-primitive", shape, output, *extra)

    def failure(self, *args, output="bad.nvdb", parser=False):
        result = self.call(*args, succeeds=False)
        self.assertTrue(result.stderr.strip())
        if parser:
            self.assertNotIn("ENABLE_NANOVDB_IO", result.stderr)
        self.assertFalse((self.directory / output).exists())
        return result

    def validate(self, result):
        self.assertEqual(result["schemaVersion"], 1)
        self.assertEqual(result["operation"], "nano-primitive")
        self.assertTrue(result["reportProduced"])
        self.assertTrue(result["nativeIoAvailable"])
        self.assertEqual(result["openVdbConversionAvailable"], self.conversion)
        settings = result["settings"]
        self.assertEqual(result["combinedWorkspaceUpperBound"],
                         result["workspaceUpperBound"] + result["reportReservationBytes"])
        self.assertEqual(result["combinedWorkUpperBound"],
                         result["workUpperBound"] + result["reportReservationWork"])
        self.assertLessEqual(result["combinedWorkspaceUpperBound"], settings["requestedLimits"]["maxWorkspaceBytes"])
        self.assertLessEqual(result["combinedWorkUpperBound"], settings["requestedLimits"]["maxWork"])
        self.assertEqual(settings["nativeLimits"]["maxWorkspaceBytes"],
                         settings["requestedLimits"]["maxWorkspaceBytes"] - result["reportReservationBytes"])
        self.assertEqual(settings["nativeLimits"]["maxWork"],
                         settings["requestedLimits"]["maxWork"] - result["reportReservationWork"])
        self.assertEqual(result["file"]["nativeGridCount"], 1)
        self.assertEqual(len(result["file"]["grids"]), 1)
        grid = result["file"]["grids"][0]
        self.assertTrue(grid["supported"])
        self.assertTrue(grid["structureValidated"])
        self.assertEqual(grid["name"], settings["gridName"])
        self.assertGreater(grid["activeVoxels"], 0)
        self.assertLessEqual(grid["storedLeafVoxels"], result["storedLeafVoxelUpperBound"])
        self.assertLessEqual(grid["nativeBytes"], result["nativeByteUpperBound"])
        self.assertFalse(result["diagnosticsTruncated"])
        self.assertFalse(result["file"]["diagnosticsTruncated"])

    def raw(self, output):
        report = self.report("--validate-nano", output)
        self.assertTrue(report["grids"][0]["structureValidated"])
        return RawScalar(self.directory / output)

    def test_capability_and_help_are_native_io_not_openvdb(self):
        caps = self.report("--nano-capabilities")
        self.assertEqual(caps["nativeIo"], self.native)
        self.assertEqual(caps["openVdbConversion"], self.conversion)
        self.assertIn("--nano-primitive", self.call("--help").stdout)

    def test_strict_shapes_duplicates_arity_and_irrelevant_flags(self):
        cases = [("--nano-primitive",), ("--nano-primitive", "sphere"),
                 ("--nano-primitive", "other", "bad.nvdb"),
                 ("--nano-primitive", "sphere", ""),
                 ("--nano-primitive", "sphere", "-h"),
                 ("--nano-primitive", "sphere", "bad.nvdb", "--radius"),
                 ("--nano-primitive", "sphere", "bad.nvdb", "--center", "0", "1"),
                 ("--nano-primitive", "sphere", "bad.nvdb", "--radius", "1", "--radius", "1"),
                 ("--nano-primitive", "box", "bad.nvdb", "--radius", "1"),
                 ("--nano-primitive", "torus", "bad.nvdb", "--box-dimensions", "2", "2", "2"),
                 ("--nano-primitive", "sphere", "bad.nvdb", "--minor-radius", ".25"),
                 ("--nano-primitive", "sphere", "bad.nvdb", "--grid", "ignored"),
                 ("--nano-primitive", "sphere", "bad.nvdb", "--allow-metadata-loss"),
                 ("--nano-primitive", "sphere", "bad.nvdb", "--allow-float-cache-rounding"),
                 ("--nano-primitive", "sphere", "bad.nvdb", "--nano-storage", "half"),
                 ("--nano-primitive", "sphere", "bad.nvdb", "--nano-class", "other"),
                 ("--nano-primitive", "sphere", "bad.nvdb", "--native-statistics", "other"),
                 ("--nano-primitive", "sphere", "bad.nvdb", "--checksum", "other"),
                 ("--nano-primitive", "sphere", "bad.nvdb", "--raw", "--raw"),
                 ("--nano-primitive", "sphere", "bad.nvdb", "--raw", "--codec", "zip")]
        for arguments in cases:
            with self.subTest(arguments=arguments):
                self.failure(*arguments, parser=True)
        for text in ("NaN", "inf", "1e999", "1e-999", " 1", "1junk", "+-1"):
            with self.subTest(text=text):
                self.failure("--nano-primitive", "sphere", "bad.nvdb", "--radius", text, parser=True)
        for text in ("0", "-1", "18446744073709551616"):
            self.failure("--nano-primitive", "sphere", "bad.nvdb", "--max-nano-work", text, parser=True)

    def test_disabled_precedes_paths_and_report_workspace(self):
        if self.native:
            self.skipTest("native I/O is enabled")
        for extra in ((), ("--max-workspace-bytes", "1"), ("--max-nano-work", "1"),
                      ("--max-input-bytes", "1")):
            result = self.failure("--nano-primitive", "sphere", "missing/never.nvdb", *extra)
            self.assertIn("ENABLE_NANOVDB_IO", result.stderr)
            self.assertNotIn("ENABLE_OPENVDB", result.stderr)

    def test_raw_sdf_shapes_and_storage_have_independent_analytic_values(self):
        self.native_only()
        samples = {"sphere": [((7, 0, 0), -.125), ((9, 0, 0), .125),
                              ((6, 6, 0), math.sqrt(.75**2 + .75**2) - 1)],
                   "box": [((7, 0, 0), -.125), ((9, 0, 0), .125)],
                   "torus": [((8, 0, 0), -.25), ((9, 0, 0), -.125), ((11, 0, 0), .125)]}
        for shape, storage in itertools.product(samples, ("float", "double")):
            with self.subTest(shape=shape, storage=storage):
                name = f"{shape}-{storage}.nvdb"
                result = self.primitive(shape, name, "--nano-storage", storage, "--raw")
                self.validate(result)
                raw = self.raw(name)
                self.assertEqual(raw.grid_class, 1)
                self.assertEqual(raw.kind, 1 if storage == "float" else 2)
                self.assertEqual(raw.background, .375)
                self.assertEqual(raw.voxel_size, (.125,) * 3)
                for point, expected in samples[shape]:
                    actual, active = raw.sample_leaf(point)
                    self.assertTrue(active)
                    self.assertAlmostEqual(actual, expected, delta=2e-7 if storage == "float" else 5e-14)

    def test_fog_zero_root_background_and_independent_interior_ramp(self):
        self.native_only()
        for shape, storage in itertools.product(("sphere", "box", "torus"), ("float", "double")):
            with self.subTest(shape=shape, storage=storage):
                name = f"fog-{shape}-{storage}.nvdb"
                result = self.primitive(shape, name, "--nano-class", "fog", "--nano-storage", storage, "--raw")
                self.validate(result)
                raw = self.raw(name)
                self.assertEqual(raw.grid_class, 2)
                self.assertEqual(raw.background, 0)
                point = (7, 0, 0) if shape != "torus" else (9, 0, 0)
                value, active = raw.sample_leaf(point)
                self.assertTrue(active)
                self.assertAlmostEqual(value, 1 / 3, delta=2e-7 if storage == "float" else 5e-14)

    def test_float_map_cache_optin_is_separate_from_float_value_rounding(self):
        self.native_only()
        self.failure("--nano-primitive", "sphere", "bad.nvdb", "--voxel-size", ".1")
        result = self.primitive("sphere", "map.nvdb", "--voxel-size", ".1", "--allow-float-map-rounding")
        self.validate(result)
        self.assertTrue(result["floatMapCachesRounded"])
        self.assertGreater(result["maximumFloatMapCacheErrorVoxels"], 0)
        self.assertLessEqual(result["maximumFloatMapCacheErrorVoxels"], 1e-4)
        self.assertGreater(result["roundedFloatValues"], 0)
        double = self.primitive("sphere", "double.nvdb", "--nano-storage", "double")
        self.assertEqual(double["roundedFloatValues"], 0)
        self.assertFalse(double["floatMapCachesRounded"])

    def test_nondefault_shape_origin_and_world_scale_are_native(self):
        self.native_only()
        result = self.primitive("box", "box.nvdb", "--nano-storage", "double", "--raw",
                                "--voxel-size", ".25", "--origin", "4", "-2", "1",
                                "--center", "4.5", "-2", "1", "--box-dimensions", "2", "1", "3")
        self.validate(result)
        raw = self.raw("box.nvdb")
        self.assertEqual(raw.voxel_size, (.25,) * 3)
        self.assertEqual(raw.sample_leaf((7, 0, 0))[0], .25)
        self.primitive("torus", "torus.nvdb", "--major-radius", "1.5", "--minor-radius", ".5")
        self.failure("--nano-primitive", "sphere", "bad.nvdb", "--radius", ".1")
        self.failure("--nano-primitive", "torus", "bad.nvdb", "--major-radius", ".2")

    def test_checksum_statistics_and_available_codecs_remain_explicit(self):
        self.native_only()
        caps = self.report("--nano-capabilities")
        for mode, statistics in zip(("disabled", "header", "full"), ("bounds", "minmax", "all")):
            result = self.primitive("sphere", f"{mode}.nvdb", "--checksum", mode, "--native-statistics", statistics)
            self.validate(result)
            self.assertEqual(result["file"]["grids"][0]["checksum"], mode)
        for codec in ("none", "zip", "blosc"):
            if codec != "none" and not caps[codec]:
                self.failure("--nano-primitive", "sphere", "bad.nvdb", "--codec", codec)
                continue
            result = self.primitive("box", f"{codec}.nvdb", "--codec", codec)
            self.assertEqual(result["file"]["grids"][0]["codec"], codec)
            self.report("--validate-nano", f"{codec}.nvdb", "--require-full-checksum")

    def test_shared_caps_report_reservation_and_uint64_remain_bounded(self):
        self.native_only()
        for flag in ("--max-input-bytes", "--max-native-bytes", "--max-workspace-bytes",
                     "--max-active-voxels", "--max-stored-voxels", "--max-nodes",
                     "--max-nano-work", "--max-name-bytes"):
            with self.subTest(flag=flag):
                self.failure("--nano-primitive", "sphere", "bad.nvdb", flag, "1")
        result = self.primitive("sphere", "uint64.nvdb", "--max-active-voxels", str(2**64 - 1))
        self.validate(result)
        self.assertEqual(result["settings"]["requestedLimits"]["maxActiveVoxels"], 2**64 - 1)
        # Keep destination byte length identical: report capacity is honestly
        # charged from caller text as well as native output work.
        bounded = self.primitive("sphere", "bound0.nvdb", "--max-workspace-bytes",
                                 result["combinedWorkspaceUpperBound"], "--max-nano-work", result["combinedWorkUpperBound"])
        self.validate(bounded)

    def test_unicode_long_names_and_existing_outputs_are_transactional(self):
        self.native_only()
        parent = self.directory / "体積-é"
        parent.mkdir()
        output = parent / "球.nvdb"
        result = self.primitive("sphere", output, "--grid-name", "λ-球")
        self.validate(result)
        self.assertEqual(result["settings"]["output"], str(output))
        old = output.read_bytes()
        self.call("--nano-primitive", "box", output, succeeds=False)
        self.assertEqual(output.read_bytes(), old)
        long = self.primitive("sphere", "long.nvdb", "--grid-name", "x" * 256)
        self.validate(long)
        self.assertEqual(self.report("--inspect-nano", "long.nvdb")["grids"][0]["name"], "x" * 256)
        self.primitive("sphere", "./-h")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--tool", required=True, type=Path)
    parser.add_argument("--native-enabled", required=True, choices=("0", "1"))
    parser.add_argument("--enabled", choices=("0", "1"))
    args, remaining = parser.parse_known_args()
    NanoPrimitivesCli.tool = args.tool.resolve(strict=True)
    NanoPrimitivesCli.native = args.native_enabled == "1"
    capabilities = subprocess.run([str(NanoPrimitivesCli.tool), "--nano-capabilities"],
                                  capture_output=True, text=True, encoding="utf-8", check=True, timeout=30)
    NanoPrimitivesCli.conversion = json.loads(capabilities.stdout)["openVdbConversion"]
    if args.enabled is not None and NanoPrimitivesCli.conversion != (NanoPrimitivesCli.native and args.enabled == "1"):
        parser.error("runtime VDB conversion capability differs from requested configuration")
    unittest.main(argv=[__file__, *remaining])
