"""Check BIM floor-plan display, depth, and scene-linear capture across techniques."""

import argparse
import hashlib
import json
import math
import os
from pathlib import Path
import subprocess
import tempfile

from display_capture_test_support import validation_log_error


WIDTH, HEIGHT = 320, 240
POSITION, TARGET = [0, 3, 8], [0, 1, 0]
FOV = 45
FRAMES = {"off": 4, "ground": 5, "source": 6, "dim": 7, "narrow": 8, "depth": 9, "off-again": 10}
COMPARISONS = ("path-tracing", "forward-raster")


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


def fixture(path):
    def panel(mesh_id, bounds, z):
        left, bottom, right, top = bounds
        # Dotbim Z-up axes map (x, y, z) to (x, z, -y) in renderer space.
        points = [[x, -z, y] for x, y in ((left, bottom), (right, bottom), (right, top), (left, top))]
        return {"mesh_id": mesh_id, "coordinates": [value for point in points for value in point],
                "indices": [0, 1, 2, 0, 2, 3]}

    model = {
        "schema_version": "1.1.0",
        "meshes": [panel(1, (-2, 1.2, 2, 2.1), 0), panel(2, (-2.2, .2, -.25, .8), 1.2)],
        "elements": [
            {"mesh_id": 1, "guid": "wall", "type": "IfcWall", "storeyName": "Upper",
             "color": {"r": 164, "g": 164, "b": 164, "a": 255}},
            # Roofs contribute depth, but the floor-plan extractor excludes them.
            {"mesh_id": 2, "guid": "occluder", "type": "IfcRoof",
             "color": {"r": 202, "g": 202, "b": 202, "a": 255}},
        ],
    }
    path.write_text(json.dumps(model, indent=2) + "\n", encoding="utf-8")


def settings(*, enabled=True, elevation="projected-ground", depth=False, color=(1, 0, 0), opacity=1, width=5):
    return {"enabled": enabled, "elevation": elevation, "depthTest": depth,
            "color": list(color), "opacity": opacity, "lineWidth": width}


def sequence_plan(mode, comparison):
    if mode == "styled":
        events = [
            (4, settings(enabled=False)),
            (5, settings()),
            (6, settings(elevation="source-elevation", color=(0, 0, 1))),
            (7, settings(opacity=.25)),
            (8, settings(width=1)),
            (9, settings(depth=True)),
            (10, settings(enabled=False)),
        ]
        return list(FRAMES.values()), [{"frame": frame, "bimFloorPlan": style} for frame, style in events]
    if mode == "linear":
        events = [
            (1, settings(enabled=False)), (2, settings()),
            (3, settings(elevation="source-elevation", color=(0, 0, 1))),
            (4, settings(depth=True, opacity=.25, width=1)), (5, settings(enabled=False)),
        ]
        return [1, 2, 3, 4, 5], [{"frame": frame, "bimFloorPlan": style} for frame, style in events]
    if mode == "linear-control":
        return [1, 2, 3, 4, 5], []
    if mode == "switch":
        return [4, 8, 12], [
            {"frame": 1, "bimFloorPlan": settings(elevation="source-elevation", color=(0, 0, 1))},
            {"frame": 5, "technique": comparison},
            {"frame": 9, "technique": "deferred-raster"},
        ]
    raise ValueError(mode)


def read_capture(image, frame, technique):
    import cv2
    import numpy as np
    from PIL import Image

    metadata = json.loads(Path(f"{image}.capture.json").read_bytes())
    state = json.loads(image.with_suffix(".telemetry.json").read_bytes())
    require((metadata["width"], metadata["height"], metadata["includesUi"]) == (WIDTH, HEIGHT, False), metadata)
    require(state["technique"] == technique and state["validationEnabled"], state.get("technique"))
    if image.suffix == ".exr":
        source = "path-trace-mean" if technique == "path-tracing" else "raster-scene-composite"
        require(metadata["source"] == source and metadata["colorSpace"] == "linear-sRGB"
                and not metadata["exposureApplied"] and not metadata["toneMappingApplied"], metadata)
        bgr = cv2.imdecode(np.frombuffer(image.read_bytes(), dtype=np.uint8), cv2.IMREAD_UNCHANGED)
        require(bgr is not None and bgr.shape == (HEIGHT, WIDTH, 3) and bgr.dtype == np.float32
                and np.isfinite(bgr).all(), f"Invalid raw scene-linear capture {image}")
        pixels = bgr[:, :, ::-1]
    else:
        require(metadata["source"] == "viewport", metadata)
        with Image.open(image) as png:
            require(png.format == "PNG" and png.size == (WIDTH, HEIGHT), image)
            pixels = np.asarray(png.convert("RGB"))
    return {"frame": frame, "image": str(image), "state": state, "pixels": pixels,
            "metadata": metadata, "decodedSha256": hashlib.sha256(pixels.tobytes()).hexdigest()}


def check_floor_plan_telemetry(sample, style):
    state, metadata = sample["state"], sample["metadata"]
    bim = state["bim"]
    require(metadata["bim"] == bim, "Image sidecar and CLI BIM telemetry disagree")
    require(bim["modelFormat"] == "bim" and bim["revision"] == state["build"]["revision"]
            and bim["technique"] == state["technique"], bim)
    overlay = bim["floorPlan"]
    if style is None:
        style = settings(enabled=False, color=(.02, .02, .02), opacity=.85, width=1)
    require(overlay["requested"] == overlay["enabled"] == style["enabled"], overlay)
    for key in ("elevation", "depthTest", "lineWidth"):
        require(overlay[key] == style[key], overlay)
    require(math.isclose(overlay["opacity"], style["opacity"], abs_tol=1e-6)
            and all(math.isclose(actual, expected, abs_tol=1e-6)
                    for actual, expected in zip(overlay["color"], style["color"], strict=True)), overlay)
    width = style["lineWidth"] if state["wideLinesSupported"] else 1
    require(overlay["rasterLineWidth"] == width, overlay)
    projected, source = overlay["projectedGroundDrawCount"], overlay["sourceElevationDrawCount"]
    require(projected > 0 and source > 0, overlay)
    require(overlay["drawCount"] == (source if style["elevation"] == "source-elevation" else projected), overlay)
    pipeline = "bim-floor-plan-display" if state["technique"] == "path-tracing" else "bim-floor-plan"
    require(overlay["pipelineReady"] and overlay["pipeline"] ==
            f"{pipeline}-{'depth' if style['depthTest'] else 'no-depth'}", overlay)


def record(renderer, model, output, technique, mode, report, *, comparison, generate_only, check_telemetry):
    frames, events = sequence_plan(mode, comparison)
    name = f"{technique}-{mode}"
    sequence = output / f"{name}.sequence.json"
    sequence.write_text(json.dumps({"schemaVersion": 1, "frames": frames[-1], "sampleFrames": frames,
                                    "events": events}, indent=2) + "\n", encoding="utf-8")
    extension = "exr" if mode.startswith("linear") else "png"
    screenshot = output / f"{name}.{extension}"
    command = [str(renderer), "--model", str(model), "--import-scale", "1", "--hidden", "--no-ui", "--validation",
               "--width", str(WIDTH), "--height", str(HEIGHT), "--msaa", "1", "--no-taa", "--no-bloom",
               "--render-technique", technique, "--display-mode", "lit", "--exposure", "1",
               "--camera-position", *map(str, POSITION), "--camera-target", *map(str, TARGET), "--camera-fov", str(FOV),
               "--sky", "plain", "--sky-horizon", "0", "0", "0", "--environment-intensity", "0",
               "--directional-intensity", "2", "--directional-direction", "-.3", "-.2", "-1", "--bounce-intensity", "0",
               "--path-spp", "4", "--path-samples", "13", "--path-bounces", "1", "--path-seed", "23",
               "--fixed-dt", ".016666667", "--screenshot-viewport", "--capture-sequence", str(sequence),
               "--screenshot", str(screenshot)]
    log = screenshot.with_suffix(".log")
    row = {"technique": technique, "mode": mode, "command": command, "log": str(log), "captures": []}
    report["runs"].append(row)
    if generate_only:
        return []
    with log.open("w", encoding="utf-8") as stream:
        result = subprocess.run(command, cwd=renderer.parent, env=dict(os.environ, VK_LAYER_SETTINGS_PATH=str(output)),
                                stdout=stream, stderr=subprocess.STDOUT, timeout=150,
                                creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
    row["exitCode"] = result.returncode
    text = log.read_text(encoding="utf-8", errors="replace")
    require(result.returncode == 0 and not validation_log_error(text), f"Vulkan capture failed: {log}")
    if os.environ.get("VK_LOADER_DEBUG") == "layer":
        require('Insert instance layer "VK_LAYER_KHRONOS_validation"' in text, f"Validation layer was not loaded: {log}")
    samples = []
    for frame in frames:
        image = screenshot if frame == frames[-1] else output / f"{name}.frame-{frame:04}.{extension}"
        active_technique = comparison if mode == "switch" and frame == 8 else technique
        sample = read_capture(image, frame, active_technique)
        state = sample["state"]
        require(Path(state["scene"]["auxiliary"]).resolve() == model, "The authored BIM fixture was not loaded")
        if check_telemetry:
            style = next((event["bimFloorPlan"] for event in reversed(events)
                          if event["frame"] <= frame and "bimFloorPlan" in event), None)
            check_floor_plan_telemetry(sample, style)
        row["captures"].append({key: sample[key] for key in ("frame", "image", "metadata", "decodedSha256")}
                               | {"technique": state["technique"], "referencePath": state.get("referencePath"),
                                  "revision": state["build"]["revision"]})
        samples.append(sample)
    return samples


def analytic_line_row(y):
    view_angle = math.atan2(POSITION[1] - TARGET[1], POSITION[2])
    dy, dz = y - POSITION[1], -POSITION[2]
    camera_y = math.cos(view_angle) * dy - math.sin(view_angle) * dz
    distance = -math.sin(view_angle) * dy - math.cos(view_angle) * dz
    return HEIGHT * (1 - camera_y / (distance * math.tan(math.radians(FOV) / 2))) / 2


def line_mask(pixels, *, channel=0):
    import numpy as np

    rgb = pixels.astype(np.int16)
    return (rgb[:, :, channel] > 60) & (rgb[:, :, channel] > np.max(np.delete(rgb, channel, axis=2), axis=2) + 25)


def check_band(mask, y):
    import numpy as np

    rows, cols = np.where(mask)
    require(len(cols) > 100 and abs(float(np.median(rows)) - analytic_line_row(y)) <= 2,
            f"Expected a line at projected world height {y}, observed {len(cols)} pixels")
    return len(cols), [int(cols.min()), int(rows.min()), int(cols.max()), int(rows.max())]


def fixed_camera(samples):
    first = samples[0]["state"]["camera"]["unjitteredViewProjColumns"]
    require(all(s["state"]["camera"]["unjitteredViewProjColumns"] == first for s in samples),
            "The fixed camera changed while exercising floor-plan settings")


def check_styled(samples, *, missing=False):
    import numpy as np

    fixed_camera(samples)
    images = {name: sample["pixels"] for name, sample in zip(FRAMES, samples, strict=True)}
    for name in ("off", "off-again"):
        require(not line_mask(images[name]).any() and not line_mask(images[name], channel=2).any(), name)
    np.testing.assert_array_equal(images["off"], images["off-again"])
    if missing:
        for name, image in images.items():
            np.testing.assert_array_equal(image, images["off"], err_msg=f"Expected baseline to omit {name}")
        return {"baselineOverlayMissing": True}
    ground = line_mask(images["ground"])
    source = line_mask(images["source"], channel=2)
    ground_count, ground_bounds = check_band(ground, 0)
    source_count, source_bounds = check_band(source, 1.2)
    require(not np.any(ground & source), "Both elevation modes occupy the same pixels")
    require(not line_mask(images["ground"], channel=2).any() and not line_mask(images["source"]).any(),
            "The authored overlay colors were not honored")
    ground_rgb = images["ground"].astype(np.int16)
    dim_rgb = images["dim"].astype(np.int16)
    off_rgb = images["off"].astype(np.int16)
    strong_delta = np.abs(ground_rgb - off_rgb)[ground].mean()
    dim_delta = np.abs(dim_rgb - off_rgb)[ground].mean()
    require(0 < dim_delta < strong_delta, "Opacity did not reduce visible line contribution")
    narrow = line_mask(images["narrow"])
    narrow_count = int(np.count_nonzero(narrow))
    require(0 < narrow_count <= ground_count and np.any(narrow & ground), "Line-width control lost its centerline")
    wide_lines = samples[0]["state"].get("wideLinesSupported")
    if wide_lines is not None:
        require((narrow_count < ground_count) == wide_lines,
                f"Unexpected line-width coverage for wideLinesSupported={wide_lines}")
    depth = line_mask(images["depth"])
    left, right = slice(0, WIDTH // 2 - 10), slice(WIDTH // 2 + 10, WIDTH)
    left_removed = int(np.count_nonzero(ground[:, left] & ~depth[:, left]))
    right_removed = int(np.count_nonzero(ground[:, right] & ~depth[:, right]))
    require(left_removed > 50 and right_removed == 0 and np.count_nonzero(depth[:, right]) > 50,
            f"Depth mode must hide the covered left segment and retain the right segment: {left_removed}, {right_removed}")
    return {"groundPixels": ground_count, "groundBounds": ground_bounds, "sourcePixels": source_count,
            "sourceBounds": source_bounds, "narrowPixels": narrow_count, "wideLinesSupported": wide_lines,
            "lineWidthChangedCoverage": narrow_count < ground_count,
            "fullOpacityMeanDelta": float(strong_delta), "quarterOpacityMeanDelta": float(dim_delta),
            "depthRemovedLeftPixels": left_removed, "depthRemovedRightPixels": right_removed}


def check_linear(samples, controls, *, path):
    import numpy as np

    fixed_camera([*samples, *controls])
    require(float(np.max(samples[0]["pixels"])) > .01, "Scene-linear comparison is entirely dark")
    for sample, control in zip(samples, controls, strict=True):
        np.testing.assert_array_equal(sample["pixels"], control["pixels"], err_msg=f"Overlay changed raw frame {sample['frame']}")
    resets = []
    attempted = []
    if path:
        for sample, control, count in zip(samples, controls, (4, 8, 12, 13, 13), strict=True):
            for capture in (sample, control):
                ref = capture["state"]["referencePath"]
                metadata = capture["metadata"]["referencePath"]
                require(ref["active"] and ref["attemptedSamples"] == metadata["attemptedSamplesPerPixel"] == count
                        and ref["historyResets"] == metadata["historyResets"] == 1
                        and ref["unsupportedSamples"] == ref["rejectedSamples"] == 0, ref)
            attempted.append(count)
            resets.append(sample["state"]["referencePath"]["historyResets"])
    return {"rawFramesExact": len(samples), "attemptedSamples": attempted, "historyResets": resets}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--renderer", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--comparison", choices=COMPARISONS, default="path-tracing",
                        help="compare this technique with Deferred raster")
    parser.add_argument("--generate-only", action="store_true")
    parser.add_argument("--managed-output", action="store_true", help="write to a new run directory under --output")
    baseline = parser.add_mutually_exclusive_group()
    baseline.add_argument("--expect-missing-path-overlay", action="store_true",
                          help="expect the pre-fix Path tracing behavior on a build with capture controls")
    baseline.add_argument("--expect-missing-forward-overlay", action="store_true",
                          help="expect the pre-fix Forward raster behavior on a build with capture controls")
    args = parser.parse_args()
    if ((args.expect_missing_path_overlay and args.comparison != "path-tracing") or
            (args.expect_missing_forward_overlay and args.comparison != "forward-raster")):
        parser.error("The baseline option must match --comparison")
    expect_missing = args.expect_missing_path_overlay or args.expect_missing_forward_overlay
    if not args.generate_only and os.environ.get("CONTAINER_RUN_GPU_BIM_FLOOR_PLAN") != "1":
        print("SKIP: set CONTAINER_RUN_GPU_BIM_FLOOR_PLAN=1 to run GPU captures")
        return 77
    os.environ.setdefault("OPENCV_IO_ENABLE_OPENEXR", "1")
    renderer, output = args.renderer.resolve(), args.output.resolve()
    if args.managed_output:
        output.mkdir(parents=True, exist_ok=True)
        output = Path(tempfile.mkdtemp(prefix="run-", dir=output))
        print(f"Captures: {output}", flush=True)
    else:
        output.mkdir(parents=True)
    (output / "vk_layer_settings.txt").write_text("khronos_validation.validate_sync = true\n", encoding="utf-8")
    model = output / "floor-plan.bim"
    fixture(model)
    report = {"fixture": str(model), "comparison": args.comparison,
              "expectedMissingPathOverlay": args.expect_missing_path_overlay,
              "expectedMissingForwardOverlay": args.expect_missing_forward_overlay,
              "passed": False, "runs": [], "checks": {}}
    if not args.generate_only:
        report["executableSha256"] = hashlib.sha256(renderer.read_bytes()).hexdigest()
    try:
        for technique in ("deferred-raster", args.comparison):
            styled, linear, controls = (record(renderer, model, output, technique, mode, report,
                                               comparison=args.comparison, generate_only=args.generate_only,
                                               check_telemetry=not expect_missing)
                                        for mode in ("styled", "linear", "linear-control"))
            if args.generate_only:
                continue
            path = technique == "path-tracing"
            missing = technique == args.comparison and expect_missing
            report["checks"][technique] = {"viewport": check_styled(styled, missing=missing),
                                           "sceneLinear": check_linear(linear, controls, path=path)}
            print(technique, report["checks"][technique], flush=True)
        switched = record(renderer, model, output, "deferred-raster", "switch", report,
                          comparison=args.comparison, generate_only=args.generate_only,
                          check_telemetry=not expect_missing)
        if args.generate_only:
            return 0
        import numpy as np

        fixed_camera(switched)
        np.testing.assert_array_equal(switched[0]["pixels"], switched[2]["pixels"])
        blue = [int(np.count_nonzero(line_mask(sample["pixels"], channel=2))) for sample in switched]
        require(blue[0] == blue[2] and blue[0] > 100 and (blue[1] == 0 if expect_missing else blue[1] > 100),
                f"Unexpected source-elevation overlay after switching renderers: {blue}")
        report["checks"]["liveTechniqueSwitchSourcePixels"] = blue
        require(hashlib.sha256(renderer.read_bytes()).hexdigest() == report["executableSha256"],
                "Renderer executable changed during capture")
        report["passed"] = True
    finally:
        (output / "results.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
