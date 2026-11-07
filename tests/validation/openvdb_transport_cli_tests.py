"""Opt-in dependency-free CLI workflows for bounded particles and fog transport."""
import argparse
import json
import pathlib
import subprocess
import tempfile
import unittest


class OpenVdbTransportCli(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="container-vdb-transport-")
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

    def particles(self, name="particles.json", records=None):
        if records is None:
            records = [{"position": [.2, -.1, 0], "radius": .4}]
        path = self.directory / name
        path.write_text(json.dumps(records), encoding="utf-8")
        return path

    def fog_fixture(self):
        particles = self.particles()
        self.run_tool("--particles", particles, "source.vdb", "--voxel-size", ".1",
                      "--grid-name", "surface")
        self.run_tool("--fog", "source.vdb", "density.vdb", "--input-grid", "surface",
                      "--grid-name", "density")
        return particles

    def sample(self, path, position, grid="density"):
        result = self.run_tool("--sample", path, *position, "--input-grid", grid)
        return float(result.stdout.strip())

    def test_help_and_invalid_transport_parameters(self):
        help_text = self.run_tool("--help").stdout
        for command in ("--particles", "--velocity", "--advect", "--velocity-grid"):
            self.assertIn(command, help_text)
        invalid = [
            ["--particles"],
            ["--velocity", "invalid.vdb"],
            ["--velocity", "invalid.vdb", "--vector", "1", "0"],
            ["--velocity", "invalid.vdb", "--vector", "nan", "0", "0"],
            ["--particles", "missing.json", "invalid.vdb", "--max-particles", "0"],
            ["--particles", "missing.json", "invalid.vdb", "--max-particles", "-1"],
            ["--advect", "missing.vdb", "velocity.vdb", "invalid.vdb"],
            ["--advect", "missing.vdb", "velocity.vdb", "invalid.vdb", "--time", "nan"],
            ["--advect", "missing.vdb", "velocity.vdb", "invalid.vdb", "--time", "inf"],
            ["--advect", "missing.vdb", "velocity.vdb", "invalid.vdb", "--time", "3601"],
            ["--advect", "missing.vdb", "velocity.vdb", "invalid.vdb", "--time", "1", "--steps", "0"],
            ["--advect", "missing.vdb", "velocity.vdb", "invalid.vdb", "--time", "1", "--steps", "65"],
            ["--particles", "missing.json", "invalid.vdb", "--time", "1"],
            ["--particles", "missing.json", "invalid.vdb", "--steps", "2"],
            ["--velocity", "invalid.vdb", "--vector", "1", "0", "0", "--radius", "1"],
            ["--velocity", "invalid.vdb", "--vector", "1", "0", "0", "--max-particles", "1"],
            ["--advect", "missing.vdb", "velocity.vdb", "invalid.vdb", "--time", "1",
             "--vector", "1", "0", "0"],
            ["--advect", "missing.vdb", "velocity.vdb", "invalid.vdb", "--time", "1",
             "--voxel-size", ".2"],
        ]
        for arguments in invalid:
            with self.subTest(arguments=arguments):
                self.run_tool(*arguments, succeeds=False)
                self.assertFalse((self.directory / "invalid.vdb").exists())

    def test_disabled_commands_explain_the_build_option(self):
        if self.enabled:
            self.skipTest("OpenVDB is enabled")
        particles = self.particles()
        for arguments in (
            ["--particles", particles, "disabled.vdb", "--voxel-size", ".1"],
            ["--velocity", "disabled.vdb", "--vector", "1", "0", "0"],
            ["--advect", "missing.vdb", "velocity.vdb", "disabled.vdb", "--time", "1"],
        ):
            with self.subTest(arguments=arguments):
                result = self.run_tool(*arguments, succeeds=False)
                self.assertIn("ENABLE_OPENVDB=ON", result.stdout + result.stderr)
                self.assertFalse((self.directory / "disabled.vdb").exists())

    def test_particle_schema_count_and_radius_fail_without_outputs(self):
        if not self.enabled:
            self.skipTest("OpenVDB is disabled")
        malformed = [
            "{not JSON}",
            json.dumps({"particles": [{"position": [0, 0, 0], "radius": .4}]}),
            "[]",
            json.dumps([{"position": [0, 0, 0]}]),
            json.dumps([{"radius": .4}]),
            json.dumps([{"position": [0, 0], "radius": .4}]),
            json.dumps([{"position": [0, 0, 0, 0], "radius": .4}]),
            json.dumps([{"position": [0, "wrong", 0], "radius": .4}]),
            json.dumps([{"position": [0, 0, 0], "radius": 0}]),
            json.dumps([{"position": [0, 0, 0], "radius": -.4}]),
            json.dumps([{"position": [0, 0, 0], "radius": .149}]),
            json.dumps([{"position": [0, 0, 0], "radius": 10.001}]),
            '[{"position":[0,0,0],"radius":NaN}]',
            '[{"position":[0,0,0],"radius":0.4,"radius":0.5}]',
            '[{"position":[0,0,0],"radius":0.4}] []',
            json.dumps([{"position": [0, 0, 0], "radius": .4, "velocity": [0, 0, 0]}]),
        ]
        for index, content in enumerate(malformed):
            with self.subTest(index=index):
                path = self.directory / f"invalid-{index}.json"
                path.write_text(content, encoding="utf-8")
                self.run_tool("--particles", path, "invalid.vdb", "--voxel-size", ".1", succeeds=False)
                self.assertFalse((self.directory / "invalid.vdb").exists())
        two = self.particles("two.json", [{"position": [0, 0, 0], "radius": .4},
                                          {"position": [1, 0, 0], "radius": .4}])
        self.run_tool("--particles", two, "invalid.vdb", "--voxel-size", ".1",
                      "--max-particles", "1", succeeds=False)
        self.run_tool("--particles", two, "invalid.vdb", "--voxel-size", ".1",
                      "--max-input-bytes", "1", succeeds=False)
        self.assertFalse((self.directory / "invalid.vdb").exists())

    def test_particles_fog_constant_velocity_and_signed_time(self):
        if not self.enabled:
            self.skipTest("OpenVDB is disabled")
        self.fog_fixture()
        self.assertLess(self.sample("source.vdb", (.2, -.1, 0), "surface"), 0)
        self.assertGreater(self.sample("source.vdb", (.8, -.1, 0), "surface"), 0)
        self.assertGreater(self.sample("density.vdb", (.2, -.1, 0)), .9)
        # Independent scalar/velocity voxel sizes must retain world units.
        self.run_tool("--velocity", "velocity.vdb", "--vector", "1", "0", "0",
                      "--voxel-size", ".2", "--grid-name", "velocity")
        originals = {name: (self.directory / name).read_bytes()
                     for name in ("source.vdb", "density.vdb", "velocity.vdb")}
        for name, time, center in (("positive", "1", (1.2, -.1, 0)),
                                    ("negative", "-1", (-.8, -.1, 0))):
            self.run_tool("--advect", "density.vdb", "velocity.vdb", name + ".vdb",
                          "--time", time, "--steps", "2", "--input-grid", "density",
                          "--velocity-grid", "velocity", "--grid-name", "transported")
            self.assertGreater(self.sample(name + ".vdb", center, "transported"), .9)
            self.assertLess(self.sample(name + ".vdb", (.2, -.1, 0), "transported"), .05)
        self.run_tool("--advect", "density.vdb", "velocity.vdb", "zero-time.vdb", "--time", "0",
                      "--input-grid", "density", "--velocity-grid", "velocity", "--grid-name", "density")
        self.run_tool("--velocity", "zero-velocity.vdb", "--vector", "0", "0", "0",
                      "--grid-name", "velocity")
        self.run_tool("--advect", "density.vdb", "zero-velocity.vdb", "zero-flow.vdb", "--time", "1",
                      "--input-grid", "density", "--velocity-grid", "velocity", "--grid-name", "density")
        for position in ((.2, -.1, 0), (.4, -.1, 0), (1.2, -.1, 0)):
            expected = self.sample("density.vdb", position)
            for output in ("zero-time.vdb", "zero-flow.vdb"):
                self.assertAlmostEqual(self.sample(output, position), expected, delta=1e-6)
        for name, before in originals.items():
            self.assertEqual((self.directory / name).read_bytes(), before)

    def test_diagonal_transport_retains_leading_material(self):
        if not self.enabled:
            self.skipTest("OpenVDB is disabled")
        self.fog_fixture()
        self.run_tool("--velocity", "diagonal.vdb", "--vector", ".5", ".5", ".5",
                      "--grid-name", "velocity")
        # The leading sample moves beyond upstream face-neighbor dilation's
        # unpadded reach. A material-space sample must survive diagonal motion.
        source_position = (.3, 0, .1)
        expected = self.sample("density.vdb", source_position)
        self.assertGreater(expected, .25)
        self.run_tool("--advect", "density.vdb", "diagonal.vdb", "diagonal-density.vdb",
                      "--time", "1", "--input-grid", "density", "--velocity-grid", "velocity",
                      "--grid-name", "density")
        self.assertAlmostEqual(self.sample("diagonal-density.vdb", (.8, .5, .6)), expected, delta=1e-5)

    def test_outputs_sources_and_workspaces_are_protected(self):
        if not self.enabled:
            self.skipTest("OpenVDB is disabled")
        particles = self.fog_fixture()
        self.run_tool("--velocity", "velocity.vdb", "--vector", "1", "0", "0",
                      "--grid-name", "velocity")
        protected = self.directory / "protected.vdb"
        protected.write_bytes(b"existing output must remain untouched")
        before = protected.read_bytes()
        for arguments in (
            ["--particles", particles, protected, "--voxel-size", ".1"],
            ["--velocity", protected, "--vector", "1", "0", "0"],
            ["--advect", "density.vdb", "velocity.vdb", protected, "--time", "1"],
        ):
            result = self.run_tool(*arguments, succeeds=False)
            self.assertIn("output", result.stdout + result.stderr)
            self.assertEqual(protected.read_bytes(), before)
        source_bytes = (self.directory / "density.vdb").read_bytes()
        self.run_tool("--advect", "density.vdb", "velocity.vdb", "density.vdb", "--time", "1",
                      succeeds=False)
        self.assertEqual((self.directory / "density.vdb").read_bytes(), source_bytes)
        for arguments in (
            ["--particles", particles, "invalid.vdb", "--voxel-size", ".1", "--max-voxels", "1"],
            ["--advect", "density.vdb", "velocity.vdb", "invalid.vdb", "--time", "1",
             "--max-voxels", "1"],
            ["--advect", "density.vdb", "velocity.vdb", "invalid.vdb", "--time", "1",
             "--max-input-bytes", "1"],
            ["--advect", "source.vdb", "velocity.vdb", "invalid.vdb", "--time", "1"],
        ):
            self.run_tool(*arguments, succeeds=False)
            self.assertFalse((self.directory / "invalid.vdb").exists())


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--tool", required=True, type=pathlib.Path)
    parser.add_argument("--enabled", required=True, choices=("0", "1"))
    args = parser.parse_args()
    OpenVdbTransportCli.tool = args.tool.resolve()
    OpenVdbTransportCli.enabled = args.enabled == "1"
    unittest.main(argv=[__file__], verbosity=2)
