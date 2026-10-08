"""Opt-in runtime ray visibility against independent segment/AABB integration."""
from __future__ import annotations
import argparse
import base64
import copy
import io
import json
import os
from pathlib import Path
import subprocess
import struct

import numpy as np
from PIL import Image
from area_shadow_regression import floor_points, hdr, reference


def blocked(points, emitter, translation=0):
    lower = np.array([-.24 + translation, 0, -.15])
    upper = np.array([.24 + translation, .72, .36])
    direction = np.asarray(emitter) - points
    with np.errstate(divide="ignore", invalid="ignore"):
        a, b = (lower - points) / direction, (upper - points) / direction
    enter = np.maximum(np.minimum(a, b).max(axis=1), 1e-6)
    leave = np.minimum(np.maximum(a, b).min(axis=1), 1 - 1e-6)
    return enter <= leave


def receiver_artifact_fixture(path, grazing=False, blocker=True,
                             close_parallel=False, same_instance=False):
    """Opaque colour-separated planes make each receiver's lighting measurable."""
    blob, views, accessors, meshes = bytearray(), [], [], []

    def attribute(values):
        offset = len(blob)
        flat = [component for value in values for component in value]
        blob.extend(struct.pack("<" + "f" * len(flat), *flat))
        views.append(dict(buffer=0, byteOffset=offset, byteLength=len(blob) - offset))
        accessors.append(dict(bufferView=len(views) - 1, componentType=5126,
            count=len(values), type="VEC3", min=np.min(values, axis=0).tolist(),
            max=np.max(values, axis=0).tolist()))
        return len(accessors) - 1

    def quad(name, positions, normal, material):
        vertices = [positions[i] for i in (0, 1, 2, 0, 2, 3)]
        meshes.append(dict(name=name, primitives=[dict(mode=4, material=material,
            attributes=dict(POSITION=attribute(vertices), NORMAL=attribute([normal] * 6)))]))

    if grazing:
        # At this camera distance the projected width is 0.57 pixels, centred
        # on column 128. The light faces the receiver but is nearly tangent.
        tangent = np.array([.1, 0, np.sqrt(.99)])
        normal = [-np.sqrt(.99), 0, .1]
        center, half = np.array([.00142, 0, 0]), .008
        foreground = [(center + side * half * tangent + [0, y, 0]).tolist()
                      for side, y in ((-1, -2), (1, -2), (1, 2), (-1, 2))]
        quad("Thin grazing receiver", foreground, normal, 0)
        quad("Background behind the receiver",
             [[-3, -2, -.1], [3, -2, -.1], [3, 2, -.1], [-3, 2, -.1]], [0, 0, 1], 1)
    elif close_parallel:
        # Three millimetres separate two equally tilted receivers. Their depth
        # separation fits a half-pixel slope bound, so plane/instance identity
        # must discriminate them rather than widening the depth tolerance.
        normal = [-1 / np.sqrt(5), 0, 2 / np.sqrt(5)]
        foreground = [[x, y, .5 * x] for x, y in
                      ((-3, -2), (-.17, -2), (.17, 2), (-3, 2))]
        background = [[x, y, .5 * x - .003] for x, y in
                      ((-3, -2), (3, -2), (3, 2), (-3, 2))]
        if same_instance:
            # Vertex colours distinguish two planes in one primitive/BLAS
            # instance: instance identity alone cannot make this case pass.
            vertices = [positions[i] for positions in (foreground, background)
                        for i in (0, 1, 2, 0, 2, 3)]
            colors = [[1, 0, 0]] * 6 + [[0, 1, 0]] * 6
            meshes.append(dict(name="Two receivers in one instance", primitives=[dict(
                mode=4, material=0, attributes=dict(POSITION=attribute(vertices),
                    NORMAL=attribute([normal] * 12), COLOR_0=attribute(colors)))]))
        else:
            quad("Close red receiver", foreground, normal, 0)
            quad("Close green receiver", background, normal, 1)
        if blocker:
            # L=(1,0,.5005): the background reaches this off-camera plane
            # after three world units in X, while foreground rays leave it.
            quad("Off-axis close-receiver blocker",
                 [[x, y, .5 * x - .0015] for x, y in
                  ((2, -3), (4, -3), (4, 3), (2, 3))], normal, 2)
    else:
        foreground = [[-3, -2, 0], [-.17, -2, 0], [.17, 2, 0], [-3, 2, 0]]
        quad("Lit red foreground", foreground, [0, 0, 1], 0)
        quad("Shadowed green background",
             [[-3, -2, -1], [3, -2, -1], [3, 2, -1], [-3, 2, -1]], [0, 0, 1], 1)
        if blocker:
            # The +X/+Z shadow ray reaches x in [1,2] from the background,
            # while foreground rays pass to the left of this off-axis plane.
            quad("Off-axis blocker", [[1, -3, .5], [2, -3, .5], [2, 3, .5], [1, 3, .5]],
                 [0, 0, 1], 2)
    materials = [dict(name=name, doubleSided=True, pbrMetallicRoughness=dict(
        baseColorFactor=[*color, 1], metallicFactor=1, roughnessFactor=1))
        for name, color in (("Red receiver", [1, 0, 0]), ("Green receiver", [0, 1, 0]),
                            ("Black blocker", [0, 0, 0]))]
    if close_parallel and same_instance:
        materials[0]["pbrMetallicRoughness"]["baseColorFactor"] = [1, 1, 1, 1]
    doc = dict(asset=dict(version="2.0"), buffers=[dict(byteLength=len(blob),
        uri="data:application/octet-stream;base64," + base64.b64encode(blob).decode())],
        bufferViews=views, accessors=accessors, meshes=meshes, materials=materials,
        nodes=[dict(mesh=i) for i in range(len(meshes))],
        scenes=[dict(nodes=list(range(len(meshes))))], scene=0)
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(doc), encoding="utf-8")
    return foreground


def projected_quad_mask(vertices, telemetry):
    width, height = telemetry["resolution"]
    matrix = np.array(telemetry["camera"]["unjitteredViewProjColumns"]).T
    clip = np.column_stack((vertices, np.ones(4))) @ matrix.T
    ndc = clip[:, :2] / clip[:, 3:4]
    # Scene viewport has negative height: positive NDC Y maps toward the top.
    polygon = (ndc * [.5, -.5] + .5) * [width, height]
    y, x = np.mgrid[:height, :width]
    pixels = np.stack((x + .5, y + .5), axis=-1)
    sides = []
    for start, end in zip(polygon, np.roll(polygon, -1, axis=0)):
        edge, delta = end - start, pixels - start
        sides.append(edge[0] * delta[:, :, 1] - edge[1] * delta[:, :, 0])
    sides = np.array(sides)
    return np.all(sides >= 0, axis=0) | np.all(sides <= 0, axis=0)


def mixed_receiver_metrics(shadowed, clear):
    # Both channels must have measurable coverage in the clear 4x resolve.
    # Pure-metal red/green materials keep their direct radiance in one channel.
    mixed = (clear[:, :, 0] > .1) & (clear[:, :, 1] > .1)
    mixed[:50] = False
    mixed[206:] = False
    assert mixed.sum() >= 80, ("fixture lacks mixed MSAA receivers", mixed.sum())
    red = shadowed[:, :, 0][mixed] / clear[:, :, 0][mixed]
    green = shadowed[:, :, 1][mixed] / clear[:, :, 1][mixed]
    return dict(mixedPixels=int(mixed.sum()), foregroundP05=float(np.percentile(red, 5)),
                foregroundP95Error=float(np.percentile(abs(red - 1), 95)),
                backgroundP95=float(np.percentile(green, 95)))


def grazing_receiver_metrics(raster, ray, visibility, vertices, telemetry):
    mask = projected_quad_mask(vertices, telemetry)
    mask[:50] = False
    mask[206:] = False
    assert mask.sum() == 156 and np.max(mask.sum(axis=1)) == 1, "receiver must be one pixel wide"
    assert np.min(raster[:, :, 0][mask]) > .1, "unblocked raster receiver is not lit"
    assert np.max(raster[:, :, 1][mask]) < 1e-5, "mask includes background"
    direct_visibility = visibility.mean(axis=2)[mask]
    lighting_ratio = ray[:, :, 0][mask] / raster[:, :, 0][mask]
    return dict(receiverPixels=int(mask.sum()), visibilityP05=float(np.percentile(direct_visibility, 5)),
                lightingP05=float(np.percentile(lighting_ratio, 5)),
                lightingP95Error=float(np.percentile(abs(lighting_ratio - 1), 95)))


def hard_msaa_receiver_fixture(path, kind="directional", slope=0.0, blocker=True):
    """A subpixel off-axis blocker shadows pixel centres on one covered plane."""
    blob, views, accessors, meshes = bytearray(), [], [], []

    def attribute(values):
        offset = len(blob)
        flat = [component for value in values for component in value]
        blob.extend(struct.pack("<" + "f" * len(flat), *flat))
        views.append(dict(buffer=0, byteOffset=offset, byteLength=len(blob) - offset))
        accessors.append(dict(bufferView=len(views) - 1, componentType=5126,
            count=len(values), type="VEC3", min=np.min(values, axis=0).tolist(),
            max=np.max(values, axis=0).tolist()))
        return len(accessors) - 1

    def quad(positions, normal, material):
        vertices = [positions[i] for i in (0, 1, 2, 0, 2, 3)]
        meshes.append(dict(primitives=[dict(mode=4, material=material,
            attributes=dict(POSITION=attribute(vertices), NORMAL=attribute([normal] * 6)))]))

    normal = np.array([-slope, 0, 1]) / np.sqrt(1 + slope * slope)
    quad([[x, y, slope * x] for x, y in ((-3, -2), (3, -2), (3, 2), (-3, 2))], normal, 0)
    if blocker:
        # Row 128 shades at world Y=-0.007109 for the 256px/40-degree
        # camera. This strip blocks that centre, but misses all standard MSAA
        # MAX sample origins on both the flat and shallow positive-X plane.
        quad([[2.5, -.008, 1], [3.5, -.008, 1], [3.5, -.006, 1], [2.5, -.006, 1]],
             [0, 0, 1], 1)
    materials = [dict(doubleSided=True, pbrMetallicRoughness=dict(
        baseColorFactor=[value, value, value, 1], metallicFactor=1, roughnessFactor=1))
        for value in (1, 0)]
    doc = dict(asset=dict(version="2.0"), buffers=[dict(byteLength=len(blob),
        uri="data:application/octet-stream;base64," + base64.b64encode(blob).decode())],
        bufferViews=views, accessors=accessors, meshes=meshes, materials=materials,
        nodes=[dict(mesh=i) for i in range(len(meshes))],
        scenes=[dict(nodes=list(range(len(meshes))))], scene=0)
    if kind != "directional":
        light = dict(type="point", intensity=5e8, color=[1, 1, 1])
        if kind == "area":
            # Hard mode queries the emitter centre. Keep sufficient solid
            # angle for the existing contribution-weight cutoff at this range.
            light["extras"] = dict(areaLight=dict(shape="rect", width=1, height=1))
        doc["extensionsUsed"] = ["KHR_lights_punctual"]
        doc["extensions"] = dict(KHR_lights_punctual=dict(lights=[light]))
        yaw = np.arctan2(3000, 1000)
        doc["nodes"].append(dict(name=f"Distant hard {kind} emitter", translation=[3000, 0, 1000],
            rotation=[0, float(np.sin(yaw / 2)), 0, float(np.cos(yaw / 2))],
            extensions=dict(KHR_lights_punctual=dict(light=0))))
        doc["scenes"][0]["nodes"].append(len(doc["nodes"]) - 1)
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(doc), encoding="utf-8")


def hard_msaa_receiver_metrics(shadowed, clear, telemetry, kind, slope):
    # Intersect camera rays with z=slope*x, then intersect the receiver-to-light
    # ray with the independent blocker plane z=1. No renderer depth or query
    # output participates in this reference, including MSAA sample selection.
    width, height = telemetry["resolution"]
    matrix = np.array(telemetry["camera"]["unjitteredViewProjColumns"]).T
    inverse = np.linalg.inv(matrix)
    y, x = np.mgrid[126:132, 112:144]
    uv = np.column_stack(((x.ravel() + .5) / width, (y.ravel() + .5) / height))
    # Negative-height scene viewport: framebuffer +Y is opposite NDC +Y.
    ndc = uv * [2, -2] + [-1, 1]
    near = np.column_stack((ndc, np.ones(len(ndc)), np.ones(len(ndc)))) @ inverse.T
    far = np.column_stack((ndc, np.zeros(len(ndc)), np.ones(len(ndc)))) @ inverse.T
    near, far = near[:, :3] / near[:, 3:4], far[:, :3] / far[:, 3:4]
    direction = far - near
    distance = -(near[:, 2] - slope * near[:, 0]) / (direction[:, 2] - slope * direction[:, 0])
    points = near + direction * distance[:, None]
    light_direction = (np.broadcast_to([3, 0, 1], points.shape) if kind == "directional"
                       else np.array([3000, 0, 1000]) - points)
    hit_distance = (1 - points[:, 2]) / light_direction[:, 2]
    hit = points + light_direction * hit_distance[:, None]
    expected_blocked = ((hit[:, 0] > 2.5) & (hit[:, 0] < 3.5) &
                        (hit[:, 1] > -.008) & (hit[:, 1] < -.006) & (hit_distance > 0))
    if kind != "directional":
        expected_blocked &= hit_distance < 1
    assert expected_blocked.sum() == 32 and (~expected_blocked).sum() >= 32, "fixture lacks the blocked centre strip"
    clear_values = clear[y, x].mean(axis=2).ravel()
    assert np.min(clear_values) > .1, "clear hard-shadow receiver is not lit"
    visibility = shadowed[y, x].mean(axis=2).ravel() / clear_values
    dark, lit = visibility[expected_blocked], visibility[~expected_blocked]
    return dict(blockedPixels=int(len(dark)), litPixels=int(len(lit)),
        blockedP95=float(np.percentile(dark, 95)), litP95Error=float(np.percentile(abs(lit - 1), 95)),
        incorrectBlockedPixels=int(np.count_nonzero(dark > .08)),
        incorrectLitPixels=int(np.count_nonzero(abs(lit - 1) > .05)))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--exe", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--case", choices=("all", "quality", "coverage", "motion", "lifecycle", "budget", "artifacts", "msaa", "msaa-motion", "msaa-hard"), default="all")
    args = parser.parse_args()
    if os.environ.get("CONTAINER_RUN_GPU_RAY_SHADOW") != "1":
        print("SKIP: set CONTAINER_RUN_GPU_RAY_SHADOW=1")
        return 77
    exe, output = args.exe.resolve(), args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    (output / "vk_layer_settings.txt").write_text("khronos_validation.validate_sync = true\n")
    source = json.loads((exe.parent / "models/validation/cornell_box_local_light.gltf").read_text())
    records = []
    oracle_cache = {}

    def oracle(points, size, height, disk=False, resolution=256, source_x=0):
        key = (points.shape, points.tobytes(), size, height, disk, resolution, source_x)
        if key not in oracle_cache:
            oracle_cache[key] = reference(points, size, height, disk, resolution, source_x)
        return oracle_cache[key]

    def fixture(name, kind="rect", blocker=True, size=(.8, .8)):
        doc = copy.deepcopy(source)
        primitives = doc["meshes"][0]["primitives"]
        doc["meshes"][0]["primitives"] = [primitives[0]]
        doc["meshes"].append({"primitives": primitives[5:]})
        if blocker:
            doc["nodes"].append({"name": "Shadow blocker", "mesh": 1})
            doc["scenes"][0]["nodes"].append(len(doc["nodes"]) - 1)
        light = doc["extensions"]["KHR_lights_punctual"]["lights"][0]
        if kind == "point":
            light.pop("extras", None)
        else:
            light["extras"]["areaLight"].update(shape="disk" if kind == "disk" else "rect",
                width=size[0], height=size[1], radius=size[0] * .5)
        path = output / name / "models/validation/cornell_box_local_light.gltf"
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(json.dumps(doc))
        return path

    def capture(name, model, technique, mode, sequence=None, extra=()):
        screenshot = output / (name + ".png")
        for stale in [screenshot, *output.glob(name + ".frame-*.png")]:
            stale.unlink(missing_ok=True)
        command = [str(exe), "--model", str(model), "--hidden", "--no-ui", "--validation",
            "--no-bloom", "--msaa", "1", "--width", "960", "--height", "540",
            "--display-mode", "lit", "--camera-position", "0", "1.05", "4.1",
            "--camera-target", "0", ".95", "0", "--camera-fov", "36",
            "--environment-intensity", "0", "--directional-intensity", "0",
            "--exposure", ".03", "--render-technique", technique, "--no-taa",
            "--ray-shadows", mode, "--ray-shadow-samples", "32",
            "--warmup-frames", "48", "--capture-frame", "49", "--screenshot", str(screenshot), *extra]
        if sequence:
            path = output / (name + "-sequence.json")
            path.write_text(json.dumps(sequence))
            command += ["--capture-sequence", str(path)]
        log = output / (name + ".log")
        with log.open("w") as stream:
            run = subprocess.run(command, cwd=exe.parent,
                env=dict(os.environ, VK_LAYER_SETTINGS_PATH=str(output)),
                stdout=stream, stderr=subprocess.STDOUT, timeout=180)
        text = log.read_text(errors="replace")
        assert run.returncode == 0 and "VUID-" not in text and "SYNC-HAZARD" not in text, log
        telemetry = json.loads(screenshot.with_suffix(".telemetry.json").read_text())
        if mode != "raster" and "--no-ray-query" not in extra:
            if not telemetry["rayShadows"]["supported"]:
                print("SKIP: selected device lacks ray queries")
                raise SystemExit(77)
            assert telemetry["rayShadows"]["active"], telemetry["rayShadows"]
            assert telemetry["rayShadows"]["build"]["instances"] > 0
        return screenshot, telemetry

    if args.case in ("all", "artifacts", "msaa", "msaa-hard"):
        # Flat depth does not identify one MSAA sample: all samples tie. A
        # shallow slope can also fit the numerical centre/resolved depth test.
        # Exact hard visibility must still be evaluated at the raster shading
        # centre, rather than borrowing a laterally displaced compute query.
        hard_output = output / "hard-msaa-receivers"
        camera_options = ("--width", "256", "--height", "256", "--camera-position", "0", "0", "5",
            "--camera-target", "0", "0", "0", "--camera-fov", "40", "--directional-color", "1", "1", "1",
            "--directional-direction", "-3", "0", "-1", "--exposure", "1", "--warmup-frames", "8", "--capture-frame", "9")
        for kind in ("directional", "point", "area"):
            for slope in (0.0, 1e-4):
                receiver = "flat" if slope == 0 else "shallow"
                models = {}
                for label, with_blocker in (("blocked", True), ("clear", False)):
                    model = hard_output / f"{receiver}-{kind}-{label}.gltf"
                    hard_msaa_receiver_fixture(model, kind, slope, with_blocker)
                    models[label] = model
                for samples in (1, 2, 4, 8):
                    name = f"hard-msaa{samples}-{receiver}-{kind}"
                    extra = (*camera_options, "--msaa", str(samples), "--directional-intensity",
                             "50" if kind == "directional" else "0")
                    clear_image, info = capture(name + "-clear", models["clear"], "forward-raster", "hard", extra=extra)
                    if info["msaaSamples"] != samples:
                        assert samples in (2, 8), ("required MSAA mode unavailable", samples, info["msaaSamples"])
                        records.append(dict(case="msaa-hard", name=name, samples=samples,
                            skipped="unsupported sample count"))
                        continue
                    assert not info["taa"]["enabled"] and info["rayShadows"]["mode"] == 1
                    if kind != "directional":
                        assert info["lighting"]["pointLightCount"] == (1 if kind == "point" else 0)
                        assert info["lighting"]["areaLightCount"] == (1 if kind == "area" else 0)
                        assert info["rayShadows"]["localLightBudget"] >= 1
                    shadowed_image, blocked_info = capture(name + "-blocked", models["blocked"],
                        "forward-raster", "hard", extra=extra)
                    assert blocked_info["msaaSamples"] == samples
                    metrics = hard_msaa_receiver_metrics(hdr(shadowed_image), hdr(clear_image), info, kind, slope)
                    assert metrics["incorrectBlockedPixels"] == 0 and metrics["incorrectLitPixels"] == 0, (name, metrics)
                    records.append(dict(case="msaa-hard", name=name, samples=samples,
                        light=kind, receiverSlope=slope, **metrics))
                    print(name, metrics, flush=True)

    if args.case in ("all", "artifacts", "msaa"):
        artifact_output = output / "receiver-artifacts"
        edge_model, clear_model = artifact_output / "msaa-edge.gltf", artifact_output / "msaa-clear.gltf"
        receiver_artifact_fixture(edge_model)
        receiver_artifact_fixture(clear_model, blocker=False)
        camera_options = ("--width", "256", "--height", "256", "--camera-position", "0", "0", "5",
            "--camera-target", "0", "0", "0", "--camera-fov", "40", "--directional-intensity", "20",
            "--directional-color", "1", "1", "1", "--directional-direction", "-1", "0", "-1",
            "--exposure", "1", "--warmup-frames", "8", "--capture-frame", "9")
        captures = {}
        for samples in (1, 4):
            for label, model in (("blocked", edge_model), ("clear", clear_model)):
                image, telemetry = capture(f"artifact-msaa{samples}-{label}", model,
                    "forward-raster", "hard", extra=(*camera_options, "--msaa", str(samples)))
                assert telemetry["msaaSamples"] == samples
                assert not telemetry["taa"]["enabled"]
                captures[samples, label] = hdr(image), telemetry
        shadowed, clear = captures[4, "blocked"][0], captures[4, "clear"][0]
        metrics = mixed_receiver_metrics(shadowed, clear)
        # The foreground retains ray visibility. Background samples resolve a
        # different receiver and must use its per-fragment raster visibility.
        assert metrics["foregroundP05"] > .95 and metrics["foregroundP95Error"] < .05, metrics
        assert metrics["backgroundP95"] < .12, metrics

        # Fully covered interiors still use the ray path at 1x and 4x. The
        # blocker is analytically between the background and directional light
        # for world X in (-.5,.5), independent of any visibility-buffer code.
        interior_results = []
        for samples in (1, 4):
            shadowed, telemetry = captures[samples, "blocked"]
            clear = captures[samples, "clear"][0]
            matrix = np.array(telemetry["camera"]["unjitteredViewProjColumns"]).T
            scale = matrix[0, 0]
            world_x = (2 * (np.arange(256) + .5) / 256 - 1) * 6 / scale
            central = (abs(world_x) < .35)[None, :] & (np.arange(256)[:, None] >= 50) & (np.arange(256)[:, None] < 206)
            foreground = central & (clear[:, :, 0] > .1) & (clear[:, :, 1] < 1e-5)
            background = central & (clear[:, :, 1] > .1) & (clear[:, :, 0] < 1e-5)
            assert foreground.sum() > 100 and background.sum() > 100
            lit = shadowed[:, :, 0][foreground] / clear[:, :, 0][foreground]
            dark = shadowed[:, :, 1][background] / clear[:, :, 1][background]
            assert np.percentile(abs(lit - 1), 95) < .05 and np.percentile(dark, 95) < .08
            interior_results.append(dict(samples=samples, foregroundPixels=int(foreground.sum()),
                backgroundPixels=int(background.sum()), foregroundP95Error=float(np.percentile(abs(lit - 1), 95)),
                backgroundP95=float(np.percentile(dark, 95))))
        records.append(dict(case="artifacts", name="forward-msaa-mixed-receivers", **metrics,
                            interiors=interior_results))
        print("forward MSAA mixed receivers", metrics, flush=True)

        for same_instance in (False, True):
            close_captures = {}
            for label, with_blocker in (("blocked", True), ("clear", False)):
                model = artifact_output / f"msaa-close-instance{int(same_instance)}-{label}.gltf"
                receiver_artifact_fixture(model, blocker=with_blocker,
                    close_parallel=True, same_instance=same_instance)
                doc = json.loads(model.read_text())
                # A negligible renderable area selects the soft receiver-reuse
                # path while the grazing directional light isolates visibility.
                doc["extensionsUsed"] = ["KHR_lights_punctual"]
                doc["extensions"] = dict(KHR_lights_punctual=dict(lights=[dict(
                    type="point", intensity=1e-5, color=[1, 1, 1], range=3.2,
                    extras=dict(areaLight=dict(shape="rect", width=.8, height=.8)))]))
                doc["nodes"].append(dict(name="Receiver verification area",
                    translation=[0, 1.86, .18],
                    extensions=dict(KHR_lights_punctual=dict(light=0))))
                doc["scenes"][0]["nodes"].append(len(doc["nodes"]) - 1)
                model.write_text(json.dumps(doc))
                for mode in ("hard", "soft"):
                    image, info = capture(f"artifact-msaa-close-instance{int(same_instance)}-{mode}-{label}",
                        model, "forward-raster", mode, extra=(*camera_options,
                            "--msaa", "4", "--directional-intensity", "40000",
                            "--directional-direction", "-1", "0", "-.5005",
                            "--ray-shadow-samples", "8"))
                    assert info["msaaSamples"] == 4 and info["rayShadows"]["denoise"]
                    close_captures[mode, label] = hdr(image)
            for mode in ("hard", "soft"):
                metrics = mixed_receiver_metrics(close_captures[mode, "blocked"], close_captures[mode, "clear"])
                assert metrics["foregroundP05"] > .9 and metrics["foregroundP95Error"] < .1, metrics
                assert metrics["backgroundP95"] < .12, metrics
                records.append(dict(case="artifacts", name="forward-msaa-close-parallel-receivers",
                                    sameInstance=same_instance, mode=mode, **metrics))
                print("close parallel receivers", same_instance, mode, metrics, flush=True)

            # A narrow distant emitter has the same grazing direction as above,
            # but contributes all lighting through the verified soft-area path.
            # Its physical radiance is intensity * area / distance squared.
            area_captures = {}
            for label, with_blocker in (("blocked", True), ("clear", False)):
                model = artifact_output / f"msaa-close-area-instance{int(same_instance)}-{label}.gltf"
                receiver_artifact_fixture(model, blocker=with_blocker,
                    close_parallel=True, same_instance=same_instance)
                doc = json.loads(model.read_text())
                yaw = np.arctan2(4000, 2002)
                doc["extensionsUsed"] = ["KHR_lights_punctual"]
                doc["extensions"] = dict(KHR_lights_punctual=dict(lights=[dict(
                    type="point", intensity=2.0008e11, color=[1, 1, 1],
                    extras=dict(areaLight=dict(shape="rect", width=.01, height=400)))]))
                doc["nodes"].append(dict(name="Grazing soft-area emitter",
                    translation=[4000, 0, 2002], rotation=[0, float(np.sin(yaw / 2)), 0, float(np.cos(yaw / 2))],
                    extensions=dict(KHR_lights_punctual=dict(light=0))))
                doc["scenes"][0]["nodes"].append(len(doc["nodes"]) - 1)
                model.write_text(json.dumps(doc))
                for denoise in (True, False):
                    extra = (*camera_options, "--msaa", "4", "--directional-intensity", "0", "--ray-shadow-samples", "8")
                    if not denoise:
                        extra += ("--no-ray-shadow-denoise",)
                    image, info = capture(f"artifact-msaa-close-area-instance{int(same_instance)}-filter{int(denoise)}-{label}",
                        model, "forward-raster", "soft", extra=extra)
                    assert info["lighting"]["areaLightCount"] == 1
                    area_captures[denoise, label] = hdr(image)
            for denoise in (True, False):
                metrics = mixed_receiver_metrics(area_captures[denoise, "blocked"], area_captures[denoise, "clear"])
                assert metrics["foregroundP05"] > .9 and metrics["foregroundP95Error"] < .1, metrics
                assert metrics["backgroundP95"] < .12, metrics
                matrix = np.array(info["camera"]["unjitteredViewProjColumns"]).T
                ratio = (2 * (np.arange(256) + .5) / 256 - 1) / matrix[0, 0]
                world_x = ratio * 5.003 / (1 + .5 * ratio)
                central = (abs(world_x) < .35)[None, :] & (np.arange(256)[:, None] >= 50) & (np.arange(256)[:, None] < 206)
                clear = area_captures[denoise, "clear"]
                background = central & (clear[:, :, 1] > .1) & (clear[:, :, 0] < 1e-5)
                foreground = central & (clear[:, :, 0] > .1) & (clear[:, :, 1] < 1e-5)
                assert background.sum() > 500 and foreground.sum() > 500, "close-area fixture lacks covered interiors"
                dark = area_captures[denoise, "blocked"][:, :, 1][background] / clear[:, :, 1][background]
                lit = area_captures[denoise, "blocked"][:, :, 0][foreground] / clear[:, :, 0][foreground]
                metrics.update(backgroundInteriorPixels=int(background.sum()),
                    foregroundInteriorPixels=int(foreground.sum()),
                    backgroundInteriorP95=float(np.percentile(dark, 95)),
                    backgroundIncorrectLitPixels=int(np.count_nonzero(dark > .12)),
                    foregroundInteriorP95Error=float(np.percentile(abs(lit - 1), 95)))
                assert metrics["backgroundIncorrectLitPixels"] == 0, metrics
                assert metrics["foregroundInteriorP95Error"] < .1, metrics
                records.append(dict(case="artifacts", name="forward-msaa-close-soft-area-receivers",
                                    sameInstance=same_instance, denoise=denoise, **metrics))
                print("close soft-area receivers", same_instance, denoise, metrics, flush=True)

        # Ray-selected local lights need receiver-specific queries even when
        # the raster atlas cannot provide a fallback for those lights. Soft mode
        # also exercises identity verification in mixed area-light receivers.
        for kind in ("point", "area"):
            local_captures = {}
            for label, with_blocker in (("blocked", True), ("clear", False)):
                model = artifact_output / f"msaa-{kind}-{label}.gltf"
                receiver_artifact_fixture(model, blocker=with_blocker)
                doc = json.loads(model.read_text())
                if kind == "point":
                    # The default one-point atlas budget shadows only one of
                    # these identical emitters, regardless of ECS light order.
                    lights = [dict(type="point", intensity=160, range=20,
                                   color=[1, 1, 1]) for _ in range(2)]
                else:
                    # This black point still takes six raster atlas layers;
                    # the remaining two cannot fit an area-light cube map.
                    lights = [dict(type="point", intensity=1, range=20,
                                   color=[0, 0, 0]),
                              dict(type="point", intensity=3000, range=20,
                                   color=[1, 1, 1], extras=dict(areaLight=dict(
                                       shape="rect", width=.4, height=.4)))]
                doc["extensionsUsed"] = ["KHR_lights_punctual"]
                doc["extensions"] = dict(KHR_lights_punctual=dict(lights=lights))
                for index in range(2):
                    doc["nodes"].append(dict(name=f"MSAA {kind} emitter {index}",
                        translation=[3, 0, 2], rotation=[0, .3826834324, 0, .9238795325],
                        extensions=dict(KHR_lights_punctual=dict(light=index))))
                    doc["scenes"][0]["nodes"].append(len(doc["nodes"]) - 1)
                model.write_text(json.dumps(doc))
                for mode in ("hard", "soft"):
                    image, telemetry = capture(f"artifact-msaa-{kind}-{mode}-{label}", model,
                        "forward-raster", mode, extra=(*camera_options, "--msaa", "4",
                            "--directional-intensity", "0", "--ray-shadow-samples", "8"))
                    assert telemetry["lighting"]["activeLocalShadowLayers"] == 6, telemetry["lighting"]
                    assert telemetry["lighting"]["pointLightCount"] == (2 if kind == "point" else 1)
                    if kind == "area":
                        assert telemetry["lighting"]["areaLightCount"] == 1
                        assert telemetry["lighting"]["areaShadowOrigins"] == 0
                    local_captures[mode, label] = hdr(image)
            for mode in ("hard", "soft"):
                metrics = mixed_receiver_metrics(local_captures[mode, "blocked"], local_captures[mode, "clear"])
                assert metrics["foregroundP05"] > .95 and metrics["foregroundP95Error"] < .05, (kind, mode, metrics)
                assert metrics["backgroundP95"] < .12, (kind, mode, metrics)
                records.append(dict(case="artifacts", name=f"forward-msaa-{kind}-without-raster-fallback", mode=mode, **metrics))
                print("forward MSAA", kind, mode, "without raster fallback", metrics, flush=True)

        grazing_model = artifact_output / "grazing-no-blocker.gltf"
        grazing_vertices = receiver_artifact_fixture(grazing_model, grazing=True)
        grazing_options = (*camera_options, "--camera-position", "0", "0", "1",
                           "--directional-direction", "-.08", "0", "-1")
        for technique in ("deferred-raster", "forward-raster"):
            raster, _ = capture(technique + "-artifact-grazing-raster", grazing_model, technique,
                                "raster", extra=grazing_options)
            ray, telemetry = capture(technique + "-artifact-grazing-ray", grazing_model, technique,
                                     "hard", extra=grazing_options)
            visibility, _ = capture(technique + "-artifact-grazing-visibility", grazing_model, technique,
                "hard", extra=(*grazing_options, "--ray-shadow-debug-layer", "1"))
            metrics = grazing_receiver_metrics(hdr(raster), hdr(ray), hdr(visibility),
                                               grazing_vertices, telemetry)
            # All rays point away from both planes. Incompatible depth normals
            # must not offset this grazing receiver back into its own triangles.
            assert metrics["visibilityP05"] > .98, (technique, metrics)
            assert metrics["lightingP05"] > .9 and metrics["lightingP95Error"] < .1, (technique, metrics)
            records.append(dict(case="artifacts", name="grazing-no-blocker", technique=technique, **metrics))
            print(technique, "grazing receiver", metrics, flush=True)

        # Disabled emitter layers stay lit rather than retaining/querying
        # occlusion for an area light that contributes no radiance.
        enabled_area = fixture("artifact-enabled-area")
        enabled, telemetry = capture("artifact-enabled-area-visibility", enabled_area, "forward-raster", "hard",
            extra=("--ray-shadow-debug-layer", "6", "--exposure", "1", "--warmup-frames", "8", "--capture-frame", "9"))
        enabled_visibility = hdr(enabled).mean(axis=2)
        formerly_blocked = np.zeros(enabled_visibility.shape, dtype=bool)
        for row in (485, 490, 495):
            points, mask = floor_points(telemetry, row)
            occluded = blocked(points, [0, 1.86, .18])
            selected = np.flatnonzero(mask)[occluded]
            formerly_blocked[row, selected] = True
        assert formerly_blocked.sum() > 30
        assert np.percentile(enabled_visibility[formerly_blocked], 95) < .02
        for disabled in ("intensity", "rgb"):
            model = fixture("artifact-disabled-area-" + disabled)
            doc = json.loads(model.read_text())
            light = doc["extensions"]["KHR_lights_punctual"]["lights"][0]
            light["intensity" if disabled == "intensity" else "color"] = 0 if disabled == "intensity" else [0, 0, 0]
            model.write_text(json.dumps(doc))
            image, info = capture("artifact-disabled-area-" + disabled, model, "forward-raster", "hard",
                extra=("--ray-shadow-debug-layer", "6", "--exposure", "1", "--warmup-frames", "8", "--capture-frame", "9"))
            assert info["lighting"]["areaLightCount"] == 1, "disabled area light must retain its traced layer"
            minimum_visibility = float(np.min(hdr(image).mean(axis=2)[formerly_blocked]))
            assert minimum_visibility > .98, (disabled, minimum_visibility)
            records.append(dict(case="artifacts", name="disabled-area-light", disabled=disabled,
                                previouslyBlockedPixels=int(formerly_blocked.sum()), minimumVisibility=minimum_visibility))
            print("disabled area light", disabled, "minimum visibility", minimum_visibility, flush=True)

    for technique in (() if args.case not in ("all", "quality") else ("deferred-raster", "forward-raster")):
        prefix = technique
        model = fixture("rect")
        clear = fixture("clear", blocker=False)
        raster, _ = capture(prefix + "-raster", model, technique, "raster")
        fallback, info = capture(prefix + "-fallback", model, technique, "soft", extra=("--no-ray-query",))
        assert not info["rayShadows"]["active"]
        assert np.array_equal(np.asarray(Image.open(raster)), np.asarray(Image.open(fallback)))
        records.append(dict(technique=technique, fallbackExact=True))
        for name, kind, size in (("rect", "rect", (.8, .8)), ("disk", "disk", (.8, .8)),
                                 ("small-rect", "rect", (.3, .3)),
                                 ("wide-rect", "rect", (1.4, .6)), ("wide-disk", "disk", (1.4, 1.4))):
            model = fixture(name, kind, size=size)
            clear = fixture(name + "-clear", kind, False, size=size)
            white, _ = capture(prefix + "-" + name + "-clear", clear, technique, "soft")
            candidate, telemetry = capture(prefix + "-" + name, model, technique, "soft")
            actual_hdr, clear_hdr = hdr(candidate), hdr(white)
            errors = []
            for row in (485, 490, 495):
                points, mask = floor_points(telemetry, row)
                expected = oracle(points, size, 1.86, kind == "disk")
                actual = np.mean(actual_hdr[row, mask] / np.maximum(clear_hdr[row, mask], 1e-5), axis=1)
                penumbra = (expected > .05) & (expected < .95)
                errors.extend(abs(actual[penumbra] - expected[penumbra]))
            mae, p95 = float(np.mean(errors)), float(np.percentile(errors, 95))
            assert mae < .035 and p95 < .10, (technique, name, mae, p95)
            records.append(dict(technique=technique, emitter=kind, size=size, penumbraMae=mae, p95=p95,
                memory=telemetry["rayShadows"], passes=telemetry.get("passes")))
            print(technique, name, "visibility MAE", mae, "P95", p95, flush=True)
        point = fixture("point", "point")
        debug, telemetry = capture(prefix + "-point", point, technique, "hard",
            extra=("--ray-shadow-debug-layer", "2", "--exposure", "1"))
        image = np.asarray(Image.open(debug).convert("RGB"), dtype=float) / 255
        errors = []
        for row in (485, 490, 495):
            points, mask = floor_points(telemetry, row)
            expected = ~blocked(points, [0, 1.86, .18])
            actual = image[row, mask].mean(axis=1) > .25
            errors.extend(actual != expected)
        assert np.mean(errors) < .005, (technique, "hard point", np.mean(errors))
        records.append(dict(technique=technique, hardPointMismatch=float(np.mean(errors))))
        point_clear = fixture("point-clear", "point", False)
        lit_clear, _ = capture(prefix + "-point-lit-clear", point_clear, technique, "hard")
        lit_shadow, info = capture(prefix + "-point-lit", point, technique, "hard")
        errors = []
        for row in (485, 490, 495):
            points, mask = floor_points(info, row)
            expected = ~blocked(points, [0, 1.86, .18])
            actual = (hdr(lit_shadow)[row, mask] / np.maximum(hdr(lit_clear)[row, mask], 1e-5)).mean(axis=1) > .5
            errors.extend(actual != expected)
        assert np.mean(errors) < .005, (technique, "point lighting integration", np.mean(errors))
        records.append(dict(technique=technique, hardPointShadingMismatch=float(np.mean(errors))))

    def plane_fixture(name, policy="opaque", mirrored=False, double_sided=True):
        path = fixture(name, "point", False)
        doc = json.loads(path.read_text())
        blob = bytearray()

        def accessor(values, components, index=False):
            offset = len(blob)
            flat = values if index else [component for value in values for component in value]
            blob.extend(struct.pack("<" + ("I" if index else "f") * len(flat), *flat))
            view = len(doc["bufferViews"])
            doc["bufferViews"].append(dict(buffer=len(doc["buffers"]), byteOffset=offset, byteLength=len(blob) - offset))
            result = len(doc["accessors"])
            data = dict(bufferView=view, componentType=5125 if index else 5126, count=len(values),
                        type="SCALAR" if index else "VEC" + str(components))
            if components == 3 and not index:
                data.update(min=np.min(values, axis=0).tolist(), max=np.max(values, axis=0).tolist())
            doc["accessors"].append(data)
            return result

        positions = [[-.45, .65, -.25], [-.45, .65, .65], [.45, .65, .65], [.45, .65, -.25]]
        uv = [[0, 0], [0, 1], [1, 1], [1, 0]]
        attrs = dict(POSITION=accessor(positions, 3), NORMAL=accessor([[0, -1 if policy == "back" else 1, 0]] * 4, 3),
                     TEXCOORD_0=accessor(uv, 2), TEXCOORD_1=accessor([[1 - u, v] for u, v in uv], 2))
        indices = accessor([0, 2, 1, 0, 3, 2] if policy == "back" else [0, 1, 2, 0, 2, 3], 1, True)
        material = dict(doubleSided=double_sided, pbrMetallicRoughness=dict(
            baseColorFactor=[1, 1, 1, 1], metallicFactor=0, roughnessFactor=1))
        if policy == "alpha":
            texture = Image.new("RGBA", (2, 2))
            texture.putdata([(255, 255, 255, 255), (255, 255, 255, 0),
                             (255, 255, 255, 0), (255, 255, 255, 255)])
            png = io.BytesIO(); texture.save(png, format="PNG")
            doc["images"] = [dict(uri="data:image/png;base64," + base64.b64encode(png.getvalue()).decode())]
            doc["samplers"] = [dict(magFilter=9728, minFilter=9728, wrapS=10497, wrapT=10497)]
            doc["textures"] = [dict(source=0, sampler=0)]
            material.update(alphaMode="MASK", alphaCutoff=.5)
            material["pbrMetallicRoughness"]["baseColorTexture"] = dict(index=0, extensions={
                "KHR_texture_transform": dict(texCoord=1, offset=[.13, .21], scale=[1.7, .8])})
            doc["extensionsUsed"].append("KHR_texture_transform")
        elif policy == "blend":
            material.update(alphaMode="BLEND")
            material["pbrMetallicRoughness"]["baseColorFactor"][3] = .5
        elif policy == "transmission":
            material["extensions"] = {"KHR_materials_transmission": dict(transmissionFactor=1)}
            doc["extensionsUsed"].append("KHR_materials_transmission")
        doc["materials"].append(material)
        doc["meshes"].append(dict(primitives=[dict(attributes=attrs, indices=indices, material=len(doc["materials"]) - 1)]))
        doc["nodes"].append(dict(name="Thin blocker", mesh=len(doc["meshes"]) - 1,
                                  scale=[-1 if mirrored else 1, 1, 1]))
        doc["scenes"][0]["nodes"].append(len(doc["nodes"]) - 1)
        doc["buffers"].append(dict(byteLength=len(blob), uri="data:application/octet-stream;base64," + base64.b64encode(blob).decode()))
        path.write_text(json.dumps(doc))
        return path

    for technique in (() if args.case not in ("all", "coverage") else ("deferred-raster", "forward-raster")):
        for kind in ("directional", "spot"):
            name = technique + "-" + kind
            model = fixture(name, "point")
            doc = json.loads(model.read_text())
            light = doc["extensions"]["KHR_lights_punctual"]["lights"][0]
            light["type"] = kind
            if kind == "spot": light["spot"] = dict(innerConeAngle=.6, outerConeAngle=1.0)
            else:
                light.pop("range", None)
                doc["nodes"][1]["rotation"] = [-.9238795325, 0, 0, .3826834324]
            model.write_text(json.dumps(doc))
            layer = "1" if kind == "directional" else "2"
            image, telemetry = capture(name, model, technique, "hard",
                extra=("--ray-shadow-debug-layer", layer, "--exposure", "1", "--directional-intensity", "2" if kind == "directional" else "0"))
            errors = []
            for row in (485, 490, 495):
                points, mask = floor_points(telemetry, row)
                emitter = [0, 1.86, .18] if kind == "spot" else points - np.array(telemetry["lighting"]["directionalDirection"]) * 10
                expected = ~blocked(points, emitter)
                errors.extend((hdr(image)[row, mask].mean(axis=1) > .5) != expected)
            mismatch = float(np.mean(errors))
            assert mismatch < .005, (name, mismatch)
            records.append(dict(case="coverage", name=name, mismatch=mismatch))
        for policy, mirrored, sided in (("opaque", False, True), ("opaque", True, True),
                                        ("opaque", False, False), ("back", False, False), ("alpha", False, True),
                                        ("alpha", True, True), ("blend", False, True),
                                        ("transmission", False, True)):
            name = f"{technique}-{policy}-mirror{int(mirrored)}-double{int(sided)}"
            model = plane_fixture(name, policy, mirrored, sided)
            image, telemetry = capture(name, model, technique, "hard",
                extra=("--ray-shadow-debug-layer", "2", "--exposure", "1"))
            pixels = hdr(image).mean(axis=2)
            errors = []
            for row in (485, 490, 495):
                points, mask = floor_points(telemetry, row)
                hit = points + (np.array([0, 1.86, .18]) - points) * (.65 / 1.86)
                inside = (abs(hit[:, 0]) < .45) & (hit[:, 2] > -.25) & (hit[:, 2] < .65)
                if policy in ("blend", "transmission", "back"):
                    inside[:] = False
                if policy == "alpha":
                    u = (hit[:, 0] * (-1 if mirrored else 1) + .45) / .9
                    v = (hit[:, 2] + .25) / .9
                    texel_x = np.floor(np.mod((1 - u) * 1.7 + .13, 1) * 2).astype(int)
                    texel_y = np.floor(np.mod(v * .8 + .21, 1) * 2).astype(int)
                    inside &= texel_x == texel_y
                errors.extend((pixels[row, mask] > .5) != ~inside)
            mismatch = float(np.mean(errors))
            assert mismatch < .015, (name, mismatch)
            records.append(dict(case="coverage", name=name, mismatch=mismatch))
            print(name, "mismatch", mismatch, flush=True)

        # A thin blocker completely outside the camera frustum still casts onto
        # the visible floor. The independent plane intersection ignores culling.
        name = technique + "-offscreen"
        model = plane_fixture(name)
        doc = json.loads(model.read_text())
        doc["nodes"][-1]["translation"] = [6, 0, 0]
        doc["nodes"][1]["translation"] = [17, 1.86, .18]
        doc["extensions"]["KHR_lights_punctual"]["lights"][0]["range"] = 40
        model.write_text(json.dumps(doc))
        image, telemetry = capture(name, model, technique, "hard",
            extra=("--ray-shadow-debug-layer", "2", "--exposure", "1"))
        errors = []
        for row in (485, 490, 495):
            points, mask = floor_points(telemetry, row)
            hit = points + (np.array([17, 1.86, .18]) - points) * (.65 / 1.86)
            inside = (abs(hit[:, 0] - 6) < .45) & (hit[:, 2] > -.25) & (hit[:, 2] < .65)
            assert inside.sum() > 10, "offscreen blocker missed reference rays"
            errors.extend((hdr(image)[row, mask].mean(axis=1) > .5) != ~inside)
        mismatch = float(np.mean(errors))
        assert mismatch < .005, (name, mismatch)
        records.append(dict(case="coverage", name=name, mismatch=mismatch))

    for technique in (() if args.case not in ("all", "motion") else ("deferred-raster", "forward-raster")):
        model = fixture("motion")
        for motion in ("blocker", "light", "camera"):
            samples = [1, 16, 17, 32, 48, 49, 64, 80]
            sequence = dict(schemaVersion=1, frames=80, sampleFrames=samples)
            if motion == "blocker":
                sequence.update(objectName="Shadow blocker", objectTranslation=[dict(frame=f, translation=[x, 0, 0])
                    for f, x in ((1, 0), (16, 0), (17, -.25), (48, -.25), (49, 0), (80, 0))])
            elif motion == "light":
                sequence["events"] = [dict(frame=f, areaLightPosition=[x, 1.86, .18]) for f, x in ((17, .25), (49, 0))]
            else:
                sequence["camera"] = [dict(frame=f, position=[x, 1.05, 4.1], target=[x, .95, 0])
                    for f, x in ((1, 0), (40, .15), (80, 0))]
            name = f"{technique}-{motion}-motion"
            image, _ = capture(name, model, technique, "soft", sequence,
                extra=("--ray-shadow-debug-layer", "6", "--exposure", "1", "--ray-shadow-samples", "8"))
            previous = None
            for frame in samples:
                path = image if frame == 80 else image.with_name(image.stem + f".frame-{frame:04d}.png")
                telemetry = json.loads(path.with_suffix(".telemetry.json").read_text())
                ray = telemetry["rayShadows"]
                if motion != "camera" and frame in (17, 49):
                    assert ray["historyResets"] > previous["historyResets"], (name, frame, ray)
                    if motion == "blocker":
                        assert ray["generation"] > previous["generation"] and ray["build"]["blasBuilt"] == 0
                        assert ray["build"]["blasReused"] == 7
                    else:
                        assert ray["generation"] == previous["generation"]
                errors = []
                visibility = hdr(path).mean(axis=2)
                for row in (485, 490, 495):
                    points, mask = floor_points(telemetry, row)
                    translation = -.25 if motion == "blocker" and 17 <= frame <= 48 else 0
                    source_x = .25 if motion == "light" and 17 <= frame <= 48 else 0
                    expected = oracle(points - np.array([translation, 0, 0]), (.8, .8), 1.86, False, 128, source_x - translation)
                    errors.extend(abs(visibility[row, mask] - expected))
                mae = float(np.mean(errors))
                assert mae < (.085 if frame in (1, 17, 49) else .035), (name, frame, mae)
                records.append(dict(case="motion", name=name, frame=frame, mae=mae, ray=ray, passes=telemetry["passes"]))
                previous = ray
                print(name, frame, "MAE", mae, flush=True)

    if args.case in ("all", "msaa", "msaa-motion"):
        # Current/previous receiver worlds must use the same sample convention
        # while the camera moves, not just when static history accumulates.
        model = fixture("msaa-camera-motion")
        samples = [1, 16, 17, 32, 48, 49, 64, 80]
        sequence = dict(schemaVersion=1, frames=80, sampleFrames=samples,
            camera=[dict(frame=f, position=[x, 1.05, 4.1], target=[x, .95, 0])
                    for f, x in ((1, 0), (40, .15), (80, 0))])
        name = "forward-raster-msaa4-camera-motion"
        image, _ = capture(name, model, "forward-raster", "soft", sequence,
            extra=("--msaa", "4", "--ray-shadow-debug-layer", "6", "--exposure", "1", "--ray-shadow-samples", "8"))
        for frame in samples:
            path = image if frame == 80 else image.with_name(image.stem + f".frame-{frame:04d}.png")
            telemetry = json.loads(path.with_suffix(".telemetry.json").read_text())
            assert telemetry["msaaSamples"] == 4 and telemetry["rayShadows"]["denoise"]
            visibility = hdr(path).mean(axis=2)
            errors = []
            for row in (485, 490, 495):
                points, mask = floor_points(telemetry, row)
                expected = oracle(points, (.8, .8), 1.86, False, 128)
                errors.extend(abs(visibility[row, mask] - expected))
            mae = float(np.mean(errors))
            assert mae < (.085 if frame in (1, 17, 49) else .035), (name, frame, mae)
            records.append(dict(case="msaa-motion", name=name, frame=frame, samples=4, mae=mae,
                                ray=telemetry["rayShadows"], passes=telemetry["passes"]))
            print(name, frame, "MAE", mae, flush=True)

    if args.case in ("all", "lifecycle"):
        model = fixture("lifecycle")
        empty = fixture("empty", "point", False)
        empty_doc = json.loads(empty.read_text())
        empty_doc["scenes"][0]["nodes"] = [1] # Keep valid geometry assets, with only the light node instantiated.
        empty.write_text(json.dumps(empty_doc))
        models = exe.parent / "models"
        for technique in ("deferred-raster", "forward-raster"):
            alternative = "forward-raster" if technique == "deferred-raster" else "deferred-raster"
            frames = [1, 12, 24, 36, 48, 60, 72, 84, 96, 108, 120, 132, 144, 156, 168, 180, 192, 204]
            sequence = dict(schemaVersion=1, frames=204, sampleFrames=frames, events=[
                dict(frame=8, rayShadows="hard"), dict(frame=20, rayShadows="soft", rayShadowSamples=8),
                dict(frame=32, rayShadows="raster"), dict(frame=44, rayShadows="soft"),
                dict(frame=56, reload=str(empty)), dict(frame=68, reload=str(models / "validation/taa_equivalent.bim")),
                dict(frame=80, reload=str(models / "validation/taa_equivalent.usda")),
                dict(frame=92, reload=str(models / "buildingSMART-IFC5-development/examples/Hello Wall/hello-wall.ifc")),
                dict(frame=104, reload=str(models / "buildingSMART-IFC5-development/examples/Hello Wall/hello-wall.ifcx")),
                dict(frame=116, reload=str(model)), dict(frame=128, samples=4),
                dict(frame=140, samples=1, taa=True), dict(frame=152, resize=[800, 450]),
                dict(frame=164, technique=alternative), dict(frame=176, rayShadows="hard"),
                dict(frame=188, rayShadows="soft", rayShadowSamples=32, rayShadowDenoise=False),
                dict(frame=200, rayShadowDenoise=True)])
            image, _ = capture(technique + "-lifecycle", model, technique, "soft", sequence)
            previous_epoch = None
            for frame in frames:
                path = image if frame == 204 else image.with_name(image.stem + f".frame-{frame:04d}.png")
                telemetry = json.loads(path.with_suffix(".telemetry.json").read_text())
                ray = telemetry["rayShadows"]
                assert ray["active"] == (frame != 36), (technique, frame, ray)
                if frame == 60: assert ray["build"]["instances"] == 0
                elif frame != 36: assert ray["build"]["instances"] > 0
                if frame in (72, 84, 96, 108): assert telemetry["scene"]["auxiliaryObjectCount"] > 0
                if frame in (180, 192, 204): assert telemetry["taa"]["epoch"] > previous_epoch
                previous_epoch = telemetry["taa"]["epoch"]
                records.append(dict(case="lifecycle", technique=technique, frame=frame, ray=ray,
                                    passes=telemetry["passes"]))

    if args.case in ("all", "budget", "msaa"):
        # The floor is tilted relative to the camera and has fully covered
        # penumbra interiors. Pixel-center depth differs from MAX sample depth;
        # those fragments must retain spatial/temporal denoising under MSAA.
        tilted = fixture("msaa-tilted")
        tilted_clear = fixture("msaa-tilted-clear", blocker=False)
        msaa_errors = {}
        for msaa in (1, 2, 4, 8):
            white, info = capture(f"forward-msaa{msaa}-tilted-clear", tilted_clear,
                "forward-raster", "soft", extra=("--msaa", str(msaa), "--ray-shadow-samples", "8"))
            if info["msaaSamples"] != msaa:
                assert msaa in (2, 8), ("required MSAA mode unavailable", msaa, info["msaaSamples"])
                records.append(dict(case="msaa", samples=msaa, skipped="unsupported sample count"))
                continue
            clear_hdr = hdr(white)
            images = {}
            for denoise in (True, False):
                extra = ("--msaa", str(msaa), "--ray-shadow-samples", "8")
                if not denoise:
                    extra += ("--no-ray-shadow-denoise",)
                image, telemetry = capture(f"forward-msaa{msaa}-tilted-filter{int(denoise)}",
                    tilted, "forward-raster", "soft", extra=extra)
                actual_hdr = hdr(image)
                errors, profile = [], []
                for row in (485, 490, 495):
                    points, mask = floor_points(telemetry, row)
                    expected = oracle(points, (.8, .8), 1.86)
                    actual = (actual_hdr[row, mask] / np.maximum(clear_hdr[row, mask], 1e-5)).mean(axis=1)
                    penumbra = (expected > .05) & (expected < .95)
                    errors.extend(abs(actual[penumbra] - expected[penumbra]))
                    profile.extend(actual[penumbra])
                mae = float(np.mean(errors))
                msaa_errors[msaa, denoise] = mae
                images[denoise] = np.array(profile)
                records.append(dict(case="msaa", samples=msaa, denoise=denoise, penumbraMae=mae,
                                    ray=telemetry["rayShadows"], passes=telemetry["passes"]))
            filtered, raw = msaa_errors[msaa, True], msaa_errors[msaa, False]
            assert filtered < raw * .8 and filtered < .035, ("tilted MSAA denoise", msaa, filtered, raw)
            assert np.mean(abs(images[True] - images[False])) > .02, ("filter has no visible effect", msaa)
            assert abs(filtered - msaa_errors[1, True]) < .015, ("MSAA filtering quality changed", msaa)
            print("tilted MSAA", msaa, "filtered/raw MAE", filtered, raw, flush=True)

    if args.case in ("all", "budget"):
        model = fixture("budget")
        for technique in ("deferred-raster", "forward-raster"):
            clear = fixture("budget-clear", blocker=False)
            white, _ = capture(technique + "-budget-clear", clear, technique, "soft")
            clear_hdr = hdr(white)
            for samples, denoise in ((1, True), (8, True), (32, True), (8, False)):
                extra = ("--ray-shadow-samples", str(samples)) + (() if denoise else ("--no-ray-shadow-denoise",))
                image, info = capture(f"{technique}-budget-{samples}-filter{int(denoise)}", model, technique, "soft", extra=extra)
                errors = []
                for row in (485, 490, 495):
                    points, mask = floor_points(info, row)
                    expected = oracle(points, (.8, .8), 1.86)
                    actual = (hdr(image)[row, mask] / np.maximum(clear_hdr[row, mask], 1e-5)).mean(axis=1)
                    penumbra = (expected > .05) & (expected < .95)
                    errors.extend(abs(actual[penumbra] - expected[penumbra]))
                records.append(dict(case="budget", technique=technique, samples=samples, denoise=denoise,
                                    mae=float(np.mean(errors)), ray=info["rayShadows"], passes=info["passes"]))
            filtered = next(r["mae"] for r in records if r.get("case") == "budget" and r["technique"] == technique and r["samples"] == 8 and r["denoise"])
            raw = next(r["mae"] for r in records if r.get("case") == "budget" and r["technique"] == technique and r["samples"] == 8 and not r["denoise"])
            assert filtered < raw * .8 and filtered < .035, (technique, filtered, raw)
            sequence = dict(schemaVersion=1, frames=24, sampleFrames=[4, 8, 16, 24], objectName="Shadow blocker",
                objectTranslation=[dict(frame=1, translation=[0, 0, 0]), dict(frame=24, translation=[.2, 0, 0])])
            image, _ = capture(technique + "-build-cost", model, technique, "soft", sequence)
            for frame in sequence["sampleFrames"]:
                path = image if frame == 24 else image.with_name(image.stem + f".frame-{frame:04d}.png")
                telemetry = json.loads(path.with_suffix(".telemetry.json").read_text())
                assert telemetry["rayShadows"]["build"]["blasBuilt"] == 0
                assert telemetry["rayShadows"]["build"]["blasReused"] == 7
                records.append(dict(case="build-cost", technique=technique, frame=frame,
                                    ray=telemetry["rayShadows"], passes=telemetry["passes"]))

    for record in records:
        record["validationClean"] = True
    (output / "results.json").write_text(json.dumps(records, indent=2) + "\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
