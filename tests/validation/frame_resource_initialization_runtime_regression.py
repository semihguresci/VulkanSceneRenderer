"""Check initialized frame resources across startup, resize, and live MSAA changes."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import tempfile

from display_capture_test_support import validation_log_error


START = (320, 180)
RESIZED = (384, 216)
TECHNIQUES = ("deferred-raster", "forward-raster")
PHASES = (
    ("startup", 2, START, 1, True, ()),
    ("opaque-control", 3, START, 1, False, {"nodeFlags": {"sourceNode": 1, "visible": False}}),
    ("resize", 4, RESIZED, 1, True, {"resize": list(RESIZED), "nodeFlags": {"sourceNode": 1, "visible": True}}),
    ("msaa-4x", 6, RESIZED, 4, True, {"samples": 4}),
    ("msaa-1x", 8, RESIZED, 1, True, {"samples": 1}),
)


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


def make_sequence(path):
    frames = [frame for _, frame, *_ in PHASES]
    events = [{"frame": frame, "depthProbe": [width // 2, height // 2], **dict(options)}
              for _, frame, (width, height), _, _, options in PHASES]
    path.write_text(json.dumps({"schemaVersion": 1, "frames": frames[-1], "sampleFrames": frames,
                                "events": events}, indent=2) + "\n", encoding="utf-8")


def capture(renderer, model, sequence, output, technique):
    image = output / f"{technique}.png"
    log = image.with_suffix(".log")
    command = [str(renderer), "--model", str(model), "--import-scale", "1", "--hidden", "--no-ui", "--validation",
               "--no-taa", "--no-ray-query", "--no-ddgi", "--no-bloom", "--msaa", "1", "--display-mode", "lit",
               "--width", str(START[0]), "--height", str(START[1]), "--render-technique", technique,
               "--camera-position", "0", "0", "6", "--camera-target", "0", "0", "0", "--camera-fov", "45",
               "--sky", "plain", "--sky-horizon", "0", "0", "0", "--environment-intensity", "0", "--bounce-intensity", "0",
               "--directional-intensity", "3", "--directional-direction", "0", "0", "-1", "--exposure", "1",
               "--fixed-dt", ".016666667", "--screenshot-viewport", "--capture-sequence", str(sequence), "--screenshot", str(image)]
    with log.open("w", encoding="utf-8") as stream:
        result = subprocess.run(command, cwd=renderer.parent, env=dict(os.environ, VK_LAYER_SETTINGS_PATH=str(output)),
                                stdout=stream, stderr=subprocess.STDOUT, timeout=180,
                                creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
    text = log.read_text(encoding="utf-8", errors="replace")
    require(result.returncode == 0 and not validation_log_error(text), f"Vulkan capture failed: {log}")
    if os.environ.get("VK_LOADER_DEBUG") == "layer":
        require('Insert instance layer "VK_LAYER_KHRONOS_validation"' in text, f"Validation layer was not loaded: {log}")
    return {"technique": technique, "command": command, "log": str(log), "captures": []}


def check_capture(output, technique, phase):
    from gltf_pick_depth_runtime_regression import projected_depth
    from PIL import Image

    name, frame, extent, samples, transparent, _ = phase
    stem = f"{technique}.frame-{frame:04d}" if frame != PHASES[-1][1] else technique
    path = output / f"{stem}.png"
    state = json.loads(path.with_suffix(".telemetry.json").read_bytes())
    metadata = json.loads(Path(f"{path}.capture.json").read_bytes())
    require(state["validationEnabled"] and state["technique"] == technique and
            state["resolution"] == list(extent) and state["msaaSamples"] == samples,
            f"Wrong renderer state for {technique} {name}")
    require((metadata["width"], metadata["height"]) == extent and metadata["source"] == "viewport"
            and not metadata["includesUi"], metadata)
    require(state["swapchainRecreates"] == (0 if extent == START else 1), state["swapchainRecreates"])
    active = {item["name"] for item in state["passes"] if item["active"]}
    require("DepthPrepass" in active and
            ({"OitClear", "OitResolve", "TransparentPick"} <= active if transparent else "OitResolve" not in active),
            f"Wrong transparency or picking passes for {technique} {name}: {sorted(active)}")
    probe = state["depthProbe"]
    center = (extent[0] // 2, extent[1] // 2)
    require(probe["pixel"] == list(center), probe)
    expected = projected_depth(state, 0)
    require(probe["opaque"] is not None and abs(probe["opaque"] - expected) < 2e-6, probe)
    if transparent:
        expected = projected_depth(state, 1)
        interaction = probe["sceneInteraction"]
        require(probe["pick"] is not None and abs(probe["pick"] - expected) < 2e-6 and
                interaction["rawGpuId"] > 0 and interaction["gpu"]["hit"]["sourceNode"] == 1,
                f"The transparent surface did not survive GPU picking in {technique} {name}: {probe}")
    with Image.open(path) as png:
        require(png.format == "PNG" and png.size == extent, path)
        rgb = png.convert("RGB")
        # Center samples are well inside both planes and free of MSAA edges.
        center_rgb = rgb.getpixel(center)
        decoded_hash = hashlib.sha256(rgb.tobytes()).hexdigest()
    return {"phase": name, "frame": frame, "image": str(path), "extent": list(extent), "msaa": samples,
            "revision": state["build"]["revision"], "swapchainImages": state["swapchainImages"],
            "swapchainRecreates": state["swapchainRecreates"], "passes": sorted(active), "depthProbe": probe,
            "centerRgb": center_rgb, "decodedSha256": decoded_hash}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--renderer", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--technique", choices=TECHNIQUES, action="append")
    parser.add_argument("--managed-output", action="store_true", help="write to a fresh run directory under --output")
    parser.add_argument("--validation-settings", type=Path, help="additional Vulkan layer settings to use with sync validation")
    args = parser.parse_args()
    if os.environ.get("CONTAINER_RUN_GPU_FRAME_INITIALIZATION") != "1":
        print("SKIP: set CONTAINER_RUN_GPU_FRAME_INITIALIZATION=1 to run GPU captures")
        return 77

    from gltf_pick_depth_runtime_regression import write_scene

    renderer, output = args.renderer.resolve(strict=True), args.output.resolve()
    if args.managed_output:
        output.mkdir(parents=True, exist_ok=True)
        output = Path(tempfile.mkdtemp(prefix="run-", dir=output))
        print(f"Captures: {output}", flush=True)
    else:
        output.mkdir(parents=True)
    settings = args.validation_settings.read_text(encoding="utf-8") if args.validation_settings else ""
    if "khronos_validation.validate_sync = true\n" not in settings:
        settings += "\nkhronos_validation.validate_sync = true\n"
    (output / "vk_layer_settings.txt").write_text(settings, encoding="utf-8")
    model, sequence = output / "planes.gltf", output / "lifecycle.json"
    write_scene(model)
    make_sequence(sequence)
    renderer_hash = hashlib.sha256(renderer.read_bytes()).hexdigest()
    report = {"executableSha256": renderer_hash, "fixture": str(model), "sequence": str(sequence), "passed": False, "runs": []}
    try:
        for technique in args.technique or TECHNIQUES:
            row = capture(renderer, model, sequence, output, technique)
            report["runs"].append(row)
            row["captures"] = [check_capture(output, technique, phase) for phase in PHASES]
            startup, control, resized, msaa, restored = row["captures"]
            require(startup["centerRgb"][1] > control["centerRgb"][1] + 15,
                    f"No visible contribution from the transparent surface in {technique}")
            require(resized["decodedSha256"] == restored["decodedSha256"],
                    f"Returning to 1× MSAA changed the resized image in {technique}")
            for item in (resized, msaa, restored):
                require(max(abs(a - b) for a, b in zip(item["centerRgb"], startup["centerRgb"], strict=True)) <= 2,
                        f"Center transparency changed during {technique} {item['phase']}")
            print(f"{technique}: startup, resize, live 4x to 1x MSAA, OIT and picking verified", flush=True)
        require(hashlib.sha256(renderer.read_bytes()).hexdigest() == renderer_hash, "Renderer executable changed during capture")
        report["passed"] = True
    finally:
        (output / "results.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
