"""Drive a fresh hidden renderer through its localhost MCP endpoint."""

import argparse
import base64
import http.client
import json
import os
from pathlib import Path
import secrets
import shutil
import socket
import struct
import subprocess
import time


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
            if "Renderer command timed out" not in str(error):
                raise
        time.sleep(.05)
    raise AssertionError("Timed out waiting for a renderer MCP result")


def triangle(path):
    data = struct.pack("<9f", -.8, -.5, 0, .8, -.5, 0, 0, .7, 0)
    document = {
        "asset": {"version": "2.0"},
        "buffers": [{"byteLength": len(data),
                     "uri": "data:application/octet-stream;base64," + base64.b64encode(data).decode()}],
        "bufferViews": [{"buffer": 0, "byteLength": len(data), "target": 34962}],
        "accessors": [{"bufferView": 0, "componentType": 5126, "count": 3, "type": "VEC3",
                       "min": [-.8, -.5, 0], "max": [.8, .7, 0]}],
        "meshes": [{"primitives": [{"attributes": {"POSITION": 0}}]}],
        "nodes": [{"mesh": 0}],
        "scenes": [{"nodes": [0]}],
        "scene": 0,
    }
    path.write_text(json.dumps(document), encoding="utf-8")


def exercise(client, root, *, shader_reload, environment=None):
    evidence = {}
    status, init = wait_for(
        lambda: client.request("initialize", {"protocolVersion": "2025-11-25",
            "capabilities": {}, "clientInfo": {"name": "renderer-mcp-regression", "version": "1"}}),
        timeout=90, startup=True)
    assert status == 200 and init["result"]["protocolVersion"] == "2025-11-25", init
    assert client.request("notifications/initialized", notification=True) == (202, None)
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
    client.call("renderer_load_model", {"path": "fixture.gltf"})
    def loaded():
        status = client.call("renderer_get_status")
        return status if (not status["modelLoading"] and
                          status["activeModel"].endswith("fixture.gltf")) else None
    wait_for(loaded)

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
    session = client.call("renderer_start_gpu_measurement",
                          {"timer": timer, "warmupFrames": 2, "samples": 6, "timeoutMs": 20000})
    def completed():
        report = client.call("renderer_poll_gpu_measurement", {"id": session["id"]})
        return report if report["status"] != "running" else None
    measurement = wait_for(completed)
    assert measurement["status"] == "complete" and measurement["samples"] == 6, measurement
    assert measurement["lastRecordingSerial"] - measurement["firstRecordingSerial"] >= 5, measurement
    stats = measurement["milliseconds"]
    assert 0 <= stats["min"] <= stats["median"] <= stats["max"] and stats["stddev"] >= 0, stats

    session = client.call("renderer_start_gpu_measurement",
                          {"timer": timer, "warmupFrames": 240, "samples": 240})
    cancelled = client.call("renderer_cancel_gpu_measurement", {"id": session["id"]})
    assert cancelled["status"] == "cancelled", cancelled

    capture = root / "mcp-output.png"
    client.call("renderer_capture", {"path": capture.name})
    wait_for(lambda: capture.is_file() and capture.with_suffix(".png.capture.json").is_file())
    client.call("renderer_capture", {"path": capture.name}, error=True)
    return {"lighting": lighting, "measurement": measurement, "capture": str(capture), "logs": logs,
            **evidence}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--exe", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True, help="New empty evidence directory")
    parser.add_argument("--environment", type=Path, help="Optional native HDR or EXR input")
    parser.add_argument("--shader-reload", action="store_true", help="Require the configured Slang source toolchain")
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
               CONTAINER_MCP_ROOT=str(root), VK_LAYER_SETTINGS_PATH=str(root))
    (root / "vk_layer_settings.txt").write_text("khronos_validation.validate_sync = true\n")
    command = [str(exe), "--model", str(root / "startup.gltf"), "--hidden", "--no-ui", "--validation",
               "--width", "192", "--height", "160", "--no-taa", "--no-bloom", "--no-ddgi", "--msaa", "1"]
    with (root / "renderer.log").open("wb") as log:
        process = subprocess.Popen(command, cwd=exe.parent, env=env, stdout=log, stderr=subprocess.STDOUT)
        try:
            report = exercise(Client(port, token), root, shader_reload=args.shader_reload,
                              environment=environment)
        finally:
            process.terminate()
            process.wait(timeout=20)
    log = (root / "renderer.log").read_text(encoding="utf-8", errors="replace")
    assert not any(marker in log for marker in ("Validation Error", "SYNC-HAZARD", "VUID-")), log
    (root / "results.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(report))


if __name__ == "__main__":
    main()
