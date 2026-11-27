"""Validate the headless parts of the BIM robotics benchmark runner."""

import copy
import json
from pathlib import Path
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools"))

from bim_robotics_benchmark import (
    CAMERA_BYTE_METRICS, CAMERA_CPU_METRICS, CAMERA_GPU_METRICS, CPU_PHASES,
    run_child, stats, summarize, validate_camera_timing, validate_cpu_timing,
)
from bim_robotics_benchmark_fixtures import simulation_scenario, write_synthetic_ifc


def camera_timing(available):
    return {
        "schema": "container.sensor-camera-benchmark", "version": 1, "width": 8, "height": 4,
        "device": {"name": "Test GPU"}, "build": {"revision": "test"},
        "gpuTiming": {"available": available, "timedFrames": int(available), "status": "test"},
        "finalFenceWaitMs": 0.0, "memory": {"allocationBytes": 128, "deviceLocalBytes": 64}, "frames": [
            {"sampleIndex": 0, "tick": 0, **dict.fromkeys(CAMERA_CPU_METRICS, 0.01),
             **dict.fromkeys(CAMERA_GPU_METRICS, 0.05 if available else None),
             **dict.fromkeys(CAMERA_BYTE_METRICS, 128)},
        ],
    }


def cpu_timing():
    return {
        "schema": "container.bim-robotics-cpu-benchmark", "version": 1,
        "assetCacheStatus": "disabled",
        "timingsNanoseconds": dict.fromkeys(CPU_PHASES, 10_000),
        "counts": {"physicsTicks": 12, "lidarScans": 3, "lidarRays": 2304},
    }


class BenchmarkRunnerTests(unittest.TestCase):
    def test_statistics_use_nearest_rank_and_keep_missing_values_null(self):
        self.assertIsNone(stats([]))
        self.assertEqual(stats([4, 2, 3, 1]), {"samples": 4, "min": 1, "median": 2.5, "p95": 4, "max": 4})

    def test_cpu_timing_requires_real_native_work_and_cold_cooking(self):
        timing = cpu_timing()
        validate_cpu_timing(timing, 12, 3, 2304)
        with self.assertRaisesRegex(RuntimeError, "lidarRays"):
            validate_cpu_timing(timing, 12, 3, 44)
        timing["timingsNanoseconds"]["collisionCook"] = None
        with self.assertRaisesRegex(RuntimeError, "collisionCook"):
            validate_cpu_timing(timing, 12, 3, 2304)

    def test_camera_timing_rejects_missing_frames_and_invented_gpu_samples(self):
        timing = camera_timing(False)
        validate_camera_timing(timing, [0], (8, 4))
        with self.assertRaisesRegex(RuntimeError, "scheduled frame"):
            validate_camera_timing(timing, [0, 6], (8, 4))
        timing["frames"][0]["gpuRgbReadbackMs"] = 0.0
        with self.assertRaisesRegex(RuntimeError, "GPU timing"):
            validate_camera_timing(timing, [0], (8, 4))
        timing = camera_timing(True)
        validate_camera_timing(timing, [0], (8, 4))
        timing["frames"][0]["gpuDepthLabelsReadbackMs"] = float("inf")
        with self.assertRaisesRegex(RuntimeError, "GPU timing"):
            validate_camera_timing(timing, [0], (8, 4))

    def test_summary_rejects_changed_simulation_and_distinguishes_unsupported_timestamps(self):
        phase = {"wallSeconds": 0.02, "processMemory": {"peakResidentBytes": 1024}}
        first = {
            "simulation": phase, "capture": phase, "verify": phase,
            "record": {**phase, "throughput": {"outputMiBPerSecond": 2}},
            "simulationSha256": "first", "cameraTiming": camera_timing(False), "cpuTiming": cpu_timing(),
        }
        second = copy.deepcopy(first)
        summary = summarize([first, second])
        self.assertEqual(summary["gpuReadbackSamples"], 0)
        self.assertIsNone(summary["cameraFrameMs"]["gpuRgbReadbackMs"])
        self.assertEqual(summary["peakProcessResidentMiB"]["record"]["median"], 1 / 1024)
        second["simulationSha256"] = "different"
        with self.assertRaisesRegex(RuntimeError, "changed simulation output"):
            summarize([first, second])

    def test_children_have_separate_diagnostics_and_failures_stop_the_runner(self):
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory)
            host = Path(sys.executable).resolve()
            result = run_child(host, ["-c", "import sys; print(123); print(321, file=sys.stderr)"],
                               output, "success", 15)
            self.assertEqual(json.loads((output / result["stdout"]).read_bytes()), 123)
            self.assertEqual((output / result["stderr"]).read_bytes().strip(), b"321")
            self.assertGreater(result["wallSeconds"], 0)
            self.assertIn("peakResidentBytes", result["processMemory"])
            with self.assertRaisesRegex(RuntimeError, "exit.*7"):
                run_child(host, ["-c", "raise SystemExit(7)"], output, "failure", 15)
            with self.assertRaisesRegex(RuntimeError, "exceeded 1 seconds"):
                run_child(host, ["-c", "import time; time.sleep(20)"], output, "timeout", 1)

    def test_synthetic_scene_and_scenario_are_repeatable(self):
        with tempfile.TemporaryDirectory() as directory:
            one, two = (Path(directory) / name for name in ("one.ifc", "two.ifc"))
            self.assertEqual(write_synthetic_ifc(one, 2), write_synthetic_ifc(two, 2))
            self.assertEqual(one.read_bytes(), two.read_bytes())
            self.assertGreater(write_synthetic_ifc(Path(directory) / "larger.ifc", 3), 4)
        self.assertEqual(simulation_scenario("synthetic", 60), simulation_scenario("synthetic", 60))


if __name__ == "__main__":
    unittest.main()
