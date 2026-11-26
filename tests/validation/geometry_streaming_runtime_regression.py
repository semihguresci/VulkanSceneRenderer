"""Validate bounded native geometry streaming, cancellation and raster continuation."""

from __future__ import annotations

import argparse
import hashlib
import math
import os
from pathlib import Path
import statistics
import tempfile
import time

from display_capture_test_support import Asset
from geometry_lod_runtime_regression import BIM_STOREY, PANEL_CENTERS, PANEL_GUIDS, TECHNIQUES, bim_mesh, write_fixtures
from scene_asset_budget_runtime_regression import panel_grid, require, write_json


OPT_IN = "CONTAINER_RUN_GPU_GEOMETRY_STREAMING"
PROVIDERS = ("gltf", "bim", "usd")
WIDTH, HEIGHT, FOV = 960, 540, 42
NEAR, FAR = 6, 205
CHUNK_KIB = 256
CHUNK_BYTES = CHUNK_KIB * 1024
STAGING_MIB = 2
STAGING_BYTES = STAGING_MIB * 1024 * 1024
FINAL_GUID = "streaming-accepted-panel"


def projected_probe(distance):
    focal = HEIGHT / (2 * math.tan(math.radians(FOV / 2)))
    return [int(WIDTH / 2 + focal * PANEL_CENTERS[0] / (distance - .25)), HEIGHT // 2]


def dense_panels(provider, name, color):
    # glTF keeps shared vertices and dotBIM/USD expand triangles. Each source
    # is large enough for the vertex allocation alone to exceed the 2 MiB cap.
    size = 200 if provider == "gltf" else 97
    point_data, indices = panel_grid(size)
    visible = [(x + PANEL_CENTERS[0], y - 1, z) for x, y, z in point_data]

    def quad(x, z):
        return [(x - .20, -.20, z), (x + .20, -.20, z), (x + .20, .20, z), (x - .20, .20, z)]

    low = [0, 1, 2, 0, 2, 3]
    return [(name, visible, indices, color, BIM_STOREY),
            ("occluded-panel", quad(PANEL_CENTERS[0], -15), low, (.02, .10, .95), BIM_STOREY),
            ("offscreen-panel", quad(500, 0), low, (.95, .9, .02), "Other Storey")]


def write_candidate(path, provider, guid, color):
    panels = dense_panels(provider, guid, color)
    if provider == "gltf":
        asset = Asset()
        asset.model["nodes"] = []
        asset.model["scenes"] = [dict(nodes=list(range(len(panels))))]
        for index, (name, points, indices, color, _) in enumerate(panels):
            attributes = dict(POSITION=asset.accessor("VEC3", points),
                              NORMAL=asset.accessor("VEC3", [(0, 0, 1)] * len(points)))
            primitive = dict(attributes=attributes, material=index,
                             indices=asset.accessor("SCALAR", indices, 5123, indices=True))
            asset.model["nodes"].append(dict(name=name, mesh=index))
            asset.model["meshes"].append(dict(primitives=[primitive]))
            asset.model["materials"].append(dict(pbrMetallicRoughness=dict(baseColorFactor=[*color, 1],
                                                                          metallicFactor=0, roughnessFactor=.85)))
        asset.write(path)
    elif provider == "bim":
        meshes, elements = [], []
        for index, (name, points, indices, color, storey) in enumerate(panels):
            meshes.append(bim_mesh(index, points, indices))
            elements.append(dict(mesh_id=index, guid=name, sourceId=name, type="IfcBuildingElementProxy",
                                 storeyName=storey, color=dict(zip("rgba", [*(round(c * 255) for c in color), 255]))))
        write_json(path, dict(schema_version="1.1.0", meshes=meshes, elements=elements))
    else:
        lines = ['#usda 1.0', '(upAxis = "Y" metersPerUnit = 1)', 'def Xform "StreamingFixture" {']
        for index, (name, points, indices, color, storey) in enumerate(panels):
            lines += [f'def Mesh "Panel{index}" (customData = {{ string guid = "{name}"',
                      f'    string storeyName = "{storey}" }}) {{',
                      'point3f[] points = [' + ', '.join(str(point) for point in points) + ']',
                      'int[] faceVertexCounts = [' + ', '.join(['3'] * (len(indices) // 3)) + ']',
                      'int[] faceVertexIndices = [' + ', '.join(map(str, indices)) + ']',
                      'uniform token subdivisionScheme = "none"',
                      f'color3f[] primvars:displayColor = [{color}]', 'bool doubleSided = false', '}']
        path.write_text('\n'.join([*lines, '}', '']), encoding="utf-8")


def capture_sequence(provider, source, abandoned, candidate, *, correctness):
    def camera(frame, distance):
        return dict(frame=frame, position=[0, 0, distance], target=[0, 0, 0])

    def request(path):
        return {"guiReload": str(path), "guiWaitForLoad": False, "guiWaitForUploadChunks": 1}

    far_probe = {"depthProbe": projected_probe(FAR)}
    events = {1: {"bimStoreyFilter": BIM_STOREY}} if provider != "gltf" else {}
    if correctness:
        # Let the optional primary LOD worker publish the small source before
        # testing fallback continuity, both at startup and after reloading it.
        steps = dict(before=20, first=21, firstLater=25, second=26, secondLater=30, unloaded=35,
                     restored=57, acceptedFirst=58, acceptedLater=62, published=104, near=111, clean=116)
        near_probe = {"depthProbe": projected_probe(NEAR)}
        events.update({steps[name]: far_probe for name in ("before", "unloaded", "restored", "published")})
        events.update({steps[name]: near_probe for name in ("near", "clean")})
        for key, incoming in (("first", abandoned), ("second", candidate), ("acceptedFirst", candidate)):
            events[steps[key]] = {**request(incoming), **far_probe}
        # Rebuild at an odd extent while the first copy batch is still live.
        # Return to the original capture extent to compare the old view and pick exactly.
        events[steps["first"] + 1] = {"resize": [WIDTH + 9, HEIGHT + 7]}
        events[steps["first"] + 2] = {"resize": [WIDTH, HEIGHT]}
        for name in ("firstLater", "secondLater", "acceptedLater"):
            events[steps[name]] = {"guiWaitForUploadChunks": 4, **far_probe}
        events[steps["secondLater"] + 1] = {"guiUnload": True}
        events[steps["unloaded"] + 1] = {"guiReload": str(source)}
        events[steps["near"] + 1] = {"guiUnload": True}
        change_view = steps["published"] + 3
        if provider != "gltf":
            events[change_view] = {"bimStoreyFilter": ""}
        cameras = [camera(1, FAR), camera(change_view - 1, FAR), camera(change_view, NEAR), camera(steps["clean"], NEAR)]
    else:
        steps = dict(before=4, published=36)
        events.update({4: far_probe, 5: {"guiReload": str(candidate)}, 36: far_probe})
        cameras = [camera(1, FAR), camera(36, FAR)]
    sequence = dict(schemaVersion=1, frames=max(steps.values()), sampleFrames=list(steps.values()),
                    camera=cameras, events=[dict(frame=frame, **event) for frame, event in sorted(events.items())])
    return sequence, steps


def run_capture(renderer, root, name, source, sequence, technique, *, chunk_kib, lod, staging_mib):
    from gui_model_loading_regression import run_lifecycle_capture

    directory = root / name
    directory.mkdir()
    (directory / "vk_layer_settings.txt").write_text("khronos_validation.validate_sync = true\n", encoding="utf-8")
    flags = ("--screenshot-viewport", "--import-scale", "1", "--ray-shadows", "raster", "--camera-fov", str(FOV),
             "--sky", "plain", "--sky-horizon", "0", "0", "0", "--background-color", "0", "0", "0",
             "--environment-intensity", ".15", "--directional-direction", "0", "0", "-1",
             "--directional-intensity", "2.5", "--exposure", ".8", "--tone-map", "neutral",
             "--geometry-upload-kb", str(chunk_kib), "--geometry-lod-error", str(lod),
             "--asset-upload-mb", str(staging_mib), "--asset-memory-mb", "64" if staging_mib else "0")
    start = time.perf_counter()
    record = run_lifecycle_capture(renderer, directory, name, source, sequence, technique, "raster", extra=flags)
    record["processSeconds"] = time.perf_counter() - start
    log = directory / f"{name}.log"
    require('Insert instance layer "VK_LAYER_KHRONOS_validation"' in log.read_text(encoding="utf-8", errors="replace"),
            f"Vulkan loader did not confirm an active validation layer. See {log}")
    record["log"] = str(log)
    return record


def captures_by_step(record, steps):
    frames = {entry["frame"]: entry for entry in record["samples"]}
    return {label: frames[frame] for label, frame in steps.items()}


def provider_record(state, expected_path):
    scene = state["scene"]
    paths = [Path(value).resolve() for value in (scene["primary"], scene["auxiliary"]) if value]
    providers = state["assetResidency"]["providers"]
    if expected_path is None:
        require(not paths and not providers, f"An unloaded scene still published {paths} or {providers}")
        return None
    require(paths == [expected_path] and len(providers) == 1 and providers[0]["geometryAssetId"] > 0,
            f"Expected only {expected_path.name} to be live, got {paths} and {providers}")
    return providers[0]


def opaque_pick(state, provider, expected, pixel):
    probe = state["depthProbe"]
    interaction = probe["sceneInteraction"]
    raw = interaction["rawGpuId"]
    require(probe["pixel"] == pixel and probe["opaque"] is not None and probe["opaque"] > 0 and
            type(raw) is int and raw > 0, f"The visible {provider} panel lost its GPU pick at {pixel}")
    if provider == "gltf":
        cpu, gpu = (interaction[route]["hit"] for route in ("cpu", "gpu"))
        require(cpu and gpu and cpu["name"] == expected and gpu["name"] == expected,
                f"The CPU or GPU picked the wrong primary panel at {pixel}: {cpu} and {gpu}")
        return dict(raw=raw, cpu=cpu, gpu=gpu)
    bim = interaction["bim"]
    require(bim and bim["guid"] == expected, f"GPU picked the wrong BIM/USD panel: {bim}")
    return dict(raw=raw, **bim)


def pixel_evidence(path, pixel):
    from PIL import Image

    with Image.open(path) as image:
        rgb = image.convert("RGB")
        return rgb.getpixel(tuple(pixel)), hashlib.sha256(rgb.tobytes()).hexdigest()


def gpu_route(state, provider, *, use_compaction=True, prove_culling=False):
    if provider == "gltf":
        passes = {entry["name"]: entry for entry in state["passes"]}
        names = ("FrustumCull", "HiZGenerate", "OcclusionCull")
        require(all(passes[name]["active"] for name in names), "Primary scene lost its GPU indirect culling passes")
        counts = state["rasterCulling"]
        if prove_culling:
            require(counts["inputCount"] > counts["frustumPassedCount"] > counts["occlusionPassedCount"] > 0,
                    f"Expected the offscreen and occluded panels to be rejected by their GPU passes: {counts}")
        return dict(passes={name: passes[name] for name in names}, counts=counts)
    bim = state["geometryLod"]["bim"]
    route = {key: bim[key] for key in ("opaqueCompactionReady", "viewportFrustumReady", "viewportOcclusionReady")}
    require(route["opaqueCompactionReady"] is use_compaction and route["viewportFrustumReady"] is True and
            route["viewportOcclusionReady"] is True,
            f"Unexpected BIM/USD semantic compaction or viewport frustum/Hi-Z readiness: {route}")
    return route


def streaming_stats(state, chunk_kib, staging_mib):
    stream = state["geometryStreaming"]
    asset = state["assetResidency"]
    require(stream["enabled"] is bool(chunk_kib) and stream["chunkCapBytes"] == chunk_kib * 1024 and
            state["validationEnabled"] and not state["rayShadows"]["supported"],
            f"Wrong geometry streaming or raster fallback mode: {stream}")
    staging = staging_mib * 1024 * 1024
    require(asset["limits"]["uploadBytes"] == staging and
            0 <= asset["used"]["uploadBytes"] <= asset["peak"]["uploadBytes"] and
            (not staging or asset["peak"]["uploadBytes"] <= staging),
            f"Geometry streaming exceeded its staging allocation cap: {asset}")
    resident = 64 * 1024 * 1024 if staging else 0
    require(asset["limits"]["residentBytes"] == resident and
            0 <= asset["used"]["residentBytes"] <= asset["peak"]["residentBytes"] and
            (not resident or asset["peak"]["residentBytes"] <= resident),
            f"Geometry streaming exceeded its resident allocation cap: {asset}")
    reload = stream["reload"]
    require(math.isfinite(reload["maxServiceMs"]) and reload["maxServiceMs"] >= 0,
            f"Missing model service time: {reload}")
    if chunk_kib:
        require(reload["maxBytesPerTick"] <= chunk_kib * 1024,
                f"A geometry upload tick exceeded the configured byte quantum: {reload}")
        optional = stream.get("primaryLod")
        if optional:
            require(optional["maxBytesPerTick"] <= chunk_kib * 1024 and
                    (not staging or optional["peakStagingBytes"] <= staging),
                    f"The optional primary LOD upload exceeded its copy or staging cap: {optional}")
    return reload


def snapshot(entry, state, reload, **extra):
    path = Path(entry["png"])
    return dict(frame=entry["frame"], image=str(path), telemetry=str(path.with_suffix(".telemetry.json")),
                reload=reload, guiLoading=state["gui"]["modelLoading"], residency=state["assetResidency"],
                swapchainRecreates=state["swapchainRecreates"],
                primaryLodUpload=state["geometryStreaming"].get("primaryLod"),
                vmaBytes=state["shaderReload"]["vmaBytes"], gpuKnownMs=state["gpuKnownMs"], **extra)


def verify_correctness(record, steps, provider, source, candidate):
    entries = captures_by_step(record, steps)
    results = {}
    pending = ("first", "firstLater", "second", "secondLater", "acceptedFirst", "acceptedLater")
    for name, entry in entries.items():
        state = entry["telemetry"]
        reload = streaming_stats(state, CHUNK_KIB, STAGING_MIB)
        if name in ("unloaded", "clean"):
            provider_record(state, None)
            require(reload["state"] == "idle" and state["assetResidency"]["used"]["uploadBytes"] == 0 and
                    state["depthProbe"]["sceneInteraction"]["rawGpuId"] in (0, None),
                    f"Cancelled or unloaded resources remained reachable: {name}")
            results[name] = snapshot(entry, state, reload)
            continue

        old_scene = name not in ("published", "near")
        geometry = provider_record(state, source if old_scene else candidate)
        pixel = projected_probe(NEAR if name == "near" else FAR)
        pick = opaque_pick(state, provider, PANEL_GUIDS[0] if old_scene else FINAL_GUID, pixel)
        color, digest = pixel_evidence(entry["png"], pixel)
        route = gpu_route(state, provider, use_compaction=name != "near", prove_culling=name == "near")
        lod = state["geometryLod"]["primary" if provider == "gltf" else "bim"]
        if old_scene:
            require(color[0] > color[1] + 12 and lod["triangles"] < lod["baseTriangles"],
                    f"Old source lost the visible red fallback or resident LOD at {name}")
        else:
            require(color[1] > color[0] + 12, f"Accepted geometry did not replace the visible panel at {name}")
        if name in pending:
            require(reload["state"] in ("uploading", "retiring") and reload["requestId"] > 0 and
                    0 <= reload["completedBytes"] <= reload["submittedBytes"] < reload["totalBytes"] and
                    reload["submittedBytes"] > 0 and reload["totalBytes"] > STAGING_BYTES and
                    0 < reload["peakStagingBytes"] <= STAGING_BYTES and
                    reload["submittedChunks"] >= math.ceil(reload["submittedBytes"] / CHUNK_BYTES) and
                    state["gui"]["modelLoading"]["pending"], f"Checkpoint did not retain an incomplete upload: {reload}")
        results[name] = snapshot(entry, state, reload, geometry=geometry, gpuRoute=route, lod=lod,
                                 pick=pick, rgb=color, pixelSha256=digest)

    for first_name, later_name, stable_name in (("first", "firstLater", "before"),
                                               ("second", "secondLater", "before"),
                                               ("acceptedFirst", "acceptedLater", "restored")):
        first, later, stable = (results[name] for name in (first_name, later_name, stable_name))
        for row in (first, later):
            require(all(row[field] == stable[field] for field in ("geometry", "pick", "pixelSha256")),
                    f"A partial upload changed the old geometry, pick, or viewport at {row['frame']}")
            require(row["reload"]["publications"] == stable["reload"]["publications"],
                    "A partial upload reported an early publication")
            require(row["guiLoading"]["progressFrames"] == stable["guiLoading"]["progressFrames"],
                    "The upload presented a progress-only frame over the live viewport")
        require(first["reload"]["requestId"] == later["reload"]["requestId"] and
                first["reload"]["submittedBytes"] < later["reload"]["submittedBytes"] and
                later["reload"]["submittedChunks"] >= 4,
                f"The {later_name} checkpoint did not observe further bounded upload work")

    before, second, unloaded, restored, accepted, published, clean = [results[name] for name in
        ("before", "second", "unloaded", "restored", "acceptedFirst", "published", "clean")]
    require(results["firstLater"]["swapchainRecreates"] == results["first"]["swapchainRecreates"] + 2,
            "The partial upload did not survive both viewport resizes")
    require(second["reload"]["requestId"] > results["first"]["reload"]["requestId"] and
            second["reload"]["cancellations"] == before["reload"]["cancellations"] + 1 and
            unloaded["reload"]["cancellations"] == before["reload"]["cancellations"] + 2,
            "Replacement and unload did not retire both partially submitted requests")
    optional_upload = published["primaryLodUpload"]
    optional_staging = 0
    if provider == "gltf" and optional_upload and optional_upload["state"] in ("uploading", "retiring"):
        optional_staging = optional_upload["peakStagingBytes"]
    require(published["reload"]["requestId"] == accepted["reload"]["requestId"] and
            published["reload"]["state"] == "idle" and
            published["reload"]["completedBytes"] == published["reload"]["submittedBytes"] ==
            published["reload"]["totalBytes"] > STAGING_BYTES and
            published["reload"]["publications"] == restored["reload"]["publications"] + 1 and
            published["geometry"]["geometryAssetId"] != restored["geometry"]["geometryAssetId"] and
            published["residency"]["used"]["uploadBytes"] <= optional_staging,
            "The completed geometry did not publish atomically or left a staging allocation active")
    require(clean["residency"]["used"]["residentBytes"] == unloaded["residency"]["used"]["residentBytes"],
            "Retiring the accepted scene and cancelling an upload left different scene allocations")
    return dict(log=record["log"], samples=results)


def verify_performance(record, steps, provider, source, candidate, chunk_kib):
    entries = captures_by_step(record, steps)
    state = entries["published"]["telemetry"]
    before = entries["before"]["telemetry"]
    provider_record(before, source)
    provider_record(state, candidate)
    reload = streaming_stats(state, chunk_kib, 0)
    lod = state["geometryLod"]["primary" if provider == "gltf" else "bim"]
    require(reload["state"] == "idle" and reload["maxServiceMs"] > 0 and
            lod["baseTriangles"] == lod["triangles"] and lod["extraIndexBytes"] == 0,
            "Timing run did not finish or used geometry LOD processing")
    history = [row for row in state["frameHistory"][-16:] if row["cpuPhasesMs"]["Screenshot"] == 0]
    gpu_ms = [row["gpuGraphicsMs"] for row in history if row["gpuGraphicsMs"] is not None]
    require(gpu_ms and all(value >= 0 for value in gpu_ms), "No retired GPU frame timing samples were recorded")
    return snapshot(entries["published"], state, reload, log=record["log"], processSeconds=record["processSeconds"],
                    medianGraphicsGpuMs=statistics.median(gpu_ms), graphicsSamples=len(gpu_ms),
                    medianCpuFrameMs=statistics.median(row["cpuPhasesMs"]["Frame"] for row in history))


def verify_performance_comparison(modes):
    legacy, staged = (modes[key] for key in ("legacy", "staged"))
    peak_before, peak_after = (row["residency"]["peak"]["uploadBytes"] for row in (legacy, staged))
    require(peak_before > STAGING_BYTES >= peak_after > 0 and
            staged["reload"]["submittedBytes"] > STAGING_BYTES and staged["reload"]["submittedChunks"] >= 4,
            f"Matched runs did not replace the monolithic staging allocation with bounded uploads: {peak_before}, {peak_after}")
    _, legacy_pixels = pixel_evidence(legacy["image"], projected_probe(FAR))
    _, staged_pixels = pixel_evidence(staged["image"], projected_probe(FAR))
    require(legacy_pixels == staged_pixels, "Legacy and staged uploads published different viewport pixels")
    return dict(modes=modes, peakStagingDeltaBytes=peak_after - peak_before,
                peakModelServiceDeltaMs=staged["reload"]["maxServiceMs"] - legacy["reload"]["maxServiceMs"],
                medianGraphicsDeltaMs=staged["medianGraphicsGpuMs"] - legacy["medianGraphicsGpuMs"],
                vmaDeltaBytes=staged["vmaBytes"] - legacy["vmaBytes"])


def contact_sheet(path, cases):
    from PIL import Image, ImageDraw

    phases = [("before", "Before"), ("firstLater", "Partial upload"), ("acceptedLater", "After cancellation"),
              ("published", "Published")]
    width, height, label_height = 260, 154, 22
    canvas = Image.new("RGB", (len(phases) * width, len(cases) * (height + label_height)), "#15181b")
    labels = ImageDraw.Draw(canvas)
    for row, (name, case) in enumerate(cases.items()):
        for column, (key, label) in enumerate(phases):
            x, y = column * width, row * (height + label_height)
            labels.text((x + 5, y + 4), f"{name}  {label}", fill="white")
            with Image.open(case["samples"][key]["image"]) as source:
                crop = source.crop((WIDTH // 2 - 24, HEIGHT // 2 - 14, WIDTH // 2 + 24, HEIGHT // 2 + 14))
                canvas.paste(crop.convert("RGB").resize((width, height), Image.Resampling.NEAREST),
                             (x, y + label_height))
    canvas.save(path)


def digest(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--renderer", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--provider", choices=PROVIDERS, action="append")
    parser.add_argument("--technique", choices=TECHNIQUES, action="append")
    parser.add_argument("--suite", choices=("all", "correctness", "performance"), default="all")
    parser.add_argument("--managed-output", action="store_true")
    parser.add_argument("--require-opt-in", action="store_true")
    parser.add_argument("--generate-only", action="store_true")
    args = parser.parse_args()
    if args.require_opt_in and os.environ.get(OPT_IN) != "1":
        print(f"SKIP: set {OPT_IN}=1 to run native geometry streaming captures")
        return 77

    renderer, output = args.renderer.resolve(strict=not args.generate_only), args.output.resolve()
    if args.managed_output:
        output.mkdir(parents=True, exist_ok=True)
        output = Path(tempfile.mkdtemp(prefix="run-", dir=output))
    else:
        output.mkdir(parents=True, exist_ok=False)
    checksum = None if args.generate_only else digest(renderer)
    os.environ["VK_LOADER_DEBUG"] = "layer"
    sources = write_fixtures(output)
    techniques = args.technique or TECHNIQUES
    results = dict(passed=False, generateOnly=args.generate_only, rendererSha256=checksum,
                   correctness={}, performance={})
    try:
        for provider in args.provider or PROVIDERS:
            source = sources[provider]
            abandoned = output / f"abandoned{source.suffix}"
            candidate = output / f"accepted{source.suffix}"
            write_candidate(abandoned, provider, "streaming-abandoned-panel", (.75, .02, .65))
            write_candidate(candidate, provider, FINAL_GUID, (.05, .80, .10))
            if args.suite in ("all", "correctness"):
                sequence, steps = capture_sequence(provider, source, abandoned, candidate, correctness=True)
                if args.generate_only:
                    write_json(output / f"{provider}-correctness.json", sequence)
                else:
                    for technique in techniques:
                        name = f"{provider}-{technique}"
                        record = run_capture(renderer, output, name, source, sequence, technique,
                                             chunk_kib=CHUNK_KIB, lod=2, staging_mib=STAGING_MIB)
                        results["correctness"][name] = verify_correctness(record, steps, provider, source, candidate)
            if args.suite in ("all", "performance"):
                sequence, steps = capture_sequence(provider, source, abandoned, candidate, correctness=False)
                if args.generate_only:
                    write_json(output / f"{provider}-performance.json", sequence)
                else:
                    modes = {}
                    for mode, chunk_kib in (("legacy", 0), ("staged", CHUNK_KIB)):
                        record = run_capture(renderer, output, f"{provider}-{techniques[0]}-{mode}",
                                             source, sequence, techniques[0], chunk_kib=chunk_kib, lod=0, staging_mib=0)
                        modes[mode] = verify_performance(record, steps, provider, source, candidate, chunk_kib)
                    results["performance"][provider] = verify_performance_comparison(modes)
        if results["correctness"]:
            comparison = output / "comparison.png"
            contact_sheet(comparison, results["correctness"])
            results["comparison"] = str(comparison)
        if not args.generate_only:
            require(digest(renderer) == checksum, "Renderer binary changed during validation")
            results["passed"] = True
    finally:
        write_json(output / "results.json", results)
    print(f"{'PLAN' if args.generate_only else 'PASS'} {output / 'results.json'}", flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
