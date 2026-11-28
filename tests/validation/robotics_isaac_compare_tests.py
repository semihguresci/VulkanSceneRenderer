"""Validate native fixture generation, portable bundle integrity and OpenUSD composition."""

import argparse
import copy
import hashlib
import importlib.util
import json
from pathlib import Path
import shutil
import subprocess
import sys
from tempfile import TemporaryDirectory
from types import SimpleNamespace
import unittest
from unittest.mock import patch

from robotics_isaac_compare import load_bundle, verify_dependencies, verify_robot, verify_stage
from robotics_isaac_smoke import check_sdk_process


NATIVE_TOOLS = None

SYNTHETIC_SCENE = '''#usda 1.0
(
    defaultPrim = "World"
    metersPerUnit = 1
    upAxis = "Z"
)
def Xform "World"
{
    custom string robotics:sourceNamespace = "usd-corner-check"
    custom string[] robotics:diagnostics = ["unsupported-semantic-class [#3]"]
    def PhysicsScene "PhysicsScene" {}
}
'''


def write_json(path, data):
    path.write_text(json.dumps(data, allow_nan=False), encoding="utf-8")


def fingerprint(path):
    payload = path.read_bytes()
    return {"bytes": len(payload), "sha256": hashlib.sha256(payload).hexdigest()}


def manufacture_bundle(directory):
    """Produce small synthetic receipts for protocol tests only."""
    directory.mkdir()
    source = directory / "lidar-corner.ifc"
    source.write_bytes(b"synthetic fixture source")
    scenario = {"version": 1, "robotId": "isaac-corner-smoke", "ticksPerSecond": 60, "ticks": 90,
                "seed": 221, "initialFeet": [0., 0., 0.], "initialYaw": 0., "commands": [
                    {"tick": 0, "linear": .2, "angular": 0.},
                    {"tick": 30, "linear": 0., "angular": .7},
                    {"tick": 75, "linear": 0., "angular": 0.},
                ]}
    write_json(directory / "scenario.json", scenario)
    history = []
    for tick in range(91):
        position = [.1 * min(30, tick) / 30, 0., 0.]
        yaw = .525 * min(45, max(0, tick - 30)) / 45
        history.append({"tick": tick, "seconds": tick / 60, "base": {"position": position, "yaw": yaw},
                        "obstruction": "none", "blockingEntityId": ""})
    simulation = {"schema": "container.bim-robotics-simulation", "version": 1, "sourceNamespace": "usd-corner-check",
                  "coordinateConvention": "ROS REP-103", "ticksPerSecond": 60, "robotId": "isaac-corner-smoke",
                  "seed": 221, "state": history[-1], "history": history,
                  "sceneSource": {"metersPerUnscaledRendererUnit": 1., "layers": [
                      {"byteSize": source.stat().st_size, "sha256": fingerprint(source)["sha256"]},
                  ]}}
    write_json(directory / "simulation.json", simulation)
    results = []
    for endpoint, hit_id, hit_point, normal, fraction in (
            ([8, 0, 1], 27, [4, 0, 1], [-1, 0, 0], .5),
            ([0, 8, 1], 37, [0, 3, 1], [0, -1, 0], .375),
            ([0, 0, -7], 17, [0, 0, 0], [0, 0, 1], .125),
            ([-8, 0, 1], None, None, None, None)):
        hit = None if hit_id is None else {"entityId": f"bim/16:usd-corner-check/guid/22:{hit_id:022d}",
                                           "position": hit_point, "normal": normal, "fraction": fraction,
                                           "walkableSupport": hit_id == 17}
        results.append({"from": [0, 0, 1], "to": endpoint, "hit": hit})
    write_json(directory / "collision.json", {"schema": "container.bim-collision-report", "version": 1,
              "coordinates": "ROS REP-103, metres", "sourceNamespace": "usd-corner-check", "raycasts": results})
    write_json(directory / "queries.json", {"raycasts": [{"from": row["from"], "to": row["to"]} for row in results]})
    (directory / "scene.usda").write_text(SYNTHETIC_SCENE, encoding="utf-8")
    sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools"))
    try:
        from bim_robotics_replay_usd import write_replay_stage
    finally:
        sys.path.pop(0)
    write_replay_stage(directory / "replay.usda", "scene.usda", simulation)
    for name in ("simulator.log", "collision.log"):
        (directory / name).write_text("Synthetic receipts for unit testing only.\n", encoding="utf-8")
    manifest = {"schema": "container.bim-robotics-isaac-smoke", "version": 1, "sourceNamespace": "usd-corner-check",
                "ticksPerSecond": 60, "witnessTicks": [0, 30, 60, 90],
                "provenance": {"rendererImportScale": 2.5,
                    "authoredFixture": {"name": source.name, **fingerprint(source)}, "authoredScenario": {
                    "name": "lidar-corner-isaac-smoke.json", **fingerprint(directory / "scenario.json")}},
                "motion": {"translatedMetres": .1, "turnedRadians": .35}, "files": {
                    path.name: fingerprint(path) for path in directory.iterdir()}}
    write_json(directory / "bundle.json", manifest)
    return manifest, simulation


class BundleWorkspace(unittest.TestCase):
    def setUp(self):
        temporary = TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.directory = Path(temporary.name).resolve() / "original"
        self.manifest, self.simulation = manufacture_bundle(self.directory)

    def refresh(self, name):
        self.manifest["files"][name] = fingerprint(self.directory / name)
        write_json(self.directory / "bundle.json", self.manifest)


class BundleIntegrityTests(BundleWorkspace):
    def test_verify_relocated_native_receipts_without_pxr(self):
        relocated = self.directory.with_name("relocated replay with spaces")
        shutil.move(self.directory, relocated)
        bundle = load_bundle(relocated)
        self.assertEqual(bundle["stage"], relocated / "replay.usda")
        self.assertEqual(len(bundle["samples"]), 91)
        self.assertEqual(bundle["witnessTicks"], [0, 30, 60, 90])
        self.assertAlmostEqual(bundle["samples"][60]["yawRadians"], .35)
        self.assertEqual([row["distanceMeters"] for row in bundle["rays"]], [4., 3., 1., None])

    def test_unsafe_manifest_path_rejected(self):
        self.manifest["files"]["../scene.usda"] = self.manifest["files"].pop("scene.usda")
        write_json(self.directory / "bundle.json", self.manifest)
        with self.assertRaisesRegex(RuntimeError, "unexpected payload paths"):
            load_bundle(self.directory)

    def test_bundle_requires_original_ifc_fingerprint(self):
        del self.manifest["provenance"]["authoredFixture"]
        write_json(self.directory / "bundle.json", self.manifest)
        with self.assertRaisesRegex(RuntimeError, "missing the original IFC fingerprint"):
            load_bundle(self.directory)

    def test_bad_hash_and_missing_layer_rejected(self):
        layer = self.directory / "scene.usda"
        layer.write_bytes(b"!" + layer.read_bytes()[1:])
        with self.assertRaisesRegex(RuntimeError, "scene.usda SHA-256"):
            load_bundle(self.directory)
        layer.unlink()
        with self.assertRaisesRegex(RuntimeError, "Missing bundle file scene.usda"):
            load_bundle(self.directory)

    def test_rehashed_native_pose_still_requires_numeric_analytic_witness(self):
        for replacement, message in ((False, "must be a finite number"), (.9, "expected")):
            with self.subTest(replacement=replacement):
                simulation = copy.deepcopy(self.simulation)
                simulation["history"][59]["base"]["yaw"] = replacement
                write_json(self.directory / "simulation.json", simulation)
                self.refresh("simulation.json")
                with self.assertRaisesRegex(RuntimeError, f"Native yaw at tick 59.*{message}"):
                    load_bundle(self.directory)

    def test_rehashed_collision_hit_must_name_actual_fixture_wall(self):
        collision_path = self.directory / "collision.json"
        collision = json.loads(collision_path.read_bytes())
        collision["raycasts"][0]["hit"]["entityId"] = collision["raycasts"][1]["hit"]["entityId"]
        write_json(collision_path, collision)
        self.refresh("collision.json")
        with self.assertRaisesRegex(RuntimeError, "wall-x native ray did not hit the fixture entity #27"):
            load_bundle(self.directory)


@unittest.skipUnless(importlib.util.find_spec("pxr"), "OpenUSD required for stage composition controls")
class StageCompositionTests(BundleWorkspace):
    def test_expected_layers_and_all_authored_robot_samples(self):
        from pxr import Usd

        bundle = load_bundle(self.directory)
        verify_dependencies(bundle)
        verify_robot(Usd.Stage.Open(str(bundle["stage"])), bundle)

    def test_rehashed_replay_rejects_external_layer(self):
        layer = self.directory / "replay.usda"
        replay = layer.read_text(encoding="utf-8")
        layer.write_text(replay.replace("@scene.usda@", "@../scene.usda@"), encoding="utf-8")
        self.refresh("replay.usda")
        with self.assertRaisesRegex(RuntimeError, "replay.usda.*sublayers must be exactly"):
            verify_dependencies(load_bundle(self.directory))

    def test_rehashed_scene_rejects_external_asset(self):
        layer = self.directory / "scene.usda"
        scene = layer.read_text(encoding="utf-8")
        layer.write_text(scene.replace('def PhysicsScene', 'asset texture = @external.png@\n    def PhysicsScene'),
                         encoding="utf-8")
        self.refresh("scene.usda")
        with self.assertRaisesRegex(RuntimeError, "scene.usda.*unexpected external dependencies"):
            verify_dependencies(load_bundle(self.directory))

    def test_rehashed_replay_rejects_corrupt_authored_pose(self):
        from pxr import Usd

        layer = self.directory / "replay.usda"
        replay = layer.read_text(encoding="utf-8")
        before = next(line for line in replay.splitlines() if line.lstrip().startswith("29: ("))
        layer.write_text(replay.replace(before, "            29: (5.0, 0.0, 0.0),"), encoding="utf-8")
        self.refresh("replay.usda")
        bundle = load_bundle(self.directory)
        with self.assertRaisesRegex(RuntimeError, "robot translation at tick 29"):
            verify_robot(Usd.Stage.Open(str(bundle["stage"])), bundle)

    def test_composed_stage_rejects_corrupt_guid(self):
        from pxr import Usd

        layer = self.directory / "scene.usda"
        scene = layer.read_text(encoding="utf-8")
        layer.write_text(scene.replace('def PhysicsScene', '''def Xform "Wall"
    {
        custom string ifc:guid = "incorrect-guid"
        custom string ifc:sourceId = "#27"
    }
    def PhysicsScene'''), encoding="utf-8")
        self.refresh("scene.usda")
        bundle = load_bundle(self.directory)
        with self.assertRaisesRegex(RuntimeError, "incorrect identity #27 / incorrect-guid"):
            verify_stage(Usd.Stage.Open(str(bundle["stage"])), bundle)


class NativeFixtureTests(unittest.TestCase):
    def test_prepare_and_relocate_bundle(self):
        if NATIVE_TOOLS is None:
            self.skipTest("native tools were not supplied")
        from robotics_isaac_fixture import prepare_bundle

        with TemporaryDirectory() as directory:
            prepared = Path(directory) / "prepared"
            prepared.mkdir()
            manifest = prepare_bundle(*NATIVE_TOOLS, prepared)
            moved = prepared.with_name("relocated bundle")
            prepared.rename(moved)
            bundle = load_bundle(moved)
            self.assertEqual(len(bundle["samples"]), 91)
            self.assertEqual(bundle["witnessTicks"], [0, 30, 60, 90])
            self.assertEqual([ray["sourceId"] for ray in bundle["rays"]], [27, 37, 17, None])
            self.assertEqual(bundle["stage"], moved / "replay.usda")
            self.assertEqual(manifest["provenance"]["rendererImportScale"], 2.5)
            if importlib.util.find_spec("pxr") is not None:
                from pxr import Usd

                verify_dependencies(bundle)
                verify_stage(Usd.Stage.Open(str(bundle["stage"])), bundle)


class SdkProcessTests(unittest.TestCase):
    def test_kit_shutdown_exit_status_reaches_worker(self):
        stub = """\
from robotics_isaac_sdk import _close_app

class KitApp:
    def close(self, *, exit_code):
        assert exit_code == 0
        raise SystemExit(19)

_close_app(KitApp(), {"passed": True})
"""
        worker = subprocess.run([sys.executable, "-B", "-c", stub],
                                cwd=Path(__file__).resolve().parent,
                                capture_output=True, text=True, timeout=10)
        self.assertEqual(worker.returncode, 19, worker.stderr)

    def test_sdk_process_requires_exit_success_and_matching_receipt(self):
        with TemporaryDirectory() as directory:
            output = Path(directory)
            with patch("robotics_isaac_smoke.subprocess.run") as run:
                run.return_value = SimpleNamespace(returncode=0)
                with self.assertRaisesRegex(RuntimeError, "did not write its completion receipt"):
                    check_sdk_process(output, output, "isaac-sim", 900)
                receipt = output / "sdk" / "sdk-report.json"
                receipt.parent.mkdir()
                receipt.write_text("[]", encoding="utf-8")
                with self.assertRaisesRegex(RuntimeError, "did not record a passed comparison"):
                    check_sdk_process(output, output, "isaac-sim", 900)
                passed = {"backend": "isaac-sim", "status": "passed", "passed": True}
                receipt.write_text(json.dumps(passed), encoding="utf-8")
                with self.assertRaisesRegex(RuntimeError, "did not record a passed comparison"):
                    check_sdk_process(output, output, "isaac-lab", 900)
                self.assertEqual(check_sdk_process(output, output, "isaac-sim", 900), passed)
                run.return_value = SimpleNamespace(returncode=1)
                with self.assertRaisesRegex(RuntimeError, "exited with code 1"):
                    check_sdk_process(output, output, "isaac-sim", 900)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(add_help=False)
    parser.add_argument("--native-tools", nargs=3, type=Path, metavar=("EXPORTER", "SIMULATOR", "COLLISION"))
    options, remaining = parser.parse_known_args()
    NATIVE_TOOLS = options.native_tools
    unittest.main(argv=[sys.argv[0], *remaining])
