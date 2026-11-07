"""Real-tool occupancy morphology/Booleans and native point-column sampling."""

from array import array
import argparse
import json
import math
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "python"))
from container_openvdb import DenseType, DenseVolume, write_dense_array, read_dense_array


class TopologySamplingCli(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="container-vdb-topology-cli-")
        self.directory = Path(self.temporary.name)
        self.originals = {}

    def tearDown(self):
        try:
            for name, data in self.originals.items():
                self.assertEqual((self.directory / name).read_bytes(), data)
            self.assertFalse(list(self.directory.glob(".container-*")))
        finally:
            self.temporary.cleanup()

    def call(self, *arguments, succeeds=True):
        result = subprocess.run([str(self.tool), *map(str, arguments)], cwd=self.directory,
                                capture_output=True, text=True, encoding="utf-8", timeout=60)
        self.assertEqual(result.returncode == 0, succeeds,
                         f"{arguments!r}\n{result.stdout}\n{result.stderr}")
        return result

    def report(self, *arguments):
        return json.loads(self.call(*arguments).stdout)

    def enabled_only(self):
        if not self.enabled:
            self.skipTest("OpenVDB is disabled")

    def protect(self, name):
        self.originals[name] = (self.directory / name).read_bytes()

    def dense(self, name, values, active, origin=(0, 0, 0), dimensions=None):
        volume = DenseVolume(DenseType.FLOAT64, dimensions or (len(values), 1, 1),
                             array("d", values), bytearray(active), origin=origin,
                             grid_name="field")
        encoded = name + ".cvarr"
        write_dense_array(self.directory / encoded, volume)
        self.call("--dense-import", encoded, name)
        self.protect(encoded)
        self.protect(name)

    def points(self):
        (self.directory / "points.json").write_text(json.dumps(
            [{"position": [x, 0, 0]} for x in (.25, .75, 1.25)]), encoding="utf-8")
        self.call("--points", "points.json", "raw.vdb", "--voxel-size", 1)
        edits = {"version": 1, "attributes": [
            {"op": "add", "name": "id", "type": "int64", "values": [2**63-1]},
            {"op": "add", "name": "chosen", "type": "double", "values": [-0.0]}],
            "groups": [{"op": "add", "name": "keep", "values": [True, False, True]}]}
        (self.directory / "edits.json").write_text(json.dumps(edits), encoding="utf-8")
        self.call("--edit-point-attributes", "raw.vdb", "edits.json", "points.vdb", "--grid-name", "points")
        self.protect("points.vdb")

    def column(self, source, attribute):
        destination = source + "." + attribute + ".json"
        self.call("--point-attribute", source, attribute, destination)
        return json.loads((self.directory / destination).read_text(encoding="utf-8"))["values"]

    def test_help_and_rejected_arity_or_ignored_options(self):
        help_text = self.call("--help").stdout
        for option in ("--topology-mask", "--topology-boolean", "--sample-point-attribute",
                       "--topology-tiles", "--point-interpolation"):
            self.assertIn(option, help_text)
        cases = [
            ["--topology-mask", "a", "--topology-operation", "dilate"],
            ["--topology-boolean", "a", "b", "--topology-neighbors", "6"],
            ["--sample-point-attribute", "a", "--point-interpolation", "linear"],
            ["--sphere", "bad.vdb", "--topology-operation", "erode"],
            ["--topology-mask", "a", "bad.vdb", "--topology-boolean-operation", "union"],
            ["--topology-boolean", "a", "b", "bad.vdb", "--topology-tiles", "expand"],
            ["--topology-mask", "a", "bad.vdb", "--field-grid", "ignored"],
            ["--topology-mask", "a", "bad.vdb", "--max-triangles", "1"],
            ["--topology-mask", "a", "bad.vdb", "--topology-iterations", "33"],
            ["--topology-mask", "a", "bad.vdb", "--topology-neighbors", "7"],
            ["--topology-mask", "a", "bad.vdb", "--max-topology-work", "0"],
            ["--sample-point-attribute", "a", "b", "bad.vdb", "--grid-a", "ignored"],
            ["--sample-point-attribute", "a", "b", "bad.vdb", "--max-edits", "1"],
            ["--sample-point-attribute", "a", "b", "bad.vdb", "--voxel-size", ".1"],
            ["--sample-point-attribute", "a", "b", "bad.vdb", "--max-triangles", "1"],
            ["--sphere", "bad.vdb", "--attribute-name", "ignored"],
            ["--sample-point-attribute", "a", "b", "bad.vdb", "--attribute-operation", "rename"],
            ["--sample-point-attribute", "a", "b", "bad.vdb", "--point-interpolation", "cubic"],
            ["--sample-point-attribute", "a", "b", "bad.vdb", "--max-point-field-grids", "4097"],
        ]
        for arguments in cases:
            with self.subTest(arguments=arguments):
                self.assertTrue(self.call(*arguments, succeeds=False).stderr.strip())
                self.assertFalse((self.directory / "bad.vdb").exists())
        self.assertEqual(list(self.directory.iterdir()), [])

    def test_disabled_routes_fail_before_input_access(self):
        if self.enabled:
            self.skipTest("OpenVDB is enabled")
        for arguments in (("--topology-mask", "missing", "bad.vdb"),
                          ("--topology-boolean", "missing-a", "missing-b", "bad.vdb"),
                          ("--sample-point-attribute", "missing-a", "missing-b", "bad.vdb")):
            self.assertIn("ENABLE_OPENVDB=ON", self.call(*arguments, succeeds=False).stderr)
        self.assertEqual(list(self.directory.iterdir()), [])

    def test_native_edit_option_matrix_and_disabled_route(self):
        self.assertIn("--edit-topology", self.call("--help").stdout)
        cases = [(), ("--edit-operation", "bad"),
                 ("--edit-operation", "prune-exact", "--native-value", "0"),
                 ("--edit-operation", "activate-value"),
                 ("--edit-operation", "fill-box", "--native-value", "1"),
                 ("--edit-operation", "clip-box"),
                 ("--edit-operation", "prune-exact", "--fill-inactive"),
                 ("--edit-operation", "prune-exact", "--grid", "a", "--grid", "b"),
                 ("--edit-operation", "prune-exact", "--max-voxels", "-1"),
                 ("--edit-operation", "prune-exact", "--voxel-size", ".5"),
                 ("--edit-operation", "activate-value", "--native-value", "1", "--native-vector", "1", "2", "3")]
        for options in cases:
            self.call("--edit-topology", "missing", "bad.vdb", *options, succeeds=False)
            self.assertFalse((self.directory / "bad.vdb").exists())
        if not self.enabled:
            self.assertIn("ENABLE_OPENVDB=ON", self.call("--edit-topology", "missing", "bad.vdb",
                          "--edit-operation", "prune-exact", succeeds=False).stderr)

    def native_dense(self, name, maximum=(2, 0, 0), minimum=(0, 0, 0)):
        output = name + ".cvarr"
        dimensions = tuple(b - a + 1 for a, b in zip(minimum, maximum))
        self.call("--dense-export", name, output, "--dense-origin", *minimum, "--dense-dimensions", *dimensions)
        return read_dense_array(self.directory / output)

    def test_native_value_edits_preserve_signed_zero_and_values(self):
        self.enabled_only()
        self.dense("source.vdb", [-0., 1.125, 2.5], [0, 1, 0])
        self.call("--edit-topology", "source.vdb", "active.vdb", "--edit-operation", "activate-value",
                  "--native-value", "-0")
        out = self.native_dense("active.vdb")
        self.assertEqual(list(out.active), [1, 1, 0])
        self.assertEqual(list(out.values), [-0., 1.125, 2.5])
        self.assertEqual(math.copysign(1, out.values[0]), -1)
        self.call("--edit-topology", "active.vdb", "off.vdb", "--edit-operation", "deactivate-value",
                  "--native-value", "1.125")
        out = self.native_dense("off.vdb")
        self.assertEqual(list(out.active), [1, 0, 0]); self.assertEqual(out.values[1], 1.125)
        self.call("--edit-topology", "off.vdb", "pruned.vdb", "--edit-operation", "prune-exact")
        out = self.native_dense("pruned.vdb")
        self.assertEqual(list(out.active), [1, 0, 0]); self.assertEqual(math.copysign(1, out.values[0]), -1)
        self.call("--edit-topology", "source.vdb", "limited.vdb", "--edit-operation", "prune-exact",
                  "--max-topology-work", 1, succeeds=False)
        self.assertFalse((self.directory / "limited.vdb").exists())

    def test_native_fill_clip_background_and_unicode_parents(self):
        self.enabled_only()
        self.dense("source.vdb", [1., 2., 3.], [1, 0, 1])
        parent = self.directory / "体積-é"; parent.mkdir()
        source = parent / "入力.vdb"; source.write_bytes((self.directory / "source.vdb").read_bytes())
        output = parent / "出力.vdb"
        self.call("--edit-topology", source, output, "--edit-operation", "fill-box", "--native-value", "4.125",
                  "--index-box", -1, 0, 0, 1, 0, 0, "--fill-inactive")
        (self.directory / "fill.vdb").write_bytes(output.read_bytes())
        out = self.native_dense("fill.vdb", minimum=(-1, 0, 0))
        self.assertEqual(list(out.values), [4.125, 4.125, 4.125, 3.]); self.assertEqual(list(out.active), [0, 0, 0, 1])
        self.call("--edit-topology", "fill.vdb", "clip.vdb", "--edit-operation", "clip-box",
                  "--index-box", 0, 0, 0, 1, 0, 0)
        out = self.native_dense("clip.vdb")
        self.assertEqual(list(out.values), [4.125, 4.125, 0.]); self.assertEqual(list(out.active), [0, 0, 0])
        self.call("--edit-topology", "clip.vdb", "bg.vdb", "--edit-operation", "replace-background",
                  "--native-value", "-0")
        out = self.native_dense("bg.vdb")
        self.assertEqual(math.copysign(1, out.background[0]), -1)
        self.assertEqual(math.copysign(1, out.values[2]), -1)
        before = output.read_bytes()
        self.call("--edit-topology", source, output, "--edit-operation", "prune-exact", succeeds=False)
        self.assertEqual(output.read_bytes(), before)

    def test_native_vector_components_use_native_type_and_frame(self):
        self.enabled_only()
        volume = DenseVolume(DenseType.VEC3_FLOAT64, (1, 1, 1), array("d", [1.125, -2., 3.]), bytearray([0]))
        write_dense_array(self.directory / "vector.cvarr", volume)
        self.call("--dense-import", "vector.cvarr", "vector.vdb")
        self.call("--edit-topology", "vector.vdb", "on.vdb", "--edit-operation", "activate-value",
                  "--native-vector", "1.125", "-2", "3")
        out = self.native_dense("on.vdb", maximum=(0, 0, 0))
        self.assertEqual(list(out.values), [1.125, -2., 3.]); self.assertEqual(list(out.active), [1])
        self.call("--edit-topology", "vector.vdb", "bad.vdb", "--edit-operation", "activate-value",
                  "--native-value", "1.125", succeeds=False)
        self.assertFalse((self.directory / "bad.vdb").exists())

    def test_masked_value_cli_rejects_irrelevant_options_and_disabled_build(self):
        self.assertIn("--edit-topology-mask", self.call("--help").stdout)
        for options in ((), ("--mask-operation", "bad"),
                        ("--mask-operation", "activate", "--native-value", "1"),
                        ("--mask-operation", "clip-inside", "--mask-grid", "a", "--mask-grid", "b"),
                        ("--mask-operation", "deactivate", "--max-topology-work", "0"),
                        ("--mask-operation", "activate", "--index-box", "0", "0", "0", "1", "1", "1")):
            self.call("--edit-topology-mask", "missing", "mask", "bad.vdb", *options, succeeds=False)
            self.assertFalse((self.directory / "bad.vdb").exists())
        if not self.enabled:
            self.assertIn("ENABLE_OPENVDB=ON", self.call("--edit-topology-mask", "missing", "mask", "bad.vdb",
                          "--mask-operation", "intersect-activity", succeeds=False).stderr)

    def test_masked_native_values_and_activity_use_mask_states(self):
        self.enabled_only()
        self.dense("source.vdb", [1.125, -0., 3.125], [1, 0, 1])
        self.dense("mask.vdb", [0., 0., 9.], [1, 1, 0])
        expected = {
            "activate": ([1.125, -0., 3.125], [1, 1, 1]),
            "deactivate": ([1.125, -0., 3.125], [0, 0, 1]),
            "intersect-activity": ([1.125, -0., 3.125], [1, 0, 0]),
            "clip-inside": ([1.125, -0., 0.], [1, 0, 0]),
            "clip-outside": ([0., 0., 3.125], [0, 0, 1]),
        }
        for operation, (values, active) in expected.items():
            output = operation + ".vdb"
            report = self.report("--edit-topology-mask", "source.vdb", "mask.vdb", output,
                                 "--mask-operation", operation, "--grid", "field", "--mask-grid", "field")
            self.assertEqual(report["maskActiveVoxelCount"], 2)
            out = self.native_dense(output)
            self.assertEqual(list(out.values), values); self.assertEqual(list(out.active), active)
            self.assertEqual(math.copysign(1, out.values[1]), 1 if operation == "clip-outside" else -1)
        self.call("--edit-topology-mask", "source.vdb", "mask.vdb", "limited.vdb",
                  "--mask-operation", "activate", "--max-topology-work", 1, succeeds=False)
        self.assertFalse((self.directory / "limited.vdb").exists())

    def test_sdf_background_option_matrix_and_disabled_route(self):
        self.assertIn("--sdf-background", self.call("--help").stdout)
        for options in ((), ("--sdf-mode", "bad"), ("--sdf-mode", "background", "--native-value", "0"),
                        ("--sdf-mode", "background", "--grid", "a", "--grid", "b"),
                        ("--sdf-mode", "flood-fill", "--outside-width", "nan"),
                        ("--sdf-mode", "background", "--max-topology-work", "0")):
            self.call("--sdf-background", "missing", "bad.vdb", *options, succeeds=False)
            self.assertFalse((self.directory / "bad.vdb").exists())
        if not self.enabled:
            self.assertIn("ENABLE_OPENVDB=ON", self.call("--sdf-background", "missing", "bad.vdb",
                          "--sdf-mode", "background", succeeds=False).stderr)

    def test_sdf_filter_option_matrix_and_disabled_route(self):
        self.assertIn("--sdf-filter",self.call("--help").stdout)
        for options in ((),("--sdf-filter-operation","bad"),
                        ("--sdf-filter-operation","mean","--sdf-width","0"),
                        ("--sdf-filter-operation","mean","--sdf-normalizations","65"),
                        ("--sdf-filter-operation","offset"),
                        ("--sdf-filter-operation","mean","--sdf-distance","0"),
                        ("--sdf-filter-operation","track","--sdf-width","1"),
                        ("--sdf-filter-operation","normalize","--sdf-mask","missing"),
                        ("--sdf-filter-operation","mean","--sdf-normalization-mask","missing"),
                        ("--sdf-filter-operation","normalize","--sdf-normalization-grid","mask"),
                        ("--sdf-filter-operation","normalize","--sdf-normalization-mask",""),
                        ("--sdf-filter-operation","normalize","--sdf-normalization-mask","a","--sdf-normalization-mask","b"),
                        ("--sdf-filter-operation","mean","--sdf-alpha-range","0","1"),
                        ("--sdf-filter-operation","mean","--sdf-spatial","bad"),
                        ("--sdf-filter-operation","mean","--sdf-temporal","bad"),
                        ("--sdf-filter-operation","mean","--sdf-trim","bad"),
                        ("--sdf-filter-operation","prune","--sdf-spatial","first"),
                        ("--sdf-filter-operation","resize-band","--sdf-trim","all"),
                        ("--sdf-filter-operation","mean","--grid","a","--grid","b"),
                        ("--sdf-filter-operation","mean","--width","1")):
            self.call("--sdf-filter","missing","bad.vdb",*options,succeeds=False)
            self.assertFalse((self.directory/"bad.vdb").exists())
        if not self.enabled:
            self.assertIn("ENABLE_OPENVDB=ON",self.call("--sdf-filter","missing","bad.vdb",
                          "--sdf-filter-operation","mean",succeeds=False).stderr)

    def test_all_native_sdf_filter_modes_and_quality_reports(self):
        self.enabled_only()
        self.call("--sphere","source.vdb","--voxel-size",.25)
        self.protect("source.vdb")
        for mode in ("gaussian","mean","median","mean-curvature","laplacian","fillet","offset",
                     "normalize","track","prune","resize-band","dilate-band","erode-band"):
            output=mode+".vdb"
            extra=("--sdf-distance",.125) if mode=="offset" else ("--sdf-band-width",4) if mode=="resize-band" else ()
            r=self.report("--sdf-filter","source.vdb",output,"--sdf-filter-operation",mode,*extra)
            self.assertGreater(r["activeVoxelCount"],0)
            self.assertGreater(r["gradientSampleCount"],0)
            self.assertTrue(math.isfinite(r["meanGradientError"]))
            self.assertLess(r["meanGradientError"],.5)
            width=4 if mode in ("resize-band","dilate-band") else 2 if mode=="erode-band" else 3
            self.assertAlmostEqual(r["halfWidthVoxels"],width)
        expanded=self.native_dense("offset.vdb",maximum=(8,0,0))
        self.assertLess(expanded.values[4],0)
        self.call("--sdf-filter","source.vdb","source.vdb","--sdf-filter-operation","mean",succeeds=False)

    def test_sdf_filter_masks_schemes_noops_and_shared_limits(self):
        self.enabled_only()
        self.call("--sphere","source.vdb","--voxel-size",.25)
        self.protect("source.vdb")
        mask=DenseVolume(DenseType.FLOAT64,(1,1,1),array("d",[.5]),bytearray([0]),
                         linear=(.25,0,0,0,.25,0,0,0,.25),background=array("d",[.5]))
        write_dense_array(self.directory/"mask.cvarr",mask)
        self.call("--dense-import","mask.cvarr","mask.vdb");self.protect("mask.vdb")
        r=self.report("--sdf-filter","source.vdb","masked.vdb","--sdf-filter-operation","mean-curvature",
                      "--sdf-mask","mask.vdb","--sdf-mask-grid","field","--sdf-alpha-range",0,1,
                      "--sdf-invert-alpha","--sdf-spatial","second","--sdf-temporal","rk3","--sdf-trim","interior")
        self.assertGreater(r["activeVoxelCount"],0)
        original=self.native_dense("source.vdb",maximum=(8,0,0))
        self.call("--sdf-filter","source.vdb","no-op.vdb","--sdf-filter-operation","normalize","--sdf-normalizations",0)
        copied=self.native_dense("no-op.vdb",maximum=(8,0,0))
        self.assertEqual(original.values.tobytes(),copied.values.tobytes())
        self.assertEqual(original.active,copied.active)
        for limit in (("--max-sdf-work",1),("--max-voxels",1),("--max-input-bytes",1024)):
            self.call("--sdf-filter","source.vdb","bad.vdb","--sdf-filter-operation","mean",*limit,succeeds=False)
            self.assertFalse((self.directory/"bad.vdb").exists())

    def test_sdf_normalization_uses_activity_and_snapshots_unicode_masks(self):
        self.enabled_only()
        self.call("--sphere","source.vdb","--voxel-size",.25)
        self.protect("source.vdb")
        original=self.native_dense("source.vdb",maximum=(8,0,0))
        mask=DenseVolume(DenseType.FLOAT64,(3,1,1),array("d",[0.,9.,0.]),bytearray([1,0,1]),origin=(4,0,0),
                         linear=(.25,0,0,0,.25,0,0,0,.25),background=array("d",[7.]))
        write_dense_array(self.directory/"mask.cvarr",mask)
        self.call("--dense-import","mask.cvarr","mask.vdb");self.protect("mask.vdb")
        parent=self.directory/"正規化-é";parent.mkdir()
        mask_path=parent/"選択.vdb";mask_path.write_bytes((self.directory/"mask.vdb").read_bytes())
        r=self.report("--sdf-filter","source.vdb","normalized.vdb","--sdf-filter-operation","normalize",
                      "--sdf-normalization-mask",mask_path,"--sdf-normalization-grid","field",
                      "--sdf-spatial","third","--sdf-temporal","rk3")
        self.assertEqual(r["normalizationSelectedVoxels"],2)
        out=self.native_dense("normalized.vdb",maximum=(8,0,0))
        self.assertEqual(out.active,original.active)
        for i in range(9):
            if i not in (4,6):
                self.assertEqual(out.values[i:i+1].tobytes(),original.values[i:i+1].tobytes())
        self.assertEqual(mask_path.read_bytes(),(self.directory/"mask.vdb").read_bytes())
        r=self.report("--sdf-filter","source.vdb","no-op.vdb","--sdf-filter-operation","normalize",
                      "--sdf-normalization-mask",mask_path,"--sdf-normalizations",0)
        self.assertEqual(r["normalizationSelectedVoxels"],2)
        copied=self.native_dense("no-op.vdb",maximum=(8,0,0))
        self.assertEqual(copied.values.tobytes(),original.values.tobytes())
        self.call("--sdf-filter","source.vdb","bad.vdb","--sdf-filter-operation","normalize",
                  "--sdf-normalization-mask",mask_path,"--sdf-normalization-grid","missing",succeeds=False)
        self.assertFalse((self.directory/"bad.vdb").exists())

    def test_sdf_asymmetric_backgrounds_and_flood_fill_preserve_active_distances(self):
        self.enabled_only()
        self.call("--sphere", "source.vdb", "--radius", 1, "--voxel-size", .25)
        self.protect("source.vdb")
        original = self.native_dense("source.vdb", maximum=(8, 0, 0))
        self.call("--sdf-background", "source.vdb", "background.vdb", "--sdf-mode", "background",
                  "--outside-width", 1, "--inside-width", 1.25, "--grid", "surface", "--grid-name", "edited")
        result = self.native_dense("background.vdb", maximum=(8, 0, 0))
        self.assertEqual(list(result.active), list(original.active))
        for i, active in enumerate(original.active):
            if active:
                self.assertEqual(result.values[i], original.values[i])
        self.assertEqual(result.values[0], -1.25); self.assertEqual(result.values[8], 1.)
        self.call("--sdf-background", "background.vdb", "filled.vdb", "--sdf-mode", "flood-fill",
                  "--outside-width", 1, "--inside-width", 1.25)
        filled = self.native_dense("filled.vdb", maximum=(8, 0, 0))
        self.assertEqual(list(filled.active), list(result.active)); self.assertEqual(list(filled.values), list(result.values))
        self.assertTrue(any(original.active))

    def test_sdf_background_width_class_limits_and_no_overwrite(self):
        self.enabled_only()
        self.call("--sphere", "source.vdb", "--radius", 1, "--voxel-size", .25)
        self.protect("source.vdb")
        for options in (("--outside-width", ".01"), ("--inside-width", "0"),
                        ("--max-topology-work", "1"), ("--max-voxels", "1"),
                        ("--outside-width", "1e-300")):
            self.call("--sdf-background", "source.vdb", "bad.vdb", "--sdf-mode", "flood-fill", *options, succeeds=False)
            self.assertFalse((self.directory / "bad.vdb").exists())
        self.dense("ordinary.vdb", [1.], [1])
        self.call("--sdf-background", "ordinary.vdb", "bad.vdb", "--sdf-mode", "background", succeeds=False)
        self.call("--sdf-background", "source.vdb", "source.vdb", "--sdf-mode", "background", succeeds=False)
        self.assertFalse((self.directory / "bad.vdb").exists())

    def test_topology_boolean_uses_active_states_and_morphology_neighbors(self):
        self.enabled_only()
        self.dense("a.vdb", [0, 0, 0], [1, 0, 1], origin=(-1, 0, 0))
        self.dense("b.vdb", [9, 9, 9], [0, 1, 1], origin=(-1, 0, 0))
        for operation, expected in (("union", 3), ("intersection", 1), ("difference", 1)):
            result = self.report("--topology-boolean", "a.vdb", "b.vdb", operation + ".vdb",
                                 "--topology-boolean-operation", operation, "--grid-a", "field", "--grid-b", "field")
            self.assertEqual(result["activeVoxels"], expected)
            self.assertEqual(result["gridName"], "mask")
            self.assertEqual(result["inputActiveVoxelsA"], 2)
        self.dense("single.vdb", [0], [1])
        for neighbors, expected in ((6, 7), (18, 19), (26, 27)):
            result = self.report("--topology-mask", "single.vdb", f"dilate-{neighbors}.vdb",
                                 "--topology-operation", "dilate", "--topology-neighbors", neighbors)
            self.assertEqual(result["activeVoxels"], expected)
        for operation in ("erode", "open"):
            result = self.report("--topology-mask", "single.vdb", operation + ".vdb", "--topology-operation", operation)
            self.assertEqual(result["activeVoxels"], 0)
        result = self.report("--topology-mask", "single.vdb", "close.vdb", "--topology-operation", "close")
        self.assertEqual(result["activeVoxels"], 1)
        self.call("--topology-mask", "single.vdb", "limited.vdb", "--max-topology-work", 1, succeeds=False)
        self.assertFalse((self.directory / "limited.vdb").exists())

    def test_native_linear_sampling_preserves_groups_and_exact_ids(self):
        self.enabled_only()
        self.points()
        values = [float(x) for z in range(3) for y in range(3) for x in range(-2, 4)]
        self.dense("field.vdb", values, [1] * len(values), origin=(-2, -1, -1), dimensions=(6, 3, 3))
        report = self.report("--sample-point-attribute", "points.vdb", "field.vdb", "sampled.vdb",
                             "--input-grid", "points", "--field-grid", "field")
        self.assertEqual(report["writtenCount"], 3)
        self.assertEqual(self.column("sampled.vdb", "sampled"), [.25, .75, 1.25])
        self.assertEqual(self.column("sampled.vdb", "id"), [2**63-1] * 3)
        self.call("--sample-point-attribute", "points.vdb", "field.vdb", "selected.vdb",
                  "--attribute-name", "chosen", "--attribute-operation", "set", "--any-group", "keep")
        selected = self.column("selected.vdb", "chosen")
        self.assertEqual(selected, [.25, 0, 1.25])
        self.assertEqual(math.copysign(1, selected[1]), -1)
        self.call("--sample-point-attribute", "points.vdb", "field.vdb", "limited.vdb",
                  "--max-point-field-work", 1, succeeds=False)
        self.assertFalse((self.directory / "limited.vdb").exists())

    def test_exact_integer_sampling_from_point_count_grid(self):
        self.enabled_only()
        self.points()
        self.call("--point-mask", "points.vdb", "counts.vdb", "--point-counts")
        report = self.report("--sample-point-attribute", "points.vdb", "counts.vdb", "sampled.vdb",
                             "--field-grid", "point_count", "--point-interpolation", "nearest")
        self.assertEqual(report["fieldValueType"], "int64")
        self.assertEqual(self.column("sampled.vdb", "sampled"), [1, 2, 2])
        self.call("--sample-point-attribute", "points.vdb", "counts.vdb", "bad.vdb",
                  "--field-grid", "point_count", succeeds=False)
        self.assertFalse((self.directory / "bad.vdb").exists())


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--tool", required=True, type=Path)
    parser.add_argument("--enabled", required=True, choices=("0", "1"))
    arguments, remaining = parser.parse_known_args()
    TopologySamplingCli.tool = arguments.tool.resolve(strict=True)
    TopologySamplingCli.enabled = arguments.enabled == "1"
    unittest.main(argv=[__file__, *remaining])
