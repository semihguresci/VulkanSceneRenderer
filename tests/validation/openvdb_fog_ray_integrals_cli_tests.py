"""Actual Tool fog protocol; independent stdlib CVDBARR and polynomial oracles."""
import argparse
from fractions import Fraction
import itertools
import json
import math
import os
from pathlib import Path
import struct
import subprocess
import tempfile
import unittest


class FogRayIntegralsCli(unittest.TestCase):
    ordinary_ray = (-1, 0, 0, 7, 0, 0, 0, 2)

    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="container-fog-integrals-cli-")
        self.directory = Path(self.temporary.name)
        self.originals = {}
        self.serial = 0

    def tearDown(self):
        try:
            for path, data in self.originals.items():
                self.assertEqual(path.read_bytes(), data, str(path))
            self.assertFalse(list(self.directory.glob(".container-vdb-*")))
        finally:
            self.temporary.cleanup()

    def enabled_only(self):
        if not self.enabled:
            self.skipTest("OpenVDB is disabled")

    def call(self, *args, succeeds=True, env=None):
        result = subprocess.run([str(self.tool), *map(str, args)], cwd=self.directory,
            capture_output=True, encoding="utf-8", timeout=90, env=env)
        self.assertEqual(result.returncode == 0, succeeds,
            f"{args!r}\n{result.stdout}\n{result.stderr}")
        if not succeeds:
            self.assertEqual(result.stdout, "", "failure emitted partial JSON")
            self.assertTrue(result.stderr.strip())
        return result

    def failure(self, *args, parser=False, diagnostic=None):
        before = set(self.directory.rglob("*"))
        result = self.call("--integrate-fog-rays", *args, succeeds=False)
        if parser:
            self.assertNotIn("ENABLE_OPENVDB=ON", result.stderr)
        if diagnostic:
            self.assertIn(diagnostic, result.stderr)
        self.assertEqual(set(self.directory.rglob("*")), before)
        return result

    def protect(self, path):
        self.originals[path] = path.read_bytes()

    def fixture(self, name="input.vdb", values=(1,), activity=(1,), *,
                dimensions=(1, 1, 1), origin=(0, 0, 0), background=0.,
                dtype=1, grid="density", grid_class=2, linear=None,
                translation=(0., 0., 0.)):
        """Independent version1 CVDBARR frame, native IEEE values, explicit states."""
        self.assertIn(dtype, (0, 1))
        count = math.prod(dimensions)
        self.assertEqual(len(values), count)
        self.assertEqual(len(activity), count)
        self.assertTrue(all(value in (0, 1) for value in activity))
        code, width = ("f", 4) if dtype == 0 else ("d", 8)
        linear = linear or (1., 0., 0., 0., 1., 0., 0., 0., 1.)
        prefix = struct.pack("<8s6I3i3I9d3d", b"CVDBARR\0", 1, dtype,
            grid_class, 0, 1, 0, *origin, *dimensions, *linear, *translation)
        encoded_name = grid.encode("utf-8")
        payload = struct.pack(f"<{count}{code}", *values) + bytes(activity)
        background_bytes = struct.pack("<" + code, background)
        total = len(prefix) + width + struct.calcsize("<IIQIIQQQ") + len(encoded_name) + len(payload)
        tail = struct.pack("<IIQIIQQQ", len(encoded_name), 0, 0, 0, 0, count, len(payload), total)
        self.serial += 1
        encoded = self.directory / f"fixture-{self.serial}.cvarr"
        encoded.write_bytes(prefix + background_bytes + tail + encoded_name + payload)
        self.protect(encoded)
        destination = self.directory / name
        destination.parent.mkdir(parents=True, exist_ok=True)
        self.call("--dense-import", encoded, destination)
        self.protect(destination)
        return destination

    def native_text(self, value, expected=None):
        self.assertEqual(set(value), {"text", "hex", "byteCount"})
        self.assertIs(type(value["byteCount"]), int)
        if value["text"] is None:
            self.assertIs(type(value["hex"]), str)
            raw = bytes.fromhex(value["hex"])
        else:
            self.assertIsNone(value["hex"])
            raw = value["text"].encode("utf-8")
        self.assertEqual(len(raw), value["byteCount"])
        if expected is not None:
            self.assertEqual(raw, expected.encode("utf-8") if isinstance(expected, str) else expected)
        return raw

    def validate(self, report, count):
        self.assertEqual(report["schemaVersion"], 1)
        self.assertEqual(report["operation"], "integrate-fog-rays")
        self.assertTrue(report["reportProduced"])
        self.assertTrue(report["openvdbAvailable"])
        self.assertTrue(report["approximate"])
        self.assertEqual(report["method"], "bounded-rational-cell-simpson")
        self.assertIn("inactive/background participate", report["fieldPolicy"])
        self.assertIn("native Double inverse", report["mappingPolicy"])
        self.assertIn("no ideal-inverse/error certificate", report["mappingPolicy"])
        self.assertIn("no scattering", report["scope"])
        self.assertEqual(len(report["rays"]), count)
        self.assertEqual(len(report["settings"]["rays"]), count)
        for key in ("reportReservationBytes", "reportReservationWork", "cellSegments",
                    "cellSegmentUpperBound", "nativeWorkItems", "combinedWorkUpperBound"):
            self.assertIs(type(report[key]), int, key)
            self.assertGreaterEqual(report[key], 0)
        self.assertEqual(report["combinedWorkUpperBound"], report["nativeWorkItems"] + report["reportReservationWork"])
        requested, native = report["settings"]["requestedLimits"], report["settings"]["nativeLimits"]
        self.assertEqual(requested["maxInputBytes"] - native["maxInputBytes"], report["reportReservationBytes"])
        self.assertEqual(requested["maxWorkItems"] - native["maxWorkItems"], report["reportReservationWork"])
        self.assertLessEqual(report["combinedWorkUpperBound"], requested["maxWorkItems"])
        self.assertLessEqual(report["cellSegments"], report["cellSegmentUpperBound"])
        self.assertLessEqual(report["cellSegmentUpperBound"], requested["maxCellSegments"])
        source = report["source"]
        for key in ("descriptorName", "gridName", "valueType"):
            self.native_text(source[key])
        self.assertIs(type(source["empty"]), bool)
        self.assertIs(type(report["numericallyConstant"]), bool)
        self.assertEqual(len(source["indexToWorld"]), 16)
        self.assertTrue(all(math.isfinite(x) for x in source["indexToWorld"]))
        for key in ("inputActiveVoxelCount", "inputActiveTileCount", "inputStoredLeafCount", "workItems"):
            self.assertIs(type(source[key]), int)
        self.assertEqual(source["workItems"], report["nativeWorkItems"])
        for item in source["metadata"] + source["fileMetadata"]:
            for key in ("name", "type", "value"):
                self.native_text(item[key])
            self.assertIs(type(item["nativeBytes"]), int)
            self.assertIs(type(item["opaque"]), bool)
        self.assertEqual(sum(r["cellSegments"] for r in report["rays"]), report["cellSegments"])
        for r in report["rays"]:
            self.assertAlmostEqual(math.hypot(*r["normalizedDirection"]), 1, delta=2e-14)
            self.assertIs(type(r["cellSegments"]), int)
            self.assertLessEqual(r["cellSegments"], requested["maxCellSegmentsPerRay"])
            for key in ("densityIntegral", "opticalDepth", "transmittance", "absorbedFraction"):
                self.assertTrue(math.isfinite(r[key]))
                self.assertGreaterEqual(r[key], 0)
            self.assertLessEqual(r["transmittance"], 1)
            self.assertLessEqual(r["absorbedFraction"], 1)
            self.assertIn(r["transmissionStatus"], ("represented", "opaque-underflow"))

    def query(self, source, rays=(), flags=(), env=None):
        before = set(self.directory.rglob("*"))
        args = ["--integrate-fog-rays", source]
        for ray in rays:
            args += ["--ray", *ray]
        result = json.loads(self.call(*args, *flags, env=env).stdout)
        self.validate(result, len(rays))
        self.assertEqual(set(self.directory.rglob("*")), before, "read-only query created output")
        return result

    def near(self, value, expected, relative=3e-12):
        self.assertAlmostEqual(value, expected,
            delta=max(abs(expected) * relative, 8 * math.ulp(0.)))

    def test_help_and_strict_parser_aliases_numbers_and_inapplicable_flags(self):
        help_text = self.call("--help").stdout
        for flag in ("--integrate-fog-rays", "--ray", "--grid/--input-grid", "--extinction",
                     "--transmission-policy", "--max-rays", "--max-ray-grids", "--max-ray-work",
                     "--max-ray-metadata", "--max-cell-segments-per-ray", "--max-cell-segments",
                     "--max-voxels", "--max-input-bytes"):
            self.assertIn(flag, help_text)
        for arguments in ((), ("",), ("-h",), ("--help",)):
            self.failure(*arguments, parser=True)
        cases = [(("--grid", "a", "--input-grid", "b"), "duplicate"),
                 (("--transmission-policy", "opaque"), "requires"),
                 (("--extinction", -1), "nonnegative"),
                 (("--extinction", "nan"), "finite"),
                 (("--extinction", "1e-9999"), "finite"),
                 (("--ray-normals", "on"), "unknown/incompatible"),
                 (("--max-ray-spans", 1), "unknown/incompatible"),
                 (("--voxel-size", 1), "unknown/incompatible"),
                 (("--output", "out.vdb"), "unknown/incompatible")]
        integer_flags = {"--max-rays": 1_000_000, "--max-ray-grids": 4096,
                         "--max-ray-work": 10_000_000_000, "--max-ray-metadata": 2**64-1,
                         "--max-cell-segments-per-ray": 1_000_000,
                         "--max-cell-segments": 1_000_000,
                         "--max-voxels": 2**64-1, "--max-input-bytes": 2**64-1}
        for flag, maximum in integer_flags.items():
            cases += [((flag,), "nonempty"), ((flag, 0), "range"),
                      ((flag, maximum+1), "range"), ((flag, "+1"), "unsigned"),
                      ((flag, " 1"), "unsigned"), ((flag, 2, flag, 3), "duplicate")]
        for arguments, expected in cases:
            with self.subTest(arguments=arguments):
                self.failure("missing.vdb", *arguments, parser=True, diagnostic=expected)
        for size in range(8):
            self.failure("missing.vdb", "--ray", *self.ordinary_ray[:size], parser=True)
        for index, value in ((3, 0), (6, -1), (7, 0), (0, "NaN"), (0, "1e9999")):
            ray = list(self.ordinary_ray)
            ray[index] = value
            self.failure("missing.vdb", "--ray", *ray, parser=True)
        self.failure("missing.vdb", "--max-rays", 1,
                     "--ray", *self.ordinary_ray, "--ray", *self.ordinary_ray, parser=True)

    def test_disabled_native_availability_precedes_paths_and_report_capacities(self):
        if self.enabled:
            self.skipTest("OpenVDB is enabled")
        for flags in ((), ("--max-input-bytes", 1), ("--max-ray-work", 1),
                      ("--max-ray-metadata", 2**64-1)):
            self.failure("missing-parent/input.vdb", *flags,
                         diagnostic="ENABLE_OPENVDB=ON")

    def test_real_default_tent_and_inactive_values_have_analytic_world_integrals(self):
        self.enabled_only()
        for dtype in (0, 1):
            for activity in (0, 1):
                with self.subTest(dtype=dtype, activity=activity):
                    source = self.fixture(f"tent-{dtype}-{activity}.vdb", (8,), (activity,), dtype=dtype)
                    # No work/image/limit override: actual default route must work.
                    result = self.query(source, (self.ordinary_ray,))
                    self.near(result["rays"][0]["densityIntegral"], 8)
                    self.near(result["rays"][0]["opticalDepth"], 8)
                    self.near(result["rays"][0]["transmittance"], math.exp(-8))
                    self.assertEqual(result["source"]["empty"], not activity)
                    self.assertFalse(result["numericallyConstant"])

    def test_cubic_and_general_multilinear_integrals_use_independent_antiderivative(self):
        self.enabled_only()
        source = self.fixture("cubic.vdb", [8. if (x, y, z) == (1, 1, 1) else 0.
            for z, y, x in itertools.product(range(2), repeat=3)], [1]*8, dimensions=(2, 2, 2))
        diagonal = self.query(source, ((0, 0, 0, 1, 1, 1, 0, math.sqrt(3)),))
        self.near(diagonal["rays"][0]["densityIntegral"], 2 * math.sqrt(3))
        # rho=1+2x+3y+4z+5xy+6xz+7yz+8xyz, integrated as an independently
        # expanded polynomial in u; no Simpson/trapezoid sampling oracle.
        coefficients = (1, 2, 3, 4, 5, 6, 7, 8)
        masks = (0, 1, 2, 4, 3, 5, 6, 7)
        def rho(x, y, z):
            return 1+2*x+3*y+4*z+5*x*y+6*x*z+7*y*z+8*x*y*z
        source = self.fixture("polynomial.vdb", [rho(x, y, z)
            for z, y, x in itertools.product(range(2), repeat=3)], [1]*8, dimensions=(2, 2, 2))
        start = tuple(map(Fraction, (Fraction(1, 8), Fraction(1, 4), Fraction(3, 8))))
        finish = (Fraction(7, 8), Fraction(3, 4), Fraction(5, 8))
        delta = tuple(b-a for a, b in zip(start, finish))
        polynomial = [Fraction(0)]*4
        for coefficient, mask in zip(coefficients, masks):
            term = [Fraction(coefficient)]
            for axis in range(3):
                if mask & (1 << axis):
                    multiplied = [Fraction(0)]*(len(term)+1)
                    for degree, value in enumerate(term):
                        multiplied[degree] += value*start[axis]
                        multiplied[degree+1] += value*delta[axis]
                    term = multiplied
            for degree, value in enumerate(term):
                polynomial[degree] += value
        distance = math.sqrt(sum(float(x*x) for x in delta))
        expected = float(sum(value/Fraction(degree+1) for degree, value in enumerate(polynomial))) * distance
        result = self.query(source, ((*map(float, start), *map(float, delta), 0, distance),))
        self.near(result["rays"][0]["densityIntegral"], expected)

    def test_background_holes_and_activity_empty_constant_fields_are_not_vacuum(self):
        self.enabled_only()
        source = self.fixture("hole.vdb", (0,), (0,), background=2)
        result = self.query(source, ((-2, 0, 0, 3, 0, 0, 0, 4),))
        self.assertTrue(result["source"]["empty"])
        self.assertFalse(result["numericallyConstant"])
        self.assertEqual(result["backgroundDensity"], 2)
        self.near(result["rays"][0]["densityIntegral"], 6)
        for activity in (0, 1):
            source = self.fixture(f"constant-{activity}.vdb", (3,), (activity,), background=3)
            result = self.query(source, ((7, 8, 9, -3, 0, 0, 0, 4),))
            self.assertTrue(result["numericallyConstant"])
            self.assertEqual(result["source"]["empty"], not activity)
            self.assertEqual(result["rays"][0]["cellSegments"], 0)
            self.near(result["rays"][0]["densityIntegral"], 12)

    def test_direction_reversal_partition_and_scaled_reflected_maps(self):
        self.enabled_only()
        source = self.fixture(values=(8,), activity=(1,))
        rays = (self.ordinary_ray, (1, 0, 0, -3, 0, 0, 0, 2),
                (-1, 0, 0, 7, 0, 0, 0, .37), (-1, 0, 0, 7, 0, 0, .37, 2))
        result = self.query(source, rays, flags=("--max-ray-work", 10_000_000_000))
        self.near(result["rays"][0]["densityIntegral"], 8)
        self.near(result["rays"][1]["densityIntegral"], 8)
        self.near(result["rays"][2]["densityIntegral"], 4*.37**2)
        self.near(sum(r["densityIntegral"] for r in result["rays"][2:]), 8)
        for h in (.25, 1., 1e6):
            # Reflected90-degree rotation, WORLD columns (0,-h,0),(-h,0,0),(0,0,h).
            linear = (0., -h, 0., -h, 0., 0., 0., 0., h)
            translation = (32*h, -16*h, 8*h)
            source = self.fixture(f"map-{h}.vdb", (2,), (0,), linear=linear, translation=translation)
            origin = (translation[0], translation[1]+h, translation[2])
            result = self.query(source, ((*origin, 0, -7, 0, 0, 2*h),),
                                flags=("--extinction", 1/h))
            self.near(result["rays"][0]["densityIntegral"], 2*h)
            self.near(result["rays"][0]["opticalDepth"], 2)
            self.assertEqual(result["source"]["indexToWorld"], [
                0., -h, 0., translation[0], -h, 0., 0., translation[1],
                0., 0., h, translation[2], 0., 0., 0., 1.])

    def test_beer_thin_vacuum_zero_extinction_and_explicit_opaque_status(self):
        self.enabled_only()
        ray = (0, 0, 0, 1, 0, 0, 0, 1)
        tiny = math.ldexp(1., -1060)
        source = self.fixture("thin.vdb", (tiny,), (0,), background=tiny)
        result = self.query(source, (ray,))
        self.assertEqual(result["rays"][0]["transmittance"], 1)
        self.assertEqual(result["rays"][0]["absorbedFraction"], tiny)
        source = self.fixture("opaque.vdb", (1000,), (0,), background=1000)
        self.failure(source, "--ray", *ray, diagnostic="transmission underflows")
        result = self.query(source, (ray,), flags=("--transmission-policy", "allow-opaque-underflow"))
        self.assertEqual(result["rays"][0]["transmissionStatus"], "opaque-underflow")
        self.assertEqual(result["rays"][0]["transmittance"], 0)
        self.assertEqual(result["rays"][0]["absorbedFraction"], 1)
        result = self.query(source, (ray,), flags=("--extinction", 0))
        self.assertEqual(result["rays"][0]["densityIntegral"], 1000)
        self.assertEqual(result["rays"][0]["transmittance"], 1)
        self.assertEqual(result["rays"][0]["absorbedFraction"], 0)
        source = self.fixture("vacuum.vdb", (0,), (0,))
        result = self.query(source, (ray,))
        self.assertEqual(result["rays"][0]["densityIntegral"], 0)
        self.assertEqual(result["rays"][0]["transmittance"], 1)

    def test_exact_uint64_limits_and_joint_report_native_reservations(self):
        self.enabled_only()
        source = self.fixture()
        result = self.query(source, (self.ordinary_ray,), flags=(
            "--max-voxels", 2**64-1, "--max-input-bytes", 2**64-1,
            "--max-ray-work", 10_000_000_000))
        requested, native = result["settings"]["requestedLimits"], result["settings"]["nativeLimits"]
        self.assertEqual(requested["maxVoxelCount"], 2**64-1)
        self.assertEqual(native["maxVoxelCount"], 2**64-1)
        self.assertEqual(requested["maxInputBytes"], 2**64-1)
        self.assertIs(type(native["maxInputBytes"]), int)
        self.assertGreater(native["maxInputBytes"], 2**53)
        for flags in (("--max-input-bytes", 1), ("--max-ray-work", 1),
                      ("--max-ray-metadata", 1), ("--max-voxels", 511),
                      ("--max-cell-segments-per-ray", 1), ("--max-cell-segments", 1),
                      ("--max-ray-metadata", 2**64-1)):
            self.failure(source, "--ray", *self.ordinary_ray, *flags)

    def test_source_validation_empty_queries_and_final_range_errors_have_no_stdout(self):
        self.enabled_only()
        source = self.fixture("negative.vdb", (-1,), (0,))
        self.failure(source)  # Empty batch must still census inactive negatives.
        source = self.fixture("sdf.vdb", (1,), (1,), grid_class=1)
        self.failure(source)
        source = self.fixture("nonuniform.vdb", linear=(1., 0., 0., 0., 2., 0., 0., 0., 1.))
        self.failure(source)
        maximum = float.fromhex("0x1.fffffffffffffp+1023")
        source = self.fixture("maximum.vdb", (maximum,), (0,), background=maximum)
        self.failure(source, "--ray", *self.ordinary_ray, "--extinction", 0)
        result = self.query(source, ((0, 0, 0, 1, 0, 0, 0, .25),), flags=("--extinction", 0))
        self.near(result["rays"][0]["densityIntegral"], maximum*.25)
        source = self.fixture("denorm.vdb", (math.ulp(0.),), (0,), background=math.ulp(0.))
        self.failure(source, "--ray", 0, 0, 0, 1, 0, 0, 0, .75, "--extinction", 0)
        source = self.fixture("ordinary.vdb")
        result = self.query(source)
        self.assertEqual(result["cellSegments"], 0)
        self.assertGreater(result["nativeWorkItems"], 0)
        corrupt = self.directory/"corrupt.vdb"
        corrupt.write_bytes(b"not a native VDB archive")
        self.protect(corrupt)
        self.failure(corrupt)

    def test_author_file_metadata_catalog_and_unicode_native_source_temp_are_preserved(self):
        self.enabled_only()
        source = self.fixture("入力-é/密度.vdb")
        spec = self.directory/"bundle.json"
        value = "quoted\"\\nul\0é"
        spec.write_text(json.dumps({"version": 1, "inputs": [{"path": str(source),
            "edits": [{"selector": "density", "newName": "密度", "metadata": [
                {"name": "artist", "operation": "set", "type": "string", "value": value},
                {"name": "exact_id", "operation": "set", "type": "int64", "value": 9007199254740993}]}]}],
            "metadata": [{"name": "scene", "operation": "set", "type": "string", "value": "file_é"}]}), encoding="utf-8")
        self.protect(spec)
        authored = self.directory/"authored.vdb"
        self.call("--author-file", spec, authored)
        self.protect(authored)
        result = self.query(authored, (self.ordinary_ray,), flags=("--grid", "descriptor:密度"))
        self.native_text(result["source"]["gridName"], "密度")
        metadata = {self.native_text(item["name"]): item for item in result["source"]["metadata"]}
        self.native_text(metadata[b"artist"]["value"], value)
        self.assertEqual(metadata[b"artist"]["nativeBytes"], len(value.encode("utf-8")))
        self.native_text(metadata[b"exact_id"]["value"], "9007199254740993")
        files = {self.native_text(item["name"]): item for item in result["source"]["fileMetadata"]}
        self.native_text(files[b"scene"]["value"], "file_é")
        self.failure(authored, "--grid", "missing")
        self.query(authored, flags=("--input-grid", "descriptor:密度"))
        literal = self.directory/"-h"
        literal.write_bytes(source.read_bytes())
        self.protect(literal)
        self.query("./-h", (self.ordinary_ray,))
        temporary = self.directory/"一時_é"
        temporary.mkdir()
        environment = os.environ.copy()
        environment.update(TEMP=str(temporary), TMP=str(temporary))
        result = self.query(source, (self.ordinary_ray,), env=environment)
        self.near(result["rays"][0]["densityIntegral"], 1)
        self.assertEqual(list(temporary.iterdir()), [])


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--tool", type=Path, required=True)
    parser.add_argument("--enabled", choices=("0", "1"), required=True)
    args, unittest_args = parser.parse_known_args()
    FogRayIntegralsCli.tool = args.tool.resolve()
    FogRayIntegralsCli.enabled = args.enabled == "1"
    unittest.main(argv=[__file__, *unittest_args])
