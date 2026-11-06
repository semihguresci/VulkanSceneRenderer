"""Fast guards for large-fixture result identity, failures and opt-in execution."""

import argparse
import importlib.util
import io
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import Mock, patch


SPEC = importlib.util.spec_from_file_location(
    "scene_browser_scale", Path(__file__).resolve().parents[1] /
    "benchmarks/run_scene_browser_scale.py")
scale = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(scale)


def valid_payload(case="bim_filtered_query"):
    return {"fixture_profile": "compact", "variant": "after", "cases": [{
        "name": case, "elements": 100_000, "verification_requested": True,
        "validation_passed": True,
        "row_interaction_applicable": case in (
            "hierarchy_expanded", "bim_empty_query", "bim_filtered_query"),
        "middle_row_verified": case in ("hierarchy_expanded", "bim_empty_query"),
        "final_row_verified": case in (
            "hierarchy_expanded", "bim_empty_query", "bim_filtered_query"),
        "search_fields_verified": case == "bim_empty_query",
        "cache_reuse_verified": True,
        "unload_reset_verified": case == "bim_empty_query"}]}


class ChildProcess:
    def __init__(self, exit_code=0, running=False):
        self.returncode = None if running else exit_code
        self.pid = 12345
        self.killed = False
        self.waited = False

    def poll(self):
        return self.returncode

    def kill(self):
        self.killed = True
        self.returncode = -9

    def wait(self):
        self.waited = True
        return self.returncode


class SceneBrowserScaleTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.root = Path(self.directory.name)

    def run_child(self, text, exit_code=0, stderr_text="", running=False,
                  case="bim_filtered_query"):
        child = ChildProcess(exit_code, running)

        def start(command, cwd, stdout, stderr):
            self.assertEqual(command[1:], ["100000", "--case", case,
                                          "--verify-large", "--fixture", "compact"])
            stdout.write(text if isinstance(text, bytes) else text.encode("utf-8"))
            stderr.write(stderr_text.encode("utf-8"))
            return child

        memory = Mock()
        memory.result.return_value = {"backend": "test", "successful_samples": 1}
        with patch.object(scale.subprocess, "Popen", side_effect=start), \
                patch.object(scale, "ProcessMemory", return_value=memory), \
                patch.object(scale, "system_memory", return_value={}), \
                patch.object(scale.time, "monotonic", side_effect=iter(range(100))), \
                patch.object(scale.time, "sleep"):
            result = scale.run_case(self.root / "benchmark.exe", "after", 100_000,
                                    case, "compact", self.root, 0.5, 0.05)
        return result, child

    def test_single_matching_verified_case_passes_and_keeps_evidence(self):
        payload = valid_payload()
        result, _ = self.run_child(json.dumps(payload), stderr_text="diagnostic text\n")
        self.assertEqual(result["status"], "ok")
        self.assertEqual(result["benchmark"], payload)
        self.assertEqual(result["exit_code"], 0)
        self.assertEqual(Path(result["stderr_path"]).read_text(), "diagnostic text\n")
        self.assertEqual(json.loads(Path(result["stdout_path"]).read_text()), payload)
        self.assertIn("process_memory", result)

    def test_malformed_or_ambiguous_json_cannot_pass(self):
        for text in ("not JSON", "[]", "{}", '{"cases": {"wrong": 1}}',
                     '{"cases": [null]}', json.dumps({"cases": valid_payload()["cases"] * 2})):
            with self.subTest(text=text):
                result = scale.decode_benchmark_result(text, 100_000, "bim_filtered_query")
                self.assertEqual(result["status"], "invalid_json_result")
                self.assertIn("error", result)

    def test_stale_case_name_or_count_cannot_pass(self):
        for field, value in (("name", "bim_empty_query"), ("elements", 10_001),
                             ("elements", "100000")):
            payload = valid_payload()
            payload["cases"][0][field] = value
            with self.subTest(field=field, value=value):
                result = scale.decode_benchmark_result(json.dumps(payload), 100_000,
                                                        "bim_filtered_query")
                self.assertEqual(result["status"], "invalid_json_result")

    def test_invalid_utf8_output_is_recorded_without_aborting_matrix(self):
        result, _ = self.run_child(b"\xff\xfeinvalid benchmark output")
        self.assertEqual(result["status"], "invalid_json_result")
        self.assertEqual(Path(result["stdout_path"]).read_bytes(),
                         b"\xff\xfeinvalid benchmark output")

    def test_missing_false_or_nonboolean_validation_cannot_pass(self):
        for value in (None, False, 1, "true"):
            payload = valid_payload()
            if value is None:
                del payload["cases"][0]["validation_passed"]
            else:
                payload["cases"][0]["validation_passed"] = value
            with self.subTest(value=value):
                result = scale.decode_benchmark_result(json.dumps(payload), 100_000,
                                                        "bim_filtered_query")
                self.assertEqual(result["status"], "verification_failed")
                self.assertEqual(result["benchmark"], payload)

    def test_closed_view_contract_can_pass_without_claiming_row_interactions(self):
        payload = valid_payload("bim_panel_closed")
        result = scale.decode_benchmark_result(json.dumps(payload), 100_000,
                                               "bim_panel_closed")
        self.assertEqual(result["status"], "ok")
        self.assertFalse(result["benchmark"]["cases"][0]["final_row_verified"])

    def test_fixture_profile_must_match_requested_metadata_profile(self):
        for profile in (None, "rich", "unknown"):
            payload = valid_payload()
            if profile is None:
                del payload["fixture_profile"]
            else:
                payload["fixture_profile"] = profile
            with self.subTest(profile=profile):
                result = scale.decode_benchmark_result(json.dumps(payload), 100_000,
                                                        "bim_filtered_query", "compact")
                self.assertEqual(result["status"], "invalid_json_result")

    def test_each_case_requires_its_specific_boolean_verification_evidence(self):
        flags = ("verification_requested", "row_interaction_applicable",
                 "middle_row_verified", "final_row_verified", "search_fields_verified",
                 "cache_reuse_verified", "unload_reset_verified")
        for case in scale.CASES:
            payload = valid_payload(case)
            self.assertEqual(scale.decode_benchmark_result(
                json.dumps(payload), 100_000, case)["status"], "ok")
            for flag in flags:
                expected = payload["cases"][0][flag]
                for value in (None, not expected, int(expected)):
                    changed = valid_payload(case)
                    if value is None:
                        del changed["cases"][0][flag]
                    else:
                        changed["cases"][0][flag] = value
                    with self.subTest(case=case, flag=flag, value=value):
                        result = scale.decode_benchmark_result(json.dumps(changed), 100_000, case)
                        self.assertEqual(result["status"], "verification_failed")
                        self.assertIn(flag, result["error"])

    def test_baseline_roles_cannot_weaken_current_executable_verification(self):
        for variant in ("before", "before_packed", "before_interning"):
            payload = valid_payload("bim_empty_query")
            payload["variant"] = variant
            if variant == "before":
                payload["cases"][0]["cache_reuse_verified"] = False
                payload["cases"][0]["unload_reset_verified"] = False
            result = scale.decode_benchmark_result(json.dumps(payload), 100_000,
                                                    "bim_empty_query", variant="before")
            self.assertEqual(result["status"], "ok")
            result = scale.decode_benchmark_result(json.dumps(payload), 100_000,
                                                    "bim_empty_query", variant="after")
            self.assertEqual(result["status"], "invalid_json_result")

    def test_nonzero_exit_preserves_code_and_allocation_failure_evidence(self):
        result, child = self.run_child(json.dumps(valid_payload()),
                                       exit_code=0xC0000017,
                                       stderr_text="benchmark: std::bad_alloc\n")
        self.assertEqual(result["status"], "nonzero_exit")
        self.assertEqual(result["exit_code"], 0xC0000017)
        self.assertEqual(result["exit_code_hex"], "0xC0000017")
        self.assertIn("bad_alloc", result["allocation_failure_evidence"])
        self.assertFalse(child.killed)

    def test_deadline_kills_and_reaps_child_without_claiming_oom(self):
        result, child = self.run_child("", stderr_text="fixture started\n", running=True)
        self.assertEqual(result["status"], "timeout")
        self.assertTrue(child.killed)
        self.assertTrue(child.waited)
        self.assertEqual(result["exit_code"], -9)
        self.assertEqual(result["allocation_failure_evidence"], [])
        self.assertEqual(Path(result["stderr_path"]).read_text(), "fixture started\n")

    def test_entity_counts_accept_separators_and_enforce_bounds(self):
        for text, expected in (("100,000", 100_000), ("1_000_000", 1_000_000),
                               ("10000000", 10_000_000)):
            self.assertEqual(scale.entity_count(text), expected)
        for text in ("0", "99", "10_000_001", "-1"):
            with self.subTest(text=text), self.assertRaises(argparse.ArgumentTypeError):
                scale.entity_count(text)

    def test_opt_in_skip_precedes_executable_validation_or_output_creation(self):
        output = self.root / "must-not-exist/results.json"
        for environment_value in (None, "0", "true"):
            environment = ({} if environment_value is None else
                           {"CONTAINER_RUN_LARGE_SCENE_BROWSER_TESTS": environment_value})
            with self.subTest(value=environment_value), \
                    patch.dict(scale.os.environ, environment, clear=True), \
                    patch.object(scale.Path, "is_file") as is_file, \
                    patch.object(scale.subprocess, "Popen") as popen, \
                    patch("sys.stdout", new=io.StringIO()):
                code = scale.main(["--require-opt-in", "--exe", str(self.root / "missing.exe"),
                                   "--output", str(output)])
                self.assertEqual(code, 77)
                is_file.assert_not_called()
                popen.assert_not_called()
            self.assertFalse(output.parent.exists())

    def test_manual_run_needs_no_opt_in_and_flushes_each_case_before_continuing(self):
        executable = self.root / "benchmark.exe"
        executable.write_bytes(b"not launched by this test")
        output = self.root / "results.json"
        calls = []

        def measure(exe, variant, count, case, fixture, logs, timeout, interval):
            previous = json.loads(output.read_text())
            self.assertEqual(len(previous["results"]), len(calls))
            calls.append(case)
            return {"status": "nonzero_exit" if len(calls) == 1 else "ok",
                    "case": case, "elapsed_seconds": 0.0, "exit_code": 1 if len(calls) == 1 else 0}

        with patch.dict(scale.os.environ, {}, clear=True), \
                patch.object(scale, "run_case", side_effect=measure), \
                patch.object(scale.platform, "platform", return_value="unit-test"), \
                patch("sys.stdout", new=io.StringIO()):
            code = scale.main(["--exe", str(executable), "--counts", "100,000",
                               "--cases", "hierarchy_expanded", "bim_filtered_query",
                               "--output", str(output)])
        self.assertEqual(code, 1)
        summary = json.loads(output.read_text())
        self.assertTrue(summary["complete"])
        self.assertFalse(summary["all_passed"])
        self.assertEqual([row["status"] for row in summary["results"]], ["nonzero_exit", "ok"])
        self.assertEqual(summary["counts"], [100_000])
        self.assertEqual(summary["fixture"], "compact")


if __name__ == "__main__":
    unittest.main()
