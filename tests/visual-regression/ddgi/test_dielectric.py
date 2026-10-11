#!/usr/bin/env python3
"""CPU checks for the production dielectric native-capture acceptance harness."""
import copy
import json
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest.mock import patch

import run_dielectric as glass


def synthetic_cases():
    cases = []
    for technique in glass.TECHNIQUES:
        for fixture in glass.VARIANTS:
            for enabled in ((False, True) if fixture == "thick-clear" else (True,)):
                for primary in (True, False):
                    rgb = [1, 1, 1] if fixture == "open-control" else (
                        [.5, .5, .5] if fixture == "thick-clear" and enabled else
                        [.2, .2, .2] if fixture == "thick-rough" else
                        [.01, .1, .2] if fixture == "thick-absorbing" else [.001] * 3)
                    value = sum(rgb) / 3 if primary else 0
                    cases.append({"name": glass.case_name(fixture, technique, enabled, primary),
                        "technique": technique, "fixture": fixture, "dielectric": enabled,
                        "primaryIndirect": primary, "passed": True,
                        "receivers": {"shaded-back": {
                            "estimatedLuminance": .3 + value,
                            "estimatedRadiance": [c + .3 for c in rgb] if primary else [.3] * 3,
                            "saturatedChannelFraction": 0}}})
    return cases


class DielectricHarnessTests(unittest.TestCase):
    def test_closed_thickness_and_rough_absorption_materials_are_authored_distinctly(self):
        with tempfile.TemporaryDirectory() as temporary:
            output = Path(temporary)
            manifest = glass.write_dielectric_fixtures(output)
            self.assertEqual(len(manifest["fixtures"]), 7)
            for scene in manifest["fixtures"]:
                document = json.loads((output / scene["gltf"]).read_text())
                material = next(m for m in document["materials"] if m["name"] == "partition")
                self.assertEqual(material["alphaMode"], "OPAQUE")
                self.assertEqual(material["pbrMetallicRoughness"]["baseColorFactor"], [1] * 4)
                self.assertEqual(material["extensions"]["KHR_materials_volume"]["thicknessFactor"], .2)
                self.assertEqual(material["extensions"]["KHR_materials_transmission"]["transmissionFactor"], 1)
                self.assertEqual(material["extensions"]["KHR_materials_ior"]["ior"], 1.5)
                self.assertEqual(material["pbrMetallicRoughness"]["roughnessFactor"],
                                 .45 if scene["name"] == "thick-rough" else 0)
                volume = material["extensions"]["KHR_materials_volume"]
                if scene["name"] == "thick-absorbing":
                    self.assertEqual(volume["attenuationColor"], [.001, .15, .8])
                    self.assertEqual(volume["attenuationDistance"], .1)
                else:
                    self.assertNotIn("attenuationDistance", volume)
                self.assertEqual(len(scene["gltfSha256"]), 64)

    def test_actual_closed_slab_has_both_entry_exit_and_no_probe_inside(self):
        scene = glass.dielectric_scene("thick-clear")
        faces = [quad for quad in scene["quads"] if quad["material"] == "partition"]
        self.assertEqual(len(faces), 6)
        self.assertEqual({tuple(quad["normal"]) for quad in faces},
                         {(1., 0., 0.), (-1., 0., 0.), (0., 1., 0.),
                          (0., -1., 0.), (0., 0., 1.), (0., 0., -1.)})
        volume = scene["volume"]
        self.assertTrue(all(abs(volume["origin"][0] + column * volume["spacing"][0]) > .1
                            for column in range(volume["counts"][0])))
        self.assertEqual(len([q for q in glass.dielectric_scene("malformed-open")["quads"]
                              if q["material"] == "partition"]), 1)
        self.assertEqual(len([q for q in glass.dielectric_scene("nested-volumes")["quads"]
                              if q["material"] == "partition"]), 12)

    def test_opaque_terminal_uses_closed_nontransmitting_white_mesh(self):
        scene = glass.dielectric_scene("opaque-behind")
        opaque = [q for q in scene["quads"] if q["name"].startswith("opaque-terminal")]
        self.assertEqual(len(opaque), 6)
        self.assertEqual({q["material"] for q in opaque}, {"white"})
        self.assertTrue(all(q["center"][0] >= .15 - 1e-8 for q in opaque))

    def test_commands_are_matched_active_fields_with_only_new_transport_opt_in(self):
        scene = glass.dielectric_scene("thick-clear")
        scene["gltf"] = "fixture.gltf"
        on = glass.command_for("renderer.exe", Path("out"), scene, "test", 160, primary=True)
        off = glass.command_for("renderer.exe", Path("out"), scene, "test", 160, primary=False)
        self.assertIn("--ddgi-dielectric", on)
        self.assertNotIn("--ddgi-thin-glass", on)
        self.assertNotIn("--no-ddgi", off)
        off[off.index("--ddgi-intensity") + 1] = "1"
        self.assertEqual(on, off)
        disabled = glass.command_for("renderer.exe", Path("out"), scene, "test", 160,
                                     dielectric=False)
        self.assertEqual(disabled[-1], "--no-ddgi-dielectric")
        self.assertEqual(disabled[:-1], on[:-1])

    def test_comparison_measures_clear_and_rough_transport_and_selective_absorption(self):
        result = glass.compare_dielectric(synthetic_cases())
        self.assertEqual(set(result), set(glass.TECHNIQUES))
        for values in result.values():
            receivers = values["receivers"]
            self.assertAlmostEqual(receivers["thick-clear"]["indirectLuminance"], .5)
            self.assertAlmostEqual(receivers["thick-rough"]["indirectLuminance"], .2)
            self.assertAlmostEqual(receivers["thick-absorbing"]["indirectRadiance"][0], .01)
            self.assertAlmostEqual(values["blockedIndirectLimit"], .05)

    def test_comparison_rejects_missing_duplicate_failed_or_nonfinite_captures(self):
        cases = synthetic_cases()
        with self.assertRaisesRegex(AssertionError, "Missing matched"):
            glass.compare_dielectric(cases[1:])
        with self.assertRaisesRegex(AssertionError, "Duplicate"):
            glass.compare_dielectric(cases + [copy.deepcopy(cases[0])])
        for field, value in (("estimatedLuminance", float("nan")),
                             ("estimatedRadiance", [0, float("inf"), 0]),
                             ("saturatedChannelFraction", .1)):
            invalid = copy.deepcopy(cases)
            invalid[0]["receivers"]["shaded-back"][field] = value
            with self.assertRaises(AssertionError):
                glass.compare_dielectric(invalid)
        failed = copy.deepcopy(cases)
        failed[0]["passed"] = False
        with self.assertRaisesRegex(AssertionError, "Failed dielectric"):
            glass.compare_dielectric(failed)

    def test_comparison_rejects_no_signal_wrong_absorption_and_terminal_leaks(self):
        for fixture in glass.VARIANTS:
            if fixture == "thick-absorbing":
                field, value = "estimatedRadiance", [.8] * 3
            else:
                field, value = "estimatedLuminance", (
                    .3 if fixture in ("open-control", "thick-clear", "thick-rough") else 1.)
            cases = synthetic_cases()
            chosen = next(case for case in cases if case["fixture"] == fixture and
                          case["dielectric"] and case["primaryIndirect"])
            chosen["receivers"]["shaded-back"][field] = value
            with self.assertRaises(AssertionError):
                glass.compare_dielectric(cases)

    def test_capture_rejects_stale_success_artifacts(self):
        scene = glass.dielectric_scene("thick-clear")
        scene["gltf"] = "fixture.gltf"
        with tempfile.TemporaryDirectory() as temporary:
            output = Path(temporary)
            name = glass.case_name(scene["name"], "deferred-raster", True, True)
            stale = output / f"{name}.telemetry.json"
            stale.write_text("{}")
            (output / f"{name}.png").write_bytes(b"stale")
            with patch.object(glass.subprocess, "run", return_value=subprocess.CompletedProcess([], 0, "", "")):
                result = glass.capture(Path("renderer.exe"), output, {}, scene, 160, 60,
                                       technique="deferred-raster", dielectric=True, primary=True)
            self.assertFalse(result["passed"])
            self.assertFalse(stale.exists())
            self.assertFalse((output / f"{name}.png").exists())

    def test_scene_and_command_input_validation_is_preserved(self):
        with self.assertRaisesRegex(AssertionError, "Unknown dielectric"):
            glass.dielectric_scene("missing")
        scene = glass.dielectric_scene("thick-clear")
        scene["gltf"] = "fixture.gltf"
        with self.assertRaisesRegex(AssertionError, "frames"):
            glass.command_for("renderer.exe", Path("out"), scene, "capture", 0)
        with self.assertRaisesRegex(AssertionError, "technique"):
            glass.command_for("renderer.exe", Path("out"), scene, "capture", 1, technique="missing")


if __name__ == "__main__":
    unittest.main()
