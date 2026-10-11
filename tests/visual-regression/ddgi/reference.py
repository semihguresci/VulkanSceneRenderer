#!/usr/bin/env python3
"""Independent scene-linear diffuse transport for the generated DDGI quality scenes.

The reference intersects analytic opaque rectangles, integrates direct light
with Gauss-Legendre quadrature, and estimates indirect light with cosine-weighted
Monte Carlo paths plus one uniform rectangular-emitter sample per vertex. It
never reads the renderer's probe field, shaders, buffers, or screenshots.

Reported standard errors describe Monte Carlo noise only. Finite bounce depth,
point-versus-image-ROI averaging and the renderer's PBR Fresnel term are separate
biases; these results are a transport reference, not a pixel-perfect golden image.
"""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import time

import numpy as np


EPSILON = 1.0e-6


def prepared_quads(scene):
    return [{"center": np.asarray(quad["center"], dtype=np.float64),
             "normal": np.asarray(quad["normal"], dtype=np.float64),
             "low": np.min(quad["corners"], axis=0),
             "high": np.max(quad["corners"], axis=0),
             "albedo": np.asarray(quad["albedo"], dtype=np.float64)}
            for quad in scene["quads"]]


def intersect(origins, directions, quads, max_distance=None):
    """Closest exact plane/rectangle intersection; includes opaque back faces."""
    origins = np.asarray(origins, dtype=np.float64)
    directions = np.asarray(directions, dtype=np.float64)
    count = len(origins)
    nearest = (np.full(count, np.inf) if max_distance is None
               else np.asarray(max_distance, dtype=np.float64).copy())
    if nearest.ndim == 0:
        nearest = np.full(count, float(nearest))
    indices = np.full(count, -1, dtype=np.int32)
    for index, quad in enumerate(quads):
        axis = int(np.argmax(np.abs(quad["normal"])))
        denominator = directions[:, axis]
        valid = np.abs(denominator) > 1.0e-12
        distances = np.full(count, np.inf)
        np.divide(quad["center"][axis] - origins[:, axis], denominator,
                  out=distances, where=valid)
        valid &= (distances > EPSILON) & (distances < nearest)
        # Plane-axis bounds are implicit. Avoid inf * 0 on parallel rays.
        for other in (value for value in range(3) if value != axis):
            coordinate = origins[:, other] + np.where(valid, distances, 0) * directions[:, other]
            valid &= (coordinate >= quad["low"][other] - EPSILON) & \
                     (coordinate <= quad["high"][other] + EPSILON)
        nearest[valid] = distances[valid]
        indices[valid] = index
    return nearest, indices


def cosine_directions(normals, rng):
    normals = np.asarray(normals, dtype=np.float64)
    count = len(normals)
    sample = rng.random((count, 2))
    radius, phi = np.sqrt(sample[:, 0]), 2.0 * np.pi * sample[:, 1]
    axis = np.zeros_like(normals)
    use_x = np.abs(normals[:, 1]) > 0.99
    axis[:, 1] = ~use_x
    axis[:, 0] = use_x
    tangent = np.cross(axis, normals)
    tangent /= np.linalg.norm(tangent, axis=1, keepdims=True)
    bitangent = np.cross(normals, tangent)
    return (tangent * (radius * np.cos(phi))[:, None] +
            bitangent * (radius * np.sin(phi))[:, None] +
            normals * np.sqrt(1.0 - sample[:, 0])[:, None])


def source_samples(light, order=32):
    """Independent tensor-product quadrature over the exact rectangular emitter."""
    nodes, weights = np.polynomial.legendre.leggauss(order)
    a, b = np.meshgrid(nodes, nodes, indexing="ij")
    wa, wb = np.meshgrid(weights, weights, indexing="ij")
    u, v = np.asarray(light["u"]), np.asarray(light["v"])
    positions = np.asarray(light["position"]) + a.ravel()[:, None] * u + b.ravel()[:, None] * v
    area_weights = np.linalg.norm(np.cross(u, v)) * (wa * wb).ravel()
    return positions, area_weights


def irradiance_sample(points, normals, emitter_positions, area_weights, light, quads):
    """Matched arrays of receivers and samples; output already includes dA/PDF."""
    origins = points + normals * (4 * EPSILON)
    delta = emitter_positions - origins
    distance_sq = np.sum(delta * delta, axis=1)
    distance = np.sqrt(distance_sq)
    directions = delta / distance[:, None]
    receiver_cosine = np.maximum(np.sum(normals * directions, axis=1), 0)
    source_cosine = np.maximum(-directions @ np.asarray(light["normal"]), 0)
    attenuation = np.ones(len(points))
    if light.get("range", 0) > 0:
        attenuation = np.maximum(1 - distance_sq / light["range"] ** 2, 0) ** 2
    _, blocker = intersect(origins, directions, quads, distance - 4 * EPSILON)
    weight = receiver_cosine * source_cosine * area_weights * attenuation / distance_sq
    weight[blocker >= 0] = 0
    return weight[:, None] * np.asarray(light["radiance"])


def direct_irradiance(points, normals, scene, quads=None, order=32):
    points, normals = np.atleast_2d(points), np.atleast_2d(normals)
    quads = prepared_quads(scene) if quads is None else quads
    emitters, weights = source_samples(scene["light"], order)
    result = np.zeros((len(points), 3))
    for sample, weight in zip(emitters, weights):
        result += irradiance_sample(points, normals, np.broadcast_to(sample, points.shape),
                                    np.full(len(points), weight), scene["light"], quads)
    return result


def reference_receiver(scene, receiver, samples=16384, bounces=6, seed=42, direct_order=32):
    if samples < 2 or bounces < 1 or direct_order < 1:
        raise ValueError("Reference requires samples >= 2, bounces >= 1 and quadrature order >= 1")
    started = time.perf_counter()
    rng = np.random.default_rng(seed)
    quads = prepared_quads(scene)
    point = np.asarray(receiver["position"], dtype=np.float64)
    normal = np.asarray(receiver["normal"], dtype=np.float64)
    albedo = np.asarray(receiver["albedo"], dtype=np.float64)
    direct = direct_irradiance(point, normal, scene, quads, direct_order)[0] * albedo / np.pi
    values = np.zeros((samples, 3))
    throughput = np.broadcast_to(albedo, (samples, 3)).copy()
    origins = np.broadcast_to(point + normal * 4 * EPSILON, (samples, 3)).copy()
    normals = np.broadcast_to(normal, (samples, 3))
    directions = cosine_directions(normals, rng)
    active = np.arange(samples)
    albedos = np.asarray([quad["albedo"] for quad in quads])
    face_normals = np.asarray([quad["normal"] for quad in quads])
    light = scene["light"]
    emitter_u, emitter_v = np.asarray(light["u"]), np.asarray(light["v"])
    area = 4.0 * np.linalg.norm(np.cross(emitter_u, emitter_v))
    alive_counts = []
    for _ in range(bounces - 1):
        distance, hit = intersect(origins, directions, quads)
        valid = hit >= 0
        safe_hit = np.maximum(hit, 0)
        # Room faces are authored single-sided. A backface still occludes,
        # but is not an exterior reflector in the reference cutaway shell.
        valid &= np.sum(face_normals[safe_hit] * directions, axis=1) < 0
        active = active[valid]
        if not len(active):
            alive_counts.append(0)
            break
        hit = hit[valid]
        points = origins[valid] + directions[valid] * distance[valid, None]
        normals = face_normals[hit]
        rho = albedos[hit]
        throughput = throughput[valid]
        uv = rng.random((len(active), 2)) * 2.0 - 1.0
        emitters = np.asarray(light["position"]) + uv[:, :1] * emitter_u + uv[:, 1:] * emitter_v
        illumination = irradiance_sample(points, normals, emitters, np.full(len(active), area), light, quads)
        values[active] += throughput * rho * illumination / np.pi
        throughput *= rho
        origins = points + normals * 4 * EPSILON
        directions = cosine_directions(normals, rng)
        alive_counts.append(len(active))
    indirect = values.mean(axis=0)
    error = values.std(axis=0, ddof=1) / np.sqrt(samples)
    luminance_weights = np.asarray([0.2126, 0.7152, 0.0722])
    luminance_values = values @ luminance_weights
    return {"directRadiance": direct.tolist(), "indirectRadiance": indirect.tolist(),
            "totalRadiance": (direct + indirect).tolist(), "standardError": error.tolist(),
            "directLuminance": float(direct @ luminance_weights),
            "indirectLuminance": float(indirect @ luminance_weights),
            "totalLuminance": float((direct + indirect) @ luminance_weights),
            "luminanceStandardError": float(luminance_values.std(ddof=1) / np.sqrt(samples)),
            "samples": samples, "bounces": bounces, "seed": seed,
            "directQuadratureOrder": direct_order, "alivePathsByBounce": alive_counts,
            "elapsedSeconds": time.perf_counter() - started}


def reference_receivers(scene, samples=16384, bounces=6, seed=42, direct_order=32):
    return {receiver["name"]: reference_receiver(scene, receiver, samples, bounces,
                                                seed + index, direct_order)
            for index, receiver in enumerate(scene["receivers"])}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("manifest", type=Path)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--samples", type=int, default=16384)
    parser.add_argument("--bounces", type=int, default=6)
    parser.add_argument("--seed", type=int, default=42)
    parser.add_argument("--variant", action="append", default=[])
    args = parser.parse_args()
    manifest = json.loads(args.manifest.read_text(encoding="utf-8"))
    results = {"schemaVersion": 1, "model": "independent CPU Lambertian exact-quad transport",
               "manifestSha256": hashlib.sha256(args.manifest.read_bytes()).hexdigest(),
               "limitations": ["finite bounce count truncates transport",
                               "receiver reference is a point; capture ROI averages pixels",
                               "GPU diffuse PBR includes Fresnel energy attenuation",
                               "reported error covers Monte Carlo variance, not these biases"],
               "fixtures": {}}
    for scene in manifest["fixtures"]:
        if args.variant and scene["name"] not in args.variant:
            continue
        results["fixtures"][scene["name"]] = reference_receivers(scene, args.samples, args.bounces, args.seed)
        print(f"Referenced {scene['name']}: {args.samples} paths/receiver, {args.bounces} bounces")
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(results, indent=2) + "\n", encoding="utf-8")


if __name__ == "__main__":
    main()
