"""Run the opt-in native robotics GPU probes from an already built CTest tree.

The two default probes require Vulkan and its validation layer. The optional
physics demo also requires ray-query support, staged Cornell assets, Pillow
and NumPy. Nothing is built or downloaded. Every invocation needs a new output
directory, and skipped CTest cases fail the smoke run.
"""

import argparse
from collections import Counter
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import xml.etree.ElementTree as ET


CORE_TESTS = ("bim_sensor_camera_gpu_regression", "bim_robotics_dataset_pipeline_gpu_regression")
PHYSICS_TEST = "physics_runtime_regression"
OPT_IN = {"CONTAINER_RUN_GPU_SENSOR_CAMERA": "1", "CONTAINER_RUN_GPU_ROBOTICS_DATASET": "1"}
EXECUTABLE_FLAGS = {
    CORE_TESTS[0]: ("--renderer",),
    CORE_TESTS[1]: ("--renderer", "--simulator", "--recorder"),
    PHYSICS_TEST: ("--exe",),
}


class SmokeFailure(RuntimeError):
    pass


def write_json(path, value):
    with path.open("x", encoding="utf-8") as stream:
        json.dump(value, stream, indent=2)
        stream.write("\n")


def argument(command, flag):
    if command.count(flag) != 1:
        raise SmokeFailure(f"CTest command must contain exactly one {flag}: {command}")
    index = command.index(flag)
    if index + 1 == len(command):
        raise SmokeFailure(f"CTest command has no value for {flag}")
    return command[index + 1]


def registrations(document, expected, build):
    if not isinstance(document, dict) or not isinstance(document.get("tests"), list):
        raise SmokeFailure("CTest listing has no tests array")
    rows = document["tests"]
    names = [row.get("name") for row in rows if isinstance(row, dict)]
    if not all(isinstance(name, str) for name in names) or Counter(names) != Counter(expected) or len(rows) != len(expected):
        raise SmokeFailure(f"CTest registrations must be exactly {list(expected)}, found {names}")
    outputs = {}
    for row in rows:
        name, command = row["name"], row.get("command")
        if not isinstance(command, list) or not command or not all(isinstance(part, str) for part in command):
            raise SmokeFailure(f"CTest registration {name} has no runnable command")
        for flag in EXECUTABLE_FLAGS[name]:
            executable = Path(argument(command, flag))
            if not executable.is_absolute():
                executable = build / executable
            if not executable.is_file():
                raise SmokeFailure(f"Build the required executable first: {executable}")
        if "--managed-output" not in command:
            raise SmokeFailure(f"Reconfigure {name} with --managed-output to preserve existing captures")
        output = Path(argument(command, "--output"))
        if not output.is_absolute():
            output = build / output
        output = output.resolve()
        if not output.is_relative_to(build):
            raise SmokeFailure(f"CTest capture output must be inside the build tree: {output}")
        outputs[name] = output
    return outputs


def junit_results(path, expected):
    try:
        root = ET.parse(path).getroot()
    except (OSError, ET.ParseError) as error:
        raise SmokeFailure(f"CTest did not produce readable JUnit evidence: {error}") from error
    if root.tag not in ("testsuite", "testsuites"):
        raise SmokeFailure(f"Unexpected JUnit root: {root.tag}")
    cases = list(root.iter("testcase"))
    names = [case.get("name") for case in cases]
    if Counter(names) != Counter(expected):
        raise SmokeFailure(f"JUnit cases must be exactly {list(expected)}, found {names}")
    failures, results = [], []
    for suite in root.iter():
        if suite.tag not in ("testsuite", "testsuites"):
            continue
        for field in ("failures", "errors", "skipped", "disabled"):
            value = suite.get(field, "0")
            if not value.isdecimal():
                raise SmokeFailure(f"Invalid JUnit {field} count: {value}")
            if int(value):
                failures.append(f"JUnit reports {value} {field}")
    for case in cases:
        name, status = case.get("name"), case.get("status", "run")
        problems = [node.tag for node in case if node.tag in ("skipped", "failure", "error")]
        if status != "run":
            problems.append(f"status={status}")
        if problems:
            failures.append(f"{name}: {', '.join(problems)}")
        results.append({"name": name, "status": status, "seconds": case.get("time"), "problems": problems})
    if failures:
        raise SmokeFailure("GPU smoke requires every test to run and pass: " + "; ".join(failures))
    return results


def capture_snapshot(outputs):
    return {name: set(path.iterdir()) if path.is_dir() else set() for name, path in outputs.items()}


def save_new_captures(outputs, before, destination):
    saved = []
    for name, source in outputs.items():
        if not source.is_dir():
            continue
        for path in sorted(set(source.iterdir()) - before[name]):
            if path.is_symlink() or not path.is_dir():
                raise SmokeFailure(f"Unexpected managed capture artifact: {path}")
            # The registered harnesses create fresh run-* directories. Do not
            # collect unrelated directories from a concurrent build operation.
            if not path.name.startswith("run-"):
                continue
            target = destination / "captures" / name / path.name
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copytree(path, target, symlinks=True)
            saved.append(str(target.relative_to(destination)))
    return saved


def run_smoke(build_dir, config, output, include_physics_demo=False):
    build, destination = Path(build_dir).resolve(), Path(output).absolute()
    # Reserve before writing anything. A rejected repeated invocation leaves
    # all previous evidence intact, including a symlink at the destination.
    destination.mkdir(parents=True, exist_ok=False)
    expected = CORE_TESTS + ((PHYSICS_TEST,) if include_physics_demo else ())
    environment = dict(os.environ)
    environment.update(OPT_IN)
    if include_physics_demo:
        environment["CONTAINER_RUN_GPU_PHYSICS"] = "1"
    report = {"schema": "container.robotics-gpu-smoke", "version": 1, "passed": False,
              "buildDirectory": str(build), "configuration": config, "tests": list(expected),
              "includePhysicsDemo": include_physics_demo, "captures": []}
    outputs, before, executed = {}, {}, False
    failure = None
    try:
        if not build.is_dir() or not (build / "CTestTestfile.cmake").is_file():
            raise SmokeFailure(f"Configure an existing CTest build first: {build}")
        selection = "^(" + "|".join(map(re.escape, expected)) + ")$"
        common = ["ctest", "--test-dir", str(build), "-C", config, "-R", selection]
        listing_command = [*common, "--show-only=json-v1"]
        report["listingCommand"] = listing_command
        try:
            listing = subprocess.run(listing_command, env=environment, capture_output=True, text=True,
                                     encoding="utf-8", errors="replace", timeout=30,
                                     creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
        except subprocess.TimeoutExpired as error:
            with (destination / "ctest-listing.log").open("x", encoding="utf-8") as stream:
                for value in (error.stdout, error.stderr):
                    if value:
                        stream.write(value.decode("utf-8", errors="replace") if isinstance(value, bytes) else value)
            raise SmokeFailure("CTest registration lookup timed out after 30 seconds") from error
        with (destination / "ctest-listing.log").open("x", encoding="utf-8") as stream:
            stream.write(listing.stdout)
            stream.write(listing.stderr)
        if listing.returncode:
            raise SmokeFailure(f"CTest registration lookup failed with exit code {listing.returncode}")
        try:
            document = json.loads(listing.stdout)
        except json.JSONDecodeError as error:
            raise SmokeFailure(f"CTest registration lookup returned invalid JSON: {error}") from error
        write_json(destination / "ctest-listing.json", document)
        outputs = registrations(document, expected, build)
        before = capture_snapshot(outputs)
        report["captureDirectories"] = {name: str(path) for name, path in outputs.items()}
        junit = destination / "robotics-gpu.junit.xml"
        command = [*common, "--parallel", "1", "--no-tests=error", "--output-on-failure",
                   "--output-junit", str(junit), "--output-log", str(destination / "ctest.log")]
        report["testCommand"] = command
        with (destination / "ctest.stdout.log").open("x", encoding="utf-8") as stream:
            executed = True
            result = subprocess.run(command, env=environment, stdout=stream, stderr=subprocess.STDOUT,
                                    timeout=1560 + (1200 if include_physics_demo else 0),
                                    creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
        report["ctestExitCode"] = result.returncode
        report["results"] = junit_results(junit, expected)
        if result.returncode:
            raise SmokeFailure(f"CTest exited with code {result.returncode}, see ctest.stdout.log")
        report["passed"] = True
    except (OSError, ValueError, SmokeFailure, subprocess.SubprocessError) as error:
        failure = str(error)
    finally:
        if executed:
            try:
                report["captures"] = save_new_captures(outputs, before, destination)
            except (OSError, SmokeFailure) as error:
                failure = f"{failure + '; ' if failure else ''}Saving capture evidence failed: {error}"
        if failure:
            report["passed"] = False
            report["failure"] = failure
        write_json(destination / "summary.json", report)
    if failure:
        print(f"FAIL robotics GPU smoke: {failure}\nEvidence: {destination}", file=sys.stderr)
        return 1
    print(f"PASS robotics GPU smoke ({len(expected)} non-skipped CTest cases)\nEvidence: {destination}")
    return 0


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", required=True, type=Path)
    parser.add_argument("--config", default="Release")
    parser.add_argument("--output", required=True, type=Path, help="New directory for logs, JUnit and capture evidence")
    parser.add_argument("--include-physics-demo", action="store_true",
                        help="Also run the forward/deferred raster and ray+TAA Jolt demo (Pillow/NumPy required)")
    args = parser.parse_args(argv)
    try:
        return run_smoke(args.build_dir, args.config, args.output, args.include_physics_demo)
    except OSError as error:
        print(f"FAIL robotics GPU smoke: cannot reserve output directory: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
