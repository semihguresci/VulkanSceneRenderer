"""Validation-enabled DDGI correctness cases for the opt-in device matrix."""

import argparse
import json
import math
import os
from pathlib import Path
import re
import subprocess
import sys
from types import SimpleNamespace

import numpy as np
from PIL import Image

import run_glass
import run_runtime as runtime

sys.path.insert(0, str(runtime.REPO_ROOT / "tests" / "validation"))
import ddgi_queue_runtime_regression as queues
from msaa_attachment_capabilities_runtime_regression import VALIDATION, verify_layers


CORNELL = runtime.REPO_ROOT / "models/validation/cornell_box_local_light.gltf"
LAYER_MANIFEST = "VkLayer_khronos_validation.json"
LAYER_SETTINGS = "khronos_validation.validate_sync = true\n"
GLASS_CASES = ("open-control", "glass-pane", "thick-unsupported")
require = runtime.require


def read_json(path):
    return json.loads(path.read_bytes())


def validation_layer(directory):
    manifest = directory / LAYER_MANIFEST
    declaration = read_json(manifest)["layer"]
    require(declaration["name"] == VALIDATION, f"Unexpected layer in {manifest}")
    declared = declaration["library_path"]
    candidate = directory / declared
    library = candidate.resolve() if candidate.is_file() else None
    if library is None and (not sys.platform.startswith("linux") or Path(declared).name != declared):
        raise FileNotFoundError(f"Validation library does not exist: {candidate}")
    identity = {"apiVersion": declaration["api_version"], "manifestSha256": queues.sha(manifest),
                "librarySha256": queues.sha(library) if library else None, "declaredLibrary": declared}

    def check_layers(log, _library=None, _profiles=None):
        if library is not None:
            verify_layers(log, library, None)
            return
        # Installed Linux manifests commonly delegate lookup of a bare soname
        # to the dynamic linker. Check both loader insertion events and retain
        # the binary hash when the loader reveals its location.
        for kind in ("Insert(?:ed)? instance", "Insert(?:ed)? device"):
            match = re.search(rf'{kind} layer "{VALIDATION}" \(([^\r\n]+)\)', log)
            require(match and Path(match[1]).name == declared, "The declared Linux validation layer was not inserted")
            observed = Path(match[1])
            if observed.is_absolute() and observed.is_file():
                digest = queues.sha(observed)
                require(identity["librarySha256"] in (None, digest), "The validation library changed between captures")
                identity["librarySha256"] = digest
        require(not re.search(r'Insert(?:ed)? instance layer "VK_LAYER_KHRONOS_profiles"', log),
                "Vulkan Profiles emulation was active in the native device run")

    return library, identity, check_layers


def check_available_probe(_case, frames):
    for sample in frames:
        gi = sample["ddgi"]
        require(gi["enabled"] is True, "The capture did not request DDGI")
        require(type(gi["supported"]) is bool and gi["active"] is gi["supported"],
                f"Unexpected DDGI start state: {gi['status']}")
        if gi["supported"]:
            require(gi["updatedProbes"] > 0 and gi["tracedRays"] > 0,
                    "The supported device did not execute DDGI")
        else:
            require(gi["status"] and gi["updatedProbes"] == gi["tracedRays"] == gi["updateFrame"] == 0,
                    "Capability fallback must report a reason and execute zero probes")


def check_route(gi, preference):
    route = gi["queueRoute"]
    allowed = {"auto": set(queues.ROUTES), "same-family": {"same-family", "serialized"},
               "serialized": {"serialized"}}
    require(gi["settings"]["asyncCompute"] and route in allowed[preference],
            f"Invalid DDGI queue route {route} for {preference}")
    family = gi.get("asyncQueueFamily")
    submitted, published = gi["asyncSubmittedUpdates"], gi["asyncPublishedUpdates"]
    if route == "serialized":
        require(not gi["asyncActive"] and family is None and submitted == published == 0,
                "Serialized DDGI submitted asynchronous work")
    else:
        require(gi["active"] and gi["asyncActive"] and family is not None and
                (family == gi["graphicsQueueFamily"]) == (route == "same-family") and
                0 < published <= submitted <= published + 1,
                "DDGI asynchronous queue, submission or publication is inconsistent")
    return {"requested": preference, "actual": route, "status": gi["status"],
            "graphicsFamily": gi["graphicsQueueFamily"], "asyncFamily": family,
            "asyncSubmittedUpdates": submitted, "asyncPublishedUpdates": published}


def check_fallback(state, *, forced_off):
    gi = state["ddgi"]
    require(gi["enabled"] and not gi["supported"] and not gi["active"] and gi["status"],
            "The unavailable DDGI request did not produce a raster fallback reason")
    require(not any(gi[key] for key in ("updatedProbes", "tracedRays", "updateFrame",
                    "asyncSubmittedUpdates", "asyncPublishedUpdates")),
            "Unavailable DDGI did work")
    if forced_off:
        require(not state["rayShadows"]["supported"] and not state["rayShadows"]["active"] and
                "ray quer" in gi["status"].lower().replace("-", " "),
                "Force-disabled ray queries did not report the raster fallback")
    return gi["status"]


def read_telemetry(report, image, telemetry, technique, revision):
    report["captures"].append(str(telemetry))
    state = read_json(telemetry)
    sidecar = image.with_suffix(image.suffix + ".capture.json")
    metadata = read_json(sidecar)
    require(state["build"]["revision"] == revision and state["validationEnabled"] is True and
            state["technique"] == technique, f"Wrong build, validation or technique in {telemetry}")
    require(metadata["build"]["revision"] == revision and metadata["width"] == state["resolution"][0] and
            metadata["height"] == state["resolution"][1] and not metadata["includesUi"],
            f"Capture metadata differs from rendered state in {image}")
    for artifact in (image, telemetry, sidecar):
        report["captureSha256"][str(artifact)] = queues.sha(artifact)
    return state


def rgb(path):
    with Image.open(path) as image:
        return np.asarray(image.convert("RGB"), dtype=np.int16)


def runtime_case(case, renderer, output, environment, timeout, revision, report, check_layers, *,
                 frame_check=runtime.check_frames):
    result = runtime.capture(case, renderer, output, environment, timeout, frame_check=frame_check)
    report["cases"].append(result)
    log = Path(result["log"])
    try:
        states = [read_telemetry(report, image, telemetry, case.technique, revision)
                  for _, image, telemetry in runtime.capture_paths(case, output)
                  if image.is_file() and telemetry.is_file()]
        require(result["passed"], "; ".join(result["failures"]))
        check_layers(log.read_text(encoding="utf-8"))
        require(int(np.ptp(rgb(output / f"{case.name}.png"))) > 8,
                "Expected a rendered scene, got a flat image")
    except (AssertionError, KeyError, OSError, ValueError) as error:
        result["passed"] = False
        result["failure"] = str(error)
        raise RuntimeError(f"{case.name}: {error}. See {log}") from error
    return result, states[-1]


def run_correctness(renderer, output, revision, validation_dir, *, timeout=240, environment=None):
    """Run real GPU raster cases. The output directory must be fresh.

    Only a validated native capture with DDGI unavailable permits skipping its
    dependent cases. Queue availability is also determined from real captures.
    Loader insertion must confirm the validation layer, including when Linux
    resolves a bare library name. Validation-run timestamps are optional and
    separate from Release timing evidence.
    """
    renderer = Path(renderer).resolve(strict=True)
    output = Path(output).resolve()
    validation_dir = Path(validation_dir).resolve(strict=True)
    if validation_dir.is_file():
        if validation_dir.name != LAYER_MANIFEST:
            raise ValueError(f"Expected {LAYER_MANIFEST}")
        validation_dir = validation_dir.parent
    if not renderer.is_file() or not revision or not math.isfinite(timeout) or timeout <= 0:
        raise ValueError("Expected an executable, revision and finite positive timeout")
    library, identity, check_layers = validation_layer(validation_dir)
    output.mkdir(parents=True, exist_ok=False)
    report_path = output / "correctness-results.json"
    report = {"passed": False, "revision": revision, "rendererSha256": queues.sha(renderer),
              "validationLayer": identity, "synchronizationValidation": True,
              "captures": [], "captureSha256": {}, "cases": [], "unsupported": [],
              "queueProbes": [], "runtimeShaderSha256": run_glass.runtime_shader_hashes(renderer),
              "modelSha256": {str(CORNELL): queues.sha(CORNELL)}}
    env = dict(os.environ)
    env.update(environment or {})
    env.pop("VK_LOADER_LAYERS_ENABLE", None)
    env.pop("VK_LAYER_DISABLES", None)
    env.update(VK_LAYER_PATH=str(validation_dir), VK_LAYER_SETTINGS_PATH=str(output),
               VK_INSTANCE_LAYERS=VALIDATION, VK_LOADER_DEBUG="layer", CONTAINER_DDGI_ASYNC_QUEUE="serialized")
    (output / "vk_layer_settings.txt").write_text(LAYER_SETTINGS, encoding="utf-8")

    def run_raster(case, *, preference="serialized", frame_check=runtime.check_frames):
        _, state = runtime_case(case, renderer, output, dict(env, CONTAINER_DDGI_ASYNC_QUEUE=preference),
                                timeout, revision, report, check_layers, frame_check=frame_check)
        runtime.write_json(report_path, report)
        return state

    expected = []
    try:
        for technique in queues.TECHNIQUES:
            state = run_raster(runtime.Case(f"forced-no-ray-query-{technique}", technique,
                ("--no-ray-query", "--ray-shadows", "hard", "--ddgi-async"), frame_count=4, kind="unsupported"))
            check_route(state["ddgi"], "serialized")
            report["cases"][-1]["fallbackReason"] = check_fallback(state, forced_off=True)

        state = run_raster(runtime.Case("native-availability", extra=("--ddgi-async",), frame_count=16),
                           preference="auto", frame_check=check_available_probe)
        probe = check_route(state["ddgi"], "auto")
        report["queueProbes"].append(probe)
        report["nativeDdgiSupported"] = state["ddgi"]["supported"]
        if state["ddgi"]["supported"]:
            expected = [("serialized", "serialized"), ("auto", probe["actual"])]
            if probe["actual"] != "same-family":
                same_family = run_raster(runtime.Case("same-family-availability", extra=("--ddgi-async",), frame_count=16),
                                         preference="same-family", frame_check=check_available_probe)
                require(same_family["ddgi"]["supported"], "The device lost DDGI support between queue probes")
                actual = check_route(same_family["ddgi"], "same-family")
                report["queueProbes"].append(actual)
                if actual["actual"] == "same-family":
                    expected.append(("same-family", "same-family"))
            available = {route for _, route in expected}
            for route in ("dedicated", "same-family"):
                if route not in available:
                    report["unsupported"].append({"case": "DDGI queue lifecycle", "route": route,
                                                   "reason": "Not selected by the requested queue probes"})
        else:
            reason = check_fallback(state, forced_off=False)
            report["unsupported"].append({"case": "active DDGI runtime, overlay and glass lanes", "reason": reason})

        if expected:
            run_native_cases(renderer, output, revision, validation_dir, timeout, env, report, library,
                             expected, run_raster, check_layers)
        report["passed"] = True
    except (AssertionError, OSError, RuntimeError, ValueError, KeyError, TypeError, subprocess.SubprocessError) as error:
        report["failure"] = str(error)
    finally:
        runtime.write_json(report_path, report)
    return report


def run_native_cases(renderer, output, revision, validation_dir, timeout, env, report, library,
                     routes, run_raster, check_layers):
    inputs = runtime.write_inputs(output)
    for case in runtime.make_cases(inputs, include_memory=False):
        if case.kind in {"dynamic", "surface-raster", "surface-hard"} or case.name == "cornell-one-probe-budget":
            if case.scene:
                report["modelSha256"][str(case.scene)] = queues.sha(case.scene)
            run_raster(case)

    report["probeOverlays"] = []
    for technique in queues.TECHNIQUES:
        prefix = technique.split("-")[0]
        off, on = (f"debug-{prefix}-{view}" for view in ("off", "irradiance"))
        for case in runtime.debug_cases():
            if case.name in (off, on):
                state = run_raster(case)
                require((state["ddgi"]["debugView"] != 0) == (case.name == on), "Incorrect probe overlay mode")
        baseline, overlay = (rgb(output / f"{name}.png") for name in (off, on))
        changed = int(np.count_nonzero(np.max(np.abs(overlay - baseline), axis=2) > 8))
        require(changed > 20, f"{technique} irradiance overlay did not change the image")
        report["probeOverlays"].append({"technique": technique, "changedPixels": changed})

    queue_output = output / "queue-lifecycle"
    queue_output.mkdir()
    (queue_output / "vk_layer_settings.txt").write_text(LAYER_SETTINGS, encoding="utf-8")
    original, replacement, texture, sequence = queues.make_inputs(queue_output)
    report["queueInputSha256"] = {str(path): queues.sha(path) for path in (original, replacement, texture, sequence)}
    args = SimpleNamespace(renderer=renderer, revision=revision, validation_dir=validation_dir,
                           timeout=timeout, require_async_timestamps=False)
    report["queueLifecycle"] = []
    for preference, route in routes:
        for technique in queues.TECHNIQUES:
            name = f"{preference}-{technique}"
            entry = queues.run_case(args, queue_output, route, technique, (original, replacement), sequence, library,
                                    environment=env, preference=preference, name=name,
                                    require_serialized_timestamps=False, check_layers=check_layers)
            for frame in queues.SAMPLES.values():
                stem = "capture" if frame == max(queues.SAMPLES.values()) else f"capture.frame-{frame:04d}"
                image = queue_output / name / f"{stem}.exr"
                read_telemetry(report, image, image.with_suffix(".telemetry.json"), technique, revision)
            report["queueLifecycle"].append(dict(entry, preference=preference))

    glass_output = output / "thin-glass"
    scenes = run_glass.write_glass_fixtures(glass_output)["fixtures"]
    records = []
    for scene in scenes:
        if scene["name"] not in GLASS_CASES:
            continue
        report["modelSha256"][str(glass_output / scene["gltf"])] = scene["gltfSha256"]
        for thin in ((False, True) if scene["name"] == "glass-pane" else (True,)):
            for intensity in (False, True):
                technique = "deferred-raster"
                entry = run_glass.capture(renderer, glass_output, env, scene, run_glass.FRAMES, timeout,
                                          technique=technique, thin_glass=thin, primary_indirect=intensity)
                stem = glass_output / entry["name"]
                check_layers(stem.with_suffix(".log").read_text(encoding="utf-8"))
                read_telemetry(report, stem.with_suffix(".png"), stem.with_suffix(".telemetry.json"), technique, revision)
                records.append(entry)
                report["cases"].append(entry)
    report["thinGlass"] = run_glass.compare_glass(records, ("deferred-raster",), GLASS_CASES)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--renderer", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True, help="fresh evidence directory")
    parser.add_argument("--revision", required=True)
    parser.add_argument("--validation-dir", type=Path, required=True, help="Khronos validation layer directory or manifest")
    parser.add_argument("--timeout", type=float, default=240, help="maximum seconds per renderer invocation")
    args = parser.parse_args()
    result = run_correctness(args.renderer, args.output, args.revision, args.validation_dir, timeout=args.timeout)
    if not result["passed"]:
        print(result["failure"], file=sys.stderr)
    return 0 if result["passed"] else 1


if __name__ == "__main__":
    sys.exit(main())
