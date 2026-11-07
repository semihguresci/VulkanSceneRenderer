"""Density scattering, point trajectories/attributes and closed MAC projection."""

import argparse
import json
import math
import pathlib
import subprocess
import tempfile
import unittest


class OpenVdbPointFlowCli(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="container-vdb-point-flow-")
        self.directory = pathlib.Path(self.temporary.name)

    def tearDown(self):
        self.temporary.cleanup()

    def run_tool(self, *arguments, succeeds=True):
        result = subprocess.run([str(self.tool), *map(str, arguments)], cwd=self.directory,
                                capture_output=True, text=True, timeout=60)
        self.assertEqual(result.returncode == 0, succeeds, result.stdout + result.stderr)
        return result

    def require_enabled(self):
        if not self.enabled:
            self.skipTest("OpenVDB is disabled")

    def fog(self):
        self.run_tool("--sphere", "sphere.vdb", "--radius", "1", "--voxel-size", ".25")
        self.run_tool("--fog", "sphere.vdb", "fog.vdb", "--grid-name", "density")

    def points(self):
        records = [{"position": [-.25, 0, 0], "velocity": [7, 8, 9], "radius": .3},
                   {"position": [.25, 0, 0], "velocity": [4, 5, 6], "radius": .4}]
        (self.directory / "points.json").write_text(json.dumps(records), encoding="utf-8")
        self.run_tool("--points", "points.json", "points.vdb", "--voxel-size", ".25")
        return records

    def records(self, source, name="records.json"):
        self.run_tool("--export-points", source, name)
        return json.loads((self.directory / name).read_text(encoding="utf-8"))

    def test_help_and_parameter_routing(self):
        text = self.run_tool("--help").stdout
        for command in ("--scatter", "--advect-points", "--sample-point-velocity",
                        "--sample-point-radius", "--project", "--simulate", "--simulate-sequence",
                        "--points-sdf", "--splat-points-density", "--splat-points-velocity",
                        "--splat-points-radius", "--viscosity", "--fluid-sources"):
            self.assertIn(command, text)
        invalid = [
            ["--scatter", "missing.vdb", "bad.vdb", "--seed", "-1"],
            ["--scatter", "missing.vdb", "bad.vdb", "--seed", "18446744073709551616"],
            ["--scatter", "missing.vdb", "bad.vdb", "--point-count", "+2"],
            ["--scatter", "missing.vdb", "bad.vdb", "--integration", "rk4"],
            ["--advect-points", "a.vdb", "b.vdb", "bad.vdb"],
            ["--advect-points", "a.vdb", "b.vdb", "bad.vdb", "--time", "1", "--integration", "euler"],
            ["--advect-points", "a.vdb", "b.vdb", "bad.vdb", "--time", "1", "--point-count", "2"],
            ["--project", "a.vdb", "b.vdb", "bad.vdb", "--grid-name", "ignored"],
            ["--project", "a.vdb", "b.vdb", "bad.vdb", "--steps", "2"],
            ["--project", "a.vdb", "b.vdb", "bad.vdb", "--max-fluid-cells", "1000001"],
            ["--poisson", "a.vdb", "bad.vdb", "--no-domain"],
            ["--sample-point-radius", "a.vdb", "b.vdb", "bad.vdb", "--time", "1"],
            ["--simulate", "a.vdb", "b.vdb", "c.vdb"],
            ["--simulate", "a.vdb", "b.vdb", "c.vdb", "bad.vdb", "--no-domain"],
            ["--simulate", "a.vdb", "b.vdb", "c.vdb", "bad.vdb", "--acceleration", "0", "1", "0"],
            ["--simulate", "a.vdb", "b.vdb", "c.vdb", "bad.vdb", "--temperature-grid", "unused"],
            ["--project", "a.vdb", "b.vdb", "bad.vdb", "--gravity", "0", "1", "0"],
            ["--simulate", "a.vdb", "b.vdb", "c.vdb", "bad.vdb", "--frames", "2"],
            ["--simulate-sequence", "a.vdb", "b.vdb", "c.vdb", "bad.vdb", "--frames", "257"],
            ["--points-sdf", "a.vdb", "bad.vdb", "--point-mass", "1"],
            ["--splat-points-density", "a.vdb", "bad.vdb", "--half-width", "4"],
            ["--splat-points-density", "a.vdb", "bad.vdb", "--splat-density-name", "ignored"],
            ["--splat-points-radius", "a.vdb", "bad.vdb", "--kinematic-viscosity", ".1"],
            ["--viscosity", "a.vdb", "bad.vdb", "--input-grid", "ignored"],
            ["--viscosity", "a.vdb", "bad.vdb", "--domain-grid", "ignored"],
            ["--viscosity", "a.vdb", "bad.vdb", "--steps", "2"],
            ["--viscosity", "a.vdb", "bad.vdb", "--gravity", "0", "1", "0"],
            ["--viscosity", "a.vdb", "bad.vdb", "--grid-name", "ignored"],
            ["--viscosity", "a.vdb", "bad.vdb", "--voxel-size", ".1"],
            ["--fluid-sources", "a.vdb", "bad.vdb", "--density-rate", "1"],
            ["--fluid-sources", "a.vdb", "bad.vdb", "--input-grid", "ignored"],
            ["--fluid-sources", "a.vdb", "bad.vdb", "--max-iterations", "4"],
            ["--points", "a.json", "bad.vdb", "--source", "0", "0", "0", "1"],
        ]
        for arguments in invalid:
            with self.subTest(arguments=arguments):
                self.run_tool(*arguments, succeeds=False)
                self.assertFalse((self.directory / "bad.vdb").exists())

    def test_simulation_sequence_manifest_restart_and_output_protection(self):
        if not self.enabled:
            failed=self.run_tool("--simulate-sequence", "missing", "missing", "missing", "frames", "--frames", 1, succeeds=False)
            self.assertIn("ENABLE_OPENVDB=ON",failed.stdout+failed.stderr)
            self.assertFalse((self.directory/"frames").exists())
            return
        self.fog()
        self.run_tool("--velocity", "wind.vdb", "--vector", 0, 0, 0)
        self.run_tool("--project", "wind.vdb", "fog.vdb", "initial.vdb", "--domain-grid", "density")
        arguments=("--simulate-sequence", "fog.vdb", "initial.vdb", "initial.vdb", "frames",
                   "--input-grid", "density", "--velocity-grid", "velocity", "--domain-grid", "fluid",
                   "--frames", 2, "--time", .1, "--density-name", "smoke", "--temperature-name", "kelvin")
        report=json.loads(self.run_tool(*arguments).stdout)
        self.assertEqual(len(report["frames"]),2)
        self.assertTrue(all(frame["converged"] for frame in report["frames"]))
        manifest=json.loads((self.directory/"frames"/"sequence.json").read_text(encoding="utf-8"))
        self.assertEqual(manifest["grids"]["density"],"smoke")
        self.assertEqual(manifest["grids"]["temperature"],"kelvin")
        self.assertEqual([frame["timeSeconds"] for frame in manifest["frames"]],[.1,.2])
        first=manifest["frames"][0]; last=manifest["frames"][1]
        self.assertEqual(first["densityMassBefore"],last["densityMassAfter"])
        existing=(self.directory/"frames"/"sequence.json").read_bytes()
        self.run_tool(*arguments,succeeds=False)
        self.assertEqual((self.directory/"frames"/"sequence.json").read_bytes(),existing)
        limited=list(arguments); limited[4]="limited"
        self.run_tool(*limited,"--max-output-bytes",1,succeeds=False)
        self.assertFalse((self.directory/"limited").exists())

    def test_disabled_operations_fail_before_input_io(self):
        if self.enabled:
            self.skipTest("OpenVDB is enabled")
        commands = [["--scatter", "missing.vdb", "bad.vdb"],
                    ["--advect-points", "a.vdb", "b.vdb", "bad.vdb", "--time", "1"],
                    ["--sample-point-velocity", "a.vdb", "b.vdb", "bad.vdb"],
                    ["--sample-point-radius", "a.vdb", "b.vdb", "bad.vdb"],
                    ["--project", "a.vdb", "b.vdb", "bad.vdb"],
                    ["--points-sdf", "a.vdb", "bad.vdb"],
                    ["--splat-points-density", "a.vdb", "bad.vdb"],
                    ["--splat-points-velocity", "a.vdb", "bad.vdb"],
                    ["--splat-points-radius", "a.vdb", "bad.vdb"],
                    ["--viscosity", "a.vdb", "bad.vdb"],
                    ["--fluid-sources", "a.vdb", "bad.vdb"],
                    ["--simulate", "a.vdb", "b.vdb", "c.vdb", "bad.vdb"]]
        for arguments in commands:
            result = self.run_tool(*arguments, succeeds=False)
            self.assertIn("ENABLE_OPENVDB=ON", result.stdout + result.stderr)
        self.assertFalse((self.directory / "bad.vdb").exists())

    def test_native_point_sdf_and_mass_conserving_attribute_splats(self):
        self.require_enabled()
        self.points()
        original=(self.directory/"points.vdb").read_bytes()
        sdf=json.loads(self.run_tool("--points-sdf","points.vdb","sdf.vdb","--voxel-size",.1,
                                     "--input-grid","points","--grid-name","shape","--max-points",2).stdout)
        self.assertEqual(sdf["gridName"],"shape")
        self.assertLess(float(self.run_tool("--sample","sdf.vdb",-.25,0,0,"--input-grid","shape").stdout),0)
        for field in ("density","velocity","radius"):
            extra=() if field=="density" else ("--splat-density-name","rho")
            report=json.loads(self.run_tool("--splat-points-"+field,"points.vdb",field+".vdb",
                "--voxel-size",.25,"--point-mass",2.5,"--max-points",2,"--grid-name","deposited",*extra).stdout)
            self.assertEqual(report["points"],2)
            self.assertAlmostEqual(report["totalMass"],5)
            self.assertAlmostEqual(report["depositedMass"],5)
            self.assertEqual(report["densityGrid"],"" if field=="density" else "rho")
            command="--sample-vector" if field=="velocity" else "--sample"
            sample=json.loads(self.run_tool(command,field+".vdb",-.25,0,0,"--input-grid","deposited").stdout)
            expected=[7,8,9] if field=="velocity" else .3 if field=="radius" else 160
            self.assertEqual(sample,expected)
        self.assertEqual((self.directory/"points.vdb").read_bytes(),original)

    def test_viscosity_preserves_scalar_state_custom_names_and_restart(self):
        self.require_enabled()
        self.fog()
        self.run_tool("--velocity","zero.vdb","--vector",0,0,0)
        self.run_tool("--project","zero.vdb","fog.vdb","initial.vdb","--domain-grid","density")
        self.run_tool("--simulate","fog.vdb","initial.vdb","initial.vdb","state.vdb",
            "--input-grid","density","--velocity-grid","velocity","--domain-grid","fluid",
            "--density-name","rho","--temperature-name","kelvin","--velocity-name","flow",
            "--pressure-name","p","--domain-name","cells")
        source=(self.directory/"state.vdb").read_bytes()
        names=("--density-name","rho","--temperature-name","kelvin","--velocity-name","flow",
               "--pressure-name","p","--domain-name","cells")
        for name,nu in (("identity.vdb",0),("diffused.vdb",.1)):
            report=json.loads(self.run_tool("--viscosity","state.vdb",name,"--time",.1,
                "--kinematic-viscosity",nu,*names).stdout)
            self.assertTrue(report["converged"])
            self.assertEqual(report["skipped"],nu==0)
            self.assertEqual(report["kineticEnergyAfterProjection"],0)
            self.assertEqual(float(self.run_tool("--sample",name,0,0,0,"--input-grid","rho").stdout),1)
            self.assertAlmostEqual(float(self.run_tool("--sample",name,0,0,0,"--input-grid","kelvin").stdout),293.15)
        self.assertEqual((self.directory/"state.vdb").read_bytes(),source)
        common=("--input-grid","rho","--velocity-grid","flow","--domain-grid","cells",
                "--temperature-input","state.vdb","--temperature-grid","kelvin",*names,
                "--kinematic-viscosity",.1,"--source",0,0,0,2,"--density-rate",1,"--heat-rate",2)
        coupled=json.loads(self.run_tool("--simulate","state.vdb","state.vdb","state.vdb","coupled.vdb",
            "--time",.1,"--steps",2,*common).stdout)
        self.assertEqual(coupled["viscositySubsteps"],coupled["actualSubsteps"])
        self.assertEqual(coupled["sourceSubsteps"],coupled["actualSubsteps"])
        self.assertGreater(coupled["sourceChangedCellApplications"],0)
        self.assertGreater(coupled["sourceDensityMassChange"],0)
        self.assertAlmostEqual(coupled["densityMassAfter"],coupled["densityMassBefore"]+coupled["sourceDensityMassChange"],delta=1e-10)
        self.assertAlmostEqual(float(self.run_tool("--sample","coupled.vdb",0,0,0,"--input-grid","rho").stdout),1.1,delta=1e-12)
        sequence=json.loads(self.run_tool("--simulate-sequence","state.vdb","state.vdb","state.vdb","coupled-frames",
            "--time",.05,"--frames",2,*common).stdout)
        self.assertEqual(len(sequence["frames"]),2)
        self.assertTrue(all(frame["viscositySubsteps"]==frame["actualSubsteps"] and
                            frame["sourceSubsteps"]==frame["actualSubsteps"] for frame in sequence["frames"]))
        manifest=json.loads((self.directory/"coupled-frames"/"sequence.json").read_text())
        self.assertTrue(all(frame["sourceDensityMassChange"]>0 for frame in manifest["frames"]))
        self.assertEqual((self.directory/"state.vdb").read_bytes(),source)
        source_report=json.loads(self.run_tool("--fluid-sources","state.vdb","emitted.vdb",*names,
            "--time",.5,"--source",0,0,0,2,"--density-rate",2,"--heat-rate",10,
            "--density-decay",.5,"--temperature-relaxation",1).stdout)
        self.assertTrue(source_report["converged"])
        self.assertGreater(source_report["affectedCells"],0)
        self.assertGreater(source_report["densityMassAfter"],source_report["densityMassBefore"])
        density=float(self.run_tool("--sample","emitted.vdb",0,0,0,"--input-grid","rho").stdout)
        kelvin=float(self.run_tool("--sample","emitted.vdb",0,0,0,"--input-grid","kelvin").stdout)
        self.assertAlmostEqual(density,math.exp(-.25)+4*(-math.expm1(-.25)),delta=1e-12)
        self.assertAlmostEqual(kelvin,293.15+10*(-math.expm1(-.5)),delta=1e-10)
        self.run_tool("--fluid-sources","state.vdb","cleared.vdb",*names,"--time",.25,
            "--source",0,0,0,2,"--clear-density","--density-rate",1)
        self.assertEqual(float(self.run_tool("--sample","cleared.vdb",0,0,0,"--input-grid","rho").stdout),.25)
        self.assertEqual((self.directory/"state.vdb").read_bytes(),source)

    def test_scatter_reproduces_seed_and_preserves_density_input(self):
        self.require_enabled()
        self.fog()
        original = (self.directory / "fog.vdb").read_bytes()
        for output, seed in (("first.vdb", "0"), ("same.vdb", "0"), ("other.vdb", "1")):
            self.run_tool("--scatter", "fog.vdb", output, "--input-grid", "density",
                          "--point-count", "200", "--seed", seed, "--point-radius", ".125",
                          "--voxel-size", ".25", "--max-points", "200")
        first = self.records("first.vdb", "first.json")
        self.assertEqual(first, self.records("same.vdb", "same.json"))
        self.assertNotEqual(first, self.records("other.vdb", "other.json"))
        self.assertEqual(len(first), 200)
        for record in first:
            self.assertEqual(record["velocity"], [0, 0, 0])
            self.assertEqual(record["radius"], .125)
            self.assertLess(max(map(abs, record["position"])), 1.3)
        self.run_tool("--scatter", "fog.vdb", "empty.vdb", "--point-count", "0")
        self.assertEqual(self.records("empty.vdb", "empty.json"), [])
        self.assertEqual((self.directory / "fog.vdb").read_bytes(), original)

    def test_signed_advection_and_velocity_preservation(self):
        self.require_enabled()
        initial = self.points()
        original = (self.directory / "points.vdb").read_bytes()
        self.run_tool("--velocity", "wind.vdb", "--vector", "1", ".5", "0")
        self.run_tool("--advect-points", "points.vdb", "wind.vdb", "moved.vdb", "--time", "-.5",
                      "--steps", "2", "--integration", "rk4", "--keep-velocity", "--voxel-size", ".25")
        actual = self.records("moved.vdb")
        for record, before in zip(actual, initial):
            for value, expected in zip(record["position"],
                                       [before["position"][0] - .5, -.25, 0]):
                self.assertAlmostEqual(value, expected, delta=1e-6)
            self.assertEqual(record["velocity"], before["velocity"])
            self.assertEqual(record["radius"], before["radius"])
        self.assertEqual((self.directory / "points.vdb").read_bytes(), original)

    def test_standard_attribute_sampling_preserves_other_records(self):
        self.require_enabled()
        before = self.points()
        self.run_tool("--export-mesh", "points.vdb", "bad.obj", succeeds=False)
        self.assertFalse((self.directory / "bad.obj").exists())
        self.fog()
        self.run_tool("--velocity", "wind.vdb", "--vector", "2", "3", "4", "--grid-name", "wind")
        self.run_tool("--sample-point-velocity", "points.vdb", "wind.vdb", "with-v.vdb", "--field-grid", "wind")
        self.run_tool("--sample-point-radius", "with-v.vdb", "fog.vdb", "with-radius.vdb", "--field-grid", "density")
        after_v = self.records("with-v.vdb", "v.json")
        after_radius = self.records("with-radius.vdb", "radius.json")
        for initial, velocity, radius in zip(before, after_v, after_radius):
            for actual, expected in zip(radius["position"], initial["position"]):
                self.assertAlmostEqual(actual, expected, delta=1e-6)
            self.assertEqual(velocity["velocity"], [2, 3, 4])
            self.assertEqual(velocity["radius"], initial["radius"])
            self.assertEqual(radius["velocity"], [2, 3, 4])
            self.assertAlmostEqual(radius["radius"], 1, delta=1e-6)

    def test_projection_reduces_divergence_and_reuses_native_mac_output(self):
        self.require_enabled()
        self.fog()
        self.run_tool("--velocity", "wind.vdb", "--vector", "1", ".5", "-.25")
        original = (self.directory / "wind.vdb").read_bytes()
        result = json.loads(self.run_tool("--project", "wind.vdb", "fog.vdb", "projected.vdb",
                                          "--input-grid", "velocity", "--domain-grid", "density",
                                          "--time", ".1", "--tolerance", "1e-6").stdout)
        self.assertTrue(result["converged"])
        self.assertIn("roundoffLimited", result)
        self.assertGreaterEqual(result["divergenceRoundoffBound"], 0)
        self.assertGreater(result["fluidCells"], 0)
        self.assertGreater(result["maxDivergenceBefore"], 0)
        self.assertLessEqual(result["maxDivergenceAfter"], 1e-6 * result["maxDivergenceBefore"])
        report = json.loads(self.run_tool("--inspect", "projected.vdb").stdout)
        grids = {grid["name"]: grid for grid in report["grids"]}
        self.assertEqual(set(grids), {"velocity", "pressure", "fluid"})
        self.assertFalse(grids["velocity"]["inWorldSpace"])
        self.assertEqual(grids["pressure"]["valueType"], "double")
        second = json.loads(self.run_tool("--project", "projected.vdb", "projected.vdb", "second.vdb",
                                          "--input-grid", "velocity", "--domain-grid", "fluid",
                                          "--velocity-name", "flow", "--pressure-name", "p",
                                          "--no-domain").stdout)
        self.assertTrue(second["converged"])
        names = {grid["name"] for grid in json.loads(self.run_tool("--inspect", "second.vdb").stdout)["grids"]}
        self.assertEqual(names, {"flow", "p"})
        self.assertEqual((self.directory / "wind.vdb").read_bytes(), original)

    def test_invalid_domains_types_budgets_and_existing_outputs_are_preserved(self):
        self.require_enabled()
        self.points()
        self.fog()
        self.run_tool("--velocity", "wind.vdb", "--vector", "1", "0", "0")
        (self.directory / "existing.vdb").write_bytes(b"keep existing")
        for arguments in (["--scatter", "fog.vdb", "bad.vdb", "--point-count", "3", "--max-points", "2"],
                          ["--scatter", "fog.vdb", "existing.vdb"],
                          ["--project", "wind.vdb", "sphere.vdb", "bad.vdb"],
                          ["--project", "wind.vdb", "fog.vdb", "bad.vdb", "--max-fluid-cells", "1"],
                          ["--project", "wind.vdb", "fog.vdb", "bad.vdb", "--max-input-bytes", "1"],
                          ["--sample-point-radius", "points.vdb", "wind.vdb", "bad.vdb"],
                          ["--advect-points", "points.vdb", "wind.vdb", "bad.vdb", "--time", "3600"]):
            self.run_tool(*arguments, succeeds=False)
            self.assertFalse((self.directory / "bad.vdb").exists())
        self.assertEqual((self.directory / "existing.vdb").read_bytes(), b"keep existing")

    def test_coupled_smoke_state_diagnostics_and_restart(self):
        self.require_enabled()
        self.fog()
        self.run_tool("--velocity", "zero.vdb", "--vector", "0", "0", "0")
        self.run_tool("--project", "zero.vdb", "fog.vdb", "prepared.vdb", "--domain-grid", "density")
        before = (self.directory / "prepared.vdb").read_bytes()
        report = json.loads(self.run_tool("--simulate", "fog.vdb", "prepared.vdb", "prepared.vdb", "state.vdb",
                                          "--input-grid", "density", "--velocity-grid", "velocity", "--domain-grid", "fluid",
                                          "--time", ".1", "--steps", "2").stdout)
        self.assertTrue(report["converged"])
        self.assertEqual(report["actualSubsteps"], 2)
        self.assertAlmostEqual(report["simulatedTime"], .1)
        self.assertAlmostEqual(report["densityMassAfter"], report["densityMassBefore"], delta=1e-10)
        self.assertEqual(report["maxDivergenceAfter"], 0)
        names = {grid["name"]: grid for grid in json.loads(self.run_tool("--inspect", "state.vdb").stdout)["grids"]}
        self.assertEqual(set(names), {"density", "velocity", "pressure", "temperature", "fluid"})
        self.assertEqual(names["density"]["valueType"], "double")
        self.assertAlmostEqual(float(self.run_tool("--sample", "state.vdb", "0", "0", "0",
                                                  "--input-grid", "temperature").stdout), 293.15)
        second = json.loads(self.run_tool("--simulate", "state.vdb", "state.vdb", "state.vdb", "state2.vdb",
                                          "--temperature-input", "state.vdb", "--input-grid", "density",
                                          "--velocity-grid", "velocity", "--domain-grid", "fluid",
                                          "--temperature-grid", "temperature", "--time", ".025").stdout)
        self.assertTrue(second["converged"])
        self.assertAlmostEqual(second["densityMassBefore"], report["densityMassAfter"], delta=1e-10)
        self.assertEqual((self.directory / "prepared.vdb").read_bytes(), before)
        self.run_tool("--simulate", "state.vdb", "state.vdb", "state.vdb", "bad.vdb", "--input-grid", "density",
                      "--velocity-grid", "velocity", "--domain-grid", "fluid", "--steps", "2", "--max-substeps", "1",
                      succeeds=False)
        self.assertFalse((self.directory / "bad.vdb").exists())


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--tool", required=True, type=pathlib.Path)
    parser.add_argument("--enabled", required=True, choices=("0", "1"))
    args, remaining = parser.parse_known_args()
    OpenVdbPointFlowCli.tool = args.tool.resolve()
    OpenVdbPointFlowCli.enabled = args.enabled == "1"
    unittest.main(argv=[__file__, *remaining])
