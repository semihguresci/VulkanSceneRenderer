"""Validate procedural sky preset round trips and Vulkan capture equivalence."""

import argparse
import json
from pathlib import Path
import subprocess

from PIL import ImageChops

from display_grade_runtime_regression import capture, verify_presets
from gltf_extended_materials_runtime_regression import extension_material, fixture


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("renderer", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    renderer, output = args.renderer.resolve(), args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    (output / "vk_layer_settings.txt").write_text("khronos_validation.validate_sync = true\n")
    model = output / "preset-receiver.gltf"
    fixture(model, {"material": extension_material(base=(.4, .4, .4))})

    def run(*flags, accepted=True):
        process = subprocess.run([str(renderer), *map(str, flags)], cwd=renderer.parent,
                                 capture_output=True, text=True, encoding="utf-8",
                                 errors="replace", timeout=20)
        if (process.returncode == 0) != accepted:
            raise RuntimeError(f"Unexpected result for {flags}: {process.stderr}")

    options = ("--sky", "atmosphere", "--sky-ground", ".25", ".125", ".0625",
               "--sky-rayleigh", "1.5", "--sky-mie", ".7", "--sky-ozone", ".75",
               "--sky-planet-km", "6400", "--sky-atmosphere-km", "90", "--sky-observer-km", ".5",
               "--sky-solar", "3", "--sky-location", "48.85", "2.35", "--sky-date", "2028-02-29",
               "--sky-time", "13:20", "--sky-utc-offset", "60", "--sky-aerial-scale", "25",
               "--sky-multiple-scattering")
    preset = output / "sky-\u0131.json"
    copy = output / "sky-copy.json"
    run(*options, "--save-sky-preset", preset)
    run("--sky-preset", preset, "--save-sky-preset", copy)
    original = preset.read_bytes()
    if copy.read_bytes() != original:
        raise RuntimeError("Sky preset round trip changed settings")
    run("--sky-preset", preset, "--sky-observer-km", "1", "--save-sky-preset", copy)
    value = json.loads(original)
    if json.loads(copy.read_bytes()) != dict(value, observerHeightKm=1):
        raise RuntimeError("CLI override changed unexpected sky settings")
    invalid = output / "invalid-sky.json"
    legacy = dict(value, version=1)
    for field in ("aerialMetersPerUnit", "gradientUp", "zenithCurve", "groundCurve", "multipleScattering"):
        del legacy[field]
    invalid.write_text(json.dumps(legacy), encoding="utf-8")
    run("--sky-preset", invalid, "--save-sky-preset", copy)
    if json.loads(copy.read_bytes()) != dict(value, aerialMetersPerUnit=0, multipleScattering=False):
        raise RuntimeError("Version 1 preset did not migrate with aerial perspective disabled")
    legacy = dict(legacy, version=2, aerialMetersPerUnit=value["aerialMetersPerUnit"])
    invalid.write_text(json.dumps(legacy), encoding="utf-8")
    run("--sky-preset", invalid, "--save-sky-preset", copy)
    if json.loads(copy.read_bytes()) != dict(value, multipleScattering=False):
        raise RuntimeError("Version 2 preset did not migrate with a linear, upright gradient")
    legacy = dict(value, version=3)
    del legacy["multipleScattering"]
    invalid.write_text(json.dumps(legacy), encoding="utf-8")
    run("--sky-preset", invalid, "--save-sky-preset", copy)
    if json.loads(copy.read_bytes()) != dict(value, multipleScattering=False):
        raise RuntimeError("Version 3 preset did not migrate with multiple scattering disabled")
    valid_destination = copy.read_bytes()
    malformed = [
        json.dumps(dict(value, mode="hdr")), json.dumps(dict(value, version=5)),
        json.dumps(dict(value, ground=[0, 1])), json.dumps(dict(value, useLocation=1)),
        json.dumps(dict(value, year=2027)), json.dumps(dict(value, observerHeightKm=100)),
        json.dumps(dict(value, unknown=1)),
        original.decode().replace('"version": 4', '"version": 4, "version": 4'),
        " " * 4097,
    ]
    for item in malformed:
        invalid.write_text(item, encoding="utf-8")
        run("--sky-preset", invalid, "--save-sky-preset", copy, accepted=False)
        if copy.read_bytes() != valid_destination:
            raise RuntimeError("Rejected preset damaged existing destination")
    blocked = output / "blocked-sky.json"
    blocked.mkdir(exist_ok=True)
    run("--sky-preset", preset, "--save-sky-preset", blocked, accepted=False)
    if not blocked.is_dir() or list(output.glob("blocked-sky.json.tmp-*")):
        raise RuntimeError("Failed preset publication damaged the destination or leaked a temporary")
    for mode in ("plain", "gradient"):
        run("--sky-preset", preset, "--sky", mode, "--save-sky-preset", copy)
        if json.loads(copy.read_bytes()) != dict(value, mode=mode):
            raise RuntimeError(f"{mode} preset lost dormant settings")
    verify_presets(renderer, output, ("--tone-map", "agx", "--contrast", "1.25"))
    for technique in ("deferred-raster", "forward-raster"):
        shared = ("--environment-intensity", "1", "--directional-intensity", "1")
        explicit = capture(renderer, output, model, technique, "sky-flags", (*options, *shared))
        saved = capture(renderer, output, model, technique, "sky-preset", ("--sky-preset", str(preset), *shared))
        if ImageChops.difference(explicit, saved).getbbox():
            raise RuntimeError(f"{technique}: preset changed the Vulkan output")
        print(f"{technique}: atmosphere preset reproduced the explicit settings pixel-for-pixel", flush=True)
    print("Sky and display preset validation passed")


if __name__ == "__main__":
    main()
