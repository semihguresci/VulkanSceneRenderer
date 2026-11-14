"""Render six extruded IFC area profiles in both raster routes and TAA modes."""

import argparse
import json
import math
import os
from pathlib import Path
import subprocess
import tempfile

from display_capture_test_support import validation_log_error


WIDTH, HEIGHT = 640, 384
DISTANCE, FOV, DEPTH = 11., 43., .8
TECHNIQUES = ("deferred-raster", "forward-raster")
CENTERS = ((-2.8, 1.25), (0., 1.25), (2.8, 1.25), (-2.8, -1.25), (0., -1.25), (2.8, -1.25))
COLORS = ((.12, .72, .83), (.85, .25, .09), (.7, .19, .79), (.18, .82, .31), (.88, .67, .10), (.26, .42, .91))
NAMES = ("Rounded rectangle", "Mapped hollow rectangle", "Ellipse", "Trapezium", "Butt-ended centreline", "Composite islands")
TURN = math.radians(27.)
# Analytic outer/inner path envelopes around an orthogonal, open, 0.32 m strip.
STRIP = ((-.94, -.76), (.51, -.76), (.51, .46), (.94, .46), (.94, .78), (.19, .78), (.19, -.44), (-.94, -.44))
TRAPEZIUM = ((-1.05, -.73), (1.05, -.73), (.56, .73), (-.56, .73))
ISLANDS = ((-.64, -.20), (.64, .20))
WITNESSES = (
    (0, "flat center", (0., 0.), True), (0, "rounded corner", (.99, .76), False),
    (1, "mapped right rim", (.94, 0.), True), (1, "mapped top rim", (0., .69), True),
    (1, "clear void", (0., 0.), False), (1, "clear mapped corner", (1.035, .815), False),
    (2, "ellipse tip", (.88, 0.), True), (2, "outside ellipse", (.85, .53), False),
    (3, "wide base", (.78, -.5), True), (3, "narrow top", (.94, .5), False),
    (4, "outer miter", (.44, -.69), True), (4, "return leg", (.75, .62), True),
    (4, "open interior", (-.28, .14), False), (4, "past butt end", (-1.08, -.6), False),
    (5, "left island", ISLANDS[0], True), (5, "right island", ISLANDS[1], True),
    (5, "unbridged gap", (0., 0.), False),
)


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


def save_json(path, value):
    path.write_text(json.dumps(value, indent=2, allow_nan=False) + "\n", encoding="utf-8")


class StepFixture:
    def __init__(self):
        self.elements = []

    def add(self, family, *fields):
        self.elements.append(f"#{len(self.elements) + 1}={family}({','.join(map(str, fields))});")
        return f"#{len(self.elements)}"

    def vector(self, family, values):
        return self.add(family, "(" + ",".join(map(str, values)) + ")")

    def point(self, *values):
        return self.vector("IFCCARTESIANPOINT", values)

    def direction(self, *values):
        return self.vector("IFCDIRECTION", values)

    def profile_placement(self, center):
        return self.add("IFCAXIS2PLACEMENT2D", self.point(*center), "$")

    def representation(self, kind, item):
        return self.add("IFCSHAPEREPRESENTATION", "$", "'Body'", f"'{kind}'", f"({item})")


def fixture(path):
    step = StepFixture()
    step.add("IFCSIUNIT", "*", ".LENGTHUNIT.", "$", ".METRE.")
    origin = step.add("IFCAXIS2PLACEMENT3D", step.point(0., 0., 0.), "$", "$")
    up = step.direction(0., 0., 1.)
    ax, ay = step.direction(1., 0., 0.), step.direction(0., -1., 0.)
    line_points = [step.point(*p) for p in ((-.94, -.6), (.35, -.6), (.35, .62), (.94, .62))]
    line = step.add("IFCPOLYLINE", f"({','.join(line_points)})")
    left = step.add("IFCRECTANGLEPROFILEDEF", ".AREA.", "$", step.profile_placement(ISLANDS[0]), .7, .88)
    right = step.add("IFCELLIPSEPROFILEDEF", ".AREA.", "$", step.profile_placement(ISLANDS[1]), .38, .58)
    profiles = (
        step.add("IFCROUNDEDRECTANGLEPROFILEDEF", ".AREA.", "$", "$", 2.02, 1.6, .36),
        step.add("IFCRECTANGLEHOLLOWPROFILEDEF", ".AREA.", "$", "$", 2.1, 1.66, .27, .13, .4),
        step.add("IFCELLIPSEPROFILEDEF", ".AREA.", "$", "$", 1.03, .69),
        step.add("IFCTRAPEZIUMPROFILEDEF", ".AREA.", "$", "$", 2.1, 1.12, 1.46, .49),
        step.add("IFCCENTERLINEPROFILEDEF", ".AREA.", "$", line, .32),
        step.add("IFCCOMPOSITEPROFILEDEF", ".AREA.", "$", f"({left},{right})", "$"),
    )
    for index, (name, center, color, profile) in enumerate(zip(NAMES, CENTERS, COLORS, profiles, strict=True)):
        solid = step.add("IFCEXTRUDEDAREASOLID", profile, origin, up, DEPTH)
        rgb = step.add("IFCCOLOURRGB", "$", *color)
        style = step.add("IFCSURFACESTYLERENDERING", rgb, "0.", "$", "$", "$", "$", "$", "$", ".NOTDEFINED.")
        material = step.add("IFCSURFACESTYLE", f"'{name}'", ".BOTH.", f"({style})")
        step.add("IFCSTYLEDITEM", solid, f"({material})", "$")
        represented = step.representation("SweptSolid", solid)
        if index == 1:
            mapped = step.add("IFCREPRESENTATIONMAP", origin, represented)
            c, s = math.cos(TURN), math.sin(TURN)
            transform = step.add("IFCCARTESIANTRANSFORMATIONOPERATOR3D", step.direction(c, s, 0.),
                                 step.direction(-s, c, 0.), step.point(0., 0., 0.), 1., up)
            instance = step.add("IFCMAPPEDITEM", mapped, transform)
            represented = step.representation("MappedRepresentation", instance)
        shape = step.add("IFCPRODUCTDEFINITIONSHAPE", "$", "$", f"({represented})")
        # Source profile X/Y become renderer X/Y. Extrusion +Z faces the camera.
        placement = step.add("IFCAXIS2PLACEMENT3D", step.point(center[0], 0., center[1]), ay, ax)
        local = step.add("IFCLOCALPLACEMENT", "$", placement)
        guid = f"{index + 1:022d}"
        step.add("IFCBUILDINGELEMENTPROXY", f"'{guid}'", "$", f"'{name}'", "$", "$", local, shape, "$", "$")
    header = "ISO-10303-21;\nHEADER;\nFILE_DESCRIPTION(('ViewDefinition [DesignTransferView]'),'2;1');\n"
    header += "FILE_NAME('area-profiles.ifc','2026-11-14T00:00:00',(''),(''),'Container','','');\nFILE_SCHEMA(('IFC4'));\nENDSEC;\nDATA;\n"
    path.write_text(header + "\n".join(step.elements) + "\nENDSEC;\nEND-ISO-10303-21;\n", encoding="ascii")


def rotated(x, y, angle):
    c, s = math.cos(angle), math.sin(angle)
    return c * x - s * y, s * x + c * y


def project(index, point):
    x, y = rotated(*point, TURN) if index == 1 else point
    x, y = x + CENTERS[index][0], y + CENTERS[index][1]
    scale = HEIGHT / (2. * (DISTANCE - DEPTH) * math.tan(math.radians(FOV / 2.)))
    return [int(WIDTH / 2. + x * scale), int(HEIGHT / 2. - y * scale)]


def write_sequence(path):
    frames = list(range(16, 16 + len(WITNESSES)))
    events = [dict(frame=frame, depthProbe=project(index, point))
              for frame, (index, _, point, _) in zip(frames, WITNESSES, strict=True)]
    save_json(path, dict(schemaVersion=1, frames=frames[-1] + 1, sampleFrames=[frames[-1] + 1],
                         telemetryFrames=frames, events=events))
    return frames


def polygon(x, y, vertices):
    import numpy as np

    inside = np.zeros_like(x, dtype=bool)
    for (ax, ay), (bx, by) in zip(vertices, (*vertices[1:], vertices[0]), strict=True):
        if ay != by:
            inside ^= ((ay > y) != (by > y)) & (x < (bx - ax) * (y - ay) / (by - ay) + ax)
    return inside


def footprint(index, x, y):
    import numpy as np

    def rounded(u, v, hw, hh, r):
        dx, dy = np.abs(u) - hw + r, np.abs(v) - hh + r
        return np.hypot(np.maximum(dx, 0.), np.maximum(dy, 0.)) <= r

    if index == 0:
        return rounded(x, y, 1.01, .8, .36)
    if index == 1:
        u, v = rotated(x, y, -TURN)
        return rounded(u, v, 1.05, .83, .4) & ~rounded(u, v, .78, .56, .13)
    if index == 2:
        return (x / 1.03) ** 2 + (y / .69) ** 2 <= 1.
    if index == 3:
        return polygon(x, y, TRAPEZIUM)
    if index == 4:
        return polygon(x, y, STRIP)
    return ((np.abs(x - ISLANDS[0][0]) <= .35) & (np.abs(y - ISLANDS[0][1]) <= .44) |
            (((x - ISLANDS[1][0]) / .38) ** 2 + ((y - ISLANDS[1][1]) / .58) ** 2 <= 1.))


def interior_mask(index, x, y):
    # Classify only pixels at least three pixels from the analytic boundary.
    # This allows the fixed-pitch conic tessellation and subpixel TAA motion.
    pixels = 2. * (DISTANCE - DEPTH) * math.tan(math.radians(FOV / 2.)) / HEIGHT
    d = 3. * pixels
    solid = footprint(index, x, y)
    for dx, dy in ((d, 0.), (-d, 0.), (0., d), (0., -d), (d, d), (d, -d), (-d, d), (-d, -d)):
        solid = solid & footprint(index, x + dx, y + dy)
    return solid


def check_pixels(image):
    import numpy as np
    from PIL import Image

    with Image.open(image) as source:
        pixels = np.asarray(source.convert("RGB"), dtype=np.int16)
    require(pixels.shape == (HEIGHT, WIDTH, 3), "Wrong IFC image extent")
    # Quiet near-black sky is stable across both routes, with tone mapping and TAA.
    threshold = np.max(pixels[:12, :12]) + 16
    visible = np.max(pixels, axis=2) > threshold
    dx = 2. * (DISTANCE - DEPTH) * math.tan(math.radians(FOV / 2.)) / HEIGHT
    yy, xx = np.mgrid[:HEIGHT, :WIDTH]
    x, y = (xx + .5 - WIDTH / 2.) * dx, (HEIGHT / 2. - yy - .5) * dx
    results = {}
    for index, (cx, cy) in enumerate(CENTERS):
        solid = interior_mask(index, x - cx, y - cy)
        lit = pixels[solid].mean(axis=0)
        contrast = float(lit.max() - lit.min())
        coverage = float(np.mean(visible[solid]))
        require(coverage >= .97 and contrast >= 20. and int(np.sum(solid)) >= 350,
                f"Missing, unlit, or unstyled {NAMES[index]} interior: {coverage}, {lit.tolist()}")
        results[NAMES[index]] = {"interiorPixels": int(np.sum(solid)), "visibleFraction": coverage,
                                 "meanRgb8": lit.tolist(), "meanChannelContrast": contrast}
    return results


def read_state(path):
    return json.loads(path.with_suffix(".telemetry.json").read_bytes())


def record(renderer, output, model, sequence, frames, technique, taa):
    import numpy as np

    base = output / f"{technique}-taa{int(taa)}.png"
    log = base.with_suffix(".log")
    command = [str(renderer), "--model", str(model), "--hidden", "--no-ui", "--validation", "--msaa", "1",
               "--width", str(WIDTH), "--height", str(HEIGHT), "--render-technique", technique,
               "--taa" if taa else "--no-taa", "--no-bloom", "--no-ray-query", "--no-ddgi",
               "--import-scale", "1", "--display-mode", "lit", "--sky", "plain", "--sky-horizon", "0", "0", "0",
               "--camera-position", "0", "0", str(DISTANCE), "--camera-target", "0", "0", "0", "--camera-fov", str(FOV),
               "--directional-intensity", "2", "--directional-direction", "-.3", "-.35", "-1", "--environment-intensity", "0",
               "--bounce-intensity", "0", "--exposure", ".65", "--fixed-dt", ".016666667", "--screenshot-viewport",
               "--capture-sequence", str(sequence), "--screenshot", str(base)]
    with log.open("w", encoding="utf-8") as stream:
        process = subprocess.run(command, cwd=renderer.parent, env=dict(os.environ, VK_LAYER_SETTINGS_PATH=str(output)),
                                 stdout=stream, stderr=subprocess.STDOUT, timeout=150,
                                 creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
    text = log.read_text(encoding="utf-8", errors="replace")
    require(process.returncode == 0 and not validation_log_error(text), f"Native IFC profile capture failed: {log}")
    if os.environ.get("VK_LOADER_DEBUG") == "layer":
        require('Insert instance layer "VK_LAYER_KHRONOS_validation"' in text, f"Missing validation layer: {log}")
    state = read_state(base)
    require(state["validationEnabled"] and state["technique"] == technique and state["taa"]["enabled"] == taa and
            state["resolution"] == [WIDTH, HEIGHT], f"Unexpected renderer settings: {base}")
    scene = state["scene"]
    require(not scene["primary"] and Path(scene["auxiliary"]).resolve() == model, f"Wrong IFC model: {scene}")
    report = scene["import"]
    require(report["status"] == "Complete" and report["sourceProducts"] == report["importedProducts"] == len(NAMES) and
            report["skippedProducts"] == report["partialProducts"] == 0 and not report["representationWarnings"], report)
    witnesses = {}
    for frame, (index, label, point, positive) in zip(frames, WITNESSES, strict=True):
        frame_path = base.with_name(f"{base.stem}.frame-{frame:04d}.png")
        sample = read_state(frame_path)
        depth = sample["depthProbe"]["opaque"]
        require(sample["depthProbe"]["pixel"] == project(index, point), sample["depthProbe"])
        if positive:
            matrix = np.asarray(sample["camera"]["viewProjColumns"]).T
            clip = matrix @ [0., 0., DEPTH, 1.]
            require(depth is not None and abs(depth - clip[2] / clip[3]) < 2e-6,
                    f"{NAMES[index]} {label} misses the front cap: {depth}")
        else:
            require(depth is None or depth < 1e-7, f"{NAMES[index]} {label} is unexpectedly filled: {depth}")
        witnesses[f"{NAMES[index]} {label}"] = depth
    return dict(technique=technique, taa=taa, revision=state["build"]["revision"], imported=report,
                footprints=check_pixels(base), depthWitnesses=witnesses, command=command, log=str(log))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--renderer", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--managed-output", action="store_true")
    parser.add_argument("--generate-only", action="store_true")
    args = parser.parse_args()
    if not args.generate_only and os.environ.get("CONTAINER_RUN_GPU_IFC_AREA_PROFILES") != "1":
        print("SKIP: set CONTAINER_RUN_GPU_IFC_AREA_PROFILES=1 to run IFC profile captures")
        return 77
    renderer, output = args.renderer.resolve(), args.output.resolve()
    if args.managed_output:
        output.mkdir(parents=True, exist_ok=True)
        output = Path(tempfile.mkdtemp(prefix="run-", dir=output))
    else:
        output.mkdir(parents=True)
    model, sequence = output / "area-profiles.ifc", output / "depth-witnesses.json"
    fixture(model)
    frames = write_sequence(sequence)
    if args.generate_only:
        return 0
    (output / "vk_layer_settings.txt").write_text("khronos_validation.validate_sync = true\n", encoding="utf-8")
    results = []
    for technique in TECHNIQUES:
        for taa in (False, True):
            results.append(record(renderer, output, model, sequence, frames, technique, taa))
            save_json(output / "results.json", results)
            print(f"PASS {technique}, TAA {taa}: six IFC4 profile footprints and {len(frames)} depth samples", flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
