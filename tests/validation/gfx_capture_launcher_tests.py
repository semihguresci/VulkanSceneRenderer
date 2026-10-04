"""Launcher contract tests without a Vulkan runtime or external tool installation."""
import argparse
import importlib.util
import io
import json
import os
from pathlib import Path
import struct
import subprocess
import tempfile
import unittest
from contextlib import redirect_stderr, redirect_stdout
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location("gfx_launcher", ROOT / "tools/gfxreconstruct.py")
gfx = importlib.util.module_from_spec(spec)
spec.loader.exec_module(gfx)


def pe(path, machine=0x8664):
    data = bytearray(70)
    data[:2] = b"MZ"
    struct.pack_into("<I", data, 60, 64)
    data[64:68] = b"PE\0\0"
    struct.pack_into("<H", data, 68, machine)
    path.write_bytes(data)


class LauncherTests(unittest.TestCase):
    def test_ranges_reject_empty_zero_overflow_overlap_reordering_and_trailing_data(self):
        self.assertEqual(gfx.frame_ranges("9-12,20"), 20)
        for value in ("", "0", "-1", "4-2", "1,1", "1-3,3-5", "2,1", "1,", "1, 2", "1foo", "4294967296"):
            with self.subTest(value=value), self.assertRaises(ValueError):
                gfx.frame_ranges(value)

    def test_architecture_is_read_from_binary_not_filename(self):
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / "renderer.exe"
            for code, expected in ((0x8664, "x64"), (0xAA64, "arm64"), (0x14C, "x86")):
                pe(path, code)
                self.assertEqual(gfx.architecture(path), expected)
            path.write_bytes(b"not an executable")
            with self.assertRaises(ValueError):
                gfx.architecture(path)

    def test_inspection_process_is_not_accidentally_captured(self):
        parent = {"GFXRECON_CAPTURE_FILE": "old.gfxr", "VK_INSTANCE_LAYERS": gfx.LAYER, "OTHER": "keep"}
        env = gfx.tool_environment(parent)
        self.assertEqual(env["GFXRECON_DISABLE"], "1")
        self.assertNotIn("GFXRECON_CAPTURE_FILE", env)
        self.assertEqual(env["OTHER"], "keep")
        self.assertEqual(parent["GFXRECON_CAPTURE_FILE"], "old.gfxr")

    def test_explicit_missing_tools_does_not_fall_back_to_sdk(self):
        with tempfile.TemporaryDirectory() as folder, self.assertRaises(ValueError):
            gfx.discover_tools(folder)

    def test_requested_capture_options_clear_inherited_ranges_and_do_not_mutate_parent(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            exe = root / "renderer with spaces.exe"
            pe(exe)
            settings = {"machine": "x64", "manifestPath": str(root / "VkLayer_gfxreconstruct.json"),
                        "supportedSettings": ["capture_frames", "capture_trigger", "capture_file", "memory_tracking_mode"]}
            args = argparse.Namespace(exe=str(exe), frames="9", trigger=None, all_frames=False,
                    trigger_frames=None, tools=str(root), renderer_args=["--", "--model", "a model.gltf"],
                    output_dir=str(root / "capture with spaces"), capture_name="cornell-shadow", compression="LZ4", memory_mode="page_guard",
                    log_level="info", dry_run=False)
            parent = {"VK_LAYER_PATH": "existing-path", "VK_INSTANCE_LAYERS": "VK_LAYER_KHRONOS_validation",
                      "GFXRECON_CAPTURE_TRIGGER": "F3", "GFXRECON_DISABLE": "1"}
            def child(command, **kwargs):
                env = kwargs["env"]
                self.assertEqual(env["GFXRECON_CAPTURE_FRAMES"], "9")
                self.assertEqual(env["GFXRECON_CAPTURE_TRIGGER"], "")
                self.assertEqual(Path(env["GFXRECON_CAPTURE_FILE"]).name, "cornell-shadow.gfxr")
                self.assertNotIn("GFXRECON_DISABLE", env)
                self.assertEqual(env["VK_INSTANCE_LAYERS"], "VK_LAYER_KHRONOS_validation")
                self.assertIn("existing-path", env["VK_LAYER_PATH"])
                self.assertEqual(command[-2:], ["--model", "a model.gltf"])
                Path(env["GFXRECON_CAPTURE_FILE"]).write_bytes(b"fake capture")
                return subprocess.CompletedProcess(command, 0)
            with patch.dict(os.environ, parent, clear=True), patch.object(gfx, "discover_tools", return_value=(root, settings, "1.0.5")), patch.object(gfx.subprocess, "run", side_effect=child), redirect_stdout(io.StringIO()):
                self.assertEqual(gfx.capture(args), 0)
                self.assertEqual(os.environ["GFXRECON_CAPTURE_TRIGGER"], "F3")
                self.assertEqual(os.environ["GFXRECON_DISABLE"], "1")
                with self.assertRaises(ValueError):
                    gfx.capture(args)  # Existing artifacts must survive a repeated launch.
            session = json.loads((Path(args.output_dir) / "session.json").read_text())
            self.assertEqual(session["stopAfterPresent"], 9)
            self.assertEqual(Path(session["captureFile"]).name, "cornell-shadow.gfxr")

    def test_cli_rejects_conflicting_modes_and_reserved_hotkeys(self):
        with redirect_stderr(io.StringIO()):
            with self.assertRaises(SystemExit) as result:
                gfx.main(["capture", "--exe", "missing.exe", "--frames", "9", "--trigger", "F3"])
            self.assertEqual(result.exception.code, 2)
            reserved = ["F6", "F7", "F8", "TAB", "CONTROL"] + (["F12"] if os.name == "nt" else [])
            with patch.object(gfx, "discover_tools") as discover:
                for key in reserved:
                    self.assertEqual(gfx.main(["capture", "--exe", __file__, "--trigger", key]), 2)
                discover.assert_not_called()
            self.assertEqual(gfx.main(["capture", "--exe", "missing.exe", "--frames", "0"]), 2)
            self.assertEqual(gfx.main(["capture", "--exe", "missing.exe", "--frames", ""]), 2)
            self.assertEqual(gfx.main(["capture", "--exe", "missing.exe", "--trigger", ""]), 2)
            self.assertEqual(gfx.main(["capture", "--exe", "missing.exe", "--trigger-frames", "0"]), 2)
            for name in ("", "../capture", "CON", "NUL.gfxr", "COM1", "capture."):
                self.assertEqual(gfx.main(["capture", "--exe", "missing.exe", "--capture-name", name]), 2)

    def test_default_hotkey_arms_f3_without_mutating_inherited_settings(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            exe = root / "renderer.exe"
            pe(exe)
            layer = {"machine": "x64", "manifestPath": str(root / "layer.json"),
                     "supportedSettings": ["capture_frames", "capture_trigger", "capture_file", "memory_tracking_mode"]}
            output = io.StringIO()
            with patch.dict(os.environ, {"GFXRECON_CAPTURE_TRIGGER": "F12"}), patch.object(gfx, "discover_tools", return_value=(root, layer, "1.0.5")), redirect_stdout(output):
                self.assertEqual(gfx.main(["capture", "--exe", str(exe), "--dry-run"]), 0)
                self.assertEqual(os.environ["GFXRECON_CAPTURE_TRIGGER"], "F12")
            session = json.loads(output.getvalue())["session"]
            self.assertEqual(session["mode"], "hotkey")
            self.assertEqual(session["trigger"], "F3")
            self.assertEqual(session["environment"]["GFXRECON_CAPTURE_TRIGGER"], "F3")

    def test_unsupported_tool_option_fails_before_running_capture(self):
        with patch.object(gfx, "run_text", return_value="--help --version"), self.assertRaises(ValueError):
            gfx.require_options(Path("gfxrecon-replay"), ["--screenshots"])

    def test_replay_preserves_native_failure_codes_and_catches_logged_errors(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            source = root / "capture.gfxr"
            source.write_bytes(b"fixture")
            args = argparse.Namespace(capture=str(source), tools=str(root), output_dir=None,
                    screenshots=None, offscreen=False, validation=True, memory_translation="rebind",
                    extract_shaders=False, json_lines=False)
            for failure in ("info", "replay-log"):
                args.output_dir = str(root / failure)
                def tool(command, **kwargs):
                    if "gfxrecon-info" in command[0] and failure == "info":
                        return subprocess.CompletedProcess(command, 0xC0000005)
                    if "gfxrecon-replay" in command[0]:
                        kwargs["stdout"].write("[gfxrecon] ERROR replay failed\n")
                    return subprocess.CompletedProcess(command, 0)
                with patch.object(gfx, "discover_tools", return_value=(root, {}, "1.0.5")), patch.object(gfx, "require_options"), patch.object(gfx.subprocess, "run", side_effect=tool), redirect_stderr(io.StringIO()):
                    self.assertEqual(gfx.replay(args), 1 if failure == "info" else 2)
                report = json.loads((Path(args.output_dir) / "result.json").read_text())
                if failure == "info":
                    self.assertEqual(report["infoExitCode"], 0xC0000005)
                    self.assertEqual(report["failedStep"], "Inspection")
                else:
                    self.assertEqual(report["toolExitCode"], 0)
                    self.assertEqual(report["diagnosticErrors"], 1)


if __name__ == "__main__":
    unittest.main()
