"""Check Technical BIM contours against front, rear and opaque geometry."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import tempfile

from bim_floor_plan_runtime_regression import panel, read_capture as read_scene_capture
from display_capture_test_support import validation_log_error


WIDTH, HEIGHT = 512, 384
EXR_MAX_LINEAR_DELTA = .002
TECHNIQUES = ("deferred-raster", "forward-raster")
MODES = ("lit", "overview", "exr")
FRAMES = {"off": 4, "no-depth": 8, "depth": 12, "off-again": 16}
ZONES = (("clear", -1.2, ()), ("rear", -.4, ((-1.5, 250), (-1.2, 250))),
         ("front", .4, ((1.2, 250), (1.5, 250))), ("opaque", 1.2, ((1.5, 255),)))
# Pixel centers inside the Overview Lit tile, matching post_process.slang.
OVERVIEW_LIT = (13, 18, 171, 101)


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


def write_json(path, value):
    path.write_text(json.dumps(value, indent=2) + "\n", encoding="utf-8")


def fixture(path):
    meshes, elements = [], []
    for zone, x, covers in ZONES:
        mesh_id = len(meshes) + 1
        # Keep the target translucent so an opaque target prepass cannot hide
        # rear glass and accidentally make unconditionally early lines pass.
        meshes.append(panel(mesh_id, (x - .23, 1.16, x + .23, 2.19), 0))
        elements.append(dict(mesh_id=mesh_id, guid=f"{zone}-wall", type="IfcWall",
                             color=dict(r=150, g=150, b=150, a=64)))
        for layer, (z, alpha) in enumerate(((-1.9, 255), *covers)):
            mesh_id = len(meshes) + 1
            # Use a tall triangle whose sloping edge stays outside the contour
            # measurement region. Match clear and covered background colors.
            meshes.append(dict(mesh_id=mesh_id,
                               coordinates=[x - .39, -z, -8, x + .39, -z, -8,
                                            x + .39, -z, 160], indices=[0, 1, 2]))
            elements.append(dict(mesh_id=mesh_id, guid=f"{zone}-panel-{layer}", type="IfcRoof",
                                 color=dict(r=180, g=180, b=180, a=alpha)))
    write_json(path, dict(schema_version="1.1.0", meshes=meshes, elements=elements))


def sequence_plan():
    return dict(schemaVersion=1, frames=max(FRAMES.values()), sampleFrames=list(FRAMES.values()), events=[
        {"frame": 1, "orthographic": True, "bimAnnotations": {}},
        {"frame": 5, "bimAnnotations": {"technicalLines": False}},
        {"frame": 9, "bimAnnotations": {"technicalLines": True}},
        {"frame": 13, "bimAnnotations": {}},
    ])


def read_capture(image, frame, technique, mode, model):
    sample = read_scene_capture(image, frame, technique, extent=(WIDTH, HEIGHT))
    metadata, state = sample["metadata"], sample["state"]
    require(mode == "exr" or metadata["displayMode"] == (8 if mode == "overview" else 0),
            f"Wrong display mode: {image}")
    require(state["msaaSamples"] == 1 and not state["taa"]["enabled"], f"Wrong renderer settings: {image}")
    require(Path(state["scene"]["auxiliary"]).resolve() == model and
            state["scene"]["auxiliaryObjectCount"] >= sum(2 + len(covers) for _, _, covers in ZONES),
            f"Incomplete BIM fixture: {image}")
    require(not state["oit"]["overflowed"], f"OIT overflow invalidates the occlusion control: {image}")
    if technique == "forward-raster" or mode != "overview":
        active = {entry["name"] for entry in state["passes"] if entry["active"]}
        require({"OitClear", "OitResolve"} <= active, f"Missing transparent passes: {image}")
    return sample


def record(renderer, output, model, sequence, technique, mode, report, generate_only):
    folder = output / f"{technique}-{mode}"
    folder.mkdir()
    extension = "exr" if mode == "exr" else "png"
    screenshot, log = folder / f"lines.{extension}", folder / "renderer.log"
    command = [str(renderer), "--model", str(model), "--import-scale", "1", "--hidden", "--no-ui",
               "--validation", "--width", str(WIDTH), "--height", str(HEIGHT), "--msaa", "1", "--no-taa",
               "--no-bloom", "--no-ddgi", "--no-ray-query", "--render-technique", technique,
               "--display-mode", "lit" if mode == "exr" else mode,
               "--camera-position", "0", "1.675", "8", "--camera-target", "0", "1.675", "0",
               "--camera-fov", "45", "--tone-map", "neutral", "--exposure", "1", "--sky", "plain",
               "--sky-horizon", ".15", ".15", ".15", "--directional-intensity", "4",
               "--directional-direction", "-.2", "-.25", "-1", "--environment-intensity", ".5",
               "--bounce-intensity", ".25", "--fixed-dt", ".016666667", "--capture-sequence", str(sequence),
               "--screenshot", str(screenshot), "--screenshot-viewport"]
    row = dict(technique=technique, displayMode=mode, command=command, log=str(log), captures=[])
    report["runs"].append(row)
    write_json(folder / "command.json", command)
    if generate_only:
        return {}
    with log.open("w", encoding="utf-8") as stream:
        process = subprocess.run(command, cwd=renderer.parent,
                                 env=dict(os.environ, VK_LAYER_SETTINGS_PATH=str(output), VK_LOADER_DEBUG="layer"),
                                 stdout=stream, stderr=subprocess.STDOUT, timeout=180,
                                 creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
    row["exitCode"] = process.returncode
    text = log.read_text(encoding="utf-8", errors="replace")
    require(process.returncode == 0 and not validation_log_error(text), f"Vulkan capture failed: {log}")
    require('Insert instance layer "VK_LAYER_KHRONOS_validation"' in text, f"Missing validation layer: {log}")
    images, camera = {}, None
    for label, frame in FRAMES.items():
        image = screenshot if frame == max(FRAMES.values()) else folder / f"lines.frame-{frame:04}.{extension}"
        sample = read_capture(image, frame, technique, mode, model)
        current_camera = sample["state"]["camera"]["unjitteredViewProjColumns"]
        if camera is None:
            camera = current_camera
        require(current_camera == camera and [column[3] for column in camera] == [0, 0, 0, 1]
                and abs(camera[0][0] - .625) < 1e-5 and abs(camera[1][1] - 5 / 6) < 1e-5,
                f"Unexpected Technical measurement camera: {image}")
        images[label] = sample["pixels"]
        row["captures"].append(dict(kind=label, frame=frame, image=str(image),
                                    revision=sample["state"]["build"]["revision"],
                                    decodedSha256=sample["decodedSha256"]))
    return images


def inspect(images, technique, mode):
    import numpy as np

    off = images["off"]
    errors = []
    if mode == "exr":
        require(float(off.max()) > .01, f"{technique}: empty scene-linear control")
        raw = {}
        for label in ("no-depth", "depth", "off-again"):
            delta = np.max(np.abs(images[label] - off), axis=2)
            raw[label] = dict(changedPixels=int(np.count_nonzero(delta)),
                              pixelsAboveLimit=int(np.count_nonzero(delta > EXR_MAX_LINEAR_DELTA)),
                              maxLinearDelta=float(delta.max()))
            if raw[label]["pixelsAboveLimit"]:
                errors.append(f"{label} Technical contour changed the scene-linear image")
        return {"sceneLinearFrames": raw, "maxAllowedLinearDelta": EXR_MAX_LINEAR_DELTA}, errors

    if not np.array_equal(off, images["off-again"]):
        errors.append("disabling Technical left stale pixels")

    overview = mode == "overview"
    if overview:
        left, top, right, bottom = OVERVIEW_LIT
        viewport = np.s_[top:bottom, left:right]
    else:
        viewport = np.s_[:, :]
    background = off[viewport].astype(np.int16)
    height, width = background.shape[:2]
    rows = slice(round(.19 * height), round(.81 * height))
    margin = round(.0375 * width)
    counts = {}
    for label in ("no-depth", "depth"):
        delta = np.max(np.abs(images[label][viewport].astype(np.int16) - background), axis=2)
        counts[label] = {}
        for index, (name, _, _) in enumerate(ZONES):
            columns = slice(index * width // 4 + margin, (index + 1) * width // 4 - margin)
            patch = delta[rows, columns]
            counts[label][name] = dict(visiblePixels=int(np.count_nonzero(patch > 8)),
                                       changedPixels=int(np.count_nonzero(patch)))

    minimum = 8 if overview else 32
    no_depth, depth = counts["no-depth"], counts["depth"]
    for name, pixels in no_depth.items():
        if pixels["visiblePixels"] < max(minimum, .6 * no_depth["clear"]["visiblePixels"]):
            errors.append(f"no-depth contour disappeared in the {name} control")
    if depth["clear"]["visiblePixels"] < minimum:
        errors.append("clear depth-tested contour disappeared")
    if depth["rear"]["visiblePixels"] < max(minimum, .65 * depth["clear"]["visiblePixels"]):
        errors.append("rear transparent geometry attenuated the depth-tested contour")
    if depth["opaque"]["changedPixels"]:
        errors.append("depth-tested contour leaked through opaque geometry")

    # Deferred Overview has no transparent node production. Its depth-tested
    # contour still draws in the Lit tile and respects the opaque control.
    sorted_depth = technique == "forward-raster" or not overview
    if sorted_depth and depth["front"]["visiblePixels"]:
        errors.append("depth-tested contour escaped the nearly opaque front glass")
    if not sorted_depth and depth["front"]["visiblePixels"] < minimum:
        errors.append("Deferred Overview lost its depth-tested contour")

    diagnostics = {}
    if overview:
        outside = np.ones((HEIGHT, WIDTH), dtype=bool)
        outside[viewport] = False
        if not np.array_equal(images["no-depth"][outside], off[outside]):
            errors.append("no-depth Technical changed pixels outside the Overview Lit tile")
        for name, left, right in (("transparency", .03, .33), ("revealage", .35, .65)):
            pane = np.s_[round(.51 * HEIGHT):round(.72 * HEIGHT), round(left * WIDTH):round(right * WIDTH)]
            changed = int(np.count_nonzero(np.any(images["depth"][pane] != off[pane], axis=2)))
            diagnostics[name] = changed
            if changed:
                errors.append(f"depth-tested Technical changed the Overview {name} tile")
    return {"interiorPixels": counts, "sortedDepth": sorted_depth, "diagnosticPixelsChanged": diagnostics}, errors


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--renderer", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--generate-only", action="store_true")
    parser.add_argument("--managed-output", action="store_true", help="Make a fresh run directory beneath the output.")
    args = parser.parse_args()
    if not args.generate_only and os.environ.get("CONTAINER_RUN_GPU_BIM_FLOOR_PLAN") != "1":
        print("SKIP: set CONTAINER_RUN_GPU_BIM_FLOOR_PLAN=1 to run GPU captures")
        return 77
    os.environ.setdefault("OPENCV_IO_ENABLE_OPENEXR", "1")
    renderer, output = args.renderer.resolve(), args.output.resolve()
    if args.managed_output:
        output.mkdir(parents=True, exist_ok=True)
        output = Path(tempfile.mkdtemp(prefix="run-", dir=output)).resolve()
    else:
        output.mkdir(parents=True)
    model, sequence = output / "technical-lines-and-transparent-roof.bim", output / "events.json"
    fixture(model)
    write_json(sequence, sequence_plan())
    (output / "vk_layer_settings.txt").write_text("khronos_validation.validate_sync = true\n", encoding="utf-8")
    report = dict(passed=False, generatedOnly=args.generate_only, runs=[], checks={}, failures=[])
    if not args.generate_only:
        report["rendererSha256"] = hashlib.sha256(renderer.read_bytes()).hexdigest()
    report["fixtureSha256"] = hashlib.sha256(model.read_bytes()).hexdigest()
    report["sequenceSha256"] = hashlib.sha256(sequence.read_bytes()).hexdigest()
    try:
        for technique in TECHNIQUES:
            for mode in MODES:
                images = record(renderer, output, model, sequence, technique, mode, report, args.generate_only)
                if not args.generate_only:
                    checks, failures = inspect(images, technique, mode)
                    report["checks"][f"{technique}-{mode}"] = checks
                    report["failures"].extend(f"{technique}/{mode}: {failure}" for failure in failures)
                    print(technique, mode, checks, flush=True)
        if not args.generate_only:
            require(hashlib.sha256(renderer.read_bytes()).hexdigest() == report["rendererSha256"], "Renderer changed")
            require(not report["failures"], "\n".join(report["failures"]))
            report["passed"] = True
    finally:
        write_json(output / "results.json", report)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
