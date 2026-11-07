"""Exercise the installed OpenVDB tool's parsing and complete file workflows."""
import argparse
from array import array
import json
import math
import pathlib
import subprocess
import sys
import tempfile
import unittest

sys.path.insert(0,str(pathlib.Path(__file__).resolve().parents[2]/"python"))
from container_openvdb import DenseType, DenseVolume, write_dense_array


class OpenVdbToolCli(unittest.TestCase):
    def run_tool(self, *arguments, succeeds=True):
        result = subprocess.run(
            [str(self.tool), *map(str, arguments)], cwd=self.directory,
            text=True, capture_output=True, timeout=60,
        )
        if succeeds:
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        else:
            self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
        return result

    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="container-vdb-cli-")
        self.directory = pathlib.Path(self.temporary.name)

    def tearDown(self):
        self.temporary.cleanup()

    def test_help_and_invalid_parameters(self):
        help_text = self.run_tool("--help").stdout
        self.assertIn("--fog", help_text)
        self.assertIn("--offset", help_text)
        self.assertIn("--smooth", help_text)
        self.assertIn("--histogram-bins", help_text)
        for args in [
            ["--sphere"],
            ["--sphere", "invalid.vdb", "--radius", "nan"],
            ["--sphere", "invalid.vdb", "--voxel-size", "0"],
            ["--sphere", "invalid.vdb", "--max-voxels", "-1"],
            ["--sphere", "invalid.vdb", "--unknown"],
            ["--sample", "missing.vdb", "0", "0"],
            ["--sample", "missing.vdb", "nan", "0", "0"],
            ["--fog", "missing.vdb", "invalid.vdb", "--voxel-size", "0.2"],
            ["--offset", "missing.vdb", "invalid.vdb"],
            ["--offset", "missing.vdb", "invalid.vdb", "--distance", "nan"],
            ["--smooth", "missing.vdb", "invalid.vdb", "--filter", "invalid"],
            ["--smooth", "missing.vdb", "invalid.vdb", "--width", "0"],
            ["--smooth", "missing.vdb", "invalid.vdb", "--width", "5"],
            ["--smooth", "missing.vdb", "invalid.vdb", "--iterations", "33"],
            ["--smooth", "missing.vdb", "invalid.vdb", "--distance", "0.2"],
            ["--offset", "missing.vdb", "invalid.vdb", "--distance", "0.2", "--filter", "mean"],
            ["--sphere", "invalid.vdb", "--iterations", "2"],
            ["--inspect", "missing.vdb", "--histogram-bins", "4"],
            ["--inspect", "missing.vdb", "--stats", "--histogram-range", "0", "1"],
            ["--inspect", "missing.vdb", "--stats", "--histogram-bins", "0"],
            ["--inspect", "missing.vdb", "--stats", "--histogram-bins", "4097"],
            ["--inspect", "missing.vdb", "--stats", "--histogram-bins", "4", "--histogram-range", "1", "1"],
            ["--inspect", "missing.vdb", "--stats", "--histogram-bins", "4", "--histogram-range", "nan", "1"],
            ["--inspect", "missing.vdb", "--stats", "--histogram-bins", "4", "--histogram-bins", "8"],
            ["--inspect", "missing.vdb", "--stats", "--histogram-bins", "4", "--histogram-range", "0", "1", "--histogram-range", "0", "2"],
            ["--sphere", "invalid.vdb", "--stats", "--histogram-bins", "4"],
            ["--offset", "missing.vdb", "invalid.vdb", "--distance", "0.2", "--half-width", "4"],
        ]:
            self.run_tool(*args, succeeds=False)
        self.assertFalse((self.directory / "invalid.vdb").exists())

    def test_disabled_tool_explains_build_option(self):
        if self.enabled:
            self.skipTest("OpenVDB is enabled")
        result = self.run_tool("--sphere", "disabled.vdb", succeeds=False)
        self.assertIn("ENABLE_OPENVDB=ON", result.stderr)
        self.assertFalse((self.directory / "disabled.vdb").exists())
        for operation in (
            ["--inspect", "missing.vdb", "--stats", "--histogram-bins", "4"],
            ["--offset", "missing.vdb", "disabled.vdb", "--distance", "0.2"],
            ["--smooth", "missing.vdb", "disabled.vdb", "--filter", "median"],
        ):
            result = self.run_tool(*operation, succeeds=False)
            self.assertIn("ENABLE_OPENVDB=ON", result.stderr)
            self.assertFalse((self.directory / "disabled.vdb").exists())

    def import_dense(self,name,values,active):
        volume=DenseVolume(DenseType.FLOAT64,(len(values),1,1),array("d",values),bytearray(active))
        write_dense_array(self.directory/(name+".cvarr"),volume)
        self.run_tool("--dense-import",name+".cvarr",name+".vdb")
        return self.directory/(name+".vdb")

    def inspection(self,name,*options):
        return json.loads(self.run_tool("--inspect",name,"--stats",*options).stdout)["grids"][0]["statistics"]

    def test_inspection_moments_histograms_and_source_preservation(self):
        if not self.enabled:
            self.skipTest("OpenVDB is disabled")
        source=self.import_dense("moments",[-2,-1,0,1,2,99],[1,1,1,1,1,0])
        before=source.read_bytes()
        s=self.inspection(source,"--histogram-bins",4,"--histogram-range",-1,1)
        self.assertEqual((s["count"],s["sum"],s["mean"]),(5,0,0))
        self.assertAlmostEqual(s["populationStdDev"],math.sqrt(2))
        self.assertAlmostEqual(s["rootMeanSquare"],math.sqrt(2))
        self.assertEqual(s["histogram"],{"minimum":-1,"maximum":1,"bins":[1,0,1,1],"belowRange":1,"aboveRange":1})
        automatic=self.inspection(source,"--histogram-bins",4)["histogram"]
        self.assertEqual((automatic["minimum"],automatic["maximum"]),(-2,2))
        self.assertEqual(automatic["bins"],[1,1,1,2])
        self.assertIsNone(self.inspection(source)["histogram"])
        self.assertEqual(source.read_bytes(),before)

    def test_inspection_extreme_empty_histograms_and_report_limits(self):
        if not self.enabled:
            self.skipTest("OpenVDB is disabled")
        maximum=sys.float_info.max
        tiny=float.fromhex("0x0.0000000000001p-1022")
        source=self.import_dense("extreme",[-maximum,maximum,-tiny,tiny],[1]*4)
        s=self.inspection(source,"--histogram-bins",2,"--histogram-range",-maximum,maximum)
        self.assertEqual((s["sum"],s["mean"]),(0,0))
        self.assertTrue(math.isclose(s["populationStdDev"],maximum/math.sqrt(2),rel_tol=2e-15))
        self.assertEqual(s["histogram"]["bins"],[2,2])
        empty=self.import_dense("empty",[0],[0])
        s=self.inspection(empty,"--histogram-bins",3)
        for key in ("mean","populationStdDev","rootMeanSquare"):
            self.assertIsNone(s[key])
        self.assertEqual(s["histogram"],{"minimum":None,"maximum":None,"bins":[0,0,0],"belowRange":0,"aboveRange":0})
        before=source.read_bytes()
        self.run_tool("--inspect",source,"--stats","--histogram-bins",4096,"--max-metadata-bytes",32768,succeeds=False)
        self.assertEqual(source.read_bytes(),before)

    def test_generate_combine_convert_and_protect_outputs(self):
        if not self.enabled:
            self.skipTest("OpenVDB is disabled")
        self.run_tool("--sphere", "a.vdb", "--voxel-size", "0.2")
        self.run_tool("--sphere", "b.vdb", "--voxel-size", "0.2",
                      "--center", "0.5", "0", "0")
        for operation in ("union", "intersection", "difference"):
            self.run_tool("--" + operation, "a.vdb", "b.vdb", operation + ".vdb")
            self.assertGreater((self.directory / (operation + ".vdb")).stat().st_size, 0)
        self.run_tool("--fog", "union.vdb", "density.vdb", "--grid-name", "density")
        self.assertGreater((self.directory / "density.vdb").stat().st_size, 0)
        original = (self.directory / "a.vdb").read_bytes()
        self.run_tool("--sphere", "a.vdb", succeeds=False)
        self.assertEqual((self.directory / "a.vdb").read_bytes(), original)

    def test_mesh_file_to_sdf(self):
        if not self.enabled:
            self.skipTest("OpenVDB is disabled")
        fixture = {
            "schema_version": "1.1.0",
            "meshes": [{"mesh_id": 0,
                        "coordinates": [-1, -1, -1, 1, -1, -1, 1, 1, -1, -1, 1, -1,
                                        -1, -1, 1, 1, -1, 1, 1, 1, 1, -1, 1, 1],
                        "indices": [0, 2, 1, 0, 3, 2, 4, 5, 6, 4, 6, 7,
                                    0, 1, 5, 0, 5, 4, 3, 7, 6, 3, 6, 2,
                                    0, 4, 7, 0, 7, 3, 1, 2, 6, 1, 6, 5]}],
            "elements": [{"mesh_id": 0, "guid": "cube", "type": "cube",
                          "vector": {"x": 3, "y": 0, "z": 0},
                          "rotation": {"qx": 0, "qy": 0, "qz": 0, "qw": 1},
                          "color": {"r": 255, "g": 255, "b": 255, "a": 255}}],
        }
        (self.directory / "cube.bim").write_text(json.dumps(fixture), encoding="utf-8")
        self.run_tool("--mesh", "cube.bim", "cube.vdb", "--voxel-size", "0.2")
        self.assertGreater((self.directory / "cube.vdb").stat().st_size, 0)

    def test_edit_sdf_offsets_filters_and_output_protection(self):
        if not self.enabled:
            self.skipTest("OpenVDB is disabled")
        self.run_tool("--sphere", "source.vdb", "--voxel-size", "0.2", "--half-width", "4")
        source = (self.directory / "source.vdb").read_bytes()
        for name, distance, surface in (("expanded", "0.4", "1.4"),
                                         ("eroded", "-0.4", "0.6")):
            self.run_tool("--offset", "source.vdb", name + ".vdb", "--distance", distance,
                          "--input-grid", "surface", "--grid-name", "edited")
            sample = self.run_tool("--sample", name + ".vdb", surface, "0", "0",
                                   "--input-grid", "edited")
            self.assertAlmostEqual(float(sample.stdout.strip()), 0.0, delta=0.07)
        for filter_name in ("gaussian", "mean", "median"):
            self.run_tool("--smooth", "source.vdb", filter_name + ".vdb",
                          "--filter", filter_name, "--width", "1", "--iterations", "2")
            self.assertGreater((self.directory / (filter_name + ".vdb")).stat().st_size, 0)
        for operation in (
            ["--offset", "source.vdb", "source.vdb", "--distance", "0.2"],
            ["--smooth", "source.vdb", "source.vdb"],
        ):
            result = self.run_tool(*operation, succeeds=False)
            self.assertIn("already exists", result.stderr)
            self.assertEqual((self.directory / "source.vdb").read_bytes(), source)
        self.run_tool("--fog", "source.vdb", "fog.vdb")
        self.run_tool("--smooth", "fog.vdb", "invalid.vdb", succeeds=False)
        self.run_tool("--offset", "source.vdb", "invalid.vdb", "--distance", "100", succeeds=False)
        self.run_tool("--offset", "source.vdb", "invalid.vdb", "--distance", "0.2",
                      "--max-voxels", "10", succeeds=False)
        self.assertFalse((self.directory / "invalid.vdb").exists())

    def test_resample_and_sample_world_distances_before_csg(self):
        if not self.enabled:
            self.skipTest("OpenVDB is disabled")
        self.run_tool("--sphere", "a.vdb", "--voxel-size", "0.2")
        self.run_tool("--sphere", "b.vdb", "--voxel-size", "0.3",
                      "--half-width", "4", "--center", "0.6", "0", "0")
        self.run_tool("--union", "a.vdb", "b.vdb", "mismatched.vdb", succeeds=False)
        for name in ("a", "b"):
            self.run_tool("--resample", name + ".vdb", name + "-match.vdb",
                          "--voxel-size", "0.1", "--half-width", "3")
        exterior = self.run_tool("--sample", "a-match.vdb", "1.2", "0", "0")
        self.assertAlmostEqual(float(exterior.stdout.strip()), 0.2, delta=0.06)
        self.run_tool("--union", "a-match.vdb", "b-match.vdb", "matched.vdb")
        interior = self.run_tool("--sample", "matched.vdb", "1.4", "0", "0",
                                 "--input-grid", "surface")
        self.assertLess(float(interior.stdout.strip()), 0.0)
        self.run_tool("--fog", "matched.vdb", "fog.vdb", "--grid-name", "density")
        self.run_tool("--resample", "fog.vdb", "fog-match.vdb",
                      "--input-grid", "density", "--voxel-size", "0.15",
                      "--grid-name", "density")
        self.assertGreater(float(self.run_tool("--sample", "fog-match.vdb", "0", "0", "0",
                                              "--input-grid", "density").stdout.strip()), 0.9)


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--tool", required=True, type=pathlib.Path)
    parser.add_argument("--enabled", required=True, choices=("0", "1"))
    args = parser.parse_args()
    OpenVdbToolCli.tool = args.tool.resolve()
    OpenVdbToolCli.enabled = args.enabled == "1"
    unittest.main(argv=[__file__], verbosity=2)
