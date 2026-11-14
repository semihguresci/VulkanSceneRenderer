"""Compare ordinary native raster frames under the default and serialized graphics policies."""

import argparse
import hashlib
import itertools
import json
import math
import os
from pathlib import Path
import re
import statistics
import subprocess
import tempfile

from display_capture_test_support import validation_log_error


TECHNIQUES = ("deferred-raster", "forward-raster")
POLICIES = ("default", "serialized")
CAMERAS = {
    "gltf": ((4, 3, 8), (0, 1, 0)),
    "ifcx": ((90, 53, 50), (31, 13, -19)),
}
GPU_FIELDS = ("gpuGraphicsMs", "gpuPassSumMs", "gpuResultLatencyFrames")
VALIDATION_LAYER = "VK_LAYER_KHRONOS_validation"


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


def file_identity(path):
    path = path.resolve(strict=True)
    with path.open("rb") as stream:
        return {"path": str(path), "sha256": hashlib.file_digest(stream, "sha256").hexdigest()}


def stats(samples):
    values = sorted(samples)
    if not values:
        return None
    return {
        "count": len(values),
        "mean": statistics.fmean(values),
        "median": statistics.median(values),
        "p95": values[math.ceil(0.95 * len(values)) - 1],
    }


def measurement(value):
    return type(value) in (int, float) and math.isfinite(value) and value >= 0


def series(rows, field):
    values = [(row["frameIndex"], row[field]) for row in rows if measurement(row.get(field))]
    return {"frameIndices": [frame for frame, _ in values], "stats": stats(value for _, value in values)}


def summarize(state, warmup, count):
    # The renderer numbers CLI captures from 1 and telemetry submissions from 0.
    terminal = warmup + count
    history = state["frameHistory"]
    require(history and history[-1]["frameIndex"] == terminal and history[-1]["cpuPhasesMs"]["Screenshot"] > 0,
            "Expected a single terminal screenshot immediately after the steady window")
    rows = [row for row in history if warmup <= row["frameIndex"] < terminal]
    expected = list(range(warmup, terminal))
    require([row["frameIndex"] for row in rows] == expected
            and all(row["cpuPhasesMs"]["Screenshot"] == 0 for row in rows),
            "Steady window contains a missing, repeated or captured submission")

    phases = rows[0]["cpuPhasesMs"]
    require(all(row["cpuPhasesMs"].keys() == phases.keys()
                and all(measurement(value) for value in row["cpuPhasesMs"].values())
                and row["cpuPhasesMs"]["Frame"] > 0 for row in rows),
            "Steady window has inconsistent CPU phase timings")
    cpu = {phase: stats(row["cpuPhasesMs"][phase] for row in rows) for phase in phases}

    # gpuKnownMs reflects the profiler's previous ready UI update, so attribute
    # GPU timings only to rows with a matching submission identity.
    retired = [row for row in rows if row.get("gpuFrameIndex") is not None]
    require(all(type(row["gpuFrameIndex"]) is int and row["gpuFrameIndex"] == row["frameIndex"]
                and type(row.get("gpuRecordingSerial")) is int for row in retired),
            "GPU timing is associated with the wrong submission")
    serials = [row["gpuRecordingSerial"] for row in retired]
    require(serials == sorted(set(serials)), "GPU timing recording identities repeat or are out of order")
    gpu = {field: series(retired, field) for field in GPU_FIELDS}

    policies = {(row["serializedConcurrency"], row["concurrencyReason"]) for row in rows}
    require(len(policies) == 1, "Graphics policy changed during the steady window")
    serialized, reason = policies.pop()
    require(isinstance(serialized, bool) and isinstance(reason, str) and reason, "Missing graphics policy")
    pending = [row["graphicsFramesInFlight"] for row in rows if row["graphicsFramesInFlight"] is not None]
    require(all(type(depth) is int and 0 <= depth <= state["framesInFlight"] for depth in pending),
            "Invalid pending graphics frame count")
    require(not state["gpuProfiler"]["available"] or len(pending) == count,
            "Active profiler did not report pending graphics for the full window")
    peak = max(pending, default=None)
    require(not serialized or peak is None or peak <= 1, "Serialized run queued multiple graphics frames")

    return {
        "steadyFrameIndices": expected,
        "cpuMs": cpu,
        "drawFramesPerSecond": 1000 / cpu["Frame"]["mean"],
        "gpuProfiler": state["gpuProfiler"],
        "matchedGpuFrameCount": len(retired),
        **gpu,
        "graphicsFramesInFlight": series(rows, "graphicsFramesInFlight"),
        "graphicsPolicy": {
            "serialized": serialized,
            "reason": reason,
            "peakPending": peak,
            "framesWithMultiplePending": sum(depth > 1 for depth in pending) if pending else None,
        },
        "vmaAllocationBytes": state["shaderReload"]["vmaBytes"],
        "vmaDeviceLocalAllocationBytes": state["shaderReload"]["vmaDeviceLocalBytes"],
        "vmaAllocationCount": state["shaderReload"]["vmaAllocations"],
    }


def make_command(args, model, technique, slots, policy, image):
    command = [
        str(args.renderer), "--model", model["path"], "--import-scale", "1", "--hidden", "--no-ui",
        "--validation" if args.validation_manifest else "--no-validation",
        "--render-technique", technique, "--frames-in-flight", str(slots),
        "--width", str(args.width), "--height", str(args.height), "--msaa", "1",
        "--camera-position", *map(str, model["cameraPosition"]), "--camera-target", *map(str, model["cameraTarget"]),
        "--camera-fov", "45", "--display-mode", "lit", "--taa", "--bloom", "--auto-exposure",
        "--fixed-dt", ".016666667", "--ray-shadows", "raster", "--no-ray-query", "--no-ddgi",
        "--warmup-frames", str(args.warmup + args.samples),
        "--capture-frame", str(args.warmup + args.samples + 1),
        "--screenshot-viewport", "--screenshot", str(image),
    ]
    if policy == "serialized":
        command.append("--serialize-graphics")
    return command


def validation_environment(manifest, output, environment):
    manifest = manifest.resolve(strict=True)
    source = json.loads(manifest.read_bytes())["layer"]
    require(source["name"] == VALIDATION_LAYER, f"Wrong validation layer in {manifest}")
    library = file_identity(manifest.parent / source["library_path"])
    settings = "khronos_validation.validate_sync = true\n"
    (output / "vk_layer_settings.txt").write_text(settings, encoding="ascii")
    environment.update(VK_LAYER_PATH=str(manifest.parent), VK_LAYER_SETTINGS_PATH=str(output), VK_LOADER_DEBUG="layer")
    return {"manifest": file_identity(manifest), "library": library, "settings": settings.strip()}


def run_case(args, model, row, output, environment, validation):
    info = (row["model"], row["technique"], row["slots"], row["policy"], row["repetition"])
    stem = output / "{}-{}-{}-{}-rep{}".format(*info)
    log, image = stem.with_suffix(".log"), stem.with_suffix(".png")
    telemetry = stem.with_suffix(".telemetry.json")
    command = make_command(args, model, row["technique"], row["slots"], row["policy"], image)
    row.update(command=command, log=log.name, image=image.name, telemetry=telemetry.name)
    if args.plan_only:
        return

    with log.open("w", encoding="utf-8") as stream:
        result = subprocess.run(command, cwd=args.renderer.parent, env=environment, stdout=stream,
                                stderr=subprocess.STDOUT, timeout=args.timeout,
                                creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
    messages = log.read_text(encoding="utf-8", errors="replace")
    require(result.returncode == 0 and not validation_log_error(messages), f"Native run failed. See {log}")
    if validation:
        inserted = re.search(r'Insert instance layer "VK_LAYER_KHRONOS_validation" \(([^\r\n]+)\)', messages)
        require(inserted and Path(inserted[1]).resolve() == Path(validation["library"]["path"]),
                f"Requested validation library was not loaded. See {log}")
    require(image.is_file(), f"Missing terminal screenshot. See {log}")

    state = json.loads(telemetry.read_bytes())
    route = "primary" if row["model"] == "gltf" else "auxiliary"
    require(state["technique"] == row["technique"] and state["framesInFlight"] == row["slots"]
            and state["resolution"] == [args.width, args.height] and state["swapchainRecreates"] == 0
            and state["validationEnabled"] == bool(validation) and state["msaaSamples"] == 1
            and state["taa"]["enabled"] and state["scene"][f"{route}ObjectCount"] > 0
            and Path(state["scene"][route]).resolve() == Path(model["path"]),
            f"Renderer did not execute the requested steady scene configuration. See {log}")
    require(args.revision is None or state["build"]["revision"] == args.revision, f"Wrong build revision. See {log}")
    row["configuration"] = {field: state[field] for field in (
        "gpu", "vendorId", "deviceId", "driverVersion", "vulkanApiVersion", "build",
        "swapchainImages", "swapchainFormat", "swapchainColorSpace",
    )}
    row["configuration"]["gpuTimingSource"] = state["gpuProfiler"]["source"]
    row["metrics"] = summarize(state, args.warmup, args.samples)
    policy = row["metrics"]["graphicsPolicy"]
    if row["policy"] == "serialized":
        require(policy["serialized"] and policy["reason"] == "forced serialized graphics",
                f"Forced graphics serialization was not selected. See {log}")
    print(f"{stem.name}: CPU median {row['metrics']['cpuMs']['Frame']['median']:.3f} ms, "
          f"peak pending graphics {policy['peakPending']} ({policy['reason']})", flush=True)


def compare(pair):
    default, serialized = pair["default"], pair["serialized"]
    require(default["configuration"] == serialized["configuration"],
            f"Renderer build, device, swapchain or timing source differed in pair {default['log']}")
    active, control = default["metrics"], serialized["metrics"]
    graphics_frames = active["gpuGraphicsMs"]["frameIndices"]
    timings_comparable = graphics_frames and graphics_frames == control["gpuGraphicsMs"]["frameIndices"]
    return {
        "defaultUsesOverlapPolicy": not active["graphicsPolicy"]["serialized"],
        "cpuFrameMeanChangePercent": 100 * (active["cpuMs"]["Frame"]["mean"] / control["cpuMs"]["Frame"]["mean"] - 1),
        "matchedGraphicsSampleCount": len(graphics_frames) if timings_comparable else 0,
        "gpuGraphicsMeanDifferenceMs": (
            active["gpuGraphicsMs"]["stats"]["mean"] - control["gpuGraphicsMs"]["stats"]["mean"]
            if timings_comparable else None
        ),
        "defaultFramesWithMultiplePending": active["graphicsPolicy"]["framesWithMultiplePending"],
        "vmaAllocationByteDifference": active["vmaAllocationBytes"] - control["vmaAllocationBytes"],
        "vmaDeviceLocalAllocationByteDifference": (
            active["vmaDeviceLocalAllocationBytes"] - control["vmaDeviceLocalAllocationBytes"]
        ),
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--renderer", type=Path, required=True)
    parser.add_argument("--revision", help="Require the renderer's embedded Git revision")
    parser.add_argument("--gltf", type=Path, help="Cornell box with local light glTF")
    parser.add_argument("--ifcx", type=Path, help="Tekla House IFCX")
    parser.add_argument("--validation-manifest", type=Path, help="VkLayer_khronos_validation.json to enable sync validation")
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--technique", choices=TECHNIQUES, action="append")
    parser.add_argument("--slots", choices=(1, 2, 3), type=int, action="append")
    parser.add_argument("--warmup", type=int, default=30)
    parser.add_argument("--samples", type=int, default=120)
    parser.add_argument("--repeat", type=int, default=2)
    parser.add_argument("--width", type=int, default=960)
    parser.add_argument("--height", type=int, default=540)
    parser.add_argument("--timeout", type=int, default=300, help="Timeout per renderer process in seconds")
    parser.add_argument("--plan-only", action="store_true", help="Write the run manifest without launching the renderer")
    args = parser.parse_args()
    if args.gltf is None and args.ifcx is None:
        parser.error("supply --gltf, --ifcx, or both")
    if args.warmup < 0 or not 1 <= args.samples < 240 or min(args.repeat, args.width, args.height, args.timeout) <= 0:
        parser.error("require nonnegative warmup, 1–239 samples and positive repetition, extent and timeout")

    executable = file_identity(args.renderer)
    args.renderer = Path(executable["path"])
    models = {}
    for name, path in (("gltf", args.gltf), ("ifcx", args.ifcx)):
        if path:
            position, target = CAMERAS[name]
            models[name] = {**file_identity(path), "cameraPosition": position, "cameraTarget": target}
    args.output.mkdir(parents=True, exist_ok=True)
    output = Path(tempfile.mkdtemp(prefix="run-", dir=args.output.resolve()))
    print(f"Benchmark evidence in {output}", flush=True)

    environment = dict(os.environ)
    validation = validation_environment(args.validation_manifest, output, environment) if args.validation_manifest else None
    configs = list(itertools.product(models, dict.fromkeys(args.technique or TECHNIQUES),
                                    dict.fromkeys(args.slots or (1, 2, 3))))
    report = {
        "schemaVersion": 1,
        "status": "planned" if args.plan_only else "incomplete",
        "benchmarkSha256": file_identity(Path(__file__))["sha256"],
        "executable": executable,
        "expectedRevision": args.revision,
        "models": models,
        "validation": validation,
        "warmupFrames": args.warmup,
        "framesPerWindow": args.samples,
        "resolution": [args.width, args.height],
        "runs": [],
        "pairs": [],
    }
    try:
        for repeat in range(args.repeat):
            for number, (model, technique, slots) in enumerate(configs if repeat % 2 == 0 else reversed(configs)):
                # Alternate within each repetition and reverse each case's order
                # on the next repetition, including when only one case is selected.
                case_index = number if repeat % 2 == 0 else len(configs) - 1 - number
                order = POLICIES if (case_index + repeat) % 2 == 0 else POLICIES[::-1]
                pair = {}
                for policy in order:
                    row = dict(model=model, technique=technique, slots=slots, policy=policy, repetition=repeat + 1)
                    report["runs"].append(row)
                    run_case(args, models[model], row, output, environment, validation)
                    pair[policy] = row
                comparison = {} if args.plan_only else compare(pair)
                report["pairs"].append(dict(model=model, technique=technique, slots=slots, repetition=repeat + 1,
                                            policyOrder=order, **comparison))
        if not args.plan_only:
            identities = [executable, *models.values()]
            if validation:
                identities += [validation["manifest"], validation["library"]]
            for identity in identities:
                require(file_identity(Path(identity["path"]))["sha256"] == identity["sha256"],
                        f"Benchmark input changed during the run: {identity['path']}")
            report["status"] = "complete"
    finally:
        (output / "results.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
