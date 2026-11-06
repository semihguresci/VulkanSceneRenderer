"""Real-tool bounded diagnostic reports and union masks through public fixtures.

Exit zero means a report exists, independently of Fail/Unavailable check status.
Finite scalar/vector fixtures use CVDBARR; exact Int64 fields use native point
counts followed by exact native fill. Standard point JSON only makes active
records; malformed/inactive native layouts remain covered by native tests.
"""

from array import array
import argparse
import itertools
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "python"))
from container_openvdb import DenseType, DenseVolume, GridClass, write_dense_array


class DiagnosticsCli(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="container-diagnostics-cli-")
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

    def enabled_only(self):
        if not self.enabled:
            self.skipTest("OpenVDB is disabled")

    def call(self, *args, succeeds=True):
        result = subprocess.run([str(self.tool), *map(str, args)], cwd=self.directory,
                                capture_output=True, text=True, encoding="utf-8", timeout=90)
        self.assertEqual(result.returncode == 0, succeeds,
                         f"{args!r}\n{result.stdout}\n{result.stderr}")
        return result

    def report(self, *args):
        return json.loads(self.call(*args).stdout)

    def protect(self, name):
        path = self.directory / name
        self.originals[path] = path.read_bytes()

    def write_json(self, name, data):
        (self.directory / name).write_text(json.dumps(data, ensure_ascii=False), encoding="utf-8")

    def failure(self, *args, output="bad.vdb", parser=False, diagnostic=None):
        result = self.call(*args, succeeds=False)
        if parser:
            self.assertNotIn("ENABLE_OPENVDB=ON", result.stderr)
        if diagnostic:
            self.assertIn(diagnostic, result.stderr)
        self.assertFalse((self.directory / output).exists(), repr(args))
        return result

    def fixture(self, name="field.vdb", values=(-2., 0., 2., 5.), active=(1, 0, 1, 0),
                *, dtype=DenseType.FLOAT64, background=0., grid="field", origin=(0, 0, 0),
                dimensions=None, h=1., grid_class=GridClass.UNKNOWN):
        flat = list(values) if dtype.components == 1 else [x for value in values for x in value]
        bg = [background] if dtype.components == 1 else list(background)
        volume = DenseVolume(dtype, dimensions or (len(values), 1, 1), array(dtype.typecode, flat),
                             bytearray(active), origin=origin, background=array(dtype.typecode, bg),
                             grid_name=grid, grid_class=grid_class,
                             linear=(h, 0, 0, 0, h, 0, 0, 0, h))
        encoded = str(name) + ".cvarr"
        write_dense_array(self.directory / encoded, volume)
        self.call("--dense-import", encoded, name)
        self.protect(encoded)
        self.protect(name)
        return volume

    def plane(self, name, radius=2, slope=1., *, h=1., only_center=False):
        values, active = [], []
        for z, y, x in itertools.product(range(-radius, radius + 1), repeat=3):
            values.append((slope*x + .25)*h)
            active.append(not only_center or (x, y, z) == (0, 0, 0))
        return self.fixture(name, values, active, background=16*h, origin=(-radius,)*3,
                            dimensions=(2*radius+1,)*3, h=h, grid_class=GridClass.LEVEL_SET)

    @staticmethod
    def check(report, identifier):
        found = [row for row in report["checks"] if row["id"] == identifier]
        if len(found) != 1:
            raise AssertionError(f"expected one {identifier!r} check: {report['checks']!r}")
        return found[0]

    def string_field(self, item, field, original=None):
        if original is None:
            self.assertIsInstance(item[field], str, field)
            original = item[field].encode("utf-8")
        self.assertEqual(item[field + "BytesHex"], original[:4096].hex())
        self.assertEqual(item[field + "ByteCount"], len(original))
        self.assertIs(item[field + "Truncated"], len(original) > 4096)

    def validate(self, report, *, operation="diagnose", profile="stored"):
        self.assertEqual(report["schemaVersion"], 1)
        self.assertIs(report["openvdbAvailable"], True)
        self.assertIs(report["reportProduced"], True)
        self.assertEqual(report["operation"], operation)
        self.assertEqual(report["profile"], profile)
        for name in ("descriptorName", "gridName", "valueType", "gridClass", "vectorType"):
            if report[name] is not None:
                self.string_field(report, name)
        for name in ("leafCount", "storedLeafSlots", "activeVoxelCount", "activeTileCount",
                     "inactiveTileCount", "decodedBytes", "workUpperBound", "maskVoxelCount"):
            self.assertIs(type(report[name]), int, name)
            self.assertGreaterEqual(report[name], 0)
        for name in ("inWorldSpace", "affine", "allRequestedPassed", "complete", "unlocatedFailures"):
            self.assertIs(type(report[name]), bool, name)
        if report["affine"]:
            self.assertEqual(len(report["indexToWorld"]), 16)
        ids = set()
        for row in report["checks"]:
            self.string_field(row, "id")
            self.string_field(row, "description")
            self.assertNotIn(row["id"], ids)
            ids.add(row["id"])
            self.assertIn(row["status"], ("pass", "fail", "unavailable", "not-applicable"))
            self.assertIs(type(row["coverageComplete"]), bool)
            for name in ("examinedLeafVoxels", "examinedTiles", "examinedPoints", "failedLeafVoxels",
                         "failedTiles", "failedTileVoxelVolume", "failedPoints", "candidateCount",
                         "evaluatedCount", "missingFootprintCount", "nonfiniteFootprintCount",
                         "inactiveContinuationCount"):
                self.assertIs(type(row[name]), int, name)
                self.assertGreaterEqual(row[name], 0)
            self.assertLessEqual(len(row["examples"]), report["settings"]["caps"]["maxExamples"])
            for example in row["examples"]:
                self.string_field(example, "detail")
                if example["index"] is not None:
                    self.assertEqual(len(example["index"]), 3)
                    self.assertTrue(all(type(n) is int for n in example["index"]))
                if example["pointOrdinal"] is not None:
                    self.assertIs(type(example["pointOrdinal"]), int)
        for diagnostic in report["diagnostics"]:
            if diagnostic["text"] is not None:
                raw = diagnostic["text"].encode("utf-8")
                self.assertEqual(diagnostic["bytesHex"], raw[:4096].hex())
                self.assertEqual(diagnostic["byteCount"], len(raw))
        if report["mask"] is not None:
            self.string_field(report["mask"], "gridName")
            self.string_field(report["mask"], "gridClass")
            self.assertEqual(report["mask"]["valueType"], "bool")
            self.assertEqual(report["mask"]["activeVoxelCount"], report["maskVoxelCount"])
        caps = report["settings"]["caps"]
        self.assertEqual(caps["nativeMaxInputBytes"] + caps["reportReservedBytes"], caps["maxInputBytes"])
        self.assertGreater(caps["reportReservedBytes"], 0)
        self.assertEqual(caps["nativeMaxWork"] + caps["reportReservedWork"], caps["maxWork"])
        self.assertGreater(caps["reportReservedWork"], 0)

    def inspect_mask(self, output, count, name="diagnostic_failures"):
        report = self.report("--inspect", output, "--input-grid", "descriptor:" + name)
        self.assertEqual(len(report["grids"]), 1)
        grid = report["grids"][0]
        self.assertEqual(grid["name"], name)
        self.assertEqual(grid["valueType"], "bool")
        self.assertEqual(grid["gridClass"], "unknown")
        self.assertEqual(grid["recorded"]["activeVoxelCount"], count)

    def point_fixture(self):
        self.write_json("points.json", [{"position": p} for p in ((8.125, 0, 0), (.25, 0, 0), (.125, 0, 0))])
        self.call("--points", "points.json", "raw.vdb", "--voxel-size", 1)
        query = self.report("--point-neighbors", "raw.vdb", "--neighbor-query", 0, 0, 0, 16)
        selected = [False] * 3
        for hit in query["queries"][0]["hits"]:
            selected[hit["reference"]["ordinal"]] = hit["positionWorld"][0] < 1
        self.assertEqual(sum(selected), 2)
        edits = {"version": 1, "attributes": [
            {"op": "add", "name": "id", "type": "int64", "values": [2**53+1]},
            {"op": "add", "name": "label", "type": "string", "values": ["valid-\u0000-世界"]}],
            "groups": [{"op": "add", "name": "A", "values": selected},
                       {"op": "add", "name": "B", "values": [True]},
                       {"op": "add", "name": "empty", "values": [False]}]}
        self.write_json("edits.json", edits)
        self.call("--edit-point-attributes", "raw.vdb", "edits.json", "points.vdb")
        self.protect("raw.vdb")
        self.protect("points.vdb")

    def test_help_and_required_path_arity_are_strict(self):
        help_text = self.call("--help").stdout
        for flag in ("--diagnose", "--diagnostic-mask", "--diagnostic-profile", "--diagnostic-range",
                     "--diagnostic-gradient", "--diagnostic-mask-name", "--max-diagnostic-examples"):
            self.assertIn(flag, help_text)
        for args in (("--diagnose",), ("--diagnose", ""), ("--diagnose", "-h"),
                     ("--diagnose", "--diagnostic-profile", "stored"),
                     ("--diagnostic-mask",), ("--diagnostic-mask", "source"),
                     ("--diagnostic-mask", "source", "-h"), ("--diagnostic-mask", "", "bad.vdb")):
            with self.subTest(args=args):
                self.failure(*args, parser=True, diagnostic="requires")
        self.assertEqual(list(self.directory.iterdir()), [])

    def test_duplicate_options_aliases_missing_values_and_scope_grammar(self):
        base = ("--diagnose", "missing.vdb")
        cases = [("--diagnostic-profile", "stored", "--diagnostic-profile", "stored"),
                 ("--grid", "a", "--input-grid", "b"),
                 ("--diagnostic-profile", "points", "--diagnostic-attribute", "id", "--diagnostic-attribute", "id"),
                 ("--diagnostic-profile", "points", "--point-any-group", "A", "--any-group", "A"),
                 ("--diagnostic-profile", "points", "--point-all-group", "A", "--all-group", "A"),
                 ("--diagnostic-profile", "points", "--point-none-group", "A", "--exclude-group", "A"),
                 ("--diagnostic-profile", "liquid"), ("--diagnostic-gradient", "bad"),
                 ("--diagnostic-gradient", "first"), ("--diagnostic-range-scope", "stored"),
                 ("--diagnostic-range-scope", "active"), ("--diagnostic-tolerance", 0),
                 ("--diagnostic-norm-range", .5, 1.5), ("--diagnostic-footprint", "active"),
                 ("--diagnostic-mask-name", "mask"), ("--diagnostic-attribute", "id"),
                 ("--point-any-group", "A"), ("--max-points", 3),
                 ("--diagnostic-profile", "points", "--diagnostic-attribute", "P"),
                 ("--diagnostic-profile", "points", "--diagnostic-range", 0, 1),
                 ("--diagnostic-profile", "sdf", "--diagnostic-gradient", "none", "--diagnostic-footprint", "stored"),
                 ("--diagnostic-range", 1, -1), ("--diagnostic-range", "nan", 1),
                 ("--diagnostic-range", 9007199254740993, 9007199254740993),
                 ("--diagnostic-profile", "sdf", "--diagnostic-gradient", "first", "--diagnostic-norm-range", -1, 1),
                 ("--max-diagnostic-examples", -1), ("--max-diagnostic-examples", 129),
                 ("--max-diagnostic-work", 0), ("--max-diagnostic-work", 10000000001),
                 ("--max-diagnostic-grids", 4097), ("--max-diagnostic-metadata", 0),
                 ("--max-voxels", "+1"), ("--max-input-bytes", 0),
                 ("--voxel-size", .1), ("--grid-name", "ignored"), ("--stats",),
                 ("--max-triangles", 1), ("--diagnostic-profile", "points", "--max-edits", 1)]
        for flag in ("--diagnostic-profile", "--diagnostic-range", "--diagnostic-range-scope",
                     "--diagnostic-gradient", "--diagnostic-norm-range", "--diagnostic-footprint",
                     "--diagnostic-tolerance", "--diagnostic-attribute", "--point-any-group",
                     "--max-diagnostic-work", "--max-diagnostic-grids", "--max-diagnostic-metadata",
                     "--max-diagnostic-examples", "--grid", "--max-input-bytes"):
            cases += [(flag,), (flag, "--grid", "field")]
        for extra in cases:
            with self.subTest(extra=extra):
                self.failure(*base, *extra, parser=True)
        self.assertEqual(list(self.directory.iterdir()), [])

    def test_disabled_reports_availability_before_paths_or_report_reservation(self):
        if self.enabled:
            self.skipTest("OpenVDB is enabled")
        for command in (("--diagnose", "不存在.vdb"),
                        ("--diagnostic-mask", "不存在.vdb", "missing/output.vdb")):
            for extra in ((), ("--max-input-bytes", 1, "--max-diagnostic-examples", 0),
                          ("--diagnostic-profile", "points", "--diagnostic-attribute", "missing",
                           "--max-string-bytes", 1)):
                result = self.call(*command, *extra, succeeds=False)
                self.assertIn("ENABLE_OPENVDB=ON", result.stderr)
        self.assertEqual(list(self.directory.iterdir()), [])

    def test_readonly_success_and_failure_reports_exit_zero_without_creating_files(self):
        self.enabled_only()
        self.fixture()
        before = set(self.directory.iterdir())
        plain = self.report("--diagnose", "field.vdb")
        self.validate(plain)
        self.assertTrue(plain["complete"])
        self.assertTrue(plain["allRequestedPassed"])
        self.assertEqual(self.check(plain, "finite")["status"], "pass")
        self.assertEqual(self.check(plain, "gradient")["status"], "not-applicable")
        failed = self.report("--diagnose", "field.vdb", "--diagnostic-range", -1, 1)
        self.validate(failed)
        self.assertEqual(self.check(failed, "range")["failedLeafVoxels"], 2)
        self.assertTrue(failed["complete"])
        self.assertFalse(failed["allRequestedPassed"])
        self.assertEqual(failed["maskVoxelCount"], 2)
        self.assertIsNone(failed["mask"])
        self.assertIsNone(failed["points"])
        self.assertEqual(failed["settings"]["componentRange"], [-1, 1])
        self.assertEqual(set(self.directory.iterdir()), before)

    def test_writer_native_bool_mask_and_stored_range_have_independent_activity(self):
        self.enabled_only()
        self.fixture()
        report = self.report("--diagnostic-mask", "field.vdb", "mask.vdb",
                             "--diagnostic-range", -1, 1, "--diagnostic-range-scope", "stored",
                             "--diagnostic-mask-name", "failures")
        self.validate(report, operation="diagnostic-mask")
        self.assertEqual(self.check(report, "range")["failedLeafVoxels"], 3)
        self.assertEqual(report["maskVoxelCount"], 3)
        self.assertEqual(report["mask"]["gridName"], "failures")
        self.inspect_mask("mask.vdb", 3, "failures")

    def test_native_float_double_and_vector_ranges_are_componentwise(self):
        self.enabled_only()
        for index, dtype in enumerate((DenseType.FLOAT32, DenseType.FLOAT64,
                                       DenseType.VEC3_FLOAT32, DenseType.VEC3_FLOAT64)):
            vector = dtype.components == 3
            values = [(.75, .75, .75), (1.125, -.125, 0)] if vector else [.75, 1.125]
            name = f"native-{index}.vdb"
            self.fixture(name, values, [1, 1], dtype=dtype, background=(0, 0, 0) if vector else 0)
            report = self.report("--diagnose", name, "--diagnostic-range", -1, 1)
            self.validate(report)
            self.assertEqual(report["valueType"], ("float", "double", "vec3s", "vec3d")[index])
            self.assertEqual(self.check(report, "finite")["status"], "pass")
            self.assertEqual(self.check(report, "range")["failedLeafVoxels"], 1)
            self.assertEqual(report["maskVoxelCount"], 1)

    def test_exact_int64_field_ranges_do_not_round_native_values_through_double(self):
        self.enabled_only()
        self.write_json("one.json", [{"position": [.125, 0, 0]}])
        self.call("--points", "one.json", "one.vdb", "--voxel-size", 1)
        self.call("--point-mask", "one.vdb", "counts.vdb", "--point-counts")
        self.protect("one.vdb")
        self.protect("counts.vdb")
        for serial, native, endpoint in ((0, 2**53+1, 2**53), (1, 2**63-1, 2**63)):
            name = f"int64-{serial}.vdb"
            self.call("--edit-topology", "counts.vdb", name, "--grid", "point_count",
                      "--edit-operation", "fill-box", "--native-value", native,
                      "--index-box", 0, 0, 0, 0, 0, 0, "--grid-name", "exact")
            self.protect(name)
            report = self.report("--diagnose", name, "--diagnostic-range", endpoint, endpoint)
            self.validate(report)
            self.assertEqual(report["valueType"], "int64")
            self.assertEqual(self.check(report, "range")["failedLeafVoxels"], 1)
            self.assertEqual(report["settings"]["componentRange"], [endpoint, endpoint])

    def test_genuine_sdf_and_normalized_fog_profiles_differ_from_general_density(self):
        self.enabled_only()
        self.call("--sphere", "sphere.vdb", "--radius", 1, "--voxel-size", .25)
        self.protect("sphere.vdb")
        sdf = self.report("--diagnose", "sphere.vdb", "--diagnostic-profile", "sdf")
        self.validate(sdf, profile="sdf")
        self.assertTrue(sdf["complete"])
        self.assertTrue(sdf["allRequestedPassed"])
        self.call("--fog", "sphere.vdb", "fog.vdb")
        self.protect("fog.vdb")
        fog = self.report("--diagnose", "fog.vdb", "--diagnostic-profile", "fog")
        self.validate(fog, profile="fog")
        self.assertTrue(fog["allRequestedPassed"])
        self.fixture("physical.vdb", [0, .5, 2.], [1, 1, 1], grid_class=GridClass.FOG_VOLUME)
        self.assertTrue(self.report("--diagnose", "physical.vdb")["allRequestedPassed"])
        bounded = self.report("--diagnose", "physical.vdb", "--diagnostic-profile", "fog")
        self.assertEqual(self.check(bounded, "fog-active-range")["failedLeafVoxels"], 1)
        self.assertFalse(bounded["allRequestedPassed"])

    def test_first_and_weno_use_independent_seven_and_nineteen_sample_coverage(self):
        self.enabled_only()
        self.plane("first.vdb", slope=2., h=.5)
        first = self.report("--diagnostic-mask", "first.vdb", "first-mask.vdb",
                            "--diagnostic-profile", "sdf", "--diagnostic-gradient", "first")
        self.validate(first, operation="diagnostic-mask", profile="sdf")
        gradient = self.check(first, "gradient")
        self.assertEqual(gradient["candidateCount"], 125)
        self.assertEqual(gradient["evaluatedCount"], 27)
        self.assertEqual(gradient["missingFootprintCount"], 98)
        self.assertEqual(gradient["failedLeafVoxels"], 27)
        self.assertFalse(gradient["coverageComplete"])
        self.assertFalse(first["complete"])
        self.inspect_mask("first-mask.vdb", 27)
        self.plane("weno.vdb", radius=4, slope=1.25)
        weno = self.report("--diagnose", "weno.vdb", "--diagnostic-profile", "sdf",
                           "--diagnostic-gradient", "weno", "--diagnostic-norm-range", 1.2, 1.3)
        self.validate(weno, profile="sdf")
        gradient = self.check(weno, "gradient")
        self.assertEqual(gradient["candidateCount"], 729)
        self.assertEqual(gradient["evaluatedCount"], 18)
        self.assertEqual(gradient["failedLeafVoxels"], 0)
        self.assertEqual(gradient["status"], "unavailable")
        self.assertFalse(weno["complete"])
        self.assertEqual(weno["settings"]["gradientNormRange"], [1.2, 1.3])

    def test_active_footprints_and_explicit_stored_continuation_are_not_conflated(self):
        self.enabled_only()
        self.plane("continuation.vdb", radius=1, only_center=True)
        base = ("--diagnose", "continuation.vdb", "--diagnostic-profile", "sdf",
                "--diagnostic-gradient", "first", "--diagnostic-norm-range", .99, 1.01)
        active = self.report(*base)
        self.assertEqual(self.check(active, "gradient")["status"], "unavailable")
        self.assertEqual(self.check(active, "gradient")["missingFootprintCount"], 1)
        stored = self.report(*base, "--diagnostic-footprint", "stored")
        self.validate(stored, profile="sdf")
        gradient = self.check(stored, "gradient")
        self.assertEqual(gradient["status"], "pass")
        self.assertEqual(gradient["evaluatedCount"], 1)
        self.assertEqual(gradient["inactiveContinuationCount"], 1)
        self.assertFalse(stored["allRequestedPassed"], "nonbackground inactive phi violates symmetric-band profile")
        self.fixture("noncrossing.vdb", [.25], [1], background=3., grid_class=GridClass.LEVEL_SET)
        noncrossing = self.report("--diagnose", "noncrossing.vdb", "--diagnostic-profile", "sdf",
                                  "--diagnostic-gradient", "weno")
        self.assertEqual(self.check(noncrossing, "gradient")["missingFootprintCount"], 1)
        self.assertEqual(self.check(noncrossing, "gradient")["status"], "unavailable")

    def test_point_groups_exact_ids_missing_attributes_and_empty_native_archive(self):
        self.enabled_only()
        self.point_fixture()
        args = ("--diagnostic-profile", "points", "--diagnostic-attribute", "id",
                "--diagnostic-attribute", "missing", "--diagnostic-range", 2**53, 2**53,
                "--point-any-group", "A", "--all-group", "B")
        report = self.report("--diagnostic-mask", "points.vdb", "points-mask.vdb", *args)
        self.validate(report, operation="diagnostic-mask", profile="points")
        self.assertEqual(report["points"]["total"], 3)
        self.assertEqual(report["points"]["selected"], 2)
        self.assertEqual(report["points"]["selectedActive"], 2)
        self.assertEqual(report["points"]["selectedInactive"], 0)
        self.assertTrue(report["points"]["countsValid"])
        self.assertEqual(self.check(report, "point-attribute:id")["failedPoints"], 2)
        self.assertEqual(self.check(report, "point-attribute:missing")["status"], "unavailable")
        self.assertFalse(report["complete"])
        self.assertEqual(report["maskVoxelCount"], 1)
        self.inspect_mask("points-mask.vdb", 1)
        missing_group = self.report("--diagnose", "points.vdb", "--diagnostic-profile", "points",
                                    "--point-none-group", "absent", "--diagnostic-attribute", "id")
        self.assertEqual(self.check(missing_group, "point-groups")["status"], "fail")
        self.assertEqual(self.check(missing_group, "point-position")["status"], "unavailable")
        self.call("--select-points", "points.vdb", "archive.vdb", "--any-group", "empty")
        self.protect("archive.vdb")
        archived = self.report("--diagnose", "archive.vdb", "--diagnostic-profile", "points",
                               "--diagnostic-attribute", "id")
        self.validate(archived, profile="points")
        self.assertTrue(archived["points"]["archivedSchema"])
        self.assertEqual(archived["points"]["total"], 0)
        self.assertTrue(archived["allRequestedPassed"])

    def test_unicode_binary_native_names_and_existing_outputs_remain_reviewable(self):
        self.enabled_only()
        self.fixture("入力-é.vdb", grid="descriptor:量")
        report = self.report("--diagnostic-mask", "入力-é.vdb", "検査-é.vdb",
                             "--grid", "descriptor:descriptor:量", "--diagnostic-range", -1, 1,
                             "--diagnostic-mask-name", "失敗")
        self.validate(report, operation="diagnostic-mask")
        self.assertEqual(report["gridName"], "descriptor:量")
        self.inspect_mask("検査-é.vdb", 2, "失敗")
        old = (self.directory / "検査-é.vdb").read_bytes()
        self.failure("--diagnostic-mask", "入力-é.vdb", "検査-é.vdb", output="unused.vdb")
        self.assertEqual((self.directory / "検査-é.vdb").read_bytes(), old)
        self.failure("--diagnostic-mask", "入力-é.vdb", "入力-é.vdb", output="unused.vdb")
        self.fixture("binary.vdb", [0.], [1], grid="raw-\udcff")
        binary = self.report("--diagnose", "binary.vdb")
        self.validate(binary)
        self.assertIsNone(binary["gridName"])
        self.string_field(binary, "gridName", b"raw-\xff")
        self.assertTrue(binary["allRequestedPassed"])
        (self.directory / "-h").write_bytes((self.directory / "binary.vdb").read_bytes())
        self.assertTrue(self.report("--diagnose", "./-h")["allRequestedPassed"])

    def test_examples_and_caps_abort_operations_without_partial_outputs(self):
        self.enabled_only()
        self.fixture()
        none = self.report("--diagnose", "field.vdb", "--diagnostic-range", -1, 1,
                           "--max-diagnostic-examples", 0)
        self.validate(none)
        self.assertTrue(all(not row["examples"] for row in none["checks"]))
        one = self.report("--diagnose", "field.vdb", "--diagnostic-range", -1, 1,
                          "--max-diagnostic-examples", 1)
        self.assertEqual(len(self.check(one, "range")["examples"]), 1)
        for extra in (("--max-input-bytes", 1), ("--max-diagnostic-work", 1),
                      ("--max-diagnostic-metadata", 1), ("--max-voxels", 511), ("--grid", "missing")):
            with self.subTest(extra=extra):
                self.failure("--diagnostic-mask", "field.vdb", "bad.vdb", *extra)
        self.fixture("other.vdb", [0.], [1], grid="other")
        self.write_json("bundle.json", {"version": 1, "inputs": [
            {"path": "field.vdb"}, {"path": "other.vdb"}]})
        self.call("--author-file", "bundle.json", "bundle.vdb")
        self.protect("bundle.vdb")
        self.failure("--diagnostic-mask", "bundle.vdb", "bad.vdb", "--grid", "field",
                     "--max-diagnostic-grids", 1)
        self.point_fixture()
        for extra in (("--max-points", 1), ("--max-attributes", 1),
                      ("--max-groups", 1), ("--max-string-bytes", 1)):
            with self.subTest(extra=extra):
                self.failure("--diagnostic-mask", "points.vdb", "bad.vdb",
                             "--diagnostic-profile", "points", *extra)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--tool", required=True, type=Path)
    parser.add_argument("--enabled", required=True, choices=("0", "1"))
    args, remaining = parser.parse_known_args()
    DiagnosticsCli.tool = args.tool.resolve(strict=True)
    DiagnosticsCli.enabled = args.enabled == "1"
    unittest.main(argv=[__file__, *remaining])
