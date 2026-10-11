"""Exercise Scene Controls model requests with an active ImGui frame lifecycle.

Uses the same sample-selection and load-request methods as the widgets; it does
not simulate OS mouse clicks. Real buildingSMART assets are staged by CMake.
"""
import argparse
import hashlib
import json
import math
import os
from pathlib import Path
import re
import subprocess

try:
    import numpy as np
    from PIL import Image
except ImportError:
    print("SKIP: GUI model regression requires Pillow and NumPy")
    raise SystemExit(77)


def legacy_main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--exe", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args(argv)
    if os.environ.get("CONTAINER_RUN_GPU_GUI_MODEL_LOADING") != "1":
        print("SKIP: set CONTAINER_RUN_GPU_GUI_MODEL_LOADING=1")
        return 77
    exe, output = args.exe.resolve(), args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    models = exe.parent / "models"
    examples = models / "buildingSMART-IFC5-development/examples"
    wall = examples / "Hello Wall/hello-wall.ifc"
    tekla = examples / "Tekla House/TeklaHouse.ifcx"
    if not wall.is_file() or not tekla.is_file():
        print("SKIP: buildingSMART Hello Wall and Tekla House assets unavailable")
        return 77
    cornell = models / "validation/cornell_box_local_light.gltf"
    missing = output / "missing.ifcx"
    malformed = output / "malformed.ifcx"
    malformed.write_text("{broken JSON", encoding="utf-8")
    (output / "vk_layer_settings.txt").write_text(
        "khronos_validation.validate_sync = true\n", encoding="utf-8")
    # Collapse ordinary panels to keep the model visible. Rendering/UI remain on.
    (output / "imgui.ini").write_text(
        "[Window][Scene Controls]\nPos=0,0\nSize=350,700\nCollapsed=1\n\n"
        "[Window][Renderer Telemetry]\nPos=0,25\nSize=350,700\nCollapsed=1\n",
        encoding="utf-8")

    def capture(name, technique, source, sequence, gui=True):
        script = output / (name + ".json")
        script.write_text(json.dumps(sequence), encoding="utf-8")
        screenshot = output / (name + ".png")
        # Do not allow stale samples to satisfy the assertions after a crash.
        for stale in [screenshot, *output.glob(name + ".frame-*.png")]:
            stale.unlink(missing_ok=True)
        command = [str(exe), "--model", str(source), "--hidden", "--validation",
                   "--no-ray-query", "--no-taa", "--msaa", "1", "--width", "960",
                   "--height", "540", "--display-mode", "lit", "--exposure", ".25",
                   "--fixed-dt", "0.016666667", "--render-technique", technique,
                   "--capture-sequence", str(script), "--screenshot", str(screenshot)]
        if not gui:
            command.append("--no-ui")
        logfile = output / (name + ".log")
        with logfile.open("w", encoding="utf-8") as log:
            result = subprocess.run(command, cwd=output, stdout=log,
                                    stderr=subprocess.STDOUT, timeout=240,
                                    env=dict(os.environ, VK_LAYER_SETTINGS_PATH=str(output)))
        logtext = logfile.read_text(encoding="utf-8", errors="replace")
        assert result.returncode == 0, f"App failed: {logfile} ({result.returncode})"
        assert "VUID-" not in logtext and "SYNC-HAZARD" not in logtext, logfile
        files = sorted(output.glob(name + ".frame-*.png")) + [screenshot]
        return [(path, json.loads(path.with_suffix(".telemetry.json").read_text()))
                for path in files]

    sample_frames = [8, 20, 32, 44, 56, 68, 80, 92, 104]
    sequence = {"schemaVersion": 1, "frames": 104, "sampleFrames": sample_frames,
                "events": [
                    {"frame": 12, "guiSampleModel": "IFC5 / Hello Wall / hello-wall (STEP)"},
                    {"frame": 24, "guiSampleModel": "IFC5 / Hello Wall / hello-wall"},
                    {"frame": 36, "guiSampleModel": "IFC5 / Hello Wall / hello-wall-add-fire-rating-60"},
                    {"frame": 48, "guiReload": str(missing)},
                    {"frame": 60, "guiReload": str(malformed)},
                    {"frame": 72, "guiSampleModel": "IFC5 / Tekla House / TeklaHouse"},
                    {"frame": 84, "guiSampleModel": "IFC5 / Tekla House / TeklaHouse (STEP)"},
                    {"frame": 96, "guiReload": str(cornell)}]}
    results = {}
    for technique in ["deferred-raster", "forward-raster"]:
        prefix = technique.split("-")[0]
        samples = capture(prefix + "-ui", technique, cornell, sequence)
        assert len(samples) == len(sample_frames), "Missing GUI load captures"
        check_model_request_progress(
            {frame: sample[1] for frame, sample in zip(sample_frames, samples)},
            {frame: index for index, frame in enumerate(sample_frames)},
            successful_frames=(20, 32, 44, 80, 92, 104))
        for index in [0, 8]:
            lighting = samples[index][1]["lighting"]
            assert lighting["directionalIntensity"] == lighting["environmentIntensity"] == 0
            assert lighting["areaLightCount"] == 1
        for index in range(1, 8):
            telemetry = samples[index][1]
            assert telemetry["gui"]["displayMode"] == 0, "UI ignored --display-mode lit"
            assert telemetry["scene"]["auxiliaryObjectCount"] > 0, "BIM scene absent"
            assert telemetry["scene"]["primary"] == "", "Old glTF scene retained"
            lighting = telemetry["lighting"]
            assert lighting["directionalIntensity"] == 2, (prefix, sample_frames[index], lighting)
            assert lighting["environmentIntensity"] == 1, (prefix, sample_frames[index], lighting)
            assert lighting["areaLightCount"] == lighting["pointLightCount"] == 0
        for index, expected in [(1, wall), (2, wall.with_suffix(".ifcx")),
                                (3, examples / "Hello Wall/hello-wall-add-fire-rating-60.ifcx"),
                                (6, tekla), (7, tekla.with_suffix(".ifc"))]:
            assert Path(samples[index][1]["scene"]["auxiliary"]) == expected
            assert samples[index][1]["gui"]["status"].startswith("Loaded model:")
        for index in [4, 5]:
            status = samples[index][1]["gui"]["status"]
            assert status.startswith("Failed to load model:") and "\n" in status, status
            assert samples[index][1]["scene"]["auxiliary"] == samples[3][1]["scene"]["auxiliary"]
            assert samples[index][1]["scene"]["auxiliaryObjectCount"] == samples[3][1]["scene"]["auxiliaryObjectCount"]
        assert "failed to open IFCX file" in samples[4][1]["gui"]["status"]
        assert "parse error" in samples[5][1]["gui"]["status"]
        assert samples[1][1]["scene"]["import"]["importedProducts"] == 4
        assert samples[7][1]["scene"]["import"]["importedProducts"] == 10042
        # Compare illumination and framing against direct startup without UI.
        reference = capture(prefix + "-direct", technique, wall,
                            {"schemaVersion": 1, "frames": 20}, gui=False)[0]
        assert samples[1][1]["camera"]["position"] == reference[1]["camera"]["position"]
        camera = samples[1][1]["camera"]["position"]
        assert camera[0] > 10 or camera[1] > 3.1 or camera[2] < -5.1 or camera[2] > .1, (
            "Hello Wall starts inside its bounds", camera)
        def pixels(path):
            with Image.open(path) as image:
                # Exclude the left tool palette, top/right navigation HUD and
                # collapsed panels while retaining the center of the model.
                return np.asarray(image.convert("RGB"), dtype=np.float32)[70:440, 370:755] / 255
        mae = float(np.abs(pixels(samples[1][0]) - pixels(reference[0])).mean())
        assert mae < .003, f"UI reload differs from direct IFC startup: {mae}"
        recovery_mae = max(float(np.abs(pixels(samples[index][0]) -
                                        pixels(samples[3][0])).mean()) for index in [4, 5])
        assert recovery_mae < .001, f"Failed load changed the restored scene: {recovery_mae}"
        classic = models / "buildingSMART-Sample-Test-Files/IFC 4.0.2.1 (IFC 4)/ISO Spec archive/basin-tessellation.ifc"
        assert classic.is_file() and not classic.read_bytes().startswith(b"version https://git-lfs"), (
            "Classic IFC asset is missing or still an LFS pointer", classic)
        classic_sample = capture(prefix + "-classic", technique, cornell,
                                 {"schemaVersion": 1, "frames": 20, "events": [{"frame": 12,
                                  "guiSampleModel": "IFC / IFC 4.0.2.1 (IFC 4) / ISO Spec archive / basin-tessellation (STEP)"}]})[0][1]
        assert Path(classic_sample["scene"]["auxiliary"]) == classic
        assert classic_sample["scene"]["auxiliaryObjectCount"] > 0
        assert classic_sample["gui"]["status"].startswith("Loaded model:")
        assert classic_sample["lighting"]["directionalIntensity"] == 2
        assert classic_sample["lighting"]["environmentIntensity"] == 1
        results[prefix] = {"captures": len(samples), "ifcStartupVsUiRgbMae": mae,
                           "failedLoadRecoveryRgbMae": recovery_mae,
                           "nativeIfcProducts": [4, 10042], "failedLoadsRestoreScene": True,
                           "detailedErrorsPreserved": True, "validationClean": True,
                           "modelPreparationAndCommitResponsive": True}
        results[prefix]["classicIfcSampleLoaded"] = True
        print(f"{prefix}: IFC/IFCX sample selection, layers, reload recovery and lighting passed",
              flush=True)
    (output / "results.json").write_text(json.dumps(results, indent=2) + "\n",
                                         encoding="utf-8")
    return 0


TECHNIQUES = ("deferred-raster", "forward-raster")
LIFECYCLE_MODES = ("raster", "rays-taa", "ddgi-async")
VALIDATION_ERROR = re.compile(
    r"VUID-|SYNC-HAZARD|Validation Error|VK_ERROR_[A-Z_]+|"
    r"Unhandled exception|uncaught exception|Assertion failed|"
    r"Access violation|terminate called|\bfatal\b", re.IGNORECASE)


def require(condition, message):
    if not condition:
        raise AssertionError(message)


def lifecycle_sequence(cornell, wall, bim, missing_gltf, malformed_gltf,
                       missing_bim, malformed_bim, malformed_dot_bim, mode):
    require(mode in LIFECYCLE_MODES, "Unknown model lifecycle mode")
    events = [
        {"frame": 12, "guiReload": str(wall)},
        {"frame": 24, "guiReload": str(wall)},
        {"frame": 36, "guiReload": str(missing_gltf)},
        {"frame": 44, "guiReload": str(malformed_gltf)},
        {"frame": 52, "guiReload": str(missing_bim)},
        {"frame": 56, "guiReload": str(malformed_dot_bim)},
        {"frame": 60, "guiReload": str(malformed_bim)},
        {"frame": 68, "guiUnload": True},
        {"frame": 76, "guiUnload": True},
        {"frame": 84, "guiReload": str(cornell)},
        {"frame": 96, "guiReload": str(cornell)},
        {"frame": 108, "guiReload": str(malformed_gltf)},
        {"frame": 116, "guiReload": str(missing_bim)},
        {"frame": 120, "guiReload": str(malformed_dot_bim)},
        {"frame": 124, "guiReload": str(bim)},
        {"frame": 136, "guiUnload": True},
    ]
    if mode == "ddgi-async":
        for event in events:
            if event["frame"] in (12, 24, 84, 96, 124):
                event["ddgiFitProbes"] = 64
    # Capture failed requests at their exact event frame. A continuously applied
    # custom camera would conceal a failed-load camera reset on the NEXT frame.
    # Immediate samples expose that reset before the track is re-applied.
    return {"schemaVersion": 1, "frames": 144,
        "sampleFrames": [8, 20, 32, 36, 44, 52, 56, 60, 72, 80, 92, 104, 108, 116, 120, 132, 144],
        "camera": [{"frame": 1, "position": [4, 3, 8], "target": [0, 1, 0]},
                   {"frame": 144, "position": [4, 3, 8], "target": [0, 1, 0]}],
        "events": events}


def asynchronous_cancel_sequence(cornell, large_bim):
    # Sample the actual request frame: even a fast importer must leave that
    # first frame interactive. Cancel on the very next frame before a ready
    # result can be committed, then queue a new load behind any old CPU work.
    return {"schemaVersion": 1, "frames": 20, "sampleFrames": [3, 4, 5, 10, 20],
            "events": [{"frame": 4, "guiReload": str(large_bim), "guiWaitForLoad": False},
                       {"frame": 5, "guiUnload": True},
                       {"frame": 12, "guiReload": str(cornell)}]}


def capture_paths(output, name, sequence):
    frames = sequence.get("sampleFrames", [sequence["frames"]])
    require(frames and len(frames) == len(set(frames)), "Capture samples must be nonempty and unique")
    require(frames == sorted(frames), "Capture samples must be ordered")
    require(all(isinstance(frame, int) and not isinstance(frame, bool) and
                1 <= frame <= sequence["frames"] for frame in frames), "Capture sample frame is invalid")
    return [(frame, Path(output) / (f"{name}.png" if frame == sequence["frames"]
                                   else f"{name}.frame-{frame:04d}.png")) for frame in frames]


def lifecycle_command(exe, output, name, source, sequence_path, technique, mode, extra=()):
    require(technique in TECHNIQUES, "Unknown model lifecycle technique")
    require(mode in LIFECYCLE_MODES, "Unknown model lifecycle mode")
    command = [str(exe), "--model", str(source), "--hidden", "--validation",
        "--taa" if mode != "raster" else "--no-taa", "--msaa", "1", "--width", "960",
        "--height", "540", "--display-mode", "lit", "--exposure", ".25", "--no-bloom",
        "--taa-jitter-seed", "0", "--fixed-dt", "0.016666667", "--render-technique", technique,
        "--capture-sequence", str(sequence_path), "--screenshot", str(Path(output) / f"{name}.png")]
    if mode == "raster":
        command += ["--no-ray-query", "--no-ddgi"]
    else:
        command += ["--ray-shadows", "hard"]
        if mode == "rays-taa":
            command += ["--no-ddgi"]
        else:
            command += ["--ddgi", "--ddgi-async", "--ddgi-fit-scene", "--ddgi-fit-probes", "64",
                        "--ddgi-rays", "32", "--ddgi-probes-per-frame", "64", "--ddgi-ray-budget", "2048",
                        "--ddgi-hysteresis", "0"]
    return command + list(extra)


def finite_json(value):
    if isinstance(value, dict):
        return all(finite_json(item) for item in value.values())
    if isinstance(value, list):
        return all(finite_json(item) for item in value)
    return not isinstance(value, float) or math.isfinite(value)


def run_lifecycle_capture(exe, output, name, source, sequence, technique, mode, *, extra=(), timeout=240):
    output = Path(output)
    output.mkdir(parents=True, exist_ok=True)
    script = output / f"{name}.json"
    script.write_text(json.dumps(sequence), encoding="utf-8")
    expected = capture_paths(output, name, sequence)
    # Remove both image AND telemetry artifacts. An exit-zero process that
    # produces nothing must fail rather than inherit a previous successful run.
    for _, png in expected:
        png.unlink(missing_ok=True)
        png.with_suffix(".telemetry.json").unlink(missing_ok=True)
    command = lifecycle_command(exe, output, name, source, script, technique, mode, extra)
    logfile = output / f"{name}.log"
    with logfile.open("w", encoding="utf-8") as log:
        result = subprocess.run(command, cwd=output, stdout=log, stderr=subprocess.STDOUT,
            timeout=timeout, env=dict(os.environ, VK_LAYER_SETTINGS_PATH=str(output)))
    logtext = logfile.read_text(encoding="utf-8", errors="replace")
    require(result.returncode == 0, f"Model lifecycle process failed: {logfile} ({result.returncode})")
    errors = [line for line in logtext.splitlines() if VALIDATION_ERROR.search(line)]
    require(not errors, f"Validation/runtime errors in {logfile}: {errors[:3]}")
    samples = []
    for frame, png in expected:
        require(png.is_file(), f"Missing model lifecycle image at frame {frame}: {png}")
        telemetry = json.loads(png.with_suffix(".telemetry.json").read_text(encoding="utf-8"))
        require(finite_json(telemetry), f"Nonfinite telemetry at frame {frame}")
        require(telemetry["schemaVersion"] == 1 and telemetry["technique"] == technique,
                f"Wrong telemetry schema/technique at frame {frame}")
        with Image.open(png) as image:
            image.load()
            require(image.size == (960, 540), f"Wrong image size at frame {frame}")
        samples.append({"frame": frame, "png": str(png), "telemetry": telemetry})
    return {"name": name, "mode": mode, "technique": technique, "command": command,
            "validationErrors": errors, "samples": samples}


def scene_signature(telemetry):
    scene = telemetry["scene"]
    return {key: scene[key] for key in (
        "primary", "auxiliary", "primaryImportScale", "auxiliaryImportScale",
        "primaryObjectCount", "primaryIndexCount", "primaryVertexCount", "primaryNodeCount",
        "auxiliaryObjectCount", "geometryArenaCapacityBytes")}


def stable_camera(telemetry):
    return {key: telemetry["camera"][key] for key in ("position", "unjitteredViewProjColumns")}


def stable_lighting(telemetry):
    return {key: telemetry["lighting"][key] for key in (
        "directionalIntensity", "environmentIntensity", "bounceIntensity",
        "pointLightCount", "areaLightCount", "directionalColor", "directionalDirection")}


def stable_reload_state(telemetry):
    # Publication counters and scheduling cursors advance during normal GI
    # rendering. Scene identity, settings and reset epochs must not change merely
    # because a GUI request was rejected before replacing any scene resources.
    return {
        "taaEpoch": telemetry["taa"]["epoch"],
        "rayGeneration": telemetry["rayShadows"]["generation"],
        "selection": {key: telemetry["gui"][key] for key in
                      ("selectedMeshNodeId", "selectedBimObjectIndex")},
        "ddgi": {key: telemetry["ddgi"][key] for key in
                 ("enabled", "paused", "historyResets", "contentEpoch", "settings")},
    }


def check_loaded(telemetry, path, *, auxiliary, require_gui=True):
    scene = telemetry["scene"]
    provider = "auxiliary" if auxiliary else "primary"
    require(Path(scene[provider]) == Path(path), f"Wrong active {provider} model: {scene[provider]}")
    require(scene["primary" if auxiliary else "auxiliary"] == "", "Old model provider survived replacement")
    require(scene[provider + "ObjectCount"] > 0, f"Loaded {provider} has no renderable objects")
    require(scene["primaryObjectCount"] == 0 if auxiliary else scene["auxiliaryObjectCount"] == 0,
            "Old provider geometry survived replacement")
    if auxiliary:
        require(scene["primaryIndexCount"] == scene["primaryVertexCount"] == scene["primaryNodeCount"] == 0,
                "BIM replacement retained primary geometry/hierarchy")
    else:
        require(scene["primaryIndexCount"] > 0 and scene["primaryVertexCount"] > 0 and
                scene["primaryNodeCount"] > 0, "glTF load left geometry/hierarchy empty")
    if require_gui:
        require(telemetry["gui"]["status"].startswith("Loaded model:"), "GUI load did not finish successfully")
        require(telemetry["gui"]["selectedMeshNodeId"] == -1 and
                telemetry["gui"]["selectedBimObjectIndex"] == -1, "Replacement retained stale selection")


def check_unloaded(telemetry):
    scene = telemetry["scene"]
    require(scene["primary"] == scene["auxiliary"] == "", "Unload retained a model path")
    for key in ("primaryObjectCount", "primaryIndexCount", "primaryVertexCount", "primaryNodeCount",
                "auxiliaryObjectCount"):
        require(scene[key] == 0, f"Unload retained {key}: {scene[key]}")
    require(telemetry["gui"]["status"] == "Unloaded models.", "GUI unload did not report success")
    require(telemetry["gui"]["selectedMeshNodeId"] == -1 and
            telemetry["gui"]["selectedBimObjectIndex"] == -1, "Unload retained stale selection")
    require(not telemetry["rayShadows"]["active"], "Unload retained active ray shadows")
    if "build" in telemetry["rayShadows"]:
        require(telemetry["rayShadows"]["build"]["instances"] == 0, "Unload retained acceleration instances")
    require(not telemetry["ddgi"]["active"] and telemetry["ddgi"]["updatedProbes"] == 0,
            "Unload continued scene GI updates")


def check_lifecycle_mode(telemetry, mode, loaded):
    require(telemetry["taa"]["enabled"] is (mode != "raster"), "TAA mode did not survive a model request")
    require(telemetry["gui"]["displayMode"] == 0, "GUI changed requested lit display mode")
    ray, gi = telemetry["rayShadows"], telemetry["ddgi"]
    if mode == "raster":
        require(ray["supported"] is False and ray["active"] is False and gi["active"] is False,
                "Explicit ray-query fallback did not remain inactive")
    elif loaded:
        require(ray["supported"] and ray["active"] and ray["mode"] == 1,
                "Loaded scene lost hard ray shadows")
        if mode == "ddgi-async":
            require(gi["supported"] and gi["enabled"] and gi["active"], "Loaded scene lost DDGI")
            require(gi["settings"]["asyncCompute"] is True, "Async request did not survive scene replacement")
            require(gi["updateFrame"] > 0, "Reloaded scene never submitted GI updates")
            require(gi["asyncPublishedUpdates"] <= gi["asyncSubmittedUpdates"],
                    "GI published an unsubmitted async update")
            if gi["asyncActive"]:
                require(gi["asyncSubmittedUpdates"] > 0 and gi["asyncPublishedUpdates"] > 0,
                        "Async-capable reload never published new probe history")
            else:
                require("fallback" in gi["status"].lower(), "Serialized async fallback is not reported")
        else:
            require(gi["enabled"] is False and gi["active"] is False, "Disabled GI became active on reload")


def check_model_request_progress(samples, expected_requests, *, successful_frames):
    """Prove queued imports and GPU commits keep servicing the application.

    Simulation frames settle GUI actions before sampling. These cumulative
    counters cover the additional rendered preparation/progress frames, rather
    than allowing a synchronous stall to pass solely because it eventually
    produced the right scene and image.
    """
    require(sorted(samples) == sorted(expected_requests), "Missing model request progress samples")
    previous = None
    first_request = True
    counters = ("requests", "completed", "preparationFrames", "progressFrames", "eventPumps")
    for frame in sorted(samples):
        state = samples[frame]["gui"].get("modelLoading")
        require(isinstance(state, dict), f"Missing model loading telemetry at {frame}")
        require(state.get("pending") is False, f"Model request is still pending at capture {frame}")
        for key in counters:
            value = state.get(key)
            require(isinstance(value, int) and not isinstance(value, bool) and value >= 0,
                    f"Invalid model loading counter {key} at {frame}")
            if previous is not None:
                require(value >= previous[key], f"Model loading counter {key} regressed at {frame}")
        count = expected_requests[frame]
        require(state["requests"] == count and state["completed"] == count,
                f"Model request did not finish exactly once at {frame}: {state}")
        if count and first_request:
            require(state["preparationFrames"] > 0,
                    f"First model load blocked rendering during preparation at {frame}")
            first_request = False
        if frame in successful_frames:
            require(previous is not None, "Successful model action has no previous progress sample")
            require(state["progressFrames"] > previous["progressFrames"],
                    f"Successful model load/unload did not render a progress frame at {frame}")
            require(state["eventPumps"] > previous["eventPumps"],
                    f"Successful model load/unload did not pump window events at {frame}")
        previous = state


def check_lifecycle(record, cornell, wall, bim, *, image_checks=True):
    samples = {sample["frame"]: sample for sample in record["samples"]}
    expected_frames = [8, 20, 32, 36, 44, 52, 56, 60, 72, 80, 92, 104, 108, 116, 120, 132, 144]
    require(len(samples) == len(record["samples"]) and sorted(samples) == expected_frames,
            "Missing/duplicate model lifecycle samples")
    mode = record["mode"]
    for frame, sample in samples.items():
        telemetry = sample["telemetry"]
        if frame in (72, 80, 144):
            check_unloaded(telemetry)
        else:
            path, auxiliary = ((cornell, False) if frame == 8 or 92 <= frame <= 120 else
                               (bim, True) if frame == 132 else (wall, True))
            if frame not in (36, 44, 52, 56, 60, 108, 116, 120):
                check_loaded(telemetry, path, auxiliary=auxiliary, require_gui=frame != 8)
        check_lifecycle_mode(telemetry, mode, frame not in (72, 80, 144))
    for failed, previous in ((36, 32), (44, 32), (52, 32), (56, 32), (60, 32),
                             (108, 104), (116, 104), (120, 104)):
        now, before = samples[failed]["telemetry"], samples[previous]["telemetry"]
        require(now["gui"]["status"].startswith("Failed to load model:"),
                f"Invalid model request at frame {failed} did not report an error")
        require(scene_signature(now) == scene_signature(before), f"Failed request changed scene at {failed}")
        require(stable_camera(now) == stable_camera(before), f"Failed request changed camera at {failed}")
        require(stable_lighting(now) == stable_lighting(before), f"Failed request changed lighting at {failed}")
        require(stable_reload_state(now) == stable_reload_state(before),
                f"Failed request changed history/settings/selection at {failed}")
    for before, after in ((8, 20), (20, 32), (32, 72), (80, 92), (92, 104), (104, 132), (132, 144)):
        require(samples[after]["telemetry"]["taa"]["epoch"] > samples[before]["telemetry"]["taa"]["epoch"],
                f"Model replacement/unload retained temporal history at {after}")
    require(scene_signature(samples[72]["telemetry"]) == scene_signature(samples[80]["telemetry"]),
            "Repeated unload changed the empty scene")
    initial_capacity = samples[8]["telemetry"]["scene"]["geometryArenaCapacityBytes"]
    require(initial_capacity > 0, "Loaded glTF has no geometry arena capacity")
    for frame in (92, 104):
        require(samples[frame]["telemetry"]["scene"]["geometryArenaCapacityBytes"] == initial_capacity,
                f"Repeated glTF load grew geometry arena capacity at {frame}")
    for frame in (72, 80, 144):
        require(samples[frame]["telemetry"]["scene"]["geometryArenaCapacityBytes"] <= initial_capacity,
                f"Unload grew geometry arena capacity at {frame}")
    if mode != "raster":
        require(samples[132]["telemetry"]["rayShadows"]["generation"] >
                samples[32]["telemetry"]["rayShadows"]["generation"],
                "Reloaded BIM reused an old acceleration generation")
    check_model_request_progress(
        {frame: sample["telemetry"] for frame, sample in samples.items()},
        {frame: index for index, frame in enumerate(expected_frames)},
        successful_frames=(20, 32, 72, 80, 92, 104, 132, 144))
    metrics = {}
    if image_checks:
        def pixels(frame):
            with Image.open(samples[frame]["png"]) as image:
                return np.asarray(image.convert("RGB"), dtype=np.float32)[70:440, 370:755] / 255
        empty = pixels(80)
        for frame in (8, 20, 92, 132):
            changed = float(np.abs(pixels(frame) - empty).mean())
            require(changed > .001, f"Loaded model produced an empty-scene image at {frame}")
            metrics[f"loadedVsEmptyMae{frame}"] = changed
        if mode == "raster":
            rollback = max(float(np.abs(pixels(frame) - pixels(previous)).mean())
                           for frame, previous in ((36, 32), (44, 32), (52, 32), (56, 32), (60, 32),
                                                   (108, 104), (116, 104), (120, 104)))
            require(rollback < .001, f"Failed load changed the raster scene pixels: {rollback}")
            metrics["failedRequestRgbMae"] = rollback
            repeated = float(np.abs(pixels(72) - empty).mean())
            require(repeated < .001, f"Repeated unload left changing scene geometry: {repeated}")
            metrics["repeatedUnloadRgbMae"] = repeated
    return metrics


def check_mixed_replacement(record, cornell, wall):
    samples = {sample["frame"]: sample["telemetry"] for sample in record["samples"]}
    require(len(samples) == len(record["samples"]) and sorted(samples) == [8, 20, 36],
            "Mixed startup samples are incomplete/duplicated")
    require(Path(samples[8]["scene"]["primary"]) == Path(cornell) and
            Path(samples[8]["scene"]["auxiliary"]) == Path(wall) and
            samples[8]["scene"]["primaryObjectCount"] > 0 and
            samples[8]["scene"]["auxiliaryObjectCount"] > 0, "Mixed startup did not load both providers")
    require(samples[8]["gui"]["selectedBimObjectIndex"] == 0,
            "Mixed startup did not select a real BIM object before replacement")
    check_loaded(samples[20], cornell, auxiliary=False)
    check_unloaded(samples[36])
    check_model_request_progress(samples, {8: 0, 20: 1, 36: 2}, successful_frames=(20, 36))


def check_asynchronous_cancellation(record, cornell):
    samples = {sample["frame"]: sample["telemetry"] for sample in record["samples"]}
    require(len(samples) == len(record["samples"]) and sorted(samples) == [3, 4, 5, 10, 20],
            "Asynchronous cancellation samples are incomplete/duplicated")
    before, pending = samples[3], samples[4]
    check_loaded(before, cornell, auxiliary=False, require_gui=False)
    state = pending["gui"]["modelLoading"]
    require(state["pending"] is True and state["requests"] == 1 and state["completed"] == 0,
            "Asynchronous GUI request did not render its pending preparation frame")
    require(state["preparationFrames"] > 0,
            "Asynchronous GUI preparation blocked ordinary rendering")
    require(scene_signature(pending) == scene_signature(before) and
            stable_camera(pending) == stable_camera(before) and
            stable_lighting(pending) == stable_lighting(before) and
            stable_reload_state(pending) == stable_reload_state(before),
            "Background preparation modified the live scene before commit")
    for frame in (5, 10):
        check_unloaded(samples[frame])
    require(scene_signature(samples[5]) == scene_signature(samples[10]),
            "Canceled BIM import published a late scene after unload")
    check_loaded(samples[20], cornell, auxiliary=False)
    for frame, expected in ((3, 0), (4, 0), (5, 1), (10, 1), (20, 1)):
        cancelled = samples[frame]["gui"]["modelLoading"].get("cancelled")
        require(isinstance(cancelled, int) and not isinstance(cancelled, bool) and cancelled == expected,
                f"Superseded import was not counted as exactly one cancellation at {frame}")
    check_model_request_progress({frame: sample for frame, sample in samples.items() if frame != 4},
                                 {3: 0, 5: 2, 10: 2, 20: 3}, successful_frames=(5, 20))


def run_lifecycle_matrix(exe, output, modes=LIFECYCLE_MODES, techniques=TECHNIQUES, timeout=240):
    output = Path(output)
    output.mkdir(parents=True, exist_ok=True)
    models = Path(exe).parent / "models"
    examples = models / "buildingSMART-IFC5-development/examples/Hello Wall"
    cornell = models / "validation/cornell_box_local_light.gltf"
    wall, bim = examples / "hello-wall.ifcx", models / "validation/taa_equivalent.bim"
    large_bim = models / "buildingSMART-IFC5-development/examples/Tekla House/TeklaHouse.ifcx"
    require(all(path.is_file() for path in (cornell, wall, bim, large_bim)), "Lifecycle fixtures are missing")
    missing_gltf, missing_bim = output / "missing-gui.gltf", output / "missing-gui.ifcx"
    require(not missing_gltf.exists() and not missing_bim.exists(), "Missing-model controls unexpectedly exist")
    malformed_gltf, malformed_bim = output / "malformed-gui.gltf", output / "malformed-gui.ifcx"
    malformed_dot_bim = output / "malformed-gui.bim"
    for path in (malformed_gltf, malformed_bim, malformed_dot_bim):
        path.write_text("{broken JSON", encoding="utf-8")
    (output / "vk_layer_settings.txt").write_text("khronos_validation.validate_sync = true\n", encoding="utf-8")
    (output / "imgui.ini").write_text(
        "[Window][Scene Controls]\nPos=0,0\nSize=350,700\nCollapsed=1\n\n"
        "[Window][Renderer Telemetry]\nPos=0,25\nSize=350,700\nCollapsed=1\n", encoding="utf-8")
    result = {"schemaVersion": 1, "exeSha256": hashlib.sha256(Path(exe).read_bytes()).hexdigest(),
              "cases": [], "failures": [], "passed": False}
    for technique in techniques:
        prefix = technique.split("-")[0]
        for mode in modes:
            name = f"{prefix}-{mode}-lifecycle"
            try:
                sequence = lifecycle_sequence(cornell, wall, bim, missing_gltf, malformed_gltf,
                                              missing_bim, malformed_bim, malformed_dot_bim, mode)
                record = run_lifecycle_capture(exe, output, name, cornell, sequence, technique, mode, timeout=timeout)
                record["imageMetrics"] = check_lifecycle(record, cornell, wall, bim)
                record["passed"] = True
                result["cases"].append(record)
            except (AssertionError, OSError, ValueError, KeyError, subprocess.TimeoutExpired) as error:
                result["cases"].append({"name": name, "passed": False, "error": str(error)})
                result["failures"].append(f"{name}: {error}")
            (output / "lifecycle-results.json").write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
            print(json.dumps({"case": name, "passed": result["cases"][-1]["passed"]}), flush=True)
        name = f"{prefix}-mixed-startup-replacement"
        try:
            sequence = {"schemaVersion": 1, "frames": 36, "sampleFrames": [8, 20, 36],
                        "events": [{"frame": 4, "bimSelectedObject": 0},
                                   {"frame": 12, "guiReload": str(cornell)}, {"frame": 28, "guiUnload": True}]}
            record = run_lifecycle_capture(exe, output, name, cornell, sequence, technique, "raster",
                                           extra=["--bim-model", str(wall)], timeout=timeout)
            check_mixed_replacement(record, cornell, wall)
            record["passed"] = True
            result["cases"].append(record)
        except (AssertionError, OSError, ValueError, KeyError, subprocess.TimeoutExpired) as error:
            result["cases"].append({"name": name, "passed": False, "error": str(error)})
            result["failures"].append(f"{name}: {error}")
        name = f"{prefix}-asynchronous-load-cancellation"
        try:
            sequence = asynchronous_cancel_sequence(cornell, large_bim)
            record = run_lifecycle_capture(exe, output, name, cornell, sequence, technique, "raster",
                                           timeout=timeout)
            check_asynchronous_cancellation(record, cornell)
            record["passed"] = True
            result["cases"].append(record)
        except (AssertionError, OSError, ValueError, KeyError, subprocess.TimeoutExpired) as error:
            result["cases"].append({"name": name, "passed": False, "error": str(error)})
            result["failures"].append(f"{name}: {error}")
        print(json.dumps({"case": name, "passed": result["cases"][-1]["passed"]}), flush=True)
    result["passed"] = not result["failures"]
    (output / "lifecycle-results.json").write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--exe", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--suite", choices=("legacy", "lifecycle", "all"), default="all")
    parser.add_argument("--modes", nargs="+", choices=LIFECYCLE_MODES, default=list(LIFECYCLE_MODES))
    parser.add_argument("--technique", choices=("both",) + TECHNIQUES, default="both")
    parser.add_argument("--timeout", type=float, default=240)
    args = parser.parse_args()
    if os.environ.get("CONTAINER_RUN_GPU_GUI_MODEL_LOADING") != "1":
        print("SKIP: set CONTAINER_RUN_GPU_GUI_MODEL_LOADING=1")
        return 77
    exe, output = args.exe.resolve(), args.output.resolve()
    if not exe.is_file() or not math.isfinite(args.timeout) or args.timeout <= 0:
        parser.error("An existing executable and finite positive timeout are required")
    techniques = TECHNIQUES if args.technique == "both" else (args.technique,)
    result = {"schemaVersion": 1, "passed": True}
    if args.suite in ("legacy", "all"):
        code = legacy_main(["--exe", str(exe), "--output", str(output / "legacy")])
        if code:
            return code
        result["legacy"] = json.loads((output / "legacy/results.json").read_text(encoding="utf-8"))
    if args.suite in ("lifecycle", "all"):
        result["lifecycle"] = run_lifecycle_matrix(exe, output / "lifecycle", tuple(args.modes), techniques, args.timeout)
        result["passed"] &= result["lifecycle"]["passed"]
    output.mkdir(parents=True, exist_ok=True)
    (output / "results.json").write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    return 0 if result["passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
