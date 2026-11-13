"""Exercise captures against a test layer that emulates permitted WSI clipping."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import tempfile

from display_capture_test_support import emitter_fixture, validation_log_error


LAYER = "VK_LAYER_CONTAINER_clip_fixture"
TECHNIQUES = ("deferred-raster", "forward-raster")
CLIPPED_MODES = ("covered", "hidden")
KINDS = ("viewport", "window", "radiance")
START = (640, 480)
REPLACEMENT = (672, 480)
FRAMES = {3: START, 5: START, 7: REPLACEMENT}


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


def read_image(path, kind, extent):
    import cv2
    import numpy as np
    from PIL import Image

    metadata = json.loads(Path(f"{path}.capture.json").read_bytes())
    require((metadata["width"], metadata["height"]) == extent, path)
    require(metadata["includesUi"] == (kind == "window"), path)
    if kind == "radiance":
        require(metadata["source"] == "raster-scene-composite" and
                metadata["colorSpace"] == "linear-sRGB" and
                not metadata["toneMappingApplied"], path)
        bgr = cv2.imdecode(np.frombuffer(path.read_bytes(), dtype=np.uint8), cv2.IMREAD_UNCHANGED)
        require(bgr is not None and bgr.dtype == np.float32 and
                bgr.shape == (extent[1], extent[0], 3), path)
        return bgr[:, :, ::-1].copy()
    require(metadata["source"] == kind and metadata["colorSpace"] == "sRGB", path)
    with Image.open(path) as image:
        require(image.format == "PNG" and image.size == extent, path)
        return np.array(image.convert("RGBA"))


def launch(renderer, layer_dir, output, model, sequence, technique, mode, kind, force, report):
    directory = output / technique / ("forced" if force else "normal") / mode
    directory.mkdir(parents=True, exist_ok=True)
    target = directory / (f"{kind}.exr" if kind == "radiance" else f"{kind}.png")
    log = target.with_suffix(".log")
    command = [str(renderer), "--hidden", "--validation", "--msaa", "1", "--no-taa", "--no-bloom",
               "--no-ddgi", "--no-ray-query", "--width", str(START[0]), "--height", str(START[1]),
               "--render-technique", technique, "--model", str(model), "--import-scale", "1",
               "--camera-position", "0", "0", "4", "--camera-target", "0", "0", "0",
               "--camera-fov", "45", "--sky", "plain", "--sky-horizon", "0", "0", "0",
               "--directional-intensity", "0", "--environment-intensity", "0", "--bounce-intensity", "0",
               "--exposure", "1", "--tone-map", "reinhard", "--display-mode", "lit", "--fixed-dt", ".016666667",
               "--capture-sequence", str(sequence), "--screenshot", str(target)]
    if kind == "viewport":
        command.append("--screenshot-viewport")
    env = dict(os.environ, VK_LOADER_LAYERS_ENABLE=LAYER,
               VK_LAYER_SETTINGS_PATH=str(output), VK_LOADER_DEBUG="layer", OPENCV_IO_ENABLE_OPENEXR="1",
               CONTAINER_CLIP_TEST_MODE=mode, CONTAINER_CLIP_TEST_FORCE_TRUE=str(int(force)),
               CONTAINER_CLIP_TEST_START_FRAME="4")
    if env.get("VK_LAYER_PATH"):
        env["VK_LAYER_PATH"] += os.pathsep + str(layer_dir)
    else:
        env["VK_ADD_LAYER_PATH"] = str(layer_dir)
    with log.open("w", encoding="utf-8") as stream:
        result = subprocess.run(command, cwd=renderer.parent, env=env, stdout=stream,
                                stderr=subprocess.STDOUT, timeout=90,
                                creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
    log_text = log.read_text(encoding="utf-8", errors="replace")
    row = dict(technique=technique, mode=mode, kind=kind, forced=force,
               log=str(log), exitCode=result.returncode, captures=[])
    report["runs"].append(row)
    require(result.returncode == 0 and not validation_log_error(log_text), f"Vulkan capture failed: {log}")
    for name in (LAYER, "VK_LAYER_KHRONOS_validation"):
        for action in ("Insert instance", "Inserted device"):
            require(f'{action} layer "{name}"' in log_text, f"{name} missing from {action}: {log}")

    generations = re.findall(r"\[clip-fixture\] swapchain generation=(\d+) requestedClipped=(\d+) "
                             r"effectiveClipped=(\d+) format=(\d+) extent=(\d+)x(\d+) mode=(\w+) startFrame=4", log_text)
    require(len(generations) == 2, f"Expected startup and recreation in {log}")
    for index, (generation, requested, enabled, _, w, h, actual_mode) in enumerate(generations):
        require((int(generation), requested, int(enabled), (int(w), int(h)), actual_mode) ==
                (index + 1, "0", int(force), (START, REPLACEMENT)[index], mode),
                f"Unexpected swapchain creation in {log}: {generations}")
    row["swapchains"] = [dict(generation=int(generation), requestedClipped=bool(int(requested)),
                              effectiveClipped=bool(int(enabled)), format=int(fmt), extent=[int(w), int(h)])
                         for generation, requested, enabled, fmt, w, h, _ in generations]
    scopes = re.findall(r"\[clip-fixture\] scissor generation=(\d+) mode=\w+ applied=(\d+) frame=(\d+) "
                        r"x=0 y=0 width=(\d+) height=(\d+) clippedX=0 clippedY=0 clippedWidth=(\d+) clippedHeight=(\d+)",
                        log_text)
    expected_scopes = []
    for frame in range(1, max(FRAMES) + 1):
        generation = 1 if frame < 6 else 2
        width, height = (START, REPLACEMENT)[generation - 1]
        apply_clip = force and frame >= 4
        clipped_width = {"covered": width // 2, "hidden": 0}[mode] if apply_clip else width
        clipped_height = 0 if apply_clip and mode == "hidden" else height
        expected_scopes.append((generation, int(apply_clip), frame, width, height, clipped_width, clipped_height))
    require([tuple(map(int, scope)) for scope in scopes] == expected_scopes,
            f"Unexpected swapchain color scopes or scissor extent: {log}")
    row["scissorMutations"] = sum(scope[1] for scope in expected_scopes)

    images = {}
    for frame, extent in FRAMES.items():
        path = target if frame == max(FRAMES) else target.with_name(f"{target.stem}.frame-{frame:04d}{target.suffix}")
        state = json.loads(path.with_suffix(".telemetry.json").read_bytes())
        require(state["technique"] == technique and state["validationEnabled"] and
                state["resolution"] == list(extent) and state["swapchainRecreates"] == (frame == max(FRAMES)), path)
        image = read_image(path, kind, extent)
        images[frame] = image
        row["captures"].append(dict(frame=frame, path=str(path), revision=state["build"]["revision"],
                                    decodedSha256=hashlib.sha256(image.tobytes()).hexdigest()))
    return images


def compare(controls, samples, mode, kind, force):
    import numpy as np

    np.testing.assert_array_equal(samples[3], controls[3])
    if not force:
        for frame in (5, 7):
            require(np.isfinite(samples[frame]).all(), "Nonfinite capture")
            np.testing.assert_array_equal(samples[frame], controls[frame])
        return 0

    for frame in (5, 7):
        control, sample = controls[frame], samples[frame]
        midpoint = control.shape[1] // 2
        if mode == "covered":
            np.testing.assert_array_equal(sample[:, :midpoint], control[:, :midpoint])
    # Frame 5 should overwrite bright EXR history with black at the same extent.
    # Frame 7 shows missing lit PNG pixels on the replacement swapchain.
    frame = 5 if kind == "radiance" else 7
    start = controls[frame].shape[1] // 2 if mode == "covered" else 0
    affected = samples[frame][:, start:] != controls[frame][:, start:]
    changed_pixels = int(np.count_nonzero(np.any(affected, axis=-1)))
    require(changed_pixels > 1000,
            f"No pixel loss or stale radiance in forced {mode} {kind}, frame {frame}")
    if kind == "radiance":
        x, y = START[0] // 2 + 20, START[1] // 2
        np.testing.assert_allclose(controls[3][y, x], [4, 2, 1], atol=.008, rtol=0)
        np.testing.assert_allclose(controls[5][y, x], [0, 0, 0], atol=.008, rtol=0)
        np.testing.assert_array_equal(samples[5][y, x], controls[3][y, x])
    return changed_pixels


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--renderer", type=Path, required=True)
    parser.add_argument("--layer-dir", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    if os.environ.get("CONTAINER_RUN_GPU_CAPTURE_CLIPPING") != "1":
        print("SKIP: set CONTAINER_RUN_GPU_CAPTURE_CLIPPING=1 to run")
        return 77
    os.environ.setdefault("OPENCV_IO_ENABLE_OPENEXR", "1")
    renderer, layer_dir = args.renderer.resolve(strict=True), args.layer_dir.resolve(strict=True)
    args.output.mkdir(parents=True, exist_ok=True)
    output = Path(tempfile.mkdtemp(prefix="run-", dir=args.output)).resolve()
    print(f"Captures: {output}", flush=True)
    (output / "vk_layer_settings.txt").write_text("khronos_validation.validate_sync = true\n", encoding="utf-8")
    model = output / "emitter.gltf"
    asset = emitter_fixture(model, {"emissiveFactor": [.5, .25, .125],
        "extensions": {"KHR_materials_emissive_strength": {"emissiveStrength": 8}},
        "pbrMetallicRoughness": {"baseColorFactor": [0, 0, 0, 1]}})
    asset["nodes"][0]["name"] = "Emitter"
    model.write_text(json.dumps(asset), encoding="utf-8")
    sequence = output / "sequence.json"
    sequence.write_text(json.dumps(dict(schemaVersion=1, frames=7, sampleFrames=list(FRAMES),
        objectName="Emitter", events=[dict(frame=4, objectVisible=False),
                                     dict(frame=6, resize=REPLACEMENT, objectVisible=True)])), encoding="utf-8")
    renderer_hash = hashlib.sha256(renderer.read_bytes()).hexdigest()
    report = dict(rendererSha256=renderer_hash, emulatedClipping=True, passed=False, runs=[])
    try:
        for technique in TECHNIQUES:
            visible = {}
            for kind in KINDS:
                visible[kind] = launch(renderer, layer_dir, output, model, sequence, technique,
                                       "visible", kind, False, report)
                for mode in CLIPPED_MODES:
                    for force in (False, True):
                        samples = launch(renderer, layer_dir, output, model, sequence, technique,
                                         mode, kind, force, report)
                        report["runs"][-1]["changedControlPixels"] = compare(visible[kind], samples, mode, kind, force)
            import numpy as np
            require(np.count_nonzero(np.any(visible["viewport"][3] != visible["window"][3], axis=-1)) > 1000,
                    f"The window capture lost ImGui in {technique}")
            print(f"{technique}: viewport PNG, window PNG and EXR passed emulated clipping checks", flush=True)
        require(hashlib.sha256(renderer.read_bytes()).hexdigest() == renderer_hash, "Renderer changed during the test")
        report["passed"] = True
    finally:
        (output / "results.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
