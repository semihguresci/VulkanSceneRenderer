"""Verify metric IFC strokes and dash-gap picking in both Vulkan raster routes."""

import argparse
import json
import math
import os
from pathlib import Path
import subprocess
import tempfile

from PIL import Image
import numpy as np

from display_capture_test_support import validation_log_error
from ifc_area_profiles_runtime_regression import (
    DISTANCE, FOV, HEIGHT, TECHNIQUES, WIDTH, StepFixture, require, save_json,
)
from ifc_spherical_edge_loops_runtime_regression import placement


CURVE_LENGTH_MM = 5600.
# The second pair has different on/off lengths, so repeating only the first
# authored entry cannot match the silhouette.
PATTERN_MM = ((550., 360.), (270., 470.))
STROKES = (
    ("Narrow cyan", 1.02, 160., 1., (.11, .77, .92)),
    ("Wide orange", -1.02, 200., 1.5, (.92, .36, .09)),
)
PANELS = (
    ("Behind the cyan stroke", .68, 1.35, -.45),
    ("In front of the cyan stroke", 2.18, 2.59, .35),
)
PANEL_HALF_HEIGHT = .24
PIXELS_PER_METRE = HEIGHT / (2. * math.tan(math.radians(FOV / 2.)) * DISTANCE)
X_AXIS, Z_AXIS = (1., 0., 0.), (0., 0., 1.)


def world_x(stroke, station):
    return (station - CURVE_LENGTH_MM / 2.) * .001 * stroke[3]


def project(x, y, z=0.):
    scale = PIXELS_PER_METRE * DISTANCE / (DISTANCE - z)
    return [int(WIDTH / 2. + scale * x), int(HEIGHT / 2. - scale * y)]


def pick_points():
    points = []
    for index, stroke in enumerate(STROKES):
        _name, y, width, scale, _color = stroke
        radius = width * scale * .0005
        for label, station, offset, hit in (
            ("dash center", 255., 0., True),
            ("dash wing", 330., radius * .70, True),
            ("first gap", 720., 0., False),
            ("short dash", 1050., 0., True),
            ("second gap", 1385., 0., False),
            ("outside stroke", 1900., radius + .065, False),
            ("dash after polyline vertex", 2745., 0., True),
        ):
            points.append((f"{stroke[0]} {label}", index if hit else None,
                           project(world_x(stroke, station), y + offset)))
    stroke = STROKES[0]
    y = stroke[1]
    points.extend((
        ("dash in front of regular BIM mesh", 0, project(world_x(stroke, 3690.), y)),
        ("dash gap reveals rear BIM mesh", 2, project(world_x(stroke, 4010.), y)),
        ("rear BIM mesh control", 2, project(1.02, y + .15, PANELS[0][3])),
        ("foreground BIM mesh occludes dash", 3, project(world_x(stroke, 5200.), y)),
        ("foreground BIM mesh control", 3, project(2.38, y + .15, PANELS[1][3])),
    ))
    return points


def write_fixture(path):
    step = StepFixture()
    step.add("IFCSIUNIT", "*", ".LENGTHUNIT.", ".MILLI.", ".METRE.")
    model_frame = placement(step, (0., 0., 0.), Z_AXIS, X_AXIS)
    context = step.add("IFCGEOMETRICREPRESENTATIONCONTEXT", "$", "'Model'", 3, 1e-6, model_frame, "$")
    pairs = [step.add("IFCCURVESTYLEFONTPATTERN", visible, gap) for visible, gap in PATTERN_MM]
    font = step.add("IFCCURVESTYLEFONT", "'Alternating long and short dashes'", f"({','.join(pairs)})")

    def product(index, name, rep, y):
        shape = step.add("IFCPRODUCTDEFINITIONSHAPE", "$", "$", f"({rep})")
        # Match local and renderer XYZ axes after the source up-axis conversion.
        position = placement(step, (0., 0., y * 1000.), (0., -1., 0.), X_AXIS)
        local = step.add("IFCLOCALPLACEMENT", "$", position)
        step.add("IFCBUILDINGELEMENTPROXY", f"'{index + 1:022d}'", "$", f"'{name}'", "$", "$", local, shape, "$", "$")

    # The interior collinear vertex lies inside an authored dash. Both
    # occurrences reuse this curve while taking their styles at the mapped item.
    stations = (0., 2670., CURVE_LENGTH_MM)
    points = [step.point(station - CURVE_LENGTH_MM / 2., 0., 0.) for station in stations]
    curve = step.add("IFCPOLYLINE", f"({','.join(points)})")
    curves = step.add("IFCGEOMETRICCURVESET", f"({curve})")
    source_rep = step.add("IFCSHAPEREPRESENTATION", context, "'Body'", "'GeometricCurveSet'", f"({curves})")
    mapping = step.add("IFCREPRESENTATIONMAP", model_frame, source_rep)
    assignments = []
    for index, (name, y, width, scale, color) in enumerate(STROKES):
        rgb = step.add("IFCCOLOURRGB", "$", *color)
        style = step.add("IFCCURVESTYLE", f"'{name}'", font, f"IFCPOSITIVELENGTHMEASURE({width})", rgb, ".T.")
        transform = step.add("IFCCARTESIANTRANSFORMATIONOPERATOR3D", "$", "$", step.point(0., 0., 0.), scale, "$")
        mapped = step.add("IFCMAPPEDITEM", mapping, transform)
        assignments.append((mapped, style))
        rep = step.add("IFCSHAPEREPRESENTATION", context, "'Body'", "'MappedRepresentation'", f"({mapped})")
        product(index, name, rep, y)
    (cyan, cyan_style), (orange, orange_style) = assignments
    step.add("IFCPRESENTATIONLAYERWITHSTYLE", "'Corridor strokes'", "$", f"({cyan},{orange})", "$",
             ".T.", ".F.", ".F.", f"({orange_style})")
    # Cyan directly overrides the orange layer default.
    step.add("IFCSTYLEDITEM", cyan, f"({cyan_style})", "$")

    for index, (name, left, right, depth) in enumerate(PANELS, len(STROKES)):
        lo, hi = -PANEL_HALF_HEIGHT, PANEL_HALF_HEIGHT
        points = ",".join(f"({x * 1000.},{y * 1000.},{depth * 1000.})"
                          for x, y in ((left, lo), (right, lo), (right, hi), (left, hi)))
        listing = step.add("IFCCARTESIANPOINTLIST3D", f"({points})")
        panel = step.add("IFCTRIANGULATEDFACESET", listing, "$", ".F.", "((1,2,3),(1,3,4))", "$")
        rgb = step.add("IFCCOLOURRGB", "$", .35, .35, .35)
        shader = step.add("IFCSURFACESTYLESHADING", rgb, "0.")
        style = step.add("IFCSURFACESTYLE", f"'{name}'", ".BOTH.", f"({shader})")
        step.add("IFCSTYLEDITEM", panel, f"({style})", "$")
        rep = step.add("IFCSHAPEREPRESENTATION", context, "'Body'", "'Tessellation'", f"({panel})")
        product(index, name, rep, STROKES[0][1])
    header = "ISO-10303-21;\nHEADER;\nFILE_DESCRIPTION(('ViewDefinition [DesignTransferView]'),'2;1');\n"
    header += (f"FILE_NAME('{path.name}','2026-11-14T00:00:00',(''),(''),'Container','','');\n"
               "FILE_SCHEMA(('IFC4'));\nENDSEC;\nDATA;\n")
    path.write_text(header + "\n".join(step.elements) + "\nENDSEC;\nEND-ISO-10303-21;\n", encoding="ascii")


def write_sequence(path):
    picks = pick_points()
    events = []
    checks = []
    for index, (label, stroke, pixel) in enumerate(picks):
        frame = 17 + 2 * index
        events.append(dict(frame=frame, viewportPointer=dict(action="select", pixel=pixel)))
        checks.append((frame + 1, label, stroke, pixel))
    deselect = events[-1]["frame"] + 2
    events.append(dict(frame=deselect, viewportPointer=dict(action="select", pixel=[6, 6])))
    save_json(path, dict(schemaVersion=1, frames=deselect + 2, sampleFrames=[deselect + 2],
                         telemetryFrames=[frame for frame, _label, _target, _pixel in checks], events=events))
    return checks


def projected_mask(stroke):
    _name, y, width, scale, _color = stroke
    radius = width * scale * .0005
    yy, xx = np.mgrid[:HEIGHT, :WIDTH]
    x = (xx + .5 - WIDTH / 2.) / PIXELS_PER_METRE
    local_station = x / (scale * .001) + CURVE_LENGTH_MM / 2.
    phase = np.mod(local_station, sum(dash + gap for dash, gap in PATTERN_MM))
    active = np.zeros_like(phase, dtype=bool)
    margin = np.full_like(phase, np.inf)
    offset = 0.
    for dash, gap in PATTERN_MM:
        active |= (phase > offset) & (phase < offset + dash)
        margin = np.minimum(margin, np.abs(phase - offset))
        margin = np.minimum(margin, np.abs(phase - (offset + dash)))
        offset += dash + gap
    # Projecting a capped tube from its near side can move the apparent
    # endpoint by x * radius / (distance - radius). Leave a pixel for sampling.
    projected_span = CURVE_LENGTH_MM * scale * .0005 * PIXELS_PER_METRE
    endpoint_shift = projected_span * radius / (DISTANCE - radius)
    tolerance = (endpoint_shift + 1.) / (PIXELS_PER_METRE * scale * .001)
    safe = (margin > tolerance) & (phase < offset - tolerance)
    safe &= (local_station > tolerance) & (local_station < CURVE_LENGTH_MM - tolerance)
    y_difference = np.abs((HEIGHT / 2. - yy - .5) / PIXELS_PER_METRE - y)
    return active & safe, ~active & safe, y_difference, radius


def inspect_image(image):
    with Image.open(image) as source:
        pixels = np.asarray(source.convert("RGB"), dtype=np.int16)
    require(pixels.shape == (HEIGHT, WIDTH, 3), f"Unexpected image extent: {image}")
    lit = pixels.max(axis=2) > pixels[:12, :12].max() + 18
    evidence = {}
    for index, stroke in enumerate(STROKES):
        name = stroke[0]
        solid, gap, dy, radius = projected_mask(stroke)
        dominant, opposite = (2, 0) if index == 0 else (0, 2)
        visible = lit & (pixels[:, :, dominant] > pixels[:, :, opposite] + 18)
        if index == 0:
            _name, left, right, depth = PANELS[1]
            first, _ = project(left, 0., depth)
            last, _ = project(right, 0., depth)
            # Keep the foreground occlusion from skewing the stroke width
            # measurements. The picks and occlusion witnesses also check it.
            solid[:, first - 2:last + 3] = False
        # Centerline samples prove the dash order, repetition, and continuous
        # stationing across authored polyline edges.
        center = dy < 1.25 / PIXELS_PER_METRE
        on = np.count_nonzero(visible & center & solid)
        off = np.count_nonzero(visible & center & gap)
        require(on == np.count_nonzero(center & solid) and off == 0,
                f"{name} dash mask differs from authored lengths: {on} on, {off} leaked into gaps")
        inside = dy < radius - .9 / PIXELS_PER_METRE
        outside = (dy > radius + 1.3 / PIXELS_PER_METRE) & (dy < .35)
        require(np.all(visible[inside & solid]) and not np.any(visible[outside & solid]),
                f"{name} metric stroke coverage differs from authored width")
        safe_columns = np.any(center & solid, axis=0)
        samples = np.count_nonzero(visible[:, safe_columns] & (dy[:, safe_columns] < .35), axis=0)
        expected = 2. * radius * PIXELS_PER_METRE
        require(samples.size > 20 and abs(float(np.median(samples)) - expected) <= 1.5,
                f"{name} measured width {float(np.median(samples)):.2f}px, expected {expected:.2f}px")
        evidence[name] = dict(expectedWidthPixels=expected, measuredMedianWidthPixels=float(np.median(samples)),
                              onDashCenterPixels=int(on), leakedGapCenterPixels=int(off))
    for label, station in (("visible dash over rear mesh", 3690.), ("rear mesh through dash gap", 4010.),
                           ("front mesh hides dash", 5200.)):
        x, y = project(world_x(STROKES[0], station), STROKES[0][1])
        patch = pixels[y - 1:y + 2, x - 1:x + 2]
        cyan = patch[:, :, 2] > patch[:, :, 0] + 18
        require(np.all(patch.max(axis=2) > 18) and np.all(cyan == (station == 3690.)),
                f"Wrong composited stroke/panel color for {label}: {patch.tolist()}")
    return evidence


def read_state(path):
    return json.loads(path.with_suffix(".telemetry.json").read_bytes())


def record(renderer, output, model, sequence, checks, technique, taa, require_no_wide_lines):
    image = output / f"{technique}-taa{int(taa)}.png"
    log = image.with_suffix(".log")
    command = [
        str(renderer), "--model", str(model), "--import-scale", "1", "--hidden", "--validation",
        "--width", str(WIDTH), "--height", str(HEIGHT), "--msaa", "1",
        "--render-technique", technique, "--taa" if taa else "--no-taa", "--no-bloom", "--no-ray-query", "--no-ddgi",
        "--camera-position", "0", "0", str(DISTANCE), "--camera-target", "0", "0", "0", "--camera-fov", str(FOV),
        "--display-mode", "lit", "--sky", "plain", "--sky-horizon", "0", "0", "0", "--tone-map", "clip",
        "--exposure", "1", "--directional-intensity", "2", "--directional-direction", "0", "0", "-1",
        "--environment-intensity", "0", "--bounce-intensity", "0",
        "--fixed-dt", ".016666667", "--screenshot-viewport", "--capture-sequence", str(sequence),
        "--screenshot", str(image),
    ]
    with log.open("w", encoding="utf-8") as stream:
        process = subprocess.run(command, cwd=renderer.parent, env=dict(os.environ, VK_LAYER_SETTINGS_PATH=str(output)),
                                 stdout=stream, stderr=subprocess.STDOUT, timeout=150,
                                 creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
    text = log.read_text(encoding="utf-8", errors="replace")
    require(process.returncode == 0 and not validation_log_error(text), f"Vulkan capture failed: {log}")
    if os.environ.get("VK_LOADER_DEBUG") == "layer":
        require('Insert instance layer "VK_LAYER_KHRONOS_validation"' in text, f"Missing validation layer: {log}")
    state = read_state(image)
    require(state["validationEnabled"] and state["technique"] == technique and state["taa"]["enabled"] == taa and
            state["resolution"] == [WIDTH, HEIGHT], f"Unexpected renderer settings: {image}")
    if require_no_wide_lines:
        require(not state["wideLinesSupported"], "Expected Vulkan wideLines to be disabled")
    scene = state["scene"]
    report = scene["import"]
    require(not scene["primary"] and Path(scene["auxiliary"]).resolve() == model, f"Wrong model: {scene}")
    expected_products = len(STROKES) + len(PANELS)
    require(report["status"] == "Complete" and report["sourceProducts"] == report["importedProducts"] == expected_products and
            report["skippedProducts"] == report["partialProducts"] == 0 and not report["representationWarnings"], report)
    require(state["gui"]["selectedBimObjectIndex"] == -1, "Final screenshot retains a BIM selection tint")
    strokes = inspect_image(image)
    picked_strokes = {}
    witnesses = {}
    for frame, label, target, pixel in checks:
        sample = read_state(image.with_name(f"{image.stem}.frame-{frame:04d}.png"))
        selection = sample["gui"]["selectedBimObjectIndex"]
        if target is None:
            require(selection == -1, f"{label} picked object {selection} in an empty pixel {pixel}")
        else:
            require(selection >= 0, f"{label} failed to pick the authored stroke at {pixel}")
            expected = picked_strokes.setdefault(target, selection)
            require(selection == expected, f"{label} changed BIM identity from {expected} to {selection}")
        witnesses[label] = dict(pixel=pixel, objectIndex=selection)
    require(len(set(picked_strokes.values())) == expected_products, "Distinct IFC products picked the same BIM object")
    return dict(technique=technique, taa=taa, wideLines=state["wideLinesSupported"], strokes=strokes,
                picks=witnesses, command=command)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--renderer", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--managed-output", action="store_true")
    parser.add_argument("--generate-only", action="store_true")
    parser.add_argument("--require-no-wide-lines", action="store_true",
                        help="Assert telemetry from an externally selected Vulkan profile without wideLines.")
    args = parser.parse_args()
    if not args.generate_only and os.environ.get("CONTAINER_RUN_GPU_IFC_AREA_PROFILES") != "1":
        print("SKIP: set CONTAINER_RUN_GPU_IFC_AREA_PROFILES=1 to run IFC geometry captures")
        return 77
    renderer, output = args.renderer.resolve(), args.output.resolve()
    if args.managed_output:
        output.mkdir(parents=True, exist_ok=True)
        output = Path(tempfile.mkdtemp(prefix="run-", dir=output))
    else:
        output.mkdir(parents=True)
    model, sequence = output / "authored-curve-style.ifc", output / "curve-picks.json"
    write_fixture(model)
    checks = write_sequence(sequence)
    if args.generate_only:
        return 0
    (output / "vk_layer_settings.txt").write_text("khronos_validation.validate_sync = true\n", encoding="utf-8")
    results = []
    for technique in TECHNIQUES:
        for taa in (False, True):
            results.append(record(renderer, output, model, sequence, checks, technique, taa, args.require_no_wide_lines))
            save_json(output / "results.json", results)
            print(f"PASS {technique}, TAA {taa}: metric widths, dash/gap silhouette and {len(checks)} picks", flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
