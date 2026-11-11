"""Statistics and compatibility checks for the repeated LTC GPU benchmark."""

import math
import random
import re
from statistics import median


SCHEMA_VERSION = 1
MODES = ("sampled", "ltc")
REQUIRED_METRICS = ("lightingGpuMs", "gBufferGpuMs", "timedPassesSumGpuMs")
FULL_FRAME_METRIC = "fullFrameGpuMs"
# The sum can cover different passes in different frames.
COMPARABLE_METRICS = ("lightingGpuMs", "gBufferGpuMs", FULL_FRAME_METRIC)
DEVICE_FIELDS = ("gpu", "vendorId", "deviceId", "driverVersion", "vulkanApiVersion")
LTC_TABLES = ("matrix.bin", "amplitude.bin")
GUARD_METRIC = "lightingGpuMs"
MIN_REPEATS = 3
BOOTSTRAP_DRAWS = 2000
UNCERTAINTY_METHOD = (
    "Descriptive 95% percentile bootstrap of the median of per-run medians "
    "(2,000 resamples of independent process repeats). Mode comparisons "
    "resample paired run-median ratios. "
    "Build comparisons resample each build's repeats independently. "
    "Individual frame samples are used only within each run. "
    "Few repeats can understate drift across benchmark sessions."
)


def _percentile(ordered, fraction):
    rank = (len(ordered) - 1) * fraction
    left = math.floor(rank)
    right = math.ceil(rank)
    return ordered[left] + (ordered[right] - ordered[left]) * (rank - left)


def _finite_number(value):
    try:
        return type(value) in (int, float) and math.isfinite(value)
    except OverflowError:
        return False


def summarize_run(samples):
    """Return the median and linearly interpolated P95 of each timed metric."""
    if (not isinstance(samples, (list, tuple)) or not samples
            or any(not isinstance(sample, dict) for sample in samples)):
        raise ValueError("expected a run with GPU timing samples")
    frames = [sample.get("frame") for sample in samples]
    if (any(type(frame) is not int or frame <= 0 for frame in frames)
            or any(a >= b for a, b in zip(frames, frames[1:]))):
        raise ValueError("timing sample frame numbers must be positive and increasing")

    has_full_frame = [FULL_FRAME_METRIC in sample for sample in samples]
    if any(has_full_frame) and not all(has_full_frame):
        raise ValueError("full-frame timestamp availability changed within a run")
    metrics = REQUIRED_METRICS + ((FULL_FRAME_METRIC,) if all(has_full_frame) else ())
    result = {}
    for metric in metrics:
        values = [sample.get(metric) for sample in samples]
        if any(not _finite_number(value) or value <= 0 for value in values):
            raise ValueError(f"{metric} requires positive, finite GPU timings")
        ordered = sorted(values)
        result[metric] = {"median": median(ordered), "p95": _percentile(ordered, .95)}
    return result


def _prepare_case(runs):
    if not isinstance(runs, (list, tuple)) or not runs:
        raise ValueError("expected at least one sampled/LTC repeat")
    per_repeat = {}
    annotated = []
    expected_frames = None
    expected_metrics = None
    for run in runs:
        if not isinstance(run, dict):
            raise ValueError("each timing run must be an object")
        mode = run.get("mode")
        repeat = run.get("repeat")
        if mode not in MODES or type(repeat) is not int or repeat < 0:
            raise ValueError("each timing run needs a mode and a nonnegative repeat number")
        repeat_modes = per_repeat.setdefault(repeat, {})
        if mode in repeat_modes:
            raise ValueError(f"duplicate {mode} timing run for repeat {repeat}")
        samples = run.get("samples")
        metrics = summarize_run(samples)
        frames = tuple(sample["frame"] for sample in samples)
        if expected_frames is None:
            expected_frames = frames
            expected_metrics = tuple(metrics)
        if expected_frames != frames or expected_metrics != tuple(metrics):
            raise ValueError("all runs in a case need the same frames and timed metrics")
        repeat_modes[mode] = metrics
        annotated.append({**run, "metrics": metrics})
    if any(set(repeat) != set(MODES) for repeat in per_repeat.values()):
        raise ValueError("each repeat needs a matched sampled and LTC run")
    paired_runs = [per_repeat[repeat] for repeat in sorted(per_repeat)]
    series = {
        mode: {
            metric: [repeat[mode][metric]["median"] for repeat in paired_runs]
            for metric in expected_metrics
        } for mode in MODES
    }
    series["ltcOverSampled"] = {
        metric: [ltc / sampled for sampled, ltc in
                 zip(series["sampled"][metric], series["ltc"][metric])]
        for metric in expected_metrics if metric in COMPARABLE_METRICS
    }
    return annotated, expected_frames, series


def _interval(draws):
    ordered = sorted(draws)
    return [_percentile(ordered, .025), _percentile(ordered, .975)]


def _repeat_summary(values, rng):
    bounds = None
    if len(values) >= MIN_REPEATS:
        bounds = _interval(
            median(rng.choices(values, k=len(values))) for _ in range(BOOTSTRAP_DRAWS)
        )
    return {"median": median(values), "interval95": bounds}


def summarize_case(runs, *, seed):
    """Retain execution order and raw samples, then summarize across repeats."""
    annotated, _, series = _prepare_case(runs)
    rng = random.Random(seed)
    return {
        "runs": annotated,
        "repeatCount": len(annotated) // len(MODES),
        "summary": {
            mode: {metric: _repeat_summary(values, rng) for metric, values in metrics.items()}
            for mode, metrics in series.items()
        },
    }


def _report_cases(report, label):
    if (not isinstance(report, dict) or type(report.get("schemaVersion")) is not int
            or report["schemaVersion"] != SCHEMA_VERSION):
        raise ValueError(f"{label} must use repeated benchmark schemaVersion {SCHEMA_VERSION}")
    if report.get("validation") is not False or report.get("buildConfiguration") != "Release":
        raise ValueError(f"{label} must describe a Release run with validation disabled")
    device = report.get("device")
    if not isinstance(device, dict) or any(device.get(key) is None for key in DEVICE_FIELDS):
        raise ValueError(f"{label} is missing GPU, driver or Vulkan identity")
    report_cases = report.get("cases")
    if not isinstance(report_cases, list) or not report_cases:
        raise ValueError(f"{label} has no benchmark cases")
    cases = {}
    for case in report_cases:
        if not isinstance(case, dict):
            raise ValueError(f"{label} cases must be objects")
        key = case.get("id")
        if not isinstance(key, str) or not key or key in cases:
            raise ValueError(f"{label} has a missing or duplicate case ID")
        if not isinstance(case.get("settings"), dict) or not case["settings"]:
            raise ValueError(f"{label} case {key} has no settings")
        cases[key] = case
    return cases


def _table_hashes(report, label):
    provenance = report.get("provenance")
    tables = provenance.get("ltcTableSha256") if isinstance(provenance, dict) else None
    if not isinstance(tables, dict):
        raise ValueError(f"{label} is missing LTC table SHA-256 hashes")
    hashes = []
    for name in LTC_TABLES:
        digest = tables.get(name)
        if not isinstance(digest, str) or not re.fullmatch(r"[0-9a-fA-F]{64}", digest):
            raise ValueError(f"{label} requires an LTC table SHA-256 for {name}")
        hashes.append(digest.lower())
    return hashes


def _compare_series(current, baseline, threshold_ratio, rng):
    before = median(baseline)
    after = median(current)
    bounds = None
    status = "inconclusive"
    if min(len(current), len(baseline)) >= MIN_REPEATS:
        bounds = _interval(
            median(rng.choices(current, k=len(current))) /
            median(rng.choices(baseline, k=len(baseline)))
            for _ in range(BOOTSTRAP_DRAWS)
        )
        if bounds[0] > threshold_ratio:
            status = "regressed"
        elif bounds[1] <= threshold_ratio:
            status = "within_threshold"
    return {
        "baselineMedian": before,
        "currentMedian": after,
        "changePercent": 100 * (after / before - 1),
        "changeInterval95Percent": (
            None if bounds is None else [100 * (bound - 1) for bound in bounds]
        ),
        "baselineRepeats": len(baseline),
        "currentRepeats": len(current),
        "status": status,
    }


def compare_reports(current, baseline, *, threshold_percent, seed):
    """Compare matched devices and fixtures. Only absolute LTC Lighting time gates."""
    if not _finite_number(threshold_percent) or threshold_percent < 0:
        raise ValueError("regression threshold must be a finite, nonnegative percentage")
    current_cases = _report_cases(current, "current report")
    baseline_cases = _report_cases(baseline, "baseline report")
    if current["device"] != baseline["device"]:
        raise ValueError("baseline GPU, driver and Vulkan identity must match the current run")
    if _table_hashes(current, "current report") != _table_hashes(baseline, "baseline report"):
        raise ValueError("baseline LTC table SHA-256 hashes must match the current run")

    rng = random.Random(seed)
    threshold_ratio = 1 + threshold_percent / 100
    compared = []
    gate_statuses = []
    for key, case in current_cases.items():
        reference = baseline_cases.get(key)
        if reference is None:
            raise ValueError(f"baseline is missing case {key}")
        if case["settings"] != reference["settings"]:
            raise ValueError(f"baseline settings differ for case {key}")
        _, current_frames, after = _prepare_case(case.get("runs"))
        _, baseline_frames, before = _prepare_case(reference.get("runs"))
        if current_frames != baseline_frames:
            raise ValueError(f"baseline sample frames differ for case {key}")

        rows = []
        for mode, metrics in after.items():
            for metric, values in metrics.items():
                if metric not in COMPARABLE_METRICS or metric not in before[mode]:
                    continue
                row = _compare_series(values, before[mode][metric], threshold_ratio, rng)
                gates = mode == "ltc" and metric == GUARD_METRIC
                rows.append({"mode": mode, "metric": metric, "gates": gates, **row})
                if gates:
                    gate_statuses.append(row["status"])
        compared.append({"id": key, "metrics": rows})

    if "regressed" in gate_statuses:
        overall = "regressed"
    elif "inconclusive" in gate_statuses:
        overall = "inconclusive"
    else:
        overall = "within_threshold"
    return {
        "thresholdPercent": threshold_percent,
        "uncertaintyMethod": UNCERTAINTY_METHOD,
        "minimumIndependentRepeats": MIN_REPEATS,
        "seed": seed,
        "status": overall,
        "cases": compared,
    }
