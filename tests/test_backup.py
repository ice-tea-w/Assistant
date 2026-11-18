"""备份测试：增量去重、加密落盘、版本回滚、误删恢复、完整性校验、清理。"""

from __future__ import annotations

import json
import tempfile
import unittest
from pathlib import Path

from helpers import TEST_ITERATIONS, TEST_PASSWORD

from file_guard.backup import BackupStore
from file_guard.errors import DecryptionError, FileGuardError

ITER = {"iterations": TEST_ITERATIONS, "chunk_size": 1024, "log_iterations": TEST_ITERATIONS}


class BackupTest(unittest.TestCase):
    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory()
        self.root = Path(self._tmp.name)
        self.src = self.root / "docs" / "report.txt"
        self.src.parent.mkdir(parents=True, exist_ok=True)
        self.src.write_text("版本一：初稿", encoding="utf-8")
        self.store = BackupStore(self.root / "backups", TEST_PASSWORD, **ITER)

    def tearDown(self) -> None:
        self._tmp.cleanup()

    def test_backup_list_and_dedupe(self) -> None:
        first = self.store.backup_file(self.src)
        self.assertEqual(first["version"], 1)
        self.assertFalse(first["skipped"])
        second = self.store.backup_file(self.src)
        self.assertTrue(second["skipped"])  # 内容未变 → 增量跳过
        self.assertEqual(len(self.store.list(self.src)), 1)
        self.assertEqual(self.store.stats()["objects"], 1)

    def test_versions_and_rollback(self) -> None:
        self.store.backup_file(self.src)
        self.src.write_text("版本二：定稿", encoding="utf-8")
        second = self.store.backup_file(self.src)
        self.assertEqual(second["version"], 2)
        versions = [row["version"] for row in self.store.list(self.src)]
        self.assertEqual(versions, [1, 2])

        target1 = self.root / "restore-v1.txt"
        self.store.restore(self.src, version=1, dest=target1)
        self.assertEqual(target1.read_text(encoding="utf-8"), "版本一：初稿")

        target2 = self.root / "restore-v2.txt"
        self.store.restore(self.src, dest=target2)
        self.assertEqual(target2.read_text(encoding="utf-8"), "版本二：定稿")

    def test_backup_is_encrypted_at_rest(self) -> None:
        marker = "绝密内容-MARKER-123456"
        self.src.write_text(marker, encoding="utf-8")
        self.store.backup_file(self.src)
        objects = list((self.root / "backups" / "objects").rglob("*.fgc"))
        self.assertEqual(len(objects), 1)
        self.assertNotIn(marker.encode(), objects[0].read_bytes())
        index = (self.root / "backups" / "index.jsonl").read_bytes()
        self.assertNotIn(b"report.txt", index)  # 路径也不落明文

    def test_restore_deleted_file(self) -> None:
        self.store.backup_file(self.src)
        self.src.unlink()
        restored = self.store.restore(self.src)
        self.assertTrue(Path(restored["restored_to"]).exists())
        self.assertEqual(Path(restored["restored_to"]).read_text(encoding="utf-8"), "版本一：初稿")

    def test_restore_requires_force_if_exists(self) -> None:
        self.store.backup_file(self.src)
        with self.assertRaises(FileGuardError):
            self.store.restore(self.src)  # 原文件还在
        self.store.restore(self.src, overwrite=True)

    def test_missing_version(self) -> None:
        self.store.backup_file(self.src)
        with self.assertRaises(FileGuardError):
            self.store.restore(self.src, version=99)

    def test_verify_index_and_objects(self) -> None:
        self.store.backup_file(self.src)
        result = self.store.verify(full=True)
        self.assertTrue(result["ok"], result["problems"])
        self.assertEqual(result["objects_checked"], 1)

    def test_verify_detects_index_tamper(self) -> None:
        self.store.backup_file(self.src)
        index = self.root / "backups" / "index.jsonl"
        lines = index.read_text(encoding="utf-8").splitlines()
        row = json.loads(lines[0])
        row["h"] = "00" * 32
        lines[0] = json.dumps(row)
        index.write_text("\n".join(lines) + "\n", encoding="utf-8")
        result = self.store.verify()
        self.assertFalse(result["ok"])

    def test_wrong_password(self) -> None:
        self.store.backup_file(self.src)
        wrong = BackupStore(self.root / "backups", "wrong-password", **ITER)
        result = wrong.verify()
        self.assertFalse(result["ok"])
        with self.assertRaises((FileGuardError, DecryptionError)):
            wrong.list()

    def test_retention_and_gc(self) -> None:
        store = BackupStore(self.root / "retained", TEST_PASSWORD, keep_versions=1, **ITER)
        store.backup_file(self.src)
        self.src.write_text("版本二", encoding="utf-8")
        store.backup_file(self.src)
        versions = [row["version"] for row in store.list(self.src)]
        self.assertEqual(versions, [2])  # 只保留最新一版
        before = store.stats()["objects"]
        self.assertEqual(before, 2)  # 旧对象还没回收
        gc = store.gc()
        self.assertEqual(gc["removed_objects"], 1)
        self.assertEqual(store.stats()["objects"], 1)
        store.restore(self.src, version=2, dest=self.root / "v2.txt", overwrite=True)

    def test_directory_backup(self) -> None:
        (self.src.parent / "b.txt").write_text("B", encoding="utf-8")
        (self.src.parent / "sub").mkdir()
        (self.src.parent / "sub" / "c.txt").write_text("C", encoding="utf-8")
        results = self.store.backup_paths([self.src.parent])
        backed = [r for r in results if not r.get("skipped")]
        self.assertEqual(len(backed), 3)
        self.assertEqual(self.store.stats()["files"], 3)

    def test_export_index(self) -> None:
        self.store.backup_file(self.src)
        out = self.store.export_index(self.root / "index-export.json")
        rows = json.loads(out.read_text(encoding="utf-8"))
        self.assertEqual(rows[0]["event"], "backup")


if __name__ == "__main__":
    unittest.main(verbosity=2)
