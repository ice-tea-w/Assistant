"""新增能力回归测试：统一社会信用代码检测、监听自动备份、服务异步任务、doctor 自检。

这些是 v0.3 的功能增量，和 tests/test_security.py（安全评审回归）互补。
"""

from __future__ import annotations

import http.client
import json
import os
import subprocess
import sys
import tempfile
import threading
import time
import unittest
from pathlib import Path

from helpers import PROJECT_ROOT, TEST_ITERATIONS, TEST_PASSWORD, make_id_card

from file_guard.audit import AuditLog
from file_guard.backup import BackupStore
from file_guard.config import Config
from file_guard.scan import (
    SensitiveScanner,
    is_valid_uscc,
    scan_paths,
    uscc_check_char,
)
from file_guard.service import Session, create_server
from file_guard.watch import DirectoryWatcher, auto_backup_changes

ITER = {"iterations": TEST_ITERATIONS}
BACKUP_ITER = {
    "iterations": TEST_ITERATIONS,
    "chunk_size": 1024,
    "log_iterations": TEST_ITERATIONS,
}


class UsccDetectionTest(unittest.TestCase):
    def test_valid_and_invalid_checksum(self) -> None:
        body = "91350211M000100Y4"  # 17 位，校验位算出来
        valid = body + uscc_check_char(body)
        self.assertTrue(is_valid_uscc(valid))
        # 改一位必须失败（否则正则型误报挡不住）
        broken = body[:-1] + ("0" if body[-1] != "0" else "1") + valid[17]
        self.assertFalse(is_valid_uscc(broken))
        self.assertFalse(is_valid_uscc("91350211M000100Y4!"))
        self.assertFalse(is_valid_uscc("12345"))

    def test_scan_hits_uscc_but_not_random_license(self) -> None:
        body = "91350211M000100Y4"
        valid = body + uscc_check_char(body)
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / "company.txt").write_text(
                f"客户统一社会信用代码：{valid}\n合同编号：91350211M000100Y4Z\n",
                encoding="utf-8",
            )
            risk = SensitiveScanner().scan_file(root / "company.txt")
            kinds = {finding.kind for finding in risk.findings}
            self.assertIn("uscc", kinds)
            self.assertEqual(risk.risk, "medium")
            # 报告里的样本应打码，不能出现完整代码
            payload = json.dumps(scan_paths([root]).to_dict(), ensure_ascii=False)
            self.assertNotIn(valid, payload)

    def test_clean_text_has_no_uscc_hit(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "clean.txt"
            path.write_text("会议纪要：讨论下阶段排期，没有敏感信息。", encoding="utf-8")
            report = scan_paths([Path(tmp)])
            self.assertEqual(report.summary["findings"], 0)


class WatchAutoBackupTest(unittest.TestCase):
    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory()
        self.root = Path(self._tmp.name)
        self.data = self.root / "data"
        self.data.mkdir()
        self.store = BackupStore(self.root / "backups", TEST_PASSWORD, **BACKUP_ITER)

    def tearDown(self) -> None:
        self._tmp.cleanup()

    def test_created_and_modified_files_are_backed_up(self) -> None:
        watcher = DirectoryWatcher([self.data], interval=0.5)
        watcher.poll_once()  # 建立基线
        first = self.data / "a.txt"
        first.write_text("v1", encoding="utf-8")
        second = self.data / "b.txt"
        second.write_text("b", encoding="utf-8")
        changes = watcher.poll_once()
        results = auto_backup_changes(watcher, self.store, changes=changes)
        self.assertEqual(len(results), 2)
        self.assertEqual([item["version"] for item in results], [1, 1])
        self.assertEqual(len(self.store.list(first)), 1)

        # 改内容 → 产生 v2
        first.write_text("v2", encoding="utf-8")
        results = auto_backup_changes(watcher, self.store, changes=watcher.poll_once())
        self.assertEqual(results[0]["version"], 2)
        self.assertFalse(results[0]["skipped"])
        # 再写一次同样内容：监听层仍会看到 mtime 变化，但备份应"增量跳过"不产生新版本
        time.sleep(0.01)
        first.write_text("v2", encoding="utf-8")
        results = auto_backup_changes(watcher, self.store, changes=watcher.poll_once())
        self.assertEqual(len(results), 1)
        self.assertTrue(results[0]["skipped"])
        self.assertEqual(len(self.store.list(first)), 2)

    def test_deleted_file_is_not_backed_up(self) -> None:
        keep = self.data / "keep.txt"
        keep.write_text("keep", encoding="utf-8")
        watcher = DirectoryWatcher([self.data], interval=0.5)
        watcher.poll_once()
        keep.unlink()
        changes = watcher.poll_once()
        results = auto_backup_changes(watcher, self.store, changes=changes)
        self.assertEqual(results, [])

    def test_audit_records_new_versions(self) -> None:
        audit = AuditLog(self.root / "audit", TEST_PASSWORD, **ITER)
        watcher = DirectoryWatcher([self.data], interval=0.5)
        watcher.poll_once()
        target = self.data / "c.txt"
        target.write_text("c", encoding="utf-8")
        auto_backup_changes(watcher, self.store, changes=watcher.poll_once(), audit=audit)
        rows = audit.query(event="backup")
        self.assertEqual(len(rows), 1)
        self.assertIn("监听自动备份", rows[0]["detail"])


class ServiceJobTest(unittest.TestCase):
    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory()
        self.root = Path(self._tmp.name)
        self.cfg = Config(home=self.root / "home", iterations=TEST_ITERATIONS,
                          log_iterations=TEST_ITERATIONS)
        self.cfg.ensure_dirs()
        self.cfg.save()
        self.session = Session(self.cfg, TEST_PASSWORD)
        self.token = "test-token-0123456789"
        self.server = create_server(self.session, self.token, port=0)
        self.port = self.server.server_port
        self.thread = threading.Thread(target=self.server.serve_forever, daemon=True)
        self.thread.start()

    def tearDown(self) -> None:
        self.server.shutdown()
        self.server.server_close()
        self.thread.join(timeout=5)
        self.session.close()
        self._tmp.cleanup()

    def _rpc(self, rpc_method: str, **params) -> dict:
        body = json.dumps({"method": rpc_method, "params": params}).encode("utf-8")
        connection = http.client.HTTPConnection("127.0.0.1", self.port, timeout=20)
        connection.request("POST", "/rpc", body=body,
                           headers={"X-FG-Token": self.token, "Host": f"127.0.0.1:{self.port}"})
        response = connection.getresponse()
        payload = json.loads(response.read().decode("utf-8"))
        connection.close()
        return payload

    def test_submit_and_poll_scan_job(self) -> None:
        target = self.root / "id.txt"
        target.write_text(f"身份证 {make_id_card()}", encoding="utf-8")
        submitted = self._rpc("job.submit", method="scan",
                              params={"paths": [str(target)], "min_risk": "low"})
        self.assertTrue(submitted["ok"], submitted)
        job_id = submitted["result"]["job_id"]
        self.assertIn(submitted["result"]["state"], ("queued", "running", "done"))

        deadline = time.monotonic() + 30
        status: dict = {}
        while time.monotonic() < deadline:
            payload = self._rpc("job.status", job_id=job_id)
            status = payload["result"]
            if status["state"] in ("done", "error", "cancelled"):
                break
            time.sleep(0.05)
        self.assertEqual(status["state"], "done", status)
        self.assertEqual(status["result"]["summary"]["high"], 1)
        self.assertIn(job_id, json.dumps(self._rpc("job.list")["result"]))

    def test_unknown_job_and_unsupported_method(self) -> None:
        bad_method = self._rpc("job.submit", method="ping", params={})
        self.assertFalse(bad_method["ok"])
        bad_id = self._rpc("job.status", job_id="deadbeef")
        self.assertFalse(bad_id["ok"])

    def test_cancel_finished_job_is_rejected(self) -> None:
        target = self.root / "one.txt"
        target.write_text("hello", encoding="utf-8")
        job_id = self._rpc("job.submit", method="scan",
                           params={"paths": [str(target)]})["result"]["job_id"]
        deadline = time.monotonic() + 30
        while time.monotonic() < deadline:
            state = self._rpc("job.status", job_id=job_id)["result"]["state"]
            if state in ("done", "error", "cancelled"):
                break
            time.sleep(0.05)
        cancelled = self._rpc("job.cancel", job_id=job_id)
        self.assertFalse(cancelled["ok"])


class DoctorCliTest(unittest.TestCase):
    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory()
        self.home = Path(self._tmp.name) / "home"
        self.env = os.environ.copy()
        self.env["FG_PASSWORD"] = "doctor-test-password"
        self.env["PYTHONIOENCODING"] = "utf-8"
        self.env.pop("FG_HOME", None)

    def tearDown(self) -> None:
        self._tmp.cleanup()

    def _run(self, *args: str) -> subprocess.CompletedProcess:
        return subprocess.run(
            [sys.executable, "-m", "file_guard", *args],
            cwd=str(PROJECT_ROOT), env=self.env, capture_output=True, text=True,
            encoding="utf-8", errors="replace", timeout=180,
        )

    def test_doctor_ok_after_init(self) -> None:
        init = self._run("init", "--home", str(self.home), "--no-vault", "--iterations", "1000")
        self.assertEqual(init.returncode, 0, init.stderr)
        proc = self._run("doctor", "--home", str(self.home), "--json")
        payload = json.loads(proc.stdout)
        self.assertEqual(proc.returncode, 0, payload)
        self.assertTrue(payload["ok"], payload)
        names = {item["name"] for item in payload["checks"]}
        self.assertTrue({"数据目录可读写", "审计日志哈希链"} <= names, names)

    def test_doctor_reports_broken_audit_chain(self) -> None:
        self._run("init", "--home", str(self.home), "--no-vault", "--iterations", "1000")
        log_path = self.home / "audit" / "audit.jsonl"
        lines = log_path.read_text(encoding="utf-8").splitlines()
        rows = [json.loads(line) for line in lines]
        rows[0]["h"] = "00" * 32
        log_path.write_text("\n".join(json.dumps(row) for row in rows) + "\n", encoding="utf-8")
        proc = self._run("doctor", "--home", str(self.home), "--json")
        payload = json.loads(proc.stdout)
        self.assertEqual(proc.returncode, 5, payload)
        self.assertFalse(payload["ok"])
        broken = [item for item in payload["checks"] if not item["ok"]]
        self.assertTrue(any("审计" in item["name"] for item in broken), broken)


if __name__ == "__main__":
    unittest.main(verbosity=2)
