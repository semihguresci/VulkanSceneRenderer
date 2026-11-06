"""Exercise the adapter's HTTP and spool contracts against a loopback ComfyUI mock."""

from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import io
import json
from pathlib import Path
import sys
import tempfile
import threading
import time
import unittest
from urllib.parse import parse_qs, urlsplit

from PIL import Image

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools"))
from comfyui_bridge import Adapter, ComfyClient, adapter_lock, atomic_write, read_json


PROMPT_ID = "01234567-89ab-cdef-0123-456789abcdef"


def png(width=8, height=4):
    output = io.BytesIO()
    Image.new("RGB", (width, height), (128, 64, 32)).save(output, format="PNG")
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
        payload = self.rfile.read(int(self.headers.get("Content-Length", 0)))
        self.server.posts.append((self.path, payload))
        if self.path == "/upload/image":
            self.reply({"name": "source.png", "subfolder": "test", "type": "input"})
        elif self.path == "/prompt":
            self.reply({"prompt_id": PROMPT_ID})
        elif self.path == f"/api/jobs/{PROMPT_ID}/cancel":
            self.reply({}, 200 if self.server.targeted_cancel else 404)
        elif self.path == "/queue":
            self.reply({})
        else:
            self.reply({}, 404)

    def do_GET(self):
        if self.path == "/queue":
            self.reply({"queue_running": [[0, PROMPT_ID]]})
        elif self.path == f"/history/{PROMPT_ID}":
            self.reply({PROMPT_ID: self.server.history} if self.server.history else {})
        elif urlsplit(self.path).path == "/view":
            self.server.view = parse_qs(urlsplit(self.path).query)
            self.reply(self.server.output)
        else:
            self.reply({}, 404)


def success(filename="generated image.png"):
    return {"status": {"status_str": "success", "completed": True}, "outputs": {
        "9": {"images": [{"filename": filename, "subfolder": "local output", "type": "output"}]}}}


class AdapterTest(unittest.TestCase):
    def setUp(self):
        scratch = tempfile.TemporaryDirectory()
        self.addCleanup(scratch.cleanup)
        self.root = Path(scratch.name).resolve()
        server = ThreadingHTTPServer(("127.0.0.1", 0), ComfyMock)
        self.addCleanup(server.server_close)
        self.addCleanup(server.shutdown)
        threading.Thread(target=server.serve_forever, daemon=True).start()
        self.server = server
        server.posts = []
        server.history = None
        server.targeted_cancel = True
        server.output = png()
        workflows = {
            "hdri": {"7": {"class_type": "MockPanorama", "inputs": {"text": "${prompt}", "seed": "${seed}"}}},
            "restyle": {"4": {"class_type": "MockRestyle", "inputs": {
                "text": "${prompt}", "seed": "${seed}", "image": "${image}"}}},
        }
        self.adapter = Adapter(self.root, ComfyClient(server.server_port), workflows)

    def request(self, kind, *, expired=False):
        folder = self.root / "jobs" / ("0a" * 16)
        folder.mkdir()
        if kind == "restyle":
            atomic_write(folder / "input.png", png(6, 3))
        payload = dict(version=1, id=folder.name, kind=kind, prompt="photo of a ${seed}", seed=2**64 - 1,
                       expiresAtUnix=int(time.time()) + (-5 if expired else 30))
        atomic_write(folder / "request.json", json.dumps(payload).encode())
        return folder

    @staticmethod
    def status(folder):
        return read_json(folder / "status.json")

    def test_restyle_upload_substitutes_workflow_and_keeps_png_bytes(self):
        folder = self.request("restyle")
        self.adapter.poll()
        self.assertEqual(self.status(folder)["phase"], "queued")
        self.assertEqual([route for route, _ in self.server.posts], ["/upload/image", "/prompt"])
        self.assertIn(png(6, 3), self.server.posts[0][1])
        fields = json.loads(self.server.posts[1][1])["prompt"]["4"]["inputs"]
        self.assertEqual(fields, {"text": "photo of a ${seed}", "seed": 2**64 - 1, "image": "test/source.png"})
        self.adapter.poll()
        self.assertEqual(self.status(folder)["phase"], "running")
        self.server.history = success()
        self.adapter.poll()
        self.assertEqual(self.status(folder)["phase"], "succeeded")
        self.assertEqual(self.server.view, {"filename": ["generated image.png"], "subfolder": ["local output"], "type": ["output"]})
        self.assertEqual((folder / "generated.png").read_bytes(), self.server.output)

    def test_hdri_converts_ldr_panorama_to_linear_radiance(self):
        folder = self.request("hdri")
        self.adapter.poll()
        self.server.history = success()
        self.adapter.poll()
        self.assertEqual(self.status(folder)["output"], "generated.hdr")
        output = (folder / "generated.hdr").read_bytes()
        header = b"#?RADIANCE\nFORMAT=32-bit_rle_rgbe\n\n-Y 4 +X 8\n"
        self.assertTrue(output.startswith(header))
        red, green, blue, exponent = (output[len(header) + 5 + 9 * channel] for channel in range(4))
        decoded = [channel * 2. ** (exponent - 136) for channel in (red, green, blue)]
        for value, expected in zip(decoded, (.21586, .05127, .01444)):
            self.assertAlmostEqual(value, expected, delta=.001)

    def test_bad_output_and_expired_requests_fail_safely(self):
        folder = self.request("hdri")
        self.adapter.poll()
        self.server.history = success("../unrelated.hdr")
        self.adapter.poll()
        self.assertEqual(self.status(folder)["phase"], "failed")
        self.assertIn("invalid filename", self.status(folder)["message"])
        self.assertFalse((folder / "generated.hdr").exists())
        folder.rename(folder.with_name("expired-test"))
        folder = self.request("hdri", expired=True)
        self.adapter.poll()
        self.assertEqual(self.status(folder)["phase"], "cancelled")
        self.assertEqual([route for route, _ in self.server.posts], ["/prompt"])

    def test_cancel_targets_only_the_own_prompt_and_never_publishes_late_output(self):
        folder = self.request("hdri")
        self.adapter.poll()
        atomic_write(folder / "cancel.json", b"{}")
        self.server.targeted_cancel = False
        self.server.history = success()
        self.adapter.poll()
        self.adapter.poll()
        self.assertEqual(self.status(folder)["phase"], "cancelled")
        self.assertEqual([route for route, _ in self.server.posts],
                         ["/prompt", f"/api/jobs/{PROMPT_ID}/cancel", "/queue"])
        self.assertEqual(json.loads(self.server.posts[-1][1]), {"delete": [PROMPT_ID]})
        self.assertFalse((folder / "generated.hdr").exists())

    def test_adapter_has_exclusive_spool_ownership_until_exit(self):
        with adapter_lock(self.root):
            with self.assertRaisesRegex(RuntimeError, "already watching"):
                with adapter_lock(self.root):
                    self.fail("Two adapters acquired the same root")
        with adapter_lock(self.root):
            self.assertTrue((self.root / ".adapter.lock").is_file())


if __name__ == "__main__":
    unittest.main(verbosity=2)
