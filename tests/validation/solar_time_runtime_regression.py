"""Validate geolocated solar positions through both Vulkan lighting paths."""

import argparse
import json
import math
from pathlib import Path
import subprocess

import numpy as np
from PIL import ImageChops

from display_grade_runtime_regression import capture
from gltf_extended_materials_runtime_regression import extension_material, fixture


def solar_angles(ray):
    east, up, north = (-component for component in ray)
    return math.degrees(math.atan2(east, north)) % 360, math.degrees(math.atan2(up, math.hypot(east, north)))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("renderer", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    renderer, output = args.renderer.resolve(), args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    (output / "vk_layer_settings.txt").write_text("khronos_validation.validate_sync = true\n")
    model = output / "solar-time-receiver.gltf"
    fixture(model, {"material": extension_material(base=(.4, .4, .4))})
    for technique in ("deferred-raster", "forward-raster"):
        def run(name, latitude, longitude, date, time, offset):
            image = capture(renderer, output, model, technique, name, (
                "--sky", "atmosphere", "--environment-intensity", "1", "--directional-intensity", "1",
                "--sky-location", str(latitude), str(longitude), "--sky-date", date,
                "--sky-time", time, "--sky-utc-offset", str(offset)))
            state = json.loads((output / f"{technique}-{name}.telemetry.json").read_text(encoding="utf-8"))
            return image, state["lighting"]

        # Reda and Andreas SPA example, 30 seconds rounded down and no refraction.
        _, colorado = run("colorado", 39.742476, -105.1786, "2003-10-17", "12:30", -420)
        azimuth, elevation = solar_angles(colorado["directionalDirection"])
        if abs(azimuth - 194.34) > .3 or abs(elevation - 39.87) > .3:
            raise RuntimeError(f"{technique}: unexpected Colorado solar angles {azimuth}, {elevation}")

        local, local_light = run("utc-plus14", 1.87, -157.4, "2026-06-21", "00:30", 840)
        utc, utc_light = run("utc-previous-day", 1.87, -157.4, "2026-06-20", "10:30", 0)
        if (not np.allclose(local_light["directionalDirection"], utc_light["directionalDirection"], atol=1e-6)
                or ImageChops.difference(local, utc).getbbox()):
            raise RuntimeError(f"{technique}: UTC rollover changed the sun or the captured image")

        _, june = run("polar-summer", 69.65, 18.96, "2026-06-21", "00:30", 120)
        _, december = run("polar-winter", 69.65, 18.96, "2026-12-21", "12:00", 60)
        if solar_angles(june["directionalDirection"])[1] < 2 or june["directionalIntensity"] <= 0:
            raise RuntimeError(f"{technique}: missing midnight sun")
        if solar_angles(december["directionalDirection"])[1] >= -1 or december["directionalIntensity"] != 0:
            raise RuntimeError(f"{technique}: unexpected beam during polar night")
        print(f"{technique}: reference position, UTC rollover, midnight sun and polar night passed", flush=True)

    invalid = [
        ("--sky-date", "2025-02-29"), ("--sky-time", "24:00"),
        ("--sky-location", "91", "0"), ("--sky-utc-offset", "+841"),
    ]
    for flags in invalid:
        process = subprocess.run([str(renderer), "--sky", "atmosphere", *flags],
                                 cwd=renderer.parent, capture_output=True, timeout=20)
        if process.returncode == 0:
            raise RuntimeError(f"Invalid solar input accepted: {flags}")


if __name__ == "__main__":
    main()
