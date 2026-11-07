"""Actual-tool fixed-boundary fluid steps and independent finite-volume oracles.

Fixtures are made by dense-import and the real canonical boundary producer.
Output framing/IEEE readback is decoded independently with struct. Scalar ring
and channel fluxes, MAC upwind decay, pressure impulses and WORLD units have
analytic expected values; restart equivalence is an additional check.
"""
import argparse
from array import array
import json
import math
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "python"))
from container_openvdb.dense import (DenseType, DenseVolume, GridClass,
                                    VectorType, write_dense_array)


class BoundaryFluidStepCli(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="container-boundary-fluid-cli-")
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
            capture_output=True, text=True, encoding="utf-8", timeout=120)
        self.assertEqual(result.returncode == 0, succeeds,
                         f"{args!r}\n{result.stdout}\n{result.stderr}")
        if not succeeds:
            self.assertEqual(result.stdout, "", "failure emitted partial JSON")
        return result

    def enabled_only(self):
        if not self.enabled:
            self.skipTest("OpenVDB is disabled")

    def protect(self, source):
        path = self.directory / source
        self.originals[path] = path.read_bytes()

    def dense(self, name, function, *, dtype=DenseType.FLOAT64, grid="field",
              origin=(0, 0, 0), dimensions=(4, 1, 1), h=1., linear=None,
              translation=(0., 0., 0.), background=None, active=None,
              grid_class=GridClass.UNKNOWN, world=True, tag=VectorType.INVARIANT):
        values, activity = array(dtype.typecode), bytearray()
        for z in range(origin[2], origin[2] + dimensions[2]):
            for y in range(origin[1], origin[1] + dimensions[1]):
                for x in range(origin[0], origin[0] + dimensions[0]):
                    value = function(x, y, z)
                    values.extend((value,) if dtype.components == 1 else value)
                    activity.append(1 if active is None else bool(active(x, y, z)))
        volume = DenseVolume(dtype, dimensions, values, activity, origin=origin,
            linear=linear or (h, 0., 0., 0., h, 0., 0., 0., h),
            translation=translation, grid_name=grid, grid_class=grid_class,
            vector_type=tag, world_space=world,
            background=array(dtype.typecode, background or [0] * dtype.components))
        output = self.directory / name
        output.parent.mkdir(parents=True, exist_ok=True)
        encoded = output.with_suffix(".cvarr")
        write_dense_array(encoded, volume)
        self.call("--dense-import", encoded, output)
        self.protect(name)
        return name

    def read(self, source, grid, origin=(0, 0, 0), dimensions=(4, 1, 1)):
        """Independently decode exact CVDBARR v1 framing and native IEEE bits."""
        self.serial += 1
        encoded = self.directory / f"read-{self.serial}.cvarr"
        self.call("--dense-export", source, encoded, "--input-grid", grid,
                  "--dense-origin", *origin, "--dense-dimensions", *dimensions)
        data = encoded.read_bytes()
        prefix = struct.Struct("<8s6I3i3I9d3d")
        head = prefix.unpack_from(data)
        self.assertEqual(head[:2], (b"CVDBARR\0", 1))
        self.assertEqual(head[6], 0)
        self.assertIn(head[2], (0, 1, 2, 3))
        self.assertEqual(tuple(head[7:10]), tuple(origin))
        self.assertEqual(tuple(head[10:13]), tuple(dimensions))
        width, components = (4 if head[2] % 2 == 0 else 8), (1 if head[2] < 2 else 3)
        code = "f" if width == 4 else "d"
        offset = prefix.size
        background = struct.unpack_from(f"<{components}{code}", data, offset)
        offset += width * components
        tail = struct.Struct("<IIQIIQQQ")
        name_size, metadata_count, outside, stored, reserved, count, payload, total = tail.unpack_from(data, offset)
        offset += tail.size
        self.assertEqual(reserved, 0)
        self.assertEqual(count, math.prod(dimensions))
        self.assertEqual(payload, count * (width * components + 1))
        self.assertEqual(total, len(data))
        expected_name = grid[len("descriptor:"):] if grid.startswith("descriptor:") else grid
        self.assertEqual(data[offset:offset + name_size].decode("utf-8"), expected_name)
        offset += name_size
        for _ in range(metadata_count):
            length, = struct.unpack_from("<I", data, offset)
            offset += 4 + length
            self.assertLessEqual(offset, len(data))
        self.assertEqual(offset + payload, len(data))
        values = struct.unpack_from(f"<{count * components}{code}", data, offset)
        bits = data[offset:offset + count * components * width]
        activity = data[offset + count * components * width:]
        self.assertTrue(all(a in (0, 1) for a in activity))
        result = {}
        for z in range(dimensions[2]):
            for y in range(dimensions[1]):
                for x in range(dimensions[0]):
                    i = x + dimensions[0] * (y + dimensions[1] * z)
                    q = (x + origin[0], y + origin[1], z + origin[2])
                    result[q] = values[i] if components == 1 else tuple(values[components * i:components * (i + 1)])
        return {"values": result, "bits": bits, "activity": activity,
                "dtype": head[2], "class": head[3], "tag": head[4], "world": head[5],
                "linear": tuple(head[13:22]), "translation": tuple(head[22:25]),
                "background": background, "outside": outside, "stored": stored}

    def query(self, source, grid):
        return json.loads(self.call("--query-active-regions", source, "--grid", grid,
            "--region", -2, -2, -2, 8, 3, 3, "--counts-only",
            "--max-region-metadata", 1024 * 1024).stdout)

    def channel(self, *, n=4, velocity=.5, density=None, temperature=None,
                h=1., linear=None, translation=(0., 0., 0.), prefix="c",
                left_pressure=0., right_pressure=0., all_open=False,
                names=None):
        """Open X ends, stationary side walls; prescribed target local MAC U."""
        self.enabled_only()
        map_args = {"h": h, "linear": linear, "translation": translation}
        inside = lambda x, y, z: 0 <= x < n and y == 0 and z == 0
        fluid = self.dense(prefix + "-fluid.vdb", lambda x, y, z: float(inside(x, y, z)),
            grid="F", origin=(-2, -1, -1), dimensions=(n + 4, 3, 3), active=inside, **map_args)
        # Cancel the known first producer impulse so the canonical state's
        # velocity equals the manufactured target even with nonuniform pD.
        raw_u = velocity - .125 * (left_pressure - right_pressure) / (n * h * h)
        velocity_file = self.dense(prefix + "-velocity.vdb", lambda x, y, z: (raw_u, 0., 0.),
            dtype=DenseType.VEC3_FLOAT64, grid="U", background=[raw_u, 0., 0.],
            origin=(-2, -1, -1), dimensions=(n + 4, 3, 3),
            grid_class=GridClass.STAGGERED, world=False, tag=VectorType.CONTRAVARIANT_RELATIVE, **map_args)
        args = ["--project-boundaries", velocity_file, fluid, prefix + "-state.vdb",
                "--velocity-grid", "U", "--fluid-grid", "F", "--time-step", .125,
                "--tolerance", 1e-10]
        if all_open:
            args.extend(("--missing-boundary", "open"))
        else:
            outside = lambda x, y, z: x in (-1, n) and y == 0 and z == 0
            exterior = self.dense(prefix + "-open.vdb", lambda x, y, z: float(outside(x, y, z)),
                grid="O", origin=(-2, -1, -1), dimensions=(n + 4, 3, 3), active=outside, **map_args)
            args.extend(("--boundary-open", exterior, "--boundary-open-grid", "O"))
        if left_pressure != 0 or right_pressure != 0:
            pressure = self.dense(prefix + "-pressure.vdb",
                lambda x, y, z: left_pressure + (right_pressure-left_pressure)*(x+.5)/n,
                grid="PD", origin=(-2, -1, -1), dimensions=(n + 4, 3, 3), **map_args)
            args.extend(("--boundary-pressure", pressure, "--boundary-pressure-grid", "PD"))
        if names:
            for flag, name in names.items():
                args.extend((flag, name))
        self.call(*args)
        state = prefix + "-state.vdb"
        self.protect(state)
        d = self.dense(prefix + "-density.vdb", lambda x, y, z: (density or [1.] * n)[x],
            grid="D", dimensions=(n, 1, 1), grid_class=GridClass.FOG_VOLUME, **map_args)
        t = None
        if temperature is not None:
            t = self.dense(prefix + "-temperature.vdb", lambda x, y, z: temperature[x],
                grid="T", dimensions=(n, 1, 1), **map_args)
        return state, d, t

    def step(self, state, density, output="out.vdb", *flags, temperature=None, bindings=()):
        args = ["--step-boundary-fluid", state, density, output, "--density-grid", "D", *bindings]
        if temperature is not None:
            args.extend(("--temperature", temperature, "--temperature-grid", "T"))
        args.extend(flags)
        report = json.loads(self.call(*args).stdout)
        self.assert_report(report)
        return report

    def assert_report(self, report):
        self.assertEqual(report["schemaVersion"], 1)
        self.assertEqual(report["operation"], "step-boundary-fluid")
        integer_fields = ("fluidCells", "incidentFaces", "pressureFaces", "wallFaces",
            "requestedSubsteps", "actualSubsteps", "attempts", "rejectedAttempts",
            "reservoirFaceApplications", "outflowFaceApplications", "roundedScalarRows",
            "roundedMomentumRows", "totalProjectionIterations", "nodeUpperBound",
            "nativeWorkUpperBound", "formattingWorkUpperBound", "workUpperBound",
            "nativeWorkspaceUpperBound", "reportReservation", "workspaceUpperBound")
        for name in integer_fields:
            self.assertIs(type(report[name]), int, name)
            self.assertGreaterEqual(report[name], 0)
        self.assertEqual(report["workUpperBound"], report["nativeWorkUpperBound"] + report["formattingWorkUpperBound"])
        self.assertEqual(report["workspaceUpperBound"], report["nativeWorkspaceUpperBound"] + report["reportReservation"])
        self.assertLessEqual(report["workUpperBound"], report["limits"]["maxWork"])
        self.assertLessEqual(report["workspaceUpperBound"], report["limits"]["maxInputBytes"])
        for name in ("roundoffLimited", "diagnosticUnderflow", "temperatureInitialized", "initialDivergenceWithinTolerance", "converged", "diagnosticsTruncated"):
            self.assertIs(type(report[name]), bool, name)
        self.assertTrue(report["converged"])
        self.assertIn(report["controls"]["initialVelocity"],("stored","require-solenoidal"))
        self.assertEqual(report["restart"]["initialVelocity"],report["controls"]["initialVelocity"])
        self.assertEqual(report["actualSubsteps"], len(report["steps"]))
        self.assertEqual(report["attempts"], report["actualSubsteps"] + report["rejectedAttempts"])
        self.assertTrue(all(math.isfinite(s["timeStep"]) and s["timeStep"] >= 1e-6 for s in report["steps"]))
        for row in report["steps"]:
            self.assertLessEqual(row["transportCfl"], report["controls"]["maxCfl"])
            self.assertLessEqual(row["endpointCfl"], report["controls"]["maxCfl"])
        for name in ("simulatedTime", "timePartitionError", "cellVolume", "maxTransportCfl",
                     "maxEndpointCfl", "maxConstantResponseDrift", "maxDivergenceBefore",
                     "maxDivergenceAfter", "divergenceRoundoffBound", "pressureResidual",
                     "densityMassBefore", "densityMassAfter", "densityOutwardTransfer",
                     "temperatureVolumeBefore", "temperatureVolumeAfter", "temperatureOutwardTransfer",
                     "densityBalanceError", "temperatureBalanceError",
                     "densityBalanceRoundoffBound", "temperatureBalanceRoundoffBound"):
            self.assertTrue(math.isfinite(report[name]), name)
        self.assertLessEqual(report["densityBalanceError"], report["densityBalanceRoundoffBound"])
        self.assertLessEqual(report["temperatureBalanceError"], report["temperatureBalanceRoundoffBound"])
        self.assertLessEqual(len(report["diagnostics"]), 8)
        self.assertFalse(report["diagnosticsTruncated"])
        self.assertIn("no normalization", report["scalarPolicy"])
        self.assertIn("advective MAC", report["momentumPolicy"])
        self.assertIn("fresh impulse", report["restartPolicy"])
        self.assertIn("cell-centered", report["speedPolicy"])

    def restart_args(self, report, output, *flags):
        binding = report["restart"]
        args = ["--step-boundary-fluid", binding["state"], binding["density"]["file"], output]
        for flag, key in (("--velocity-grid","velocityGrid"), ("--fluid-grid","fluidGrid"),
                          ("--face-kind-grid","faceKindGrid"), ("--face-fraction-grid","faceFractionGrid"),
                          ("--face-pressure-grid","facePressureGrid"), ("--wall-velocity-grid","wallVelocityGrid")):
            args.extend((flag,binding[key]))
        args.extend(("--density-grid",binding["density"]["grid"], "--temperature",binding["temperature"]["file"],
                     "--temperature-grid",binding["temperature"]["grid"],*flags))
        if "--initial-velocity" not in flags:
            args.extend(("--initial-velocity",binding["initialVelocity"]))
        return args

    def restart_report(self, report, output, *flags):
        result = json.loads(self.call(*self.restart_args(report,output,*flags)).stdout)
        self.assert_report(result)
        return result

    def test_help_strict_parser_aliases_ranges_and_irrelevant_options(self):
        text = self.call("--help").stdout
        for flag in ("--step-boundary-fluid", "--density-grid", "--temperature",
                     "--max-step-attempts", "--reservoir-density", "--max-fluid-work",
                     "--face-fraction-grid", "--wall-velocity-name", "--initial-velocity"):
            self.assertIn(flag, text)
        operands = [("--step-boundary-fluid",), ("--step-boundary-fluid", "a"),
                    ("--step-boundary-fluid", "a", "b"), ("--step-boundary-fluid", "a", "b", ""),
                    ("--step-boundary-fluid", "-h", "b", "bad.vdb"),
                    ("--step-boundary-fluid", "a", "--temperature", "bad.vdb")]
        flags = [("--initial-velocity","cleanup"),("--initial-velocity",""),
                 ("--initial-velocity","stored","--initial-velocity","require-solenoidal"),
                 ("--density-grid", "a", "--grid", "b"),
                 ("--grid", "a", "--input-grid", "b"),
                 ("--gravity", 0, 0, 0, "--acceleration", 0, 0, 0),
                 ("--iterations", 1, "--max-iterations", 2),
                 ("--max-work", 1, "--max-fluid-work", 2),
                 ("--max-nodes", 1, "--max-fluid-nodes", 2),
                 ("--max-voxels", 1, "--max-stored-leaf-voxels", 2),
                 ("--temperature-grid", "T"), ("--temperature", ""),
                 ("--velocity-grid", "-h"), ("--fluid-grid", ""),
                 ("--time-step", 0), ("--time-step", "nan"), ("--time-step", "1e999"),
                 ("--time-step", "1e-999"), ("--substeps", 0), ("--substeps", 65),
                 ("--max-substeps", 1, "--substeps", 2), ("--max-step-attempts", 1, "--substeps", 2),
                 ("--time-step", 1e-6, "--substeps", 2), ("--max-cfl", 0), ("--max-cfl", .5001),
                 ("--gravity", 0, 0), ("--gravity", 1e13, 0, 0),
                 ("--buoyancy-density", -1), ("--buoyancy-temperature", 1000001),
                 ("--ambient-temperature", -1), ("--reservoir-density", -1),
                 ("--reservoir-temperature", 1e7), ("--buoyancy-direction", 0, 0, 0),
                 ("--tolerance", 1e-15), ("--iterations", 10001),
                 ("--max-fluid-cells", 1000001), ("--max-fluid-nodes", 1000001),
                 ("--max-fluid-work", 10000000001), ("--max-grids", 4097),
                 ("--max-metadata-bytes", 16777217), ("--max-input-bytes", 17179869185),
                 ("--max-output-bytes", 0), ("--max-voxels", "+1"),
                 ("--max-voxels", " 1"), ("--max-voxels", "18446744073709551616"),
                 ("--density-name", "same", "--temperature-name", "same"),
                 ("--density-name", "x"*257), ("--wall-velocity", 0, 0, 0),
                 ("--boundary-open", "unused"), ("--ambient-pressure", 0),
                 ("--viscosity", 0), ("--sources", "unused.json"),
                 ("--velocity-name", ""), ("--voxel-size", 1), ("--max-triangles", 10)]
        operands += [("--step-boundary-fluid", "missing", "missing", "bad.vdb", *f) for f in flags]
        for args in operands:
            with self.subTest(args=args):
                result = self.call(*args, succeeds=False)
                self.assertNotIn("ENABLE_OPENVDB=ON", result.stderr)
                self.assertFalse((self.directory / "bad.vdb").exists())
        self.assertEqual(list(self.directory.iterdir()), [])

    def test_disabled_availability_precedes_paths_and_tiny_positive_report_caps(self):
        if self.enabled:
            self.skipTest("OpenVDB is enabled")
        for caps in (("--max-input-bytes", 1), ("--max-fluid-work", 1),
                     ("--max-output-bytes", 1), ("--max-metadata-bytes", 1)):
            result = self.call("--step-boundary-fluid", "不存在/状態.vdb", "missing.vdb",
                "missing-parent/out.vdb", *caps, succeeds=False)
            self.assertIn("ENABLE_OPENVDB=ON", result.stderr)
        result=self.call("--step-boundary-fluid","missing","missing","missing-parent/strict.vdb",
            "--initial-velocity","require-solenoidal","--max-input-bytes",1,succeeds=False)
        self.assertIn("ENABLE_OPENVDB=ON",result.stderr)
        self.assertFalse(list(self.directory.iterdir()))

    def test_constant_channel_nine_grid_restart_metadata_and_typed_report(self):
        state, density, temperature = self.channel(n=3, velocity=.25, density=[2]*3,
                                                   temperature=[300]*3)
        r = self.step(state, density, "out.vdb", "--time-step", .5,
                      "--reservoir-density", 2, "--reservoir-temperature", 300,
                      temperature=temperature)
        self.assertEqual(r["actualSubsteps"], 1)
        self.assertEqual(r["maxConstantResponseDrift"], 0)
        self.assertEqual(r["densityMassBefore"], 6)
        self.assertEqual(r["densityMassAfter"], 6)
        self.assertEqual(r["densityOutwardTransfer"], 0)
        self.assertEqual(r["reservoirFaceApplications"], 1)
        self.assertEqual(r["outflowFaceApplications"], 1)
        self.assertFalse(r["temperatureInitialized"])
        self.assertEqual(list(self.read("out.vdb", "density", dimensions=(3,1,1))["values"].values()), [2]*3)
        self.assertEqual(list(self.read("out.vdb", "temperature", dimensions=(3,1,1))["values"].values()), [300]*3)
        key = "container:boundary_projection_state_id"
        metadata = lambda q: {m["name"]["text"]: m for m in q["gridMetadata"]}
        old = metadata(self.query(state, "fluid"))[key]
        new = metadata(self.query("out.vdb", "fluid"))[key]
        self.assertNotEqual(old, new)
        for name in ("velocity", "pressure", "fluid", "boundary_kind", "boundary_fraction", "boundary_pressure", "boundary_wall_velocity"):
            self.assertEqual(metadata(self.query("out.vdb", name))[key], new)
        for name in ("density", "temperature"):
            self.assertFalse(any(k.startswith("container:boundary_") for k in metadata(self.query("out.vdb", name))))
        self.assertEqual(r["restart"]["state"], "out.vdb")
        self.assertEqual(r["restart"]["density"], {"file":"out.vdb", "grid":"descriptor:density"})
        self.assertEqual(r["restart"]["temperature"], {"file":"out.vdb", "grid":"descriptor:temperature"})

    def test_shared_flux_pulse_matches_independent_binomial_and_restart_partition(self):
        state, density, _ = self.channel(density=[1,0,0,0])
        self.step(state, density, "first.vdb", "--time-step", .5)
        self.assertEqual(list(self.read("first.vdb", "density")["values"].values()), [.75,.25,0,0])
        r = json.loads(self.call("--step-boundary-fluid", "first.vdb", "first.vdb", "second.vdb",
            "--density-grid", "density", "--temperature", "first.vdb", "--temperature-grid", "temperature",
            "--time-step", .5).stdout)
        self.assert_report(r)
        combined = self.step(state, density, "combined.vdb", "--time-step", 1, "--substeps", 2)
        expected = [.5625,.375,.0625,0]
        for name in ("second.vdb", "combined.vdb"):
            self.assertEqual(list(self.read(name, "density")["values"].values()), expected)
            u = self.read(name, "velocity", dimensions=(5,2,2))["values"]
            self.assertEqual([u[(x,0,0)][0] for x in range(5)], [.5]*5)
        self.assertEqual([s["timeStep"] for s in combined["steps"]], [.5,.5])

    def test_reverse_reservoir_and_ambient_temperature_initialization(self):
        state, density, _ = self.channel(n=3, velocity=-.5, density=[0,0,0])
        r = self.step(state, density, "reverse.vdb", "--time-step", .5,
                      "--reservoir-density", 1, "--ambient-temperature", 290)
        self.assertEqual(list(self.read("reverse.vdb", "density", dimensions=(3,1,1))["values"].values()), [0,0,.25])
        self.assertEqual(list(self.read("reverse.vdb", "temperature", dimensions=(3,1,1))["values"].values()), [290]*3)
        self.assertEqual(r["densityMassAfter"], .25)
        self.assertEqual(r["densityOutwardTransfer"], -.25)
        self.assertTrue(r["temperatureInitialized"])

    def test_closed_ring_has_independent_scalar_and_momentum_oracle(self):
        self.enabled_only()
        cells = {(0,0,0), (1,0,0), (1,1,0), (0,1,0)}
        self.dense("ring-fluid.vdb", lambda x,y,z: float((x,y,z) in cells), grid="F", dimensions=(2,2,1))
        velocities = {(1,0,0):(.25,0,0), (1,1,0):(-.25,.25,0), (0,1,0):(0,-.25,0)}
        self.dense("ring-u.vdb", lambda x,y,z: velocities.get((x,y,z),(0,0,0)),
            dtype=DenseType.VEC3_FLOAT64, grid="U", dimensions=(3,3,2),
            grid_class=GridClass.STAGGERED, world=False, tag=VectorType.CONTRAVARIANT_RELATIVE)
        self.dense("ring-d.vdb", lambda x,y,z: float((x,y,z)==(0,0,0)), grid="D",
            dimensions=(2,2,1), grid_class=GridClass.FOG_VOLUME)
        self.call("--project-boundaries", "ring-u.vdb", "ring-fluid.vdb", "ring-state.vdb",
                  "--velocity-grid", "U", "--fluid-grid", "F", "--tolerance", 1e-10)
        self.protect("ring-state.vdb")
        r = self.step("ring-state.vdb", "ring-d.vdb", "ring.vdb", "--time-step", .5)
        d = self.read("ring.vdb", "density", dimensions=(2,2,1))["values"]
        self.assertEqual(d, {(0,0,0):.875, (1,0,0):.125, (0,1,0):0., (1,1,0):0.})
        u = self.read("ring.vdb", "velocity", dimensions=(3,3,2))["values"]
        # a' = a - dt*a*a from physical normal-wall zeros; transverse terms cancel.
        self.assertEqual((u[(1,0,0)][0], u[(1,1,0)][0], u[(1,1,0)][1], u[(0,1,0)][1]),
                         (.21875,-.21875,.21875,-.21875))
        self.assertEqual(r["densityMassAfter"], 1)
        self.assertEqual(r["densityOutwardTransfer"], 0)
        self.assertEqual(r["maxDivergenceAfter"], 0)

    def test_world_force_buoyancy_and_storage_rounding(self):
        state, density, temperature = self.channel(n=1, velocity=0, density=[2], temperature=[300], all_open=True)
        r = self.step(state, density, "buoyant.vdb", "--time-step", .125,
            "--gravity", .125, 0, 0, "--buoyancy-density", .25, "--buoyancy-temperature", .5,
            "--ambient-temperature", 290, "--buoyancy-direction", 1, 0, 0, temperature=temperature)
        u = self.read("buoyant.vdb", "velocity", dimensions=(2,2,2))["values"]
        self.assertEqual(u[(0,0,0)][0], .578125)
        self.assertEqual(u[(1,0,0)][0], .578125)
        self.assertEqual(r["pressureResidual"], 0)
        rounded = self.step(state, density, "rounded.vdb", "--time-step", .1, "--gravity", .1, 0, 0)
        self.assertEqual(rounded["roundedMomentumRows"], 2)
        self.assertEqual(self.read("rounded.vdb", "velocity", dimensions=(2,2,2))["values"][(0,0,0)][0], .1*.1)
        self.dense("float-d.vdb",lambda x,y,z:2.25,dtype=DenseType.FLOAT32,grid="D",
                   dimensions=(1,1,1),grid_class=GridClass.FOG_VOLUME)
        self.dense("float-t.vdb",lambda x,y,z:299.5,dtype=DenseType.FLOAT32,grid="T",dimensions=(1,1,1))
        self.step(state,"float-d.vdb","float.vdb","--time-step",.5,temperature="float-t.vdb")
        for grid,expected in (("density",2.25),("temperature",299.5)):
            result = self.read("float.vdb",grid,dimensions=(1,1,1))
            self.assertEqual(result["dtype"],1)
            self.assertEqual(result["values"][(0,0,0)],expected)

    def test_nonuniform_pressure_retry_counts_discarded_solves_and_caps(self):
        state, density, _ = self.channel(n=3, velocity=0, density=[1]*3,
                                        left_pressure=1.5, right_pressure=-1.5)
        initial = self.read(state, "velocity", dimensions=(4,2,2))["values"]
        self.assertTrue(all(abs(initial[(x,0,0)][0]) < 1e-12 for x in range(4)))
        r = self.step(state, density, "retry.vdb", "--time-step", 1, "--max-cfl", .3,
                      "--reservoir-density", 1)
        self.assertEqual([s["timeStep"] for s in r["steps"]], [.5,.25,.25])
        self.assertEqual(r["rejectedAttempts"], 2)
        self.assertEqual(r["attempts"], 5)
        self.assertGreater(r["totalProjectionIterations"], sum(s["projectionIterations"] for s in r["steps"]))
        u = self.read("retry.vdb", "velocity", dimensions=(4,2,2))["values"]
        for x in range(4):
            self.assertAlmostEqual(u[(x,0,0)][0], 1., delta=2e-6)
        for flag, value in (("--max-step-attempts",1), ("--max-substeps",2)):
            name = f"capped-{flag[2:]}.vdb"
            self.call("--step-boundary-fluid", state, density, name, "--density-grid", "D",
                "--time-step", 1, "--max-cfl", .3, flag, value, succeeds=False)
            self.assertFalse((self.directory/name).exists())

    def test_hydrostatic_world_units_and_reflected_native_map(self):
        state, density, _ = self.channel(n=3, velocity=0, density=[0]*3, h=.5,
                                        left_pressure=4.5, right_pressure=0.)
        r = self.step(state, density, "hydro.vdb", "--time-step", .125, "--gravity", -3, 0, 0)
        self.assertEqual(r["cellVolume"], .125)
        self.assertEqual(r["controls"]["initialVelocity"],"stored")
        self.assertFalse(r["initialDivergenceWithinTolerance"])
        pressure = self.read("hydro.vdb", "pressure", dimensions=(3,1,1))["values"]
        for q, expected in zip(((0,0,0),(1,0,0),(2,0,0)), (3.75,2.25,.75)):
            self.assertAlmostEqual(pressure[q], expected, delta=2e-6)
        velocity = self.read("hydro.vdb", "velocity", dimensions=(4,2,2))["values"]
        for x in range(4):
            self.assertAlmostEqual(velocity[(x,0,0)][0], 0, delta=2e-6)
        # Independent exact one-cell static equilibrium at the same physical
        # voxel size and gravity also verifies literal zero and strict restart.
        static_state,static_density,_=self.channel(n=1,velocity=0,density=[0],h=.5,
            left_pressure=1.5,right_pressure=0.,prefix="static-cell")
        static=self.step(static_state,static_density,"static.vdb","--time-step",.125,"--gravity",-3,0,0,
            "--initial-velocity","require-solenoidal")
        self.assertTrue(static["initialDivergenceWithinTolerance"])
        self.assertEqual(self.read("static.vdb","pressure",dimensions=(1,1,1))["values"][(0,0,0)],.75)
        self.assertTrue(all(v==0 for xyz in self.read("static.vdb","velocity",dimensions=(2,2,2))["values"].values() for v in xyz))
        self.restart_report(static,"static-next.vdb","--time-step",.125,"--gravity",-3,0,0,
            "--initial-velocity","require-solenoidal")
        matrix = (0.,-.5,0., -.5,0.,0., 0.,0.,.5)
        state, density, _ = self.channel(n=2, velocity=.25, density=[1,1], h=.5,
            linear=matrix, translation=(20.,-30.,40.), prefix="reflected")
        r = self.step(state, density, "reflect.vdb", "--time-step", .5, "--reservoir-density", 1)
        decoded = self.read("reflect.vdb", "velocity", dimensions=(3,2,2))
        self.assertEqual(decoded["linear"], matrix)
        self.assertEqual(decoded["translation"], (20.,-30.,40.))
        self.assertEqual(decoded["class"], 3)
        self.assertEqual(decoded["tag"], 3)
        self.assertEqual(decoded["world"], 0)
        self.assertEqual(r["cellVolume"], .125)
        self.assertEqual(r["maxWorldSpeedAfter"], .125)

    def test_custom_output_names_complete_restart_selectors_and_aliases(self):
        names = {"--velocity-name":"seedU", "--fluid-name":"seedF", "--face-kind-name":"seedK",
                 "--face-fraction-name":"seedTheta", "--face-pressure-name":"seedPD", "--wall-velocity-name":"seedW"}
        state, density, _ = self.channel(n=2, velocity=.25, density=[1,0], names=names)
        bindings = ("--velocity-grid","seedU", "--fluid-grid","seedF", "--face-kind-grid","seedK",
                    "--face-fraction-grid","seedTheta", "--face-pressure-grid","seedPD", "--wall-velocity-grid","seedW")
        output_names = {"density":"smoke", "temperature":"heat", "velocity":"speed", "pressure":"p",
            "fluid":"domain", "face-kind":"kind", "face-fraction":"theta", "face-pressure":"pd", "wall-velocity":"walls"}
        flags = [x for key,name in output_names.items() for x in (f"--{key}-name",name)]
        r = json.loads(self.call("--step-boundary-fluid", state, density, "named.vdb", "--grid", "D",
            *bindings, *flags, "--time-step", .5, "--acceleration", 0,0,0,
            "--max-iterations", 1000, "--max-work", 2000000000,
            "--max-nodes",1000000,"--max-voxels",18446744073709551615).stdout)
        self.assert_report(r)
        self.assertEqual(r["limits"]["maxStoredLeafVoxels"], 18446744073709551615)
        self.assertEqual(r["restart"]["velocityGrid"], "descriptor:speed")
        self.assertEqual(r["restart"]["density"]["grid"], "descriptor:smoke")
        self.assertEqual(r["restart"]["temperature"]["grid"], "descriptor:heat")
        self.assertEqual(list(self.read("named.vdb", "smoke", dimensions=(2,1,1))["values"].values()), [.875,.125])
        flags = ("--velocity-grid","speed", "--fluid-grid","domain", "--face-kind-grid","kind",
                 "--face-fraction-grid","theta", "--face-pressure-grid","pd", "--wall-velocity-grid","walls",
                 "--input-grid","smoke", "--temperature","named.vdb", "--temperature-grid","heat")
        restarted = json.loads(self.call("--step-boundary-fluid", "named.vdb", "named.vdb", "named-next.vdb",
                                        *flags,"--time-step",.5).stdout)
        self.assert_report(restarted)
        self.assertEqual(list(self.read("named-next.vdb", "density", dimensions=(2,1,1))["values"].values()), [.765625,.21875])

    def test_reported_exact_descriptors_restart_selector_like_output_names(self):
        state,density,_ = self.channel(n=3,velocity=.5,density=[1,0,0])
        raw_names = {"density":"descriptor:smoke", "temperature":"descriptor:heat",
            "velocity":"descriptor:speed", "pressure":"descriptor:p", "fluid":"descriptor:domain",
            "face-kind":"descriptor:kind", "face-fraction":"descriptor:theta",
            "face-pressure":"descriptor:pd", "wall-velocity":"descriptor:wall"}
        flags = [x for key,name in raw_names.items() for x in (f"--{key}-name",name)]
        r = self.step(state,density,"selectors.vdb","--time-step",.5,*flags)
        self.assertEqual(r["gridNames"]["density"],"descriptor:smoke")
        self.assertEqual(r["restart"]["velocityGrid"],"descriptor:descriptor:speed")
        self.assertEqual(r["restart"]["density"]["grid"],"descriptor:descriptor:smoke")
        # Distinct, nonempty raw names remain literal unique descriptors in
        # actual Archive::write; no invented native [0] suffix is needed.
        for raw in raw_names.values():
            q = self.query("selectors.vdb","descriptor:"+raw)
            self.assertEqual(q["descriptorName"]["text"],raw)
            self.assertEqual(q["gridName"]["text"],raw)
        first = self.read("selectors.vdb",r["restart"]["density"]["grid"],dimensions=(3,1,1))
        self.assertEqual(list(first["values"].values()),[.75,.25,0])
        # Every selector is taken from the report, including six canonical
        # fields plus both scalar bindings, rather than reconstructing names.
        self.restart_report(r,"selectors-next.vdb","--time-step",.5)
        second = self.read("selectors-next.vdb","density",dimensions=(3,1,1))
        self.assertEqual(list(second["values"].values()),[.5625,.375,.0625])

    def test_native_shared_caps_malformed_scalars_and_no_partial_output(self):
        state, density, _ = self.channel(n=3, velocity=.25, density=[1,0,0])
        for i,flags in enumerate((("--max-fluid-cells",1), ("--max-fluid-work",1),
            ("--max-fluid-nodes",1), ("--max-input-bytes",1024), ("--max-output-bytes",1),
            ("--max-metadata-bytes",1), ("--max-grids",1), ("--max-voxels",1))):
            name = f"capped-{i}.vdb"
            with self.subTest(flags=flags):
                self.call("--step-boundary-fluid",state,density,name,"--grid","D",*flags,succeeds=False)
                self.assertFalse((self.directory/name).exists())
        for i,kwargs in enumerate(({"negative":True}, {"wrong_class":True}, {"outside":True})):
            self.dense(f"invalid-{i}.vdb", lambda x,y,z: -1. if kwargs.get("negative") else 1.,
                grid="bad", dimensions=(4 if kwargs.get("outside") else 3,1,1),
                grid_class=GridClass.UNKNOWN if kwargs.get("wrong_class") else GridClass.FOG_VOLUME)
            name = f"invalid-out-{i}.vdb"
            self.call("--step-boundary-fluid",state,f"invalid-{i}.vdb",name,"--grid","bad",succeeds=False)
            self.assertFalse((self.directory/name).exists())

    def test_unicode_sources_names_literal_filename_and_exclusive_destinations(self):
        state, density, _ = self.channel(n=2,velocity=.25,density=[1,0],prefix="入力-é/煙")
        output = "入力-é/状態.vdb"
        r = self.step(state,density,output,"--time-step",.5,"--density-name","密度-ç")
        self.assertEqual(r["gridNames"]["density"], "密度-ç")
        self.assertEqual(list(self.read(output,"密度-ç",dimensions=(2,1,1))["values"].values()), [.875,.125])
        self.protect(output)
        for destination in (output,state,density):
            self.call("--step-boundary-fluid",state,density,destination,"--grid","D",succeeds=False)
        foreign = self.directory/"foreign.vdb"
        foreign.write_bytes(b"foreign destination bytes")
        self.protect("foreign.vdb")
        self.call("--step-boundary-fluid",state,density,"foreign.vdb","--grid","D",succeeds=False)
        self.step(state,density,"./-h","--time-step",.5)
        self.assertTrue((self.directory/"-h").is_file())


    def test_actual_arrest_output_restarts_with_reported_bindings_and_strict_enforces_both_gates(self):
        state,density,_=self.channel(n=3,velocity=.25,density=[0]*3,h=.5,
            left_pressure=4.5,right_pressure=0.,prefix="arrest")
        first=self.step(state,density,"arrest.vdb","--time-step",.125,"--gravity",-4,0,0)
        self.assertTrue(first["initialDivergenceWithinTolerance"])
        self.assertEqual(first["controls"]["initialVelocity"],"stored")
        self.assertEqual(first["actualSubsteps"],1)
        self.assertEqual(first["attempts"],1)
        u=self.read("arrest.vdb","velocity",dimensions=(4,2,2))["values"]
        self.assertLess(max(abs(u[(x,0,0)][0]) for x in range(4)),1e-12)
        # WORLD pressure slope gives +3 acceleration; gravity-4 gives net-1.
        # With h=.5, dt=.125 this changes local MAC by-.25 every actual step.
        second=self.restart_report(first,"arrest-next.vdb","--time-step",.125,"--gravity",-4,0,0)
        self.assertFalse(second["initialDivergenceWithinTolerance"])
        self.assertEqual(second["actualSubsteps"],1)
        self.assertEqual(second["attempts"],1)
        v=self.read("arrest-next.vdb","velocity",dimensions=(4,2,2))["values"]
        for x in range(4):
            self.assertAlmostEqual(v[(x,0,0)][0],-.25,delta=2e-6)
        p=self.read("arrest-next.vdb","pressure",dimensions=(3,1,1))["values"]
        for x,expected in enumerate((3.75,2.25,.75)):
            self.assertAlmostEqual(p[(x,0,0)],expected,delta=2e-6)
        # Strict rejects both this non-solenoidal entry and a valid entry whose
        # projected nearrest endpoint fails the same stored-only criterion.
        self.call(*self.restart_args(first,"strict-entry.vdb","--time-step",.125,"--gravity",-4,0,0,
            "--initial-velocity","require-solenoidal"),succeeds=False)
        self.call("--step-boundary-fluid",state,density,"strict-endpoint.vdb","--density-grid","D",
            "--time-step",.125,"--gravity",-4,0,0,"--initial-velocity","require-solenoidal",succeeds=False)
        self.assertFalse((self.directory/"strict-entry.vdb").exists())
        self.assertFalse((self.directory/"strict-endpoint.vdb").exists())


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--tool",required=True,type=Path)
    parser.add_argument("--enabled",required=True,choices=("0","1"))
    args, remaining = parser.parse_known_args()
    BoundaryFluidStepCli.tool = args.tool.resolve(strict=True)
    BoundaryFluidStepCli.enabled = args.enabled == "1"
    unittest.main(argv=[__file__,*remaining])
