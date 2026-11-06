"""Real particle geometry Tool protocol with independent stdlib stamp/bit oracles."""
import argparse
import itertools
import json
import math
from pathlib import Path
import struct
import subprocess
import tempfile
import unittest


def f32(value):
    return struct.unpack("<f", struct.pack("<f", value))[0]


class ParticleGeometryCli(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="container-particle-geometry-cli-")
        self.directory = Path(self.temp.name)
        self.originals, self.serial = {}, 0

    def tearDown(self):
        try:
            for path, original in self.originals.items():
                self.assertEqual(path.read_bytes(), original, path)
        finally:
            self.temp.cleanup()

    def call(self, *args, succeeds=True):
        result = subprocess.run([str(self.tool), *map(str, args)], cwd=self.directory,
            capture_output=True, encoding="utf-8", timeout=90)
        self.assertEqual(result.returncode == 0, succeeds,
                         f"{args!r}\n{result.stdout}\n{result.stderr}")
        if not succeeds:
            self.assertEqual(result.stdout, "", "failure printed partial publishing report")
        return result

    def enabled_only(self):
        if not self.enabled:
            self.skipTest("OpenVDB is disabled")

    def source(self, particles, name="particles.json"):
        path = self.directory / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(json.dumps({"version": 1, "particles": particles}), encoding="utf-8")
        self.originals[path] = path.read_bytes()
        return path

    def report(self, source, output="out.vdb", *flags):
        value = json.loads(self.call("--particle-geometry", source, output, *flags).stdout)
        self.assertEqual(value["schemaVersion"], 1)
        self.assertEqual(value["operation"], "particle-geometry")
        for key in ("particleCount", "sphereParticleCount", "trailParticleCount", "sphereInstances",
                    "instanceUpperBound", "candidateVoxelUpperBound", "storedLeafVoxelUpperBound",
                    "nodeUpperBound", "nativeWorkspaceUpperBound", "reportReservation", "workspaceUpperBound",
                    "nativeWorkUpperBound", "formattingWorkUpperBound", "workUpperBound",
                    "roundedDistanceValues", "centerCandidateCount", "centerRepairPairUpperBound",
                    "repairedNativeCenterValues", "nativeDiagnosticCount"):
            self.assertIs(type(value[key]), int, key)
            self.assertGreaterEqual(value[key], 0)
        self.assertEqual(value["workspaceUpperBound"], value["nativeWorkspaceUpperBound"] + value["reportReservation"])
        self.assertEqual(value["workUpperBound"], value["nativeWorkUpperBound"] + value["formattingWorkUpperBound"])
        self.assertLessEqual(value["workUpperBound"], value["limits"]["maxWork"])
        self.assertLessEqual(value["workspaceUpperBound"], value["limits"]["maxInputBytes"])
        self.assertIn("OPPOSITE", value["trailPolicy"])
        self.assertIn("strictly", value["maskPolicy"])
        self.assertIn("not generated", value["attributePolicy"])
        if value["mask"] is not None:
            self.assertEqual(value["mask"]["gridClass"], "unknown")
        self.assertIs(type(value["diagnosticsTruncated"]), bool)
        self.assertFalse(value["diagnosticsTruncated"])
        return value

    def dense(self, source, grid="surface", lower=(-8, -6, -2), dimensions=(16, 13, 5)):
        """Independent CVDBARR framing/IEEE decoder; no Python package dependency."""
        self.serial += 1
        destination = self.directory / f"dense-{self.serial}.cvarr"
        self.call("--dense-export", source, destination, "--input-grid", grid,
                  "--dense-origin", *lower, "--dense-dimensions", *dimensions)
        data = destination.read_bytes()
        prefix = struct.Struct("<8s6I3i3I9d3d")
        head = prefix.unpack_from(data)
        self.assertEqual(head[:3], (b"CVDBARR\0", 1, 0))  # native Float scalar
        self.assertEqual(tuple(head[7:10]), lower)
        self.assertEqual(tuple(head[10:13]), dimensions)
        offset = prefix.size
        background, = struct.unpack_from("<f", data, offset)
        offset += 4
        tail = struct.Struct("<IIQIIQQQ")
        name_bytes, metadata_count, outside, stored, reserved, count, payload, total = tail.unpack_from(data, offset)
        offset += tail.size
        self.assertEqual(reserved, 0)
        self.assertEqual(count, math.prod(dimensions))
        self.assertEqual(payload, count * 5)
        self.assertEqual(total, len(data))
        self.assertEqual(data[offset:offset + name_bytes].decode("utf-8"), grid)
        offset += name_bytes
        for _ in range(metadata_count):
            length, = struct.unpack_from("<I", data, offset)
            offset += 4 + length
            self.assertLessEqual(offset, len(data))
        self.assertEqual(offset + payload, len(data))
        values = struct.unpack_from(f"<{count}f", data, offset)
        active = data[offset + count * 4:]
        self.assertTrue(all(a in (0, 1) for a in active))
        return {"values": values, "active": active, "background": background,
                "linear": tuple(head[13:22]), "translation": tuple(head[22:25]),
                "lower": lower, "dimensions": dimensions, "outside": outside, "stored": stored}

    def query(self, source, grid, lower=(-8, -6, -2), upper=(7, 6, 2)):
        return json.loads(self.call("--query-active-regions", source, "--grid", grid,
            "--region", *lower, *upper, "--max-region-records", 10000,
            "--max-region-total-records", 10000, "--max-region-metadata", 65536).stdout)

    def active_values(self, report):
        query, = report["queries"]
        self.assertTrue(query["countsComplete"])
        self.assertFalse(query["recordsTruncated"])
        result = {}
        for record in query["records"]:
            native = record["value"]
            value = (struct.unpack(">f", bytes.fromhex(native["bits"]))[0]
                     if native["type"] == "float32" else native["value"])
            box = record["clipped"]
            axes = [range(lo, hi + 1) for lo, hi in zip(box["minimum"], box["maximum"])]
            for q in itertools.product(*axes):
                self.assertNotIn(q, result)
                result[q] = value
        self.assertEqual(len(result), query["matchedVoxelCount"])
        return result

    @staticmethod
    def cells(volume):
        low, (nx, ny, nz) = volume["lower"], volume["dimensions"]
        for z in range(nz):
            for y in range(ny):
                for x in range(nx):
                    index = x + nx * (y + ny * z)
                    yield (x + low[0], y + low[1], z + low[2]), volume["values"][index], volume["active"][index]

    def assert_union(self, volume, stamps, h=1, band=3):
        for q, value, active in self.cells(volume):
            distance = min(math.hypot(*(q[a] - center[a] for a in range(3))) - radius
                           for center, radius in stamps)
            expected = f32(max(-band, min(band, distance)) * h)
            self.assertAlmostEqual(value, expected, delta=max(1e-12, h * 5e-7), msg=str(q))
            self.assertEqual(active, abs(distance) < band, q)

    def test_help_and_strict_parser_modes_aliases_arity_and_unsigned_limits(self):
        help_text = self.call("--help").stdout
        for flag in ("--particle-geometry", "--particle-output", "--trail-time", "--trail-spacing",
                     "--particle-origin", "--min-radius-voxels", "--max-radius-voxels",
                     "--sdf-grid-name", "--mask-grid-name", "--max-trail-instances",
                     "--max-trail-length-voxels", "--max-particle-nodes", "--max-particle-work",
                     "--max-particle-metadata", "--max-output-bytes"):
            self.assertIn(flag, help_text)
        cases = [(("--particle-geometry",), "requires INPUT OUTPUT"),
                 (("--particle-geometry", "missing"), "requires INPUT OUTPUT"),
                 (("--particle-geometry", "", "out.vdb"), "requires nonempty"),
                 (("--particle-geometry", "-h", "out.vdb"), "requires nonempty"),
                 (("--particle-geometry", "missing", "--help"), "requires nonempty")]
        options = [(("--particle-output", "fog"), "requires sdf|mask|both"),
                   (("--particle-output", "mask", "--half-width", 3), "require sdf or both"),
                   (("--mask-grid-name", "mask"), "requires mask or both"),
                   (("--particle-output", "mask", "--grid-name", "sdf"), "require sdf or both"),
                   (("--grid-name", "a", "--sdf-grid-name", "b"), "duplicate"),
                   (("--trail-time", 1, "--time-step", 2), "duplicate"),
                   (("--particle-origin", 0, 0), "missing"),
                   (("--trail-time", "nan"), "finite Double"),
                   (("--trail-time", " 1"), "finite Double"),
                   (("--trail-spacing", 1, "--trail-spacing", 2), "duplicate"),
                   (("--max-particles", 0), "count exceeds range"),
                   (("--max-particles", 4294967295), "count exceeds range"),
                   (("--max-trail-instances", 16000001), "count exceeds range"),
                   (("--max-particle-work", 10000000001), "count exceeds range"),
                   (("--max-particle-nodes", 1000001), "count exceeds range"),
                   (("--max-particle-metadata", 16777217), "count exceeds range"),
                   (("--max-input-bytes", 17179869185), "count exceeds range"),
                   (("--max-voxels", 18446744073709551616), "count exceeds range"),
                   (("--max-voxels", " -1"), "unsigned decimal"),
                   (("--max-output-bytes", "+1"), "unsigned decimal"),
                   (("--sdf-grid-name", "--help"), "missing nonempty"),
                   (("--input-grid", "shape"), "unknown/incompatible"),
                   (("--max-triangles", 100), "unknown/incompatible"),
                   (("--radius", 2), "unknown/incompatible"),
                   (("--density", 1), "unknown/incompatible")]
        cases += [(("--particle-geometry", "missing", "out.vdb", *flags), expected)
                  for flags, expected in options]
        for args, expected in cases:
            with self.subTest(args=args):
                result = self.call(*args, succeeds=False)
                self.assertIn(expected, result.stderr)
                self.assertFalse((self.directory / "out.vdb").exists())

    def test_disabled_availability_before_paths_and_report_reservation(self):
        if self.enabled:
            self.skipTest("disabled-build availability only")
        result = self.call("--particle-geometry", "missing.json", "missing-parent/out.vdb",
                           "--max-input-bytes", 1, "--max-particle-work", 1, succeeds=False)
        self.assertIn("ENABLE_OPENVDB=ON", result.stderr)

    def test_real_sphere_union_center_repairs_mask_boundary_and_sdf_activity(self):
        self.enabled_only()
        source = self.source([{"position": [0, 0, 0], "radius": 2},
                              {"position": [2, 0, 0], "radius": 2}])
        result = self.report(source, "both.vdb", "--particle-output", "both", "--voxel-size", 1,
                             "--particle-origin", 0, 0, 0)
        self.assertEqual(result["sphereInstances"], 2)
        self.assertEqual(result["repairedNativeCenterValues"], 2)
        self.assertEqual(result["centerCandidateCount"], 2)
        self.assertEqual(result["centerRepairPairUpperBound"], 4)
        self.assert_union(self.dense("both.vdb"), [((0, 0, 0), 2), ((2, 0, 0), 2)])
        report = self.query("both.vdb", "particle_mask")
        self.assertEqual(report["nativeType"], "mask")
        active = self.active_values(report)
        for q in itertools.product(range(-8, 8), range(-6, 7), range(-2, 3)):
            inside = min(math.dist(q, c) for c in ((0, 0, 0), (2, 0, 0))) < 2
            self.assertEqual(q in active, inside, q)
        self.assertNotIn((-2, 0, 0), active)
        self.assertNotIn((-3, 0, 0), active)  # present in SDF band, absent from interior mask

    def test_real_taper_uses_three_exact_stamps_and_does_not_add_endpoint_capsule(self):
        self.enabled_only()
        source = self.source([{"position": [0, 0, 0], "radius": 4, "velocity": [4, 0, 0]}])
        result = self.report(source, "trail.vdb", "--particle-output", "both", "--voxel-size", 1,
                             "--particle-origin", 0, 0, 0, "--min-radius-voxels", 2)
        self.assertEqual(result["sphereInstances"], 3)
        self.assertEqual(result["trailParticleCount"], 1)
        self.assertEqual(result["maximumTrailLengthWorld"], 4)
        stamps = [((0, 0, 0), 4), ((-2, 0, 0), 3), ((-3.5, 0, 0), 2.25)]
        volume = self.dense("trail.vdb")
        self.assert_union(volume, stamps)
        sdf = self.active_values(self.query("trail.vdb", "surface"))
        self.assertEqual(sdf[-6, 0, 0], .25)
        active = self.active_values(self.query("trail.vdb", "particle_mask"))
        self.assertNotIn((-6, 0, 0), active)

    def test_signed_time_zero_speed_and_native_values_are_deterministic(self):
        self.enabled_only()
        a = self.source([{"position": [0, 0, 0], "radius": 4, "velocity": [4, 0, 0]}], "a.json")
        b = self.source([{"position": [0, 0, 0], "radius": 4, "velocity": [-4, 0, 0]}], "b.json")
        common = ("--voxel-size", 1, "--particle-origin", 0, 0, 0, "--min-radius-voxels", 2)
        self.report(a, "a.vdb", *common, "--trail-time", -1)
        self.report(b, "b.vdb", *common, "--time-step", 1)
        self.assertEqual(self.dense("a.vdb")["values"], self.dense("b.vdb")["values"])
        result = self.report(a, "zero.vdb", *common, "--trail-time", 0)
        self.assertEqual(result["trailParticleCount"], 0)
        self.assertEqual(result["sphereInstances"], 1)
        self.assert_union(self.dense("zero.vdb"), [((0, 0, 0), 4)])

    def test_global_center_repair_and_squared_underflow_deduplication(self):
        self.enabled_only()
        source = self.source([{"position": [0, 0, 0], "radius": 2},
                              {"position": [1, 0, 0], "radius": 3.25}])
        report = self.report(source, "overlap.vdb", "--voxel-size", 1, "--particle-origin", 0, 0, 0)
        self.assertEqual(report["repairedNativeCenterValues"], 1)
        values = self.active_values(self.query("overlap.vdb", "surface"))
        self.assertEqual(values[0, 0, 0], -2.25)
        self.assert_union(self.dense("overlap.vdb"), [((0, 0, 0), 2), ((1, 0, 0), 3.25)])
        source = self.source([{"position": [1e-200, 0, 0], "radius": 2},
                              {"position": [-1e-200, 0, 0], "radius": 2}], "tiny-offset.json")
        report = self.report(source, "tiny-offset.vdb", "--voxel-size", 1, "--particle-origin", 0, 0, 0)
        self.assertEqual(report["centerCandidateCount"], 1)
        self.assertEqual(report["centerRepairPairUpperBound"], 2)
        self.assertEqual(self.active_values(self.query("tiny-offset.vdb", "surface"))[0, 0, 0], -2)

    def test_exact_tiny_large_maps_world_radii_rounding_and_origin_phase(self):
        self.enabled_only()
        for index, h in enumerate((1e-6, .07, .125, 1e6)):
            origin = [123.125, -22.25, 44.5] if h < 1e6 else [1e12 + .125, -2e12, 3e12]
            source = self.source([{"position": origin, "radius": 2 * h}], f"map-{index}.json")
            result = self.report(source, f"map-{index}.vdb", "--voxel-size", h,
                                 "--particle-origin", *origin)
            query = self.query(f"map-{index}.vdb", "surface", (-1, -1, -1), (1, 1, 1))
            self.assertEqual(result["originWorld"], origin)
            expected = [h, 0, 0, origin[0], 0, h, 0, origin[1], 0, 0, h, origin[2], 0, 0, 0, 1]
            self.assertEqual(query["indexToWorld"], expected)
            self.assertEqual(self.active_values(query)[1, 0, 0], f32(-h))
            self.assertLessEqual(result["maximumPositionErrorVoxels"], 1e-4)
            if h == .07:
                self.assertGreater(result["roundedDistanceValues"], 0)
        center = [1e6 + .125, 2e6 + .25, -3e6 - .5]
        source = self.source([{"position": center, "radius": 2}], "auto.json")
        auto = self.report(source, "auto.vdb", "--voxel-size", 1)
        self.assertEqual(auto["originWorld"], center)
        phase = [center[0] + .25, *center[1:]]
        self.report(source, "phase.vdb", "--voxel-size", 1, "--particle-origin", *phase)
        self.assertEqual(self.active_values(self.query("phase.vdb", "surface"))[0, 0, 0], -1.75)

    def test_empty_and_mask_only_modes_and_exact_uint64_forwarding(self):
        self.enabled_only()
        empty = self.source([])
        value = self.report(empty, "empty.vdb", "--particle-output", "both")
        self.assertEqual(value["particleCount"], 0)
        self.assertEqual(value["levelSet"]["activeVoxelCount"], 0)
        self.assertEqual(value["mask"]["activeVoxelCount"], 0)
        source = self.source([{"position": [0, 0, 0], "radius": 2}], "one.json")
        value = self.report(source, "mask.vdb", "--particle-output", "mask", "--voxel-size", 1,
                            "--max-voxels", 18446744073709551615)
        self.assertIsNone(value["levelSet"])
        self.assertIsNone(value["controls"]["halfWidth"])
        self.assertEqual(value["limits"]["maxVoxelCount"], 18446744073709551615)
        self.assertGreater(value["mask"]["activeVoxelCount"], 0)
        self.assertEqual(value["effectiveExteriorBand"], 0)

    def test_strict_json_schema_tokens_utf8_integer_precision_and_underflow(self):
        self.enabled_only()
        invalid = [b"[]", b"{}", b'{"version":1.0,"particles":[]}',
            b'{"version":1,"particles":[],"particles":[]}',
            b'{"version":1,"particles":[{"position":[0,0],"radius":2}]}',
            b'{"version":1,"particles":[{"position":[0,0,0,0],"radius":2}]}',
            b'{"version":1,"particles":[{"position":[0,0,0],"radius":2,"radius":3}]}',
            b'{"version":1,"particles":[{"position":[9007199254740993,0,0],"radius":2}]}',
            b'{"version":1,"particles":[{"position":[0,0,0],"radius":2,"velocity":[18446744073709551617,0,0]}]}',
            b'{"version":1,"particles":[{"position":[0,0,0],"radius":2,"velocity":[1e-999,0,0]}]}',
            b'{"version":1,"particles":[{"position":[0,0,0],"radius":2,"velocity":[0,false,0]}]}',
            b'{"version":1,"particles":[{"position":[0,0,0],"radius":2,"id":0}]}',
            b'{"version":1,"particles":[],"\xff":0}', b'{"' + b"x" * 129 + b'":0}']
        for index, data in enumerate(invalid):
            source = self.directory / f"bad-{index}.json"
            source.write_bytes(data)
            self.originals[source] = data
            self.call("--particle-geometry", source, "bad.vdb", "--voxel-size", 1, succeeds=False)
            self.assertFalse((self.directory / "bad.vdb").exists())

    def test_native_shared_limits_radius_precision_and_displacement_guards(self):
        self.enabled_only()
        source = self.source([{"position": [0, 0, 0], "radius": 4, "velocity": [4, 0, 0]},
                              {"position": [0, 4, 0], "radius": 2}])
        flags = [("--max-particles", 1), ("--max-trail-instances", 2),
                 ("--max-trail-length-voxels", 1), ("--max-particle-nodes", 1),
                 ("--max-particle-work", 1), ("--max-particle-metadata", 1),
                 ("--max-input-bytes", 1024), ("--max-output-bytes", 1), ("--max-voxels", 512),
                 ("--min-radius-voxels", 3), ("--max-radius-voxels", 3),
                 ("--trail-spacing", .249), ("--trail-spacing", 4.001),
                 ("--half-width", 2), ("--particle-origin", 1e15, 0, 0)]
        for options in flags:
            with self.subTest(options=options):
                self.call("--particle-geometry", source, "bad.vdb", "--voxel-size", 1, *options, succeeds=False)
                self.assertFalse((self.directory / "bad.vdb").exists())
        for index, (position, velocity, time) in enumerate((
            ([1e6, 0, 0], [1e-20, 0, 0], 1), ([0, 0, 0], [2, math.ulp(0.), 0], 1),
            ([0, 0, 0], [math.ulp(0.), 0, 0], .25), ([0, 0, 0], [1e308, 0, 0], 2))):
            source = self.source([{"position": position, "radius": 2, "velocity": velocity}], f"precision-{index}.json")
            self.call("--particle-geometry", source, "bad.vdb", "--voxel-size", 1,
                      "--particle-origin", 0, 0, 0, "--trail-time", time, succeeds=False)
            self.assertFalse((self.directory / "bad.vdb").exists())

    def test_unicode_names_parents_source_literal_h_and_exclusive_destinations(self):
        self.enabled_only()
        source = self.source([{"position": [0, 0, 0], "radius": 2}], "入力-é/粒子.json")
        parent = self.directory / "出力-é"
        parent.mkdir()
        output = parent / "軌跡.vdb"
        result = self.report(source, output, "--particle-output", "both", "--voxel-size", 1,
                             "--sdf-grid-name", "距離", "--mask-grid-name", "内部")
        self.assertEqual(result["levelSet"]["gridName"], "距離")
        self.assertEqual(result["mask"]["gridName"], "内部")
        self.assertEqual(self.query(output, "距離")["gridName"]["text"], "距離")
        completed = output.read_bytes()
        self.call("--particle-geometry", source, output, succeeds=False)
        self.assertEqual(output.read_bytes(), completed)
        self.call("--particle-geometry", source, source, succeeds=False)
        foreign = parent / "既存.vdb"
        foreign.write_bytes(b"foreign bytes\0\xff")
        self.originals[foreign] = foreign.read_bytes()
        self.call("--particle-geometry", source, foreign, succeeds=False)
        literal = self.directory / "-h"
        literal.write_bytes(source.read_bytes())
        self.originals[literal] = literal.read_bytes()
        self.report("./-h", "literal.vdb", "--voxel-size", 1)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--tool", required=True, type=Path)
    parser.add_argument("--enabled", required=True, choices=("0", "1"))
    args, remaining = parser.parse_known_args()
    ParticleGeometryCli.tool = args.tool.resolve(strict=True)
    ParticleGeometryCli.enabled = args.enabled == "1"
    unittest.main(argv=[__file__, *remaining])
