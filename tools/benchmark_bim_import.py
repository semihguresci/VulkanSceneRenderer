#!/usr/bin/env python3
"""Compare two BimImportBenchmark builds on identical inputs."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
from statistics import median
import subprocess
import sys


def file_identity(path):
    with path.open("rb") as stream:
        digest = hashlib.file_digest(stream, "sha256").hexdigest()
    return {"path": str(path), "bytes": path.stat().st_size, "sha256": digest}


def positive_int(value):
    number = int(value)
    if number < 1:
        raise argparse.ArgumentTypeError("must be a positive integer")
    return number


def run_import(executable, model, iterations):
    result = subprocess.run(
        [str(executable), str(model), str(iterations)],
        capture_output=True, encoding="utf-8", errors="replace", check=False,
    )
    if result.returncode:
        raise RuntimeError(f"{executable} failed ({result.returncode}) for {model}\n{result.stderr}")
    return json.loads(result.stdout)


def build_identity(executable, build_dir, configuration):
    identity = file_identity(executable)
    if build_dir is None:
        build_dir = next((p for p in executable.parents if (p / "CMakeCache.txt").is_file()), None)
    if build_dir is None:
        return identity
    cache_file = build_dir / "CMakeCache.txt"
    cache = {}
    for line in cache_file.read_text(encoding="utf-8").splitlines():
        if line.startswith(("//", "#")) or "=" not in line:
            continue
        key_and_type, value = line.split("=", 1)
        key = key_and_type.split(":", 1)[0]
        if key in {"CMAKE_CXX_FLAGS", f"CMAKE_CXX_FLAGS_{configuration.upper()}",
                   "CMAKE_MAP_IMPORTED_CONFIG_DEBUG", "CMAKE_MSVC_RUNTIME_LIBRARY",
                   "CMAKE_GENERATOR"} or key.startswith("ENABLE_"):
            cache[key] = value
    identity["cmakeCache"] = cache
    commands = build_dir / "compile_commands.json"
    if commands.is_file():
        measured_sources = {
            "bim_import_benchmark.cpp", "bim_import_allocation_counter.cpp",
            "BimManager.cpp", "BimRelationshipGraph.cpp", "IfcTessellatedLoader.cpp", "IfcxLoader.cpp",
        }
        identity["compileCommands"] = [
            entry for entry in json.loads(commands.read_text(encoding="utf-8"))
            if Path(entry["file"]).name in measured_sources
        ]
    return identity


def summarize(runs, repeat):
    samples = [sample for run in runs for sample in run["samples"]
               if (sample["iteration"] > 1) == repeat]
    if not samples:
        return None
    memory = {}
    for key in samples[0]["processMemory"]:
        values = [sample["processMemory"][key] for sample in samples
                  if sample["processMemory"][key] is not None]
        if values:
            memory[key] = median(values)
    summary = {
        "samples": len(samples),
        "medianElapsedMs": median(s["elapsedMs"] for s in samples),
        "medianStageMs": {key: median(s["stagesMs"][key] for s in samples)
                          for key in samples[0]["stagesMs"]},
        "medianProcessMemory": memory,
    }
    if all("stagesAllocations" in sample for sample in samples):
        summary["medianStageAllocations"] = {
            stage: {key: median(s["stagesAllocations"][stage][key] for s in samples)
                    for key in counters}
            for stage, counters in samples[0]["stagesAllocations"].items()
        }
    return summary


def compare_model(executables, model, iterations, processes, index):
    initial_stat = model.stat()
    samples = {name: [] for name in executables}
    process_order = []
    for process in range(processes):
        order = list(executables)
        if (process + index) % 2:
            order.reverse()
        process_order.append(order)
        for name in order:
            samples[name].append(run_import(executables[name], model, iterations))
    final_stat = model.stat()
    if (initial_stat.st_size, initial_stat.st_mtime_ns) != (final_stat.st_size, final_stat.st_mtime_ns):
        raise RuntimeError(f"model changed during the benchmark: {model}")

    reference = samples["before"][0]
    comparable_fields = ["configuration", "compiler", "input", "loadedInput", "scale", "semantics"]
    differences = [f"{name} process {i}: {field}" for name, runs in samples.items()
                   for i, run in enumerate(runs, 1) for field in comparable_fields
                   if reference[field] != run[field]]
    has_allocations = "stagesAllocations" in reference["samples"][0]
    for name, runs in samples.items():
        for i, run in enumerate(runs, 1):
            if any(("stagesAllocations" in sample) != has_allocations for sample in run["samples"]):
                differences.append(f"{name} process {i}: allocation instrumentation")
    loaded = Path(reference["loadedInput"]).resolve()
    model_file = file_identity(model)
    return {
        "requestedSource": model_file,
        "loadedSource": model_file if loaded == model else file_identity(loaded),
        "processOrder": process_order,
        "differences": differences,
        "runs": samples,
        "summaries": {name: {"freshProcess": summarize(runs, False),
                             "repeatInProcess": summarize(runs, True)}
                      for name, runs in samples.items()},
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--before", required=True, type=Path, help="baseline BimImportBenchmark executable")
    parser.add_argument("--after", required=True, type=Path, help="optimized BimImportBenchmark executable")
    parser.add_argument("--before-build", type=Path, help="baseline CMake build dir for archived executables")
    parser.add_argument("--after-build", type=Path, help="optimized CMake build dir for archived executables")
    parser.add_argument("--model", required=True, action="append", type=Path, help="model file, repeat as needed")
    parser.add_argument("--iterations", default=4, type=positive_int, help="imports in each child process")
    parser.add_argument("--processes", default=3, type=positive_int, help="fresh processes per build per model")
    parser.add_argument("--output", type=Path, help="write the JSON report to this file (default stdout)")
    args = parser.parse_args()

    executables = {"before": args.before.resolve(), "after": args.after.resolve()}
    models = [compare_model(executables, model.resolve(), args.iterations, args.processes, index)
              for index, model in enumerate(args.model)]
    builds = {
        name: build_identity(executable, getattr(args, f"{name}_build"),
                             models[0]["runs"][name][0]["configuration"])
        for name, executable in executables.items()
    }
    differences = [difference for model in models for difference in model["differences"]]
    if all("cmakeCache" in build for build in builds.values()):
        for key in builds["before"]["cmakeCache"].keys() | builds["after"]["cmakeCache"].keys():
            if builds["before"]["cmakeCache"].get(key) != builds["after"]["cmakeCache"].get(key):
                differences.append(f"CMake setting differs: {key}")
    report = {
        "host": {"platform": platform.platform(), "architecture": platform.machine(),
                 "processor": os.environ.get("PROCESSOR_IDENTIFIER") or platform.processor(),
                 "logicalCpus": os.cpu_count()},
        "protocol": {
            "freshProcess": "first import in a new child process; OS file cache is uncontrolled",
            "repeatInProcess": "subsequent imports of the same file in the same child process",
            "processMemory": "process lifetime high-water marks sampled immediately after import, before semantic hashing",
            "elapsedTime": "steady-clock wall time in CPU preparation, including reads and excluding scene destruction",
            "allocations": ("when enabled, replaceable C++ allocation calls and requested bytes on the preparation thread; "
                            "collect timing with the uninstrumented target"),
            "semanticDigest": "FNV-1a-64 of selected ordered source and prepared scene fields, per section",
            "iterationsPerProcess": args.iterations,
            "processesPerBuildAndModel": args.processes,
        },
        "builds": builds, "models": models, "differences": sorted(set(differences)),
    }
    payload = json.dumps(report, indent=2, ensure_ascii=False) + "\n"
    if args.output:
        args.output.write_text(payload, encoding="utf-8")
    else:
        sys.stdout.write(payload)
    return 1 if differences else 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (OSError, RuntimeError, ValueError) as error:
        sys.exit(str(error))
