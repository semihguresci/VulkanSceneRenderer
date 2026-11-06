"""CPU image/report regressions; these tests never start the renderer."""

import contextlib
import io
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

try:
    import numpy as np
    from PIL import Image, ImageDraw
except ImportError:
    print("SKIP: physics image checker tests require Pillow and NumPy")
    raise SystemExit(77)

import physics_runtime_regression as physics


CAMERA = [[.125, 0, 0, 0], [0, 0, 1, 0], [0, .125, 0, 0], [0, 0, 0, 1]]
DIRECTION = [-.3958, -.8796, -.2639]


def matrix(position, scale=(1, 1, 1)):
    result = np.diag([*scale, 1.0])
    result[:3, 3] = position
    return result.T.flatten().tolist()


def bodies(frame):
    floor_position = [0, -.25, 0]
    floor_matrix = matrix(floor_position, (12, .5, 12))
    result = [{"name": "Physics floor", "motion": 0, "position": floor_position,
               "sceneWorldMatrix": floor_matrix, "renderWorldMatrix": floor_matrix}]
    for index in (1, 2, 3):
        position = [(index - 2) * 1.5, {1: 3.0, 30: 2.0, 180: .49}[frame], 0]
        transform = matrix(position)
        result.append({"name": f"Physics box {index}", "motion": 2,
                       "position": position, "sceneWorldMatrix": transform,
                       "renderWorldMatrix": transform})
    return result


def record(directory, technique="deferred-raster", mode="raster", shadow=None):
    name = f"physics-{technique}-{mode}"
    samples = []
    for frame in (1, 30, 180, 184):
        image = Image.new("RGB", (128, 128), (118, 118, 118))
        image.putpixel((0, 0), (frame, 0, 0))  # Distinct motion samples outside the floor.
        if frame == 180 and shadow:
            ImageDraw.Draw(image).rectangle(shadow, fill=(0, 0, 0))
        path = directory / f"{name}.frame-{frame:04d}.png"
        image.save(path)
        telemetry = {"camera": {"unjitteredViewProjColumns": CAMERA},
                     "lighting": {"directionalDirection": DIRECTION},
                     "physics": {"available": True, "initialized": True, "playing": True,
                                 "bodyCount": 4, "fixedStepSeconds": 1 / 60,
                                 "droppedSeconds": 0, "totalSteps": frame,
                                 "bodies": bodies(frame) if frame != 184 else []},
                     "scene": {"primaryObjectCount": 4},
                     "rayShadows": {"active": mode == "rays-taa"},
                     "taa": {"enabled": mode == "rays-taa"}}
        if frame == 184:
            telemetry["physics"]["bodyCount"] = 0
            telemetry["scene"]["primaryObjectCount"] = 0
        samples.append({"frame": frame, "png": str(path), "telemetry": telemetry})
    return {"name": name, "mode": mode, "technique": technique, "samples": samples}


class PhysicsRuntimeImageChecks(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.directory = Path(self.temporary.name)

    def pair(self, shadow=None):
        return (record(self.directory), record(self.directory, mode="rays-taa", shadow=shadow))

    def test_healthy_floor_passes(self):
        metrics = physics.check_floor_shadow_footprint(*self.pair())
        self.assertTrue(metrics["passed"])
        self.assertGreater(metrics["comparisonPixels"], 1000)
        self.assertEqual(metrics["extraDarkPixels"], 0)

    def test_contact_shadow_is_excluded(self):
        metrics = physics.check_floor_shadow_footprint(*self.pair(shadow=(52, 60, 76, 67)))
        self.assertEqual(metrics["extraDarkPixels"], 0)

    def test_oversized_remote_shadow_fails_and_retains_metric(self):
        raster, rays = self.pair(shadow=(20, 20, 108, 108))
        with self.assertRaisesRegex(AssertionError, "Oversized ray-shadow floor footprint"):
            physics.check_floor_shadow_footprint(raster, rays)
        metrics = rays["checks"]["floorShadowFootprint"]
        self.assertFalse(metrics["passed"])
        self.assertGreater(metrics["extraDarkFraction"], .5)

    def test_dark_reference_cannot_hide_missing_floor(self):
        raster, rays = self.pair()
        Image.new("RGB", (128, 128)).save(raster["samples"][2]["png"])
        with self.assertRaisesRegex(AssertionError, "Too little lit floor"):
            physics.check_floor_shadow_footprint(raster, rays)

    def test_different_camera_is_rejected(self):
        raster, rays = self.pair()
        rays["samples"][2]["telemetry"]["camera"]["unjitteredViewProjColumns"] = np.eye(4).tolist()
        with self.assertRaisesRegex(AssertionError, "cameras differ"):
            physics.check_floor_shadow_footprint(raster, rays)

    def test_different_image_sizes_are_rejected(self):
        raster, rays = self.pair()
        Image.new("RGB", (64, 64)).save(rays["samples"][2]["png"])
        with self.assertRaisesRegex(AssertionError, "image sizes differ"):
            physics.check_floor_shadow_footprint(raster, rays)

    def report(self, broken):
        cases = []
        for technique in ("deferred-raster", "forward-raster"):
            cases.append(record(self.directory, technique))
            cases.append(record(self.directory, technique, "rays-taa",
                                shadow=(20, 20, 108, 108) if broken else None))
        report = {"passed": True, "cases": cases}
        (self.directory / "results.json").write_text(json.dumps(report), encoding="utf-8")

    def test_recheck_retires_old_pass_and_persists_failure(self):
        self.report(broken=True)
        with contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(physics.recheck_existing(self.directory), 1)
        report = json.loads((self.directory / "results.json").read_text())
        self.assertFalse(report["passed"])
        self.assertIn("Oversized ray-shadow floor footprint", report["failure"])
        self.assertFalse(report["cases"][1]["checks"]["floorShadowFootprint"]["passed"])

    def test_recheck_all_healthy_cases_passes(self):
        self.report(broken=False)
        self.assertEqual(physics.recheck_existing(self.directory), 0)
        report = json.loads((self.directory / "results.json").read_text())
        self.assertTrue(report["passed"])
        for index in (1, 3):
            self.assertTrue(report["cases"][index]["checks"]["floorShadowFootprint"]["passed"])

    def test_opt_in_skip_uses_only_standard_library(self):
        environment = dict(os.environ)
        environment.pop("CONTAINER_RUN_GPU_PHYSICS", None)
        result = subprocess.run(
            [sys.executable, "-S", str(Path(physics.__file__).resolve()),
             "--require-opt-in", "--output", str(self.directory / "unused")],
            env=environment, capture_output=True, text=True, timeout=10)
        self.assertEqual(result.returncode, 77, result.stdout + result.stderr)
        self.assertIn("Set CONTAINER_RUN_GPU_PHYSICS=1", result.stdout)
        self.assertNotIn("Traceback", result.stderr)
        self.assertFalse((self.directory / "unused").exists())

    def test_help_uses_only_standard_library(self):
        result = subprocess.run(
            [sys.executable, "-S", str(Path(physics.__file__).resolve()), "--help"],
            capture_output=True, text=True, timeout=10)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("--check-existing", result.stdout)


if __name__ == "__main__":
    unittest.main()
