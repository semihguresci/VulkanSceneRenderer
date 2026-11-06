"""Real-tool generic numeric point transfer with independent exact CIC oracles.

CLI point records are active. Inactive records, registered non-Null codecs and
Vec3I inputs are covered by the native point-transfer suite; this test never
pretends standard JSON point creation can produce those native layouts.
"""
import argparse
from fractions import Fraction
import itertools
import json
import math
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "python"))
from container_openvdb import DenseType, GridClass, VectorType, read_dense_array


class PointTransferCli(unittest.TestCase):
    identity = (1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1)

    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="container-point-transfer-cli-")
        self.directory = Path(self.temporary.name)
        self.originals = {}
        self.serial = 0

    def tearDown(self):
        try:
            for path, original in self.originals.items():
                self.assertEqual(path.read_bytes(), original, str(path))
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
        path = self.directory / name
        self.originals[path] = path.read_bytes()

    def write_json(self, name, document):
        (self.directory / name).write_text(json.dumps(document, ensure_ascii=False), encoding="utf-8")

    def fixture(self, rows=None, name="points.vdb", grid="points"):
        if rows is None:
            rows = [dict(p=(-.25, .25, .5), value=2., mass=1., vector=(2., -4., 6.)),
                    dict(p=(.25, .25, .5), value=4., mass=3., vector=(4., -8., 12.)),
                    dict(p=(1.25, .5, -.25), value=-8., mass=2., vector=(-8., 16., -24.))]
        self.serial += 1
        prefix = f"fixture-{self.serial}"
        self.write_json(prefix + ".json", [{"position": row["p"]} for row in rows])
        self.call("--points", prefix + ".json", prefix + "-raw.vdb", "--voxel-size", 1)
        queried = self.report("--point-neighbors", prefix + "-raw.vdb", "--neighbor-query", 0, 0, 0, 128)
        # Attribute edits use native leaf/local order, independently of input
        # JSON order. Join the native ordinal to the query's WORLD position.
        native = [None] * len(rows)
        remaining = list(enumerate(rows))
        for hit in queried["queries"][0]["hits"]:
            match = next((i, item) for i, item in enumerate(remaining)
                         if all(abs(a-b) < 1e-6 for a, b in zip(item[1]["p"], hit["positionWorld"])))
            index, (original, row) = match
            remaining.pop(index)
            item = dict(row, p=tuple(hit["positionWorld"]), original=original)
            native[hit["reference"]["ordinal"]] = item
        self.assertFalse(remaining)
        self.assertNotIn(None, native)
        attributes = [
            {"op": "add", "name": "value", "type": "double", "values": [r["value"] for r in native]},
            {"op": "add", "name": "mass", "type": "double", "values": [r["mass"] for r in native]},
            {"op": "add", "name": "vector", "type": "vec3d", "values": [r["vector"] for r in native]},
            {"op": "add", "name": "flag", "type": "bool", "values": [r["original"] % 2 == 0 for r in native]},
            {"op": "add", "name": "counter", "type": "int32", "values": [-7+r["original"] for r in native]},
            {"op": "add", "name": "f32", "type": "float", "values": [.125*(1+r["original"]) for r in native]},
            {"op": "add", "name": "v32", "type": "vec3f", "values": [[.125, -2., .5] for r in native]},
            {"op": "add", "name": "id", "type": "int64", "values": [2**53+1+r["original"] for r in native]},
            {"op": "add", "name": "label", "type": "string", "values": ["unrelated-世界-\u0000"]}]
        groups = [{"op": "add", "name": "A", "values": [r["original"] != 1 for r in native]},
                  {"op": "add", "name": "B", "values": [r["original"] < 2 for r in native]},
                  {"op": "add", "name": "C", "values": [r["original"] == 2 for r in native]},
                  {"op": "add", "name": "empty", "values": [False]}]
        self.write_json(prefix + "-edits.json", {"version": 1, "attributes": attributes, "groups": groups})
        self.call("--edit-point-attributes", prefix + "-raw.vdb", prefix + "-edits.json",
                  prefix + "-edited.vdb", "--grid-name", grid)
        self.write_json(prefix + "-metadata.json", {"version": 1, "inputs": [{"path": prefix + "-edited.vdb", "edits": [
            {"selector": grid, "metadata": [{"name": "artist", "operation": "set", "type": "string", "value": "transfer-\u0000-世界"}]}]}],
            "metadata": [{"name": "scene", "operation": "set", "type": "int64", "value": 9007199254740993}]})
        self.call("--author-file", prefix + "-metadata.json", name)
        for file in (prefix + ".json", prefix + "-raw.vdb", prefix + "-edits.json", prefix + "-edited.vdb", name):
            self.protect(file)
        return native

    def transfer(self, source="points.vdb", attribute="value", output="field.vdb", *flags):
        return self.report("--transfer-point-attribute", source, attribute, output,
                           "--transfer-target-transform", *self.identity, *flags)

    def dense(self, source, grid, origin, dimensions):
        self.serial += 1
        output = f"readback-{self.serial}.cvarr"
        self.call("--dense-export", source, output, "--input-grid", grid,
                  "--dense-origin", *origin, "--dense-dimensions", *dimensions)
        return read_dense_array(self.directory / output)

    @staticmethod
    def index(point, matrix):
        # Exact independent inverse of the COLUMN-vector 3x3 map.
        a, b, c, d, e, f, g, h, i = (Fraction(matrix[k]) for k in (0, 1, 2, 4, 5, 6, 8, 9, 10))
        det = a*(e*i-f*h)-b*(d*i-f*g)+c*(d*h-e*g)
        inverse = ((e*i-f*h, c*h-b*i, b*f-c*e),
                   (f*g-d*i, a*i-c*g, c*d-a*f),
                   (d*h-e*g, b*g-a*h, a*e-b*d))
        relative = [Fraction(point[r])-Fraction(matrix[4*r+3]) for r in range(3)]
        return tuple(sum(row[j]*relative[j] for j in range(3))/det for row in inverse)

    @classmethod
    def oracle(cls, rows, *, attribute="value", matrix=None, predicate=lambda r: True,
               mass=None, rounded_integers=False):
        cells = {}
        for row in rows:
            if not predicate(row):
                continue
            position = cls.index(row["p"], matrix or cls.identity)
            base = tuple(math.floor(x) for x in position)
            fraction = tuple(x-n for x, n in zip(position, base))
            value = row[attribute]
            values = value if isinstance(value, (tuple, list)) else (value,)
            if rounded_integers:
                values = tuple(float(v) for v in values)
            for corner in itertools.product((0, 1), repeat=3):
                weight = Fraction(row["mass"] if mass is None else mass)
                for bit, part in zip(corner, fraction):
                    weight *= part if bit else 1-part
                if not weight:
                    continue
                cell = tuple(n+bit for n, bit in zip(base, corner))
                numerator, total = cells.setdefault(cell, ([Fraction(0)]*len(values), Fraction(0)))
                for component, payload in enumerate(values):
                    numerator[component] += weight*Fraction(payload)
                cells[cell] = numerator, total+weight
        return cells

    def assert_report(self, report, count=3):
        self.assertEqual(report["schemaVersion"], 1)
        self.assertEqual(report["operation"], "transfer-point-attribute")
        self.assertEqual(report["inputCount"], count)
        for key in ("inputCount", "selectedCount", "selectedInactiveCount", "contributionCount",
                    "roundedIntegerComponents", "roundedFloatComponents", "workUpperBound", "componentCount", "nativeDiagnosticCount"):
            self.assertIs(type(report[key]), int, key)
        for key in ("voxelVolume", "totalMass", "depositedMass", "massErrorUpperBound", "maxFloatAbsoluteError"):
            self.assertTrue(math.isfinite(report[key]), key)
        self.assertEqual(report["selectedInactiveCount"], 0)
        self.assertIs(type(report["hasValues"]), bool)
        self.assertIs(type(report["archivedSchema"]), bool)
        self.assertFalse(report["diagnosticsTruncated"])
        self.assertEqual(len(report["diagnostics"]), report["nativeDiagnosticCount"])
        self.assertGreaterEqual(len(report["diagnostics"]), 2)
        self.assertIn("no implicit", report["payloadSemantics"])

    def assert_fields(self, source, report, expected, *, mode="mean", density=False, matrix=None):
        matrix = matrix or self.identity
        origin = tuple(min(c[k] for c in expected)-1 for k in range(3)) if expected else (-1, -1, -1)
        dimensions = tuple(max(c[k] for c in expected)-origin[k]+2 for k in range(3)) if expected else (3, 3, 3)
        field = self.dense(source, report["field"]["gridName"], origin, dimensions)
        companion = self.dense(source, report["companion"]["gridName"], origin, dimensions)
        dtype = (DenseType.FLOAT64 if report["componentCount"] == 1 else DenseType.VEC3_FLOAT64)
        if report["storage"] == "float":
            dtype = DenseType.FLOAT32 if report["componentCount"] == 1 else DenseType.VEC3_FLOAT32
        self.assertEqual(field.dtype, dtype)
        self.assertEqual(companion.dtype, DenseType.FLOAT64)
        self.assertEqual(field.active, companion.active)
        self.assertEqual(sum(field.active), len(expected))
        self.assertEqual(report["field"]["activeVoxelCount"], len(expected))
        self.assertEqual(report["companion"]["activeVoxelCount"], len(expected))
        self.assertEqual(field.grid_class, GridClass.UNKNOWN)
        self.assertEqual(field.vector_type, VectorType.INVARIANT)
        self.assertTrue(field.world_space)
        self.assertEqual(field.linear, tuple(matrix[k] for k in (0, 1, 2, 4, 5, 6, 8, 9, 10)))
        self.assertEqual(field.translation, tuple(matrix[k] for k in (3, 7, 11)))
        self.assertEqual(companion.linear, field.linear)
        integrated = Fraction(0)
        extrema = [[] for _ in range(3)]
        for coordinate in itertools.product(*(range(start, start+size) for start, size in zip(origin, dimensions))):
            self.assertEqual(field.active_at(coordinate), coordinate in expected)
            if coordinate not in expected:
                actual = field.value_at(coordinate)
                self.assertEqual(actual, 0 if field.dtype.components == 1 else (0, 0, 0))
                self.assertEqual(companion.value_at(coordinate), 0)
                continue
            numerator, weight = expected[coordinate]
            value = [float(x/weight if mode == "mean" else x) for x in numerator]
            if report["storage"] == "float":
                value = [struct.unpack("<f", struct.pack("<f", x))[0] for x in value]
            for component, payload in enumerate(value):
                extrema[component].append(payload)
            self.assertEqual(field.value_at(coordinate), value[0] if len(value) == 1 else tuple(value))
            expected_weight = float(weight/Fraction(report["voxelVolume"])) if density else float(weight)
            self.assertEqual(companion.value_at(coordinate), expected_weight)
            integrated += Fraction(expected_weight)*(Fraction(report["voxelVolume"]) if density else 1)
        self.assertEqual(report["depositedMass"], float(integrated))
        exact_total = sum((item[1] for item in expected.values()), Fraction(0))
        self.assertEqual(report["totalMass"], float(exact_total))
        self.assertGreaterEqual(Fraction(report["massErrorUpperBound"]), abs(exact_total-integrated))
        self.assertEqual(report["minimum"], [min(values) if values else 0 for values in extrema])
        self.assertEqual(report["maximum"], [max(values) if values else 0 for values in extrema])
        return field, companion

    def test_help_strict_parser_duplicates_missing_operands_and_irrelevant_flags(self):
        text = self.call("--help").stdout
        for flag in ("--transfer-point-attribute", "--transfer-mode", "--transfer-storage", "--transfer-companion",
                     "--transfer-target-transform", "--transfer-companion-name", "--transfer-weight-attribute",
                     "--transfer-mass", "--transfer-integers", "--max-point-transfer-work",
                     "--max-point-transfer-grids", "--max-point-transfer-metadata"):
            self.assertIn(flag, text)
        base = ("--transfer-point-attribute", "missing", "value", "bad.vdb")
        cases = [("--transfer-point-attribute",), (*base[:3],),
                 (base[0], "-h", "value", "bad.vdb"), (base[0], "missing", "-h", "bad.vdb"),
                 (base[0], "missing", "value", "-h"), (base[0], "", "value", "bad.vdb"),
                 (base[0], "missing", "", "bad.vdb"), (base[0], "missing", "value", "")]
        flags = [("--transfer-mode", "bad"), ("--transfer-storage", "half"), ("--transfer-companion", "fog"),
                 ("--transfer-integers", "rounded"), ("--transfer-mass", 0), ("--transfer-mass", -1),
                 ("--transfer-mass", "nan"), ("--transfer-mass", "inf"),
                 ("--transfer-weight-attribute", "mass", "--transfer-mass", 1),
                 ("--transfer-mode", "mean", "--transfer-mode", "mean"),
                 ("--grid", "a", "--input-grid", "b"), ("--any-group", "A", "--any-group", "A"),
                 ("--all-group", ""), ("--transfer-companion-name", "-h"),
                 ("--transfer-target-transform", *self.identity[:15]),
                 ("--transfer-target-transform", *self.identity[:15], 0),
                 ("--transfer-target-transform", "nan", *self.identity[1:]),
                 ("--max-point-transfer-work", 0), ("--max-point-transfer-work", 10000000001),
                 ("--max-point-transfer-grids", 4097), ("--max-point-transfer-metadata", 0),
                 ("--max-points", 2**63), ("--max-attributes", 1025), ("--max-groups", 4097),
                 ("--max-string-bytes", " -1"), ("--max-input-bytes", 0), ("--max-voxels", "+1"),
                 ("--max-voxels", 2**64), ("--transfer-mode",), ("--grid-name", ""),
                 ("--transfer-weight-attribute", "--transfer-mode", "sum"),
                 ("--voxel-size", .1), ("--half-width", 3), ("--max-triangles", 1),
                 ("--max-edits", 1), ("--transfer-storage", "float", "extra")]
        for args in [*cases, *((*base, *item) for item in flags)]:
            with self.subTest(args=args):
                result = self.call(*args, succeeds=False)
                self.assertNotIn("ENABLE_OPENVDB=ON", result.stderr)
                self.assertFalse((self.directory / "bad.vdb").exists())
        self.assertEqual(list(self.directory.iterdir()), [])

    def test_disabled_valid_routes_report_availability_without_input_io(self):
        if self.enabled:
            self.skipTest("OpenVDB is enabled")
        for mode, storage, companion in itertools.product(("sum", "mean"), ("double", "float"), ("weight", "density")):
            result = self.call("--transfer-point-attribute", "不存在.vdb", "value", "bad.vdb",
                               "--transfer-mode", mode, "--transfer-storage", storage,
                               "--transfer-companion", companion, "--any-group", "missing", succeeds=False)
            self.assertIn("ENABLE_OPENVDB=ON", result.stderr)
        for cap in (1, 1024):
            result = self.call("--transfer-point-attribute", "不存在.vdb", "value", "bad.vdb",
                               "--max-input-bytes", cap, succeeds=False)
            self.assertIn("ENABLE_OPENVDB=ON", result.stderr)
        self.assertEqual(list(self.directory.iterdir()), [])

    def test_independent_eight_cell_cic_weighted_mean_sum_and_mass_readback(self):
        self.enabled_only()
        rows = self.fixture()
        expected = self.oracle(rows)
        self.assertGreater(len(expected), 8)
        for mode in ("mean", "sum"):
            output = mode + ".vdb"
            report = self.transfer("points.vdb", "value", output,
                                   "--transfer-weight-attribute", "mass", "--transfer-mode", mode,
                                   "--input-grid", "descriptor:points")
            self.assert_report(report)
            self.assertEqual(report["selectedCount"], 3)
            self.assertEqual(report["contributionCount"], 24)
            self.assertEqual(report["totalMass"], 6)
            self.assertEqual(report["targetIndexToWorld"], list(self.identity))
            self.assert_fields(output, report, expected, mode=mode)

    def test_reflected_rotated_world_target_density_and_vector_components_are_explicit(self):
        self.enabled_only()
        rows = self.fixture()
        matrix = (0, -.5, 0, 1, .5, 0, 0, -.25, 0, 0, -.5, .5, 0, 0, 0, 1)
        expected = self.oracle(rows, attribute="vector", matrix=matrix)
        report = self.report("--transfer-point-attribute", "points.vdb", "vector", "vector.vdb",
                             "--transfer-target-transform", *matrix, "--transfer-companion", "density",
                             "--transfer-weight-attribute", "mass", "--transfer-companion-name", "rho")
        self.assert_report(report)
        self.assertEqual(report["componentCount"], 3)
        self.assertEqual(report["voxelVolume"], .125)
        self.assertEqual(report["companion"]["gridName"], "rho")
        self.assertEqual(report["field"]["valueType"], "vec3d")
        self.assert_fields("vector.vdb", report, expected, density=True, matrix=matrix)

    def test_group_predicates_constant_mass_empty_selection_and_native_archive(self):
        self.enabled_only()
        rows = self.fixture()
        report = self.transfer("points.vdb", "value", "selected.vdb", "--transfer-mass", 2,
                               "--any-group", "A", "--all-group", "B", "--exclude-group", "C")
        self.assert_report(report)
        self.assertEqual(report["selectedCount"], 1)
        self.assert_fields("selected.vdb", report, self.oracle(rows, mass=2, predicate=lambda r: r["original"] == 0))
        empty = self.transfer("points.vdb", "vector", "empty.vdb", "--any-group", "empty")
        self.assert_report(empty)
        self.assertEqual(empty["selectedCount"], 0)
        self.assertFalse(empty["hasValues"])
        self.assert_fields("empty.vdb", empty, {})
        self.call("--select-points", "points.vdb", "archived.vdb", "--any-group", "empty")
        self.protect("archived.vdb")
        archived = self.transfer("archived.vdb", "vector", "archive-field.vdb")
        self.assert_report(archived, count=0)
        self.assertTrue(archived["archivedSchema"])
        self.assertEqual(archived["componentCount"], 3)
        self.assert_fields("archive-field.vdb", archived, {})

    def test_float_rounding_integer_policy_and_numeric_families_have_visible_diagnostics(self):
        self.enabled_only()
        rows = self.fixture([dict(p=(0, 0, 0), value=1/3, mass=2., vector=(1/3, -1/7, 0.))])
        report = self.transfer("points.vdb", "value", "float.vdb", "--transfer-storage", "float")
        self.assert_report(report, count=1)
        self.assertEqual(report["roundedFloatComponents"], 1)
        self.assertGreater(report["maxFloatAbsoluteError"], 0)
        self.assert_fields("float.vdb", report, self.oracle(rows, mass=1))
        result = self.call("--transfer-point-attribute", "points.vdb", "id", "bad.vdb", succeeds=False)
        self.assertIn("Int64", result.stderr)
        self.assertFalse((self.directory / "bad.vdb").exists())
        rows[0]["id"] = 2**53+1
        report = self.transfer("points.vdb", "id", "rounded.vdb", "--transfer-integers", "allow-rounded")
        self.assert_report(report, count=1)
        self.assertEqual(report["roundedIntegerComponents"], 1)
        self.assert_fields("rounded.vdb", report, self.oracle(rows, attribute="id", mass=1, rounded_integers=True))
        for attr, value in (("flag", True), ("counter", -7), ("f32", .125), ("v32", (.125, -2., .5))):
            rows[0][attr] = value
            report = self.transfer("points.vdb", attr, attr + ".vdb")
            self.assert_fields(attr + ".vdb", report, self.oracle(rows, attribute=attr, mass=1))
        for value, source in ((1e100, "float-overflow.vdb"), (1e-300, "float-underflow.vdb")):
            self.fixture([dict(p=(0, 0, 0), value=value, mass=1., vector=(0., 0., 0.))], name=source)
            self.call("--transfer-point-attribute", source, "value", "bad.vdb",
                      "--transfer-storage", "float", succeeds=False)
            self.assertFalse((self.directory / "bad.vdb").exists())

    def test_huge_cancellation_permutation_and_positive_support_do_not_silently_overflow(self):
        self.enabled_only()
        rows = [dict(p=(0, 0, 0), value=1e308, mass=1e154, vector=(0., 0., 0.)),
                dict(p=(0, 0, 0), value=-1e308, mass=1e154, vector=(0., 0., 0.)),
                dict(p=(0, 0, 0), value=1., mass=1e154, vector=(0., 0., 0.))]
        for index, ordering in enumerate((rows, list(reversed(rows)))):
            source, output = f"cancel-{index}.vdb", f"sum-{index}.vdb"
            native = self.fixture(ordering, name=source)
            report = self.transfer(source, "value", output, "--transfer-mode", "sum", "--transfer-weight-attribute", "mass")
            self.assert_report(report)
            field, _ = self.assert_fields(output, report, self.oracle(native), mode="sum")
            self.assertEqual(field.value_at((0, 0, 0)), 1e154)
        native = self.fixture(rows[:2], name="zero.vdb")
        report = self.transfer("zero.vdb", "value", "zero-field.vdb", "--transfer-mode", "sum", "--transfer-weight-attribute", "mass")
        field, _ = self.assert_fields("zero-field.vdb", report, self.oracle(native), mode="sum")
        self.assertTrue(field.active_at((0, 0, 0)))
        self.assertEqual(field.value_at((0, 0, 0)), 0)

    def test_unicode_literal_dash_h_metadata_exclusive_output_and_limits_preserve_sources(self):
        self.enabled_only()
        rows = self.fixture(name="入力-é.vdb", grid="粒子")
        parent = self.directory / "転送-é"
        parent.mkdir()
        output = parent / "場.vdb"
        report = self.transfer("入力-é.vdb", "value", output, "--input-grid", "粒子", "--grid-name", "量",
                               "--transfer-companion-name", "重み", "--transfer-weight-attribute", "mass")
        self.assert_report(report)
        self.assertEqual(report["field"]["gridName"], "量")
        self.assert_fields(output, report, self.oracle(rows))
        for grid in self.report("--inspect", output)["grids"]:
            metadata = {entry["name"]: entry["value"] for entry in grid["metadata"]}
            self.assertEqual(metadata["artist"], "transfer-\u0000-世界")
        before = output.read_bytes()
        self.call("--transfer-point-attribute", "入力-é.vdb", "value", output, succeeds=False)
        self.assertEqual(output.read_bytes(), before)
        (self.directory / "-h").write_bytes((self.directory / "入力-é.vdb").read_bytes())
        self.transfer("./-h", "value", "literal.vdb")
        self.assertTrue((self.directory / "literal.vdb").is_file())
        for flags in (("--max-input-bytes", 1024), ("--max-voxels", 1), ("--max-points", 1),
                      ("--max-attributes", 1), ("--max-groups", 1), ("--max-string-bytes", 1),
                      ("--max-point-transfer-work", 1), ("--max-point-transfer-metadata", 1),
                      ("--grid", "missing"), ("--any-group", "missing"),
                      ("--transfer-weight-attribute", "missing"), ("--transfer-weight-attribute", "vector"),
                      ("--grid-name", "field", "--transfer-companion-name", "field"),
                      ("--transfer-target-transform", 1, 0, 0, 0, 0, 2, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1)):
            with self.subTest(flags=flags):
                self.call("--transfer-point-attribute", "入力-é.vdb", "value", "bad.vdb", *flags, succeeds=False)
                self.assertFalse((self.directory / "bad.vdb").exists())
        for attr in ("P", "label", "missing", "__group"):
            self.call("--transfer-point-attribute", "入力-é.vdb", attr, "bad.vdb", succeeds=False)
            self.assertFalse((self.directory / "bad.vdb").exists())
        self.call("--sphere", "scalar.vdb", "--voxel-size", .25)
        self.call("--transfer-point-attribute", "scalar.vdb", "value", "bad.vdb", succeeds=False)
        self.assertFalse((self.directory / "bad.vdb").exists())


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--tool", required=True, type=Path)
    parser.add_argument("--enabled", required=True, choices=("0", "1"))
    args, remaining = parser.parse_known_args()
    PointTransferCli.tool = args.tool.resolve(strict=True)
    PointTransferCli.enabled = args.enabled == "1"
    unittest.main(argv=[__file__, *remaining])
