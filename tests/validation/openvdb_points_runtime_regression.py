"""Opt-in PointDataGrid mesh-marker and optional smoke-state rendering proof.

Requires an enabled renderer/tool, a Vulkan display/runtime and Pillow. Each
run keeps generated inputs, commands, validation logs, images and results.json
in a unique directory. --self-test checks image analysis without using Vulkan.
"""
from __future__ import annotations

import argparse
from datetime import datetime, timezone
import json
import math
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile
import time
import traceback
import uuid


WIDTH, HEIGHT = 320, 200
CAMERA_DISTANCE, CAMERA_FOV = 4.0, 45.0
PIXELS_PER_WORLD_UNIT = HEIGHT / (2 * math.tan(math.radians(CAMERA_FOV / 2))) / CAMERA_DISTANCE
RUNTIME_BUDGET_SECONDS = 480
VALIDATION_ERROR = re.compile(r"VUID-|Validation Error|SYNC-HAZARD", re.IGNORECASE)
RECORDS = [
    {"position": [-.8, 0, 0], "radius": .25, "velocity": [1, 0, 0]},
    {"position": [.8, 0, 0], "radius": .5, "velocity": [-1, 0, 0]},
]
SHIFT = .5


def pixels(image):
    getter = getattr(image, "get_flattened_data", None)
    return list(getter() if getter else image.getdata())


def projected_center(position):
    if position[2] != 0:
        raise ValueError("this fixed-camera fixture requires marker centers at z=0")
    return (WIDTH / 2 + position[0] * PIXELS_PER_WORLD_UNIT,
            HEIGHT / 2 - position[1] * PIXELS_PER_WORLD_UNIT)


def read_rgb(path):
    from PIL import Image
    with Image.open(path) as source:
        image = source.convert("RGB")
    if image.size != (WIDTH, HEIGHT):
        raise AssertionError(f"unexpected capture dimensions in {path}: {image.size}")
    return image


def largest_component(mask):
    remaining = set(mask)
    largest = set()
    while remaining:
        start = remaining.pop()
        component, stack = {start}, [start]
        while stack:
            x, y = stack.pop()
            for neighbor in ((x-1, y), (x+1, y), (x, y-1), (x, y+1)):
                if neighbor in remaining:
                    remaining.remove(neighbor)
                    component.add(neighbor)
                    stack.append(neighbor)
        if len(component) > len(largest):
            largest = component
    return largest


def marker_metrics(empty, capture, records):
    """Find actual silhouettes near analytically projected world-radius bounds."""
    from PIL import ImageChops
    from statistics import median

    baseline, actual = read_rgb(empty), read_rgb(capture)
    difference = ImageChops.difference(baseline, actual)
    delta, colors = pixels(difference), pixels(actual)
    markers = []
    for record in records:
        cx, cy = projected_center(record["position"])
        radius = record["radius"] * PIXELS_PER_WORLD_UNIT
        reach = math.ceil(radius * 1.3 + 4)
        region = (max(0, int(cx)-reach), max(0, int(cy)-reach),
                  min(WIDTH, int(cx)+reach+1), min(HEIGHT, int(cy)+reach+1))
        selected = {(x, y) for y in range(region[1], region[3])
                    for x in range(region[0], region[2])
                    if max(delta[y*WIDTH+x]) >= 16}
        component = largest_component(selected)
        if len(component) < 80:
            raise AssertionError(f"marker near {(cx, cy)} has only {len(component)} visible pixels")
        xs, ys = [p[0] for p in component], [p[1] for p in component]
        bounds = [min(xs), min(ys), max(xs)+1, max(ys)+1]
        width, height = bounds[2]-bounds[0], bounds[3]-bounds[1]
        center = [(bounds[0]+bounds[2]-1)/2, (bounds[1]+bounds[3]-1)/2]
        diameter = 2 * radius
        for label, measured in (("width", width), ("height", height)):
            if abs(measured-diameter) > max(4, diameter*.18):
                raise AssertionError(f"marker {label} {measured} disagrees with world radius {record['radius']} ({diameter:.2f}px)")
        if math.hypot(center[0]-cx, center[1]-cy) > 2.5:
            raise AssertionError(f"marker center {center} disagrees with projected world position {(cx, cy)}")
        interior = [(x, y) for x, y in component
                    if all(n in component for n in ((x-1,y), (x+1,y), (x,y-1), (x,y+1)))]
        if len(interior) < 40:
            raise AssertionError("marker has no meaningful interior color patch")
        median_color = [median(colors[y*WIDTH+x][channel] for x, y in interior)
                        for channel in range(3)]
        markers.append({"worldPosition": record["position"], "worldRadius": record["radius"],
                        "expectedCenter": [cx, cy], "bounds": bounds, "center": center,
                        "width": width, "height": height, "visiblePixels": len(component),
                        "medianRgb": median_color})
    for dimension in ("width", "height"):
        ratio = markers[1][dimension] / markers[0][dimension]
        if not 1.7 <= ratio <= 2.3:
            raise AssertionError(f"physical marker radii are not visibly unequal: {dimension} ratio {ratio}")
    positive, negative = (marker["medianRgb"] for marker in markers)
    if positive[0]-positive[1] < 10 or negative[1]-negative[0] < 10:
        raise AssertionError(f"signed +/-X velocity colors are not distinct: {positive}, {negative}")
    return {"markers": markers, "widthRatio": markers[1]["width"]/markers[0]["width"],
            "heightRatio": markers[1]["height"]/markers[0]["height"]}


def image_difference(reference, candidate):
    from PIL import ImageChops, ImageStat
    expected, actual = read_rgb(reference), read_rgb(candidate)
    delta = ImageChops.difference(expected, actual)
    return {"meanAbsoluteError": sum(ImageStat.Stat(delta).mean)/3,
            "maxChannelAbsoluteError": max(high for low, high in delta.getextrema()),
            "changedPixels": sum(max(pixel) >= 8 for pixel in pixels(delta)),
            "centerMeanAbsoluteError": sum(ImageStat.Stat(delta.crop((50, 55, 285, 145))).mean)/3}


def require_equivalent(metrics, name):
    if metrics["meanAbsoluteError"] > .35 or metrics["centerMeanAbsoluteError"] > 1:
        raise AssertionError(f"{name}: image differs from independently generated reference: {metrics}")


def self_test():
    from PIL import Image, ImageDraw
    with tempfile.TemporaryDirectory(prefix="container-vdb-points-analysis-") as directory:
        root = Path(directory)
        background = Image.new("RGB", (WIDTH, HEIGHT), (11, 13, 17))
        empty, original, shifted = (root/name for name in ("empty.png", "original.png", "shifted.png"))
        background.save(empty)
        for path, shift in ((original, 0), (shifted, SHIFT)):
            image, records = background.copy(), json.loads(json.dumps(RECORDS))
            draw = ImageDraw.Draw(image)
            for record, color in zip(records, ((220,140,140), (30,140,140))):
                record["position"][0] += shift
                cx, cy = projected_center(record["position"])
                r = record["radius"] * PIXELS_PER_WORLD_UNIT
                draw.polygon([(cx-r,cy), (cx,cy-r), (cx+r,cy), (cx,cy+r)], fill=color)
            image.save(path)
            marker_metrics(empty, path, records)
        require_equivalent(image_difference(original, original), "self equality")
        try:
            marker_metrics(empty, empty, RECORDS)
        except AssertionError:
            pass
        else:
            raise AssertionError("empty image was accepted as visible physical markers")
        try:
            wrong = json.loads(json.dumps(RECORDS))
            wrong[1]["radius"] = .25
            marker_metrics(empty, original, wrong)
        except AssertionError:
            pass
        else:
            raise AssertionError("incorrect/global-pixel marker radius was accepted")
    print("PASS: point-runtime image-analysis self-test")
    return 0


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--exe", type=Path)
    parser.add_argument("--tool", type=Path)
    parser.add_argument("--output-dir", type=Path)
    parser.add_argument("--simulation-parity", action="store_true",
                        help="also render a zero-motion Double M05 state against its Float fog source")
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()
    if args.self_test:
        return self_test()
    if os.environ.get("CONTAINER_RUN_OPENVDB_REGRESSION") != "1":
        print("SKIP: set CONTAINER_RUN_OPENVDB_REGRESSION=1")
        return 77
    if not all((args.exe, args.tool, args.output_dir)):
        parser.error("runtime execution requires --exe, --tool and --output-dir")
    try:
        from PIL import Image  # noqa: F401
    except ImportError:
        print("SKIP: point runtime image analysis requires Pillow")
        return 77
    stamp = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ")
    output = args.output_dir.resolve() / f"run-{stamp}-{os.getpid()}-{uuid.uuid4().hex[:8]}"
    output.mkdir(parents=True, exist_ok=False)
    exe, tool = args.exe.resolve(), args.tool.resolve()
    report = {"schemaVersion": 1, "passed": False, "outputDirectory": str(output),
              "simulationParityRequested": args.simulation_parity,
              "commands": [], "captures": {}, "comparisons": [], "errors": [],
              "thresholds": {"maskRgbDifference": 16, "minMarkerPixels": 80,
                             "maxCenterErrorPixels": 2.5, "radiusRatio": [1.7, 2.3],
                             "minSignedRedGreenDifference": 10, "maxEquivalentMeanError": .35,
                             "maxEquivalentCenterError": 1}}
    started = time.monotonic()
    deadline = started + RUNTIME_BUDGET_SECONDS
    (output / "vk_layer_settings.txt").write_text("khronos_validation.validate_sync = true\n", encoding="utf-8")
    environment = dict(os.environ, VK_LAYER_SETTINGS_PATH=str(output))

    def save():
        report["elapsedSeconds"] = round(time.monotonic()-started, 3)
        (output / "results.json").write_text(json.dumps(report, indent=2)+"\n", encoding="utf-8")

    def run(name, command):
        remaining = deadline-time.monotonic()
        if remaining <= 0:
            raise TimeoutError(f"point regression exceeded {RUNTIME_BUDGET_SECONDS}s budget")
        log = output / (name+".log")
        record = {"name": name, "argv": list(map(str, command)), "cwd": str(exe.parent),
                  "log": str(log), "timeoutSeconds": min(90, remaining)}
        report["commands"].append(record)
        save()
        begin = time.monotonic()
        try:
            with log.open("w", encoding="utf-8") as stream:
                process = subprocess.run(record["argv"], cwd=exe.parent, env=environment,
                                         stdout=stream, stderr=subprocess.STDOUT,
                                         timeout=min(90, remaining),
                                         creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
            record["returnCode"] = process.returncode
        except subprocess.TimeoutExpired as error:
            record["timedOut"] = True
            raise TimeoutError(f"{name} timed out; see {log}") from error
        finally:
            record["elapsedSeconds"] = round(time.monotonic()-begin, 3)
            save()
        messages = log.read_text(encoding="utf-8", errors="replace")
        record["validationErrors"] = [line[:300] for line in messages.splitlines() if VALIDATION_ERROR.search(line)]
        if record["validationErrors"]:
            raise AssertionError(f"{name} reported Vulkan validation errors; see {log}")
        if process.returncode != 0:
            raise AssertionError(f"{name} exited with {process.returncode}; see {log}")
        return messages

    def capture(name, technique, msaa, *, model=None, display="lit", volume=None):
        screenshot = output / (name+".png")
        command = [exe, "--model", str(model) if model else "", "--hidden", "--no-ui", "--validation",
                   "--width", WIDTH, "--height", HEIGHT, "--camera-position", 0, 0, CAMERA_DISTANCE,
                   "--camera-target", 0, 0, 0, "--camera-fov", CAMERA_FOV,
                   "--display-mode", display, "--render-technique", technique, "--msaa", msaa,
                   "--exposure", 1, "--directional-intensity", 2, "--environment-intensity", .5,
                   "--no-bloom", "--no-taa", "--no-ray-query", "--fixed-dt", .016666667,
                   "--warmup-frames", 3, "--capture-frame", 4, "--screenshot", screenshot]
        if volume:
            command += ["--vdb-volume", volume, "--vdb-grid", "density"]
        run(name, command)
        if not screenshot.is_file():
            raise AssertionError(f"{name} produced no screenshot")
        read_rgb(screenshot)
        report["captures"][name] = str(screenshot)
        save()
        return screenshot

    try:
        if not exe.is_file() or not tool.is_file():
            raise FileNotFoundError(f"renderer/tool executable is missing: {exe}, {tool}")
        source_json, shifted_json = output/"points.json", output/"shifted-reference.json"
        source, advected, reference, wind = (output/name for name in
            ("points.vdb", "advected.vdb", "shifted-reference.vdb", "wind.vdb"))
        shifted_records = json.loads(json.dumps(RECORDS))
        for record in shifted_records:
            record["position"][0] += SHIFT
        source_json.write_text(json.dumps(RECORDS, indent=2)+"\n", encoding="utf-8")
        shifted_json.write_text(json.dumps(shifted_records, indent=2)+"\n", encoding="utf-8")
        run("write-points", [tool, "--points", source_json, source, "--voxel-size", .125])
        run("write-shifted-reference", [tool, "--points", shifted_json, reference, "--voxel-size", .125])
        run("write-positive-wind", [tool, "--velocity", wind, "--vector", 1, 0, 0])
        original_bytes = source.read_bytes()
        run("advect-points", [tool, "--advect-points", source, wind, advected,
                              "--time", SHIFT, "--steps", 2, "--keep-velocity", "--voxel-size", .125])
        if source.read_bytes() != original_bytes:
            raise AssertionError("point advection mutated its source VDB")
        for label, technique, msaa, display in (("deferred", "deferred-raster", 1, "albedo"),
                                               ("forward-msaa4", "forward-raster", 4, "lit")):
            empty = capture(label+"-empty", technique, msaa, display=display)
            original = capture(label+"-points", technique, msaa, model=source, display=display)
            moved = capture(label+"-advected", technique, msaa, model=advected, display=display)
            expected = capture(label+"-reference", technique, msaa, model=reference, display=display)
            original_metrics = marker_metrics(empty, original, RECORDS)
            moved_metrics = marker_metrics(empty, moved, shifted_records)
            for before, after in zip(original_metrics["markers"], moved_metrics["markers"]):
                dx, dy = after["center"][0]-before["center"][0], after["center"][1]-before["center"][1]
                if abs(dx-SHIFT*PIXELS_PER_WORLD_UNIT)>2 or abs(dy)>1:
                    raise AssertionError(f"positive world-space advection did not move marker by its expected pixel displacement: {(dx,dy)}")
            report["comparisons"].append({"name": label+"-physical-radii-signed-colors", **original_metrics})
            report["comparisons"].append({"name": label+"-positive-advection", **moved_metrics})
            equivalent = image_difference(expected, moved)
            report["comparisons"].append({"name": label+"-independent-shifted-reference", **equivalent})
            require_equivalent(equivalent, label+" advection")
            save()
        if args.simulation_parity:
            sdf, fog, zero, state, stepped = (output/name for name in
                ("sphere.vdb", "fog.vdb", "zero-wind.vdb", "zero-mac.vdb", "stepped.vdb"))
            run("simulation-sphere", [tool, "--sphere", sdf, "--radius", .6, "--voxel-size", .1])
            run("simulation-fog", [tool, "--fog", sdf, fog, "--grid-name", "density"])
            run("simulation-zero-wind", [tool, "--velocity", zero, "--vector", 0, 0, 0])
            run("simulation-zero-mac", [tool, "--project", zero, fog, state,
                                        "--input-grid", "velocity", "--domain-grid", "density"])
            stats = json.loads(run("simulation-static-step", [tool, "--simulate", fog, state, state, stepped,
                                   "--input-grid", "density", "--velocity-grid", "velocity",
                                   "--domain-grid", "fluid", "--time", .1, "--steps", 2]))
            if stats.get("converged") is not True or stats.get("actualSubsteps") != 2:
                raise AssertionError(f"zero-motion M05 did not perform its two requested steps: {stats}")
            report["simulationStatistics"] = stats
            inspected = json.loads(run("simulation-inspect", [tool, "--inspect", stepped]))
            density = next(grid for grid in inspected["grids"] if grid["name"] == "density")
            if density["valueType"] != "double":
                raise AssertionError(f"M05 parity fixture did not exercise Double density: {density}")
            for label, technique, msaa in (("deferred", "deferred-raster", 1),
                                            ("forward-msaa4", "forward-raster", 4)):
                before = capture(label+"-float-fog", technique, msaa, volume=fog)
                after = capture(label+"-double-smoke", technique, msaa, volume=stepped)
                equivalent = image_difference(before, after)
                report["comparisons"].append({"name": label+"-float-double-zero-motion-parity", **equivalent})
                require_equivalent(equivalent, label+" zero-motion Float/Double density")
        report["passed"] = True
    except Exception as error:
        report["errors"].append({"type": type(error).__name__, "message": str(error),
                                 "traceback": traceback.format_exc()})
        print(f"FAIL: {error}", file=sys.stderr)
    finally:
        save()
        print(f"OpenVDB point runtime results: {output/'results.json'}", flush=True)
    return 0 if report["passed"] else 1


if __name__ == "__main__":
    sys.exit(main())
