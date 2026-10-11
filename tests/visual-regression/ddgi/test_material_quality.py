#!/usr/bin/env python3
"""CPU checks for authored material diagnostics and live DDGI event assertions."""

import copy
import json
from pathlib import Path
import tempfile
import unittest

import numpy as np

import run_material_quality as material


def live_samples():
    samples, updates, cursor = [], 0, 0
    for frame in range(1, 25):
        enabled, paused = (3 <= frame < 16 or frame >= 19), 9 <= frame < 12
        active = enabled and not paused
        first = cursor if active else 0
        if active:
            updates += 1
            cursor = (cursor + 256) % 637
        samples.append({"frame": frame, "ddgi": {
            "supported": True, "enabled": enabled, "active": enabled, "paused": paused,
            "updatedProbes": 256 if active else 0, "tracedRays": 32768 if active else 0,
            "firstProbe": first, "nextProbe": cursor if active else 0, "updateFrame": updates,
            "schedulingCursor": cursor,
            "totalAllocatedBytes": 1024 if frame < 3 else 5353008,
            "settings": {"counts": [13, 7, 7]}, "contentEpoch": int(frame >= 3),
            "historyResets": int(frame >= 3), "refreshRemainingProbes": 0,
            "status": "DDGI paused: using submitted probe history" if paused else "DDGI active"}})
    return samples


def comparisons():
    cases = [{"name": f"material-{name}", "fixture": name, "passed": True,
             "frames": [{"receivers": {receiver: {
                 "estimatedRadiance": [value, value, value], "estimatedLuminance": value}
                 for receiver in ("shaded-back", "lit-back")}}]}
            for name, value in zip(material.VARIANTS, (.0, .1, .2, .3, .4, .5, 1.))]
    for name, value in (("sheet-single", .08), ("sheet-double", .09)):
        cases.append({"name": f"primary-off-{name}", "fixture": name, "passed": True,
                      "frames": [{"receivers": {"shaded-back": {
                          "estimatedRadiance": [value, value, value],
                          "estimatedLuminance": value}}}]})
    return cases


class MaterialQualityTests(unittest.TestCase):
    def test_material_pairs_preserve_geometry_and_change_only_partition_flag(self):
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory)
            manifest = material.write_material_fixtures(output)
            self.assertEqual(len(manifest["fixtures"]), 7)
            for family in ("solid", "sheet", "doorway"):
                one = json.loads((output / f"material-{family}-single.gltf").read_text())
                two = json.loads((output / f"material-{family}-double.gltf").read_text())
                for gltf in (one, two):
                    gltf["scenes"][0]["name"] = "matched"
                self.assertEqual([m["name"] for m in one["materials"]], ["white", "partition", "red"])
                self.assertFalse(one["materials"][1]["doubleSided"])
                self.assertTrue(two["materials"][1]["doubleSided"])
                two["materials"][1]["doubleSided"] = False
                self.assertEqual(one, two)
                self.assertFalse(two["materials"][0]["doubleSided"])
                self.assertFalse(two["materials"][2]["doubleSided"])

    def test_solid_sheet_normals_and_topology_are_authored_explicitly(self):
        solid = material.material_scene("solid-single")
        faces = [q for q in solid["quads"] if q["material"] == "partition"]
        self.assertEqual(len(faces), 6)
        self.assertEqual(solid["partitionThickness"], .03)
        sheet = material.material_scene("sheet-single")
        quads = [q for q in sheet["quads"] if q["material"] == "partition"]
        self.assertEqual(len(quads), 1)
        self.assertEqual(sheet["partitionThickness"], 0.)
        self.assertEqual(quads[0]["normal"], [1., 0., 0.])
        for scene in (solid, sheet):
            for quad in scene["quads"]:
                corners = np.array(quad["corners"])
                winding = np.cross(corners[1] - corners[0], corners[2] - corners[0])
                self.assertGreater(np.dot(winding, quad["normal"]), 0.)

    def test_live_event_schema_contains_false_pause_and_enable_events(self):
        sequence = material.live_sequence()
        self.assertEqual(sequence["sampleFrames"], list(range(1, 25)))
        self.assertEqual(sequence["events"][2], {"frame": 12, "ddgiPaused": False})
        self.assertEqual(sequence["events"][3], {"frame": 16, "ddgiEnabled": False})
        sequence["events"][0]["ddgiEnabled"] = False
        self.assertTrue(material.live_sequence()["events"][0]["ddgiEnabled"])

    def test_commands_start_disabled_live_but_requested_enabled_capability_fallback(self):
        scene = material.material_scene("doorway-single")
        scene["gltf"] = "fixture.gltf"
        common = (Path("renderer.exe"), Path("captures"), scene, "live", 24)
        live = material.command_for(*common, live=True)
        self.assertIn("--no-ddgi", live)
        self.assertNotIn("--ddgi", live)
        self.assertIn("--capture-sequence", live)
        fallback = material.command_for(*common, unsupported=True)
        self.assertIn("--ddgi", fallback)
        self.assertIn("--no-ray-query", fallback)
        self.assertNotIn("--capture-sequence", fallback)
        off = material.command_for(*common, primary_indirect=False)
        self.assertIn("--ddgi", off)
        self.assertEqual(off[off.index("--ddgi-intensity") + 1], "0")

    def test_live_check_accepts_retained_history_cursor_and_false_events(self):
        result = material.check_live_frames(live_samples())
        self.assertEqual(result["updatedFrames"], 16)
        self.assertEqual(result["retainedProbeAllocationBytes"], 5353008)
        self.assertEqual(result["pausedFrames"], [9, 10, 11])
        self.assertEqual(result["finalSchedulingCursor"], 274)

    def test_live_check_rejects_retained_cursor_corruption_at_every_state(self):
        for frame in (1, 3, 9, 16, 19, 24):
            samples = live_samples()
            samples[frame - 1]["ddgi"]["schedulingCursor"] += 1
            with self.assertRaisesRegex(AssertionError, "committed scheduling cursor"):
                material.check_live_frames(samples)

    def test_live_check_rejects_ignored_false_event_and_cursor_restart(self):
        samples = live_samples()
        samples[11]["ddgi"]["paused"] = True
        with self.assertRaisesRegex(AssertionError, "false values"):
            material.check_live_frames(samples)
        samples = live_samples()
        samples[11]["ddgi"]["firstProbe"] = 0
        with self.assertRaisesRegex(AssertionError, "restarted scheduling cursor"):
            material.check_live_frames(samples)

    def test_live_check_rejects_pause_history_or_reenable_allocation_loss(self):
        for frame, key in ((9, "contentEpoch"), (9, "refreshRemainingProbes"),
                           (19, "updateFrame"), (19, "totalAllocatedBytes")):
            samples = live_samples()
            samples[frame - 1]["ddgi"][key] += 1
            with self.assertRaises(AssertionError):
                material.check_live_frames(samples)
        with self.assertRaisesRegex(AssertionError, "every frame"):
            material.check_live_frames(live_samples()[:-1])

    def test_capability_fallback_must_publish_reason_and_no_gpu_work(self):
        sample = {"ddgi": {"enabled": True, "supported": False, "active": False,
                            "updatedProbes": 0, "tracedRays": 0, "updateFrame": 0,
                            "firstProbe": 0, "nextProbe": 0,
                            "schedulingCursor": 0,
                            "status": "DDGI unavailable: ray queries, descriptor limits or atlas formats unsupported"},
                  "rayShadows": {"active": False}}
        material.check_unsupported_frames([sample])
        for key in ("tracedRays", "updateFrame", "nextProbe"):
            invalid = copy.deepcopy(sample)
            invalid["ddgi"][key] = 1
            with self.assertRaises(AssertionError):
                material.check_unsupported_frames([invalid])
        with self.assertRaisesRegex(AssertionError, "Missing"):
            material.check_unsupported_frames([])

    def test_material_comparison_reports_signed_differences_and_measured_ratios(self):
        result = material.compare_materials(comparisons())
        self.assertAlmostEqual(result["shadedBackToOpenLuminanceRatios"]["solid-double"], .1)
        self.assertAlmostEqual(result["doubleSidedDifferences"]["sheet"]["shaded-back"]
                               ["doubleMinusSingleLuminanceEstimate"], .1)
        cases = comparisons()
        cases[1]["frames"][0]["receivers"]["shaded-back"]["estimatedLuminance"] = -.1
        result = material.compare_materials(cases)
        self.assertAlmostEqual(result["doubleSidedDifferences"]["solid"]["shaded-back"]
                               ["doubleMinusSingleLuminanceEstimate"], -.1)

    def test_sheet_zero_primary_controls_do_not_overwrite_material_diagnostics(self):
        result = material.compare_materials(comparisons())
        self.assertAlmostEqual(result["shadedBackToOpenLuminanceRatios"]["sheet-single"], .2)
        single = result["sheetControlComparisons"]["sheet-single"]
        self.assertAlmostEqual(single["totalLuminanceEstimate"], .2)
        self.assertAlmostEqual(single["directOnlyLuminanceEstimate"], .08)
        self.assertAlmostEqual(single["isolatedIndirectLuminanceDeltaEstimate"], .12)
        self.assertAlmostEqual(single["isolatedIndirectToOpenLuminanceRatio"], .12)
        cases = comparisons()
        cases[-2]["frames"][0]["receivers"]["shaded-back"]["estimatedLuminance"] = .25
        signed = material.compare_materials(cases)["sheetControlComparisons"]["sheet-single"]
        self.assertAlmostEqual(signed["isolatedIndirectLuminanceDeltaEstimate"], -.05)

    def test_material_comparison_rejects_missing_dark_or_nonfinite_control(self):
        with self.assertRaisesRegex(AssertionError, "Missing"):
            material.compare_materials([case for case in comparisons() if case["fixture"] != "open-control"])
        with self.assertRaisesRegex(AssertionError, "zero-primary-indirect controls"):
            material.compare_materials(comparisons()[:-1])
        for value in (0, float("nan")):
            cases = comparisons()
            cases[6]["frames"][0]["receivers"]["shaded-back"]["estimatedLuminance"] = value
            with self.assertRaisesRegex(AssertionError, "illuminated open control"):
                material.compare_materials(cases)
        cases = comparisons()
        cases[0]["frames"][0]["receivers"]["shaded-back"]["estimatedRadiance"][0] = float("nan")
        with self.assertRaisesRegex(AssertionError, "Nonfinite"):
            material.compare_materials(cases)

    def test_runtime_shader_hashes_bind_every_standalone_asset(self):
        with tempfile.TemporaryDirectory() as directory:
            exe = Path(directory) / "renderer.exe"
            shaders = exe.parent / "spv_shaders"
            shaders.mkdir()
            first = shaders / "ddgi_update.comp.spv"
            second = shaders / "forward_opaque_ray.frag.spv"
            first.write_bytes(b"update")
            second.write_bytes(b"lighting")
            (shaders / "ignored.log").write_text("not SPIR-V")
            hashes = material.runtime_shader_hashes(exe)
            self.assertEqual(list(hashes), ["spv_shaders/ddgi_update.comp.spv",
                                           "spv_shaders/forward_opaque_ray.frag.spv"])
            self.assertEqual(len(hashes["spv_shaders/ddgi_update.comp.spv"]), 64)
            first.write_bytes(b"changed update")
            changed = material.runtime_shader_hashes(exe)
            self.assertNotEqual(changed["spv_shaders/ddgi_update.comp.spv"],
                                hashes["spv_shaders/ddgi_update.comp.spv"])
            self.assertEqual(changed["spv_shaders/forward_opaque_ray.frag.spv"],
                             hashes["spv_shaders/forward_opaque_ray.frag.spv"])
            first.unlink()
            second.unlink()
            with self.assertRaisesRegex(AssertionError, "no standalone SPIR-V"):
                material.runtime_shader_hashes(exe)


if __name__ == "__main__":
    unittest.main()
