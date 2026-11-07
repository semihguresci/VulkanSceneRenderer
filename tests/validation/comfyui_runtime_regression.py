"""Exercise native generation capture, preview, and HDR publication with a local spool mock."""

import argparse
import io
import json
import os
from pathlib import Path
import subprocess
import sys
import threading

from PIL import Image, ImageChops

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools"))
from comfyui_bridge import atomic_write, png_to_hdr, read_json


def image_bytes(size, color):
    output = io.BytesIO()
    Image.new("RGB", size, color).save(output, format="PNG")
    return output.getvalue()


def adapter_loop(root, stop, records, errors):
    jobs = root / "jobs"
    seen = set()
    hdr = png_to_hdr(image_bytes((64, 32), (210, 150, 110)))
    while not stop.wait(.02):
        if not jobs.is_dir():
            continue
        for folder in jobs.iterdir():
            request_path = folder / "request.json"
            if folder in seen or not request_path.is_file():
                continue
            seen.add(folder)
            status = {"version": 1, "id": folder.name, "phase": "succeeded", "progress": 1.}
            try:
                request = read_json(request_path)
                assert request["seed"] == 8241, request
                records[request["prompt"]] = folder
                if request["prompt"] == "mock-failure":
                    status.update(phase="failed", progress=0., message="Mock model unavailable")
                elif request["kind"] == "hdri":
                    data = hdr if request["prompt"] == "studio" else hdr.replace(b"-Y 32 +X 64", b"-Y 16 +X 64")
                    atomic_write(folder / "generated.hdr", data)
                    status["output"] = "generated.hdr"
                else:
                    with Image.open(folder / "input.png") as source:
                        assert source.size == (240, 180), source.size
                    capture = read_json(folder / "input.png.capture.json")
                    assert capture["source"] == "viewport" and not capture["includesUi"], capture
                    atomic_write(folder / "generated.png", image_bytes((80, 48), (230, 30, 170)))
                    status["output"] = "generated.png"
            except (OSError, AssertionError, ValueError, KeyError) as error:
                errors.append(str(error))
                status.update(phase="failed", progress=0., message=str(error))
            atomic_write(folder / "status.json", json.dumps(status).encode())


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--renderer", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--model", type=Path, required=True)
    parser.add_argument("--check-preview-recreation", action="store_true",
                        help="Exercise generated preview retirement and recreation across MSAA changes")
    args = parser.parse_args()
    renderer, output, model = (p.resolve() for p in (args.renderer, args.output, args.model))
    output.mkdir(parents=True, exist_ok=False)
    root = output / "bridge"
    root.mkdir()
    (output / "vk_layer_settings.txt").write_text("khronos_validation.validate_sync = true\n")
    sequence = output / "capture-sequence.json"
    actions = [("generate-environment", "studio"), ("apply-environment", None),
               ("restyle-viewport", "watercolor"), ("generate-environment", "bad-aspect"),
               ("apply-environment", None), ("generate-environment", "mock-failure")]
    events = []
    for frame, (action, prompt) in enumerate(actions, 2):
        event = {"action": action, "wait": True}
        if prompt:
            event.update(prompt=prompt, seed=8241)
        events.append({"frame": frame, "comfyUi": event})
    frames, failure_start = 7, 5
    if args.check_preview_recreation:
        frames, failure_start = 11, 9
        for event in events:
            if event["frame"] >= 5:
                event["frame"] += 4
        events += [{"frame": 5, "samples": 4}, {"frame": 7, "samples": 1}]
        events.sort(key=lambda event: event["frame"])
    sequence.write_text(json.dumps({"schemaVersion": 1, "frames": frames,
                                    "sampleFrames": list(range(1, frames + 1)), "events": events}))
    command = [str(renderer), "--model", str(model), "--hidden", "--validation", "--import-scale", "1",
               "--width", "240", "--height", "180", "--no-taa", "--no-bloom", "--no-ddgi", "--msaa", "1",
               "--ray-shadows", "raster", "--sky", "plain", "--exposure", "1", "--capture-sequence", str(sequence),
               "--screenshot", str(output / "capture.png"), "--screenshot-viewport"]
    env = dict(os.environ, CONTAINER_COMFYUI_ROOT=str(root), VK_LAYER_SETTINGS_PATH=str(output))
    stop, records, errors = threading.Event(), {}, []
    worker = threading.Thread(target=adapter_loop, args=(root, stop, records, errors))
    worker.start()
    try:
        process = subprocess.run(command, cwd=renderer.parent, env=env, text=True, errors="replace",
                                 capture_output=True, timeout=240)
    finally:
        stop.set()
        worker.join()
    log = process.stdout + process.stderr
    (output / "renderer.log").write_text(log, encoding="utf-8")
    assert process.returncode == 0, log
    assert not any(marker in log for marker in ("Validation Error", "SYNC-HAZARD", "VUID-")), log
    assert not errors, errors

    def name(frame):
        return "capture" if frame == frames else f"capture.frame-{frame:04d}"

    states = {frame: json.loads((output / (name(frame) + ".telemetry.json")).read_text(encoding="utf-8"))
              for frame in range(1, frames + 1)}
    assert "comfyUi" not in states[1], states[1].keys()
    pending, applied, restyle, invalid, rejected, failure = [states[frame]["comfyUi"]
        for frame in (2, 3, 4, failure_start, failure_start + 1, failure_start + 2)]
    active = Path(applied["activeEnvironment"])
    assert pending["phase"] == "ready" and pending["activeEnvironment"] != applied["activeEnvironment"], pending
    assert applied["resultStatus"] == "Generated environment applied" and active == records["studio"] / "generated.hdr", applied
    assert restyle["phase"] == "ready" and restyle["previewReady"], restyle
    assert Path(restyle["result"]) == records["watercolor"] / "generated.png", restyle
    assert invalid["phase"] == "ready" and invalid["activeEnvironment"] == applied["activeEnvironment"], invalid
    assert rejected["activeEnvironment"] == applied["activeEnvironment"] and "2:1 equirectangular" in rejected["resultStatus"], rejected
    assert failure["phase"] == "failed" and "Mock model unavailable" in failure["message"], failure
    assert failure["activeEnvironment"] == applied["activeEnvironment"], failure
    if args.check_preview_recreation:
        for transition, restored, samples in ((5, 6, 4), (7, 8, 1)):
            assert states[transition]["msaaSamples"] == states[restored]["msaaSamples"] == samples, states[transition]
            assert states[transition]["comfyUi"]["previewReady"], states[transition]["comfyUi"]
            assert states[restored]["comfyUi"]["previewReady"], states[restored]["comfyUi"]
            assert states[restored]["comfyUi"]["result"] == restyle["result"], states[restored]["comfyUi"]

    with Image.open(output / (name(3) + ".png")) as reference:
        with Image.open(records["watercolor"] / "input.png") as captured:
            assert ImageChops.difference(reference.convert("RGB"), captured.convert("RGB")).getbbox() is None
        for frame in (4, *range(failure_start, frames + 1)):
            with Image.open(output / (name(frame) + ".png")) as captured:
                assert ImageChops.difference(reference.convert("RGB"), captured.convert("RGB")).getbbox() is None, frame
    (output / "results.json").write_text(json.dumps({"passed": True, "command": command,
        "previewRecreation": args.check_preview_recreation,
        "applied": applied, "restyle": restyle, "rejected": rejected, "failure": failure}, indent=2))
    print("PASS generated HDR publication, rejected HDR retention, separate restyle preview, and mock failure")


if __name__ == "__main__":
    main()
