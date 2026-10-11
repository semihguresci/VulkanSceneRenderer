import copy
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch
import run_features as features


def batch(indices, count=8):
    return {"selectedProbes": indices, "updatedProbes": len(indices),
            "tracedRays": len(indices) * 32, "schedulingCursor": 0,
            "active": True, "contentEpoch": 2, "refreshRemainingProbes": 0}


def motion():
    result = []
    for frame in range(1, 17):
        start = (frame - 1) * 2 % 8
        sample = batch([(start + index) % 8 for index in range(4)])
        sample["contentEpoch"] = 1 if frame < 10 else 2
        result.append({"frame": frame, "ddgi": sample})
    return result


class FeaturesHarnessTests(unittest.TestCase):
    def test_sequence_reads_renderer_intermediate_and_final_capture_names(self):
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory)
            sequence = output / "sequence.json"
            sequence.write_text(json.dumps({"sampleFrames": [1, 6]}))
            result = type("Run", (), {"returncode": 0, "stdout": "", "stderr": ""})()
            def emit(*args, **kwargs):
                for stem in ("sequence.frame-0001", "sequence"):
                    (output / f"{stem}.png").write_bytes(b"\x89PNG\r\n\x1a\n")
                    (output / f"{stem}.telemetry.json").write_text(json.dumps({"ddgi": batch([0])}))
                return result
            with patch.object(features.subprocess, "run", side_effect=emit):
                record = features.run_capture(output / "renderer.exe", output, "sequence", [], {}, 1,
                                              sequence=sequence, frames=6)
            self.assertEqual([value["frame"] for value in record["samples"]], [1, 6])

    def test_success_exit_cannot_reuse_stale_captures(self):
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory)
            (output / "stale.png").write_bytes(b"\x89PNG\r\n\x1a\n")
            (output / "stale.telemetry.json").write_text(json.dumps({"ddgi": batch([0])}))
            result = type("Run", (), {"returncode": 0, "stdout": "", "stderr": ""})()
            with patch.object(features.subprocess, "run", return_value=result):
                with self.assertRaises(OSError):
                    features.run_capture(output / "renderer.exe", output, "stale", [], {}, 1)

    def test_invalid_preset_must_fail_for_expected_reason_before_gpu_initialization(self):
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory)
            for log in ("DDGI preset: unsupported version", "counts must be nonzero\nGPU pass profiler"):
                result = type("Run", (), {"returncode": 1, "stdout": log, "stderr": ""})()
                with patch.object(features.subprocess, "run", return_value=result):
                    with self.assertRaises(AssertionError):
                        features.invalid_capture(output / "renderer.exe", output, "bad", [],
                                                 "counts must be nonzero", {}, 1)

    def test_accepts_noncontiguous_packed_selection_with_complete_ray_sets(self):
        features.check_batch(batch([0, 1, 6, 7]), 8, 4, 32)

    def test_duplicate_out_of_bounds_and_incomplete_probe_sets_fail(self):
        for value in (batch([1, 1]), batch([0, 8]), batch([-1]), batch([True])):
            with self.assertRaises(AssertionError):
                features.check_batch(value, 8, 4, 32)
        value = batch([0, 1])
        value["tracedRays"] = 63
        with self.assertRaisesRegex(AssertionError, "incomplete"):
            features.check_batch(value, 8, 4, 32)

    def test_fairness_bound_accepts_all_probes_and_rejects_priority_starvation(self):
        features.check_motion(motion(), 8, 4)
        samples = motion()
        for sample in samples:
            sample["ddgi"]["selectedProbes"] = [0, 1, 2, 3]
        with self.assertRaisesRegex(AssertionError, "starved"):
            features.check_motion(samples, 8, 4)

    def test_stopped_geometry_must_stop_invalidating_and_finish_fresh_sweep(self):
        samples = motion()
        samples[-1]["ddgi"]["contentEpoch"] = 3
        with self.assertRaisesRegex(AssertionError, "invalidating"):
            features.check_motion(samples, 8, 4)
        samples = motion()
        for sample in samples:
            sample["ddgi"]["refreshRemainingProbes"] = 1
        with self.assertRaisesRegex(AssertionError, "never completed"):
            features.check_motion(samples, 8, 4)

    def test_short_capture_cannot_claim_full_fairness_validation(self):
        with self.assertRaisesRegex(AssertionError, "starvation bound"):
            features.check_motion(motion()[:12], 8, 4)


if __name__ == "__main__":
    unittest.main()
