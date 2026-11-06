"""Validate atmosphere transport over finite camera-to-surface distances."""

import argparse
import math
from pathlib import Path

import numpy as np
from PIL import ImageChops

from atmosphere_runtime_regression import RAYLEIGH, density, extinction, incoming, ray
from display_grade_runtime_regression import HEIGHT, WIDTH, capture, expected_rgb
from gltf_extended_materials_runtime_regression import fixture


COLOR = np.array([.125, .25, .5])
BACKGROUND = np.array([.06, .10, .16])


def surface_reference(x, y, scale):
    direction = ray(x, y)
    distance_km = (4 / -direction[2]) * scale / 1000
    radius, top = 6360., 6460.
    origin = np.array([0., radius + 1., 0.])
    azimuth, elevation = map(math.radians, (130, 35))
    sun = np.array([math.sin(azimuth) * math.cos(elevation), math.sin(elevation),
                    math.cos(azimuth) * math.cos(elevation)])
    step = distance_km / 192
    points = origin + ((np.arange(192) + .5) * step)[:, None] * direction
    medium = density(points, radius)
    segments = extinction(medium) * step
    attenuation = np.exp(-np.cumsum(segments, axis=0) + .5 * segments)
    mu = direction @ sun
    rayleigh = 3 * (1 + mu**2) / (16 * math.pi)
    mie = .36 / (4 * math.pi * (1.64 - 1.6 * mu)**1.5)
    scatter = rayleigh * medium[:, :1] * RAYLEIGH + mie * .003996 * medium[:, 1:2]
    irradiance = incoming(points, sun, radius, top, (1., 1., 1.))
    return COLOR * np.exp(-segments.sum(axis=0)) + (attenuation * irradiance * scatter).sum(axis=0) * step


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("renderer", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--baseline", action="store_true", help="Capture before aerial perspective is available")
    args = parser.parse_args()
    renderer, output = args.renderer.resolve(), args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    (output / "vk_layer_settings.txt").write_text("khronos_validation.validate_sync = true\n")
    models = {}
    for kind, alpha in (("opaque", 1.), ("transparent", .5)):
        model = output / f"aerial-{kind}.gltf"
        material = dict(pbrMetallicRoughness=dict(baseColorFactor=[*COLOR, alpha]),
                        extensions=dict(KHR_materials_unlit={}))
        if alpha < 1:
            material["alphaMode"] = "BLEND"
        fixture(model, {"material": material})
        models[kind] = model

    sample = (WIDTH // 2, HEIGHT // 2 - 1)
    sky = (0, 0, WIDTH, 40)
    for technique in ("deferred-raster", "forward-raster"):
        for kind, model in models.items():
            flags = ("--sky", "atmosphere", "--environment-intensity", "1",
                     "--sky-solar", "1", "--sky-sun-azimuth", "130", "--sky-sun-elevation", "35",
                     "--sky-observer-km", "1", "--background-color", *map(str, BACKGROUND))
            if args.baseline:
                capture(renderer, output, model, technique, f"{kind}-baseline", flags)
                continue
            images = []
            for scale in (0, 500, 1000):
                image = capture(renderer, output, model, technique, f"{kind}-{scale}", (
                    *flags, "--sky-aerial-scale", str(scale)))
                expected = surface_reference(*sample, scale)
                if kind == "transparent":
                    expected = .5 * (expected + BACKGROUND)
                expected_rgb8 = expected_rgb(expected, "aces", 1., 1.)
                observed = image.getpixel(sample)
                if max(abs(a - b) for a, b in zip(observed, expected_rgb8, strict=True)) > 3:
                    raise RuntimeError(f"{technique} {kind} scale {scale}: {observed} expected {expected_rgb8}")
                images.append(image)
            baseline = output / f"{technique}-{kind}-baseline.png"
            if baseline.exists():
                from PIL import Image
                with Image.open(baseline) as original:
                    if ImageChops.difference(images[0], original.convert("RGB")).getbbox():
                        raise RuntimeError(f"{technique}: disabled aerial perspective changed {kind}")
            if ImageChops.difference(images[0].crop(sky), images[-1].crop(sky)).getbbox():
                raise RuntimeError(f"{technique}: aerial perspective changed the background")
            if images[0].getpixel(sample) == images[-1].getpixel(sample):
                raise RuntimeError(f"{technique}: aerial scale did not change the receiver")
        if not args.baseline:
            print(f"{technique}: opaque and transparent surface transport matched the CPU reference", flush=True)


if __name__ == "__main__":
    main()
