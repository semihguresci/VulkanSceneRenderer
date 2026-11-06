"""Opt-in rendering check for segmented VDB surfaces and mesh interchange."""

from __future__ import annotations

import argparse
from datetime import datetime, timezone
import json
import os
from pathlib import Path
import re
import subprocess
import time
import traceback
import uuid

from openvdb_runtime_regression import HEIGHT, WIDTH, image_metrics
from openvdb_transport_runtime_regression import foreground_metrics


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--exe", required=True, type=Path)
    parser.add_argument("--tool", required=True, type=Path)
    parser.add_argument("--output-dir", required=True, type=Path)
    args = parser.parse_args()
    if os.environ.get("CONTAINER_RUN_OPENVDB_INTERCHANGE_GPU") != "1":
        print("SKIP: set CONTAINER_RUN_OPENVDB_INTERCHANGE_GPU=1")
        return 77
    try:
        from PIL import Image  # noqa: F401
    except ImportError:
        print("SKIP: interchange image comparisons require Pillow")
        return 77
    exe, tool = args.exe.resolve(), args.tool.resolve()
    stamp = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ")
    output = args.output_dir.resolve() / f"run-{stamp}-{os.getpid()}-{uuid.uuid4().hex[:8]}"
    output.mkdir(parents=True, exist_ok=False)
    started = time.monotonic()
    report = {"schemaVersion": 1, "passed": False, "outputDirectory": str(output),
              "commands": [], "captures": {}, "comparisons": [], "errors": []}
    (output / "vk_layer_settings.txt").write_text(
        "khronos_validation.validate_sync = true\n", encoding="utf-8")
    environment = dict(os.environ, VK_LAYER_SETTINGS_PATH=str(output))

    def save():
        report["elapsedSeconds"] = round(time.monotonic() - started, 3)
        (output / "results.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")

    def run(name, command):
        remaining = 180 - (time.monotonic() - started)
        if remaining <= 0:
            raise TimeoutError("interchange regression exceeded 180 seconds")
        log = output / f"{name}.log"
        record = {"name": name, "argv": command, "log": str(log), "cwd": str(exe.parent)}
        report["commands"].append(record)
        save()
        with log.open("w", encoding="utf-8") as stream:
            result = subprocess.run(command, cwd=exe.parent, env=environment, stdout=stream,
                                    stderr=subprocess.STDOUT, timeout=min(60, remaining),
                                    creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
        record["returnCode"] = result.returncode
        text = log.read_text(encoding="utf-8", errors="replace")
        record["validationErrors"] = sorted(set(re.findall(
            r"VUID-|Validation Error|SYNC-HAZARD", text, flags=re.IGNORECASE)))
        save()
        if record["validationErrors"] or result.returncode:
            raise AssertionError(f"{name} failed; see {log}")
        return text

    def capture(name, model=None):
        screenshot = output / f"{name}.png"
        run(name, [str(exe), "--model", str(model) if model else "", "--hidden", "--no-ui",
                   "--validation", "--display-mode", "lit", "--width", str(WIDTH),
                   "--height", str(HEIGHT), "--camera-position", "0", "0", "6",
                   "--camera-target", "0", "0", "0", "--camera-fov", "45", "--exposure", "1",
                   "--directional-intensity", "2", "--environment-intensity", ".5",
                   "--no-bloom", "--no-taa", "--no-ray-query", "--fixed-dt", "0.016666667",
                   "--warmup-frames", "3", "--capture-frame", "4", "--render-technique",
                   "deferred-raster", "--msaa", "1", "--screenshot", str(screenshot)])
        if not screenshot.is_file():
            raise AssertionError(f"{name} did not produce a screenshot")
        report["captures"][name] = str(screenshot)
        save()
        return screenshot

    try:
        if not exe.is_file() or not tool.is_file():
            raise FileNotFoundError("renderer and tool must exist")
        particles = output / "particles.json"
        particles.write_text(json.dumps([
            {"position": [-.8, -.1, 0], "radius": .55},
            {"position": [.8, .1, 0], "radius": .55},
        ]) + "\n", encoding="utf-8")
        original, segmented, obj = output / "original.vdb", output / "segmented.vdb", output / "pieces.obj"
        run("generate-particles", [str(tool), "--particles", str(particles), str(original),
                                    "--voxel-size", ".05"])
        run("segment-surfaces", [str(tool), "--segment", str(original), str(segmented),
                                  "--grid-name", "piece"])
        inspection = json.loads(run("inspect-components", [str(tool), "--inspect", str(segmented), "--stats"]))
        if [grid["name"] for grid in inspection["grids"]] != ["piece_000", "piece_001"]:
            raise AssertionError("expected two named components")
        report["inspection"] = inspection
        run("export-components", [str(tool), "--export-mesh", str(segmented), str(obj)])
        obj_lines = obj.read_text(encoding="utf-8").splitlines()
        report["mesh"] = {"groups": sum(line.startswith("g ") for line in obj_lines),
                          "vertices": sum(line.startswith("v ") for line in obj_lines),
                          "normals": sum(line.startswith("vn ") for line in obj_lines),
                          "triangles": sum(line.startswith("f ") for line in obj_lines)}
        if (report["mesh"]["groups"] != 2 or report["mesh"]["triangles"] == 0 or
                report["mesh"]["vertices"] != report["mesh"]["normals"]):
            raise AssertionError("OBJ export is incomplete")
        empty = capture("empty")
        source = capture("original-surface", original)
        pieces = capture("segmented-surfaces", segmented)
        for label, image in (("original", source), ("segmented", pieces)):
            visibility = foreground_metrics(empty, image)
            report["comparisons"].append({"name": f"visible-{label}", **visibility})
            if visibility["changedFraction"] < .005:
                raise AssertionError(f"{label} surfaces are not visibly rendered")
        parity = image_metrics(source, pieces)
        report["comparisons"].append({"name": "segmented-versus-original", **parity})
        if parity["maxChannelAbsoluteError"] > 1:
            raise AssertionError("segmentation changed the rendered surface")
        report["passed"] = True
    except Exception as error:
        report["errors"].append(str(error))
        (output / "exception.txt").write_text(traceback.format_exc(), encoding="utf-8")
    finally:
        save()
    print(json.dumps({"passed": report["passed"], "results": str(output / "results.json"),
                      "elapsedSeconds": report["elapsedSeconds"], "errors": report["errors"]}, indent=2))
    return 0 if report["passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
