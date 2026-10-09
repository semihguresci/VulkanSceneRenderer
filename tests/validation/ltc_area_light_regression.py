"""Independent area-BRDF quadrature, LTC captures, parity and GPU cost probes."""
from __future__ import annotations
import argparse
import base64
import copy
import hashlib
import json
import os
from pathlib import Path
import struct
import subprocess
import shutil

try:
    import numpy as np
    from PIL import Image
except ImportError:
    print("SKIP: LTC area-light regressions require numpy and Pillow")
    raise SystemExit(77)
from area_shadow_regression import hdr, floor_points


def unit(values):
    values = np.asarray(values, dtype=np.float64)
    return values / np.maximum(np.linalg.norm(values, axis=-1, keepdims=True), 1e-30)


def case(name, **changes):
    result = dict(name=name, disk=False, halfSize=[.4, .3], light=[0, 0, 1.5],
        yaw=0., camera=[0, 0, 5], albedo=[.55, .3, .12], metallic=0., roughness=.6,
        specular=1., clearcoat=0., clearcoatRoughness=.3, sheen=[0, 0, 0],
        sheenRoughness=.5, iridescence=0., iridescenceIor=1.3,
        iridescenceThicknessMinimum=100., iridescenceThicknessMaximum=400.,
        intensity=8., range=0., alpha=1., mirrored=False, doubleSided=True)
    result.update(changes)
    return result


def quality_cases():
    cases = []
    for disk in (False, True):
        shape = "disk" if disk else "rect"
        half = [.4, .4] if disk else [.4, .3]
        cases += [case(shape + "-diffuse", disk=disk, halfSize=half, specular=0.),
                  case(shape + "-near", disk=disk, halfSize=[.6, .6 if disk else .4],
                       light=[0, 0, .2], roughness=.35, intensity=1.),
                  case(shape + "-horizon", disk=disk, halfSize=[.65, .65 if disk else .3],
                       light=[.35, 0, .12], yaw=np.pi / 3, metallic=1., roughness=.35, intensity=1.),
                  case(shape + "-grazing", disk=disk, halfSize=half,
                       camera=[4, 0, .8], light=[1.1, .2, 1.], yaw=np.pi / 4),
                  case(shape + "-opposite-diffuse", disk=disk, halfSize=[.15, .15 if disk else .12],
                       camera=[4, 0, .8], light=[-1.5, 0, .3], specular=0., intensity=40.),
                  case(shape + "-back-facing", disk=disk, halfSize=half, yaw=np.pi),
                  case(shape + "-below-receiver", disk=disk, halfSize=half,
                       light=[0, 0, -1.5], yaw=np.pi),
                  case(shape + "-mirrored-frame", disk=disk, halfSize=half,
                       light=[.2, .1, 1.5], mirrored=True)]
        for roughness in (.045, .15, .20, .25, .274, .28, .30, .35, .7, 1.):
            cases.append(case(f"{shape}-metal-r{roughness:g}", disk=disk, halfSize=half,
                              metallic=1., roughness=roughness))
    cases += [case("layered", clearcoat=.8, clearcoatRoughness=.2,
                   sheen=[.15, .08, .03], sheenRoughness=.4),
              case("layered-grazing", camera=[4, 0, .8], clearcoat=.8,
                   clearcoatRoughness=.2, sheen=[.15, .08, .03], sheenRoughness=.4),
              case("layered-ltc", roughness=.7, clearcoat=.8, clearcoatRoughness=.6,
                   sheen=[.15, .08, .03], sheenRoughness=.4),
              case("iridescence", iridescence=1., iridescenceIor=1.4,
                   iridescenceThicknessMinimum=100., iridescenceThicknessMaximum=420.),
              case("iridescence-metallic", metallic=1., roughness=.7,
                   iridescence=1., iridescenceIor=1.33,
                   iridescenceThicknessMinimum=100., iridescenceThicknessMaximum=400.),
              case("finite-range", range=2., light=[0, 0, 1.4]),
              case("rect-thin-far", halfSize=[.005, 200.], light=[4000., 0., 2002.],
                   yaw=float(np.arctan2(4000., 2002.)), metallic=1., roughness=1., intensity=2.0008e11),
              case("distance-clamp", light=[0, 0, .08], halfSize=[.08, .06], intensity=.2),
              case("transparent", alpha=.7, clearcoat=.3, doubleSided=False),
              case("transparent-thin", alpha=.7, clearcoat=.3)]
    return cases


def material_f0(settings, nv):
    # Constant, texture-free iridescence uses the authored maximum thickness.
    # This checks the shared view-dependent F0 independently of G-buffer packing.
    phase = (settings["iridescenceThicknessMaximum"] * .018 +
             settings["iridescenceIor"] * 1.7 + (1 - np.clip(nv, 0, 1)) * 4)
    wave = .5 + .5 * np.cos(phase + np.array([0., 2.094, 4.188]))
    tint = 1 + settings["iridescence"] * (.65 + .7 * wave - 1)
    base = (.04 * settings["specular"] * (1 - settings["metallic"]) +
            np.array(settings["albedo"]) * settings["metallic"])
    return np.clip(base * tint, 0, 1)


def emitter_frame(settings):
    angle = settings["yaw"]
    normal = np.array([-np.sin(angle), 0, -np.cos(angle)])
    tangent = np.array([np.cos(angle), 0, -np.sin(angle)])
    if settings["mirrored"]: tangent *= -1
    return np.array(settings["light"]), normal, tangent, np.array([0., 1., 0.])


def emitter_samples(settings, resolution):
    center, normal, tangent, bitangent = emitter_frame(settings)
    nodes, weights = np.polynomial.legendre.leggauss(resolution)
    if settings["disk"]:
        radius = np.sqrt((nodes + 1) / 2)
        angle = 2 * np.pi * (np.arange(resolution * 4) + .5) / (resolution * 4)
        x, y = radius[:, None] * np.cos(angle), radius[:, None] * np.sin(angle)
        sample_weights = np.broadcast_to(weights[:, None] / (2 * len(angle)), x.shape)
        area = np.pi * settings["halfSize"][0] ** 2
    else:
        x, y = np.meshgrid(nodes, nodes)
        sample_weights = weights[:, None] * weights[None, :] / 4
        area = 4 * np.prod(settings["halfSize"])
    emitters = (center + x.ravel()[:, None] * tangent * settings["halfSize"][0] +
                y.ravel()[:, None] * bitangent * settings["halfSize"][1])
    return emitters, sample_weights.ravel() * area, normal


def brdf_lobe(normal, view, light, albedo, f0, metallic, roughness):
    """Scalar geometry with RGB Fresnel; matches the declared engine equations."""
    roughness = np.clip(roughness, .045, 1.)
    nl = np.maximum(light @ normal, 0.)
    nv = np.maximum(np.sum(view * normal, axis=-1), 0.)
    half = unit(view + light)
    nh = np.maximum(half @ normal, 0.)
    hv = np.maximum(np.sum(half * view, axis=-1), 0.)
    alpha_squared = roughness ** 4
    denominator = nh * nh * (alpha_squared - 1) + 1
    distribution = alpha_squared / np.maximum(np.pi * denominator * denominator, 1e-4)
    k = (roughness + 1) ** 2 / 8
    geometry = (nv / np.maximum(nv * (1 - k) + k, 1e-4) *
                nl / np.maximum(nl * (1 - k) + k, 1e-4))
    fresnel = f0 + (1 - f0) * (1 - hv[..., None]) ** 5
    specular = (distribution * geometry / np.maximum(4 * nv * nl, 1e-4))[..., None] * fresnel
    diffuse = (1 - fresnel) * (1 - metallic) * albedo / np.pi
    return (specular + diffuse) * nl[..., None] * (nv > 0)[..., None]


def integration_samples(settings, resolution=128, sampled_count=None):
    if sampled_count:
        order = int(np.sqrt(sampled_count))
        center, source_normal, tangent, bitangent = emitter_frame(settings)
        nodes, weights = np.polynomial.legendre.leggauss(order)
        if settings["disk"]:
            radius = np.sqrt((nodes + 1) / 2)
            angle = 2 * np.pi * np.arange(order) / order
            x, y = radius[:, None] * np.cos(angle), radius[:, None] * np.sin(angle)
            weights = np.broadcast_to(weights[:, None] / (2 * order), x.shape)
            area = np.pi * settings["halfSize"][0] ** 2
        else:
            x, y = np.meshgrid(nodes, nodes)
            weights = weights[:, None] * weights[None, :] / 4
            area = 4 * np.prod(settings["halfSize"])
        emitters = (center + x.ravel()[:, None] * tangent * settings["halfSize"][0] +
                    y.ravel()[:, None] * bitangent * settings["halfSize"][1])
        weights = weights.ravel() * area
    else:
        emitters, weights, source_normal = emitter_samples(settings, resolution)
    return emitters, weights, source_normal


def numerical_reference(points, settings, resolution=128, sampled_count=None):
    emitters, weights, source_normal = integration_samples(settings, resolution, sampled_count)
    albedo = np.array(settings["albedo"])
    metallic = settings["metallic"]
    normal = np.array([0., 0., 1.])
    results = []
    for point in np.asarray(points):
        view = unit(np.array(settings["camera"]) - point)
        f0 = material_f0(settings, max(view[2], 0.))
        delta = emitters - point
        distance_squared = np.sum(delta * delta, axis=1)
        light = unit(delta)
        source_cosine = np.maximum(light @ -source_normal, 0.)
        fade = (np.maximum(1 - distance_squared / settings["range"] ** 2, 0.) ** 2
                if settings["range"] > 0 else np.ones(len(light)))
        radiance = (settings["intensity"] * source_cosine * fade * weights /
                    np.maximum(distance_squared, .01))
        lobe = brdf_lobe(normal, view, light, albedo, f0, metallic, settings["roughness"])
        lobe *= 1 - settings["clearcoat"] * .04
        if settings["clearcoat"]:
            lobe += settings["clearcoat"] * brdf_lobe(normal, view, light,
                np.zeros(3), np.full(3, .04), 1., settings["clearcoatRoughness"])
        sheen = np.array(settings["sheen"])
        lobe *= 1 - np.max(sheen) * .25
        nv = max(np.dot(normal, view), 0.)
        lobe += sheen * (np.maximum(light @ normal, 0.) * (1 - nv) ** 2 *
                        (1 - .35 * settings["sheenRoughness"]) * .35)[:, None]
        if settings["alpha"] < 1 and settings["doubleSided"]:
            # The renderer's double-sided BLEND policy adds thin-surface
            # transmission after the reflective material layers. This fixture
            # views the authored front face, so its face weight is 0.45.
            wrapped = np.clip((light @ normal + .45) / 1.45, 0, 1)
            back = np.clip(light @ -normal, 0, 1)
            through = np.clip(light @ -view, 0, 1) ** 2
            transmission = np.maximum(wrapped * .35, back * through) * .45 * (1 - settings["alpha"] * .5)
            lobe += albedo * transmission[:, None]
        results.append(np.sum(lobe * radiance[:, None], axis=0))
    return np.array(results)


def read_table(path):
    data = Path(path).read_bytes()
    assert len(data) == 16 + 64 * 64 * 16
    assert struct.unpack("<4sIII", data[:16]) == (b"CLTC", 1, 64, 64)
    table = np.frombuffer(data[16:], dtype="<f4").reshape(64, 64, 4).astype(np.float64)
    assert np.isfinite(table).all()
    return table


def sampled_direct_components(points, settings, samples=None, resolution=128):
    """Independently isolate the directional Schlick residual using F0=0."""
    full = numerical_reference(points, settings, resolution=resolution, sampled_count=samples)
    unit_q_settings = dict(settings, albedo=[0, 0, 0], metallic=1., specular=0.,
                           clearcoat=0., sheen=[0, 0, 0])
    unit_q = numerical_reference(points, unit_q_settings, resolution=resolution, sampled_count=samples)
    views = unit(np.array(settings["camera"]) - np.asarray(points))
    f0 = np.array([material_f0(settings, max(view[2], 0)) for view in views])
    q = unit_q * (1 - f0) * (1 - settings["clearcoat"] * .04)
    if settings["clearcoat"]:
        unit_q_settings["roughness"] = settings["clearcoatRoughness"]
        coat_q = numerical_reference(points, unit_q_settings, resolution=resolution, sampled_count=samples)
        q += settings["clearcoat"] * .96 * coat_q
    q *= 1 - max(settings["sheen"]) * .25
    return full - q, q


def lookup(table, roughness, nv):
    x, y = np.clip(roughness, .045, 1.) * 63, np.sqrt(1 - np.clip(nv, 0, 1)) * 63
    xi, yi = int(x), int(y)
    xf, yf = x - xi, y - yi
    return ((1 - yf) * ((1 - xf) * table[yi, xi] + xf * table[yi, min(xi + 1, 63)]) +
            yf * ((1 - xf) * table[min(yi + 1, 63), xi] + xf * table[min(yi + 1, 63), min(xi + 1, 63)]))


def horizon_clip(vertices):
    result = []
    for start, end in zip(vertices, np.roll(vertices, -1, axis=0)):
        if start[2] >= 0: result.append(start)
        if (start[2] >= 0) != (end[2] >= 0):
            result.append(start + (end - start) * (-start[2] / (end[2] - start[2])))
    return np.array(result)


def cosine_polygon(vertices):
    if len(vertices) < 3: return 0.
    directions = unit(vertices)
    cross = np.cross(directions, np.roll(directions, -1, axis=0))
    length = np.linalg.norm(cross, axis=1)
    angles = np.arctan2(length, np.sum(directions * np.roll(directions, -1, axis=0), axis=1))
    return abs(np.sum(cross[:, 2] * angles / np.maximum(length, 1e-30))) / (2 * np.pi)


def adaptive_disk_vertices(center, axis_x, axis_y, transform=None):
    """Bound circle-to-chord deviation in the physical and fitted LTC spaces.

    Returns None if the finite 512-subdivision/sector budget cannot meet the
    shader's bound, so callers preserve the configured sampled response.
    """
    transform = np.eye(3) if transform is None else np.asarray(transform)
    center, axis_x, axis_y = map(np.asarray, (center, axis_x, axis_y))
    radius = np.linalg.norm(axis_x)
    normal = unit(np.cross(axis_x, axis_y))
    height = float(np.dot(center, normal))
    radial_gap = np.linalg.norm(center - normal * height) - radius
    boundary = np.hypot(height, radial_gap)
    transformed_axes = transform @ np.column_stack((axis_x, axis_y))
    curvature = radius * (np.pi / 32) ** 2 / 2
    transformed_curvature = np.linalg.norm(transformed_axes, ord=2) * 1.000001 * (np.pi / 32) ** 2 / 2
    transformed_boundary = boundary * np.linalg.svd(transform, compute_uv=False)[-1] * .999999
    angles = np.arange(32) * 2 * np.pi / 32
    starts = center + np.cos(angles)[:, None] * axis_x + np.sin(angles)[:, None] * axis_y
    ends = np.roll(starts, -1, axis=0)
    def chord_distance(a, b):
        chord = b - a
        t = np.clip(-np.sum(a * chord, axis=1) /
                    np.maximum(np.sum(chord * chord, axis=1), 1e-20), 0, 1)
        return np.linalg.norm(a + t[:, None] * chord, axis=1)
    distances = np.maximum(boundary, chord_distance(starts, ends) - curvature)
    transformed_distances = np.maximum(transformed_boundary,
        chord_distance(starts @ transform.T, ends @ transform.T) - transformed_curvature)
    relative = np.maximum(curvature / np.maximum(distances, 1e-20),
                          transformed_curvature / np.maximum(transformed_distances, 1e-20))
    subdivisions = np.ones(32, dtype=int)
    for _ in range(9):
        refine = relative > .002
        subdivisions[refine] *= 2
        relative[refine] *= .25
    if not np.isfinite(relative).all() or np.any(relative > .002): return None
    refined = np.concatenate([angle + np.arange(count) * 2 * np.pi / (32 * count)
                              for angle, count in zip(angles, subdivisions)])
    return center + np.cos(refined)[:, None] * axis_x + np.sin(refined)[:, None] * axis_y


def smoothstep(low, high, value):
    value = np.clip((value - low) / (high - low), 0, 1)
    return value * value * (3 - 2 * value)


def fresnel_interval(point, view, settings):
    center, source_normal, _, _ = emitter_frame(settings)
    vector = center - point
    distance_squared = np.dot(vector, vector)
    radius = settings["halfSize"][0] if settings["disk"] else np.linalg.norm(settings["halfSize"])
    low, high = -1., 1.
    def cone(dot, cosine, sine):
        dot = np.clip(dot, -1, 1)
        other = np.sqrt(max(1 - dot * dot, 0))
        return (-1 if dot <= -cosine else dot * cosine - other * sine,
                 1 if dot >= cosine else dot * cosine + other * sine)
    if distance_squared > radius * radius:
        sine = radius / np.sqrt(distance_squared)
        sphere_low, sphere_high = cone(np.dot(view, vector) / np.sqrt(distance_squared),
                                      np.sqrt(max(1 - sine * sine, 0)), sine)
        # Widen the valid sphere constraint conservatively as it activates;
        # otherwise crossing its boundary can abruptly change material policy.
        strength = smoothstep(1., 1.1, np.sqrt(distance_squared) / radius)
        low, high = -1 + (sphere_low + 1) * strength, 1 + (sphere_high - 1) * strength
    axis = -source_normal
    distance = np.dot(vector, axis)
    if distance > 0:
        planar_radius = np.linalg.norm(vector - axis * distance) + radius
        length = np.sqrt(distance * distance + planar_radius * planar_radius)
        plane_low, plane_high = cone(np.dot(view, axis), distance / length, planar_radius / length)
        low, high = max(low, plane_low), min(high, plane_high)
    low = max(low, -np.sqrt(max(1 - view[2] * view[2], 0)))
    if low > high: return np.zeros(2)
    return (1 - np.sqrt(np.maximum(.5 * (1 + np.array([high, low])), 0))) ** 5


def projected_angular_confidence(point, settings):
    """Conservative lower bound avoids a nearly singular projected Gram matrix."""
    center, normal, _, _ = emitter_frame(settings)
    delta = np.asarray(point) - center
    distance = np.linalg.norm(delta)
    minimum, maximum = min(settings["halfSize"]), max(settings["halfSize"])
    harmonic_half = minimum / (1 + minimum / maximum)
    source_cosine = max(float(np.dot(normal, delta) / distance), 0.)
    bound = harmonic_half / distance * source_cosine
    return float(smoothstep(1e-4, 2e-4, bound)), bound


def ltc_reference(points, settings, matrix_table, amplitude_table, guarded=False, samples=25, disk_segments=None,
                  policy_details=False):
    """Independent CPU polygon evaluation for asset QA before GPU compilation."""
    center, source_normal, tangent, bitangent = emitter_frame(settings)
    if settings["disk"]:
        count = disk_segments or 32
        angles = np.arange(count) * 2 * np.pi / count
        x, y = np.cos(angles), np.sin(angles)
    else:
        x, y = np.array([-1, 1, 1, -1]), np.array([-1, -1, 1, 1])
    vertices = center + x[:, None] * tangent * settings["halfSize"][0] + y[:, None] * bitangent * settings["halfSize"][1]
    albedo, metallic = np.array(settings["albedo"]), settings["metallic"]
    results, weights, geometry_weights = [], [], []
    fallback_stable, fallback_q = sampled_direct_components(points, settings, samples) if guarded else (None, None)
    for point_index, point in enumerate(points):
        view = unit(np.array(settings["camera"]) - point)
        nv = max(view[2], 0.)
        f0 = material_f0(settings, nv)
        if np.dot(source_normal, point - center) <= 0 or nv <= 0:
            results.append(np.zeros(3)); weights.append(1.); geometry_weights.append(1.); continue
        basis_x = unit([view[0], view[1], 0.]) if np.linalg.norm(view[:2]) > 1e-8 else np.array([1., 0., 0.])
        basis_y = np.cross([0., 0., 1.], basis_x)
        basis = np.column_stack((basis_x, basis_y, [0., 0., 1.]))
        geometry_safe = True
        local_center = (center - point) @ basis
        local_x = tangent * settings["halfSize"][0] @ basis
        local_y = bitangent * settings["halfSize"][1] @ basis
        local_vertices = (vertices - point) @ basis
        if settings["disk"] and disk_segments is None:
            local_vertices = adaptive_disk_vertices(local_center, local_x, local_y)
            geometry_safe = local_vertices is not None
        physical = horizon_clip(local_vertices) if geometry_safe else np.empty((0, 3))
        diffuse = cosine_polygon(physical)
        interval = fresnel_interval(point, view, settings)
        relative_errors, absolute_errors = [], []
        def specular(roughness, reflectance):
            nonlocal geometry_safe
            if len(physical) < 3: return np.zeros(3), np.zeros(3)
            m = lookup(matrix_table, roughness, nv)
            transform = np.array([[m[0], 0, m[2]], [0, 1, 0], [m[1], 0, m[3]]])
            fitted_physical = physical
            if settings["disk"] and disk_segments is None:
                fitted_vertices = adaptive_disk_vertices(local_center, local_x, local_y, transform)
                if fitted_vertices is None:
                    geometry_safe = False
                    return np.zeros(3), np.zeros(3)
                fitted_physical = horizon_clip(fitted_vertices)
            integral = cosine_polygon(horizon_clip(fitted_physical @ transform.T))
            a = lookup(amplitude_table, roughness, nv)
            if guarded:
                moment = np.clip(a[1] / max(a[0], 1e-20), *interval)
                error = (1 - reflectance) * max(abs(moment - interval))
                minimum = reflectance + (1 - reflectance) * interval[0]
                relative_errors.append(float(np.max(error / np.maximum(minimum, 1e-20))))
                absolute_errors.append(error)
                return integral * a[0] * reflectance, integral * a[0] * (1 - reflectance) * moment
            return integral * a[0] * reflectance, integral * (1 - reflectance) * a[1]
        a = lookup(amplitude_table, settings["roughness"], nv)
        diffuse_scale = (1 - f0) * (1 - metallic) * albedo
        correct_diffuse = guarded and max(abs(a[2] - interval)) > .01 * (1 - interval[1])
        stable_specular, q_value = specular(settings["roughness"], f0)
        value = diffuse * (1 if correct_diffuse else 1 - a[2]) * diffuse_scale + stable_specular
        value *= 1 - settings["clearcoat"] * .04
        q_value *= 1 - settings["clearcoat"] * .04
        if settings["clearcoat"]:
            coat_stable, coat_q = specular(settings["clearcoatRoughness"], np.full(3, .04))
            value += settings["clearcoat"] * coat_stable
            q_value += settings["clearcoat"] * coat_q
        sheen = np.array(settings["sheen"])
        value *= 1 - np.max(sheen) * .25
        q_value *= 1 - np.max(sheen) * .25
        value += diffuse * np.pi * sheen * (1 - nv) ** 2 * (1 - .35 * settings["sheenRoughness"]) * .35
        weight, range_scale = 1., 1.
        if guarded:
            minimum_roughness = min(settings["roughness"], settings["clearcoatRoughness"]
                if settings["clearcoat"] > 0 else 1.)
            weight = float(smoothstep(.28, .34, minimum_roughness))
            if not geometry_safe: weight = 0.
            weight = min(weight, projected_angular_confidence(point, settings)[0])
            if settings["alpha"] < 1 and settings["doubleSided"]:
                weight = 0.  # Thin-surface lighting retains sampled integration.
            delta = point - center
            projected = np.array([np.dot(delta, tangent), np.dot(delta, bitangent)])
            if settings["disk"]:
                closest = projected * min(1, settings["halfSize"][0] / max(np.linalg.norm(projected), 1e-8))
            else: closest = np.clip(projected, -np.array(settings["halfSize"]), settings["halfSize"])
            closest_distance = np.dot(delta - tangent * closest[0] - bitangent * closest[1],
                                      delta - tangent * closest[0] - bitangent * closest[1])
            weight = min(weight, float(smoothstep(.01, .0144, closest_distance)), float(smoothstep(.01, .02, nv)))
            if settings["range"] > 0:
                distance = np.linalg.norm(center - point)
                radius = settings["halfSize"][0] if settings["disk"] else np.linalg.norm(settings["halfSize"])
                def fade(distance): return max(1 - distance * distance / settings["range"] ** 2, 0) ** 2
                maximum_fade, minimum_fade = fade(max(distance - radius, 0)), fade(distance + radius)
                spread = (maximum_fade - minimum_fade) / maximum_fade if maximum_fade else 0
                weight = min(weight, float(1 - smoothstep(.005, .01, spread)))
                range_scale = fade(distance)
            geometry_weight = weight
            if absolute_errors:
                base_scale = (1 - settings["clearcoat"] * .04) * (1 - max(settings["sheen"]) * .25)
                absolute = absolute_errors[0] * base_scale
                if len(absolute_errors) > 1:
                    absolute += absolute_errors[1] * settings["clearcoat"] * (1 - max(settings["sheen"]) * .25)
                lower_diffuse = diffuse_scale * diffuse * (1 - interval[1]) * base_scale
                total_error = float(np.max(absolute / np.maximum(lower_diffuse, 1e-20)))
                fresnel_error = min(max(relative_errors), total_error)
                confidence = min(1, .02 / max(fresnel_error, 1e-20)) * (1 - smoothstep(.02, .04, fresnel_error))
                weight = min(weight, float(confidence))
            if correct_diffuse:
                emitters, quadrature_weights, _ = integration_samples(settings, sampled_count=samples)
                light_delta = emitters - point
                light_distance = np.sum(light_delta * light_delta, axis=1)
                light_direction = unit(light_delta)
                q = (1 - np.sum(unit(view + light_direction) * view, axis=1)) ** 5
                attenuation = np.maximum(light_direction @ -source_normal, 0) / np.maximum(light_distance, .01)
                if settings["range"] > 0: attenuation *= np.maximum(1 - light_distance / settings["range"] ** 2, 0) ** 2
                moment = np.sum(quadrature_weights * attenuation * np.maximum(light_direction[:, 2], 0) * q / np.pi)
                correction = diffuse_scale * moment * (1 - settings["clearcoat"] * .04) * (1 - max(settings["sheen"]) * .25)
                limit = diffuse_scale * diffuse * range_scale * (1 - settings["clearcoat"] * .04) * (1 - max(settings["sheen"]) * .25)
                tolerance = np.maximum(np.maximum(limit, correction) * settings["intensity"] * 4e-6, 1e-12)
                if np.any(correction * settings["intensity"] > limit * settings["intensity"] + tolerance):
                    results.append(fallback_stable[point_index] + fallback_q[point_index])
                    weights.append(0.); geometry_weights.append(0.); continue
                value = value * range_scale - correction
            else: value *= range_scale
            value = value * settings["intensity"]
            q_value *= range_scale * settings["intensity"]
            results.append(np.maximum(value, 0) * geometry_weight + q_value * weight +
                           fallback_stable[point_index] * (1 - geometry_weight) +
                           fallback_q[point_index] * (1 - weight))
            geometry_weights.append(geometry_weight)
        else: results.append((value + q_value) * settings["intensity"])
        weights.append(weight)
    if guarded and policy_details:
        return np.array(results), np.array(geometry_weights), np.array(weights)
    return (np.array(results), np.array(weights)) if guarded else np.array(results)


def oracle_checks(assets, output):
    # Parallel unit disk irradiance has exact normalized Lambert response
    # radius^2/(radius^2+height^2), independently validating disk area measure.
    for distance in (.2, .5, 1., 2.):
        settings = case("analytic", disk=True, halfSize=[1, 1], light=[0, 0, distance], intensity=1.)
        emitters, weights, normal = emitter_samples(settings, 128)
        d2 = np.sum(emitters * emitters, axis=1)
        estimate = np.sum(weights * distance * distance / (np.pi * d2 * d2))
        assert abs(estimate - 1 / (1 + distance * distance)) < 1e-8
    matrix, amplitude = read_table(assets / "matrix.bin"), read_table(assets / "amplitude.bin")
    assert np.all(matrix[:, :, 0] * matrix[:, :, 3] - matrix[:, :, 1] * matrix[:, :, 2] > 0)
    assert np.all(amplitude[:, :, 0] >= amplitude[:, :, 1] - 1e-5)
    assert np.all(amplitude[:, :, :2] >= -1e-5)
    assert np.all((amplitude[:, :, 2] >= 0) & (amplitude[:, :, 2] <= 1))
    assert np.all(amplitude[:, :, 3] == 0)
    continuity = fresnel_continuity_check(matrix, amplitude)
    angular_continuity = projected_angular_continuity_check()
    disk_rim = disk_rim_checks(matrix, amplitude)
    points = np.array([[0., 0., 0.], [-.12, .08, 0.], [.14, -.1, 0.]])
    records = []
    for settings in quality_cases():
        reference = numerical_reference(points, settings, 128)
        coarse = numerical_reference(points, settings, 64)
        scale = max(float(np.max(reference)), 1e-4)
        convergence = float(np.max(abs(reference - coarse)) / scale)
        assert convergence < .015, (settings["name"], "unconverged reference", convergence)
        estimate = ltc_reference(points, settings, matrix, amplitude)
        error = float(np.max(abs(reference - estimate)) / scale)
        effective, geometry_weights, weights = ltc_reference(points, settings, matrix, amplitude,
                                                            guarded=True, policy_details=True)
        effective_error = float(np.max(abs(reference - effective)) / scale)
        if float(geometry_weights.min()) >= 1 - 1e-5:
            assert effective_error < .06, (settings["name"], "full LTC numerical envelope", effective_error)
        record = dict(case="cpu-oracle", name=settings["name"], referenceConvergence=convergence,
                      rawLtcMaximumNormalizedError=error, effectiveMaximumNormalizedError=effective_error,
                      expectedLtcWeights=weights.tolist(), expectedGeometryWeights=geometry_weights.tolist(),
                      reference=reference.tolist(), rawLtc=estimate.tolist(),
                      effective=effective.tolist())
        records.append(record)
        print(settings["name"], "convergence", convergence, "LTC max error", error, flush=True)
    output.mkdir(parents=True, exist_ok=True)
    (output / "oracle-results.json").write_text(json.dumps(records, indent=2) + "\n")
    (output / "continuity-results.json").write_text(json.dumps(continuity, indent=2) + "\n")
    (output / "angular-continuity-results.json").write_text(json.dumps(angular_continuity, indent=2) + "\n")
    (output / "disk-rim-results.json").write_text(json.dumps(disk_rim, indent=2) + "\n")
    return records


def disk_rim_brdf(radius, height, order=256):
    """Independent true-circle BRDF integral at a parallel disk's rim.

    Receiver-centred polar coordinates put the far boundary at 2R*cos(theta).
    t=atan(rho/h) gives solid-angle measure sin(t) dt dtheta and resolves
    the near-rim peak that fixed source-area quadrature can miss.
    """
    nodes, weights = np.polynomial.legendre.leggauss(order)
    theta = nodes * np.pi / 2
    tmax = np.arctan(2 * radius * np.cos(theta) / height)
    t = (nodes[:, None] + 1) * tmax[None, :] / 2
    w = weights[:, None] * tmax[None, :] / 2 * weights[None, :] * np.pi / 2
    nl = np.cos(t)
    nh = np.sqrt((1 + nl) / 2)
    q = (1 - nh) ** 5
    roughness = .6
    alpha2 = roughness ** 4
    distribution = alpha2 / np.maximum(np.pi * (nh * nh * (alpha2 - 1) + 1) ** 2, 1e-4)
    k = (roughness + 1) ** 2 / 8
    geometry = nl / (nl * (1 - k) + k)
    specular = distribution * geometry * nl * q / np.maximum(4 * nl, 1e-4)
    diffuse = (1 - q) * nl / np.pi
    return float(np.sum((diffuse + specular) * np.sin(t) * w))


def disk_rim_checks(matrix, amplitude):
    records = []
    for radius in (1., 10., 100.):
        height = .13
        exact_cosine = .5 * (1 - height / np.hypot(height, 2 * radius))
        reference = disk_rim_brdf(radius, height, 512)
        convergence = abs(reference - disk_rim_brdf(radius, height, 256)) / reference
        assert convergence < 1e-7, (radius, "disk-rim BRDF convergence", convergence)
        estimates = []
        for angle in (0., np.pi / 64, np.pi / 32):
            settings = case("disk-rim", disk=True, halfSize=[radius, radius],
                light=[-radius * np.cos(angle), -radius * np.sin(angle), height],
                specular=0., albedo=[1., 1., 1.], intensity=1., roughness=.6)
            center, _, tangent, bitangent = emitter_frame(settings)
            vertices = adaptive_disk_vertices(center, tangent * radius, bitangent * radius)
            assert vertices is not None
            cosine = cosine_polygon(vertices)
            assert abs(cosine - exact_cosine) / exact_cosine < .002
            estimate, geometry, fresnel = ltc_reference(np.zeros((1, 3)), settings,
                matrix, amplitude, guarded=True, policy_details=True)
            assert geometry[0] == 1 and fresnel[0] == 1
            error = float(np.max(abs(estimate[0] - reference)) / reference)
            assert error < .003, (radius, angle, "disk-rim analytic response", error)
            estimates.append(float(estimate[0, 0]))
            records.append(dict(radius=radius, planeDistance=height, angle=angle,
                vertices=len(vertices), exactCosine=exact_cosine, polygonCosine=cosine,
                independentBrdf=reference, referenceConvergence=convergence,
                guardedLtc=estimate[0].tolist(), relativeError=error))
        assert (max(estimates) - min(estimates)) / reference < .001
    # A fitted LTC can magnify curvature or tilt its horizon independently
    # of the physical horizon. Validate both clips against a dense boundary.
    radius, height = 10., .13
    theta = np.pi / 3
    axis_x = radius * np.array([np.cos(theta), 0., -np.sin(theta)])
    axis_y = np.array([0., radius, 0.])
    source_axis = np.array([np.sin(theta), 0., np.cos(theta)])
    dense_angles = np.arange(32768) * 2 * np.pi / 32768
    for label, center in (("physical-horizon", -axis_y + source_axis * height),
                          ("rim", -axis_x + source_axis * height)):
        dense = center + np.cos(dense_angles)[:, None] * axis_x + np.sin(dense_angles)[:, None] * axis_y
        for roughness, nv in ((.35, .02), (.7, .2), (1., .7)):
            m = lookup(matrix, roughness, nv)
            transform = np.array([[m[0], 0, m[2]], [0, 1, 0], [m[1], 0, m[3]]])
            vertices = adaptive_disk_vertices(center, axis_x, axis_y, transform)
            assert vertices is not None
            reference = cosine_polygon(horizon_clip(horizon_clip(dense) @ transform.T))
            estimate = cosine_polygon(horizon_clip(horizon_clip(vertices) @ transform.T))
            error = abs(estimate - reference) / max(reference, .01)
            assert error < .003, (label, roughness, nv, "transformed disk rim", error)
            records.append(dict(case=label, roughness=roughness, NdotV=nv,
                vertices=len(vertices), denseBoundaryReference=reference,
                adaptiveBoundaryIntegral=estimate, relativeError=error))
    # Very large sources cannot silently exceed the finite work budget.
    assert adaptive_disk_vertices(np.array([-1e8, 0, .13]),
        np.array([1e8, 0, 0]), np.array([0, 1e8, 0])) is None
    return records


def projected_angular_continuity_check():
    # Equal-half-size, normal-facing sources have harmonic half-size .5,
    # so distances5000/2500 hit the two confidence boundaries exactly.
    # Tiny changes in distance must not cause a full-response policy jump.
    boundaries = []
    for bound in (1e-4, 2e-4):
        distance = .5 / bound
        samples = []
        for offset in (-1e-6, 0., 1e-6):
            settings = case("angular-boundary", halfSize=[1., 1.], light=[0, 0, distance * (1 + offset)])
            confidence, actual_bound = projected_angular_confidence([0, 0, 0], settings)
            samples.append(dict(distance=settings["light"][2], angularBound=actual_bound,
                                confidence=confidence))
        assert abs(samples[0]["confidence"] - samples[2]["confidence"]) < 1e-9
        boundaries.append(dict(boundary=bound, samples=samples))
    bounds = np.linspace(.9e-4, 2.1e-4, 1001)
    weights = np.array([projected_angular_confidence([0, 0, 0], case("angular-sweep",
        halfSize=[1., 1.], light=[0, 0, .5 / bound]))[0] for bound in bounds])
    refined = np.array([projected_angular_confidence([0, 0, 0], case("angular-sweep",
        halfSize=[1., 1.], light=[0, 0, .5 / bound]))[0] for bound in np.linspace(.9e-4, 2.1e-4, 2001)])
    assert np.all(np.diff(weights) >= 0)
    assert weights[0] == 0 and weights[-1] == 1
    assert np.max(np.diff(refined)) < .6 * np.max(np.diff(weights))
    return dict(boundaries=boundaries, maximumSweepStep=float(np.max(np.diff(weights))),
                refinedMaximumSweepStep=float(np.max(np.diff(refined))))


def fresnel_continuity_check(matrix, amplitude):
    radius, height = .5, .15
    settings = case("sphere-boundary-continuity", disk=True, halfSize=[radius, radius],
                    camera=[.477, 0, height], metallic=1., roughness=.7)
    point, view = np.zeros(3), unit(settings["camera"])
    def evaluate(distance):
        settings["light"] = [float(np.sqrt(distance * distance - height * height)), 0, height]
        interval = fresnel_interval(point, view, settings)
        _, weights = ltc_reference([point], settings, matrix, amplitude, guarded=True)
        assert np.isfinite(interval).all() and 0 <= interval[0] <= interval[1] <= 1
        return interval, float(weights[0])
    transitions = []
    for boundary in (radius, 1.1 * radius):
        before, middle, after = [evaluate(boundary + offset) for offset in (-1e-6, 0, 1e-6)]
        interval_step = float(np.max(abs(after[0] - before[0])))
        weight_step = abs(after[1] - before[1])
        assert interval_step < 1e-4 and weight_step < .002, (boundary, before, middle, after)
        transitions.append(dict(distance=boundary, intervalChange=interval_step,
                                confidenceChange=weight_step))
    distances = np.linspace(radius - .01, 1.1 * radius + .01, 1001)
    sweep = [evaluate(distance)[1] for distance in distances]
    largest_index = int(np.argmax(abs(np.diff(sweep))))
    maximum_step = abs(sweep[largest_index + 1] - sweep[largest_index])
    midpoint = evaluate(.5 * (distances[largest_index] + distances[largest_index + 1]))[1]
    refined_step = max(abs(midpoint - sweep[largest_index]), abs(sweep[largest_index + 1] - midpoint))
    # A genuine policy jump keeps the same size as the distance interval is
    # subdivided; a smooth, steep confidence transition shrinks with the step.
    assert refined_step < .6 * maximum_step, ("Fresnel confidence discontinuity", maximum_step, refined_step)
    assert min(sweep) < 1e-5 and max(sweep) > 1 - 1e-5
    print("sphere-boundary confidence continuity", maximum_step, flush=True)
    return dict(case="cpu-continuity", receiverHeight=height, emitterRadius=radius,
                camera=settings["camera"], points=len(sweep), boundaries=transitions,
                maximumAdjacentConfidenceChange=maximum_step,
                maximumStepAfterSubdivision=refined_step)


def receiver_fixture(path, settings, light_count=1):
    """One authored +Z plane with no environment, indirect light or textures."""
    positions = np.array([[-8, -8, 0], [8, -8, 0], [8, 8, 0],
                          [-8, -8, 0], [8, 8, 0], [-8, 8, 0]], dtype="<f4")
    normals = np.tile(np.array([0, 0, 1], dtype="<f4"), (6, 1))
    blob = positions.tobytes() + normals.tobytes()
    material = dict(name="Uniform numerical reference receiver", doubleSided=settings["doubleSided"],
        pbrMetallicRoughness=dict(baseColorFactor=[*settings["albedo"], settings["alpha"]],
            metallicFactor=settings["metallic"], roughnessFactor=settings["roughness"]),
        extensions=dict(KHR_materials_specular=dict(specularFactor=settings["specular"]),
            KHR_materials_clearcoat=dict(clearcoatFactor=settings["clearcoat"],
                clearcoatRoughnessFactor=settings["clearcoatRoughness"]),
            KHR_materials_sheen=dict(sheenColorFactor=settings["sheen"],
                sheenRoughnessFactor=settings["sheenRoughness"]),
            KHR_materials_iridescence=dict(iridescenceFactor=settings["iridescence"],
                iridescenceIor=settings["iridescenceIor"],
                iridescenceThicknessMinimum=settings["iridescenceThicknessMinimum"],
                iridescenceThicknessMaximum=settings["iridescenceThicknessMaximum"])))
    if settings["alpha"] < 1: material["alphaMode"] = "BLEND"
    area = dict(shape="disk" if settings["disk"] else "rect",
                width=2 * settings["halfSize"][0], height=2 * settings["halfSize"][1],
                radius=settings["halfSize"][0])
    light = dict(name="Numerical area emitter", type="point", color=[1, 1, 1],
                 intensity=settings["intensity"] / light_count, extras=dict(areaLight=area))
    if settings["range"] > 0:
        light["range"] = settings["range"]
        area["range"] = settings["range"]
    nodes = [dict(name="Numerical receiver", mesh=0)]
    for i in range(light_count):
        nodes.append(dict(name=f"Area emitter {i}", translation=settings["light"],
            rotation=[0, float(np.sin(settings["yaw"] / 2)), 0, float(np.cos(settings["yaw"] / 2))],
            extensions=dict(KHR_lights_punctual=dict(light=0))))
        if settings["mirrored"]: nodes[-1]["scale"] = [-1, 1, 1]
    document = dict(asset=dict(version="2.0", generator="Independent LTC numerical fixture"),
        extensionsUsed=["KHR_lights_punctual", *material["extensions"]],
        extensions=dict(KHR_lights_punctual=dict(lights=[light])),
        buffers=[dict(byteLength=len(blob), uri="data:application/octet-stream;base64," +
            base64.b64encode(blob).decode())],
        bufferViews=[dict(buffer=0, byteOffset=0, byteLength=positions.nbytes),
                     dict(buffer=0, byteOffset=positions.nbytes, byteLength=normals.nbytes)],
        accessors=[dict(bufferView=0, componentType=5126, count=6, type="VEC3",
            min=positions.min(axis=0).tolist(), max=positions.max(axis=0).tolist()),
            dict(bufferView=1, componentType=5126, count=6, type="VEC3")],
        materials=[material], meshes=[dict(primitives=[dict(mode=4, material=0,
            attributes=dict(POSITION=0, NORMAL=1))])], nodes=nodes,
        scenes=[dict(nodes=list(range(len(nodes))))], scene=0)
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(document), encoding="utf-8")
    return path


def receiver_points(telemetry, pixels=None):
    width, height = telemetry["resolution"]
    if pixels is None:
        offsets = np.array([-8, 0, 8])
        x, y = np.meshgrid(width // 2 + offsets, height // 2 + offsets)
        pixels = np.column_stack((x.ravel(), y.ravel()))
    pixels = np.asarray(pixels)
    inverse = np.linalg.inv(np.array(telemetry["camera"]["unjitteredViewProjColumns"]).T)
    ndc = (pixels + .5) / [width, height] * [2, -2] + [-1, 1]
    near = np.column_stack((ndc, np.ones(len(ndc)), np.ones(len(ndc)))) @ inverse.T
    near = near[:, :3] / near[:, 3:4]
    origin = np.array(telemetry["camera"]["position"])
    direction = near - origin
    points = origin + direction * (-origin[2] / direction[:, 2:3])
    assert np.isfinite(points).all() and np.max(abs(points[:, :2])) < 7.5
    return points, pixels


def invert_display(srgb):
    mapped = np.where(srgb <= .04045, srgb / 12.92, ((srgb + .055) / 1.055) ** 2.4)
    a, b, c = 2.43 * mapped - 2.51, .59 * mapped - .03, .14 * mapped
    return (-b - np.sqrt(np.maximum(b * b - 4 * a * c, 0))) / (2 * a)


def measured_radiance(path, pixels, exposure):
    with Image.open(path) as image:
        values = np.asarray(image.convert("RGB"), dtype=np.float64)[pixels[:, 1], pixels[:, 0]]
    assert values.max() < 254, (path, "clipped reference pixels", values.max())
    middle = invert_display(values / 255) / exposure
    lower = invert_display(np.maximum(values - .5, 0) / 255) / exposure
    upper = invert_display(np.minimum(values + .5, 255) / 255) / exposure
    return middle, np.maximum(upper - middle, middle - lower)


class Runtime:
    def __init__(self, exe, output, assets, reuse_quality_results=None):
        self.exe, self.output = exe.resolve(), output.resolve()
        self.records, self.cache = [], {}
        self.reuse_quality = set()
        if reuse_quality_results:
            for record in json.loads(reuse_quality_results.read_text()):
                if record["case"] == "quality" and record.get("validationClean"):
                    self.reuse_quality.add((record["name"], record["technique"], record["mode"], record["samples"]))
        self.matrix = read_table(assets.resolve() / "matrix.bin")
        self.amplitude = read_table(assets.resolve() / "amplitude.bin")
        self.output.mkdir(parents=True, exist_ok=True)
        (self.output / "vk_layer_settings.txt").write_text("khronos_validation.validate_sync = true\n")

    def record(self, record):
        self.records.append(record)
        (self.output / "results.json").write_text(json.dumps(self.records, indent=2) + "\n")
        print(json.dumps({key: value for key, value in record.items() if key != "lighting"}), flush=True)

    def validate_telemetry(self, telemetry, mode, samples, ltc_ready=True):
        lighting = telemetry["lighting"]
        assert lighting["bounceIntensity"] == 0 and lighting["areaLightCount"] > 0, lighting
        assert lighting["areaLightingRequestedMode"] == int(mode == "ltc"), lighting
        assert lighting["areaLightingMode"] == int(mode == "ltc" and ltc_ready), lighting
        assert lighting["ltcReady"] == ltc_ready, lighting
        assert lighting["areaLightSampleCount"] == samples, lighting
        assert lighting["ltcAllocatedImageBytes"] > 0, lighting

    def capture(self, name, model, settings, technique, mode, samples=25,
                exposure=.2, sequence=None, extra=(), width=320, height=240,
                executable=None, ltc_ready=True, expected_mode=None, expected_samples=None):
        executable = executable or self.exe
        screenshot, log = self.output / (name + ".png"), self.output / (name + ".log")
        for stale in [screenshot, *self.output.glob(name + ".frame-*.png")]:
            stale.unlink(missing_ok=True)
        command = [str(executable), "--model", str(model), "--hidden", "--no-ui", "--validation",
            "--no-bloom", "--no-taa", "--msaa", "1", "--width", str(width), "--height", str(height),
            "--display-mode", "lit", "--camera-position", *map(str, settings["camera"]),
            "--camera-target", *map(str, settings.get("target", [0, 0, 0])), "--camera-fov", "36",
            "--environment-intensity", "0", "--directional-intensity", "0",
            "--exposure", str(exposure), "--render-technique", technique,
            "--area-lighting", mode, "--area-light-samples", str(samples),
            "--warmup-frames", "16", "--capture-frame", "17", "--screenshot", str(screenshot), *extra]
        if sequence:
            script = self.output / (name + "-sequence.json")
            script.write_text(json.dumps(sequence))
            command += ["--capture-sequence", str(script)]
        with log.open("w") as stream:
            run = subprocess.run(command, cwd=executable.parent,
                env=dict(os.environ, VK_LAYER_SETTINGS_PATH=str(self.output)),
                stdout=stream, stderr=subprocess.STDOUT, timeout=240)
        text = log.read_text(errors="replace")
        assert run.returncode == 0 and "VUID-" not in text and "SYNC-HAZARD" not in text, log
        telemetry = json.loads(screenshot.with_suffix(".telemetry.json").read_text())
        final_mode = expected_mode or mode
        self.validate_telemetry(telemetry, final_mode, expected_samples or samples, ltc_ready)
        return screenshot, telemetry

    def quality(self, settings, technique="deferred-raster", samples=25):
        key = (settings["name"], technique, samples)
        if key in self.cache: return self.cache[key]
        model = receiver_fixture(self.output / settings["name"] /
            "models/validation/cornell_box_local_light.gltf", settings)
        reference_at_center = numerical_reference([[0, 0, 0]], settings)[0]
        exposure = .18 / max(float(reference_at_center.max()), .02)
        measurements = {}
        for mode in ("sampled", "ltc"):
            name = f'{settings["name"]}-{technique}-{mode}-n{samples}'
            reused = (settings["name"], technique, mode, samples) in self.reuse_quality
            if reused:
                # Reuse requires an explicit validation-clean record list. The
                # current numerical equations and thresholds still recheck PNGs.
                image = self.output / (name + ".png")
                telemetry = json.loads(image.with_suffix(".telemetry.json").read_text())
                self.validate_telemetry(telemetry, mode, samples)
            else:
                image, telemetry = self.capture(name, model, settings, technique, mode,
                    samples=samples, exposure=exposure, extra=("--no-ray-query",))
            points, pixels = receiver_points(telemetry)
            reference = numerical_reference(points, settings) * settings["alpha"]
            sampled = numerical_reference(points, settings, sampled_count=samples) * settings["alpha"]
            actual, uncertainty = measured_radiance(image, pixels, exposure)
            ltc_expected, geometry_weights, ltc_weights = ltc_reference(points, settings, self.matrix,
                self.amplitude, guarded=True, samples=samples, policy_details=True)
            ltc_expected *= settings["alpha"]
            scale = max(float(reference.max()), .01)
            error = np.maximum(abs(actual - reference) - uncertainty, 0)
            sampled_error = np.maximum(abs(actual - sampled) - uncertainty, 0)
            maximum = float(error.max() / scale)
            sampled_maximum = float(sampled_error.max() / max(float(sampled.max()), .01))
            policy_maximum = float(np.max(np.maximum(abs(actual - ltc_expected) - uncertainty, 0)) /
                                   max(float(ltc_expected.max()), .01))
            # The sampled mode must reproduce its independently constructed
            # quadrature, even when that quadrature is a coarse BRDF reference.
            if mode == "sampled":
                assert sampled_maximum < .025, (name, "sampled equation mismatch", sampled_maximum)
            minimum_roughness = min(settings["roughness"], settings["clearcoatRoughness"]
                if settings["clearcoat"] > 0 else 1)
            fallback = (minimum_roughness <= .28 or
                (settings["alpha"] < 1 and settings["doubleSided"]) or
                float(geometry_weights.max()) <= 1e-5)
            if mode == "ltc" and fallback:
                assert sampled_maximum < .025, (name, "material/geometry fallback mismatch", sampled_maximum)
            elif mode == "ltc":
                assert policy_maximum < .025, (name, "guarded LTC equation mismatch", policy_maximum)
                baseline_error = float(np.max(abs(sampled - reference)) / scale)
                # Geometrically eligible Schlick-residual paths retain the
                # strict6% reference gate, even when their q confidence is0.
                envelope = .06 + (1 - float(geometry_weights.min())) * baseline_error
                assert maximum < envelope, (name, "LTC reference mismatch", maximum, envelope)
            self.record(dict(case="quality", name=settings["name"], technique=technique,
                mode=mode, samples=samples, referenceMaximumNormalizedError=maximum,
                sampledMaximumNormalizedError=sampled_maximum,
                guardedLtcMaximumNormalizedError=policy_maximum if mode == "ltc" else None,
                perMaterialPolicy="explicit-sampled" if mode == "sampled" else
                    "sampled-fallback" if float(geometry_weights.max()) <= 1e-5 else
                    "ltc" if float(ltc_weights.min()) >= 1 - 1e-5 else
                    "schlick-residual" if float(geometry_weights.min()) >= 1 - 1e-5 else "blend",
                expectedLtcWeightRange=[float(ltc_weights.min()), float(ltc_weights.max())],
                expectedGeometryLtcWeightRange=[float(geometry_weights.min()), float(geometry_weights.max())],
                pixels=len(points), exposure=exposure, lighting=telemetry["lighting"],
                captureReused=reused, validationClean=True))
            measurements[mode] = actual, uncertainty, reference, telemetry
        self.cache[key] = measurements
        return measurements

    def quality_group(self):
        for settings in quality_cases(): self.quality(settings)
        # The configured counts control both explicit sampled integration and
        # guarded LTC fallback/residual work, rather than shadow ray counts.
        for name in ("rect-metal-r0.15", "disk-metal-r0.15", "rect-opposite-diffuse"):
            settings = next(item for item in quality_cases() if item["name"] == name)
            for samples in (9, 64): self.quality(settings, samples=samples)

    def parity_group(self):
        selected = [item for item in quality_cases() if item["name"] in
            ("rect-diffuse", "disk-diffuse", "rect-near", "disk-horizon", "rect-grazing",
             "rect-opposite-diffuse", "rect-metal-r0.15", "disk-metal-r0.28",
             "rect-metal-r0.7", "layered", "layered-grazing", "layered-ltc",
             "iridescence", "iridescence-metallic", "finite-range", "rect-thin-far",
             "transparent", "transparent-thin")]
        for settings in selected:
            deferred, forward = self.quality(settings), self.quality(settings, "forward-raster")
            for mode in ("sampled", "ltc"):
                a, ua, reference, _ = deferred[mode]
                b, ub, _, _ = forward[mode]
                error = np.maximum(abs(a - b) - ua - ub, 0)
                maximum = float(error.max() / max(float(reference.max()), .01))
                assert maximum < .025, (settings["name"], mode, "forward/deferred mismatch", maximum)
                self.record(dict(case="parity", name=settings["name"], mode=mode,
                    maximumNormalizedError=maximum, validationClean=True))

    def shadow_group(self):
        original = json.loads((self.exe.parent / "models/validation/cornell_box_local_light.gltf").read_text())
        settings = case("shadows", camera=[0, 1.05, 4.1])
        settings["target"] = [0, .95, 0]
        for disk in (False, True):
            shape = "disk" if disk else "rect"
            models = {}
            for blocked in (False, True):
                document = copy.deepcopy(original)
                primitives = document["meshes"][0]["primitives"]
                document["meshes"][0]["primitives"] = [primitives[0]]
                # F0=1 removes Schlick-moment uncertainty, so this floor
                # remains full LTC while exercising shadow visibility once.
                material = document["materials"][primitives[0]["material"]]
                material["pbrMetallicRoughness"].update(baseColorFactor=[1, 1, 1, 1],
                    metallicFactor=1, roughnessFactor=.6)
                document["meshes"].append(dict(primitives=primitives[5:]))
                if blocked:
                    document["nodes"].append(dict(name="Shadow blocker", mesh=1))
                    document["scenes"][0]["nodes"].append(len(document["nodes"]) - 1)
                light = document["extensions"]["KHR_lights_punctual"]["lights"][0]
                # A long, finite range keeps raster shadow allocation and
                # permits the bounded LTC range approximation on the floor.
                light["range"] = 100.
                light["extras"]["areaLight"].update(shape=shape, width=.8, height=.8, radius=.4, range=100.)
                # Move the source behind the blocker so these visible floor
                # profiles contain a full umbra as well as penumbra and light.
                document["nodes"][1]["translation"][2] = -.8
                path = (self.output / f'shadow-{shape}-{"blocked" if blocked else "clear"}' /
                    "models/validation/cornell_box_local_light.gltf")
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text(json.dumps(document))
                models[blocked] = path
            for technique in ("deferred-raster", "forward-raster"):
                for shadow in ("raster", "hard", "soft"):
                    profiles = {}
                    for mode in ("sampled", "ltc"):
                        images = {}
                        for blocked, model in models.items():
                            name = f'shadow-{shape}-{technique}-{shadow}-{mode}-{"blocked" if blocked else "clear"}'
                            image, telemetry = self.capture(name, model, settings, technique, mode,
                                exposure=.03, width=960, height=540,
                                extra=("--ray-shadows", shadow, "--area-shadow-quality", "2",
                                       "--ray-shadow-samples", "32"))
                            if shadow != "raster" and not telemetry["rayShadows"]["supported"]:
                                self.record(dict(case="shadows", shape=shape, technique=technique,
                                    shadow=shadow, skipped="device lacks ray queries"))
                                break
                            if shadow != "raster": assert telemetry["rayShadows"]["active"]
                            images[blocked] = hdr(image), telemetry
                        if len(images) != 2: break
                        clear, telemetry = images[False]
                        blocked = images[True][0]
                        values = []
                        for row in (485, 490, 495):
                            _, mask = floor_points(telemetry, row)
                            assert int(mask.sum()) > 100
                            values.extend(np.mean(blocked[row, mask] / np.maximum(clear[row, mask], 1e-5), axis=1))
                        profiles[mode] = np.array(values)
                    if len(profiles) != 2: continue
                    difference = abs(profiles["sampled"] - profiles["ltc"])
                    assert profiles["sampled"].min() < .2 and profiles["sampled"].max() > .9
                    mae, p95 = float(difference.mean()), float(np.percentile(difference, 95))
                    assert mae < .01 and p95 < .035, (shape, technique, shadow, mae, p95)
                    self.record(dict(case="shadows", shape=shape, technique=technique, shadow=shadow,
                        sampledLtcVisibilityMae=mae, sampledLtcVisibilityP95=p95,
                        pixels=len(difference), receiverF0=[1, 1, 1], receiverRoughness=.6,
                        perMaterialLtcWeight=1., validationClean=True))

    def spatial_group(self):
        """Near-field annulus includes configured-sample spokes and their gaps."""
        settings = next(item for item in quality_cases() if item["name"] == "disk-near")
        model = receiver_fixture(self.output / "spatial-disk-near" /
            "models/validation/cornell_box_local_light.gltf", settings)
        center_reference = numerical_reference([[0, 0, 0]], settings)[0]
        exposure, scale = .18 / float(center_reference.max()), float(center_reference.max())
        pixels = [[160, 120], [176, 75], [210, 120], [175, 164], [118, 145], [118, 94],
                  [200, 91], [150, 76], [117, 121], [154, 164], [202, 147],
                  [203, 120], [204, 120], [205, 120]]
        for radius in (36, 44, 52):
            for angle in np.arange(20) * 2 * np.pi / 20:
                pixels.append([int(round(160 + radius * np.cos(angle))),
                               int(round(120 + radius * np.sin(angle)))])
        pixels = np.unique(pixels, axis=0)
        records, measured = [], {}
        def record(value):
            records.append(value)
            (self.output / "spatial-results.json").write_text(json.dumps(records, indent=2) + "\n")
            print(json.dumps(value), flush=True)
        for samples in (9, 25, 64):
            for technique in ("deferred-raster", "forward-raster"):
                for mode in ("sampled", "ltc"):
                    name = f"spatial-disk-near-{technique}-{mode}-n{samples}"
                    image, telemetry = self.capture(name, model, settings, technique, mode,
                        samples=samples, exposure=exposure, extra=("--no-ray-query",))
                    points, _ = receiver_points(telemetry, pixels)
                    reference = numerical_reference(points, settings, 128)
                    coarse = numerical_reference(points, settings, 64)
                    convergence = float(np.max(abs(reference - coarse)) / scale)
                    assert convergence < .015, (name, "annulus reference convergence", convergence)
                    sampled = numerical_reference(points, settings, sampled_count=samples)
                    expected, geometry, fresnel = ltc_reference(points, settings, self.matrix,
                        self.amplitude, guarded=True, samples=samples, policy_details=True)
                    actual, uncertainty = measured_radiance(image, pixels, exposure)
                    target = sampled if mode == "sampled" else expected
                    equation_error = float(np.max(np.maximum(abs(actual - target) - uncertainty, 0)) / scale)
                    assert equation_error < .025, (name, "annulus policy equation mismatch", equation_error)
                    physical_error = np.maximum(abs(actual - reference) - uncertainty, 0)
                    reference_error = float(np.max(physical_error) / scale)
                    local_error = float(np.max(physical_error / np.maximum(reference.max(axis=1)[:, None], .01)))
                    if mode == "ltc":
                        assert float(geometry.min()) >= 1 - 1e-5
                        assert reference_error < .06, (name, "annulus reference mismatch", reference_error)
                    measured[(samples, technique, mode)] = actual, uncertainty
                    record(dict(case="spatial-quality", name="disk-near-annulus", mode=mode,
                        samples=samples, technique=technique, pixels=len(pixels),
                        referenceMaximumCenterNormalizedError=reference_error,
                        referenceMaximumLocalNormalizedError=local_error,
                        policyMaximumCenterNormalizedError=equation_error,
                        referenceConvergence=convergence,
                        expectedGeometryWeightRange=[float(geometry.min()), float(geometry.max())],
                        expectedFresnelWeightRange=[float(fresnel.min()), float(fresnel.max())],
                        validationClean=True))
            for mode in ("sampled", "ltc"):
                a, ua = measured[(samples, "deferred-raster", mode)]
                b, ub = measured[(samples, "forward-raster", mode)]
                difference = float(np.max(np.maximum(abs(a - b) - ua - ub, 0)) / scale)
                assert difference < .025, (mode, samples, "annulus forward/deferred mismatch", difference)
                record(dict(case="spatial-parity", name="disk-near-annulus", mode=mode,
                    samples=samples, maximumCenterNormalizedError=difference, validationClean=True))

    def disk_rim_group(self):
        """The reviewed near-rim circular geometry must remain analytic."""
        for radius in (10., 100.):
            height = .13
            reference = disk_rim_brdf(radius, height, 512)
            for technique in ("deferred-raster", "forward-raster"):
                measurements = []
                for label, angle in (("vertex", 0.), ("edge-midpoint", np.pi / 32)):
                    name = f"disk-rim-r{radius:g}-{technique}-{label}"
                    settings = case(name, disk=True, halfSize=[radius, radius],
                        light=[-radius * np.cos(angle), -radius * np.sin(angle), height],
                        specular=0., albedo=[1., 1., 1.], intensity=1., roughness=.6,
                        doubleSided=False)
                    model = receiver_fixture(self.output / name /
                        "models/validation/cornell_box_local_light.gltf", settings)
                    image, telemetry = self.capture(name, model, settings, technique, "ltc",
                        exposure=1., width=321, height=241, extra=("--no-ray-query",))
                    points, pixels = receiver_points(telemetry, [[160, 120]])
                    actual, uncertainty = measured_radiance(image, pixels, 1.)
                    expected, geometry, fresnel = ltc_reference(points, settings, self.matrix,
                        self.amplitude, guarded=True, policy_details=True)
                    assert geometry[0] == 1 and fresnel[0] == 1, (name, geometry, fresnel)
                    error = float(np.max(np.maximum(abs(actual - reference) - uncertainty, 0)) / reference)
                    policy_error = float(np.max(np.maximum(abs(actual - expected) - uncertainty, 0)) / reference)
                    assert error < .01, (name, "true circular-disk reference", error)
                    assert policy_error < .01, (name, "adaptive disk policy", policy_error)
                    measurements.append((actual, uncertainty))
                    self.record(dict(case="disk-rim", name=name, radius=radius,
                        planeDistance=height, technique=technique, angle=angle,
                        independentBrdf=reference, measured=actual[0].tolist(),
                        measurementUncertainty=uncertainty[0].tolist(),
                        guardedLtc=expected[0].tolist(), geometryWeight=float(geometry[0]),
                        fresnelWeight=float(fresnel[0]), relativeReferenceError=error,
                        relativePolicyError=policy_error, validationClean=True))
                a, ua = measurements[0]
                b, ub = measurements[1]
                rotational_error = float(np.max(np.maximum(abs(a - b) - ua - ub, 0)) / reference)
                assert rotational_error < .01, (radius, technique, "disk rotational invariance", rotational_error)

    def isolated_executable(self, name):
        directory = self.output / "isolated-runtime" / name
        directory.mkdir(parents=True, exist_ok=True)
        for source in [self.exe, *self.exe.parent.glob("*.dll")]:
            shutil.copy2(source, directory / source.name)
        for name in ("spv_shaders", "hdr", "materials"):
            destination = directory / name
            # These copies are independent: malformed LUT fixtures must never
            # modify the release assets through a shared link or junction.
            shutil.copytree(self.exe.parent / name, destination, dirs_exist_ok=True)
        return directory / self.exe.name

    def asset_group(self):
        settings = case("asset-fallback", roughness=.6)
        model = receiver_fixture(self.output / settings["name"] /
            "models/validation/cornell_box_local_light.gltf", settings)
        for state in ("missing", "corrupt"):
            executable = self.isolated_executable(state)
            directory = executable.parent / "materials/ltc"
            if state == "missing":
                (directory / "amplitude.bin").unlink()
            else:
                data = (directory / "matrix.bin").read_bytes()
                (directory / "matrix.bin").write_bytes(b"FAIL" + data[4:])
            image, telemetry = self.capture("asset-" + state, model, settings,
                "deferred-raster", "ltc", executable=executable, ltc_ready=False,
                extra=("--no-ray-query",))
            points, pixels = receiver_points(telemetry)
            expected = numerical_reference(points, settings, sampled_count=25)
            actual, uncertainty = measured_radiance(image, pixels, .2)
            error = float(np.max(np.maximum(abs(actual - expected) - uncertainty, 0)) /
                          max(float(expected.max()), .01))
            assert error < .025, (state, "optional asset fallback mismatch", error)
            assert "Sampled area lighting" in telemetry["lighting"]["ltcStatus"]
            self.record(dict(case="assets", state=state, sampledMaximumNormalizedError=error,
                lighting=telemetry["lighting"], validationClean=True))

    def switch_group(self):
        settings = next(item for item in quality_cases() if item["name"] == "rect-near")
        model = receiver_fixture(self.output / "runtime-switch" /
            "models/validation/cornell_box_local_light.gltf", settings)
        states = [(17, "ltc", 25), (25, "sampled", 25), (33, "sampled", 9),
                  (41, "ltc", 9), (49, "ltc", 64), (51, "ltc", 64),
                  (53, "ltc", 64), (57, "ltc", 64)]
        # Isolate each setter, then repeat each unchanged value separately and
        # together. The final frame sends an explicit event rather than merely
        # holding the previous state without exercising capture-event handling.
        events = [dict(frame=25, areaLighting="sampled"),
                  dict(frame=33, areaLightSamples=9),
                  dict(frame=41, areaLighting="ltc"),
                  dict(frame=49, areaLightSamples=64),
                  dict(frame=51, areaLighting="ltc"),
                  dict(frame=53, areaLightSamples=64),
                  dict(frame=57, areaLighting="ltc", areaLightSamples=64)]
        events_by_frame = {event["frame"]: event for event in events}
        changed_frames, noop_frames = (25, 33, 41, 49), (51, 53, 57)
        sequence = dict(schemaVersion=1, frames=57, sampleFrames=[s[0] for s in states],
                        events=events)
        for technique in ("deferred-raster", "forward-raster"):
            captures = {}
            for taa in (False, True):
                image, _ = self.capture(f"switch-{technique}-taa{int(taa)}", model, settings,
                    technique, "ltc", exposure=.3, sequence=sequence,
                    expected_samples=64, extra=("--no-ray-query", "--taa" if taa else "--no-taa"))
                captures[taa] = image
            age_image, _ = self.capture(f"switch-{technique}-taa-age", model, settings,
                technique, "ltc", exposure=.3, sequence=sequence, expected_samples=64,
                extra=("--no-ray-query", "--taa", "--display-mode", "taa-age"))
            epochs = {}
            last_changed_frame = None
            for frame, mode, samples in states:
                paths = {}
                frame_epoch, reset_reason, lighting_state = None, None, None
                for taa, image in captures.items():
                    paths[taa] = image if frame == 57 else image.with_name(image.stem + f".frame-{frame:04d}.png")
                    telemetry = json.loads(paths[taa].with_suffix(".telemetry.json").read_text())
                    self.validate_telemetry(telemetry, mode, samples)
                    lighting_state = telemetry["lighting"]
                    if taa:
                        assert telemetry["taa"]["enabled"]
                        epoch = telemetry["taa"]["epoch"]
                        frame_epoch, reset_reason = epoch, telemetry["taa"]["resetReason"]
                        if frame in changed_frames:
                            assert epoch > epochs.get(taa, 0)
                            assert reset_reason == "area light integration changed"
                            last_changed_frame = frame
                        if frame in noop_frames:
                            assert epoch == epochs[taa], (technique, frame, "unchanged lighting event reset history")
                        epochs[taa] = epoch
                age_path = age_image if frame == 57 else age_image.with_name(
                    age_image.stem + f".frame-{frame:04d}.png")
                age_telemetry = json.loads(age_path.with_suffix(".telemetry.json").read_text())
                self.validate_telemetry(age_telemetry, mode, samples)
                assert age_telemetry["taa"]["enabled"] and age_telemetry["taa"]["epoch"] == frame_epoch
                with Image.open(age_path) as image:
                    srgb = np.asarray(image.convert("RGB"), dtype=float)[100:140, 135:185] / 255
                linear = np.where(srgb <= .04045, srgb / 12.92, ((srgb + .055) / 1.055) ** 2.4)
                # The diagnostic encodes age/64 directly, before tone mapping.
                age = np.mean(linear, axis=2) * 64
                median_age, age_p05 = float(np.median(age)), float(np.percentile(age, 5))
                if frame in changed_frames:
                    assert abs(median_age - 1) < .15, (technique, frame, "edit retained old GPU history", median_age)
                if frame in noop_frames:
                    assert age_p05 >= frame - last_changed_frame, (
                        technique, frame, "unchanged lighting event discarded GPU history", age_p05)
                # The receiver is stationary and well inside the viewport.
                # A mode/quality edit must retire old lighting history at once.
                with Image.open(paths[False]) as image:
                    baseline = np.asarray(image.convert("RGB"), dtype=float) / 255
                with Image.open(paths[True]) as image:
                    candidate = np.asarray(image.convert("RGB"), dtype=float) / 255
                difference = abs(baseline[100:140, 135:185] - candidate[100:140, 135:185])
                mae, p95 = float(difference.mean()), float(np.percentile(difference, 95))
                assert mae < .008 and p95 < .025, (technique, frame, "stale lighting history", mae, p95)
                self.record(dict(case="runtime-switch", technique=technique, frame=frame,
                    mode=mode, samples=samples, requestedMode=lighting_state["areaLightingRequestedMode"],
                    effectiveMode=lighting_state["areaLightingMode"], taaEpoch=frame_epoch,
                    taaResetReason=reset_reason, taaAgeMedian=median_age, taaAgeP05=age_p05,
                    eventFields=[key for key in events_by_frame.get(frame, {}) if key != "frame"],
                    historyPolicy="reset" if frame in changed_frames else "retain" if frame in noop_frames else "initial",
                    taaMae=mae, taaP95=p95, validationClean=True))

    def benchmark_group(self, metallic=1., counts=(1, 8, 32, 128)):
        for disk in (False, True):
            settings = case("benchmark-disk" if disk else "benchmark-rect", disk=disk,
                            halfSize=[.4, .4 if disk else .3], metallic=metallic, roughness=.6)
            for count in counts:
                model = receiver_fixture(self.output / f'{settings["name"]}-{count}' /
                    "models/validation/cornell_box_local_light.gltf", settings, count)
                timings = {}
                for mode in ("sampled", "ltc"):
                    frames = [49, 53, 57, 61, 65, 69, 73, 77]
                    sequence = dict(schemaVersion=1, frames=77, sampleFrames=frames)
                    name = f'{settings["name"]}-{count}-{mode}'
                    image, _ = self.capture(name, model, settings, "deferred-raster", mode,
                        sequence=sequence, extra=("--no-ray-query",), width=960, height=540)
                    values = []
                    for frame in frames:
                        path = image if frame == 77 else image.with_name(image.stem + f".frame-{frame:04d}.png")
                        telemetry = json.loads(path.with_suffix(".telemetry.json").read_text())
                        assert telemetry["lighting"]["areaLightCount"] == count
                        assert telemetry["lighting"]["activeLocalShadowLayers"] == 0
                        lighting_pass = next(p for p in telemetry["passes"] if p["name"] == "Lighting")
                        assert lighting_pass["active"] and lighting_pass["gpuTimed"], lighting_pass
                        values.append(lighting_pass["gpuMs"])
                    timings[mode] = float(np.median(values))
                    self.record(dict(case="benchmark", shape="disk" if disk else "rect",
                        lights=count, mode=mode, lightingMedianGpuMs=timings[mode],
                        lightingP95GpuMs=float(np.percentile(values, 95)), frames=len(values),
                        resolution=[960, 540], shadows=False, metallic=metallic,
                        roughness=settings["roughness"], validationClean=True))
                self.record(dict(case="benchmark-comparison", shape="disk" if disk else "rect",
                    lights=count, sampledToLtcSpeedup=timings["sampled"] / max(timings["ltc"], 1e-6)))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--exe", type=Path)
    parser.add_argument("--assets", type=Path, default=Path("materials/ltc"))
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--reuse-quality-results", type=Path,
        help="explicit prior validation-clean quality records whose unchanged captures may be rechecked")
    parser.add_argument("--benchmark-material", choices=("metallic", "dielectric"), default="metallic")
    parser.add_argument("--benchmark-lights", type=int, choices=(1, 8, 32, 128), nargs="+", default=(1, 8, 32, 128))
    parser.add_argument("--case", choices=("cpu", "quality", "parity", "spatial", "disk-rim", "shadows", "benchmark", "assets", "switch", "all"), default="all")
    args = parser.parse_args()
    if args.case != "cpu" and os.environ.get("CONTAINER_RUN_GPU_LTC") != "1":
        print("SKIP: set CONTAINER_RUN_GPU_LTC=1")
        return 77
    oracle_checks(args.assets.resolve(), args.output.resolve())
    if args.case == "cpu": return 0
    if not args.exe: parser.error("--exe is required for GPU cases")
    runtime = Runtime(args.exe, args.output, args.assets, args.reuse_quality_results)
    if args.case in ("quality", "all"): runtime.quality_group()
    if args.case in ("parity", "all"): runtime.parity_group()
    if args.case in ("spatial", "all"): runtime.spatial_group()
    if args.case in ("disk-rim", "all"): runtime.disk_rim_group()
    if args.case in ("benchmark", "all"):
        runtime.benchmark_group(float(args.benchmark_material == "metallic"), args.benchmark_lights)
    if args.case in ("shadows", "all"): runtime.shadow_group()
    if args.case in ("assets", "all"): runtime.asset_group()
    if args.case in ("switch", "all"): runtime.switch_group()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
