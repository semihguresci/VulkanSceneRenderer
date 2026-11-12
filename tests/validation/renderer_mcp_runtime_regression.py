"""Drive a fresh hidden renderer through its localhost MCP endpoint."""

import argparse
import base64
import http.client
import json
import math
import os
from pathlib import Path
import secrets
import shutil
import socket
import struct
import subprocess
import time


WIDTH, HEIGHT = 192, 160
LIT_INTENSITY = 2


class ToolFailure(RuntimeError):
    pass


class Client:
    def __init__(self, port, token):
        self.port, self.token = port, token
        self.next_id = 0

    def request(self, method, params=None, *, headers=None, notification=False):
        self.next_id += 1
        message = {"jsonrpc": "2.0", "method": method, "params": params or {}}
        if not notification:
            message["id"] = self.next_id
        request_headers = {
            "Authorization": f"Bearer {self.token}",
            "Content-Type": "application/json",
            "Accept": "application/json, text/event-stream",
            "MCP-Protocol-Version": "2025-11-25",
            **(headers or {}),
        }
        connection = http.client.HTTPConnection("127.0.0.1", self.port, timeout=4)
        try:
            connection.request("POST", "/mcp", json.dumps(message), request_headers)
            response = connection.getresponse()
            payload = response.read()
            return response.status, json.loads(payload) if payload else None
        finally:
            connection.close()

    def call(self, tool, arguments=None, *, error=False):
        code, body = self.request("tools/call", {"name": tool, "arguments": arguments or {}})
        assert code == 200 and "result" in body, body
        result = body["result"]
        payload = json.loads(result["content"][0]["text"])
        if result["isError"] and not error:
            raise ToolFailure(payload["message"])
        assert result["isError"] is error, result
        return payload


def wait_for(callback, timeout=20, *, startup=False):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        try:
            value = callback()
            if value:
                return value
        except (ConnectionError, TimeoutError, OSError):
            if not startup:
                raise
        except ToolFailure as error:
            # The callbacks only read state, so a response timeout is safe to retry.
            if not str(error).startswith(("Renderer command timed out",
                                         "Renderer did not accept the command before the request timed out",
                                         "Renderer accepted the command, but its response timed out")):
                raise
        time.sleep(.05)
    raise AssertionError("Timed out waiting for a renderer MCP result")


def triangle(path):
    # Startup and model replacement both frame this face from the default -Z view.
    data = struct.pack("<18f", -.8, -.5, 0, 0, .7, 0, .8, -.5, 0, *((0, 0, -1) * 3))
    document = {
        "asset": {"version": "2.0"},
        "buffers": [{"byteLength": len(data),
                     "uri": "data:application/octet-stream;base64," + base64.b64encode(data).decode()}],
        "bufferViews": [{"buffer": 0, "byteLength": 36, "target": 34962},
                        {"buffer": 0, "byteOffset": 36, "byteLength": 36, "target": 34962}],
        "accessors": [{"bufferView": 0, "componentType": 5126, "count": 3, "type": "VEC3",
                       "min": [-.8, -.5, 0], "max": [.8, .7, 0]},
                      {"bufferView": 1, "componentType": 5126, "count": 3, "type": "VEC3"}],
        "materials": [{"pbrMetallicRoughness": {"baseColorFactor": [.5, .5, .5, 1],
                                                "metallicFactor": 0, "roughnessFactor": .6}}],
        "meshes": [{"primitives": [{"attributes": {"POSITION": 0, "NORMAL": 1}, "material": 0}]}],
        "nodes": [{"mesh": 0}],
        "scenes": [{"nodes": [0]}],
        "scene": 0,
    }
    path.write_text(json.dumps(document), encoding="utf-8")


def initialize(client):
    status, init = wait_for(
        lambda: client.request("initialize", {"protocolVersion": "2025-11-25",
            "capabilities": {}, "clientInfo": {"name": "renderer-mcp-regression", "version": "1"}}),
        timeout=90, startup=True)
    assert status == 200 and init["result"]["protocolVersion"] == "2025-11-25", init
    assert client.request("notifications/initialized", notification=True) == (202, None)


def remote_capture(client, root, name):
    image = root / name
    assert client.call("renderer_capture", {"path": name})["queued"]

    def saved():
        status = client.call("renderer_get_status")
        assert not status["captureStatus"].startswith("Capture failed:"), status
        return (not status["capturePending"] and status["captureStatus"] == "Saved" and
                image.is_file() and Path(f"{image}.capture.json").is_file())

    wait_for(saved)
    return image


def exercise_rendered_lighting(client, root, *, replace_model=None):
    original = client.call("renderer_get_settings")["lighting"]
    assert original["directionalIntensity"] == original["environmentIntensity"] == original["bounceIntensity"] == 0, original
    captures = {"off": remote_capture(client, root, "mcp-lighting-off.exr")}
    reply = client.call("renderer_set_lighting", {"directionalIntensity": LIT_INTENSITY})
    lit = {**original, "directionalIntensity": LIT_INTENSITY}
    assert reply["lighting"] == lit, reply
    captures["on"] = remote_capture(client, root, "mcp-lighting-on.exr")
    if replace_model:
        replace_model()
        actual = client.call("renderer_get_settings")["lighting"]
        assert actual == lit, f"Model replacement changed live lighting: {actual}"
    captures["onRepeat"] = remote_capture(client, root, "mcp-lighting-on-repeat.exr")
    reply = client.call("renderer_set_lighting", {"directionalIntensity": 0})
    assert reply["lighting"] == original, reply
    captures["offRestored"] = remote_capture(client, root, "mcp-lighting-off-restored.exr")
    return captures


def read_scene_linear(path):
    os.environ.setdefault("OPENCV_IO_ENABLE_OPENEXR", "1")
    import cv2
    import numpy as np

    metadata = json.loads(Path(f"{path}.capture.json").read_bytes())
    assert (metadata["width"], metadata["height"]) == (WIDTH, HEIGHT), metadata
    assert metadata["source"] == "raster-scene-composite" and metadata["colorSpace"] == "linear-sRGB", metadata
    assert not (metadata["includesUi"] or metadata["exposureApplied"] or metadata["toneMappingApplied"]), metadata
    bgr = cv2.imdecode(np.frombuffer(path.read_bytes(), dtype=np.uint8), cv2.IMREAD_UNCHANGED)
    assert bgr is not None and bgr.shape == (HEIGHT, WIDTH, 3) and bgr.dtype == np.float32, path
    assert np.isfinite(bgr).all(), path
    return bgr[:, :, ::-1]


def center_rgb(image):
    return image[HEIGHT // 2, WIDTH // 2].tolist()


def verify_rendered_lighting(remote, scripted):
    import numpy as np

    captures = {name: read_scene_linear(path) for name, path in remote.items()}
    controls = {name: read_scene_linear(path) for name, path in scripted.items()}
    centers = {name: center_rgb(image) for name, image in captures.items()}
    control_centers = {name: center_rgb(image) for name, image in controls.items()}
    assert max(abs(value) for value in control_centers["off"]) <= 1e-5, control_centers
    assert min(control_centers["on"]) > .05, control_centers
    assert min(centers["on"]) > .05, (
        f"MCP accepted directional intensity {LIT_INTENSITY} but did not light the receiver. "
        f"Remote EXR center {centers['on']}, scripted CLI control {control_centers['on']}.")
    np.testing.assert_array_equal(captures["off"], captures["offRestored"])
    np.testing.assert_array_equal(captures["on"], captures["onRepeat"])
    for name in ("off", "on"):
        np.testing.assert_array_equal(captures[name], controls[name])
    return {"acceptedDirectionalIntensity": LIT_INTENSITY, "decodedPixelsMatchScriptedControls": True,
            "repeatedAndRestoredPixelsMatch": True, "remoteCenterRgb": centers, "scriptedCenterRgb": control_centers,
            "captures": {name: str(path) for name, path in {**remote, "scriptedOff": scripted["off"],
                                                          "scriptedOn": scripted["on"]}.items()}}


def exercise(client, root, *, shader_reload, environment=None):
    evidence = {}
    initialize(client)
    assert client.request("ping", headers={"Authorization": "Bearer invalid"})[0] == 401
    assert client.request("ping", headers={"Origin": "https://example.invalid"})[0] == 403
    assert client.request("ping", headers={"Host": "example.invalid"})[0] == 403
    _, listed = client.request("tools/list")
    names = {tool["name"] for tool in listed["result"]["tools"]}
    assert {"renderer_load_model", "renderer_set_lighting",
            "renderer_start_gpu_measurement", "renderer_capture"} <= names

    lighting_schema = next(tool for tool in listed["result"]["tools"] if tool["name"] == "renderer_set_lighting")["inputSchema"]
    ranges = {"environmentIntensity": (0, 16), "directionalIntensity": (0, 16),
              "bounceIntensity": (0, 2), "environmentYaw": (-180, 180)}
    assert lighting_schema["additionalProperties"] is False and set(lighting_schema["properties"]) == set(ranges)
    original_lighting = client.call("renderer_get_settings")["lighting"]
    for name, (low, high) in ranges.items():
        assert lighting_schema["properties"][name] == {"type": "number", "minimum": low, "maximum": high}
        for boundary in (low, high):
            client.call("renderer_set_lighting", {name: boundary})
            actual = client.call("renderer_get_settings")["lighting"]
            assert actual[name] == boundary and all(actual[key] == original_lighting[key] for key in ranges if key != name), actual
            client.call("renderer_set_lighting", original_lighting)
        for invalid in (low - .001, high + .001):
            # A changed valid field must not publish when any sibling is invalid.
            other = "bounceIntensity" if name != "bounceIntensity" else "directionalIntensity"
            value = .375 if original_lighting[other] != .375 else .5
            client.call("renderer_set_lighting", {other: value, name: invalid}, error=True)
            assert client.call("renderer_get_settings")["lighting"] == original_lighting
    for invalid in (16.0000001, -1e-8):
        client.call("renderer_set_lighting", {"environmentIntensity": invalid}, error=True)
        assert client.call("renderer_get_settings")["lighting"] == original_lighting
    evidence["lightingBoundaries"] = dict(fields=4, acceptedEndpoints=8, rejectedAtomicRequests=10)

    client.call("renderer_set_lighting", {"environmentIntensity": 1e6}, error=True)
    client.call("renderer_load_model", {"path": "../escape.gltf"}, error=True)
    external = json.loads((root / "fixture.gltf").read_text(encoding="utf-8"))
    external["buffers"][0]["uri"] = "../escape.bin"
    (root / "external.gltf").write_text(json.dumps(external), encoding="utf-8")
    client.call("renderer_load_model", {"path": "external.gltf"})
    def rejected():
        status = client.call("renderer_get_status")
        return status if not status["modelLoading"] else None
    rejected_status = wait_for(rejected)
    assert "self-contained glTF cannot reference external buffers" in rejected_status["modelStatus"], rejected_status
    assert rejected_status["activeModel"].endswith("startup.gltf"), rejected_status
    logs = client.call("renderer_recent_logs", {"limit": 8})["entries"]
    assert 1 <= len(logs) <= 8 and any(entry["event"] == "Model load failed" for entry in logs), logs
    assert all(set(entry) == {"component", "severity", "event", "recordedAtUnixMs"} for entry in logs), logs
    serialized_logs = json.dumps(logs)
    assert "external.gltf" not in serialized_logs and client.token not in serialized_logs
    def loaded():
        status = client.call("renderer_get_status")
        return status if (not status["modelLoading"] and
                          status["activeModel"].endswith("fixture.gltf")) else None

    def replace_model():
        client.call("renderer_load_model", {"path": "fixture.gltf"})
        wait_for(loaded)

    rendered = exercise_rendered_lighting(client, root, replace_model=replace_model)
    evidence["lightingRetainedAcrossModelLoad"] = True

    if environment:
        client.call("renderer_load_environment", {"path": environment})
        def environment_ready():
            status = client.call("renderer_get_status")
            return status if not status["environmentLoading"] else None
        ready = wait_for(environment_ready, timeout=90)
        assert ready["environmentStatus"].startswith("Environment HDR loaded and IBL generated"), ready
        assert environment in ready["environmentStatus"], ready
        evidence["environmentStatus"] = ready["environmentStatus"]

    changed = {"environmentIntensity": .35, "directionalIntensity": .9, "environmentYaw": 15}
    client.call("renderer_set_lighting", changed)
    lighting = client.call("renderer_get_settings")["lighting"]
    assert all(abs(lighting[name] - value) < 1e-6 for name, value in changed.items()), lighting

    if shader_reload:
        generation = client.call("renderer_get_status")["shaderReloadGeneration"]
        client.call("renderer_reload_shaders")
        def reloaded():
            status = client.call("renderer_get_status")
            return status if not status["shaderReloadBusy"] else None
        result = wait_for(reloaded, timeout=150)
        assert result["shaderReloadGeneration"] > generation, result
        evidence["shaderReloadGeneration"] = result["shaderReloadGeneration"]

    def timers_ready():
        result = client.call("renderer_list_gpu_timers")
        return result if result["timers"] else None
    timers = wait_for(timers_ready)
    timer = "Lighting" if "Lighting" in timers["timers"] else timers["timers"][0]
    client.call("renderer_start_gpu_measurement", {"timer": "missing"}, error=True)

    def terminal(session):
        def status():
            report = client.call("renderer_poll_gpu_measurement", {"id": session["id"]})
            return report if report["status"] != "running" else None
        return wait_for(status)

    def measure():
        session = client.call("renderer_start_gpu_measurement",
                              {"timer": timer, "warmupFrames": 2, "samples": 6, "timeoutMs": 20000})
        measurement = terminal(session)
        assert measurement["status"] == "complete" and measurement["samples"] == 6, measurement
        assert measurement["lastRecordingSerial"] - measurement["firstRecordingSerial"] >= 5, measurement
        stats = measurement["milliseconds"]
        assert all(math.isfinite(value) for value in stats.values()), stats
        assert 0 <= stats["min"] <= stats["median"] <= stats["max"] and stats["stddev"] >= 0, stats
        assert stats["min"] - 1e-9 <= stats["mean"] <= stats["max"] + 1e-9, stats
        return measurement

    measurement, repeated = measure(), measure()
    assert repeated["timer"] == measurement["timer"] and repeated["source"] == measurement["source"], repeated
    assert repeated["firstRecordingSerial"] > measurement["lastRecordingSerial"], (measurement, repeated)
    evidence["repeatedMeasurement"] = repeated

    timeout = client.call("renderer_start_gpu_measurement",
                          {"timer": timer, "warmupFrames": 240, "samples": 240, "timeoutMs": 100})
    timed_out = terminal(timeout)
    assert timed_out["status"] == "timedOut" and timed_out["samples"] < 240 and timed_out["reason"], timed_out
    evidence["timedOutMeasurement"] = timed_out

    session = client.call("renderer_start_gpu_measurement",
                          {"timer": timer, "warmupFrames": 240, "samples": 240})
    cancelled = client.call("renderer_cancel_gpu_measurement", {"id": session["id"]})
    assert cancelled["status"] == "cancelled", cancelled

    capture = remote_capture(client, root, "mcp-output.png")
    client.call("renderer_capture", {"path": capture.name}, error=True)
    return ({"lighting": lighting, "measurement": measurement, "capture": str(capture), "logs": logs,
             **evidence}, rendered)


def renderer_command(exe, model, intensity=0):
    return [
        str(exe), "--model", str(model), "--hidden", "--no-ui", "--validation",
        "--width", str(WIDTH), "--height", str(HEIGHT), "--no-taa", "--no-bloom", "--no-ddgi", "--msaa", "1",
        "--render-technique", "deferred-raster", "--display-mode", "lit", "--ray-shadows", "raster",
        "--sky", "plain", "--sky-horizon", "0", "0", "0", "--import-scale", "1",
        "--environment-intensity", "0", "--directional-intensity", str(intensity), "--bounce-intensity", "0",
        "--directional-color", "1", "1", "1", "--directional-direction", "0", "0", "1",
    ]


def validate_log(path):
    log = path.read_text(encoding="utf-8", errors="replace")
    assert not any(marker in log for marker in ("Validation Error", "SYNC-HAZARD", "VUID-")), path
    assert 'Insert instance layer "VK_LAYER_KHRONOS_validation"' in log, path
    return log


def scripted_capture(exe, root, model, env, name, intensity):
    image, log = root / f"{name}.exr", root / f"{name}.log"
    command = renderer_command(exe, model, intensity)
    command += ["--capture-frame", "3", "--screenshot", str(image)]
    # A correctly isolated scripted capture never attempts to bind this occupied port.
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as occupied:
        occupied.bind(("127.0.0.1", 0))
        occupied.listen(1)
        scripted_env = {**env, "CONTAINER_MCP_PORT": str(occupied.getsockname()[1])}
        with log.open("wb") as output:
            process = subprocess.run(command, cwd=exe.parent, env=scripted_env, stdout=output,
                                     stderr=subprocess.STDOUT, timeout=120)
    assert process.returncode == 0, log
    text = validate_log(log)
    assert "Renderer MCP endpoint " not in text, log
    return image


def verify_endpoint_disabled(exe, root, env, port):
    log = root / "endpoint-disabled-interactive.log"
    command = renderer_command(exe, root / "startup.gltf")
    with log.open("wb") as output:
        process = subprocess.Popen(command, cwd=exe.parent, env=env, stdout=output, stderr=subprocess.STDOUT)
        try:
            def initialized():
                assert process.poll() is None, f"Disabled renderer exited during initialization. See {log}"
                return "Initializing Vulkan renderer" in log.read_text(encoding="utf-8", errors="replace")
            wait_for(initialized, timeout=90)
            deadline = time.monotonic() + 1
            while time.monotonic() < deadline:
                assert process.poll() is None, f"Disabled renderer exited. See {log}"
                with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as probe:
                    if os.name == "nt":
                        probe.setsockopt(socket.SOL_SOCKET, socket.SO_EXCLUSIVEADDRUSE, 1)
                    try:
                        probe.bind(("127.0.0.1", port))
                    except OSError as error:
                        raise AssertionError(f"MCP port {port} is in use with the endpoint disabled") from error
                time.sleep(.05)
        finally:
            process.terminate()
            process.wait(timeout=20)
    assert "Renderer MCP endpoint " not in validate_log(log), log
    image = scripted_capture(exe, root, root / "startup.gltf", env,
                             "endpoint-disabled-scripted", LIT_INTENSITY)
    center = center_rgb(read_scene_linear(image))
    assert min(center) > .05, center
    return {"interactiveListenerAbsent": True, "scriptedListenerAbsent": True, "scriptedCenterRgb": center,
            "capture": str(image), "interactiveLog": str(log)}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--exe", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True, help="New empty evidence directory")
    parser.add_argument("--environment", type=Path, help="Optional native HDR or EXR input")
    parser.add_argument("--shader-reload", action="store_true", help="Require the configured Slang source toolchain")
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument("--rendered-lighting-only", action="store_true", help="Run only the directional EXR witness")
    mode.add_argument("--endpoint-disabled-only", action="store_true", help="Verify an ENABLE_RENDERER_MCP=OFF build")
    args = parser.parse_args()
    exe, root = args.exe.resolve(strict=True), args.output.resolve()
    root.mkdir(parents=True, exist_ok=False)
    triangle(root / "startup.gltf")
    triangle(root / "fixture.gltf")
    environment = None
    if args.environment:
        source = args.environment.resolve(strict=True)
        environment = "mcp-environment" + source.suffix.lower()
        shutil.copyfile(source, root / environment)
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as listener:
        listener.bind(("127.0.0.1", 0))
        port = listener.getsockname()[1]
    token = secrets.token_hex(24)
    env = dict(os.environ, CONTAINER_MCP_PORT=str(port), CONTAINER_MCP_TOKEN=token,
               CONTAINER_MCP_ROOT=str(root), VK_LAYER_SETTINGS_PATH=str(root), VK_LOADER_DEBUG="layer")
    (root / "vk_layer_settings.txt").write_text("khronos_validation.validate_sync = true\n")
    if args.endpoint_disabled_only:
        report = verify_endpoint_disabled(exe, root, env, port)
    else:
        command = renderer_command(exe, root / "startup.gltf")
        log_path = root / "renderer.log"
        with log_path.open("wb") as log:
            process = subprocess.Popen(command, cwd=exe.parent, env=env, stdout=log, stderr=subprocess.STDOUT)
            try:
                client = Client(port, token)
                if args.rendered_lighting_only:
                    initialize(client)
                    remote = exercise_rendered_lighting(client, root)
                    report = {}
                else:
                    report, remote = exercise(client, root, shader_reload=args.shader_reload, environment=environment)
            finally:
                process.terminate()
                process.wait(timeout=20)
        validate_log(log_path)
        model = root / ("startup.gltf" if args.rendered_lighting_only else "fixture.gltf")
        controls = {"off": scripted_capture(exe, root, model, env, "scripted-lighting-off", 0),
                    "on": scripted_capture(exe, root, model, env, "scripted-lighting-on", LIT_INTENSITY)}
        report["renderedLighting"] = verify_rendered_lighting(remote, controls)
    (root / "results.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(report))


if __name__ == "__main__":
    main()
