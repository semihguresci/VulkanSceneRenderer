"""Test the dataset preview's CPU inputs, completion checks and diagnostics."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch


sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools"))
import robotics_preview_dataset as preview


class DatasetPreviewTests(unittest.TestCase):
    simulator: Path
    recorder: Path

    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="robotics-preview-")
        self.addCleanup(temporary.cleanup)
        self.work = Path(temporary.name)

    def simulate(self, name):
        directory = self.work / name
        directory.mkdir()
        preview.write_inputs(directory)
        report = directory / "simulation.json"
        preview.run_stage("simulate", self.simulator,
                          (preview.MODEL, report, preview.NAMESPACE, directory / "scenario.json"),
                          directory, 15)
        return report

    def stub_bin_dir(self):
        directory = self.work / "preview binaries"
        directory.mkdir()
        suffix = ".exe" if os.name == "nt" else ""
        for name in ("BimRoboticsSim", "VulkanSceneRenderer", "BimRoboticsDataset"):
            (directory / (name + suffix)).touch()
        return directory

    def test_generated_inputs_drive_repeatable_native_motion_and_sensor_cadence(self):
        report = self.simulate("first")
        repeat = self.simulate("repeat")
        self.assertEqual(report.read_bytes(), repeat.read_bytes())
        simulation = preview.read_object(report)
        camera = preview.read_object(report.parent / "camera.json")
        self.assertEqual(simulation["sourceNamespace"], "doorway-preview")
        self.assertEqual(simulation["ticksPerSecond"], 12)
        history = simulation["history"]
        self.assertEqual([row["tick"] for row in history], list(range(8)))
        for tick, row in enumerate(history):
            self.assertAlmostEqual(row["seconds"], tick / 12)
        self.assertAlmostEqual(history[0]["base"]["position"][0], -1.5)
        self.assertAlmostEqual(history[-1]["base"]["position"][0], -1.5 + 0.2 * 7 / 12, delta=0.01)
        model_bytes = preview.MODEL.read_bytes()
        self.assertEqual(simulation["sceneSource"]["layers"], [
            {"byteSize": len(model_bytes), "sha256": hashlib.sha256(model_bytes).hexdigest()},
        ])
        imu = simulation["driveSensors"]["samples"]
        self.assertEqual([row["tick"] for row in imu], [1, 4, 7])
        self.assertGreater(imu[-1]["encoderCounts"]["left"], imu[0]["encoderCounts"]["left"])
        lidar = simulation["lidar"]["scans"]
        self.assertEqual([row["tick"] for row in lidar], [2, 6])
        self.assertTrue(any(beam["entityId"] for scan in lidar for beam in scan["returns"]))
        self.assertEqual(camera["timing"], {"ticksPerSecond": 12, "framesPerSecondMilliHz": 6000,
                                            "firstTick": 0, "lastTick": 7})
        self.assertEqual(camera["baseFromCamera"], {"xyz": [0, 0, 0.7]})

    def test_preflight_requires_tools_and_fresh_output_without_running_commands(self):
        new_output = self.work / "new output"
        with patch.object(preview, "run_stage") as stage:
            with self.assertRaisesRegex(RuntimeError, "Missing preview binaries"):
                preview.generate(new_output, self.work / "missing tools", 15)
            self.assertFalse(new_output.exists())
            bins = self.stub_bin_dir()
            new_output.mkdir()
            keep = new_output / "user data.txt"
            keep.write_text("preserve this", encoding="utf-8")
            with self.assertRaisesRegex(RuntimeError, "Output already exists"):
                preview.generate(new_output, bins, 15)
            self.assertEqual(keep.read_text(encoding="utf-8"), "preserve this")
            self.assertEqual(list(new_output.iterdir()), [keep])
            stage.assert_not_called()

    def test_native_record_rejects_camera_from_another_report_without_publishing(self):
        output = self.work / "incomplete preview"
        bins = self.stub_bin_dir()
        native_stage = preview.run_stage
        stages = []

        def dispatch(label, executable, arguments, logs, timeout, **options):
            stages.append(label)
            if label == "capture":
                self.assertEqual(executable.parent, bins)
                self.assertEqual(arguments[arguments.index("--model") + 1], preview.MODEL)
                self.assertEqual(arguments[arguments.index("--sensor-trajectory") + 1], output / "simulation.json")
                self.assertEqual(arguments[arguments.index("--sensor-namespace") + 1], "doorway-preview")
                camera = arguments[arguments.index("--sensor-output") + 1]
                camera.mkdir()
                # Deliberately invalid metadata exercises the native rejection path without any images or GPU.
                preview.write_json(camera / "dataset.json", {
                    "schema": "container.sensor-camera-dataset", "version": 1, "frameCount": 4,
                    "labels": "labels.json", "frameImagePattern": "frame-%08d.exr",
                    "frameMetadataPattern": "frame-%08d.json", "simulationReportSha256": "0" * 64,
                })
                return
            tool = {"simulate": self.simulator, "record": self.recorder}[label]
            native_stage(label, tool, arguments, logs, timeout, **options)

        with patch.object(preview, "run_stage", side_effect=dispatch):
            with self.assertRaisesRegex(RuntimeError, "capture must replay the exact supplied simulation report"):
                preview.generate(output, bins, 15)
        self.assertEqual(stages, ["simulate", "capture", "record"])
        self.assertTrue((output / "simulation.json").is_file())
        record_log = output / "logs" / "record.log"
        self.assertIn("capture must replay the exact supplied simulation report", record_log.read_text(encoding="utf-8"))
        for path in ("dataset/dataset.json", "dataset/.recording", "replay.jsonl", "summary.json"):
            self.assertFalse((output / path).exists(), path)

    def test_cpu_replay_metadata_verifies_ticks_motion_and_camera_alignment(self):
        report = self.simulate("replay source")
        dataset = self.work / "dataset"
        dataset.mkdir()
        verified = self.work / "verify.json"
        replay = self.work / "replay.partial.jsonl"
        manifest = {
            "schema": "container.robotics-dataset", "version": 1, "sourceNamespace": "doorway-preview",
            "ticksPerSecond": 12, "tickCount": 8, "ticksPerChunk": 3, "chunkCount": 3,
            "cameraFrameCount": 4, "imuSampleCount": 3, "lidarScanCount": 2,
        }
        preview.write_json(dataset / "dataset.json", manifest)
        preview.write_json(verified, manifest)
        native = preview.read_object(report)
        imu = {sample["tick"]: sample for sample in native["driveSensors"]["samples"]}
        lidar = {sample["tick"]: sample for sample in native["lidar"]["scans"]}
        # Only the wrapper's JSON agreement check runs here. Native EXR validation has its own GPU test.
        rows = [
            {"tick": tick, "base": base, "imu": imu.get(tick), "lidar": lidar.get(tick),
             "camera": {"sampleIndex": tick // 2} if tick in (0, 2, 4, 6) else None}
            for tick, base in enumerate(native["history"])
        ]

        def write_rows():
            replay.write_text("".join(json.dumps(row) + "\n" for row in rows), encoding="utf-8")

        write_rows()
        self.assertEqual(preview.verify_replay(dataset, report, verified, replay), manifest)
        rows[2]["camera"], rows[3]["camera"] = rows[3]["camera"], rows[2]["camera"]
        write_rows()
        with self.assertRaisesRegex(RuntimeError, "Expected camera samples at ticks"):
            preview.verify_replay(dataset, report, verified, replay)
        rows[-1]["base"] = rows[0]["base"]
        write_rows()
        with self.assertRaisesRegex(RuntimeError, "Replay did not preserve the simulated motion"):
            preview.verify_replay(dataset, report, verified, replay)
        preview.write_json(verified, {**manifest, "tickCount": 7})
        with self.assertRaisesRegex(RuntimeError, "Verification output differs"):
            preview.verify_replay(dataset, report, verified, replay)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--simulator", required=True, type=Path)
    parser.add_argument("--recorder", required=True, type=Path)
    args = parser.parse_args()
    DatasetPreviewTests.simulator = args.simulator.resolve(strict=True)
    DatasetPreviewTests.recorder = args.recorder.resolve(strict=True)
    unittest.main(argv=[sys.argv[0]])


if __name__ == "__main__":
    main()
