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
            card_size=(.63, .55), lit_card=False, blocker=None, floor_patch=None, blocker_color=(0, 0, 0),
            receiver_patches=None, blocker_setup=None, floor_normal=(0, 1, 0), floor_double_sided=False,
            card_setup=None, card_back=False, blocker_back=False):
    asset = Asset()

    def quad(positions, normal, material, *, back=False):
        indices = [0, 2, 1, 0, 3, 2] if back else [0, 1, 2, 0, 2, 3]
        return {"attributes": {"POSITION": asset.accessor("VEC3", positions),
                               "NORMAL": asset.accessor("VEC3", [normal] * 4)},
                "indices": asset.accessor("SCALAR", indices, 5123, indices=True),
                "material": material}

    def panel(center, half_size, material, *, back=False):
        x, y, z = center
        w, h = half_size
        return quad([[x - w, y - h, z], [x + w, y - h, z],
                     [x + w, y + h, z], [x - w, y + h, z]], [0, 0, -1 if back else 1], material, back=back)

    def material(color, metallic, level):
        return {"pbrMetallicRoughness": {"baseColorFactor": list(color),
                                         "metallicFactor": metallic, "roughnessFactor": level}}

    def floor(x, z, w, h, index):
        return quad([[x - w, 0, z + h], [x + w, 0, z + h], [x + w, 0, z - h], [x - w, 0, z - h]], floor_normal, index)

    floor_materials = [material((.95, .95, .95, 1), 1, roughness)]
    if receiver_patches is None:
        patch = floor_patch if floor_patch is not None else (0., -1., 4., 4.)
        receivers = [floor(*patch, 0)]
    else:
        floor_materials = [material((1, 1, 1, 1), metallic, level) for _, _, _, _, metallic, level in receiver_patches]
        receivers = [floor(*patch[:4], index) for index, patch in enumerate(receiver_patches)]
    if floor_double_sided:
        for floor_material in floor_materials:
            floor_material["doubleSided"] = True
    card_material = material(card_color, 0, 1)
    if not lit_card:
        card_material["extensions"] = {"KHR_materials_unlit": {}}
    card = panel(card_center, card_size, len(floor_materials), back=card_back)
    asset.model.update(
        extensionsUsed=["KHR_materials_unlit"],
        meshes=[{"primitives": receivers}, {"primitives": [card]}],
        nodes=[{"mesh": 0, "name": "Mirror"}, {"mesh": 1, "name": "Red card"}],
        scenes=[{"nodes": [0, 1]}],
        materials=[*floor_materials, card_material])
    if card_setup is not None:
        card_setup(asset, card, card_material)
    if blocker:
        center, half_size, alpha = blocker
        primitive = panel(center, half_size, len(asset.model["materials"]), back=blocker_back)
        asset.model["meshes"].append({"primitives": [primitive]})
        asset.model["nodes"].append({"mesh": 2, "name": "Black blocker"})
        asset.model["scenes"][0]["nodes"].append(2)
        blocker_material = {"pbrMetallicRoughness": {"baseColorFactor": [*blocker_color, alpha]},
                            "doubleSided": True, "extensions": {"KHR_materials_unlit": {}}}
        if alpha < 1:
            blocker_material["alphaMode"] = "MASK"
            blocker_material["alphaCutoff"] = .5
        if blocker_setup is not None:
            blocker_setup(asset, primitive, blocker_material)
        asset.model["materials"].append(blocker_material)
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


def verify_furnace(output, capture, environments):
    center = np.array([0., 1.05, -1.1])
    levels = (.045, .28, .44, .58)
    patches, probes = [], {}
    for span, roughness in zip((.35, 1., 1.65, 2.3), levels):
        for sign, metal in ((-1, 0), (1, 1)):
            pixel, receiver, _, _ = analytic_hit(center + [sign * span, 0, 0])
            probes[roughness, metal] = pixel
            patches.append((receiver[0], receiver[2], .235, .42, metal, roughness))
    models = {"black": 0., "matched": .5, "white": 1.}
    for label, radiance in models.items():
        fixture(output / f"energy-{label}.gltf", .045, card_color=(radiance,) * 3 + (1,),
                card_center=center, card_size=(3., .78), receiver_patches=patches)

    def render(label, mode, card, incident=.5):
        return capture(f"energy-{label}", mode, model=f"energy-{card}.gltf", no_query=True, linear=True,
                       hdr=environments[incident], environment=1, bounce=0)

    half_off = render("matched-off", "off", "matched")
    half_on = render("matched-high", "high", "matched")
    black_on = render("black-high", "high", "black")
    bright_on = render("white-against-half-high", "high", "white")
    unit_off = render("unit-off", "off", "white", 1.)
    unit_on = render("unit-high", "high", "white", 1.)
    confidence = render("confidence", "confidence", "matched")

    cx, cy = project(center)
    for image, expected in ((black_on, 0.), (half_off, .5), (half_on, .5), (bright_on, 1.),
                            (unit_off, 1.), (unit_on, 1.)):
        np.testing.assert_allclose(image[cy, cx], expected, atol=.002, rtol=0,
                                   err_msg="Unlit card did not deliver the authored scene-linear radiance")

    results = {}
    for roughness in levels:
        levels_report = {}
        for metal, kind in ((0, "dielectric"), (1, "metal")):
            x, y = probes[roughness, metal]
            roi = np.s_[y - 2:y + 3, x - 2:x + 3]
            weight = float(confidence[roi][..., 0].mean())
            observed = [float(image[roi].mean()) for image in (half_off, half_on, black_on, bright_on, unit_off, unit_on)]
            base, matched, black, bright, base_unit, matched_unit = observed
            context = (roughness, kind, (x, y), observed, weight)
            for incident, off, on in ((.5, half_off, half_on), (1., unit_off, unit_on)):
                for image in (off, on):
                    assert float(image[roi].max()) <= incident * 1.02 + .002, ("Furnace exceeds incident radiance", context)
                    assert float(image[roi].min()) >= incident * .70, ("White receiver loses excessive furnace energy", context)
                np.testing.assert_allclose(on[roi], off[roi], atol=incident * .018 + .002, rtol=0,
                                           err_msg=f"Matched radiance changed {kind} at roughness {roughness}")

            # With the geometry, lighting, and camera fixed, the two changes in
            # source radiance isolate the specular part of the receiver's response.
            loss, gain = matched - black, bright - matched
            if roughness > .55:
                assert weight == 0., context
                for image in (black_on, bright_on):
                    np.testing.assert_array_equal(image[roi], half_off[roi])
            else:
                assert weight >= .30, context
                assert min(loss, gain) > (.003 if not metal else .30 * weight), context
                assert abs(loss - gain) < .012, ("Unequal response to opposite incident changes", context)
                if metal:
                    np.testing.assert_allclose([loss, gain], [.5 * weight] * 2, rtol=.17, atol=.008,
                                               err_msg=f"White conductor replacement at roughness {roughness}")
                else:
                    assert black >= .75 * base, ("Dark screen hit suppressed dielectric diffuse", context)

            levels_report[kind] = dict(receiverPixel=[x, y], confidence=weight, offHalf=base, onHalf=matched,
                                       offUnit=base_unit, onUnit=matched_unit, blackHit=black, brightHit=bright,
                                       darkening=loss, brightening=gain)
        if roughness <= .55:
            assert levels_report["dielectric"]["darkening"] < .25 * levels_report["metal"]["darkening"], levels_report
        results[str(roughness)] = levels_report
    assert results["0.44"]["metal"]["confidence"] <= results["0.28"]["metal"]["confidence"] - .30, results
    return results


def card_point(x, y, z, target):
    forward, right, up = camera_axes(target)
    focal = (HEIGHT * .5) / math.tan(math.radians(FOV) * .5)
    direction = forward + right * ((x - WIDTH * .5) / focal) + up * ((HEIGHT * .5 - y) / focal)
    return EYE + direction * ((z - EYE[2]) / direction[2])


def verify_edge_energy(output, capture, environment):
    target = np.array([0., -.5, 0.])
    center = np.array([.37, 1.6767378, -1.5])
    card_size = (.45, .4360411)
    model = "energy-edge-black.gltf"
    fixture(output / model, .045, card_color=(0, 0, 0, 1), card_center=center, card_size=card_size,
            receiver_patches=[(.22, .5, .65, 1.4, 1, .045)])

    def render(label, mode, **options):
        return capture(f"energy-edge-{label}", mode, model=model, no_query=True, linear=True,
                       hdr=environment, environment=1, bounce=0, target=target, **options)

    off, on, hit, confidence = (render(mode, mode) for mode in ("off", "high", "hit-uv", "confidence"))
    samples = []
    for row in (-5, 2, 5, 9, 13, 18, 25):
        point = card_point(264, row, center[2], target)
        (x, y), _, expected_pixel, _ = analytic_hit(point, target)
        uv, distance = hit[y, x, :2], float(hit[y, x, 2])
        measured, source = float(confidence[y, x, 0]), (uv * (WIDTH, HEIGHT)).tolist()
        if row < 0:
            assert expected_pixel[1] < 0 and distance == 0 and measured == 0, (row, expected_pixel, source, measured)
            expected_confidence = 0.
        else:
            assert distance > 0 and np.max(np.abs(source - expected_pixel)) < 3., (row, expected_pixel, source)
            # Use the measured depth texel center to account for discrete hits.
            inset = float(min(*uv, *(1 - uv)))
            t = min(1., inset / .045)
            expected_confidence = t * t * (3 - 2 * t)
        base, reflected = (float(image[y, x].mean()) for image in (off, on))
        assert abs(measured - expected_confidence) < .006 and base > .35, (row, measured, expected_confidence, base)
        assert abs((base - reflected) / base - measured) < .025, (row, base, reflected, measured)
        samples.append(dict(receiverPixel=[x, y], expectedHitPixel=expected_pixel.tolist(), hitPixel=source,
                            confidence=measured, expectedConfidence=expected_confidence, off=base, reflected=reflected))
    weights = [sample["confidence"] for sample in samples]
    assert weights[0] == 0. and .015 < weights[1] < .15 and weights[-1] > .98, weights
    assert all(a < b for a, b in zip(weights[:5], weights[1:6])), weights

    # At fixed sight-line geometry and roughness, pan just enough to make a
    # modest-contrast card approach the border while its history still matches.
    moving_model = "energy-edge-moving.gltf"
    fixture(output / moving_model, .045, card_color=(.375, .375, .375, 1), card_center=center, card_size=card_size,
            receiver_patches=[(.22, .5, .65, 1.4, 1, .045)])
    initial_target = target + [0, .04, 0]
    motion = {"schemaVersion": 1, "frames": 5, "sampleFrames": [4, 5],
              "camera": [{"frame": 1, "position": EYE.tolist(), "target": initial_target.tolist()},
                         {"frame": 4, "position": EYE.tolist(), "target": initial_target.tolist()},
                         {"frame": 5, "position": EYE.tolist(), "target": target.tolist()}]}
    first = {"schemaVersion": 1, "frames": 1, "sampleFrames": [1]}

    def motion_capture(label, mode, sequence):
        return capture(f"energy-edge-{label}", mode, model=moving_model, no_query=True, linear=True,
                       hdr=environment, environment=1, bounce=0, target=target, sequence=sequence)

    fresh, fresh_routes = motion_capture("fresh", "high", first)
    warmed, warmed_routes = motion_capture("warmed", "high", motion)
    accepted, _ = motion_capture("warmed-history", "history", motion)
    motion_confidence, _ = motion_capture("warmed-confidence", "confidence", motion)
    assert not fresh_routes[1]["route"]["historyReprojected"] and warmed_routes[5]["route"]["historyReprojected"]
    witnesses = {}
    for row in (2, 5, 6, 7, 8):
        for column in (250, 264, 276):
            point = card_point(column, row, center[2], target)
            (x, y), receiver, _, _ = analytic_hit(point, target)
            px, py = project(receiver, initial_target)
            prev_conf = float(motion_confidence[4][py, px, 0])
            current_conf = float(motion_confidence[5][y, x, 0])
            if not (.03 < current_conf < .45 and .075 < prev_conf - current_conf < .24):
                continue
            base = float(off[y, x].mean())
            cold = float(fresh[1][y, x].mean())
            history = float(warmed[5][y, x].mean())
            assert abs((base - cold) - .125 * current_conf) < .003, (x, y, base, cold, current_conf)
            if accepted[5][y, x, 0] > .9:
                assert abs(history - cold) < .0025, (x, y, base, cold, history, prev_conf, current_conf)
                witnesses[x, y] = dict(receiverPixel=[x, y], prevPixel=[px, py], prevConfidence=prev_conf,
                                       currentConfidence=current_conf, off=base, fresh=cold, warmed=history)
    assert len(witnesses) >= 2, ("No usable history while edge confidence was falling", witnesses)
    return dict(static=samples, moving=list(witnesses.values()))


def verify_energy(output, capture, evidence):
    from reference_path_runtime_regression import write_environment

    environments = {incident: output / f"energy-{label}.hdr" for incident, label in ((.5, "half"), (1., "unit"))}
    for incident, hdr in environments.items():
        write_environment(hdr, [incident] * 3)
    result = dict(furnace=verify_furnace(output, capture, environments),
                  edge=verify_edge_energy(output, capture, environments[.5]))
    for label, capture_evidence in evidence.items():
        samples = [capture_evidence] if "resources" in capture_evidence else capture_evidence.values()
        for sample in samples:
            route = sample.get("resources", sample.get("route"))
            if sample["pass"] and sample["pass"]["active"]:
                assert route and not route["rayFallbackActive"] and route["depthPyramidUsed"], (label, route)
    return result


def history_trail(previous, current, observed):
    contrast = previous - current
    error = observed - current
    energy = np.square(contrast).sum(axis=-1)
    changed = energy > .01
    assert changed.mean() > .95, "Disocclusion fixture did not change enough pixels"
    stale = np.maximum(np.sum(error * contrast, axis=-1)[changed], 0) / energy[changed]
    return dict(pixels=int(changed.sum()), maxTrailFraction=float(stale.max()),
                p95TrailFraction=float(np.percentile(stale, 95)), maxLinearError=float(np.abs(error).max()),
                meanLinearError=float(np.abs(error).mean()))


def verify_disocclusion(output, capture):
    (x, y), receiver, _, distance = analytic_hit()
    patch = np.s_[y - 4:y + 5, x - 4:x + 5]
    direction = (receiver - EYE) * [1, -1, 1]
    direction /= np.linalg.norm(direction)
    rear = CARD_CENTER + direction * .045
    samples = [1, 3, 4, 5, 6, 8]
    motion = {"schemaVersion": 1, "frames": max(samples), "sampleFrames": samples,
              "objectName": "Red card",
              "objectTranslation": [{"frame": 1, "translation": [0, 0, 0]},
                                    {"frame": 4, "translation": [0, 0, 0]},
                                    {"frame": 5, "translation": [2.5, 0, 0]}],
              "camera": [{"frame": 1, "position": EYE.tolist(), "target": TARGET.tolist()},
                         {"frame": 8, "position": (EYE + [.035, 0, 0]).tolist(), "target": TARGET.tolist()}]}
    cold_start = {**motion, "events": [{"frame": frame, "reset": True} for frame in samples]}
    warm_color = [.72, .22, .12, 1.]
    rear_color = [.56, .34, .20, 1.]

    def tint_card(asset, primitive, _material):
        primitive["attributes"]["COLOR_0"] = asset.accessor("VEC4", [warm_color] * 4)

    def tint_rear(asset, primitive, _material):
        primitive["material"] = asset.model["meshes"][1]["primitives"][0]["material"]
        primitive["attributes"]["COLOR_0"] = asset.accessor("VEC4", [rear_color] * 4)

    result = {}
    for case in ("miss", "same-material"):
        model = f"denoise-{case}.gltf"
        options = {} if case == "miss" else dict(blocker=(rear, (.50, .61), 1), blocker_setup=tint_rear)
        fixture(output / model, .045, card_color=(1, 1, 1, 1), card_size=(.25, .42), card_setup=tint_card, **options)
        if case == "same-material":
            asset = json.loads((output / model).read_bytes())
            assert asset["meshes"][1]["primitives"][0]["material"] == asset["meshes"][2]["primitives"][0]["material"]

        def render(label, mode, sequence=motion, **settings):
            return capture(f"denoise-{case}-{label}", mode, model=model, no_query=True, linear=True,
                           environment=.35 if case == "miss" else 0, bounce=0, sequence=sequence, **settings)

        enabled, enabled_routes = render("history-on", "high")
        reset, reset_routes = render("same-frame-reset", "high", cold_start)
        reuse, _ = render("accepted", "history")
        hits, _ = render("hit-uv", "hit-uv")
        np.testing.assert_array_equal(enabled[1], reset[1])
        assert enabled_routes[4]["route"]["historyReprojected"] and enabled_routes[5]["route"]["historyReprojected"]
        assert all(not state["route"]["historyReprojected"] for state in reset_routes.values())
        assert reuse[4][patch][..., 0].mean() > .90 and reuse[5][patch][..., 0].mean() < .08, (case, reuse[5][patch])
        before, after = (image[patch][..., 2] for image in (hits[4], hits[5]))
        assert abs(float(np.median(before)) - distance) < .14 and np.all(before > 0), (case, before)
        if case == "miss":
            assert np.all(after == 0), after
        else:
            assert np.all(after > 0) and .020 < float(np.median(after - before)) < .095, (before, after)
            assert reuse[6][patch][..., 0].mean() > .85, (case, reuse[6][patch])

        comparisons = {}
        for frame in (5, 6, 8):
            scores = history_trail(reset[4][patch], reset[frame][patch], enabled[frame][patch])
            assert scores["maxTrailFraction"] < .015 and scores["maxLinearError"] < .006, (case, frame, scores)
            comparisons[frame] = scores

        disabled, disabled_routes = render("history-disabled", "high", no_history=True)
        assert all(not state["route"]["historyAvailable"] and not state["route"]["historyReprojected"]
                   for state in disabled_routes.values()), disabled_routes
        for frame in samples:
            np.testing.assert_array_equal(disabled[frame], reset[frame], err_msg=f"History fallback frame {frame} in {case}")
        enabled_resources = enabled_routes[4]["route"]
        disabled_resources = disabled_routes[4]["route"]
        enabled_bytes = enabled_resources["historyAllocationBytes"]
        disabled_bytes = disabled_resources["historyAllocationBytes"]
        assert enabled_bytes >= WIDTH * HEIGHT * 32 and 0 < disabled_bytes < enabled_bytes // 100
        id_bytes = enabled_resources["surfaceIdAllocationBytes"]
        assert id_bytes >= WIDTH * HEIGHT * 4 and disabled_resources["surfaceIdAllocationBytes"] == id_bytes
        for routes in (enabled_routes, disabled_routes):
            snapshot = routes[4]["surfaceIdPass"]
            assert snapshot and snapshot["active"] and snapshot["gpuTimed"] and snapshot["gpuMs"] > 0, snapshot
        assert (enabled_resources["allocationBytes"] - disabled_resources["allocationBytes"]
                == enabled_bytes - disabled_bytes), (case, enabled_resources, disabled_resources)
        result[case] = dict(receiverPixel=[x, y], sampleCount=int(before.size), staleColor=comparisons,
                            historyAccepted={f: float(reuse[f][patch][..., 0].mean()) for f in samples},
                            warmHitDistance=float(np.median(before)), currentHitDistance=float(np.median(after)),
                            historyBytes=enabled_bytes, fallbackHistoryBytes=disabled_bytes,
                            surfaceIdBytes=id_bytes, disabledMatchesReset=True)
        if case == "same-material" and 4 in enabled_routes[4]["supportedMsaaSamples"]:
            live_msaa, live_routes = render("msaa4-history-on", "high", msaa=4)
            reset_msaa, cold_routes = render("msaa4-same-frame-reset", "high", cold_start, msaa=4)
            reuse_msaa, debug_routes = render("msaa4-accepted", "history", msaa=4)
            for routes in (live_routes, cold_routes, debug_routes):
                assert routes[4]["msaaSamples"] == 4 and routes[5]["msaaSamples"] == 4, routes
                assert routes[5]["surfaceIdPass"]["active"], routes[5]
            assert live_routes[5]["route"]["historyReprojected"] and not cold_routes[5]["route"]["historyReprojected"]
            reused = {f: float(reuse_msaa[f][patch][..., 0].mean()) for f in (4, 5)}
            assert reused[4] > .90 and reused[5] < .08, reused
            scores = history_trail(reset_msaa[4][patch], reset_msaa[5][patch], live_msaa[5][patch])
            assert scores["maxTrailFraction"] < .015 and scores["maxLinearError"] < .006, scores
            result[case]["msaa4"] = dict(historyAccepted=reused, firstFrame=scores)
    return result


def verify_stationary_denoising(output, capture):
    texture_name = "denoise-stripes.png"
    stripes = np.where((np.arange(40) // 3) % 2 == 0, 148, 169).astype(np.uint8)
    Image.fromarray(np.tile(stripes[np.newaxis, :, np.newaxis], (8, 1, 3))).save(output / texture_name)

    def stripe_card(asset, primitive, material):
        asset.model.update(images=[{"uri": texture_name}], textures=[{"source": 0, "sampler": 0}],
                           samplers=[{"minFilter": 9728, "magFilter": 9728}])
        primitive["attributes"]["TEXCOORD_0"] = asset.accessor("VEC2", [[0, 0], [1, 0], [1, 1], [0, 1]])
        material["pbrMetallicRoughness"]["baseColorTexture"] = {"index": 0}

    model = "denoise-stripes.gltf"
    fixture(output / model, .42, card_color=(1, 1, 1, 1), card_size=(.83, .65), card_setup=stripe_card)
    (x, y), _, _, _ = analytic_hit()
    patch = np.s_[y - 5:y + 6, x - 7:x + 8]
    samples = list(range(1, 17))
    stationary = {"schemaVersion": 1, "frames": max(samples), "sampleFrames": samples}
    cold_start = {**stationary, "events": [{"frame": frame, "reset": True} for frame in samples]}
    settings = dict(model=model, linear=True, environment=0, bounce=0, no_query=True)
    live, live_routes = capture("denoise-rough-history-on", "high", sequence=stationary, **settings)
    spatial, spatial_routes = capture("denoise-rough-spatial-only", "high", sequence=cold_start, **settings)
    np.testing.assert_array_equal(live[1], spatial[1])
    assert all(live_routes[f]["route"]["historyReprojected"] for f in samples[1:])
    assert not any(state["route"]["historyReprojected"] for state in spatial_routes.values())
    live_patch, spatial_patch = (np.stack([images[f][patch] for f in samples[3:]]) for images in (live, spatial))
    live_std, spatial_std = (float(np.sqrt(data.var(axis=0).mean())) for data in (live_patch, spatial_patch))
    live_mean, spatial_mean = (float(data.mean()) for data in (live_patch, spatial_patch))
    assert spatial_std > .003 and live_std <= .90 * spatial_std, (live_std, spatial_std)
    assert spatial_mean > .1 and abs(live_mean - spatial_mean) < .012, (live_mean, spatial_mean)
    return dict(receiverPixel=[x, y], comparedFrames=samples[3:], historySigma=live_std, spatialSigma=spatial_std,
                historyMean=live_mean, spatialMean=spatial_mean)


def verify_denoise_lifecycle(output, capture):
    alternate_model = output / "denoise-reload.gltf"
    fixture(alternate_model, .045, card_color=(.02, .24, .73, 1))
    expected = {1: False, 3: True, 5: False, 6: True, 7: False, 8: False, 9: False,
                10: True, 11: False, 12: True, 13: False, 15: True, 16: False, 17: True,
                18: False, 19: True, 20: False, 21: True}
    sequence = {"schemaVersion": 1, "frames": max(expected), "sampleFrames": list(expected),
                "camera": [{"frame": 1, "position": EYE.tolist(), "target": TARGET.tolist()},
                           {"frame": 6, "position": EYE.tolist(), "target": TARGET.tolist()},
                           {"frame": 7, "position": (EYE + [6, 0, 0]).tolist(), "target": TARGET.tolist()}],
                "events": [{"frame": 5, "reset": True}, {"frame": 8, "technique": "forward-raster"},
                           {"frame": 9, "technique": "deferred-raster"},
                           {"frame": 11, "materialEdit": {"sourceMaterial": 0, "values": [
                               {"property": "pbrMetallicRoughness/roughnessFactor", "value": .22}]}},
                           {"frame": 13, "resize": list(ODD_EXTENT)}, {"frame": 16, "lightAdd": {"type": "point"}},
                           {"frame": 18, "sectionPlane": [0, 0, -1, .8]},
                           {"frame": 20, "reload": str(alternate_model)}]}
    frames, samples = capture("denoise-lifecycle", "history", sequence=sequence, no_query=True)
    observed = {frame: sample["route"]["historyReprojected"] for frame, sample in samples.items()}
    assert observed == expected, (observed, expected)
    assert not samples[8]["pass"] or not samples[8]["pass"]["active"], samples[8]
    assert samples[9]["pass"]["active"], samples[9]
    assert frames[12].shape[:2] == (HEIGHT, WIDTH) and frames[13].shape[:2] == ODD_EXTENT[::-1]
    assert samples[13]["route"]["historyAllocationBytes"] >= math.prod(ODD_EXTENT) * 32
    return dict(historyAfterEvents=observed, originalExtent=[WIDTH, HEIGHT], resizedExtent=list(ODD_EXTENT))


def verify_denoising(output, capture):
    return dict(disocclusion=verify_disocclusion(output, capture), stationary=verify_stationary_denoising(output, capture),
                lifecycle=verify_denoise_lifecycle(output, capture))


def verify_bim_overlay(output, capture):
    from bim_floor_plan_runtime_regression import panel, settings

    # The wall's floor-plan line is on the same view ray as the visible card,
    # but far enough behind the card that scene depth should hide the line.
    px, py, pz = EYE + 1.035 * (CARD_CENTER - EYE)
    wall = panel(1, (float(px - .8), float(py), float(px + .8), float(py + .35)), float(pz))
    auxiliary = output / "reflection-floor-plan.bim"
    auxiliary.write_text(json.dumps({
        "schema_version": "1.1.0", "meshes": [wall],
        "elements": [dict(mesh_id=1, guid="floor-plan-behind-card", type="IfcWall", storeyName="Overlay proof",
                          color=dict(r=165, g=165, b=165, a=255))],
    }), encoding="utf-8")
    phases = {2: "off", 4: "no-depth", 6: "depth", 8: "off-again"}
    events = []
    for frame, name in phases.items():
        style = settings(enabled=name in ("no-depth", "depth"), elevation="source-elevation", depth=name == "depth",
                         color=(0, 1, 0), width=5)
        events.append(dict(frame=frame - 1, bimFloorPlan=style))
    sequence = dict(schemaVersion=1, frames=max(phases), sampleFrames=list(phases), events=events)

    reflected_pixel, _, card_pixel, hit_distance = analytic_hit()
    patches = {}
    for label, (x, y) in (("card", tuple(map(int, card_pixel))), ("mirror", reflected_pixel)):
        patches[label] = np.s_[y - 8:y + 9, x - 8:x + 9]

    samples, route_reports = {}, {}
    for mode in ("off", "high"):
        frames, captures = capture(f"bim-overlay-{mode}", mode, environment=.05, no_query=True, no_history=True,
                                   sequence=sequence, bim_model=auxiliary, tone_map="neutral")
        samples[mode] = {name: frames[frame].astype(np.int16) for frame, name in phases.items()}
        route_reports[mode] = {}
        for frame, name in phases.items():
            frame_evidence = captures[frame]
            image = frame_evidence["image"]
            metadata = json.loads(Path(f"{image}.capture.json").read_bytes())
            state = json.loads(Path(frame_evidence["telemetry"]).read_bytes())
            plan = state["bim"]["floorPlan"]
            enabled = name in ("no-depth", "depth")
            assert metadata["source"] == "viewport" and not metadata["includesUi"] and metadata["displayMode"] == 0, metadata
            assert (metadata["width"], metadata["height"]) == (WIDTH, HEIGHT) and state["technique"] == "deferred-raster"
            assert plan["requested"] == enabled and plan["enabled"] == enabled, (mode, name, plan)
            if enabled:
                assert plan["viewportRoute"] == "scene-color" and plan["depthTest"] == (name == "depth"), (mode, name, plan)
            reflection_pass = frame_evidence["pass"]
            assert bool(reflection_pass and reflection_pass["active"]) == (mode == "high"), (mode, name, reflection_pass)
            post_pass = next((entry for entry in state["passes"] if entry["name"] == "BimReflectionDisplayOverlays"), None)
            if mode == "off":
                assert not post_pass or not post_pass["active"], (mode, name, post_pass)
            elif enabled:
                assert post_pass and post_pass["active"] and post_pass["gpuTimed"], (mode, name, post_pass)
            if mode == "high":
                route = frame_evidence["route"]
                assert not route["historyAvailable"] and not route["rayFallbackActive"], route
            route_reports[mode][name] = dict(floorPlan=plan, reflectionPass=reflection_pass, overlayPass=post_pass)

    def compare(previous, current, patch):
        delta = current[patch] - previous[patch]
        return dict(maximumRgbDelta=int(np.abs(delta).max()),
                    changedPixels=int(np.count_nonzero(np.abs(delta).max(axis=2) > 3)),
                    maxGreenGain=int(delta[..., 1].max()), greenPixels=int(np.count_nonzero(delta[..., 1] > 12)))

    off, high = samples["off"], samples["high"]
    mirror_patch = patches["mirror"]
    mirror_gain = int((high["off"][mirror_patch] - off["off"][mirror_patch])[..., 0].max())
    assert mirror_gain > 30, ("Mirror has no red reflection", mirror_gain)
    result = dict(reflectedPixel=list(reflected_pixel), projectedCardPixel=card_pixel.tolist(),
                  reflectionHitDistance=hit_distance, mirrorRedGain=mirror_gain, routes=route_reports, pixels={})
    for mode, frames in samples.items():
        np.testing.assert_array_equal(frames["off"], frames["off-again"], err_msg=f"{mode} overlay toggle did not restore pixels")
        deltas = {phase: {name: compare(frames["off"], frames[phase], patch) for name, patch in patches.items()}
                  for phase in ("no-depth", "depth")}
        result["pixels"][mode] = deltas
        visible = deltas["no-depth"]["card"]
        assert visible["greenPixels"] >= 3 and visible["maxGreenGain"] > 24, (mode, deltas)
        assert deltas["depth"]["card"]["changedPixels"] == 0, (mode, deltas)
        assert all(phase["mirror"]["maximumRgbDelta"] == 0 for phase in deltas.values()), (mode, deltas)
    return result


def ray_policy_blocker(profile):
    def configure(asset, primitive, material):
        if profile == "textured":
            transform = "KHR_texture_transform"
            asset.model["extensionsUsed"].append(transform)
            asset.model.update(images=[{"uri": "ray-policy-alpha.png"}],
                               samplers=[{"magFilter": 9728, "minFilter": 9728, "wrapS": 33071, "wrapT": 33071}],
                               textures=[{"source": 0, "sampler": 0}])
            primitive["attributes"]["TEXCOORD_0"] = asset.accessor("VEC2", [[.875, .5]] * 4)
            primitive["attributes"]["TEXCOORD_1"] = asset.accessor("VEC2", [[0, 0], [1, 0], [1, 1], [0, 1]])
            material.update(alphaMode="MASK", alphaCutoff=.5)
            material["pbrMetallicRoughness"]["baseColorTexture"] = {
                "index": 0, "texCoord": 0, "extensions": {transform: {"texCoord": 1, "scale": [.5, 1], "offset": [.375, 0]}}}
        elif profile in ("flat-below", "flat-equal"):
            alpha = float(np.nextafter(np.float32(.5), np.float32(0))) if profile == "flat-below" else .5
            primitive["attributes"]["COLOR_0"] = asset.accessor("VEC4", [[1, 1, 1, alpha]] * 4)
            material.update(alphaMode="MASK", alphaCutoff=.5)
        elif profile == "blend":
            material["pbrMetallicRoughness"]["baseColorFactor"][3] = .75
            material["alphaMode"] = "BLEND"
        elif profile == "transmission":
            extension = "KHR_materials_transmission"
            asset.model["extensionsUsed"].append(extension)
            material.update(alphaMode="OPAQUE", emissiveFactor=material["pbrMetallicRoughness"]["baseColorFactor"][:3],
                            extensions={extension: {"transmissionFactor": .9}})
    return configure


def verify_ray_materials(output, capture, center, card_size, target):
    (x, y), receiver, hit, _ = analytic_hit(center, target)
    assert hit[1] < 0, hit
    region = np.s_[y - 1:y + 2, x - 1:x + 2]

    def texture(asset, primitive, filename):
        asset.model.update(images=[{"uri": filename}], textures=[{"source": 0}])
        primitive["attributes"]["TEXCOORD_0"] = asset.accessor("VEC2", [[0, 0], [1, 0], [1, 1], [0, 1]])

    def render(label, light, *, profile=None, hybrid=True):
        return capture(f"ray-policy-{label}", "high", model=f"ray-policy-{profile or label}.gltf", target=target,
                       linear=True, bounce=0, directional=1, light_direction=-light, hybrid=hybrid)[region]

    spec_gloss = "KHR_materials_pbrSpecularGlossiness"
    for name, alpha in (("spec-gloss-smooth", 230), ("spec-gloss-rough", 26)):
        filename = f"ray-policy-{name}.png"
        Image.new("RGBA", (4, 4), (255, 255, 255, alpha)).save(output / filename)

        def gloss_setup(asset, primitive, material):
            texture(asset, primitive, filename)
            asset.model["extensionsUsed"].append(spec_gloss)
            material["extensions"] = {spec_gloss: {"diffuseFactor": [.005, .005, .005, 1.],
                                                   "specularFactor": [.6, .6, .6], "glossinessFactor": 1.,
                                                   "specularGlossinessTexture": {"index": 0}}}

        fixture(output / f"ray-policy-{name}.gltf", .045, card_center=center, card_size=card_size, lit_card=True,
                card_setup=gloss_setup)

    # Keep the half vector on the hit panel's +Z normal. The directional
    # light also reaches the floor, so compare the added reflected radiance.
    view = (EYE - receiver) * [1, -1, 1]
    view /= np.linalg.norm(view)
    aligned_light = view * [-1, -1, 1]
    assert np.allclose((view + aligned_light)[:2], 0) and aligned_light[2] > 0
    baseline = render("spec-gloss-screen", aligned_light, profile="spec-gloss-smooth", hybrid=False)
    smooth = float((render("spec-gloss-smooth", aligned_light) - baseline).mean())
    rough = float((render("spec-gloss-rough", aligned_light) - baseline).mean())
    # At this view angle the capped GGX denominator predicts a glossy-to-rough
    # highlight ratio of about 2.25 for the two authored alpha values.
    assert rough > .02 and smooth > max(.10, 1.65 * rough), ("Incorrect packed glossiness response", smooth, rough)

    normal_png = "ray-policy-tangent-normal.png"
    normal_rgb = (242, 128, 183)
    Image.new("RGB", (4, 4), normal_rgb).save(output / normal_png)

    def normal_setup(asset, primitive, material):
        texture(asset, primitive, normal_png)
        primitive["attributes"]["TANGENT"] = asset.accessor("VEC4", [[1, 0, 0, 1]] * 4)
        material.update(normalTexture={"index": 0}, doubleSided=True)

    for name, back in (("tangent-front", False), ("tangent-back", True)):
        fixture(output / f"ray-policy-{name}.gltf", .045, card_color=(.65, .55, .45, 1), card_center=center,
                card_size=card_size, lit_card=True, card_setup=normal_setup, card_back=back)
    tangent_light = np.array([.93, 0., .36])
    tangent_light /= np.linalg.norm(tangent_light)
    opposite_light = tangent_light * [-1, 1, 1]
    authored_normal = np.asarray(normal_rgb) / 127.5 - 1.
    assert authored_normal @ view > 0 and authored_normal @ tangent_light > .9 and authored_normal @ opposite_light < 0
    front = render("tangent-front", tangent_light)
    back = render("tangent-back", tangent_light)
    dark = render("tangent-opposite", opposite_light, profile="tangent-front")
    assert float(front.mean()) > .05 and float(dark.max()) < .005, (front, dark)
    np.testing.assert_allclose(back, front, atol=.003, rtol=.015, err_msg="Back-face tangent map differs from front")

    return dict(receiverPixel=[x, y], specularGlossiness=dict(smoothAlpha=230, roughAlpha=26,
                smoothRadianceGain=smooth, roughRadianceGain=rough),
                tangentNormal=dict(frontRgb=front.mean(axis=(0, 1)).tolist(), backRgb=back.mean(axis=(0, 1)).tolist(),
                                   oppositeLightRgb=dark.mean(axis=(0, 1)).tolist()))


def verify_reflected_shadow_facing(output, capture):
    center, target = np.array([.35, 2.8, -1.5]), np.array([0., -.5, 0.])
    card_size, caster_size = (.45, .30), (.28, .20)
    caster = center + [0., 0., .7]
    for point, size in ((center, card_size), (caster, caster_size)):
        assert all(project_point(point + [dx, dy, 0.], target)[1] < 0
                   for dx in (-size[0], size[0]) for dy in (-size[1], size[1])), point
    (x, y), receiver, _, _ = analytic_hit(center, target)
    direction = (receiver - EYE) * [1, -1, 1]
    caster_intersection = receiver + direction * ((caster[2] - receiver[2]) / direction[2])
    assert caster_intersection[1] < caster[1] - caster_size[1] - .15, caster_intersection
    roi = np.s_[y - 3:y + 4, x - 3:x + 4]

    def one_sided(_asset, _primitive, material):
        material["doubleSided"] = False

    captures = {}
    for name in ("clear", "two-sided", "toward-light", "away-from-light"):
        options = {}
        if name != "clear":
            options = dict(blocker=(caster, caster_size, 1), blocker_back=name == "away-from-light",
                           blocker_setup=one_sided if name != "two-sided" else None)
        label = f"ray-shadow-facing-{name}"
        fixture(output / f"{label}.gltf", .045, card_color=(.9, .16, .07, 1), card_center=center,
                card_size=card_size, lit_card=True, **options)
        captures[name] = capture(label, "high", model=f"{label}.gltf", target=target, directional=3,
                                 bounce=0, hybrid=True, linear=True)[roi]

    means = {name: frame.mean(axis=(0, 1)).tolist() for name, frame in captures.items()}
    clear, shaded = means["clear"][0], means["two-sided"][0]
    assert clear > .2 and shaded < clear * .05, means
    # The shadow ray travels toward +Z. A caster with +Z winding faces the
    # light and must match the two-sided caster, despite being a query back face.
    gain = {name: (clear - means[name][0]) / (clear - shaded) for name in ("toward-light", "away-from-light")}
    error = {name: float(np.abs(captures[name] - captures[control]).max())
             for name, control in (("toward-light", "two-sided"), ("away-from-light", "clear"))}
    report = dict(receiverPixel=[x, y], receiverPixels=49, meanSceneLinearRgb=means, normalizedShadowGain=gain,
                  maximumLinearError=error)
    assert gain["toward-light"] > .95 and abs(gain["away-from-light"]) < .05 and max(error.values()) < .005, report
    return report


def verify_ray_policy(output, capture, evidence, missing_renderer):
    target = np.array([0., -.5, 0.])
    center = np.array([.35, 2.8, -1.5])
    card_size = (.45, .30)
    plane = [-1, 0, 0, .35]
    virtual = center * [1, -1, 1]
    floor_hit = EYE + (virtual - EYE) * (EYE[1] / (EYE[1] + center[1]))
    blocker = floor_hit + (center - floor_hit) * .86
    blocker_size = (.50, .34)
    for point, size in ((center, card_size), (blocker, blocker_size)):
        assert all(project_point(point + [dx, dy, 0], target)[1] < 0
                   for dx in (-size[0], size[0]) for dy in (-size[1], size[1])), point

    # White in RGB isolates alpha coverage; 128/255 is above the cutoff
    # when alpha remains linear. A UV0 or transform regression misses the stripe.
    rgba = np.full((4, 16, 4), 255, dtype=np.uint8)
    rgba[:, :, 3] = 0
    rgba[:, 7:10, 3] = 128
    Image.fromarray(rgba).save(output / "ray-policy-alpha.png")
    profiles = ("opaque", "textured", "flat-below", "flat-equal", "blend", "transmission")
    for profile in ("clear", *profiles):
        options = {} if profile == "clear" else dict(blocker=(blocker, blocker_size, 1), blocker_color=(.008, 1, .015),
                                                     blocker_setup=ray_policy_blocker(profile))
        fixture(output / f"ray-policy-{profile}.gltf", .045, card_center=center, card_size=card_size, **options)

    left, right = (analytic_hit(center + [dx, 0, 0], target) for dx in (-.24, .24))
    probes, footprints = {}, {}
    for side, ((x, y), receiver, projected, _) in (("left", left), ("right", right)):
        assert projected[1] < 0 and receiver[0] < plane[3], (side, receiver, projected)
        direction = (receiver - EYE) * [1, -1, 1]
        target_hit = receiver + direction * ((center[2] - receiver[2]) / direction[2])
        nearer_hit = receiver + direction * ((blocker[2] - receiver[2]) / direction[2])
        u = .5 + (nearer_hit[0] - blocker[0]) / (2 * blocker_size[0])
        transformed_u = .375 + .5 * u
        assert (target_hit[0] < plane[3]) == (side == "left"), (side, target_hit)
        assert (.4375 < transformed_u < .625) == (side == "left"), (side, u, transformed_u)
        assert not (.4375 < u < .625), (side, u)
        probes[side] = dict(receiverPixel=[x, y], projectedHitPixel=projected.tolist(), target=target_hit.tolist(),
                            blockerUv=u, transformedU=transformed_u)
        footprints[side] = np.s_[y - 1:y + 2, x - 1:x + 2]

    def render(label, mode="high", *, profile="clear", hybrid=True, **options):
        return capture(f"ray-policy-{label}", mode, model=f"ray-policy-{profile}.gltf", target=target, linear=True,
                       environment=1, bounce=0, hybrid=hybrid, **options)

    screen = render("screen-only", hybrid=False)
    clear = render("hit")
    source = render("source", "source")
    disabled = render("queries-disabled", no_query=True)
    np.testing.assert_array_equal(screen, disabled)
    images = {profile: render(profile, profile=profile) for profile in profiles}
    section_plan = {"schemaVersion": 1, "frames": 5, "sampleFrames": [5],
                    "events": [{"frame": 1, "sectionPlane": plane}]}
    sectioned, section_route = render("sectioned", sequence=section_plan)
    section_screen, section_screen_route = render("sectioned-screen", sequence=section_plan, no_query=True)
    assert section_route[5]["route"]["rayFallbackActive"] and not section_screen_route[5]["route"]["rayFallbackActive"]

    for side, roi in footprints.items():
        base, hit, opaque = (frame[roi].mean(axis=(0, 1)) for frame in (screen, clear, images["opaque"]))
        assert .5 < base.min() and np.max(base) - np.min(base) < .01, (side, base)
        assert hit[0] > max(hit[1:]) + .5 and opaque[1] > max(opaque[0], opaque[2]) + .5, (side, hit, opaque)
        np.testing.assert_allclose(source[roi], np.broadcast_to([0, 0, 1], source[roi].shape), atol=.002, rtol=0)
        references = {"textured": images["opaque"] if side == "left" else clear,
                      "flat-below": clear, "flat-equal": images["opaque"], "blend": clear, "transmission": clear}
        for profile, reference in references.items():
            np.testing.assert_allclose(images[profile][roi], reference[roi], atol=.002, rtol=0,
                                       err_msg=f"{profile} coverage on the {side}")
        clipped = sectioned[5][roi].mean(axis=(0, 1))
        if side == "left":
            assert clipped[0] > max(clipped[1:]) + .5, (side, clipped)
        else:
            np.testing.assert_array_equal(sectioned[5][roi], section_screen[5][roi])
            assert clipped.min() > .5 and np.max(clipped) - np.min(clipped) < .01, (side, clipped)
        probes[side].update(screenRgb=base.tolist(), rayRgb=hit.tolist(), sectionRgb=clipped.tolist(),
                            blockers={name: image[roi].mean(axis=(0, 1)).tolist() for name, image in images.items()})

    def ray_scene(label):
        telemetry = Path(evidence[f"ray-policy-{label}"]["telemetry"])
        return json.loads(telemetry.read_text(encoding="utf-8"))["rayShadows"]

    ray_receipt = ray_scene("hit")
    assert ray_receipt["supported"] and ray_receipt["generation"] > 0 and ray_receipt["totalAllocatedBytes"] > 0, ray_receipt
    build = ray_receipt["build"]
    assert ray_scene("opaque")["build"]["instances"] > build["instances"], (ray_receipt, ray_scene("opaque"))
    for profile in ("blend", "transmission"):
        candidate = ray_scene(profile)["build"]
        for counter in ("instances", "asAllocatedBytes", "inputAllocatedBytes"):
            assert candidate[counter] == build[counter], (profile, counter, build, candidate)

    self_target = np.zeros(3)
    self_normal = np.array([0., .95, -.31])
    self_normal /= np.linalg.norm(self_normal)
    incoming = -EYE / np.linalg.norm(EYE)
    reflected = incoming - 2 * (incoming @ self_normal) * self_normal
    assert reflected[1] < -.05 < 0 < reflected @ self_normal, reflected
    legacy_intersection = float(-.00025 * self_normal[1] / reflected[1])
    assert .00025 < legacy_intersection < .004, legacy_intersection
    self_model = "ray-policy-self-hit.gltf"
    fixture(output / self_model, .045, card_center=np.array([0., 3., 7.5]), floor_normal=self_normal.tolist(),
            floor_double_sided=True)
    self_source = capture("ray-policy-self-hit", "source", model=self_model, target=self_target, hybrid=True,
                          linear=True, bounce=0)
    x, y = project(self_target, self_target)
    assert np.all(self_source[y - 2:y + 3, x - 2:x + 3, 2] == 0.), (x, y, self_source[y, x])
    result = dict(probes=probes, sectionPlane=plane, capabilityFallbackPixelExact=True, rayScene=ray_receipt,
                  materials=verify_ray_materials(output, capture, center, card_size, target),
                  reflectedShadowFacing=verify_reflected_shadow_facing(output, capture),
                  selfHit=dict(receiverPixel=[x, y], oldHitDistanceMeters=legacy_intersection,
                               rayConfidence=float(self_source[y, x, 2])))

    if missing_renderer is not None:
        off = capture("ray-policy-onscreen-off", "off", no_query=True, linear=True, bounce=0)
        on = capture("ray-policy-onscreen-screen", "high", no_query=True, linear=True, bounce=0)
        missing = capture("ray-policy-onscreen-missing-spv", "high", hybrid=True, linear=True, bounce=0,
                          alternate=missing_renderer, ray_available=False)
        hidden = render("offscreen-missing-spv", alternate=missing_renderer, ray_available=False)
        np.testing.assert_array_equal(missing, on)
        np.testing.assert_array_equal(hidden, screen)
        (x, y), _, _, _ = analytic_hit()
        assert missing[y, x, 0] > off[y, x, 0] + .5, (x, y, missing[y, x], off[y, x])
        for label in ("ray-policy-onscreen-missing-spv", "ray-policy-offscreen-missing-spv"):
            sample = evidence[label]
            assert sample["resources"]["depthPyramidUsed"], sample
            assert "Screen reflection ray fallback unavailable" in Path(sample["log"]).read_text(encoding="utf-8"), sample
        result["missingRayShaderFallback"] = dict(onscreenPixelExact=True, offscreenPixelExact=True,
                                                  screenHitPixel=[x, y], screenHitRgb=missing[y, x].tolist())
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--renderer", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--ray-fallback", action="store_true", help="Also verify off-screen ray hits on a ray-query device")
    parser.add_argument("--ray-policy", action="store_true", help="Check ray reflection coverage and raster fallback policy")
    parser.add_argument("--ray-shadow-facing", action="store_true", help="Check reflected one-sided shadow casting")
    parser.add_argument("--bim-overlay", action="store_true", help="Check that BIM floor plans stay out of reflections")
    parser.add_argument("--missing-ray-renderer", type=Path,
                        help="With --ray-policy, check a staged runtime without the optional ray reflection shader")
    parser.add_argument("--temporal", action="store_true", help="Also verify reflection history with motion and cuts")
    parser.add_argument("--denoise", action="store_true", help="Measure scene-linear disocclusion trails and denoising")
    parser.add_argument("--marcher", action="store_true", help="Run the focused raster-only Hi-Z and numeric EXR checks")
    parser.add_argument("--energy", action="store_true", help="Check scene-linear furnace energy and screen-edge fading")
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
    if args.energy and (args.marcher or args.temporal or args.ray_fallback):
        parser.error("--energy runs separately from --marcher, --temporal, and --ray-fallback")
    if args.ray_policy and (args.energy or args.marcher or args.temporal or args.ray_fallback):
        parser.error("--ray-policy runs separately from the other suites")
    if args.ray_shadow_facing and (args.ray_policy or args.energy or args.marcher or args.temporal or args.ray_fallback or args.denoise):
        parser.error("--ray-shadow-facing runs separately from the other suites")
    if args.denoise and (args.energy or args.marcher or args.temporal or args.ray_fallback or args.ray_policy):
        parser.error("--denoise runs separately from the other suites")
    if args.bim_overlay and (args.energy or args.marcher or args.temporal or args.ray_fallback or args.ray_policy
                             or args.ray_shadow_facing or args.denoise):
        parser.error("--bim-overlay runs separately from the other suites")
    if args.missing_ray_renderer and not args.ray_policy:
        parser.error("--missing-ray-renderer requires --ray-policy")
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
                linear=False, alternate=None, hdr=None, bounce=None, ray_available=None, light_direction=(0, 0, -1),
                no_history=False, msaa=1, bim_model=None, tone_map=None):
        executable = alternate if alternate else renderer
        image = output / (f"{label}.exr" if linear else f"{label}.png")
        command = [str(executable), "--hidden", "--no-ui", "--validation", "--import-scale", "1",
                   "--width", str(extent[0]), "--height", str(extent[1]), "--msaa", str(msaa), "--taa" if taa else "--no-taa",
                   "--no-bloom", "--no-ddgi", "--ray-shadows", "raster", "--display-mode", "lit",
                   "--environment-intensity", str(environment), "--directional-intensity", str(directional),
                   "--directional-direction", *map(str, light_direction), "--exposure", "1",
                   "--camera-position", *map(str, EYE), "--camera-target", *map(str, target),
                   "--camera-fov", str(FOV), "--render-technique", "forward-raster" if forward else "deferred-raster",
                   "--screen-reflections", mode,
                   "--screenshot", str(image), "--model",
                   str(output / model)]
        if hdr is not None:
            command.extend(("--environment", str(hdr)))
        else:
            command.extend(("--sky", "plain", "--sky-horizon", "1", "1", "1"))
        if bounce is not None:
            command.extend(("--bounce-intensity", str(bounce)))
        if no_history:
            command.append("--no-screen-reflections-history")
        if bim_model is not None:
            command.extend(("--bim-model", str(bim_model), "--bim-import-scale", "1"))
        if tone_map is not None:
            command.extend(("--tone-map", tone_map))
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
        if args.marcher or args.energy or args.ray_policy or args.ray_shadow_facing or args.denoise or args.bim_overlay or validation:
            matched = VALIDATION_INSERTED.search(log)
            assert matched, f"Could not confirm Khronos validation layer insertion: {log_path}"
            layer = file_identity(executable.parent / Path(matched[1]))
            if validation:
                assert layer == validation["library"], (log_path, layer)

        def read_receipt(path):
            receipt = json.loads(path.with_suffix(".telemetry.json").read_text(encoding="utf-8"))
            if hdr is not None:
                lighting = receipt["environment"]
                assert Path(lighting["hdrPath"]).resolve() == hdr and not lighting["loading"] \
                    and not lighting["generating"], (label, lighting)
            return receipt

        if sequence is not None:
            images, routes = {}, {}
            for frame in sequence["sampleFrames"]:
                path = image if frame == sequence["frames"] else output / f"{label}.frame-{frame:04d}{image.suffix}"
                receipt = read_receipt(path)
                route = receipt.get("screenReflections")
                pass_state = next((p for p in receipt["passes"] if p["name"] == "ScreenReflections"), None)
                routes[frame] = {"route": route, "pass": pass_state}
                if args.denoise:
                    routes[frame].update(msaaSamples=receipt["msaaSamples"], supportedMsaaSamples=receipt["supportedMsaaSamples"])
                    routes[frame]["surfaceIdPass"] = next(
                        (p for p in receipt["passes"] if p["name"] == "ReflectionSurfaceIds"), None)
                if linear or args.denoise or args.bim_overlay:
                    assert receipt["validationEnabled"], receipt
                    routes[frame].update(image=str(path), telemetry=str(path.with_suffix(".telemetry.json")),
                                         log=str(log_path), command=command, validationLayer=layer)
                if linear:
                    assert receipt["resolution"] == list(extent), receipt
                    images[frame] = read_linear(path, extent)
                else:
                    with Image.open(path) as result:
                        images[frame] = np.array(result.convert("RGB"))
            evidence[label] = routes
            return images, routes
        receipt = read_receipt(image)
        active = mode != "off" and not forward
        pass_state = next((entry for entry in receipt["passes"] if entry["name"] == "ScreenReflections"), None)
        assert bool(pass_state and pass_state["active"]) == active, (label, pass_state)
        if active:
            assert pass_state["gpuTimed"] and pass_state["gpuMs"] > 0, pass_state
            assert receipt["screenReflections"]["allocationBytes"] >= math.prod(extent) * 8, receipt["screenReflections"]
        if hybrid:
            route = receipt["screenReflections"]
            available = not no_query if ray_available is None else ray_available
            assert route["rayFallbackRequested"]
            assert route["rayFallbackAvailable"] == available, route
            assert route["rayFallbackActive"] == (available and active), route
        evidence[label] = {"pass": pass_state, "resources": receipt.get("screenReflections"), "image": str(image),
                           "telemetry": str(image.with_suffix(".telemetry.json")), "log": str(log_path), "command": command}
        if layer:
            evidence[label]["validationLayer"] = layer
        if linear:
            assert receipt["resolution"] == list(extent) and receipt["validationEnabled"], receipt
            return read_linear(image, extent)
        with Image.open(image) as result:
            return np.array(result.convert("RGB"))

    if args.marcher or args.energy or args.ray_policy or args.ray_shadow_facing or args.denoise or args.bim_overlay:
        fallback = args.mip_zero_renderer.resolve(strict=True) if args.mip_zero_renderer else None
        missing = args.missing_ray_renderer.resolve(strict=True) if args.missing_ray_renderer else None
        renderer_id = file_identity(renderer)
        result = dict(passed=False, renderer=renderer_id, captures=evidence)
        try:
            if fallback:
                fallback_id = file_identity(fallback)
                assert fallback_id["sha256"] == renderer_id["sha256"], "The staged mip-zero control uses another executable"
                assert not (fallback.parent / "spv_shaders/screen_reflections_hiz.comp.spv").is_file(), fallback
                result["mipZeroRenderer"] = fallback_id
            if missing:
                missing_id = file_identity(missing)
                assert missing_id["sha256"] == renderer_id["sha256"], "The missing-ray control uses another executable"
                expected_shader = "spv_shaders/screen_reflections_ray.comp.spv"
                assert (renderer.parent / expected_shader).is_file() and not (missing.parent / expected_shader).exists()
                for shader in ("screen_reflections.comp.spv", "screen_reflections_hiz.comp.spv"):
                    original = file_identity(renderer.parent / "spv_shaders" / shader)
                    staged = file_identity(missing.parent / "spv_shaders" / shader)
                    assert original["sha256"] == staged["sha256"], (original, staged)
                result["missingRayRenderer"] = missing_id
            if args.marcher:
                result["marcher"] = verify_marcher(output, capture, evidence, fallback)
            elif args.energy:
                result["energy"] = verify_energy(output, capture, evidence)
            elif args.ray_policy:
                result["rayPolicy"] = verify_ray_policy(output, capture, evidence, missing)
            elif args.ray_shadow_facing:
                result["reflectedShadowFacing"] = verify_reflected_shadow_facing(output, capture)
            elif args.bim_overlay:
                result["bimOverlay"] = verify_bim_overlay(output, capture)
            else:
                result["denoise"] = verify_denoising(output, capture)
            assert file_identity(renderer) == renderer_id, "Renderer changed during SSR capture"
            if fallback:
                assert file_identity(fallback) == fallback_id, "Staged mip-zero executable changed during capture"
            if missing:
                assert file_identity(missing) == missing_id, "Staged missing-ray executable changed during capture"
            result["passed"] = True
        except Exception as error:
            result["error"] = str(error)
            raise
        finally:
            (output / "results.json").write_text(json.dumps(result, indent=2, allow_nan=False) + "\n", encoding="utf-8")
        if args.marcher:
            print("PASS analytic SSR hit UV/distance, roughness, clipped exit, and Hi-Z at even and odd extents")
        elif args.energy:
            print("PASS constant-radiance dielectric and metal energy, roughness, and screen-edge fading")
        elif args.ray_policy:
            print("PASS off-screen ray coverage, section clipping, and requested raster fallback checks")
        elif args.ray_shadow_facing:
            print("PASS directional shadows on reflected one-sided geometry")
        elif args.bim_overlay:
            print("PASS BIM floor-plan display, depth occlusion, and reflection isolation")
        else:
            print("PASS bounded linear reflection trails, stationary denoising, history fallback, and lifecycle")
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
