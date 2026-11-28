"""Check native BIM points and curves against front, rear and opaque geometry."""

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


def fixture(path):
    nodes = []
    for label, center, depths in ZONES:
        nodes.extend(primitive_nodes(label, center))
        if depths:
            nodes.append(panel_node(f"{label}-panels", center, depths, 250 / 255))
    # A separate upper strip guards against making native primitives ignore all
    # depth tests. The main three zones and their original camera stay unchanged.
    nodes.extend(primitive_nodes("opaque-front", 1.06, point_rows=(2.5, 2.6), curve_rows=(2.72,)))
    nodes.append(panel_node("opaque-front-panel", 1.06, (2.,), 1., bottom=2.4, top=2.82))
    children = {node["path"]: node["path"] for node in nodes}
    save(path, {"data": [{"path": "root", "children": children}, *nodes]})


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
        pixels = pixels[round(.046 * HEIGHT):round(.264 * HEIGHT), round(.026 * WIDTH):round(.334 * WIDTH)]

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
    require(state["technique"] == technique and state["validationEnabled"] and state["msaaSamples"] == 1
            and not state["taa"]["enabled"] and Path(state["scene"]["auxiliary"]).resolve() == model
            and state["scene"]["auxiliaryObjectCount"] == 11
            and state["scene"]["import"]["status"] in ("Complete", "Unreported")
            and not state["oit"]["overflowed"], f"Unexpected geometry/render state: {image}")
    require((receipt["width"], receipt["height"], receipt["source"], receipt["displayMode"], receipt["includesUi"])
            == (WIDTH, HEIGHT, "viewport", 8 if mode == "overview" else 0, False)
            and not receipt["bim"]["floorPlan"]["requested"], f"Wrong capture route: {image}")
    projection = state["camera"]["unjitteredViewProjColumns"]
    require([column[3] for column in projection] == [0, 0, 0, 1], f"Expected orthographic camera: {image}")
    require(abs(projection[0][0] - .625) < 1e-5 and abs(projection[1][1] - 5 / 6) < 1e-5,
            f"Orthographic scale moved the measurement regions: {image}")
    with Image.open(image) as decoded:
        require(decoded.format == "PNG" and decoded.size == (WIDTH, HEIGHT), f"Invalid image: {image}")
        pixels = np.asarray(decoded.convert("RGB"))
    return pixels, state


def run(renderer, output, model, spec, mode, report, *, layer_dir, generate_only):
    lane = output / mode
    lane.mkdir()
    target, log = lane / "depth-sorting.png", lane / "renderer.log"
    command = [str(renderer), "--model", str(model), "--import-scale", "1", "--hidden", "--validation",
               "--width", str(WIDTH), "--height", str(HEIGHT), "--msaa", "1", "--no-taa", "--no-bloom",
               "--no-ddgi", "--no-ray-query", "--render-technique", "deferred-raster", "--display-mode", mode,
               "--camera-position", "0", "1.675", "8", "--camera-target", "0", "1.675", "0", "--camera-fov", "45",
               "--tone-map", "neutral", "--exposure", "1", "--sky", "plain", "--sky-horizon", ".15", ".15", ".15",
               "--directional-intensity", "4", "--directional-direction", "-.2", "-.25", "-1",
               "--environment-intensity", ".5", "--bounce-intensity", ".25", "--fixed-dt", ".016666667",
               "--capture-sequence", str(spec), "--screenshot", str(target), "--screenshot-viewport"]
    save(lane / "command.json", command)
    row = dict(command=command, log=str(log), samples={})
    report["lanes"][mode] = row
    if generate_only:
        return
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
    images, projection = {}, None
    for frame, technique in SAMPLES.items():
        image = target if frame == max(SAMPLES) else target.with_name(f"{target.stem}.frame-{frame:04d}.png")
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


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--renderer", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--layer-dir", type=Path)
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
    model, spec = output / "native-primitives-with-depth-ordered-roofs.ifcx", output / "events.json"
    fixture(model)
    sequence(spec)
    (output / "vk_layer_settings.txt").write_text("khronos_validation.validate_sync = true\n", encoding="utf-8")
    report = dict(passed=False, generatedOnly=args.generate_only, lanes={}, primitiveZ=0,
                  roofZByScreenZone={label: depths for label, _, depths in ZONES}, opaqueForegroundZ=2.)
    if not args.generate_only:
        report["rendererSha256"] = hashlib.sha256(renderer.read_bytes()).hexdigest()
    report["fixtureSha256"] = hashlib.sha256(model.read_bytes()).hexdigest()
    report["sequenceSha256"] = hashlib.sha256(spec.read_bytes()).hexdigest()
    try:
        for mode in ("lit", "overview"):
            run(renderer, output, model, spec, mode, report, layer_dir=args.layer_dir,
                generate_only=args.generate_only)
        if not args.generate_only:
            require(report["rendererSha256"] == hashlib.sha256(renderer.read_bytes()).hexdigest(), "Renderer changed")
            report["passed"] = True
    finally:
        save(output / "results.json", report)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
