"""Real-tool potential-flow contracts, conditioning, fields and strict routing."""

import argparse
import json
import math
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "python"))
from container_openvdb import DenseType, read_dense_array


class PotentialFlowCli(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="container-potential-flow-cli-")
        self.directory = Path(self.temporary.name)
        self.originals = {}
        self.serial = 0

    def tearDown(self):
        try:
            for path, original in self.originals.items():
                self.assertEqual(path.read_bytes(), original, str(path))
            self.assertFalse(list(self.directory.glob(".container-*")), "private output leaked")
        finally:
            self.temporary.cleanup()

    def call(self, *arguments, succeeds=True):
        result = subprocess.run(
            [str(self.tool), *map(str, arguments)], cwd=self.directory,
            capture_output=True, text=True, encoding="utf-8", timeout=90)
        self.assertEqual(result.returncode == 0, succeeds,
                         f"{arguments!r}\n{result.stdout}\n{result.stderr}")
        return result

    def report(self, *arguments):
        return json.loads(self.call(*arguments).stdout)

    def enabled_only(self):
        if not self.enabled:
            self.skipTest("OpenVDB is disabled")

    def protect(self, path):
        path = self.directory / path
        self.originals[path] = path.read_bytes()

    def sphere(self, name="source.vdb", grid="surface"):
        self.call("--sphere", name, "--radius", 1, "--voxel-size", .25,
                  "--grid-name", grid)
        self.protect(name)

    def vector(self, path, grid, position):
        value = self.report("--sample-vector", path, *position,
                            "--input-grid", "descriptor:" + grid)
        self.assertEqual(len(value), 3)
        self.assertTrue(all(math.isfinite(component) for component in value))
        return value

    def dense(self, path, grid):
        self.serial += 1
        output = f"readback-{self.serial}.cvarr"
        self.call("--dense-export", path, output, "--input-grid", "descriptor:" + grid,
                  "--dense-origin", -12, -12, -12,
                  "--dense-dimensions", 25, 25, 25)
        result = read_dense_array(self.directory / output)
        self.assertEqual(result.active_voxels_outside, 0, "crop must cover the full field")
        self.assertFalse(result.stored_nonbackground_outside)
        return result

    def validate_report(self, result, names=("flow", "potential", "flow_domain", "neumann"),
                        tolerance=1e-6, iteration_limit=1000):
        self.assertEqual(result["schemaVersion"], 1)
        self.assertIs(result["openvdbAvailable"], True)
        self.assertIs(result["converged"], True)
        self.assertEqual(result["iterationLimit"], iteration_limit)
        self.assertEqual(result["tolerance"], tolerance)
        for key in ("domainCellCount", "componentCount", "neumannCellCount", "workItems",
                    "iterations", "interiorDiagnosticCellCount"):
            self.assertIs(type(result[key]), int, key)
            self.assertGreaterEqual(result[key], 0, key)
        self.assertGreater(result["domainCellCount"], 0)
        self.assertEqual(result["componentCount"], 1)
        self.assertGreater(result["neumannCellCount"], 0)
        self.assertLessEqual(result["neumannCellCount"], result["domainCellCount"])
        self.assertGreater(result["workItems"], 0)
        self.assertLessEqual(result["iterations"], iteration_limit)
        self.assertTrue(math.isfinite(result["residual"]))
        self.assertGreaterEqual(result["residual"], 0)
        self.assertLessEqual(result["residual"], tolerance)
        self.assertTrue(math.isfinite(result["maxBoundaryNormalError"]))
        self.assertGreaterEqual(result["maxBoundaryNormalError"], 0)
        available = result["interiorDiagnosticCellCount"] != 0
        self.assertIs(result["interiorDiagnosticsAvailable"], available)
        for key in ("maxInteriorDivergence", "maxInteriorCurl"):
            if available:
                self.assertTrue(math.isfinite(result[key]), key)
                self.assertGreaterEqual(result[key], 0, key)
            else:
                self.assertIsNone(result[key], key)
        self.assertEqual(len(result["grids"]), 4)
        grids = {grid["name"]: grid for grid in result["grids"]}
        self.assertEqual(set(grids), set(names))
        for index, name in enumerate(names):
            self.assertEqual(grids[name]["gridClass"], "unknown")
            self.assertEqual(grids[name]["voxelSize"], .25)
            self.assertEqual(grids[name]["activeVoxelCount"],
                             result["neumannCellCount" if index == 3 else "domainCellCount"])
        self.assertTrue(result["diagnostics"])

    def parser_failure(self, arguments, diagnostic=None):
        failure = self.call(*arguments, succeeds=False)
        if diagnostic:
            self.assertIn(diagnostic, failure.stderr)
        # Missing inputs or OFF availability must not hide ignored syntax.
        self.assertNotIn("ENABLE_OPENVDB=ON", failure.stderr)
        self.assertFalse((self.directory / "bad.vdb").exists())
        self.assertFalse((self.directory / "-h").exists())

    def test_capabilities_have_authoritative_defaults_ranges_and_limitations(self):
        result = self.report("--potential-flow-capabilities")
        self.assertEqual(result["schemaVersion"], 1)
        self.assertIs(result["openvdbAvailable"], self.enabled)
        self.assertIsInstance(result["contracts"], list)
        contracts = " ".join(result["contracts"]).lower()
        for contract in ("uniform", "18-neighbor", "neumann", "dirichlet", "tangential", "residual"):
            self.assertIn(contract, contracts)
        defaults = result["defaults"]
        self.assertEqual(defaults["farFieldVelocity"], [1, 0, 0])
        self.assertEqual(defaults["colliderVelocity"], [0, 0, 0])
        self.assertEqual(defaults["exteriorWidth"], 6)
        self.assertEqual(defaults["maxIterations"], 1000)
        self.assertEqual(defaults["maxSolverVoxels"], 100000)
        self.assertEqual(defaults["tolerance"], 1e-6)
        limits = result["limits"]
        self.assertEqual(limits["exteriorWidth"], [1, 128])
        self.assertEqual(limits["maxSolverVoxels"], [1, 1000000])
        self.assertEqual(limits["maxIterations"], [1, 10000])
        self.assertEqual(limits["maxWorkItems"], [1, 1000000000000])
        self.assertEqual(limits["maxGrids"], [1, 4096])
        self.assertEqual(limits["tolerance"], [1e-12, 1e-2])
        self.assertEqual(list(self.directory.iterdir()), [])

    def test_help_arity_unique_options_and_incompatible_flags(self):
        text = self.call("--help").stdout
        for flag in ("--potential-flow", "--potential-flow-capabilities", "--far-field",
                     "--collider-velocity", "--exterior-width", "--max-flow-work",
                     "--max-flow-grids", "--max-flow-metadata", "--potential-name", "--neumann-name"):
            self.assertIn(flag, text)
        for arguments in (
            ("--potential-flow",), ("--potential-flow", "missing.vdb"),
            ("--potential-flow", "missing.vdb", "--far-field", 1, 0, 0),
            ("--potential-flow", "--grid", "bad.vdb"),
            ("--potential-flow", "missing.vdb", "-h"),
            ("--potential-flow", "-h", "bad.vdb"),
            ("--potential-flow", "", "bad.vdb"),
            ("--potential-flow", "missing.vdb", ""),
        ):
            with self.subTest(arguments=arguments):
                self.parser_failure(arguments, "requires INPUT OUTPUT before options")
        for extra in (("unexpected",), ("--help",), ("--far-field", 1, 0, 0)):
            self.parser_failure(("--potential-flow-capabilities", *extra), "accepts no paths or options")
        flags = {"--far-field": (1, 0, 0), "--collider-velocity": (0, 0, 0),
                 "--exterior-width": (6,), "--max-solver-voxels": (100000,),
                 "--max-iterations": (1000,), "--tolerance": (1e-6,),
                 "--max-flow-work": (8000000000,), "--max-flow-grids": (256,),
                 "--max-flow-metadata": (1048576,), "--velocity-name": ("flow",),
                 "--potential-name": ("potential",), "--domain-name": ("domain",),
                 "--neumann-name": ("neumann",), "--grid": ("surface",),
                 "--max-voxels": (16000000,), "--max-input-bytes": (536870912,)}
        for flag, values in flags.items():
            with self.subTest(duplicate=flag):
                self.parser_failure(("--potential-flow", "missing.vdb", "bad.vdb",
                                     flag, *values, flag, *values), "duplicate")
            with self.subTest(missing_value=flag):
                self.parser_failure(("--potential-flow", "missing.vdb", "bad.vdb", flag), "missing value")
        for extra in (("--input-grid", "surface"), ("--grid-name", "ignored"),
                      ("--voxel-size", .1), ("--half-width", 3), ("--stats",),
                      ("--acceleration", 0, 1, 0), ("--no-domain",),
                      ("--kinematic-viscosity", .1), ("--max-triangles", 10)):
            with self.subTest(incompatible=extra):
                self.parser_failure(("--potential-flow", "missing.vdb", "bad.vdb", *extra),
                                    "unknown/incompatible")
        for extra in (("--far-field", 1, 0, 0), ("--max-flow-work", 1),
                      ("--potential-name", "ignored")):
            with self.subTest(leaked_flag=extra):
                self.parser_failure(("--sphere", "bad.vdb", *extra))
        self.assertEqual(list(self.directory.iterdir()), [])

    def test_numeric_ranges_are_checked_before_api_or_files(self):
        invalid = (("--exterior-width", 0), ("--exterior-width", 129),
                   ("--max-solver-voxels", 0), ("--max-solver-voxels", 1000001),
                   ("--max-iterations", 0), ("--max-iterations", 10001),
                   ("--tolerance", "nan"), ("--tolerance", 0),
                   ("--tolerance", 1e-13), ("--tolerance", .1),
                   ("--max-flow-work", 0), ("--max-flow-work", 1000000000001),
                   ("--max-flow-grids", 0), ("--max-flow-grids", 4097),
                   ("--max-flow-metadata", 0), ("--max-voxels", 0),
                   ("--max-input-bytes", 0), ("--max-iterations", "-1"),
                   ("--max-flow-work", "+1"), ("--max-flow-grids", "1.0"),
                   ("--far-field", "inf", 0, 0), ("--far-field", 1000000000001, 0, 0),
                   ("--collider-velocity", 0, "nan", 0))
        for extra in invalid:
            with self.subTest(arguments=extra):
                self.parser_failure(("--potential-flow", "missing.vdb", "bad.vdb", *extra))
        self.assertEqual(list(self.directory.iterdir()), [])

    def test_off_reports_availability_before_input_access(self):
        if self.enabled:
            self.skipTest("OpenVDB is enabled")
        for source in ("不存在.vdb", "./-h"):
            result = self.call("--potential-flow", source, "bad.vdb", "--far-field", 0, 0, 0,
                               "--grid", "descriptor:surface", succeeds=False)
            self.assertIn("ENABLE_OPENVDB=ON", result.stderr)
        self.assertEqual(list(self.directory.iterdir()), [])

    def test_zero_and_comoving_fields_have_zero_potential_and_world_background(self):
        self.enabled_only()
        self.sphere()
        for name, velocity in (("still", (0, 0, 0)), ("comoving", (.75, -.25, .125))):
            with self.subTest(name=name):
                output = name + ".vdb"
                result = self.report("--potential-flow", "source.vdb", output,
                                     "--far-field", *velocity, "--collider-velocity", *velocity)
                self.validate_report(result)
                self.assertEqual(result["residual"], 0)
                statistics = self.report("--inspect", output, "--stats",
                                         "--input-grid", "descriptor:potential")["grids"][0]["statistics"]
                for key in ("minimum", "maximum", "sum", "background"):
                    self.assertEqual(statistics[key], 0, key)
                self.assertEqual(statistics["count"], result["domainCellCount"])
                for position in ((1.25, 0, 0), (0, 0, 0), (25, -11, 3)):
                    self.assertEqual(self.vector(output, "flow", position), list(velocity))
                    self.assertEqual(self.vector(output, "neumann", position), [0, 0, 0])

    def test_galilean_shift_preserves_dense_potential_and_adds_world_flow(self):
        self.enabled_only()
        self.sphere()
        base = self.report("--potential-flow", "source.vdb", "base.vdb", "--tolerance", 1e-8)
        shifted = self.report("--potential-flow", "source.vdb", "shifted.vdb", "--tolerance", 1e-8,
                              "--far-field", 1.25, -.4, .1, "--collider-velocity", .25, -.4, .1)
        self.validate_report(base, tolerance=1e-8)
        self.validate_report(shifted, tolerance=1e-8)
        self.assertGreater(base["iterations"], 0)
        self.assertEqual(base["domainCellCount"], shifted["domainCellCount"])
        a, b = self.dense("base.vdb", "potential"), self.dense("shifted.vdb", "potential")
        self.assertEqual(a.dtype, DenseType.FLOAT64)
        self.assertEqual(a.active, b.active)
        self.assertEqual(a.values.tobytes(), b.values.tobytes())
        self.assertEqual(a.background.tobytes(), b.background.tobytes())
        self.assertGreater(max(map(abs, a.values)), 0)
        a, b = self.dense("base.vdb", "flow"), self.dense("shifted.vdb", "flow")
        self.assertEqual(a.dtype, DenseType.VEC3_FLOAT64)
        self.assertEqual(a.active, b.active)
        shift = (.25, -.4, .1)
        self.assertLess(max(abs(y - x - shift[index % 3])
                            for index, (x, y) in enumerate(zip(a.values, b.values))), 1e-11)
        for position in ((1.25, 0, 0), (0, 1.5, 0), (0, 0, 0), (25, -11, 3)):
            before = self.vector("base.vdb", "flow", position)
            after = self.vector("shifted.vdb", "flow", position)
            for x, y, delta in zip(before, after, shift):
                self.assertAlmostEqual(y - x, delta, delta=1e-11)

    def test_literal_names_and_explicit_grid_selectors(self):
        self.enabled_only()
        self.sphere(grid="descriptor:shape")
        names = ("descriptor:wind", "descriptor:phi", "descriptor:exterior", "descriptor:wall")
        result = self.report("--potential-flow", "source.vdb", "named.vdb",
                             "--grid", "descriptor:descriptor:shape",
                             "--velocity-name", names[0], "--potential-name", names[1],
                             "--domain-name", names[2], "--neumann-name", names[3],
                             "--max-flow-grids", 1, "--max-iterations", 777, "--tolerance", 1e-7)
        self.validate_report(result, names=names, tolerance=1e-7, iteration_limit=777)
        inspected = self.report("--inspect", "named.vdb")
        self.assertEqual({grid["name"] for grid in inspected["grids"]}, set(names))
        self.assertEqual(self.vector("named.vdb", names[0], (0, 0, 0)), [1, 0, 0])
        failed = self.call("--potential-flow", "source.vdb", "bad.vdb", "--grid", "missing", succeeds=False)
        self.assertNotIn("ENABLE_OPENVDB=ON", failed.stderr)
        self.assertFalse((self.directory / "bad.vdb").exists())

    def test_native_limits_and_failed_convergence_preserve_sources(self):
        self.enabled_only()
        self.sphere()
        cases = (("--max-solver-voxels", 1), ("--max-flow-work", 1),
                 ("--max-flow-metadata", 1), ("--max-voxels", 1),
                 ("--max-input-bytes", 1048576), ("--exterior-width", 4),
                 ("--velocity-name", "same", "--potential-name", "same"),
                 ("--max-iterations", 1, "--tolerance", 1e-12))
        for extra in cases:
            with self.subTest(arguments=extra):
                self.call("--potential-flow", "source.vdb", "bad.vdb", *extra, succeeds=False)
                self.assertFalse((self.directory / "bad.vdb").exists())
        self.call("--fog", "source.vdb", "fog.vdb")
        self.protect("fog.vdb")
        self.call("--potential-flow", "fog.vdb", "bad.vdb", succeeds=False)
        self.assertFalse((self.directory / "bad.vdb").exists())

    def test_unicode_paths_exclusive_output_and_source_alias(self):
        self.enabled_only()
        self.sphere()
        parent = self.directory / "風場-é"
        parent.mkdir()
        source, output = parent / "入力_球.vdb", parent / "流れ_球.vdb"
        shutil.copyfile(self.directory / "source.vdb", source)
        self.protect(source)
        result = self.report("--potential-flow", source, output)
        self.validate_report(result)
        self.assertTrue(output.is_file())
        before = output.read_bytes()
        self.call("--potential-flow", source, output, succeeds=False)
        self.assertEqual(output.read_bytes(), before)
        self.call("--potential-flow", source, source, succeeds=False)
        # Decode an ASCII copy: only the potential-flow API promises Unicode
        # output publication, not every unrelated upstream narrow-path reader.
        shutil.copyfile(output, self.directory / "readback.vdb")
        self.assertEqual(len(self.report("--inspect", "readback.vdb")["grids"]), 4)
        sentinel = self.directory / "protected.vdb"
        sentinel.write_bytes(b"existing output must survive")
        self.protect(sentinel)
        self.call("--potential-flow", source, sentinel, succeeds=False)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--tool", required=True, type=Path)
    parser.add_argument("--enabled", required=True, choices=("0", "1"))
    args, remaining = parser.parse_known_args()
    PotentialFlowCli.tool = args.tool.resolve(strict=True)
    PotentialFlowCli.enabled = args.enabled == "1"
    unittest.main(argv=[__file__, *remaining])
