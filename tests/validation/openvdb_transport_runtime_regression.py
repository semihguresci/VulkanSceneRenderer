"""Opt-in particle-rasterization and fog-advection rendering regression.

Generates every fixture locally, retains commands, logs, captures and a JSON
report in a unique run directory, and enables Vulkan synchronization validation.
The tool/renderer must have OpenVDB enabled; --native-sparse also needs NanoVDB.
"""
from __future__ import annotations

import argparse
from datetime import datetime, timezone
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import time
import traceback
import uuid

from openvdb_runtime_regression import HEIGHT, WIDTH, image_metrics


RUNTIME_BUDGET_SECONDS = 300
VALIDATION_ERROR = re.compile(r"VUID-|Validation Error|SYNC-HAZARD", re.IGNORECASE)


def foreground_metrics(empty: Path, candidate: Path) -> dict:
    """Measure the changed silhouette against the identical empty camera."""
    from PIL import Image, ImageChops

    with Image.open(empty) as source:
        background = source.convert("RGB")
    with Image.open(candidate) as source:
        foreground = source.convert("RGB")
    if background.size != (WIDTH, HEIGHT) or foreground.size != background.size:
        raise AssertionError("unexpected capture dimensions")
    difference = ImageChops.difference(background, foreground)
    getter = getattr(difference, "get_flattened_data", None)
    pixels = getter() if getter else difference.getdata()
    count, total, x_sum, y_sum = 0, 0.0, 0.0, 0.0
    for index, pixel in enumerate(pixels):
        weight = max(pixel)
        if weight < 8:
            continue
        count += 1
        total += weight
        x_sum += (index % WIDTH) * weight
        y_sum += (index // WIDTH) * weight
    return {"changedPixels": count, "changedFraction": count / (WIDTH * HEIGHT),
            "differenceWeight": total,
            "weightedCentroid": [x_sum / total, y_sum / total] if total else None}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--exe", required=True, type=Path)
    parser.add_argument("--tool", required=True, type=Path)
    parser.add_argument("--output-dir", required=True, type=Path)
    parser.add_argument("--native-sparse", action="store_true")
    args = parser.parse_args()
    if os.environ.get("CONTAINER_RUN_OPENVDB_TRANSPORT_GPU") != "1":
        print("SKIP: set CONTAINER_RUN_OPENVDB_TRANSPORT_GPU=1")
        return 77
    try:
        from PIL import Image  # noqa: F401
    except ImportError:
        print("SKIP: particle/advection rendering comparisons require Pillow")
        return 77

    exe, tool = args.exe.resolve(), args.tool.resolve()
    stamp = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ")
    output = args.output_dir.resolve() / f"run-{stamp}-{os.getpid()}-{uuid.uuid4().hex[:8]}"
    output.mkdir(parents=True, exist_ok=False)
    thresholds = {"minimumVisibleFraction": .005, "minimumMotionPixels": 20,
                  "maximumMotionPixels": 65, "maximumVerticalMotionPixels": 4,
                  "maximumReferenceCentroidErrorPixels": 3,
                  "minimumReferenceSignalRatio": .9, "maximumReferenceSignalRatio": 1.1,
                  "maximumReferenceMeanError": 3, "maximumReferenceCloudMeanError": 6,
                  "maximumNativeMeanError": 3, "maximumNativeCloudMeanError": 6}
    report = {"schemaVersion": 1, "passed": False, "outputDirectory": str(output),
              "nativeSparseRequested": args.native_sparse, "thresholds": thresholds,
              "commands": [], "captures": {}, "comparisons": [], "errors": []}
    started = time.monotonic()
    deadline = started + RUNTIME_BUDGET_SECONDS
    (output / "vk_layer_settings.txt").write_text(
        "khronos_validation.validate_sync = true\n", encoding="utf-8")
    environment = dict(os.environ, VK_LAYER_SETTINGS_PATH=str(output))

    def save_report() -> None:
        report["elapsedSeconds"] = round(time.monotonic() - started, 3)
        (output / "results.json").write_text(
            json.dumps(report, indent=2) + "\n", encoding="utf-8")

    def run(name: str, command: list[str]) -> None:
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            raise TimeoutError(f"transport regression exceeded {RUNTIME_BUDGET_SECONDS} seconds")
        log = output / f"{name}.log"
        record = {"name": name, "argv": command, "cwd": str(exe.parent),
                  "log": str(log), "timeoutSeconds": min(60, remaining)}
        report["commands"].append(record)
        save_report()
        subprocess_started = time.monotonic()
        try:
            with log.open("w", encoding="utf-8") as stream:
                process = subprocess.run(command, cwd=exe.parent, env=environment,
                                         stdout=stream, stderr=subprocess.STDOUT,
                                         timeout=min(60, remaining),
                                         creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
            record["returnCode"] = process.returncode
        except subprocess.TimeoutExpired as error:
            record["timedOut"] = True
            raise TimeoutError(f"{name} timed out; see {log}") from error
        finally:
            record["elapsedSeconds"] = round(time.monotonic() - subprocess_started, 3)
            save_report()
        messages = log.read_text(encoding="utf-8", errors="replace")
        record["validationErrors"] = sorted(set(VALIDATION_ERROR.findall(messages)))
        if record["validationErrors"]:
            raise AssertionError(f"{name} reported Vulkan validation errors; see {log}")
        if process.returncode != 0:
            raise AssertionError(f"{name} exited with {process.returncode}; see {log}")

    def capture(name: str, *, surface: Path | None = None,
                fog: Path | None = None, native=False) -> Path:
        screenshot = output / f"{name}.png"
        command = [str(exe), "--model", str(surface) if surface else "",
                   "--hidden", "--no-ui", "--validation", "--display-mode", "lit",
                   "--width", str(WIDTH), "--height", str(HEIGHT),
                   "--camera-position", "0", "0", "6", "--camera-target", "0", "0", "0",
                   "--camera-fov", "45", "--exposure", "1",
                   "--directional-intensity", "2", "--environment-intensity", ".5",
                   "--no-bloom", "--no-taa", "--no-ray-query",
                   "--fixed-dt", "0.016666667", "--warmup-frames", "3", "--capture-frame", "4",
                   "--render-technique", "deferred-raster", "--msaa", "1",
                   "--screenshot", str(screenshot)]
        if fog:
            command += ["--vdb-volume", str(fog), "--vdb-grid", "density"]
        if native:
            command += ["--vdb-sparse"]
        run(name, command)
        if not screenshot.is_file():
            raise AssertionError(f"{name} did not create its screenshot")
        report["captures"][name] = str(screenshot)
        save_report()
        return screenshot

    try:
        if not exe.is_file() or not tool.is_file():
            raise FileNotFoundError(f"renderer and tool must exist: {exe}, {tool}")
        particles = [{"position": [-.8, -.1, 0], "radius": .65},
                     {"position": [.1, .15, 0], "radius": .55}]
        shifted = [{"position": [p["position"][0] + 1, *p["position"][1:]],
                    "radius": p["radius"]} for p in particles]
        particle_json, shifted_json = output / "particles.json", output / "shifted-particles.json"
        particle_json.write_text(json.dumps(particles, indent=2) + "\n", encoding="utf-8")
        shifted_json.write_text(json.dumps(shifted, indent=2) + "\n", encoding="utf-8")
        sdf, original = output / "particles-sdf.vdb", output / "original-fog.vdb"
        shifted_sdf, reference = output / "shifted-sdf.vdb", output / "shifted-fog.vdb"
        velocity, advected = output / "velocity.vdb", output / "advected-fog.vdb"
        for label, source, target in (("original", particle_json, sdf),
                                      ("reference", shifted_json, shifted_sdf)):
            run(f"generate-{label}-particles", [str(tool), "--particles", str(source), str(target),
                                               "--voxel-size", ".05"])
        for label, source, target in (("original", sdf, original),
                                      ("reference", shifted_sdf, reference)):
            run(f"generate-{label}-fog", [str(tool), "--fog", str(source), str(target),
                                         "--grid-name", "density"])
        run("generate-velocity", [str(tool), "--velocity", str(velocity),
                                  "--vector", "1", "0", "0", "--voxel-size", ".2"])
        run("advect-fog", [str(tool), "--advect", str(original), str(velocity), str(advected),
                           "--time", "1", "--steps", "2", "--input-grid", "density",
                           "--velocity-grid", "velocity", "--grid-name", "density"])
        for fixture in (sdf, original, shifted_sdf, reference, velocity, advected):
            if not fixture.is_file():
                raise AssertionError(f"fixture was not generated: {fixture}")

        empty = capture("empty")
        surface = capture("particle-surface", surface=sdf)
        initial = capture("original-fog", fog=original)
        transported = capture("advected-fog", fog=advected)
        expected = capture("shifted-reference-fog", fog=reference)
        silhouettes = {}
        for name, path in (("surface", surface), ("original", initial),
                           ("advected", transported), ("reference", expected)):
            metrics = foreground_metrics(empty, path)
            silhouettes[name] = metrics
            report["comparisons"].append({"name": f"visible-{name}", **metrics})
            if metrics["changedFraction"] < thresholds["minimumVisibleFraction"]:
                raise AssertionError(f"{name} did not produce a visible surface/volume")
        original_center = silhouettes["original"]["weightedCentroid"]
        advected_center = silhouettes["advected"]["weightedCentroid"]
        reference_center = silhouettes["reference"]["weightedCentroid"]
        movement = [advected_center[i] - original_center[i] for i in range(2)]
        centroid_error = max(abs(advected_center[i] - reference_center[i]) for i in range(2))
        signal_ratio = silhouettes["advected"]["differenceWeight"] / silhouettes["reference"]["differenceWeight"]
        report["comparisons"].append({"name": "positive-world-x-transport",
                                      "centroidMotionPixels": movement,
                                      "referenceCentroidMaxErrorPixels": centroid_error,
                                      "referenceSignalRatio": signal_ratio})
        if not thresholds["minimumMotionPixels"] <= movement[0] <= thresholds["maximumMotionPixels"]:
            raise AssertionError(f"world +X transport did not move the visible fog rightward: {movement}")
        if abs(movement[1]) > thresholds["maximumVerticalMotionPixels"]:
            raise AssertionError(f"world +X transport introduced excessive vertical image motion: {movement}")
        if centroid_error > thresholds["maximumReferenceCentroidErrorPixels"]:
            raise AssertionError(f"advected fog does not align with translated particle reference: {centroid_error}")
        if not thresholds["minimumReferenceSignalRatio"] <= signal_ratio <= thresholds["maximumReferenceSignalRatio"]:
            raise AssertionError(f"advected fog does not preserve translated-reference image signal: {signal_ratio}")
        metrics = image_metrics(expected, transported)
        report["comparisons"].append({"name": "advected-versus-translated-reference", **metrics})
        if (metrics["meanAbsoluteError"] > thresholds["maximumReferenceMeanError"] or
                metrics["cloudMeanAbsoluteError"] > thresholds["maximumReferenceCloudMeanError"]):
            raise AssertionError("advected fog exceeds the translated-reference image error bounds")
        if args.native_sparse:
            native = capture("advected-native-fog", fog=advected, native=True)
            metrics = image_metrics(transported, native)
            report["comparisons"].append({"name": "native-versus-dense-advected-fog", **metrics})
            if (metrics["meanAbsoluteError"] > thresholds["maximumNativeMeanError"] or
                    metrics["cloudMeanAbsoluteError"] > thresholds["maximumNativeCloudMeanError"]):
                raise AssertionError("native/dense advected fog exceeds image error bounds")
        report["passed"] = True
    except Exception as error:
        report["errors"].append({"type": type(error).__name__, "message": str(error),
                                 "traceback": traceback.format_exc()})
        print(f"FAIL: {error}", file=sys.stderr)
    finally:
        save_report()
        print(f"OpenVDB transport runtime results: {output / 'results.json'}", flush=True)
    return 0 if report["passed"] else 1


if __name__ == "__main__":
    sys.exit(main())
