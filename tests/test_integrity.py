"""模块自校验测试（对应风险预案：安全模块被篡改）。"""

from __future__ import annotations

import tempfile
import unittest
from pathlib import Path

from helpers import TEST_ITERATIONS, TEST_PASSWORD

from file_guard.integrity import build_manifest, load_manifest, save_manifest, verify_manifest


class IntegrityTest(unittest.TestCase):
    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory()
        self.root = Path(self._tmp.name) / "pkg"
        (self.root / "sub").mkdir(parents=True)
        (self.root / "__init__.py").write_text("VERSION = 1\n", encoding="utf-8")
        (self.root / "sub" / "mod.py").write_text("def f():\n    return 1\n", encoding="utf-8")

    def tearDown(self) -> None:
        self._tmp.cleanup()

    def test_manifest_roundtrip(self) -> None:
        manifest = build_manifest(self.root, TEST_PASSWORD)
        self.assertEqual(len(manifest["files"]), 2)
        self.assertTrue(manifest["mac"])
        result = verify_manifest(self.root, manifest, TEST_PASSWORD)
        self.assertTrue(result["ok"], result["problems"])
        self.assertEqual(result["checked"], 2)

    def test_detects_modified_file(self) -> None:
        manifest = build_manifest(self.root, TEST_PASSWORD)
        (self.root / "sub" / "mod.py").write_text("def f():\n    return 999  # 被篡改\n", encoding="utf-8")
        result = verify_manifest(self.root, manifest, TEST_PASSWORD)
        self.assertFalse(result["ok"])
        self.assertIn("sub/mod.py", result["modified"])

    def test_detects_missing_and_extra(self) -> None:
        manifest = build_manifest(self.root, TEST_PASSWORD)
        (self.root / "sub" / "mod.py").unlink()
        (self.root / "evil.py").write_text("import os\n", encoding="utf-8")
        result = verify_manifest(self.root, manifest, TEST_PASSWORD)
        self.assertFalse(result["ok"])
        self.assertIn("sub/mod.py", result["missing"])
        self.assertIn("evil.py", result["extra"])

    def test_tampered_manifest_signature(self) -> None:
        manifest = build_manifest(self.root, TEST_PASSWORD)
        manifest["files"]["__init__.py"] = "0" * 64
        result = verify_manifest(self.root, manifest, TEST_PASSWORD)
        self.assertFalse(result["ok"])
        self.assertTrue(any("签名" in p for p in result["problems"]))

    def test_wrong_password_signature(self) -> None:
        manifest = build_manifest(self.root, TEST_PASSWORD)
        result = verify_manifest(self.root, manifest, "wrong-password")
        self.assertFalse(result["ok"])

    def test_save_and_load(self) -> None:
        manifest = build_manifest(self.root, TEST_PASSWORD)
        path = save_manifest(manifest, Path(self._tmp.name) / "integrity.json")
        loaded = load_manifest(path)
        self.assertTrue(verify_manifest(self.root, loaded, TEST_PASSWORD)["ok"])


if __name__ == "__main__":
    unittest.main(verbosity=2)
