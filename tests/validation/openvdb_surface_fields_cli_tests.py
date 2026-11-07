"""Real-tool surface differential fields, validity, precision and strict routing.

Fixtures and scalar/vector readback use the public stdlib dense bridge. Expected
activity footprints and analytic geometry are calculated independently here.
Bool validity uses the public inspection protocol, not a private VDB decoder.
"""

from array import array
import argparse
import itertools
import json
import math
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "python"))
from container_openvdb import (
    DenseType, DenseVolume, GridClass, VectorType, read_dense_array, write_dense_array,
)


class SurfaceFieldsCli(unittest.TestCase):
    operations = ("mean", "gaussian", "normal", "closest-point")

    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="container-surface-fields-cli-")
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

    def failure(self, *arguments, output="bad.vdb", diagnostic=None, parser=False):
        result = self.call(*arguments, succeeds=False)
        if diagnostic:
            self.assertIn(diagnostic, result.stderr)
        if parser:
            self.assertNotIn("ENABLE_OPENVDB=ON", result.stderr)
        self.assertFalse((self.directory / output).exists(), str(arguments))
        return result

    def surface(self, source, output, operation, *options):
        return self.report("--surface-fields", source, output,
                           "--surface-field-operation", operation, *options)

    def fixture(self, name="source.vdb", dtype=DenseType.FLOAT64, h=1,
                slope=1, offset=.25, removed=(), translation=(0, 0, 0),
                grid="plane", empty=False, hidden_negative=False, degenerate=False,
                grid_class=GridClass.LEVEL_SET):
        values, active = array(dtype.typecode), bytearray()
        removed = set(removed)
        for z, y, x in itertools.product(range(-2, 3), repeat=3):
            coordinate = (x, y, z)
            on = not empty and coordinate not in removed
            value = (slope * x + offset) * h if on else 8 * h
            if degenerate and abs(x) + abs(y) + abs(z) <= 1:
                value = offset * h
            if hidden_negative and coordinate == (0, 0, 0):
                value, on = -8 * h, False
            values.append(value)
            active.append(on)
        volume = DenseVolume(
            dtype, (5, 5, 5), values, active, origin=(-2, -2, -2),
            linear=(h, 0, 0, 0, h, 0, 0, 0, h), translation=translation,
            background=array(dtype.typecode, [8 * h]), grid_name=grid,
            grid_class=grid_class)
        encoded = str(name) + ".cvarr"
        write_dense_array(self.directory / encoded, volume)
        self.call("--dense-import", encoded, name)
        self.protect(encoded)
        self.protect(name)
        return volume

    def dense(self, name, grid="surface", origin=(-2, -2, -2), dimensions=(5, 5, 5)):
        self.serial += 1
        output = f"readback-{self.serial}.cvarr"
        self.call("--dense-export", name, output, "--input-grid", "descriptor:" + grid,
                  "--dense-origin", *origin, "--dense-dimensions", *dimensions)
        return read_dense_array(self.directory / output)

    def validate_report(self, result, operation, dtype, h=1,
                        names=("surface", "surface_valid"), promoted=False):
        self.assertEqual(result["schemaVersion"], 1)
        self.assertIs(result["openvdbAvailable"], True)
        self.assertEqual(result["operation"], operation)
        self.assertIs(result["promoteToDouble"], promoted)
        expected_type = {
            DenseType.FLOAT32: "float", DenseType.FLOAT64: "double",
            DenseType.VEC3_FLOAT32: "vec3s", DenseType.VEC3_FLOAT64: "vec3d",
        }[dtype]
        for label, name, value_type in (("field", names[0], expected_type),
                                         ("validity", names[1], "bool")):
            grid = result[label]
            self.assertEqual(grid["gridName"], name)
            self.assertEqual(grid["gridClass"], "unknown")
            self.assertEqual(grid["valueType"], value_type)
            self.assertEqual(grid["activeVoxelCount"], result["validVoxelCount"])
            self.assertAlmostEqual(grid["voxelSize"], h, delta=h * 1e-12)
        for key in ("inputActiveVoxelCount", "validVoxelCount", "missingFootprintVoxelCount",
                    "degenerateGradientVoxelCount", "gradientSampleCount", "workItems"):
            self.assertIs(type(result[key]), int, key)
            self.assertGreaterEqual(result[key], 0, key)
        self.assertEqual(result["inputActiveVoxelCount"],
                         result["validVoxelCount"] + result["missingFootprintVoxelCount"] +
                         result["degenerateGradientVoxelCount"])
        if result["gradientSampleCount"]:
            self.assertTrue(math.isfinite(result["maximumUnitGradientError"]))
            self.assertGreaterEqual(result["maximumUnitGradientError"], 0)
        else:
            self.assertIsNone(result["maximumUnitGradientError"])
        self.assertGreater(result["workItems"], 0)
        self.assertIsInstance(result["diagnostics"], list)
        self.assertTrue(result["diagnostics"])

    def inspect_validity(self, name, expected, grid="surface_valid"):
        inspected = self.report("--inspect", name, "--input-grid", "descriptor:" + grid)
        self.assertEqual(len(inspected["grids"]), 1)
        item = inspected["grids"][0]
        self.assertEqual(item["name"], grid)
        self.assertEqual(item["valueType"], "bool")
        self.assertEqual(item["gridClass"], "unknown")
        self.assertEqual(item["recorded"]["activeVoxelCount"], expected)

    def test_help_arity_required_operation_and_incompatible_flags(self):
        help_text = self.call("--help").stdout
        for flag in ("--surface-fields", "--surface-field-operation", "--surface-field-double",
                     "--surface-validity-name", "--max-surface-field-work"):
            self.assertIn(flag, help_text)
        for arguments in (("--surface-fields",), ("--surface-fields", "a.vdb"),
                          ("--surface-fields", "a.vdb", "-h"),
                          ("--surface-fields", "", "bad.vdb"),
                          ("--surface-fields", "a.vdb", "--surface-field-double")):
            with self.subTest(arguments=arguments):
                self.failure(*arguments, diagnostic="requires", parser=True)
        self.failure("--surface-fields", "missing.vdb", "bad.vdb",
                     diagnostic="--surface-field-operation", parser=True)
        self.failure("--surface-fields", "missing.vdb", "bad.vdb",
                     "--surface-field-operation", "unknown",
                     diagnostic="--surface-field-operation", parser=True)
        for extra in (("--voxel-size", .1), ("--half-width", 3), ("--stats",),
                      ("--max-triangles", 10), ("--max-surface-work", 10),
                      ("--position", 0, 0, 0), ("--surface-field-double", "true")):
            with self.subTest(extra=extra):
                self.failure("--surface-fields", "missing.vdb", "bad.vdb",
                             "--surface-field-operation", "normal", *extra,
                             diagnostic="unknown/incompatible", parser=True)
        for extra in (("--surface-field-double",), ("--surface-field-operation", "mean"),
                      ("--max-surface-field-work", 1)):
            self.failure("--sphere", "bad.vdb", *extra, parser=True)
        self.assertEqual(list(self.directory.iterdir()), [])

    def test_unique_options_aliases_values_ranges_and_names_are_strict(self):
        base = ("--surface-fields", "missing.vdb", "bad.vdb",
                "--surface-field-operation", "normal")
        arguments = {
            "--surface-field-operation": ("normal",), "--surface-field-double": (),
            "--surface-validity-name": ("valid",), "--max-surface-field-work": (200000000,),
            "--max-surface-field-grids": (256,), "--max-surface-field-metadata": (1048576,),
            "--grid": ("plane",), "--grid-name": ("derived",),
            "--max-voxels": (16000000,), "--max-input-bytes": (536870912,),
        }
        for flag, values in arguments.items():
            with self.subTest(duplicate=flag):
                suffix = (flag, *values) if flag == "--surface-field-operation" else \
                         (flag, *values, flag, *values)
                self.failure(*base, *suffix, diagnostic="duplicate", parser=True)
            if values:
                with self.subTest(missing=flag):
                    prefix = base[:3] if flag == "--surface-field-operation" else base
                    self.failure(*prefix, flag, diagnostic="missing value", parser=True)
        for aliases in (("--grid", "a", "--input-grid", "b"),
                        ("--input-grid", "a", "--grid", "b")):
            self.failure(*base, *aliases, diagnostic="duplicate", parser=True)
        for flag, value in (("--max-surface-field-work", 10000000001),
                            ("--max-surface-field-grids", 4097),
                            ("--max-surface-field-work", 0),
                            ("--max-surface-field-metadata", 0),
                            ("--max-surface-field-grids", "1.5"),
                            ("--max-voxels", "-1"), ("--max-input-bytes", 0),
                            ("--max-surface-field-work", "+1"),
                            ("--max-surface-field-work", "nan"),
                            ("--max-surface-field-metadata", 2**64)):
            with self.subTest(flag=flag, value=value):
                self.failure(*base, flag, value, diagnostic=flag, parser=True)
        for extra in (("--grid-name", ""), ("--surface-validity-name", ""),
                      ("--grid-name", "n" * 257), ("--surface-validity-name", "n" * 257),
                      ("--grid-name", "same", "--surface-validity-name", "same")):
            self.failure(*base, *extra, parser=True)
        self.assertEqual(list(self.directory.iterdir()), [])

    def test_disabled_reports_availability_before_input_access(self):
        if self.enabled:
            self.skipTest("OpenVDB is enabled")
        for operation in self.operations:
            self.failure("--surface-fields", "不存在.vdb", "bad.vdb",
                         "--surface-field-operation", operation, "--surface-field-double",
                         "--surface-validity-name", "valid", "--input-grid", "descriptor:plane",
                         diagnostic="ENABLE_OPENVDB=ON")
        self.failure("--surface-fields", "./-h", "bad.vdb", "--surface-field-operation", "mean",
                     diagnostic="ENABLE_OPENVDB=ON")
        self.assertEqual(list(self.directory.iterdir()), [])

    def test_planes_keep_native_precision_world_units_and_field_semantics(self):
        self.enabled_only()
        for dtype, h in ((DenseType.FLOAT32, .5), (DenseType.FLOAT64, 2)):
            source = f"plane-{int(dtype)}.vdb"
            self.fixture(source, dtype=dtype, h=h)
            for operation in self.operations:
                with self.subTest(dtype=dtype, operation=operation):
                    output = f"plane-{int(dtype)}-{operation}.vdb"
                    vector = operation in ("normal", "closest-point")
                    output_type = DenseType(int(dtype) + 2) if vector else dtype
                    result = self.surface(source, output, operation)
                    self.validate_report(result, operation, output_type, h=h)
                    self.assertEqual(result["inputActiveVoxelCount"], 125)
                    self.assertEqual(result["validVoxelCount"], 27)
                    self.assertAlmostEqual(result["maximumUnitGradientError"], 0, delta=1e-12)
                    field = self.dense(output)
                    self.assertEqual(field.dtype, output_type)
                    self.assertEqual(field.grid_class, GridClass.UNKNOWN)
                    self.assertTrue(field.world_space)
                    expected_frame = VectorType.COVARIANT_NORMALIZE if operation == "normal" else \
                                     VectorType.CONTRAVARIANT_ABSOLUTE if vector else VectorType.INVARIANT
                    self.assertEqual(field.vector_type, expected_frame)
                    self.assertEqual(field.active_voxels_outside, 0)
                    for z, y, x in itertools.product(range(-2, 3), repeat=3):
                        c = (x, y, z)
                        expected_active = all(abs(v) <= 1 for v in c)
                        self.assertEqual(field.active_at(c), expected_active)
                        expected = (1, 0, 0) if operation == "normal" else \
                                   (-.25 * h, y * h, z * h) if vector else 0
                        if not expected_active:
                            expected = (0, 0, 0) if vector else 0
                        self.assertEqual(field.value_at(c), expected)
                    self.inspect_validity(output, 27)

    def test_sphere_curvature_uses_cell_radius_and_cpt_is_in_world_units(self):
        self.enabled_only()
        self.call("--sphere", "sphere.vdb", "--radius", 1, "--voxel-size", .125)
        self.protect("sphere.vdb")
        for operation in self.operations:
            output = "sphere-" + operation + ".vdb"
            result = self.surface("sphere.vdb", output, operation)
            dtype = DenseType.FLOAT32 if operation in ("mean", "gaussian") else DenseType.VEC3_FLOAT32
            self.validate_report(result, operation, dtype, h=.125)
            field = self.dense(output, origin=(-12, -12, -12), dimensions=(25, 25, 25))
            for index in (7, 9):
                c, rho = (index, 0, 0), index * .125
                self.assertTrue(field.active_at(c))
                if operation == "mean":
                    self.assertAlmostEqual(field.value_at(c), 1 / rho, delta=.012)
                elif operation == "gaussian":
                    self.assertAlmostEqual(field.value_at(c), 1 / rho**2, delta=.03)
                else:
                    self.assertEqual(field.value_at(c), (1, 0, 0))
            self.inspect_validity(output, result["validVoxelCount"])

    def test_exact_active_footprints_keep_zero_geometry_active_and_invalid_cells_zero(self):
        self.enabled_only()
        missing = (1, 1, 0)
        self.fixture(removed=(missing,))
        active = {c for c in itertools.product(range(-2, 3), repeat=3) if c != missing}
        for operation in self.operations:
            distance = 2 if operation in ("mean", "gaussian") else 1
            offsets = [d for d in itertools.product((-1, 0, 1), repeat=3)
                       if sum(map(abs, d)) <= distance]
            expected = {c for c in active if all(tuple(c[a] + d[a] for a in range(3)) in active
                                                 for d in offsets)}
            output = "footprint-" + operation + ".vdb"
            result = self.surface("source.vdb", output, operation)
            self.assertEqual(result["inputActiveVoxelCount"], 124)
            self.assertEqual(result["validVoxelCount"], len(expected))
            self.assertEqual(result["missingFootprintVoxelCount"], 124 - len(expected))
            self.assertEqual(result["degenerateGradientVoxelCount"], 0)
            field = self.dense(output)
            for c in itertools.product(range(-2, 3), repeat=3):
                self.assertEqual(field.active_at(c), c in expected)
                if c not in expected:
                    self.assertEqual(field.value_at(c), (0, 0, 0) if field.dtype.components == 3 else 0)
            self.assertEqual(field.active_at((0, 0, 0)), operation in ("normal", "closest-point"))
            self.inspect_validity(output, len(expected))

    def test_local_cpt_keeps_unnormalized_gradient_and_degeneracy_is_explicit(self):
        self.enabled_only()
        self.fixture(slope=2)
        result = self.surface("source.vdb", "cpt.vdb", "closest-point")
        self.assertAlmostEqual(result["maximumUnitGradientError"], 1, delta=1e-12)
        self.assertTrue(any("unnormalized" in d for d in result["diagnostics"]))
        self.assertEqual(self.dense("cpt.vdb").value_at((0, 0, 0)), (-.5, 0, 0))
        self.fixture("degenerate.vdb", degenerate=True)
        result = self.surface("degenerate.vdb", "degenerate-normal.vdb", "normal")
        self.assertGreater(result["degenerateGradientVoxelCount"], 0)
        field = self.dense("degenerate-normal.vdb")
        self.assertFalse(field.active_at((0, 0, 0)))
        self.assertEqual(field.value_at((0, 0, 0)), (0, 0, 0))

    def test_float_absolute_position_promotion_is_explicit_and_derivatives_stay_native(self):
        self.enabled_only()
        self.fixture(dtype=DenseType.FLOAT32, translation=(1e8, 0, 0))
        self.surface("source.vdb", "normal.vdb", "normal")
        self.assertEqual(self.dense("normal.vdb").dtype, DenseType.VEC3_FLOAT32)
        self.failure("--surface-fields", "source.vdb", "bad.vdb",
                     "--surface-field-operation", "closest-point", diagnostic="precision")
        result = self.surface("source.vdb", "promoted.vdb", "closest-point", "--surface-field-double")
        self.validate_report(result, "closest-point", DenseType.VEC3_FLOAT64, promoted=True)
        field = self.dense("promoted.vdb")
        self.assertEqual(field.dtype, DenseType.VEC3_FLOAT64)
        self.assertEqual(field.value_at((0, 0, 0)), (1e8 - .25, 0, 0))

    def test_empty_fields_have_nullable_quality_and_hidden_negative_or_bad_sources_reject(self):
        self.enabled_only()
        self.fixture(empty=True)
        for operation in self.operations:
            output = "empty-" + operation + ".vdb"
            result = self.surface("source.vdb", output, operation)
            dtype = DenseType.FLOAT64 if operation in ("mean", "gaussian") else DenseType.VEC3_FLOAT64
            self.validate_report(result, operation, dtype)
            self.assertEqual(result["inputActiveVoxelCount"], 0)
            self.assertEqual(result["validVoxelCount"], 0)
            self.assertEqual(result["gradientSampleCount"], 0)
            self.assertIsNone(result["maximumUnitGradientError"])
            self.assertFalse(any(self.dense(output).active))
            self.inspect_validity(output, 0)
        self.fixture("hidden.vdb", empty=True, hidden_negative=True)
        self.fixture("fog.vdb", grid_class=GridClass.FOG_VOLUME)
        for source in ("hidden.vdb", "fog.vdb"):
            self.failure("--surface-fields", source, "bad.vdb", "--surface-field-operation", "normal")

    def test_exact_selectors_custom_metadata_unicode_and_file_protection(self):
        self.enabled_only()
        self.fixture("left.vdb", grid="descriptor:plane")
        self.fixture("right.vdb", grid="other", offset=.5)
        spec = {"version": 1, "inputs": [
            {"path": "left.vdb", "edits": [{"selector": "descriptor:descriptor:plane", "metadata": [
                {"name": "artist", "operation": "set", "type": "string", "value": "Ω source"},
                {"name": "exact_id", "operation": "set", "type": "int64", "value": 9007199254740993}]}]},
            {"path": "right.vdb"}]}
        (self.directory / "bundle.json").write_text(json.dumps(spec, ensure_ascii=False), encoding="utf-8")
        self.call("--author-file", "bundle.json", "bundle.vdb")
        self.protect("bundle.vdb")
        source = self.directory / "source_球.vdb"
        output = self.directory / "derived_é_球.vdb"
        shutil.copyfile(self.directory / "bundle.vdb", source)
        self.protect(source)
        names = ("descriptor:derived", "descriptor:valid")
        result = self.surface(source, output, "normal", "--input-grid", "descriptor:descriptor:plane",
                              "--grid-name", names[0], "--surface-validity-name", names[1])
        self.validate_report(result, "normal", DenseType.VEC3_FLOAT64, names=names)
        self.protect(output)
        # Decode an ASCII copy: unrelated native readers need not accept every
        # Unicode output path supported by this API's publication wrapper.
        shutil.copyfile(output, self.directory / "read.vdb")
        field = self.dense("read.vdb", grid=names[0])
        self.assertEqual(field.value_at((0, 0, 0)), (1, 0, 0))
        info = self.report("--inspect", "read.vdb", "--input-grid", "descriptor:" + names[0])["grids"][0]
        metadata = {m["name"]: m for m in info["metadata"]}
        self.assertEqual(metadata["artist"]["value"], "Ω source")
        self.assertEqual(metadata["exact_id"]["value"], "9007199254740993")
        self.inspect_validity("read.vdb", result["validVoxelCount"], grid=names[1])
        args = ("--surface-field-operation", "normal")
        self.call("--surface-fields", source, source, *args, succeeds=False)
        self.call("--surface-fields", source, output, *args, succeeds=False)
        sentinel = self.directory / "protected.vdb"
        sentinel.write_bytes(b"existing output must survive")
        self.protect(sentinel)
        self.call("--surface-fields", source, sentinel, *args, succeeds=False)
        self.failure("--surface-fields", "bundle.vdb", "bad.vdb", *args,
                     "--grid", "descriptor:missing")

    def test_shared_caps_fail_without_output_and_explicit_selection_bounds_catalog(self):
        self.enabled_only()
        self.fixture()
        self.fixture("other.vdb", grid="unselected")
        spec = {"version": 1, "inputs": [{"path": "source.vdb"}, {"path": "other.vdb"}]}
        (self.directory / "two.json").write_text(json.dumps(spec), encoding="utf-8")
        self.call("--author-file", "two.json", "two.vdb")
        self.protect("two.vdb")
        for extra in (("--max-surface-field-work", 1), ("--max-surface-field-metadata", 1),
                      ("--max-surface-field-grids", 1), ("--max-voxels", 1),
                      ("--max-input-bytes", 1024)):
            with self.subTest(extra=extra):
                self.failure("--surface-fields", "two.vdb", "bad.vdb",
                             "--surface-field-operation", "mean", "--grid", "plane", *extra)
        result = self.surface("two.vdb", "selected.vdb", "mean", "--grid", "descriptor:plane",
                              "--max-surface-field-grids", 2)
        self.assertEqual(result["validVoxelCount"], 27)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--tool", required=True, type=Path)
    parser.add_argument("--enabled", required=True, choices=("0", "1"))
    args, remaining = parser.parse_known_args()
    SurfaceFieldsCli.tool = args.tool.resolve(strict=True)
    SurfaceFieldsCli.enabled = args.enabled == "1"
    unittest.main(argv=[__file__, *remaining])
