#!/usr/bin/env python3
"""Prepare a portable native BIM replay and compare its imported witnesses."""

import argparse
import hashlib
import importlib.util
import json
import math
from pathlib import Path
import subprocess
import sys


BACKEND_MODULES = {
    "openusd": ("pxr",),
    "isaac-sim": ("isaacsim",),
    "isaac-lab": ("isaacsim", "isaaclab"),
}


def write_json(path, value):
    temporary = path.with_suffix(".json.tmp")
    temporary.write_text(json.dumps(value, indent=2, allow_nan=False) + "\n", encoding="utf-8", newline="\n")
    temporary.replace(path)


def check_sdk_process(directory, output, backend, timeout):
    # Kit can terminate its own Python process during shutdown. Keep that
    # process separate so its console log and actual exit status are evidence.
    command = [sys.executable, "-B", str(Path(__file__).resolve()), "_sdk-worker",
               "--bundle", str(directory), "--backend", backend, "--output-dir", str(output)]
    log_path = output / "sdk-process.log"
    with log_path.open("wb") as log:
        try:
            process = subprocess.run(command, stdout=log, stderr=subprocess.STDOUT,
                                     stdin=subprocess.DEVNULL, timeout=timeout)
        except subprocess.TimeoutExpired as error:
            raise RuntimeError(f"{backend} exceeded {timeout:g} seconds. See {log_path}") from error
    if process.returncode != 0:
        raise RuntimeError(f"{backend} exited with code {process.returncode}. See {log_path}")
    report_path = output / "sdk" / "sdk-report.json"
    try:
        report = json.loads(report_path.read_bytes())
    except (OSError, ValueError) as error:
        raise RuntimeError(f"{backend} did not write its completion receipt. See {log_path}") from error
    if (not isinstance(report, dict) or report.get("backend") != backend or
            report.get("status") != "passed" or report.get("passed") is not True):
        raise RuntimeError(f"{backend} did not record a passed comparison in {report_path}")
    return report


def check_bundle(directory, output, backend, timeout):
    from robotics_isaac_compare import load_bundle, verify_dependencies, verify_stage

    output.mkdir(parents=True, exist_ok=False)
    receipt = output / "result.json"
    report = {"schema": "container.bim-robotics-isaac-smoke-result", "version": 1,
              "backend": backend, "status": "running"}
    write_json(receipt, report)
    try:
        bundle = load_bundle(directory)
        report["bundleManifestSha256"] = hashlib.sha256((directory / "bundle.json").read_bytes()).hexdigest()
        unavailable = [module for module in BACKEND_MODULES[backend] if importlib.util.find_spec(module) is None]
        if unavailable:
            report.update(status="unavailable", missingModules=unavailable)
            print(f"SKIP {backend} requires {', '.join(unavailable)} in this Python environment", file=sys.stderr)
            return 77
        if backend == "openusd":
            from pxr import Usd

            verify_dependencies(bundle)
            stage = Usd.Stage.Open(str(bundle["stage"]))
            report["runtime"] = {"openusdVersion": list(Usd.GetVersion())}
            report["comparison"] = verify_stage(stage, bundle)
        else:
            report["sdkProcessLog"] = "sdk-process.log"
            report["comparison"] = check_sdk_process(directory, output, backend, timeout)
        load_bundle(directory)
        if hashlib.sha256((directory / "bundle.json").read_bytes()).hexdigest() != report["bundleManifestSha256"]:
            raise RuntimeError("The bundle manifest changed during comparison")
        report["status"] = "passed"
        print(f"Verified {backend} comparison in {receipt}", flush=True)
        return 0
    except KeyboardInterrupt:
        report.update(status="interrupted", error="Comparison interrupted")
        raise
    except Exception as error:
        report.update(status="failed", error=str(error))
        raise
    finally:
        write_json(receipt, report)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True, metavar="{prepare,check}")
    prepare = commands.add_parser("prepare", help="generate the portable fixture using native CPU tools")
    prepare.add_argument("--exporter", type=Path, required=True, help="BimRoboticsUsd executable")
    prepare.add_argument("--simulator", type=Path, required=True, help="BimRoboticsSim executable")
    prepare.add_argument("--collision", type=Path, required=True, help="BimRoboticsCollision executable")
    prepare.add_argument("--bundle", type=Path, required=True, help="new directory for the portable fixture")
    check = commands.add_parser("check", help="compare the imported stage in the selected runtime")
    check.add_argument("--bundle", type=Path, required=True)
    check.add_argument("--backend", choices=BACKEND_MODULES, default="openusd")
    check.add_argument("--output-dir", type=Path, required=True, help="new directory for the comparison evidence")
    check.add_argument("--timeout", type=float, default=900, help="timeout in seconds for the Isaac SDK process")
    worker = commands.add_parser("_sdk-worker")
    worker.add_argument("--bundle", type=Path, required=True)
    worker.add_argument("--backend", choices=("isaac-sim", "isaac-lab"), required=True)
    worker.add_argument("--output-dir", type=Path, required=True)
    args = parser.parse_args()
    try:
        if args.command == "_sdk-worker":
            from robotics_isaac_compare import load_bundle
            from robotics_isaac_sdk import run_sdk

            run_sdk(args.backend, load_bundle(args.bundle.resolve(strict=True)), args.output_dir.resolve(strict=True))
            return 0
        if args.command == "check":
            if not math.isfinite(args.timeout) or args.timeout <= 0:
                parser.error("--timeout must be positive and finite")
            return check_bundle(args.bundle.resolve(strict=True), args.output_dir.resolve(), args.backend, args.timeout)
        from robotics_isaac_fixture import prepare_bundle

        bundle = args.bundle.resolve()
        exporter, simulator, collision = (tool.resolve(strict=True) for tool in
                                          (args.exporter, args.simulator, args.collision))
        bundle.mkdir(parents=True, exist_ok=False)
        prepare_bundle(exporter, simulator, collision, bundle)
        print(f"Prepared native BIM replay at {bundle}", flush=True)
        return 0
    except Exception as error:
        print(f"Isaac smoke {args.command} failed: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
