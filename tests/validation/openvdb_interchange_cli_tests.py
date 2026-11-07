"""CPU CLI workflows for VDB inspection, connected surfaces and OBJ export."""

import argparse
import json
import pathlib
import subprocess
import tempfile
import unittest


class OpenVdbInterchangeCli(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="container-vdb-interchange-")
        self.directory = pathlib.Path(self.temporary.name)

    def tearDown(self):
        self.temporary.cleanup()

    def run_tool(self, *arguments, succeeds=True):
        result = subprocess.run([str(self.tool), *map(str, arguments)], cwd=self.directory,
                                text=True, capture_output=True, timeout=60)
        if succeeds:
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        else:
            self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
        return result

    def fixture(self):
        particles = self.directory / "particles.json"
        particles.write_text(json.dumps([
            {"position": [-1, 0, 0], "radius": .4},
            {"position": [1, 0, 0], "radius": .4},
        ]), encoding="utf-8")
        self.run_tool("--particles", particles, "source.vdb", "--voxel-size", ".1")

    def inspect(self, path, *arguments):
        return json.loads(self.run_tool("--inspect", path, *arguments).stdout)

    def require_enabled(self):
        if not self.enabled:
            self.skipTest("OpenVDB is disabled")

    def test_help_and_option_compatibility(self):
        help_text = self.run_tool("--help").stdout
        for command in ("--segment", "--max-components", "--export-mesh", "--inspect", "--stats"):
            self.assertIn(command, help_text)
        invalid = [
            ["--segment"],
            ["--inspect"],
            ["--export-mesh", "missing.vdb"],
            ["--segment", "missing.vdb", "invalid.vdb", "--max-components", "0"],
            ["--segment", "missing.vdb", "invalid.vdb", "--max-components", "1025"],
            ["--inspect", "missing.vdb", "--max-grids", "0"],
            ["--inspect", "missing.vdb", "--max-grids", "4097"],
            ["--inspect", "missing.vdb", "--max-metadata-bytes", "0"],
            ["--inspect", "missing.vdb", "--grid-name", "ignored"],
            ["--inspect", "missing.vdb", "--max-triangles", "1"],
            ["--inspect", "missing.vdb", "--iso-value", "0"],
            ["--segment", "missing.vdb", "invalid.vdb", "--stats"],
            ["--export-mesh", "missing.vdb", "invalid.obj", "--adaptivity", "1.01"],
            ["--export-mesh", "missing.vdb", "invalid.obj", "--iso-value", "nan"],
            ["--export-mesh", "missing.vdb", "invalid.obj", "--grid-name", "ignored"],
            ["--export-mesh", "missing.vdb", "invalid.obj", "--voxel-size", ".2"],
            ["--sphere", "invalid.vdb", "--max-components", "2"],
            ["--sphere", "invalid.vdb", "--max-grids", "2"],
            ["--sphere", "invalid.vdb", "--stats"],
        ]
        for arguments in invalid:
            with self.subTest(arguments=arguments):
                self.run_tool(*arguments, succeeds=False)
                self.assertFalse((self.directory / "invalid.vdb").exists())
                self.assertFalse((self.directory / "invalid.obj").exists())

    def test_disabled_operations_report_build_option(self):
        if self.enabled:
            self.skipTest("OpenVDB is enabled")
        for arguments in (
            ["--segment", "missing.vdb", "disabled.vdb"],
            ["--export-mesh", "missing.vdb", "disabled.obj"],
            ["--inspect", "missing.vdb"],
            ["--inspect", "missing.vdb", "--stats"],
        ):
            result = self.run_tool(*arguments, succeeds=False)
            self.assertIn("ENABLE_OPENVDB=ON", result.stdout + result.stderr)
        self.assertFalse((self.directory / "disabled.vdb").exists())
        self.assertFalse((self.directory / "disabled.obj").exists())

    def test_inspection_is_json_with_optional_loaded_statistics(self):
        self.require_enabled()
        self.fixture()
        original = (self.directory / "source.vdb").read_bytes()
        report = self.inspect("source.vdb")
        self.assertEqual(report["schemaVersion"], 1)
        self.assertEqual(report["fileBytes"], len(original))
        self.assertEqual(len(report["grids"]), 1)
        grid = report["grids"][0]
        self.assertEqual(grid["name"], "surface")
        self.assertEqual(grid["valueType"], "float")
        self.assertEqual(grid["gridClass"], "level set")
        self.assertEqual(grid["voxelSize"], [.1, .1, .1])
        self.assertTrue(grid["affine"])
        self.assertIsNone(grid["statistics"])
        loaded = self.inspect("source.vdb", "--stats")["grids"][0]["statistics"]
        self.assertGreater(loaded["count"], 0)
        self.assertLess(loaded["minimum"], 0)
        self.assertGreater(loaded["maximum"], 0)
        self.assertGreater(loaded["background"], 0)
        self.assertGreater(loaded["actualMemoryBytes"], 0)
        self.assertEqual((self.directory / "source.vdb").read_bytes(), original)

    def test_segments_are_separate_named_surfaces_and_exportable(self):
        self.require_enabled()
        self.fixture()
        original = (self.directory / "source.vdb").read_bytes()
        self.run_tool("--segment", "source.vdb", "pieces.vdb", "--grid-name", "piece")
        report = self.inspect("pieces.vdb", "--stats")
        self.assertEqual([g["name"] for g in report["grids"]], ["piece_000", "piece_001"])
        for grid in report["grids"]:
            self.assertGreater(grid["statistics"]["count"], 0)
            self.assertEqual(grid["voxelSize"], [.1, .1, .1])
        for x, own, other in ((-1, "piece_000", "piece_001"), (1, "piece_001", "piece_000")):
            own_value = float(self.run_tool("--sample", "pieces.vdb", x, 0, 0,
                                            "--input-grid", own).stdout)
            other_value = float(self.run_tool("--sample", "pieces.vdb", x, 0, 0,
                                              "--input-grid", other).stdout)
            self.assertLess(own_value, 0)
            self.assertGreater(other_value, 0)
        self.run_tool("--export-mesh", "pieces.vdb", "pieces.obj")
        lines = (self.directory / "pieces.obj").read_text(encoding="utf-8").splitlines()
        positions = [[float(v) for v in line.split()[1:]] for line in lines if line.startswith("v ")]
        normals = [line for line in lines if line.startswith("vn ")]
        faces = [line for line in lines if line.startswith("f ")]
        self.assertTrue(positions)
        self.assertEqual(len(positions), len(normals))
        self.assertTrue(faces)
        self.assertEqual(len([line for line in lines if line.startswith("g ")]), 2)
        self.assertLess(min(p[0] for p in positions), -1.3)
        self.assertGreater(max(p[0] for p in positions), 1.3)
        for face in faces:
            self.assertEqual(len(face.split()), 4)
            for vertex in face.split()[1:]:
                index, normal = map(int, vertex.split("//"))
                self.assertTrue(1 <= index <= len(positions))
                self.assertTrue(1 <= normal <= len(normals))
        self.run_tool("--export-mesh", "pieces.vdb", "one.obj", "--input-grid", "piece_000",
                      "--iso-value", "0", "--adaptivity", ".1")
        one = (self.directory / "one.obj").read_text(encoding="utf-8").splitlines()
        self.assertEqual(sum(line.startswith("g ") for line in one), 1)
        self.assertEqual((self.directory / "source.vdb").read_bytes(), original)

    def test_vector_metadata_remains_inspectable_without_scalar_statistics(self):
        self.require_enabled()
        self.run_tool("--velocity", "flow.vdb", "--vector", "1", "2", "3")
        grid = self.inspect("flow.vdb", "--stats")["grids"][0]
        self.assertEqual(grid["name"], "velocity")
        self.assertTrue(grid["inWorldSpace"])
        self.assertIsNone(grid["statistics"])
        self.assertTrue(grid["statisticsUnavailableReason"])
        self.run_tool("--inspect", "flow.vdb", "--stats", "--input-grid", "velocity", succeeds=False)

    def test_limits_and_selection_fail_without_output(self):
        self.require_enabled()
        self.fixture()
        failures = [
            ["--segment", "source.vdb", "invalid.vdb", "--max-components", "1"],
            ["--segment", "source.vdb", "invalid.vdb", "--input-grid", "missing"],
            ["--segment", "source.vdb", "invalid.vdb", "--max-voxels", "1"],
            ["--segment", "source.vdb", "invalid.vdb", "--max-input-bytes", "1"],
            ["--export-mesh", "source.vdb", "invalid.obj", "--max-triangles", "1"],
            ["--export-mesh", "source.vdb", "invalid.obj", "--max-voxels", "1"],
            ["--export-mesh", "source.vdb", "invalid.obj", "--input-grid", "missing"],
            ["--inspect", "source.vdb", "--input-grid", "missing"],
            ["--inspect", "source.vdb", "--stats", "--max-voxels", "1"],
            ["--inspect", "source.vdb", "--max-metadata-bytes", "1"],
            ["--inspect", "source.vdb", "--max-input-bytes", "1"],
        ]
        for arguments in failures:
            with self.subTest(arguments=arguments):
                self.run_tool(*arguments, succeeds=False)
                self.assertFalse((self.directory / "invalid.vdb").exists())
                self.assertFalse((self.directory / "invalid.obj").exists())

    def test_existing_outputs_and_input_are_protected(self):
        self.require_enabled()
        self.fixture()
        original = (self.directory / "source.vdb").read_bytes()
        for name, operation in (("existing.vdb", "--segment"), ("existing.obj", "--export-mesh")):
            target = self.directory / name
            target.write_bytes(b"preserve this existing output")
            self.run_tool(operation, "source.vdb", name, succeeds=False)
            self.assertEqual(target.read_bytes(), b"preserve this existing output")
            self.run_tool(operation, "source.vdb", "source.vdb", succeeds=False)
            self.assertEqual((self.directory / "source.vdb").read_bytes(), original)


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--tool", type=pathlib.Path, required=True)
    parser.add_argument("--enabled", choices=("0", "1"), required=True)
    args, remaining = parser.parse_known_args()
    OpenVdbInterchangeCli.tool = args.tool.resolve()
    OpenVdbInterchangeCli.enabled = args.enabled == "1"
    unittest.main(argv=[__file__, *remaining])
