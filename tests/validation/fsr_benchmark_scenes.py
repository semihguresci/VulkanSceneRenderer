"""Scene, capture and image-comparison inputs for the FSR performance benchmark."""

import json
import math
from pathlib import Path

import numpy as np

from graphics_concurrency_benchmark import file_identity, require
from large_scene_asset_benchmark import local_gltf_resource


KHRONOS_MODELS = "https://github.com/KhronosGroup/glTF-Sample-Assets/tree/8f492390ddc7a2ae221a1369ef560589cf8c70fb/Models/"
TEKLA_MODEL = "https://github.com/buildingSMART/IFC5-development/tree/1a63082ada967c683cfacee2005f8f749c8e1b79/examples/Tekla%20House"
PRESETS = {
    "sponza": dict(provider="primary", minimumObjectCount=1, upstream=KHRONOS_MODELS + "Sponza",
                   extensions=(".gltf",), fov=65, exposure=1.6, environment=1.2, sun=3,
                   sunDirection=(.35, -1, -.08), start=((8, 1.8, 0), (-8, 2.8, 0)),
                   end=((7.55, 1.88, .25), (-8, 2.65, .25))),
    "chess": dict(provider="primary", minimumObjectCount=49, upstream=KHRONOS_MODELS + "ABeautifulGame",
                  extensions=(".gltf",), fov=37, exposure=1.6, environment=1, sun=2,
                  sunDirection=(.16, -1, -.38), start=((.65, .46, .8), (0, -.045, 0)),
                  end=((.60, .44, .83), (0, -.035, 0))),
    "tekla": dict(provider="auxiliary", minimumObjectCount=8000, upstream=TEKLA_MODEL,
                  extensions=(".ifcx", ".ifc"), fov=45, exposure=1.3, environment=.8, sun=3.5,
                  sunDirection=(-.55, -.75, -.35), start=((90, 53, 50), (31, 13, -19)),
                  end=((87.5, 52, 51.5), (31, 13, -19))),
}
HIGH_ERROR_CUTOFF = 16


def benchmark_scene(name, model):
    """Materialize a fixed scene preset with an absolute source path and fixed lighting."""
    preset = PRESETS[name]
    model = Path(model).resolve(strict=True)
    require(model.is_file() and model.suffix.lower() in preset["extensions"], f"Unexpected {name} input: {model}")
    arguments = ["--model", str(model), "--import-scale", "1", "--camera-fov", str(preset["fov"]),
                 "--sky", "plain", "--sky-horizon", ".55", ".64", ".72", "--tone-map", "agx",
                 "--exposure", str(preset["exposure"]), "--environment-intensity", str(preset["environment"]),
                 "--directional-intensity", str(preset["sun"]), "--directional-direction", *map(str, preset["sunDirection"])]
    camera = {name: {"position": list(position), "target": list(target)}
              for name, (position, target) in (("start", preset["start"]), ("end", preset["end"]))}
    return dict(name=name, model=str(model), arguments=arguments, provider=preset["provider"],
                minimumObjectCount=preset["minimumObjectCount"], camera=camera, upstream=preset["upstream"])


def benchmark_sequence(scene, frames, sample_frames, telemetry_frames, taa):
    """Hold the overview through warm-up, then move gently during the measured interval."""
    samples, telemetry = sorted(sample_frames), sorted(telemetry_frames)
    require(samples and len(set(samples)) == len(samples) and 1 <= samples[0] <= samples[-1] <= frames,
            "Invalid benchmark capture frame selection")
    require(len(set(telemetry)) == len(telemetry) and all(1 <= frame <= frames for frame in telemetry),
            "Invalid benchmark telemetry frame selection")
    camera = scene["camera"]
    departure = samples[0] if samples[0] < frames else max(1, frames // 2)
    keys = [dict(frame=1, **camera["start"])]
    if departure > 1:
        keys.append(dict(frame=departure, **camera["start"]))
    if frames > departure:
        keys.append(dict(frame=frames, **camera["end"]))
    return dict(schemaVersion=1, frames=frames, sampleFrames=samples, telemetryFrames=telemetry,
                events=[dict(frame=1, taa=taa)], camera=keys)


def scene_identity(scene):
    """Hash the authored scene and each local external buffer, image or native IFC sidecar."""
    model = Path(scene["model"])
    paths = {Path(model.name)}
    if model.suffix.lower() == ".gltf":
        source = json.loads(model.read_bytes())
        for entry in source.get("buffers", []) + source.get("images", []):
            if "uri" in entry and (resource := local_gltf_resource(model.parent, entry["uri"])):
                paths.add(resource)
    elif model.suffix.lower() == ".ifc" and model.with_suffix(".ifcx").is_file():
        paths.add(Path(model.with_suffix(".ifcx").name))
    files = []
    for relative in sorted(paths, key=Path.as_posix):
        path = model.parent / relative
        files.append({**file_identity(path), "relativePath": relative.as_posix(), "bytes": path.stat().st_size})
    return dict(model=str(model), upstream=scene["upstream"], files=files)


def compare_images(native, actual):
    """Compare matched sRGB PNGs in 8-bit channel units; an exact match has null PSNR."""
    require(native.shape == actual.shape and native.ndim == 3 and native.shape[2] == 3 and native.size > 0,
            "Expected matching nonempty RGB images")
    require(native.dtype == actual.dtype == np.uint8, "Expected decoded 8-bit RGB images")
    absolute = np.abs(np.subtract(native, actual, dtype=np.int16))
    max_channel = np.max(absolute, axis=2)
    high_error = int(np.count_nonzero(max_channel >= HIGH_ERROR_CUTOFF))
    changed = int(np.count_nonzero(max_channel))
    pixel_count = max_channel.size
    mse = float(np.mean(np.square(absolute, dtype=np.float32), dtype=np.float64))
    return dict(meanAbsoluteError8Bit=float(absolute.mean(dtype=np.float64)), rmse8Bit=math.sqrt(mse),
                psnrDb=10 * math.log10(255 ** 2 / mse) if mse else None,
                maxChannelError8Bit=int(max_channel.max()), changedPixelCount=changed,
                changedPixelFraction=changed / pixel_count, highErrorPixelCount=high_error,
                highErrorPixelFraction=high_error / pixel_count, highErrorCutoff8Bit=HIGH_ERROR_CUTOFF,
                pixelCount=pixel_count)
