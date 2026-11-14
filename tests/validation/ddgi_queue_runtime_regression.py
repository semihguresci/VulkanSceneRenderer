"""Exercise DDGI queue routes and live resource changes through the renderer CLI."""

import argparse
import json
import math
import os
from pathlib import Path
import statistics
import subprocess
import sys

os.environ.setdefault("OPENCV_IO_ENABLE_OPENEXR", "1")
import cv2
import numpy as np
from PIL import Image

from gltf_accessor_runtime_regression import Asset, require
from gltf_extended_materials_runtime_regression import validation_log_error
from msaa_attachment_capabilities_runtime_regression import VALIDATION, layer_files, verify_layers
from ray_footprint_normal_alpha_runtime_regression import freeze, sha


TECHNIQUES = ("deferred-raster", "forward-raster")
ROUTES = {"dedicated": "auto", "same-family": "same-family", "serialized": "serialized"}
START = (256, 192)
RESIZED = (304, 208)
PROBES = 5 * 5 * 8
RAYS = PROBES * 64
SAMPLES = {
    "steady-1": 17, "steady-2": 19, "steady-3": 21, "paused": 27, "disabled": 33,
    "re-enabled": 45, "material-texture": 63, "resized": 75, "resize-restored": 88,
    "replacement": 104, "scrolled": 120,
}


def write(path, value):
    path.write_text(json.dumps(value, indent=2, allow_nan=False) + "\n", encoding="utf-8")


def make_scene(path, emission):
    asset = Asset()
    texture = asset.image(np.full((4, 4, 4), (*emission, 255), dtype=np.uint8))

    def quad(left, right, low, high, z, material):
        front = material == 0
        indices = (0, 1, 2, 0, 2, 3) if front else (0, 2, 1, 0, 3, 2)
        return dict(attributes=dict(
            POSITION=asset.accessor("VEC3", [(left, low, z), (right, low, z),
                                            (right, high, z), (left, high, z)]),
            NORMAL=asset.accessor("VEC3", [(0, 0, 1 if front else -1)] * 4),
            TEXCOORD_0=asset.accessor("VEC2", [(0, 0), (1, 0), (1, 1), (0, 1)])),
            indices=asset.accessor("SCALAR", indices, 5123, indices=True), material=material)

    asset.model.update(
        materials=[
            dict(pbrMetallicRoughness=dict(baseColorFactor=[1, 1, 1, 1], metallicFactor=0, roughnessFactor=1)),
            dict(pbrMetallicRoughness=dict(baseColorFactor=[0, 0, 0, 1], metallicFactor=0, roughnessFactor=1),
                 emissiveFactor=[1, 1, 1], emissiveTexture=dict(index=texture),
                 extensions={"KHR_materials_emissive_strength": dict(emissiveStrength=8)})],
        meshes=[dict(primitives=[quad(-1.5, 1.5, -1.5, 1.5, 0, 0),
                                 quad(.48, 2.2, -1.2, 1.2, .75, 1)])],
        nodes=[dict(mesh=0)], scenes=[dict(nodes=[0])],
        extensionsUsed=["KHR_materials_emissive_strength"],
    )
    asset.write(path)


def make_inputs(output):
    original = output / "original.gltf"
    replacement = output / "replacement.gltf"
    texture = output / "green.png"
    sequence = output / "sequence.json"
    make_scene(original, (255, 0, 0))
    make_scene(replacement, (0, 0, 255))
    Image.new("RGBA", (4, 4), (0, 255, 0, 255)).save(texture)
    write(sequence, dict(schemaVersion=1, frames=max(SAMPLES.values()), sampleFrames=list(SAMPLES.values()),
        camera=[dict(frame=1, position=[0, 0, 1.5], target=[0, 0, 0]),
                dict(frame=105, position=[0, 0, 1.5], target=[0, 0, 0]),
                dict(frame=106, position=[-.5, 0, 1.5], target=[-.5, 0, 0])],
        events=[dict(frame=23, ddgiPaused=True), dict(frame=30, ddgiEnabled=False),
                dict(frame=35, ddgiEnabled=True, ddgiPaused=False),
                dict(frame=47, materialTextureEdit=dict(sourceMaterial=1, slot="emissive",
                                                         operation="importFile", path=str(texture))),
                dict(frame=65, resize=RESIZED), dict(frame=77, resize=START),
                dict(frame=90, reload=str(replacement))]))
    return original, replacement, texture, sequence


def read_sample(image, expected_size, technique, revision, source="raster-scene-composite"):
    metadata_path = Path(str(image) + ".capture.json")
    telemetry_path = image.with_suffix(".telemetry.json")
    metadata = json.loads(metadata_path.read_bytes())
    state = json.loads(telemetry_path.read_bytes())
    width, height = expected_size
    require(state["build"]["revision"] == revision and state["validationEnabled"],
            f"Wrong build or validation state in {image.name}")
    require(state["technique"] == technique and state["resolution"] == [width, height] and
            state["msaaSamples"] == 1 and not state["taa"]["enabled"], f"Wrong raster state in {image.name}")
    require(metadata["source"] == source and metadata["colorSpace"] == "linear-sRGB" and
            not any(metadata[key] for key in ("includesUi", "exposureApplied", "toneMappingApplied")),
            f"Capture did not use raw viewport radiance: {image.name}")
    rgb = cv2.imdecode(np.frombuffer(image.read_bytes(), np.uint8), cv2.IMREAD_UNCHANGED)
    require(rgb is not None and rgb.shape == (height, width, 3) and rgb.dtype == np.float32 and
            np.isfinite(rgb).all(), f"Invalid EXR pixels in {image.name}")
    region = rgb[height//3:2*height//3, width//3:2*width//3, ::-1]
    return state, region.mean(axis=(0, 1), dtype=np.float64).tolist(), {
        path.name: sha(path) for path in (image, telemetry_path, metadata_path)}


def verify_sample(name, state, color, route, model):
    gi = state["ddgi"]
    graphics = gi["graphicsQueueFamily"]
    compute = gi.get("asyncQueueFamily")
    submitted, published = gi["asyncSubmittedUpdates"], gi["asyncPublishedUpdates"]
    require(gi["supported"] and gi["settings"]["asyncCompute"] and gi["queueRoute"] == route and
            graphics >= 0, f"{name}: DDGI did not select the requested {route} route: {gi['status']}")
    require(gi["asyncActive"] == (route != "serialized"), f"{name}: async flag differs from route")
    if route == "serialized":
        require(compute is None and submitted == published == 0 and "asyncGpuMs" not in gi,
                f"{name}: serialized fallback submitted asynchronous work")
    else:
        require(compute is not None and (graphics == compute) == (route == "same-family") and
                0 < published <= submitted <= published + 1, f"{name}: invalid queue or publication state")

    require(Path(state["scene"]["primary"]).resolve() == model, f"{name}: wrong primary model")
    require(gi["settings"]["cameraFollow"] and gi["settings"]["counts"] == [5, 5, 8] and
            gi["settings"]["raysPerProbe"] == 64 and gi["totalAllocatedBytes"] > 0,
            f"{name}: probe workload or allocation differs")
    require(gi["enabled"] == (name != "disabled") and gi["active"] == (name != "disabled") and
            gi["paused"] == (name in ("paused", "disabled")), f"{name}: wrong live enable or pause state")
    require(0 <= gi["updatedProbes"] <= PROBES and gi["tracedRays"] == 64 * gi["updatedProbes"],
            f"{name}: DDGI crossed the matched probe or ray budget")
    if name in ("paused", "disabled"):
        require(gi["updatedProbes"] == gi["tracedRays"] == 0, f"{name}: probe work did not pause")
    if name == "disabled":
        # The legacy raster ambient floor is neutral and differs by technique.
        require(max(color) < .08 and max(color) - min(color) < .001,
                f"{name}: no-GI control contains excess or colored light: {color}")
    else:
        expected = 2 if name in ("replacement", "scrolled") else 1 if name in (
            "material-texture", "resized", "resize-restored") else 0
        require(color[expected] > max(.005, 3 * max(c for index, c in enumerate(color) if index != expected)),
                f"{name}: indirect receiver has the wrong emission color: {color}")
        require(state["rayShadows"]["build"]["instances"] == 2, f"{name}: wrong ray scene instance count")

    if name in ("material-texture", "resized", "resize-restored"):
        edits = state["gltfMaterialTextures"]
        require(not edits["lastError"] and any(row["sourceMaterial"] == 1 and row["slot"] == "emissive" and
                    row["authoredTexture"] >= 1 and row["runtimeTexture"] is not None for row in edits["slots"]),
                f"{name}: new texture binding was not published")
    if name == "scrolled":
        require(gi["scroll"]["count"] > 0 and 0 < gi["scroll"]["preservedProbes"] < PROBES and
                np.allclose(gi["scroll"]["origin"], [-1.5, -1, -.25], rtol=0, atol=1e-6),
                f"{name}: scroll did not move and partially preserve the grid")


def timing(state, route):
    if route != "serialized":
        elapsed = state["ddgi"].get("asyncGpuMs")
        if elapsed is not None:
            require(math.isfinite(elapsed) and elapsed > 0, "Invalid async compute timestamp")
        return elapsed
    passes = {entry["name"]: entry for entry in state["passes"]}
    scoped = [passes[name] for name in ("DdgiTrace", "DdgiUpdate")]
    require(all(entry["active"] and entry["gpuTimed"] and math.isfinite(entry["gpuMs"]) and entry["gpuMs"] > 0
                for entry in scoped), "Missing serialized DDGI graphics pass timings")
    return sum(entry["gpuMs"] for entry in scoped)


def launch(args, output, case, route, command, layer_library):
    log = case / "renderer.log"
    env = dict(os.environ, CONTAINER_DDGI_ASYNC_QUEUE=ROUTES[route], VK_LOADER_DEBUG="layer",
               VK_LAYER_PATH=str(args.validation_dir), VK_LAYER_SETTINGS_PATH=str(output), VK_INSTANCE_LAYERS=VALIDATION)
    env.pop("VK_LOADER_LAYERS_ENABLE", None)
    with log.open("w", encoding="utf-8") as stream:
        process = subprocess.run(command, cwd=args.renderer.parent, env=env, stdout=stream, stderr=subprocess.STDOUT,
                                 timeout=args.timeout, creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
    raw = log.read_text(encoding="utf-8", errors="replace")
    verify_layers(raw, layer_library, None)
    require(process.returncode == 0 and not validation_log_error(raw), f"Renderer exit or Vulkan validation failure: {log}")
    return log


def capture_command(renderer, technique, model, sequence, image):
    return [str(renderer), "--hidden", "--no-ui", "--validation", "--no-bloom", "--no-taa",
        "--msaa", "1", "--width", str(START[0]), "--height", str(START[1]), "--fixed-dt", ".016666667",
        "--render-technique", technique, "--model", str(model), "--import-scale", "1", "--display-mode", "lit",
        "--directional-intensity", "0", "--environment-intensity", "0", "--sky", "plain", "--sky-horizon", "0", "0", "0",
        "--camera-position", "0", "0", "1.5", "--camera-target", "0", "0", "0", "--camera-fov", "35",
        "--ray-shadows", "raster", "--ddgi", "--ddgi-async",
        "--ddgi-origin", "-1", "-1", "-.25", "--ddgi-counts", "5", "5", "8", "--ddgi-spacing", ".5", ".5", ".5",
        "--ddgi-rays", "64", "--ddgi-probes-per-frame", str(PROBES), "--ddgi-ray-budget", str(RAYS), "--ddgi-hysteresis", "0",
        "--screenshot-viewport", "--capture-sequence", str(sequence), "--screenshot", str(image)]


def run_case(args, output, route, technique, models, sequence, layer_library):
    case = output / f"{route}-{technique}"
    case.mkdir()
    image = case / "capture.exr"
    command = capture_command(args.renderer, technique, models[0], sequence, image) + ["--ddgi-camera-follow"]
    log = launch(args, output, case, route, command, layer_library)
    states, samples, measured = {}, {}, []
    for name, frame in SAMPLES.items():
        capture = image if frame == max(SAMPLES.values()) else case / f"capture.frame-{frame:04d}.exr"
        state, color, artifacts = read_sample(capture, RESIZED if name == "resized" else START, technique, args.revision)
        verify_sample(name, state, color, route, models[int(name in ("replacement", "scrolled"))])
        states[name] = state
        gi = state["ddgi"]
        samples[name] = dict(frame=frame, receiverMeanRgb=color, artifactSha256=artifacts,
            swapchainRecreates=state["swapchainRecreates"], deviceIdleWaits=state["deviceIdleWaits"],
            frameWaitCpuMs=state["cpuPhasesMs"]["Frame wait"], raySceneBytes=state["rayShadows"]["totalAllocatedBytes"],
            ddgi={key: gi[key] for key in ("queueRoute", "graphicsQueueFamily", "asyncQueueFamily", "asyncGpuMs",
                "asyncSubmittedUpdates", "asyncPublishedUpdates", "updateFrame", "updatedProbes", "tracedRays", "scroll",
                "totalAllocatedBytes") if key in gi})
        if name.startswith("steady-"):
            measured.append(timing(state, route))

    gi = {name: state["ddgi"] for name, state in states.items()}
    require(gi["paused"]["updateFrame"] >= gi["steady-3"]["updateFrame"] and
            gi["paused"]["updateFrame"] == gi["disabled"]["updateFrame"] < gi["re-enabled"]["updateFrame"],
            "DDGI did not stop updating while paused/disabled and resume when re-enabled")
    require(states["resize-restored"]["swapchainRecreates"] > states["resized"]["swapchainRecreates"] >
            states["material-texture"]["swapchainRecreates"], "Two requested swapchain rebuilds were not observed")
    gpu_times = [value for value in measured if value is not None]
    require(not args.require_async_timestamps or len(gpu_times) == len(measured),
            f"{route} {technique}: missing timestamps on one or more steady captures")
    return dict(route=route, technique=technique, hardware=states["steady-1"]["gpu"],
        driverVersion=states["steady-1"]["driverVersion"], logSha256=sha(log), validationClean=True,
        timingScope="sum of graphics trace/update pass timestamps" if route == "serialized" else
                    "last completed async command, including history copies and barriers",
        steadyGpuMs=measured, steadyMedianGpuMs=statistics.median(gpu_times) if gpu_times else None, samples=samples)


def run_reference(args, output, models, layer_library):
    case = output / "dedicated-path-tracing"
    case.mkdir()
    sequence = case / "sequence.json"
    image = case / "capture.exr"
    write(sequence, dict(schemaVersion=1, frames=12, sampleFrames=[3, 7, 12],
        events=[dict(frame=5, reload=str(models[1])), dict(frame=9, reloadShaders=True)]))
    sequence_sha = sha(sequence)
    command = capture_command(args.renderer, "path-tracing", models[0], sequence, image) + [
        "--path-spp", "4", "--path-samples", "64", "--path-bounces", "1"]
    log = launch(args, output, case, "dedicated", command, layer_library)
    samples = []
    for frame, model in zip((3, 7, 12), (models[0], models[1], models[1]), strict=True):
        capture = image if frame == 12 else case / f"capture.frame-{frame:04d}.exr"
        state, _, hashes = read_sample(capture, START, "path-tracing", args.revision, source="path-trace-mean")
        path = state["referencePath"]
        gi = state["ddgi"]
        require(Path(state["scene"]["primary"]).resolve() == model and path["active"] and
                path["attemptedSamples"] > 0, "Reference tracer/model did not execute")
        require(gi["supported"] and gi["enabled"] and gi["settings"]["asyncCompute"] and not gi["active"] and
                all(gi[key] == 0 for key in ("updatedProbes", "tracedRays", "asyncSubmittedUpdates", "asyncPublishedUpdates")),
                "Reference path tracing submitted DDGI probe work")
        samples.append(dict(frame=frame, referenceSamples=path["attemptedSamples"], asyncSubmittedUpdates=0,
                            shaderReload=state["shaderReload"], artifactSha256=hashes))
    require(sha(sequence) == sequence_sha, "Reference capture sequence changed during execution")
    reload = samples[-1]["shaderReload"]
    require(reload["generation"] > 0 or "source build" in reload["status"],
            "Reference shader reload request did not publish or identify absent source tooling")
    return dict(validationClean=True, samples=samples, sequenceSha256=sequence_sha, logSha256=sha(log))


def compare_routes(runs):
    rows = []
    for technique in TECHNIQUES:
        variants = {row["route"]: row for row in runs if row["technique"] == technique}
        if "serialized" not in variants:
            continue
        control = variants.pop("serialized")
        for route, row in variants.items():
            rows.append(dict(technique=technique, route=route,
                serializedGpuMs=control["steadyMedianGpuMs"], asyncGpuMs=row["steadyMedianGpuMs"],
                serializedDdgiBytes=control["samples"]["steady-3"]["ddgi"]["totalAllocatedBytes"],
                asyncDdgiBytes=row["samples"]["steady-3"]["ddgi"]["totalAllocatedBytes"]))
    return rows


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--renderer", type=Path)
    parser.add_argument("--revision")
    parser.add_argument("--manifest", type=Path)
    parser.add_argument("--validation-dir", type=Path)
    parser.add_argument("--route", action="append", choices=ROUTES)
    parser.add_argument("--technique", action="append", choices=TECHNIQUES)
    parser.add_argument("--timeout", type=float, default=240)
    parser.add_argument("--require-async-timestamps", action="store_true")
    parser.add_argument("--reference-control", action="store_true", help="Also check no DDGI writer in path tracing")
    parser.add_argument("--reference-only", action="store_true")
    parser.add_argument("--generate-only", action="store_true")
    args = parser.parse_args()
    if not args.generate_only and not all((args.renderer, args.revision, args.manifest, args.validation_dir)):
        parser.error("native mode requires --renderer, --revision, --manifest and --validation-dir")
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    original, replacement, texture, sequence = make_inputs(output)
    if args.generate_only:
        return

    args.renderer, args.manifest, args.validation_dir = (path.resolve(strict=True) for path in
        (args.renderer, args.manifest, args.validation_dir))
    layer, identity = layer_files(args.validation_dir, VALIDATION)
    require(identity["apiVersion"] == "1.4.363", "Expected Vulkan Validation 1.4.363")
    layer_json = args.validation_dir / "VkLayer_khronos_validation.json"
    runtime = args.renderer.parent
    shaders = sorted((runtime / "spv_shaders").glob("*.spv"))
    require({"ddgi_trace.comp.spv", "ddgi_update.comp.spv"} <= {path.name for path in shaders},
            "Renderer runtime is missing DDGI shaders")
    required = [args.renderer, layer, layer_json, *runtime.glob("*.dll"), *shaders]
    assets = [path for directory in ("hdr", "materials") for path in (runtime / directory).rglob("*")
              if path.is_file()]
    source = [Path(__file__), *(Path(sys.modules[helper.__module__].__file__) for helper in
                               (Asset, validation_log_error, verify_layers, freeze))]
    settings = output / "vk_layer_settings.txt"
    settings.write_text("khronos_validation.validate_sync = true\n", encoding="utf-8")
    inputs = {path: sha(path) for path in (original, replacement, texture, sequence, settings, *source, *assets)}
    before = freeze(args.manifest, required)
    report = dict(passed=False, rendererSha256=sha(args.renderer), revision=args.revision,
                  validationLayer=identity, synchronizationValidation=True, freezeBefore=before,
                  inputSha256={str(path.resolve()): digest for path, digest in inputs.items()}, runs=[])
    try:
        if not args.reference_only:
            for technique in dict.fromkeys(args.technique or TECHNIQUES):
                for route in dict.fromkeys(args.route or ROUTES):
                    run = run_case(args, output, route, technique, (original, replacement), sequence, layer)
                    report["runs"].append(run)
                    print(f"PASS {route} {technique}: {len(run['samples'])} captures", flush=True)
            require(len({(row["hardware"], row["driverVersion"]) for row in report["runs"]}) == 1,
                    "Queue comparison used different devices or drivers")
        if args.reference_control or args.reference_only:
            report["referenceControl"] = run_reference(args, output, (original, replacement), layer)
            print("PASS dedicated path tracing: zero DDGI writers", flush=True)
        report["freezeAfter"] = freeze(args.manifest, required, identity=before["sha256"])
        require(all(sha(path) == digest for path, digest in inputs.items()), "Inputs changed during execution")
        report["comparisons"] = compare_routes(report["runs"])
        report["passed"] = True
    except Exception as error:
        report["failure"] = str(error)
        raise
    finally:
        reference_samples = report.get("referenceControl", {}).get("samples", [])
        report.update(processes=len(report["runs"]) + bool(reference_samples),
                      captures=sum(len(row["samples"]) for row in report["runs"]) + len(reference_samples))
        write(output / "results.json", report)


if __name__ == "__main__":
    main()
