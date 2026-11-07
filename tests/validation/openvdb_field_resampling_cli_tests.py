"""Real-tool ordinary typed field sampling, affine/vector semantics and limits."""
import argparse
from array import array
import json
import math
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "python"))
from container_openvdb.dense import (DenseType, DenseVolume, GridClass, VectorType,
                                    read_dense_array, write_dense_array)


IDENTITY = (1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1)


class FieldResamplingCli(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="container-field-resampling-cli-")
        self.directory = Path(self.temporary.name)
        self.serial = 0
        self.originals = {}

    def tearDown(self):
        try:
            for source, data in self.originals.items():
                self.assertEqual(source.read_bytes(), data, source)
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

    def dense(self, name, function, *, dtype=DenseType.FLOAT64, half=3, background=None,
              active=None, tag=VectorType.INVARIANT, world=True, grid="input",
              grid_class=GridClass.UNKNOWN):
        components = dtype.components
        values, activity = array(dtype.typecode), bytearray()
        for z in range(-half, half + 1):
            for y in range(-half, half + 1):
                for x in range(-half, half + 1):
                    value = function(x, y, z)
                    values.extend((value,) if components == 1 else value)
                    activity.append(1 if active is None else bool(active(x, y, z)))
        volume = DenseVolume(dtype, (2 * half + 1,) * 3, values, activity,
            origin=(-half,) * 3, background=array(dtype.typecode, background or [0] * components),
            grid_name=grid, grid_class=grid_class, vector_type=tag, world_space=world)
        encoded = self.directory / (name + ".cvarr")
        write_dense_array(encoded, volume)
        self.call("--dense-import", encoded, name)
        self.protect(name)

    def sample(self, source, origin=(0, 0, 0), dimensions=(1, 1, 1), grid="field"):
        self.serial += 1
        encoded = self.directory / f"readback-{self.serial}.cvarr"
        self.call("--dense-export", source, encoded, "--input-grid", grid,
                  "--dense-origin", *origin, "--dense-dimensions", *dimensions)
        return read_dense_array(encoded)

    def assert_report(self, report):
        self.assertEqual(report["schemaVersion"], 1)
        self.assertEqual(report["operation"], "resample-field")
        for name in ("activeVoxelCount", "inputActiveVoxelCount", "inputStoredLeafCount",
                     "outputStoredLeafCount", "sampledVoxelCount", "inactiveSampleCount",
                     "workUpperBound", "roundedFloatComponents"):
            self.assertIs(type(report[name]), int, name)
            self.assertGreaterEqual(report[name], 0)
        self.assertIs(type(report["exactCopy"]), bool)
        self.assertTrue(math.isfinite(report["maximumFloatAbsoluteError"]))
        self.assertGreaterEqual(report["maximumFloatAbsoluteError"], 0)
        for name in ("sourceSupportBounds", "targetBounds"):
            if report[name] is not None:
                self.assertEqual(len(report[name]), 6)
                self.assertTrue(all(type(x) is int for x in report[name]))
        self.assertTrue(all(type(x) is str for x in report["diagnostics"]))

    def test_help_strict_operands_duplicate_aliases_modes_matrices_and_irrelevant_flags(self):
        help_text = self.call("--help").stdout
        for flag in ("--resample-field", "--field-target-transform", "--field-world-transform",
                     "--field-interpolation", "--field-activity", "--field-vector-values",
                     "--max-field-resampling-work", "--max-field-resampling-grids",
                     "--max-field-resampling-metadata"):
            self.assertIn(flag, help_text)
        cases = [("--resample-field",), ("--resample-field", "missing"),
                 ("--resample-field", "", "bad.vdb"), ("--resample-field", "missing", ""),
                 ("--resample-field", "-h", "bad.vdb"), ("--resample-field", "missing", "-h"),
                 ("--resample-field", "--sphere", "bad.vdb"),
                 ("--resample-field", "missing", "--grid-name", "bad.vdb")]
        invalid = [("--field-interpolation", "cubic"), ("--field-activity", "all"),
                   ("--field-vector-values", "velocity"), ("--field-interpolation", ""),
                   ("--field-activity", "-h"), ("--grid", "a", "--input-grid", "b"),
                   ("--input-grid", "a", "--grid", "b"),
                   ("--grid-name", "a", "--grid-name", "b"),
                   ("--grid", "--field-activity", "any"), ("--grid-name", ""),
                   ("--field-interpolation", "nearest", "--field-interpolation", "linear"),
                   ("--max-field-resampling-work", 0), ("--max-field-resampling-work", " -1"),
                   ("--max-field-resampling-work", 10000000001),
                   ("--max-field-resampling-grids", 4097), ("--max-field-resampling-grids", "+2"),
                   ("--max-field-resampling-metadata", 0), ("--max-input-bytes", 0),
                   ("--max-voxels", 0), ("--max-triangles", 1000),
                   ("--voxel-size", .1), ("--half-width", 3), ("--radius", 1),
                   ("--field-world-transform", *IDENTITY[:15]),
                   ("--field-target-transform", *IDENTITY, 7),
                   ("--field-target-transform", *IDENTITY, "--field-target-transform", *IDENTITY),
                   ("--field-world-transform", *IDENTITY[:15], 0),
                   ("--field-target-transform", "nan", *IDENTITY[1:])]
        cases.extend(("--resample-field", "missing", "bad.vdb", *flags) for flags in invalid)
        for args in cases:
            with self.subTest(args=args):
                self.call(*args, succeeds=False)
                self.assertFalse((self.directory / "bad.vdb").exists())
        self.assertEqual(list(self.directory.iterdir()), [])

    def test_disabled_reports_availability_before_path_or_report_processing(self):
        if self.enabled:
            self.skipTest("OpenVDB is enabled")
        result = self.call("--resample-field", "不存在", "missing-parent/out.vdb", succeeds=False)
        self.assertIn("ENABLE_OPENVDB=ON", result.stderr)
        self.assertFalse((self.directory / "missing-parent").exists())

    def test_real_scalar_and_vector_linear_quadratic_affine_polynomial_oracles(self):
        self.enabled_only()
        target = (.5, .25, 0, .25, 0, .5, 0, .25, 0, 0, -.5, .25, 0, 0, 0, 1)
        for dtype in DenseType:
            for quadratic in (False, True):
                with self.subTest(dtype=dtype, quadratic=quadratic):
                    def function(x, y, z):
                        value = x*x + 2*y*y + 3*z*z + x*y if quadratic else 2*x - 3*y + z + 5
                        return value if dtype.components == 1 else (value, 2*value+1, -value)
                    name = f"source-{int(dtype)}-{quadratic}.vdb"
                    output = f"out-{int(dtype)}-{quadratic}.vdb"
                    self.dense(name, function, dtype=dtype)
                    report = self.report("--resample-field", name, output,
                        "--field-target-transform", *target,
                        "--field-interpolation", "quadratic" if quadratic else "linear")
                    self.assert_report(report)
                    self.assertFalse(report["exactCopy"])
                    volume = self.sample(output, (-1, -1, -1), (3, 3, 3))
                    self.assertEqual(volume.dtype, dtype)
                    self.assertEqual(volume.translation, (.25, .25, .25))
                    self.assertEqual(volume.linear, (.5, .25, 0., 0., .5, 0., 0., 0., -.5))
                    for z in range(-1, 2):
                        for y in range(-1, 2):
                            for x in range(-1, 2):
                                expected = function(.5*x + .25*y + .25, .5*y + .25, -.5*z + .25)
                                value = volume.value_at((x, y, z))
                                if dtype.components == 1:
                                    self.assertAlmostEqual(value, expected, delta=1e-6)
                                else:
                                    for a, b in zip(value, expected):
                                        self.assertAlmostEqual(a, b, delta=1e-6)
                                self.assertTrue(volume.active_at((x, y, z)))

    def test_inactive_values_activity_policy_and_identity_signed_zero(self):
        self.enabled_only()
        self.dense("source.vdb", lambda x, y, z: 2 if (x, y, z) == (0, 0, 0) else
                   4 if (x, y, z) == (1, 0, 0) else 0,
                   half=1, active=lambda x, y, z: (x, y, z) == (1, 1, 0))
        target = (1, 0, 0, .25, 0, 1, 0, .25, 0, 0, 1, 0, 0, 0, 0, 1)
        for policy in ("nearest", "any"):
            output = policy + ".vdb"
            report = self.report("--resample-field", "source.vdb", output,
                "--field-target-transform", *target, "--field-activity", policy)
            self.assert_report(report)
            volume = self.sample(output)
            self.assertEqual(volume.value_at((0, 0, 0)), 1.875)
            self.assertEqual(volume.active_at((0, 0, 0)), policy == "any")
        self.dense("zero.vdb", lambda x, y, z: -0., half=0,
                   dtype=DenseType.FLOAT32, active=lambda x, y, z: False)
        report = self.report("--resample-field", "zero.vdb", "zero-copy.vdb")
        self.assertTrue(report["exactCopy"])
        volume = self.sample("zero-copy.vdb")
        self.assertEqual(struct.pack("<f", volume.value_at((0, 0, 0))), b"\0\0\0\x80")
        self.assertFalse(volume.active_at((0, 0, 0)))
        self.dense("rounding.vdb", lambda x, y, z: 1/3 if (x, y, z) == (1, 0, 0) else 0.,
                   half=1, dtype=DenseType.FLOAT32)
        target = (1, 0, 0, .1, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1)
        report = self.report("--resample-field", "rounding.vdb", "rounded.vdb",
                             "--field-target-transform", *target)
        self.assertGreater(report["roundedFloatComponents"], 0)
        self.assertGreater(report["maximumFloatAbsoluteError"], 0)
        rounded = lambda value: struct.unpack("<f", struct.pack("<f", value))[0]
        self.assertEqual(self.sample("rounded.vdb").value_at((0, 0, 0)), rounded(rounded(1/3)*.1))

    def test_all_five_world_vector_semantics_target_deformation_and_infinite_background(self):
        self.enabled_only()
        deformation = (2, 0, 0, 5, 0, -1, 0, 6, 0, 0, .5, 7, 0, 0, 0, 1)
        expected = {
            VectorType.INVARIANT: (2, 3, 4),
            VectorType.COVARIANT: (1, -3, 8),
            VectorType.COVARIANT_NORMALIZE: tuple(x/math.sqrt(74) for x in (1, -3, 8)),
            VectorType.CONTRAVARIANT_RELATIVE: (4, -3, 2),
            VectorType.CONTRAVARIANT_ABSOLUTE: (9, 3, 9)}
        for tag in VectorType:
            name = f"vector-{int(tag)}.vdb"
            self.dense(name, lambda x, y, z: (2, 3, 4), half=0,
                       dtype=DenseType.VEC3_FLOAT64, background=(2, 3, 4), tag=tag)
            for raw in (False, True):
                output = f"vector-out-{int(tag)}-{raw}.vdb"
                report = self.report("--resample-field", name, output,
                    "--field-target-transform", *deformation, "--field-world-transform", *deformation,
                    "--field-vector-values", "raw" if raw else "semantic")
                self.assert_report(report)
                volume = self.sample(output)
                self.assertEqual(volume.vector_type, tag)
                self.assertTrue(volume.world_space)
                for a, b in zip(volume.value_at((0, 0, 0)), (2, 3, 4) if raw else expected[tag]):
                    self.assertAlmostEqual(a, b, delta=2e-14)
                self.assertEqual(volume.background.tolist(), list(volume.value_at((0, 0, 0))))
            output = f"target-only-{int(tag)}.vdb"
            self.report("--resample-field", name, output, "--field-target-transform", *deformation)
            self.assertEqual(self.sample(output).value_at((0, 0, 0)), (2, 3, 4))

    def test_unicode_selectors_custom_metadata_literal_dash_h_and_exclusive_source_protection(self):
        self.enabled_only()
        self.dense("入力.vdb", lambda x, y, z: x + y + z, half=1, grid="元-場")
        spec = {"version": 1, "inputs": [{"path": "入力.vdb", "edits": [{"selector": "元-場",
                "metadata": [{"name": "artist", "operation": "set", "type": "string", "value": "é-作品"},
                             {"name": "stableId", "operation": "set", "type": "int64", "value": 9007199254740993}]}]}]}
        (self.directory / "spec.json").write_text(json.dumps(spec, ensure_ascii=False), encoding="utf-8")
        self.call("--author-file", "spec.json", "作品.vdb")
        self.protect("作品.vdb")
        parent = self.directory / "再標本化-é"
        parent.mkdir()
        output = parent / "結果.vdb"
        report = self.report("--resample-field", "作品.vdb", output,
                             "--input-grid", "元-場", "--grid-name", "標本")
        self.assert_report(report)
        self.assertTrue(report["exactCopy"])
        inspection = self.report("--inspect", output)["grids"][0]
        self.assertEqual(inspection["name"], "標本")
        metadata = {item["name"]: item for item in inspection["metadata"]}
        self.assertEqual(metadata["artist"]["value"], "é-作品")
        self.assertEqual(metadata["stableId"]["value"], "9007199254740993")
        previous = output.read_bytes()
        self.call("--resample-field", "作品.vdb", output, succeeds=False)
        self.assertEqual(output.read_bytes(), previous)
        self.call("--resample-field", "作品.vdb", "作品.vdb", succeeds=False)
        (self.directory / "-h").write_bytes((self.directory / "作品.vdb").read_bytes())
        self.assertTrue(self.report("--resample-field", "./-h", "literal.vdb")["exactCopy"])

    def test_caps_semantic_rejections_and_invalid_maps_leave_no_output(self):
        self.enabled_only()
        self.dense("source.vdb", lambda x, y, z: 1., half=0)
        for flags in (("--max-input-bytes", 1024), ("--max-voxels", 512),
                      ("--max-field-resampling-work", 512), ("--max-field-resampling-metadata", 1),
                      ("--grid", "missing"),
                      ("--field-world-transform", 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1),
                      ("--field-target-transform", 1, 1, 0, 0, 0, 1e-10, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1)):
            with self.subTest(flags=flags):
                self.call("--resample-field", "source.vdb", "bad.vdb", *flags, succeeds=False)
                self.assertFalse((self.directory / "bad.vdb").exists())
        self.call("--sphere", "sdf.vdb", "--voxel-size", .25)
        self.call("--resample-field", "sdf.vdb", "bad.vdb", succeeds=False)
        self.dense("local.vdb", lambda x, y, z: (1, 2, 3), half=0,
                   dtype=DenseType.VEC3_FLOAT64, world=False)
        self.call("--resample-field", "local.vdb", "bad.vdb", succeeds=False)
        self.dense("fog.vdb", lambda x, y, z: 1. if (x, y, z) == (1, 0, 0) else 0.,
                   half=1, grid_class=GridClass.FOG_VOLUME)
        target = (1, 0, 0, .5, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1)
        self.call("--resample-field", "fog.vdb", "bad.vdb", "--field-interpolation", "quadratic",
                  "--field-target-transform", *target, succeeds=False)
        self.assertFalse((self.directory / "bad.vdb").exists())


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--tool", required=True, type=Path)
    parser.add_argument("--enabled", required=True, choices=("0", "1"))
    arguments, remaining = parser.parse_known_args()
    FieldResamplingCli.tool = arguments.tool.resolve(strict=True)
    FieldResamplingCli.enabled = arguments.enabled == "1"
    unittest.main(argv=[__file__, *remaining])
