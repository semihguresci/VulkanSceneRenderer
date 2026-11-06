"""Real-tool field arithmetic/value transforms and original-point voxel masks.

Dense fixtures use the public stdlib-only binary interchange package. Expected
values/activity are calculated here, independently of the native implementation.
Mask/Int64 payloads have separate native tests: the CLI inspection path verifies
their types, common activity bounds, provenance and exact reported point counts.
"""

from array import array
import argparse
import json
import math
import pathlib
import subprocess
import sys
import tempfile
import unittest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[2] / "python"))
from container_openvdb import DenseType, DenseVolume, read_dense_array, write_dense_array


class OpenVdbCompositeCli(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="container-vdb-composite-")
        self.directory = pathlib.Path(self.temporary.name)
        self.originals = {}
        self.serial = 0

    def tearDown(self):
        try:
            for name, original in self.originals.items():
                self.assertEqual((self.directory / name).read_bytes(), original, name)
            self.assertFalse(list(self.directory.glob(".container-*")), "temporary directories leaked")
        finally:
            self.temporary.cleanup()

    def run_tool(self, *arguments, succeeds=True):
        result = subprocess.run([str(self.tool), *map(str, arguments)], cwd=self.directory,
                                capture_output=True, text=True, encoding="utf-8", timeout=60)
        self.assertEqual(result.returncode == 0, succeeds,
                         f"{arguments!r}\n{result.stdout}\n{result.stderr}")
        return result

    def report(self, *arguments):
        return json.loads(self.run_tool(*arguments).stdout)

    def require_enabled(self):
        if not self.enabled:
            self.skipTest("OpenVDB is disabled")

    def protect(self, name):
        self.originals[name] = (self.directory / name).read_bytes()

    def write_json(self, name, data):
        (self.directory / name).write_text(json.dumps(data, ensure_ascii=False), encoding="utf-8")

    @staticmethod
    def flatten(values, components):
        return list(values) if components == 1 else [v for point in values for v in point]

    def fixture(self, name, dtype, values, active, background, grid="field", translation=(0, 0, 0)):
        components = dtype.components
        background = [background] if components == 1 else background
        volume = DenseVolume(dtype, (len(values), 1, 1),
                             array(dtype.typecode, self.flatten(values, components)), bytearray(active),
                             background=array(dtype.typecode, background), grid_name=grid,
                             translation=translation)
        encoded = name + ".cvarr"
        write_dense_array(self.directory / encoded, volume)
        self.run_tool("--dense-import", encoded, name)
        self.protect(encoded)
        self.protect(name)
        return volume

    def dense(self, name, count=4, grid=None, origin=(0, 0, 0)):
        self.serial += 1
        output = f"readback-{self.serial}.cvarr"
        arguments = ["--dense-export", name, output, "--dense-origin", *origin,
                     "--dense-dimensions", count, 1, 1]
        if grid is not None:
            arguments += ["--input-grid", grid]
        self.run_tool(*arguments)
        return read_dense_array(self.directory / output)

    def assert_dense(self, actual, dtype, values, active, background):
        components = dtype.components
        self.assertEqual(actual.dtype, dtype)
        self.assertEqual(actual.values.tobytes(),
                         array(dtype.typecode, self.flatten(values, components)).tobytes())
        self.assertEqual(actual.active, bytearray(active))
        expected_background = [background] if components == 1 else background
        self.assertEqual(actual.background.tobytes(), array(dtype.typecode, expected_background).tobytes())

    def failure(self, *arguments, output="bad.vdb", diagnostic=None):
        result = self.run_tool(*arguments, succeeds=False)
        if diagnostic:
            self.assertIn(diagnostic, result.stderr)
        self.assertFalse((self.directory / output).exists(), str(arguments))
        self.assertFalse(list(self.directory.glob(".container-*")), str(arguments))

    def test_help_strict_arity_and_ignored_flags(self):
        text = self.run_tool("--help").stdout
        for name in ("--point-mask", "--point-counts", "--composite-fields", "divide",
                     "--transform-field", "--value-scope", "--max-field-work"):
            self.assertIn(name, text)
        missing_path_cases = {
            ("--point-mask", "a", "--point-counts"),
            ("--composite-fields", "a", "b", "--point-counts"),
            ("--composite-fields", "a", "--field-operation", "sum"),
            ("--transform-field", "a", "--value-operation", "copy"),
        }
        for arguments in (
            ["--point-mask"], ["--composite-fields", "a", "b"], ["--transform-field", "a"],
            ["--point-mask", "a", "--point-counts"],
            ["--composite-fields", "a", "b", "--point-counts"],
            ["--composite-fields", "a", "--field-operation", "sum"],
            ["--transform-field", "a", "--value-operation", "copy"],
            ["--point-mask", "a", "bad.vdb", "--point-count-name", "counts"],
            ["--point-mask", "a", "bad.vdb", "--max-edits", "1"],
            ["--point-mask", "a", "bad.vdb", "--max-triangles", "1"],
            ["--point-mask", "a", "bad.vdb", "--voxel-size", ".1"],
            ["--point-mask", "a", "bad.vdb", "--value-operation", "copy"],
            ["--composite-fields", "a", "b", "bad.vdb", "--input-grid", "ignored"],
            ["--composite-fields", "a", "b", "bad.vdb", "--value-operation", "copy"],
            ["--composite-fields", "a", "b", "bad.vdb", "--max-triangles", "1"],
            ["--composite-fields", "a", "b", "bad.vdb", "--blend-weight", ".25"],
            ["--composite-fields", "a", "b", "bad.vdb", "--merge-preference", "right"],
            ["--composite-fields", "a", "b", "bad.vdb", "--vector-extrema", "magnitude"],
            ["--composite-fields", "a", "b", "bad.vdb", "--field-operation", "merge-active",
             "--field-activity", "intersection"],
            ["--composite-fields", "a", "b", "bad.vdb", "--field-operation", "nonsense"],
            ["--transform-field", "a", "bad.vdb", "--grid-a", "ignored"],
            ["--transform-field", "a", "bad.vdb", "--value-scale", "2", "1", "1"],
            ["--transform-field", "a", "bad.vdb", "--value-min", "0", "0", "0"],
            ["--transform-field", "a", "bad.vdb", "--value-scope", "all"],
            ["--transform-field", "a", "bad.vdb", "--value-operation", "nonsense"],
            ["--sphere", "bad.vdb", "--field-operation", "sum"],
            ["--sphere", "bad.vdb", "--max-field-work", "2"],
            ["--sphere", "bad.vdb", "--point-counts"],
        ):
            with self.subTest(arguments=arguments):
                self.failure(*arguments, diagnostic="missing path operand" if tuple(arguments) in missing_path_cases else None)
        self.assertEqual(list(self.directory.iterdir()), [])

    def test_disabled_routes_report_availability_before_paths(self):
        if self.enabled:
            self.skipTest("OpenVDB is enabled")
        commands = [["--point-mask", "不存在.vdb", "bad.vdb", "--point-counts"],
                    ["--point-mask", "./--point-counts", "bad.vdb"]]
        for operation in ("sum", "product", "divide", "min", "max", "blend", "merge-active"):
            commands.append(["--composite-fields", "missing-a", "missing-b", "bad.vdb",
                             "--field-operation", operation])
        for operation in ("copy", "affine", "absolute", "clamp"):
            commands.append(["--transform-field", "missing", "bad.vdb", "--value-operation", operation])
        for arguments in commands:
            with self.subTest(arguments=arguments):
                self.failure(*arguments, diagnostic="ENABLE_OPENVDB=ON")
        self.assertEqual(list(self.directory.iterdir()), [])

    def test_every_binary_operation_retains_native_type_and_logical_values(self):
        self.require_enabled()
        for dtype in DenseType:
            with self.subTest(dtype=dtype):
                vector = dtype.components == 3
                a = [(-4, 2, -8), (6, -3, 12), (0, 4, 2), (10, -2, 8)] if vector else [-4, 6, 0, 10]
                b = [(2, 1, -4), (-3, 3, 6), (4, 2, 1), (2, -1, 4)] if vector else [2, -3, 4, 2]
                bg_a, bg_b = ((8, 4, -4), (2, 2, -2)) if vector else (8, 2)
                name_a, name_b = f"a-{int(dtype)}.vdb", f"b-{int(dtype)}.vdb"
                self.fixture(name_a, dtype, a, [1, 0, 1, 0], bg_a)
                self.fixture(name_b, dtype, b, [0, 1, 1, 0], bg_b)
                functions = {"sum": lambda x, y: x + y, "product": lambda x, y: x * y,
                             "divide": lambda x, y: x / y, "min": min, "max": max,
                             "blend": lambda x, y: .75 * x + .25 * y}
                for operation in (*functions, "merge-active"):
                    output = f"{operation}-{int(dtype)}.vdb"
                    extra = ("--blend-weight", .25) if operation == "blend" else \
                            ("--merge-preference", "right") if operation == "merge-active" else ()
                    result = self.report("--composite-fields", name_a, name_b, output,
                                         "--field-operation", operation, *extra)
                    if operation == "merge-active":
                        expected = [a[0], b[1], b[2], a[3]]
                        background = bg_a
                    else:
                        f = functions[operation]
                        expected = [tuple(f(x, y) for x, y in zip(xyz, uvw)) for xyz, uvw in zip(a, b)] if vector else \
                                   [f(x, y) for x, y in zip(a, b)]
                        background = tuple(f(x, y) for x, y in zip(bg_a, bg_b)) if vector else f(bg_a, bg_b)
                    self.assertEqual(result["gridName"], "field")
                    self.assertEqual(result["activeVoxels"], 3)
                    self.assertEqual(result["inputActiveVoxelsA"], 2)
                    self.assertEqual(result["inputActiveVoxelsB"], 2)
                    self.assertGreater(result["workItems"], 0)
                    self.assert_dense(self.dense(output), dtype, expected, [1, 1, 1, 0], background)

    def test_activity_policies_and_whole_vector_extrema(self):
        self.require_enabled()
        self.fixture("a.vdb", DenseType.FLOAT64, [1, 2, 3, 4], [1, 0, 1, 0], 5)
        self.fixture("b.vdb", DenseType.FLOAT64, [10, 20, 30, 40], [0, 1, 1, 0], 6)
        for policy, active in {"union": [1, 1, 1, 0], "intersection": [0, 0, 1, 0],
                               "left": [1, 0, 1, 0], "right": [0, 1, 1, 0]}.items():
            output = f"activity-{policy}.vdb"
            result = self.report("--composite-fields", "a.vdb", "b.vdb", output,
                                 "--field-activity", policy)
            self.assertEqual(result["activeVoxels"], sum(active))
            self.assert_dense(self.dense(output), DenseType.FLOAT64, [11, 22, 33, 44], active, 11)
        self.fixture("va.vdb", DenseType.VEC3_FLOAT64, [(3, 0, 0)], [1], (0, 0, 0))
        self.fixture("vb.vdb", DenseType.VEC3_FLOAT64, [(2, 2, 0)], [1], (0, 0, 0))
        for rank, expected in (("componentwise", (3, 2, 0)), ("magnitude", (3, 0, 0))):
            output = f"extrema-{rank}.vdb"
            result = self.report("--composite-fields", "va.vdb", "vb.vdb", output,
                                 "--field-operation", "max", "--vector-extrema", rank)
            self.assert_dense(self.dense(output, count=1), DenseType.VEC3_FLOAT64, [expected], [1], (0, 0, 0))
            if rank == "magnitude":
                self.assertTrue(any("rounded" in diagnostic for diagnostic in result["diagnostics"]))

    def test_metadata_explicit_selectors_exact_maps_and_native_types(self):
        self.require_enabled()
        self.fixture("a.vdb", DenseType.FLOAT32, [2, 4, 6, 8], [1, 0, 1, 0], 1, grid="left")
        self.fixture("b.vdb", DenseType.FLOAT32, [1, 2, 3, 4], [0, 1, 1, 0], 2, grid="right")
        self.fixture("unused.vdb", DenseType.FLOAT32, [100] * 4, [1] * 4, 100, grid="unused")
        self.write_json("bundle.json", {"version": 1, "inputs": [
            {"path": "a.vdb", "edits": [{"selector": "left", "metadata": [
                {"name": "artist", "operation": "set", "type": "string", "value": "Ω source A"},
                {"name": "exact_id", "operation": "set", "type": "int64", "value": 9007199254740993}]}]},
            {"path": "unused.vdb"}]})
        self.run_tool("--author-file", "bundle.json", "bundle.vdb")
        self.protect("bundle.vdb")
        result = self.report("--composite-fields", "bundle.vdb", "b.vdb", "selected.vdb",
                             "--grid-a", "descriptor:left", "--grid-b", "descriptor:right", "--grid-name", "selected")
        self.assertEqual(result["gridName"], "selected")
        self.assert_dense(self.dense("selected.vdb", grid="selected"), DenseType.FLOAT32,
                          [3, 6, 9, 12], [1, 1, 1, 0], 3)
        inspected = self.report("--inspect", "selected.vdb", "--stats")["grids"][0]
        self.assertEqual(inspected["name"], "selected")
        self.assertEqual(inspected["valueType"], "float")
        self.assertEqual(inspected["recorded"]["activeVoxelCount"], 3)
        self.assertEqual(inspected["statistics"]["sum"], 18)
        metadata = {item["name"]: item for item in inspected["metadata"]}
        self.assertEqual(metadata["artist"]["value"], "Ω source A")
        self.assertEqual(metadata["exact_id"]["type"], "int64")
        self.assertEqual(metadata["exact_id"]["value"], "9007199254740993")
        self.fixture("double.vdb", DenseType.FLOAT64, [1] * 4, [1] * 4, 1)
        self.fixture("translated.vdb", DenseType.FLOAT32, [1] * 4, [1] * 4, 1, translation=(1e-9, 0, 0))
        for b, selector in (("double.vdb", "right"), ("translated.vdb", "right"), ("b.vdb", "missing")):
            # No selector on B is needed for type/map cases; the missing A
            # selector independently proves exact descriptor forwarding.
            selected = "descriptor:missing" if selector == "missing" else "descriptor:left"
            self.failure("--composite-fields", "bundle.vdb", b, "bad.vdb", "--grid-a", selected)
        self.failure("--composite-fields", "bundle.vdb", "b.vdb", "bad.vdb",
                     "--grid-a", "descriptor:left", "--grid-b", "descriptor:missing")
        self.run_tool("--transform-field", "bundle.vdb", "unary-selected.vdb", "--input-grid", "descriptor:left")
        self.assert_dense(self.dense("unary-selected.vdb"), DenseType.FLOAT32, [2, 4, 6, 8], [1, 0, 1, 0], 1)

    def test_copy_affine_absolute_and_clamp_preserve_native_scope_bits(self):
        self.require_enabled()
        for dtype in DenseType:
            vector = dtype.components == 3
            values = [(-2, 1, -4), (3, -6, 2), (-0., -0., -0.), (-4, 5, -1)] if vector else [-2, 3, -0., -4]
            background = (-8, 4, -2) if vector else -8
            scale, offset = ((2, -1, .5), (1, 2, -1)) if vector else ((2, 1, 1), (1, 0, 0))
            minimum = (0, -2, -1) if vector else (0, -sys.float_info.max, -sys.float_info.max)
            maximum = (2, 3, 0) if vector else (2, sys.float_info.max, sys.float_info.max)
            source = f"unary-{int(dtype)}.vdb"
            self.fixture(source, dtype, values, [1, 0, 1, 0], background)
            for operation, scope in (("copy", None), ("identity", "active"), ("affine", "active"),
                                     ("affine", "all"), ("absolute", "active"), ("absolute", "all"),
                                     ("clamp", "active"), ("clamp", "all")):
                output = f"{operation}-{scope}-{int(dtype)}.vdb"
                args = ["--transform-field", source, output]
                extra = []
                if operation == "identity":
                    extra = ["--value-operation", "affine"]
                elif operation == "affine":
                    extra = ["--value-operation", operation, "--value-scale", *scale, "--value-offset", *offset]
                elif operation == "clamp":
                    extra = ["--value-operation", operation, "--value-min", *minimum, "--value-max", *maximum]
                elif operation != "copy":
                    extra = ["--value-operation", operation]
                if scope:
                    extra += ["--value-scope", scope]
                self.report(*args, *extra)

                def component(value, index):
                    if operation in ("copy", "identity"):
                        return value
                    if operation == "affine":
                        return value * scale[index] + offset[index]
                    if operation == "absolute":
                        return abs(value)
                    return minimum[index] if value < minimum[index] else maximum[index] if value > maximum[index] else value

                def transform(value):
                    return tuple(component(v, index) for index, v in enumerate(value)) if vector else component(value, 0)

                expected = [transform(value) if scope == "all" or active else value
                            for value, active in zip(values, [1, 0, 1, 0])]
                expected_background = transform(background) if scope == "all" else background
                self.assert_dense(self.dense(output), dtype, expected, [1, 0, 1, 0], expected_background)

    def test_zero_division_range_work_and_shared_budgets_leave_no_output(self):
        self.require_enabled()
        self.fixture("a.vdb", DenseType.FLOAT64, [1, 2, 3, 4], [1, 0, 1, 0], 1)
        self.fixture("b.vdb", DenseType.FLOAT64, [2, 2, 2, 2], [1, 0, 1, 0], 2)
        self.fixture("zero-bg.vdb", DenseType.FLOAT64, [2, 2, 2, 2], [1, 0, 1, 0], 0)
        self.fixture("inactive-zero.vdb", DenseType.FLOAT64, [2, 0, 2, 2], [1, 0, 1, 0], 2)
        for denominator in ("zero-bg.vdb", "inactive-zero.vdb"):
            self.failure("--composite-fields", "a.vdb", denominator, "bad.vdb", "--field-operation", "divide",
                         diagnostic="nonzero denominator")
        for command in (("--composite-fields", "a.vdb", "b.vdb", "bad.vdb"),
                        ("--transform-field", "a.vdb", "bad.vdb")):
            for flag, limit in (("--max-field-work", 1), ("--max-voxels", 511), ("--max-input-bytes", 1)):
                self.failure(*command, flag, limit)
        recorded = self.report("--inspect", "a.vdb")["grids"][0]["recorded"]["memoryBytes"]
        # This cap admits one decoded tree/clone plus substantial metadata, but
        # not two equal trees in the API's shared conservative workspace.
        cap = 4 * 1024 * 1024 + (self.directory / "a.vdb").stat().st_size + 16 * recorded + 1024 * 1024
        self.run_tool("--transform-field", "a.vdb", "single-fits.vdb", "--max-input-bytes", cap)
        self.failure("--composite-fields", "a.vdb", "b.vdb", "bad.vdb", "--max-input-bytes", cap)
        self.fixture("huge.vdb", DenseType.FLOAT32, [3.4028234663852886e38], [1], 1)
        self.fixture("tiny.vdb", DenseType.FLOAT32, [math.ldexp(1., -149)], [1], 1)
        for source, scale in (("huge.vdb", 2), ("tiny.vdb", .5)):
            self.failure("--transform-field", source, "bad.vdb", "--value-operation", "affine",
                         "--value-scale", scale, 1, 1)

    def test_point_mask_groups_int64_counts_and_empty_archives(self):
        self.require_enabled()
        self.write_json("points.json", [{"position": [.1, 0, 0]}, {"position": [.2, 0, 0]},
                                        {"position": [2.1, 0, 0]}])
        self.run_tool("--points", "points.json", "points.vdb", "--voxel-size", 1)
        self.protect("points.vdb")
        self.write_json("groups.json", {"version": 1, "groups": [
            {"op": "add", "name": "keep", "values": [True, True, False]},
            {"op": "add", "name": "exclude", "values": [False, True, False]}]})
        self.run_tool("--edit-point-attributes", "points.vdb", "groups.json", "grouped.vdb")
        self.write_json("note.json", {"version": 1, "inputs": [{"path": "grouped.vdb", "edits": [
            {"selector": "points", "metadata": [{"name": "container:point_empty_note", "operation": "set",
                                                   "type": "string", "value": "custom metadata survives"}]}]}]})
        self.run_tool("--author-file", "note.json", "native.vdb")
        self.protect("native.vdb")
        self.run_tool("--point-group", "native.vdb", "keep", "keep.json")
        self.assertEqual(json.loads((self.directory / "keep.json").read_text(encoding="utf-8"))["values"],
                         [True, True, False])
        for name, groups, selected, occupied in (("all", [], 3, 2), ("keep", ["--any-group", "keep"], 2, 1),
                                                 ("excluded", ["--all-group", "keep", "--exclude-group", "exclude"], 1, 1)):
            output = f"mask-{name}.vdb"
            result = self.report("--point-mask", "native.vdb", output, "--point-counts", *groups,
                                 "--input-grid", "descriptor:points", "--grid-name", "occupancy",
                                 "--point-count-name", "counts")
            self.assertEqual(result["inputCount"], 3)
            self.assertEqual(result["selectedCount"], selected)
            self.assertEqual(result["selectedActive"], selected)
            self.assertEqual(result["selectedInactive"], 0)
            self.assertEqual(result["occupiedVoxels"], occupied)
            self.assertEqual(result["maskGridName"], "occupancy")
            self.assertEqual(result["countGridName"], "counts")
            self.assertFalse(result["sourceArchivedSchema"])
            grids = {grid["name"]: grid for grid in self.report("--inspect", output)["grids"]}
            # GridBase::valueType() exposes MaskGrid's public bool value type;
            # the native API tests independently verify its MaskTree storage.
            self.assertEqual(grids["occupancy"]["valueType"], "bool")
            self.assertEqual(grids["counts"]["valueType"], "int64")
            self.assertEqual(grids["occupancy"]["recorded"]["activeVoxelCount"], occupied)
            self.assertEqual(grids["counts"]["recorded"]["activeVoxelCount"], occupied)
            self.assertEqual(grids["occupancy"]["recorded"]["indexBounds"], grids["counts"]["recorded"]["indexBounds"])
            for grid in grids.values():
                metadata = {item["name"]: item["value"] for item in grid["metadata"]}
                self.assertEqual(metadata["container:point_empty_note"], "custom metadata survives")
                self.assertTrue(any(key.startswith("container:point_voxel_") for key in metadata))
        mask_only = self.report("--point-mask", "native.vdb", "mask-only.vdb")
        self.assertEqual(mask_only["maskGridName"], "point_mask")
        self.assertEqual(mask_only["countGridName"], "")
        self.assertEqual(len(self.report("--inspect", "mask-only.vdb")["grids"]), 1)
        self.run_tool("--delete-points", "native.vdb", "empty.vdb")
        self.protect("empty.vdb")
        empty = self.report("--point-mask", "empty.vdb", "empty-mask.vdb", "--point-counts")
        self.assertTrue(empty["sourceArchivedSchema"])
        self.assertEqual((empty["inputCount"], empty["selectedCount"], empty["occupiedVoxels"], empty["leaves"]), (0, 0, 0, 0))
        archive_keys = {"container:point_empty_manifest_v1", "container:point_empty_descriptor_v1",
                        "container:point_empty_leaf_flags_v1"}
        for grid in self.report("--inspect", "empty-mask.vdb")["grids"]:
            self.assertEqual(grid["gridClass"], "unknown")
            self.assertEqual(grid["recorded"]["activeVoxelCount"], 0)
            metadata = {item["name"]: item["value"] for item in grid["metadata"]}
            self.assertFalse(archive_keys.intersection(metadata))
            self.assertEqual(metadata["container:point_empty_note"], "custom metadata survives")
        for extra in (("--max-points", 2), ("--max-input-bytes", 1), ("--max-voxels", 512),
                      ("--input-grid", "missing"), ("--point-count-name", "point_mask")):
            self.failure("--point-mask", "native.vdb", "bad.vdb", "--point-counts", *extra)

    def test_all_outputs_are_exclusive_unicode_paths_and_sources_stay_intact(self):
        self.require_enabled()
        self.fixture("粒子場.vdb", DenseType.FLOAT64, [1, 2, 3, 4], [1, 0, 1, 0], 1)
        self.fixture("b.vdb", DenseType.FLOAT64, [2, 2, 2, 2], [1, 1, 0, 0], 2)
        self.run_tool("--transform-field", "粒子場.vdb", "出力.vdb", "--input-grid", "descriptor:field")
        self.assert_dense(self.dense("出力.vdb"), DenseType.FLOAT64, [1, 2, 3, 4], [1, 0, 1, 0], 1)
        original = (self.directory / "出力.vdb").read_bytes()
        for arguments in (("--transform-field", "粒子場.vdb", "出力.vdb"),
                          ("--composite-fields", "粒子場.vdb", "b.vdb", "出力.vdb"),
                          ("--composite-fields", "粒子場.vdb", "b.vdb", "粒子場.vdb"),
                          ("--transform-field", "粒子場.vdb", "粒子場.vdb")):
            self.run_tool(*arguments, succeeds=False)
        self.assertEqual((self.directory / "出力.vdb").read_bytes(), original)
        self.write_json("points.json", [{"position": [0, 0, 0]}])
        self.run_tool("--points", "points.json", "points.vdb")
        self.protect("points.vdb")
        self.run_tool("--point-mask", "points.vdb", "mask.vdb")
        mask = (self.directory / "mask.vdb").read_bytes()
        self.run_tool("--point-mask", "points.vdb", "mask.vdb", succeeds=False)
        self.run_tool("--point-mask", "points.vdb", "points.vdb", succeeds=False)
        self.assertEqual((self.directory / "mask.vdb").read_bytes(), mask)


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--tool", required=True, type=pathlib.Path)
    parser.add_argument("--enabled", required=True, choices=("0", "1"))
    args, remaining = parser.parse_known_args()
    OpenVdbCompositeCli.tool = args.tool.resolve()
    OpenVdbCompositeCli.enabled = args.enabled == "1"
    unittest.main(argv=[__file__, *remaining], verbosity=2)
