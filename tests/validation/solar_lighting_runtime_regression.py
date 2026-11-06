"""Verify the solar disk and the directional light share atmospheric extinction."""

import argparse
import json
import math
from pathlib import Path

import numpy as np
from PIL import ImageChops

from atmosphere_runtime_regression import incoming
from display_grade_runtime_regression import HEIGHT, WIDTH, capture, expected_rgb
from gltf_extended_materials_runtime_regression import extension_material, fixture


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("renderer", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    renderer, output = args.renderer.resolve(), args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    (output / "vk_layer_settings.txt").write_text("khronos_validation.validate_sync = true\n")
    model = output / "solar-receiver.gltf"
    fixture(model, {"material": extension_material(base=(.4, .4, .4))})
    elevation = math.radians(15)
    irradiance = incoming(np.array([[0., 6360.002, 0.]]),
                          np.array([0., math.sin(elevation), math.cos(elevation)]),
                          6360., 6460., (1., 1., 1.))[0]
    disk_y = round(HEIGHT / 2 - math.tan(elevation) * HEIGHT / (2 * math.tan(math.radians(22.5))) - .5)
    disk_region = (WIDTH // 2 - 2, disk_y - 2, WIDTH // 2 + 2, disk_y + 3)
    sky_corner = (0, 0, 48, 40)
    for technique in ("deferred-raster", "forward-raster"):
        def run(name, *flags):
            image = capture(renderer, output, model, technique, name, (
                "--sky", "atmosphere", "--environment-intensity", "1",
                "--sky-sun-elevation", "15", "--sky-solar", "1", *flags))
            state = json.loads((output / f"{technique}-{name}.telemetry.json").read_text(encoding="utf-8"))
            return image, state["lighting"]

        linked, light = run("linked", "--directional-intensity", "1", "--exposure", ".25")
        unlinked, _ = run("unlinked", "--sky-no-direct-sun", "--exposure", ".25")
        actual = np.array(light["directionalColor"]) * light["directionalIntensity"]
        if not np.allclose(actual, irradiance, rtol=.015, atol=.002):
            raise RuntimeError(f"{technique}: directional irradiance {actual} expected {irradiance}")
        if not np.allclose(light["directionalDirection"], (0, -math.sin(elevation), -math.cos(elevation)),
                           atol=1e-5):
            raise RuntimeError(f"{technique}: incorrect directional ray orientation")
        center = (WIDTH // 2, HEIGHT // 2)
        if max(a - b for a, b in zip(linked.getpixel(center), unlinked.getpixel(center), strict=True)) < 10:
            raise RuntimeError(f"{technique}: linked sun did not light the receiver")
        if ImageChops.difference(linked.crop(sky_corner), unlinked.crop(sky_corner)).getbbox():
            raise RuntimeError(f"{technique}: directional link changed scattered sky")

        _, previous = run("authored", "--sky-no-direct-sun", "--directional-intensity", "2",
                          "--directional-direction", "0", "-1", "0")
        if previous["directionalDirection"] != [0., -1., 0.] or previous["directionalIntensity"] != 2.:
            raise RuntimeError(f"{technique}: disabling the link did not preserve independent lighting")

        disk, _ = run("disk", "--sky-sun-azimuth", "180", "--exposure", ".00005")
        yaw_disk, yaw_light = run("disk-yaw", "--sky-sun-azimuth", "90",
                                  "--environment-yaw", "90", "--exposure", ".00005")
        peak = np.array(disk.crop(disk_region)).reshape(-1, 3).max(axis=0)
        expected = expected_rgb(irradiance / (math.pi * math.sin(math.radians(.26667))**2) * .00005,
                                "aces", 1., 1.)
        if np.max(abs(peak - expected)) > 3 or not peak.any():
            raise RuntimeError(f"{technique}: visible disk {peak} expected {expected}")
        if ImageChops.difference(disk.crop(disk_region), yaw_disk.crop(disk_region)).getbbox():
            raise RuntimeError(f"{technique}: yaw displaced or recolored the disk")
        if not np.allclose(yaw_light["directionalDirection"], (0, -math.sin(elevation), math.cos(elevation)),
                           atol=1e-5):
            raise RuntimeError(f"{technique}: directional light did not rotate with the disk")

        night, dark = run("night", "--sky-sun-azimuth", "180", "--sky-sun-elevation", "-10",
                          "--directional-intensity", "1", "--exposure", ".00005")
        if dark["directionalIntensity"] != 0 or np.asarray(night.crop(disk_region)).any():
            raise RuntimeError(f"{technique}: planet failed to occlude the solar beam")
        print(f"{technique}: solar energy, disk yaw, opt-out and planet occlusion passed", flush=True)


if __name__ == "__main__":
    main()
