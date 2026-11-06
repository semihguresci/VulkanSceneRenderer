"""Opt-in hidden GPU proof of exact Int64, Bool/Mask and Vec3I slice routes.

The native generator writes one retained seven-grid fixture. Independent small
Float reference fields have exactly the expected vertex colors. PNG parity and
visible ordered color patches prevent empty-scene or collapsed-ID false passes.
"""
from __future__ import annotations

import argparse
from datetime import datetime, timezone
import json
import math
import os
from pathlib import Path
import re
from statistics import median
import subprocess
import time
import traceback
import uuid

WIDTH, HEIGHT = 320, 200
TIME_BUDGET_SECONDS = 120
VALIDATION_ERROR = re.compile(r"VUID-[A-Za-z0-9_-]+|Validation Error|SYNC-HAZARD[A-Za-z0-9_-]*", re.I)


def difference(first: Path, second: Path) -> dict:
    from PIL import Image, ImageChops, ImageStat
    with Image.open(first) as source:
        a = source.convert("RGB")
    with Image.open(second) as source:
        b = source.convert("RGB")
    if a.size != (WIDTH, HEIGHT) or b.size != a.size:
        raise AssertionError("unexpected screenshot dimensions")
    delta = ImageChops.difference(a, b)
    getter = getattr(delta, "get_flattened_data", None)
    data = getter() if getter else delta.getdata()
    return {"changedFraction": sum(max(p) >= 8 for p in data) / (WIDTH * HEIGHT),
            "meanAbsoluteError": sum(ImageStat.Stat(delta).mean) / 3,
            "maximumChannelError": max(high for low, high in delta.getextrema())}


def patch(image: Path, world_x: float) -> dict:
    from PIL import Image
    focal = HEIGHT / (2 * math.tan(math.radians(45) / 2))
    x, y = round(WIDTH / 2 + focal * world_x / 4), HEIGHT // 2
    with Image.open(image) as source:
        data = list(source.convert("RGB").crop((x - 1, y - 1, x + 2, y + 2)).getdata())
    rgb = [median(p[channel] for p in data) for channel in range(3)]
    return {"worldX": world_x, "pixelCenter": [x, y], "medianRgb": rgb,
            "redMinusBlue": rgb[0] - rgb[2]}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--generator", required=True, type=Path)
    parser.add_argument("--exe", required=True, type=Path)
    parser.add_argument("--output-dir", required=True, type=Path)
    args = parser.parse_args()
    if os.environ.get("CONTAINER_RUN_OPENVDB_DISCRETE_PREVIEW_GPU") != "1":
        print("SKIP: set CONTAINER_RUN_OPENVDB_DISCRETE_PREVIEW_GPU=1")
        return 77
    try:
        from PIL import Image  # noqa: F401
    except ImportError:
        print("SKIP: discrete preview comparisons require Pillow")
        return 77
    exe, generator = args.exe.resolve(), args.generator.resolve()
    output = args.output_dir.resolve() / f"run-{datetime.now(timezone.utc):%Y%m%dT%H%M%SZ}-{os.getpid()}-{uuid.uuid4().hex[:8]}"
    output.mkdir(parents=True, exist_ok=False)
    started = time.monotonic()
    deadline = started + TIME_BUDGET_SECONDS
    report = {"schemaVersion": 1, "passed": False, "outputDirectory": str(output),
              "commands": [], "captures": {}, "comparisons": [], "errors": [],
              "thresholds": {"minimumVisibleFraction": .02, "maximumParityMeanError": .25,
                             "maximumParityChannelError": 2, "minimumOrderedPatchDelta": 5,
                             "minimumBoolMaskPatchDelta": 30}}
    (output / "vk_layer_settings.txt").write_text("khronos_validation.validate_sync = true\n", encoding="utf-8")
    environment = dict(os.environ, VK_LAYER_SETTINGS_PATH=str(output))

    def save():
        report["elapsedSeconds"] = round(time.monotonic() - started, 3)
        (output / "results.json").write_text(json.dumps(report, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")

    def run(name, command):
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            raise TimeoutError(f"discrete preview exceeded {TIME_BUDGET_SECONDS}s")
        log = output / f"{name}.log"
        record = {"name": name, "argv": list(map(str, command)), "cwd": str(exe.parent),
                  "log": str(log), "timeoutSeconds": min(45, remaining)}
        report["commands"].append(record); save()
        began = time.monotonic()
        try:
            with log.open("w", encoding="utf-8") as stream:
                process = subprocess.run(record["argv"], cwd=exe.parent, env=environment,
                                         stdout=stream, stderr=subprocess.STDOUT,
                                         timeout=record["timeoutSeconds"],
                                         creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
            record["returnCode"] = process.returncode
        except subprocess.TimeoutExpired as error:
            record["timedOut"] = True
            raise TimeoutError(f"{name} timed out; see {log}") from error
        finally:
            record["elapsedSeconds"] = round(time.monotonic() - began, 3); save()
        messages = log.read_text(encoding="utf-8", errors="replace")
        record["validationErrors"] = sorted(set(VALIDATION_ERROR.findall(messages))); save()
        if record["validationErrors"] or process.returncode:
            raise AssertionError(f"{name} failed or reported Vulkan validation errors; see {log}")

    def capture(name, grid=None, *, color="blue-red", options=()):
        image = output / f"{name}.png"
        command = [exe, "--model", fixture if grid else "", "--hidden", "--no-ui", "--validation",
                   "--width", WIDTH, "--height", HEIGHT, "--camera-position", 0, 0, 4,
                   "--camera-target", 0, 0, 0, "--camera-fov", 45, "--exposure", 1,
                   "--render-technique", "deferred-raster", "--display-mode", "lit",
                   "--no-bloom", "--no-taa", "--no-ray-query", "--msaa", 1,
                   "--fixed-dt", .016666667, "--warmup-frames", 3, "--capture-frame", 4,
                   "--screenshot", image]
        if grid:
            command += ["--vdb-slice", "--vdb-model-grid", grid, "--vdb-slice-extent", 2, 1.5,
                        "--vdb-slice-samples", 5, 3, "--vdb-slice-map", color, *options]
        run(name, command)
        if not image.is_file():
            raise AssertionError(f"{name} did not create a screenshot")
        report["captures"][name] = str(image); save()
        return image

    try:
        if not exe.is_file() or not generator.is_file():
            raise FileNotFoundError("renderer and native fixture generator must exist")
        fixture = output / "離散-ID-原生.vdb"
        run("generate-native-fixtures", [generator, fixture])
        original = fixture.read_bytes()
        empty = capture("empty")
        reference = capture("id-float-reference", "id_reference")
        ids = capture("int64-exact-range", "ids", options=("--vdb-slice-integer-range", "9007199254740992", "9007199254740996"))
        bool_reference = capture("bool-float-reference", "bool_reference", color="gray")
        boolean = capture("bool-stored-values", "bool_values", color="gray")
        mask_reference = capture("mask-float-reference", "mask_reference", color="gray")
        mask = capture("mask-native-activity", "mask_activity", color="gray")
        vector = capture("vec3i-component-x", "vector_i", options=("--vdb-slice-value", "x", "--vdb-slice-range", 0, 4))
        for name, actual, expected in (("int64-adjacent-vs-independent-float", ids, reference),
                                       ("bool-inactive-values-vs-independent-float", boolean, bool_reference),
                                       ("mask-activity-vs-independent-float", mask, mask_reference),
                                       ("vec3i-world-component-vs-independent-float", vector, reference)):
            visible = difference(empty, expected)
            report["comparisons"].append({"name": name + "-visible", **visible})
            if visible["changedFraction"] < report["thresholds"]["minimumVisibleFraction"]:
                raise AssertionError(f"{name} reference is not visibly rendered")
            parity = difference(actual, expected)
            report["comparisons"].append({"name": name, **parity})
            if parity["meanAbsoluteError"] > .25 or parity["maximumChannelError"] > 2:
                raise AssertionError(f"{name} differs from its independent expected colors")
        patches = [patch(ids, x) for x in (-.9, -.45, 0., .45, .9)]
        deltas = [b["redMinusBlue"] - a["redMinusBlue"] for a, b in zip(patches, patches[1:])]
        report["comparisons"].append({"name": "five-adjacent-int64-colors-ordered", "patches": patches, "deltas": deltas})
        if any(delta < report["thresholds"]["minimumOrderedPatchDelta"] for delta in deltas):
            raise AssertionError("adjacent IDs above2^53 did not produce five visibly distinct ordered colors")
        for x in (-.5, 0.):
            a, b = patch(boolean, x), patch(mask, x)
            delta = max(abs(u - v) for u, v in zip(a["medianRgb"], b["medianRgb"]))
            report["comparisons"].append({"name": "bool-stored-vs-mask-activity", "worldX": x,
                                          "bool": a, "mask": b, "maximumChannelDelta": delta})
            if delta < report["thresholds"]["minimumBoolMaskPatchDelta"]:
                raise AssertionError("active-false/inactive-true Bool values were conflated with Mask activity")
        if fixture.read_bytes() != original:
            raise AssertionError("renderer modified the native fixture")
        report["passed"] = True
    except Exception as error:
        report["errors"].append({"type": type(error).__name__, "message": str(error), "traceback": traceback.format_exc()})
    finally:
        save()
    print(json.dumps({"passed": report["passed"], "results": str(output / "results.json")}, indent=2))
    return 0 if report["passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
