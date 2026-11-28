"""Exercise hard clashes, result selection, and BCF export in Vulkan."""

import argparse
import json
import math
import os
from pathlib import Path
import subprocess
import tempfile
import xml.etree.ElementTree as ET

from bim_floor_plan_runtime_regression import panel, settings
from display_capture_test_support import validation_log_error
from physics_auxiliary_regression import cube


WIDTH, HEIGHT = 320, 240
TECHNIQUE = "deferred-raster"
FRAMES = {label: frame for frame, label in enumerate((
    "idle", "complete", "select", "export", "marker_off", "marker_restored",
    "same_category", "start", "cancel", "drain", "restart", "reselect",
    "replace_selected", "replacement_results", "restore_scene", "start_before_replace", "replace_pending",
    "restore_for_saved_sets", "saved_sets"), start=1)}
PICTURES = ("export", "marker_off", "marker_restored")
PENETRATION_CENTER = (.5, 1., 0.)
XRAY_WIDTH, XRAY_HEIGHT = 768, 576
XRAY_POSITION, XRAY_TARGET = (.5, 1., 8.), (.5, 1., 0.)
XRAY_FRAMES = {"covered_on": 4, "covered_off": 6, "clear_off": 8, "clear_on": 10}
TETRAHEDRON_INDICES = [0, 2, 1, 0, 1, 3, 0, 3, 2, 1, 2, 3]


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


def guid(element_number):
    return f"{element_number:022d}"


def save_json(path, contents):
    path.write_text(json.dumps(contents, indent=2) + "\n", encoding="utf-8")


class DotBimFixture:
    def __init__(self):
        self.meshes = []
        self.elements = []

    def mesh(self, label, number, category, points, indices, center):
        mesh_id = len(self.meshes)
        # Convert from renderer Y-up coordinates to authored DotBIM Z-up axes.
        coordinates = [component for x, y, z in points for component in (x, -z, y)]
        x, y, z = center
        self.meshes.append(dict(mesh_id=mesh_id, coordinates=coordinates, indices=indices))
        self.elements.append(dict(mesh_id=mesh_id, name=label, guid=guid(number), type=category,
                                  vector=dict(x=x, y=-z, z=y), color=dict(r=160, g=160, b=160, a=255)))

    def box(self, label, number, category, center, size):
        points, _, indices = cube()
        self.mesh(label, number, category,
                  [tuple(component * size for component in point) for point in points], indices, center)

    def write(self, path):
        save_json(path, dict(schema_version="1.1.0", meshes=self.meshes, elements=self.elements))


def fixtures(model, replacement):
    scene = DotBimFixture()
    scene.box("Penetration A", 1, "IfcWall", (0., 1., 0.), 2.)
    scene.box("Penetration B", 2, "IfcBeam", (1., 1., 0.), 2.)
    scene.box("Containment A", 3, "IfcWall", (6., 2., 0.), 3.)
    scene.box("Containment B", 4, "IfcBeam", (6., 2., 0.), 1.)
    scene.box("Touching A", 5, "IfcWall", (12., 1., 0.), 2.)
    scene.box("Touching B", 6, "IfcBeam", (14., 1., 0.), 2.)

    # Both tetrahedra span the same 2x2x2 box. Their opposing diagonal
    # planes bound x+y+z <= 2 and x+y+z >= 4 in their local coordinates.
    lower = [(0., 0., 0.), (2., 0., 0.), (0., 2., 0.), (0., 0., 2.)]
    upper = [tuple(2. - component for component in point) for point in lower]
    outward_upper = [TETRAHEDRON_INDICES[index + corner]
                     for index in range(0, len(TETRAHEDRON_INDICES), 3) for corner in (0, 2, 1)]
    scene.mesh("Disjoint tetrahedron A", 7, "IfcWall", lower, TETRAHEDRON_INDICES, (18., 0., 0.))
    scene.mesh("Disjoint tetrahedron B", 8, "IfcBeam", upper, outward_upper, (18., 0., 0.))
    scene.mesh("Open surface", 9, "IfcWall", [(-.8, -.8, -.8), (.8, .2, -.4), (-.5, .4, .8)],
               [0, 1, 2], (24., 1., 0.))
    scene.box("Solid containing the open surface", 10, "IfcBeam", (24., 1., 0.), 2.)
    scene.box("Same category A", 11, "IfcWall", (30., 1., 0.), 2.)
    scene.box("Same category B", 12, "IfcWall", (31., 1., 0.), 2.)
    scene.write(model)

    clean = DotBimFixture()
    clean.box("Replacement A", 21, "IfcWall", (0., 1., 0.), 2.)
    clean.box("Replacement B", 22, "IfcBeam", (5., 1., 0.), 2.)
    clean.write(replacement)


def capture_sequence(sequence, model, replacement, viewpoint):
    def event(label, action, **fields):
        return dict(frame=FRAMES[label], bimClash=dict(action=action, **fields))

    def start(label, category_b="IfcBeam", *, wait=True):
        return event(label, "start", categoryA="IfcWall", categoryB=category_b, wait=wait)

    events = [
        start("complete"),
        event("select", "select", result=0),
        event("export", "export", path=str(viewpoint)),
        dict(frame=FRAMES["marker_off"], bimAnnotations={}),
        event("marker_restored", "select", result=0),
        start("same_category", "IfcWall"),
        start("start", wait=False),
        event("cancel", "cancel"),
        event("drain", "wait"),
        start("restart"),
        event("reselect", "select", result=0),
        event("replace_selected", "wait") | dict(reload=str(replacement)),
        start("replacement_results"),
        dict(frame=FRAMES["restore_scene"], reload=str(model)),
        start("start_before_replace", wait=False),
        event("replace_pending", "wait") | dict(reload=str(replacement)),
        dict(frame=FRAMES["restore_for_saved_sets"], reload=str(model)),
        event("saved_sets", "start", guidsA=[guid(4), guid(3)], guidsB=[guid(3), guid(4), guid(5)], wait=True),
    ]
    save_json(sequence, dict(schemaVersion=1, frames=max(FRAMES.values()), events=events,
                             sampleFrames=[FRAMES[label] for label in PICTURES],
                             telemetryFrames=list(FRAMES.values())))


def capture_path(base, label):
    frame = FRAMES[label]
    return base if frame == max(FRAMES.values()) else base.with_name(f"{base.stem}.frame-{frame:04d}{base.suffix}")


def read_states(base, model, replacement):
    states = {}
    for label in FRAMES:
        state = json.loads(capture_path(base, label).with_suffix(".telemetry.json").read_bytes())
        require(state["validationEnabled"] and state["technique"] == TECHNIQUE and
                state["resolution"] == [WIDTH, HEIGHT], f"Wrong capture state at {label}")
        expected_model = replacement if label in ("replace_selected", "replacement_results", "replace_pending") else model
        require(Path(state["scene"]["auxiliary"]).resolve() == expected_model, f"Wrong BIM model at {label}")
        states[label] = state
    return states


def check_results(state, expected, *, checked, skipped):
    require(state["status"] == "complete" and state["failedPairs"] == 0, state)
    require((state["checkedPairs"], state["skippedElements"]) == (checked, skipped), state)
    pairs = state["pairs"]
    require(len(pairs) == len(expected), f"Expected {len(expected)} distinct clashes, observed {pairs}")
    for pair in pairs:
        key = frozenset(pair["objects"])
        require(key in expected and len(set(pair["objectIndices"])) == 2, pair)
        volume, center = expected[key]
        require(math.isclose(pair["volume"], volume, abs_tol=1e-5), pair)
        require(math.dist(pair["position"], center) < 1e-5, pair)
    require({frozenset(pair["objects"]) for pair in pairs} == set(expected), pairs)


def check_lifecycle(states):
    result = {label: state["bimClash"] for label, state in states.items()}
    cross_category = {frozenset((guid(1), guid(2))): (4., PENETRATION_CENTER),
                      frozenset((guid(3), guid(4))): (1., (6., 2., 0.))}
    for label in ("complete", "restart"):
        check_results(result[label], cross_category, checked=3, skipped=1)
    check_results(result["same_category"], {frozenset((guid(11), guid(12))): (4., (30.5, 1., 0.))},
                  checked=1, skipped=1)
    check_results(result["saved_sets"], {frozenset((guid(3), guid(4))): (1., (6., 2., 0.))}, checked=1, skipped=0)
    check_results(result["replacement_results"], {}, checked=0, skipped=0)
    for label in ("idle", "cancel", "drain", "replace_selected", "replacement_results", "restore_scene", "replace_pending"):
        state = result[label]
        require(state["pairs"] == [] and state["selected"] is None and state["markerDrawCount"] == 0,
                f"Stale clash state at {label}: {state}")
    require(result["idle"]["status"] == "idle", result["idle"])
    for label in ("start", "start_before_replace"):
        require(result[label]["status"] in ("running", "complete"), result[label])
    for label in ("cancel", "drain"):
        require(result[label]["status"] == "cancelled", result[label])
    for label in ("replace_selected", "restore_scene", "replace_pending"):
        require(result[label]["status"] == "idle", result[label])
    for label in ("select", "export", "marker_off", "marker_restored", "reselect"):
        state = result[label]
        require(state["selected"] == 0 and set(state["pairs"][0]["objects"]) == {guid(1), guid(2)}, state)
        require((state["markerDrawCount"] > 0) == (label != "marker_off"), state)


def read_picture(base, label):
    import numpy as np
    from PIL import Image

    path = capture_path(base, label)
    meta = json.loads(Path(f"{path}.capture.json").read_bytes())
    require(meta["source"] == "viewport" and not meta["includesUi"], f"Wrong viewport capture route: {path}")
    with Image.open(path) as image:
        require(image.format == "PNG" and image.size == (WIDTH, HEIGHT), f"Wrong capture dimensions: {path}")
        return np.asarray(image.convert("RGB"), dtype=np.int16)


def check_marker(base, states):
    import numpy as np

    camera = states["export"]["camera"]
    matrix = np.asarray(camera["unjitteredViewProjColumns"]).T
    require(states["idle"]["camera"]["position"] != camera["position"], "Selecting the pair did not focus the viewport")
    for label in ("select", "marker_off", "marker_restored"):
        np.testing.assert_allclose(states[label]["camera"]["unjitteredViewProjColumns"], matrix.T,
                                   atol=1e-5, rtol=0, err_msg=f"Camera changed at {label}")
    corners = [(x, y, z, 1.) for x in (-1., 2.) for y in (0., 2.) for z in (-1., 1.)]
    clip = matrix @ np.transpose(corners)
    require(bool(np.all(clip[3] > 0.) and np.all(np.abs(clip[:2] / clip[3]) < 1.)),
            "The focused camera does not frame both clash elements")

    on, off, restored = (read_picture(base, label) for label in PICTURES)
    np.testing.assert_array_equal(on, restored, err_msg="Reselecting the result did not restore the marker")
    delta = np.max(np.abs(on - off), axis=2) > 4
    rows, columns = np.where(delta & (on[:, :, 0] > on[:, :, 2] + 15))
    require(len(rows) >= 10, "The computed clash contributed no visible red marker")
    require(abs(float(np.median(rows)) - HEIGHT / 2) < HEIGHT / 8 and
            abs(float(np.median(columns)) - WIDTH / 2) < WIDTH / 8, "The clash marker missed the focused pair")
    return int(len(rows))


def check_viewpoint(path, camera):
    def vector(node):
        require(node is not None, f"Missing BCF vector in {path}")
        return tuple(float(node.findtext(axis)) for axis in "XYZ")

    root = ET.parse(path).getroot()
    selected = [component.get("IfcGuid") for component in root.findall("./Components/Selection/Component")]
    require(sorted(selected) == [guid(1), guid(2)], f"BCF selection omitted a clash element: {selected}")
    position = vector(root.find("./PerspectiveCamera/CameraViewPoint"))
    direction = vector(root.find("./PerspectiveCamera/CameraDirection"))
    require(math.dist(position, camera["position"]) < 1e-4 and
            math.isclose(math.hypot(*direction), 1., abs_tol=1e-4), "BCF camera differs from the focused viewport")
    to_clash = tuple(point - origin for point, origin in zip(PENETRATION_CENTER, position, strict=True))
    require(sum(delta * axis for delta, axis in zip(to_clash, direction, strict=True)) / math.hypot(*to_clash) > .999,
            "The exported camera does not point at the selected clash")
    locations = [vector(node) for node in root.findall("./ContainerMarkup/Pin/Location")]
    require(any(math.dist(point, PENETRATION_CENTER) < 1e-5 for point in locations),
            f"Missing clash location in {path}")


def xray_inputs(output):
    scene = DotBimFixture()
    scene.box("Penetration A", 1, "IfcWall", (0., 1., 0.), 2.)
    scene.box("Penetration B", 2, "IfcBeam", (1., 1., 0.), 2.)
    # Both alpha-250 panels belong to one IFC roof. Hiding that element makes
    # a clear control with the same selection, intersecting solids and camera.
    bounds = (-.9, -.35, 1.9, 2.35)
    back, front = (panel(2, bounds, z) for z in (1.55, 1.85))
    scene.meshes.append({"mesh_id": 2, "coordinates": back["coordinates"] + front["coordinates"],
                         "indices": back["indices"] + [index + 4 for index in front["indices"]]})
    scene.elements.append({"mesh_id": 2, "guid": guid(3), "type": "IfcRoof",
                           "color": {"r": 180, "g": 180, "b": 180, "a": 250}})
    model = output / "xray-clash.bim"
    scene.write(model)
    select = {"action": "select", "result": 0}
    events = [
        {"frame": 1, "bimFloorPlan": settings(enabled=False), "bimClash": {
            "action": "start", "categoryA": "IfcWall", "categoryB": "IfcBeam", "wait": True}},
        {"frame": 2, "bimClash": select},
        {"frame": 5, "bimAnnotations": {}},
        {"frame": 7, "bimHiddenObject": 2},
        {"frame": 9, "bimClash": select},
    ]
    camera = [{"frame": 1, "position": XRAY_POSITION, "target": XRAY_TARGET}]
    sequences = {}
    for extension, labels in (("png", tuple(XRAY_FRAMES)), ("exr", ("covered_on", "covered_off"))):
        path = output / f"xray-clash-{extension}.sequence.json"
        frames = [XRAY_FRAMES[label] for label in labels]
        save_json(path, {"schemaVersion": 1, "frames": frames[-1], "sampleFrames": frames, "camera": camera,
                         "events": [event for event in events if event["frame"] <= frames[-1]]})
        sequences[extension] = (path, labels)
    return model, sequences


def record_capture(command, renderer, output, log):
    with log.open("w", encoding="utf-8") as stream:
        process = subprocess.run(command, cwd=renderer.parent, stdout=stream, stderr=subprocess.STDOUT, timeout=180,
                                 env=dict(os.environ, VK_LAYER_SETTINGS_PATH=str(output)),
                                 creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
    text = log.read_text(encoding="utf-8", errors="replace")
    require(process.returncode == 0 and not validation_log_error(text), f"Vulkan capture failed: {log}")
    if os.environ.get("VK_LOADER_DEBUG") == "layer":
        require('Insert instance layer "VK_LAYER_KHRONOS_validation"' in text, f"Missing validation layer: {log}")


def capture_xray_case(renderer, output, model, sequence, labels, technique, mode, extension):
    base = output / f"xray-{technique}-{mode}.{extension}"
    command = [str(renderer), "--model", str(model), "--import-scale", "1", "--hidden", "--no-ui",
               "--validation", "--msaa", "1", "--no-taa", "--no-bloom", "--no-ray-query", "--no-ddgi",
               "--render-technique", technique, "--display-mode", mode,
               "--width", str(XRAY_WIDTH), "--height", str(XRAY_HEIGHT),
               "--camera-position", *map(str, XRAY_POSITION), "--camera-target", *map(str, XRAY_TARGET),
               "--camera-fov", "45", "--tone-map", "neutral", "--exposure", "1", "--sky", "plain",
               "--sky-horizon", ".15", ".15", ".15", "--directional-intensity", "4",
               "--directional-direction", "-.2", "-.25", "-1", "--environment-intensity", ".5",
               "--bounce-intensity", ".25", "--fixed-dt", ".016666667", "--screenshot-viewport",
               "--capture-sequence", str(sequence), "--screenshot", str(base)]
    record_capture(command, renderer, output, base.with_suffix(f".{extension}.log"))
    paths, projection = {}, None
    for label in labels:
        frame = XRAY_FRAMES[label]
        path = base if label == labels[-1] else output / f"{base.stem}.frame-{frame:04d}.{extension}"
        state = json.loads(path.with_suffix(".telemetry.json").read_bytes())
        sidecar = json.loads(Path(f"{path}.capture.json").read_bytes())
        require(state["validationEnabled"] and state["technique"] == technique and
                state["resolution"] == [XRAY_WIDTH, XRAY_HEIGHT] and
                Path(state["scene"]["auxiliary"]).resolve() == model, f"Wrong capture state: {path}")
        expected_source = "viewport" if extension == "png" else "raster-scene-composite"
        require(not sidecar["includesUi"] and sidecar["source"] == expected_source,
                f"Wrong capture route: {path}")
        if extension == "png":
            require(sidecar["displayMode"] == (8 if mode == "overview" else 0), f"Wrong display mode: {path}")
        clash = state["bimClash"]
        check_results(clash, {frozenset((guid(1), guid(2))): (4., PENETRATION_CENTER)}, checked=1, skipped=0)
        require(clash["selected"] == 0 and clash["markerDrawCount"] == int(label.endswith("_on")),
                f"Computed marker toggle missed: {path}: {clash}")
        if projection is None:
            projection = state["camera"]["unjitteredViewProjColumns"]
        require(state["camera"]["unjitteredViewProjColumns"] == projection, f"Camera drift: {path}")
        paths[label] = path
    return paths, projection


def check_xray_png(paths, technique, mode):
    import numpy as np
    from PIL import Image

    def region(left, top, right, bottom):
        return (slice(round(top * XRAY_HEIGHT), round(bottom * XRAY_HEIGHT)),
                slice(round(left * XRAY_WIDTH), round(right * XRAY_WIDTH)))

    images = {}
    for label, path in paths.items():
        with Image.open(path) as image:
            require(image.format == "PNG" and image.size == (XRAY_WIDTH, XRAY_HEIGHT), f"Invalid viewport: {path}")
            images[label] = np.asarray(image.convert("RGB"), dtype=np.int16)
    lit = region(.026, .046, .334, .264) if mode == "overview" else np.s_[:, :]
    outside_lit = np.ones(images["covered_on"].shape[:2], dtype=bool)
    outside_lit[lit] = False
    counts = {}
    for cover in ("covered", "clear"):
        on, off = (images[f"{cover}_{suffix}"] for suffix in ("on", "off"))
        np.testing.assert_array_equal(on[outside_lit], off[outside_lit],
                                      err_msg=f"{technique}: marker changed a pixel outside the Overview Lit tile")
        on = on[lit]
        delta = np.abs(on - off[lit]).max(axis=2) > 8
        red = (on[:, :, 0] > on[:, :, 1] + 15) & (on[:, :, 0] > on[:, :, 2] + 15)
        rows, columns = np.where(delta & red)
        require(len(rows) >= 10 and abs(float(np.median(rows)) - red.shape[0] / 2) < red.shape[0] / 8
                and abs(float(np.median(columns)) - red.shape[1] / 2) < red.shape[1] / 8,
                f"{technique} {mode} {cover}: computed clash missing from its projected position ({len(rows)} red pixels)")
        counts[cover] = len(rows)

    if mode == "lit" or technique == "forward-raster":
        delta = np.abs(images["covered_off"][lit] - images["clear_off"][lit]).max(axis=2)
        require(np.count_nonzero(delta > 8) >= 100, f"{technique} {mode}: roof did not visibly cover the scene")
    if technique == "forward-raster" and mode == "overview":
        transparency = region(.026, .506, .334, .724)
        revealage = region(.346, .506, .654, .724)
        for tile, threshold, opacity in ((transparency, 32, True), (revealage, 223, False)):
            covered = images["covered_off"][tile].max(axis=2)
            clear = images["clear_off"][tile].max(axis=2)
            covered = covered > threshold if opacity else covered <= threshold
            clear = clear > threshold if opacity else clear <= threshold
            require(np.count_nonzero(covered) >= 100 and not clear.any(),
                    "Forward Overview OIT diagnostics did not distinguish the roof from the clear control")
    print(f"PASS {technique} {mode} computed clash x-ray pixels {counts}", flush=True)


def check_xray_exr(paths):
    import cv2
    import numpy as np

    buffers = []
    for path in paths.values():
        sidecar = json.loads(Path(f"{path}.capture.json").read_bytes())
        require(sidecar["colorSpace"] == "linear-sRGB" and not sidecar["exposureApplied"] and not sidecar["toneMappingApplied"],
                f"Wrong scene-linear output: {path}")
        pixels = cv2.imdecode(np.frombuffer(path.read_bytes(), dtype=np.uint8), cv2.IMREAD_UNCHANGED)
        require(pixels is not None and pixels.shape == (XRAY_HEIGHT, XRAY_WIDTH, 3) and
                pixels.dtype == np.float32 and np.isfinite(pixels).all() and pixels.max() > .1,
                f"Invalid or empty scene-linear witness: {path}")
        buffers.append(pixels)
    np.testing.assert_array_equal(*buffers, err_msg="Computed clash marker entered the scene-linear image")


def check_xray_markers(renderer, output, inputs):
    os.environ.setdefault("OPENCV_IO_ENABLE_OPENEXR", "1")
    model, sequences = inputs
    baseline_projection = None
    for technique in ("forward-raster", "deferred-raster"):
        for mode, extension in (("overview", "png"), ("lit", "png"), ("lit", "exr")):
            sequence, labels = sequences[extension]
            paths, projection = capture_xray_case(renderer, output, model, sequence, labels, technique, mode, extension)
            if baseline_projection is None:
                baseline_projection = projection
            require(projection == baseline_projection, f"Camera mismatch across {technique} {mode} {extension}")
            if extension == "png":
                check_xray_png(paths, technique, mode)
            else:
                check_xray_exr(paths)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--renderer", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--generate-only", action="store_true")
    parser.add_argument("--managed-output", action="store_true", help="Make a fresh run directory beneath the output.")
    args = parser.parse_args()
    if not args.generate_only and os.environ.get("CONTAINER_RUN_GPU_BIM_HARD_CLASH") != "1":
        print("SKIP: set CONTAINER_RUN_GPU_BIM_HARD_CLASH=1 to run GPU captures")
        return 77
    renderer = args.renderer.resolve()
    output = args.output.resolve()
    if args.managed_output:
        output.mkdir(parents=True, exist_ok=True)
        output = Path(tempfile.mkdtemp(prefix="run-", dir=output))
    else:
        output.mkdir(parents=True)
    model = output / "hard-clash.bim"
    replacement = output / "replacement.bim"
    sequence = output / "hard-clash.sequence.json"
    viewpoint = output / "clash.bcfv"
    base = output / "hard-clash.png"
    fixtures(model, replacement)
    capture_sequence(sequence, model, replacement, viewpoint)
    xray = xray_inputs(output)
    if args.generate_only:
        return 0

    command = [str(renderer), "--model", str(model), "--import-scale", "1",
               "--hidden", "--no-ui", "--validation", "--msaa", "1", "--no-taa", "--no-bloom",
               "--no-ray-query", "--no-ddgi", "--render-technique", TECHNIQUE,
               "--display-mode", "lit", "--width", str(WIDTH), "--height", str(HEIGHT),
               "--camera-position", "4", "3", "8", "--camera-target", "4", "1", "0", "--camera-fov", "45",
               "--sky", "plain", "--sky-horizon", "0", "0", "0", "--environment-intensity", "0",
               "--directional-intensity", "2", "--directional-direction", "-.3", "-.2", "-1", "--bounce-intensity", "0",
               "--fixed-dt", ".016666667", "--screenshot-viewport", "--capture-sequence", str(sequence),
               "--screenshot", str(base)]
    (output / "vk_layer_settings.txt").write_text("khronos_validation.validate_sync = true\n", encoding="utf-8")
    record_capture(command, renderer, output, base.with_suffix(".log"))
    states = read_states(base, model, replacement)
    check_lifecycle(states)
    marker_pixels = check_marker(base, states)
    check_viewpoint(viewpoint, states["export"]["camera"])
    print(f"PASS hard clash volumes, deduplication, skips, cancellation, and reload ({output})", flush=True)
    print(f"PASS focused BCF viewpoint and {marker_pixels} visible clash marker pixels", flush=True)
    check_xray_markers(renderer, output, xray)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
