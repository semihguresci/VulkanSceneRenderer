"""Exercise the shared higher-order atmosphere table on both Vulkan raster paths."""

import argparse
from pathlib import Path

import numpy as np
from PIL import ImageChops

from display_grade_runtime_regression import HEIGHT, WIDTH, capture
from gltf_extended_materials_runtime_regression import fixture


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("renderer", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    renderer, output = args.renderer.resolve(), args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    (output / "vk_layer_settings.txt").write_text("khronos_validation.validate_sync = true\n")

    models = {}
    for name, alpha in (("opaque", 1.), ("transparent", .5)):
        model = output / f"multiple-{name}.gltf"
        material = dict(pbrMetallicRoughness=dict(baseColorFactor=[.03, .06, .12, alpha]),
                        extensions=dict(KHR_materials_unlit={}))
        if alpha < 1:
            material["alphaMode"] = "BLEND"
        fixture(model, {"material": material})
        models[name] = model

    common = ("--sky", "atmosphere", "--environment-intensity", "1",
              "--sky-solar", "10", "--sky-sun-azimuth", "130",
              "--sky-sun-elevation", "5", "--sky-observer-km", "1")
    enabled = ("--sky-multiple-scattering",)
    sky = (WIDTH // 2, 24)
    surface = (WIDTH // 2, HEIGHT // 2 - 1)
    for technique in ("deferred-raster", "forward-raster"):
        def render(name, *options, kind="opaque"):
            return capture(renderer, output, models[kind], technique, name, (*common, *options))

        single = render("sky-single")
        multiple = render("sky-multiple", *enabled)
        sky_gain = np.array(multiple.getpixel(sky)) - single.getpixel(sky)
        if max(sky_gain) < 3 or min(sky_gain) < 0:
            raise RuntimeError(f"{technique}: unexpected sky gain {sky_gain}")
        if single.getpixel(surface) != multiple.getpixel(surface):
            raise RuntimeError(f"{technique}: atmosphere changed an unlit surface with aerial transport off")

        for name, overrides in (
            ("no-sun", ("--sky-solar", "0")),
            ("night", ("--sky-sun-elevation", "-30")),
            ("no-scattering", ("--sky-rayleigh", "0", "--sky-mie", "0")),
        ):
            baseline = render(f"{name}-single", *overrides)
            higher = render(f"{name}-multiple", *overrides, *enabled)
            if ImageChops.difference(baseline, higher).getbbox():
                raise RuntimeError(f"{technique}: multiple scattering changed the {name} control")

        for kind in models:
            aerial = ("--sky-aerial-scale", "1000", "--background-color", ".06", ".10", ".16")
            single = render(f"{kind}-single", *aerial, kind=kind)
            multiple = render(f"{kind}-multiple", *aerial, *enabled, kind=kind)
            gain = np.array(multiple.getpixel(surface)) - single.getpixel(surface)
            if max(gain) < 2 or min(gain) < 0:
                raise RuntimeError(f"{technique}: unexpected {kind} aerial gain {gain}")
            if single.getpixel(sky) != multiple.getpixel(sky):
                raise RuntimeError(f"{technique}: atmosphere changed the solid background override")
            print(f"{technique} {kind}: aerial gain {gain.tolist()} RGB codes", flush=True)
        print(f"{technique}: sky gain {sky_gain.tolist()} RGB codes", flush=True)


if __name__ == "__main__":
    main()
