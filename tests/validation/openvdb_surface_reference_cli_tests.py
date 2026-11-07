"""Real surface-reference CLI with an independent stdlib FLOAT EXR decoder.

Camera, radial depth, coverage and status oracles come from analytic geometry.
Only public dense/sphere routes create VDB fixtures; no private VDB decoder,
image library, substitute tool or precomputed renderer output is used.
"""

from array import array
import argparse
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
from container_openvdb import DenseType, DenseVolume, GridClass, write_dense_array


CHANNELS = sorted(("R", "G", "B", "A", "Z", "N.X", "N.Y", "N.Z",
                   "N.VALID", "N.STATUS", "HIT.KIND"))


class FloatScanlineExr:
    """Read a single-part, uncompressed, non-tiled OpenEXR v2 independently."""

    def __init__(self, data):
        self.data = data
        if len(data) < 9 or struct.unpack_from("<I", data)[0] != 20000630:
            raise ValueError("invalid OpenEXR magic")
        version, = struct.unpack_from("<I", data, 4)
        if version != 2:
            raise ValueError("expected single-part scanline OpenEXR v2")
        self.attributes = {}
        cursor = 8
        while data[cursor:cursor + 1] != b"\0":
            name, cursor = self._cstring(cursor)
            kind, cursor = self._cstring(cursor)
            length, = self._unpack("<I", cursor)
            cursor += 4
            end = cursor + length
            if end > len(data) or name in self.attributes:
                raise ValueError("truncated or duplicate EXR attribute")
            self.attributes[name] = (kind, data[cursor:end])
            cursor = end
        cursor += 1
        if self.attributes.get("compression") != ("compression", b"\0"):
            raise ValueError("expected uncompressed EXR")
        if self.attributes.get("lineOrder") != ("lineOrder", b"\0"):
            raise ValueError("expected increasing scanline order")
        kind, window = self.attributes["dataWindow"]
        if kind != "box2i" or len(window) != 16:
            raise ValueError("invalid data window")
        xmin, ymin, xmax, ymax = struct.unpack("<4i", window)
        self.width, self.height = xmax - xmin + 1, ymax - ymin + 1
        if xmin or ymin or not 1 <= self.width <= 2048 or not 1 <= self.height <= 2048:
            raise ValueError("unexpected EXR data window")
        kind, raw_channels = self.attributes["channels"]
        if kind != "chlist":
            raise ValueError("missing channel list")
        self.names = []
        offset = 0
        while offset < len(raw_channels) and raw_channels[offset] != 0:
            end = raw_channels.index(0, offset)
            name = raw_channels[offset:end].decode("ascii")
            offset = end + 1
            if offset + 16 > len(raw_channels):
                raise ValueError("truncated channel record")
            pixel_type, linear, reserved, xs, ys = struct.unpack_from(
                "<iB3sii", raw_channels, offset)
            offset += 16
            if pixel_type != 2 or xs != 1 or ys != 1 or reserved != b"\0\0\0":
                raise ValueError("expected full-resolution FLOAT channels")
            if linear not in (0, 1) or name in self.names:
                raise ValueError("invalid or duplicate channel")
            self.names.append(name)
        if offset != len(raw_channels) - 1 or raw_channels[-1:] != b"\0":
            raise ValueError("invalid channel-list termination")
        if self.names != CHANNELS:
            raise ValueError("expected eleven lexical surface-reference channels")
        offsets = self._unpack("<" + "Q" * self.height, cursor)
        table_end = cursor + 8 * self.height
        self.channels = {name: [None] * (self.width * self.height) for name in self.names}
        seen, ranges = set(), []
        expected_size = self.width * len(self.names) * 4
        for offset in offsets:
            if offset < table_end:
                raise ValueError("scanline overlaps EXR header")
            y, length = self._unpack("<iI", offset)
            start, end = offset + 8, offset + 8 + length
            if y in seen or not 0 <= y < self.height or length != expected_size or end > len(data):
                raise ValueError("invalid uncompressed scanline")
            seen.add(y)
            ranges.append((offset, end))
            for c, name in enumerate(self.names):
                row = self._unpack("<" + "f" * self.width, start + c * self.width * 4)
                if not all(math.isfinite(value) for value in row):
                    raise ValueError("nonfinite reference AOV")
                self.channels[name][y * self.width:(y + 1) * self.width] = row
        ranges.sort()
        if not ranges or ranges[0][0] != table_end or ranges[-1][1] != len(data):
            raise ValueError("unexpected EXR chunk framing")
        if any(left[1] != right[0] for left, right in zip(ranges, ranges[1:])):
            raise ValueError("overlapping or unaccounted EXR chunks")
        if len(seen) != self.height:
            raise ValueError("missing EXR scanline")

    def _unpack(self, fmt, offset):
        if offset < 0 or offset + struct.calcsize(fmt) > len(self.data):
            raise ValueError("truncated EXR record")
        return struct.unpack_from(fmt, self.data, offset)

    def _cstring(self, offset):
        end = self.data.find(b"\0", offset)
        if end < 0:
            raise ValueError("unterminated EXR string")
        return self.data[offset:end].decode("ascii"), end + 1

    def __getitem__(self, name):
        return self.channels[name]


class SurfaceReferenceCli(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="container-surface-reference-cli-")
        self.directory = Path(self.temporary.name)
        self.originals = {}

    def tearDown(self):
        try:
            for path, original in self.originals.items():
                self.assertEqual(path.read_bytes(), original, str(path))
            self.assertFalse(list(self.directory.glob(".container-*")), "private file leaked")
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

    def protect(self, name):
        path = self.directory / name
        self.originals[path] = path.read_bytes()

    def failure(self, *args, diagnostic=None, parser=False):
        before = {p: p.read_bytes() for p in self.directory.iterdir() if p.is_file()}
        result = self.call(*args, succeeds=False)
        self.assertFalse(result.stdout.strip(), "failure emitted a success summary")
        self.assertTrue(result.stderr.strip())
        if parser:
            self.assertNotIn("ENABLE_OPENVDB=ON", result.stderr)
        if diagnostic:
            self.assertIn(diagnostic, result.stderr)
        after = {p: p.read_bytes() for p in self.directory.iterdir() if p.is_file()}
        self.assertEqual(after, before, "failed command modified or published a file")
        return result

    def fixture(self, name="plane.vdb", *, dtype=DenseType.FLOAT64, h=1.,
                grid="plane", linear=None, translation=(0, 0, 0),
                removed=(), empty=False, plateau=False):
        values, active = array(dtype.typecode), bytearray()
        removed = set(removed)
        for z, y, x in itertools.product(range(-3, 4), repeat=3):
            on = not empty and (x, y, z) not in removed
            value = ((0 if z in (0, 1) else z) if plateau else z - .25) * h
            values.append(value if on else 4 * h)
            active.append(on)
        volume = DenseVolume(
            dtype, (7, 7, 7), values, active, origin=(-3, -3, -3),
            linear=linear or (h, 0, 0, 0, h, 0, 0, 0, h), translation=translation,
            background=array(dtype.typecode, [4 * h]), grid_name=grid,
            grid_class=GridClass.LEVEL_SET)
        encoded = str(name) + ".cvarr"
        write_dense_array(self.directory / encoded, volume)
        self.call("--dense-import", encoded, name)
        self.protect(encoded)
        self.protect(name)

    def render(self, source="plane.vdb", output="surface.exr", *args):
        arguments = ["--surface-reference", source, output, *args]
        if "--width" not in args:
            arguments += ["--width", 1]
        if "--height" not in args:
            arguments += ["--height", 1]
        if "--max-ray-work" not in args:
            arguments += ["--max-ray-work", 2_000_000_000]
        result = json.loads(self.call(*arguments).stdout)
        exr = FloatScanlineExr((self.directory / output).read_bytes())
        self.validate(result, exr, output)
        return result, exr

    def native_text(self, item, expected=None):
        self.assertIs(type(item["byteCount"]), int)
        if item["text"] is not None:
            raw = item["text"].encode("utf-8")
            if item["hex"] is not None:
                self.assertEqual(raw.hex(), item["hex"])
        else:
            raw = bytes.fromhex(item["hex"])
        self.assertEqual(len(raw), item["byteCount"])
        if expected is not None:
            self.assertEqual(raw, expected.encode("utf-8"))

    def validate(self, report, exr, output):
        self.assertEqual(report["schemaVersion"], 1)
        self.assertEqual(report["operation"], "surface-reference")
        for key in ("reportProduced", "openvdbAvailable", "outputPublished"):
            self.assertIs(report[key], True)
        self.assertEqual(Path(report["output"]).name, Path(output).name)
        dimensions = report["dimensions"]
        self.assertEqual((dimensions["width"], dimensions["height"]), (exr.width, exr.height))
        count = exr.width * exr.height
        self.assertEqual(dimensions["pixelCount"], count)
        self.assertEqual(report["format"]["channels"], CHANNELS)
        self.assertEqual(report["format"]["compression"], "none")
        self.assertEqual(report["format"]["storage"], "float32")
        hits = sum(kind != 0 for kind in exr["HIT.KIND"])
        self.assertEqual(report["results"]["hitCount"], hits)
        self.assertEqual(report["results"]["missCount"], count - hits)
        self.assertEqual(report["results"]["unavailableNormalCount"],
                         sum(kind != 0 and normal != 2
                             for kind, normal in zip(exr["HIT.KIND"], exr["N.STATUS"])))
        for code, name in enumerate(("notRequested", "noHit", "available",
                                     "missingActiveFootprint", "degenerateGradient")):
            self.assertEqual(report["results"]["normalStatusCounts"][name],
                             exr["N.STATUS"].count(code))
        for code, name in enumerate(("miss", "sampledZero", "bracketedCrossing")):
            self.assertEqual(report["results"]["kindCounts"][name], exr["HIT.KIND"].count(code))
        for i, kind in enumerate(exr["HIT.KIND"]):
            self.assertIn(kind, (0, 1, 2))
            self.assertIn(exr["N.STATUS"][i], (0, 1, 2, 3, 4))
            self.assertEqual(exr["A"][i], float(kind != 0))
            self.assertEqual(exr["N.VALID"][i], float(exr["N.STATUS"][i] == 2))
            if not kind:
                self.assertEqual(exr["Z"][i], 0)
            if exr["N.STATUS"][i] != 2:
                for channel in ("N.X", "N.Y", "N.Z"):
                    self.assertEqual(exr[channel][i], 0)
        budget, work = report["budgets"], report["work"]
        self.assertGreater(budget["reportReservationBytes"], 0)
        self.assertEqual(budget["requestedInputBytes"] - budget["nativeInputBytes"],
                         budget["reportReservationBytes"])
        self.assertEqual(budget["requestedMaxWork"] - budget["nativeMaxWork"],
                         work["reportReservation"])
        self.assertGreater(work["reportReservation"], 0)
        self.assertEqual(work["combinedUpperBound"],
                         work["nativeAndImage"] + work["reportReservation"])
        self.assertLessEqual(work["combinedUpperBound"], budget["requestedMaxWork"])
        self.assertGreater(budget["nativeExportBytes"], 0)
        self.assertEqual(budget["nativeExportBytes"],
                         min(budget["requestedExportBytes"], budget["requestedInputBytes"]) -
                         budget["reportReservationBytes"])
        conversion = report["conversion"]
        self.assertEqual(conversion["encodedBytes"], len(exr.data))
        for key in ("shaderRoundedValues", "exportRoundedValues", "encodedBytes"):
            self.assertIs(type(conversion[key]), int)
            self.assertGreaterEqual(conversion[key], 0)
        for key in ("maxShaderAbsoluteError", "maxExportAbsoluteError", "maxExportRelativeError"):
            self.assertTrue(math.isfinite(conversion[key]))
            self.assertGreaterEqual(conversion[key], 0)
        source = report["source"]
        for key in ("descriptorName", "gridName", "valueType"):
            self.native_text(source[key])
        self.assertEqual(len(source["indexToWorld"]), 16)
        self.assertTrue(all(math.isfinite(value) for value in source["indexToWorld"]))

    def assert_pixel(self, exr, index, *, depth, color=None, normal=None,
                     status=2, kind=None, tolerance=1e-6):
        self.assertAlmostEqual(exr["Z"][index], depth, delta=tolerance)
        self.assertEqual(exr["A"][index], 1)
        self.assertEqual(exr["N.STATUS"][index], status)
        if kind is not None:
            self.assertEqual(exr["HIT.KIND"][index], kind)
        else:
            self.assertNotEqual(exr["HIT.KIND"][index], 0)
        if color is not None:
            for channel, expected in zip(("R", "G", "B"), color):
                self.assertAlmostEqual(exr[channel][index], expected, delta=tolerance)
        if normal is not None:
            for channel, expected in zip(("N.X", "N.Y", "N.Z"), normal):
                self.assertAlmostEqual(exr[channel][index], expected, delta=tolerance)

    def test_help_path_arity_and_incompatible_flags(self):
        help_text = self.call("--help").stdout
        for flag in ("--surface-reference", "--surface-shader", "--max-pixels",
                     "--max-export-bytes", "--projection"):
            self.assertIn(flag, help_text)
        for args in (("--surface-reference",), ("--surface-reference", "a.vdb"),
                     ("--surface-reference", "", "out.exr"),
                     ("--surface-reference", "a.vdb", "-h"),
                     ("--surface-reference", "a.vdb", "--width", 1)):
            with self.subTest(args=args):
                self.failure(*args, diagnostic="requires", parser=True)
        base = ("--surface-reference", "missing.vdb", "bad.exr")
        for extra in (("ignored.exr",), ("--voxel-size", 1), ("--half-width", 4),
                      ("--grid-name", "ignored"), ("--stats",), ("--max-triangles", 1),
                      ("--max-ray-spans", 1), ("--surface-field-double",)):
            with self.subTest(extra=extra):
                self.failure(*base, *extra, diagnostic="unknown/incompatible", parser=True)

    def test_strict_unique_aliases_numbers_and_camera_mode_options(self):
        base = ("--surface-reference", "missing.vdb", "bad.exr")
        flags = {
            "--projection": ("perspective",), "--eye": (0, 0, 3),
            "--target": (0, 0, 0), "--up": (0, 1, 0), "--fov": (45,),
            "--ray-min": (0,), "--ray-max": (20,), "--width": (1,), "--height": (1,),
            "--max-pixels": (1,), "--surface-shader": ("headlight",),
            "--color": (1, 1, 1), "--background": (0, 0, 0),
            "--invalid-normal-color": (1, 0, 1), "--grid": ("plane",),
            "--ray-isovalue": (0,), "--ray-refinements": (0,), "--ray-normals": ("on",),
            "--max-rays": (1,), "--max-ray-grids": (256,), "--max-ray-work": (200000000,),
            "--max-ray-metadata": (1048576,), "--max-voxels": (16000000,),
            "--max-input-bytes": (536870912,), "--max-export-bytes": (536870912,),
        }
        for flag, values in flags.items():
            with self.subTest(duplicate=flag):
                self.failure(*base, flag, *values, flag, *values,
                             diagnostic="duplicate", parser=True)
            with self.subTest(missing=flag):
                self.failure(*base, flag, diagnostic="required", parser=True)
        for args in (("--grid", "a", "--input-grid", "b"),
                     ("--fov", 45, "--vertical-fov", 60),
                     ("--projection", "orthographic", "--ortho-height", 2,
                      "--orthographic-height", 3)):
            self.failure(*base, *args, diagnostic="duplicate", parser=True)
        for flag, value in (("--width", 0), ("--height", 2049), ("--width", "+1"),
                            ("--max-pixels", "1.5"), ("--max-rays", 1000001),
                            ("--max-ray-grids", 4097), ("--max-ray-work", 10000000001),
                            ("--max-ray-metadata", 0), ("--max-input-bytes", 0),
                            ("--max-voxels", -1), ("--max-export-bytes", 2**64),
                            ("--fov", 151), ("--ray-min", -1), ("--ray-max", "inf"),
                            ("--ray-isovalue", "nan"), ("--ray-isovalue", "1e-400"),
                            ("--ray-refinements", 9), ("--ray-normals", "true"),
                            ("--projection", "unknown"), ("--surface-shader", "physical")):
            with self.subTest(flag=flag, value=value):
                diagnostic = "camera FOV/height/radial" if flag in ("--fov", "--ray-min") else flag
                self.failure(*base, flag, value, diagnostic=diagnostic, parser=True)
        for args, diagnostic in (
                (("--projection", "perspective", "--ortho-height", 2), "--ortho-height"),
                (("--projection", "orthographic", "--fov", 45), "--fov"),
                (("--surface-shader", "matte", "--invalid-normal-color", 1, 0, 1),
                 "--invalid-normal-color"),
                (("--eye", 0, 0), "required"), (("--color", 0, 0, "1e-400"), "--color"),
                (("--background", -1, 0, 0), "colors"),
                (("--invalid-normal-color", 2, 0, 0), "colors")):
            with self.subTest(args=args):
                self.failure(*base, *args, parser=True, diagnostic=diagnostic)

    def test_disabled_availability_precedes_paths_and_tiny_valid_caps(self):
        if self.enabled:
            self.skipTest("OpenVDB is enabled")
        for args in ((), ("--projection", "orthographic", "--ortho-height", 2),
                     ("--color", "5e-324", 1, 1, "--ray-refinements", 0,
                      "--width", 1, "--height", 1, "--max-pixels", 1, "--max-rays", 1,
                      "--max-input-bytes", 1, "--max-export-bytes", 1,
                      "--max-ray-work", 1, "--max-ray-metadata", 1)):
            self.failure("--surface-reference", "不存在.vdb", "bad.exr", *args,
                         diagnostic="ENABLE_OPENVDB=ON")
        self.failure("--surface-reference", "./-h", "bad.exr",
                     diagnostic="ENABLE_OPENVDB=ON")
        self.assertEqual(list(self.directory.iterdir()), [])

    def test_plane_depth_native_shader_colors_and_status_counters(self):
        self.enabled_only()
        self.fixture()
        for shader, color in (("matte", (.25, .5, 1)),
                              ("normal", (.125, .25, 1)),
                              ("headlight", (.25, .5, 1))):
            with self.subTest(shader=shader):
                args = ["--surface-shader", shader, "--color", .25, .5, 1]
                if shader == "matte":
                    args += ["--ray-normals", "off"]
                report, exr = self.render("plane.vdb", shader + ".exr", *args)
                self.assert_pixel(exr, 0, depth=2.75, color=color,
                                  normal=None if shader == "matte" else (0, 0, 1),
                                  status=0 if shader == "matte" else 2, tolerance=0)
                self.assertEqual(report["conversion"]["shaderRoundedValues"], 0)
                self.assertEqual(report["camera"]["projection"], "perspective")
                self.assertIsNone(report["camera"]["orthographicHeight"])
                self.native_text(report["source"]["valueType"], "double")

    def test_non_square_perspective_and_orthographic_pixel_centers(self):
        self.enabled_only()
        self.fixture()
        perspective, exr = self.render(
            "plane.vdb", "perspective.exr", "--eye", 0, 0, 2,
            "--vertical-fov", 90, "--width", 4, "--height", 2)
        self.assertEqual(perspective["results"]["hitCount"], 8)
        self.assertEqual(perspective["camera"]["verticalFovDegrees"], 90)
        for y in range(2):
            for x in range(4):
                sx, sy = (2 * (x + .5) / 4 - 1) * 2, 1 - 2 * (y + .5) / 2
                self.assert_pixel(exr, y * 4 + x,
                                  depth=1.75 * math.hypot(sx, sy, 1), normal=(0, 0, 1))
        orthographic, exr = self.render(
            "plane.vdb", "orthographic.exr", "--projection", "orthographic",
            "--orthographic-height", 2, "--eye", 0, 0, 2,
            "--width", 4, "--height", 2)
        self.assertEqual(orthographic["results"]["hitCount"], 8)
        self.assertIsNone(orthographic["camera"]["verticalFovDegrees"])
        self.assertEqual(orthographic["camera"]["orthographicHeight"], 2)
        for i in range(8):
            self.assert_pixel(exr, i, depth=1.75, normal=(0, 0, 1), tolerance=0)
        # An independently rotated plane makes orthographic top-row origins and
        # X-fastest positions observable in depth, rather than just in normals.
        n = (.3, .4, math.sqrt(.75))
        scale = math.hypot(n[0], n[2])
        u = (n[2] / scale, 0, -n[0] / scale)
        v = (n[1]*u[2], n[2]*u[0] - n[0]*u[2], -n[1]*u[0])
        matrix = tuple(column[row] for row in range(3) for column in (u, v, n))
        self.fixture("tilted.vdb", linear=matrix)
        _, tilted = self.render("tilted.vdb", "tilted.exr", "--projection", "orthographic",
                                "--ortho-height", 2, "--eye", 0, 0, 2,
                                "--width", 4, "--height", 2)
        for y in range(2):
            for x in range(4):
                ox, oy = (2 * (x + .5) / 4 - 1) * 2, 1 - 2 * (y + .5) / 2
                z = (.25 - n[0]*ox - n[1]*oy) / n[2]
                self.assert_pixel(tilted, y*4 + x, depth=2-z, normal=n)
        self.assertLess(tilted["Z"][0], tilted["Z"][3])
        self.assertGreater(tilted["Z"][0], tilted["Z"][4])

    def test_radial_clipping_and_miss_background_conversion(self):
        self.enabled_only()
        self.fixture()
        common = ("--eye", 0, 0, 2, "--fov", 90, "--width", 4, "--height", 2)
        report, exr = self.render("plane.vdb", "clipped.exr", *common,
                                  "--ray-max", 2, "--background", .1, .25, .5)
        self.assertEqual(report["results"]["hitCount"], 0)
        self.assertGreater(report["conversion"]["shaderRoundedValues"], 0)
        f32 = struct.unpack("<f", struct.pack("<f", .1))[0]
        for i in range(8):
            self.assertEqual((exr["R"][i], exr["G"][i], exr["B"][i]), (f32, .25, .5))
            self.assertEqual(exr["A"][i], 0)
            self.assertEqual(exr["N.STATUS"][i], 1)
        report, exr = self.render("plane.vdb", "minimum.exr", *common,
                                  "--ray-min", 2, "--ray-max", 20)
        self.assertEqual(report["results"]["hitCount"], 8)
        self.assertGreater(exr["Z"][0], 3)

    def test_sphere_silhouette_depth_and_top_row_order(self):
        self.enabled_only()
        self.call("--sphere", "sphere.vdb", "--radius", 1, "--voxel-size", .125)
        self.protect("sphere.vdb")
        report, exr = self.render("sphere.vdb", "sphere.exr",
                                  "--projection", "orthographic", "--ortho-height", 4,
                                  "--width", 5, "--height", 5)
        expected = {7, 11, 12, 13, 17}
        self.assertEqual(report["results"]["hitCount"], len(expected))
        for i in range(25):
            self.assertEqual(exr["A"][i] == 1, i in expected, i)
            if i in expected:
                x, y = (i % 5 - 2) * .8, (2 - i // 5) * .8
                self.assertAlmostEqual(exr["Z"][i], 3 - math.sqrt(1 - x*x - y*y), delta=.04)
        self.assert_pixel(exr, 12, depth=2, tolerance=1e-6)
        self.assertGreater(exr["N.Y"][7], 0)
        self.assertLess(exr["N.Y"][17], 0)
        self.assertLess(exr["N.X"][11], 0)
        self.assertGreater(exr["N.X"][13], 0)

    def test_default_32_pixel_square_sphere_uses_image_work_budget(self):
        self.enabled_only()
        self.call("--sphere", "sphere.vdb", "--radius", 1, "--voxel-size", .125)
        self.protect("sphere.vdb")
        # A deliberately smaller caller cap still rejects before publication.
        self.failure("--surface-reference", "sphere.vdb", "limited.exr",
                     "--max-ray-work", 200_000_000, diagnostic="maxWorkItems")
        # Bypass render(): this is the public workflow with no resolution,
        # camera, work, or other surface-reference option overrides.
        output = "default.exr"
        report = json.loads(self.call("--surface-reference", "sphere.vdb", output).stdout)
        exr = FloatScanlineExr((self.directory / output).read_bytes())
        self.validate(report, exr, output)
        self.assertEqual((exr.width, exr.height), (32, 32))
        self.assertEqual(report["dimensions"]["pixelCount"], 1024)
        self.assertEqual(report["budgets"]["requestedMaxWork"], 2_000_000_000)
        self.assertGreater(report["work"]["combinedUpperBound"], 200_000_000)
        self.assertGreater(report["results"]["hitCount"], 0)
        self.assertGreater(report["results"]["missCount"], 0)
        self.assertEqual(report["results"]["unavailableNormalCount"], 0)
        self.assertEqual(sum(exr["A"]) + report["results"]["missCount"], 1024)

    def test_missing_degenerate_and_unrequested_normals_keep_hit_coverage(self):
        self.enabled_only()
        self.fixture(removed=((1, 1, 1),))
        report, exr = self.render("plane.vdb", "missing.exr", "--surface-shader", "normal",
                                  "--invalid-normal-color", .25, .5, .75)
        self.assert_pixel(exr, 0, depth=2.75, color=(.25, .5, .75), status=3, tolerance=0)
        self.assertEqual(report["results"]["unavailableNormalCount"], 1)
        self.fixture("ordinary.vdb")
        _, exr = self.render("ordinary.vdb", "unrequested.exr", "--ray-normals", "off",
                              "--invalid-normal-color", .25, .5, .75)
        self.assert_pixel(exr, 0, depth=2.75, color=(.25, .5, .75), status=0, tolerance=0)
        self.fixture("plateau.vdb", plateau=True)
        _, exr = self.render("plateau.vdb", "plateau.exr", "--eye", 0, 0, .25,
                              "--target", 0, 0, -1, "--invalid-normal-color", .25, .5, .75)
        self.assert_pixel(exr, 0, depth=0, color=(.25, .5, .75), status=4, kind=1, tolerance=0)

    def test_float_double_reflected_rotated_world_maps_and_isovalue(self):
        self.enabled_only()
        angle = .37
        sine, cosine = math.sin(angle), math.cos(angle)
        translation = (100.125, -50, 7)
        for index, (dtype, h) in enumerate(((DenseType.FLOAT32, .5), (DenseType.FLOAT64, 2))):
            matrix = (h*cosine, 0, -h*sine, 0, h, 0, -h*sine, 0, -h*cosine)
            name = f"mapped-{index}.vdb"
            self.fixture(name, dtype=dtype, h=h, linear=matrix, translation=translation)
            eye = tuple(translation[a] + 3 * matrix[a*3 + 2] for a in range(3))
            report, exr = self.render(name, f"mapped-{index}.exr", "--eye", *eye,
                                      "--target", *translation, "--up", 0, 1, 0,
                                      "--surface-shader", "normal", "--ray-isovalue", .5*h)
            self.assert_pixel(exr, 0, depth=2.25*h, normal=(-sine, 0, -cosine),
                              color=((1-sine)*.5, .5, (1-cosine)*.5), tolerance=2e-6)
            self.native_text(report["source"]["valueType"], "float" if index == 0 else "double")
            for row in range(3):
                for column in range(3):
                    self.assertAlmostEqual(report["source"]["indexToWorld"][4*row + column],
                                           matrix[3*row + column], delta=1e-12)

    def test_true_empty_source_unicode_selectors_and_exclusive_outputs(self):
        self.enabled_only()
        source, output = "source_球.vdb", "surface_球_é.exr"
        self.fixture(source, grid="descriptor:plane")
        report, exr = self.render(source, output, "--input-grid", "descriptor:descriptor:plane")
        self.native_text(report["source"]["gridName"], "descriptor:plane")
        self.assert_pixel(exr, 0, depth=2.75, normal=(0, 0, 1), tolerance=0)
        self.protect(output)
        self.failure("--surface-reference", source, output, "--width", 1, "--height", 1)
        self.failure("--surface-reference", source, source, "--width", 1, "--height", 1)
        self.failure("--surface-reference", source, "missing-selector.exr",
                     "--grid", "descriptor:absent", "--width", 1, "--height", 1)
        _, second = self.render(source, "second.exr", "--grid", "descriptor:descriptor:plane")
        self.assertEqual(exr.channels, second.channels)
        self.fixture("empty.vdb", empty=True)
        report, empty = self.render("empty.vdb", "empty.exr", "--width", 2, "--height", 2,
                                    "--background", .25, .5, .75)
        self.assertEqual(report["results"]["hitCount"], 0)
        for i in range(4):
            self.assertEqual((empty["R"][i], empty["G"][i], empty["B"][i], empty["A"][i]),
                             (.25, .5, .75, 0))

    def test_shared_image_report_export_caps_and_source_failures_publish_nothing(self):
        self.enabled_only()
        self.fixture()
        for index, extra in enumerate((("--max-input-bytes", 1), ("--max-export-bytes", 1),
                                       ("--max-ray-work", 1), ("--max-voxels", 1),
                                       ("--max-ray-metadata", 1),
                                       ("--width", 2, "--max-rays", 1),
                                       ("--width", 2, "--max-pixels", 1),
                                       ("--eye", 0, 0, 3, "--target", 0, 0, 3),
                                       ("--up", 0, 0, 1),
                                       ("--color", "5e-324", 1, 1))):
            with self.subTest(extra=extra):
                args = ["--surface-reference", "plane.vdb", f"failed-{index}.exr", *extra]
                if "--width" not in extra:
                    args += ["--width", 1]
                args += ["--height", 1]
                self.failure(*args)
        self.failure("--surface-reference", "absent.vdb", "missing.exr", "--width", 1, "--height", 1)
        # Unequal limits verify each user budget stays authoritative after the
        # summary reservation, rather than silently replacing either one.
        report, _ = self.render("plane.vdb", "bounded.exr", "--max-input-bytes", 268435456,
                                "--max-export-bytes", 134217728, "--max-voxels", 2**64 - 1)
        self.assertEqual(report["budgets"]["requestedInputBytes"], 268435456)
        self.assertEqual(report["budgets"]["requestedExportBytes"], 134217728)
        self.assertEqual(report["budgets"]["maxVoxels"], 2**64 - 1)


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--tool", type=Path, required=True)
    parser.add_argument("--enabled", type=int, choices=(0, 1), required=True)
    arguments, remaining = parser.parse_known_args()
    SurfaceReferenceCli.tool = arguments.tool.resolve()
    SurfaceReferenceCli.enabled = bool(arguments.enabled)
    unittest.main(argv=[sys.argv[0], *remaining])
