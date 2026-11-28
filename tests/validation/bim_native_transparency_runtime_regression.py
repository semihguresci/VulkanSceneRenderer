"""Check native BIM points, curves and their highlights against layered geometry."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import tempfile

from display_capture_test_support import validation_log_error


WIDTH, HEIGHT = 768, 576
SAMPLES = {6: "deferred-raster", 14: "forward-raster", 22: "deferred-raster"}
ZONES = (("front-roof", -1.06, (1.2, 1.5)), ("rear-roof", 0., (-1.2, -1.5)), ("clear", 1.06, ()))
LIT_TILE = (slice(round(.046 * HEIGHT), round(.264 * HEIGHT)),
            slice(round(.026 * WIDTH), round(.334 * WIDTH)))


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


def save(path, value):
    path.write_text(json.dumps(value, indent=2) + "\n", encoding="utf-8")


def position(x, y, z):
    # IFCX Z-up feeds the renderer's +Y-up axes as (x, z, -y).
    return [x, -z, y]


def panel_mesh(center, depths, bottom=.85, top=2.47):
    points, indices = [], []
    for z in depths:
        base = len(points)
        points.extend(position(x, y, z) for x, y in (
            (center - .4, bottom), (center + .4, bottom), (center + .4, top), (center - .4, top)))
        indices.extend(base + index for index in (0, 1, 2, 0, 2, 3))
    return dict(points=points, faceVertexIndices=indices, faceVertexCounts=[3] * (len(indices) // 3))


def primitive_nodes(label, center, point_rows=(1.15, 1.265, 1.38, 1.495), curve_rows=(1.86, 2.01, 2.16)):
    return [
        {"path": f"{label}-points", "attributes": {
            "bsi::ifc::globalId": f"{label}-points",
            "points::array": {"positions": [position(center + .12 * j, y, 0)
                                           for y in point_rows for j in range(-2, 3)]}}},
        {"path": f"{label}-curves", "attributes": {
            "bsi::ifc::globalId": f"{label}-curves",
            "bsi::ifc::presentation::diffuseColor": [1., .34, .05],
            "usd::usdgeom::basiscurves": {
                "points": [position(center + dx, y, 0) for y in curve_rows for dx in (-.28, .28)],
                "curveVertexCounts": [2] * len(curve_rows)}}},
    ]


def panel_node(label, center, depths, opacity, bottom=.85, top=2.47):
    return {"path": label, "attributes": {
        "bsi::ifc::globalId": label, "bsi::ifc::class": "IfcRoof",
        "bsi::ifc::presentation::diffuseColor": [.60, .60, .60],
        "bsi::ifc::presentation::opacity": opacity,
        "usd::usdgeom::mesh": panel_mesh(center, depths, bottom, top)}}


def fixture(path, *, native_only=False):
    nodes = []
    for label, center, depths in ZONES:
        nodes.extend(primitive_nodes(label, center))
        if depths and not native_only:
            nodes.append(panel_node(f"{label}-panels", center, depths, 250 / 255))
    # A separate upper strip guards against making native primitives ignore all
    # depth tests. The main three zones and their original camera stay unchanged.
    if not native_only:
        nodes.extend(primitive_nodes("opaque-front", 1.06, point_rows=(2.5, 2.6), curve_rows=(2.72,)))
        nodes.append(panel_node("opaque-front-panel", 1.06, (2.,), 1., bottom=2.4, top=2.82))
    children = {node["path"]: node["path"] for node in nodes}
    save(path, {"data": [{"path": "root", "children": children}, *nodes]})


def highlight_panels():
    yield "clear-backdrop", 1.06, (-1.5,), 1., .85, 2.47
    yield "opaque-front", 1.06, (2.,), 1., 2.4, 2.82
    for name, center, depths in ZONES[:2]:
        yield name, center, depths, 250 / 255, .85, 2.47


def highlight_ifcx(path):
    # A single IFCX point product and a single line product each cross all
    # four controls. The two opaque meshes sort before native object IDs.
    parts = [primitive_nodes(name, center) for name, center, _ in ZONES]
    parts.append(primitive_nodes("opaque", 1.06, point_rows=(2.5, 2.6), curve_rows=(2.72,)))
    points = [p for point, _ in parts for p in point["attributes"]["points::array"]["positions"]]
    curves = [p for _, line in parts
              for p in line["attributes"]["usd::usdgeom::basiscurves"]["points"]]
    nodes = [
        {"path": "points", "attributes": {
            "bsi::ifc::globalId": "highlight-points", "points::array": {"positions": points}}},
        {"path": "curves", "attributes": {
            "bsi::ifc::globalId": "highlight-lines", "bsi::ifc::presentation::diffuseColor": [.6] * 3,
            "usd::usdgeom::basiscurves": {"points": curves, "curveVertexCounts": [2] * (len(curves) // 2)}}},
    ]
    nodes.extend(panel_node(*panel) for panel in highlight_panels())
    save(path, {"data": [{"path": "root", "children": {node["path"]: node["path"] for node in nodes}}, *nodes]})


def highlight_ifc(path):
    from ifc_area_profiles_runtime_regression import StepFixture
    from ifc_spherical_edge_loops_runtime_regression import placement

    step = StepFixture()
    step.add("IFCSIUNIT", "*", ".LENGTHUNIT.", ".MILLI.", ".METRE.")
    x_axis, z_axis = (1., 0., 0.), (0., 0., 1.)
    frame = placement(step, (0., 0., 0.), z_axis, x_axis)
    context = step.add("IFCGEOMETRICREPRESENTATIONCONTEXT", "$", "'Model'", 3, 1e-6, frame, "$")
    local_frame = placement(step, (0., 0., 0.), (0., -1., 0.), x_axis)
    local = step.add("IFCLOCALPLACEMENT", "$", local_frame)
    gray = step.add("IFCCOLOURRGB", "$", .6, .6, .6)

    def product(index, name, representation, geometry):
        rep = step.add("IFCSHAPEREPRESENTATION", context, "'Body'", f"'{representation}'", f"({geometry})")
        shape = step.add("IFCPRODUCTDEFINITIONSHAPE", "$", "$", f"({rep})")
        step.add("IFCBUILDINGELEMENTPROXY", f"'{index + 1:022d}'", "$", f"'{name}'", "$", "$", local, shape, "$", "$")

    style = step.add("IFCCURVESTYLE", "'Metric stroke'", "$", "IFCPOSITIVELENGTHMEASURE(56.)", gray, ".T.")
    strokes = []
    for center, y in [(x, 1.8) for _, x, _ in ZONES] + [(1.06, 2.6)]:
        ends = [step.point((center + dx) * 1000., y * 1000., 0.) for dx in (-.28, .28)]
        curve = step.add("IFCPOLYLINE", f"({','.join(ends)})")
        step.add("IFCSTYLEDITEM", curve, f"({style})", "$")
        strokes.append(curve)
    product(0, "Authored width", "GeometricCurveSet", step.add("IFCGEOMETRICCURVESET", f"({','.join(strokes)})"))

    index = 1
    for name, center, depths, alpha, bottom, top in highlight_panels():
        shader = step.add("IFCSURFACESTYLESHADING", gray, f"{1 - alpha:.7f}")
        style = step.add("IFCSURFACESTYLE", "$", ".BOTH.", f"({shader})")
        for z in depths:
            points = ",".join(f"({x * 1000.},{y * 1000.},{z * 1000.})" for x, y in
                              ((center - .4, bottom), (center + .4, bottom),
                               (center + .4, top), (center - .4, top)))
            listing = step.add("IFCCARTESIANPOINTLIST3D", f"({points})")
            mesh = step.add("IFCTRIANGULATEDFACESET", listing, "$", ".F.", "((1,2,3),(1,3,4))", "$")
            step.add("IFCSTYLEDITEM", mesh, f"({style})", "$")
            product(index, name, "Tessellation", mesh)
            index += 1
    header = ("ISO-10303-21;\nHEADER;\nFILE_DESCRIPTION(('ViewDefinition [DesignTransferView]'),'2;1');\n"
              f"FILE_NAME('{path.name}','2026-11-14T00:00:00',(''),(''),'Container','','');\n"
              "FILE_SCHEMA(('IFC4'));\nENDSEC;\nDATA;\n")
    path.write_text(header + "\n".join(step.elements) + "\nENDSEC;\nEND-ISO-10303-21;\n", encoding="ascii")


def highlight_sequence(path, kind, techniques, *, overview=False):
    blank = [5, 5]
    pixels_per_metre = HEIGHT / 2.4
    exposed = [round(WIDTH / 2 + 1.06 * pixels_per_metre),
               round(HEIGHT / 2 - (1.8 - 1.675) * pixels_per_metre)]
    events, samples = [{"frame": 1, "orthographic": True}], {}
    for index, technique in enumerate(techniques):
        base = 8 + index * 20
        if index:
            events.append({"frame": base - 3, "technique": technique})
        samples[base] = (technique, "off", -1)
        if kind == "ifcx":
            events.extend([{"frame": base + 3, "bimSelectedObject": 2},
                           {"frame": base + 7, "bimSelectedObject": 3}])
            samples[base + 4] = (technique, "points", 2)
            samples[base + 8] = (technique, "lines", 3)
        else:
            if not overview:
                events.append({"frame": base + 3, "viewportPointer": {"action": "hover", "pixel": exposed}})
                samples[base + 4] = (technique, "stroke-hover", -1)
                events.append({"frame": base + 7, "viewportPointer": {"action": "select", "pixel": exposed}})
            else:
                events.append({"frame": base + 7, "bimSelectedObject": 4})
            samples[base + 8] = (technique, "stroke", 4)
        events.append({"frame": base + 11, "viewportPointer": {"action": "select", "pixel": blank}})
        samples[base + 12] = (technique, "off-again", -1)
    save(path, {"schemaVersion": 1, "frames": max(samples), "sampleFrames": list(samples), "events": events})
    return samples


def sequence(path):
    save(path, {"schemaVersion": 1, "frames": max(SAMPLES), "sampleFrames": list(SAMPLES), "events": [
        {"frame": 1, "orthographic": True, "bimFloorPlan": {
            "enabled": False, "elevation": "source-elevation", "depthTest": False,
            "color": [1, 0, 1], "opacity": 1, "lineWidth": 5}},
        {"frame": 9, "technique": "forward-raster"},
        {"frame": 17, "technique": "deferred-raster"},
    ]})


def screen_stats(pixels, mode):
    import numpy as np

    if mode == "overview":
        # Overview's inner Lit tile in post_process.slang, expressed in pixel centers.
        pixels = pixels[LIT_TILE]

    def colors(region):
        red, green, blue = np.moveaxis(region.astype(np.int16), -1, 0)
        return dict(cyanPointPixels=int(np.count_nonzero(np.minimum(green - red, blue - red) > 22)),
                    amberCurvePixels=int(np.count_nonzero(np.minimum(red - green, red - blue) > 22)))

    # Native control rows are above y=2.4. Ordinary rows are below y=2.16.
    # With the locked orthographic camera, the upper quarter separates them.
    split = pixels.shape[0] // 4
    zones = {label: colors(section) for (label, _, _), section in
             zip(ZONES, np.array_split(pixels[split:], 3, axis=1), strict=True)}
    return dict(zones=zones, opaqueForeground=colors(pixels[:split]))


def inspect(stats, technique, mode):
    zones = stats["zones"]
    for channel, minimum in (("cyanPointPixels", 3 if mode == "overview" else 32),
                             ("amberCurvePixels", 5 if mode == "overview" else 64)):
        clear = zones["clear"][channel]
        require(clear >= minimum, f"{technique}/{mode}: exposed native geometry is missing: {stats}")
        if mode == "transparency":
            require(all(zone[channel] >= max(minimum, clear * .5) for zone in zones.values()),
                    f"{technique}: native-only geometry did not reach the OIT diagnostic: {stats}")
            continue
        require(zones["rear-roof"][channel] >= max(minimum, clear * .5),
                f"{technique}/{mode}: transparent geometry behind the native surface covered it: {stats}")
        # Two front alpha=250/255 layers transmit only (5/255)^2. Their
        # remaining primitive color contrast must fall below the PNG threshold.
        # Deferred Overview currently omits OIT production by design.
        if technique == "forward-raster" or mode == "lit":
            require(zones["front-roof"][channel] == 0,
                    f"{technique}/{mode}: native geometry incorrectly appeared above front transparency: {stats}")
        else:
            require(zones["front-roof"][channel] >= max(minimum, clear * .5),
                    f"{technique}/{mode}: Deferred Overview lost its native control: {stats}")
        require(stats["opaqueForeground"][channel] == 0,
                f"{technique}/{mode}: native geometry leaked through an opaque foreground: {stats}")


def read_capture(image, mode, technique, model):
    import numpy as np
    from PIL import Image

    state = json.loads(image.with_suffix(".telemetry.json").read_bytes())
    receipt = json.loads(Path(f"{image}.capture.json").read_bytes())
    display_mode, object_count = {"lit": (0, 11), "overview": (8, 11), "transparency": (6, 6)}[mode]
    require(state["technique"] == technique and state["validationEnabled"] and state["msaaSamples"] == 1
            and not state["taa"]["enabled"] and Path(state["scene"]["auxiliary"]).resolve() == model
            and state["scene"]["auxiliaryObjectCount"] == object_count
            and state["scene"]["import"]["status"] in ("Complete", "Unreported")
            and not state["oit"]["overflowed"], f"Unexpected geometry/render state: {image}")
    require((receipt["width"], receipt["height"], receipt["source"], receipt["displayMode"], receipt["includesUi"])
            == (WIDTH, HEIGHT, "viewport", display_mode, False)
            and not receipt["bim"]["floorPlan"]["requested"], f"Wrong capture route: {image}")
    projection = state["camera"]["unjitteredViewProjColumns"]
    require([column[3] for column in projection] == [0, 0, 0, 1], f"Expected orthographic camera: {image}")
    require(abs(projection[0][0] - .625) < 1e-5 and abs(projection[1][1] - 5 / 6) < 1e-5,
            f"Orthographic scale moved the measurement regions: {image}")
    with Image.open(image) as decoded:
        require(decoded.format == "PNG" and decoded.size == (WIDTH, HEIGHT), f"Invalid image: {image}")
        pixels = np.asarray(decoded.convert("RGB"))
    return pixels, state


def record(renderer, output, lane_name, model, spec, mode, technique, report, *,
           layer_dir, generate_only, extension="png"):
    lane = output / lane_name
    lane.mkdir()
    target, log = lane / f"depth-sorting.{extension}", lane / "renderer.log"
    command = [str(renderer), "--model", str(model), "--import-scale", "1", "--hidden", "--validation",
               "--width", str(WIDTH), "--height", str(HEIGHT), "--msaa", "1", "--no-taa", "--no-bloom",
               "--no-ddgi", "--no-ray-query", "--render-technique", technique, "--display-mode", mode,
               "--camera-position", "0", "1.675", "8", "--camera-target", "0", "1.675", "0", "--camera-fov", "45",
               "--tone-map", "neutral", "--exposure", "1", "--sky", "plain", "--sky-horizon", ".15", ".15", ".15",
               "--directional-intensity", "4", "--directional-direction", "-.2", "-.25", "-1",
               "--environment-intensity", ".5", "--bounce-intensity", ".25", "--fixed-dt", ".016666667",
               "--capture-sequence", str(spec), "--screenshot", str(target), "--screenshot-viewport"]
    save(lane / "command.json", command)
    row = dict(command=command, log=str(log), samples={})
    report["lanes"][lane_name] = row
    if generate_only:
        return target, row
    env = dict(os.environ, VK_LAYER_SETTINGS_PATH=str(output), VK_LOADER_DEBUG="layer")
    if layer_dir:
        env["VK_LAYER_PATH"] = str(layer_dir)
    with log.open("w", encoding="utf-8") as stream:
        process = subprocess.run(command, cwd=renderer.parent, stdout=stream, stderr=subprocess.STDOUT,
                                 timeout=180, creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0), env=env)
    row["exitCode"] = process.returncode
    messages = log.read_text(encoding="utf-8", errors="replace")
    require(process.returncode == 0 and not validation_log_error(messages)
            and 'Insert instance layer "VK_LAYER_KHRONOS_validation"' in messages, f"Vulkan capture failed: {log}")
    row["validationClean"] = True
    return target, row


def sample_path(target, frame, last):
    return target if frame == last else target.with_name(f"{target.stem}.frame-{frame:04d}{target.suffix}")


def run(renderer, output, model, spec, mode, report, *, layer_dir, generate_only):
    target, row = record(renderer, output, mode, model, spec, mode, "deferred-raster", report,
                         layer_dir=layer_dir, generate_only=generate_only)
    if generate_only:
        return
    images, projection = {}, None
    for frame, technique in SAMPLES.items():
        image = sample_path(target, frame, max(SAMPLES))
        pixels, state = read_capture(image, mode, technique, model)
        current = state["camera"]["unjitteredViewProjColumns"]
        if projection is None:
            projection = current
        require(current == projection, f"Camera changed during the engine switch: {image}")
        stats = screen_stats(pixels, mode)
        row["samples"][str(frame)] = dict(technique=technique, revision=state["build"]["revision"],
                                          image=str(image), projection=current, stats=stats,
                                          decodedSha256=hashlib.sha256(pixels.tobytes()).hexdigest())
        inspect(stats, technique, mode)
        images[frame] = pixels
        print(mode, frame, technique, stats, flush=True)
    import numpy as np

    np.testing.assert_array_equal(images[min(SAMPLES)], images[max(SAMPLES)],
                                  err_msg=f"{mode}: switching back to Deferred changed the fixed scene")
    row["deferredSwitchRestored"] = True


def highlight_stats(off, on, stage, mode):
    import numpy as np

    if mode == "overview":
        off, on = off[LIT_TILE], on[LIT_TILE]
    delta = on.astype(np.int16) - off.astype(np.int16)
    # Outline color is orange for selection and cyan for hover. Isolate the
    # change relative to this exact native primitive with its highlight off.
    signal = delta[:, :, 0] - delta[:, :, 2]
    if stage == "stroke-hover":
        signal = -signal

    def score(coverage, colors):
        # The authored stroke has no expandable perimeter. Its equal-depth
        # highlight may initially differ from the base fill by a few levels.
        visible = coverage > (2 if stage.startswith("stroke") else 12)
        return dict(changedPixels=int(np.count_nonzero(np.any(colors, axis=2))),
                    signalPixels=int(np.count_nonzero(visible)), signal=int(coverage[visible].sum()))

    split = signal.shape[0] // 4
    zones = {
        name: score(region, colors)
        for (name, _, _), region, colors in zip(ZONES, np.array_split(signal[split:], 3, axis=1),
                                                 np.array_split(delta[split:], 3, axis=1), strict=True)}
    zones["opaque-front"] = score(signal[:split], delta[:split])
    return zones


def inspect_highlights(images, row, mode):
    import numpy as np

    errors = []
    for (technique, stage), selected in images.items():
        if stage == "off":
            continue
        off = images[technique, "off"]
        if mode == "exr":
            delta = np.max(np.abs(off - selected), axis=2)
            result = dict(maxAbsoluteDelta=float(delta.max()),
                          changedPixels=int(np.count_nonzero(delta)),
                          fractionAboveNoiseFloor=float(np.count_nonzero(delta > .0005) / delta.size))
            row.setdefault("sceneLinear", {})[f"{technique}/{stage}"] = result
            # Concurrent insertion of unselected transparent surfaces can
            # differ by a few half-float steps even between two off frames.
            if result["maxAbsoluteDelta"] > .002 or result["fractionAboveNoiseFloor"] > .005:
                errors.append(f"{technique}/{stage} changed scene-linear radiance: {result}")
            continue
        if stage == "off-again":
            if not np.array_equal(off, selected):
                errors.append(f"{technique}/{mode}/{stage} changed the unselected capture")
            continue
        zones = highlight_stats(off, selected, stage, mode)
        row["highlight"][f"{technique}/{stage}"] = zones
        clear, rear, front = (zones[name]["signal"] for name in ("clear", "rear-roof", "front-roof"))
        minimum = 140 if mode == "overview" else 650
        if clear < minimum or zones["clear"]["signalPixels"] < (3 if mode == "overview" else 10):
            errors.append(f"{technique}/{mode}/{stage} lacks an exposed native highlight: {zones}")
        if rear < .65 * clear or rear < minimum:
            errors.append(f"{technique}/{mode}/{stage} highlight is hidden by rear transparency: {zones}")
        if front > .15 * clear:
            errors.append(f"{technique}/{mode}/{stage} highlight leaks through front transparency: {zones}")
        if zones["opaque-front"]["changedPixels"]:
            errors.append(f"{technique}/{mode}/{stage} changed pixels under opaque geometry: {zones}")
        if mode == "overview":
            diagnostics = row.setdefault("diagnostics", {})
            for name, left, right in (("Transparency", .026, .334), ("Revealage", .346, .654)):
                pane = np.s_[round(.51 * HEIGHT):round(.72 * HEIGHT), round((left + .004) * WIDTH):round((right - .004) * WIDTH)]
                brightness = off[pane].max(axis=2)
                roof_pixels = np.count_nonzero(brightness > 32 if name == "Transparency" else brightness < 223)
                changed = int(np.count_nonzero(np.any(off[pane] != selected[pane], axis=2)))
                diagnostics.setdefault(name, {})[stage] = dict(roofPixels=int(roof_pixels), changedPixels=changed)
                if roof_pixels <= 100:
                    errors.append(f"{technique}/{stage} {name} diagnostic lacks transparent roof coverage")
                if changed:
                    errors.append(f"{technique}/{stage} changed the {name} diagnostic")
        print(mode, technique, stage, zones, flush=True)
    return errors


def run_highlights(renderer, output, model, kind, mode, report, *, layer_dir, generate_only):
    import numpy as np
    from PIL import Image

    techniques = ("forward-raster",) if mode == "overview" else ("deferred-raster", "forward-raster")
    lane = f"highlight-{kind}-{mode}"
    spec = output / f"{lane}.json"
    samples = highlight_sequence(spec, kind, techniques, overview=mode == "overview")
    target, row = record(renderer, output, lane, model, spec, "lit" if mode == "exr" else mode, techniques[0],
                         report, layer_dir=layer_dir, generate_only=generate_only,
                         extension="exr" if mode == "exr" else "png")
    row.update(highlight={}, modelSha256=hashlib.sha256(model.read_bytes()).hexdigest())
    if generate_only:
        return []
    images, projection = {}, None
    for frame, (technique, stage, expected_index) in samples.items():
        image = sample_path(target, frame, max(samples))
        state = json.loads(image.with_suffix(".telemetry.json").read_bytes())
        capture = json.loads(Path(f"{image}.capture.json").read_bytes())
        active_passes = {item["name"] for item in state["passes"] if item["active"]}
        current = state["camera"]["unjitteredViewProjColumns"]
        if projection is None:
            projection = current
        require(state["technique"] == technique and state["validationEnabled"] and state["msaaSamples"] == 1
                and not state["taa"]["enabled"]
                and current == projection and [column[3] for column in current] == [0, 0, 0, 1]
                and abs(current[0][0] - .625) < 1e-5 and abs(current[1][1] - 5 / 6) < 1e-5
                and not state["oit"]["overflowed"] and {"OitClear", "OitResolve"} <= active_passes,
                f"Unexpected render configuration: {image}")
        scene = state["scene"]
        require(Path(scene["auxiliary"]).resolve() == model
                and scene["auxiliaryObjectCount"] == (6 if kind == "ifcx" else 12), f"Unexpected BIM import: {image}")
        if kind == "ifc":
            receipt = scene["import"]
            require(receipt["status"] == "Complete" and receipt["importedProducts"] == 7
                    and not receipt["representationWarnings"], f"Incomplete authored stroke fixture: {image}")
        selected_index = state["gui"]["selectedBimObjectIndex"]
        require(selected_index == expected_index,
                f"{technique}/{stage}: expected BIM object {expected_index}, selected {selected_index}: {image}")
        require((capture["width"], capture["height"], capture["includesUi"]) == (WIDTH, HEIGHT, False),
                f"Wrong capture size or UI state: {image}")
        if mode == "exr":
            import cv2

            require(capture["source"] == "raster-scene-composite" and capture["colorSpace"] == "linear-sRGB"
                    and not capture["exposureApplied"] and not capture["toneMappingApplied"],
                    f"Unexpected scene-linear capture: {image}")
            pixels = cv2.imdecode(np.frombuffer(image.read_bytes(), dtype=np.uint8), cv2.IMREAD_UNCHANGED)
            require(pixels is not None and pixels.shape == (HEIGHT, WIDTH, 3) and pixels.dtype == np.float32
                    and np.isfinite(pixels).all(), f"Invalid scene-linear radiance: {image}")
        else:
            require(capture["source"] == "viewport" and capture["displayMode"] == (8 if mode == "overview" else 0),
                    f"Unexpected viewport capture: {image}")
            with Image.open(image) as decoded:
                require(decoded.format == "PNG" and decoded.size == (WIDTH, HEIGHT), f"Invalid viewport image: {image}")
                pixels = np.asarray(decoded.convert("RGB"))
        images[technique, stage] = pixels
        row["samples"][str(frame)] = dict(technique=technique, stage=stage, image=str(image),
                                          selectedBimObjectIndex=selected_index, revision=state["build"]["revision"],
                                          sha256=hashlib.sha256(pixels.tobytes()).hexdigest())
    return inspect_highlights(images, row, mode)


def main():
    os.environ.setdefault("OPENCV_IO_ENABLE_OPENEXR", "1")
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--renderer", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--layer-dir", type=Path)
    parser.add_argument("--generate-only", action="store_true")
    parser.add_argument("--highlight-only", action="store_true")
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
    model, spec = output / "native-primitives-with-depth-ordered-roofs.ifcx", output / "events.json"
    native_only_model = output / "native-primitives-only.ifcx"
    fixture(model)
    fixture(native_only_model, native_only=True)
    sequence(spec)
    (output / "vk_layer_settings.txt").write_text("khronos_validation.validate_sync = true\n", encoding="utf-8")
    report = dict(passed=False, generatedOnly=args.generate_only, lanes={}, primitiveZ=0,
                  roofZByScreenZone={label: depths for label, _, depths in ZONES}, opaqueForegroundZ=2.)
    if not args.generate_only:
        report["rendererSha256"] = hashlib.sha256(renderer.read_bytes()).hexdigest()
    report["fixtureSha256"] = hashlib.sha256(model.read_bytes()).hexdigest()
    report["nativeOnlyFixtureSha256"] = hashlib.sha256(native_only_model.read_bytes()).hexdigest()
    report["sequenceSha256"] = hashlib.sha256(spec.read_bytes()).hexdigest()
    try:
        if not args.highlight_only:
            for mode in ("lit", "overview"):
                run(renderer, output, model, spec, mode, report, layer_dir=args.layer_dir,
                    generate_only=args.generate_only)
            run(renderer, output, native_only_model, spec, "transparency", report, layer_dir=args.layer_dir,
                generate_only=args.generate_only)
        errors = []
        for kind, write_fixture in (("ifcx", highlight_ifcx), ("ifc", highlight_ifc)):
            highlight_model = output / f"native-highlight-depth-order.{kind}"
            write_fixture(highlight_model)
            for mode in ("lit", "overview", "exr"):
                errors.extend(run_highlights(renderer, output, highlight_model, kind, mode, report,
                                             layer_dir=args.layer_dir, generate_only=args.generate_only))
        report["highlightFailures"] = errors
        require(not errors, "\n".join(errors))
        if not args.generate_only:
            require(report["rendererSha256"] == hashlib.sha256(renderer.read_bytes()).hexdigest(), "Renderer changed")
            report["passed"] = True
    finally:
        save(output / "results.json", report)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
