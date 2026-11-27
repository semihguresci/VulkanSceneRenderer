"""CPU-only runner contracts. CTest and the renderer are never launched."""

import contextlib
import io
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest import mock
import xml.etree.ElementTree as ET

import run_robotics_gpu_smoke as smoke


def write_junit(path, names, problems=None, suite_attributes=None):
    root = ET.Element("testsuite", {"tests": str(len(names)), "failures": "0", "errors": "0", "skipped": "0"})
    root.attrib.update(suite_attributes or {})
    for name in names:
        settings = (problems or {}).get(name, {})
        case = ET.SubElement(root, "testcase", {"name": name, "status": settings.get("status", "run"), "time": "0.01"})
        if settings.get("child"):
            ET.SubElement(case, settings["child"], {"message": "stub evidence"})
    ET.ElementTree(root).write(path, encoding="utf-8", xml_declaration=True)


class RoboticsGpuSmokeRunnerTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="robotics-gpu-runner-")
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.build = self.root / "build"
        self.build.mkdir()
        (self.build / "CTestTestfile.cmake").write_text("# configured stub\n", encoding="utf-8")
        self.executables = {}
        for name in ("renderer", "simulator", "recorder"):
            path = self.build / (name + ".exe")
            path.write_text("stub already-built executable\n", encoding="utf-8")
            self.executables[name] = path
        self.output = self.root / "evidence"
        self.listing = {"kind": "ctestInfo", "version": {"major": 1, "minor": 0},
                        "tests": [self.registration(name) for name in smoke.CORE_TESTS]}
        self.list_code, self.run_code = 0, 0
        self.list_stdout = None
        self.problems = {}
        self.emit_junit = True
        self.run_timeout = False
        self.calls = []
        self.mock_run = mock.patch.object(smoke.subprocess, "run", side_effect=self.ctest).start()
        self.addCleanup(mock.patch.stopall)

    def registration(self, name):
        output = self.build / "test_results" / name
        command = [sys.executable, "stub-capture.py"]
        for flag in smoke.EXECUTABLE_FLAGS[name]:
            role = {"--renderer": "renderer", "--simulator": "simulator", "--recorder": "recorder", "--exe": "renderer"}[flag]
            command.extend((flag, str(self.executables[role])))
        command.extend(("--output", str(output), "--managed-output"))
        return {"name": name, "command": command}

    def ctest(self, command, **kwargs):
        self.calls.append((command, kwargs))
        if "--show-only=json-v1" in command:
            return subprocess.CompletedProcess(command, self.list_code,
                                               self.list_stdout if self.list_stdout is not None else json.dumps(self.listing),
                                               "stub listing diagnostic\n")
        kwargs["stdout"].write("stub CTest runtime output\n")
        for row in self.listing["tests"]:
            output = Path(smoke.argument(row["command"], "--output")) / "run-new"
            output.mkdir(parents=True)
            (output / "capture.log").write_text("native capture diagnostic\n", encoding="utf-8")
            (output / "results.json").write_text('{"passed":true}\n', encoding="utf-8")
        if self.run_timeout:
            raise subprocess.TimeoutExpired(command, kwargs["timeout"])
        Path(smoke.argument(command, "--output-log")).write_text("stub detailed CTest log\n", encoding="utf-8")
        if self.emit_junit:
            write_junit(Path(smoke.argument(command, "--output-junit")),
                        [row["name"] for row in self.listing["tests"]], self.problems)
        return subprocess.CompletedProcess(command, self.run_code)

    def run_smoke(self, *, physics=False, output=None):
        with contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
            return smoke.main(["--build-dir", str(self.build), "--config", "Release", "--output", str(output or self.output),
                               *(["--include-physics-demo"] if physics else [])])

    def report(self):
        return json.loads((self.output / "summary.json").read_text(encoding="utf-8"))

    def test_runs_exact_native_probes_and_saves_non_skipped_evidence(self):
        self.assertEqual(self.run_smoke(), 0)
        report = self.report()
        self.assertTrue(report["passed"])
        self.assertEqual(report["tests"], list(smoke.CORE_TESTS))
        self.assertEqual(len(report["results"]), 2)
        self.assertEqual(len(report["captures"]), 2)
        self.assertEqual(len(self.calls), 2)
        command = self.calls[1][0]
        self.assertEqual(smoke.argument(command, "--parallel"), "1")
        self.assertEqual(smoke.argument(command, "-C"), "Release")
        self.assertIn("--no-tests=error", command)
        self.assertEqual(smoke.argument(command, "-R"),
                         "^(bim_sensor_camera_gpu_regression|bim_robotics_dataset_pipeline_gpu_regression)$")
        for relative in report["captures"]:
            self.assertEqual((self.output / relative / "capture.log").read_text(encoding="utf-8"), "native capture diagnostic\n")
        for name in ("ctest-listing.json", "ctest-listing.log", "ctest.log", "ctest.stdout.log", "robotics-gpu.junit.xml"):
            self.assertTrue((self.output / name).is_file(), name)

    def test_environment_is_preserved_and_opt_in_is_internal(self):
        with mock.patch.dict(os.environ, {"ROBOTICS_SMOKE_SENTINEL": "preserved", "VK_LAYER_PATH": "existing-sdk-layers",
                                          "CONTAINER_RUN_GPU_SENSOR_CAMERA": "0"}, clear=False):
            before = dict(os.environ)
            self.assertEqual(self.run_smoke(), 0)
            self.assertEqual(dict(os.environ), before)
        for _, kwargs in self.calls:
            self.assertEqual(kwargs["env"]["ROBOTICS_SMOKE_SENTINEL"], "preserved")
            self.assertEqual(kwargs["env"]["VK_LAYER_PATH"], "existing-sdk-layers")
            for key in smoke.OPT_IN:
                self.assertEqual(kwargs["env"][key], "1")

    def test_existing_destination_is_rejected_without_any_writes(self):
        self.output.mkdir()
        sentinel = self.output / "keep.txt"
        sentinel.write_text("do not replace", encoding="utf-8")
        self.assertEqual(self.run_smoke(), 1)
        self.mock_run.assert_not_called()
        self.assertEqual(list(self.output.iterdir()), [sentinel])
        self.assertEqual(sentinel.read_text(encoding="utf-8"), "do not replace")

    def test_optional_physics_demo_uses_registered_managed_output_and_keeps_prior_runs(self):
        registration = self.registration(smoke.PHYSICS_TEST)
        self.listing["tests"].append(registration)
        source = Path(smoke.argument(registration["command"], "--output"))
        old = source / "run-old"
        old.mkdir(parents=True)
        (old / "keep.txt").write_text("old evidence", encoding="utf-8")
        self.assertEqual(self.run_smoke(physics=True), 0)
        self.assertEqual(self.report()["tests"], [*smoke.CORE_TESTS, smoke.PHYSICS_TEST])
        self.assertEqual(len(self.report()["captures"]), 3)
        self.assertTrue(all("run-old" not in path for path in self.report()["captures"]))
        self.assertEqual((old / "keep.txt").read_text(encoding="utf-8"), "old evidence")
        for _, kwargs in self.calls:
            self.assertEqual(kwargs["env"]["CONTAINER_RUN_GPU_PHYSICS"], "1")

    def test_missing_or_duplicate_registration_is_rejected_before_runtime(self):
        self.listing["tests"] = [self.listing["tests"][0], self.listing["tests"][0]]
        self.assertEqual(self.run_smoke(), 1)
        self.assertEqual(len(self.calls), 1)
        self.assertIn("registrations must be exactly", self.report()["failure"])

    def test_legacy_fixed_capture_output_is_rejected_before_runtime(self):
        self.listing["tests"][0]["command"].remove("--managed-output")
        self.assertEqual(self.run_smoke(), 1)
        self.assertEqual(len(self.calls), 1)
        self.assertIn("Reconfigure", self.report()["failure"])

    def test_unbuilt_required_executable_is_rejected_before_runtime(self):
        self.executables["recorder"].unlink()
        self.assertEqual(self.run_smoke(), 1)
        self.assertEqual(len(self.calls), 1)
        self.assertIn("Build the required executable", self.report()["failure"])

    def test_registration_output_outside_build_is_rejected(self):
        command = self.listing["tests"][0]["command"]
        command[command.index("--output") + 1] = str(self.root / "external")
        self.assertEqual(self.run_smoke(), 1)
        self.assertEqual(len(self.calls), 1)
        self.assertIn("inside the build tree", self.report()["failure"])

    def test_listing_failure_keeps_diagnostic_log(self):
        self.list_code = 1
        self.assertEqual(self.run_smoke(), 1)
        self.assertIn("stub listing diagnostic", (self.output / "ctest-listing.log").read_text(encoding="utf-8"))
        self.assertEqual(len(self.calls), 1)

    def test_invalid_listing_json_fails_without_runtime(self):
        self.list_stdout = "not CTest JSON"
        self.assertEqual(self.run_smoke(), 1)
        self.assertIn("invalid JSON", self.report()["failure"])
        self.assertEqual(len(self.calls), 1)

    def test_malformed_registration_name_is_reported_without_runtime(self):
        self.listing["tests"][0]["name"] = {"invalid": "name"}
        self.assertEqual(self.run_smoke(), 1)
        self.assertIn("registrations must be exactly", self.report()["failure"])
        self.assertEqual(len(self.calls), 1)

    def test_listing_timeout_saves_partial_diagnostics(self):
        self.mock_run.side_effect = subprocess.TimeoutExpired(["ctest"], 30, output=b"partial CTest listing\n")
        self.assertEqual(self.run_smoke(), 1)
        self.assertIn("timed out", self.report()["failure"])
        self.assertIn("partial CTest listing", (self.output / "ctest-listing.log").read_text(encoding="utf-8"))

    def test_skipped_runtime_is_failure_even_when_ctest_exits_zero(self):
        self.problems[smoke.CORE_TESTS[0]] = {"child": "skipped"}
        self.assertEqual(self.run_smoke(), 1)
        report = self.report()
        self.assertFalse(report["passed"])
        self.assertIn("skipped", report["failure"])
        self.assertEqual(len(report["captures"]), 2)
        self.assertTrue((self.output / "robotics-gpu.junit.xml").is_file())
        self.assertIn("stub CTest runtime output", (self.output / "ctest.stdout.log").read_text(encoding="utf-8"))

    def test_ctest_failure_cannot_pass_using_successful_junit(self):
        self.run_code = 8
        self.assertEqual(self.run_smoke(), 1)
        self.assertIn("code 8", self.report()["failure"])
        self.assertEqual(self.report()["ctestExitCode"], 8)
        self.assertTrue((self.output / "ctest.log").is_file())

    def test_missing_junit_cannot_pass(self):
        self.emit_junit = False
        self.assertEqual(self.run_smoke(), 1)
        self.assertIn("readable JUnit", self.report()["failure"])
        self.assertEqual(len(self.report()["captures"]), 2)

    def test_runtime_timeout_keeps_partial_stdout_and_captures(self):
        self.run_timeout = True
        self.assertEqual(self.run_smoke(), 1)
        self.assertIn("timed out", self.report()["failure"])
        self.assertEqual(len(self.report()["captures"]), 2)
        self.assertTrue((self.output / "ctest.stdout.log").is_file())

    def test_junit_rejects_failure_error_notrun_missing_and_duplicate_cases(self):
        path = self.root / "junit.xml"
        for problem in ({"child": "failure"}, {"child": "error"}, {"status": "notrun"}):
            with self.subTest(problem=problem):
                write_junit(path, smoke.CORE_TESTS, {smoke.CORE_TESTS[0]: problem})
                with self.assertRaises(smoke.SmokeFailure):
                    smoke.junit_results(path, smoke.CORE_TESTS)
        for names in ((smoke.CORE_TESTS[0],), (smoke.CORE_TESTS[0], smoke.CORE_TESTS[0]), (*smoke.CORE_TESTS, "unexpected")):
            with self.subTest(names=names):
                write_junit(path, names)
                with self.assertRaisesRegex(smoke.SmokeFailure, "cases must be exactly"):
                    smoke.junit_results(path, smoke.CORE_TESTS)

    def test_junit_rejects_aggregate_skip_and_malformed_xml(self):
        path = self.root / "junit.xml"
        write_junit(path, smoke.CORE_TESTS, suite_attributes={"skipped": "1"})
        with self.assertRaisesRegex(smoke.SmokeFailure, "skipped"):
            smoke.junit_results(path, smoke.CORE_TESTS)
        path.write_text("<truncated", encoding="utf-8")
        with self.assertRaisesRegex(smoke.SmokeFailure, "readable JUnit"):
            smoke.junit_results(path, smoke.CORE_TESTS)


if __name__ == "__main__":
    unittest.main()
