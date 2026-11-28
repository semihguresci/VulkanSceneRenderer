"""Keep no-depth BIM Technical lines visible through real transparent surfaces."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import tempfile

from bim_floor_plan_runtime_regression import panel
from display_capture_test_support import validation_log_error


WIDTH, HEIGHT = 512, 384
TECHNIQUES = ("deferred-raster", "forward-raster")
MODES = ("lit", "overview")
FRAMES = {"off": 4, "no-depth": 8, "depth": 12, "off-again": 16}
# The roof's own triangle edges lie outside these mirrored wall interiors.
# Including the roof border gives false evidence of a covered wall contour.
REGIONS = {
    "lit": ((29, 106, 166, 278), (346, 106, 483, 278)),
    "overview": ((23, 43, 64, 76), (120, 43, 162, 76)),
}
# Pixel centers inside the Overview Lit tile, matching post_process.slang.
OVERVIEW_LIT = (13, 18, 171, 101)


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


def write_json(path, value):
    path.write_text(json.dumps(value, indent=2) + "\n", encoding="utf-8")


def fixture(path):
    meshes = [panel(1, (-3, 1.1, -.55, 2.25), 0),
              panel(2, (.55, 1.1, 3, 2.25), 0)]
    for mesh_id, depth in ((3, 1.2), (4, 1.5)):
        # DotBIM +Z is renderer +Y and source -Y is renderer +Z.
        # Oversized triangles cover the left wall without putting their own
        # Technical contour in the interior measurement regions.
        meshes.append(dict(mesh_id=mesh_id,
                           coordinates=[-12, -depth, -9, -.25, -depth, -9, -.25, -depth, 11],
                           indices=[0, 1, 2]))
    elements = [dict(mesh_id=index, guid=name, type=kind,
                     color=dict(r=color, g=color, b=color, a=alpha))
                for index, name, kind, color, alpha in (
                    (1, "left-wall", "IfcWall", 145, 255),
                    (2, "right-wall", "IfcWall", 145, 255),
                    (3, "left-roof-front-1", "IfcRoof", 180, 250),
                    (4, "left-roof-front-2", "IfcRoof", 180, 250))]
    write_json(path, dict(schema_version="1.1.0", meshes=meshes, elements=elements))


def sequence_plan():
    return dict(schemaVersion=1, frames=max(FRAMES.values()), sampleFrames=list(FRAMES.values()), events=[
        {"frame": 1, "orthographic": True, "bimAnnotations": {}},
        {"frame": 5, "bimAnnotations": {"technicalLines": False}},
        {"frame": 9, "bimAnnotations": {"technicalLines": True}},
        {"frame": 13, "bimAnnotations": {}},
    ])


def read_capture(image, technique, mode, model):
    import numpy as np
    from PIL import Image

    metadata = json.loads(Path(f"{image}.capture.json").read_bytes())
    state = json.loads(image.with_suffix(".telemetry.json").read_bytes())
    require((metadata["width"], metadata["height"], metadata["includesUi"], metadata["source"],
             metadata["displayMode"]) == (WIDTH, HEIGHT, False, "viewport", 8 if mode == "overview" else 0),
            f"Wrong capture route: {image}")
    require(state["technique"] == technique and state["validationEnabled"] and state["msaaSamples"] == 1
            and not state["taa"]["enabled"], f"Wrong renderer settings: {image}")
    require(Path(state["scene"]["auxiliary"]).resolve() == model and
            state["scene"]["auxiliaryObjectCount"] >= 4, f"Incomplete BIM fixture: {image}")
    require(not state["oit"]["overflowed"], f"OIT overflow invalidates the occlusion control: {image}")
    with Image.open(image) as decoded:
        require(decoded.format == "PNG" and decoded.size == (WIDTH, HEIGHT), f"Invalid image: {image}")
        pixels = np.asarray(decoded.convert("RGB"))
    return pixels, state


def record(renderer, output, model, sequence, technique, mode, report, generate_only):
    folder = output / f"{technique}-{mode}"
    folder.mkdir()
    screenshot, log = folder / "lines.png", folder / "renderer.log"
    command = [str(renderer), "--model", str(model), "--import-scale", "1", "--hidden", "--no-ui",
               "--validation", "--width", str(WIDTH), "--height", str(HEIGHT), "--msaa", "1", "--no-taa",
               "--no-bloom", "--no-ddgi", "--no-ray-query", "--render-technique", technique,
               "--display-mode", mode, "--camera-position", "0", "1.675", "8", "--camera-target", "0", "1.675", "0",
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
        image = screenshot if frame == max(FRAMES.values()) else folder / f"lines.frame-{frame:04}.png"
        pixels, state = read_capture(image, technique, mode, model)
        current_camera = state["camera"]["unjitteredViewProjColumns"]
        if camera is None:
            camera = current_camera
        require(current_camera == camera, f"Technical toggle moved the camera: {image}")
        images[label] = pixels
        row["captures"].append(dict(kind=label, frame=frame, image=str(image), revision=state["build"]["revision"],
                                    decodedSha256=hashlib.sha256(pixels.tobytes()).hexdigest()))
    return images


def inspect(images, technique, mode):
    import numpy as np

    np.testing.assert_array_equal(images["off"], images["off-again"],
                                  err_msg=f"{technique}/{mode}: disabling Technical left stale pixels")
    background = images["off"].astype(np.int16)
    masks = {label: np.max(np.abs(image.astype(np.int16) - background), axis=2) > 8
             for label, image in images.items()}
    regions = REGIONS[mode]

    def count(mask, bounds):
        left, top, right, bottom = bounds
        return int(np.count_nonzero(mask[top:bottom, left:right]))

    counts = {label: dict(covered=count(masks[label], regions[0]), clear=count(masks[label], regions[1]))
              for label in ("no-depth", "depth")}
    minimum = 8 if mode == "overview" else 32
    for label in counts:
        require(counts[label]["clear"] >= minimum,
                f"{technique}/{mode}: no exposed wall contour for {label}: {counts}")
    require(counts["no-depth"]["covered"] >= max(minimum, counts["no-depth"]["clear"] * .6),
            f"{technique}/{mode}: no-depth Technical contour disappeared under the roof: {counts}")

    # The current Deferred Overview deliberately omits transparent node production.
    # In all other cases the genuine roof remains an OIT occluder of depth-tested lines.
    oit_occludes = technique == "forward-raster" or mode == "lit"
    if oit_occludes:
        require(counts["depth"]["covered"] == 0,
                f"{technique}/{mode}: depth-tested Technical contour escaped the roof: {counts}")
    else:
        require(counts["depth"]["covered"] >= max(minimum, counts["depth"]["clear"] * .6),
                f"{technique}/{mode}: Deferred Overview lost its depth-tested wall contour: {counts}")
    if mode == "overview":
        left, top, right, bottom = OVERVIEW_LIT
        outside = np.ones((HEIGHT, WIDTH), dtype=bool)
        outside[top:bottom, left:right] = False
        np.testing.assert_array_equal(images["no-depth"][outside], images["off"][outside],
                                      err_msg=f"{technique}: no-depth Technical changed an Overview diagnostic tile")
    return dict(interiorPixels=counts, oitOccludesDepthTested=oit_occludes,
                offRestored=True, overviewDiagnosticsUnchanged=mode == "overview")


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
    report = dict(passed=False, generatedOnly=args.generate_only, runs=[], checks={}, interiorRegions=REGIONS)
    if not args.generate_only:
        report["rendererSha256"] = hashlib.sha256(renderer.read_bytes()).hexdigest()
    report["fixtureSha256"] = hashlib.sha256(model.read_bytes()).hexdigest()
    report["sequenceSha256"] = hashlib.sha256(sequence.read_bytes()).hexdigest()
    try:
        for technique in TECHNIQUES:
            for mode in MODES:
                images = record(renderer, output, model, sequence, technique, mode, report, args.generate_only)
                if not args.generate_only:
                    checks = inspect(images, technique, mode)
                    report["checks"][f"{technique}-{mode}"] = checks
                    print(technique, mode, checks, flush=True)
        if not args.generate_only:
            require(hashlib.sha256(renderer.read_bytes()).hexdigest() == report["rendererSha256"], "Renderer changed")
            report["passed"] = True
    finally:
        write_json(output / "results.json", report)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
