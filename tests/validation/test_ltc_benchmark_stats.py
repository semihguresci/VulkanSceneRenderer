"""CPU-only tests for repeated LTC timing and baseline comparison."""

from copy import deepcopy
import random
import unittest

from ltc_benchmark import run_order
import ltc_benchmark_stats as stats


def sample(frame, lighting, *, full_frame=False):
    result = {
        "frame": frame,
        "lightingGpuMs": lighting,
        "gBufferGpuMs": .5,
        "timedPassesSumGpuMs": lighting + .5,
    }
    if full_frame:
        result["fullFrameGpuMs"] = lighting + 1.0
    return result


def runs(sampled, ltc, *, full_frame=False):
    result = []
    for repeat, pair in enumerate(zip(sampled, ltc)):
        ordered_pair = tuple(zip(stats.MODES, pair))
        if repeat % 2:
            ordered_pair = ordered_pair[::-1]
        for mode, duration in ordered_pair:
            result.append({
                "mode": mode,
                "repeat": repeat,
                "samples": [sample(frame, duration, full_frame=full_frame)
                            for frame in (49, 53, 57)],
            })
    return result


def report(sampled=(1.0, 1.0, 1.0), ltc=(1.0, 1.0, 1.0), *, full_frame=False):
    return {
        "schemaVersion": stats.SCHEMA_VERSION,
        "device": {
            "gpu": "test GPU", "vendorId": 1, "deviceId": 2,
            "driverVersion": 3, "vulkanApiVersion": "1.4.0",
        },
        "buildConfiguration": "Release",
        "validation": False,
        "provenance": {
            "executableSha256": "baseline",
            "shaderSha256": {"deferred_lighting.frag.spv": "baseline"},
            "ltcTableSha256": {"matrix.bin": "a" * 64, "amplitude.bin": "b" * 64},
        },
        "cases": [{
            "id": "disk-1-metallic",
            "settings": {"count": 1, "warmupFrames": 48, "fixtureSha256": "fixture"},
            "runs": runs(sampled, ltc, full_frame=full_frame),
        }],
    }


class LtcBenchmarkStatisticsTests(unittest.TestCase):
    def compare(self, after, before=None):
        return stats.compare_reports(after, report() if before is None else before,
                                     threshold_percent=5.0, seed=20)

    def test_run_uses_frame_values_for_median_and_interpolated_p95(self):
        timings = stats.summarize_run([sample(49, 1), sample(53, 2), sample(57, 100)])
        self.assertEqual(timings["lightingGpuMs"]["median"], 2)
        self.assertAlmostEqual(timings["lightingGpuMs"]["p95"], 90.2)
        self.assertNotIn(stats.FULL_FRAME_METRIC, timings)
        self.assertIn(stats.FULL_FRAME_METRIC, stats.summarize_run(
            [sample(49, 1, full_frame=True)]))

    def test_summary_retains_raw_samples_order_and_paired_ratios(self):
        inputs = runs((1.0, 3.0, 100.0), (1.5, 3.0, 2.0))
        output = stats.summarize_case(inputs, seed=3)
        self.assertEqual(output, stats.summarize_case(inputs, seed=3))
        self.assertEqual(output["repeatCount"], 3)
        self.assertEqual([run["mode"] for run in output["runs"]],
                         ["sampled", "ltc", "ltc", "sampled", "sampled", "ltc"])
        self.assertEqual([run["samples"] for run in output["runs"]],
                         [run["samples"] for run in inputs])
        paired = output["summary"]["ltcOverSampled"]["lightingGpuMs"]
        self.assertEqual(paired["median"], 1.0)
        self.assertEqual(paired["interval95"], [.02, 1.5])
        self.assertEqual(output["runs"][0]["metrics"]["lightingGpuMs"]["p95"], 1.0)

    def test_partial_pass_sum_stays_descriptive_when_pass_coverage_varies(self):
        metric = "timedPassesSumGpuMs"
        current = report(full_frame=True)
        raw = current["cases"][0]["runs"]
        for run in raw:
            for timing in run["samples"]:
                timing["timedPasses"] = ["GBuffer", "Lighting"]
                if run["mode"] == "ltc":
                    timing["timedPasses"].append("PostProcess")
                    timing[metric] += 4
        summary = stats.summarize_case(raw, seed=3)
        self.assertEqual(summary["runs"][1]["samples"], raw[1]["samples"])
        self.assertEqual(summary["runs"][1]["metrics"][metric]["p95"], 5.5)
        self.assertEqual(summary["summary"]["ltc"][metric]["median"], 5.5)
        self.assertEqual(set(summary["summary"]["ltcOverSampled"]),
                         set(stats.COMPARABLE_METRICS))
        comparison = self.compare(current, report(full_frame=True))
        self.assertEqual({row["metric"] for row in comparison["cases"][0]["metrics"]},
                         set(stats.COMPARABLE_METRICS))

    def test_invalid_samples_and_pairs_fail(self):
        for samples in (
            [], [sample(49, float("inf"))], [sample(49, float("nan"))],
            [sample(49, -1)], [sample(49, True)],
            [{**sample(49, 1), "lightingGpuMs": 10 ** 1000}],
            [sample(49, 1), sample(49, 1)],
            [sample(49, 1), sample(53, 1, full_frame=True)],
        ):
            with self.subTest(samples=samples):
                with self.assertRaises(ValueError):
                    stats.summarize_run(samples)
        pair = runs((1,), (1,))
        for invalid in ([pair[0]], pair + [pair[1]], [None]):
            with self.subTest(runs=invalid):
                with self.assertRaises(ValueError):
                    stats.summarize_case(invalid, seed=0)
        pair[0]["samples"][0]["frame"] = 48
        with self.assertRaisesRegex(ValueError, "same frames"):
            stats.summarize_case(pair, seed=0)

    def test_large_frame_count_cannot_substitute_for_process_repeats(self):
        current = report((1, 1), (2, 2))
        for run in current["cases"][0]["runs"]:
            run["samples"] = [sample(frame, run["samples"][0]["lightingGpuMs"])
                              for frame in range(49, 249)]
        summarized = stats.summarize_case(current["cases"][0]["runs"], seed=5)
        self.assertIsNone(summarized["summary"]["ltc"]["lightingGpuMs"]["interval95"])
        baseline = deepcopy(current)
        for run in baseline["cases"][0]["runs"]:
            run["samples"] = [sample(frame, 1) for frame in range(49, 249)]
        comparison = self.compare(current, baseline)
        self.assertEqual(comparison["status"], "inconclusive")
        guard = next(row for row in comparison["cases"][0]["metrics"] if row["gates"])
        self.assertIsNone(guard["changeInterval95Percent"])

    def test_guard_uses_uncertainty_and_ignores_nongating_context(self):
        variants = (
            ((1.20, 1.20, 1.20), "regressed"),
            ((1.00, 1.20, 1.30), "inconclusive"),
            ((1.00, 1.02, 1.05), "within_threshold"),
        )
        for ltc, status in variants:
            with self.subTest(ltc=ltc):
                result = self.compare(report((2.0, 2.0, 2.0), ltc))
                self.assertEqual(result["status"], status)
                rows = result["cases"][0]["metrics"]
                guard, = [row for row in rows if row["gates"]]
                self.assertEqual((guard["mode"], guard["metric"]), ("ltc", "lightingGpuMs"))
                self.assertEqual(guard["status"], status)
                sampled = next(row for row in rows if row["mode"] == "sampled"
                               and row["metric"] == "lightingGpuMs")
                self.assertEqual(sampled["status"], "regressed")
                self.assertFalse(sampled["gates"])

    def test_both_builds_contribute_to_comparison_uncertainty(self):
        current = report(ltc=(1.20, 1.20, 1.20))
        baseline = report(ltc=(.95, 1.0, 1.2))
        result = self.compare(current, baseline)
        self.assertEqual(result["status"], "inconclusive")
        guard = next(row for row in result["cases"][0]["metrics"] if row["gates"])
        self.assertAlmostEqual(guard["changePercent"], 20.0)
        self.assertAlmostEqual(guard["changeInterval95Percent"][0], 0.0)
        self.assertGreater(guard["changeInterval95Percent"][1], 20.0)

    def test_report_identity_uses_device_fixture_protocol_and_frames(self):
        baseline = report()
        for key in stats.DEVICE_FIELDS:
            current = deepcopy(baseline)
            current["device"][key] = "changed"
            with self.subTest(device_field=key):
                with self.assertRaisesRegex(ValueError, "identity"):
                    self.compare(current, baseline)
        for field in ("fixtureSha256", "warmupFrames"):
            current = deepcopy(baseline)
            current["cases"][0]["settings"][field] = "changed"
            with self.subTest(setting=field):
                with self.assertRaisesRegex(ValueError, "settings differ"):
                    self.compare(current, baseline)
        current = deepcopy(baseline)
        for run in current["cases"][0]["runs"]:
            run["samples"][0]["frame"] -= 1
        with self.assertRaisesRegex(ValueError, "sample frames differ"):
            self.compare(current, baseline)

    def test_comparison_accepts_new_build_and_a_subset_of_baseline_cases(self):
        current = report(full_frame=True)
        current["provenance"]["executableSha256"] = "updated"
        current["provenance"]["shaderSha256"] = {"deferred_lighting.frag.spv": "updated"}
        baseline = report()
        baseline["cases"].append({**baseline["cases"][0], "id": "additional-case"})
        result = self.compare(current, baseline)
        self.assertEqual(result["status"], "within_threshold")
        self.assertEqual([case["id"] for case in result["cases"]], ["disk-1-metallic"])
        self.assertTrue(all(row["metric"] != stats.FULL_FRAME_METRIC
                            for row in result["cases"][0]["metrics"]))
        complete = self.compare(current, report(full_frame=True))
        self.assertTrue(any(row["metric"] == stats.FULL_FRAME_METRIC
                            for row in complete["cases"][0]["metrics"]))

    def test_ltc_table_hashes_must_be_present_valid_and_match(self):
        current = report()
        for name in stats.LTC_TABLES:
            for digest in (None, "", "x" * 64, "c" * 64):
                with self.subTest(table=name, digest=digest):
                    baseline = report()
                    tables = baseline["provenance"]["ltcTableSha256"]
                    if digest is None:
                        del tables[name]
                    else:
                        tables[name] = digest
                    with self.assertRaisesRegex(ValueError, "LTC table SHA-256"):
                        self.compare(current, baseline)
        del current["provenance"]
        with self.assertRaisesRegex(ValueError, "current report is missing LTC table"):
            self.compare(current)

    def test_historical_or_validation_enabled_data_cannot_gate(self):
        for invalid in ({}, {**report(), "schemaVersion": True},
                        {**report(), "validation": True},
                        {**report(), "buildConfiguration": "Debug"}):
            with self.subTest(invalid=invalid):
                with self.assertRaises(ValueError):
                    self.compare(report(), invalid)


class RunOrderTests(unittest.TestCase):
    def test_alternate_balances_and_alternates_pair_starts(self):
        order = run_order(6, "alternate", random.Random(71))
        self.assertEqual(order.count(stats.MODES), 3)
        self.assertTrue(all(first[0] != second[0] for first, second in zip(order, order[1:])))

    def test_random_is_seeded_and_balanced(self):
        order = run_order(7, "random", random.Random(71))
        self.assertEqual(order, run_order(7, "random", random.Random(71)))
        self.assertTrue(all(set(pair) == set(stats.MODES) for pair in order))
        self.assertIn(order.count(stats.MODES), (3, 4))


if __name__ == "__main__":
    unittest.main()
