"""本地 HTTP 服务测试：鉴权、RPC 分发、加密/扫描/审计/备份闭环。"""

from __future__ import annotations

import json
import tempfile
import threading
import unittest
import urllib.error
import urllib.request
from pathlib import Path

from helpers import TEST_ITERATIONS, TEST_PASSWORD, make_id_card

from file_guard.config import Config
from file_guard.service import Session, create_server


class ServiceTest(unittest.TestCase):
    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory()
        self.root = Path(self._tmp.name)
        cfg = Config(home=self.root / "home", iterations=TEST_ITERATIONS, log_iterations=TEST_ITERATIONS)
        cfg.ensure_dirs()
        cfg.save()
        self.session = Session(cfg, TEST_PASSWORD)
        self.token = "test-token-0123456789"
        self.server = create_server(self.session, self.token, port=0)
        self.port = self.server.server_port
        self.thread = threading.Thread(target=self.server.serve_forever, daemon=True)
        self.thread.start()
        self.url = f"http://127.0.0.1:{self.port}/rpc"

    def tearDown(self) -> None:
        self.server.shutdown()
        self.server.server_close()
        self.thread.join(timeout=5)
        self.session.close()
        self._tmp.cleanup()

    def _rpc(self, method: str, token: str | None = None, **params):
        body = json.dumps({"method": method, "params": params}).encode("utf-8")
        request = urllib.request.Request(
            self.url,
            data=body,
            headers={"Content-Type": "application/json", "X-FG-Token": token if token is not None else self.token},
        )
        with urllib.request.urlopen(request, timeout=10) as response:
            return json.loads(response.read().decode("utf-8"))

    def test_health_without_token(self) -> None:
        with urllib.request.urlopen(f"http://127.0.0.1:{self.port}/health", timeout=5) as response:
            self.assertEqual(json.loads(response.read())["result"]["status"], "alive")

    def test_rejects_bad_token(self) -> None:
        with self.assertRaises(urllib.error.HTTPError) as ctx:
            self._rpc("ping", token="wrong")
        self.assertEqual(ctx.exception.code, 401)

    def test_ping_and_status(self) -> None:
        ping = self._rpc("ping")
        self.assertTrue(ping["ok"])
        self.assertEqual(ping["result"]["name"], "file_guard")
        status = self._rpc("status")
        self.assertEqual(status["result"]["iterations"], TEST_ITERATIONS)

    def test_encrypt_decrypt_cycle(self) -> None:
        src = self.root / "a.txt"
        src.write_text("机密内容", encoding="utf-8")
        encrypted = self._rpc("encrypt", paths=[str(src)])
        self.assertTrue(encrypted["ok"], encrypted)
        container = encrypted["result"][0]["dst"]
        self.assertTrue(Path(container).exists())

        src.unlink()
        decrypted = self._rpc("decrypt", paths=[container], force=True)
        self.assertTrue(decrypted["ok"], decrypted)
        self.assertEqual(Path(decrypted["result"][0]["dst"]).read_text(encoding="utf-8"), "机密内容")

    def test_scan_report(self) -> None:
        (self.root / "id.txt").write_text(f"身份证 {make_id_card()}", encoding="utf-8")
        response = self._rpc("scan", paths=[str(self.root / "id.txt")])
        self.assertTrue(response["ok"], response)
        self.assertEqual(response["result"]["summary"]["high"], 1)

    def test_audit_and_backup(self) -> None:
        src = self.root / "b.txt"
        src.write_text("v1", encoding="utf-8")
        self.assertTrue(self._rpc("backup", paths=[str(src)])["ok"])
        src.write_text("v2", encoding="utf-8")
        self.assertTrue(self._rpc("backup", paths=[str(src)])["ok"])
        listing = self._rpc("backup.list", path=str(src))
        self.assertEqual(len(listing["result"]), 2)
        restored = self._rpc("restore", path=str(src), version=1, force=True)
        self.assertEqual(Path(restored["result"]["restored_to"]).read_text(encoding="utf-8"), "v1")
        records = self._rpc("audit.list", limit=100)
        self.assertGreaterEqual(len(records["result"]), 2)
        self.assertTrue(self._rpc("audit.verify")["result"]["ok"])

    def test_unknown_method(self) -> None:
        with self.assertRaises(urllib.error.HTTPError) as ctx:
            self._rpc("rm -rf")
        self.assertEqual(ctx.exception.code, 400)
        payload = json.loads(ctx.exception.read().decode("utf-8"))
        self.assertFalse(payload["ok"])


if __name__ == "__main__":
    unittest.main(verbosity=2)
