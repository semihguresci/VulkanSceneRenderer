"""Compare individual BIM annotation routes in forward and deferred raster captures."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import tempfile

from bim_floor_plan_runtime_regression import OVERVIEW_EXTENT, overview_panel_slice, panel
from display_capture_test_support import emitter_fixture, validation_log_error
from physics_auxiliary_regression import cube


WIDTH, HEIGHT = 320, 240
TECHNIQUES = ("deferred-raster", "forward-raster")
FRAMES = {
    "off": 4, "issue-pin": 5, "clash-marker": 6, "section-visual": 7,
    "cap-fill": 8, "cap-hatching": 9, "section-markers": 10, "hidden-lines": 11,
    "bim-hover": 12, "mesh-hover": 13, "off-again": 14,
}
TECHNICAL_FRAMES = {"technical-depth": 15, "technical-no-depth": 16, "technical-off": 17}
TECHNICAL_PREVIEWS = ("technical-depth", "technical-no-depth")
PIN_DEPTH_ZONES = (
    ("clear", -1.2, (), 0),
    ("behind", -.4, (-1.5, -1.2), 166),
    ("front", .4, (1.2, 1.5), 166),
    ("opaque", 1.2, (1.5,), 255),
)
PIN_DEPTH_STAGES = ("off", *(zone[0] for zone in PIN_DEPTH_ZONES), "off-again")
PIN_HEIGHT = 1.2


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


def fixtures(folder):
    mesh = folder / "hover.gltf"
    asset = emitter_fixture(mesh, {
        "emissiveFactor": [.45, .45, .45],
        "extensions": {"KHR_materials_emissive_strength": {"emissiveStrength": 1}},
        "pbrMetallicRoughness": {"baseColorFactor": [.5, .5, .5, 1]},
    })
    asset["nodes"][0].update(translation=[1.65, 1, -.3], scale=[.5, .7, 1])
    mesh.write_text(json.dumps(asset), encoding="utf-8")

    bim = folder / "annotations.bim"
    points, _, indices = cube()
    meshes, elements = [], []
    boxes = [((-1.2, 1., 0.), (1.2, 1.5, 1.6)),
             ((-.35, .9, -.35), (1.2, 1.5, 1.6)),
             # Entirely behind the clipped left wall from the fixed camera.
             # Only the no-depth Technical route should show its wireframe.
             ((-1.35, 1., -1.3), (.62, .62, .35))]
    for index, (center, size) in enumerate(boxes):
        points_world = [(size[0] * x + center[0], size[1] * y + center[1], size[2] * z + center[2])
                        for x, y, z in points]
        # DotBIM has Z up and Y forward. Author coordinates in its native axes.
        coordinates = [component for x, y, z in points_world for component in (x, -z, y)]
        meshes.append(dict(mesh_id=index, coordinates=coordinates, indices=indices))
        elements.append(dict(mesh_id=index, guid=f"wall-{index}", type="IfcWall",
                             color=dict(r=160, g=160, b=160, a=255)))
    bim.write_text(json.dumps(dict(schema_version="1.1.0", meshes=meshes, elements=elements)), encoding="utf-8")
    return mesh, bim


def pin_depth_fixture(path):
    meshes, elements = [], []
    for label, x, depths, alpha in PIN_DEPTH_ZONES:
        for z in depths:
            mesh_id = len(meshes) + 1
            # The octahedral pin extends 0.35 units either side of its center.
            # Keep all wire tips covered without touching either adjacent zone.
            meshes.append(panel(mesh_id, (x - .385, PIN_HEIGHT - .40, x + .385, PIN_HEIGHT + .40), z))
            elements.append(dict(mesh_id=mesh_id, guid=f"{label}-{mesh_id}", type="IfcRoof",
                                 color=dict(r=180, g=180, b=180, a=alpha)))
    path.write_text(json.dumps(dict(schema_version="1.1.0", meshes=meshes, elements=elements), indent=2) + "\n",
                    encoding="utf-8")


def pin_depth_sequence(path, techniques, stages):
    frames = {}
    events = [{"frame": 2, "orthographic": True}]
    annotations = {label: {"issuePin": [x, PIN_HEIGHT, 0]} for label, x, _, _ in PIN_DEPTH_ZONES}
    active_technique = techniques[0]
    for technique in techniques:
        for stage in stages:
            frame = (len(frames) + 1) * 4
            frames[f"{technique}/{stage}"] = frame
            event = {"frame": frame - 1, "bimAnnotations": annotations.get(stage, {})}
            if technique != active_technique:
                event["technique"] = technique
                active_technique = technique
            events.append(event)
    path.write_text(json.dumps(dict(schemaVersion=1, frames=max(frames.values()), sampleFrames=list(frames.values()),
                                    events=events), indent=2) + "\n", encoding="utf-8")
    return frames


def sequence_plan(frames):
    snapshots = {
        "off": {}, "issue-pin": {"issuePin": [-1.8, 2.45, -.25]},
        "clash-marker": {"clash": {"objects": [0, 1], "position": [1.8, 2.45, -.25]}},
        "section-visual": {"sectionVisual": True}, "cap-fill": {"capFill": True},
        "cap-hatching": {"capHatch": True}, "section-markers": {"sectionMarkers": True},
        "hidden-lines": {"hiddenLines": True}, "bim-hover": {},
    }
    if "technical-depth" in frames:
        snapshots.update({"technical-depth": {"technicalLines": True},
                          "technical-no-depth": {"technicalLines": False}, "technical-off": {}})
    events = {frames[name]: {"frame": frames[name], "bimAnnotations": state} for name, state in snapshots.items()}
    # Hold the real section plane fixed for all preview and raw captures. Hover
    # is resolved against the completed frame and affects the following draw.
    events[1] = {"frame": 1, "sectionPlane": [0, 0, -1, .15], "bimAnnotations": {},
                 "viewportPointer": {"action": "select", "pixel": [4, 4]}}
    for frame, pixel in ((11, [114, 120]), (12, [217, 120]), (13, [4, 4])):
        events.setdefault(frame, {"frame": frame})["viewportPointer"] = {"action": "hover", "pixel": pixel}
    return dict(schemaVersion=1, frames=max(frames.values()), sampleFrames=list(frames.values()),
                events=[events[frame] for frame in sorted(events)])


def read_capture(path, technique, *, extent=(WIDTH, HEIGHT), mode=None):
    import cv2
    import numpy as np
    from PIL import Image

    metadata = json.loads(Path(f"{path}.capture.json").read_bytes())
    state = json.loads(path.with_suffix(".telemetry.json").read_bytes())
    width, height = extent
    require((metadata["width"], metadata["height"], metadata["includesUi"]) == (width, height, False) and
            state["technique"] == technique and state["validationEnabled"], f"Wrong capture state for {path}")
    if path.suffix == ".exr":
        require(metadata["source"] == "raster-scene-composite" and metadata["colorSpace"] == "linear-sRGB" and
                not metadata["exposureApplied"] and not metadata["toneMappingApplied"], f"Wrong EXR metadata for {path}")
        bgr = cv2.imdecode(np.frombuffer(path.read_bytes(), dtype=np.uint8), cv2.IMREAD_UNCHANGED)
        require(bgr is not None and bgr.shape == (height, width, 3) and bgr.dtype == np.float32 and
                np.isfinite(bgr).all(), f"Invalid scene-linear radiance {path}")
        return bgr[:, :, ::-1], state
    require(metadata["source"] == "viewport"
            and (mode is None or metadata["displayMode"] == (8 if mode == "overview" else 0)),
            f"Wrong viewport capture route for {path}")
    with Image.open(path) as image:
        require(image.format == "PNG" and image.size == extent, f"Wrong viewport image {path}")
        return np.asarray(image.convert("RGB")), state


def record(renderer, output, model, bim, sequence, frames, technique, extension, report, generate_only, *,
           mode="lit", pin_ordering=False):
    name = f"issue-pin-ordering-{mode}-{extension}" if pin_ordering else f"{technique}-{extension}"
    screenshot = output / f"{name}.{extension}"
    log = output / f"{name}.log"
    extent = OVERVIEW_EXTENT if pin_ordering else (WIDTH, HEIGHT)
    width, height = extent
    camera_height = str(PIN_HEIGHT) if pin_ordering else "1"
    model_args = ["--model", str(bim)] if pin_ordering else ["--model", str(model), "--bim-model", str(bim)]
    command = [str(renderer), *model_args, "--import-scale", "1",
               "--hidden", "--no-ui", "--validation", "--width", str(width), "--height", str(height),
               "--msaa", "1", "--no-taa", "--no-bloom", "--render-technique", technique,
               "--display-mode", mode, "--camera-position", "0", camera_height, "8", "--camera-target", "0", camera_height, "0",
               "--camera-fov", "45", "--sky", "plain", "--sky-horizon", "0", "0", "0",
               "--exposure", "1", "--directional-intensity", "2", "--directional-direction", "-.3", "-.2", "-1",
               "--environment-intensity", "0", "--bounce-intensity", ".2", "--fixed-dt", ".016666667",
               "--screenshot-viewport", "--capture-sequence", str(sequence), "--screenshot", str(screenshot)]
    if pin_ordering:
        command += ["--no-ddgi", "--no-ray-query", "--tone-map", "neutral"]
    row = dict(technique=technique, mode=mode, format=extension, command=command, log=str(log), captures=[])
    report["runs"].append(row)
    if generate_only:
        return {}
    with log.open("w", encoding="utf-8") as stream:
        process = subprocess.run(command, cwd=renderer.parent, env=dict(os.environ, VK_LAYER_SETTINGS_PATH=str(output)),
                                 stdout=stream, stderr=subprocess.STDOUT, timeout=150,
                                 creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
    row["exitCode"] = process.returncode
    text = log.read_text(encoding="utf-8", errors="replace")
    require(process.returncode == 0 and not validation_log_error(text), f"Vulkan capture failed: {log}")
    if os.environ.get("VK_LOADER_DEBUG") == "layer":
        require('Insert instance layer "VK_LAYER_KHRONOS_validation"' in text, f"Missing validation layer: {log}")
    images = {}
    projection = None
    for kind, frame in frames.items():
        image = screenshot if frame == max(frames.values()) else output / f"{name}.frame-{frame:04}.{extension}"
        active_technique = kind.split("/", 1)[0] if pin_ordering else technique
        pixels, state = read_capture(image, active_technique, extent=extent, mode=mode if pin_ordering else None)
        require(Path(state["scene"]["auxiliary"]).resolve() == bim
                and state["scene"]["auxiliaryObjectCount"] >= (5 if pin_ordering else 3),
                f"Incomplete fixture in {image}")
        if pin_ordering:
            current = state["camera"]["unjitteredViewProjColumns"]
            if projection is None:
                projection = current
            active_passes = {row["name"] for row in state["passes"] if row["active"]}
            require(current == projection and [column[3] for column in current] == [0, 0, 0, 1]
                    and {"OitClear", "OitResolve"} <= active_passes and not state["oit"]["overflowed"],
                    f"Expected fixed orthographic camera and transparent surfaces in {image}")
        images[kind] = pixels
        row["captures"].append(dict(kind=kind, frame=frame, technique=active_technique,
                                     image=str(image), revision=state["build"]["revision"],
                                    decodedSha256=hashlib.sha256(pixels.tobytes()).hexdigest()))
    return images


def inspect(png, exr, missing_previews):
    import numpy as np

    require(float(np.max(exr["off"])) > .1, "The raw reference is empty")
    for name, image in exr.items():
        np.testing.assert_array_equal(image, exr["off"], err_msg=f"{name} entered raw scene-linear export")
    np.testing.assert_array_equal(png["off"], png["off-again"], err_msg="Toggling previews left stale pixels")
    if "technical-off" in png:
        np.testing.assert_array_equal(png["off"], png["technical-off"],
                                      err_msg="Disabling Technical elevation left stale pixels")
    background = png["off"].astype(np.int16)
    results = {}
    for name, image in png.items():
        if name in ("off", "off-again", "technical-off"):
            continue
        diff = np.max(np.abs(image.astype(np.int16) - background), axis=2) > 4
        yy, xx = np.nonzero(diff)
        results[name] = dict(changedPixels=len(xx), bounds=None if not len(xx) else
                             [int(xx.min()), int(yy.min()), int(xx.max()), int(yy.max())])
        if name in missing_previews:
            np.testing.assert_array_equal(image, png["off"], err_msg=f"Expected baseline Forward raster to omit {name}")
            continue
        require(len(xx) >= 12, f"No visible {name} contribution: {results[name]}")
        if name in ("issue-pin", "clash-marker", "bim-hover", "mesh-hover"):
            region = slice(0, WIDTH // 2) if name in ("issue-pin", "bim-hover") else slice(WIDTH // 2, WIDTH)
            require(np.count_nonzero(diff[:, region]) >= len(xx) * .9, f"{name} appeared outside its expected region")
            channel = 0 if name == "clash-marker" else 2
            other = 2 if channel == 0 else 0
            color = image.astype(np.int16)
            require(np.count_nonzero(diff & ((color[:, :, channel] - color[:, :, other]) > 12)) >= 10,
                    f"{name} did not produce its expected highlight color")
    checks = dict(viewport=results, sceneLinearIdentical=len(exr))
    if "technical-depth" in png:
        depth_delta = np.abs(png["technical-depth"].astype(np.int16) - png["technical-no-depth"])
        changed = int(np.count_nonzero(np.max(depth_delta, axis=2) > 4))
        if "technical-depth" in missing_previews:
            require(changed == 0, "The control changed Technical elevation with the depth toggle")
        else:
            require(changed >= 12, f"The Technical elevation depth toggle changed only {changed} pixels")
            hidden_pixels = results["technical-depth"]["changedPixels"]
            visible_pixels = results["technical-no-depth"]["changedPixels"]
            require(visible_pixels - hidden_pixels >= 12,
                    f"Disabling depth should reveal the occluded box: {hidden_pixels} to {visible_pixels} pixels")
        checks["technicalDepthTogglePixels"] = changed
    return checks


def inspect_pin_depth(images, techniques, evidence, *, mode="lit"):
    import numpy as np

    viewport = overview_panel_slice(.026, .046, .334, .264) if mode == "overview" else np.s_[:, :]

    def blue_signal(rgb):
        return np.minimum(rgb[:, :, 2] - rgb[:, :, 0], rgb[:, :, 2] - rgb[:, :, 1])

    for technique in techniques:
        off = images[f"{technique}/off"][viewport].astype(np.int16)
        signal_off = blue_signal(off)
        zones = evidence[technique] = {}
        for index, (label, _, _, _) in enumerate(PIN_DEPTH_ZONES):
            image = images[f"{technique}/{label}"][viewport].astype(np.int16)
            width = image.shape[1]
            start, end = index * width // 4, (index + 1) * width // 4
            signal = np.maximum(blue_signal(image) - signal_off, 0)[:, start:end]
            changed = np.any(image != off, axis=2)
            center = off[off.shape[0] // 2 - 2:off.shape[0] // 2 + 3, (start + end) // 2]
            zones[label] = dict(blueSignal=int(signal.sum()), visiblePixels=int(np.count_nonzero(signal > 4)),
                                backgroundLevel=float(center.mean()), changedPixels=int(changed.sum()),
                                changedOutsideZone=int(changed[:, :start].sum() + changed[:, end:].sum()))
        print(f"{technique}/{mode}: depth-ordered BIM issue pins {zones}", flush=True)

    for technique, zones in evidence.items():
        np.testing.assert_array_equal(images[f"{technique}/off-again"], images[f"{technique}/off"],
                                      err_msg=f"{technique}/{mode}: disabled pin left a stale image")
        clear, behind, front, opaque = (zones[label] for label, _, _, _ in PIN_DEPTH_ZONES)
        min_pixels = 8 if mode == "overview" else 24
        require(clear["visiblePixels"] >= min_pixels, f"{technique}/{mode}: exposed issue pin missing: {zones}")
        require(min(behind["backgroundLevel"], front["backgroundLevel"], opaque["backgroundLevel"])
                > clear["backgroundLevel"] + 5, f"{technique}/{mode}: an occluder missed the pin: {zones}")
        require(behind["blueSignal"] >= .65 * clear["blueSignal"],
                f"{technique}/{mode}: farther translucent surfaces attenuated the issue pin: {zones}")
        require(.02 * clear["blueSignal"] < front["blueSignal"] < .70 * clear["blueSignal"],
                f"{technique}/{mode}: nearer translucent surfaces must partially attenuate the issue pin: {zones}")
        require(opaque["changedPixels"] == 0 and all(zone["changedOutsideZone"] == 0 for zone in zones.values()),
                f"{technique}/{mode}: pin leaked through opaque depth or changed an unrelated zone: {zones}")

    if mode == "overview":
        diagnostics = {}
        for label, pane, threshold in (
                ("transparency", overview_panel_slice(.026, .506, .334, .724), 32),
                ("revealage", overview_panel_slice(.346, .506, .654, .724), 223)):
            off_tile = images["forward-raster/off"][pane]
            brightness = off_tile.max(axis=2)
            roof_pixels = np.count_nonzero(brightness > threshold if label == "transparency" else brightness < threshold)
            require(roof_pixels > 100, f"Forward Overview {label} diagnostic lacks transparent roofs")
            for stage in PIN_DEPTH_STAGES[1:-1]:
                np.testing.assert_array_equal(images[f"forward-raster/{stage}"][pane], off_tile,
                                              err_msg=f"Issue pin changed Forward Overview {label} at {stage}")
            diagnostics[label] = {"roofPixels": int(roof_pixels), "changedPixels": 0}
        evidence["diagnostics"] = diagnostics


def inspect_linear_pin_depth(images):
    compared = 0
    for technique in TECHNIQUES:
        off = images[f"{technique}/off"]
        require(float(off.max()) > .01, f"The {technique} EXR control lost its scene")
        for stage in ("behind", "front"):
            require(images[f"{technique}/{stage}"].tobytes() == off.tobytes(),
                    f"The {stage} issue pin entered the {technique} scene-linear EXR")
            compared += 1
    return {"unchangedOnFrames": compared}


def run_pin_depth(renderer, output, report, generate_only, *, lit_only=False):
    bim = output / "issue-pin-depth-ordering.bim"
    pin_depth_fixture(bim)
    sequence = output / "issue-pin-ordering-lit.sequence.json"
    frames = pin_depth_sequence(sequence, TECHNIQUES, PIN_DEPTH_STAGES)
    lit = record(renderer, output, None, bim, sequence, frames, TECHNIQUES[0], "png", report, generate_only,
                 pin_ordering=True)
    evidence = report["checks"].setdefault("issuePinDepth", {})
    if not generate_only:
        inspect_pin_depth(lit, TECHNIQUES, evidence.setdefault("lit", {}))
    if lit_only:
        return

    sequence = output / "issue-pin-ordering-linear.sequence.json"
    frames = pin_depth_sequence(sequence, TECHNIQUES, ("off", "behind", "front"))
    linear = record(renderer, output, None, bim, sequence, frames, TECHNIQUES[0], "exr", report, generate_only,
                    pin_ordering=True)
    sequence = output / "issue-pin-ordering-overview.sequence.json"
    frames = pin_depth_sequence(sequence, ("forward-raster",), PIN_DEPTH_STAGES)
    overview = record(renderer, output, None, bim, sequence, frames, "forward-raster", "png", report, generate_only,
                      mode="overview", pin_ordering=True)
    if not generate_only:
        evidence["sceneLinear"] = inspect_linear_pin_depth(linear)
        inspect_pin_depth(overview, ("forward-raster",), evidence.setdefault("overview", {}), mode="overview")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--renderer", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--generate-only", action="store_true")
    parser.add_argument("--managed-output", action="store_true", help="Make a fresh run directory beneath the output.")
    parser.add_argument("--issue-pin-ordering-only", nargs="?", const="all", choices=("lit", "all"),
                        help="run only the depth-tested issue-pin sequence, optionally Lit only")
    baseline = parser.add_mutually_exclusive_group()
    baseline.add_argument("--expect-missing-forward-annotations", action="store_true",
                          help="check the older capture format on a build without Forward annotations")
    baseline.add_argument("--expect-missing-forward-technical", action="store_true",
                          help="expect only Technical elevation to be missing from Forward raster")
    args = parser.parse_args()
    expect_missing = args.expect_missing_forward_annotations or args.expect_missing_forward_technical
    if args.issue_pin_ordering_only and expect_missing:
        parser.error("The issue-pin ordering check requires working raster annotation pipelines")
    if not args.generate_only and os.environ.get("CONTAINER_RUN_GPU_BIM_FLOOR_PLAN") != "1":
        print("SKIP: set CONTAINER_RUN_GPU_BIM_FLOOR_PLAN=1 to run GPU captures")
        return 77
    os.environ.setdefault("OPENCV_IO_ENABLE_OPENEXR", "1")
    renderer, output = args.renderer.resolve(), args.output.resolve()
    if args.managed_output:
        output.mkdir(parents=True, exist_ok=True)
        output = Path(tempfile.mkdtemp(prefix="run-", dir=output))
    else:
        output.mkdir(parents=True)
    (output / "vk_layer_settings.txt").write_text("khronos_validation.validate_sync = true\n", encoding="utf-8")
    report = dict(passed=False, runs=[], checks={}, expectedMissingForwardAnnotations=args.expect_missing_forward_annotations,
                  expectedMissingForwardTechnical=args.expect_missing_forward_technical)
    if not args.generate_only:
        report["rendererSha256"] = hashlib.sha256(renderer.read_bytes()).hexdigest()
    try:
        if args.issue_pin_ordering_only:
            run_pin_depth(renderer, output, report, args.generate_only, lit_only=args.issue_pin_ordering_only == "lit")
            if not args.generate_only:
                require(hashlib.sha256(renderer.read_bytes()).hexdigest() == report["rendererSha256"], "Renderer changed")
                report["passed"] = True
            return 0
        model, bim = fixtures(output)
        frames = FRAMES if args.expect_missing_forward_annotations else FRAMES | TECHNICAL_FRAMES
        sequence = output / "annotations.sequence.json"
        sequence.write_text(json.dumps(sequence_plan(frames), indent=2) + "\n", encoding="utf-8")
        for technique in TECHNIQUES:
            png, exr = (record(renderer, output, model, bim, sequence, frames, technique, extension, report, args.generate_only)
                        for extension in ("png", "exr"))
            if not args.generate_only:
                missing_previews = ()
                if technique == "forward-raster":
                    if args.expect_missing_forward_annotations:
                        missing_previews = frames
                    elif args.expect_missing_forward_technical:
                        missing_previews = TECHNICAL_PREVIEWS
                checks = inspect(png, exr, missing_previews)
                report["checks"][technique] = checks
                print(technique, checks, flush=True)
        if not expect_missing:
            run_pin_depth(renderer, output, report, args.generate_only)
        if not args.generate_only:
            require(hashlib.sha256(renderer.read_bytes()).hexdigest() == report["rendererSha256"], "Renderer changed")
            report["passed"] = True
    finally:
        (output / "results.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
