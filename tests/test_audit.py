"""审计日志测试：加密落盘、HMAC 哈希链、防篡改、归档清空、并发追加。"""

from __future__ import annotations

import json
import tempfile
import threading
import unittest
from pathlib import Path

from helpers import TEST_ITERATIONS, TEST_PASSWORD

from file_guard.audit import EVENT_ENCRYPT, EVENT_OPEN, AuditLog
from file_guard.errors import FileGuardError

ITER = {"iterations": TEST_ITERATIONS}


class AuditLogTest(unittest.TestCase):
    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory()
        self.root = Path(self._tmp.name)
        self.audit = AuditLog(self.root, TEST_PASSWORD, **ITER)

    def tearDown(self) -> None:
        self._tmp.cleanup()

    def _fill(self, count: int = 5) -> None:
        for i in range(count):
            self.audit.record(EVENT_OPEN, path=f"C:/docs/file{i}.txt", detail=f"第 {i} 次打开")

    def test_append_and_read(self) -> None:
        self._fill(5)
        rows = self.audit.tail(10)
        self.assertEqual(len(rows), 5)
        self.assertEqual(rows[0]["event"], "open")
        self.assertEqual(rows[-1]["path"], "C:/docs/file4.txt")
        self.assertIn("host", rows[0])

    def test_log_is_encrypted_at_rest(self) -> None:
        self._fill(3)
        log_path = self.root / "audit.jsonl"
        raw = log_path.read_bytes()
        self.assertNotIn("file0.txt".encode(), raw)
        self.assertNotIn("第 0 次打开".encode(), raw)
        self.assertNotIn(TEST_PASSWORD.encode(), raw)

    def test_verify_ok(self) -> None:
        self._fill(4)
        result = self.audit.verify()
        self.assertTrue(result["ok"], result["problems"])
        self.assertEqual(result["records"], 4)
        self.assertEqual(len(result["head"]), 64)

    def test_verify_detects_modification(self) -> None:
        self._fill(4)
        lines = (self.root / "audit.jsonl").read_text(encoding="utf-8").splitlines()
        row = json.loads(lines[2])
        raw = bytearray(__import__("base64").b64decode(row["d"]))
        raw[3] ^= 0xFF
        row["d"] = __import__("base64").b64encode(bytes(raw)).decode()
        lines[2] = json.dumps(row)
        (self.root / "audit.jsonl").write_text("\n".join(lines) + "\n", encoding="utf-8")
        result = self.audit.verify()
        self.assertFalse(result["ok"])
        self.assertTrue(any("无法解密" in p or "哈希" in p for p in result["problems"]))

    def test_verify_detects_deletion(self) -> None:
        self._fill(5)
        lines = (self.root / "audit.jsonl").read_text(encoding="utf-8").splitlines()
        del lines[1]
        (self.root / "audit.jsonl").write_text("\n".join(lines) + "\n", encoding="utf-8")
        result = self.audit.verify()
        self.assertFalse(result["ok"])
        self.assertTrue(any("序号不连续" in p or "哈希" in p for p in result["problems"]))

    def test_verify_detects_reorder(self) -> None:
        self._fill(4)
        lines = (self.root / "audit.jsonl").read_text(encoding="utf-8").splitlines()
        lines[0], lines[1] = lines[1], lines[0]
        (self.root / "audit.jsonl").write_text("\n".join(lines) + "\n", encoding="utf-8")
        result = self.audit.verify()
        self.assertFalse(result["ok"])

    def test_wrong_password(self) -> None:
        self._fill(2)
        wrong = AuditLog(self.root, "wrong-password", **ITER)
        result = wrong.verify()
        self.assertFalse(result["ok"])
        with self.assertRaises(FileGuardError):
            wrong.tail(1)

    def test_external_anchor(self) -> None:
        self._fill(3)
        head = self.audit.head()
        self.assertTrue(self.audit.verify(expected_head=head)["ok"])
        self.assertFalse(self.audit.verify(expected_head="00" * 32)["ok"])

    def test_query_filters(self) -> None:
        self.audit.record(EVENT_OPEN, path="a.txt")
        self.audit.record(EVENT_ENCRYPT, path="b.txt", detail="生成 b.txt.fgc")
        self.assertEqual(len(self.audit.query(event=EVENT_ENCRYPT)), 1)
        self.assertEqual(len(self.audit.query(contains="b.txt")), 1)
        self.assertEqual(len(self.audit.query(limit=10**6)), 2)

    def test_clear_archives_and_restarts(self) -> None:
        self._fill(3)
        old_head = self.audit.head()
        result = self.audit.clear(archive=True)
        self.assertTrue(result["cleared"])
        self.assertTrue(result["archived"])
        for path in result["archived"]:
            self.assertTrue(Path(path).exists())
        self.assertEqual(self.audit.tail(1), [])
        self.audit.record(EVENT_OPEN, path="after-clear.txt")
        after = self.audit.verify()
        self.assertTrue(after["ok"], after["problems"])
        self.assertEqual(after["records"], 1)
        self.assertNotEqual(after["head"], old_head)  # 新盐/新链头，nonce 不会复用
        self.audit.clear(archive=False)

    def test_export(self) -> None:
        self._fill(2)
        out = self.audit.export(self.root / "export.json")
        rows = json.loads(out.read_text(encoding="utf-8"))
        self.assertEqual(len(rows), 2)

    def test_concurrent_appends(self) -> None:
        errors: list[Exception] = []

        def worker(n: int) -> None:
            try:
                for i in range(5):
                    self.audit.record(EVENT_OPEN, path=f"C:/t/{n}-{i}.txt")
            except Exception as exc:  # noqa: BLE001
                errors.append(exc)

        threads = [threading.Thread(target=worker, args=(n,)) for n in range(4)]
        for t in threads:
            t.start()
        for t in threads:
            t.join()
        self.assertEqual(errors, [])
        result = self.audit.verify()
        self.assertTrue(result["ok"], result["problems"])
        self.assertEqual(result["records"], 20)

    def test_unknown_event_rejected(self) -> None:
        with self.assertRaises(FileGuardError):
            self.audit.record("hack")


if __name__ == "__main__":
    unittest.main(verbosity=2)
