"""Benchmark derived-cache startup, residency, image quality and AS builds on large scenes."""

import argparse
import json
import math
import os
from pathlib import Path
import platform
import re
import shutil
import struct
import subprocess
import tempfile
import time
from urllib.parse import unquote, urlsplit

from display_capture_test_support import validation_log_error
from graphics_concurrency_benchmark import file_identity, measurement, require, series, stats, validation_environment


CAMERAS = {
    "tekla": (((90, 53, 50), (31, 13, -19)), ((65, 27, 22), (39, 19, -8))),
    "chess": (((0.65, 0.46, 0.8), (0, -0.045, 0)), ((0.58, 0.32, 0.54), (0, 0.045, 0))),
}
CACHE_ROWS = {"tekla": "bim", "chess": "geometryLod"}
ROUTES = {"tekla": "auxiliary", "chess": "primary"}
PROVENANCE = {
    "tekla": "https://github.com/buildingSMART/IFC5-development/tree/1a63082ada967c683cfacee2005f8f749c8e1b79/examples/Tekla%20House",
    "chess": "https://github.com/KhronosGroup/glTF-Sample-Assets/tree/8f492390ddc7a2ae221a1369ef560589cf8c70fb/Models/ABeautifulGame",
}
VALIDATION_LAYER = 'Insert instance layer "VK_LAYER_KHRONOS_validation"'
VMA_FIELDS = ("vmaBytes", "vmaDeviceLocalBytes", "vmaAllocations")
INTERACTIVE_GEOMETRY_KIB = 1024


def write_json(path, value):
    path.write_text(json.dumps(value, indent=2, allow_nan=False) + "\n", encoding="utf-8")


def local_gltf_resource(directory, uri):
    """Resolve a local glTF resource without letting a fixture traverse outside its directory."""
    parsed = urlsplit(uri)
    if parsed.scheme == "data":
        return None
    require(not any((parsed.scheme, parsed.netloc, parsed.query, parsed.fragment)), f"Expected a local glTF URI: {uri}")
    relative = Path(unquote(parsed.path))
    require(relative.parts and not relative.is_absolute() and not relative.drive and ".." not in relative.parts,
            f"Unsafe glTF resource path: {uri}")
    require((directory / relative).resolve().is_relative_to(directory), f"glTF resource escapes fixture directory: {uri}")
    return relative


def copy_gltf(source, directory):
    """Keep the licensed source unchanged and make one editable, self-contained benchmark copy."""
    model = json.loads(source.read_bytes())
    resources = {source.name: source}
    for entry in model.get("buffers", []) + model.get("images", []):
        if "uri" in entry and (relative := local_gltf_resource(source.parent, entry["uri"])):
            resources[relative.as_posix()] = source.parent / relative
    for filename, path in resources.items():
        destination = directory / filename
        destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(path, destination)
    return directory / source.name, model, [file_identity(resources[name]) for name in sorted(resources)]


def position_edit(model_path, model):
    """Locate a small, representable geometry edit wholly inside the authored bounds."""
    for mesh in model["meshes"]:
        for primitive in mesh["primitives"]:
            accessor = model["accessors"][primitive["attributes"]["POSITION"]]
            if accessor["componentType"] != 5126 or accessor["type"] != "VEC3" or "sparse" in accessor:
                continue
            view = model["bufferViews"][accessor["bufferView"]]
            uri = model["buffers"][view["buffer"]].get("uri")
            if not uri or not (relative := local_gltf_resource(model_path.parent, uri)):
                continue
            lower, upper = accessor["min"][0], accessor["max"][0]
            margin = (upper - lower) * 0.05
            stride = view.get("byteStride", 12)
            start = view.get("byteOffset", 0) + accessor.get("byteOffset", 0)
            with (model_path.parent / relative).open("rb") as stream:
                for vertex in range(accessor["count"]):
                    offset = start + vertex * stride
                    stream.seek(offset)
                    before, = struct.unpack("<f", stream.read(4))
                    if lower + margin < before < upper - margin:
                        after, = struct.unpack("<f", struct.pack("<f", before + margin * 0.002))
                        if before != after:
                            return dict(resource=relative.as_posix(), offset=offset, before=before, after=after)
    raise RuntimeError(f"No editable, interior FLOAT POSITION in an external glTF buffer: {model_path}")


def edit_gltf_position(model_path, edit):
    resource = model_path.parent / edit["resource"]
    before = file_identity(resource)["sha256"]
    with resource.open("r+b") as stream:
        stream.seek(edit["offset"])
        require(stream.read(4) == struct.pack("<f", edit["before"]), "The copied POSITION changed before invalidation")
        stream.seek(edit["offset"])
        stream.write(struct.pack("<f", edit["after"]))
    after = file_identity(resource)["sha256"]
    require(before != after, "The external geometry payload did not change")
    return {**edit, "sha256Before": before, "sha256After": after}


def capture_plan(model_name, model_path, warmup, samples, interactive_reload):
    overview = warmup + samples + 2
    reload_start = overview + 1
    reload_frame = reload_start + int(interactive_reload)
    detail = reload_frame + warmup + samples + 2

    def camera(frame, view):
        position, target = CAMERAS[model_name][view]
        return dict(frame=frame, position=position, target=target)

    cameras = [camera(1, 0), camera(overview, 0)]
    if interactive_reload:
        events = [dict(frame=reload_start, guiReload=str(model_path), guiWaitForLoad=False),
                  dict(frame=reload_frame, guiWaitForUploadChunks=1)]
        telemetry_frames = [1, reload_start, reload_frame]
        cameras.append(camera(reload_start, 0))
        cameras.extend((camera(reload_frame, 0), camera(reload_frame + 1, 1)))
    else:
        events = [dict(frame=reload_frame, reload=str(model_path))]
        telemetry_frames = [1, reload_frame]
        cameras.append(camera(reload_frame, 1))
    return dict(schemaVersion=1, frames=detail, sampleFrames=[overview, detail], telemetryFrames=telemetry_frames,
                camera=[*cameras, camera(detail, 1)], events=events)


def capture_paths(image, frame, plan):
    if frame != plan["frames"]:
        image = image.with_name(f"{image.stem}.frame-{frame:04d}{image.suffix}")
    return image, image.with_suffix(".telemetry.json")


def image_metrics(reference, candidate):
    from PIL import Image, ImageChops

    with Image.open(reference) as ref, Image.open(candidate) as result:
        require(ref.size == result.size, f"Capture dimensions differ: {candidate}")
        lhs, rhs = ref.convert("RGB"), result.convert("RGB")
        difference = ImageChops.difference(lhs, rhs)
        channels = difference.split()
        maximum = ImageChops.lighter(ImageChops.lighter(*channels[:2]), channels[2]).histogram()
        histogram = difference.histogram()
        elements = 3 * lhs.width * lhs.height
        mean = sum((index % 256) * count for index, count in enumerate(histogram)) / elements
        squared = sum((index % 256) ** 2 * count for index, count in enumerate(histogram)) / elements
        return dict(meanAbsoluteError=mean, rmse=math.sqrt(squared),
                    psnrDb=10 * math.log10(255 ** 2 / squared) if squared else None,
                    maxChannelError=max(value for value, count in enumerate(maximum) if count),
                    pixelsChanged=sum(maximum[1:]), pixelsAboveOne=sum(maximum[2:]), pixelCount=elements // 3)


def verify_visible_image(path, extent):
    from PIL import Image

    with Image.open(path) as image:
        require(image.size == extent, f"Wrong screenshot dimensions: {path}")
        histogram = image.convert("L").histogram()
        require(sum(count > 0 for count in histogram) >= 32, f"Capture is effectively blank: {path}")
    return file_identity(path)


def frame_timings(state, capture_frame, count):
    # Capture sequence frames count from one while renderer submissions count from zero.
    # Leave one intervening ordinary submission for timestamp retirement.
    start = capture_frame - count - 2
    expected = list(range(start, start + count))
    rows = [row for row in state["frameHistory"] if start <= row["frameIndex"] < start + count]
    require([row["frameIndex"] for row in rows] == expected and
            all(row["cpuPhasesMs"]["Screenshot"] == 0 for row in rows), "Missing ordinary steady-state frames")
    retired = [row for row in rows if row["gpuFrameIndex"] == row["frameIndex"]]
    require(not state["gpuProfiler"]["available"] or len(retired) >= count - 2, "Insufficient matched GPU samples")
    return dict(frameIndices=expected, matchedGpuFrameCount=len(retired),
                cpuMs={phase: stats(row["cpuPhasesMs"][phase] for row in rows) for phase in rows[0]["cpuPhasesMs"]},
                gpuGraphicsMs=series(retired, "gpuGraphicsMs"), gpuPassSumMs=series(retired, "gpuPassSumMs"))


def benchmark_sample(state, scene, capture_frame, args):
    provider = ROUTES[scene]
    lod = state["geometryLod"]["primary" if scene == "chess" else "bim"]
    texture = state["textureStreaming"]
    images = {row["imageId"]: row for row in texture["rows"]}
    mips = {}
    for row in images.values():
        mip = row["residentFirstMip"]
        mips[mip] = mips.get(mip, 0) + 1
    memory = state["assetResidency"]
    gpu_passes = {row["name"]: row["gpuMs"] for row in state["passes"] if row["active"] and row["gpuTimed"]}
    return dict(timing=frame_timings(state, capture_frame, args.samples), camera=state["camera"],
                objectCount=state["scene"][f"{provider}ObjectCount"], geometryLod=lod,
                latestRetiredGpuPassMs=gpu_passes,
                assetMemory={key: memory[key] for key in ("used", "peak", "limits", "rejectedReservations")},
                vma={key: state["shaderReload"][key] for key in VMA_FIELDS},
                geometryStreaming=state["geometryStreaming"], rayShadows=state.get("rayShadows"),
                textureStreaming={**{key: value for key, value in texture.items() if key != "rows"},
                                  "residentMipHistogram": mips,
                                  "unsettledTrackedImages": sum(row["residentFirstMip"] != row["plannedFirstMip"]
                                                                for row in images.values()),
                                  "trackedImageAllocationBytes": sum(row["residentBytes"] for row in images.values())})


def acceleration_builds(overview, detail, reload_frame, no_ray_query, require_ray):
    if no_ray_query:
        require(all(not state.get("rayShadows", {}).get("active") and not state["raySceneBuildCosts"]
                    for state in (overview, detail)), "Forced raster fallback unexpectedly submitted AS work")
        return dict(status="disabled", startup=None, reload=None)
    if not overview.get("rayShadows", {}).get("supported"):
        require(not require_ray, "The requested ray-query benchmark is unavailable")
        return dict(status="unsupported", startup=None, reload=None)
    require(overview["rayShadows"]["active"] and detail["rayShadows"]["active"], "Ray queries did not reach a large scene")
    startup_generation = overview["rayShadows"]["generation"]
    reload_generation = detail["rayShadows"]["generation"]
    require(reload_generation > startup_generation > 0, "Reload did not publish another AS generation")

    def retired_build(state, generation, first, end):
        matching = [row for row in state["raySceneBuildCosts"] if row["generation"] == generation]
        require(len(matching) == 1, f"Expected one retired sample for AS generation {generation}")
        build = matching[0]
        require(first <= build["frame"] < end and build["instances"] > 0 and build["blasBuilt"] + build["blasReused"] > 0
                and build["asAllocatedBytes"] > 0 and build["scratchAllocatedBytes"] > 0,
                f"AS observation is from the wrong submission or did no geometry work: {build}")
        require(build["gpuMs"] is None or measurement(build["gpuMs"]), "Invalid AS timestamp duration")
        if require_ray:
            require(build["gpuMs"] is not None and build["gpuMs"] > 0, "Ray build timestamp is unavailable")
        return build

    startup = retired_build(overview, startup_generation, 0, reload_frame - 1)
    require(startup["blasBuilt"] > 0, "Startup AS observation did not build any BLAS")
    return dict(status="active", startup=startup,
                reload=retired_build(detail, reload_generation, reload_frame - 1, detail["frameHistory"][-1]["frameIndex"] + 1))


def make_command(args, model, case, sequence, image, cache):
    resident_mib, upload_mib = asset_caps(args, case)
    command = [str(args.renderer), "--model", str(model), "--import-scale", "1", "--hidden",
               "--render-technique", args.technique, "--width", str(args.width), "--height", str(args.height),
               "--fixed-dt", "0.016666667", "--msaa", "1", "--camera-fov", "45", "--display-mode", "lit",
               "--no-taa", "--no-bloom", "--no-ddgi", "--exposure", "1", "--directional-intensity", "3",
               "--directional-direction", "-0.55", "-0.75", "-0.35", "--environment-intensity", "0.8",
               "--validation" if args.validation_manifest else "--no-validation",
               "--screenshot-viewport", "--capture-sequence", str(sequence), "--screenshot", str(image),
               "--asset-memory-mb", str(resident_mib), "--asset-upload-mb", str(upload_mib)]
    if args.interactive_reload:
        command += ["--geometry-upload-kb", str(INTERACTIVE_GEOMETRY_KIB)]
    else:
        command.append("--no-ui")
    if args.no_ray_query:
        command += ["--no-ray-query", "--ray-shadows", "raster"]
    else:
        command += ["--ray-shadows", "hard", "--no-ray-shadow-denoise"]
    if case == "reference":
        command += ["--geometry-lod-error", "0"]
    else:
        command += ["--geometry-lod-error", "2", "--texture-streaming",
                    "--texture-upload-kb", str(args.texture_chunk_kib), "--asset-cache-dir", str(cache)]
    return command


def asset_caps(args, case):
    if case == "reference" and args.unbounded_reference:
        return 0, 0
    return args.asset_memory_mib, args.asset_upload_mib


def verify_asset_caps(state, expected_mib, source):
    budget = state["assetResidency"]
    for key, mib in zip(("residentBytes", "uploadBytes"), expected_mib, strict=True):
        cap = mib * 1024 * 1024
        used, peak = budget["used"][key], budget["peak"][key]
        require(budget["limits"][key] == cap and 0 <= used <= peak and (cap == 0 or peak <= cap),
                f"Asset budget was not applied or was exceeded for {key} in {source}: {budget}")


def geometry_asset(state, route):
    provider = "primary-mesh-scene" if route == "primary" else "auxiliary-bim-scene"
    assets = [entry for entry in state["assetResidency"]["providers"] if entry["id"] == provider]
    require(len(assets) == 1 and assets[0]["residency"] == "resident" and assets[0]["geometryAssetId"] > 0,
            f"Expected a published {provider} geometry asset, got {assets}")
    return assets[0]["geometryAssetId"]


def run_case(args, scene, model, case, output, environment, validation):
    warmup = args.warmup if args.warmup is not None else (480 if scene == "chess" else 120)
    plan = capture_plan(scene, model, warmup, args.samples, args.interactive_reload)
    sequence = output / "sequence.json"
    write_json(sequence, plan)
    stem = output / case
    image, log = stem.with_suffix(".png"), stem.with_suffix(".log")
    command = make_command(args, model, case, sequence, image, output / "derived-cache")
    result = dict(command=command, sequence=str(sequence), log=str(log), warmupFramesPerView=warmup)
    if args.plan_only:
        return result

    with log.open("w", encoding="utf-8") as stream:
        started = time.perf_counter()
        process = subprocess.run(command, cwd=args.renderer.parent, env=environment, stdout=stream,
                                 stderr=subprocess.STDOUT, timeout=args.timeout,
                                 creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
        result["processWallSeconds"] = time.perf_counter() - started
    messages = log.read_text(encoding="utf-8", errors="replace")
    require(process.returncode == 0 and not validation_log_error(messages), f"Native benchmark failed. See {log}")
    if validation:
        inserted = re.search(re.escape(VALIDATION_LAYER) + r" \(([^\r\n]+)\)", messages)
        require(inserted and Path(inserted[1]).resolve() == Path(validation["library"]["path"]),
                f"Requested sync validation library was absent. See {log}")

    first_frame, *preparation_frames, reload_frame = plan["telemetryFrames"]
    early = {frame: json.loads(capture_paths(image, frame, plan)[1].read_bytes())
             for frame in plan["telemetryFrames"]}
    route = ROUTES[scene]
    for frame, state in early.items():
        verify_asset_caps(state, asset_caps(args, case), f"{scene} {case} frame {frame}")
    old_asset = geometry_asset(early[first_frame], route)
    first_reload_asset = geometry_asset(early[reload_frame], route)
    states = {}
    result["views"] = {}
    for view, frame in zip(("overview", "detail"), plan["sampleFrames"], strict=True):
        screenshot, sidecar = capture_paths(image, frame, plan)
        state = states[view] = json.loads(sidecar.read_bytes())
        verify_asset_caps(state, asset_caps(args, case), sidecar)
        require(state["technique"] == args.technique and state["resolution"] == [args.width, args.height]
                and state["validationEnabled"] == bool(validation) and not state["taa"]["enabled"]
                and state["swapchainRecreates"] == 0 and state["msaaSamples"] == 1
                and Path(state["scene"][route]).resolve() == model
                and state["scene"][f"{route}ObjectCount"] >= (49 if scene == "chess" else 8000),
                f"Wrong renderer configuration or large-scene input in {sidecar}")
        sample = benchmark_sample(state, scene, frame, args)
        lod = sample["geometryLod"]
        require(0 < lod["triangles"] <= lod["baseTriangles"] and
                lod["baseTriangles"] >= (1_000_000 if scene == "chess" else 100_000),
                f"Expected substantial source geometry in {sidecar}")
        require(state["assetCache"]["enabled"] == (case != "reference")
                and sample["textureStreaming"]["enabled"] == (case != "reference"),
                f"The renderer did not apply cache/streaming configuration in {sidecar}")
        if case == "reference":
            require(lod["triangles"] == lod["baseTriangles"], f"Reference used reduced geometry in {sidecar}")
        elif scene == "chess":
            texture = sample["textureStreaming"]
            require(texture["residentMipHistogram"] and texture["promotions"] > 0
                    and 0 < texture["maxBytesPerTick"] <= args.texture_chunk_kib * 1024,
                    f"No bounded mip promotion on the native glTF input in {sidecar}")
            require(not texture["pending"] and texture["unsettledTrackedImages"] == 0 and texture["uploadFailures"] == 0,
                    f"Texture streaming has not reached the residency plan after {warmup} warm-up frames in {sidecar}")
            require(sample["geometryStreaming"]["primaryLod"]["state"] == "ready" and lod["extraIndexBytes"] > 0,
                    f"Raster geometry LOD has not published after {warmup} warm-up frames in {sidecar}")
        result["views"][view] = dict(frame=frame, telemetry=str(sidecar),
                                      image=verify_visible_image(screenshot, (args.width, args.height)), **sample)

    result["startup"] = early[first_frame]["assetStartup"]
    require(all(measurement(result["startup"].get(field)) and result["startup"][field] > 0
                for field in ("initialSceneMs", "firstSubmittedMs"))
            and result["startup"]["firstSubmittedMs"] >= result["startup"]["initialSceneMs"],
            f"Invalid startup instrumentation in {log}")
    overview = states["overview"]
    detail = states["detail"]
    detail_asset = geometry_asset(detail, route)
    require(geometry_asset(overview, route) == old_asset and detail_asset != old_asset,
            f"Reload did not publish a new geometry asset in {scene} {case}")
    if args.interactive_reload:
        preparing = early[preparation_frames[0]]
        pending = early[reload_frame]
        first_copy = pending["geometryStreaming"]["reload"]
        completed_copy = detail["geometryStreaming"]["reload"]
        old_loading = overview["gui"]["modelLoading"]
        pending_loading = pending["gui"]["modelLoading"]
        require(geometry_asset(preparing, route) == old_asset and preparing["gui"]["modelLoading"]["pending"] and
                preparing["geometryStreaming"]["reload"]["submittedBytes"] == 0,
                f"The {scene} request did not leave the previous scene active during preparation")
        require(first_reload_asset == old_asset and pending_loading["pending"] and
                pending_loading["progressFrames"] == old_loading["progressFrames"] and
                first_copy["state"] == "uploading" and 0 < first_copy["submittedBytes"] < first_copy["totalBytes"],
                f"The original {scene} view did not keep rendering during staged replacement: {first_copy}")
        require(completed_copy["state"] == "idle" and completed_copy["requestId"] == first_copy["requestId"] and
                completed_copy["completedBytes"] == completed_copy["totalBytes"] == first_copy["totalBytes"] and
                completed_copy["publications"] == first_copy["publications"] + 1 and
                0 < completed_copy["maxBytesPerTick"] <= INTERACTIVE_GEOMETRY_KIB * 1024 and
                not detail["gui"]["modelLoading"]["pending"],
                f"Staged replacement did not publish within the geometry copy limit in {scene} {case}")
        result["interactiveReload"] = dict(firstCopy=first_copy, completedCopy=completed_copy,
                                           previousGeometryAsset=old_asset, newGeometryAsset=detail_asset)
    else:
        require(first_reload_asset == detail_asset, f"Synchronous reload did not immediately publish in {scene} {case}")
    result["device"] = {field: overview[field] for field in (
        "gpu", "vendorId", "deviceId", "driverVersion", "vulkanApiVersion", "build", "framesInFlight", "swapchainFormat")}
    result["device"]["gpuTimingSource"] = overview["gpuProfiler"]["source"]
    # The detail snapshot follows a same-process reload, which can replace the startup cache diagnostics.
    result["cache"] = states["overview"]["assetCache"][CACHE_ROWS[scene]]
    result["reloadCache"] = states["detail"]["assetCache"][CACHE_ROWS[scene]]
    result["raySceneBuild"] = acceleration_builds(states["overview"], states["detail"], reload_frame,
                                                  args.no_ray_query, args.require_ray)
    result["telemetryCheckpoints"] = {
        phase: {"telemetry": str(capture_paths(image, frame, plan)[1]), "assetResidency": early[frame]["assetResidency"],
                "geometryStreaming": early[frame]["geometryStreaming"],
                "vma": {key: early[frame]["shaderReload"][key] for key in VMA_FIELDS}}
        for phase, frame in (("startup", first_frame), ("firstReloadFrame", reload_frame),
                             *(("preparingReload", frame) for frame in preparation_frames))}
    result["vmaSampledPeak"] = {
        key: max(sample["vma"][key] for sample in (*result["views"].values(), *result["telemetryCheckpoints"].values()))
        for key in VMA_FIELDS}
    print(f"{scene} {case}: initial scene {result['startup']['initialSceneMs']:.1f} ms, "
          f"cache {result['cache']['status']}", flush=True)
    return result


def compare_runs(runs):
    reference, cold, warm = (runs[name] for name in ("reference", "cold", "warm"))
    require(all(run["device"] == reference["device"] for run in runs.values()), "Build or GPU configuration changed")
    require(cold["cache"]["status"] == "miss" and warm["cache"]["status"] == "hit"
            and cold["cache"]["key"] and warm["cache"]["key"] == cold["cache"]["key"]
            and cold["cache"]["bytesWritten"] > 0 and warm["cache"]["bytesRead"] > 0,
            f"Expected a populated cold miss and same-key warm hit, got {cold['cache']} and {warm['cache']}")
    if "invalidated" in runs:
        edited, rewarmed = runs["invalidated"]["cache"], runs["rewarmed"]["cache"]
        require(edited["status"] == "miss" and edited["key"] != cold["cache"]["key"] and edited["bytesWritten"] > 0
                and rewarmed["status"] == "hit" and rewarmed["key"] == edited["key"] and rewarmed["bytesRead"] > 0,
                f"Changing external POSITION bytes did not replace the derived key: {edited} and {rewarmed}")

    def compare_views(a, b):
        comparisons = {}
        for view, first in a["views"].items():
            second = b["views"][view]
            require(first["camera"]["unjitteredViewProjColumns"] == second["camera"]["unjitteredViewProjColumns"]
                    and first["objectCount"] == second["objectCount"]
                    and first["geometryLod"]["baseTriangles"] == second["geometryLod"]["baseTriangles"],
                    f"The captures disagreed on {view} camera or source geometry")
            comparisons[view] = image_metrics(first["image"]["path"], second["image"]["path"])
        return comparisons

    original_quality = compare_views(cold, warm)
    pairs = (original_quality,) if "rewarmed" not in runs else (
        original_quality, compare_views(runs["invalidated"], runs["rewarmed"]))
    for pair in pairs:
        require(all(view["rmse"] <= 1 and view["pixelsAboveOne"] <= view["pixelCount"] * 0.005
                    for view in pair.values()), f"Derived-cache state changed the converged image: {pair}")
    return dict(referenceVsCold=compare_views(reference, cold), referenceVsWarm=compare_views(reference, warm),
                coldVsWarm=original_quality, **({"invalidatedVsRewarmed": pairs[-1]} if len(pairs) > 1 else {}),
                startupWarmMinusColdMs={key: warm["startup"][key] - cold["startup"][key]
                                        for key in ("initialSceneMs", "firstSubmittedMs")},
                derivedCacheWarmMinusColdMs=warm["cache"]["elapsedMs"] - cold["cache"]["elapsedMs"],
                vmaSampledPeakWarmMinusReference={key: warm["vmaSampledPeak"][key] - reference["vmaSampledPeak"][key]
                                                  for key in VMA_FIELDS})


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--renderer", type=Path, required=True)
    parser.add_argument("--tekla", type=Path, help="Native TeklaHouse.ifcx")
    parser.add_argument("--gltf", type=Path, help="Original external-buffer ABeautifulGame.gltf")
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--technique", default="forward-raster", choices=("forward-raster", "deferred-raster"))
    parser.add_argument("--width", type=int, default=960)
    parser.add_argument("--height", type=int, default=540)
    parser.add_argument("--warmup", type=int, help="Ordinary warm-up frames per view (default 120 Tekla, 480 chess)")
    parser.add_argument("--samples", type=int, default=24, help="Uncaptured frames in each timing window")
    parser.add_argument("--timeout", type=int, default=600, help="Seconds per renderer process")
    parser.add_argument("--asset-memory-mib", type=int, default=0)
    parser.add_argument("--asset-upload-mib", type=int, default=0)
    parser.add_argument("--unbounded-reference", action="store_true",
                        help="Apply asset caps only to streaming and cache runs")
    parser.add_argument("--interactive-reload", action="store_true",
                        help="Exercise staged replacement while the previous large scene keeps rendering")
    parser.add_argument("--texture-chunk-kib", type=int, default=4096, help="Texture copy cap for the streaming runs")
    parser.add_argument("--validation-manifest", type=Path, help="VkLayer_khronos_validation.json for sync validation")
    ray = parser.add_mutually_exclusive_group()
    ray.add_argument("--require-ray", action="store_true", help="Require timed startup and reload AS builds")
    ray.add_argument("--no-ray-query", action="store_true", help="Run the capability fallback path")
    parser.add_argument("--plan-only", action="store_true", help="Prepare inputs and commands without running the renderer")
    args = parser.parse_args()
    if not (args.tekla or args.gltf):
        parser.error("supply --tekla, --gltf, or both")
    if not ((args.warmup is None or 8 <= args.warmup <= 4500) and 8 <= args.samples <= 120
            and min(args.width, args.height, args.timeout) > 0
            and min(args.asset_memory_mib, args.asset_upload_mib) >= 0 and 1 <= args.texture_chunk_kib <= 4096):
        parser.error("require 8–4500 warm-up frames, 8–120 samples, positive extent and timeout and 1–4096 KiB chunks")
    if (args.tekla and args.tekla.suffix.lower() != ".ifcx") or (args.gltf and args.gltf.suffix.lower() != ".gltf"):
        parser.error("expected native IFCX and external-buffer glTF source files")
    if args.unbounded_reference and not (args.asset_memory_mib or args.asset_upload_mib):
        parser.error("--unbounded-reference requires at least one asset cap for the streamed runs")

    binary = file_identity(args.renderer)
    args.renderer = Path(binary["path"])
    args.output.mkdir(parents=True, exist_ok=True)
    output = Path(tempfile.mkdtemp(prefix="asset-bench-", dir=args.output.resolve()))
    report_path = output / "results.json"
    print(f"Benchmark evidence in {output}", flush=True)
    environment = os.environ.copy()
    validation = validation_environment(args.validation_manifest, output, environment) if args.validation_manifest else None
    report = dict(schemaVersion=1, passed=False, planOnly=args.plan_only, binary=binary, validation=validation,
                  platform=dict(system=platform.system(), release=platform.release(), architecture=platform.machine()),
                  coldPolicy="Fresh process with an empty renderer-derived cache. The OS file cache and the legacy BIM "
                             "metadata cache beside the executable are unmanaged.",
                  comparisonPolicy="Full-detail reference and streamed views use the same extent, camera and lighting.",
                  assetCapsMiB={case: dict(zip(("resident", "upload"), asset_caps(args, case), strict=True))
                                for case in ("reference", "streamed")},
                  interactiveReload=args.interactive_reload,
                  vmaPolicy="VMA peaks are sampled at capture checkpoints. The asset budget reports its own lifetime peak.",
                  scenes={})
    try:
        for scene, source in (("tekla", args.tekla), ("chess", args.gltf)):
            if source is None:
                continue
            source = source.resolve(strict=True)
            scene_root = output / scene
            scene_root.mkdir()
            model, files, edit = source, [file_identity(source)], None
            if scene == "chess":
                model, document, files = copy_gltf(source, scene_root / "fixture")
                edit = position_edit(model, document)
            runs = {}
            evidence = report["scenes"][scene] = dict(upstream=PROVENANCE[scene], sourceFiles=files, runs=runs)
            for case in ("reference", "cold", "warm"):
                runs[case] = run_case(args, scene, model, case, scene_root, environment, validation)
            if edit:
                evidence["invalidationEdit"] = edit if args.plan_only else edit_gltf_position(model, edit)
                for case in ("invalidated", "rewarmed"):
                    runs[case] = run_case(args, scene, model, case, scene_root, environment, validation)
            if not args.plan_only:
                evidence["comparisons"] = compare_runs(runs)
            require(all(file_identity(Path(entry["path"])) == entry for entry in files),
                    f"Original benchmark source changed during the run: {source}")
        require(file_identity(args.renderer) == binary, "Renderer executable changed during the run")
        report["passed"] = not args.plan_only
    except Exception as error:
        report["error"] = str(error)
        raise
    finally:
        write_json(report_path, report)


if __name__ == "__main__":
    main()
