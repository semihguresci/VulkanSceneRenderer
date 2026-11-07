"""Actual native source-field/component-activity exports, exact values and bounds."""
import argparse
from array import array
import json
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "python"))
from container_openvdb.dense import (DenseType, DenseVolume, GridClass, VectorType,
                                    read_dense_array, write_dense_array)


class ValueComponentsCli(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="container-value-components-cli-")
        self.directory = Path(self.temporary.name)
        self.originals, self.serial = {}, 0

    def tearDown(self):
        try:
            for path, before in self.originals.items():
                self.assertEqual(path.read_bytes(), before, path)
        finally:
            self.temporary.cleanup()

    def call(self, *args, succeeds=True):
        result = subprocess.run([str(self.tool), *map(str, args)], cwd=self.directory,
            capture_output=True, text=True, encoding="utf-8", timeout=90)
        self.assertEqual(result.returncode == 0, succeeds,
                         f"{args!r}\n{result.stdout}\n{result.stderr}")
        if not succeeds:
            self.assertEqual(result.stdout, "", "failure emitted a partial report")
        return result

    def report(self, *args):
        report = json.loads(self.call("--value-components", *args).stdout)
        self.assertEqual(report["schemaVersion"], 1)
        self.assertEqual(report["operation"], "value-components")
        self.assertIs(type(report["emptySourceCopy"]), bool)
        for key in ("activeComponentCount", "sourceActiveVoxels", "sourceActiveTiles",
                    "sourceStoredLeafVoxels", "expandedStoredLeafVoxels",
                    "aggregateOutputLeafVoxels", "nodeUpperBound", "nativeSourceBytes",
                    "nativeWorkspaceUpperBound", "reportReservation", "workspaceUpperBound",
                    "nativeWorkUpperBound", "formattingWorkUpperBound", "workUpperBound"):
            self.assertIs(type(report[key]), int, key)
            self.assertGreaterEqual(report[key], 0)
        self.assertEqual(report["workspaceUpperBound"], report["nativeWorkspaceUpperBound"]+report["reportReservation"])
        self.assertEqual(report["workUpperBound"], report["nativeWorkUpperBound"]+report["formattingWorkUpperBound"])
        self.assertEqual(report["ordering"], "component_minimum_lexicographic_xyz")
        self.assertIn("SOURCE FIELD", report["valuePolicy"])
        self.assertIn("not isolated", report["geometryPolicy"])
        self.assertIn("five canonical", report["metadataPolicy"])
        self.assertEqual(len(report["grids"]), max(1, report["activeComponentCount"]))
        self.assertEqual(sum(g["activeVoxelCount"] for g in report["grids"]), report["sourceActiveVoxels"])
        return report

    def enabled_only(self):
        if not self.enabled:
            self.skipTest("OpenVDB is disabled")

    def fixture(self, name="native.vdb"):
        self.enabled_only()
        path = self.directory / name
        result = subprocess.run([str(self.generator), "--region", str(path)],
            capture_output=True, text=True, encoding="utf-8", timeout=60)
        self.assertEqual(result.returncode, 0, result.stdout+result.stderr)
        self.originals[path] = path.read_bytes()
        return path

    def query(self, source, grid, box=(-2, 0, 0, 2, 0, 0)):
        args = ["--query-active-regions", source, "--grid", grid]
        if box is not None:
            args += ["--region", *box]
        return json.loads(self.call(*args).stdout)

    def dense(self, name, dtype=DenseType.FLOAT64, *, empty=False, classified=False):
        self.enabled_only()
        scalar = [-0., 1.5, 7., -3., 4.25]
        values = array(dtype.typecode)
        for x in scalar:
            values.extend((x, -x, x*.5) if dtype.components == 3 else (x,))
        volume = DenseVolume(dtype, (5, 1, 1), values,
            bytearray([0]*5 if empty else [1, 1, 0, 1, 1]), origin=(-2, 0, 0),
            background=array(dtype.typecode, [-0.]*dtype.components),
            linear=(.5, .25, 0, 0, .5, 0, 0, 0, -.5), translation=(2, -1, .25),
            grid_name="field", vector_type=VectorType.CONTRAVARIANT_ABSOLUTE,
            grid_class=GridClass.LEVEL_SET if classified else GridClass.UNKNOWN, world_space=False)
        encoded = self.directory / (name+".cvarr")
        write_dense_array(encoded, volume)
        self.call("--dense-import", encoded, name)
        self.originals[self.directory/name] = (self.directory/name).read_bytes()
        return volume

    def read_dense(self, name, grid):
        self.serial += 1
        output = self.directory / f"read-{self.serial}.cvarr"
        self.call("--dense-export", name, output, "--input-grid", grid,
                  "--dense-origin", -2, 0, 0, "--dense-dimensions", 5, 1, 1)
        return read_dense_array(output)

    def test_help_strict_parser_arity_aliases_ranges_and_irrelevant_flags(self):
        text = self.call("--help").stdout
        for flag in ("--value-components", "--component-prefix", "--max-components",
                     "--max-component-active-voxels", "--max-component-output-voxels",
                     "--max-component-nodes", "--max-component-work",
                     "--max-component-grids", "--max-component-metadata"):
            self.assertIn(flag, text)
        cases = [(("--value-components",), "requires INPUT OUTPUT"),
                 (("--value-components", "missing"), "requires INPUT OUTPUT"),
                 (("--value-components", "", "out.vdb"), "requires nonempty"),
                 (("--value-components", "-h", "out.vdb"), "requires nonempty"),
                 (("--value-components", "missing", "-h"), "requires nonempty")]
        flags = [(("--grid", "a", "--input-grid", "b"), "duplicate"),
                 (("--grid-name", "a", "--component-prefix", "b"), "duplicate"),
                 (("--component-prefix", "a", "--grid-name", "b"), "duplicate"),
                 (("--grid", "-h"), "missing nonempty"),
                 (("--max-components", 0), "count exceeds range"),
                 (("--max-components", 65), "count exceeds range"),
                 (("--max-component-nodes", 1000001), "count exceeds range"),
                 (("--max-component-work", 10000000001), "count exceeds range"),
                 (("--max-component-work", "+1"), "unsigned decimal"),
                 (("--max-component-grids", 4097), "count exceeds range"),
                 (("--max-component-metadata", 16777217), "count exceeds range"),
                 (("--max-component-active-voxels", "18446744073709551616"), "count exceeds range"),
                 (("--max-component-output-voxels", 0), "count exceeds range"),
                 (("--max-input-bytes", 17179869185), "count exceeds range"),
                 (("--max-output-bytes", " 1"), "unsigned decimal"),
                 (("--max-voxels", 0), "count exceeds range"),
                 (("--max-components", 2, "--max-components", 3), "duplicate"),
                 (("--region", 0, 0, 0, 1, 1, 1), "unknown/incompatible"),
                 (("--counts-only",), "unknown/incompatible"),
                 (("--voxel-size", .5), "unknown/incompatible"),
                 (("--isovalue", 0), "unknown/incompatible")]
        cases += [(("--value-components", "missing", "out.vdb", *args), error) for args, error in flags]
        for args, expected in cases:
            with self.subTest(args=args):
                result = self.call(*args, succeeds=False)
                self.assertIn(expected, result.stderr)
                self.assertNotIn("ENABLE_OPENVDB=ON", result.stderr)
                self.assertFalse((self.directory/"out.vdb").exists())
        self.assertEqual(list(self.directory.iterdir()), [])

    def test_disabled_availability_precedes_paths_and_report_reserves(self):
        if self.enabled:
            self.skipTest("OpenVDB is enabled")
        for flags in ((), ("--max-input-bytes", 1), ("--max-component-work", 1),
                      ("--max-output-bytes", 1)):
            result = self.call("--value-components", "不存在/missing.vdb", "missing-parent/out.vdb", *flags, succeeds=False)
            self.assertIn("ENABLE_OPENVDB=ON", result.stderr)
        self.assertEqual(list(self.directory.iterdir()), [])

    def test_all_nine_native_bits_background_activity_and_binary_metadata_survive(self):
        source = self.fixture()
        for grid in ("bool", "mask", "int32", "int64", "float", "double", "vec3i", "vec3f", "vec3d"):
            with self.subTest(grid=grid):
                before = self.query(source, grid, (-2, 1, 2, 0, 3, 4))
                report = self.report(source, f"{grid}-out.vdb", "--grid", grid)
                self.assertEqual(report["activeComponentCount"], 1)
                self.assertFalse(report["emptySourceCopy"])
                self.assertEqual(report["grids"][0]["gridName"], "component_000001")
                after = self.query(f"{grid}-out.vdb", "component_000001", (-2, 1, 2, 0, 3, 4))
                for name in ("nativeType", "background", "gridClass", "vectorType", "inWorldSpace", "indexToWorld", "serializedTransform"):
                    self.assertEqual(after[name], before[name], name)
                self.assertEqual(after["queries"][0]["records"], before["queries"][0]["records"])
                for catalog in ("gridMetadata", "fileMetadata"):
                    selected = lambda data: {m["name"]["text"]: m for m in data[catalog]
                                             if m["name"]["text"] in ("binary", "fixture", "exact_id", "fixture_schema")}
                    self.assertEqual(selected(after), selected(before))

    def test_four_finite_types_partition_only_activity_and_keep_every_field_value(self):
        for dtype in DenseType:
            with self.subTest(dtype=dtype):
                source = f"source-{int(dtype)}.vdb"
                original = self.dense(source, dtype)
                report = self.report(source, f"parts-{int(dtype)}.vdb", "--component-prefix", "part")
                self.assertEqual(report["activeComponentCount"], 2)
                self.assertEqual(report["sourceActiveVoxels"], 4)
                self.assertEqual([g["gridName"] for g in report["grids"]], ["part_000001", "part_000002"])
                self.assertEqual([g["activeBounds"] for g in report["grids"]],
                    [{"minimum": [-2, 0, 0], "maximum": [-1, 0, 0]},
                     {"minimum": [1, 0, 0], "maximum": [2, 0, 0]}])
                partitions = []
                for index, grid in enumerate(report["grids"]):
                    full = self.read_dense(f"parts-{int(dtype)}.vdb", grid["gridName"])
                    self.assertEqual(full.dtype, dtype)
                    self.assertEqual(full.values.tobytes(), original.values.tobytes(), "other component/inactive payload was reset")
                    self.assertEqual(full.background.tobytes(), original.background.tobytes())
                    self.assertEqual(full.linear, original.linear)
                    self.assertEqual(full.translation, original.translation)
                    self.assertEqual(full.vector_type, original.vector_type)
                    self.assertFalse(full.world_space)
                    self.assertEqual(list(full.active), [1, 1, 0, 0, 0] if index == 0 else [0, 0, 0, 1, 1])
                    query = self.query(f"parts-{int(dtype)}.vdb", grid["gridName"])
                    partitions.append({tuple(r["clipped"]["minimum"]) for r in query["queries"][0]["records"]})
                self.assertFalse(partitions[0] & partitions[1])
                self.assertEqual(partitions[0] | partitions[1], {(-2, 0, 0), (-1, 0, 0), (1, 0, 0), (2, 0, 0)})

    def test_empty_activity_retains_inactive_source_field_and_explicit_zero_components(self):
        original = self.dense("empty.vdb", empty=True)
        report = self.report("empty.vdb", "empty-out.vdb")
        self.assertTrue(report["emptySourceCopy"])
        self.assertEqual(report["activeComponentCount"], 0)
        self.assertEqual(report["grids"], [{"gridName": "component_000000", "activeVoxelCount": 0,
            "storedLeafVoxels": report["expandedStoredLeafVoxels"], "activeBounds": None}])
        output = self.read_dense("empty-out.vdb", "component_000000")
        self.assertEqual(output.values.tobytes(), original.values.tobytes())
        self.assertEqual(output.background.tobytes(), original.background.tobytes())
        self.assertFalse(any(output.active))
        self.assertTrue(any("Empty source activity" in x for x in report["diagnostics"]))

    def test_native_class_tags_are_retained_without_an_isolated_sdf_claim(self):
        self.dense("tagged.vdb", classified=True)
        before = self.query("tagged.vdb", "field")
        report = self.report("tagged.vdb", "tagged-out.vdb", "--grid-name", "tagged")
        for grid in report["grids"]:
            after = self.query("tagged-out.vdb", grid["gridName"])
            for key in ("gridClass", "vectorType", "inWorldSpace", "indexToWorld"):
                self.assertEqual(after[key], before[key])
        self.assertIn("not isolated", report["geometryPolicy"])

    def test_joint_caps_huge_tile_rejection_and_strict_selectors_leave_no_output(self):
        source = self.fixture()
        self.dense("finite.vdb")
        baseline = self.report("finite.vdb", "baseline.vdb")
        cases = [("finite.vdb", ("--max-components", 1)),
                 ("finite.vdb", ("--max-component-active-voxels", 3)),
                 ("finite.vdb", ("--max-component-output-voxels", baseline["aggregateOutputLeafVoxels"]-1)),
                 ("finite.vdb", ("--max-component-nodes", baseline["nodeUpperBound"]-1)),
                 ("finite.vdb", ("--max-component-work", 1)),
                 ("finite.vdb", ("--max-input-bytes", 1024)),
                 ("finite.vdb", ("--max-output-bytes", 1)),
                 ("finite.vdb", ("--max-voxels", 1)),
                 ("finite.vdb", ("--max-component-metadata", 1)),
                 (source, ("--grid", "int64", "--max-component-grids", 1)),
                 (source, ("--grid", "tiles")),
                 (source, ()), (source, ("--grid", "missing"))]
        for index, (input_path, flags) in enumerate(cases):
            with self.subTest(flags=flags):
                output = f"reject-{index}.vdb"
                self.call("--value-components", input_path, output, *flags, succeeds=False)
                self.assertFalse((self.directory/output).exists())

    def test_real_canonical_state_sources_lose_only_their_reserved_provenance_keys(self):
        self.enabled_only()
        for name, dtype, values, background, grid in (
                ("fluid", DenseType.FLOAT64, [1.], [0.], "fluid"),
                ("velocity", DenseType.VEC3_FLOAT64, [0., 0., 0.], [0., 0., 0.], "velocity")):
            volume = DenseVolume(dtype, (1, 1, 1), array(dtype.typecode, values), bytearray([1]),
                background=array(dtype.typecode, background), grid_name=grid)
            encoded = self.directory / (name+".cvarr")
            write_dense_array(encoded, volume)
            self.call("--dense-import", encoded, name+".vdb")
            self.originals[self.directory/(name+".vdb")] = (self.directory/(name+".vdb")).read_bytes()
        self.call("--project-boundaries", "velocity.vdb", "fluid.vdb", "canonical.vdb",
                  "--velocity-grid", "velocity", "--fluid-grid", "fluid")
        self.originals[self.directory/"canonical.vdb"] = (self.directory/"canonical.vdb").read_bytes()
        keys = {"container:boundary_projection_schema", "container:boundary_face_channels",
                "container:boundary_projection_state_id", "container:boundary_clamped_faces",
                "container:boundary_maximum_clamp"}
        clamp_keys = {"container:boundary_clamped_faces", "container:boundary_maximum_clamp"}
        native_metadata = lambda report: {m["name"]["text"]: m for m in report["gridMetadata"]}
        # The three canonical membership keys label pressure and face fields;
        # only the fraction field owns the two interface-clamp diagnostics.
        # Both real native sources are checked, rather than inventing clamp
        # metadata on pressure or weakening the all-five-key export oracle.
        for grid, expected_keys in (("pressure", keys-clamp_keys), ("boundary_fraction", keys)):
            with self.subTest(grid=grid):
                before = self.query("canonical.vdb", grid, (0, 0, 0, 1, 1, 1))
                self.assertEqual(keys & native_metadata(before).keys(), expected_keys)
                output = grid+"-component.vdb"
                self.report("canonical.vdb", output, "--grid", grid)
                after = self.query(output, "component_000001", (0, 0, 0, 1, 1, 1))
                self.assertFalse(keys & native_metadata(after).keys())
                self.assertEqual(after["queries"][0]["records"], before["queries"][0]["records"])
                # Names/statistics are native structural metadata. Every other
                # custom field stays exact, including noncanonical boundary tags.
                structural = {"name", *keys}
                retained = lambda data: {k: v for k, v in native_metadata(data).items()
                                         if k not in structural and not k.startswith("file_")}
                self.assertEqual(retained(after), retained(before))

    def test_unicode_parents_prefix_literal_h_and_exclusive_destination_protection(self):
        source = self.fixture("入力-é.vdb")
        parent = self.directory / "成分-é"
        parent.mkdir()
        output = parent / "出力.vdb"
        report = self.report(source, output, "--input-grid", "int64", "--component-prefix", "成分")
        self.assertEqual(report["grids"][0]["gridName"], "成分_000001")
        completed = output.read_bytes()
        self.call("--value-components", source, output, "--grid", "int64", succeeds=False)
        self.assertEqual(output.read_bytes(), completed)
        literal = self.directory / "-h"
        literal.write_bytes(source.read_bytes())
        self.originals[literal] = literal.read_bytes()
        self.report("./-h", "literal-out.vdb", "--grid", "bool")
        foreign = parent / "既存.vdb"
        foreign.write_bytes(b"foreign destination\0\xff")
        before = foreign.read_bytes()
        self.call("--value-components", source, foreign, "--grid", "int64", succeeds=False)
        self.assertEqual(foreign.read_bytes(), before)
        self.call("--value-components", source, source, "--grid", "int64", succeeds=False)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--tool", required=True, type=Path)
    parser.add_argument("--generator", type=Path)
    parser.add_argument("--enabled", required=True, choices=("0", "1"))
    args, remaining = parser.parse_known_args()
    ValueComponentsCli.tool = args.tool.resolve(strict=True)
    ValueComponentsCli.enabled = args.enabled == "1"
    if ValueComponentsCli.enabled and not args.generator:
        parser.error("--generator is required for enabled real nine-type fixtures")
    ValueComponentsCli.generator = args.generator.resolve(strict=True) if args.generator else None
    unittest.main(argv=[__file__, *remaining])
