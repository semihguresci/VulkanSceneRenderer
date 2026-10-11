#!/usr/bin/env python3
"""Render independent CPU diffuse reference images for architectural DDGI fixtures.

Defaults produce 192x128 images with 64 cosine paths/pixel and eight scattering
vertices, plus exact-quad direct visibility and 12x12 emitter quadrature. This is
a low-resolution, noisy Lambertian reference, not a GPU pixel golden. PNGs use
the fixed exposure/ACES-fit/sRGB display transform of the native quality run;
NPZ files preserve scene-linear means and Monte Carlo luminance standard errors.
"""

from __future__ import annotations

import argparse
from datetime import datetime, timezone
import hashlib
import json
from pathlib import Path
import time

import numpy as np
from PIL import Image

from quality_fixtures import build_scene, gltf_scene
from reference import (EPSILON, cosine_directions, intersect, irradiance_sample,
                       prepared_quads, source_samples)


def camera_rays(camera, width, height):
    eye = np.asarray(camera["position"], dtype=np.float64)
    forward = np.asarray(camera["target"], dtype=np.float64) - eye
    forward /= np.linalg.norm(forward)
    right = np.cross(forward, [0, 1, 0])
    right /= np.linalg.norm(right)
    up = np.cross(right, forward)
    y, x = np.mgrid[:height, :width]
    ndc_x = ((x + 0.5) / width * 2.0 - 1.0) * width / height
    ndc_y = 1.0 - (y + 0.5) / height * 2.0
    tangent = np.tan(np.deg2rad(camera["fov"]) * 0.5)
    directions = forward + tangent * (ndc_x[..., None] * right + ndc_y[..., None] * up)
    directions /= np.linalg.norm(directions, axis=-1, keepdims=True)
    directions = directions.reshape(-1, 3)
    return np.broadcast_to(eye, directions.shape), directions


def direct_batch(points, normals, scene, quads, emitter_positions, area_weights):
    # Flatten receiver/source pairs to avoid a Python intersection loop for
    # every source sample. The caller bounds the batch memory footprint.
    source_count = len(emitter_positions)
    values = irradiance_sample(np.repeat(points, source_count, axis=0),
                               np.repeat(normals, source_count, axis=0),
                               np.tile(emitter_positions, (len(points), 1)),
                               np.tile(area_weights, len(points)), scene["light"], quads)
    return values.reshape(len(points), source_count, 3).sum(axis=1)


def indirect_batch(points, normals, albedo, scene, quads, samples, bounces, rng):
    pixel_count = len(points)
    path_count = pixel_count * samples
    values = np.zeros((path_count, 3))
    if bounces == 1:
        return np.zeros((pixel_count, 3)), np.zeros(pixel_count)
    albedos = np.asarray([quad["albedo"] for quad in quads])
    face_normals = np.asarray([quad["normal"] for quad in quads])
    throughput = np.repeat(albedo, samples, axis=0)
    normals = np.repeat(normals, samples, axis=0)
    origins = np.repeat(points, samples, axis=0) + normals * 4 * EPSILON
    directions = cosine_directions(normals, rng)
    active = np.arange(path_count)
    light = scene["light"]
    emitter_u, emitter_v = np.asarray(light["u"]), np.asarray(light["v"])
    area = 4.0 * np.linalg.norm(np.cross(emitter_u, emitter_v))
    for _ in range(bounces - 1):
        distances, hits = intersect(origins, directions, quads)
        valid = hits >= 0
        valid &= np.sum(face_normals[np.maximum(hits, 0)] * directions, axis=1) < 0
        active = active[valid]
        if not len(active):
            break
        hits = hits[valid]
        positions = origins[valid] + directions[valid] * distances[valid, None]
        normals = face_normals[hits]
        throughput = throughput[valid]
        rho = albedos[hits]
        uv = rng.random((len(active), 2)) * 2.0 - 1.0
        emitter_positions = np.asarray(light["position"]) + uv[:, :1] * emitter_u + uv[:, 1:] * emitter_v
        illumination = irradiance_sample(positions, normals, emitter_positions,
                                        np.full(len(active), area), light, quads)
        values[active] += throughput * rho * illumination / np.pi
        throughput *= rho
        origins = positions + normals * 4 * EPSILON
        directions = cosine_directions(normals, rng)
    values = values.reshape(pixel_count, samples, 3)
    luminance = values @ np.asarray([0.2126, 0.7152, 0.0722])
    return values.mean(axis=1), luminance.std(axis=1, ddof=1) / np.sqrt(samples)


def display_rgb(radiance, exposure):
    color = np.maximum(radiance * exposure, 0)
    mapped = np.clip((color * (2.51 * color + 0.03)) /
                     (color * (2.43 * color + 0.59) + 0.14), 0, 1)
    srgb = np.where(mapped <= 0.0031308, mapped * 12.92,
                    1.055 * mapped ** (1 / 2.4) - 0.055)
    return np.clip(np.rint(srgb * 255), 0, 255).astype(np.uint8)


def render_scene(scene, width=192, height=128, samples=64, bounces=8,
                 direct_order=12, batch_pixels=128, seed=42):
    if width < 1 or height < 1 or samples < 2 or bounces < 1 or direct_order < 1 or batch_pixels < 1:
        raise ValueError("Positive dimensions/depth/order/batch and at least two paths are required")
    started = time.perf_counter()
    quads = prepared_quads(scene)
    origins, directions = camera_rays(scene["camera"], width, height)
    distances, hits = intersect(origins, directions, quads)
    quad_normals = np.asarray([quad["normal"] for quad in quads])
    quad_albedos = np.asarray([quad["albedo"] for quad in quads])
    visible = hits >= 0
    visible &= np.sum(quad_normals[np.maximum(hits, 0)] * directions, axis=1) < 0
    pixel_indices = np.flatnonzero(visible)
    direct = np.zeros_like(directions)
    indirect = np.zeros_like(directions)
    standard_error = np.zeros(len(directions))
    emitters, weights = source_samples(scene["light"], direct_order)
    rng = np.random.default_rng(seed)
    for first in range(0, len(pixel_indices), batch_pixels):
        pixels = pixel_indices[first:first + batch_pixels]
        hit = hits[pixels]
        points = origins[pixels] + directions[pixels] * distances[pixels, None]
        normals, rho = quad_normals[hit], quad_albedos[hit]
        direct[pixels] = rho * direct_batch(points, normals, scene, quads, emitters, weights) / np.pi
        indirect[pixels], standard_error[pixels] = indirect_batch(
            points, normals, rho, scene, quads, samples, bounces, rng)
    shape = (height, width)
    result = {"radiance": (direct + indirect).reshape(*shape, 3),
              "directRadiance": direct.reshape(*shape, 3),
              "indirectRadiance": indirect.reshape(*shape, 3),
              "luminanceStandardError": standard_error.reshape(shape),
              "frontFacingGeometry": visible.reshape(shape),
              "hitQuad": hits.reshape(shape)}
    active_error = standard_error[visible]
    summary = {"elapsedSeconds": time.perf_counter() - started,
               "frontFacingPixels": int(visible.sum()), "totalPixels": width * height,
               "meanLuminanceStandardError": float(active_error.mean()) if len(active_error) else 0,
               "p95LuminanceStandardError": float(np.percentile(active_error, 95)) if len(active_error) else 0,
               "maximumLuminanceStandardError": float(active_error.max()) if len(active_error) else 0}
    return result, summary


def digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--variant", action="append", choices=("doorway", "sealed-thin"), default=[])
    parser.add_argument("--width", type=int, default=192)
    parser.add_argument("--height", type=int, default=128)
    parser.add_argument("--samples", type=int, default=64)
    parser.add_argument("--bounces", type=int, default=8)
    parser.add_argument("--direct-order", type=int, default=12)
    parser.add_argument("--batch-pixels", type=int, default=128)
    parser.add_argument("--seed", type=int, default=42)
    parser.add_argument("--exposure", type=float, default=0.04)
    args = parser.parse_args()
    args.output = args.output.resolve()
    args.output.mkdir(parents=True, exist_ok=True)
    source_directory = Path(__file__).resolve().parent
    started = datetime.now(timezone.utc)
    metadata = {"schemaVersion": 1, "startedUtc": started.isoformat(),
                "model": "independent CPU opaque Lambertian exact-quad transport",
                "resolution": [args.width, args.height], "cosinePathsPerPixel": args.samples,
                "scatteringVertices": args.bounces, "directEmitterQuadratureOrder": args.direct_order,
                "primarySampling": "one pixel-center camera ray; no antialiasing or spatial filter",
                "batchPixels": args.batch_pixels, "seed": args.seed,
                "exposure": args.exposure, "displayTransform": "ACES fitted curve then sRGB",
                "sourceSha256": {name: digest(source_directory / name)
                                 for name in ("render_reference.py", "quality_fixtures.py", "reference.py")},
                "limitations": ["finite scattering depth truncates remaining diffuse transport",
                                "64 or 128 paths/pixel visibly retain Monte Carlo noise",
                                "standard error covers indirect Monte Carlo variance only",
                                "direct quadrature can bias partially occluded emitter visibility",
                                "low resolution and pixel-center sampling alias thin edges",
                                "GPU diffuse PBR includes Fresnel energy attenuation absent here",
                                "no glossy, transmission, emission, environment or image denoising"],
                "scenes": {}}
    for index, variant in enumerate(args.variant or ["doorway", "sealed-thin"]):
        scene = build_scene(variant, args.width, args.height)
        result, summary = render_scene(scene, args.width, args.height, args.samples, args.bounces,
                                       args.direct_order, args.batch_pixels, args.seed + index)
        stem = f"architecture-{variant}-cpu-reference"
        png, linear = args.output / (stem + ".png"), args.output / (stem + ".npz")
        Image.fromarray(display_rgb(result["radiance"], args.exposure)).save(png)
        np.savez_compressed(linear, **result)
        geometry = json.dumps(gltf_scene(scene), indent=2) + "\n"
        metadata["scenes"][variant] = {"camera": scene["camera"], "light": scene["light"],
                                      "geometryGltfSha256": hashlib.sha256(geometry.encode()).hexdigest(),
                                      "seed": args.seed + index, "png": png.name, "pngSha256": digest(png),
                                      "linearData": linear.name, "linearDataSha256": digest(linear), **summary}
        print(json.dumps({"scene": variant, "png": str(png), **summary}), flush=True)
    metadata["finishedUtc"] = datetime.now(timezone.utc).isoformat()
    metadata["elapsedSeconds"] = (datetime.now(timezone.utc) - started).total_seconds()
    (args.output / "reference-image-results.json").write_text(json.dumps(metadata, indent=2) + "\n",
                                                            encoding="utf-8")


if __name__ == "__main__":
    main()
