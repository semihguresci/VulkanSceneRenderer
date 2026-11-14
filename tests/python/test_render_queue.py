"""Exercise the render queue CLI with a CPU-only renderer subprocess."""

from contextlib import redirect_stderr, redirect_stdout
import importlib.util
import io
import json
import math
from pathlib import Path
import subprocess
import sys
import tempfile
import time
import unittest
from unittest import mock


ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location("render_queue", ROOT / "tools/render_queue.py")
render_queue = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = render_queue
SPEC.loader.exec_module(render_queue)

FAKE_RENDERER = r'''
import json
from pathlib import Path
import sys
import time


def flag(name):
    return sys.argv[sys.argv.index(name) + 1]


target = Path(flag("--screenshot"))
width, height = (int(flag(option)) for option in ("--width", "--height"))
technique = flag("--render-technique")
png = target.suffix.lower() == ".png"
target.write_bytes((b"\x89PNG\r\n\x1a\n" if png else b"\x76\x2f\x31\x01") + b"fake-render")
if "--wait-for-cancel-in-test" in sys.argv:
    time.sleep(30)
if "--fail-in-test" in sys.argv:
    print("intentional renderer failure", file=sys.stderr)
    sys.exit(19)

capture = {"schemaVersion": 1, "source": "viewport" if png else "raster-scene-composite",
           "width": width, "height": height, "includesUi": False,
           "colorSpace": "sRGB" if png else "linear-sRGB"}
telemetry = {"resolution": [width, height], "technique": technique,
             "msaaSamples": int(flag("--msaa")), "taa": {"enabled": "--taa" in sys.argv},
             "build": {"revision": "fake-renderer"}}
if "--gltf-camera-node" in sys.argv:
    telemetry["gltfAnimation"] = {"selectedCameraNode": int(flag("--gltf-camera-node"))}
if technique == "path-tracing":
    if not png:
        capture["source"] = "path-trace-mean"
    capture["referencePath"] = {
        "attemptedSamplesPerPixel": int(flag("--path-samples")),
        "sampleLimit": int(flag("--path-samples")),
        "samplesPerFrame": int(flag("--path-spp")),
        "maxBounces": int(flag("--path-bounces")),
        "seed": int(flag("--path-seed")),
        "unsupportedSamples": 0, "rejectedSamples": 0,
    }
if "--wrong-size-in-test" in sys.argv:
    telemetry["resolution"][0] += 1
if "--invalid-shape-in-test" in sys.argv:
    telemetry["taa"] = None
Path(f"{target}.capture.json").write_text(json.dumps(capture), encoding="utf-8")
target.with_suffix(".telemetry.json").write_text(json.dumps(telemetry), encoding="utf-8")
'''


class RenderQueueTests(unittest.TestCase):
    def setUp(self):
        folder = tempfile.TemporaryDirectory(prefix="render-queue-tests-")
        self.addCleanup(folder.cleanup)
        self.root = Path(folder.name).resolve()
        self.scene = self.root / "model with spaces.gltf"
        self.scene.write_text(json.dumps({
            "asset": {"version": "2.0"}, "scene": 0, "scenes": [{"nodes": [0]}],
            "nodes": [{"camera": 0, "translation": [0, 1, 4]}],
            "cameras": [{"type": "perspective", "perspective": {"yfov": .8, "znear": .1}}],
        }), encoding="utf-8")
        self.fake = self.root / "fake_renderer.py"
        self.fake.write_text(FAKE_RENDERER, encoding="utf-8")
        self.executable = str(Path(sys.executable).resolve())
        self.plan = self.root / "render jobs.json"
        self.calls = []
        self.child_images = []
        self.fail_calls = set()
        self.wrong_size_calls = set()
        self.invalid_shape_calls = set()
        self.concurrent_images = {}
        self.interrupt_calls = set()
        self.interrupted_processes = []
        self.popen = subprocess.Popen

    def still(self, name, output, renderer="deferred-raster"):
        return {"name": name, "scene": self.scene.name, "renderer": renderer,
                "width": 80, "height": 60, "output": output, "cameraNode": 0}

    def orbit(self, name, output):
        job = self.still(name, output, "forward-raster")
        del job["cameraNode"]
        job["orbit"] = {"target": [0, 1, 0], "radius": 4, "height": 1, "frames": 3}
        return job

    def write_queue(self, *jobs):
        self.plan.write_text(json.dumps({"schemaVersion": 1, "jobs": jobs}), encoding="utf-8")
        return self.plan

    def fake_popen(self, argv, **kwargs):
        argv = list(map(str, argv))
        self.assertEqual(Path(argv[0]).resolve(), Path(self.executable))
        self.calls.append(argv)
        target = Path(argv[argv.index("--screenshot") + 1])
        self.child_images.append(Path(kwargs.get("cwd") or Path.cwd()) / target)
        extra = []
        if len(self.calls) in self.fail_calls:
            extra.append("--fail-in-test")
        if len(self.calls) in self.wrong_size_calls:
            extra.append("--wrong-size-in-test")
        if len(self.calls) in self.invalid_shape_calls:
            extra.append("--invalid-shape-in-test")
        foreign = self.concurrent_images.get(len(self.calls))
        if foreign is not None:
            # The child is about to render, after queue preflight and before publication.
            foreign.write_bytes(b"concurrent output")
        interrupt = len(self.calls) in self.interrupt_calls
        if interrupt:
            extra.append("--wait-for-cancel-in-test")
        process = self.popen([self.executable, str(self.fake), *argv[1:], *extra], **kwargs)
        if interrupt:
            self.interrupt_first_wait(process, self.child_images[-1])
        return process

    def interrupt_first_wait(self, process, child_image):
        original_wait = process.wait

        def cleanup():
            if process.poll() is None:
                process.kill()
                original_wait(timeout=5)

        def interrupt(*args, **kwargs):
            process.wait = original_wait
            deadline = time.monotonic() + 5
            while not child_image.exists() and process.poll() is None and time.monotonic() < deadline:
                time.sleep(.01)
            self.assertTrue(child_image.exists(), "fake renderer did not start writing the current frame")
            raise KeyboardInterrupt

        self.addCleanup(cleanup)
        self.interrupted_processes.append(process)
        process.wait = interrupt

    def invoke(self, *args):
        stdout = io.StringIO()
        stderr = io.StringIO()
        with (redirect_stdout(stdout), redirect_stderr(stderr),
              mock.patch.object(render_queue.subprocess, "Popen", side_effect=self.fake_popen)):
            try:
                code = render_queue.main(list(map(str, args)))
            except SystemExit as exit_:
                code = exit_.code
        return code, stdout.getvalue() + stderr.getvalue()

    def run_queue(self):
        return self.invoke("run", "--renderer", self.executable, self.plan)

    def assert_published(self, filename, technique):
        image = self.root / filename
        self.assertTrue(image.is_file(), image)
        capture = json.loads(Path(f"{image}.capture.json").read_bytes())
        self.assertEqual((capture["width"], capture["height"]), (80, 60))
        self.assertIs(capture["includesUi"], False)
        telemetry = json.loads(image.with_suffix(".telemetry.json").read_bytes())
        self.assertEqual(telemetry["resolution"], [80, 60])
        self.assertEqual(telemetry["technique"], technique)

    def assert_unpublished(self, filename):
        image = self.root / filename
        for path in (image, Path(f"{image}.capture.json"), image.with_suffix(".telemetry.json")):
            self.assertFalse(path.exists(), path)

    def test_validate_save_as_preserves_scene_and_output_across_directories(self):
        self.write_queue(self.still("Saved still", "job.png"), self.orbit("Saved orbit", "turn.exr"))
        copy_folder = self.root / "other folder"
        copy_folder.mkdir()
        saved = copy_folder / "saved.json"

        code, messages = self.invoke("validate", self.plan, "--save-as", saved)

        self.assertEqual(code, 0, messages)
        self.assertEqual(self.calls, [])
        data = json.loads(saved.read_bytes())
        self.assertEqual(data["schemaVersion"], 1)
        self.assertEqual([job["name"] for job in data["jobs"]], ["Saved still", "Saved orbit"])
        for job in data["jobs"]:
            self.assertEqual(Path(job["scene"]), self.scene)
        self.assertEqual(Path(data["jobs"][0]["output"]), self.root / "job.png")
        self.assertEqual(Path(data["jobs"][1]["output"]), self.root / "turn.exr")
        self.assert_unpublished("job.png")

    def test_validate_rejects_conflicting_camera_settings(self):
        job = self.orbit("Ambiguous camera", "invalid.png")
        job["cameraNode"] = 0
        self.write_queue(job)
        saved = self.root / "invalid.json"

        code, messages = self.invoke("validate", self.plan, "--save-as", saved)

        self.assertNotEqual(code, 0, messages)
        self.assertEqual(self.calls, [])
        self.assertFalse(saved.exists())

    def test_authored_camera_and_orbit_publish_deterministic_views(self):
        self.write_queue(self.still("Authored still", "still.png"), self.orbit("Turntable", "turn.exr"))

        code, messages = self.run_queue()

        self.assertEqual(code, 0, messages)
        self.assertEqual(len(self.calls), 4)
        still, *orbit = self.calls
        self.assertEqual(still[still.index("--gltf-camera-node") + 1], "0")
        self.assertNotIn("--camera-position", still)
        offsets = []
        for command in orbit:
            self.assertNotIn("--gltf-camera-node", command)
            self.assertIn("--capture-frame", command)
            index = command.index("--camera-target")
            self.assertEqual(tuple(map(float, command[index + 1:index + 4])), (0, 1, 0))
            index = command.index("--camera-position")
            x, y, z = map(float, command[index + 1:index + 4])
            self.assertAlmostEqual(math.hypot(x, z), 4)
            offsets.append((x, y, z))
        self.assertEqual(len(set(offsets)), 3)
        for point, other in zip(offsets, offsets[1:] + offsets[:1]):
            self.assertAlmostEqual(point[1], other[1])
            self.assertAlmostEqual(point[0] * other[0] + point[2] * other[2], -8)
        self.assert_published("still.png", "deferred-raster")
        for frame in range(1, 4):
            self.assert_published(f"turn.{frame:04d}.exr", "forward-raster")
        self.assert_unpublished("turn.exr")

    def test_child_failure_preserves_completed_turntable_views_and_continues(self):
        self.fail_calls = {2}
        self.write_queue(self.orbit("Partial turntable", "partial.png"), self.still("Later still", "later.png"))

        code, messages = self.run_queue()

        self.assertNotEqual(code, 0, messages)
        self.assertEqual(len(self.calls), 3)
        self.assert_published("partial.0001.png", "forward-raster")
        self.assert_unpublished("partial.0002.png")
        self.assert_unpublished("partial.0003.png")
        self.assertFalse(self.child_images[1].exists())
        self.assert_published("later.png", "deferred-raster")
        self.assertIn("Partial turntable", messages)
        self.assertIn("Later still", messages)

    def test_path_sample_count_sets_the_number_of_capture_frames(self):
        job = self.still("Path-traced still", "reference.exr", "path-tracing")
        job["sampling"] = {"samples": 9, "samplesPerFrame": 4, "bounces": 3, "seed": 29}
        self.write_queue(job)

        code, messages = self.run_queue()

        self.assertEqual(code, 0, messages)
        (command,) = self.calls
        self.assertEqual(command[command.index("--warmup-frames") + 1], "0")
        self.assertEqual(command[command.index("--capture-frame") + 1], "3")
        self.assert_published("reference.exr", "path-tracing")

    def test_preflight_leaves_existing_files_and_shared_destinations_unmodified(self):
        existing = self.root / "existing.capture.png"
        sidecar = existing.with_suffix(".telemetry.json")
        sidecar.write_text("existing receipt", encoding="utf-8")
        self.write_queue(self.still("Occupied", existing.name),
                         self.orbit("Conflicting turntable", "collision.png"),
                         self.still("Conflicting still", "collision.0002.png"),
                         self.still("Disjoint still", "valid.png"))

        code, messages = self.run_queue()

        self.assertNotEqual(code, 0, messages)
        self.assertEqual(len(self.calls), 1, messages)
        self.assertEqual(sidecar.read_text(encoding="utf-8"), "existing receipt")
        self.assertFalse(existing.exists())
        self.assertFalse(Path(f"{existing}.capture.json").exists())
        for frame in range(1, 4):
            self.assert_unpublished(f"collision.{frame:04d}.png")
        self.assert_published("valid.png", "deferred-raster")

    def test_a_concurrent_image_is_preserved_and_staged_sidecars_roll_back(self):
        other_image = self.root / "race.png"
        self.concurrent_images[1] = other_image
        self.write_queue(self.still("Racing destination", other_image.name),
                         self.still("Independent destination", "independent.png"))

        code, messages = self.run_queue()

        self.assertNotEqual(code, 0, messages)
        self.assertEqual(len(self.calls), 2)
        self.assertEqual(other_image.read_bytes(), b"concurrent output")
        self.assertFalse(Path(f"{other_image}.capture.json").exists())
        self.assertFalse(other_image.with_suffix(".telemetry.json").exists())
        self.assert_published("independent.png", "deferred-raster")

    def test_cancel_active_job_reaps_child_and_continues_queue(self):
        self.interrupt_calls = {2}
        self.write_queue(self.orbit("Cancelled turntable", "cancel.png"), self.still("Next job", "next.png"))

        code, messages = self.run_queue()

        self.assertEqual(code, 130, messages)
        self.assertEqual(len(self.calls), 3)
        self.assertEqual(len(self.interrupted_processes), 1)
        self.assertIsNotNone(self.interrupted_processes[0].returncode)
        self.assertFalse(self.child_images[1].exists())
        self.assert_published("cancel.0001.png", "forward-raster")
        self.assert_unpublished("cancel.0002.png")
        self.assert_unpublished("cancel.0003.png")
        self.assert_published("next.png", "deferred-raster")

    def test_a_successful_child_exit_needs_matching_capture_telemetry(self):
        self.wrong_size_calls = {1}
        self.write_queue(self.still("Bad receipt", "bad.png"), self.still("Valid receipt", "good.png"))

        code, messages = self.run_queue()

        self.assertNotEqual(code, 0, messages)
        self.assertEqual(len(self.calls), 2)
        self.assert_unpublished("bad.png")
        self.assert_published("good.png", "deferred-raster")


    def test_malformed_json_receipt_fails_one_job_and_continues(self):
        self.invalid_shape_calls = {1}
        self.write_queue(self.still("Malformed receipt", "malformed.png"),
                         self.still("Valid receipt", "valid.png"))

        code, messages = self.run_queue()

        self.assertNotEqual(code, 0, messages)
        self.assertEqual(len(self.calls), 2)
        self.assert_unpublished("malformed.png")
        self.assert_published("valid.png", "deferred-raster")


if __name__ == "__main__":
    unittest.main()
