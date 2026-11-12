"""Run independent sampled/LTC timing pairs and separate correctness captures."""

import hashlib
import importlib.util
import json
import math
from pathlib import Path
import random

from ltc_benchmark_fixtures import (
    BENCHMARK_COUNTS, BENCHMARK_DOMAINS, BENCHMARK_MATERIALS, benchmark_fixtures,
)
from ltc_benchmark_stats import (
    COMPARABLE_METRICS, DEVICE_FIELDS, MODES, SCHEMA_VERSION, UNCERTAINTY_METHOD,
    compare_reports, summarize_case, summarize_run,
)


RESOLUTION = (960, 540)
SAMPLES = 25
FIXED_DT = 1 / 60
RENDER_FLAGS = (
    "--no-ray-query", "--ray-shadows", "raster", "--no-ddgi",
    "--bounce-intensity", "0", "--import-scale", "1", "--fixed-dt", str(FIXED_DT),
)


def add_benchmark_arguments(parser):
    parser.add_argument("--benchmark-material", choices=("both", *BENCHMARK_MATERIALS), default="both")
    parser.add_argument("--benchmark-lights", type=int, choices=BENCHMARK_COUNTS, nargs="+",
                        default=BENCHMARK_COUNTS)
    parser.add_argument("--benchmark-domain", choices=BENCHMARK_DOMAINS, nargs="+",
                        help="run all selected counts for these domains")
    parser.add_argument("--benchmark-repeats", type=int, default=3, help="independent process pairs per fixture")
    parser.add_argument("--benchmark-warmup", type=int, default=48, help="discarded GPU frames per process")
    parser.add_argument("--benchmark-frames", type=int, default=8, help="timing samples per process")
    parser.add_argument("--benchmark-stride", type=int, default=4, help="spacing between timing observations")
    parser.add_argument("--benchmark-order", choices=("alternate", "random"), default="alternate",
                        help="alternate pair orientation or shuffle a balanced list of orientations")
    parser.add_argument("--benchmark-seed", type=int, default=71)
    parser.add_argument("--benchmark-baseline", type=Path, help="compatible benchmark-results.json to compare")
    parser.add_argument("--benchmark-threshold-percent", type=float, default=5,
                        help="allowed LTC Lighting median increase over the baseline")
    parser.add_argument("--benchmark-profile-shaders", type=Path,
                        help="separately built LTC diagnostic shaders for path coverage and overhead")


def timing_frames(args):
    # Queries from a submitted frame are collected on the next draw.
    return [args.benchmark_warmup + 2 + i * args.benchmark_stride
            for i in range(args.benchmark_frames)]


def validate_benchmark_arguments(parser, args):
    if not 1 <= args.benchmark_repeats <= 50:
        parser.error("--benchmark-repeats must be in [1, 50]")
    if not 1 <= args.benchmark_frames <= 240 or args.benchmark_stride < 1 or args.benchmark_warmup < 1:
        parser.error("benchmark needs 1 to 240 samples, positive stride and at least one warm-up frame")
    if timing_frames(args)[-1] + int(args.benchmark_profile_shaders is not None) > 10000:
        parser.error("benchmark frame sequence exceeds 10000 frames")
    if not math.isfinite(args.benchmark_threshold_percent) or args.benchmark_threshold_percent < 0:
        parser.error("--benchmark-threshold-percent must be finite and nonnegative")
    if args.benchmark_baseline is not None and not args.benchmark_baseline.is_file():
        parser.error(f"Benchmark baseline file does not exist: {args.benchmark_baseline}")
    if args.benchmark_profile_shaders is not None and importlib.util.find_spec("cv2") is None:
        parser.error("LTC diagnostic EXR decoding requires OpenCV")


def run_order(repeats, method, rng):
    reverse_first = rng.randrange(2)
    reverse = [(i + reverse_first) % 2 for i in range(repeats)]
    if method == "random":
        rng.shuffle(reverse)
    return [tuple(reversed(MODES)) if flip else MODES for flip in reverse]


def _sha256(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def _binary_identity(executable):
    root = executable.parent
    shaders = {path.name: _sha256(path) for path in sorted((root / "spv_shaders").glob("*.spv"))}
    if not shaders:
        raise RuntimeError(f"No deployed SPIR-V shaders in {root / 'spv_shaders'}")
    return {
        "executableSha256": _sha256(executable),
        "shaderBundleSha256": hashlib.sha256(json.dumps(shaders, sort_keys=True).encode()).hexdigest(),
        "shaderSha256": shaders,
        "ltcTableSha256": {name: _sha256(root / "materials" / "ltc" / name)
                           for name in ("matrix.bin", "amplitude.bin")},
    }


def _scene_state(telemetry, fixture, validation):
    lighting = telemetry["lighting"]
    if (telemetry["resolution"] != list(RESOLUTION) or telemetry["technique"] != "deferred-raster"
            or telemetry["msaaSamples"] != 1 or telemetry["taa"]["enabled"]
            or lighting["areaLightCount"] != fixture.count or lighting["pointLightCount"] != 0
            or lighting["activeLocalShadowLayers"] != 0
            or any(lighting[key] != 0 for key in ("directionalIntensity", "environmentIntensity", "bounceIntensity"))
            or telemetry["validationEnabled"] is not validation
            or telemetry["build"]["configuration"] != "Release"):
        raise RuntimeError(f"Unexpected runtime settings for {fixture.id}")
    device = {key: telemetry[key] for key in DEVICE_FIELDS}
    submission = {key: telemetry[key] for key in ("framesInFlight", "serializedConcurrency")}
    return device, telemetry["build"], submission


def _timing_sample(telemetry, frame):
    passes = {entry["name"]: entry for entry in telemetry["passes"] if entry["gpuTimed"]}
    if any(name not in passes or not passes[name]["active"] for name in ("Lighting", "GBuffer")):
        raise RuntimeError(f"Lighting and GBuffer GPU timestamps are required at frame {frame}")
    profiler = telemetry["gpuProfiler"]
    if not profiler["available"] or profiler["resultLatencyFrames"] != 1 or not profiler["recordingSerial"]:
        raise RuntimeError(f"Missing retired GPU recording at frame {frame}")
    sample = {
        "frame": frame,
        "recordingSerial": profiler["recordingSerial"],
        "lightingGpuMs": passes["Lighting"]["gpuMs"],
        "gBufferGpuMs": passes["GBuffer"]["gpuMs"],
        "timedPassesSumGpuMs": telemetry["gpuKnownMs"],
        "timedPasses": sorted(passes),
    }
    if telemetry.get("fullFrameGpuMs") is not None:
        sample["fullFrameGpuMs"] = telemetry["fullFrameGpuMs"]
    return sample, profiler["source"]


def _timed_capture(runtime, fixture, model, name, mode, exposure, frames, executable=None):
    profile = executable is not None
    end = frames[-1] + int(profile)
    sequence = dict(schemaVersion=1, frames=end, telemetryFrames=frames,
                    sampleFrames=[end] if profile else [])
    image, _ = runtime.capture(name, model, fixture.settings, "deferred-raster", mode,
        samples=SAMPLES, exposure=exposure, sequence=sequence, extra=RENDER_FLAGS,
        width=RESOLUTION[0], height=RESOLUTION[1], executable=executable,
        validation=False, verify_validation=True, image_format="exr" if profile else "png")
    samples = []
    identity = None
    for frame in frames:
        telemetry_path = image.with_name(
            image.stem + ("" if frame == end else f".frame-{frame:04d}") + ".telemetry.json")
        telemetry = json.loads(telemetry_path.read_text(encoding="utf-8"))
        runtime.validate_telemetry(telemetry, mode, SAMPLES)
        device, build, submission = _scene_state(telemetry, fixture, False)
        sample, source = _timing_sample(telemetry, frame)
        observed = (device, build, submission, source)
        if identity is None:
            identity = observed
        if identity != observed:
            raise RuntimeError(f"Benchmark device, build, submission or timing source changed during {name}")
        if samples and samples[-1]["recordingSerial"] >= sample["recordingSerial"]:
            raise RuntimeError(f"GPU recording was repeated or reordered during {name}")
        samples.append(sample)
    return samples, identity, image


def _correctness_captures(runtime, fixture, model, exposure, warmup, expected_identity):
    checks = {}
    frame = warmup + 2
    sequence = dict(schemaVersion=1, frames=frame, sampleFrames=[frame])
    for mode in MODES:
        name = f"benchmark-correctness-{fixture.id}-{mode}"
        image, telemetry = runtime.capture(name, model, fixture.settings, "deferred-raster", mode,
            samples=SAMPLES, exposure=exposure, sequence=sequence, extra=RENDER_FLAGS,
            width=RESOLUTION[0], height=RESOLUTION[1], verify_validation=True)
        if _scene_state(telemetry, fixture, True) != expected_identity[:3]:
            raise RuntimeError(f"Correctness capture used different runtime settings for {fixture.id}")
        metrics, _ = runtime.check_quality_capture(
            name, image, telemetry, fixture.settings, mode, SAMPLES, exposure)
        checks[mode] = {"image": image.name, "imageSha256": _sha256(image),
                        "validationClean": True, **metrics}
    return checks


def _profile_case(runtime, fixture, model, exposure, frames, profile_exe, identity, rng):
    from ltc_path_profile import summarize_path_capture

    records = {}
    order = ["control", "instrumented"]
    if rng.randrange(2):
        order.reverse()
    for kind in order:
        executable = profile_exe if kind == "instrumented" else runtime.exe
        name = f"benchmark-profile-{fixture.id}-{kind}"
        samples, observed, image = _timed_capture(
            runtime, fixture, model, name, "ltc", exposure, frames, executable)
        if observed != identity:
            raise RuntimeError(f"Profiling changed device, build or timing source for {fixture.id}")
        records[kind] = {"samples": samples, "metrics": summarize_run(samples), "image": image.name}
        if kind == "instrumented":
            records[kind]["paths"] = summarize_path_capture(image)
    control = records["control"]["metrics"]
    profile = records["instrumented"]["metrics"]
    overhead = {key: 100 * (profile[key]["median"] / control[key]["median"] - 1)
                for key in COMPARABLE_METRICS if key in control and key in profile}
    return {"order": order, **records, "medianChangePercent": overhead}


def run_benchmark(runtime, args, make_case, make_model, exposure_for):
    rng = random.Random(args.benchmark_seed)
    materials = BENCHMARK_MATERIALS if args.benchmark_material == "both" else (args.benchmark_material,)
    frames = timing_frames(args)
    provenance = _binary_identity(runtime.exe)
    report = {
        "schemaVersion": SCHEMA_VERSION, "buildConfiguration": "Release", "validation": False,
        "provenance": provenance, "seed": args.benchmark_seed, "orderPolicy": args.benchmark_order,
        "uncertaintyMethod": UNCERTAINTY_METHOD, "cases": [],
        "timedPassesSum": "Sum of available timed GPU passes",
    }
    profile_exe = None
    if args.benchmark_profile_shaders is not None:
        from ltc_path_profile import stage_profile_executable

        profile_exe = stage_profile_executable(
            runtime.exe, args.benchmark_profile_shaders, runtime.output / "benchmark-profile-runtime")
        report["profiling"] = {"provenance": _binary_identity(profile_exe),
                               "method": "One separate matched process pair per case. Path flags overlap."}
    identity = None
    fixtures = []
    for fixture in benchmark_fixtures(make_case, materials, args.benchmark_lights, args.benchmark_domain):
        print(f"LTC timing {fixture.id} ({args.benchmark_repeats} process pairs)", flush=True)
        model = make_model(runtime.output / "benchmark-fixtures" / fixture.id /
                           "models/validation/cornell_box_local_light.gltf", fixture.settings, fixture.count)
        exposure = exposure_for(fixture.settings)
        settings = fixture.manifest(model, *RESOLUTION, SAMPLES, exposure)
        settings.update(warmupFrames=args.benchmark_warmup, observedFrames=frames,
                        fixedTimestepSeconds=FIXED_DT)
        runs = []
        for repeat, order in enumerate(run_order(args.benchmark_repeats, args.benchmark_order, rng)):
            for mode in order:
                name = f"benchmark-timing-{fixture.id}-{repeat}-{mode}"
                samples, observed, _ = _timed_capture(runtime, fixture, model, name, mode, exposure, frames)
                if identity is None:
                    identity = observed
                if observed != identity:
                    raise RuntimeError("Benchmark device, build or timing source changed across processes")
                runs.append({"repeat": repeat, "mode": mode, "samples": samples})
        settings.update(submission=identity[2], gpuTimingSource=identity[3])
        entry = {"id": fixture.id, "settings": settings, **summarize_case(runs, seed=args.benchmark_seed)}
        report["cases"].append(entry)
        fixtures.append((fixture, model, exposure, entry))

    report["device"], report["build"], report["submission"], report["gpuTimingSource"] = identity
    for fixture, model, exposure, entry in fixtures:
        print(f"LTC validation {fixture.id}", flush=True)
        entry["correctness"] = _correctness_captures(
            runtime, fixture, model, exposure, args.benchmark_warmup, identity)
    if profile_exe is not None:
        for fixture, model, exposure, entry in fixtures:
            print(f"LTC diagnostic {fixture.id}", flush=True)
            entry["profile"] = _profile_case(runtime, fixture, model, exposure, frames, profile_exe, identity, rng)

    if _binary_identity(runtime.exe) != provenance:
        raise RuntimeError("Benchmark executable, shaders or LUTs changed during the run")
    if args.benchmark_baseline is not None:
        baseline = json.loads(args.benchmark_baseline.read_text(encoding="utf-8"))
        report["baselineComparison"] = compare_reports(report, baseline,
            threshold_percent=args.benchmark_threshold_percent, seed=args.benchmark_seed)
    output = runtime.output / "benchmark-results.json"
    pending = output.with_suffix(".json.tmp")
    pending.write_text(json.dumps(report, indent=2, allow_nan=False) + "\n", encoding="utf-8")
    pending.replace(output)
    if args.benchmark_baseline is not None:
        status = report["baselineComparison"]["status"]
        print(f"LTC baseline comparison: {status}", flush=True)
        return {"within_threshold": 0, "regressed": 1, "inconclusive": 2}[status]
    return 0
