"""CPU assertions for GUI-request load, rollback and actual-unload GPU cases."""
import copy
import json
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest.mock import patch

from PIL import Image

import gui_model_loading_regression as gui


CORNELL, WALL, BIM = (Path(name) for name in ("cornell.gltf", "wall.ifcx", "fixture.bim"))
FRAMES = [8, 20, 32, 36, 44, 52, 56, 60, 72, 80, 92, 104, 108, 116, 120, 132, 144]


def synthetic_record(mode="raster"):
    samples = []
    epochs = {8: 1, 20: 2, 32: 3, 36: 3, 44: 3, 52: 3, 56: 3, 60: 3,
              72: 4, 80: 4, 92: 5, 104: 6, 108: 6, 116: 6, 120: 6, 132: 7, 144: 8}
    successful_frames = (20, 32, 72, 80, 92, 104, 132, 144)
    unload_frames = (72, 80, 144)
    for request_count, frame in enumerate(FRAMES):
        empty = frame in (72, 80, 144)
        primary = not empty and (frame == 8 or 92 <= frame <= 120)
        failed = frame in (36, 44, 52, 56, 60, 108, 116, 120)
        scene = {"primary": str(CORNELL) if primary else "",
                 "auxiliary": str(BIM if frame == 132 else WALL) if not primary and not empty else "",
                 "primaryImportScale": 1, "auxiliaryImportScale": 1,
                 "primaryObjectCount": 2 if primary else 0, "primaryNodeCount": 3 if primary else 0,
                 "primaryIndexCount": 36 if primary else 0, "primaryVertexCount": 24 if primary else 0,
                 "auxiliaryObjectCount": 4 if not primary and not empty else 0,
                 "geometryArenaCapacityBytes": 1048576}
        telemetry = {"schemaVersion": 1, "technique": "deferred-raster", "scene": scene,
            "camera": {"position": [4, 3, 8], "unjitteredViewProjColumns": [[1, 0, 0, 0]] * 4},
            "gui": {"displayMode": 0, "selectedMeshNodeId": -1, "selectedBimObjectIndex": -1,
                    "modelLoading": {
                        "pending": False, "requests": request_count, "completed": request_count,
                        "preparationFrames": request_count - sum(f <= frame for f in unload_frames),
                        "progressFrames": sum(f <= frame for f in successful_frames),
                        "eventPumps": 2 * sum(f <= frame for f in successful_frames)},
                    "status": "Unloaded models." if empty else "Failed to load model: broken.ifcx\nparse error"
                    if failed else "Initialized" if frame == 8 else "Loaded model: " + scene["primary"] + scene["auxiliary"]},
            "lighting": {"directionalIntensity": 0 if primary else 2,
                         "environmentIntensity": 0 if primary else 1, "bounceIntensity": 0,
                         "directionalColor": [1, 1, 1], "directionalDirection": [0, -1, 0],
                         "pointLightCount": 0, "areaLightCount": int(primary)},
            "taa": {"enabled": mode != "raster", "epoch": epochs[frame]},
            "rayShadows": {"supported": mode != "raster", "active": mode != "raster" and not empty,
                           "mode": 1, "generation": epochs[frame],
                           "build": {"instances": 0 if empty else 4}},
            "ddgi": {"supported": mode != "raster", "enabled": mode == "ddgi-async", "paused": False,
                     "historyResets": epochs[frame], "contentEpoch": epochs[frame],
                     "active": mode == "ddgi-async" and not empty, "updatedProbes": 0 if empty else 64,
                     "updateFrame": 0 if empty else frame, "status": "DDGI active: async",
                     "settings": {"asyncCompute": mode == "ddgi-async"},
                     "asyncActive": mode == "ddgi-async", "asyncSubmittedUpdates": frame,
                     "asyncPublishedUpdates": frame - 1}}
        samples.append({"frame": frame, "png": f"frame-{frame}.png", "telemetry": telemetry})
    return {"mode": mode, "technique": "deferred-raster", "samples": samples}


def synthetic_asynchronous_record():
    ordinary = synthetic_record()["samples"]
    record = {"mode": "raster", "technique": "deferred-raster", "samples": []}
    for frame, source, pending, requests, completed, cancelled, preparation, progress in (
            (3, ordinary[0], False, 0, 0, 0, 0, 0),
            (4, ordinary[0], True, 1, 0, 0, 1, 0),
            (5, ordinary[8], False, 2, 2, 1, 1, 1),
            (10, ordinary[8], False, 2, 2, 1, 1, 1),
            (20, ordinary[11], False, 3, 3, 1, 2, 2)):
        telemetry = copy.deepcopy(source["telemetry"])
        telemetry["gui"]["modelLoading"] = {
            "pending": pending, "requests": requests, "completed": completed,
            "cancelled": cancelled, "preparationFrames": preparation,
            "progressFrames": progress, "eventPumps": 2 * progress}
        if pending:
            telemetry["gui"]["status"] = "Reading and preparing model: large.ifcx"
        record["samples"].append({"frame": frame, "telemetry": telemetry})
    return record


class GuiModelLifecycleHarnessTests(unittest.TestCase):
    def test_sequences_use_gui_requests_and_real_unload_with_failure_event_samples(self):
        sequence = gui.lifecycle_sequence(CORNELL, WALL, BIM, "missing.gltf", "bad.gltf",
                                          "missing.ifcx", "bad.ifcx", "bad.bim", "ddgi-async")
        self.assertEqual(sequence["sampleFrames"], FRAMES)
        self.assertEqual(sequence["frames"], 144)
        self.assertTrue(all("reload" not in event and "guiSampleModel" not in event
                            for event in sequence["events"]))
        unload = [event for event in sequence["events"] if event.get("guiUnload")]
        self.assertEqual([event["frame"] for event in unload], [68, 76, 136])
        self.assertTrue(all("guiReload" not in event and "ddgiFitProbes" not in event for event in unload))
        self.assertEqual([event["frame"] for event in sequence["events"] if "ddgiFitProbes" in event],
                         [12, 24, 84, 96, 124])
        for failed in (36, 44, 52, 56, 60, 108, 116, 120):
            self.assertIn(failed, sequence["sampleFrames"])

    def test_commands_keep_gui_enabled_and_distinguish_raster_ray_taa_async_modes(self):
        for mode in gui.LIFECYCLE_MODES:
            command = gui.lifecycle_command("renderer.exe", Path("out"), "case", CORNELL,
                                            "script.json", "forward-raster", mode)
            self.assertNotIn("--no-ui", command)
            self.assertIn("--validation", command)
            self.assertEqual(command[command.index("--render-technique") + 1], "forward-raster")
            self.assertIn("--no-taa" if mode == "raster" else "--taa", command)
            self.assertEqual("--ddgi-async" in command, mode == "ddgi-async")
            self.assertEqual("--no-ray-query" in command, mode == "raster")
        with self.assertRaisesRegex(AssertionError, "mode"):
            gui.lifecycle_command("exe", Path("out"), "name", CORNELL, "sequence", "forward-raster", "bad")

    def test_capture_paths_require_exact_order_and_use_final_capture_name(self):
        files = gui.capture_paths(Path("out"), "case", {"frames": 20, "sampleFrames": [1, 8, 20]})
        self.assertEqual([path.name for _, path in files], ["case.frame-0001.png", "case.frame-0008.png", "case.png"])
        for frames in ([], [8, 8], [20, 8], [0], [21], [True]):
            with self.assertRaises(AssertionError):
                gui.capture_paths(Path("out"), "case", {"frames": 20, "sampleFrames": frames})

    def test_synthetic_mode_matrix_passes_all_state_and_history_assertions(self):
        for mode in gui.LIFECYCLE_MODES:
            with self.subTest(mode=mode):
                self.assertEqual(gui.check_lifecycle(synthetic_record(mode), CORNELL, WALL, BIM,
                                                    image_checks=False), {})

    def test_capture_requires_completed_nonpending_gui_model_requests(self):
        for key, value in (("pending", True), ("pending", 0), ("requests", 0), ("completed", 0),
                           ("requests", True), ("completed", -1)):
            record = synthetic_record()
            record["samples"][1]["telemetry"]["gui"]["modelLoading"][key] = value
            with self.subTest(key=key, value=value), self.assertRaisesRegex(
                    AssertionError, "pending|counter|exactly once"):
                gui.check_lifecycle(record, CORNELL, WALL, BIM, image_checks=False)
        record = synthetic_record()
        del record["samples"][1]["telemetry"]["gui"]["modelLoading"]
        with self.assertRaisesRegex(AssertionError, "Missing model loading telemetry"):
            gui.check_lifecycle(record, CORNELL, WALL, BIM, image_checks=False)

    def test_model_preparation_must_render_before_first_gui_load_finishes(self):
        record = synthetic_record()
        for sample in record["samples"]:
            sample["telemetry"]["gui"]["modelLoading"]["preparationFrames"] = 0
        with self.assertRaisesRegex(AssertionError, "blocked rendering during preparation"):
            gui.check_lifecycle(record, CORNELL, WALL, BIM, image_checks=False)

    def test_every_successful_load_and_unload_requires_rendered_progress_frame(self):
        for frame in (20, 32, 72, 80, 92, 104, 132, 144):
            record = synthetic_record()
            index = FRAMES.index(frame)
            state = record["samples"][index]["telemetry"]["gui"]["modelLoading"]
            before = record["samples"][index - 1]["telemetry"]["gui"]["modelLoading"]
            state["progressFrames"] = before["progressFrames"]
            with self.subTest(frame=frame), self.assertRaisesRegex(AssertionError, "progress frame"):
                gui.check_lifecycle(record, CORNELL, WALL, BIM, image_checks=False)

    def test_every_successful_load_and_unload_requires_window_event_pump(self):
        for frame in (20, 32, 72, 80, 92, 104, 132, 144):
            record = synthetic_record()
            index = FRAMES.index(frame)
            state = record["samples"][index]["telemetry"]["gui"]["modelLoading"]
            before = record["samples"][index - 1]["telemetry"]["gui"]["modelLoading"]
            state["eventPumps"] = before["eventPumps"]
            with self.subTest(frame=frame), self.assertRaisesRegex(AssertionError, "window events"):
                gui.check_lifecycle(record, CORNELL, WALL, BIM, image_checks=False)

    def test_progress_telemetry_cannot_regress_or_use_invalid_counters(self):
        for key in ("requests", "completed", "preparationFrames", "progressFrames", "eventPumps"):
            for value in (-1, True, 1.5, None):
                record = synthetic_record()
                record["samples"][3]["telemetry"]["gui"]["modelLoading"][key] = value
                with self.subTest(key=key, value=value), self.assertRaisesRegex(AssertionError, "counter"):
                    gui.check_lifecycle(record, CORNELL, WALL, BIM, image_checks=False)
        record = synthetic_record()
        record["samples"][3]["telemetry"]["gui"]["modelLoading"]["progressFrames"] = 1
        with self.assertRaisesRegex(AssertionError, "counter progressFrames regressed"):
            gui.check_lifecycle(record, CORNELL, WALL, BIM, image_checks=False)

    def test_asynchronous_case_captures_live_frame_then_cancels_before_commit(self):
        sequence = gui.asynchronous_cancel_sequence(CORNELL, "large.ifcx")
        self.assertEqual(sequence["sampleFrames"], [3, 4, 5, 10, 20])
        self.assertEqual(sequence["events"][0],
                         {"frame": 4, "guiReload": "large.ifcx", "guiWaitForLoad": False})
        self.assertEqual(sequence["events"][1], {"frame": 5, "guiUnload": True})
        self.assertEqual(sequence["events"][2], {"frame": 12, "guiReload": str(CORNELL)})
        gui.check_asynchronous_cancellation(synthetic_asynchronous_record(), CORNELL)

    def test_asynchronous_case_rejects_stalled_or_early_committed_preparation(self):
        for key, value in (("pending", False), ("requests", 0), ("completed", 1),
                           ("preparationFrames", 0)):
            record = synthetic_asynchronous_record()
            record["samples"][1]["telemetry"]["gui"]["modelLoading"][key] = value
            with self.subTest(key=key), self.assertRaisesRegex(AssertionError, "preparation|rendering"):
                gui.check_asynchronous_cancellation(record, CORNELL)
        record = synthetic_asynchronous_record()
        record["samples"][1]["telemetry"]["scene"]["primary"] = "large.ifcx"
        with self.assertRaisesRegex(AssertionError, "modified the live scene"):
            gui.check_asynchronous_cancellation(record, CORNELL)

    def test_asynchronous_case_rejects_late_scene_after_unload_and_missing_cancellation(self):
        record = synthetic_asynchronous_record()
        record["samples"][3]["telemetry"]["scene"]["auxiliary"] = "large.ifcx"
        with self.assertRaisesRegex(AssertionError, "Unload retained"):
            gui.check_asynchronous_cancellation(record, CORNELL)
        for value in (0, 2, True, None):
            record = synthetic_asynchronous_record()
            record["samples"][4]["telemetry"]["gui"]["modelLoading"]["cancelled"] = value
            with self.subTest(value=value), self.assertRaisesRegex(AssertionError, "one cancellation"):
                gui.check_asynchronous_cancellation(record, CORNELL)
        record = synthetic_asynchronous_record()
        record["samples"][4]["telemetry"]["gui"]["modelLoading"]["completed"] = 2
        with self.assertRaisesRegex(AssertionError, "exactly once"):
            gui.check_asynchronous_cancellation(record, CORNELL)

    def test_pixels_detect_missing_models_and_failed_load_scene_changes(self):
        with tempfile.TemporaryDirectory() as temporary:
            record = synthetic_record()
            for sample in record["samples"]:
                path = Path(temporary) / f"frame-{sample['frame']}.png"
                sample["png"] = str(path)
                Image.new("RGB", (960, 540), "black" if sample["frame"] in (72, 80, 144)
                          else "white").save(path)
            metrics = gui.check_lifecycle(record, CORNELL, WALL, BIM)
            self.assertEqual(metrics["failedRequestRgbMae"], 0)
            self.assertGreater(metrics["loadedVsEmptyMae132"], .001)
            failed = next(sample for sample in record["samples"] if sample["frame"] == 120)
            Image.new("RGB", (960, 540), "gray").save(failed["png"])
            with self.assertRaisesRegex(AssertionError, "Failed load changed.*pixels"):
                gui.check_lifecycle(record, CORNELL, WALL, BIM)
            Image.new("RGB", (960, 540), "white").save(failed["png"])
            bim = next(sample for sample in record["samples"] if sample["frame"] == 132)
            Image.new("RGB", (960, 540), "black").save(bim["png"])
            with self.assertRaisesRegex(AssertionError, "empty-scene image"):
                gui.check_lifecycle(record, CORNELL, WALL, BIM)

    def test_image_comparison_excludes_gui_error_messages_and_top_navigation(self):
        with tempfile.TemporaryDirectory() as temporary:
            record = synthetic_record()
            for sample in record["samples"]:
                path = Path(temporary) / f"frame-{sample['frame']}.png"
                sample["png"] = str(path)
                image = Image.new("RGB", (960, 540), "black" if sample["frame"] in (72, 80, 144)
                                  else "white")
                if sample["frame"] in (36, 44, 52, 56, 60, 108, 116, 120):
                    image.paste("red", (0, 0, 350, 540))
                    image.paste("red", (350, 0, 960, 70))
                image.save(path)
            self.assertEqual(gui.check_lifecycle(record, CORNELL, WALL, BIM)["failedRequestRgbMae"], 0)

    def test_unload_requires_zero_real_content_selection_ray_and_gi_state(self):
        for category, field, invalid in (("scene", "primary", "old.gltf"),
            ("scene", "primaryObjectCount", 1), ("scene", "primaryNodeCount", 1),
            ("scene", "primaryIndexCount", 3), ("scene", "primaryVertexCount", 3),
            ("scene", "auxiliaryObjectCount", 1), ("gui", "selectedMeshNodeId", 2),
            ("gui", "selectedBimObjectIndex", 3), ("gui", "status", "Loaded default model"),
            ("rayShadows", "active", True), ("ddgi", "active", True), ("ddgi", "updatedProbes", 1)):
            record = synthetic_record()
            record["samples"][8]["telemetry"][category][field] = invalid
            with self.subTest(field=field), self.assertRaises(AssertionError):
                gui.check_lifecycle(record, CORNELL, WALL, BIM, image_checks=False)
        record = synthetic_record()
        record["samples"][8]["telemetry"]["rayShadows"]["build"]["instances"] = 1
        with self.assertRaisesRegex(AssertionError, "acceleration"):
            gui.check_lifecycle(record, CORNELL, WALL, BIM, image_checks=False)

    def test_failed_load_must_preserve_provider_counts_camera_and_lighting(self):
        mutations = [lambda t: t["scene"].update(auxiliary="different.ifcx"),
                     lambda t: t["scene"].update(auxiliaryObjectCount=1),
                     lambda t: t["camera"].update(position=[0, 0, 4]),
                     lambda t: t["camera"]["unjitteredViewProjColumns"][0].__setitem__(0, 2),
                     lambda t: t["lighting"].update(environmentIntensity=0),
                     lambda t: t["gui"].update(status="Loaded model: missing.gltf")]
        for mutate in mutations:
            record = synthetic_record()
            mutate(record["samples"][3]["telemetry"])
            with self.assertRaises(AssertionError):
                gui.check_lifecycle(record, CORNELL, WALL, BIM, image_checks=False)

    def test_failed_load_must_preserve_temporal_ray_gi_history_settings_and_selection(self):
        for category, field, invalid in (("taa", "epoch", 999), ("rayShadows", "generation", 999),
            ("gui", "selectedMeshNodeId", 99), ("gui", "selectedBimObjectIndex", 99),
            ("ddgi", "historyResets", 999), ("ddgi", "contentEpoch", 999),
            ("ddgi", "paused", True), ("ddgi", "settings", {"asyncCompute": True, "intensity": 999})):
            record = synthetic_record("ddgi-async")
            record["samples"][3]["telemetry"][category][field] = invalid
            with self.subTest(field=field), self.assertRaisesRegex(AssertionError, "history/settings/selection"):
                gui.check_lifecycle(record, CORNELL, WALL, BIM, image_checks=False)

    def test_failed_load_allows_natural_async_publication_and_update_progress(self):
        record = synthetic_record("ddgi-async")
        for sample in record["samples"]:
            if sample["frame"] in (36, 44, 52, 56, 60, 108, 116, 120):
                sample["telemetry"]["ddgi"].update(asyncSubmittedUpdates=1000, asyncPublishedUpdates=999,
                                                  updateFrame=1000, updatedProbes=32)
        gui.check_lifecycle(record, CORNELL, WALL, BIM, image_checks=False)

    def test_replacement_cannot_resurrect_old_provider_or_leave_empty_hierarchy(self):
        for field, invalid in (("auxiliary", "startup.ifcx"), ("auxiliaryObjectCount", 4),
                               ("primaryNodeCount", 0), ("primaryVertexCount", 0), ("primaryIndexCount", 0)):
            record = synthetic_record()
            record["samples"][11]["telemetry"]["scene"][field] = invalid
            with self.subTest(field=field), self.assertRaises(AssertionError):
                gui.check_lifecycle(record, CORNELL, WALL, BIM, image_checks=False)
        for field in ("primaryIndexCount", "primaryVertexCount", "primaryNodeCount"):
            record = synthetic_record()
            record["samples"][1]["telemetry"]["scene"][field] = 1
            with self.subTest(primary=field), self.assertRaisesRegex(AssertionError, "primary geometry/hierarchy"):
                gui.check_lifecycle(record, CORNELL, WALL, BIM, image_checks=False)
        for field in ("selectedMeshNodeId", "selectedBimObjectIndex"):
            record = synthetic_record()
            record["samples"][11]["telemetry"]["gui"][field] = 0
            with self.subTest(selection=field), self.assertRaisesRegex(AssertionError, "stale selection"):
                gui.check_lifecycle(record, CORNELL, WALL, BIM, image_checks=False)

    def test_reload_requires_fresh_temporal_and_acceleration_generations(self):
        record = synthetic_record()
        for sample in record["samples"]:
            if sample["frame"] in (104, 108, 116, 120):
                sample["telemetry"]["taa"]["epoch"] = 1
        with self.assertRaisesRegex(AssertionError, "temporal"):
            gui.check_lifecycle(record, CORNELL, WALL, BIM, image_checks=False)
        record = synthetic_record("rays-taa")
        record["samples"][15]["telemetry"]["rayShadows"]["generation"] = 1
        with self.assertRaisesRegex(AssertionError, "acceleration generation"):
            gui.check_lifecycle(record, CORNELL, WALL, BIM, image_checks=False)

    def test_failed_reload_repeated_gltf_and_unload_cannot_grow_geometry_arenas(self):
        record = synthetic_record()
        record["samples"][3]["telemetry"]["scene"]["geometryArenaCapacityBytes"] *= 2
        with self.assertRaisesRegex(AssertionError, "Failed request changed scene"):
            gui.check_lifecycle(record, CORNELL, WALL, BIM, image_checks=False)
        record = synthetic_record()
        for sample in record["samples"]:
            if 92 <= sample["frame"] <= 120:
                sample["telemetry"]["scene"]["geometryArenaCapacityBytes"] *= 2
        with self.assertRaisesRegex(AssertionError, "Repeated glTF load grew geometry arena capacity"):
            gui.check_lifecycle(record, CORNELL, WALL, BIM, image_checks=False)
        record = synthetic_record()
        record["samples"][-1]["telemetry"]["scene"]["geometryArenaCapacityBytes"] *= 2
        with self.assertRaisesRegex(AssertionError, "Unload grew geometry arena capacity"):
            gui.check_lifecycle(record, CORNELL, WALL, BIM, image_checks=False)

    def test_async_reload_requires_supported_active_published_or_reported_fallback(self):
        for field, invalid in (("active", False), ("supported", False), ("updateFrame", 0),
                               ("asyncPublishedUpdates", 0), ("asyncSubmittedUpdates", 0)):
            record = synthetic_record("ddgi-async")
            record["samples"][2]["telemetry"]["ddgi"][field] = invalid
            with self.subTest(field=field), self.assertRaises(AssertionError):
                gui.check_lifecycle(record, CORNELL, WALL, BIM, image_checks=False)
        record = synthetic_record("ddgi-async")
        for sample in record["samples"]:
            gi_state = sample["telemetry"]["ddgi"]
            gi_state.update(asyncActive=False, status="DDGI active; serialized fallback: no queue",
                            asyncSubmittedUpdates=0, asyncPublishedUpdates=0)
        gui.check_lifecycle(record, CORNELL, WALL, BIM, image_checks=False)
        record["samples"][2]["telemetry"]["ddgi"]["status"] = "active"
        with self.assertRaisesRegex(AssertionError, "fallback"):
            gui.check_lifecycle(record, CORNELL, WALL, BIM, image_checks=False)

    def test_missing_duplicate_samples_cannot_satisfy_state_checks(self):
        for mutation in (lambda samples: samples.pop(), lambda samples: samples.append(copy.deepcopy(samples[0]))):
            record = synthetic_record()
            mutation(record["samples"])
            with self.assertRaisesRegex(AssertionError, "samples"):
                gui.check_lifecycle(record, CORNELL, WALL, BIM, image_checks=False)

    def test_stale_success_artifacts_are_removed_and_missing_new_output_fails(self):
        with tempfile.TemporaryDirectory() as temporary:
            output = Path(temporary)
            png = output / "case.png"
            Image.new("RGB", (960, 540), "white").save(png)
            metadata = png.with_suffix(".telemetry.json")
            metadata.write_text("{}")
            with patch.object(gui.subprocess, "run", return_value=subprocess.CompletedProcess([], 0)):
                with self.assertRaisesRegex(AssertionError, "Missing.*image"):
                    gui.run_lifecycle_capture(Path("renderer.exe"), output, "case", CORNELL,
                        {"frames": 1}, "deferred-raster", "raster")
            self.assertFalse(png.exists())
            self.assertFalse(metadata.exists())

    def test_errors_timeout_and_nonzero_process_exit_cannot_pass(self):
        for log_text, exit_code in (("VUID-1234", 0), ("SYNC-HAZARD-write", 0),
                                  ("Unhandled exception", 0), ("", 2)):
            with tempfile.TemporaryDirectory() as temporary:
                def process(*args, **kwargs):
                    kwargs["stdout"].write(log_text)
                    return subprocess.CompletedProcess([], exit_code)
                with patch.object(gui.subprocess, "run", side_effect=process), self.assertRaises(AssertionError):
                    gui.run_lifecycle_capture(Path("renderer.exe"), Path(temporary), "case", CORNELL,
                        {"frames": 1}, "deferred-raster", "raster")
        with tempfile.TemporaryDirectory() as temporary:
            with patch.object(gui.subprocess, "run", side_effect=subprocess.TimeoutExpired("exe", 1)):
                with self.assertRaises(subprocess.TimeoutExpired):
                    gui.run_lifecycle_capture(Path("renderer.exe"), Path(temporary), "case", CORNELL,
                        {"frames": 1}, "deferred-raster", "raster", timeout=1)
        self.assertIsNone(gui.VALIDATION_ERROR.search("Failed to load model: missing.ifcx\nparse error"))

    def test_fresh_complete_capture_is_accepted_and_not_an_exit_code_only_pass(self):
        with tempfile.TemporaryDirectory() as temporary:
            output = Path(temporary)
            def process(*args, **kwargs):
                Image.new("RGB", (960, 540), "white").save(output / "case.png")
                (output / "case.telemetry.json").write_text(json.dumps(
                    {"schemaVersion": 1, "technique": "deferred-raster"}), encoding="utf-8")
                return subprocess.CompletedProcess([], 0)
            with patch.object(gui.subprocess, "run", side_effect=process):
                record = gui.run_lifecycle_capture(Path("renderer.exe"), output, "case", CORNELL,
                    {"frames": 1}, "deferred-raster", "raster")
            self.assertEqual(len(record["samples"]), 1)
            self.assertEqual(record["samples"][0]["frame"], 1)

    def test_exit_zero_rejects_missing_nonfinite_or_mismatched_capture_metadata(self):
        for metadata in (None, {"schemaVersion": 2, "technique": "deferred-raster"},
                         {"schemaVersion": 1, "technique": "forward-raster"},
                         {"schemaVersion": 1, "technique": "deferred-raster", "nested": [float("nan")]}):
            with self.subTest(metadata=metadata), tempfile.TemporaryDirectory() as temporary:
                output = Path(temporary)
                def process(*args, **kwargs):
                    Image.new("RGB", (960, 540), "white").save(output / "case.png")
                    if metadata is not None:
                        (output / "case.telemetry.json").write_text(json.dumps(metadata), encoding="utf-8")
                    return subprocess.CompletedProcess([], 0)
                with patch.object(gui.subprocess, "run", side_effect=process):
                    with self.assertRaises((AssertionError, FileNotFoundError)):
                        gui.run_lifecycle_capture(Path("renderer.exe"), output, "case", CORNELL,
                            {"frames": 1}, "deferred-raster", "raster")

    def test_exit_zero_rejects_wrong_sized_or_corrupt_image(self):
        for corrupt in (False, True):
            with self.subTest(corrupt=corrupt), tempfile.TemporaryDirectory() as temporary:
                output = Path(temporary)
                def process(*args, **kwargs):
                    if corrupt:
                        (output / "case.png").write_bytes(b"not a PNG")
                    else:
                        Image.new("RGB", (960, 539), "white").save(output / "case.png")
                    (output / "case.telemetry.json").write_text(json.dumps(
                        {"schemaVersion": 1, "technique": "deferred-raster"}), encoding="utf-8")
                    return subprocess.CompletedProcess([], 0)
                with patch.object(gui.subprocess, "run", side_effect=process):
                    with self.assertRaises((AssertionError, OSError)):
                        gui.run_lifecycle_capture(Path("renderer.exe"), output, "case", CORNELL,
                            {"frames": 1}, "deferred-raster", "raster")

    def test_nonfinite_nested_telemetry_is_rejected(self):
        self.assertTrue(gui.finite_json({"nested": [1, "value", {"x": .5}]}))
        self.assertFalse(gui.finite_json({"nested": [0, {"x": float("nan")}] }))
        self.assertFalse(gui.finite_json({"nested": {"x": float("inf")}}))

    def test_mixed_startup_is_replaced_by_one_primary_and_then_truly_unloaded(self):
        ordinary = synthetic_record()["samples"]
        mixed = copy.deepcopy(ordinary[0]["telemetry"])
        mixed["scene"]["auxiliary"] = str(WALL)
        mixed["scene"]["auxiliaryObjectCount"] = 4
        mixed["gui"]["selectedBimObjectIndex"] = 0
        record = {"samples": [{"frame": 8, "telemetry": mixed},
                              {"frame": 20, "telemetry": copy.deepcopy(ordinary[11]["telemetry"])},
                              {"frame": 36, "telemetry": copy.deepcopy(ordinary[8]["telemetry"])}]}
        for requests, sample in enumerate(record["samples"]):
            sample["telemetry"]["gui"]["modelLoading"] = {
                "pending": False, "requests": requests, "completed": requests,
                "preparationFrames": int(requests > 0), "progressFrames": requests,
                "eventPumps": requests}
        gui.check_mixed_replacement(record, CORNELL, WALL)
        unselected = copy.deepcopy(record)
        unselected["samples"][0]["telemetry"]["gui"]["selectedBimObjectIndex"] = -1
        with self.assertRaisesRegex(AssertionError, "select a real BIM object"):
            gui.check_mixed_replacement(unselected, CORNELL, WALL)
        record["samples"][1]["telemetry"]["scene"]["auxiliary"] = str(WALL)
        with self.assertRaisesRegex(AssertionError, "Old model provider"):
            gui.check_mixed_replacement(record, CORNELL, WALL)


if __name__ == "__main__":
    unittest.main()
