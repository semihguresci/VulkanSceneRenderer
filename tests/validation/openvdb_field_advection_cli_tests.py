"""Real-tool passive field advection, native values and independent flow oracles."""
from array import array
import argparse
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "python"))
from container_openvdb import (DenseType, DenseVolume, GridClass, VectorType,
                              read_dense_array, write_dense_array)


class FieldAdvectionCli(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="container-field-advection-cli-")
        self.directory = Path(self.temporary.name)
        self.originals = {}
        self.serial = 0

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

    def fixture(self, name, dtype, values, active, *, origin=(0, 0, 0), dimensions=None,
                background=None, linear=(1., 0., 0., 0., 1., 0., 0., 0., 1.),
                translation=(0., 0., 0.), grid="field", grid_class=GridClass.UNKNOWN,
                vector_type=VectorType.INVARIANT, world_space=True):
        if dtype.components == 3:
            flattened = [component for value in values for component in value]
            background = [0., 0., 0.] if background is None else background
        else:
            flattened = values
            background = [0. if background is None else background]
        volume = DenseVolume(dtype, dimensions or (len(active), 1, 1),
                             array(dtype.typecode, flattened), bytearray(active), origin=origin,
                             background=array(dtype.typecode, background), linear=linear,
                             translation=translation, grid_name=grid, grid_class=grid_class,
                             vector_type=vector_type, world_space=world_space)
        encoded = name + ".cvarr"
        write_dense_array(self.directory / encoded, volume)
        self.call("--dense-import", encoded, name)
        self.protect(encoded)
        self.protect(name)
        return volume

    def velocity(self, vector=(1., 0., 0.), *, dtype=DenseType.VEC3_FLOAT64,
                 linear=(1., 0., 0., 0., 1., 0., 0., 0., 1.)):
        self.fixture("velocity.vdb", dtype, [vector], [0], background=vector,
                     linear=linear, grid="velocity", vector_type=VectorType.CONTRAVARIANT_RELATIVE)

    def dense(self, name, *, origin=(-3, 0, 0), dimensions=(7, 1, 1), grid="surface"):
        self.serial += 1
        output = f"readback-{self.serial}.cvarr"
        self.call("--dense-export", name, output, "--input-grid", grid,
                  "--dense-origin", *origin, "--dense-dimensions", *dimensions)
        return read_dense_array(self.directory / output)

    def failure(self, *flags, source="source.vdb", velocity="velocity.vdb", diagnostic=None):
        result = self.call("--advect-field", source, velocity, "bad.vdb", *flags, succeeds=False)
        if diagnostic:
            self.assertIn(diagnostic, result.stderr)
        self.assertFalse((self.directory / "bad.vdb").exists())

    def test_help_arity_duplicate_aliases_limits_and_irrelevant_flags(self):
        text = self.call("--help").stdout
        for option in ("--advect-field", "--advection-scheme", "maccormack", "bfecc",
                       "--advection-limiter", "--velocity-layout", "--advection-cfl",
                       "--advection-mask", "--advection-mask-grid",
                       "--advection-voxelize-tiles", "--max-advection-work"):
            self.assertIn(option, text)
        for arguments in (("--advect-field",), ("--advect-field", "input", "velocity"),
                          ("--advect-field", "input", "--time", "1"),
                          ("--advect-field", "input", "velocity", "--steps"),
                          ("--advect-field", "input", "velocity", "-h"),
                          ("--advect-field", "", "velocity", "bad.vdb")):
            self.call(*arguments, succeeds=False)
        invalid = [("--time", "nan"), ("--time", "inf"), ("--time", "3601"),
                   ("--steps", "0"), ("--steps", "+1"), ("--steps", "65"),
                   ("--advection-cfl", "0"), ("--advection-cfl", "16.1"),
                   ("--advection-scheme", "rk2"), ("--advection-limiter", "bad"),
                   ("--advection-scheme", "rk4", "--advection-limiter", "none"),
                   ("--advection-limiter", "clamp"), ("--velocity-layout", "mac-world"),
                   ("--advection-mask-grid", "mask"), ("--advection-mask", ""),
                   ("--advection-mask", "--grid-name", "field"),
                   ("--advection-mask", "-h"),
                   ("--time", "0", "--time", "1"),
                   ("--grid", "one", "--input-grid", "two"),
                   ("--advection-voxelize-tiles", "--advection-voxelize-tiles"),
                   ("--max-advection-work", "0"), ("--max-advection-work", "10000000001"),
                   ("--max-advection-work", "18446744073709551616"),
                   ("--max-voxels", "0"), ("--max-input-bytes", "0"),
                   ("--voxel-size", ".25"), ("--half-width", "3"),
                   ("--max-sweep-work", "1"), ("--integration", "rk4"),
                   ("--grid-name", ""), ("--grid", ""), ("--steps",), ("extra",)]
        for flags in invalid:
            with self.subTest(flags=flags):
                self.failure(*flags, source="missing", velocity="missing")
        self.assertEqual(list(self.directory.iterdir()), [])

    def test_disabled_routes_report_availability_before_input_io(self):
        if self.enabled:
            self.skipTest("OpenVDB is enabled")
        for scheme in ("semi", "midpoint", "rk3", "rk4", "maccormack", "bfecc"):
            flags = ("--advection-limiter", "revert") if scheme in ("maccormack", "bfecc") else ()
            self.failure("--advection-scheme", scheme, "--velocity-layout", "mac-index",
                         "--advection-mask", "不存在.vdb", *flags,
                         diagnostic="ENABLE_OPENVDB=ON", source="missing", velocity="missing")
        self.assertEqual(list(self.directory.iterdir()), [])

    def test_six_schemes_translate_both_scalar_precisions_and_preserve_inactive_bits(self):
        self.enabled_only()
        spacing = (.25, 0., 0., 0., .25, 0., 0., 0., .25)
        self.velocity((.25, 0, 0), linear=spacing)
        for dtype in (DenseType.FLOAT32, DenseType.FLOAT64):
            values, active = [3.] * 33, [0] * 33
            values[0], values[16], values[28], active[16] = -7., 10., -0., 1
            name = f"source-{dtype.name}.vdb"
            original = self.fixture(name, dtype, values, active, origin=(-16, 0, 0),
                                    background=3., linear=spacing, grid="channel")
            for scheme in ("semi", "midpoint", "rk3", "rk4", "maccormack", "bfecc"):
                with self.subTest(dtype=dtype, scheme=scheme):
                    output = f"translated-{dtype.name}-{scheme}.vdb"
                    report = self.report("--advect-field", name, "velocity.vdb", output, "--time", 1,
                                         "--steps", 1, "--advection-scheme", scheme,
                                         "--input-grid", "descriptor:channel", "--velocity-grid", "velocity",
                                         "--velocity-layout", "world", "--grid-name", "moved")
                    result = self.dense(output, origin=(-16, 0, 0), dimensions=(33, 1, 1), grid="moved")
                    expected = array(dtype.typecode, original.values)
                    expected[16], expected[17] = 3., 10.
                    self.assertEqual(result.values.tobytes(), expected.tobytes())
                    expected_active = bytearray(33); expected_active[17] = 1
                    self.assertEqual(result.active, expected_active)
                    self.assertEqual(result.dtype, dtype)
                    self.assertEqual(result.linear, spacing)
                    self.assertEqual(report["schemaVersion"], 1)
                    self.assertEqual(report["operation"], "field-advection")
                    self.assertEqual(report["scheme"], scheme)
                    self.assertEqual(report["limiter"], "clamp" if scheme in ("maccormack", "bfecc") else None)
                    self.assertEqual(report["activeVoxelCount"], 1)
                    self.assertEqual(report["inputActiveVoxelCount"], 1)
                    self.assertEqual(report["executedSteps"], 1)
                    self.assertEqual(report["maximumStepDisplacement"], 1)
                    self.assertEqual(report["inputMinimum"], [-7, 0, 0])
                    self.assertEqual(report["outputMaximum"], [10, 0, 0])
                    self.assertFalse(report["noOp"])
                    self.assertGreater(report["workItems"], 0)
                    self.assertGreater(len(report["diagnostics"]), 0)
            output = f"negative-time-{dtype.name}.vdb"
            report = self.report("--advect-field", name, "velocity.vdb", output, "--time", -1,
                                 "--advection-scheme", "semi")
            result = self.dense(output)
            self.assertEqual(result.value_at((-1, 0, 0)), 10)
            self.assertTrue(result.active_at((-1, 0, 0)))
            self.assertEqual(report["timeSeconds"], -1)

    def test_analytic_fractional_profiles_limiters_and_fog_class_diagnostics(self):
        self.enabled_only()
        self.fixture("source.vdb", DenseType.FLOAT64, [1.], [1], grid_class=GridClass.FOG_VOLUME)
        self.velocity()
        profiles = {"semi": [0, 0, .5, .5, 0], "maccormack": [0, -.125, .75, .375, 0],
                    "bfecc": [0, -.0625, .5625, .5625, -.0625]}
        for scheme, expected in profiles.items():
            output = f"profile-{scheme}.vdb"
            flags = ("--advection-limiter", "none") if scheme != "semi" else ()
            report = self.report("--advect-field", "source.vdb", "velocity.vdb", output,
                                 "--time", .5, "--advection-scheme", scheme, *flags)
            result = self.dense(output, origin=(-2, 0, 0), dimensions=(5, 1, 1))
            self.assertEqual(list(result.values), expected)
            self.assertEqual(report["fogClassCleared"], scheme != "semi")
            self.assertEqual(result.grid_class, GridClass.FOG_VOLUME if scheme == "semi" else GridClass.UNKNOWN)
            if scheme != "semi":
                self.assertGreater(report["overshootVoxelCount"], 0)
                self.assertGreater(report["maximumRoundTripCorrection"][0], 0)
                self.assertLess(report["outputMinimum"][0], 0)
        for scheme in ("maccormack", "bfecc"):
            for limiter in ("clamp", "revert"):
                report = self.report("--advect-field", "source.vdb", "velocity.vdb", f"{scheme}-{limiter}.vdb",
                                     "--time", .5, "--advection-scheme", scheme,
                                     "--advection-limiter", limiter, "--advection-cfl", 1)
                self.assertGreaterEqual(report["outputMinimum"][0], 0)
                self.assertLessEqual(report["outputMaximum"][0], 1)
                self.assertFalse(report["fogClassCleared"])
                self.assertGreater(report["clampedVoxelCount" if limiter == "clamp" else "revertedVoxelCount"], 0)

    def test_vector_component_limiters_match_independent_scalar_channels(self):
        self.enabled_only()
        self.velocity(dtype=DenseType.VEC3_FLOAT32)
        values = [(0., 0., 0.), (0., 0., 7.), (1., 0., 0.), (0., -3., 0.), (0., 0., 0.)]
        for vector_type, scalar_type in ((DenseType.VEC3_FLOAT32, DenseType.FLOAT32),
                                         (DenseType.VEC3_FLOAT64, DenseType.FLOAT64)):
            name = f"vector-{vector_type.name}.vdb"
            self.fixture(name, vector_type, values, [1] * 5, origin=(-2, 0, 0),
                         vector_type=VectorType.CONTRAVARIANT_RELATIVE)
            output = f"vector-out-{vector_type.name}.vdb"
            self.report("--advect-field", name, "velocity.vdb", output, "--time", .5,
                        "--advection-scheme", "maccormack", "--advection-limiter", "clamp")
            actual = self.dense(output, origin=(-5, 0, 0), dimensions=(11, 1, 1))
            self.assertEqual(actual.vector_type, VectorType.CONTRAVARIANT_RELATIVE)
            self.assertTrue(actual.world_space)
            for channel in range(3):
                scalar = f"scalar-{vector_type.name}-{channel}.vdb"
                self.fixture(scalar, scalar_type, [point[channel] for point in values], [1] * 5,
                             origin=(-2, 0, 0))
                scalar_out = f"scalar-out-{vector_type.name}-{channel}.vdb"
                self.report("--advect-field", scalar, "velocity.vdb", scalar_out, "--time", .5,
                            "--advection-scheme", "maccormack", "--advection-limiter", "clamp")
                expected = self.dense(scalar_out, origin=(-5, 0, 0), dimensions=(11, 1, 1))
                self.assertEqual(array(scalar_type.typecode, actual.values[channel::3]).tobytes(),
                                 expected.values.tobytes())

    def test_rotated_scaled_native_mac_uses_half_face_offsets_and_world_conversion(self):
        self.enabled_only()
        coordinates = [(x, y, z) for z in range(-2, 3) for y in range(-4, 5) for x in range(-4, 5)]
        self.fixture("source.vdb", DenseType.FLOAT64, [float(y) for x, y, z in coordinates],
                     [1] * len(coordinates), origin=(-4, -4, -2), dimensions=(9, 9, 5))
        coordinates = [(x, y, z) for z in range(-2, 3) for y in range(-5, 6) for x in range(-5, 6)]
        self.fixture("mac-raw.vdb", DenseType.VEC3_FLOAT64, [(float(x), 0., 0.) for x, y, z in coordinates],
                     [1] * len(coordinates), origin=(-5, -5, -2), dimensions=(11, 11, 5),
                     linear=(0., -2., 0., 2., 0., 0., 0., 0., 2.), grid="velocity",
                     grid_class=GridClass.STAGGERED, world_space=False,
                     vector_type=VectorType.CONTRAVARIANT_RELATIVE)
        spec = {"version": 1, "inputs": [{"path": "mac-raw.vdb", "edits": [{"selector": "velocity",
                "metadata": [{"name": "container:mac_layout", "operation": "set", "type": "string", "value": "negative_faces"},
                             {"name": "container:velocity_units", "operation": "set", "type": "string", "value": "voxels_per_second"}]}]}]}
        (self.directory / "mac.json").write_text(json.dumps(spec), encoding="utf-8")
        self.call("--author-file", "mac.json", "velocity.vdb")
        self.protect("velocity.vdb")
        for layout in ("auto", "mac-index"):
            report = self.report("--advect-field", "source.vdb", "velocity.vdb", f"mac-{layout}.vdb",
                                 "--time", .25, "--advection-scheme", "semi", "--velocity-layout", layout)
            result = self.dense(f"mac-{layout}.vdb", origin=(0, 0, 0), dimensions=(1, 2, 1))
            # J maps u_x=i_x to world v_y=y+1 after the native +0.5 face offset.
            self.assertAlmostEqual(result.value_at((0, 0, 0)), -.25, delta=1e-12)
            self.assertAlmostEqual(result.value_at((0, 1, 0)), .5, delta=1e-12)
            self.assertIn("MAC", " ".join(report["diagnostics"]))
        self.failure("--velocity-layout", "world")

    def test_final_mask_preserves_exact_unselected_values_and_intermediate_transport(self):
        self.enabled_only()
        values = [0., 1., 0., 0., 0., 0., 0., -0.]
        original = self.fixture("source.vdb", DenseType.FLOAT64, values, [0, 1, 0, 0, 0, 0, 0, 0], origin=(-1, 0, 0))
        self.velocity()
        self.fixture("mask.vdb", DenseType.FLOAT64, [0.], [1], origin=(2, 0, 0), grid="selected")
        report = self.report("--advect-field", "source.vdb", "velocity.vdb", "masked.vdb", "--time", 1,
                             "--steps", 2, "--advection-scheme", "semi", "--advection-mask", "mask.vdb",
                             "--advection-mask-grid", "descriptor:selected")
        result = self.dense("masked.vdb", origin=(-1, 0, 0), dimensions=(8, 1, 1))
        expected = array("d", original.values); expected[3] = .25
        self.assertEqual(result.values.tobytes(), expected.tobytes())
        expected_active = bytearray(original.active); expected_active[3] = 1
        self.assertEqual(result.active, expected_active)
        self.assertTrue(report["maskUsed"])
        self.assertEqual(report["executedSteps"], 2)
        self.fixture("empty-mask.vdb", DenseType.FLOAT32, [0.], [0])
        no_op = self.report("--advect-field", "source.vdb", "velocity.vdb", "empty.vdb",
                            "--advection-mask", "empty-mask.vdb")
        empty = self.dense("empty.vdb", origin=(-1, 0, 0), dimensions=(8, 1, 1))
        self.assertTrue(no_op["noOp"])
        self.assertEqual(no_op["executedSteps"], 0)
        self.assertEqual(empty.values.tobytes(), original.values.tobytes())
        self.assertEqual(empty.active, original.active)

    def test_tile_consent_zero_time_native_bits_unicode_and_exclusive_publication(self):
        self.enabled_only()
        self.fixture("empty.vdb", DenseType.FLOAT64, [0.], [0])
        tile = self.report("--edit-topology", "empty.vdb", "source.vdb", "--edit-operation", "fill-box",
                           "--index-box", 8, 0, 0, 15, 7, 7, "--native-value", 2)
        self.assertGreater(tile["activeTileCount"], 0)
        self.protect("source.vdb")
        self.velocity()
        (self.directory / "-h").write_bytes((self.directory / "source.vdb").read_bytes())
        literal = self.report("--advect-field", "./-h", "velocity.vdb", "literal-h.vdb", "--time", 0)
        self.assertTrue(literal["noOp"])
        self.failure("--time", 1, "--advection-scheme", "semi")
        report = self.report("--advect-field", "source.vdb", "velocity.vdb", "tiles-moved.vdb", "--time", -1,
                             "--advection-scheme", "semi", "--advection-voxelize-tiles")
        result = self.dense("tiles-moved.vdb", origin=(7, 0, 0), dimensions=(9, 1, 1))
        self.assertEqual(list(result.values), [2.] * 8 + [0.])
        self.assertEqual(report["activeVoxelCount"], 512)
        parent = self.directory / "対流-é"; parent.mkdir()
        source = parent / "入力.vdb"; source.write_bytes((self.directory / "source.vdb").read_bytes())
        output = parent / "保持.vdb"
        report = self.report("--advect-field", source, "velocity.vdb", output, "--time", 0)
        self.assertTrue(report["noOp"])
        self.assertEqual(report["executedSteps"], 0)
        before = output.read_bytes()
        self.call("--advect-field", source, "velocity.vdb", output, "--time", 0, succeeds=False)
        self.assertEqual(before, output.read_bytes())
        exact = self.dense(output, origin=(8, 0, 0), dimensions=(8, 8, 8))
        self.assertEqual(exact.active, bytearray([1] * 512))
        self.assertEqual(exact.values.tobytes(), array("d", [2.] * 512).tobytes())

    def test_native_resource_and_semantic_failures_leave_no_output(self):
        self.enabled_only()
        self.fixture("source.vdb", DenseType.FLOAT64, [1.], [1])
        self.velocity()
        for flags in (("--max-advection-work", 1), ("--max-voxels", 1), ("--max-input-bytes", 1024),
                      ("--time", 20, "--advection-cfl", 1), ("--grid", "missing"),
                      ("--velocity-grid", "missing"), ("--velocity-layout", "mac-index")):
            with self.subTest(flags=flags):
                self.failure(*flags)
        self.fixture("sdf.vdb", DenseType.FLOAT32, [0.], [1], background=3., grid_class=GridClass.LEVEL_SET)
        self.failure("--time", 0, source="sdf.vdb")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--tool", required=True, type=Path)
    parser.add_argument("--enabled", required=True, choices=("0", "1"))
    args, remaining = parser.parse_known_args()
    FieldAdvectionCli.tool = args.tool.resolve(strict=True)
    FieldAdvectionCli.enabled = args.enabled == "1"
    unittest.main(argv=[__file__, *remaining])
