"""Native file tooling with independent upstream fixtures, including no-VDB builds."""
from array import array
import argparse
import json
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "python"))
from container_openvdb import DenseType, DenseVolume, read_dense_array, write_dense_array


class NanoCli(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="container-nano-cli-")
        self.directory = Path(self.temp.name)

    def tearDown(self):
        self.temp.cleanup()

    def call(self, *arguments, succeeds=True):
        result = subprocess.run([str(self.tool), *map(str, arguments)],
                                cwd=self.directory, capture_output=True, text=True,
                                encoding="utf-8", timeout=60)
        self.assertEqual(result.returncode == 0, succeeds,
                         f"{arguments!r}\n{result.stdout}\n{result.stderr}")
        return result

    def report(self, *arguments):
        return json.loads(self.call(*arguments).stdout)

    def native_only(self):
        if not self.native:
            self.skipTest("Native I/O is disabled")

    def fixture(self):
        result = subprocess.run([str(self.generator), "source.nvdb"],
                                cwd=self.directory, capture_output=True, text=True, timeout=60)
        self.assertEqual(result.returncode, 0, result.stderr)
        return self.directory / "source.nvdb"

    def test_capabilities_and_strict_options(self):
        caps = self.report("--nano-capabilities")
        self.assertEqual(caps["nativeIo"], self.native)
        self.assertEqual(caps["openVdbConversion"], self.native and self.openvdb)
        self.assertIn("--checksum-nano", self.call("--help").stdout)
        cases = [
            ("--nano-capabilities", "--codec", "zip"),
            ("--to-nano", "missing"),
            ("--to-nano", "a", "bad.nvdb", "--allow-metadata-loss"),
            ("--copy-nano", "a", "bad.nvdb", "--allow-float-map-rounding"),
            ("--from-nano", "a", "bad.vdb", "--codec", "none"),
            ("--inspect-nano", "a", "--require-full-checksum"),
            ("--checksum-nano", "a", "bad.nvdb", "--grid", "x"),
            ("--validate-nano", "a", "--grid", "x", "--grid", "y"),
            ("--validate-nano", "a", "--max-nodes", " -1"),
            ("--validate-nano", "a", "--max-grids", "4294967296"),
            ("--validate-nano", "a", "--width", "1"),
            ("--to-nano", "a", "bad.nvdb", "--checksum", "other"),
            ("--copy-nano", "a", "bad.nvdb", "--codec", "other"),
            ("--validate-nano", "a", "--native-statistics", "all"),
            ("--stats-nano", "a", "bad.nvdb", "--native-statistics", "invalid"),
        ]
        for arguments in cases:
            with self.subTest(arguments=arguments):
                self.assertTrue(self.call(*arguments, succeeds=False).stderr.strip())
                self.assertFalse((self.directory / "bad.nvdb").exists())
                self.assertFalse((self.directory / "bad.vdb").exists())

    def test_unavailable_routes_are_explicit(self):
        for operation in ("--inspect-nano", "--validate-nano", "--copy-nano", "--checksum-nano", "--stats-nano"):
            if self.native:
                break
            arguments = (operation, "missing")
            if operation in ("--copy-nano", "--checksum-nano", "--stats-nano"):
                arguments += ("bad.nvdb",)
            self.assertIn("ENABLE_NANOVDB_IO", self.call(*arguments, succeeds=False).stderr)
        if not (self.native and self.openvdb):
            for operation in ("--to-nano", "--from-nano"):
                self.assertIn("ENABLE_OPENVDB", self.call(operation, "missing", "bad.vdb", succeeds=False).stderr)

    def test_independent_native_fixture_unicode_and_reencoding(self):
        self.native_only()
        source = self.fixture()
        original = source.read_bytes()
        parent = self.directory / "体積-é"
        parent.mkdir()
        unicode_input = parent / "入力.nvdb"
        shutil.copyfile(source, unicode_input)
        catalog = self.report("--inspect-nano", unicode_input)
        self.assertFalse(catalog["grids"][0]["structureValidated"])
        checked = self.report("--validate-nano", unicode_input, "--require-full-checksum")
        self.assertTrue(checked["grids"][0]["checksumVerified"])
        self.report("--copy-nano", unicode_input, parent / "出力.nvdb", "--raw")
        self.assertEqual((parent / "出力.nvdb").read_bytes(), original)
        caps = self.report("--nano-capabilities")
        for codec in ("none", "zip", "blosc"):
            with self.subTest(codec=codec):
                if codec != "none" and not caps[codec]:
                    self.call("--copy-nano", source, f"{codec}.nvdb", "--codec", codec, succeeds=False)
                    continue
                output = self.report("--copy-nano", source, f"{codec}.nvdb", "--codec", codec)
                self.assertEqual(output["grids"][0]["codec"], codec)
                valid = self.report("--validate-nano", f"{codec}.nvdb")
                self.assertEqual(valid["grids"][0]["activeVoxels"], checked["grids"][0]["activeVoxels"])
                self.assertTrue(valid["grids"][0]["checksumVerified"])
        self.assertEqual(source.read_bytes(), original)

    def test_crc_repair_requires_structural_validity(self):
        self.native_only()
        source = self.fixture()
        damaged = bytearray(source.read_bytes())
        index = damaged.index(b"fixture\0")
        damaged[index] = ord("F")
        (self.directory / "crc.nvdb").write_bytes(damaged)
        self.call("--validate-nano", "crc.nvdb", succeeds=False)
        repaired = self.report("--checksum-nano", "crc.nvdb", "repaired.nvdb")
        self.assertTrue(repaired["grids"][0]["checksumVerified"])
        self.assertTrue(any("Explicit checksum repair accepted" in d for d in repaired["diagnostics"]))
        self.assertEqual((self.directory / "crc.nvdb").read_bytes(), damaged)
        self.report("--validate-nano", "repaired.nvdb", "--require-full-checksum")
        self.report("--checksum-nano", "repaired.nvdb", "header.nvdb", "--checksum", "header")
        self.call("--validate-nano", "header.nvdb", "--require-full-checksum", succeeds=False)
        damaged[0] = 0
        (self.directory / "structure.nvdb").write_bytes(damaged)
        self.call("--checksum-nano", "structure.nvdb", "bad.nvdb", succeeds=False)
        self.assertFalse((self.directory / "bad.nvdb").exists())

    def test_limits_and_existing_outputs_are_transactional(self):
        self.native_only()
        source = self.fixture()
        original = source.read_bytes()
        for flag in ("--max-native-bytes", "--max-workspace-bytes", "--max-nano-work",
                     "--max-stored-voxels", "--max-nodes", "--max-input-bytes"):
            with self.subTest(flag=flag):
                self.call("--copy-nano", source, "bad.nvdb", flag, 1, succeeds=False)
                self.assertFalse((self.directory / "bad.nvdb").exists())
        self.call("--copy-nano", source, source, succeeds=False)
        self.assertEqual(source.read_bytes(), original)

    def test_native_statistics_scope_and_readback(self):
        self.native_only()
        source = self.fixture()
        original = source.read_bytes()
        for mode in ("bounds", "minmax", "all"):
            report = self.report("--stats-nano", source, f"stats-{mode}.nvdb",
                                 "--native-statistics", mode)
            summary = report["summaries"][0]
            self.assertGreater(summary["activeVoxels"], 0)
            self.assertIsNotNone(summary["indexMinimum"])
            self.assertEqual(summary["minimum"] is None, mode == "bounds")
            self.assertEqual(summary["average"] is not None, mode == "all")
            self.report("--validate-nano", f"stats-{mode}.nvdb", "--require-full-checksum")
        self.assertEqual(source.read_bytes(), original)

    def test_vdb_roundtrip_preserves_double_and_inactive_signed_zero(self):
        if not (self.native and self.openvdb):
            self.skipTest("VDB conversion is disabled")
        values = array("d", [1.0000000000000002, -0.0, 2.0])
        volume = DenseVolume(DenseType.FLOAT64, (3, 1, 1), values,
                             bytearray([1, 0, 1]), grid_name="exact")
        write_dense_array(self.directory / "source.cvarr", volume)
        self.call("--dense-import", "source.cvarr", "source.vdb")
        self.report("--to-nano", "source.vdb", "native.nvdb")
        self.report("--from-nano", "native.nvdb", "restored.vdb")
        self.call("--dense-export", "restored.vdb", "restored.cvarr",
                  "--dense-origin", 0, 0, 0, "--dense-dimensions", 3, 1, 1)
        result = read_dense_array(self.directory / "restored.cvarr")
        self.assertEqual(result.values.tobytes(), values.tobytes())
        self.assertEqual(result.active, volume.active)


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--exe", type=Path, required=True)
    parser.add_argument("--generator", type=Path, required=True)
    parser.add_argument("--native-enabled", type=int, choices=(0, 1), required=True)
    parser.add_argument("--enabled", type=int, choices=(0, 1), required=True)
    args, remaining = parser.parse_known_args()
    NanoCli.tool = args.exe.resolve()
    NanoCli.generator = args.generator.resolve()
    NanoCli.native = bool(args.native_enabled)
    NanoCli.openvdb = bool(args.enabled)
    unittest.main(argv=[sys.argv[0], *remaining])
