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
HOVER_DEPTH_STAGES = (*PIN_DEPTH_STAGES[:-1], "front-pick", "opaque-pick", "off-again")
HOVER_CAMERA_HEIGHT = 1.2
CAP_STAGES = {"off": {}, "fill": {"capFill": True}, "hatch": {"capHatch": True},
              "both": {"capFill": True, "capHatch": True}, "off-again": {}}
CAP_PLANE = {"rear-cut": [0, 0, -1, .15], "front-cut": [0, 0, 1, -.15]}
GRID_STAGES = {"off": {}, "grid": {"sectionVisual": True}, "off-again": {}}


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


def hover_depth_fixture(path):
    meshes, elements = [], []

    def add(name, kind, bounds, z, alpha, gray=180):
        mesh_id = len(meshes) + 1
        meshes.append(panel(mesh_id, bounds, z))
        elements.append(dict(mesh_id=mesh_id, guid=name, type=kind, color=dict(r=gray, g=gray, b=gray, a=alpha)))

    for zone, x, depths, alpha in PIN_DEPTH_ZONES:
        # A pickable translucent target leaves the rear glass visible within
        # its triangle edges on both native and shader-based wireframe routes.
        add(f"{zone}-target", "IfcWall", (x - .24, .55, x + .24, 1.90), 0, 64)
        lower_bounds = (x - .33, .45, x + .33, 1.10)
        if zone == "clear":
            # Match the brightness of the surface behind the hovered target
            # in the rear-glass lane without adding another OIT layer here.
            add("clear-backdrop", "IfcRoof", lower_bounds, -1.5, 255, gray=204)
        for index, depth in enumerate(depths):
            # Leave an exposed pick region above the roof while fully covering
            # the target's measured lower edges.
            add(f"{zone}-roof-{index}", "IfcRoof", lower_bounds, depth, alpha)
    path.write_text(json.dumps(dict(schema_version="1.1.0", meshes=meshes, elements=elements), indent=2) + "\n",
                    encoding="utf-8")


def hover_depth_sequence(path, techniques, stages):
    frames, probes = {}, {}
    events = [{"frame": 1, "orthographic": True}]
    # The fixed orthographic width of 3.2 places the four centers at x=64,
    # 192, 320 and 448. y=128 is exposed, y=248 is inside the partial cover.
    pointer = {"off": ([5, 5], None), "off-again": ([5, 5], None),
               "clear": ([64, 128], "clear-target"), "behind": ([192, 248], "behind-target"),
               "front": ([320, 128], "front-target"), "opaque": ([448, 128], "opaque-target"),
               "front-pick": ([320, 248], "front-roof-1"), "opaque-pick": ([448, 248], "opaque-roof-0")}
    active_technique = techniques[0]
    for technique in techniques:
        for stage in stages:
            frame = (len(frames) + 1) * 4
            label = f"{technique}/{stage}"
            frames[label] = frame
            pixel, guid = pointer[stage]
            probes[label] = (pixel, guid)
            # The renderer consumes the pointer after submitting a frame.
            if technique != active_technique:
                events.append({"frame": frame - 2, "technique": technique})
                active_technique = technique
            events.extend([{"frame": frame - 1, "viewportPointer": {"action": "hover", "pixel": pixel}},
                           {"frame": frame, "depthProbe": pixel}])
    path.write_text(json.dumps(dict(schemaVersion=1, frames=max(frames.values()), sampleFrames=list(frames.values()),
                                    events=events), indent=2) + "\n", encoding="utf-8")
    return frames, probes


def section_cap_fixture(path):
    meshes, elements = [], []

    def add(name, kind, mesh, alpha, gray):
        meshes.append(mesh)
        color = dict(r=gray, g=gray, b=gray, a=alpha)
        elements.append(dict(mesh_id=mesh["mesh_id"], guid=name, type=kind, color=color))

    for zone, x, _, _ in PIN_DEPTH_ZONES:
        # Four lateral faces intersect either half-space in the same closed
        # contour. The observer-facing opening keeps the front-cut cap visible.
        ymin, ymax, w = .65, 1.35, .28
        rim = [(x - w, ymin), (x + w, ymin), (x + w, ymax), (x - w, ymax)]
        coords = [value for z in (-.65, .65) for dx, dy in rim for value in (dx, -z, dy)]
        indices = [0, 2, 1, 0, 3, 2]
        for i in range(4):
            j = (i + 1) % 4
            indices.extend((i, j, j + 4, i, j + 4, i + 4))
        add(f"{zone}-wall", "IfcWall", dict(mesh_id=len(meshes), coordinates=coords, indices=indices), 255, 185)
        layers = {"behind": (-.10, -.25), "front": (.32, .48), "opaque": (.45,)}.get(zone, ())
        alpha = {"behind": 250, "front": 166, "opaque": 255}.get(zone, 255)
        for i, z in enumerate(layers):
            bounds = (x - w - .03, ymin - .03, x + w + .03, ymax + .03)
            add(f"{zone}-cover-{i}", "IfcRoof", panel(len(meshes), bounds, z), alpha, 185)
    path.write_text(json.dumps(dict(schema_version="1.1.0", meshes=meshes, elements=elements), indent=2) + "\n",
                    encoding="utf-8")


def section_cap_sequence(path, cuts, stages=CAP_STAGES):
    frames, events = {}, [{"frame": 1, "orthographic": True}]
    for technique in TECHNIQUES:
        for cut in cuts:
            for stage, flags in stages.items():
                frame = (len(frames) + 1) * 3 + 3
                frames[f"{technique}/{cut}/{stage}"] = frame
                event = {"frame": frame - 2, "bimAnnotations": flags}
                if stage == "off":
                    event.update(technique=technique, sectionPlane=CAP_PLANE[cut])
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
           mode="lit", ordering=None, probes=None):
    name = f"{ordering}-ordering-{mode}-{extension}" if ordering else f"{technique}-{extension}"
    screenshot = output / f"{name}.{extension}"
    log = output / f"{name}.log"
    extent = OVERVIEW_EXTENT if ordering else (WIDTH, HEIGHT)
    width, height = extent
    camera_height = str({"issue-pin": PIN_HEIGHT, "bim-hover": HOVER_CAMERA_HEIGHT}.get(ordering, 1))
    model_args = ["--model", str(bim)] if ordering else ["--model", str(model), "--bim-model", str(bim)]
    command = [str(renderer), *model_args, "--import-scale", "1",
               "--hidden", "--no-ui", "--validation", "--width", str(width), "--height", str(height),
               "--msaa", "1", "--no-taa", "--no-bloom", "--render-technique", technique,
               "--display-mode", mode, "--camera-position", "0", camera_height, "8", "--camera-target", "0", camera_height, "0",
               "--camera-fov", "45", "--sky", "plain", "--sky-horizon", "0", "0", "0",
               "--exposure", "1", "--directional-intensity", "2", "--directional-direction", "-.3", "-.2", "-1",
               "--environment-intensity", "0", "--bounce-intensity", ".2", "--fixed-dt", ".016666667",
               "--screenshot-viewport", "--capture-sequence", str(sequence), "--screenshot", str(screenshot)]
    if ordering:
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
        active_technique = kind.split("/", 1)[0] if ordering else technique
        pixels, state = read_capture(image, active_technique, extent=extent, mode=mode if ordering else None)
        require(Path(state["scene"]["auxiliary"]).resolve() == bim
                and state["scene"]["auxiliaryObjectCount"] >= (10 if probes else 5 if ordering else 3),
                f"Incomplete fixture in {image}")
        if ordering:
            current = state["camera"]["unjitteredViewProjColumns"]
            if projection is None:
                projection = current
            active_passes = {row["name"] for row in state["passes"] if row["active"]}
            require(current == projection and [column[3] for column in current] == [0, 0, 0, 1]
                    and {"OitClear", "OitResolve"} <= active_passes and not state["oit"]["overflowed"],
                    f"Expected fixed orthographic camera and transparent surfaces in {image}")
        images[kind] = pixels
        sample = dict(kind=kind, frame=frame, technique=active_technique, image=str(image),
                      revision=state["build"]["revision"], decodedSha256=hashlib.sha256(pixels.tobytes()).hexdigest())
        if probes:
            probe = state["depthProbe"]
            sample["depthProbe"] = probe
            pixel, expected_guid = probes[kind]
            hit = probe["sceneInteraction"]["bim"]
            require(probe["pixel"] == pixel and probe["sceneInteraction"]["rawGpuId"] is not None
                    and (hit["guid"] if hit else None) == expected_guid, f"{kind}: unexpected viewport hit: {probe}")
        row["captures"].append(sample)
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
        evidence["diagnostics"] = inspect_overview_diagnostics(images, PIN_DEPTH_STAGES[1:-1], "Issue pin")


def inspect_overview_diagnostics(images, stages, annotation):
    import numpy as np

    diagnostics = {}
    for label, pane, threshold in (
            ("transparency", overview_panel_slice(.026, .506, .334, .724), 32),
            ("revealage", overview_panel_slice(.346, .506, .654, .724), 223)):
        off_tile = images["forward-raster/off"][pane]
        brightness = off_tile.max(axis=2)
        roof_pixels = np.count_nonzero(brightness > threshold if label == "transparency" else brightness < threshold)
        require(roof_pixels > 100, f"Forward Overview {label} diagnostic lacks transparent surfaces")
        for stage in stages:
            np.testing.assert_array_equal(images[f"forward-raster/{stage}"][pane], off_tile,
                                          err_msg=f"{annotation} changed Forward Overview {label} at {stage}")
        diagnostics[label] = {"roofPixels": int(roof_pixels), "changedPixels": 0}
    return diagnostics


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
                 ordering="issue-pin")
    evidence = report["checks"].setdefault("issuePinDepth", {})
    if not generate_only:
        inspect_pin_depth(lit, TECHNIQUES, evidence.setdefault("lit", {}))
    if lit_only:
        return

    sequence = output / "issue-pin-ordering-linear.sequence.json"
    frames = pin_depth_sequence(sequence, TECHNIQUES, ("off", "behind", "front"))
    linear = record(renderer, output, None, bim, sequence, frames, TECHNIQUES[0], "exr", report, generate_only,
                    ordering="issue-pin")
    sequence = output / "issue-pin-ordering-overview.sequence.json"
    frames = pin_depth_sequence(sequence, ("forward-raster",), PIN_DEPTH_STAGES)
    overview = record(renderer, output, None, bim, sequence, frames, "forward-raster", "png", report, generate_only,
                      mode="overview", ordering="issue-pin")
    if not generate_only:
        evidence["sceneLinear"] = inspect_linear_pin_depth(linear)
        inspect_pin_depth(overview, ("forward-raster",), evidence.setdefault("overview", {}), mode="overview")


def inspect_hover_depth(images, techniques, evidence, *, mode="lit"):
    import numpy as np

    viewport = overview_panel_slice(.026, .046, .334, .264) if mode == "overview" else np.s_[:, :]

    def cyan(rgb):
        return np.minimum(rgb[:, :, 1] - rgb[:, :, 0], rgb[:, :, 2] - rgb[:, :, 0])

    for technique in techniques:
        off = images[f"{technique}/off"][viewport].astype(np.int16)
        height, width = off.shape[:2]
        # The common lower target area sits entirely inside each roof, and
        # separate upper rows prove that real hover activated each target.
        upper = slice(round(.17 * height), round(.51 * height))
        lower = slice(round(.57 * height), round(.80 * height))
        before = cyan(off)
        zones = evidence[technique] = {}
        for index, (stage, _, _, _) in enumerate(PIN_DEPTH_ZONES):
            image = images[f"{technique}/{stage}"][viewport].astype(np.int16)
            start, end = index * width // 4, (index + 1) * width // 4
            ink = np.maximum(cyan(image) - before, 0)[:, start:end]
            delta = np.any(image != off, axis=2)
            zones[stage] = dict(upperCyanPixels=int(np.count_nonzero(ink[upper] > 5)),
                                upperCyanSignal=int(ink[upper].sum()),
                                lowerCyanPixels=int(np.count_nonzero(ink[lower] > 5)),
                                lowerCyanSignal=int(ink[lower].sum()), lowerChangedPixels=int(delta[lower, start:end].sum()),
                                lowerBackground=float(off[round(.65 * height), (start + end) // 2].mean()),
                                changedOutsideZone=int(delta[:, :start].sum() + delta[:, end:].sum()))
        for index, stage in ((2, "front-pick"), (3, "opaque-pick")):
            image = images[f"{technique}/{stage}"][viewport].astype(np.int16)
            start, end = index * width // 4, (index + 1) * width // 4
            ink = np.maximum(cyan(image) - before, 0)[:, start:end]
            # The foreground roof is wider than the target. Its outer edge
            # provides a visible witness independent of the GPU pick GUID.
            outer_width = round(.18 * ink.shape[1])
            roof_cyan = np.count_nonzero(ink[:, :outer_width] > 5) + np.count_nonzero(ink[:, -outer_width:] > 5)
            zones[stage] = dict(roofEdgeCyanPixels=int(roof_cyan),
                                upperChangedPixels=int(np.any(image[upper] != off[upper], axis=2).sum()))
        if mode == "lit":
            # The mirror lanes have the same pixel-aligned projection. Check
            # background parity along the line itself, including its edges.
            quarter = width // 4
            clear = images[f"{technique}/clear"][viewport].astype(np.int16)
            outline = (cyan(clear) - before)[lower, :quarter] > 5
            background_delta = np.abs(off[lower, :quarter] - off[lower, quarter:2 * quarter]).max(axis=2)
            zones["clear"]["lowerBackgroundMaxChannelDelta"] = int(background_delta[outline].max(initial=0))
        print(f"{technique}/{mode}: depth-ordered BIM hover {zones}", flush=True)

    for technique, zones in evidence.items():
        np.testing.assert_array_equal(images[f"{technique}/off-again"], images[f"{technique}/off"],
                                      err_msg=f"{technique}/{mode}: moving the pointer away left hover ink")
        clear, behind, front, opaque = (zones[stage] for stage, _, _, _ in PIN_DEPTH_ZONES)
        minimum = 8 if mode == "overview" else 20
        require(all(zone["upperCyanPixels"] >= minimum and zone["changedOutsideZone"] == 0
                    for zone in (clear, behind, front, opaque))
                and clear["lowerCyanPixels"] >= minimum, f"{technique}/{mode}: target hover control failed: {zones}")
        for stage in ("front-pick", "opaque-pick"):
            require(zones[stage]["roofEdgeCyanPixels"] >= minimum and zones[stage]["upperChangedPixels"] == 0,
                    f"{technique}/{mode}: pointer did not move the highlight to the foreground roof: {zones}")
        require(min(zone["lowerBackground"] for zone in (clear, behind, front, opaque)) > 120,
                f"{technique}/{mode}: a panel missed the target wireframe: {zones}")
        require(abs(clear["lowerBackground"] - behind["lowerBackground"]) <= 8,
                f"{technique}/{mode}: clear and rear-glass backgrounds did not match: {zones}")
        if mode == "lit":
            require(clear["lowerBackgroundMaxChannelDelta"] <= 8,
                    f"{technique}: backgrounds differ along the sampled hover outline: {zones}")
        if mode == "overview":
            # Tile downsampling can miss thin edges in one lane more than
            # another. Normalize to that same target's exposed upper edges.
            for zone in (clear, behind, front, opaque):
                zone["coveredToExposedCyanRatio"] = zone["lowerCyanSignal"] / zone["upperCyanSignal"]
        signal = "coveredToExposedCyanRatio" if mode == "overview" else "lowerCyanSignal"
        require(behind[signal] >= .65 * clear[signal],
                f"{technique}/{mode}: rear transparency attenuated the hovered BIM mesh: {zones}")
        require(.01 * clear[signal] < front[signal] < .70 * clear[signal],
                f"{technique}/{mode}: front transparency did not partially attenuate hover: {zones}")
        require(behind[signal] > 2 * front[signal],
                f"{technique}/{mode}: rear and front transparency produced similar hover: {zones}")
        require(opaque["lowerChangedPixels"] == 0, f"{technique}/{mode}: hover leaked through opaque geometry: {zones}")

    if mode == "overview":
        evidence["diagnostics"] = inspect_overview_diagnostics(images, HOVER_DEPTH_STAGES[1:-1], "BIM hover")


def run_hover_depth(renderer, output, report, generate_only, *, lit_only=False):
    bim = output / "bim-hover-depth-ordering.bim"
    hover_depth_fixture(bim)
    evidence = report["checks"].setdefault("bimHoverDepth", {})
    lanes = [("lit", "png", TECHNIQUES, HOVER_DEPTH_STAGES)]
    if not lit_only:
        lanes += [("lit", "exr", TECHNIQUES, ("off", "behind")),
                  ("overview", "png", ("forward-raster",), HOVER_DEPTH_STAGES)]
    for mode, extension, techniques, stages in lanes:
        sequence = output / f"bim-hover-ordering-{mode}-{extension}.sequence.json"
        frames, probes = hover_depth_sequence(sequence, techniques, stages)
        captures = record(renderer, output, None, bim, sequence, frames, techniques[0], extension, report,
                          generate_only, mode=mode, ordering="bim-hover", probes=probes)
        if generate_only:
            continue
        if extension == "exr":
            for technique in techniques:
                off = captures[f"{technique}/off"]
                require(float(off.max()) > .01 and off.tobytes() == captures[f"{technique}/behind"].tobytes(),
                        f"BIM hover entered {technique} scene-linear export or the reference is empty")
            evidence["sceneLinear"] = {"unchangedOnFrames": len(techniques)}
        else:
            inspect_hover_depth(captures, techniques, evidence.setdefault(mode, {}), mode=mode)


def compare_section_caps(images, techniques, cuts, evidence, *, mode, extension):
    import numpy as np

    failures = []
    lit = overview_panel_slice(.026, .046, .334, .264) if mode == "overview" else np.s_[:, :]
    for technique in techniques:
        for cut in cuts:
            cases = {stage: images[f"{technique}/{cut}/{stage}"] for stage in CAP_STAGES}
            off = cases["off"]
            key = f"{technique}/{cut}"
            scores = evidence.setdefault(key, {})
            if extension == "exr":
                require(float(off.max()) > .01, f"Empty scene-linear cut reference in {key}")
                for stage in tuple(CAP_STAGES)[1:]:
                    delta = np.max(np.abs(off - cases[stage]), axis=2)
                    largest = float(delta.max())
                    scores[stage] = {"maxAbsoluteDelta": largest,
                                     "changedPixels": int(np.count_nonzero(delta))}
                    if largest > .002:
                        failures.append(f"{key}/{stage} changed scene-linear radiance: {scores[stage]}")
                continue
            if not np.array_equal(off, cases["off-again"]):
                failures.append(f"{key}/{mode} retained cap pixels after switching off")
            height = off[lit].shape[0]
            band = slice(round(.35 * height), round(.65 * height))
            rendered = {stage: image[lit][band, :, :3].astype(np.int16) for stage, image in cases.items()}
            hatch_mask = np.max(np.abs(rendered["hatch"] - rendered["off"]), axis=2) > 2
            for stage, reference in (("fill", "off"), ("hatch", "off"), ("both", "fill")):
                zones = {}
                bands = [np.array_split(pixels, 4, axis=1)
                         for pixels in (rendered[reference], rendered[stage], hatch_mask)]
                for (zone, *_), before, after, hatch in zip(PIN_DEPTH_ZONES, *bands, strict=True):
                    delta = np.max(np.abs(after - before), axis=2)
                    visible = delta > 2
                    peak = np.unravel_index(delta.argmax(), delta.shape)
                    center = (delta.shape[0] // 2, delta.shape[1] // 2)
                    zones[zone] = dict(signal=int(delta[visible].sum()),
                                       changedPixels=int(np.count_nonzero(visible)),
                                       maxAbsoluteDelta=int(delta[peak]),
                                       peakRgbBefore=before[peak].tolist(), peakRgbAfter=after[peak].tolist(),
                                       centerRgbBefore=before[center].tolist(), centerRgbAfter=after[center].tolist())
                    if stage == "both":
                        zones[zone]["pixelsOnHatch"] = int(np.count_nonzero(visible & hatch))
                scores[stage] = zones
                clear = zones["clear"]
                if clear["signal"] < (60 if mode == "overview" else 350):
                    failures.append(f"{key}/{mode}/{stage} lacks an exposed section cap: {zones}")
                if cut == "rear-cut":
                    rear = zones["behind"]
                    if mode == "overview" and stage != "fill":
                        # The subpixel placement of narrow hatch lines affects
                        # the downsampled intensity sum. Compare coverage and
                        # peak contrast independently in the smaller tile.
                        visible_behind = (rear["changedPixels"] >= .7 * clear["changedPixels"]
                                          and rear["maxAbsoluteDelta"] >= .6 * clear["maxAbsoluteDelta"])
                    else:
                        visible_behind = rear["signal"] >= .65 * clear["signal"]
                    if not visible_behind:
                        failures.append(f"{key}/{mode}/{stage} lost cap ink under rear glass: {zones}")
                else:
                    if zones["front"]["signal"] > .35 * clear["signal"]:
                        failures.append(f"{key}/{mode}/{stage} did not attenuate behind front glass: {zones}")
                    if zones["opaque"]["maxAbsoluteDelta"]:
                        failures.append(f"{key}/{mode}/{stage} appeared over opaque geometry: {zones}")
                if stage == "both":
                    for zone in ("clear", "behind") if cut == "rear-cut" else ("clear",):
                        result = zones[zone]
                        if result["pixelsOnHatch"] < .8 * result["changedPixels"]:
                            failures.append(f"{key}/{mode} changed cap fill outside its hatch: {zone}: {result}")
    if mode == "overview":
        diagnostic = {f"forward-raster/{stage}": images[f"forward-raster/rear-cut/{stage}"] for stage in CAP_STAGES}
        evidence["diagnostics"] = inspect_overview_diagnostics(diagnostic, tuple(CAP_STAGES)[1:], "Section cap")
    print(f"Section cap {mode}/{extension}: {len(images)} captures, {len(failures)} failed pixel checks", flush=True)
    return failures


def run_section_cap_depth(renderer, output, report, generate_only, *, lit_only=False):
    bim = output / "section-cap-depth-ordering.bim"
    section_cap_fixture(bim)
    evidence = report["checks"].setdefault("sectionCapDepth", {})
    lanes = [("lit", "png", tuple(CAP_PLANE))]
    if not lit_only:
        lanes += [("overview", "png", ("rear-cut",)), ("lit", "exr", tuple(CAP_PLANE))]
    failures = []
    for mode, extension, cuts in lanes:
        sequence = output / f"section-cap-ordering-{mode}-{extension}.sequence.json"
        frames = section_cap_sequence(sequence, cuts)
        captures = record(renderer, output, None, bim, sequence, frames, TECHNIQUES[0], extension, report,
                          generate_only, mode=mode, ordering="section-cap")
        if not generate_only:
            failures.extend(compare_section_caps(captures, TECHNIQUES, cuts,
                                                evidence.setdefault(f"{mode}/{extension}", {}),
                                                mode=mode, extension=extension))
    evidence["failures"] = failures
    require(not failures, "\n".join(failures))


def compare_section_grid(images, evidence, *, mode, extension):
    import numpy as np

    failures = []
    lit = overview_panel_slice(.026, .046, .334, .264) if mode == "overview" else np.s_[:, :]
    for technique in TECHNIQUES:
        for cut in CAP_PLANE:
            key = f"{technique}/{cut}"
            off, grid, restored = [images[f"{key}/{stage}"] for stage in GRID_STAGES]
            if extension == "exr":
                require(float(off.max()) > .01, f"Empty scene-linear cut-plane reference in {key}")
                evidence[key] = {stage: float(np.max(np.abs(off - selected)))
                                 for stage, selected in (("grid", grid), ("off-again", restored))}
                if max(evidence[key].values()) > .002:
                    failures.append(f"{key} leaked the cut-plane grid into scene-linear radiance: {evidence[key]}")
                continue
            if not np.array_equal(off, restored):
                failures.append(f"{key}/{mode} retained cut-plane grid pixels after switching off")
            before, after = off[lit].astype(np.int16), grid[lit].astype(np.int16)
            height, width = before.shape[:2]
            # Sample the interior common to each wall and its cover. The
            # grid also spans the unoccluded margins of the viewport.
            rows = slice(round(.4 * height), round(.6 * height))
            zones = {}
            for i, (zone, _, _, _) in enumerate(PIN_DEPTH_ZONES):
                center = (i + .5) / 4
                columns = slice(round((center - .065) * width), round((center + .065) * width))
                delta = (after - before)[rows, columns]
                blue_ink = delta[:, :, 2] - delta[:, :, 0]
                visible = blue_ink > 4
                zones[zone] = dict(signal=int(blue_ink[visible].sum()),
                                   pixels=int(np.count_nonzero(visible)),
                                   peakBlueInk=int(blue_ink.max()), maxAbsoluteDelta=int(np.abs(delta).max()))
            evidence[key] = zones
            clear = zones["clear"]
            if clear["pixels"] < (5 if mode == "overview" else 30):
                failures.append(f"{key}/{mode} lacks an exposed cut-plane grid: {zones}")
            if cut == "rear-cut":
                rear = zones["behind"]
                if (rear["pixels"] < .55 * clear["pixels"]
                        or rear["peakBlueInk"] < .55 * clear["peakBlueInk"]):
                    failures.append(f"{key}/{mode} cut-plane grid disappeared behind rear glass: {zones}")
            else:
                # Deferred Overview displays opaque geometry and the grid,
                # but omits the forward transparent surface contribution.
                if (mode == "lit" or technique == "forward-raster") and (
                        zones["front"]["signal"] > .35 * clear["signal"]):
                    failures.append(f"{key}/{mode} front glass did not attenuate the cut-plane grid: {zones}")
                if zones["opaque"]["maxAbsoluteDelta"]:
                    failures.append(f"{key}/{mode} cut-plane grid appeared over opaque geometry: {zones}")
    if mode == "overview":
        for cut in CAP_PLANE:
            diagnostic = {f"forward-raster/{stage}": images[f"forward-raster/{cut}/{stage}"]
                          for stage in GRID_STAGES}
            evidence[f"{cut}/diagnostics"] = inspect_overview_diagnostics(
                diagnostic, tuple(GRID_STAGES)[1:], f"Cut-plane grid {cut}")
    print(f"Cut-plane grid {mode}/{extension}: {len(images)} captures, {len(failures)} failed pixel checks",
          flush=True)
    return failures


def run_section_grid_depth(renderer, output, report, generate_only, *, lit_only=False):
    bim = output / "section-grid-depth-ordering.bim"
    section_cap_fixture(bim)
    evidence = report["checks"].setdefault("sectionGridDepth", {})
    lanes = [("lit", "png")]
    if not lit_only:
        lanes += [("overview", "png"), ("lit", "exr")]
    failures = []
    for mode, extension in lanes:
        sequence = output / f"section-grid-ordering-{mode}-{extension}.sequence.json"
        frames = section_cap_sequence(sequence, CAP_PLANE, GRID_STAGES)
        captures = record(renderer, output, None, bim, sequence, frames, TECHNIQUES[0], extension,
                          report, generate_only, mode=mode, ordering="section-grid")
        if not generate_only:
            failures.extend(compare_section_grid(captures, evidence.setdefault(f"{mode}/{extension}", {}),
                                                mode=mode, extension=extension))
    evidence["failures"] = failures
    require(not failures, "\n".join(failures))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--renderer", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--generate-only", action="store_true")
    parser.add_argument("--managed-output", action="store_true", help="Make a fresh run directory beneath the output.")
    focused = parser.add_mutually_exclusive_group()
    focused.add_argument("--issue-pin-ordering-only", nargs="?", const="all", choices=("lit", "all"),
                         help="run only the depth-tested issue-pin sequence, optionally Lit only")
    focused.add_argument("--bim-hover-ordering-only", nargs="?", const="all", choices=("lit", "all"),
                         help="run only the depth-tested BIM hover sequence, optionally Lit only")
    focused.add_argument("--section-cap-ordering-only", nargs="?", const="all", choices=("lit", "all"),
                         help="run only the depth-tested section cap sequence, optionally Lit only")
    focused.add_argument("--section-grid-ordering-only", nargs="?", const="all", choices=("lit", "all"),
                         help="run only the depth-tested cut-plane grid sequence, optionally Lit only")
    baseline = parser.add_mutually_exclusive_group()
    baseline.add_argument("--expect-missing-forward-annotations", action="store_true",
                          help="check the older capture format on a build without Forward annotations")
    baseline.add_argument("--expect-missing-forward-technical", action="store_true",
                          help="expect only Technical elevation to be missing from Forward raster")
    args = parser.parse_args()
    expect_missing = args.expect_missing_forward_annotations or args.expect_missing_forward_technical
    focused_check = next(((runner, mode) for runner, mode in (
        (run_pin_depth, args.issue_pin_ordering_only),
        (run_hover_depth, args.bim_hover_ordering_only),
        (run_section_cap_depth, args.section_cap_ordering_only),
        (run_section_grid_depth, args.section_grid_ordering_only),
    ) if mode), None)
    if focused_check and expect_missing:
        parser.error("The depth-ordering checks require working raster annotation pipelines")
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
        if focused_check:
            check_ordering, scope = focused_check
            check_ordering(renderer, output, report, args.generate_only, lit_only=scope == "lit")
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
            run_hover_depth(renderer, output, report, args.generate_only)
            run_section_cap_depth(renderer, output, report, args.generate_only)
            run_section_grid_depth(renderer, output, report, args.generate_only)
        if not args.generate_only:
            require(hashlib.sha256(renderer.read_bytes()).hexdigest() == report["rendererSha256"], "Renderer changed")
            report["passed"] = True
    finally:
        (output / "results.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
