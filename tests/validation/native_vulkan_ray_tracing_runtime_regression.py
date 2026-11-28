"""Compare native Vulkan ray-tracing pipeline and inline ray-query captures."""

import argparse
import hashlib
import json
import math
import os
from pathlib import Path
import subprocess
import tempfile

from display_capture_test_support import emitter_fixture, validation_log_error


OPT_IN = "CONTAINER_RUN_GPU_NATIVE_RAY_TRACING"
WIDTH, HEIGHT = 160, 120
FOV_DEGREES = 45
SEED, SAMPLES, BATCH = 314, 8, 4
INLINE, NATIVE = "path-tracing", "ray-tracing"
BACKENDS = {INLINE: "inline-ray-query", NATIVE: "vulkan-ray-tracing-pipeline"}
RAY_STAGES = ("rgen", "rmiss", "rchit", "rahit")
CAPTURE_TIMEOUT_SECONDS = 480
FAR_EMISSION = (4.0, 2.0, 1.0)
NEAR_EMISSION = (0.0, 4.0, 0.0)


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


def shader_hashes(renderer):
    directory = renderer.parent / "spv_shaders"
    names = [f"reference_pathtrace.{stage}.spv" for stage in RAY_STAGES]
    return {name: hashlib.sha256((directory / name).read_bytes()).hexdigest() for name in names}


def emitter(color, alpha=None):
    material = {
        "emissiveFactor": [channel / 4 for channel in color],
        "extensions": {"KHR_materials_emissive_strength": {"emissiveStrength": 4},
                       "KHR_materials_specular": {"specularFactor": 0}},
        "pbrMetallicRoughness": {"baseColorFactor": [0, 0, 0, 1 if alpha is None else alpha],
                                 "metallicFactor": 0, "roughnessFactor": 1},
    }
    if alpha is not None:
        material.update(alphaMode="MASK", alphaCutoff=0.5)
    return material


def fixture(path):
    asset = emitter_fixture(path, emitter(FAR_EMISSION))
    asset["extensionsUsed"].append("KHR_materials_specular")
    asset["nodes"][0]["name"] = "Far emitter"
    primitive = asset["meshes"][0]["primitives"][0]
    # These share the emitter's authored mesh coordinates. The left foreground
    # must be rejected by any-hit, while the right foreground must occlude it.
    for name, x, alpha in (("Rejected alpha mask", -0.45, 0.25), ("Accepted alpha mask", 0.45, 0.75)):
        index = len(asset["meshes"])
        asset["materials"].append(emitter(NEAR_EMISSION, alpha))
        asset["meshes"].append({"primitives": [{**primitive, "material": index}]})
        asset["nodes"].append({"name": name, "mesh": index, "translation": [x, 0, 0.6], "scale": [0.27, 0.45, 1]})
        asset["scenes"][0]["nodes"].append(index)
    path.write_text(json.dumps(asset), encoding="utf-8")


def capture(renderer, model, output, name, checkpoints, events, environment, report, *, query_disabled=False):
    base = output / name
    sequence = base.with_suffix(".sequence.json")
    frames = [number for number, _, _ in checkpoints]
    sequence.write_text(json.dumps(dict(schemaVersion=1, frames=frames[-1], sampleFrames=frames, events=events)),
                        encoding="utf-8")
    command = [str(renderer), "--model", str(model), "--import-scale", "1",
               "--hidden", "--no-ui", "--validation", "--screenshot-viewport",
               "--width", str(WIDTH), "--height", str(HEIGHT), "--msaa", "1", "--no-taa", "--no-bloom", "--no-ddgi",
               "--render-technique", checkpoints[0][1], "--display-mode", "lit",
               "--camera-position", "0", "0", "4", "--camera-target", "0", "0", "0", "--camera-fov", str(FOV_DEGREES),
               "--environment-intensity", "0", "--directional-intensity", "0", "--bounce-intensity", "0",
               "--sky", "plain", "--sky-horizon", "0", "0", "0", "--fixed-dt", ".016666667",
               "--path-spp", str(BATCH), "--path-samples", str(SAMPLES), "--path-bounces", "2", "--path-seed", str(SEED),
               "--capture-sequence", str(sequence), "--screenshot", str(base.with_suffix(".exr"))]
    if query_disabled:
        command.append("--no-ray-query")
    log = base.with_suffix(".log")
    run = {"name": name, "command": command, "log": str(log), "captures": []}
    report["runs"].append(run)
    with log.open("w", encoding="utf-8") as stream:
        try:
            result = subprocess.run(command, cwd=renderer.parent, env=environment,
                                    stdout=stream, stderr=subprocess.STDOUT, timeout=CAPTURE_TIMEOUT_SECONDS,
                                    creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
        except subprocess.TimeoutExpired as error:
            raise RuntimeError(f"Capture timed out. See {log}") from error
    run["exitCode"] = result.returncode
    contents = log.read_text(encoding="utf-8", errors="replace")
    require(result.returncode == 0 and not validation_log_error(contents), f"Vulkan capture failed. See {log}")
    for action in ('Insert instance', 'Inserted device'):
        require(f'{action} layer "VK_LAYER_KHRONOS_validation"' in contents, f"Validation layer missing. See {log}")
    require("Vulkan ray tracing pipeline: enabled" in contents, f"Native ray tracing pipeline unavailable. See {log}")
    query_status = "disabled" if query_disabled else "enabled"
    require(f"Ray queries: {query_status}" in contents, f"Unexpected ray query configuration. See {log}")

    images = []
    for frame, technique, samples in checkpoints:
        image = base.with_suffix(".exr") if frame == frames[-1] else output / f"{name}.frame-{frame:04d}.exr"
        images.append(read_capture(image, technique, samples, run, frame))
    return images


def read_capture(image, technique, samples, run, frame):
    import cv2
    import numpy as np

    metadata = json.loads(Path(f"{image}.capture.json").read_bytes())
    telemetry = json.loads(image.with_suffix(".telemetry.json").read_bytes())
    path, state = metadata.get("referencePath", {}), telemetry.get("referencePath", {})
    width, height = metadata["width"], metadata["height"]
    run["captures"].append({"frame": frame, "image": str(image), "technique": telemetry["technique"],
                            "referencePath": path, "camera": telemetry["camera"], "extent": (width, height)})
    require(telemetry["technique"] == technique and state.get("active") and telemetry["validationEnabled"],
            f"Expected active validated {technique} at {image}. See {run['log']}")
    expected_backend = BACKENDS[technique]
    require(path.get("backend") == state.get("backend") == expected_backend,
            f"Expected backend {expected_backend} in both capture records at {image}")
    require(path["attemptedSamplesPerPixel"] == state["attemptedSamples"] == samples and path["seed"] == SEED,
            f"Unexpected sample count or seed at {image}: {path}")
    require(path["rejectedSamples"] == path["unsupportedSamples"] == state["rejectedSamples"] ==
            state["unsupportedSamples"] == 0, f"Reference path diagnostics at {image}: {path}")
    require(metadata["source"] == "path-trace-mean" and metadata["colorSpace"] == "linear-sRGB" and
            width >= WIDTH and height >= HEIGHT and not metadata["includesUi"] and
            not metadata["exposureApplied"] and not metadata["toneMappingApplied"], f"Invalid raw capture at {image}")
    bgr = cv2.imdecode(np.frombuffer(image.read_bytes(), dtype=np.uint8), cv2.IMREAD_UNCHANGED)
    require(bgr is not None and bgr.shape == (height, width, 3) and bgr.dtype == np.float32 and
            np.isfinite(bgr).all(), f"Invalid float32 EXR at {image}")
    return bgr[:, :, ::-1].copy()


def check_geometry(image):
    import numpy as np

    height, width = image.shape[:2]
    y = height // 2
    # Win32 can clamp the requested width. Project the authored foreground
    # center using its camera distance and the actual vertical capture extent.
    offset = round(0.45 * height / (2 * 3.4 * math.tan(math.radians(FOV_DEGREES / 2))))
    for x, expected, name in ((width // 2 - offset, FAR_EMISSION, "alpha rejection reveals the far emitter"),
                              (width // 2 + offset, NEAR_EMISSION, "accepted alpha mask occludes the far emitter")):
        pixels = image[y - 4:y + 4, x - 4:x + 4]
        np.testing.assert_allclose(pixels, np.broadcast_to(expected, pixels.shape), rtol=0, atol=0.002, err_msg=name)
    np.testing.assert_allclose(image[:8, :8], 0, rtol=0, atol=0.002, err_msg="black background control")


def compare(reference, candidate):
    import numpy as np

    ious = {}
    for label, channel, other in (("farEmitter", 0, 1), ("nearEmitter", 1, 0)):
        a = reference[:, :, channel] > reference[:, :, other] + 0.25
        b = candidate[:, :, channel] > candidate[:, :, other] + 0.25
        intersection, union = int((a & b).sum()), int((a | b).sum())
        require(intersection > 100 and intersection / union >= 0.995,
                f"{label} capture masks differ: {intersection}/{union}")
        ious[label] = intersection / union
    error = np.max(np.abs(reference - candidate), axis=2)
    error99 = float(np.quantile(error, 0.99))
    require(error99 <= 0.004, f"99th-percentile absolute linear pixel error {error99} exceeds 0.004")
    return {"maskIoU": ious, "p99AbsoluteError": error99, "maxAbsoluteError": float(error.max())}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--renderer", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--layer-manifest", type=Path)
    args = parser.parse_args()
    if os.environ.get(OPT_IN) != "1":
        print(f"SKIP: set {OPT_IN}=1 on a Vulkan device supporting native tracing and inline ray queries")
        return 77
    os.environ.setdefault("OPENCV_IO_ENABLE_OPENEXR", "1")
    renderer = args.renderer.resolve(strict=True)
    args.output.mkdir(parents=True, exist_ok=True)
    output = Path(tempfile.mkdtemp(prefix="run-", dir=args.output)).resolve()
    print(f"Captures: {output}", flush=True)
    environment = dict(os.environ, VK_LAYER_SETTINGS_PATH=str(output), VK_LOADER_DEBUG="layer")
    if args.layer_manifest is not None:
        manifest = args.layer_manifest.resolve(strict=True)
        require(manifest.is_file(), f"Invalid validation layer manifest: {manifest}")
        environment["VK_LAYER_PATH"] = str(manifest.parent)
    (output / "vk_layer_settings.txt").write_text("khronos_validation.validate_sync = true\n", encoding="utf-8")
    model = output / "emitters.gltf"
    fixture(model)
    renderer_hash = hashlib.sha256(renderer.read_bytes()).hexdigest()
    report = {"passed": False, "rendererSha256": renderer_hash, "nativeShaderSha256": shader_hashes(renderer),
              "runs": [], "comparisons": {}}
    try:
        switched = capture(renderer, model, output, "live-switch",
                           [(4, INLINE, SAMPLES), (5, NATIVE, BATCH), (8, NATIVE, SAMPLES),
                            (9, INLINE, BATCH), (12, INLINE, SAMPLES)],
                           [{"frame": 5, "technique": NATIVE}, {"frame": 9, "technique": INLINE}], environment, report)
        native_only, = capture(renderer, model, output, "native-only", [(4, NATIVE, SAMPLES)], [],
                               environment, report, query_disabled=True)
        for image in [*switched, native_only]:
            check_geometry(image)
        cameras = [(record["extent"], record["camera"]["unjitteredViewProjColumns"])
                   for run in report["runs"] for record in run["captures"]]
        require(all(camera == cameras[0] for camera in cameras), "Camera changed between reference captures")
        report["comparisons"]["native"] = compare(switched[0], switched[2])
        report["comparisons"]["inlineReturn"] = compare(switched[0], switched[4])
        report["comparisons"]["nativeWithoutQueries"] = compare(switched[0], native_only)
        require(hashlib.sha256(renderer.read_bytes()).hexdigest() == renderer_hash, "Renderer changed during the run")
        require(shader_hashes(renderer) == report["nativeShaderSha256"], "Native shaders changed during the run")
        report["passed"] = True
    finally:
        (output / "results.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print("Verified native ray tracing, alpha acceptance, inline parity and native rendering with ray queries disabled.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
