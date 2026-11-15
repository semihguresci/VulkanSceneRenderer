"""Compare independent base and clearcoat normals in Vulkan raster lighting."""

from __future__ import annotations

import argparse
import json
import math
import os
from pathlib import Path
import subprocess
import tempfile


WIDTH, HEIGHT = 192, 144
CAMERA_FOV = 45
CAMERA_Z = 4
ALPHA = 0.64
BASE_COLOR = (0.025, 0.016, 0.01)
COAT_FACTOR = 0.85
COAT_ROUGHNESS = 0.32
BASE_RETENTION = 1 - 0.04 * COAT_FACTOR
BASE_TEXEL = (81, 128, 248, 255)
COAT_TEXEL = (174, 128, 248, 255)
FLAT_NORMAL = (0, 0, 1)
CLEARCOAT = "KHR_materials_clearcoat"
SPECULAR = "KHR_materials_specular"
GPU_OPT_IN = "CONTAINER_RUN_GPU_GLTF_CLEARCOAT_NORMALS"
TECHNIQUES = ("forward-raster", "deferred-raster")
LIGHTING = ("direct", "environment")
PROFILES = {
    "bare-flat": (False, False, False, False),
    "bare-base-map": (False, True, False, False),
    "bare-baked": (False, False, False, True),
    "coat-flat": (True, False, False, False),
    "coat-no-map": (True, True, False, False),
    "coat-textured": (True, True, True, False),
    "coat-baked": (True, False, False, True),
}
PANEL_ROWS = {"OPAQUE": HEIGHT // 2 - 16, "BLEND": HEIGHT // 2 + 16}


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


def unit(vector):
    length = math.sqrt(sum(value * value for value in vector))
    return tuple(value / length for value in vector)


BAKED_NORMAL = unit(tuple(2 * value / 255 - 1 for value in COAT_TEXEL[:3]))
# Aim the central view's half-vector at the coat normal. The authored base
# texture tilts the opposite way, while the surface and camera stay fixed.
LIGHT_TO_SURFACE = tuple(
    -2 * BAKED_NORMAL[2] * n + v for n, v in zip(BAKED_NORMAL, FLAT_NORMAL, strict=True))


def fixture(path, profile):
    import numpy as np
    from gltf_accessor_runtime_regression import Asset

    coated, base_map, coat_map, baked = PROFILES[profile]
    asset = Asset()
    normal = BAKED_NORMAL if baked else FLAT_NORMAL
    tangent = unit((normal[2], 0, -normal[0]))
    shared_attributes = dict(
        NORMAL=asset.accessor("VEC3", [normal] * 4),
        TANGENT=asset.accessor("VEC4", [(*tangent, 1)] * 4),
        TEXCOORD_0=asset.accessor("VEC2", [[0, 0], [1, 0], [1, 1], [0, 1]]),
    )
    indices = asset.accessor("SCALAR", [0, 1, 2, 0, 2, 3], 5123, indices=True)

    def normal_texture(texel):
        return dict(index=asset.image(np.tile(texel, (2, 2, 1)).astype(np.uint8)))

    material_extensions = {SPECULAR: dict(specularFactor=0)}
    base_texture = normal_texture(BASE_TEXEL) if base_map else None
    if coated:
        coat = dict(clearcoatFactor=COAT_FACTOR, clearcoatRoughnessFactor=COAT_ROUGHNESS)
        if coat_map:
            coat["clearcoatNormalTexture"] = normal_texture(COAT_TEXEL)
        material_extensions[CLEARCOAT] = coat

    asset.model["meshes"] = [dict(name="Clearcoat panels", primitives=[])]
    for mode, ymin, ymax, alpha in (("OPAQUE", 0.025, 8, 1), ("BLEND", -8, -0.025, ALPHA)):
        positions = [[-8, ymin, 0], [8, ymin, 0], [8, ymax, 0], [-8, ymax, 0]]
        attributes = dict(POSITION=asset.accessor("VEC3", positions), **shared_attributes)
        material = dict(name=f"{profile} {mode}", alphaMode=mode, extensions=material_extensions,
                        pbrMetallicRoughness=dict(baseColorFactor=[*BASE_COLOR, alpha],
                                                  metallicFactor=0, roughnessFactor=0.6))
        if base_texture is not None:
            material["normalTexture"] = base_texture
        primitives = asset.model["meshes"][0]["primitives"]
        primitives.append(dict(attributes=attributes, indices=indices, material=len(primitives)))
        asset.model["materials"].append(material)
    asset.model["extensionsUsed"] = sorted(material_extensions)
    asset.write(path)


def environment(path):
    from hdr_environment_runtime_regression import write_float_exr

    width, height = 64, 32
    rows = []
    # Each channel encodes a different directional component. It distinguishes
    # the reflected lobes without depending on a particular cubemap face or yaw.
    for y in range(height):
        theta = math.pi * (y + 0.5) / height
        row = []
        for x in range(width):
            phi = math.tau * (x + 0.5) / width
            direction = (math.cos(phi) * math.sin(theta), math.cos(theta),
                         math.sin(phi) * math.sin(theta))
            row.append(tuple(0.1 + 1.8 * (1 + value) ** 2 for value in direction))
        rows.append(row)
    write_float_exr(path, rows)


def capture(renderer, output, model, panorama, technique, lighting, timeout, record):
    os.environ.setdefault("OPENCV_IO_ENABLE_OPENEXR", "1")
    import cv2
    import numpy as np
    from gltf_extended_materials_runtime_regression import validation_log_error

    target = output / f"{technique}-{lighting}-{model.stem}"
    image, telemetry, log = (target.with_suffix(suffix) for suffix in (".exr", ".telemetry.json", ".log"))
    direct = lighting == "direct"
    command = [
        str(renderer), "--hidden", "--no-ui", "--screenshot-viewport", "--validation",
        "--width", str(WIDTH), "--height", str(HEIGHT), "--msaa", "1",
        "--render-technique", technique, "--display-mode", "lit", "--exposure", "1",
        "--camera-position", "0", "0", str(CAMERA_Z), "--camera-target", "0", "0", "0",
        "--camera-fov", str(CAMERA_FOV), "--import-scale", "1",
        "--no-taa", "--no-bloom", "--no-ray-query", "--ray-shadows", "raster", "--no-ddgi",
        "--background-color", "0", "0", "0", "--bounce-intensity", "0",
        "--environment-intensity", "0" if direct else "1",
        "--directional-intensity", "2" if direct else "0",
        "--directional-direction", *map(str, LIGHT_TO_SURFACE), "--directional-color", "1", "1", "1",
        "--warmup-frames", "12", "--capture-frame", "13", "--fixed-dt", ".016666667",
        "--model", str(model), "--screenshot", str(image),
    ]
    if not direct:
        command += ["--environment", str(panorama)]
    record.update(command=command, image=str(image), telemetry=str(telemetry), log=str(log))
    with log.open("w", encoding="utf-8") as stream:
        process = subprocess.run(command, cwd=renderer.parent, stdout=stream, stderr=subprocess.STDOUT,
            timeout=timeout, env=dict(os.environ, VK_LAYER_SETTINGS_PATH=str(output), VK_LOADER_DEBUG="layer"),
            creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
    log_text = log.read_text(encoding="utf-8", errors="replace")
    require(process.returncode == 0 and not validation_log_error(log_text), f"Capture failed: {log}")
    require('Insert instance layer "VK_LAYER_KHRONOS_validation"' in log_text,
            f"Vulkan validation layer unavailable: {log}")
    record["validationClean"] = True

    state = json.loads(telemetry.read_text(encoding="utf-8"))
    metadata = json.loads(Path(f"{image}.capture.json").read_text(encoding="utf-8"))
    require(state["technique"] == technique and state["msaaSamples"] == 1 and
            state["scene"]["primaryObjectCount"] >= 1 and not state["oit"]["overflowed"],
            f"Incorrect raster or panel state: {telemetry}")
    lights = state["lighting"]
    require(lights["directionalIntensity"] == (2 if direct else 0) and
            lights["environmentIntensity"] == (0 if direct else 1) and
            lights["bounceIntensity"] == lights["pointLightCount"] == lights["areaLightCount"] == 0,
            f"Incorrect light isolation: {telemetry}")
    if not direct:
        published = state["environment"]
        require(not published["loading"] and Path(published["hdrPath"]).resolve() == panorama,
                f"Authored environment was not published: {telemetry}")
    require(metadata["source"] == "raster-scene-composite" and metadata["colorSpace"] == "linear-sRGB"
            and not metadata["includesUi"], f"Wrong raw capture source: {image}")
    pixels = cv2.imdecode(np.frombuffer(image.read_bytes(), dtype=np.uint8), cv2.IMREAD_UNCHANGED)
    require(pixels is not None and pixels.dtype == np.float32 and
            pixels.shape == (HEIGHT, WIDTH, 3) and np.isfinite(pixels).all(), f"Invalid EXR: {image}")
    return pixels[:, :, ::-1]


def panel(image, mode):
    y = PANEL_ROWS[mode]
    x = WIDTH // 2
    return image[y - 6:y + 6, x - 6:x + 6]


def maximum_delta(first, second):
    return float(abs(first - second).max())


def compare(images, technique, lighting, mode):
    values = {case: panel(image, mode) for case, image in images.items()}
    flat = values["coat-flat"] - BASE_RETENTION * values["bare-flat"]
    fallback = values["coat-no-map"] - BASE_RETENTION * values["bare-base-map"]
    textured = values["coat-textured"] - BASE_RETENTION * values["bare-base-map"]
    baked = values["coat-baked"] - BASE_RETENTION * values["bare-baked"]
    peak = max(float(flat.max()), float(baked.max()))
    separation = maximum_delta(flat.mean(axis=(0, 1)), baked.mean(axis=(0, 1)))
    measured = dict(technique=technique, lighting=lighting, alphaMode=mode, peakLobe=peak,
        lobeSeparation=separation, absentMapTolerance=0.0004 + float(flat.max()) * 0.02,
        texturedMapTolerance=0.001 + peak * 0.02,
        absentMapDelta=maximum_delta(flat, fallback), texturedMapDelta=maximum_delta(textured, baked),
        baseMapDelta=maximum_delta(values["bare-flat"], values["bare-base-map"]),
        flatMeanRgb=flat.mean(axis=(0, 1)).tolist(), bakedMeanRgb=baked.mean(axis=(0, 1)).tolist(),
        texturedMeanRgb=textured.mean(axis=(0, 1)).tolist())
    return measured


def verify(measured):
    require(measured["lobeSeparation"] > max(0.005, 4 * measured["texturedMapTolerance"]),
            f"The baked and flat coats did not provide distinguishable lobes: {measured}")
    require(measured["absentMapDelta"] <= measured["absentMapTolerance"],
            f"The coat without a coat texture followed the base normal map: {measured}")
    require(measured["texturedMapDelta"] <= measured["texturedMapTolerance"],
            f"The coat normal map failed to reproduce the baked normal lobe: {measured}")
    if measured["lighting"] == "direct":
        require(measured["baseMapDelta"] > 0.001,
                f"The direct control did not exercise a visibly perturbed base normal: {measured}")
        require(max(measured["bakedMeanRgb"]) > 2 * max(measured["flatMeanRgb"]),
                f"The direct light did not favor the authored coat normal: {measured}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--renderer", type=Path)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--managed-output", action="store_true", help="Create a fresh run directory within output")
    parser.add_argument("--require-opt-in", action="store_true", help=f"Skip unless {GPU_OPT_IN}=1")
    parser.add_argument("--generate-only", action="store_true", help="Write the glTF and HDR fixtures without GPU work")
    parser.add_argument("--timeout", type=int, default=120, help="Capture timeout in seconds per case")
    args = parser.parse_args()
    if args.require_opt_in and not args.generate_only and os.environ.get(GPU_OPT_IN) != "1":
        print(f"Set {GPU_OPT_IN}=1 to run the GPU clearcoat-normal regression.", flush=True)
        return 77
    if not args.generate_only and (args.renderer is None or not args.renderer.is_file()):
        parser.error("--renderer must name an existing executable")
    output = args.output.resolve()
    if args.managed_output:
        output.mkdir(parents=True, exist_ok=True)
        output = Path(tempfile.mkdtemp(prefix="run-", dir=output))
    print(f"Clearcoat normal fixtures and captures: {output}", flush=True)
    sources = output / "sources"
    sources.mkdir(parents=True, exist_ok=False)
    fixtures = {profile: sources / f"{profile}.gltf" for profile in PROFILES}
    for profile, path in fixtures.items():
        fixture(path, profile)
    panorama = sources / "directional-gradient.exr"
    environment(panorama)
    if args.generate_only:
        return 0

    renderer = args.renderer.resolve()
    (output / "vk_layer_settings.txt").write_text("khronos_validation.validate_sync = true\n", encoding="utf-8")
    report = dict(passed=False, captures=[], checks=[])
    try:
        for technique in TECHNIQUES:
            for lighting in LIGHTING:
                images = {}
                for profile, path in fixtures.items():
                    record = dict(technique=technique, lighting=lighting, profile=profile)
                    report["captures"].append(record)
                    images[profile] = capture(renderer, output, path, panorama, technique, lighting, args.timeout, record)
                for mode in PANEL_ROWS:
                    result = compare(images, technique, lighting, mode)
                    report["checks"].append(result)
                    verify(result)
                print(f"{technique} {lighting}: opaque and blend clearcoat lobes matched", flush=True)
        report["passed"] = True
    finally:
        (output / "report.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
