"""CPU failure guards for deterministic joint capture fixtures and reports."""
import copy
import importlib.util
import json
import math
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

import physics_joint_regression as runner


def synthetic(directory, kind="hinge", provider="primary"):
    import numpy as np
    node = 3 if provider == "primary" else 6

    def encoded(matrix):
        return matrix.T.reshape(-1).tolist()

    def pose(coordinate):
        result = np.eye(4)
        if kind == "hinge":
            c, s = math.cos(coordinate), math.sin(coordinate)
            result[:3, :3] = [[c, -s, 0], [s, c, 0], [0, 0, 1]]
        result[:3, 3] = [coordinate if kind == "slider" else 0, 3, 0]
        return result

    initial = pose(0)
    record = {"joint": kind, "provider": provider, "technique": "deferred-raster",
              "mode": "raster", "validationErrors": [], "samples": []}
    limit = .5 if kind == "hinge" else .75 if kind == "slider" else 0
    for frame in runner.SAMPLES:
        coordinate = limit if frame in (120, 300) else -limit if frame == 240 else 0
        visual = pose(coordinate)
        scene = visual.copy()
        if provider == "primary":
            scene[:3, :3] = scene[:3, :3] @ np.diag([3, 1, 1])
        anchor_visual = np.diag([.4, .4, .4, 1])
        anchor_visual[:3, 3] = [0, 4, 0]
        anchor = {"node": 1, "motion": 0, "position": [0, 4, 0], "sceneWorldMatrix": encoded(anchor_visual),
                  "renderWorldMatrix": encoded(anchor_visual)}
        moving = {"node": node, "motion": 2, "position": visual[:3, 3].tolist(),
                  "sceneWorldMatrix": encoded(scene), "renderWorldMatrix": encoded(scene) if provider == "primary" else []}
        bodies = [] if frame == 304 else [anchor] if frame in (1, 302) else [anchor, moving]
        active = frame in (3, 120, 240, 242, 246, 248, 300)
        joint = {"id": 11 if frame >= 246 else 10, "type": runner.TYPES[kind],
                 "nodeA": 1, "nodeB": node, "coordinate": coordinate,
                 "settings": {"motor": {"targetVelocity": -1 if frame == 240 else 1}}}
        state = {"available": True, "error": "", "droppedSeconds": 0,
                 "bodyCount": len(bodies), "bodies": bodies, "jointCount": 1 if active else 0,
                 "joints": [joint] if active else [], "playing": frame in (3, 120, 240, 300), "auxiliaryElements": []}
        if provider == "bim" and len(bodies) == 2:
            delta = visual @ np.linalg.inv(initial)
            # Renderer import rotation from DotBIM's +Z up, +Y forward basis.
            base = np.array([[1, 0, 0, 0], [0, 0, 1, 0], [0, -1, 0, 0], [0, 0, 0, 1]])
            model = delta @ base
            normal = np.zeros((4, 3))
            normal[:3, :] = np.linalg.inv(model[:3, :3]).T
            objects = []
            for index, center_x in enumerate((-1, 1)):
                corners = np.array([[x, y, z, 1] for x in (center_x - .5, center_x + .5)
                                    for y in (2.5, 3.5) for z in (-.5, .5)])
                transformed = (delta @ corners.T).T[:, :3]
                objects.append({"objectIndex": index, "guid": runner.PRODUCT_GUID, "sourceId": f"fragment{index}",
                    "model": encoded(model), "normalMatrix": encoded(normal),
                    "boundsMin": transformed.min(axis=0).tolist(), "boundsMax": transformed.max(axis=0).tolist()})
            state["auxiliaryElements"] = [{"node": node, "productId": 1, "objects": objects}]
        png = Path(directory) / f"{kind}-{provider}-{frame}.png"
        png.write_bytes(str(frame).encode())
        record["samples"].append({"frame": frame, "png": str(png), "telemetry": {
            "physics": state, "rayShadows": {"active": True}, "taa": {"enabled": True}}})
    return record


class JointFixtureTests(unittest.TestCase):
    def test_fixtures_have_stable_primary_endpoint_nodes(self):
        with tempfile.TemporaryDirectory() as temporary:
            for provider in ("primary", "bim"):
                source, auxiliary = runner.fixtures(Path(temporary) / provider, provider)
                gltf = json.loads(source.read_text())
                self.assertEqual(gltf["nodes"][0]["translation"], [0, 4, 0])
                self.assertEqual(len(gltf["nodes"]), 3 if provider == "primary" else 2)
                self.assertEqual(bool(auxiliary), provider == "bim")

    def test_sequence_preserves_joint_through_reset_and_body_replacement(self):
        for kind in runner.TYPES:
            for provider in ("primary", "bim"):
                sequence = runner.sequence(kind, provider)
                events = {event["frame"]: event for event in sequence["events"]}
                self.assertEqual(events[3]["physicsJointCreate"]["bodyA"], {"node": 1})
                self.assertEqual(events[3]["physicsJointCreate"]["bodyB"],
                                 {"node": 3} if provider == "primary" else {"bimObject": 0})
                self.assertEqual(events[241]["physicsAction"], "reset")
                self.assertEqual(events[243]["physicsJointAction"], "remove")
                self.assertEqual(events[247]["physicsAttach"], "dynamic")
                self.assertEqual(events[301]["physicsAction"], "detach")
                self.assertEqual(sorted(events), list(events))
                if kind != "fixed":
                    self.assertLess(events[121]["physicsJointMotorVelocity"], 0)
                    self.assertGreater(events[249]["physicsJointMotorVelocity"], 0)

    def test_opt_in_skip_and_help_require_only_standard_library(self):
        with tempfile.TemporaryDirectory() as temporary:
            output = Path(temporary) / "not-created"
            environment = dict(os.environ)
            environment.pop("CONTAINER_RUN_GPU_PHYSICS", None)
            for args, expected in ((["--output", str(output), "--require-opt-in"], 77), (["--help"], 0)):
                result = subprocess.run([sys.executable, "-S", "-B", str(Path(runner.__file__)), *args],
                                        capture_output=True, text=True, env=environment)
                self.assertEqual(result.returncode, expected, result.stderr)
            self.assertFalse(output.exists())


class JointCheckerTests(unittest.TestCase):
    def setUp(self):
        if importlib.util.find_spec("numpy") is None:
            self.skipTest("NumPy required for matrix assertions")
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.record = synthetic(self.temporary.name)

    def state(self, frame):
        return next(sample for sample in self.record["samples"] if sample["frame"] == frame)["telemetry"]["physics"]

    def test_all_joint_types_and_body_providers_pass(self):
        for kind in runner.TYPES:
            for provider in ("primary", "bim"):
                with self.subTest(kind=kind, provider=provider):
                    self.assertTrue(runner.check(synthetic(self.temporary.name, kind, provider))["passed"])

    def test_motor_reversal_missing_fails(self):
        first = copy.deepcopy(self.state(120))
        self.state(240).update(first)
        with self.assertRaisesRegex(AssertionError, "Motor reversal"):
            runner.check(self.record)

    def test_rendered_hinge_motion_beyond_limit_fails(self):
        import numpy as np
        body = self.state(120)["bodies"][1]
        c, s = math.cos(.7), math.sin(.7)
        model = np.eye(4)
        model[:3, :3] = np.array([[c, -s, 0], [s, c, 0], [0, 0, 1]]) @ np.diag([3, 1, 1])
        model[:3, 3] = [0, 3, 0]
        body["sceneWorldMatrix"] = body["renderWorldMatrix"] = model.T.reshape(-1).tolist()
        self.state(120)["joints"][0]["coordinate"] = .7
        with self.assertRaisesRegex(AssertionError, "exceeded its motion limits"):
            runner.check(self.record)

    def test_body_replacement_lost_joint_fails(self):
        self.state(248)["jointCount"] = 0
        self.state(248)["joints"] = []
        with self.assertRaisesRegex(AssertionError, "Joint absent"):
            runner.check(self.record)

    def test_reset_recycled_scene_joint_identity_fails(self):
        self.state(242)["joints"][0]["id"] = 999
        with self.assertRaisesRegex(AssertionError, "Reset changed"):
            runner.check(self.record)

    def test_endpoint_detach_leaves_no_constraint(self):
        self.state(302)["jointCount"] = 1
        with self.assertRaisesRegex(AssertionError, "dangling constraint"):
            runner.check(self.record)

    def test_stale_primary_render_matrix_fails(self):
        self.state(120)["bodies"][1]["renderWorldMatrix"] = self.state(3)["bodies"][1]["renderWorldMatrix"]
        with self.assertRaisesRegex(AssertionError, "primary mesh transform is stale"):
            runner.check(self.record)

    def test_bim_rotation_updates_normal_transform(self):
        self.record = synthetic(self.temporary.name, provider="bim")
        self.state(120)["auxiliaryElements"][0]["objects"][0]["normalMatrix"] = \
            self.state(3)["auxiliaryElements"][0]["objects"][0]["normalMatrix"]
        with self.assertRaisesRegex(AssertionError, "stale BIM normals"):
            runner.check(self.record)

    def test_bim_rotation_updates_bounds(self):
        self.record = synthetic(self.temporary.name, provider="bim")
        self.state(120)["auxiliaryElements"][0]["objects"][0]["boundsMax"] = \
            self.state(3)["auxiliaryElements"][0]["objects"][0]["boundsMax"]
        with self.assertRaisesRegex(AssertionError, "stale BIM maximum bounds"):
            runner.check(self.record)


if __name__ == "__main__":
    unittest.main()
