"""Real-tool ray contracts through public dense/sphere and native fixtures.

Distances and activity-block intervals are analytic world-space oracles. Native
tiny-sign/nonfinite/binary-metadata fixtures require the real optional generator;
they are never replaced by a fabricated executable or a different file format.
"""

from array import array
import argparse
import itertools
import json
import math
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "python"))
from container_openvdb import DenseType, DenseVolume, GridClass, write_dense_array


class RayQueriesCli(unittest.TestCase):
    commands = ("--ray-intersections", "--ray-volume-spans")
    ordinary_ray = (2.25, .25, .25, -7, 0, 0, 0, 3)
    volume_ray = (-2, .25, .25, 3, 0, 0, 0, 48)

    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="container-ray-cli-")
        self.directory = Path(self.temporary.name)
        self.originals = {}

    def tearDown(self):
        try:
            for path, original in self.originals.items():
                self.assertEqual(path.read_bytes(), original, str(path))
            self.assertFalse(list(self.directory.glob(".container-*")))
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
        before = set(self.directory.iterdir())
        result = json.loads(self.call(*args).stdout)
        self.assertEqual(set(self.directory.iterdir()), before, "read-only query created output")
        return result

    def failure(self, *args, parser=False, diagnostic=None):
        before = set(self.directory.iterdir())
        result = self.call(*args, succeeds=False)
        self.assertTrue(result.stderr.strip())
        if parser:
            self.assertNotIn("ENABLE_OPENVDB=ON", result.stderr)
        if diagnostic:
            self.assertIn(diagnostic, result.stderr)
        self.assertEqual(set(self.directory.iterdir()), before)
        return result

    def protect(self, name):
        path = self.directory / name
        self.originals[path] = path.read_bytes()

    def query(self, source="plane.vdb", rays=None, *, volume=False, options=()):
        args = [self.commands[int(volume)], source]
        for ray in rays or ():
            args += ["--ray", *ray]
        return self.report(*args, *options)

    def fixture(self, name="plane.vdb", *, dtype=DenseType.FLOAT64, h=1., grid="plane",
                linear=None, translation=(0, 0, 0), removed=(), empty=False, plateau=False):
        removed = set(removed)
        values, active = array(dtype.typecode), bytearray()
        for z, y, x in itertools.product(range(-3, 4), repeat=3):
            on = not empty and (x, y, z) not in removed
            value = (x - .25)*h
            if plateau:
                value = (0 if x in (0, 1) else x)*h
            values.append(value if on else 4*h)
            active.append(on)
        self.write_dense(name, DenseVolume(
            dtype, (7, 7, 7), values, active, origin=(-3, -3, -3),
            linear=linear or (h, 0, 0, 0, h, 0, 0, 0, h), translation=translation,
            background=array(dtype.typecode, [4*h]), grid_name=grid,
            grid_class=GridClass.LEVEL_SET))

    def write_dense(self, name, volume):
        encoded = str(name) + ".cvarr"
        write_dense_array(self.directory / encoded, volume)
        self.call("--dense-import", encoded, name)
        self.protect(encoded)
        self.protect(name)

    def blocks(self, name="blocks.vdb", coordinates=(0, 8, 24)):
        on = set(coordinates)
        self.write_dense(name, DenseVolume(
            DenseType.FLOAT64, (33, 1, 1), array("d", [0 if x in on else -7 for x in range(33)]),
            bytearray(x in on for x in range(33)), background=array("d", [0]),
            grid_name="blocks", grid_class=GridClass.UNKNOWN))

    def native_text(self, item, expected=None):
        self.assertEqual(set(item), {"text", "hex", "byteCount"})
        self.assertIs(type(item["byteCount"]), int)
        self.assertGreaterEqual(item["byteCount"], 0)
        if item["text"] is not None:
            raw = item["text"].encode("utf-8")
            self.assertEqual(len(raw), item["byteCount"])
            if item["hex"] is not None:
                self.assertEqual(item["hex"], raw.hex())
        else:
            self.assertIsInstance(item["hex"], str)
            raw = bytes.fromhex(item["hex"])
            self.assertEqual(len(raw), item["byteCount"])
        if expected is not None:
            self.assertEqual(raw, expected.encode("utf-8") if isinstance(expected, str) else expected)
        return raw

    def validate(self, result, *, volume=False, count=None):
        self.assertEqual(result["schemaVersion"], 1)
        self.assertIs(result["reportProduced"], True)
        self.assertIs(result["openvdbAvailable"], True)
        self.assertEqual(result["operation"], self.commands[int(volume)][2:])
        self.assertIs(result["coarseVolumeSpans"], volume)
        self.assertIs(result["approximate"], not volume)
        self.assertEqual(result["method"], "native-coarse-active-leaf-tile-spans" if volume
                         else "native-hdda-checked-chord")
        settings, source = result["settings"], result["source"]
        requested, native = settings["requestedLimits"], settings["nativeLimits"]
        self.assertEqual(requested["maxInputBytes"] - native["maxInputBytes"],
                         result["reportReservationBytes"])
        self.assertEqual(requested["maxWorkItems"] - native["maxWorkItems"],
                         result["reportReservationWork"])
        self.assertGreater(result["reportReservationBytes"], 0)
        self.assertGreater(result["reportReservationWork"], 0)
        self.assertEqual(result["combinedWorkUpperBound"],
                         source["workItems"] + result["reportReservationWork"])
        self.assertLessEqual(result["combinedWorkUpperBound"], requested["maxWorkItems"])
        for key in ("descriptorName", "gridName", "valueType"):
            self.native_text(source[key])
        for key in ("inputActiveVoxelCount", "inputActiveTileCount", "inputStoredLeafCount", "workItems"):
            self.assertIs(type(source[key]), int)
            self.assertGreaterEqual(source[key], 0)
        self.assertEqual(len(source["indexToWorld"]), 16)
        self.assertTrue(all(math.isfinite(x) for x in source["indexToWorld"]))
        self.assertIs(type(source["empty"]), bool)
        for item in source["metadata"] + source["fileMetadata"]:
            for key in ("name", "type", "value"):
                self.native_text(item[key])
            self.assertIs(type(item["nativeBytes"]), int)
            self.assertIs(type(item["opaque"]), bool)
        self.assertTrue(result["diagnostics"])
        self.assertTrue(all(isinstance(x, str) for x in result["diagnostics"]))
        if count is not None:
            self.assertEqual(len(result["rays"]), count)
            self.assertEqual(len(settings["rays"]), count)
        if volume:
            self.assertIsNone(settings["sdf"])
            self.assertEqual(result["totalSpanCount"], sum(len(r["spans"]) for r in result["rays"]))
        for ray in result["rays"]:
            self.assertAlmostEqual(math.hypot(*ray["normalizedDirection"]), 1, delta=2e-14)
            if volume:
                for span in ray["spans"]:
                    self.assertLess(span["entryDistance"], span["exitDistance"])
                continue
            self.assertEqual(ray["hit"], ray["kind"] != "miss")
            self.assertIn(ray["normalStatus"], ("no-hit", "not-requested", "available",
                                              "missing-active-footprint", "degenerate-gradient"))
            if not ray["startSampleValid"]:
                self.assertIsNone(ray["startedInside"])
            else:
                self.assertIs(type(ray["startedInside"]), bool)
            if not ray["hit"]:
                for key in ("distance", "position", "sampledResidual", "normal"):
                    self.assertIsNone(ray[key], key)
            elif ray["normalStatus"] == "available":
                self.assertAlmostEqual(math.hypot(*ray["normal"]), 1, delta=2e-13)
            else:
                self.assertIsNone(ray["normal"])

    def hit(self, ray, distance, position, tolerance=1e-9):
        self.assertTrue(ray["hit"])
        self.assertAlmostEqual(ray["distance"], distance, delta=tolerance)
        for actual, expected in zip(ray["position"], position):
            self.assertAlmostEqual(actual, expected, delta=tolerance)
        self.assertAlmostEqual(ray["sampledResidual"], 0, delta=tolerance)

    def test_help_required_input_unknown_flags_and_missing_ray_values(self):
        text = self.call("--help").stdout
        for flag in (*self.commands, "--ray", "--ray-refinements", "--max-ray-spans"):
            self.assertIn(flag, text)
        for command in self.commands:
            for arguments in ((command,), (command, ""), (command, "-h"),
                              (command, "--ray", *self.ordinary_ray)):
                self.failure(*arguments, parser=True, diagnostic="requires")
            for extra in (("extra-output.vdb",), ("--grid-name", "ignored"),
                          ("--voxel-size", 1), ("--stats",), ("--ray", *self.ordinary_ray[:7])):
                self.failure(command, "missing.vdb", *extra, parser=True)
        self.failure(self.commands[0], "missing.vdb", "--max-ray-spans", 1, parser=True)
        for flag, value in (("--ray-isovalue", 0), ("--ray-refinements", 1), ("--ray-normals", "off")):
            self.failure(self.commands[1], "missing.vdb", flag, value, parser=True)

    def test_aliases_singletons_finite_values_and_integer_ranges_are_strict(self):
        for command in self.commands:
            for aliases in (("--grid", "a", "--input-grid", "b"),
                            ("--input-grid", "a", "--grid", "b")):
                self.failure(command, "missing.vdb", *aliases, parser=True, diagnostic="duplicate")
            flags = {"--max-rays": 5, "--max-ray-grids": 2, "--max-ray-work": 100000000,
                     "--max-ray-metadata": 1048576, "--max-voxels": 16000000,
                     "--max-input-bytes": 536870912, "--grid": "plane"}
            for flag, value in flags.items():
                self.failure(command, "missing.vdb", flag, value, flag, value,
                             parser=True, diagnostic="duplicate")
                self.failure(command, "missing.vdb", flag, parser=True)
            for flag, value in (("--max-rays", 0), ("--max-rays", 1000001),
                                ("--max-ray-grids", 4097), ("--max-ray-work", 10000000001),
                                ("--max-ray-work", "+1"), ("--max-ray-metadata", -1),
                                ("--max-input-bytes", 2**64), ("--max-voxels", "1.5")):
                self.failure(command, "missing.vdb", flag, value, parser=True)
        for value in ("nan", "inf", "1e999", "1e-999", " 1", "1junk", "+-1"):
            ray = list(self.ordinary_ray); ray[0] = value
            self.failure(self.commands[0], "missing.vdb", "--ray", *ray, parser=True)
        for flag, value in (("--ray-refinements", 9), ("--ray-refinements", -1),
                            ("--ray-normals", "yes")):
            self.failure(self.commands[0], "missing.vdb", flag, value, parser=True)
        self.failure(self.commands[1], "missing.vdb", "--max-ray-spans", 1000001, parser=True)

    def test_disabled_precedes_paths_report_reservation_and_native_caps(self):
        if self.enabled:
            self.skipTest("OpenVDB is enabled")
        for command in self.commands:
            for extra in ((), ("--ray", *self.ordinary_ray), ("--max-ray-work", 1),
                          ("--max-input-bytes", 1), ("--max-voxels", 1)):
                self.failure(command, "不存在.vdb", *extra, diagnostic="ENABLE_OPENVDB=ON")
        self.assertEqual(list(self.directory.iterdir()), [])

    def test_float_double_plane_clipping_inside_start_and_direction_normalization(self):
        self.enabled_only()
        for dtype in (DenseType.FLOAT32, DenseType.FLOAT64):
            name = dtype.name + ".vdb"
            self.fixture(name, dtype=dtype)
            rays = (self.ordinary_ray, (2.25, .25, .25, -1, 0, 0, 0, 1.5),
                    (2.25, .25, .25, -1, 0, 0, 2.1, 3),
                    (-1.25, .25, .25, 4, 0, 0, 0, 2))
            result = self.query(name, rays)
            self.validate(result, count=4)
            self.hit(result["rays"][0], 2, (.25, .25, .25))
            self.hit(result["rays"][3], 1.5, (.25, .25, .25))
            self.assertFalse(result["rays"][1]["hit"])
            self.assertFalse(result["rays"][2]["hit"])
            self.assertTrue(result["rays"][3]["startedInside"])
            self.assertEqual(result["rays"][0]["normalizedDirection"], [-1, 0, 0])
            self.assertEqual(result["settings"]["rays"][0]["direction"], [-7, 0, 0])
            self.assertEqual(result["rays"][0]["normalStatus"], "available")
            for actual, expected in zip(result["rays"][0]["normal"], (1, 0, 0)):
                self.assertAlmostEqual(actual, expected, delta=1e-12)
            shifted = self.query(name, [self.ordinary_ray],
                                 options=("--ray-isovalue", .5, "--max-voxels", 2**64-1))
            self.validate(shifted, count=1)
            self.hit(shifted["rays"][0], 1.5, (.75, .25, .25))
            self.assertEqual(shifted["settings"]["sdf"]["isovalue"], .5)
            self.assertEqual(shifted["settings"]["requestedLimits"]["maxVoxelCount"], 2**64-1)

    def test_normal_optout_missing_corner_zero_plateau_and_empty_source(self):
        self.enabled_only()
        self.fixture()
        result = self.query(rays=[self.ordinary_ray], options=("--ray-normals", "off", "--ray-refinements", 0))
        self.validate(result, count=1)
        self.assertEqual(result["rays"][0]["normalStatus"], "not-requested")
        self.assertEqual(result["settings"]["sdf"], {"isovalue": 0, "refinementSteps": 0, "computeNormals": False})
        self.fixture("missing.vdb", removed=((1, 1, 1),))
        missing = self.query("missing.vdb", [(2.25, 0, 0, -1, 0, 0, 0, 3)])
        self.validate(missing, count=1)
        self.hit(missing["rays"][0], 2, (.25, 0, 0))
        self.assertEqual(missing["rays"][0]["normalStatus"], "missing-active-footprint")
        self.fixture("plateau.vdb", plateau=True)
        zero = self.query("plateau.vdb", [(.25, .25, .25, 1, 0, 0, 0, 1)])
        self.validate(zero, count=1)
        self.hit(zero["rays"][0], 0, (.25, .25, .25))
        self.assertEqual(zero["rays"][0]["kind"], "sampled-zero")
        self.assertEqual(zero["rays"][0]["normalStatus"], "degenerate-gradient")
        self.fixture("empty.vdb", empty=True)
        empty = self.query("empty.vdb", [self.ordinary_ray])
        self.validate(empty, count=1)
        self.assertTrue(empty["source"]["empty"])
        self.assertFalse(empty["rays"][0]["hit"])

    def test_rotated_reflected_scaled_translated_plane_uses_world_units(self):
        self.enabled_only()
        h, angle, translation = 2., .37, (100.125, -50., 7.)
        c, s = math.cos(angle), math.sin(angle)
        matrix = (-h*c, -h*s, 0, -h*s, h*c, 0, 0, 0, h)
        self.fixture(h=h, linear=matrix, translation=translation)
        def mapped(point):
            return tuple(translation[a] + sum(matrix[3*a+b]*point[b] for b in range(3)) for a in range(3))
        origin, position = mapped((2.25, .25, .25)), mapped((.25, .25, .25))
        ray = (*origin, *(position[a]-origin[a] for a in range(3)), 0, 6)
        result = self.query(rays=[ray])
        self.validate(result, count=1)
        self.hit(result["rays"][0], 4, position, 2e-10)
        for actual, expected in zip(result["rays"][0]["normal"], (-c, -s, 0)):
            self.assertAlmostEqual(actual, expected, delta=1e-11)
        affine = result["source"]["indexToWorld"]
        self.assertEqual(affine[12:], [0, 0, 0, 1])
        for axis in range(3):
            self.assertAlmostEqual(sum(affine[4*axis+b]*.25 for b in range(3)) + affine[4*axis+3],
                                   position[axis], delta=1e-12)

    def test_public_sphere_first_entry_inside_exit_miss_and_distance_bounds(self):
        self.enabled_only()
        self.call("--sphere", "sphere.vdb", "--radius", 1, "--voxel-size", .25)
        self.protect("sphere.vdb")
        result = self.query("sphere.vdb", [(-2, 0, 0, 5, 0, 0, 0, 4),
                                          (0, 0, 0, 1, 0, 0, 0, 2),
                                          (-2, 2, 0, 1, 0, 0, 0, 4),
                                          (-2, 0, 0, 1, 0, 0, 0, .5)])
        self.validate(result, count=4)
        self.hit(result["rays"][0], 1, (-1, 0, 0), 1e-7)
        self.hit(result["rays"][1], 1, (1, 0, 0), 1e-7)
        self.assertFalse(result["rays"][1]["startSampleValid"])
        self.assertFalse(result["rays"][2]["hit"])
        self.assertFalse(result["rays"][3]["hit"])

    def test_coarse_activity_spans_merge_clip_and_ignore_inactive_values(self):
        self.enabled_only()
        self.blocks()
        result = self.query("blocks.vdb", [self.volume_ray, (-2, .25, .25, 1, 0, 0, 4, 12)], volume=True)
        self.validate(result, volume=True, count=2)
        self.assertEqual(result["totalSpanCount"], 3)
        for ray, expected in zip(result["rays"], (((1.5, 17.5), (25.5, 33.5)), ((4, 12),))):
            self.assertEqual(len(ray["spans"]), len(expected))
            for span, (entry, exit) in zip(ray["spans"], expected):
                self.assertAlmostEqual(span["entryDistance"], entry, delta=1e-10)
                self.assertAlmostEqual(span["exitDistance"], exit, delta=1e-10)
                self.assertAlmostEqual(span["entryPosition"][0], -2+entry, delta=1e-10)
                self.assertAlmostEqual(span["exitPosition"][0], -2+exit, delta=1e-10)
        self.failure(self.commands[1], "blocks.vdb", "--ray", *self.volume_ray, "--max-ray-spans", 1)

    def test_empty_batch_selectors_unicode_repeatability_and_readonly_files(self):
        self.enabled_only()
        self.fixture(grid="descriptor:plane")
        options = ("--input-grid", "descriptor:descriptor:plane")
        for volume in (False, True):
            empty = self.query(volume=volume, options=options)
            self.validate(empty, volume=volume, count=0)
            self.native_text(empty["source"]["gridName"], "descriptor:plane")
            self.assertGreater(empty["source"]["workItems"], 0)
        self.failure(self.commands[0], "plane.vdb", "--grid", "descriptor:missing")
        unicode = self.directory / "表面_é.vdb"
        unicode.write_bytes((self.directory / "plane.vdb").read_bytes())
        self.protect(unicode)
        first = self.query(unicode, [self.ordinary_ray, self.ordinary_ray], options=options)
        second = self.query(unicode, [self.ordinary_ray, self.ordinary_ray], options=options)
        self.validate(first, count=2)
        self.assertEqual(first, second)
        self.assertEqual(first["rays"][0], first["rays"][1])
        self.assertEqual(first["settings"]["input"], str(unicode))

    def test_shared_work_bytes_metadata_count_limits_and_invalid_native_geometry(self):
        self.enabled_only()
        self.fixture()
        for command in self.commands:
            for flag in ("--max-ray-work", "--max-input-bytes", "--max-voxels", "--max-ray-metadata"):
                self.failure(command, "plane.vdb", flag, 1)
            self.failure(command, "plane.vdb", "--max-rays", 1,
                         "--ray", *self.ordinary_ray, "--ray", *self.ordinary_ray)
            for ray in ((0, 0, 0, 0, 0, 0, 0, 1), (0, 0, 0, 1, 0, 0, -1, 1),
                        (0, 0, 0, 1, 0, 0, 1, 1)):
                self.failure(command, "plane.vdb", "--ray", *ray)
        self.failure(self.commands[0], "plane.vdb", "--ray-isovalue", 3)
        self.fixture("nonuniform.vdb", linear=(1, 0, 0, 0, 2, 0, 0, 0, 1))
        self.failure(self.commands[0], "nonuniform.vdb", "--ray", *self.ordinary_ray)
        (self.directory / "malformed.vdb").write_bytes(b"not a VDB archive")
        self.protect("malformed.vdb")
        self.failure(self.commands[0], "malformed.vdb")
        self.failure(self.commands[1], "malformed.vdb")

    def test_native_generator_tiny_signs_nine_types_and_binary_metadata(self):
        self.enabled_only()
        if self.generator is None:
            self.skipTest("real native --generator is required for tiny values/nine native types/binary metadata")
        generated = self.directory / "native-fixtures"
        generated.mkdir()
        result = subprocess.run([str(self.generator), "--ray-fixtures", str(generated)],
                                capture_output=True, text=True, encoding="utf-8", timeout=90)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(json.loads(result.stdout), {"schemaVersion": 1, "mode": "ray", "files": 4})
        for name in ("ray-weak-same.vdb", "ray-weak-opposite.vdb", "ray-volume-nine.vdb", "ray-spans.vdb"):
            self.assertTrue((generated / name).is_file(), name)
            self.protect(generated / name)
        same = self.query(generated / "ray-weak-same.vdb", [(0, .25, .25, 1, 0, 0, 0, 1)])
        opposite = self.query(generated / "ray-weak-opposite.vdb", [(0, .25, .25, 1, 0, 0, 0, 1)],
                              options=("--ray-normals", "off"))
        self.validate(same, count=1); self.validate(opposite, count=1)
        self.assertFalse(same["rays"][0]["hit"])
        self.hit(opposite["rays"][0], .25, (.25, .25, .25), 1e-12)
        for grid in ("float", "double", "bool", "mask", "int32", "int64", "vec3i", "vec3s", "vec3d"):
            value = self.query(generated / "ray-volume-nine.vdb", [self.volume_ray], volume=True,
                               options=("--grid", "descriptor:" + grid))
            self.validate(value, volume=True, count=1)
            self.native_text(value["source"]["gridName"], grid)
            self.assertEqual(value["source"]["inputActiveVoxelCount"], 1)
            spans = value["rays"][0]["spans"]
            self.assertEqual(len(spans), 1)
            self.assertAlmostEqual(spans[0]["entryDistance"], 1.5, delta=1e-10)
            self.assertAlmostEqual(spans[0]["exitDistance"], 9.5, delta=1e-10)
            for catalog in (value["source"]["metadata"], value["source"]["fileMetadata"]):
                named = {self.native_text(item["name"]): item for item in catalog}
                binary = named[b"binary"]["value"]
                self.assertIsNone(binary["text"])
                self.native_text(binary, b"raw\x00\xff")
                self.native_text(named[b"exact_id"]["value"], "9007199254740993")
        spans = self.query(generated / "ray-spans.vdb", [self.volume_ray], volume=True)
        self.validate(spans, volume=True, count=1)
        self.assertEqual([(s["entryDistance"], s["exitDistance"])
                          for s in spans["rays"][0]["spans"]], [(1.5, 17.5), (25.5, 33.5)])


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--tool", required=True, type=Path)
    parser.add_argument("--enabled", required=True, choices=("0", "1"))
    parser.add_argument("--generator", type=Path)
    args, remaining = parser.parse_known_args()
    RayQueriesCli.tool = args.tool.resolve(strict=True)
    RayQueriesCli.enabled = args.enabled == "1"
    RayQueriesCli.generator = args.generator.resolve(strict=True) if args.generator else None
    unittest.main(argv=[__file__, *remaining])
