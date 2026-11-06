"""Real-tool typed closed-contour rebuild, bands, maps and failure workflows."""
import argparse
from array import array
import json
import math
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "python"))
from container_openvdb.dense import DenseType, DenseVolume, GridClass, read_dense_array, write_dense_array


class LevelSetRebuildCli(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="container-rebuild-cli-")
        self.directory = Path(self.temporary.name)
        self.sample_serial = 0
        self.originals = {}

    def tearDown(self):
        try:
            for source, data in self.originals.items():
                self.assertEqual(source.read_bytes(), data, source)
            self.assertFalse(list(self.directory.glob(".container-*")))
        finally:
            self.temporary.cleanup()

    def call(self, *args, succeeds=True):
        result = subprocess.run([str(self.tool), *map(str, args)], cwd=self.directory,
                                capture_output=True, text=True, encoding="utf-8", timeout=90)
        self.assertEqual(result.returncode == 0, succeeds,
                         f"{args!r}\n{result.stdout}\n{result.stderr}")
        return result

    def report(self, *args):
        return json.loads(self.call(*args).stdout)

    def enabled_only(self):
        if not self.enabled:
            self.skipTest("OpenVDB is disabled")

    def protect(self, name):
        source = self.directory / name
        self.originals[source] = source.read_bytes()

    def sphere(self, name="source.vdb", grid="phi"):
        self.call("--sphere", name, "--radius", .6, "--voxel-size", .1, "--grid-name", grid)
        self.protect(name)

    def dense(self, name, function, *, background, h=1., grid_class=GridClass.UNKNOWN,
              half=4, dtype=DenseType.FLOAT64):
        values = array(dtype.typecode, (function(x, y, z)
            for z in range(-half, half+1) for y in range(-half, half+1) for x in range(-half, half+1)))
        volume = DenseVolume(dtype, (2*half+1,)*3, values, bytearray([1])*len(values),
            origin=(-half,)*3, linear=(h, 0., 0., 0., h, 0., 0., 0., h),
            background=array(dtype.typecode, [background]), grid_name="field", grid_class=grid_class)
        encoded = self.directory / (name + ".cvarr")
        write_dense_array(encoded, volume)
        self.call("--dense-import", encoded, name)
        self.protect(name)

    def index_value(self, source, index, grid="rebuilt"):
        self.sample_serial += 1
        encoded = self.directory / f"sample-{self.sample_serial}.cvarr"
        self.call("--dense-export", source, encoded, "--input-grid", grid,
                  "--dense-origin", *index, "--dense-dimensions", 1, 1, 1)
        return read_dense_array(encoded).values[0]

    def inspect(self, source, *flags):
        return self.report("--inspect", source, *flags)["grids"]

    def test_help_strict_operands_duplicates_irrelevant_options_and_matrix_arity(self):
        text = self.call("--help").stdout
        for option in ("--rebuild-level-set", "--rebuild-isovalue", "--rebuild-exterior-width",
                       "--rebuild-interior-width", "--rebuild-target-transform",
                       "--max-rebuild-work", "--max-rebuild-grids", "--max-rebuild-metadata"):
            self.assertIn(option, text)
        identity = (1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1)
        cases = [("--rebuild-level-set",), ("--rebuild-level-set", "missing"),
                 ("--rebuild-level-set", "-h", "bad.vdb"),
                 ("--rebuild-level-set", "missing", "-h"),
                 ("--rebuild-level-set", "", "bad.vdb"),
                 ("--rebuild-level-set", "missing", ""),
                 ("--rebuild-level-set", "--sphere", "bad.vdb"),
                 ("--rebuild-level-set", "missing", "--grid-name", "bad.vdb")]
        invalid = [("--rebuild-isovalue", "nan"), ("--rebuild-isovalue", "inf"),
                   ("--rebuild-isovalue", ""), ("--rebuild-isovalue", "-h"),
                   ("--rebuild-exterior-width", 0), ("--rebuild-exterior-width", 65),
                   ("--rebuild-interior-width", .5), ("--rebuild-interior-width", "nan"),
                   ("--rebuild-isovalue", 0, "--rebuild-isovalue", 1),
                   ("--grid", "a", "--input-grid", "b"),
                   ("--input-grid", "a", "--grid", "b"),
                   ("--grid-name", "a", "--grid-name", "b"),
                   ("--grid", "--grid-name", "a"), ("--grid-name", ""),
                   ("--max-rebuild-work", 0), ("--max-rebuild-work", " -1"),
                   ("--max-rebuild-grids", 4097), ("--max-rebuild-grids", "+2"),
                   ("--max-rebuild-metadata", 0), ("--max-input-bytes", 0),
                   ("--max-triangles", 0), ("--max-voxels", 0),
                   ("--half-width", 3), ("--voxel-size", .1), ("--radius", 1),
                   ("--rebuild-target-transform", *identity[:15]),
                   ("--rebuild-target-transform", *identity, 7),
                   ("--rebuild-target-transform", *identity, "--rebuild-target-transform", *identity),
                   ("--rebuild-target-transform", *identity[:15], 0),
                   ("--rebuild-target-transform", "nan", *identity[1:])]
        cases.extend(("--rebuild-level-set", "missing", "bad.vdb", *flags) for flags in invalid)
        for args in cases:
            with self.subTest(args=args):
                self.call(*args, succeeds=False)
                self.assertFalse((self.directory / "bad.vdb").exists())
        self.assertEqual(list(self.directory.iterdir()), [])

    def test_disabled_reports_availability_without_source_or_output_access(self):
        if self.enabled:
            self.skipTest("OpenVDB is enabled")
        result = self.call("--rebuild-level-set", "不存在", "bad.vdb", succeeds=False)
        self.assertIn("ENABLE_OPENVDB=ON", result.stderr)
        self.assertEqual(list(self.directory.iterdir()), [])

    def test_sphere_asymmetric_bands_native_type_report_and_world_distances(self):
        self.enabled_only(); self.sphere()
        report = self.report("--rebuild-level-set", "source.vdb", "rebuilt.vdb", "--input-grid", "phi",
                             "--grid-name", "rebuilt", "--rebuild-exterior-width", 5,
                             "--rebuild-interior-width", 2)
        self.assertEqual(report["schemaVersion"], 1)
        self.assertEqual(report["operation"], "rebuild-level-set")
        self.assertEqual(report["sourceValueType"], "float")
        self.assertEqual(report["valueType"], "float")
        self.assertEqual(report["gridClass"], "level set")
        self.assertEqual(report["effectiveExteriorWidth"], 5.)
        self.assertEqual(report["effectiveInteriorWidth"], 2.)
        for name in ("activeVoxelCount", "inputActiveVoxelCount", "meshTriangleUpperBound", "workUpperBound"):
            self.assertIs(type(report[name]), int); self.assertGreater(report[name], 0)
        for name in ("sourceIndexOrigin", "targetIndexOrigin"):
            self.assertEqual(len(report[name]), 3); self.assertTrue(all(type(x) is int for x in report[name]))
        self.assertIsNone(report["targetIndexToWorld"])
        self.assertTrue(report["diagnostics"])
        self.assertTrue(any("approximate" in text for text in report["diagnostics"]))
        grid = self.inspect("rebuilt.vdb", "--stats")[0]
        self.assertEqual(grid["valueType"], "float")
        self.assertAlmostEqual(grid["statistics"]["background"], .5, delta=1e-6)
        self.assertAlmostEqual(self.index_value("rebuilt.vdb", (0, 0, 0)), -.2, delta=1e-6)
        self.assertAlmostEqual(self.index_value("rebuilt.vdb", (7, 0, 0)), .1, delta=.03)

    def test_unknown_double_high_isovalue_and_planar_face_oracle(self):
        self.enabled_only()
        iso = float(2**53)
        self.dense("scalar.vdb", lambda x, y, z: iso + 2*max(abs(x), abs(y), abs(z))-4,
                   background=iso+4)
        result = self.report("--rebuild-level-set", "scalar.vdb", "double.vdb", "--rebuild-isovalue", iso,
                             "--grid", "field", "--grid-name", "rebuilt")
        self.assertEqual(result["sourceValueType"], "double"); self.assertEqual(result["isovalue"], iso)
        self.assertEqual(self.inspect("double.vdb")[0]["valueType"], "double")
        self.assertLess(self.index_value("double.vdb", (0, 0, 0)), 0)
        self.assertAlmostEqual(self.index_value("double.vdb", (3, 0, 0)), 1., delta=.3)

    def test_fog_density_refinement_float_rounded_bands_and_exact_column_target_map(self):
        self.enabled_only()
        self.dense("density.vdb", lambda x, y, z: max(0., 1-math.hypot(x, y, z)/8),
                   background=0., h=.1, half=8, grid_class=GridClass.FOG_VOLUME)
        c, s, h = math.cos(.3)*.05, math.sin(.3)*.05, .05
        target = (c, -s, 0, 10.125, s, c, 0, -2, 0, 0, -h, 3, 0, 0, 0, 1)
        report = self.report("--rebuild-level-set", "density.vdb", "mapped.vdb", "--rebuild-isovalue", .5,
                             "--rebuild-target-transform", *target, "--rebuild-exterior-width", 4.25,
                             "--rebuild-interior-width", 3.5, "--grid-name", "rebuilt")
        self.assertEqual(report["valueType"], "double")
        self.assertEqual(report["targetIndexToWorld"], list(target))
        grid = self.inspect("mapped.vdb", "--stats")[0]
        self.assertEqual(grid["indexToWorld"], list(target))
        self.assertAlmostEqual(grid["statistics"]["background"], .2125, delta=1e-12)
        self.assertLess(self.index_value("mapped.vdb", tuple(report["targetIndexOrigin"])), 0)
        target_simple = (.05, 0, 0, 0, 0, .05, 0, 0, 0, 0, .05, 0, 0, 0, 0, 1)
        rounded = self.report("--rebuild-level-set", "density.vdb", "rounded.vdb", "--rebuild-isovalue", .5,
                              "--rebuild-target-transform", *target_simple, "--rebuild-exterior-width", 4.1,
                              "--grid-name", "rebuilt")
        self.assertEqual(rounded["effectiveExteriorWidth"], array("f", [4.1])[0])
        self.assertAlmostEqual(self.index_value("rounded.vdb", (10, 0, 0)), .1, delta=.03)

    def test_unicode_literal_dash_h_paths_exclusive_outputs_metadata_and_caps(self):
        self.enabled_only(); self.sphere(grid="符号")
        parent = self.directory / "再建-é"; parent.mkdir()
        source, output = parent / "入力.vdb", parent / "曲面.vdb"
        source.write_bytes((self.directory / "source.vdb").read_bytes()); original = source.read_bytes()
        report = self.report("--rebuild-level-set", source, output, "--grid", "符号", "--grid-name", "再構築")
        self.assertEqual(report["gridName"], "再構築"); self.assertEqual(report["output"], str(output))
        self.assertEqual(self.inspect(output)[0]["name"], "再構築")
        preserved = output.read_bytes()
        self.call("--rebuild-level-set", source, output, succeeds=False); self.assertEqual(output.read_bytes(), preserved)
        self.call("--rebuild-level-set", source, source, succeeds=False); self.assertEqual(source.read_bytes(), original)
        (self.directory / "-h").write_bytes(original)
        self.call("--rebuild-level-set", "./-h", "literal.vdb")
        for flags in (("--max-rebuild-work", 1), ("--max-rebuild-metadata", 1),
                      ("--max-input-bytes", 1024), ("--max-voxels", 512),
                      ("--max-triangles", 1), ("--grid", "missing")):
            self.call("--rebuild-level-set", "source.vdb", "bad.vdb", *flags, succeeds=False)
            self.assertFalse((self.directory / "bad.vdb").exists())

    def test_open_planes_empty_contours_invalid_maps_and_types_never_publish(self):
        self.enabled_only()
        self.dense("open.vdb", lambda x, y, z: float(x), background=4.)
        self.call("--rebuild-level-set", "open.vdb", "bad.vdb", succeeds=False)
        self.dense("empty.vdb", lambda x, y, z: 1., background=1.)
        self.call("--rebuild-level-set", "empty.vdb", "bad.vdb", succeeds=False)
        self.sphere()
        for target in ((.1, 0, 0, 0, 0, .2, 0, 0, 0, 0, .1, 0, 0, 0, 0, 1),
                       (.1, .02, 0, 0, 0, .1, 0, 0, 0, 0, .1, 0, 0, 0, 0, 1)):
            self.call("--rebuild-level-set", "source.vdb", "bad.vdb", "--rebuild-target-transform", *target,
                      succeeds=False)
        self.call("--velocity", "vector.vdb", "--vector", 1, 2, 3)
        self.call("--rebuild-level-set", "vector.vdb", "bad.vdb", succeeds=False)
        self.assertFalse((self.directory / "bad.vdb").exists())


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--tool", required=True, type=Path)
    parser.add_argument("--enabled", required=True, choices=("0", "1"))
    arguments, remaining = parser.parse_known_args()
    LevelSetRebuildCli.tool = arguments.tool.resolve(strict=True)
    LevelSetRebuildCli.enabled = arguments.enabled == "1"
    unittest.main(argv=[__file__, *remaining])
