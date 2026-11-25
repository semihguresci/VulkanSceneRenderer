"""Exercise native and Vulkan FSR reconstruction through the viewer capture path."""

import argparse
import json
import math
import os
from pathlib import Path
import re
import statistics
import struct
import subprocess

import numpy as np
from PIL import Image

from fsr_ui_capture_proof import verify_captures as verify_ui_captures
from gltf_extended_materials_runtime_regression import validation_log_error
from graphics_concurrency_benchmark import file_identity, measurement, validation_environment


ROOT = Path(__file__).resolve().parents[2]
RATIOS = {"native": 10, "quality": 15, "balanced": 17, "performance": 20}
SDK = "2.4.0"
X64_PE_MACHINE = 0x8664
RUNTIME_HASHES = {
    "amd_fidelityfx_loader_vk.dll": "38a56a01fb3565c0757d9f5c1e4e69de4b65e5f2e5769b323b200dc1f142bab9",
    "amd_fidelityfx_upscaler_vk.dll": "4a7e895af6e0b615d40a84d59eb8c2b1054ad91cdd0c739567940270d51bdab7",
}
FALLBACK_REASONS = {
    "sdk-disabled": {"Built without the Vulkan FSR SDK"},
    "missing-runtime": {f"The pinned FSR SDK {SDK} loader/upscaler DLLs are unavailable beside the executable"},
    "unsupported-device": {
        "GPU lacks the required Vulkan FSR features",
        "GPU lacks the required Vulkan FSR image formats",
        "GPU lacks the exact image formats required by the analytical FSR provider",
    },
}
VALIDATION_INSERTED = re.compile(r'Insert instance layer "VK_LAYER_KHRONOS_validation" \(([^\r\n]+)\)')
VALIDATION_SETTINGS = ("khronos_validation.validate_sync = true\n"
                       "khronos_validation.duplicate_message_limit = 1000\n")
RESIZE_START = (640, 360)
RESIZE_ODD = (731, 409)
RESIZE_EVEN = (576, 416)
RASTER_TECHNIQUES = ("deferred-raster", "forward-raster")
SWAPCHAIN_FORMATS = {"bgra8-srgb": 50, "rgba8-srgb": 43, "bgra8-unorm": 44, "rgba8-unorm": 37}
VIEWPORT_FALLBACK = "Upscaling requires a perspective raster view at MSAA 1x and a supported display mode"
MEMORY_FIELDS = ("vmaBytes", "vmaDeviceLocalBytes", "vmaAllocations")


def sequence(frames, samples, events=(), moving="Moving cube"):
    return {"schemaVersion": 1, "frames": frames, "sampleFrames": samples,
            "events": events, "objectName": moving,
            "camera": [{"frame": 1, "position": [0, 1.8, 6.5], "target": [0, .8, 0]},
                       {"frame": frames, "position": [.2, 1.8, 6.5], "target": [0, .8, 0]}],
            "objectTranslation": [{"frame": 1, "translation": [-1, 0, 0]},
                                  {"frame": frames - 2, "translation": [1, 0, 0]}]}


def pe_machine(path):
    with path.open("rb") as stream:
        if stream.read(2) != b"MZ":
            return None
        stream.seek(0x3c)
        stream.seek(int.from_bytes(stream.read(4), "little"))
        header = stream.read(6)
    if len(header) != 6 or header[:4] != b"PE\0\0":
        return None
    return struct.unpack_from("<H", header, 4)[0]


def runtime_receipt(renderer, enabled):
    runtime = {name: file_identity(renderer.parent / name) if (renderer.parent / name).is_file() else None
               for name in RUNTIME_HASHES}
    shader = renderer.parent / "spv_shaders/fsr_inputs.comp.spv"
    machine = pe_machine(renderer)
    for name, info in runtime.items():
        if info:
            info["peMachine"] = pe_machine(Path(info["path"]))
        if enabled:
            assert info and info["sha256"] == RUNTIME_HASHES[name] and info["peMachine"] == X64_PE_MACHINE, (name, info)
    if enabled:
        assert machine == X64_PE_MACHINE and shader.is_file(), (renderer, machine, shader)
    return {"renderer": {**file_identity(renderer), "peMachine": machine}, "sdkPin": SDK, "backend": "Vulkan",
            "signedRuntime": runtime,
            "inputShader": file_identity(shader) if shader.is_file() else None}


def taa_switch_sequence():
    return {"schemaVersion": 1, "frames": 20, "sampleFrames": [4, 8, 12, 16, 20],
            "camera": [{"frame": 1, "position": [0, 1.8, 6.5], "target": [0, .8, 0]}],
            "events": [{"frame": 1, "taa": True}, {"frame": 5, "fsr": "quality"}, {"frame": 9, "taa": False},
                       {"frame": 13, "fsr": "native"}, {"frame": 17, "taa": True}]}


def temporal_resolve(state):
    return any(p["name"] == "TemporalResolve" and p["active"] for p in state["passes"])


def resize_plan(format_override=None):
    # The independently sized grids make an accidental render-aspect camera or
    # rounded-up render dimensions observable in a multiplication check.
    cases = [
        ("native-start", RESIZE_START, "native", 1, {}),
        ("quality-start", RESIZE_START, "quality", 1, {"fsr": "quality"}),
        ("odd-quality", RESIZE_ODD, "quality", 1, {"resize": RESIZE_ODD}),
        ("odd-balanced", RESIZE_ODD, "balanced", 1, {"fsr": "balanced"}),
        ("odd-native", RESIZE_ODD, "native", 1, {"fsr": "native"}),
        ("odd-performance", RESIZE_ODD, "performance", 1, {"fsr": "performance"}),
        ("even-performance", RESIZE_EVEN, "performance", 1, {"resize": RESIZE_EVEN}),
        ("even-balanced", RESIZE_EVEN, "balanced", 1, {"fsr": "balanced"}),
        ("even-native", RESIZE_EVEN, "native", 1, {"fsr": "native"}),
        ("even-quality", RESIZE_EVEN, "quality", 1, {"fsr": "quality"}),
        ("msaa-fallback", RESIZE_EVEN, "quality", 4, {"samples": 4}),
        ("msaa-restored", RESIZE_EVEN, "quality", 1, {"samples": 1}),
        ("native-restored", RESIZE_START, "native", 1, {"fsr": "native", "resize": RESIZE_START}),
        ("quality-restored", RESIZE_START, "quality", 1, {"fsr": "quality"}),
        ("acquire-recovery", RESIZE_START, "quality", 1, {"acquireOutOfDate": True}),
        ("present-recovery", RESIZE_START, "quality", 1, {"presentSuboptimal": True}),
    ]
    if format_override:
        cases.append(("format-only", RESIZE_START, "quality", 1, {"swapchainFormat": format_override}))
    phases, events, samples = [], [], []
    for index, (name, extent, mode, msaa, event) in enumerate(cases):
        start = index * 9 + 1
        # An injected present result is handled after the event frame rendered.
        first = start + int("presentSuboptimal" in event)
        settled = start + 8
        if event:
            events.append({"frame": start, **event})
        samples += [first, settled]
        phases.append(dict(name=name, displayExtent=extent, requested=mode, msaaSamples=msaa,
                           event=event, first=first, settled=settled))
    return {"schemaVersion": 1, "frames": samples[-1], "sampleFrames": samples,
            "camera": [{"frame": 1, "position": [0, 1.8, 6.5], "target": [0, .8, 0]}],
            "events": events}, phases


def resize_capture_paths(base, frame, end):
    image = base if frame == end else base.with_name(f"{base.stem}.frame-{frame:04d}.png")
    return image, image.with_suffix(".telemetry.json")


def resize_capture_metrics(state, image, phase, technique, provider, check_mode):
    extent, mode, msaa = phase["displayExtent"], phase["requested"], phase["msaaSamples"]
    active = mode != "native" and msaa == 1
    check_mode(state, mode, mode if active else "native", extent)
    assert state["technique"] == technique and state["msaaSamples"] == msaa and state["resolution"] == list(extent), image
    assert not state["taa"]["enabled"] and not state["dlss"]["active"], image
    if msaa > 1:
        assert state["fsr"]["status"] == VIEWPORT_FALLBACK, state["fsr"]

    passes = {item["name"]: item for item in state["passes"]}
    assert passes["DepthPrepass"]["active"] and passes["Lighting"]["active"] and passes["PostProcess"]["active"], image
    assert passes.get("GBuffer", {}).get("active", False) == (technique == "deferred-raster"), image
    assert passes["TemporalVelocity"]["active"] == passes["TemporalResolve"]["active"] == active, image

    # The length of each of the first two rows of the unjittered VP matrix's
    # rotational block is its projection scale, independent of view rotation.
    vp = np.asarray(state["camera"]["unjitteredViewProjColumns"], dtype=float)
    x_scale, y_scale = np.linalg.norm(vp[:3, :2], axis=0)
    aspect = y_scale / x_scale
    assert math.isclose(aspect, extent[0] / extent[1], rel_tol=1e-6), (image, aspect, extent)

    render_width, render_height = state["fsr"]["renderExtent"]
    output_pixel_bytes = 16 if provider == "3.1.6" else 8
    temporal_payload = ((47 * render_width * render_height + output_pixel_bytes * math.prod(extent))
                        * state["swapchainImages"]) if active else 0
    temporal = state["taa"]
    assert temporal["imagePayloadBytes"] == temporal_payload and temporal["allocatedImageBytes"] >= temporal_payload, image
    if not active:
        assert temporal["allocatedImageBytes"] == 0, image
    memory = {field: state["shaderReload"][field] for field in MEMORY_FIELDS}
    assert memory["vmaBytes"] >= temporal["allocatedImageBytes"] and memory["vmaAllocations"] > 0, (image, memory)
    metadata = json.loads(Path(f"{image}.capture.json").read_bytes())
    assert (metadata["width"], metadata["height"]) == extent and metadata["source"] == "viewport", (image, metadata)
    assert not metadata["includesUi"], (image, metadata)
    return dict(image=str(image), cameraAspect=aspect, engineTemporalBytes=temporal["allocatedImageBytes"],
                engineTemporalPayloadBytes=temporal_payload, temporalEpoch=temporal["epoch"],
                resetReason=temporal["resetReason"], vma=memory,
                passes={name: passes[name] for name in ("DepthPrepass", "GBuffer", "Lighting", "TemporalVelocity",
                                                       "TemporalResolve", "PostProcess") if name in passes},
                fsr=state["fsr"], guiPipelineGeneration=state["guiPipelineGeneration"],
                presentationPipelineGeneration=state["presentationPipelineGeneration"],
                swapchainRecreates=state["swapchainRecreates"], swapchainImages=state["swapchainImages"],
                swapchainFormat=state["swapchainFormat"], swapchainColorSpace=state["swapchainColorSpace"])


def verify_resize_matrix(base, states, images, phases, technique, provider, check_mode):
    first_state = states[phases[0]["first"]]
    swapchain_recreates = first_state["swapchainRecreates"]
    presentation_generation = first_state["presentationPipelineGeneration"]
    fmt = first_state["swapchainFormat"]
    prior = None
    evidence = {}
    last_frame = phases[-1]["settled"]
    for phase in phases:
        name, event, first_frame, settled_frame = (phase[key] for key in ("name", "event", "first", "settled"))
        swapchain_recreates += bool(event.keys() & {"resize", "acquireOutOfDate", "presentSuboptimal", "swapchainFormat"})
        presentation_generation += bool(event.keys() & {"samples", "swapchainFormat"})
        if "swapchainFormat" in event:
            selected = SWAPCHAIN_FORMATS[event["swapchainFormat"]]
            assert selected != fmt, (technique, "Choose a format different from the initial swapchain", selected)
            fmt = selected

        capture_evidence = {}
        for point, frame in (("first", first_frame), ("settled", settled_frame)):
            state = states[frame]
            image, telemetry = resize_capture_paths(base, frame, last_frame)
            observed = resize_capture_metrics(state, image, phase, technique, provider, check_mode)
            assert state["swapchainRecreates"] == swapchain_recreates, (image, observed, swapchain_recreates)
            assert state["presentationPipelineGeneration"] == presentation_generation, (image, observed, presentation_generation)
            assert state["swapchainFormat"] == fmt and state["swapchainColorSpace"] == first_state["swapchainColorSpace"], image
            assert np.max(np.std(images[frame], axis=(0, 1))) > 5, f"Unrendered scene after transition: {image}"
            capture_evidence[point] = {**observed, "frame": frame, "telemetry": str(telemetry)}
        first, settled = capture_evidence["first"], capture_evidence["settled"]
        assert first["temporalEpoch"] == settled["temporalEpoch"], (technique, name, capture_evidence)
        if prior:
            format_only = "swapchainFormat" in event and prior["swapchainImages"] == first["swapchainImages"]
            if format_only:
                assert first["temporalEpoch"] == prior["temporalEpoch"], (technique, name, "format-only resize reset FSR")
            else:
                assert first["temporalEpoch"] > prior["temporalEpoch"], (technique, name, "FSR history was not retired")
        prior = settled

        state = states[settled_frame]
        # Capture frame numbers are 1-based, unlike submission IDs. Count only
        # uncaptured submissions retired with matching timestamps in this phase.
        rows = [row for row in state["frameHistory"] if first_frame <= row["frameIndex"] < settled_frame - 1
                and row["gpuFrameIndex"] == row["frameIndex"]]
        gpu = [row["gpuGraphicsMs"] for row in rows if measurement(row["gpuGraphicsMs"])]
        if state["gpuProfiler"]["available"]:
            assert len(gpu) >= 2 and all(value > 0 for value in gpu), (technique, name, gpu)
        resolve = settled["passes"]["TemporalResolve"]
        capture_evidence.update(matchedGpuFrames=[row["frameIndex"] for row in rows],
                                medianGraphicsGpuMs=statistics.median(gpu) if gpu else None,
                                resolveGpuMs=resolve["gpuMs"] if resolve["active"] and resolve["gpuTimed"] else None)
        evidence[name] = {**phase, "samples": capture_evidence}

    def difference(first, second, *, flip_second=False):
        lhs = images[evidence[first]["settled"]]
        rhs = images[evidence[second]["settled"]]
        if flip_second:
            rhs = np.flipud(rhs)
        return float(np.mean(np.abs(lhs.astype(np.int16) - rhs)))

    quality = {
        "nativeBeforeAfterMae8Bit": difference("native-start", "native-restored"),
        "qualityBeforeAfterMae8Bit": difference("quality-start", "quality-restored"),
        "qualityVsNativeMae8Bit": difference("quality-restored", "native-restored"),
    }
    assert quality["nativeBeforeAfterMae8Bit"] <= .1 and quality["qualityBeforeAfterMae8Bit"] < 3, (technique, quality)
    assert 0 < quality["qualityVsNativeMae8Bit"] < 25, (technique, quality)
    quality["resizedOrientation"] = {}
    for extent in ("odd", "even"):
        native = f"{extent}-native"
        for mode in ("quality", "balanced", "performance"):
            stage = f"{extent}-{mode}"
            aligned = difference(native, stage)
            flipped = difference(native, stage, flip_second=True)
            assert aligned < 25 and 2 * aligned < flipped, (technique, stage, aligned, flipped)
            quality["resizedOrientation"][stage] = {"nativeMae8Bit": aligned, "flippedMae8Bit": flipped}
    if "format-only" in evidence:
        quality["formatOnlyMae8Bit"] = difference("present-recovery", "format-only")
        assert quality["formatOnlyMae8Bit"] < 3, (technique, quality)
    return evidence, quality


def verify_resize_ui(output, capture, check_mode):
    plan = {"schemaVersion": 1, "frames": 16, "sampleFrames": [3, 6, 9, 12, 16],
            "camera": [{"frame": 1, "position": [0, 1.8, 6.5], "target": [0, .8, 0]}],
            "events": [{"frame": 4, "fsr": "quality"}, {"frame": 7, "resize": RESIZE_ODD},
                       {"frame": 10, "fsr": "native"}, {"frame": 13, "fsr": "performance"}]}
    states, _ = capture("resize-ui-window", "native", plan, extent=RESIZE_START, ui=True)
    capture("resize-ui-viewport", "native", plan, extent=RESIZE_START)

    def image(surface, frame):
        return resize_capture_paths(output / f"resize-ui-{surface}.png", frame, plan["frames"])[0]

    pairs = (("initial-quality", 3, 6, RESIZE_START, "quality"),
             ("resized-quality", 12, 9, RESIZE_ODD, "quality"),
             ("resized-performance", 12, 16, RESIZE_ODD, "performance"))
    proof = {}
    for name, native, fsr, extent, mode in pairs:
        check_mode(states[native], "native", "native", extent)
        check_mode(states[fsr], mode, mode, extent)
        proof[name] = verify_ui_captures(image("window", native), image("window", fsr),
                                        image("viewport", native), image("viewport", fsr))
    return proof


def verify_ineligible_viewports(capture, check_mode, reference):
    events = [{"frame": 4, "orthographic": True}, {"frame": 5, "resize": RESIZE_ODD},
              {"frame": 7, "orthographic": False}]
    samples = {3: (RESIZE_START, "quality", "deferred-raster"), 6: (RESIZE_ODD, "native", "deferred-raster"),
               9: (RESIZE_ODD, "quality", "deferred-raster")}
    if reference:
        events += [{"frame": 10, "technique": "path-tracing"}, {"frame": 13, "technique": "deferred-raster"}]
        samples.update({12: (RESIZE_ODD, "native", "path-tracing"), 16: (RESIZE_ODD, "quality", "deferred-raster")})
    plan = {"schemaVersion": 1, "frames": max(samples), "sampleFrames": list(samples), "events": events,
            "camera": [{"frame": 1, "position": [0, 1.8, 6.5], "target": [0, .8, 0]}]}
    states, _ = capture("resize-viewport-fallback", "quality", plan, ray_queries=reference)
    for frame, (extent, effective, technique) in samples.items():
        state = states[frame]
        check_mode(state, "quality", effective, extent)
        assert state["technique"] == technique, (frame, state["technique"])
        if effective == "native":
            assert state["fsr"]["status"] == VIEWPORT_FALLBACK and not temporal_resolve(state), (frame, state["fsr"])
        if technique == "path-tracing":
            assert state["referencePath"]["active"], (frame, state["referencePath"])


def verify_resize_pbr(output, capture, check_mode):
    from gltf_material_authoring_runtime_regression import ANISOTROPY, PREFIX, base_material, edit, write_fixture

    initial = output / "pbr-fixtures/initial.gltf"
    incoming = output / "pbr-fixtures/extended.gltf"
    color = [.12, .48, .8, 1.]
    material = base_material()
    material["pbrMetallicRoughness"]["baseColorFactor"] = color
    material["extensions"][ANISOTROPY] = {"anisotropyStrength": .7, "anisotropyRotation": .65}
    write_fixture(initial)
    write_fixture(incoming, material)

    material_edit = edit(4, {PREFIX + "anisotropy/anisotropyStrength": .7,
                             PREFIX + "anisotropy/anisotropyRotation": .65,
                             "pbrMetallicRoughness/baseColorFactor": color})
    merge = {"frame": 4, "guiWaitForLoad": True,
             "merge": {"path": str(incoming), "parentName": "Extended PBR incoming",
                       "placementMatrix": [1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, .65, .25, 1]}}
    start = (641, 359)
    frames = (2, 5, 9, 12)
    reports = {}
    for kind, transaction in (("edit", material_edit), ("merge", merge)):
        label = f"resize-pbr-{kind}"
        plan = {"schemaVersion": 1, "frames": frames[-1], "sampleFrames": frames,
                "camera": [{"frame": 1, "position": [0, 0, 6], "target": [0, 0, 0]}],
                "events": [transaction, {"frame": 8, "resize": RESIZE_ODD}, {"frame": 11, "fsr": "balanced"}]}
        states, _ = capture(label, "quality", plan, extent=start, model_path=initial)
        reports[label] = {}
        for frame in frames:
            state = states[frame]
            mode = "balanced" if frame == 12 else "quality"
            check_mode(state, mode, mode, start if frame <= 5 else RESIZE_ODD)
            auxiliary = next(item for item in state["passes"] if item["name"] == "PbrAuxiliary")
            activated = frame >= 5
            publications = state["gltfMaterialAuthoring"]["publications"]
            merge_state = state["gltfMerge"]["outcome"]
            assert auxiliary["active"] == activated, (label, frame, auxiliary)
            assert publications == int(kind == "edit" and activated), (label, frame, publications)
            assert merge_state == ("committed" if kind == "merge" and activated else "idle"), (label, frame, merge_state)
            reports[label][frame] = {"auxiliaryActive": auxiliary["active"],
                                     "materialPublications": publications, "mergeOutcome": merge_state}
    return reports


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--renderer", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--model", type=Path, default=ROOT / "models/validation/taa_scene.gltf")
    parser.add_argument("--suite", choices=("smoke", "full", "resize"), default="smoke")
    parser.add_argument("--resize-format", choices=SWAPCHAIN_FORMATS, help="Add a supported format-only resize to the resize suite")
    parser.add_argument("--resize-reference", action="store_true", help="Check reference viewport fallback on a ray-tracing GPU")
    fallback = parser.add_mutually_exclusive_group()
    fallback.add_argument("--expect-unavailable", action="store_true",
                          help="Verify the normal renderer when the FSR SDK/runtime is unavailable")
    fallback.add_argument("--expect-fallback", choices=FALLBACK_REASONS, help="Require this exact native fallback cause")
    parser.add_argument("--expect-provider", default="3.1.6", help="Provider version returned by the signed SDK")
    parser.add_argument("--validation-manifest", type=Path, help="Use and identify this Vulkan validation layer")
    args = parser.parse_args()
    unavailable = args.expect_unavailable or args.expect_fallback is not None
    if (args.resize_format or args.resize_reference) and args.suite != "resize":
        parser.error("--resize-format and --resize-reference require --suite resize")
    if unavailable and args.suite == "resize":
        parser.error("--suite resize requires active FSR")
    renderer, output, model = (p.resolve() for p in (args.renderer, args.output, args.model))
    runtime = runtime_receipt(renderer, enabled=not unavailable)
    if args.expect_fallback == "missing-runtime":
        assert any(info is None for info in runtime["signedRuntime"].values()), runtime
    output.mkdir(parents=True, exist_ok=False)
    env = dict(os.environ, VK_LAYER_SETTINGS_PATH=str(output), VK_LOADER_DEBUG="layer")
    validation = validation_environment(args.validation_manifest, output, env) if args.validation_manifest else None
    (output / "vk_layer_settings.txt").write_text(VALIDATION_SETTINGS, encoding="ascii")
    results = {}

    def capture(label, mode, config, *, extent=(640, 360), technique="deferred-raster", display="lit", ui=False,
                model_path=model, ray_queries=False):
        sequence_path = output / f"{label}.sequence.json"
        sequence_path.write_text(json.dumps(config), encoding="utf-8")
        image = output / f"{label}.png"
        command = [str(renderer), "--hidden", "--validation", "--no-taa", "--msaa", "1",
                   "--width", str(extent[0]), "--height", str(extent[1]), "--model", str(model_path),
                   "--import-scale", "1", "--display-mode", display, "--render-technique", technique,
                   "--no-bloom", "--no-ddgi", "--ray-shadows", "raster", "--sky", "plain",
                   "--environment-intensity", ".35", "--directional-intensity", "3", "--exposure", ".5",
                   "--fixed-dt", ".016666667", "--capture-sequence", str(sequence_path), "--screenshot", str(image)]
        if mode is not None:
            command.extend(("--fsr", mode))
        if not ray_queries:
            command.append("--no-ray-query")
        if not ui:
            command.extend(("--no-ui", "--screenshot-viewport"))
        run = subprocess.run(command, cwd=renderer.parent, env=env, capture_output=True, text=True,
                             errors="replace", timeout=240)
        log = run.stdout + run.stderr
        (output / f"{label}.log").write_text(log, encoding="utf-8")
        assert run.returncode == 0 and not validation_log_error(log), (label, log)
        inserted = VALIDATION_INSERTED.search(log)
        assert inserted, f"Vulkan validation layer was not confirmed for {label}"
        layer = file_identity(renderer.parent / Path(inserted[1]))
        if validation:
            assert layer == validation["library"], (label, layer, validation["library"])

        states, images = {}, {}
        for frame in config["sampleFrames"]:
            name = label if frame == config["frames"] else f"{label}.frame-{frame:04d}"
            states[frame] = json.loads((output / f"{name}.telemetry.json").read_text(encoding="utf-8"))
            assert states[frame]["validationEnabled"], (label, frame)
            with Image.open(output / f"{name}.png") as readback:
                assert list(readback.size) == states[frame]["fsr"]["displayExtent"], states[frame]["fsr"]
                images[frame] = np.array(readback.convert("RGB"))
        timed = [p["gpuMs"] for state in states.values() for p in state["passes"]
                 if p["name"] == "TemporalResolve" and p["gpuTimed"]]
        results[label] = {"states": {frame: state["fsr"] for frame, state in states.items()},
                          "temporalStates": {frame: {"taaConfigured": state["taa"]["enabled"],
                                                     "resolveActive": temporal_resolve(state),
                                                     "epoch": state["taa"]["epoch"],
                                                     "jitterUv": state["camera"]["jitterUv"]}
                                             for frame, state in states.items()},
                          "validationLayer": layer,
                          "medianResolveGpuMs": statistics.median(timed) if timed else None,
                          "medianKnownGpuMs": statistics.median(state["gpuKnownMs"] for state in states.values()),
                          "temporalAllocationBytes": states[config["sampleFrames"][-1]]["taa"]["allocatedImageBytes"],
                          "command": command}
        print(f"{label}: {len(images)} Vulkan-validation-clean captures", flush=True)
        return states, images

    def check_mode(state, requested, effective, extent):
        fsr = state["fsr"]
        assert fsr["requested"] == requested and fsr["mode"] == effective, fsr
        assert fsr["active"] == (effective != "native") and fsr["displayExtent"] == list(extent), fsr
        assert fsr["renderExtent"] == [value * 10 // RATIOS[effective] for value in extent], fsr
        if fsr["active"]:
            assert fsr["status"] == f"FSR {args.expect_provider} is ready (SDK {SDK})", fsr
            assert temporal_resolve(state), fsr

    def check_fallback(state):
        assert state["fsr"]["status"], state["fsr"]
        if args.expect_fallback:
            assert state["fsr"]["status"] in FALLBACK_REASONS[args.expect_fallback], state["fsr"]

    def write_report(state):
        assert runtime == runtime_receipt(renderer, enabled=not unavailable), "The installed runtime changed during the captures"
        device = {key: state[key] for key in ("gpu", "vendorId", "deviceId", "driverVersion", "vulkanApiVersion")}
        report = {"passed": True, "gpu": device["gpu"], "device": device, "runtime": runtime,
                  "validationManifest": validation["manifest"] if validation else None,
                  "validationSettings": VALIDATION_SETTINGS.splitlines(),
                  "activeProvider": None if unavailable else args.expect_provider,
                  "expectedFallback": args.expect_fallback or ("unspecified" if unavailable else None), "captures": results}
        (output / "results.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")

    if args.suite == "resize":
        plan, phases = resize_plan(args.resize_format)
        for technique in RASTER_TECHNIQUES:
            label = f"resize-{technique}"
            states, images = capture(label, "native", plan, extent=RESIZE_START, technique=technique)
            phase_samples, quality = verify_resize_matrix(output / f"{label}.png", states, images, phases,
                                                         technique, args.expect_provider, check_mode)
            results[label].update(phases=phase_samples, imageComparisons=quality)
        ui_samples = verify_resize_ui(output, capture, check_mode)
        results["resize-ui-window"]["presentationUi"] = ui_samples
        verify_ineligible_viewports(capture, check_mode, args.resize_reference)
        for label, report in verify_resize_pbr(output, capture, check_mode).items():
            results[label]["auxiliaryLifecycle"] = report
        write_report(states[plan["frames"]])
        return

    size = (640, 360)
    motion = sequence(24, [1, 8, 13, 16, 18, 24], [{"frame": 12, "exposure": .8}, {"frame": 15, "reset": True}])
    motion["camera"] = [motion["camera"][0],
        {"frame": 17, "position": [.2, 1.8, 6.5], "target": [0, .8, 0]},
        {"frame": 18, "position": [2, 2.5, -6], "target": [0, .8, 0]}]
    baseline, native = capture("native", None, motion, extent=size)
    requested, upscaled = capture("quality", "quality", motion, extent=size)
    expected = "native" if unavailable else "quality"
    for frame, state in requested.items():
        check_mode(baseline[frame], "native", "native", size)
        check_mode(state, "quality", expected, size)
    if unavailable:
        for state in requested.values():
            check_fallback(state)
        assert all(np.array_equal(native[frame], upscaled[frame]) for frame in native)
    else:
        compare = {frame: float(np.mean(np.abs(upscaled[frame].astype(float) - native[frame])))
                   for frame in native}
        assert max(compare.values()) < 25, compare
        assert not np.array_equal(upscaled[8], native[8]), "FSR output did not change with reconstruction"
        assert requested[16]["taa"]["epoch"] > requested[13]["taa"]["epoch"], requested[16]["taa"]
        assert requested[18]["taa"]["epoch"] > requested[16]["taa"]["epoch"], requested[18]["taa"]
        assert requested[18]["taa"]["resetReason"] == "camera cut", requested[18]["taa"]
        results["quality"]["nativeMae8Bit"] = compare

        switched_size = (720, 400)
        events = [{"frame": 4, "fsr": "balanced"}, {"frame": 7, "fsr": "performance"},
                  {"frame": 10, "fsr": "native"}, {"frame": 13, "fsr": "quality"},
                  {"frame": 15, "reload": str(model)},
                  {"frame": 16, "resize": list(switched_size)}, {"frame": 19, "samples": 4},
                  {"frame": 22, "samples": 1}]
        lifecycle = sequence(24, [1, 4, 7, 10, 13, 15, 17, 20, 24], events)
        switched, _ = capture("switch-resize-msaa", "quality", lifecycle, extent=size)
        expected_modes = ["quality", "balanced", "performance", "native", "quality", "quality", "quality", "native", "quality"]
        for (frame, state), mode in zip(switched.items(), expected_modes):
            check_mode(state, "quality" if frame == 20 else mode, mode, switched_size if frame > 16 else size)
        epochs = [state["taa"]["epoch"] for state in switched.values()]
        assert all(b > a for a, b in zip(epochs, epochs[1:])), epochs

        if args.suite == "full":
            larger = (1280, 720)
            forward = sequence(24, [8, 16, 24], moving="Glass front")
            _, transparent_native = capture("forward-native", None, forward, extent=larger, technique="forward-raster")
            state, transparent_fsr = capture("forward-performance", "performance", forward,
                                             extent=larger, technique="forward-raster")
            for frame, frame_state in state.items():
                check_mode(frame_state, "performance", "performance", larger)
                assert np.mean(np.abs(transparent_fsr[frame].astype(float) - transparent_native[frame])) < 25
            for name in ("taa-velocity", "taa-reactive"):
                state, diagnostic = capture(name, "quality", forward, display=name)
                check_mode(state[24], "quality", "quality", size)
                assert float(np.std(diagnostic[24])) > 10, name
            ui_state, _ = capture("ui-quality", "quality", motion, ui=True)
            check_mode(ui_state[24], "quality", "quality", size)
            ui_receipt = json.loads((output / "ui-quality.png.capture.json").read_text(encoding="utf-8"))
            assert ui_receipt["includesUi"], ui_receipt

    taa_states, taa_images = capture("taa-fsr-switch", None, taa_switch_sequence(), extent=size)
    for frame, state in taa_states.items():
        quality_requested = frame in (8, 12)
        configured_taa = frame in (4, 8, 20)
        fsr_on = quality_requested and not unavailable
        temporal_on = configured_taa or fsr_on
        check_mode(state, "quality" if quality_requested else "native", "quality" if fsr_on else "native", size)
        if quality_requested and unavailable:
            check_fallback(state)
        assert state["taa"]["enabled"] == configured_taa and temporal_resolve(state) == temporal_on, state["taa"]
        assert any(state["camera"]["jitterUv"][:2]) == temporal_on, state["camera"]
    if not unavailable:
        assert taa_states[4]["taa"]["epoch"] < taa_states[8]["taa"]["epoch"] == taa_states[12]["taa"]["epoch"]
        assert taa_states[12]["taa"]["epoch"] < taa_states[16]["taa"]["epoch"]
        assert taa_states[8]["taa"]["allocatedImageBytes"] == taa_states[12]["taa"]["allocatedImageBytes"]
    assert not np.array_equal(taa_images[16], taa_images[20]), "Native TAA did not change the reconstruction"

    write_report(requested[24])


if __name__ == "__main__":
    main()
