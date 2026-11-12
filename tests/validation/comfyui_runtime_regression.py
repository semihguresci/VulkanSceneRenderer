"""Exercise the native renderer and production adapter with a loopback ComfyUI mock."""

import argparse
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import io
import json
import os
from pathlib import Path
import subprocess
import sys
import threading
import time
from urllib.parse import parse_qs, urlsplit

from PIL import Image, ImageChops

from display_capture_test_support import validation_log_error

TOOLS = Path(__file__).resolve().parents[2] / "tools"
sys.path.insert(0, str(TOOLS))
from comfyui_bridge import Adapter, ComfyClient, adapter_lock, png_to_hdr, read_json


LOST_JOB_MESSAGE = "ComfyUI no longer lists this job in its queue or history. Start a new generation request."
OUTPUT_NAMES = {"studio": "studio.png", "watercolor": "watercolor.png", "bad-aspect": "bad-aspect.hdr"}
JOB_PROMPTS = {*OUTPUT_NAMES, "mock-failure", "lost-job"}


def image_bytes(size, color):
    output = io.BytesIO()
    Image.new("RGB", size, color).save(output, format="PNG")
    return output.getvalue()


class ComfyMock(BaseHTTPRequestHandler):
    def log_message(self, *_args):
        pass

    def reply(self, value, code=200):
        data = value if isinstance(value, bytes) else json.dumps(value).encode()
        self.send_response(code)
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def do_POST(self):
        body = self.rfile.read(int(self.headers.get("Content-Length", 0)))
        if self.path == "/upload/image":
            self.server.uploads.append(body)
            self.reply({"name": "input.png", "subfolder": "bridge", "type": "input"})
        elif self.path == "/prompt":
            request = json.loads(body)
            prompt = request["prompt"]["2"]["inputs"]["text"]
            if prompt not in JOB_PROMPTS:
                self.reply({"error": "Unexpected mock prompt"}, 400)
                return
            prompt_id = f"{len(self.server.prompts) + 1:032x}"
            self.server.prompts[prompt_id] = (prompt, request)
            self.reply({"prompt_id": prompt_id})
        elif self.path.startswith("/api/jobs/") and self.path.endswith("/cancel"):
            self.reply({})
        else:
            self.reply({}, 404)

    def do_GET(self):
        route = urlsplit(self.path)
        self.server.gets.append(route.path)
        if route.path.startswith("/history/"):
            prompt_id = route.path.removeprefix("/history/")
            prompt, _ = self.server.prompts[prompt_id]
            if prompt == "lost-job":
                if self.server.gets.count(route.path) == 1:
                    # Simulate slow HTTP while the scripted wait keeps drawing.
                    time.sleep(.35)
                self.reply({})
            elif prompt == "mock-failure":
                self.reply({prompt_id: {"status": {"status_str": "error", "completed": False,
                                                   "messages": ["Mock model unavailable"]}}})
            else:
                output = {"filename": OUTPUT_NAMES[prompt], "subfolder": "bridge", "type": "output"}
                self.reply({prompt_id: {"status": {"status_str": "success", "completed": True},
                                        "outputs": {"7": {"images": [output]}}}})
        elif route.path == "/queue":
            self.reply({"queue_running": [], "queue_pending": []})
        elif route.path == "/view":
            query = parse_qs(route.query)
            if query.get("subfolder") != ["bridge"] or query.get("type") != ["output"]:
                self.reply({}, 400)
                return
            self.reply(self.server.outputs[query["filename"][0]])
        else:
            self.reply({}, 404)


def adapter_loop(root, adapter, stop, errors):
    try:
        with adapter_lock(root):
            while not stop.wait(.02):
                adapter.poll()
    except (OSError, RuntimeError, ValueError) as error:
        errors.append(str(error))


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
               ("apply-environment", None), ("generate-environment", "mock-failure"),
               ("generate-environment", "lost-job")]
    events = []
    for frame, (action, prompt) in enumerate(actions, 2):
        event = {"action": action, "wait": True}
        if prompt:
            event.update(prompt=prompt, seed=8241)
        events.append({"frame": frame, "comfyUi": event})
    frames, failure_start = 8, 5
    if args.check_preview_recreation:
        frames, failure_start = 12, 9
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
    stop, errors = threading.Event(), []
    workflows = {kind: read_json(TOOLS / "comfyui_workflows" / f"{kind}.json") for kind in ("hdri", "restyle")}
    studio = image_bytes((64, 32), (210, 150, 110))
    with ThreadingHTTPServer(("127.0.0.1", 0), ComfyMock) as server:
        server.prompts, server.uploads, server.gets = {}, [], []
        server.outputs = {"studio.png": studio, "watercolor.png": image_bytes((80, 48), (230, 30, 170)),
                          "bad-aspect.hdr": png_to_hdr(studio).replace(b"-Y 32 +X 64", b"-Y 16 +X 64")}
        adapter = Adapter(root, ComfyClient(server.server_port), workflows)
        listener = threading.Thread(target=server.serve_forever)
        worker = threading.Thread(target=adapter_loop, args=(root, adapter, stop, errors))
        listener.start()
        worker.start()
        try:
            process = subprocess.run(command, cwd=renderer.parent, env=env, text=True, errors="replace",
                                     capture_output=True, timeout=240)
        finally:
            stop.set()
            worker.join()
            server.shutdown()
            listener.join()
    log = process.stdout + process.stderr
    (output / "renderer.log").write_text(log, encoding="utf-8")
    assert process.returncode == 0, log
    assert not validation_log_error(log), log
    assert not errors, errors
    assert len(server.prompts) == len(JOB_PROMPTS), server.prompts
    records = {}
    for prompt_id, (prompt, request) in server.prompts.items():
        folder = root / "jobs" / request["client_id"]
        spool_request = read_json(folder / "request.json")
        assert spool_request["prompt"] == prompt and spool_request["seed"] == 8241, spool_request
        samplers = [node["inputs"] for node in request["prompt"].values() if node["class_type"] == "KSampler"]
        assert len(samplers) == 1 and samplers[0]["seed"] == 8241, request
        if prompt == "watercolor":
            assert request["prompt"]["4"]["inputs"]["image"] == "bridge/input.png", request
        if prompt == "lost-job":
            history = f"/history/{prompt_id}"
            assert [route for route in server.gets if route in (history, "/queue")] == [history, "/queue", history]
        records[prompt] = folder
    assert records.keys() == JOB_PROMPTS, records
    source = records["watercolor"] / "input.png"
    assert len(server.uploads) == 1 and source.read_bytes() in server.uploads[0], "Wrong viewport input was uploaded"
    capture = read_json(records["watercolor"] / "input.png.capture.json")
    assert capture["source"] == "viewport" and not capture["includesUi"], capture
    with Image.open(source) as image:
        assert image.size == (240, 180), image.size

    def name(frame):
        return "capture" if frame == frames else f"capture.frame-{frame:04d}"

    states = {frame: json.loads((output / (name(frame) + ".telemetry.json")).read_text(encoding="utf-8"))
              for frame in range(1, frames + 1)}
    assert "comfyUi" not in states[1], states[1].keys()
    pending, applied, restyle, invalid, rejected, failure, lost = [states[frame]["comfyUi"]
        for frame in (2, 3, 4, failure_start, failure_start + 1, failure_start + 2, frames)]
    active = Path(applied["activeEnvironment"])
    assert pending["phase"] == "ready" and pending["activeEnvironment"] != applied["activeEnvironment"], pending
    assert applied["resultStatus"] == "Generated environment applied" and active == records["studio"] / "generated.hdr", applied
    assert restyle["phase"] == "ready" and restyle["previewReady"], restyle
    assert Path(restyle["result"]) == records["watercolor"] / "generated.png", restyle
    assert Path(restyle["result"]).read_bytes() == server.outputs["watercolor.png"], restyle
    assert invalid["phase"] == "ready" and invalid["activeEnvironment"] == applied["activeEnvironment"], invalid
    assert rejected["activeEnvironment"] == applied["activeEnvironment"] and "2:1 equirectangular" in rejected["resultStatus"], rejected
    assert failure["phase"] == "failed" and "Mock model unavailable" in failure["message"], failure
    assert failure["activeEnvironment"] == applied["activeEnvironment"], failure
    assert lost["phase"] == "failed" and lost["message"] == LOST_JOB_MESSAGE, lost
    assert lost["id"] == records["lost-job"].name and lost["activeEnvironment"] == applied["activeEnvironment"], lost
    assert not list(records["lost-job"].glob("generated.*")), lost
    before = states[frames - 1]["frameHistory"][-1]["frameIndex"]
    after = states[frames]["frameHistory"][-1]["frameIndex"]
    assert after > before + 1, f"No intermediate rendered frames while waiting for the lost job: {before} → {after}"
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
        "previewRecreation": args.check_preview_recreation, "captures": frames, "mockHttp": True,
        "applied": applied, "restyle": restyle, "rejected": rejected, "failure": failure, "lostJob": lost,
        "lostJobInterveningFrames": after - before - 1}, indent=2))
    print("PASS generated HDR publication, rejected HDR retention, separate restyle preview, and failed and lost jobs")


if __name__ == "__main__":
    main()
