"""Verify archive pointer recovery without contacting GitHub."""
import hashlib
import importlib.util
import io
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

spec = importlib.util.spec_from_file_location(
    "model_lfs", Path(__file__).resolve().parents[2] / "cmake/materialize_lfs_models.py")
lfs = importlib.util.module_from_spec(spec)
spec.loader.exec_module(lfs)


class ModelLfsTests(unittest.TestCase):
    def fixture(self, root, data=b"ISO-10303-21;\nEND-ISO-10303-21;\n", newline="\n"):
        path = root / "nested models" / "wall.ifc"
        path.parent.mkdir()
        pointer = ("version https://git-lfs.github.com/spec/v1" + newline
                   + "oid sha256:" + hashlib.sha256(data).hexdigest() + newline
                   + f"size {len(data)}" + newline).encode()
        path.write_bytes(pointer)
        return path, pointer, data

    def test_verified_payload_replaces_pointer_and_escapes_path(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            path, pointer, data = self.fixture(root, newline="\r\n")
            with patch.object(lfs.urllib.request, "urlopen", return_value=io.BytesIO(data)) as fetch:
                self.assertTrue(lfs.materialize(path, root, "owner/repo", "a" * 40))
                self.assertIn("nested%20models/wall.ifc", fetch.call_args.args[0].full_url)
            self.assertEqual(path.read_bytes(), data)
            with patch.object(lfs.urllib.request, "urlopen") as fetch:
                self.assertFalse(lfs.materialize(path, root, "owner/repo", "a" * 40))
                fetch.assert_not_called()

    def test_bad_size_hash_and_network_failure_preserve_pointer(self):
        for payload in [b"too short", b"X" * 30, b"X" * 100]:
            with self.subTest(payload=payload), tempfile.TemporaryDirectory() as directory:
                root = Path(directory)
                path, pointer, data = self.fixture(root)
                with patch.object(lfs.urllib.request, "urlopen", return_value=io.BytesIO(payload)):
                    with self.assertRaises(ValueError):
                        lfs.materialize(path, root, "owner/repo", "a" * 40)
                self.assertEqual(path.read_bytes(), pointer)
                self.assertFalse(list(root.rglob("*.lfs-download")))
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            path, pointer, data = self.fixture(root)
            with patch.object(lfs.urllib.request, "urlopen", side_effect=OSError("offline")):
                with self.assertRaises(OSError):
                    lfs.materialize(path, root, "owner/repo", "a" * 40)
            self.assertEqual(path.read_bytes(), pointer)


if __name__ == "__main__":
    unittest.main()
