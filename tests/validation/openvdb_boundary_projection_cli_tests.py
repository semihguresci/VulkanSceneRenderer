"""Real-tool additive MAC boundaries, physical channels and canonical replay.

Analytic channels use independently specified pressure slopes and face values.
Native tests additionally cover irregular 3D full equations, exact Bool/Mask
records, Float codecs, full-memory loading and malformed packed tuple channels.
"""
import argparse
from array import array
import json
import math
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "python"))
from container_openvdb.dense import (DenseType, DenseVolume, GridClass, VectorType,
                                    read_dense_array, write_dense_array)


class BoundaryProjectionCli(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="container-boundary-cli-")
        self.directory = Path(self.temporary.name)
        self.originals = {}
        self.serial = 0

    def tearDown(self):
        try:
            for source, original in self.originals.items():
                self.assertEqual(source.read_bytes(), original, source)
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

    def dense(self, name, function, *, dtype=DenseType.FLOAT64, grid="field",
              h=1., origin=(-3, -2, -2), dimensions=(10, 5, 5), background=None,
              active=None, linear=None, translation=(0., 0., 0.),
              world=True, tag=VectorType.INVARIANT, grid_class=GridClass.UNKNOWN):
        values, activity = array(dtype.typecode), bytearray()
        for z in range(origin[2], origin[2] + dimensions[2]):
            for y in range(origin[1], origin[1] + dimensions[1]):
                for x in range(origin[0], origin[0] + dimensions[0]):
                    value = function(x, y, z)
                    values.extend((value,) if dtype.components == 1 else value)
                    activity.append(1 if active is None else bool(active(x, y, z)))
        volume = DenseVolume(dtype, dimensions, values, activity, origin=origin,
            linear=linear or (h, 0., 0., 0., h, 0., 0., 0., h), translation=translation,
            background=array(dtype.typecode, background or [0] * dtype.components),
            grid_name=grid, grid_class=grid_class, vector_type=tag, world_space=world)
        encoded = self.directory / (name + ".cvarr")
        write_dense_array(encoded, volume)
        self.call("--dense-import", encoded, name)
        self.protect(name)

    def sample(self, name, grid, origin=(0, 0, 0), dimensions=(1, 1, 1)):
        self.serial += 1
        encoded = self.directory / f"readback-{self.serial}.cvarr"
        self.call("--dense-export", name, encoded, "--input-grid", grid,
                  "--dense-origin", *origin, "--dense-dimensions", *dimensions)
        return read_dense_array(encoded)

    def channel(self, *, n=3, h=1., wall=1., air=False, predictor=(0., 0., 0.), prefix="c"):
        fluid, exterior, velocity = (prefix + suffix + ".vdb" for suffix in ("-fluid", "-exterior", "-velocity"))
        inside = lambda x, y, z: 0 <= x < n and y == 0 and z == 0
        outlet = lambda x, y, z: x == n and y == 0 and z == 0
        self.dense(fluid, lambda x, y, z: float(inside(x, y, z)), h=h, grid="fluid", active=inside)
        self.dense(exterior, lambda x, y, z: float(outlet(x, y, z)), h=h, grid="outlet", active=outlet)
        self.dense(velocity, lambda x, y, z: predictor, dtype=DenseType.VEC3_FLOAT64,
                   grid="predictor", background=predictor)
        return ["--project-boundaries", velocity, fluid], [
            "--velocity-grid", "predictor", "--fluid-grid", "fluid",
            "--boundary-air" if air else "--boundary-open", exterior,
            "--boundary-air-grid" if air else "--boundary-open-grid", "outlet",
            "--wall-velocity", wall, 0, 0, "--time-step", 1, "--tolerance", 1e-10]

    def expect_channel(self, output, pressure, velocity, *, velocity_grid="velocity",
                       pressure_grid="pressure", relative=False, scale=1.):
        n = len(pressure)
        p = self.sample(output, pressure_grid, dimensions=(n, 1, 1))
        u = self.sample(output, velocity_grid, dimensions=(n + 1, 2, 2))
        self.assertEqual(p.dtype, DenseType.FLOAT64)
        self.assertEqual(u.dtype, DenseType.VEC3_FLOAT64)
        self.assertEqual(u.grid_class, GridClass.STAGGERED)
        self.assertEqual(u.vector_type, VectorType.CONTRAVARIANT_RELATIVE)
        self.assertFalse(u.world_space)
        delta = abs(scale) * 3e-8 if relative else 3e-8
        for x in range(n + 1):
            self.assertAlmostEqual(u.value_at((x, 0, 0))[0], velocity, delta=delta)
        for x, expected in enumerate(pressure):
            self.assertAlmostEqual(p.value_at((x, 0, 0)), expected, delta=delta)
            cell = (x, 0, 0)
            div = sum(u.value_at(tuple(cell[b] + int(a == b) for b in range(3)))[a]
                      - u.value_at(cell)[a]
                      for a in range(3))
            self.assertAlmostEqual(div, 0., delta=delta)

    def assert_report(self, report, replay=False):
        self.assertEqual(report["schemaVersion"], 1)
        self.assertEqual(report["operation"], "reproject-boundaries" if replay else "project-boundaries")
        self.assertIs(report["replayed"], replay)
        self.assertIs(type(report["boundaryStateWritten"]), bool)
        p = report["projection"]
        self.assertTrue(p["converged"])
        for name in ("fluidCellCount", "faceCount", "componentCount", "iterations"):
            self.assertIs(type(p[name]), int)
            self.assertGreaterEqual(p[name], 0)
        for name in ("maxDivergenceBefore", "maxDivergenceAfter", "residual", "divergenceRoundoffBound"):
            self.assertTrue(math.isfinite(p[name]))
            self.assertGreaterEqual(p[name], 0)
        for name in ("fluidFluidFaces", "wallFaces", "openFacePressureFaces", "airCellPressureFaces",
                     "freeSurfacePressureFaces", "clampedInterfaceFaces", "dirichletComponents",
                     "neumannComponents", "totalIterations", "workUpperBound", "roundoffLimitedCells"):
            self.assertIs(type(report[name]), int)
            self.assertGreaterEqual(report[name], 0)
        self.assertIn("raw physical XYZ", report["velocityPolicy"])
        self.assertIn("fresh impulse", report["replayPolicy"])
        self.assertLessEqual(len(report["diagnostics"]), 8)
        self.assertTrue(all(type(x) is str for x in report["diagnostics"]))

    def test_help_strict_arity_duplicates_ranges_and_irrelevant_controls(self):
        text = self.call("--help").stdout
        for flag in ("--project-boundaries", "--reproject-boundaries", "--boundary-open",
                     "--boundary-air", "--boundary-phi", "--boundary-wall", "--boundary-pressure",
                     "--minimum-interface-fraction", "--max-boundary-work"):
            self.assertIn(flag, text)
        bad = [("--project-boundaries",), ("--project-boundaries", "a", "b"),
               ("--project-boundaries", "-h", "b", "bad.vdb"),
               ("--project-boundaries", "a", "--fluid-grid", "bad.vdb"),
               ("--reproject-boundaries", "a"), ("--reproject-boundaries", "a", "-h")]
        flags = [("--grid", "a", "--velocity-grid", "b"),
                 ("--input-grid", "a", "--grid", "b"), ("--fluid-grid", ""),
                 ("--fluid-grid", "-h"), ("--iterations", 0), ("--iterations", 10001),
                 ("--max-fluid-cells", 1000001), ("--time-step", -1), ("--time-step", "nan"),
                 ("--tolerance", 1e-15), ("--acceleration", 1, 2),
                 ("--wall-velocity", 1e13, 0, 0), ("--missing-boundary", "periodic"),
                 ("--air-pressure", "center"), ("--boundary-air", "a", "--air-pressure", "x"),
                 ("--boundary-phi", "a"), ("--minimum-interface-fraction", .01),
                 ("--missing-boundary", "surface"), ("--boundary-solid-grid", "x"),
                 ("--boundary-wall", "a", "--wall-velocity", 0, 0, 0),
                 ("--boundary-pressure", "a", "--ambient-pressure", 0),
                 ("--max-boundary-work", 10000000001), ("--max-boundary-grids", 4097),
                 ("--max-boundary-metadata", 0), ("--max-input-bytes", 0),
                 ("--max-voxels", "+2"), ("--no-domain",),
                 ("--no-boundary-state", "--face-kind-name", "unused"),
                 ("--no-boundary-state", "--no-domain", "--fluid-name", "unused"),
                 ("--velocity-name", "same", "--pressure-name", "same"),
                 ("--velocity-name", "x" * 257), ("--grid-name", "unused"),
                 ("--max-triangles", 100), ("--voxel-size", .1)]
        bad.extend(("--project-boundaries", "a", "b", "bad.vdb", *f) for f in flags)
        bad.extend(("--reproject-boundaries", "state", "bad.vdb", *f) for f in (
            ("--boundary-open", "a"), ("--ambient-pressure", 0), ("--missing-boundary", "wall"),
            ("--wall-velocity", 0, 0, 0), ("--minimum-interface-fraction", .01)))
        for args in bad:
            with self.subTest(args=args):
                self.call(*args, succeeds=False)
                self.assertFalse((self.directory / "bad.vdb").exists())
        self.assertFalse(list(self.directory.iterdir()))

    def test_disabled_availability_precedes_paths_and_tiny_positive_report_caps(self):
        if self.enabled:
            self.skipTest("OpenVDB is enabled")
        for args in (("--project-boundaries", "不存在", "missing", "missing-parent/out.vdb"),
                     ("--reproject-boundaries", "missing", "missing-parent/out.vdb")):
            for cap in (1, 1024):
                result = self.call(*args, "--max-input-bytes", cap, succeeds=False)
                self.assertIn("ENABLE_OPENVDB=ON", result.stderr)
        self.assertFalse(list(self.directory.iterdir()))

    def test_physical_face_air_center_pressure_distances_and_world_units(self):
        self.enabled_only()
        for air in (False, True):
            head, flags = self.channel(air=air, prefix=str(air))
            output = str(air) + ".vdb"
            report = self.report(*head, output, *flags)
            self.assert_report(report)
            self.assertEqual(report["dirichletComponents"], 1)
            self.assertEqual(report["neumannComponents"], 0)
            self.assertEqual(report["airCellPressureFaces"], int(air))
            self.assertEqual(report["openFacePressureFaces"], int(not air))
            self.expect_channel(output, [3, 2, 1] if air else [2.5, 1.5, .5], 1.)
        head, flags = self.channel(n=2, h=2, wall=2, air=True, prefix="units")
        flags[flags.index("--time-step") + 1] = .5
        self.report(*head, "units.vdb", *flags, "--ambient-pressure", 7)
        self.expect_channel("units.vdb", [23, 15], 1.)

    def test_nonuniform_pressure_drives_zero_divergence_and_replay_adds_another_impulse(self):
        self.enabled_only()
        head, flags = self.channel(h=2, wall=0)
        self.dense("both.vdb", lambda x, y, z: float(x in (-1, 3) and y == 0 and z == 0),
                   h=2, grid="both", active=lambda x, y, z: x in (-1, 3) and y == 0 and z == 0)
        self.dense("pressure.vdb", lambda x, y, z: 1.5 - x, h=2, grid="pD", dtype=DenseType.FLOAT32)
        flags[flags.index("--boundary-open") + 1] = "both.vdb"
        flags[flags.index("--boundary-open-grid") + 1] = "both"
        flags[flags.index("--time-step") + 1] = .25
        report = self.report(*head, "direct.vdb", *flags,
                             "--boundary-pressure", "pressure.vdb", "--boundary-pressure-grid", "pD")
        self.assert_report(report)
        self.assertEqual(report["projection"]["maxDivergenceBefore"], 0)
        self.expect_channel("direct.vdb", [1.5, .5, -.5], .0625)
        self.protect("direct.vdb")
        repeated = self.report("--reproject-boundaries", "direct.vdb", "twice.vdb",
                               "--time-step", .25, "--tolerance", 1e-10)
        self.assert_report(repeated, True)
        self.expect_channel("twice.vdb", [1.5, .5, -.5], .125)

    def test_explicit_solid_open_and_air_roles_share_one_finite_volume_equation(self):
        self.enabled_only()
        self.dense("fluid.vdb", lambda x, y, z: float((x, y, z) == (0, 0, 0)), grid="fluid",
                   active=lambda x, y, z: (x, y, z) == (0, 0, 0))
        self.dense("velocity.vdb", lambda x, y, z: (0., 0., 0.), dtype=DenseType.VEC3_FLOAT64, grid="u")
        for name, coordinate in (("solid", (-1, 0, 0)), ("open", (1, 0, 0)), ("air", (0, 1, 0))):
            self.dense(name + ".vdb", lambda x, y, z, c=coordinate: float((x, y, z) == c),
                       grid=name, active=lambda x, y, z, c=coordinate: (x, y, z) == c)
        self.dense("wall.vdb", lambda x, y, z: (.5-x, 0., 0.), dtype=DenseType.VEC3_FLOAT64, grid="wall")
        report = self.report("--project-boundaries", "velocity.vdb", "fluid.vdb", "mixed.vdb",
            "--velocity-grid", "u", "--fluid-grid", "fluid", "--time-step", 1, "--tolerance", 1e-10,
            "--boundary-solid", "solid.vdb", "--boundary-solid-grid", "solid",
            "--boundary-open", "open.vdb", "--boundary-open-grid", "open",
            "--boundary-air", "air.vdb", "--boundary-air-grid", "air",
            "--boundary-wall", "wall.vdb", "--boundary-wall-grid", "wall")
        self.assert_report(report)
        self.assertEqual(report["wallFaces"], 4)
        self.assertEqual(report["openFacePressureFaces"], 1)
        self.assertEqual(report["airCellPressureFaces"], 1)
        # One wall flux -1, physical-face weight2 and air-center weight1:
        # p=1/(2+1), Uright=2/3, Utop=1/3, all other wall normals zero.
        p = self.sample("mixed.vdb", "pressure")
        u = self.sample("mixed.vdb", "velocity", dimensions=(2, 2, 2))
        self.assertAlmostEqual(p.value_at((0, 0, 0)), 1/3, delta=1e-10)
        self.assertEqual(u.value_at((0, 0, 0))[0], 1)
        self.assertAlmostEqual(u.value_at((1, 0, 0))[0], 2/3, delta=1e-10)
        self.assertAlmostEqual(u.value_at((0, 1, 0))[1], 1/3, delta=1e-10)
        self.assertAlmostEqual(sum(u.value_at(tuple(int(a == b) for b in range(3)))[a]
            - u.value_at((0, 0, 0))[a] for a in range(3)), 0., delta=1e-10)
        self.call("--project-boundaries", "velocity.vdb", "fluid.vdb", "overlap.vdb",
            "--boundary-solid", "open.vdb", "--boundary-open", "open.vdb", succeeds=False)
        self.assertFalse((self.directory / "overlap.vdb").exists())

    def test_free_surface_hydrostatics_and_reported_effective_clamp(self):
        self.enabled_only()
        head, flags = self.channel(h=.5, wall=0, air=True)
        flags[flags.index("--time-step") + 1] = .125
        for clamp in (False, True):
            theta = 1/1024 if clamp else .75
            phi = f"phi-{clamp}.vdb"
            self.dense(phi, lambda x, y, z: .5 * (x - (2 + theta)), h=.5, grid="phi")
            output = f"hydro-{clamp}.vdb"
            report = self.report(*head, output, *flags, "--air-pressure", "surface",
                "--boundary-phi", phi, "--boundary-phi-grid", "phi",
                "--minimum-interface-fraction", .125 if clamp else .01,
                "--acceleration", -3, 0, 0)
            self.assert_report(report)
            self.assertEqual(report["clampedInterfaceFaces"], int(clamp))
            self.assertEqual(report["minimumEffectiveFraction"], .125 if clamp else .75)
            self.assertEqual(report["maximumFractionClamp"], .125 - 1/1024 if clamp else 0)
            self.expect_channel(output, [3.1875, 1.6875, .1875] if clamp else [4.125, 2.625, 1.125], 0)

    def test_closed_moving_walls_uniform_translation_and_incompatible_net_flux(self):
        self.enabled_only()
        head, flags = self.channel(wall=1)
        # Omit the exterior role entirely: all boundaries are moving walls.
        flags = flags[:4] + flags[8:]
        report = self.report(*head, "closed.vdb", *flags)
        self.assert_report(report)
        self.assertEqual(report["neumannComponents"], 1)
        self.assertEqual(report["maxClosedNormalFlux"], 0)
        self.assertEqual(report["maxWallNormalError"], 0)
        self.expect_channel("closed.vdb", [0, -1, -2], 1)
        self.report("--reproject-boundaries", "closed.vdb", "closed-again.vdb", "--time-step", 1)
        self.expect_channel("closed-again.vdb", [0, 0, 0], 1)
        self.dense("wall.vdb", lambda x, y, z: (2*x, 0, 0), dtype=DenseType.VEC3_FLOAT64, grid="wall")
        self.call(*head, "incompatible.vdb", *flags[:4], "--boundary-wall", "wall.vdb",
                  "--boundary-wall-grid", "wall", "--time-step", 1, succeeds=False)
        self.assertFalse((self.directory / "incompatible.vdb").exists())

    def test_reflected_world_and_native_local_mac_wall_maps_have_same_translation(self):
        self.enabled_only()
        linear = (0., -.5, 0., -.5, 0., 0., 0., 0., .5)
        translation = (2., -.5, .25)
        inside = lambda x, y, z: 0 <= x < 3 and y == 0 and z == 0
        self.dense("fluid.vdb", lambda x, y, z: float(inside(x, y, z)), grid="fluid",
                   linear=linear, translation=translation, active=inside)
        for local in (False, True):
            value = (2., 0., 0.) if local else (0., -1., 0.)
            name = f"velocity-{local}.vdb"
            self.dense(name, lambda x, y, z: value, dtype=DenseType.VEC3_FLOAT64,
                       grid="u", background=value, linear=linear, translation=translation,
                       world=not local, tag=VectorType.CONTRAVARIANT_RELATIVE,
                       grid_class=GridClass.STAGGERED if local else GridClass.UNKNOWN)
            output = f"frame-{local}.vdb"
            report = self.report("--project-boundaries", name, "fluid.vdb", output,
                "--velocity-grid", "u", "--fluid-grid", "fluid", "--time-step", 1,
                "--boundary-wall", name, "--boundary-wall-grid", "u")
            self.assert_report(report)
            self.assertEqual(report["totalIterations"], 0)
            self.expect_channel(output, [0, 0, 0], 2)
            stored = self.sample(output, "velocity")
            self.assertEqual(stored.linear, linear)
            self.assertEqual(stored.translation, translation)

    def test_unicode_names_custom_replay_selectors_reduced_outputs_and_exclusive_paths(self):
        self.enabled_only()
        head, flags = self.channel(prefix="入力")
        (self.directory / "圧力-é").mkdir()
        output = "圧力-é/流体.vdb"
        names = ("速度", "圧力", "流体", "種類", "割合", "境界圧", "壁")
        name_flags = [item for pair in zip(("--velocity-name", "--pressure-name", "--fluid-name",
                      "--face-kind-name", "--face-fraction-name", "--face-pressure-name", "--wall-velocity-name"), names) for item in pair]
        report = self.report(*head, output, *flags, *name_flags)
        self.assert_report(report)
        self.expect_channel(output, [2.5, 1.5, .5], 1, velocity_grid=names[0], pressure_grid=names[1])
        self.protect(output)
        self.call(*head, output, *flags, succeeds=False)
        self.call("--reproject-boundaries", output, "missing-selectors.vdb", succeeds=False)
        replayed = self.report("--reproject-boundaries", output, "replayed.vdb", "--time-step", 1,
            "--velocity-grid", names[0], "--fluid-grid", names[2], "--face-kind-grid", names[3],
            "--face-fraction-grid", names[4], "--face-pressure-grid", names[5], "--wall-velocity-grid", names[6])
        self.assert_report(replayed, True)
        self.expect_channel("replayed.vdb", [0, 0, 0], 1)
        self.report(*head, "two.vdb", *flags, "--no-boundary-state", "--no-domain")
        inspected = self.report("--inspect", "two.vdb")
        self.assertEqual({g["name"] for g in inspected["grids"]}, {"velocity", "pressure"})
        (self.directory / "-h").write_bytes((self.directory / head[1]).read_bytes())
        literal = list(head)
        literal[1] = "./-h"
        self.report(*literal, "literal.vdb", *flags)
        self.call(*head, head[1], *flags, succeeds=False)

    def test_subnormal_float_wall_promotion_unresolvable_pressure_and_shared_caps(self):
        self.enabled_only()
        head, flags = self.channel(n=1, wall=0)
        tiny = math.ldexp(1., -149)
        self.dense("tiny-wall.vdb", lambda x, y, z: (tiny, 0., 0.) if (x, y, z) == (0, 0, 0)
                   else (0., 0., 0.), dtype=DenseType.VEC3_FLOAT32, grid="wall")
        # Replace the constant wall flag with the independent native Float field.
        wall = flags.index("--wall-velocity")
        field_flags = flags[:wall] + flags[wall+4:]
        report = self.report(*head, "tiny.vdb", *field_flags,
            "--boundary-wall", "tiny-wall.vdb", "--boundary-wall-grid", "wall")
        self.assert_report(report)
        self.expect_channel("tiny.vdb", [tiny*.25], tiny*.5, relative=True, scale=tiny)
        self.assertEqual(self.sample("tiny.vdb", "velocity").value_at((0, 0, 0))[0], tiny*.5)
        # A physical pressure perturbation cannot be hidden beneath huge pD.
        head2, flags2 = self.channel(prefix="large")
        self.call(*head2, "lost-gradient.vdb", *flags2, "--ambient-pressure", 1e30, succeeds=False)
        self.assertFalse((self.directory / "lost-gradient.vdb").exists())
        for limits in (("--max-boundary-work", 512), ("--max-boundary-metadata", 1),
                       ("--max-input-bytes", 1024), ("--max-voxels", 2048),
                       ("--max-fluid-cells", 1)):
            with self.subTest(limits=limits):
                self.call(*head2, "bounded.vdb", *flags2, *limits, succeeds=False)
                self.assertFalse((self.directory / "bounded.vdb").exists())


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--tool", required=True, type=Path)
    parser.add_argument("--enabled", required=True, choices=("0", "1"))
    args, remaining = parser.parse_known_args()
    BoundaryProjectionCli.tool = args.tool.resolve(strict=True)
    BoundaryProjectionCli.enabled = args.enabled == "1"
    unittest.main(argv=[__file__, *remaining])
