"""Reject invalid OpenVDB preview requests before application/GPU startup."""

import argparse
from pathlib import Path
import subprocess
import tempfile
import unittest


class PreviewCliTests(unittest.TestCase):
    def run_cli(self, *arguments):
        return subprocess.run([str(self.exe), *arguments], cwd=self.directory,
                              capture_output=True, text=True, encoding="utf-8",
                              timeout=10)

    def test_help_lists_explicit_routes_and_limits(self):
        result = self.run_cli("--help")
        self.assertEqual(result.returncode, 0, result.stderr)
        for option in ("--vdb-slice", "--vdb-native-points", "--vdb-vector-mode",
                       "--vdb-slice-opacity", "--vdb-slice-integer-range", "--vdb-native-color-attribute",
                       "--vdb-native-any-group", "--vdb-preview-max-bytes"):
            self.assertIn(option, result.stdout)

    def test_invalid_requests_fail_before_model_io_or_vulkan(self):
        cases = [
            ["--vdb-preview-max-bytes", " -1"],
            ["--vdb-point-max-points", "+1"],
            ["--vdb-preview-max-voxels", " 1"],
            ["--vdb-slice", "--vdb-slice-samples", " -18446744073709551614", "3"],
            ["--vdb-slice", "--vdb-native-points"],
            ["--vdb-slice", "--vdb-vector-mode", "glyphs"],
            ["--vdb-native-points", "--vdb-vector-mode", "glyphs"],
            ["--vdb-slice-range", "0", "1"],
            ["--vdb-native-radius", ".1"],
            ["--vdb-slice", "--vdb-preview-color", "uniform"],
            ["--vdb-native-points", "--vdb-preview-speed", "2"],
            ["--vdb-slice", "--vdb-point-max-points", "2"],
            ["--vdb-vector-mode", "glyphs", "--vdb-point-max-points", "2"],
            ["--vdb-vector-mode", "glyphs", "--vdb-preview-max-metadata-bytes", "10"],
            ["--vdb-native-points", "--vdb-native-color", "scalar"],
            ["--vdb-native-points", "--vdb-native-color-attribute", "id"],
            ["--vdb-native-points", "--vdb-native-color", "direction",
             "--vdb-native-color-attribute", "v", "--vdb-native-tint", "1", "0", "0"],
            ["--vdb-native-points", "--vdb-native-radius", ".1",
             "--vdb-native-radius-attribute", "r"],
            ["--vdb-native-points", "--vdb-native-radius-attribute", "P"],
            ["--vdb-native-points", "--vdb-native-radius", "0"],
            ["--vdb-native-points", "--vdb-native-radius-scale", "-1"],
            ["--vdb-native-points", "--vdb-native-opacity", "2"],
            ["--vdb-native-points", "--vdb-native-tint", "-1", "0", "0"],
            ["--vdb-native-points", "--vdb-native-max-attributes", "1025"],
            ["--vdb-native-points", "--vdb-native-max-groups", "4097"],
            ["--vdb-native-points", "--vdb-native-max-grids", "4097"],
            ["--vdb-native-points", "--vdb-native-max-string-bytes", "0"],
            ["--vdb-native-points", "--vdb-native-any-group", ""],
            ["--vdb-point-max-points", "9223372036854775808"],
            ["--vdb-preview-max-triangles", "0"],
            ["--vdb-slice", "--vdb-slice-u", "0", "0", "0"],
            ["--vdb-slice", "--vdb-slice-v", "1", "0", "0"],
            ["--vdb-slice", "--vdb-slice-origin", "1e13", "0", "0"],
            ["--vdb-slice", "--vdb-slice-extent", "0", "1"],
            ["--vdb-slice", "--vdb-slice-samples", "1", "2"],
            ["--vdb-slice", "--vdb-slice-samples", "1024", "1024"],
            ["--vdb-slice", "--vdb-slice-range", "1", "0"],
            ["--vdb-slice", "--vdb-slice-range", "-1e308", "1e308"],
            ["--vdb-slice", "--vdb-slice-integer-range", "1", "1"],
            ["--vdb-slice", "--vdb-slice-integer-range", "0", "9223372036854775808"],
            ["--vdb-slice", "--vdb-slice-integer-range", "-9223372036854775809", "1"],
            ["--vdb-slice", "--vdb-slice-integer-range", " 0", "1"],
            ["--vdb-slice", "--vdb-slice-integer-range", "+0", "1"],
            ["--vdb-slice", "--vdb-slice-integer-range", "0", "1.0"],
            ["--vdb-slice", "--vdb-slice-integer-range", "0", "1", "--vdb-slice-range", "0", "1"],
            ["--vdb-slice", "--vdb-slice-integer-range", "0", "1", "--vdb-slice-value", "x"],
            ["--vdb-slice", "--vdb-slice-integer-range", "0", "1", "--vdb-slice-integer-range", "0", "2"],
            ["--vdb-slice", "--vdb-slice-opacity", "-1"],
            ["--vdb-slice", "--vdb-slice-max-work", "1"],
            ["--vdb-slice", "--vdb-slice-max-samples", "0"],
            ["--vdb-vector-mode", "glyphs", "--vdb-vector-seeds", "256", "256", "256"],
        ]
        for arguments in cases:
            with self.subTest(arguments=arguments):
                result = self.run_cli("--model", "missing-点云.vdb", *arguments)
                self.assertNotEqual(result.returncode, 0, result.stdout)
                self.assertTrue(result.stderr.strip(), result.stdout)
                self.assertRegex(result.stderr.lower(), r"vdb|slice|native|vector|integer")
                for unwanted in ("VUID-", "Validation Error", "VK_ERROR_", "Failed to open"):
                    self.assertNotIn(unwanted, result.stdout + result.stderr)
        self.assertEqual(list(self.directory.iterdir()), [], "parser started application output")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--exe", required=True, type=Path)
    options, extra = parser.parse_known_args()
    PreviewCliTests.exe = options.exe.resolve(strict=True)
    with tempfile.TemporaryDirectory(prefix="container-vdb-preview-cli-") as temporary:
        PreviewCliTests.directory = Path(temporary)
        unittest.main(argv=[__file__, *extra])
