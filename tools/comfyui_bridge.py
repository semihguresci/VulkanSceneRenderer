"""Run optional Container generation jobs against a local ComfyUI server."""

import argparse
from contextlib import contextmanager
import http.client
import io
import json
import math
import os
from pathlib import Path, PurePosixPath
import re
import tempfile
import time
from urllib.parse import quote, urlencode


MAX_ASSET = 64 * 1024 * 1024
MAX_JSON = 4 * 1024 * 1024
PNG = b"\x89PNG\r\n\x1a\n"
JOB_ID = re.compile(r"[0-9a-f]{32}\Z")
PROMPT_ID = re.compile(r"[0-9a-fA-F-]{32,36}\Z")
TERMINAL = {"succeeded", "failed", "cancelled"}


def read_bytes(path, limit):
    if not path.is_file() or path.resolve(strict=True) != path:
        raise ValueError(f"Expected a regular file at {path.name}")
    with path.open("rb") as source:
        data = source.read(limit + 1)
    if not data or len(data) > limit:
        raise ValueError(f"File {path.name} is empty or exceeds its size limit")
    return data


def read_json(path, limit=MAX_JSON):
    result = json.loads(read_bytes(path, limit))
    if not isinstance(result, dict):
        raise ValueError(f"Expected a JSON object in {path.name}")
    return result


def atomic_write(path, data):
    with tempfile.NamedTemporaryFile(dir=path.parent, prefix=".bridge-", delete=False) as output:
        temporary = Path(output.name)
        try:
            output.write(data)
        except BaseException:
            output.close()
            temporary.unlink(missing_ok=True)
            raise
    try:
        for attempt in range(11):
            try:
                os.replace(temporary, path)
                return
            except PermissionError:
                if attempt == 10:
                    raise
                time.sleep(.02)
    finally:
        temporary.unlink(missing_ok=True)


@contextmanager
def adapter_lock(root):
    path = root / ".adapter.lock"
    if path.is_symlink() or (path.exists() and path.resolve() != path):
        raise ValueError("Adapter lock must be inside the bridge root")
    with path.open("a+b") as handle:
        try:
            if os.name == "nt":
                import msvcrt

                if handle.tell() == 0:
                    handle.write(b"\x00")
                    handle.flush()
                handle.seek(0)
                msvcrt.locking(handle.fileno(), msvcrt.LK_NBLCK, 1)
            else:
                import fcntl

                fcntl.flock(handle, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except OSError as error:
            raise RuntimeError("Another adapter is already watching this directory") from error
        yield


class ComfyError(RuntimeError):
    def __init__(self, status, message):
        super().__init__(message)
        self.status = status


class ComfyClient:
    def __init__(self, port):
        self.port = port

    def request(self, method, route, body=None, content_type="application/json", limit=MAX_JSON):
        connection = http.client.HTTPConnection("127.0.0.1", self.port, timeout=4)
        try:
            connection.request(method, route, body, {"Content-Type": content_type} if body is not None else {})
            response = connection.getresponse()
            if not 200 <= response.status < 300:
                detail = response.read(512).decode("utf-8", "replace")
                raise ComfyError(response.status, f"ComfyUI HTTP {response.status}: {detail}")
            data = response.read(limit + 1)
            if len(data) > limit:
                raise ValueError("ComfyUI response exceeds its size limit")
            return data
        finally:
            connection.close()

    def json(self, method, route, payload=None):
        response = self.request(method, route, json.dumps(payload).encode() if payload is not None else None)
        value = json.loads(response) if response else {}
        if not isinstance(value, dict):
            raise ValueError("Expected a JSON object from ComfyUI")
        return value

    def upload(self, job_id, source):
        if not source.startswith(PNG):
            raise ValueError("The viewport capture is not a PNG")
        boundary = f"ContainerComfyBridge{job_id}"
        body = (f'--{boundary}\r\nContent-Disposition: form-data; name="image"; '
                f'filename="container-{job_id}.png"\r\nContent-Type: image/png\r\n\r\n').encode()
        body += source + f"\r\n--{boundary}--\r\n".encode()
        result = json.loads(self.request("POST", "/upload/image", body, f"multipart/form-data; boundary={boundary}"))
        return media_path(result.get("name"), result.get("subfolder", ""))

    def cancel(self, prompt_id):
        try:
            self.json("POST", f"/api/jobs/{quote(prompt_id, safe='')}/cancel", {})
        except ComfyError as error:
            if error.status not in (404, 405):
                raise
            # Older servers may provide only a global running interruption.
            # Delete the queued prompt by ID and let an already running one finish.
            self.json("POST", "/queue", {"delete": [prompt_id]})


def media_path(filename, subfolder):
    if not isinstance(filename, str) or not filename or any(c in filename for c in "/\\:\x00"):
        raise ValueError("ComfyUI returned an invalid filename")
    if filename in (".", "..") or not isinstance(subfolder, str):
        raise ValueError("ComfyUI returned an invalid file location")
    subfolder = subfolder.replace("\\", "/")
    path = PurePosixPath(subfolder)
    if path.is_absolute() or ".." in path.parts or any(c in subfolder for c in ":\x00"):
        raise ValueError("ComfyUI returned a file location outside its output folder")
    return str(path / filename)


def fill_workflow(template, request, image):
    substitutions = {"${prompt}": request["prompt"], "${seed}": request["seed"], "${image}": image}
    seen = set()

    def replace(value):
        if isinstance(value, str) and value in substitutions:
            seen.add(value)
            replacement = substitutions[value]
            if replacement is None:
                raise ValueError("An HDRI workflow cannot reference a viewport image")
            return replacement
        if isinstance(value, list):
            return [replace(item) for item in value]
        if isinstance(value, dict):
            return {key: replace(item) for key, item in value.items()}
        return value

    workflow = {}
    for node_id, node in template.items():
        if not isinstance(node, dict) or not isinstance(node.get("class_type"), str) or not isinstance(node.get("inputs"), dict):
            raise ValueError("Expected a ComfyUI API-format workflow export")
        workflow[node_id] = dict(node, inputs=replace(node["inputs"]))
    required = {"${prompt}", "${seed}"}
    if request["kind"] == "restyle":
        required.add("${image}")
    if not required <= seen:
        raise ValueError("Workflow inputs are missing placeholders: " + ", ".join(sorted(required - seen)))
    return workflow


def select_output(record, kind):
    candidates = []
    outputs = record.get("outputs", {})
    if not isinstance(outputs, dict):
        raise ValueError("ComfyUI returned invalid workflow outputs")
    for node in outputs.values():
        if not isinstance(node, dict):
            continue
        for key in ("images", "files"):
            entries = node.get(key, [])
            if not isinstance(entries, list):
                continue
            for item in entries:
                if not isinstance(item, dict) or item.get("type") != "output":
                    continue
                suffix = Path(media_path(item.get("filename"), item.get("subfolder", ""))).suffix.lower()
                if suffix == ".png" or (kind == "hdri" and suffix in (".hdr", ".exr")):
                    candidates.append((suffix == ".png", suffix, item))
    if not candidates:
        raise ValueError("ComfyUI workflow completed without a supported saved output image")
    _, suffix, item = min(candidates, key=lambda candidate: candidate[:2])
    query = urlencode({"filename": item["filename"], "subfolder": item.get("subfolder", ""), "type": "output"})
    return suffix, f"/view?{query}"


def png_to_hdr(data):
    # Pillow ships with ComfyUI. It is only needed for this optional conversion.
    from PIL import Image

    with Image.open(io.BytesIO(data)) as source:
        width, height = source.size
        if source.format != "PNG" or width != 2 * height or not 8 <= width <= 4096:
            raise ValueError("PNG environment output must be a 2:1 panorama up to 4096 pixels wide")
        pixels = source.convert("RGB").tobytes()
    linear = [v / 12.92 if v <= .04045 else ((v + .055) / 1.055) ** 2.4 for v in (n / 255 for n in range(256))]
    output = bytearray(f"#?RADIANCE\nFORMAT=32-bit_rle_rgbe\n\n-Y {height} +X {width}\n".encode())
    for y in range(height):
        channels = [bytearray(width) for _ in range(4)]
        for x in range(width):
            index = 3 * (y * width + x)
            r, g, b = (linear[v] for v in pixels[index:index + 3])
            peak = max(r, g, b)
            if peak:
                _, exponent = math.frexp(peak)
                scale = math.ldexp(256., -exponent)
                channels[0][x], channels[1][x], channels[2][x] = int(r * scale), int(g * scale), int(b * scale)
                channels[3][x] = exponent + 128
        output.extend((2, 2, width >> 8, width & 255))
        for channel in channels:
            for offset in range(0, width, 128):
                literal = channel[offset:offset + 128]
                output.append(len(literal))
                output.extend(literal)
    return bytes(output)


def convert_output(suffix, data, kind):
    if suffix == ".png":
        if not data.startswith(PNG):
            raise ValueError("ComfyUI output does not have a PNG signature")
        if kind == "hdri":
            return ".hdr", png_to_hdr(data)
        return suffix, data
    if suffix == ".exr" and data.startswith(b"\x76\x2f\x31\x01"):
        return suffix, data
    if suffix == ".hdr" and data.startswith((b"#?RADIANCE\n", b"#?RGBE\n")):
        return suffix, data
    raise ValueError("ComfyUI output does not have the expected HDR or EXR signature")


class Adapter:
    def __init__(self, root, client, workflows):
        self.jobs = root / "jobs"
        self.jobs.mkdir(exist_ok=True)
        if self.jobs.resolve(strict=True) != self.jobs:
            raise ValueError("Jobs directory must be inside the bridge root")
        self.client = client
        self.workflows = workflows

    def status(self, folder, phase, message, progress=0., **extra):
        record = dict(version=1, id=folder.name, phase=phase, message=message[:1024], progress=progress, **extra)
        atomic_write(folder / "status.json", json.dumps(record, ensure_ascii=True).encode())

    def step(self, folder):
        status_path = folder / "status.json"
        old = read_json(status_path, 8192) if status_path.exists() else {}
        if old.get("version") != 1 or old.get("id") != folder.name:
            old = {}
        if old.get("phase") in TERMINAL:
            return
        request = read_json(folder / "request.json", 8192)
        kind = request.get("kind")
        prompt = request.get("prompt")
        seed = request.get("seed")
        expires = request.get("expiresAtUnix")
        if (request.get("version") != 1 or request.get("id") != folder.name or kind not in ("hdri", "restyle") or
                not isinstance(prompt, str) or not prompt.strip() or len(prompt.encode()) > 2048 or
                type(seed) is not int or not 0 <= seed < 2**64 or type(expires) is not int):
            raise ValueError("Invalid Container generation request")
        prompt_id = old.get("promptId")
        if prompt_id is not None and (not isinstance(prompt_id, str) or not PROMPT_ID.fullmatch(prompt_id)):
            raise ValueError("Invalid persisted ComfyUI prompt ID")
        if (folder / "cancel.json").exists() or time.time() >= expires:
            try:
                if prompt_id:
                    self.client.cancel(prompt_id)
                message = "Generation cancelled or expired"
            except (OSError, RuntimeError) as error:
                message = f"Result discarded. ComfyUI cancellation request failed: {error}"
            self.status(folder, "cancelled", message)
            return
        if prompt_id is None:
            if old:
                raise RuntimeError("Previous adapter exited during submission. Start a new generation request")
            template = self.workflows.get(kind)
            if template is None:
                raise ValueError(f"No {kind} API workflow configured on the adapter")
            self.status(folder, "queued", "Submitting local ComfyUI workflow", .15)
            image = self.client.upload(folder.name, read_bytes(folder / "input.png", MAX_ASSET)) if kind == "restyle" else None
            workflow = fill_workflow(template, request, image)
            response = self.client.json("POST", "/prompt", {"prompt": workflow, "client_id": folder.name})
            prompt_id = response.get("prompt_id")
            if not isinstance(prompt_id, str) or not PROMPT_ID.fullmatch(prompt_id):
                raise ValueError("ComfyUI did not return a valid prompt ID")
            self.status(folder, "queued", "Queued in ComfyUI", .25, promptId=prompt_id)
            return
        history = self.client.json("GET", f"/history/{quote(prompt_id, safe='')}").get(prompt_id)
        if history is None:
            queue = self.client.json("GET", "/queue")
            running = any(isinstance(item, list) and len(item) > 1 and item[1] == prompt_id
                          for item in queue.get("queue_running", []))
            phase, progress = ("running", .60) if running else ("queued", .25)
            if old.get("phase") != phase:
                self.status(folder, phase, "ComfyUI is generating" if running else "Queued in ComfyUI", progress, promptId=prompt_id)
            return
        execution = history.get("status", {})
        if execution.get("status_str") != "success" or not execution.get("completed"):
            messages = execution.get("messages", [])
            raise RuntimeError(f"ComfyUI workflow failed: {str(messages)[:700]}")
        self.status(folder, "running", "Downloading generated output", .85, promptId=prompt_id)
        suffix, route = select_output(history, kind)
        suffix, data = convert_output(suffix, self.client.request("GET", route, limit=MAX_ASSET), kind)
        if (folder / "cancel.json").exists() or time.time() >= expires:
            self.status(folder, "cancelled", "Generated result discarded after cancellation")
            return
        output = "generated" + suffix
        atomic_write(folder / output, data)
        self.status(folder, "succeeded", "Generated output is ready", 1., output=output)

    def poll(self):
        for folder in self.jobs.iterdir():
            if not JOB_ID.fullmatch(folder.name) or not folder.is_dir() or folder.resolve() != folder:
                continue
            if not (folder / "request.json").is_file():
                continue
            try:
                self.step(folder)
            except (OSError, RuntimeError, ValueError, TypeError, AttributeError) as error:
                self.status(folder, "failed", str(error))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, required=True)
    parser.add_argument("--port", type=int, default=8188)
    parser.add_argument("--hdri-workflow", type=Path)
    parser.add_argument("--restyle-workflow", type=Path)
    args = parser.parse_args()
    if not 1 <= args.port <= 65535:
        parser.error("--port must be between 1 and 65535")
    workflows = {kind: read_json(path.resolve(strict=True)) for kind, path in
                 (("hdri", args.hdri_workflow), ("restyle", args.restyle_workflow)) if path is not None}
    if not workflows:
        parser.error("configure at least one API-format workflow")
    root = args.root.resolve(strict=True)
    try:
        with adapter_lock(root):
            adapter = Adapter(root, ComfyClient(args.port), workflows)
            print(f"Watching {adapter.jobs} with ComfyUI on 127.0.0.1:{args.port}", flush=True)
            while True:
                adapter.poll()
                time.sleep(.5)
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
