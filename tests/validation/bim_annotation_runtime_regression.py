"""Compare individual BIM annotation routes in forward and deferred raster captures."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import tempfile

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


def read_capture(path, technique):
    import cv2
    import numpy as np
    from PIL import Image

    metadata = json.loads(Path(f"{path}.capture.json").read_bytes())
    state = json.loads(path.with_suffix(".telemetry.json").read_bytes())
    require((metadata["width"], metadata["height"], metadata["includesUi"]) == (WIDTH, HEIGHT, False) and
            state["technique"] == technique and state["validationEnabled"], f"Wrong capture state for {path}")
    if path.suffix == ".exr":
        require(metadata["source"] == "raster-scene-composite" and metadata["colorSpace"] == "linear-sRGB" and
                not metadata["exposureApplied"] and not metadata["toneMappingApplied"], f"Wrong EXR metadata for {path}")
        bgr = cv2.imdecode(np.frombuffer(path.read_bytes(), dtype=np.uint8), cv2.IMREAD_UNCHANGED)
        require(bgr is not None and bgr.shape == (HEIGHT, WIDTH, 3) and bgr.dtype == np.float32 and
                np.isfinite(bgr).all(), f"Invalid scene-linear radiance {path}")
        return bgr[:, :, ::-1], state
    require(metadata["source"] == "viewport", f"Wrong viewport capture route for {path}")
    with Image.open(path) as image:
        require(image.format == "PNG" and image.size == (WIDTH, HEIGHT), f"Wrong viewport image {path}")
        return np.asarray(image.convert("RGB")), state


def record(renderer, output, model, bim, sequence, frames, technique, extension, report, generate_only):
    name = f"{technique}-{extension}"
    screenshot = output / f"{name}.{extension}"
    log = output / f"{name}.log"
    command = [str(renderer), "--model", str(model), "--bim-model", str(bim), "--import-scale", "1",
               "--hidden", "--no-ui", "--validation", "--width", str(WIDTH), "--height", str(HEIGHT),
               "--msaa", "1", "--no-taa", "--no-bloom", "--render-technique", technique,
               "--display-mode", "lit", "--camera-position", "0", "1", "8", "--camera-target", "0", "1", "0",
               "--camera-fov", "45", "--sky", "plain", "--sky-horizon", "0", "0", "0",
               "--exposure", "1", "--directional-intensity", "2", "--directional-direction", "-.3", "-.2", "-1",
               "--environment-intensity", "0", "--bounce-intensity", ".2", "--fixed-dt", ".016666667",
               "--screenshot-viewport", "--capture-sequence", str(sequence), "--screenshot", str(screenshot)]
    row = dict(technique=technique, format=extension, command=command, log=str(log), captures=[])
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
    for kind, frame in frames.items():
        image = screenshot if frame == max(frames.values()) else output / f"{name}.frame-{frame:04}.{extension}"
        pixels, state = read_capture(image, technique)
        require(Path(state["scene"]["auxiliary"]).resolve() == bim and state["scene"]["auxiliaryObjectCount"] >= 3,
                f"Incomplete fixture in {image}")
        images[kind] = pixels
        row["captures"].append(dict(kind=kind, frame=frame, image=str(image), revision=state["build"]["revision"],
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


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--renderer", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--generate-only", action="store_true")
    parser.add_argument("--managed-output", action="store_true", help="Make a fresh run directory beneath the output.")
    baseline = parser.add_mutually_exclusive_group()
    baseline.add_argument("--expect-missing-forward-annotations", action="store_true",
                          help="check the older capture format on a build without Forward annotations")
    baseline.add_argument("--expect-missing-forward-technical", action="store_true",
                          help="expect only Technical elevation to be missing from Forward raster")
    args = parser.parse_args()
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
    model, bim = fixtures(output)
    frames = FRAMES if args.expect_missing_forward_annotations else FRAMES | TECHNICAL_FRAMES
    sequence = output / "annotations.sequence.json"
    sequence.write_text(json.dumps(sequence_plan(frames), indent=2) + "\n", encoding="utf-8")
    (output / "vk_layer_settings.txt").write_text("khronos_validation.validate_sync = true\n", encoding="utf-8")
    report = dict(passed=False, runs=[], checks={}, expectedMissingForwardAnnotations=args.expect_missing_forward_annotations,
                  expectedMissingForwardTechnical=args.expect_missing_forward_technical)
    if not args.generate_only:
        report["rendererSha256"] = hashlib.sha256(renderer.read_bytes()).hexdigest()
    try:
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
        if not args.generate_only:
            require(hashlib.sha256(renderer.read_bytes()).hexdigest() == report["rendererSha256"], "Renderer changed")
            report["passed"] = True
    finally:
        (output / "results.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
