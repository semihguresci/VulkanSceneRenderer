"""Compare a known reflected target against the deferred raster IBL fallback."""

import argparse
import json
import math
import os
from pathlib import Path
import re
import subprocess
import tempfile

import numpy as np
from PIL import Image

from gltf_accessor_runtime_regression import Asset
from gltf_extended_materials_runtime_regression import validation_log_error
from graphics_concurrency_benchmark import file_identity, validation_environment


WIDTH, HEIGHT = 480, 360
EYE = np.array([0., 2.8, 5.])
TARGET = np.array([0., .65, 0.])
CARD_CENTER = np.array([.45, 1.05, -1.1])
FOV = 50.
TRACE_LIMIT = 40.
RECEIVER_BIAS = .008
MISS_HIT = 0
MISS_ROUGH_RECEIVER = 1
MISS_SCREEN_EXIT = 3
ODD_EXTENT = (483, 363)
WIDE_EXTENT = (1920, 1080)
VALIDATION_INSERTED = re.compile(r'Insert instance layer "VK_LAYER_KHRONOS_validation" \(([^\r\n]+)\)')
GPU_OPT_IN = "CONTAINER_RUN_GPU_VISUAL_REGRESSION"


def fixture(path, roughness, card_color=(1, .015, .008, 1), *, card_center=CARD_CENTER,
            card_size=(.63, .55), lit_card=False, blocker=None, floor_patch=None, blocker_color=(0, 0, 0)):
    asset = Asset()

    def quad(positions, normal, material):
        return {"attributes": {"POSITION": asset.accessor("VEC3", positions),
                               "NORMAL": asset.accessor("VEC3", [normal] * 4)},
                "indices": asset.accessor("SCALAR", [0, 1, 2, 0, 2, 3], 5123, indices=True),
                "material": material}

    def panel(center, half_size, material):
        x, y, z = center
        w, h = half_size
        return quad([[x - w, y - h, z], [x + w, y - h, z],
                     [x + w, y + h, z], [x - w, y + h, z]], [0, 0, 1], material)

    x, z, w, h = floor_patch if floor_patch is not None else (0., -1., 4., 4.)
    floor = quad([[x - w, 0, z + h], [x + w, 0, z + h], [x + w, 0, z - h], [x - w, 0, z - h]], [0, 1, 0], 0)
    card = panel(card_center, card_size, 1)
    asset.model.update(
        extensionsUsed=["KHR_materials_unlit"],
        meshes=[{"primitives": [floor]}, {"primitives": [card]}],
        nodes=[{"mesh": 0, "name": "Mirror"}, {"mesh": 1, "name": "Red card"}],
        scenes=[{"nodes": [0, 1]}],
        materials=[{"pbrMetallicRoughness": {"baseColorFactor": [.95, .95, .95, 1],
                                             "metallicFactor": 1, "roughnessFactor": roughness}},
                   {"pbrMetallicRoughness": {"baseColorFactor": list(card_color),
                                             "metallicFactor": 0, "roughnessFactor": 1},
                    "extensions": {"KHR_materials_unlit": {}}}])
    if lit_card:
        del asset.model["materials"][1]["extensions"]
    if blocker:
        center, half_size, alpha = blocker
        asset.model["meshes"].append({"primitives": [panel(center, half_size, 2)]})
        asset.model["nodes"].append({"mesh": 2, "name": "Black blocker"})
        asset.model["scenes"][0]["nodes"].append(2)
        material = {"pbrMetallicRoughness": {"baseColorFactor": [*blocker_color, alpha]},
                    "doubleSided": True, "extensions": {"KHR_materials_unlit": {}}}
        if alpha < 1:
            material["alphaMode"] = "MASK"
            material["alphaCutoff"] = .5
        asset.model["materials"].append(material)
    asset.write(path)


def camera_axes(target):
    forward = target - EYE
    forward /= np.linalg.norm(forward)
    right = np.cross(forward, [0, 1, 0])
    right /= np.linalg.norm(right)
    up = np.cross(right, forward)
    return forward, right, up


def project_point(world, target=TARGET, extent=(WIDTH, HEIGHT)):
    forward, right, up = camera_axes(target)
    width, height = extent
    relative = world - EYE
    pixels_per_unit = height * .5 / (math.tan(math.radians(FOV) * .5) * (forward @ relative))
    return np.array([width * .5 + (right @ relative) * pixels_per_unit,
                     height * .5 - (up @ relative) * pixels_per_unit])


def project(world, target=TARGET, extent=(WIDTH, HEIGHT)):
    return tuple(map(int, project_point(world, target, extent)))


def analytic_hit(card_center=CARD_CENTER, target=TARGET, extent=(WIDTH, HEIGHT)):
    """Reflect an analytic pixel-center ray from the y=0 plane onto the card's z plane."""
    receiver_pixel = project(card_center * [1, -1, 1], target, extent)
    forward, right, up = camera_axes(target)
    image_center = np.asarray(extent) * .5
    pixel_offset = np.asarray(receiver_pixel) + .5 - image_center
    focal = image_center[1] / math.tan(math.radians(FOV) * .5)
    # Screen pixels have an upper-left origin under the scene's negative-height viewport.
    incoming = forward + right * (pixel_offset[0] / focal) - up * (pixel_offset[1] / focal)
    incoming /= np.linalg.norm(incoming)
    receiver = EYE - incoming * (EYE[1] / incoming[1])
    origin = receiver + [0, RECEIVER_BIAS, 0]
    reflected = incoming * [1, -1, 1]
    distance = (card_center[2] - origin[2]) / reflected[2]
    hit = project_point(origin + reflected * distance, target, extent)
    return receiver_pixel, receiver, hit, float(distance)


def read_linear(image, extent):
    os.environ.setdefault("OPENCV_IO_ENABLE_OPENEXR", "1")
    import cv2

    metadata = json.loads(Path(f"{image}.capture.json").read_bytes())
    assert metadata["source"] == "raster-scene-composite" and metadata["colorSpace"] == "linear-sRGB", metadata
    assert not metadata["exposureApplied"] and not metadata["toneMappingApplied"] and not metadata["includesUi"], metadata
    assert (metadata["width"], metadata["height"]) == extent, metadata
    bgr = cv2.imdecode(np.frombuffer(image.read_bytes(), dtype=np.uint8), cv2.IMREAD_UNCHANGED)
    assert bgr is not None and bgr.shape == (extent[1], extent[0], 3) and bgr.dtype == np.float32, image
    assert np.isfinite(bgr).all(), f"Non-finite SSR output in {image}"
    return bgr[:, :, ::-1]


def checked_hit(image, extent=(WIDTH, HEIGHT), card_center=CARD_CENTER):
    points = image[..., :2] * extent
    distances = image[..., 2]
    valid = distances > 0
    assert np.all(image[~valid] == 0), "A screen miss did not produce the zero hit sentinel"
    assert np.any(valid) and np.all(distances[valid] <= TRACE_LIMIT), "Missing or out-of-range reflection distances"
    assert np.all(image[valid, :2] > 0) and np.all(image[valid, :2] < 1), "Reflection UV outside the render grid"
    centers = np.floor(points[valid]) + .5
    # The RGBA16F store can round toward zero before EXR readback. For UV in
    # [0, 1), one half-float ULP is at most 2**-11 in normalized coordinates.
    center_tolerance = np.asarray(extent) * 2**-11 + 1e-6
    assert np.all(np.abs(points[valid] - centers) <= center_tolerance), "A hit did not encode the depth texel center"

    (x, y), _, expected_pixel, expected_distance = analytic_hit(card_center, extent=extent)
    assert valid[y, x], (extent, (x, y), "Analytic card is visible but SSR missed")
    actual_pixel = points[y, x]
    actual_distance = float(distances[y, x])
    assert np.max(np.abs(actual_pixel - expected_pixel)) <= 3., (extent, expected_pixel, actual_pixel)
    assert abs(actual_distance - expected_distance) <= .12, (extent, expected_distance, actual_distance)
    return dict(receiverPixel=[x, y], expectedHitPixel=expected_pixel.tolist(), hitPixel=actual_pixel.tolist(),
                expectedDistanceMeters=expected_distance, distanceMeters=actual_distance,
                screenHitPixels=int(np.count_nonzero(valid)))


def miss_codes(rgb):
    assert np.all((rgb == 0) | (rgb == 1)), "Miss diagnostics were not encoded as RGB bits"
    return rgb.astype(np.uint8) @ np.array([1, 2, 4], dtype=np.uint8)


def verify_long_low(output, capture, evidence):
    card = np.array([.65, 1.6, -1.1])
    card_size = (.48, .12)
    (x, y), receiver, end, distance = analytic_hit(card, extent=WIDE_EXTENT)
    start = project_point(receiver + [0, RECEIVER_BIAS, 0], extent=WIDE_EXTENT)
    span = float(np.max(np.abs(end - start)))
    # A 24-visit traversal restricted to mip 4 covers at most 384 pixels.
    assert span > 384 and 0 < distance < TRACE_LIMIT, (span, distance)
    assert all(0 < pixel[0] < WIDE_EXTENT[0] and 0 < pixel[1] < WIDE_EXTENT[1] for pixel in (start, end))

    blocker = np.array([.085, 2.456, 4.2])
    blocker_size = (.05, .102)
    corners = np.array([project_point(blocker + [dx, dy, 0], extent=WIDE_EXTENT)
                        for dx in (-blocker_size[0], blocker_size[0]) for dy in (-blocker_size[1], blocker_size[1])])
    lower, upper = corners.min(axis=0), corners.max(axis=0)
    crossing_y = float((lower[1] + upper[1]) / 2)
    along_ray = (crossing_y - start[1]) / (end[1] - start[1])
    crossing = start + (end - start) * along_ray
    assert end[1] + 60 < lower[1] < upper[1] < start[1] - 60 and upper[1] - lower[1] > 200
    assert np.all(crossing > lower + 20) and np.all(crossing < upper - 20), (start, end, corners)
    assert max(receiver[2], card[2]) < blocker[2] < EYE[2], "Foreground strip physically intersects the reflected ray"

    patch = (float(receiver[0]), float(receiver[2]), .10, .05)
    result = dict(extent=list(WIDE_EXTENT), receiverPixel=[x, y], expectedHitPixel=end.tolist(),
                  expectedDistanceMeters=distance, projectedSpanPixels=span,
                  foregroundScreenBounds=[lower.tolist(), upper.tolist()], cases={})
    probe_x, probe_y = map(int, crossing)
    card_x, card_y = map(int, end)
    for name, options in (("empty", {}), ("foreground", {"blocker": (blocker, blocker_size, 1),
                                                          "blocker_color": (.01, 1., .015)})):
        model = f"marcher-long-{name}.gltf"
        fixture(output / model, .045, card_center=card, card_size=card_size, floor_patch=patch, **options)
        label = f"marcher-long-{name}"
        off = capture(f"{label}-off", "off", model=model, extent=WIDE_EXTENT, no_query=True)
        low = capture(f"{label}-low", "low", model=model, extent=WIDE_EXTENT, no_query=True)
        route = evidence[f"{label}-low"]["resources"]
        assert route["depthPyramidUsed"] and not route["rayFallbackActive"], (label, route)
        roi = np.s_[y - 3:y + 4, x - 3:x + 4]
        base, reflection = (frame[roi].mean(axis=(0, 1)) for frame in (off, low))
        card_rgb = off[card_y, card_x].astype(int)
        assert card_rgb[0] > max(card_rgb[1:]) + 70, (label, [card_x, card_y], card_rgb)
        # Highlight compression may reduce the color separation in bright reflections.
        assert reflection[0] > base[0] + 65 and reflection[0] > max(reflection[1:]) + 12, (label, base, reflection)
        strip_rgb = off[probe_y, probe_x].astype(int)
        green_excess = strip_rgb[1] - max(strip_rgb[0], strip_rgb[2])
        assert (green_excess > 70) == (name == "foreground"), (label, strip_rgb)
        result["cases"][name] = dict(offReceiverRgb=base.tolist(), lowReceiverRgb=reflection.tolist(),
                                     directCardPixel=[card_x, card_y], directCardRgb=card_rgb.tolist(),
                                     stripProbePixel=[probe_x, probe_y], stripProbeRgb=strip_rgb.tolist())
    return result


def verify_marcher(output, capture, evidence, fallback_renderer):
    def diagnostic(label, mode, *, model="mirror.gltf", target=TARGET, extent=(WIDTH, HEIGHT), alternate=None):
        image = capture(label, mode, model=model, target=target, extent=extent, no_query=True,
                        linear=True, alternate=alternate)
        route = evidence[label]["resources"]
        assert not route["rayFallbackActive"] and not route["historyReprojected"], (label, route)
        if alternate is None:
            assert route["depthPyramidUsed"] and route["depthPyramidMipLevels"] > 1 \
                and route["depthPyramidAllocationBytes"] > 0, (label, route)
        else:
            assert not route["depthPyramidUsed"] and route["depthPyramidMipLevels"] == 0 \
                and route["depthPyramidAllocationBytes"] == 0, (label, route)
        return image

    mirror = diagnostic("marcher-mirror-hit", "hit-uv")
    result = dict(mirror=checked_hit(mirror))
    x, y = result["mirror"]["receiverPixel"]
    reasons = miss_codes(diagnostic("marcher-mirror-reasons", "miss-reason"))
    assert reasons[y, x] == MISS_HIT and np.array_equal(reasons == MISS_HIT, mirror[..., 2] > 0), (x, y)
    capture("marcher-mirror-miss-display", "miss-reason", no_query=True)

    fixture(output / "marcher-odd-thin.gltf", .045, card_size=(.055, .32))
    odd = diagnostic("marcher-odd-thin-hit", "hit-uv", model="marcher-odd-thin.gltf", extent=ODD_EXTENT)
    result["oddThinCard"] = dict(extent=list(ODD_EXTENT), **checked_hit(odd, ODD_EXTENT))

    rough_hit = diagnostic("marcher-rough-hit", "hit-uv", model="rough.gltf")
    rough_codes = miss_codes(diagnostic("marcher-rough-reasons", "miss-reason", model="rough.gltf"))
    assert np.all(rough_hit == 0) and rough_codes[y, x] == MISS_ROUGH_RECEIVER, (x, y, rough_codes[y, x])
    fixture(output / "marcher-eligible-rough.gltf", .44)
    smooth_confidence = float(diagnostic("marcher-smooth-confidence", "confidence")[y, x, 0])
    rough_confidence = float(diagnostic("marcher-rough-confidence", "confidence", model="marcher-eligible-rough.gltf")[y, x, 0])
    assert .8 < smooth_confidence <= 1 and .10 < rough_confidence < smooth_confidence - .25, (smooth_confidence, rough_confidence)
    result["roughness"] = dict(smoothConfidence=smooth_confidence, roughConfidence=rough_confidence,
                               ineligibleReason=int(rough_codes[y, x]))

    offscreen_center = np.array([.35, 2.8, -1.5])
    down = np.array([0., -.5, 0.])
    (x, y), receiver, hit_pixel, distance = analytic_hit(offscreen_center, down)
    assert 0 <= x < WIDTH and 0 <= y < HEIGHT and hit_pixel[1] < 0 and 0 < distance < TRACE_LIMIT
    # Keep only the receiver under the test pixel. The bias and first step leave
    # this one-row patch, so no other surface can supersede the clipped exit.
    fixture(output / "marcher-offscreen.gltf", .045, card_center=offscreen_center, card_size=(.45, .30),
            floor_patch=(float(receiver[0]), float(receiver[2]), .08, .01))
    outside, reason = (diagnostic(f"marcher-offscreen-{mode}", mode, model="marcher-offscreen.gltf", target=down)
                       for mode in ("hit-uv", "miss-reason"))
    reason_code = int(miss_codes(reason)[y, x])
    assert np.all(outside[y, x] == 0) and reason_code == MISS_SCREEN_EXIT, (x, y, outside[y, x], reason_code)
    capture("marcher-offscreen-miss-display", "miss-reason", model="marcher-offscreen.gltf", target=down, no_query=True)
    result["offscreen"] = dict(receiverPixel=[x, y], projectedHitPixel=hit_pixel.tolist(), reason=reason_code)
    result["longLow"] = verify_long_low(output, capture, evidence)

    if fallback_renderer is not None:
        fallback = diagnostic("marcher-mip-zero-hit", "hit-uv", alternate=fallback_renderer)
        result["mipZeroFallback"] = checked_hit(fallback)
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--renderer", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--ray-fallback", action="store_true", help="Also verify off-screen ray hits on a ray-query device")
    parser.add_argument("--temporal", action="store_true", help="Also verify reflection history with motion and cuts")
    parser.add_argument("--marcher", action="store_true", help="Run the focused raster-only Hi-Z and numeric EXR checks")
    parser.add_argument("--mip-zero-renderer", type=Path,
                        help="Also check a staged renderer with the optional reflection depth-pyramid shader omitted")
    parser.add_argument("--validation-manifest", type=Path, help="Use this Khronos synchronization validation layer")
    parser.add_argument("--managed-output", action="store_true", help="Create a fresh run directory under --output")
    parser.add_argument("--require-opt-in", action="store_true", help=f"Skip unless {GPU_OPT_IN}=1")
    args = parser.parse_args()
    if args.require_opt_in and os.environ.get(GPU_OPT_IN) != "1":
        print(f"SKIP: set {GPU_OPT_IN}=1 to run GPU captures", flush=True)
        return 77
    if args.marcher and (args.temporal or args.ray_fallback):
        parser.error("--marcher is a focused run separate from --temporal and --ray-fallback")
    if args.mip_zero_renderer and not args.marcher:
        parser.error("--mip-zero-renderer requires --marcher")
    renderer, output = args.renderer.resolve(), args.output.resolve()
    output.mkdir(parents=True, exist_ok=args.managed_output)
    if args.managed_output:
        output = Path(tempfile.mkdtemp(prefix="run-", dir=output))
    print(f"SSR results: {output / 'results.json'}", flush=True)
    env = dict(os.environ, VK_LAYER_SETTINGS_PATH=str(output), VK_LOADER_DEBUG="layer")
    validation = validation_environment(args.validation_manifest, output, env) if args.validation_manifest else None
    (output / "vk_layer_settings.txt").write_text("khronos_validation.validate_sync = true\n"
                                                  "khronos_validation.duplicate_message_limit = 1000\n", encoding="ascii")
    fixture(output / "mirror.gltf", .045)
    fixture(output / "rough.gltf", .85)
    fixture(output / "black-card.gltf", .045, card_color=(0, 0, 0, 1))
    evidence = {}

    def capture(label, mode, *, model="mirror.gltf", forward=False, no_query=False, environment=0,
                hybrid=False, target=TARGET, directional=0, sequence=None, taa=False, extent=(WIDTH, HEIGHT),
                linear=False, alternate=None):
        executable = alternate if alternate else renderer
        image = output / (f"{label}.exr" if linear else f"{label}.png")
        command = [str(executable), "--hidden", "--no-ui", "--validation", "--import-scale", "1",
                   "--width", str(extent[0]), "--height", str(extent[1]), "--msaa", "1", "--taa" if taa else "--no-taa",
                   "--no-bloom", "--no-ddgi", "--ray-shadows", "raster", "--sky", "plain",
                   "--sky-horizon", "1", "1", "1", "--display-mode", "lit",
                   "--environment-intensity", str(environment), "--directional-intensity", str(directional),
                   "--directional-direction", "0", "0", "-1", "--exposure", "1",
                   "--camera-position", *map(str, EYE), "--camera-target", *map(str, target),
                   "--camera-fov", str(FOV), "--render-technique", "forward-raster" if forward else "deferred-raster",
                   "--screen-reflections", mode,
                   "--screenshot", str(image), "--model",
                   str(output / model)]
        if not linear:
            command.append("--screenshot-viewport")
        if sequence is None:
            command.extend(("--warmup-frames", "4", "--capture-frame", "5"))
        else:
            plan = output / f"{label}.sequence.json"
            plan.write_text(json.dumps(sequence), encoding="utf-8")
            command.extend(("--capture-sequence", str(plan), "--fixed-dt", ".016666667"))
        if no_query:
            command.append("--no-ray-query")
        if hybrid:
            command.append("--screen-reflections-ray-query")
        run = subprocess.run(command, cwd=executable.parent, env=env, capture_output=True,
                             text=True, errors="replace", timeout=240)
        log = run.stdout + run.stderr
        log_path = output / f"{label}.log"
        log_path.write_text(log, encoding="utf-8")
        assert run.returncode == 0 and not validation_log_error(log), log_path
        layer = None
        if args.marcher or validation:
            matched = VALIDATION_INSERTED.search(log)
            assert matched, f"Could not confirm Khronos validation layer insertion: {log_path}"
            layer = file_identity(executable.parent / Path(matched[1]))
            if validation:
                assert layer == validation["library"], (log_path, layer)
        if sequence is not None:
            images, routes = {}, {}
            for frame in sequence["sampleFrames"]:
                path = image if frame == sequence["frames"] else output / f"{label}.frame-{frame:04d}.png"
                receipt = json.loads(path.with_suffix(".telemetry.json").read_text(encoding="utf-8"))
                route = receipt["screenReflections"]
                pass_state = next((p for p in receipt["passes"] if p["name"] == "ScreenReflections"), None)
                routes[frame] = {"route": route, "pass": pass_state}
                with Image.open(path) as result:
                    images[frame] = np.array(result.convert("RGB"))
            evidence[label] = routes
            return images, routes
        receipt = json.loads(image.with_suffix(".telemetry.json").read_text(encoding="utf-8"))
        active = mode != "off" and not forward
        pass_state = next((entry for entry in receipt["passes"] if entry["name"] == "ScreenReflections"), None)
        assert bool(pass_state and pass_state["active"]) == active, (label, pass_state)
        if active:
            assert pass_state["gpuTimed"] and pass_state["gpuMs"] > 0, pass_state
            assert receipt["screenReflections"]["allocationBytes"] >= math.prod(extent) * 8, receipt["screenReflections"]
        if hybrid:
            route = receipt["screenReflections"]
            assert route["rayFallbackRequested"]
            assert route["rayFallbackAvailable"] == (not no_query), route
            assert route["rayFallbackActive"] == (not no_query and active), route
        evidence[label] = {"pass": pass_state, "resources": receipt.get("screenReflections"), "image": str(image),
                           "telemetry": str(image.with_suffix(".telemetry.json")), "log": str(log_path), "command": command}
        if layer:
            evidence[label]["validationLayer"] = layer
        if linear:
            assert receipt["resolution"] == list(extent) and receipt["validationEnabled"], receipt
            return read_linear(image, extent)
        with Image.open(image) as result:
            return np.array(result.convert("RGB"))

    if args.marcher:
        fallback = args.mip_zero_renderer.resolve(strict=True) if args.mip_zero_renderer else None
        renderer_id = file_identity(renderer)
        result = dict(passed=False, renderer=renderer_id, captures=evidence)
        try:
            if fallback:
                fallback_id = file_identity(fallback)
                assert fallback_id["sha256"] == renderer_id["sha256"], "The staged mip-zero control uses another executable"
                assert not (fallback.parent / "spv_shaders/screen_reflections_hiz.comp.spv").is_file(), fallback
                result["mipZeroRenderer"] = fallback_id
            result["marcher"] = verify_marcher(output, capture, evidence, fallback)
            assert file_identity(renderer) == renderer_id, "Renderer changed during SSR capture"
            if fallback:
                assert file_identity(fallback) == fallback_id, "Staged mip-zero executable changed during capture"
            result["passed"] = True
        except Exception as error:
            result["error"] = str(error)
            raise
        finally:
            (output / "results.json").write_text(json.dumps(result, indent=2, allow_nan=False) + "\n", encoding="utf-8")
        print("PASS analytic SSR hit UV/distance, roughness, clipped exit, and Hi-Z at even and odd extents")
        return

    base = capture("mirror-off", "off")
    mirror = capture("mirror-high", "high")
    raster_only = capture("mirror-no-query", "high", no_query=True)
    confidence = capture("mirror-confidence", "confidence")
    distance = capture("mirror-distance", "distance")
    rough_off = capture("rough-off", "off", model="rough.gltf")
    rough_on = capture("rough-high", "high", model="rough.gltf")
    forward_off = capture("forward-off", "off", forward=True)
    forward_on = capture("forward-high", "high", forward=True)
    ibl = capture("ibl-off", "off", model="black-card.gltf", environment=1)
    occluded_ibl = capture("ibl-high", "high", model="black-card.gltf", environment=1)

    reflected = CARD_CENTER * [1, -1, 1]
    x, y = project(reflected)
    roi = np.s_[y - 8:y + 9, x - 12:x + 13]
    assert np.array_equal(raster_only, mirror)
    assert np.array_equal(rough_off, rough_on)
    assert np.array_equal(forward_off, forward_on)
    assert mirror[roi][..., 0].mean() > base[roi][..., 0].mean() + 60, (x, y, mirror[roi].mean(axis=(0, 1)))
    assert mirror[roi][..., 0].mean() > mirror[roi][..., 1].mean() + 60
    assert confidence[roi][..., 0].mean() > 65 and distance[roi][..., 0].max() > 0
    assert ibl[roi].mean() > occluded_ibl[roi].mean() + 15, (ibl[roi].mean(), occluded_ibl[roi].mean())
    assert np.array_equal(ibl[:32], occluded_ibl[:32])
    results = {"passed": True, "expectedPixel": [x, y],
        "roiMeanRGB": mirror[roi].mean(axis=(0, 1)).tolist(),
        "iblRoiMeans": [ibl[roi].mean(), occluded_ibl[roi].mean()], "captures": evidence}

    if args.ray_fallback:
        center = np.array([.35, 2.8, -1.5])
        target = np.array([0., -.5, 0.])
        card_size = (.45, .3)
        virtual = center * [1, -1, 1]
        mirror_point = EYE + (virtual - EYE) * (EYE[1] / (EYE[1] + center[1]))
        occluder = mirror_point + (center - mirror_point) * .86
        shadow = center + [0., 0., .7]
        for point, size in ((center, card_size), (occluder, (.50, .34)), (shadow, (.26, .19))):
            assert all(project(point + [dx, dy, 0.], target)[1] < 0
                       for dx in (-size[0], size[0]) for dy in (-size[1], size[1])), point
        fixtures = {
            "offscreen": {},
            "offscreen-blocked": {"blocker": (occluder, (.50, .34), 1)},
            "offscreen-mask": {"blocker": (occluder, (.50, .34), 0)},
            "offscreen-lit": {"lit_card": True},
            "offscreen-shadow": {"lit_card": True, "blocker": (shadow, (.26, .19), 1)},
        }
        for name, options in fixtures.items():
            fixture(output / f"{name}.gltf", .045, card_center=center, card_size=card_size, **options)
        screen = capture("offscreen-screen", "high", model="offscreen.gltf", target=target)
        hybrid = capture("offscreen-hybrid", "high", model="offscreen.gltf", target=target, hybrid=True)
        source = capture("offscreen-source", "source", model="offscreen.gltf", target=target, hybrid=True)
        unsupported = capture("offscreen-unsupported", "high", model="offscreen.gltf", target=target,
                              hybrid=True, no_query=True)
        blocked = capture("offscreen-nearest-blocker", "high", model="offscreen-blocked.gltf", target=target, hybrid=True)
        cutout = capture("offscreen-alpha-mask", "high", model="offscreen-mask.gltf", target=target, hybrid=True)
        lit = capture("offscreen-lit-card", "high", model="offscreen-lit.gltf", target=target, hybrid=True, directional=3)
        shaded = capture("offscreen-shadowed-card", "high", model="offscreen-shadow.gltf", target=target,
                         hybrid=True, directional=3)
        x, y = project(virtual, target)
        region = np.s_[y - 4:y + 5, x - 5:x + 6]
        red = lambda capture: float(capture[region][..., 0].mean())
        assert np.array_equal(screen, unsupported)
        assert red(hybrid) > red(screen) + 70 and source[region][..., 2].mean() > 90
        assert source[region][..., 1].mean() < 20
        assert red(hybrid) > red(blocked) + 70
        np.testing.assert_array_equal(hybrid[region], cutout[region])
        assert red(lit) > red(shaded) + 30, (red(lit), red(shaded))
        results["rayFallback"] = {"expectedPixel": [x, y], "screenRed": red(screen), "rayRed": red(hybrid),
            "nearestBlockerRed": red(blocked), "alphaCutoutRed": red(cutout), "litRed": red(lit),
            "shadowedRed": red(shaded), "unsupportedPixelExact": True}

    if args.temporal:
        # A thin target moves clear of the analytically projected reflector.
        # Camera motion is small enough to reproject the stationary receiver.
        thin = (.18, .44)
        fixture(output / "thin-card.gltf", .045, card_size=thin)
        moving = {"schemaVersion": 1, "frames": 8, "sampleFrames": [1, 3, 4, 5, 6, 8],
                  "objectName": "Red card",
                  "objectTranslation": [{"frame": 1, "translation": [0, 0, 0]},
                                        {"frame": 4, "translation": [0, 0, 0]},
                                        {"frame": 5, "translation": [2.5, 0, 0]}],
                  "camera": [{"frame": 1, "position": EYE.tolist(), "target": TARGET.tolist()},
                             {"frame": 8, "position": (EYE + [.035, 0, 0]).tolist(), "target": TARGET.tolist()}]}
        filtered, motion = capture("moving-thin-card", "high", model="thin-card.gltf", sequence=moving)
        debug, history = capture("moving-thin-history", "history", model="thin-card.gltf", sequence=moving)
        x, y = project(reflected)
        center = np.s_[y - 4:y + 5, x - 4:x + 5]
        red = {frame: float(image[center][..., 0].mean()) for frame, image in filtered.items()}
        accepted = {frame: float(image[center][..., 0].mean()) for frame, image in debug.items()}
        assert motion[3]["route"]["historyReprojected"] and history[3]["route"]["historyReprojected"], history
        assert accepted[1] == 0 and accepted[3] > 150 and accepted[5] < 5, accepted
        assert red[3] > red[6] + 65 and abs(red[8] - red[6]) < 5, red
        taa_images, _ = capture("moving-thin-card-taa", "high", model="thin-card.gltf", sequence=moving, taa=True)
        taa_red = {frame: float(image[center][..., 0].mean()) for frame, image in taa_images.items()}
        assert taa_red[3] > taa_red[8] + 65 and abs(taa_red[8] - red[8]) < 8, taa_red

        lifecycle = {"schemaVersion": 1, "frames": 17, "sampleFrames": [1, 3, 5, 6, 7, 8, 9, 10, 11, 12, 13, 15, 16, 17],
                     "camera": [{"frame": 1, "position": EYE.tolist(), "target": TARGET.tolist()},
                                {"frame": 6, "position": EYE.tolist(), "target": TARGET.tolist()},
                                {"frame": 7, "position": (EYE + [6, 0, 0]).tolist(), "target": TARGET.tolist()}],
                     "events": [{"frame": 5, "reset": True}, {"frame": 8, "technique": "forward-raster"},
                                {"frame": 9, "technique": "deferred-raster"},
                                {"frame": 11, "materialEdit": {"sourceMaterial": 0, "values": [
                                    {"property": "pbrMetallicRoughness/roughnessFactor", "value": .22}]}},
                                {"frame": 13, "resize": [400, 300]},
                                {"frame": 16, "lightAdd": {"type": "point"}}]}
        _, routes = capture("history-lifecycle", "history", sequence=lifecycle)
        expected = {1: False, 3: True, 5: False, 6: True, 7: False, 8: False, 9: False,
                    10: True, 11: False, 12: True, 13: False, 15: True, 16: False, 17: True}
        assert {frame: state["route"]["historyReprojected"] for frame, state in routes.items()} == expected, routes
        assert not routes[8]["pass"] or not routes[8]["pass"]["active"], routes[8]

        roughness = {}
        for level in (.16, .30, .44, .58):
            label = f"roughness-{level:.2f}"
            fixture(output / f"{label}.gltf", level)
            image = capture(label, "high", model=f"{label}.gltf")
            roughness[str(level)] = float(image[roi][..., 0].mean())
        assert roughness["0.16"] > roughness["0.58"] + 70, roughness
        assert roughness["0.44"] < roughness["0.16"], roughness
        results["temporal"] = {"movingRed": red, "movingTaaRed": taa_red, "acceptedHistory": accepted,
                               "historyAfterEvents": expected, "roughnessRed": roughness}
        if args.ray_fallback:
            stationary = {"schemaVersion": 1, "frames": 3, "sampleFrames": [1, 3]}
            ray_history, routes = capture("offscreen-history", "history", model="offscreen.gltf",
                                          target=target, hybrid=True, sequence=stationary)
            accepted = {frame: float(image[region][..., 0].mean()) for frame, image in ray_history.items()}
            assert routes[3]["route"]["rayFallbackActive"] and routes[3]["route"]["historyReprojected"], routes
            assert accepted[1] == 0 and accepted[3] > 150, accepted
            results["temporal"]["rayHistory"] = accepted

    (output / "results.json").write_text(json.dumps(results, indent=2) + "\n", encoding="utf-8")
    print("PASS mirror reflection, IBL replacement, diagnostics, and requested fallback/history coverage")


if __name__ == "__main__":
    raise SystemExit(main())
