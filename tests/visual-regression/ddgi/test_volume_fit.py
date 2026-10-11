#!/usr/bin/env python3
"""CPU checks for native DDGI scene-fit commands, bounds, and rejection checks."""
import base64
import copy
import json
from pathlib import Path
import struct
import subprocess
import tempfile
import unittest
from unittest import mock

import run_volume_fit as fit


def telemetry():
    return {"enabled": True, "supported": True, "active": True,
            "debugProbeRadius": .12, "status": "DDGI active", "updateFrame": 6,
            "updatedProbes": 125, "tracedRays": 8000, "firstProbe": 0,
            "historyResets": 1,
            "sceneFit": {"applied": True, "boundsMin": [-1, 0, -1],
                         "boundsMax": [1, 2, 1], "probeBudget": 512,
                         "probeCount": 125, "payloadBytes": 645824,
                         "status": "Fitted current source triangle bounds"},
            "settings": {"origin": [-2, -1, -2], "spacing": [1, 1, 1],
                         "counts": [5, 5, 5], "maxDistance": 7,
                         "normalBias": .06, "viewBias": .02, "visibilityBias": .1}}


class VolumeFitTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.output = Path(self.directory.name)
        self.bounds = ((-1, 0, -1), (1, 2, 1))
        self.case = fit.FitCase("fit-unit", self.output / "input.gltf", self.bounds)

    def source(self):
        positions = struct.pack("<12f", 0, 0, 0, 1, 1, 1, .25, .5, .75, 1000, 1000, 1000)
        indices = struct.pack("<3H", 0, 1, 2)
        payload = positions + indices
        value = {"asset": {"version": "2.0"},
                 "buffers": [{"byteLength": len(payload), "uri": "data:application/octet-stream;base64," + base64.b64encode(payload).decode()}],
                 "bufferViews": [{"buffer": 0, "byteLength": len(positions)},
                                 {"buffer": 0, "byteOffset": len(positions), "byteLength": len(indices)}],
                 "accessors": [{"bufferView": 0, "componentType": 5126, "type": "VEC3", "count": 4},
                               {"bufferView": 1, "componentType": 5123, "type": "SCALAR", "count": 3}],
                 "meshes": [{"primitives": [{"attributes": {"POSITION": 0}, "indices": 1}]}],
                 "nodes": [{"translation": [2, 3, 4], "children": [1]},
                           {"name": "Partition", "mesh": 0, "scale": [2, 3, 4]},
                           {"mesh": 0, "translation": [9999, 9999, 9999]}],
                 "scenes": [{"nodes": [0]}], "scene": 0}
        path = self.output / "bounds.gltf"
        fit.write_json(path, value)
        return path, value

    def test_reference_bounds_use_active_hierarchy_and_referenced_vertices(self):
        path, _ = self.source()
        self.assertEqual(fit.gltf_triangle_bounds(path), ((2, 3, 4), (4, 6, 8)))
        self.assertEqual(fit.gltf_triangle_bounds(path, scale=.001),
                         ((.002, .003, .004), (.004, .006, .008)))
        self.assertEqual(fit.gltf_triangle_bounds(path, moved_node="Partition", translation=[5, 0, 0]),
                         ((7, 3, 4), (9, 6, 8)))

    def test_reference_bounds_decode_column_major_matrix_and_reject_bad_indices(self):
        path, value = self.source()
        value["nodes"][0] = {"matrix": [1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 7, 8, 9, 1], "children": [1]}
        fit.write_json(path, value)
        self.assertEqual(fit.gltf_triangle_bounds(path), ((7, 8, 9), (9, 11, 13)))
        value["accessors"][0]["count"] = 1
        fit.write_json(path, value)
        with self.assertRaisesRegex(AssertionError, "index out of bounds"):
            fit.gltf_triangle_bounds(path)

    def test_fit_checker_accepts_uniform_padded_volume_and_scaled_biases(self):
        result = fit.check_fit(telemetry(), self.bounds, 512, True)
        self.assertEqual(result["probeCount"], 125)
        self.assertEqual(result["paddedMin"], [-2, -1, -2])
        self.assertEqual(result["paddedMax"], [2, 3, 2])

    def test_fit_checker_rejects_stale_bounds_incorrect_cap_and_bad_coverage(self):
        for changed in ("bounds", "cap", "coverage", "padding", "count", "payload"):
            gi = telemetry()
            if changed == "bounds":
                gi["sceneFit"]["boundsMax"][0] = 5
            elif changed == "cap":
                gi["sceneFit"]["probeBudget"] = 1024
            elif changed == "coverage":
                gi["settings"]["origin"][0] = 0
            elif changed == "padding":
                gi["settings"]["origin"][0] = -1
            elif changed == "count":
                gi["sceneFit"]["probeCount"] += 1
            else:
                gi["sceneFit"]["payloadBytes"] = 65 * 1024 * 1024
            with self.subTest(changed=changed), self.assertRaises(AssertionError):
                fit.check_fit(gi, self.bounds, 512, True)

    def test_fit_checker_rejects_nonfinite_spacing_nonuniform_counts_and_short_distance(self):
        for changed in ("spacing", "nan", "dimension", "distance", "bias", "radius"):
            gi = telemetry()
            if changed == "spacing":
                gi["settings"]["spacing"][1] = 2
            elif changed == "nan":
                gi["settings"]["origin"][0] = float("nan")
            elif changed == "dimension":
                gi["settings"]["counts"][0] = 2
            elif changed == "distance":
                gi["settings"]["maxDistance"] = 2
            elif changed == "bias":
                gi["settings"]["normalBias"] = .03
            else:
                gi["debugProbeRadius"] = .06
            with self.subTest(changed=changed), self.assertRaises(AssertionError):
                fit.check_fit(gi, self.bounds, 512, True)

    def test_fit_checker_preserves_disable_and_capability_fallback(self):
        for disabled in (False, True):
            gi = telemetry()
            gi.update(enabled=not disabled, supported=disabled, active=False,
                      updatedProbes=0, tracedRays=0, updateFrame=0)
            fit.check_fit(gi, self.bounds, 512, not disabled, unsupported=not disabled)
            gi["updatedProbes"] = 1
            with self.assertRaises(AssertionError):
                fit.check_fit(gi, self.bounds, 512, not disabled, unsupported=not disabled)

    def test_tight_memory_checker_uses_the_requested_payload_cap(self):
        gi = telemetry()
        gi["settings"].update(counts=[4, 4, 4], spacing=[1.5, 1.5, 1.5],
                              maxDistance=8, normalBias=.09, viewBias=.03, visibilityBias=.15)
        gi["debugProbeRadius"] = .18
        gi["sceneFit"].update(probeBudget=64, probeCount=64, payloadBytes=400000)
        gi.update(updatedProbes=64, tracedRays=4096)
        fit.check_fit(gi, self.bounds, 64, True, payload_budget=1024 * 1024)
        gi["sceneFit"]["payloadBytes"] = 1024 * 1024 + 1
        with self.assertRaisesRegex(AssertionError, "payload"):
            fit.check_fit(gi, self.bounds, 64, True, payload_budget=1024 * 1024)

    def test_native_command_has_no_manual_fit_conflicts_and_preserves_flags(self):
        case = fit.FitCase("fit-command", self.case.scene, self.bounds, scale=.001,
                           enabled=False, unsupported=True)
        command = fit.command_for(case, Path("renderer.exe"), self.output)
        self.assertIn("--ddgi-fit-scene", command)
        self.assertIn("--no-ddgi", command)
        self.assertIn("--no-ray-query", command)
        self.assertEqual(command[command.index("--import-scale") + 1], "0.001")
        self.assertEqual(command[command.index("--ddgi-fit-probes") + 1], "512")
        for forbidden in ("--ddgi-origin", "--ddgi-spacing", "--ddgi-counts", "--ddgi-max-distance",
                          "--ddgi-normal-bias", "--ddgi-view-bias", "--ddgi-visibility-bias", "--ddgi-probe-radius"):
            self.assertNotIn(forbidden, command)
        default = fit.FitCase("default", self.case.scene, self.bounds, probe_budget=2048, default_budget=True)
        self.assertNotIn("--ddgi-fit-probes", fit.command_for(default, Path("renderer.exe"), self.output))

    def test_matrix_has_twelve_cases_formats_scales_and_live_request(self):
        cases = fit.make_cases(self.output)
        self.assertEqual(len(cases), 12)
        self.assertEqual({case.scale for case in cases[:6]}, {.001, 1, 1000})
        self.assertEqual({case.technique for case in cases[:6]}, {"forward-raster", "deferred-raster"})
        self.assertEqual({case.scene.suffix for case in cases[6:9]}, {".gltf", ".bim", ".usda"})
        tight = next(case for case in cases if case.memory_mb == 1)
        self.assertEqual(tight.probe_budget, 64)
        self.assertIn("--ddgi-memory-mb", fit.command_for(tight, Path("renderer.exe"), self.output))
        live = cases[-1]
        sequence = json.loads(live.sequence.read_text())
        self.assertEqual(sequence["events"], [{"frame": 4, "ddgiFitProbes": 512}])
        self.assertGreater(live.moved_bounds[1][0], live.bounds[1][0])
        self.assertEqual([frame for frame, _, _ in fit.capture_paths(live, self.output)], [1, 3, 4, 5, 6, 8])

    def test_live_checker_requires_explicit_refit_and_stable_postfit_history(self):
        before = telemetry()
        after = telemetry()
        after["settings"]["origin"][0] = -3
        after["settings"]["spacing"] = [2, 2, 2]
        after["settings"].update(normalBias=.12, viewBias=.04, visibilityBias=.2, maxDistance=14)
        after["debugProbeRadius"] = .24
        after["sceneFit"]["boundsMax"][0] = 4
        after["historyResets"] = 3
        samples = []
        for frame in (1, 3, 4, 5, 6, 8):
            gi = copy.deepcopy(before if frame < 4 else after)
            if frame >= 4:
                gi["updateFrame"] = frame - 3
            samples.append({"frame": frame, "ddgi": gi})
        case = fit.FitCase("live", self.case.scene, self.bounds, sequence=Path("live.json"),
                           moved_bounds=((-1, 0, -1), (4, 2, 1)))
        fit.check_frames(case, samples)
        stale = copy.deepcopy(samples)
        stale[2]["ddgi"] = copy.deepcopy(before)  # frame4 retained the pre-move fit
        with self.assertRaisesRegex(AssertionError, "same-frame live refit"):
            fit.check_frames(case, stale)
        samples[-1]["ddgi"]["historyResets"] += 1
        with self.assertRaisesRegex(AssertionError, "repeatedly reset"):
            fit.check_frames(case, samples)

    def test_parser_matrix_checks_all_conflicts_and_stops_before_vulkan(self):
        cases = fit.invalid_cli_cases()
        self.assertEqual(len(cases), 11)
        self.assertEqual(len({name for name, _, _ in cases}), 11)
        fit.check_cli_failure(1, "Error: cannot be combined", "cannot be combined")
        for code, log in ((0, "cannot be combined"), (None, "cannot be combined"),
                          (1, "unrelated error"), (1, "cannot be combined\nInitializing Vulkan renderer"),
                          (1, "cannot be combined\nVUID-test")):
            with self.assertRaises(AssertionError):
                fit.check_cli_failure(code, log, "cannot be combined")

    def test_capture_discards_stale_outputs_and_reports_missing_current_outputs(self):
        _, png, capture_json = next(fit.capture_paths(self.case, self.output))
        png.write_bytes(b"\x89PNG\r\n\x1a\n")
        fit.write_json(capture_json, {"ddgi": telemetry()})
        with mock.patch.object(fit.subprocess, "run", return_value=subprocess.CompletedProcess([], 0, "", "")):
            record = fit.capture(self.case, Path("renderer.exe"), self.output, {}, 1)
        self.assertFalse(record["passed"])
        self.assertFalse(png.exists())
        self.assertFalse(capture_json.exists())
        self.assertTrue(any("missing/invalid PNG" in failure for failure in record["failures"]))

    def test_capture_records_timeout_and_validation_errors(self):
        timeout = subprocess.TimeoutExpired("renderer", 1, output=b"partial", stderr=b"VUID-test")
        with mock.patch.object(fit.subprocess, "run", side_effect=timeout):
            record = fit.capture(self.case, Path("renderer.exe"), self.output, {}, 1)
        self.assertFalse(record["passed"])
        self.assertIsNone(record["returnCode"])
        self.assertTrue(record["validationErrors"])
        self.assertIn("partialVUID-test", (self.output / "fit-unit.log").read_text())


if __name__ == "__main__":
    unittest.main()
