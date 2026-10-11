#!/usr/bin/env python3
"""Pure CPU checks for native DDGI performance capture validation/statistics."""

import copy
from pathlib import Path
import unittest

import run_performance as perf


def telemetry(frame, budget=128, paused=False, off=False):
    update_frame = 120 if paused else 0 if off else frame
    passes = [{"name": "Lighting", "gpuTimed": True, "gpuMs": 2.0},
              {"name": "DdgiTrace", "gpuTimed": True, "gpuMs": .1},
              {"name": "DdgiUpdate", "gpuTimed": True, "gpuMs": .2}]
    return {"passes": passes, "gpuKnownMs": 2.3,
            "ddgi": {"enabled": not off, "supported": True, "active": not off,
                     "paused": paused, "debugView": 0, "updateFrame": update_frame,
                     "tracedRays": 0 if off or paused else budget * 128,
                     "updatedProbes": 0 if off or paused else budget,
                     "contentEpoch": 0 if off else 1, "refreshRemainingProbes": 0,
                     "historyResets": 1,
                     "firstProbe": 0 if paused or off else ((update_frame - 1) * budget) % 343,
                     "nextProbe": 0 if paused or off else (update_frame * budget) % 343,
                     "schedulingCursor": 0 if off else (update_frame * budget) % 343,
                     "totalAllocatedBytes": 3038992,
                     "settings": {"intensity": 1, "maxProbesPerFrame": budget,
                                  "maxRaysPerFrame": budget * 128}},
            "rayShadows": {"totalAllocatedBytes": 70496, "generation": 1},
            "resolution": [1024, 768], "technique": "deferred-raster",
            "camera": {"position": [0, 1, 3.5]},
            "taa": {"enabled": False, "allocatedImageBytes": 0},
            "lighting": {"localShadowAtlasAllocatedBytes": 805306368,
                         "ltcAllocatedImageBytes": 131072},
            "gpu": "GPU", "driverVersion": 1, "vulkanApiVersion": 1,
            "deviceIdleWaits": 0, "swapchainRecreates": 0,
            "gpuProfiler": {"available": True, "resultLatencyFrames": 1}}


class PerformanceHarnessTests(unittest.TestCase):
    def setUp(self):
        self.config = perf.Config(samples=2, repeats=1)
        self.case = perf.Case("deferred-raster", "paused", 0)
        self.frames = [perf.compact_frame(frame, telemetry(frame, paused=frame > 120))
                       for frame in [120, *self.config.measurement_frames]]

    def test_percentile_interpolates_and_preserves_signed_delta(self):
        stats = perf.statistic(list(range(1, 21)))
        self.assertEqual(stats["median"], 10.5)
        self.assertAlmostEqual(stats["p95"], 19.05)
        self.assertEqual(perf.statistic([-.2, -.1, 0])["median"], -.1)

    def test_statistics_reject_empty_and_nonfinite_data(self):
        for values in ([], [1, float("nan")], [float("inf")]):
            with self.assertRaises(AssertionError):
                perf.statistic(values)

    def test_sequence_pauses_after_warmup_and_samples_after_settling(self):
        seq = perf.sequence_for(self.case, self.config)
        self.assertEqual(seq["events"], [{"frame": 121, "ddgiPaused": True}])
        self.assertEqual(seq["sampleFrames"], [120, 127, 128])
        self.assertEqual(seq["frames"], 128)
        self.assertNotIn("events", perf.sequence_for(perf.Case("deferred-raster", "off", 0), self.config))

    def test_rotated_matrix_retains_all_modes_once_per_repeat(self):
        cases = perf.make_cases(perf.TECHNIQUES, perf.Config())
        self.assertEqual(len(cases), 30)
        for repeat in range(3):
            for technique in perf.TECHNIQUES:
                selected = [c.mode for c in cases if c.repeat == repeat and c.technique == technique]
                self.assertCountEqual(selected, perf.MODES)
        self.assertNotEqual([c.mode for c in cases[:5]], [c.mode for c in cases[10:15]])

    def test_off_command_disables_gi_and_keeps_intensity_one(self):
        command = perf.command_for(perf.Case("deferred-raster", "off", 0), self.config,
                                   Path("renderer.exe"), Path("scene.gltf"), Path("captures"))
        self.assertIn("--no-ddgi", command)
        self.assertNotIn("--ddgi", command)
        self.assertEqual(command[command.index("--ddgi-intensity") + 1], "1")
        self.assertIn("--no-validation", command)

    def test_timed_lighting_and_finite_matching_total_are_required(self):
        for mutation in (lambda t: t["passes"][0].update(gpuTimed=False),
                         lambda t: t["passes"][0].update(gpuMs=float("nan")),
                         lambda t: t.update(gpuKnownMs=4)):
            source = telemetry(120)
            mutation(source)
            with self.assertRaises(AssertionError):
                perf.compact_frame(120, source)

    def test_paused_empty_batch_retains_warmed_history_and_update_frame(self):
        perf.check_frames(self.case, self.config, self.frames)
        for key, value in (("updateFrame", 121), ("firstProbe", 1), ("nextProbe", 1),
                           ("schedulingCursor", 0),
                           ("tracedRays", 128), ("paused", False),
                           ("refreshRemainingProbes", 1), ("contentEpoch", 2), ("historyResets", 2)):
            bad = copy.deepcopy(self.frames)
            bad[1]["ddgi"][key] = value
            with self.assertRaises(AssertionError):
                perf.check_frames(self.case, self.config, bad)

    def test_retained_cursor_is_separate_from_paused_batch_and_gpu_timestamp_lag(self):
        self.assertNotEqual(self.frames[0]["ddgi"]["schedulingCursor"], 0)
        for sample in self.frames[1:]:
            self.assertEqual(sample["ddgi"]["nextProbe"], 0)
            self.assertEqual(sample["ddgi"]["schedulingCursor"],
                             self.frames[0]["ddgi"]["schedulingCursor"])
            sample["gpuProfiler"]["resultLatencyFrames"] = self.config.settle
        perf.check_frames(self.case, self.config, self.frames)
        summary = perf.summarize(self.case, self.config, self.frames)
        self.assertEqual(summary["warmedSchedulingCursor"], (120 * 128) % 343)
        self.assertEqual(summary["finalSchedulingCursor"], summary["warmedSchedulingCursor"])

    def test_missing_invalid_cursor_and_off_cursor_progress_are_rejected(self):
        for value in (None, True, -1, 343, .5):
            bad = copy.deepcopy(self.frames)
            bad[1]["ddgi"]["schedulingCursor"] = value
            with self.assertRaises(AssertionError):
                perf.check_frames(self.case, self.config, bad)
        bad = copy.deepcopy(self.frames)
        del bad[1]["ddgi"]["schedulingCursor"]
        with self.assertRaises(AssertionError):
            perf.check_frames(self.case, self.config, bad)
        case = perf.Case("deferred-raster", "off", 0)
        frames = [perf.compact_frame(frame, telemetry(frame, off=True))
                  for frame in [120, *self.config.measurement_frames]]
        perf.check_frames(case, self.config, frames)
        frames[1]["ddgi"]["schedulingCursor"] = 1
        with self.assertRaises(AssertionError):
            perf.check_frames(case, self.config, frames)

    def test_active_cursor_tracks_current_submission_independently_of_timestamps(self):
        case = perf.Case("deferred-raster", "low", 0)
        frames = [perf.compact_frame(frame, telemetry(frame, budget=16))
                  for frame in [120, *self.config.measurement_frames]]
        frames[1]["gpuProfiler"]["resultLatencyFrames"] = self.config.settle
        perf.check_frames(case, self.config, frames)
        for key in ("firstProbe", "nextProbe", "schedulingCursor"):
            bad = copy.deepcopy(frames)
            bad[1]["ddgi"][key] = (bad[1]["ddgi"][key] + 1) % 343
            with self.assertRaises(AssertionError):
                perf.check_frames(case, self.config, bad)

    def test_recreation_or_profiler_latency_contaminates_measurement(self):
        for key, value in (("deviceIdleWaits", 1), ("swapchainRecreates", 1), ("taaEnabled", True)):
            bad = copy.deepcopy(self.frames)
            bad[1][key] = value
            with self.assertRaises(AssertionError):
                perf.check_frames(self.case, self.config, bad)
        bad = copy.deepcopy(self.frames)
        bad[1]["gpuProfiler"]["resultLatencyFrames"] = 7
        with self.assertRaises(AssertionError):
            perf.check_frames(self.case, self.config, bad)

    def test_active_sweep_reports_submitted_and_simulation_latency(self):
        case = perf.Case("deferred-raster", "low", 0)
        frames = [perf.compact_frame(f, telemetry(f, budget=16))
                  for f in [120, *self.config.measurement_frames]]
        perf.check_frames(case, self.config, frames)
        summary = perf.summarize(case, self.config, frames)
        self.assertEqual(summary["completeSweepSubmittedFrames"], 22)
        self.assertAlmostEqual(summary["completeSweepSimulationMs"], 1000 * 22 / 60)
        self.assertAlmostEqual(summary["sweepGpuKnownMsEstimate"], 22 * 2.3)

    def test_lookup_difference_keeps_negative_measurement_noise(self):
        def record(mode, ms):
            frames = copy.deepcopy(self.frames)
            return {"passed": True, "mode": mode, "technique": "deferred-raster", "repeat": 1,
                    "frames": frames, "summary": {"passesGpuMs": {"Lighting": {"median": ms}}}}
        values = perf.matched_comparisons([record("off", 2), record("paused", 1.9)])
        self.assertAlmostEqual(values[0]["lightingRouteDeltaMs"], -.1)

    def test_invalid_capture_ranges_and_incomplete_warmup_are_rejected(self):
        for config in (perf.Config(warmup=21), perf.Config(settle=5), perf.Config(samples=1),
                       perf.Config(samples=10000), perf.Config(width=0)):
            with self.assertRaises(AssertionError):
                perf.validate_config(config)


if __name__ == "__main__":
    unittest.main()
