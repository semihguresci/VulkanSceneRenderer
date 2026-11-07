"""Real-tool region masks, approximate surface queries and fracture workflows."""
import argparse
import json
import math
from pathlib import Path
import subprocess
import tempfile
import unittest


class RegionsSurfaceFractureCli(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="container-regions-surface-cli-")
        self.directory = Path(self.temporary.name)

    def tearDown(self):
        self.temporary.cleanup()

    def call(self, *args, succeeds=True):
        r = subprocess.run([str(self.tool), *map(str, args)], cwd=self.directory,
                           capture_output=True, text=True, encoding="utf-8", timeout=60)
        self.assertEqual(r.returncode == 0, succeeds, f"{args!r}\n{r.stdout}\n{r.stderr}")
        return r

    def report(self, *args):
        return json.loads(self.call(*args).stdout)

    def enabled_only(self):
        if not self.enabled:
            self.skipTest("OpenVDB is disabled")

    def test_help_strict_options_and_arity(self):
        help_text = self.call("--help").stdout
        for option in ("--regions", "--surface-query", "--fracture"):
            self.assertIn(option, help_text)
        cases = [("--regions",), ("--surface-query",), ("--fracture", "a", "b"),
                 ("--regions", "missing", "bad.vdb"),
                 ("--regions", "missing", "bad.vdb", "--region-operation", "unknown"),
                 ("--regions", "missing", "bad.vdb", "--region-operation", "components", "--region-isovalue", 0),
                 ("--regions", "missing", "bad.vdb", "--region-operation", "interior", "--max-region-components", 64),
                 ("--regions", "missing", "bad.vdb", "--region-operation", "components", "--max-region-components", 0),
                 ("--surface-query", "missing", "--position", 1, 2),
                 ("--surface-query", "missing", "--position", "nan", 0, 0),
                 ("--surface-query", "missing", "--grid", "a", "--grid", "b"),
                 ("--surface-query", "missing", "--voxel-size", .1),
                 ("--fracture", "missing", "cutter", "bad.vdb", "--fracture-pivot", 0, 0, 0),
                 ("--fracture", "missing", "cutter", "bad.vdb", "--fracture-instance", 0, 0, 0),
                 ("--fracture", "missing", "cutter", "bad.vdb", "--max-fracture-pieces", 257)]
        for args in cases:
            with self.subTest(args=args):
                self.call(*args, succeeds=False)
                self.assertFalse((self.directory / "bad.vdb").exists())
        self.assertEqual(list(self.directory.iterdir()), [])
        # Missing operands must not turn the help flag into a new filename.
        for args in (("--regions", "missing", "-h", "--region-operation", "interior"),
                     ("--regions", "", "new.vdb", "--region-operation", "interior"),
                     ("--surface-query", "-h"), ("--surface-query", ""),
                     ("--fracture", "missing", "cutter", "-h"),
                     ("--fracture", "missing", "", "new.vdb")):
            result = self.call(*args, succeeds=False)
            self.assertIn("requires INPUT", result.stderr)
        self.assertEqual(list(self.directory.iterdir()), [])

    def test_disabled_workflows_fail_before_files(self):
        if self.enabled:
            self.skipTest("OpenVDB is enabled")
        for args in (("--regions", "missing", "bad.vdb", "--region-operation", "components"),
                     ("--surface-query", "missing", "--position", 0, 0, 0),
                     ("--fracture", "missing", "cutter", "bad.vdb")):
            self.assertIn("ENABLE_OPENVDB=ON", self.call(*args, succeeds=False).stderr)
        self.assertEqual(list(self.directory.iterdir()), [])

    def test_region_masks_have_analytic_interior_and_no_sphere_cavity(self):
        self.enabled_only()
        self.call("--sphere", "source.vdb", "--voxel-size", .25)
        source = (self.directory / "source.vdb").read_bytes()
        interior = self.report("--regions", "source.vdb", "interior.vdb", "--region-operation", "interior")
        expected = sum(x*x+y*y+z*z < 16 for z in range(-4, 5) for y in range(-4, 5) for x in range(-4, 5))
        self.assertEqual(interior["totalActiveVoxelCount"], expected)
        cavity = self.report("--regions", "source.vdb", "cavity.vdb", "--region-operation", "cavities")
        self.assertEqual(cavity["totalActiveVoxelCount"], 0)
        components = self.report("--regions", "interior.vdb", "components.vdb", "--region-operation", "components",
                                 "--grid-name", "occupied")
        self.assertEqual(len(components["grids"]), 1)
        self.assertEqual(components["grids"][0]["name"], "occupied_000")
        self.assertEqual(components["totalActiveVoxelCount"], expected)
        enclosed = self.report("--regions", "source.vdb", "enclosed.vdb", "--region-operation", "enclosed")
        self.assertEqual(enclosed["totalActiveVoxelCount"], expected)
        self.assertEqual(source, (self.directory / "source.vdb").read_bytes())
        before = (self.directory / "interior.vdb").read_bytes()
        self.call("--regions", "source.vdb", "interior.vdb", "--region-operation", "interior", succeeds=False)
        self.assertEqual(before, (self.directory / "interior.vdb").read_bytes())

    def test_surface_query_point_cloud_contract_and_validity_flags(self):
        self.enabled_only()
        self.call("--sphere", "source.vdb", "--voxel-size", .1)
        source = (self.directory / "source.vdb").read_bytes()
        r = self.report("--surface-query", "source.vdb", "--position", 1.2, 0, 0, "--position", 3, 0, 0)
        self.assertIn("POINT CLOUD", r["diagnostic"])
        self.assertEqual(len(r["queries"]), 2)
        near, far = r["queries"]
        self.assertAlmostEqual(near["distance"], .2, delta=.03)
        self.assertTrue(near["querySdfValid"]); self.assertAlmostEqual(near["querySdf"], .2, delta=.02)
        self.assertTrue(near["geometryValid"])
        self.assertGreater(near["normal"][0], .95)
        self.assertAlmostEqual(near["meanCurvature"], 1, delta=.2)
        self.assertAlmostEqual(near["gaussianCurvature"], 1, delta=.4)
        self.assertFalse(far["querySdfValid"]); self.assertIsNone(far["querySdf"])
        self.assertAlmostEqual(far["distance"], 2, delta=.03)
        self.assertTrue(all(math.isfinite(x) for q in r["queries"] for x in q["closestPosition"]))
        empty = self.report("--surface-query", "source.vdb")
        self.assertEqual(empty["queries"], [])
        self.assertEqual(source, (self.directory / "source.vdb").read_bytes())
        self.call("--surface-query", "source.vdb", "--position", 0, 0, 0, "--max-surface-work", 1, succeeds=False)

    def test_fracture_partition_accounting_instances_and_unicode_output(self):
        self.enabled_only()
        self.call("--sphere", "source.vdb", "--voxel-size", .25)
        self.call("--sphere", "cutter.vdb", "--voxel-size", .25, "--radius", .6, "--center", .5, 0, 0)
        source = (self.directory / "source.vdb").read_bytes()
        cutter = (self.directory / "cutter.vdb").read_bytes()
        parent = self.directory / "破砕-é"; parent.mkdir()
        output = parent / "pieces.vdb"
        r = self.report("--fracture", "source.vdb", "cutter.vdb", output, "--max-fracture-pieces", 8,
                        "--fracture-instance", .25, 0, 0, 0, 0, .1, "--fracture-pivot", 0, 0, 0)
        self.assertGreaterEqual(len(r["pieces"]), 2)
        self.assertTrue(any(p["residual"] for p in r["pieces"]))
        self.assertTrue(any(not p["residual"] for p in r["pieces"]))
        self.assertEqual(r["inputNegativeVoxelCount"], r["outputNegativeVoxelCount"] + r["cutBoundaryVoxelCount"])
        self.assertEqual(sum(p["negativeVoxelCount"] for p in r["pieces"]), r["outputNegativeVoxelCount"])
        before = output.read_bytes()
        self.call("--fracture", "source.vdb", "cutter.vdb", output, succeeds=False)
        self.assertEqual(before, output.read_bytes())
        for flags in (("--max-fracture-work", 1), ("--max-voxels", 1), ("--max-input-bytes", 1024)):
            self.call("--fracture", "source.vdb", "cutter.vdb", "bad.vdb", *flags, succeeds=False)
            self.assertFalse((self.directory / "bad.vdb").exists())
        self.assertEqual(source, (self.directory / "source.vdb").read_bytes())
        self.assertEqual(cutter, (self.directory / "cutter.vdb").read_bytes())


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--tool", required=True, type=Path)
    parser.add_argument("--enabled", required=True, choices=("0", "1"))
    args, remaining = parser.parse_known_args()
    RegionsSurfaceFractureCli.tool = args.tool.resolve(strict=True)
    RegionsSurfaceFractureCli.enabled = args.enabled == "1"
    unittest.main(argv=[__file__, *remaining])
