"""Verify BIM selection above transparency without changing viewport picking."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import tempfile

from bim_floor_plan_runtime_regression import overview_transparency_fixture, require, settings
from display_capture_test_support import validation_log_error


WIDTH, HEIGHT = 512, 384
TECHNIQUES = ("deferred-raster", "forward-raster")
DISPLAY_MODES = {"lit": 0, "overview": 8}
FRAMES = {"baseline": 4, "covered": 8, "clear": 12, "deselected": 16}
ROOF_PIXEL = [120, HEIGHT // 2]
OPT_IN = "CONTAINER_RUN_GPU_BIM_SELECTION"


def write_json(path, value):
    path.write_text(json.dumps(value, indent=2) + "\n", encoding="utf-8")


def write_sequence(path, mode):
    frames = dict(FRAMES)
    events = [
        {"frame": 1, "orthographic": True, "bimFloorPlan": settings(enabled=False, elevation="source-elevation")},
        {"frame": 5, "bimSelectedObject": 0},
        {"frame": 9, "bimSelectedObject": 1},
        {"frame": 13, "viewportPointer": {"action": "select", "pixel": [5, 5]}},
    ]
    if mode == "lit":
        # Probe and select through the real viewport path. The left wall remains
        # visually outlined when selected by ID, but the nearer roof owns the hit.
        frames["roof-pick"] = 20
        events.extend([
            {"frame": 18, "viewportPointer": {"action": "select", "pixel": ROOF_PIXEL}},
            {"frame": 20, "depthProbe": ROOF_PIXEL},
        ])
    write_json(path, {"schemaVersion": 1, "frames": max(frames.values()),
                      "sampleFrames": list(frames.values()), "events": events})
    return frames


def checkpoint(path, technique, mode, model):
    import numpy as np
    from PIL import Image

    state = json.loads(path.with_suffix(".telemetry.json").read_bytes())
    sidecar = json.loads(Path(f"{path}.capture.json").read_bytes())
    require(state["technique"] == technique and state["validationEnabled"] and not state["taa"]["enabled"]
            and Path(state["scene"]["auxiliary"]).resolve() == model, f"Wrong renderer state: {path}")
    require(sidecar["source"] == "viewport" and sidecar["displayMode"] == DISPLAY_MODES[mode]
            and not sidecar["includesUi"] and (sidecar["width"], sidecar["height"]) == (WIDTH, HEIGHT)
            and sidecar["build"] == state["build"] and not sidecar["bim"]["floorPlan"]["requested"],
            f"Wrong capture receipt: {path}")
    with Image.open(path) as image:
        require(image.format == "PNG" and image.size == (WIDTH, HEIGHT), f"Invalid PNG: {path}")
        pixels = np.asarray(image.convert("RGB"), dtype=np.int16)
    return pixels, state


def outline_count(baseline, selected):
    import numpy as np

    delta = np.abs(selected - baseline).max(axis=-1)
    red, green, blue = np.moveaxis(selected, -1, 0)
    orange = (red > green + 30) & (green > blue + 18)
    return int((orange & (delta > 8)).sum())


def check_outlines(images, technique, mode):
    import numpy as np

    baseline = images["baseline"]
    if mode == "overview":
        # Lit panel interior; leave the Overview grid border outside this ROI.
        left, right = np.s_[18:101, 13:92], np.s_[18:101, 92:171]
    else:
        left, right = np.s_[:, :WIDTH // 2], np.s_[:, WIDTH // 2:]

    covered = outline_count(baseline[left], images["covered"][left])
    clear = outline_count(baseline[right], images["clear"][right])
    minimum = 70 if mode == "overview" else 500
    require(clear >= minimum and covered >= minimum and .7 * clear <= covered <= 1.35 * clear,
            f"{technique} {mode}: selected BIM outline lost coverage (covered={covered}, clear={clear})")
    for label, untouched in (("covered", right), ("clear", left)):
        np.testing.assert_array_equal(images[label][untouched], baseline[untouched],
                                      err_msg=f"{technique} {mode}: {label} outlined the other wall")
    np.testing.assert_array_equal(images["deselected"], baseline,
                                  err_msg=f"{technique} {mode}: outline survived deselection")
    if mode == "overview":
        outside = np.ones((HEIGHT, WIDTH), dtype=bool)
        outside[15:105, 10:175] = False
        for label in ("covered", "clear"):
            np.testing.assert_array_equal(images[label][outside], baseline[outside],
                                          err_msg=f"{technique}: {label} selection escaped the Overview Lit tile")

    diagnostics = {}
    if mode == "overview" and technique == "forward-raster":
        for name, tile in (("transparency", np.s_[194:278, 13:171]), ("revealage", np.s_[194:278, 177:335])):
            values = baseline[tile].max(axis=-1)
            roof = values > 32 if name == "transparency" else values < 223
            occupied, clear_half = (int(half.sum()) for half in np.array_split(roof, 2, axis=1))
            require(occupied > 100 and occupied > 10 * clear_half, f"Missing Forward roof in the {name} tile")
            diagnostics[name] = {"coveredPixels": occupied, "clearPixels": clear_half}
    return {"coveredOutlinePixels": covered, "clearOutlinePixels": clear, "diagnostics": diagnostics}


def run_lane(renderer, output, model, technique, mode, *, generate_only):
    lane = output / f"{technique}-{mode}"
    lane.mkdir()
    sequence = lane / "events.json"
    frames = write_sequence(sequence, mode)
    image = lane / "selection.png"
    command = [str(renderer), "--model", str(model), "--import-scale", "1", "--hidden", "--validation",
               "--width", str(WIDTH), "--height", str(HEIGHT), "--msaa", "1", "--no-taa", "--no-bloom",
               "--no-ddgi", "--no-ray-query", "--render-technique", technique, "--display-mode", mode,
               "--camera-position", "0", "1.675", "8", "--camera-target", "0", "1.675", "0", "--camera-fov", "45",
               "--tone-map", "neutral", "--exposure", "1", "--sky", "plain", "--sky-horizon", ".15", ".15", ".15",
               "--directional-intensity", "4", "--directional-direction", "-.2", "-.25", "-1",
               "--environment-intensity", ".5", "--bounce-intensity", ".25", "--fixed-dt", ".016666667",
               "--capture-sequence", str(sequence), "--screenshot", str(image), "--screenshot-viewport"]
    write_json(lane / "command.json", command)
    report = {"command": command, "sequence": str(sequence), "frames": frames}
    if generate_only:
        return report
    log = lane / "renderer.log"
    with log.open("wb") as stream:
        process = subprocess.run(command, cwd=renderer.parent, stdout=stream, stderr=subprocess.STDOUT,
                                 timeout=150, creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0),
                                 env=dict(os.environ, VK_LAYER_SETTINGS_PATH=str(output)))
    messages = log.read_text(encoding="utf-8", errors="replace")
    require(process.returncode == 0 and not validation_log_error(messages), f"Vulkan capture failed: {log}")
    if os.environ.get("VK_LOADER_DEBUG") == "layer":
        require('Insert instance layer "VK_LAYER_KHRONOS_validation"' in messages, f"Missing validation layer: {log}")

    images, states, paths = {}, {}, {}
    for label, frame in frames.items():
        path = image if frame == max(frames.values()) else lane / f"selection.frame-{frame:04d}.png"
        images[label], states[label] = checkpoint(path, technique, mode, model)
        paths[label] = str(path)
    projection = states["baseline"]["camera"]["unjitteredViewProjColumns"]
    require([column[3] for column in projection] == [0, 0, 0, 1] and
            all(state["camera"]["unjitteredViewProjColumns"] == projection for state in states.values()),
            f"Camera projection changed: {lane}")
    selections = {name: state["gui"]["selectedBimObjectIndex"] for name, state in states.items()}
    require([selections[label] for label in FRAMES] == [-1, 0, 1, -1], f"Unexpected BIM selection state: {selections}")
    measurements = check_outlines(images, technique, mode)
    if "roof-pick" in states:
        probe = states["roof-pick"]["depthProbe"]
        hit = probe["sceneInteraction"]["bim"]
        require(probe["pixel"] == ROOF_PIXEL and hit and hit["guid"] == "left-roof-front"
                and selections["roof-pick"] == hit["objectIndex"] and probe["pick"] is not None
                and probe["opaque"] is not None and probe["pick"] > probe["opaque"] > 0,
                f"Roof lost the foreground viewport pick: {probe}")
        measurements["roofPick"] = {"pixel": ROOF_PIXEL, "guid": hit["guid"], "objectIndex": hit["objectIndex"],
                                    "pickDepth": probe["pick"], "opaqueDepth": probe["opaque"]}
    report.update(measurements, captures=paths, selections=selections, cameraProjection=projection,
                  revision=states["baseline"]["build"]["revision"], validationClean=True, log=str(log))
    print(f"{technique} {mode}: {measurements}", flush=True)
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--renderer", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--managed-output", action="store_true")
    parser.add_argument("--generate-only", action="store_true")
    args = parser.parse_args()
    if not args.generate_only and os.environ.get(OPT_IN) != "1":
        print(f"SKIP: set {OPT_IN}=1 to run Vulkan BIM selection captures")
        return 77
    renderer, output = args.renderer.resolve(strict=True), args.output.resolve()
    if args.managed_output:
        output.mkdir(parents=True, exist_ok=True)
        output = Path(tempfile.mkdtemp(prefix="run-", dir=output))
    else:
        output.mkdir(parents=True, exist_ok=False)
    (output / "vk_layer_settings.txt").write_text("khronos_validation.validate_sync = true\n", encoding="utf-8")
    model = output / "selection-behind-transparent-roofs.bim"
    overview_transparency_fixture(model)
    identity = hashlib.sha256(renderer.read_bytes()).hexdigest()
    report = {"passed": False, "rendererSha256": identity, "fixture": str(model), "runs": {}}
    try:
        for technique in TECHNIQUES:
            for mode in DISPLAY_MODES:
                report["runs"][f"{technique}-{mode}"] = run_lane(renderer, output, model, technique, mode,
                                                               generate_only=args.generate_only)
        require(hashlib.sha256(renderer.read_bytes()).hexdigest() == identity, "Renderer changed during the test")
        if not args.generate_only:
            projections = {json.dumps(row["cameraProjection"]) for row in report["runs"].values()}
            require(len(projections) == 1, "The four lanes did not use the same camera projection")
            report["passed"] = True
    finally:
        write_json(output / "results.json", report)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
