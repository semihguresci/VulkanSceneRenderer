"""Exercise optional OMI sky metadata through the Vulkan environment bake."""

import argparse
import copy
import json
import math
from pathlib import Path
import subprocess

import numpy as np
from PIL import ImageChops

from display_grade_runtime_regression import capture
from gltf_extended_materials_runtime_regression import extension_material, fixture
from hdr_environment_runtime_regression import write_float_exr


EXTENSION = "OMI_environment_sky"
PLAIN = (.125, .375, .625)
HORIZON = (.25, .5, .125)
ZENITH = (.75, .125, .25)
GROUND = (.0625, .25, .625)


def write_gradient_reference(path):
    rows = []
    width, height = 512, 256
    for y in range(height):
        latitude = (.5 - (y + .5) / height) * math.pi
        row = []
        for x in range(width):
            longitude = ((x + .5) / width - .5) * math.tau
            # Authored +90° rotation around Z turns the gradient's up toward -X.
            elevation = math.asin(-math.cos(latitude) * math.cos(longitude))
            pole, exponent = (ZENITH, 4) if elevation >= 0 else (GROUND, 2)
            band = abs(elevation) * 2 / math.pi
            weight = 1 - (1 - band) ** exponent
            row.append(tuple((1 - weight) * a + weight * b for a, b in zip(HORIZON, pole, strict=True)))
        rows.append(row)
    write_float_exr(path, rows)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("renderer", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    renderer, output = args.renderer.resolve(), args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    (output / "vk_layer_settings.txt").write_text("khronos_validation.validate_sync = true\n")
    scene = output / "authored.gltf"
    fixture(scene, {"material": extension_material(base=(.8, .8, .8), metallic=1., specular=1.)})
    model = json.loads(scene.read_text(encoding="utf-8"))
    model.setdefault("extensionsUsed", []).append(EXTENSION)
    model.setdefault("extensions", {})[EXTENSION] = dict(skies=[
        dict(type="plain", plain=dict(color=PLAIN)),
        dict(type="gradient", rotation=[0, 0, math.sqrt(.5), math.sqrt(.5)],
             gradient=dict(horizonColor=HORIZON, topColor=ZENITH, bottomColor=GROUND,
                           topCurve=.25, bottomCurve=.5)),
    ])
    model["scenes"] = [dict(nodes=[0], extensions={EXTENSION: dict(sky=i)}) for i in (0, 1)]
    scene.write_text(json.dumps(model), encoding="utf-8")
    bad_scene = output / "invalid.gltf"
    malformed = copy.deepcopy(model)
    malformed["scenes"][0]["extensions"][EXTENSION]["sky"] = 12
    bad_scene.write_text(json.dumps(malformed), encoding="utf-8")
    unsupported_scene = output / "unsupported.gltf"
    unsupported = copy.deepcopy(model)
    unsupported["extensions"][EXTENSION]["skies"][0] = dict(type="physical", physical={})
    unsupported_scene.write_text(json.dumps(unsupported), encoding="utf-8")
    required = output / "required.gltf"
    required.write_text(json.dumps(dict(model, extensionsRequired=[EXTENSION])), encoding="utf-8")
    gradient = output / "gradient.exr"
    write_gradient_reference(gradient)

    for technique in ("deferred-raster", "forward-raster"):
        def run(label, asset, expected_mode, *flags):
            image = capture(renderer, output, asset, technique, label, ("--environment-intensity", "1", *flags))
            telemetry = json.loads((output / f"{technique}-{label}.telemetry.json").read_text(encoding="utf-8"))
            if telemetry["environment"]["skyMode"] != expected_mode:
                raise RuntimeError(f"{technique} {label}: wrong active sky")
            return image

        fallback = run("disabled", scene, 0)
        for label, asset, reason in (("invalid", bad_scene, "out of range"),
                                     ("unsupported", unsupported_scene, "only plain and gradient")):
            rejected = run(label, asset, 0, "--gltf-sky")
            log = (output / f"{technique}-{label}.log").read_text(encoding="utf-8")
            if reason not in log or ImageChops.difference(fallback, rejected).getbbox():
                raise RuntimeError(f"{technique}: rejected metadata changed the default scene")
        plain = run("plain", scene, 1, "--gltf-sky")
        reference = run("plain-reference", scene, 1, "--sky", "plain", "--sky-horizon", *map(str, PLAIN))
        if ImageChops.difference(plain, reference).getbbox():
            raise RuntimeError(f"{technique}: authored plain sky differs from explicit settings")
        curved = run("curved", scene, 2, "--gltf-sky", "--gltf-scene", "1")
        reference = run("curved-reference", scene, 0, "--environment", str(gradient), "--gltf-scene", "1")
        error = np.abs(np.asarray(curved, dtype=np.int16) - np.asarray(reference, dtype=np.int16))
        if np.percentile(error, 95) > 2 or error.max() > 5:
            raise RuntimeError(f"{technique}: tilted gradient differs from EXR by {error.max()} codes")
        print(f"{technique}: optional plain, tilted gradient, and rejected metadata passed", flush=True)

    rejected = subprocess.run([str(renderer), "--hidden", "--no-ui", "--model", str(required),
                               "--gltf-sky", "--capture-frame", "1", "--screenshot", str(output / "required.png")],
                              cwd=renderer.parent, capture_output=True, timeout=45)
    if b"unsupported_required_extension" not in rejected.stdout + rejected.stderr:
        raise RuntimeError("Required draft extension was not diagnosed")


if __name__ == "__main__":
    main()
