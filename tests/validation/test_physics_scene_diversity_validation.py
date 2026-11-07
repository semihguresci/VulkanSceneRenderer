"""CPU checks for independent geometry and diverse physics scene assertions."""
import copy
import importlib.util
import json
import math
import os
from pathlib import Path
import subprocess
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest import mock

import physics_scene_diversity_regression as runner


def synthetic_kinematic_contact(directory, sequence, fixture):
    import numpy as np
    resize = fixture["case"] == "kinematic-resize-contact"
    samples = []
    for frame in sequence["sampleFrames"]:
        bodies, elements = [], []
        primary_live = frame < 11 and (resize or frame >= 2)
        auxiliary_live = frame < 9 and (not resize or frame >= 2)
        pushed_x = fixture["contactCenter"] if frame == 4 else fixture["contactCenter"] + 7 if frame == 6 else 0
        support_x = -2 if frame < 4 else 2
        support_id = 11 if frame < 4 else 12 if frame < 7 else 13
        pushed_id = 21 if frame < 7 else 22
        if primary_live:
            model = runner.translation([support_x if resize else pushed_x, 0, 0])
            if resize and frame >= 4:
                model[:3, :3] *= 1.2
            bodies.append({"node": fixture["primaryNode"], "id": support_id if resize else pushed_id,
                "motion": 1 if resize else 2, "position": model[:3, 3].tolist(),
                "rotation": [0, 0, 0, 1],
                "sceneWorldMatrix": runner.encode(model), "renderWorldMatrix": runner.encode(model),
                "settings": {"gravityFactor": 0, "friction": 0}})
        if auxiliary_live:
            x = pushed_x if resize else support_x
            model = runner.translation([x if resize else x + 2, 0, 0]) @ runner.matrix(runner.BIM_ROTATION)
            proxy = runner.translation([x, 0, 0])
            normal = np.zeros((4, 3))
            normal[:3, :] = np.linalg.inv(model[:3, :3]).T
            spec = fixture["bimGeometry"]["0"]
            positions = runner.transform_vertices(model, spec["vertices"])
            bodies.append({"node": 50, "id": pushed_id if resize else support_id,
                "motion": 2 if resize else 1, "position": [x, 0, 0],
                "rotation": [0, 0, 0, 1],
                "sceneWorldMatrix": runner.encode(proxy), "renderWorldMatrix": [],
                "settings": {"gravityFactor": 0, "friction": .25 if not resize and frame < 4 else 0}})
            elements.append({"node": 50, "productId": 3, "objects": [{"objectIndex": 0,
                "guid": spec["guid"], "sourceId": "", "model": runner.encode(model),
                "normalMatrix": runner.encode(normal), "boundsMin": positions.min(axis=0).tolist(),
                "boundsMax": positions.max(axis=0).tolist()}]})
        playing = 2 <= frame < 7
        state = {"available": True, "error": "", "droppedSeconds": 0, "bodies": bodies,
            "bodyCount": len(bodies), "auxiliaryElements": elements, "joints": [], "jointCount": 0,
            "playing": playing, "substeps": 4 if playing else 0,
            "totalSteps": 4 * (frame - 1) if frame < 7 else 0}
        scene = {"primaryObjectCount": int(frame < 15), "auxiliaryObjectCount": int(frame < 15)}
        png = Path(directory) / f"{fixture['case']}-{frame}.png"
        png.write_bytes(f"frame {frame}".encode())
        samples.append({"frame": frame, "png": str(png), "telemetry": {"physics": state,
            "scene": scene, "rayShadows": {"active": True}, "taa": {"enabled": True}}})
    return {"technique": "deferred-raster", "mode": "raster", "fixture": fixture,
        "sampleFrames": sequence["sampleFrames"], "validationErrors": [], "samples": samples}


def synthetic_dynamic_support_teleport(directory, sequence, fixture):
    import numpy as np
    samples = []
    for frame in sequence["sampleFrames"]:
        bodies, elements = [], []
        if frame < 225:
            for node in (fixture["floorNode"], fixture["supportNode"]):
                if node == fixture["supportNode"] and frame >= 219:
                    continue
                model = runner.matrix(fixture["primaryGeometry"][str(node)]["authoredModel"])
                if node == fixture["supportNode"] and frame >= 182:
                    model[0, 3] += 10
                bodies.append({"node": node, "id": node + (100 if frame >= 211 else 1),
                    "motion": 0 if node == fixture["floorNode"] else 2, "position": model[:3, 3].tolist(),
                    "rotation": [0, 0, 0, 1], "sceneWorldMatrix": runner.encode(model),
                    "renderWorldMatrix": runner.encode(model), "settings": {"gravityFactor": 1, "friction": .5}})
            if frame < 215:
                height = {3: 1.499, 180: 1.5, 182: 1.5, 184: 1.49, 210: .5, 214: 1.5}[frame]
                proxy = runner.translation([0, height, 0])
                model = runner.translation([0, height - 1.5, 0]) @ runner.matrix(runner.BIM_ROTATION)
                normal = np.zeros((4, 3))
                normal[:3, :] = np.linalg.inv(model[:3, :3]).T
                spec = fixture["bimGeometry"]["0"]
                positions = runner.transform_vertices(model, spec["vertices"])
                bodies.append({"node": 50, "id": 150 if frame >= 211 else 50, "motion": 2,
                    "position": [0, height, 0], "rotation": [0, 0, 0, 1], "sceneWorldMatrix": runner.encode(proxy),
                    "renderWorldMatrix": [], "settings": {"gravityFactor": 1, "friction": .5}})
                elements.append({"node": 50, "productId": 3, "objects": [{"objectIndex": 0,
                    "guid": spec["guid"], "sourceId": "", "model": runner.encode(model),
                    "normalMatrix": runner.encode(normal), "boundsMin": positions.min(axis=0).tolist(),
                    "boundsMax": positions.max(axis=0).tolist()}]})
        playing = frame <= 180 or 183 <= frame <= 210
        steps = frame - 2 if frame <= 180 else 178 if frame <= 182 else frame - 4 if frame <= 210 else 0
        state = {"available": True, "error": "", "droppedSeconds": 0, "bodies": bodies,
            "bodyCount": len(bodies), "activeBodyCount": 0 if frame == 180 or frame >= 215 else 2,
            "auxiliaryElements": elements, "joints": [], "jointCount": 0,
            "playing": playing, "substeps": int(playing), "totalSteps": steps}
        png = Path(directory) / f"dynamic-support-teleport-{frame}.png"
        png.write_bytes(f"frame {frame}".encode())
        samples.append({"frame": frame, "png": str(png), "telemetry": {"physics": state,
            "scene": {"primaryObjectCount": 2 if frame < 225 else 0, "auxiliaryObjectCount": int(frame < 225)},
            "rayShadows": {"active": True}, "taa": {"enabled": True}}})
    return {"technique": "deferred-raster", "mode": "raster", "fixture": fixture,
        "sampleFrames": sequence["sampleFrames"], "validationErrors": [], "samples": samples}


def synthetic_contact_wake_chain(directory, sequence, fixture):
    import numpy as np
    samples = []
    nodes = fixture["chainNodes"]
    for frame in sequence["sampleFrames"]:
        bodies, elements, joints = [], [], []
        if frame < 225:
            ticks = 2 if frame == 184 else 28 if frame == 210 else 0
            drop = -9.81 / 60**2 * ticks * (ticks + 1) / 2
            for index, node in enumerate(nodes):
                if frame >= 215 and index == 1:
                    continue
                center = [0, fixture["chainHeights"][index] + drop, 0]
                pose = runner.translation(center)
                bodies.append({"node": node, "id": node + (1000 if frame >= 211 else 100),
                    "motion": 2, "position": center, "rotation": [0, 0, 0, 1],
                    "sceneWorldMatrix": runner.encode(pose), "renderWorldMatrix": runner.encode(pose),
                    "settings": {"gravityFactor": 1}})
            if frame < 219:
                shift = 0 if frame < 182 else 10
                pose = runner.translation([shift, -.5, 0])
                model = runner.translation([shift, 0, 0]) @ runner.matrix(runner.BIM_ROTATION)
                normal = np.zeros((4, 3))
                normal[:3, :] = np.linalg.inv(model[:3, :3]).T
                spec = fixture["bimGeometry"]["0"]
                positions = runner.transform_vertices(model, spec["vertices"])
                bodies.append({"node": 50, "id": 1500 if frame >= 211 else 500, "motion": 0,
                    "position": [shift, -.5, 0], "rotation": [0, 0, 0, 1],
                    "sceneWorldMatrix": runner.encode(pose), "renderWorldMatrix": [], "settings": {}})
                elements.append({"node": 50, "productId": 7, "objects": [{"objectIndex": 0,
                    "guid": spec["guid"], "sourceId": "", "model": runner.encode(model),
                    "normalMatrix": runner.encode(normal), "boundsMin": positions.min(axis=0).tolist(),
                    "boundsMax": positions.max(axis=0).tolist()}]})
            if frame < 215:
                joints = [{"id": index + 7, "type": "fixed", "nodeA": a, "nodeB": b,
                    "settings": {"anchor": [0, (fixture["chainHeights"][index] + fixture["chainHeights"][index + 1]) / 2, 0]}}
                    for index, (a, b) in enumerate(zip(nodes, nodes[1:]))]
        playing = frame <= 180 or 183 <= frame <= 210
        steps = frame - 6 if frame <= 180 else 174 if frame <= 182 else frame - 8 if frame <= 210 else 0
        state = {"available": True, "error": "", "droppedSeconds": 0, "bodies": bodies,
            "bodyCount": len(bodies), "activeBodyCount": 0 if frame == 180 or frame >= 225 else 3,
            "auxiliaryElements": elements, "joints": joints, "jointCount": len(joints),
            "playing": playing, "substeps": int(playing), "totalSteps": steps}
        png = Path(directory) / f"contact-wake-chain-{frame}.png"
        png.write_bytes(f"frame {frame}".encode())
        samples.append({"frame": frame, "png": str(png), "telemetry": {"physics": state,
            "scene": {"primaryObjectCount": 3 if frame < 225 else 0, "auxiliaryObjectCount": int(frame < 225)},
            "rayShadows": {"active": True}, "taa": {"enabled": True}}})
    return {"technique": "deferred-raster", "mode": "raster", "fixture": fixture,
        "sampleFrames": sequence["sampleFrames"], "validationErrors": [], "samples": samples}


def synthetic(directory, case, ccd_speed=500):
    import numpy as np
    _, _, sequence, fixture = runner.fixture(case, Path(directory) / case, ccd_speed=ccd_speed)
    if case in runner.KINEMATIC_CONTACT_CASES:
        return synthetic_kinematic_contact(directory, sequence, fixture)
    if case == "dynamic-support-teleport":
        return synthetic_dynamic_support_teleport(directory, sequence, fixture)
    if case == "contact-wake-chain":
        return synthetic_contact_wake_chain(directory, sequence, fixture)
    final = sequence["frames"]
    sample_frames = sequence["sampleFrames"]
    dynamic = fixture.get("dynamic")
    base = runner.matrix(runner.BIM_ROTATION)
    record = {"technique": "deferred-raster", "mode": "raster", "fixture": fixture,
              "sampleFrames": sample_frames, "validationErrors": [], "samples": []}

    def primary(node, model, motion, settings=None):
        spec = fixture["primaryGeometry"][str(node)]
        vertices = np.asarray(spec["vertices"])
        center = (vertices.min(axis=0) + vertices.max(axis=0)) / 2
        return {"node": node, "motion": motion, "position": (model @ np.append(center, 1))[:3].tolist(),
                "sceneWorldMatrix": runner.encode(model), "renderWorldMatrix": runner.encode(model), "settings": settings or {}}

    def group(objects, offset, node, motion):
        specifications = [fixture["bimGeometry"][str(index)] for index in objects]
        world = np.concatenate([runner.transform_vertices(base, spec["vertices"]) for spec in specifications])
        center = (world.min(axis=0)+world.max(axis=0))/2
        proxy = runner.translation(center + offset)
        model = runner.translation(offset) @ base
        normal = np.zeros((4, 3))
        normal[:3, :] = np.linalg.inv(model[:3, :3]).T
        fragments = []
        for index, spec in zip(objects, specifications):
            positions = runner.transform_vertices(model, spec["vertices"])
            fragments.append({"objectIndex": index, "guid": spec["guid"], "sourceId": "",
                "model": runner.encode(model), "normalMatrix": runner.encode(normal),
                "boundsMin": positions.min(axis=0).tolist(), "boundsMax": positions.max(axis=0).tolist()})
        return {"node": node, "motion": motion, "position": proxy[:3, 3].tolist(),
                "sceneWorldMatrix": runner.encode(proxy), "renderWorldMatrix": [], "settings": {"gravityFactor": 0}}, \
               {"node": node, "productId": node, "objects": fragments}

    for frame in sample_frames:
        bodies, elements, joints = [], [], []
        active_primary = frame != final and not (case == "repository-model-replacement" and frame >= 121) \
            and not (case == "bim-global-translation" and frame >= 191)
        if active_primary:
            for key, spec in fixture["primaryGeometry"].items():
                node = int(key)
                if node == dynamic:
                    continue
                if case == "joint-during-kinematic-motion":
                    if node == fixture["pusher"]:
                        bodies.append(primary(node, runner.translation([-2 if frame < 4 else 2, 0, 0]), 1, {"friction": 0}))
                    elif 3 <= frame < 11:
                        bodies.append(primary(node, runner.matrix(spec["authoredModel"]), 2, {"gravityFactor": 0}))
                elif case in ("nested-offcenter", "mirrored-parent", "bim-global-translation") or \
                   (case == "ramp-contact" and (node == 1 or frame >= 2)) or \
                   (case == "mixed-provider-slider" and node == 1) or \
                   (case == "repository-model-replacement" and node == 2) or case == "ccd-thin-wall":
                    bodies.append(primary(node, runner.matrix(spec["authoredModel"]), 0))
        if dynamic is not None and active_primary:
            included = frame in (3, 30, 180, 184) if case in ("nested-offcenter", "mirrored-parent") else \
                       frame in (4, 60, 180) if case == "ramp-contact" else \
                       frame in (3, 120) if case == "repository-model-replacement" else frame in (2, 3, 5, 16)
            if included:
                model = runner.matrix(fixture["primaryGeometry"][str(dynamic)]["authoredModel"])
                settings = {}
                if case in ("nested-offcenter", "mirrored-parent"):
                    height = {3: 3.99, 30: 2.9, 180: .9, 184: 4}[frame]
                    model[1, 3] = height
                elif case == "ramp-contact":
                    model[:3, 3] = {4: [-1, 5, 0], 60: [0, 3, 0], 180: [3, .5, 0]}[frame]
                elif case == "repository-model-replacement":
                    model[1, 3] += 1.5 if frame == 120 else 0
                else:
                    model[0, 3] = -5 if frame == 2 else -.125
                    settings = {"gravityFactor": 0, "useCCD": True, "linearVelocity": fixture["initialVelocity"]}
                bodies.append(primary(dynamic, model, 2, settings))
        if case == "bim-global-translation" and frame in (4, 30, 90, 92, 180, 184):
            shift = np.array([5, 0, -1] if frame >= 92 else [0, 0, 0], dtype=float)
            for objects, node, motion in (([0, 1], 50, 2), ([2], 51, 0)):
                offset = shift.copy()
                if node == 50:
                    offset[1] += {4: -.01, 30: -1.2, 90: -2.75, 92: -2.75, 180: -2.75, 184: 0}[frame]
                body, element = group(objects, offset, node, motion)
                bodies.append(body); elements.append(element)
        if case == "mixed-provider-slider" and frame in (3, 120, 144, 148, 240):
            offset = np.array([2.75 if frame in (120, 240) else 2, 0, -1], dtype=float)
            body, element = group([0, 1], offset, 50, 2)
            bodies.append(body); elements.append(element)
            joints = [{"id": 8}]
        if case == "repository-model-replacement":
            if frame in (3, 120):
                joints = [{"id": 8}]
            if frame in (126, 240):
                for objects, node, motion, height in (([0], 50, 0, 0), ([2], 51, 2, .01 if frame == 126 else 1.5)):
                    body, element = group(objects, np.array([0, height, 0]), node, motion)
                    bodies.append(body); elements.append(element)
                joints = [{"id": 9}]
        if case == "joint-during-kinematic-motion":
            if 2 <= frame < 9:
                offset = np.array([3 if frame == 4 else 10 if frame == 6 else 0, 0, 0], dtype=float)
                pushed, element = group([0], offset, 50, 2)
                pushed["settings"]["friction"] = 0
                bodies.append(pushed); elements.append(element)
            if 4 <= frame < 11:
                joints = [{"id": 12, "type": "fixed", "nodeA": None, "nodeB": fixture["jointBody"],
                           "settings": {"anchor": [20, 0, 0]}}]
        state = {"available": True, "error": "", "droppedSeconds": 0, "bodies": bodies, "bodyCount": len(bodies),
                 "auxiliaryElements": elements, "joints": joints, "jointCount": len(joints), "playing": frame not in (144, 148, 184)}
        if case == "joint-during-kinematic-motion":
            state.update({"playing": 2 <= frame < 7, "substeps": 4 if 2 <= frame < 7 else 0,
                          "totalSteps": 4 * (frame - 1) if frame < 7 else 0})
        scene = {"primaryObjectCount": len(fixture["primaryGeometry"]) if active_primary else 0,
                 "auxiliaryObjectCount": 0 if frame == final else 15 if case == "repository-model-replacement" and frame >= 121 else
                 len(fixture["bimGeometry"]) if case in ("bim-global-translation", "mixed-provider-slider", "joint-during-kinematic-motion") else 0}
        png = Path(directory) / f"{case}-{frame}.png"
        png.write_bytes(f"frame {frame}".encode())
        record["samples"].append({"frame": frame, "png": str(png), "telemetry": {
            "physics": state, "scene": scene, "rayShadows": {"active": True}, "taa": {"enabled": True}}})
    return record


class DiversityFixtureTests(unittest.TestCase):
    def test_all_conditions_create_valid_ordered_sequences(self):
        with tempfile.TemporaryDirectory() as temporary:
            for case in runner.CASES:
                source, auxiliary, sequence, fixture = runner.fixture(case, Path(temporary) / case)
                self.assertTrue(source.is_file())
                self.assertEqual(sequence["sampleFrames"], sorted(set(sequence["sampleFrames"])))
                self.assertEqual([event["frame"] for event in sequence["events"]], sorted(event["frame"] for event in sequence["events"]))
                self.assertEqual(fixture["case"], case)
                if auxiliary:
                    self.assertTrue(auxiliary.is_file())

    def test_repository_files_are_unchanged_inputs(self):
        with tempfile.TemporaryDirectory() as temporary:
            source, auxiliary, sequence, fixture = runner.fixture("repository-model-replacement", temporary)
            self.assertEqual(source.name, "taa_scene.gltf")
            self.assertIsNone(auxiliary)
            self.assertEqual(fixture["dynamic"], 6)
            self.assertEqual(len(fixture["bimGeometry"]), 15)
            self.assertEqual(len(fixture["sourceSha256"]), 64)
            self.assertTrue(any(event.get("guiReload", "").endswith("taa_equivalent.bim") for event in sequence["events"]))

    def test_tetrahedron_and_asymmetric_fragments_are_distinct_geometry(self):
        with tempfile.TemporaryDirectory() as temporary:
            _, _, _, fixture = runner.fixture("bim-global-translation", temporary)
            self.assertEqual(len(fixture["bimGeometry"]["2"]["vertices"]), 4)
            self.assertEqual(fixture["bimGeometry"]["0"]["guid"], fixture["bimGeometry"]["1"]["guid"])
            self.assertNotEqual(fixture["bimGeometry"]["1"]["guid"], fixture["bimGeometry"]["2"]["guid"])

    def test_tetrahedron_faces_point_away_from_its_interior(self):
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "tetrahedron.bim"
            runner.write_bim(path)
            mesh = json.loads(path.read_text())["meshes"][2]
            points = [mesh["coordinates"][i:i+3] for i in range(0, len(mesh["coordinates"]), 3)]
            interior = [sum(point[axis] for point in points)/len(points) for axis in range(3)]
            for start in range(0, len(mesh["indices"]), 3):
                a, b, c = [points[index] for index in mesh["indices"][start:start+3]]
                ab, ac = [b[i]-a[i] for i in range(3)], [c[i]-a[i] for i in range(3)]
                normal = [ab[1]*ac[2]-ab[2]*ac[1], ab[2]*ac[0]-ab[0]*ac[2], ab[0]*ac[1]-ab[1]*ac[0]]
                outward = [(a[i]+b[i]+c[i])/3-interior[i] for i in range(3)]
                self.assertGreater(sum(normal[i]*outward[i] for i in range(3)), 0)

    def test_mixed_body_replacement_explicitly_retains_zero_gravity(self):
        with tempfile.TemporaryDirectory() as temporary:
            _, _, sequence, _ = runner.fixture("mixed-provider-slider", temporary)
            events = {event["frame"]: event for event in sequence["events"]}
            self.assertEqual(events[2]["physicsSettings"]["gravityFactor"], 0)
            self.assertEqual(events[145]["physicsSettings"]["gravityFactor"], 0)

    def test_skip_and_help_use_only_standard_library(self):
        with tempfile.TemporaryDirectory() as temporary:
            output = Path(temporary) / "not-created"
            environment = dict(os.environ)
            environment.pop("CONTAINER_RUN_GPU_PHYSICS", None)
            for args, expected in ((["--output", str(output), "--require-opt-in"], 77), (["--help"], 0)):
                result = subprocess.run([sys.executable, "-S", "-B", str(Path(runner.__file__)), *args],
                                        capture_output=True, text=True, env=environment)
                self.assertEqual(result.returncode, expected, result.stderr)
            self.assertFalse(output.exists())

    def test_invalid_ccd_speeds_fail_before_output_creation(self):
        with tempfile.TemporaryDirectory() as temporary:
            output = Path(temporary) / "not-created"
            for speed in ("nan", "inf", "-inf", "499.9", "0", "-500", "1e100", "3.4028236e38", "invalid"):
                with self.subTest(speed=speed):
                    result = subprocess.run([sys.executable, "-S", "-B", str(Path(runner.__file__)),
                        "--output", str(output), "--check-existing", f"--ccd-speed={speed}"],
                        capture_output=True, text=True)
                    self.assertEqual(result.returncode, 2, result.stderr)
                    self.assertIn("finite float32 value >=500", result.stderr)
                    self.assertFalse(output.exists())

    def test_ccd_speed_bounds_and_float32_conversion(self):
        self.assertEqual(runner.parse_ccd_speed("500"), 500)
        self.assertEqual(runner.parse_ccd_speed("600"), 600)
        self.assertEqual(runner.parse_ccd_speed(float.fromhex("0x1.fffffep+127")), float.fromhex("0x1.fffffep+127"))
        self.assertEqual(runner.parse_ccd_speed("600.1"), 600.0999755859375)

    def test_joint_creation_uses_a_distinct_body_and_holds_the_pusher_until_frame_four(self):
        with tempfile.TemporaryDirectory() as temporary:
            source, auxiliary, sequence, fixture = runner.fixture("joint-during-kinematic-motion", temporary)
            self.assertEqual((fixture["pusher"], fixture["jointBody"]), (1, 3))
            self.assertEqual(sequence["objectNode"], 0)
            self.assertEqual(sequence["objectTranslation"], [
                {"frame": 1, "translation": [0, 0, 0]}, {"frame": 3, "translation": [0, 0, 0]},
                {"frame": 4, "translation": [4, 0, 0]}, {"frame": 16, "translation": [4, 0, 0]}])
            events = {event["frame"]: event for event in sequence["events"]}
            self.assertEqual(events[4]["physicsJointCreate"]["bodyB"], {"node": 3})
            self.assertEqual(events[4]["physicsJointCreate"]["bodyA"], {"world": True})
            self.assertEqual(events[7]["physicsAction"], "reset")
            self.assertEqual(events[9]["bimSelectedObject"], 0)
            self.assertEqual(events[11]["physicsNode"], 3)
            self.assertEqual(fixture["fixedRenderDeltaSeconds"], 4 / 60)
            self.assertEqual(len(json.loads(source.read_text())["scenes"][0]["nodes"]), 2)
            self.assertEqual(len(json.loads(auxiliary.read_text())["elements"]), 1)

    def test_kinematic_rebuild_fixtures_hold_then_move_scale_or_replace(self):
        with tempfile.TemporaryDirectory() as temporary:
            for case in runner.KINEMATIC_CONTACT_CASES:
                with self.subTest(case=case):
                    source, auxiliary, sequence, fixture = runner.fixture(case, Path(temporary) / case)
                    self.assertEqual(sequence["sampleFrames"], runner.CONTACT_SAMPLE_FRAMES)
                    self.assertEqual(fixture["fixedRenderDeltaSeconds"], 4 / 60)
                    self.assertEqual(len(json.loads(source.read_text())["nodes"]), 1)
                    self.assertEqual(len(json.loads(auxiliary.read_text())["elements"]), 1)
                    events = {event["frame"]: event for event in sequence["events"]}
                    self.assertEqual(events[7]["physicsAction"], "reset")
                    self.assertEqual(events[9]["bimSelectedObject"], 0)
                    self.assertEqual(events[11]["physicsNode"], fixture["primaryNode"])
                    if case == "kinematic-resize-contact":
                        self.assertEqual(sequence["objectTranslation"][2], {"frame": 4, "translation": [4, 0, 0]})
                        self.assertEqual(sequence["objectScale"][2], {"frame": 4, "scale": [1.2, 1.2, 1.2]})
                    else:
                        self.assertEqual(sequence["bimTranslation"][2], {"frame": 4, "translation": [4, 0, 0]})
                        self.assertEqual(events[4]["physicsAttach"], "kinematic")
                        self.assertEqual((events[1]["physicsSettings"]["friction"], events[4]["physicsSettings"]["friction"]), (.25, 0))

    def test_dynamic_support_moves_while_paused_after_its_stack_sleeps(self):
        with tempfile.TemporaryDirectory() as temporary:
            _, _, sequence, fixture = runner.fixture("dynamic-support-teleport", temporary)
            self.assertEqual(sequence["sampleFrames"], runner.SUPPORT_TELEPORT_SAMPLE_FRAMES)
            events = {event["frame"]: event for event in sequence["events"]}
            self.assertEqual(events[181]["physicsAction"], "pause")
            self.assertEqual(events[183]["physicsAction"], "play")
            self.assertEqual(sequence["objectTranslation"][2], {"frame": 182, "translation": [10, 0, 0]})
            self.assertEqual(events[211]["physicsAction"], "reset")
            self.assertEqual(events[215]["bimSelectedObject"], 0)
            self.assertEqual(events[219]["physicsNode"], fixture["supportNode"])

    def test_contact_chain_moves_a_nonjointed_platform_and_detaches_the_middle_link(self):
        with tempfile.TemporaryDirectory() as temporary:
            _, _, sequence, fixture = runner.fixture("contact-wake-chain", temporary)
            self.assertEqual(sequence["sampleFrames"], runner.CONTACT_WAKE_CHAIN_SAMPLE_FRAMES)
            events = {event["frame"]: event for event in sequence["events"]}
            self.assertEqual(events[1]["physicsAttach"], "static")
            self.assertEqual(events[1]["bimSelectedObject"], 0)
            self.assertEqual(events[5]["physicsJointCreate"]["bodyA"], {"node": fixture["chainNodes"][0]})
            self.assertEqual(events[6]["physicsJointCreate"]["bodyB"], {"node": fixture["chainNodes"][2]})
            self.assertEqual(events[181]["physicsAction"], "pause")
            self.assertEqual(sequence["bimTranslation"][2], {"frame": 182, "translation": [10, 0, 0]})
            self.assertEqual(events[215]["physicsNode"], fixture["chainNodes"][1])
            self.assertEqual(events[219]["bimSelectedObject"], 0)


class DiversityCheckerTests(unittest.TestCase):
    def setUp(self):
        if importlib.util.find_spec("numpy") is None:
            self.skipTest("NumPy required for independent geometry assertions")
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)

    def state(self, record, frame):
        return next(sample for sample in record["samples"] if sample["frame"] == frame)["telemetry"]["physics"]

    def contact_body(self, record, frame, support):
        primary = support == (record["fixture"]["case"] == "kinematic-resize-contact")
        node = record["fixture"]["primaryNode"] if primary else 50
        return next(body for body in self.state(record, frame)["bodies"] if body["node"] == node)

    def shift_contact_body(self, record, frame, support, x):
        state = self.state(record, frame)
        body = self.contact_body(record, frame, support)
        delta = x - body["position"][0]
        body["position"][0] = x
        body["sceneWorldMatrix"][12] += delta
        if body["renderWorldMatrix"]:
            body["renderWorldMatrix"][12] += delta
        for element in state["auxiliaryElements"]:
            if element["node"] == body["node"]:
                for obj in element["objects"]:
                    obj["model"][12] += delta
                    obj["boundsMin"][0] += delta
                    obj["boundsMax"][0] += delta

    def test_healthy_all_scene_conditions_pass(self):
        for case in runner.CASES:
            with self.subTest(case=case):
                self.assertTrue(runner.check(synthetic(self.temporary.name, case))["passed"])

    def test_offcenter_collider_at_visual_origin_fails(self):
        record = synthetic(self.temporary.name, "nested-offcenter")
        body = self.state(record, 180)["bodies"][-1]
        body["position"] = body["renderWorldMatrix"][12:15]
        with self.assertRaisesRegex(AssertionError, "Off-center collider"):
            runner.check(record)

    def test_original_geometry_rejects_stale_bim_bounds(self):
        record = synthetic(self.temporary.name, "bim-global-translation")
        self.state(record, 92)["auxiliaryElements"][0]["objects"][0]["boundsMax"][0] -= 5
        with self.assertRaisesRegex(AssertionError, "original geometry"):
            runner.check(record)

    def test_mirrored_scene_preserves_rendered_model(self):
        record = synthetic(self.temporary.name, "mirrored-parent")
        self.state(record, 180)["bodies"][-1]["renderWorldMatrix"][0] *= -1
        with self.assertRaisesRegex(AssertionError, "rendered transform is stale"):
            runner.check(record)

    def test_mixed_joint_replacement_keeps_scene_identity(self):
        record = synthetic(self.temporary.name, "mixed-provider-slider")
        self.state(record, 148)["joints"][0]["id"] += 1
        with self.assertRaisesRegex(AssertionError, "joint identity"):
            runner.check(record)

    def test_fast_body_cannot_satisfy_ccd_if_it_tunnels(self):
        record = synthetic(self.temporary.name, "ccd-thin-wall")
        body = self.state(record, 3)["bodies"][-1]
        body["position"][0] = 3.333333
        body["sceneWorldMatrix"][12] = body["renderWorldMatrix"][12] = 3.333333
        with self.assertRaisesRegex(AssertionError, "tunneled"):
            runner.check(record)

    def test_missing_ccd_setting_fails_even_if_final_body_stops(self):
        record = synthetic(self.temporary.name, "ccd-thin-wall")
        self.state(record, 2)["bodies"][-1]["settings"]["useCCD"] = False
        with self.assertRaisesRegex(AssertionError, "settings were not propagated"):
            runner.check(record)

    def test_cli_ccd_600_reaches_capture_event_and_report_specification(self):
        directory = Path(self.temporary.name)
        executable = directory / "not-run.exe"
        executable.write_bytes(b"CPU fixture test executable identity")
        output = directory / "capture-output"
        captures = []

        def capture(exe, capture_output, name, source, sequence, technique, mode, extra):
            captures.append(sequence)
            attached = next(event for event in sequence["events"] if event.get("physicsAttach") == "dynamic")
            self.assertEqual(attached["physicsSettings"]["linearVelocity"], [600, 0, 0])
            return synthetic(capture_output, "ccd-thin-wall", ccd_speed=600)

        arguments = [str(Path(runner.__file__)), "--exe", str(executable), "--output", str(output),
                     "--scene", "ccd-thin-wall", "--technique", "deferred-raster", "--ccd-speed", "600"]
        with mock.patch.object(sys, "argv", arguments), mock.patch.dict(sys.modules, {
                "gui_model_loading_regression": SimpleNamespace(run_lifecycle_capture=capture)}):
            self.assertEqual(runner.main(), 0)
        self.assertEqual(len(captures), 1)
        report = json.loads((output / "results.json").read_text(encoding="utf-8"))
        self.assertTrue(report["passed"])
        self.assertEqual(report["cases"][0]["fixture"]["initialVelocity"], [600, 0, 0])

    def test_ccd_600_rejects_telemetry_velocity_clamped_to_500(self):
        record = synthetic(self.temporary.name, "ccd-thin-wall", ccd_speed=600)
        self.assertTrue(runner.check(record)["passed"])
        self.state(record, 2)["bodies"][-1]["settings"]["linearVelocity"] = [500, 0, 0]
        with self.assertRaisesRegex(AssertionError, "velocity was not propagated"):
            runner.check(record)

    def test_legacy_ccd_records_retain_default_500_velocity(self):
        record = synthetic(self.temporary.name, "ccd-thin-wall")
        del record["fixture"]["initialVelocity"]
        self.assertTrue(runner.check(record)["passed"])

    def test_ccd_accepts_documented_penetration_slop_but_rejects_more(self):
        record = synthetic(self.temporary.name, "ccd-thin-wall")
        for frame in (3, 5, 16):
            body = self.state(record, frame)["bodies"][-1]
            body["position"][0] = -.105
            body["sceneWorldMatrix"][12] = body["renderWorldMatrix"][12] = -.105
        self.assertTrue(runner.check(record)["passed"])
        body = self.state(record, 3)["bodies"][-1]
        body["position"][0] = -.1049
        body["sceneWorldMatrix"][12] = body["renderWorldMatrix"][12] = -.1049
        with self.assertRaisesRegex(AssertionError, "tunneled"):
            runner.check(record)

    def test_joint_creation_cannot_teleport_pusher_without_transferring_contact(self):
        record = synthetic(self.temporary.name, "joint-during-kinematic-motion")
        state = self.state(record, 4)
        element = state["auxiliaryElements"][0]
        pushed = next(body for body in state["bodies"] if body["node"] == element["node"])
        delta = -pushed["position"][0]
        pushed["position"][0] += delta
        pushed["sceneWorldMatrix"][12] += delta
        for obj in element["objects"]:
            obj["model"][12] += delta
            obj["boundsMin"][0] += delta
            obj["boundsMax"][0] += delta
        with self.assertRaisesRegex(AssertionError, "skipped the kinematic contact sweep"):
            runner.check(record)

    def test_joint_motion_rejects_stale_proxy_despite_reported_body_displacement(self):
        record = synthetic(self.temporary.name, "joint-during-kinematic-motion")
        self.state(record, 4)["bodies"][-1]["position"][0] += 1
        with self.assertRaisesRegex(AssertionError, "proxy disagrees"):
            runner.check(record)

    def test_joint_motion_requires_render_publication_for_both_primary_bodies(self):
        for key in ("pusher", "jointBody"):
            with self.subTest(body=key):
                record = synthetic(self.temporary.name, "joint-during-kinematic-motion")
                primary = next(body for body in self.state(record, 4)["bodies"]
                               if body["node"] == record["fixture"][key])
                primary["renderWorldMatrix"] = []
                with self.assertRaisesRegex(AssertionError, "primary render matrix is missing"):
                    runner.check(record)

    def test_joint_motion_preserves_imported_overlay_geometry_counts(self):
        record = synthetic(self.temporary.name, "joint-during-kinematic-motion")
        for sample in record["samples"]:
            if sample["frame"] < 15:
                sample["telemetry"]["scene"]["auxiliaryObjectCount"] = 3
        self.assertTrue(runner.check(record)["passed"])
        record["samples"][-2]["telemetry"]["scene"]["auxiliaryObjectCount"] = 2
        with self.assertRaisesRegex(AssertionError, "changed imported geometry counts"):
            runner.check(record)

    def test_joint_motion_requires_batched_ticks_and_unrelated_endpoint(self):
        record = synthetic(self.temporary.name, "joint-during-kinematic-motion")
        self.state(record, 4)["substeps"] = 1
        with self.assertRaisesRegex(AssertionError, "four fixed ticks"):
            runner.check(record)
        record = synthetic(self.temporary.name, "joint-during-kinematic-motion")
        self.state(record, 4)["joints"][0]["nodeB"] = 50
        with self.assertRaisesRegex(AssertionError, "moving collision bodies"):
            runner.check(record)

    def test_joint_motion_reset_keeps_joint_identity_and_retires_detached_bim(self):
        record = synthetic(self.temporary.name, "joint-during-kinematic-motion")
        self.state(record, 8)["joints"][0]["id"] += 1
        with self.assertRaisesRegex(AssertionError, "joint identity"):
            runner.check(record)
        record = synthetic(self.temporary.name, "joint-during-kinematic-motion")
        stale = copy.deepcopy(self.state(record, 8))
        current = self.state(record, 10)
        current["bodies"].append(stale["bodies"][-1])
        current["auxiliaryElements"] = stale["auxiliaryElements"]
        current["bodyCount"] += 1
        with self.assertRaisesRegex(AssertionError, "retired correctly"):
            runner.check(record)

    def test_cli_joint_motion_overrides_the_helpers_single_tick_delta(self):
        directory = Path(self.temporary.name)
        executable = directory / "not-run.exe"
        executable.write_bytes(b"CPU fixture test executable identity")
        output = directory / "joint-capture-output"
        captures = []

        def capture(exe, capture_output, name, source, sequence, technique, mode, extra):
            captures.append(extra)
            self.assertEqual(extra[-2:], ["--fixed-dt", str(4 / 60)])
            self.assertIn("--bim-model", extra)
            return synthetic(capture_output, "joint-during-kinematic-motion")

        arguments = [str(Path(runner.__file__)), "--exe", str(executable), "--output", str(output),
                     "--scene", "joint-during-kinematic-motion", "--technique", "deferred-raster"]
        with mock.patch.object(sys, "argv", arguments), mock.patch.dict(sys.modules, {
                "gui_model_loading_regression": SimpleNamespace(run_lifecycle_capture=capture)}):
            self.assertEqual(runner.main(), 0)
        self.assertEqual(len(captures), 1)
        self.assertTrue(json.loads((output / "results.json").read_text())["passed"])

    def test_kinematic_rebuild_cannot_skip_contact_even_without_new_identity_telemetry(self):
        for case in runner.KINEMATIC_CONTACT_CASES:
            with self.subTest(case=case):
                record = synthetic(self.temporary.name, case)
                self.shift_contact_body(record, 4, False, 0)
                for sample in record["samples"]:
                    for body in sample["telemetry"]["physics"]["bodies"]:
                        del body["id"]
                with self.assertRaisesRegex(AssertionError, "skipped the pending contact sweep"):
                    runner.check(record)

    def test_kinematic_rebuild_requires_primary_render_and_current_bim_bounds(self):
        for case in runner.KINEMATIC_CONTACT_CASES:
            with self.subTest(case=case):
                record = synthetic(self.temporary.name, case)
                primary = next(body for body in self.state(record, 4)["bodies"]
                               if body["node"] == record["fixture"]["primaryNode"])
                primary["renderWorldMatrix"] = []
                with self.assertRaisesRegex(AssertionError, "missing the primary rendered transform"):
                    runner.check(record)
                record = synthetic(self.temporary.name, case)
                self.state(record, 4)["auxiliaryElements"][0]["objects"][0]["boundsMax"][0] += 1
                with self.assertRaisesRegex(AssertionError, "original geometry"):
                    runner.check(record)

    def test_kinematic_resize_rejects_missing_scale_even_when_contact_succeeds(self):
        record = synthetic(self.temporary.name, "kinematic-resize-contact")
        support = self.contact_body(record, 4, True)
        for field in ("sceneWorldMatrix", "renderWorldMatrix"):
            for diagonal in (0, 5, 10):
                support[field][diagonal] = 1
        with self.assertRaisesRegex(AssertionError, "primary proxy disagrees"):
            runner.check(record)

    def test_kinematic_contact_accepts_body_rotation_but_rejects_stale_proxy_orientation(self):
        import numpy as np
        for case in runner.KINEMATIC_CONTACT_CASES:
            with self.subTest(case=case):
                record = synthetic(self.temporary.name, case)
                pushed = self.contact_body(record, 4, False)
                pushed["rotation"] = [math.sin(.1), 0, 0, math.cos(.1)]
                pose = runner.node_matrix({"translation": pushed["position"], "rotation": pushed["rotation"]})
                pushed["sceneWorldMatrix"] = runner.encode(pose)
                if pushed["renderWorldMatrix"]:
                    pushed["renderWorldMatrix"] = runner.encode(pose)
                for element in self.state(record, 4)["auxiliaryElements"]:
                    if element["node"] != pushed["node"]:
                        continue
                    for obj in element["objects"]:
                        spec = record["fixture"]["bimGeometry"][str(obj["objectIndex"])]
                        model = pose @ runner.matrix(runner.BIM_ROTATION)
                        normal = np.zeros((4, 3))
                        normal[:3, :] = np.linalg.inv(model[:3, :3]).T
                        positions = runner.transform_vertices(model, spec["vertices"])
                        obj.update({"model": runner.encode(model), "normalMatrix": runner.encode(normal),
                            "boundsMin": positions.min(axis=0).tolist(), "boundsMax": positions.max(axis=0).tolist()})
                self.assertTrue(runner.check(record)["passed"])
                pushed["rotation"] = [0, 0, 0, 1]
                with self.assertRaisesRegex(AssertionError, "proxy disagrees with its body pose"):
                    runner.check(record)

    def test_kinematic_rebuild_requires_four_ticks_without_losing_accumulation(self):
        for case in runner.KINEMATIC_CONTACT_CASES:
            for field, value, error in (("substeps", 1, "four fixed ticks"), ("totalSteps", 4, "fixed-step accumulation")):
                with self.subTest(case=case, field=field):
                    record = synthetic(self.temporary.name, case)
                    self.state(record, 4)[field] = value
                    with self.assertRaisesRegex(AssertionError, error):
                        runner.check(record)

    def test_kinematic_rebuild_changes_only_support_handle_until_reset(self):
        for case in runner.KINEMATIC_CONTACT_CASES:
            with self.subTest(case=case):
                record = synthetic(self.temporary.name, case)
                self.contact_body(record, 4, True)["id"] = self.contact_body(record, 3, True)["id"]
                with self.assertRaisesRegex(AssertionError, "replace the support body handle"):
                    runner.check(record)
                record = synthetic(self.temporary.name, case)
                self.contact_body(record, 4, False)["id"] += 100
                with self.assertRaisesRegex(AssertionError, "unexpectedly rebuilt the pushed body"):
                    runner.check(record)

    def test_kinematic_rebuild_checks_reset_and_detached_auxiliary_membership(self):
        for case in runner.KINEMATIC_CONTACT_CASES:
            with self.subTest(case=case):
                record = synthetic(self.temporary.name, case)
                self.shift_contact_body(record, 8, False, 1)
                with self.assertRaisesRegex(AssertionError, "authored/reset pose"):
                    runner.check(record)
                record = synthetic(self.temporary.name, case)
                stale = copy.deepcopy(self.state(record, 8))
                current = self.state(record, 10)
                current["bodies"].append(next(body for body in stale["bodies"] if body["node"] == 50))
                current["auxiliaryElements"] = stale["auxiliaryElements"]
                current["bodyCount"] += 1
                with self.assertRaisesRegex(AssertionError, "retired correctly"):
                    runner.check(record)

    def test_cli_kinematic_rebuild_cases_request_four_tick_capture_delta(self):
        directory = Path(self.temporary.name)
        executable = directory / "not-run.exe"
        executable.write_bytes(b"CPU fixture test executable identity")
        for case in runner.KINEMATIC_CONTACT_CASES:
            with self.subTest(case=case):
                output = directory / case / "capture-output"
                captures = []

                def capture(exe, capture_output, name, source, sequence, technique, mode, extra):
                    captures.append(extra)
                    self.assertEqual(extra[-2:], ["--fixed-dt", str(4 / 60)])
                    self.assertIn("--bim-model", extra)
                    return synthetic(capture_output, case)

                arguments = [str(Path(runner.__file__)), "--exe", str(executable), "--output", str(output),
                             "--scene", case, "--technique", "deferred-raster"]
                with mock.patch.object(sys, "argv", arguments), mock.patch.dict(sys.modules, {
                        "gui_model_loading_regression": SimpleNamespace(run_lifecycle_capture=capture)}):
                    self.assertEqual(runner.main(), 0)
                self.assertEqual(len(captures), 1)
                self.assertTrue(json.loads((output / "results.json").read_text())["passed"])

    def test_dynamic_support_rider_must_fall_after_its_sleeping_support_moves(self):
        record = synthetic(self.temporary.name, "dynamic-support-teleport")
        state = self.state(record, 210)
        rider = next(body for body in state["bodies"] if body["node"] == 50)
        rider["position"][1] += 1
        rider["sceneWorldMatrix"][13] += 1
        for obj in state["auxiliaryElements"][0]["objects"]:
            obj["model"][13] += 1
            obj["boundsMin"][1] += 1
            obj["boundsMax"][1] += 1
        with self.assertRaisesRegex(AssertionError, "left its former rider floating"):
            runner.check(record)

    def test_dynamic_support_requires_contact_wake_without_a_paused_tick(self):
        record = synthetic(self.temporary.name, "dynamic-support-teleport")
        self.state(record, 182)["activeBodyCount"] = 1
        with self.assertRaisesRegex(AssertionError, "wake its former contact"):
            runner.check(record)
        record = synthetic(self.temporary.name, "dynamic-support-teleport")
        self.state(record, 182)["substeps"] = 1
        with self.assertRaisesRegex(AssertionError, "pause/resume timing"):
            runner.check(record)

    def test_dynamic_support_requires_render_publication_and_stable_geometry(self):
        record = synthetic(self.temporary.name, "dynamic-support-teleport")
        support = next(body for body in self.state(record, 182)["bodies"]
                       if body["node"] == record["fixture"]["supportNode"])
        support["renderWorldMatrix"] = []
        with self.assertRaisesRegex(AssertionError, "missing a primary rendered transform"):
            runner.check(record)
        record = synthetic(self.temporary.name, "dynamic-support-teleport")
        for sample in record["samples"]:
            if sample["frame"] < 225:
                sample["telemetry"]["scene"]["auxiliaryObjectCount"] = 3
        self.assertTrue(runner.check(record)["passed"])
        self.state(record, 210)["auxiliaryElements"][0]["objects"][0]["boundsMax"][1] += .5
        with self.assertRaisesRegex(AssertionError, "original geometry"):
            runner.check(record)

    def test_dynamic_support_retires_the_detached_rider(self):
        record = synthetic(self.temporary.name, "dynamic-support-teleport")
        previous = copy.deepcopy(self.state(record, 214))
        detached = self.state(record, 218)
        detached["bodies"].append(next(body for body in previous["bodies"] if body["node"] == 50))
        detached["auxiliaryElements"] = previous["auxiliaryElements"]
        detached["bodyCount"] += 1
        with self.assertRaisesRegex(AssertionError, "retired correctly"):
            runner.check(record)

    def test_contact_neighborhood_must_wake_the_whole_chain_before_resume(self):
        record = synthetic(self.temporary.name, "contact-wake-chain")
        self.state(record, 182)["activeBodyCount"] = 1
        with self.assertRaisesRegex(AssertionError, "wake every dynamic chain link"):
            runner.check(record)

    def test_contact_chain_rejects_delayed_or_nonuniform_freefall(self):
        record = synthetic(self.temporary.name, "contact-wake-chain")
        last = record["fixture"]["chainNodes"][-1]
        body = next(body for body in self.state(record, 184)["bodies"] if body["node"] == last)
        body["position"][1] += .02
        body["sceneWorldMatrix"][13] += .02
        body["renderWorldMatrix"][13] += .02
        with self.assertRaisesRegex(AssertionError, "fall uniformly"):
            runner.check(record)

    def test_contact_chain_requires_current_render_and_original_bim_geometry(self):
        record = synthetic(self.temporary.name, "contact-wake-chain")
        self.state(record, 184)["bodies"][0]["renderWorldMatrix"] = []
        with self.assertRaisesRegex(AssertionError, "missing a primary rendered transform"):
            runner.check(record)
        record = synthetic(self.temporary.name, "contact-wake-chain")
        self.state(record, 182)["auxiliaryElements"][0]["objects"][0]["boundsMin"][0] -= 10
        with self.assertRaisesRegex(AssertionError, "original geometry"):
            runner.check(record)

    def test_contact_chain_preserves_joint_identity_and_retires_both_middle_connections(self):
        record = synthetic(self.temporary.name, "contact-wake-chain")
        self.state(record, 214)["joints"][0]["id"] += 100
        with self.assertRaisesRegex(AssertionError, "fixed-joint identity"):
            runner.check(record)
        record = synthetic(self.temporary.name, "contact-wake-chain")
        state = self.state(record, 218)
        state["joints"] = copy.deepcopy(self.state(record, 214)["joints"][:1])
        state["jointCount"] = 1
        with self.assertRaisesRegex(AssertionError, "retire both fixed joints"):
            runner.check(record)

    def test_contact_chain_reset_recreates_bodies_without_changing_membership(self):
        record = synthetic(self.temporary.name, "contact-wake-chain")
        before = self.state(record, 210)["bodies"][0]
        after = self.state(record, 214)["bodies"][0]
        after["id"] = before["id"]
        with self.assertRaisesRegex(AssertionError, "obsolete body handle"):
            runner.check(record)


if __name__ == "__main__":
    unittest.main()
