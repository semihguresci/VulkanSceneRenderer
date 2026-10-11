#!/usr/bin/env python3
"""CPU checks for architectural capture radiance estimates and receiver ROIs."""

import copy
from pathlib import Path
import tempfile
import unittest

import numpy as np
from PIL import Image

import run_quality as quality
from quality_fixtures import build_scene


def encode_radiance(radiance, exposure=quality.EXPOSURE):
    """Independent forward form of the production ACES/sRGB output transform."""
    x = np.asarray(radiance, dtype=np.float64) * exposure
    linear = np.minimum((2.51 * x ** 2 + .03 * x) / (2.43 * x ** 2 + .59 * x + .14), 1)
    srgb = np.where(linear <= .0031308, linear * 12.92,
                    1.055 * linear ** (1 / 2.4) - .055)
    return srgb * 255


def metrics(rgb, luminance, roi=None):
    return {"estimatedRadiance": rgb, "estimatedLuminance": luminance,
            "pixelRoi": roi or [10, 3, 22, 15]}


def case(name, receiver, passed=True):
    return {"name": name, "passed": passed, "frames": [{"receivers": receiver}]}


def expected_motion():
    # Independent checkpoints of the authored sequence: light change at 41;
    # geometry changes every frame 81..120, followed by a stable fresh sweep.
    checkpoints = ((40, 1, 1, 0), (41, 2, 1, 381), (44, 2, 1, 0),
                   (60, 2, 1, 0), (80, 2, 1, 0), (81, 3, 2, 381),
                   (84, 6, 5, 381), (100, 22, 21, 381), (120, 42, 41, 381),
                   (124, 42, 41, 0), (140, 42, 41, 0), (160, 42, 41, 0))
    luminance = {40: .2, 60: .21, 80: .21, 100: .23}
    frames = [{"frame": frame,
               "ddgi": {"contentEpoch": epoch, "updateFrame": frame,
                        "updatedProbes": 256, "tracedRays": 32768,
                        "firstProbe": ((frame - 1) * 256) % 637,
                        "nextProbe": (frame * 256) % 637,
                        "refreshRemainingProbes": remaining,
                        "settings": {"counts": [13, 7, 7]}},
               "rayShadows": {"generation": generation},
               "receivers": {"shaded-back": metrics([0, 0, 0], luminance.get(frame, .22))}}
              for frame, epoch, generation, remaining in checkpoints]
    return {"frames": frames}


def expected_static_comparisons():
    return {"openControlLuminance": .39,
            "doorway": {"shaded-back": {"indirectLuminanceEstimate": .06},
                        "lit-back": {"indirectRgbEstimate": [.12, .03, .01]}},
            "doorway-neutral": {"lit-back": {"indirectRgbEstimate": [.04, .04, .04]}},
            "open": {"shaded-back": {"indirectLuminanceEstimate": .39}},
            "sealed-thin-on": {"shadedBackToOpenLuminanceRatio": .001},
            "sealed-thick-on": {"shadedBackToOpenLuminanceRatio": .001},
            "sealed-thin-fine": {"shadedBackToOpenLuminanceRatio": .001},
            "sealed-thin-coarse": {"shadedBackToOpenLuminanceRatio": .082,
                                   "isolatedIndirectLuminanceEstimate": .032,
                                   "coverageFallbackLuminanceEstimate": 0}}


class QualityMetricTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.png = Path(self.directory.name) / "receiver.png"
        self.scene = {"captureSize": [32, 24], "receivers": [
            {"name": "receiver", "position": [0, .25, 0], "pixelRoi": [8, 6, 20, 18]}]}
        self.camera = {"unjitteredViewProjColumns": np.identity(4).tolist()}

    def save_image(self, color=(64, 128, 192)):
        image = np.empty((24, 32, 3), dtype=np.uint8)
        image[:] = color
        Image.fromarray(image).save(self.png)

    def test_aces_inverse_roundtrips_unsaturated_radiance(self):
        values = np.array([0, .001, .01, .1, 1, 10, 50, 100])
        np.testing.assert_allclose(quality.decode_radiance(encode_radiance(values)), values,
                                   rtol=2e-12, atol=2e-12)

    def test_srgb_low_branch_and_exposure_scaling(self):
        codes = np.array([0., 1., 10., 11., 64., 128., 200.])
        decoded = quality.decode_radiance(codes)
        self.assertEqual(decoded[0], 0)
        self.assertTrue(np.all(np.diff(decoded) > 0))
        np.testing.assert_allclose(quality.decode_radiance(codes, .08), decoded * .5,
                                   rtol=1e-13, atol=1e-13)
        # Code 10 is below the sRGB transfer breakpoint; roundtrip that branch.
        np.testing.assert_allclose(encode_radiance(decoded[:3]), codes[:3], atol=1e-12)

    def test_receiver_roi_averages_decoded_pixels_and_channel_saturation(self):
        image = np.zeros((24, 32, 3), dtype=np.uint8)
        image[6:18, 8:20] = [64, 128, 255]
        Image.fromarray(image).save(self.png)
        result = quality.receiver_metrics(self.png, self.scene)["receiver"]
        self.assertEqual(result["pixelRoi"], [8, 6, 20, 18])
        self.assertEqual(result["meanRgb8"], [64, 128, 255])
        np.testing.assert_allclose(result["estimatedRadiance"], quality.decode_radiance([64, 128, 255]))
        self.assertAlmostEqual(result["saturatedChannelFraction"], 1 / 3)
        self.assertAlmostEqual(result["estimatedLuminance"],
                               np.dot(result["estimatedRadiance"], [.2126, .7152, .0722]))

    def test_camera_projection_transposes_column_upload_and_negates_scene_y(self):
        self.save_image()
        result = quality.receiver_metrics(self.png, self.scene, self.camera)["receiver"]
        self.assertEqual(result["pixelRoi"], [10, 3, 22, 15])
        camera = copy.deepcopy(self.camera)
        # Translation is column 3 in a column-major GLM upload.
        camera["unjitteredViewProjColumns"][3][0] = .25
        result = quality.receiver_metrics(self.png, self.scene, camera)["receiver"]
        self.assertEqual(result["pixelRoi"], [14, 3, 26, 15])

    def test_receiver_roi_rejects_wrong_extent_empty_or_outside_bounds(self):
        self.save_image()
        for roi in ([0, 0, 0, 2], [-1, 0, 2, 2], [0, 0, 33, 2], [0, 0, 2, 25]):
            scene = copy.deepcopy(self.scene)
            scene["receivers"][0]["pixelRoi"] = roi
            with self.assertRaises(AssertionError):
                quality.receiver_metrics(self.png, scene)
        scene = copy.deepcopy(self.scene)
        scene["captureSize"] = [31, 24]
        with self.assertRaises(AssertionError):
            quality.receiver_metrics(self.png, scene)

    def test_receiver_behind_camera_is_rejected(self):
        self.save_image()
        camera = copy.deepcopy(self.camera)
        camera["unjitteredViewProjColumns"][3][3] = -1
        with self.assertRaises(AssertionError):
            quality.receiver_metrics(self.png, self.scene, camera)

    def test_pair_difference_preserves_signed_radiance(self):
        on = {"shaded-back": metrics([1.2, 2, 3], 2)}
        off = {"shaded-back": metrics([1, 2.1, 1], 1.8)}
        result = quality.compare([case("doorway-on", on), case("doorway-off", off)])
        np.testing.assert_allclose(result["doorway"]["shaded-back"]["indirectRgbEstimate"], [.2, -.1, 2])
        self.assertAlmostEqual(result["doorway"]["shaded-back"]["indirectLuminanceEstimate"], .2)

    def test_leakage_ratio_uses_open_receiver_and_supports_zero_light(self):
        result = quality.compare([
            case("open-on", {"shaded-back": metrics([4, 4, 4], 4)}),
            case("sealed-thin-on", {"shaded-back": metrics([1, 1, 1], 1)}),
            case("sealed-thick-on", {"shaded-back": metrics([0, 0, 0], 0)}),
        ])
        self.assertEqual(result["sealed-thin-on"]["shadedBackToOpenLuminanceRatio"], .25)
        self.assertEqual(result["sealed-thick-on"]["shadedBackToOpenLuminanceRatio"], 0)
        self.assertEqual(result["openControlLuminance"], 4)

    def test_failed_or_missing_pair_does_not_produce_metrics(self):
        on = {"shaded-back": metrics([1, 2, 3], 2)}
        off = {"shaded-back": metrics([0, 0, 0], 0)}
        self.assertNotIn("doorway", quality.compare([case("doorway-on", on)]))
        self.assertNotIn("doorway", quality.compare([
            case("doorway-on", on, passed=False), case("doorway-off", off)]))

    def test_coarse_pair_separates_indirect_and_coverage_fallback(self):
        result = quality.compare([
            case("open-on", {"shaded-back": metrics([1, 1, 1], 1)}),
            case("sealed-thin-coarse", {"shaded-back": metrics([.1, .1, .1], .1)}),
            case("sealed-thin-coarse-off", {"shaded-back": metrics([.04, .04, .04], .04)}),
        ])
        coarse = result["sealed-thin-coarse"]
        self.assertAlmostEqual(coarse["shadedBackToOpenLuminanceRatio"], .1)
        self.assertAlmostEqual(coarse["isolatedIndirectLuminanceEstimate"], .06)
        self.assertAlmostEqual(coarse["coverageFallbackLuminanceEstimate"], .04)

    def test_motion_accepts_exact_epochs_generations_and_completed_sweeps(self):
        result = quality.check_motion(expected_motion())
        self.assertEqual(result["freshSweepSubmittedFrames"], 3)
        self.assertEqual(result["lastMotionFrame"], 120)
        self.assertEqual(result["checkedFreshAfterFrame"], 124)
        self.assertAlmostEqual(result["lightResponseLuminanceDelta"], .01)
        self.assertAlmostEqual(result["geometryResponseLuminanceDelta"], .02)

    def test_motion_rejects_noop_content_epoch(self):
        motion = expected_motion()
        for sample in motion["frames"]:
            sample["ddgi"]["contentEpoch"] = 1
        with self.assertRaisesRegex(AssertionError, "contentEpoch"):
            quality.check_motion(motion)

    def test_motion_rejects_stale_sweep_after_light_or_geometry_stops(self):
        for frame in (44, 80, 124, 160):
            motion = expected_motion()
            sample = next(s for s in motion["frames"] if s["frame"] == frame)
            sample["ddgi"]["refreshRemainingProbes"] = 1
            with self.assertRaisesRegex(AssertionError, "stopped scene"):
                quality.check_motion(motion)

    def test_motion_rejects_noop_geometry_or_restarted_probe_cursor(self):
        motion = expected_motion()
        next(s for s in motion["frames"] if s["frame"] == 81)["rayShadows"]["generation"] = 1
        with self.assertRaisesRegex(AssertionError, "AS generation"):
            quality.check_motion(motion)
        motion = expected_motion()
        next(s for s in motion["frames"] if s["frame"] == 100)["ddgi"]["firstProbe"] = 0
        with self.assertRaisesRegex(AssertionError, "firstProbe"):
            quality.check_motion(motion)

    def test_motion_requires_receiver_response_and_all_checkpoints(self):
        motion = expected_motion()
        for sample in motion["frames"]:
            sample["receivers"]["shaded-back"]["estimatedLuminance"] = .2
        with self.assertRaisesRegex(AssertionError, "no measurable"):
            quality.check_motion(motion)
        motion = expected_motion()
        motion["frames"] = [s for s in motion["frames"] if s["frame"] != 160]
        with self.assertRaisesRegex(AssertionError, "Missing motion checkpoints"):
            quality.check_motion(motion)

    def test_static_quality_accepts_indirect_color_bleeding_and_records_coarse_limit(self):
        result = quality.check_quality(expected_static_comparisons())
        self.assertEqual(result["doorwayShadedIndirectLuminance"], .06)
        self.assertEqual(result["litBackIndirectRedGreenRatio"], 4)
        self.assertEqual(result["neutralIndirectRedGreenRatio"], 1)
        self.assertEqual(result["coarseGridMeasuredLimitation"]["shadedBackToOpenLuminanceRatio"], .082)

    def test_static_quality_requires_measurable_indirect_and_red_bleeding(self):
        comparisons = expected_static_comparisons()
        comparisons["doorway"]["shaded-back"]["indirectLuminanceEstimate"] = .005
        with self.assertRaisesRegex(AssertionError, "not measurable"):
            quality.check_quality(comparisons)
        comparisons = expected_static_comparisons()
        comparisons["doorway"]["lit-back"]["indirectRgbEstimate"] = [.04, .04, .04]
        with self.assertRaisesRegex(AssertionError, "not measurable"):
            quality.check_quality(comparisons)

    def test_static_quality_rejects_sealed_response_at_normal_fine_and_thick_grids(self):
        for name in ("sealed-thin-on", "sealed-thick-on", "sealed-thin-fine"):
            comparisons = expected_static_comparisons()
            comparisons[name]["shadedBackToOpenLuminanceRatio"] = .0101
            with self.assertRaisesRegex(AssertionError, "sealed-room response"):
                quality.check_quality(comparisons)

    def test_static_quality_rejects_dark_open_control_even_when_sealed_values_are_zero(self):
        for luminance in (0, .005):
            comparisons = expected_static_comparisons()
            comparisons["openControlLuminance"] = luminance
            for name in ("sealed-thin-on", "sealed-thick-on", "sealed-thin-fine"):
                comparisons[name]["shadedBackToOpenLuminanceRatio"] = 0
            with self.assertRaisesRegex(AssertionError, "Open control receiver"):
                quality.check_quality(comparisons)

    def test_quality_baseline_keeps_active_field_and_changes_only_indirect_intensity(self):
        scene = build_scene("doorway")
        scene["gltf"] = "scene.gltf"
        for coarse in (False, True):
            on = quality.command_for(Path("renderer.exe"), Path("captures"), scene,
                                     "comparison", 240, enabled=True, coarse=coarse)
            off = quality.command_for(Path("renderer.exe"), Path("captures"), scene,
                                      "comparison", 240, enabled=False, coarse=coarse)
            self.assertIn("--ddgi", on)
            self.assertIn("--ddgi", off)
            self.assertNotIn("--no-ddgi", off)
            intensity_index = on.index("--ddgi-intensity") + 1
            self.assertEqual(on[intensity_index], "1")
            self.assertEqual(off[intensity_index], "0")
            expected_off = on.copy()
            expected_off[intensity_index] = "0"
            self.assertEqual(off, expected_off)


if __name__ == "__main__":
    unittest.main()
