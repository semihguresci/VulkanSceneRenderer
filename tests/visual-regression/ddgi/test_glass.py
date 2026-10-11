#!/usr/bin/env python3
"""CPU fixture and assertion checks for native DDGI thin-glass traversal tests."""
import base64
import copy
import json
from pathlib import Path
import struct
import tempfile
import unittest

import run_glass as glass


def synthetic_cases():
    cases = []
    for technique in glass.TECHNIQUES:
        for variant in glass.VARIANTS:
            for enabled in ((False, True) if variant == "glass-pane" else (True,)):
                for primary in (True, False):
                    indirect = 1.0 if variant == "open-control" else (
                        .5 if variant == "glass-pane" and enabled else .001)
                    value = .3 + (indirect if primary else 0)
                    cases.append({"name": glass.case_name(variant, technique, enabled, primary),
                        "fixture": variant, "technique": technique, "thinGlass": enabled,
                        "primaryIndirect": primary, "passed": True,
                        "receivers": {"shaded-back": {"estimatedRadiance": [value] * 3,
                            "estimatedLuminance": value, "saturatedChannelFraction": 0}}})
    return cases


def accessor_points(gltf, index):
    accessor = gltf["accessors"][index]
    view = gltf["bufferViews"][accessor["bufferView"]]
    payload = base64.b64decode(gltf["buffers"][view["buffer"]]["uri"].split(",", 1)[1])
    offset = view.get("byteOffset", 0) + accessor.get("byteOffset", 0)
    return [struct.unpack_from("<fff", payload, offset + i * view.get("byteStride", 12))
            for i in range(accessor["count"])]


class GlassHarnessTests(unittest.TestCase):
    def test_fixtures_author_alpha_one_explicit_transmission_only_on_glass(self):
        with tempfile.TemporaryDirectory() as directory:
            manifest = glass.write_glass_fixtures(directory, include_tint=True)
            self.assertEqual(len(manifest["fixtures"]), 6)
            for scene in manifest["fixtures"]:
                document = json.loads((Path(directory) / scene["gltf"]).read_text())
                partition = next(m for m in document["materials"] if m["name"] == "partition")
                self.assertEqual(partition["pbrMetallicRoughness"]["baseColorFactor"][3], 1)
                self.assertEqual(partition["alphaMode"], "OPAQUE")
                self.assertEqual(partition["extensions"]["KHR_materials_transmission"]["transmissionFactor"], 1)
                self.assertEqual(partition["extensions"]["KHR_materials_volume"]["thicknessFactor"],
                                 .1 if scene["name"] == "thick-unsupported" else 0)
                for material in document["materials"]:
                    if material["name"] != "partition":
                        self.assertNotIn("extensions", material)
                        self.assertFalse(material["doubleSided"])
                self.assertEqual(len(scene["gltfSha256"]), 64)

    def test_nine_actual_planes_are_clustered_outside_every_probe_column(self):
        with tempfile.TemporaryDirectory() as directory:
            manifest = glass.write_glass_fixtures(directory)
            scene = next(s for s in manifest["fixtures"] if s["name"] == "layer-cap")
            document = json.loads((Path(directory) / scene["gltf"]).read_text())
            primitive = document["meshes"][1]["primitives"][0]
            points = accessor_points(document, primitive["attributes"]["POSITION"])
            self.assertEqual(len(points), 36)
            planes = sorted(set(round(point[0], 5) for point in points))
            self.assertEqual(planes, [round((index - 4) * .01, 5) for index in range(9)])
            volume = scene["volume"]
            columns = [volume["origin"][0] + i * volume["spacing"][0]
                       for i in range(volume["counts"][0])]
            self.assertTrue(all(abs(x) >= .25 for x in columns))
            self.assertTrue(all(not min(planes) <= x <= max(planes) for x in columns))

    def test_opaque_terminal_wall_has_distinct_nontransmitting_material_and_closed_faces(self):
        scene = glass.glass_scene("opaque-behind")
        wall = [q for q in scene["quads"] if q["name"].startswith("opaque-behind-glass")]
        self.assertEqual(len(wall), 6)
        self.assertEqual({q["material"] for q in wall}, {"white"})
        self.assertEqual({tuple(q["normal"]) for q in wall},
                         {(1., 0., 0.), (-1., 0., 0.), (0., 1., 0.),
                          (0., -1., 0.), (0., 0., 1.), (0., 0., -1.)})
        document = glass.gltf_scene(scene)
        white = document["materials"][0]
        self.assertEqual(white["name"], "white")
        self.assertNotIn("extensions", white)
        partition = [q for q in scene["quads"] if q["material"] == "partition"]
        self.assertEqual(len(partition), 1)
        self.assertTrue(all(q["center"][0] > partition[0]["center"][0] for q in wall))

    def test_open_control_removes_pane_but_preserves_receivers_lights_and_probe_grid(self):
        open_scene, pane = (glass.glass_scene(name) for name in ("open-control", "glass-pane"))
        self.assertEqual(open_scene["receivers"], pane["receivers"])
        self.assertEqual(open_scene["camera"], pane["camera"])
        self.assertEqual(open_scene["light"], pane["light"])
        self.assertEqual(open_scene["volume"], pane["volume"])
        self.assertFalse(any(q["material"] == "partition" for q in open_scene["quads"]))
        self.assertEqual(glass.glass_scene("tinted-pane")["glassPolicy"]["tint"], [.25, .6, 1])

    def test_matched_commands_keep_active_field_shadow_quality_camera_and_exposure(self):
        scene = glass.glass_scene("glass-pane")
        scene["gltf"] = "fixture.gltf"
        output = Path("output")
        on = glass.command_for("renderer.exe", output, scene, "capture", 160,
                               technique="forward-raster", thin_glass=True, primary_indirect=True)
        off = glass.command_for("renderer.exe", output, scene, "capture", 160,
                                technique="forward-raster", thin_glass=True, primary_indirect=False)
        self.assertIn("--ddgi", on)
        self.assertNotIn("--no-ddgi", off)
        self.assertIn("--ddgi-thin-glass", on)
        self.assertEqual(on[on.index("--ddgi-intensity") + 1], "1")
        self.assertEqual(off[off.index("--ddgi-intensity") + 1], "0")
        off[off.index("--ddgi-intensity") + 1] = "1"
        self.assertEqual(on, off)
        blocked = glass.command_for("renderer.exe", output, scene, "capture", 160,
                                    technique="forward-raster", thin_glass=False)
        self.assertEqual(on[:-1], blocked)
        self.assertEqual(on[on.index("--ray-shadows") + 1], "hard")
        self.assertEqual(on[on.index("--warmup-frames") + 1], "159")
        self.assertEqual(on[on.index("--render-technique") + 1], "forward-raster")

    def test_comparisons_isolate_signed_indirect_and_check_both_routes(self):
        result = glass.compare_glass(synthetic_cases())
        self.assertEqual(set(result), set(glass.TECHNIQUES))
        for comparison in result.values():
            self.assertAlmostEqual(comparison["receivers"]["glass-pane"]["isolatedIndirectLuminance"], .5)
            self.assertAlmostEqual(comparison["receivers"]["glass-disabled"]["isolatedIndirectLuminance"], .001)
            self.assertAlmostEqual(comparison["blockedIndirectLimit"], .05)
        cases = synthetic_cases()
        opaque = next(c for c in cases if c["fixture"] == "opaque-behind" and c["primaryIndirect"])
        opaque["receivers"]["shaded-back"]["estimatedLuminance"] = .299
        signed = glass.compare_glass(cases)["deferred-raster"]["receivers"]["opaque-behind"]
        self.assertAlmostEqual(signed["isolatedIndirectLuminance"], -.001)

    def test_comparisons_reject_missing_duplicate_and_failed_pairs(self):
        with self.assertRaisesRegex(AssertionError, "Missing matched"):
            glass.compare_glass(synthetic_cases()[1:])
        cases = synthetic_cases()
        with self.assertRaisesRegex(AssertionError, "Duplicate"):
            glass.compare_glass(cases + [copy.deepcopy(cases[0])])
        cases[0]["passed"] = False
        with self.assertRaisesRegex(AssertionError, "Failed glass capture"):
            glass.compare_glass(cases)

    def test_comparisons_reject_no_open_signal_no_glass_signal_and_terminal_leaks(self):
        cases = synthetic_cases()
        cases[0]["receivers"]["shaded-back"]["estimatedLuminance"] = .3
        with self.assertRaisesRegex(AssertionError, "Open control"):
            glass.compare_glass(cases)
        for variant in ("glass-pane", "opaque-behind", "layer-cap", "thick-unsupported"):
            cases = synthetic_cases()
            case = next(c for c in cases if c["fixture"] == variant and c["thinGlass"] and c["primaryIndirect"])
            case["receivers"]["shaded-back"]["estimatedLuminance"] = .3 if variant == "glass-pane" else 1.
            with self.assertRaises(AssertionError):
                glass.compare_glass(cases)

    def test_nonfinite_and_saturated_image_metrics_cannot_pass(self):
        for field, value in (("estimatedLuminance", float("nan")),
                             ("estimatedRadiance", [0, float("inf"), 0]),
                             ("saturatedChannelFraction", .1)):
            cases = synthetic_cases()
            cases[0]["receivers"]["shaded-back"][field] = value
            with self.assertRaises(AssertionError):
                glass.compare_glass(cases)

    def test_fixture_generation_and_commands_reject_unknown_and_invalid_inputs(self):
        with self.assertRaisesRegex(AssertionError, "Unknown glass"):
            glass.glass_scene("unknown")
        with self.assertRaisesRegex(AssertionError, "resolution"):
            glass.glass_scene("glass-pane", width=20)
        scene = glass.glass_scene("glass-pane")
        scene["gltf"] = "test.gltf"
        for frames in (0, 10001):
            with self.assertRaisesRegex(AssertionError, "frames"):
                glass.command_for("exe", Path("output"), scene, "name", frames)
        with self.assertRaisesRegex(AssertionError, "technique"):
            glass.command_for("exe", Path("output"), scene, "name", 1, technique="unknown")


if __name__ == "__main__":
    unittest.main()
