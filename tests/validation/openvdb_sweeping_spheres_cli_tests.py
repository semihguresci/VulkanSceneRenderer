"""Real-tool native sphere fitting and fast-sweeping workflows."""
from array import array
import argparse
import json
import math
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "python"))
from container_openvdb import DenseType, DenseVolume, write_dense_array, read_dense_array


class SweepingSpheresCli(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="container-sweep-spheres-cli-")
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

    def test_help_arity_duplicates_and_irrelevant_options(self):
        help_text = self.call("--help").stdout
        for option in ("--volume-spheres", "--fast-sweep", "--fast-extend", "--sweep-mask", "--sphere-candidates"):
            self.assertIn(option, help_text)
        cases = [("--volume-spheres",), ("--fast-sweep", "input"), ("--fast-extend", "input", "field"),
                 ("--volume-spheres", "missing", "bad.json", "--sphere-min-count", "2", "--sphere-max-count", "1"),
                 ("--volume-spheres", "missing", "bad.json", "--sphere-candidates", "0"),
                 ("--volume-spheres", "missing", "bad.json", "--sphere-max-radius", "nan"),
                 ("--volume-spheres", "missing", "bad.json", "--grid", "a", "--grid", "b"),
                 ("--volume-spheres", "missing", "bad.json", "--voxel-size", ".25"),
                 ("--fast-sweep", "missing", "bad.vdb"),
                 ("--fast-sweep", "missing", "bad.vdb", "--sweep-operation", "bad"),
                 ("--fast-sweep", "missing", "bad.vdb", "--sweep-operation", "reinitialize", "--sweep-dilation", "1"),
                 ("--fast-sweep", "missing", "bad.vdb", "--sweep-operation", "dilate", "--sweep-isovalue", "0"),
                 ("--fast-sweep", "missing", "bad.vdb", "--sweep-operation", "mask"),
                 ("--fast-sweep", "missing", "bad.vdb", "--sweep-operation", "topology", "--sweep-mask", "mask"),
                 ("--fast-sweep", "missing", "bad.vdb", "--sweep-operation", "fog", "--sweep-side", "all"),
                 ("--fast-sweep", "missing", "bad.vdb", "--sweep-operation", "fog", "--sweep-mask-grid", "mask"),
                 ("--fast-extend", "missing", "field", "bad.vdb", "--sweep-operation", "fog"),
                 ("--fast-extend", "missing", "field", "bad.vdb", "--topology-closing", "0"),
                 ("--fast-extend", "missing", "field", "bad.vdb", "--sweep-iterations", "0")]
        for args in cases:
            with self.subTest(args=args):
                self.call(*args, succeeds=False)
                self.assertFalse((self.directory / "bad.vdb").exists())
                self.assertFalse((self.directory / "bad.json").exists())
        self.assertEqual(list(self.directory.iterdir()), [])

    def test_disabled_workflows_report_availability(self):
        if self.enabled:
            self.skipTest("OpenVDB is enabled")
        for args in (("--volume-spheres", "missing", "bad.json"),
                     ("--fast-sweep", "missing", "bad.vdb", "--sweep-operation", "reinitialize"),
                     ("--fast-extend", "missing", "field", "bad.vdb")):
            self.assertIn("ENABLE_OPENVDB=ON", self.call(*args, succeeds=False).stderr)
        self.assertEqual(list(self.directory.iterdir()), [])

    def test_spheres_are_deterministic_physical_json_and_exclusive(self):
        self.enabled_only()
        self.call("--sphere", "source.vdb", "--voxel-size", .25)
        source = (self.directory / "source.vdb").read_bytes()
        parent = self.directory / "球-é"; parent.mkdir()
        input_path = parent / "入力.vdb"; input_path.write_bytes(source)
        outputs = [parent / "spheres.json", parent / "repeat.json"]
        documents = []
        for output in outputs:
            r = self.report("--volume-spheres", input_path, output, "--sphere-max-count", 8,
                            "--sphere-candidates", 128, "--sphere-max-radius", .2)
            document = json.loads(output.read_text(encoding="utf-8")); documents.append(document)
            self.assertEqual(r["sphereCount"], len(document["spheres"]))
            self.assertEqual(document["schemaVersion"], 1)
            self.assertEqual(document["source"]["gridName"], "surface")
            self.assertGreater(len(document["spheres"]), 0)
            self.assertLessEqual(len(document["spheres"]), 8)
            for sphere in document["spheres"]:
                self.assertGreater(sphere["radius"], 0)
                self.assertLessEqual(sphere["radius"], .2)
                self.assertLess(math.hypot(*sphere["position"]) + sphere["radius"], 1.5)
        self.assertEqual(documents[0], documents[1])
        before = outputs[0].read_bytes()
        self.call("--volume-spheres", input_path, outputs[0], succeeds=False)
        self.assertEqual(before, outputs[0].read_bytes())
        self.assertEqual(source, input_path.read_bytes())

    def test_sphere_fog_and_explicit_resource_or_minimum_failure(self):
        self.enabled_only()
        self.call("--sphere", "source.vdb", "--voxel-size", .25)
        self.call("--fog", "source.vdb", "fog.vdb")
        r = self.report("--volume-spheres", "fog.vdb", "fog.json", "--sphere-candidates", 128)
        self.assertEqual(r["isovalue"], .5); self.assertGreater(r["sphereCount"], 0)
        for flags in (("--max-sphere-work", 1), ("--max-input-bytes", 1024), ("--max-voxels", 1),
                      ("--sphere-min-radius", 10, "--sphere-max-radius", 11, "--sphere-min-count", 1)):
            self.call("--volume-spheres", "source.vdb", "bad.json", "--sphere-candidates", 128, *flags, succeeds=False)
            self.assertFalse((self.directory / "bad.json").exists())

    def write_plane(self):
        coordinates = [(x, y, z) for z in range(-3, 4) for y in range(-3, 4) for x in range(-6, 7)]
        values = array("f", [1.4 * .25 * (x + .5) for x, y, z in coordinates])
        active = bytearray(int(-4 <= x <= 4 and -1 <= y <= 1 and -1 <= z <= 1) for x, y, z in coordinates)
        # Inactive halo values continue the plane. A positive constant directly
        # beside negative active cells would introduce additional iso crossings.
        volume = DenseVolume(DenseType.FLOAT32, (13, 7, 7), values, active, origin=(-6, -3, -3),
                            linear=(.25, 0, 0, 0, .25, 0, 0, 0, .25), background=array("f", [3.]), grid_class=1)
        write_dense_array(self.directory / "plane.cvarr", volume)
        self.call("--dense-import", "plane.cvarr", "plane.vdb")

    def read_line(self, name, grid="surface"):
        output = name + ".cvarr"
        self.call("--dense-export", name, output, "--input-grid", grid, "--dense-origin", -3, 0, 0,
                  "--dense-dimensions", 7, 1, 1)
        return read_dense_array(self.directory / output)

    def test_fast_reinitialization_has_analytic_world_distances(self):
        self.enabled_only(); self.write_plane()
        before = (self.directory / "plane.vdb").read_bytes()
        r = self.report("--fast-sweep", "plane.vdb", "distance.vdb", "--sweep-operation", "reinitialize", "--sweep-iterations", 2)
        self.assertEqual(r["activeVoxelCount"], 81); self.assertGreater(r["boundaryVoxelCount"], 0)
        self.assertGreater(r["sweepingVoxelCount"], 0)
        out = self.read_line("distance.vdb")
        for i, actual in enumerate(out.values):
            self.assertAlmostEqual(actual, (i - 3 + .5) * .25, delta=.002)
        self.assertEqual(before, (self.directory / "plane.vdb").read_bytes())
        self.call("--fast-sweep", "plane.vdb", "distance.vdb", "--sweep-operation", "reinitialize", succeeds=False)

    def test_fast_field_extension_preserves_constant_native_field(self):
        self.enabled_only(); self.write_plane()
        field = DenseVolume(DenseType.FLOAT32, (1, 1, 1), array("f", [7.]), bytearray([0]),
                            linear=(.25, 0, 0, 0, .25, 0, 0, 0, .25), background=array("f", [7.]))
        write_dense_array(self.directory / "field.cvarr", field)
        self.call("--dense-import", "field.cvarr", "field.vdb")
        r = self.report("--fast-extend", "plane.vdb", "field.vdb", "extended.vdb", "--sweep-side", "greater")
        self.assertEqual(r["extensionGrid"], "surface_extension")
        out = self.read_line("extended.vdb", "surface_extension")
        self.assertEqual(list(out.values), [7.] * 7)
        self.assertEqual(list(out.active), [1] * 7)
        for flags in (("--max-sweep-work", 1), ("--max-voxels", 1), ("--max-input-bytes", 1024)):
            self.call("--fast-extend", "plane.vdb", "field.vdb", "bad.vdb", *flags, succeeds=False)
            self.assertFalse((self.directory / "bad.vdb").exists())

    def test_fast_fog_dilation_mask_union_and_topology_routes(self):
        self.enabled_only()
        self.call("--sphere", "source.vdb", "--voxel-size", .25)
        self.call("--fog", "source.vdb", "fog.vdb")
        fog = self.report("--fast-sweep", "fog.vdb", "fog-distance.vdb", "--sweep-operation", "fog", "--sweep-voxelize-tiles")
        self.assertGreater(fog["boundaryVoxelCount"], 0)
        dilated = self.report("--fast-sweep", "source.vdb", "dilated.vdb", "--sweep-operation", "dilate",
                              "--sweep-dilation", 1, "--sweep-neighbors", "face-edge", "--sweep-side", "all")
        self.assertGreater(dilated["activeVoxelCount"], dilated["inputActiveVoxelCount"])
        mask = DenseVolume(DenseType.FLOAT32, (1, 1, 1), array("f", [0.]), bytearray([1]), origin=(7, 0, 0),
                           linear=(.25, 0, 0, 0, .25, 0, 0, 0, .25), background=array("f", [0.]))
        write_dense_array(self.directory / "mask.cvarr", mask)
        self.call("--dense-import", "mask.cvarr", "mask.vdb")
        extended = self.report("--fast-sweep", "source.vdb", "masked.vdb", "--sweep-operation", "mask",
                               "--sweep-mask", "mask.vdb", "--sweep-mask-grid", "field")
        self.assertEqual(extended["activeVoxelCount"], extended["inputActiveVoxelCount"] + 1)
        occupied = DenseVolume(DenseType.FLOAT32, (3, 3, 3), array("f", [0.] * 27), bytearray([1] * 27), origin=(-1, -1, -1))
        write_dense_array(self.directory / "occupied.cvarr", occupied)
        self.call("--dense-import", "occupied.cvarr", "occupied.vdb")
        topology = self.report("--fast-sweep", "occupied.vdb", "topology.vdb", "--sweep-operation", "topology",
                               "--topology-half-width", 3, "--topology-closing", 0,
                               "--topology-dilation", 0, "--topology-smoothing", 0)
        self.assertGreater(topology["activeVoxelCount"], 27)
        self.call("--dense-export", "topology.vdb", "topology.cvarr", "--dense-origin", 0, 0, 0,
                  "--dense-dimensions", 1, 1, 1)
        self.assertLess(read_dense_array(self.directory / "topology.cvarr").values[0], 0)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--tool", required=True, type=Path)
    parser.add_argument("--enabled", required=True, choices=("0", "1"))
    args, remaining = parser.parse_known_args()
    SweepingSpheresCli.tool = args.tool.resolve(strict=True)
    SweepingSpheresCli.enabled = args.enabled == "1"
    unittest.main(argv=[__file__, *remaining])
