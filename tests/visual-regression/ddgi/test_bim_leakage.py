"""Validate frozen-field pairing and matched architectural quality controls."""

from copy import deepcopy
from pathlib import Path
import unittest

import run_bim_leakage as leakage
from bim_leakage_fixtures import build_scene


def receiver(value):
    return {"pixelRoi": [10, 20, 22, 32], "estimatedLuminance": value,
            "estimatedRadiance": [value, value, value], "saturatedChannelFraction": 0,
            "luminanceQuantizationInterval": [max(0, value - .001), value + .002]}


def pair_fixture(grid="coarse", variant="sealed", units="millimetre", technique="deferred-raster",
                 policy="opaque"):
    scene = build_scene(variant, units)
    scene.update(gltf="primary.gltf", bim="walls.ifcx")
    spacing, counts = leakage.GRIDS[grid]
    warm = leakage.warm_frames(grid, 12)
    record = {"name": f"{variant}-{units}-{technique}-{grid}-{policy}", "passed": True, "frames": []}
    for index, intensity in enumerate((1, 0)):
        record["frames"].append({"frame": warm + index + 1, "technique": technique,
            "ddgi": {"supported": True, "enabled": True, "active": True, "paused": True,
                "updateFrame": warm, "refreshRemainingProbes": 0, "updatedProbes": 0, "tracedRays": 0,
                "historyResets": 1, "contentEpoch": 1, "schedulingCursor": 18, "totalAllocatedBytes": 512,
                "settings": {"intensity": intensity, "hysteresis": 0, "counts": list(counts),
                    "spacing": list(spacing), "origin": scene["volume"]["origin"],
                    "maxDistance": scene["volume"]["maxDistance"], "additionalVolumes": [],
                    "cameraFollow": False, "asyncCompute": False, "dielectricTransmission": False,
                    "thinGlassTransmission": policy == "thin-glass"}},
            "rayShadows": {"active": True, "generation": 1},
            "camera": {"position": [0, 1.65, 7]},
            "scene": {"primary": "primary.gltf", "primaryObjectCount": 1, "auxiliary": "walls.ifcx",
                      "auxiliaryObjectCount": len(scene["bimElements"]), "auxiliaryBounds": scene["bimBounds"],
                      "auxiliaryImportScale": scene["bimImportScale"]},
            "lighting": {"directionalIntensity": 0, "environmentIntensity": 0, "areaLightCount": 1,
                         "pointLightCount": 0},
            "capture": {"schemaVersion": 1, "source": "window", "colorSpace": "sRGB",
                        "displayMode": 0, "toneMapper": 0,
                        "includesUi": False, "exposureMode": "manual", "manualExposure": .04,
                        "width": scene["captureSize"][0], "height": scene["captureSize"][1]},
            "receivers": {name: receiver(.2 if intensity else .1) for name in leakage.RECEIVER_NAMES}})
    return record, scene, technique, grid, policy, warm


def row(**options):
    return leakage.compare_pair(*pair_fixture(**options))


class BimLeakageTests(unittest.TestCase):
    def test_grid_warmup_pairs_pause_before_the_first_measurement(self):
        self.assertEqual(leakage.warm_frames("coarse", 12), 12)
        self.assertEqual(leakage.warm_frames("dense", 12), 199)
        self.assertEqual(leakage.pair_sequence(12), {"schemaVersion": 1, "frames": 14,
            "sampleFrames": [13, 14], "events": [{"frame": 13, "ddgiPaused": True, "ddgiIntensity": 1},
                                                 {"frame": 14, "ddgiIntensity": 0}]})

    def test_capture_arguments_select_provider_scale_and_transport(self):
        _, scene, *_ = pair_fixture(variant="mixed")
        args = leakage.capture_args(scene, Path("output folder"), "thin-glass")
        self.assertEqual(args[args.index("--bim-model") + 1], str(Path("output folder") / "walls.ifcx"))
        self.assertEqual(args[args.index("--bim-import-scale") + 1], "0.001")
        self.assertIn("--ddgi-thin-glass", args)
        self.assertIn("--no-ddgi-thin-glass", leakage.capture_args(scene, Path("out"), "opaque"))

    def test_signed_pair_retains_negative_values_and_quantization_interval(self):
        measurement = leakage.compare_receiver(receiver(.1), receiver(.2))
        self.assertAlmostEqual(measurement["signedIndirectLuminance"], -.1)
        self.assertAlmostEqual(measurement["signedIndirectQuantizationInterval"][0], -.103)
        self.assertAlmostEqual(measurement["signedIndirectQuantizationInterval"][1], -.097)
        clipped = receiver(.1)
        clipped["saturatedChannelFraction"] = .01
        with self.assertRaisesRegex(AssertionError, "clipped"):
            leakage.compare_receiver(clipped, receiver(.2))

    def test_frozen_pair_rejects_state_history_and_input_changes(self):
        arguments = pair_fixture()
        self.assertAlmostEqual(leakage.compare_pair(*arguments)["receivers"]["lit-back"]["signedIndirectLuminance"], .1)
        for section, key, value, message in (
                ("ddgi", "active", False, "not active, warm and frozen"),
                ("ddgi", "refreshRemainingProbes", 1, "not active, warm and frozen"),
                ("ddgi", "contentEpoch", 2, "History, updates"),
                ("rayShadows", "generation", 2, "History, updates"),
                ("scene", "auxiliaryObjectCount", 0, "provider or import scale"),
                ("capture", "manualExposure", .05, "fixed ACES exposure"),
                ("camera", "position", [0, 1.65, 6], "authored fixture position")):
            with self.subTest(section=section, key=key):
                changed = deepcopy(arguments)
                changed[0]["frames"][1][section][key] = value
                with self.assertRaisesRegex(AssertionError, message):
                    leakage.compare_pair(*changed)

    def test_matching_open_controls_bound_sealed_leakage_and_false_darkening(self):
        opened = row(variant="open", grid="dense")
        sealed = row(grid="dense")
        with self.assertRaisesRegex(AssertionError, "dense sealed-wall leakage"):
            leakage.compare_rows([opened, sealed], {})
        sealed["receivers"]["shaded-back"]["signedIndirectLuminance"] = 0
        reference = {variant: {name: {"indirectLuminance": lum, "luminanceStandardError": .003}
                               for name in leakage.RECEIVER_NAMES}
                     for variant, lum in (("open", .1), ("sealed", .15))}
        leakage.compare_rows([opened, sealed], reference)
        lit = sealed["receivers"]["lit-back"]
        self.assertAlmostEqual(lit["signedDifferenceFromReference"], -.05)
        self.assertAlmostEqual(lit["signedNormalizedDifferenceFromReference"], -.5)
        self.assertAlmostEqual(lit["falseDarkeningFractionOfOpen"], .5)
        self.assertEqual(lit["excessFractionOfOpen"], 0)
        summary = leakage.summarize_bounds([opened, sealed])
        self.assertAlmostEqual(summary[1]["receivers"]["lit-back"]["maximumFalseDarkeningFractionOfOpen"], .5)
        sealed["receivers"]["lit-back"]["signedIndirectLuminance"] = .001
        with self.assertRaisesRegex(AssertionError, "darkened the lit receiver"):
            leakage.compare_rows([opened, sealed], reference)
        with self.assertRaisesRegex(AssertionError, "missing matched open control"):
            leakage.compare_rows([sealed], reference)

    def test_normalization_keeps_signed_interval_and_requires_positive_open_bound(self):
        self.assertEqual(leakage.normalized_interval([-2, 1], [2, 4]), [-1, .5])
        self.assertIsNone(leakage.normalized_interval([0, 1], [0, 1]))

    def test_scale_comparison_retains_raw_and_open_normalized_differences(self):
        first = row(variant="open", units="metre")
        second = row(variant="open", units="millimetre")
        second["receivers"]["shaded-back"]["signedIndirectLuminance"] = .3
        second["receivers"]["shaded-back"]["signedIndirectQuantizationInterval"] = [.299, .302]
        leakage.compare_rows([first, second], {})
        comparisons = leakage.compare_axis([first, second], "sourceUnits", leakage.SOURCE_UNITS)
        self.assertEqual(len(comparisons), 1)
        measurement = comparisons[0]["receivers"]["shaded-back"]
        self.assertAlmostEqual(measurement["signedLuminanceDifference"], .2)
        self.assertEqual(measurement["signedDifferenceOfOpenFractions"], 0)

    def test_raster_comparison_rejects_disagreement_beyond_measurement_interval(self):
        first = row(variant="open")
        second = row(variant="open", technique="forward-raster")
        second["receivers"]["shaded-back"]["signedIndirectLuminance"] = .3
        second["receivers"]["shaded-back"]["signedIndirectQuantizationInterval"] = [.299, .302]
        leakage.compare_rows([first, second], {})
        with self.assertRaisesRegex(AssertionError, "technique parity exceeded"):
            leakage.compare_axis([first, second], "technique", leakage.TECHNIQUES, parity=True)

    def test_linear_pair_checks_ungraded_exr_metadata_and_uses_float_measurements(self):
        arguments = pair_fixture()
        for frame in arguments[0]["frames"]:
            metadata = frame["capture"]
            frame["capture"] = {"schemaVersion": 1, "width": metadata["width"], "height": metadata["height"],
                                "source": "raster-scene-composite", "colorSpace": "linear-sRGB",
                                "includesUi": False, "exposureApplied": False, "toneMappingApplied": False}
            for measurement in frame["receivers"].values():
                measurement["luminanceFloatInterval"] = [measurement["estimatedLuminance"]] * 2
        result = leakage.compare_pair(*arguments, linear=True)
        self.assertEqual(result["captureFormat"], "scene-linear-exr")
        self.assertEqual(result["receivers"]["lit-back"]["signedIndirectFloatInterval"], [.1, .1])
        arguments[0]["frames"][1]["capture"]["exposureApplied"] = True
        with self.assertRaisesRegex(AssertionError, "ungraded scene-linear"):
            leakage.compare_pair(*arguments, linear=True)


if __name__ == "__main__":
    unittest.main()
