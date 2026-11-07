"""Compare Vulkan atmosphere captures with higher-resolution CPU quadrature."""

import argparse
import math
from pathlib import Path
import subprocess

import numpy as np

from display_grade_runtime_regression import HEIGHT, WIDTH, capture, expected_rgb
from gltf_extended_materials_runtime_regression import extension_material, fixture


RAYLEIGH = np.array([.005802, .013558, .033100])
OZONE = np.array([.000650, .001881, .000085])
GROUND = np.array([.12, .10, .08])


def density(points, radius):
    altitude = np.maximum(np.linalg.norm(points, axis=-1) - radius, 0)
    return np.stack((np.exp(-altitude / 8), np.exp(-altitude / 1.2),
                     np.maximum(1 - abs(altitude - 25) / 15, 0)), axis=-1)


def extinction(values):
    return values @ np.stack((RAYLEIGH, np.full(3, .004440), OZONE))


def intersections(points, direction, radius):
    projection = points @ direction
    discriminant = projection**2 + radius**2 - np.sum(points**2, axis=-1)
    root = np.sqrt(np.maximum(discriminant, 0))
    return -projection - root, -projection + root, discriminant >= 0


def incoming(points, sun, radius, top, scales):
    near, _, hit = intersections(points, sun, radius)
    _, far, _ = intersections(points, sun, top)
    interval = np.maximum(far, 0)
    offsets = interval[:, None] * ((np.arange(96) + .5) / 96)
    columns = density(points[:, None, :] + offsets[..., None] * sun, radius).mean(axis=1)
    transmission = np.exp(-extinction(columns * scales * interval[:, None]))
    return np.where((hit & (near >= 0))[:, None], 0, transmission)


def atmosphere(direction, azimuth, elevation, altitude, solar=20., scales=(1., 1., 1.),
               radius=6360., height=100.):
    az, el = map(math.radians, (azimuth, elevation))
    sun = np.array([math.cos(el) * math.sin(az), math.sin(el), math.cos(el) * math.cos(az)])
    origin = np.array([0., radius + altitude, 0.])
    top = radius + height
    near, _, hit = intersections(origin, direction, radius)
    ground = hit and near >= 0
    length = near if ground else intersections(origin, direction, top)[1]
    step = max(length, 0) / 192
    points = origin + ((np.arange(192) + .5) * step)[:, None] * direction
    medium = density(points, radius) * scales
    segments = extinction(medium) * step
    attenuation = np.exp(-np.cumsum(segments, axis=0) + .5 * segments)
    phase_angle = direction @ sun
    rayleigh_phase = 3 * (1 + phase_angle**2) / (16 * math.pi)
    mie_phase = .36 / (4 * math.pi * (1.64 - 1.6 * phase_angle)**1.5)
    scatter = rayleigh_phase * medium[:, :1] * RAYLEIGH + mie_phase * .003996 * medium[:, 1:2]
    radiance = np.sum(attenuation * incoming(points, sun, radius, top, scales) * scatter, axis=0) * step
    if ground:
        normal = (origin + length * direction) / radius
        illumination = incoming(np.array([normal * (radius + .002)]), sun, radius, top, scales)[0]
        radiance += np.exp(-segments.sum(axis=0)) * illumination * GROUND * max(normal @ sun, 0) / math.pi
    return solar * radiance


def ray(x, y):
    tangent = math.tan(math.radians(22.5))
    direction = np.array([(2*(x + .5) - WIDTH) / HEIGHT * tangent,
                          (1 - 2*(y + .5) / HEIGHT) * tangent, -1.])
    return direction / np.linalg.norm(direction)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("renderer", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    renderer, output = args.renderer.resolve(), args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    (output / "vk_layer_settings.txt").write_text("khronos_validation.validate_sync = true\n")
    model = output / "receiver.gltf"
    fixture(model, {"material": extension_material(base=(.8, .8, .8), metallic=1., specular=1.)})
    cases = {
        "day": (130, 45, .002, 20, (1., 1., 1.)),
        "sunrise": (130, 5, .002, 20, (1., 1., 1.)),
        "sunset": (-130, 5, .002, 20, (1., 1., 1.)),
        "below-horizon": (130, -30, .002, 20, (1., 1., 1.)),
        "high-observer": (130, 45, 30, 20, (1., 1., 1.)),
        "thin-aerosol": (130, 5, .002, 20, (1., 0., 1.)),
        "no-rayleigh": (130, 45, .002, 20, (0., 1., 1.)),
        "no-ozone": (130, 5, .002, 20, (1., 1., 0.)),
        "zero-solar": (130, 45, .002, 0, (1., 1., 1.)),
    }
    pixels = ((36, 24), (160, 24), (283, 24), (36, 218), (283, 218))
    references = {name: [expected_rgb(atmosphere(ray(*p), *settings), "aces", 1., 1.)
                         for p in pixels] for name, settings in cases.items()}
    for technique in ("deferred-raster", "forward-raster"):
        max_error = 0
        observations = {}
        for name, (azimuth, elevation, altitude, solar, scales) in cases.items():
            image = capture(renderer, output, model, technique, name, (
                "--sky", "atmosphere", "--environment-intensity", "1",
                "--sky-sun-azimuth", str(azimuth), "--sky-sun-elevation", str(elevation),
                "--sky-observer-km", str(altitude), "--sky-solar", str(solar),
                "--sky-rayleigh", str(scales[0]), "--sky-mie", str(scales[1]), "--sky-ozone", str(scales[2])))
            samples = np.array([image.getpixel(p) for p in pixels])
            observations[name] = samples
            error = int(np.max(abs(samples - references[name])))
            max_error = max(max_error, error)
            if error > 12:
                raise RuntimeError(f"{technique} {name}: atmosphere differs by {error} codes: {samples}")
        if observations["zero-solar"].any() or observations["below-horizon"].any():
            raise RuntimeError(f"{technique}: dark atmosphere emitted visible radiance")
        for a, b in (("day", "sunrise"), ("sunrise", "sunset"), ("day", "high-observer"),
                     ("sunrise", "thin-aerosol"), ("day", "no-rayleigh")):
            if np.max(abs(observations[a] - observations[b])) < 5:
                raise RuntimeError(f"{technique}: {a} and {b} were insufficiently different")
        if not np.any(observations["sunrise"] != observations["no-ozone"]):
            raise RuntimeError(f"{technique}: ozone did not affect the sampled sky")
        print(f"{technique}: {len(cases)} atmosphere captures passed, maximum error "
              f"{max_error} sRGB codes", flush=True)

    invalid = subprocess.run([str(renderer), "--sky", "atmosphere", "--sky-observer-km", "101"],
                             cwd=renderer.parent, capture_output=True, timeout=20)
    if invalid.returncode == 0 or b"outside the supported range" not in invalid.stderr:
        raise RuntimeError("Observer outside the atmosphere was not rejected")


if __name__ == "__main__":
    main()
