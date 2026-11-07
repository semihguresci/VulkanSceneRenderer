"""Opt-in native-point attribute/group and scalar-slice GPU regression.

Retains generated fixtures, exact commands, logs, PNGs and results.json in a
unique run directory. Uses the compiled tool and renderer; no pyopenvdb needed.
"""
from __future__ import annotations

import argparse
from array import array
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

TIME_BUDGET_SECONDS = 300
VALIDATION_ERROR = re.compile(r"VUID-|Validation Error|SYNC-HAZARD", re.IGNORECASE)


def foreground(empty: Path, image: Path) -> dict:
    from PIL import Image, ImageChops
    with Image.open(empty) as source:
        background = source.convert("RGB")
    with Image.open(image) as source:
        actual = source.convert("RGB")
    if actual.size != (WIDTH, HEIGHT) or background.size != actual.size:
        raise AssertionError("unexpected preview screenshot dimensions")
    difference = ImageChops.difference(background, actual)
    getter = getattr(difference, "get_flattened_data", None)
    pixels = getter() if getter else difference.getdata()
    count, weight, left, right = 0, 0, 0, 0
    for index, pixel in enumerate(pixels):
        signal = max(pixel)
        if signal < 8:
            continue
        count += 1
        weight += signal
        left += index % WIDTH < WIDTH//2-20
        right += index % WIDTH > WIDTH//2+20
    return {"changedPixels": count, "changedFraction": count/(WIDTH*HEIGHT),
            "signal": weight, "leftChangedPixels": left, "rightChangedPixels": right}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--exe", required=True, type=Path)
    parser.add_argument("--tool", required=True, type=Path)
    parser.add_argument("--output-dir", required=True, type=Path)
    args = parser.parse_args()
    if os.environ.get("CONTAINER_RUN_OPENVDB_EXTENDED_PREVIEW_GPU") != "1":
        print("SKIP: set CONTAINER_RUN_OPENVDB_EXTENDED_PREVIEW_GPU=1")
        return 77
    try:
        from PIL import Image  # noqa: F401
    except ImportError:
        print("SKIP: extended preview comparisons require Pillow")
        return 77
    sys.path.insert(0, str(Path(__file__).resolve().parents[2]/"python"))
    from container_openvdb import DenseType, DenseVolume, write_dense_array

    exe, tool = args.exe.resolve(), args.tool.resolve()
    stamp = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ")
    output = args.output_dir.resolve()/f"run-{stamp}-{os.getpid()}-{uuid.uuid4().hex[:8]}"
    output.mkdir(parents=True, exist_ok=False)
    started = time.monotonic()
    deadline = started+TIME_BUDGET_SECONDS
    thresholds = {"minimumSliceVisibleFraction": .02, "minimumPointsVisibleFraction": .001,
                  "minimumAttributeColorChangedFraction": .0005,
                  "minimumGroupCenterDifference": 5, "minimumRadiusAreaRatio": 1.5,
                  "minimumAlphaSignalRatio": .2, "maximumAlphaSignalRatio": .95,
                  "maximumSliceTechniqueMeanError": 3, "maximumPointTechniqueMeanError": 6}
    report = {"schemaVersion": 1, "passed": False, "outputDirectory": str(output),
              "thresholds": thresholds, "commands": [], "captures": {}, "comparisons": [], "errors": []}
    (output/"vk_layer_settings.txt").write_text("khronos_validation.validate_sync = true\n", encoding="utf-8")
    environment = dict(os.environ, VK_LAYER_SETTINGS_PATH=str(output))

    def save() -> None:
        report["elapsedSeconds"] = round(time.monotonic()-started, 3)
        (output/"results.json").write_text(json.dumps(report, indent=2)+"\n", encoding="utf-8")

    def run(name: str, command: list[str]) -> None:
        remaining = deadline-time.monotonic()
        if remaining <= 0:
            raise TimeoutError(f"extended preview regression exceeded{TIME_BUDGET_SECONDS}s")
        record = {"name": name, "argv": command, "cwd": str(exe.parent),
                  "log": str(output/f"{name}.log"), "timeoutSeconds": min(60, remaining)}
        report["commands"].append(record)
        save()
        began = time.monotonic()
        try:
            with Path(record["log"]).open("w", encoding="utf-8") as stream:
                process = subprocess.run(command, cwd=exe.parent, env=environment, stdout=stream,
                                         stderr=subprocess.STDOUT, timeout=min(60, remaining),
                                         creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
            record["returnCode"] = process.returncode
        except subprocess.TimeoutExpired as error:
            record["timedOut"] = True
            raise TimeoutError(f"{name} timed out; see {record['log']}") from error
        finally:
            record["elapsedSeconds"] = round(time.monotonic()-began, 3)
            save()
        messages = Path(record["log"]).read_text(encoding="utf-8", errors="replace")
        record["validationErrors"] = sorted(set(VALIDATION_ERROR.findall(messages)))
        if record["validationErrors"] or process.returncode:
            raise AssertionError(f"{name} failed/validation error; see {record['log']}")

    def capture(name: str, model: Path | None = None, options=(), technique="deferred-raster") -> Path:
        image = output/f"{name}.png"
        command = [str(exe), "--model", str(model) if model else "", "--hidden", "--no-ui", "--validation",
                   "--width", str(WIDTH), "--height", str(HEIGHT), "--display-mode", "lit", "--camera-position", "0", "0", "4",
                   "--camera-target", "0", "0", "0", "--camera-fov", "45", "--exposure", "1",
                   "--directional-intensity", "2", "--environment-intensity", ".2", "--no-bloom", "--no-taa", "--no-ray-query",
                   "--render-technique", technique, "--msaa", "1", "--fixed-dt", "0.016666667",
                   "--warmup-frames", "3", "--capture-frame", "4", "--screenshot", str(image), *options]
        run(name, command)
        if not image.is_file():
            raise AssertionError(f"{name} did not create a screenshot")
        report["captures"][name] = str(image)
        save()
        return image

    try:
        if not exe.is_file() or not tool.is_file():
            raise FileNotFoundError("renderer and tool must exist")
        points = output/"点云-input.json"
        points.write_text(json.dumps([{"position": [x, 0, 0], "radius": .1} for x in (-.65, 0, .65)])+"\n", encoding="utf-8")
        raw, native = output/"raw.vdb", output/"原生-粒子.vdb"
        edits = output/"attributes.json"
        edits.write_text(json.dumps({"version": 1, "attributes": [
            {"op": "add", "name": "width", "type": "float", "values": [.14, .20, .26]},
            {"op": "add", "name": "id", "type": "int64", "values": [2**63-3, 2**63-2, 2**63-1]},
            {"op": "add", "name": "label", "type": "string", "values": ["blue", "red", "blue"]}],
            "groups": [{"op": "add", "name": "keep", "values": [True, False, True]}]}, ensure_ascii=False)+"\n", encoding="utf-8")
        run("generate-points", [str(tool), "--points", str(points), str(raw), "--voxel-size", ".1", "--grid-name", "粒子"])
        run("generate-attributes", [str(tool), "--edit-point-attributes", str(raw), str(edits), str(native), "--input-grid", "粒子", "--grid-name", "粒子"])
        values = array("d", [(x+2)/4 for z in range(3) for y in range(5) for x in range(-2, 3)])
        dense = DenseVolume(DenseType.FLOAT64, (5, 5, 3), values, bytearray([1])*75, origin=(-2, -2, -1),
                            linear=(.5, 0, 0, 0, .5, 0, 0, 0, .5), grid_name="field")
        array_file, field = output/"field.cvarr", output/"field.vdb"
        write_dense_array(array_file, dense)
        run("generate-field", [str(tool), "--dense-import", str(array_file), str(field)])
        empty = capture("empty")
        slice_options = ["--vdb-slice", "--vdb-model-grid", "field", "--vdb-slice-extent", "2", "1.5",
                         "--vdb-slice-samples", "33", "25", "--vdb-slice-value", "scalar", "--vdb-slice-map", "blue-red", "--vdb-slice-range", "0", "1"]
        point_options = ["--vdb-native-points", "--vdb-model-grid", "粒子", "--vdb-native-radius-attribute", "width"]
        opaque_slice = capture("slice-opaque", field, slice_options)
        half_slice = capture("slice-half", field, [*slice_options, "--vdb-slice-opacity", ".5"])
        uniform = capture("native-uniform", native, [*point_options, "--vdb-native-tint", ".7", ".7", ".7"])
        scalar_options = [*point_options, "--vdb-native-color", "scalar", "--vdb-native-color-attribute", "id"]
        scalar = capture("native-int64", native, scalar_options)
        strings = capture("native-strings", native, [*point_options, "--vdb-native-color", "string", "--vdb-native-color-attribute", "label"])
        selected = capture("native-selected", native, [*scalar_options, "--vdb-native-any-group", "keep"])
        half_points = capture("native-half", native, [*scalar_options, "--vdb-native-opacity", ".5"])
        signals = {}
        for name, image in (("slice", opaque_slice), ("points", uniform), ("scalar", scalar), ("strings", strings)):
            metrics = foreground(empty, image)
            signals[name] = metrics
            report["comparisons"].append({"name": f"visible-{name}", **metrics})
            threshold = thresholds["minimumSliceVisibleFraction"] if name == "slice" else thresholds["minimumPointsVisibleFraction"]
            if metrics["changedFraction"] < threshold:
                raise AssertionError(f"{name} preview is not visibly rendered")
        radius_ratio = signals["points"]["rightChangedPixels"]/max(1, signals["points"]["leftChangedPixels"])
        report["comparisons"].append({"name": "physical-radius-attribute", "rightLeftAreaRatio": radius_ratio})
        if radius_ratio < thresholds["minimumRadiusAreaRatio"]:
            raise AssertionError("named physical radius did not change marker size")
        for name, image in (("int64", scalar), ("string", strings)):
            metrics = image_metrics(uniform, image)
            report["comparisons"].append({"name": f"attribute-color-{name}", **metrics})
            if metrics["changedFraction"] < thresholds["minimumAttributeColorChangedFraction"]:
                raise AssertionError(f"{name} attribute colors did not affect rendered markers")
        groups = image_metrics(scalar, selected)
        report["comparisons"].append({"name": "group-center-removal", **groups})
        if groups["centerPatchMeanAbsoluteError"] < thresholds["minimumGroupCenterDifference"]:
            raise AssertionError("group filtering did not remove the center marker")
        for name, reference, half in (("slice", opaque_slice, half_slice), ("points", scalar, half_points)):
            ratio = foreground(empty, half)["signal"]/max(1, foreground(empty, reference)["signal"])
            report["comparisons"].append({"name": f"opacity-{name}", "halfOpaqueSignalRatio": ratio})
            if not thresholds["minimumAlphaSignalRatio"] <= ratio <= thresholds["maximumAlphaSignalRatio"]:
                raise AssertionError(f"{name} opacity did not reduce visible image signal")
        for name, model, options, reference, maximum in (("slice", field, slice_options, opaque_slice, thresholds["maximumSliceTechniqueMeanError"]),
                                                       ("points", native, scalar_options, scalar, thresholds["maximumPointTechniqueMeanError"])):
            other = capture(f"forward-{name}", model, options, technique="forward-raster")
            metrics = image_metrics(reference, other)
            report["comparisons"].append({"name": f"technique-parity-{name}", **metrics})
            if metrics["meanAbsoluteError"] > maximum:
                raise AssertionError(f"{name} deferred/forward preview images differ excessively")
        report["passed"] = True
    except Exception as error:
        report["errors"].append(str(error))
        report["traceback"] = traceback.format_exc()
    finally:
        save()
    print(json.dumps({"passed": report["passed"], "results": str(output/"results.json")}, indent=2))
    return 0 if report["passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
