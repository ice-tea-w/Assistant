"""安全评审回归测试：每条加固/修复都对应一个可复现的用例。

覆盖：
* 审计日志：逐条随机 nonce、旧格式兼容、回滚/篡改/写坏密码检测、崩溃残行自愈、
  归档不覆盖；
* 加密容器：源文件变化检测、临时文件清理、rekey 拒绝尾部垃圾、密钥文件原子创建；
* 备份：版本号不复用、"备份期源文件被改"真的能发现、对象名不泄露明文哈希、
  恶意索引不能把恢复写到 objects/ 之外；
* 扫描/监听：不跟随目录联结与符号链接（不越出授权目录）、Office zip 炸弹；
* 本地服务：非回环拒绝、Host/Origin 校验、畸形参数 400；
* 配置/完整性清单：非法配置不崩溃、畸形清单不触发超大 KDF。
"""

from __future__ import annotations

import base64
import http.client
import json
import os
import subprocess
import tempfile
import threading
import time
import unittest
import zipfile
from pathlib import Path
from unittest import mock

from helpers import TEST_CHUNK_SIZE, TEST_ITERATIONS, TEST_PASSWORD, make_id_card

from file_guard.audit import AuditLog, EncryptedLog
from file_guard.backup import BackupStore
from file_guard.config import Config
from file_guard.crypto import (
    CorruptContainerError,
    encrypt_file,
    generate_keyfile,
    iter_files,
    rekey_file,
)
from file_guard.errors import ConfigError, FileGuardError, IntegrityError, ServiceError
from file_guard.integrity import build_manifest, verify_manifest
from file_guard.scan import SensitiveScanner, scan_paths
from file_guard.service import Session, create_server
from file_guard.watch import DirectoryWatcher, auto_encrypt_idle

LOG_ITER = {"iterations": TEST_ITERATIONS}
CRYPTO_ITER = {"iterations": TEST_ITERATIONS, "chunk_size": TEST_CHUNK_SIZE}
BACKUP_ITER = {
    "iterations": TEST_ITERATIONS,
    "chunk_size": TEST_CHUNK_SIZE,
    "log_iterations": TEST_ITERATIONS,
}


def _make_junction(link: Path, target: Path) -> bool:
    """Windows 上创建目录联结（无需管理员权限）；失败返回 False。"""
    if os.name != "nt":
        return False
    try:
        completed = subprocess.run(
            ["cmd", "/c", "mklink", "/J", str(link), str(target)],
            capture_output=True,
            timeout=30,
        )
        return completed.returncode == 0 and link.exists()
    except (OSError, subprocess.SubprocessError):
        return False


def _remove_junction(link: Path) -> None:
    try:
        link.rmdir()  # 目录联结：只删链接本身，不碰目标目录
    except OSError:
        try:
            link.unlink()  # 文件符号链接
        except OSError:
            pass


class AuditHardeningTest(unittest.TestCase):
    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory()
        self.root = Path(self._tmp.name)
        self.audit = AuditLog(self.root, TEST_PASSWORD, **LOG_ITER)
        self.log_path = self.root / "audit.jsonl"

    def tearDown(self) -> None:
        self._tmp.cleanup()

    def _raw_lines(self) -> list[dict]:
        return [json.loads(line) for line in self.log_path.read_text(encoding="utf-8").splitlines() if line]

    def test_each_record_uses_fresh_random_nonce(self) -> None:
        """v2 每条记录独立随机 nonce：旧格式"前缀+seq"在回滚后会复用。"""
        for i in range(5):
            self.audit.record("open", path=f"f{i}.txt")
        lines = self._raw_lines()
        nonces = [base64.b64decode(line["n"]) for line in lines]
        self.assertEqual(len({n for n in nonces}), 5)
        for nonce in nonces:
            self.assertEqual(len(nonce), 12)

    def test_legacy_v1_log_still_readable_and_appends_v2(self) -> None:
        """旧版本（v1）日志必须还能读；新追加的记录用 v2 格式。"""
        log = EncryptedLog(self.root, TEST_PASSWORD, iterations=TEST_ITERATIONS)
        salt = os.urandom(16)
        prefix = os.urandom(8)
        meta = {
            "version": 1,
            "iterations": TEST_ITERATIONS,
            "salt": base64.b64encode(salt).decode(),
            "nonce_prefix": base64.b64encode(prefix).decode(),
            "created": "2024-01-01T00:00:00",
        }
        (self.root / "audit.meta.json").write_text(json.dumps(meta), encoding="utf-8")
        aes = log._get_aes()
        prev = log._genesis()
        lines = []
        for seq in range(2):
            payload = {"ts": "t", "event": "open", "path": f"old{seq}.txt"}
            data = json.dumps(payload, ensure_ascii=False, separators=(",", ":")).encode("utf-8")
            nonce = log._nonce(seq)
            ct = aes.encrypt(nonce, data, log._record_aad(seq, nonce, legacy=True))
            chain = log._chain_hash(prev, ct)  # 旧格式：链只覆盖 ct
            lines.append(json.dumps({"seq": seq, "h": chain, "d": base64.b64encode(ct).decode()}, separators=(",", ":")))
            prev = chain
        self.log_path.write_text("\n".join(lines) + "\n", encoding="utf-8")

        result = log.verify()
        self.assertTrue(result["ok"], result["problems"])
        self.assertEqual(result["records"], 2)

        record = log.append({"ts": "t", "event": "open", "path": "new.txt"})
        self.assertEqual(record.seq, 2)
        self.assertIn("n", self._raw_lines()[-1])
        after = log.verify()
        self.assertTrue(after["ok"], after["problems"])
        self.assertEqual(after["records"], 3)

    def test_rollback_is_detected_on_append_and_verify(self) -> None:
        for i in range(5):
            self.audit.record("open", path=f"f{i}.txt")
        lines = self.log_path.read_text(encoding="utf-8").splitlines()
        self.log_path.write_text("\n".join(lines[:2]) + "\n", encoding="utf-8")
        with self.assertRaises(FileGuardError):
            self.audit.record("open", path="after-rollback.txt")
        result = self.audit.verify()
        self.assertFalse(result["ok"])
        self.assertTrue(any("回滚" in p or "截断" in p for p in result["problems"]), result["problems"])

    def test_meta_tamper_is_detected(self) -> None:
        self.audit.record("open", path="a.txt")
        meta_path = self.root / "audit.meta.json"
        meta = json.loads(meta_path.read_text(encoding="utf-8"))
        meta["next_seq"] = 0  # 偷偷改高水位：签名立刻对不上
        meta_path.write_text(json.dumps(meta), encoding="utf-8")
        # 换一个实例，避免读到内存里缓存的旧 meta
        tampered = AuditLog(self.root, TEST_PASSWORD, **LOG_ITER)
        result = tampered.verify()
        self.assertFalse(result["ok"])
        self.assertTrue(any("meta" in p or "元数据" in p for p in result["problems"]), result["problems"])
        with self.assertRaises(FileGuardError):
            tampered.record("open", path="b.txt")

    def test_wrong_password_cannot_corrupt_log(self) -> None:
        """旧实现用错误密码也能追加：那条记录永远解不开，还会让后续校验全红。"""
        self.audit.record("open", path="a.txt")
        before = self.log_path.read_bytes()
        wrong = AuditLog(self.root, "totally-wrong-password", **LOG_ITER)
        with self.assertRaises(FileGuardError):
            wrong.record("open", path="by-attacker.txt")
        self.assertEqual(self.log_path.read_bytes(), before)
        self.assertTrue(self.audit.verify()["ok"])

    def test_torn_tail_is_repaired(self) -> None:
        """进程崩溃留下半截写入（meta 还没前进）：下一次追加要自愈，而不是永久报废。"""
        for i in range(2):
            self.audit.record("open", path=f"f{i}.txt")
        with open(self.log_path, "ab") as f:
            f.write(b'{"seq":2,"n":"AAAA')  # 写到一半崩溃：没有换行、也不是合法 JSON
        record = self.audit.record("open", path="after-crash.txt")
        self.assertEqual(record.seq, 2)
        result = self.audit.verify()
        self.assertTrue(result["ok"], result["problems"])
        self.assertEqual(result["records"], 3)

    def test_truncation_after_commit_is_not_silently_accepted(self) -> None:
        """已提交的记录被截断（不是崩溃残行）必须报出来，不能悄悄接着写。"""
        for i in range(3):
            self.audit.record("open", path=f"f{i}.txt")
        raw = self.log_path.read_bytes()
        self.log_path.write_bytes(raw[:-20])
        with self.assertRaises(FileGuardError):
            self.audit.record("open", path="after-truncation.txt")
        result = self.audit.verify()
        self.assertFalse(result["ok"])

    def test_survives_log_clear_by_another_process(self) -> None:
        """常驻进程（如服务）在别的进程 `log clear` 之后要自愈，而不是报"密码不对"。"""
        self.audit.record("open", path="a.txt")
        other = AuditLog(self.root, TEST_PASSWORD, **LOG_ITER)
        other.clear(archive=True)
        other.record("open", path="b.txt")
        record = self.audit.record("open", path="after-clear.txt")
        self.assertEqual(record.seq, 1)
        self.assertTrue(AuditLog(self.root, TEST_PASSWORD, **LOG_ITER).verify()["ok"])

    def test_clear_in_same_second_does_not_overwrite_archive(self) -> None:
        self.audit.record("open", path="a.txt")
        first = self.audit.clear(archive=True)
        self.audit.record("open", path="b.txt")
        second = self.audit.clear(archive=True)
        archived = first["archived"] + second["archived"]
        self.assertEqual(len(archived), 4)
        self.assertEqual(len(set(archived)), 4)  # 同一秒内两轮归档，文件名不能撞
        for path in archived:
            self.assertTrue(Path(path).exists())


class CryptoHardeningTest(unittest.TestCase):
    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory()
        self.root = Path(self._tmp.name)
        self.src = self.root / "doc.txt"
        self.src.write_text("0123456789" * 10, encoding="utf-8")

    def tearDown(self) -> None:
        self._tmp.cleanup()

    def test_source_growth_during_encrypt_is_detected(self) -> None:
        """源文件在加密过程中变大时，旧实现会静默丢弃多出来的内容。"""
        dst = self.root / "doc.txt.fgc"
        grown = {"done": False}

        def progress(done: int, total: int) -> None:
            if not grown["done"] and done >= total:
                grown["done"] = True
                with open(self.src, "ab") as f:
                    f.write(b"appended-later")

        with self.assertRaises(FileGuardError):
            encrypt_file(self.src, dst, TEST_PASSWORD, progress=progress, **CRYPTO_ITER)
        self.assertFalse(dst.exists())
        self.assertEqual([p for p in self.root.iterdir() if p.suffix == ".part"], [])

    def test_rekey_rejects_trailing_garbage(self) -> None:
        container = self.root / "doc.fgc"
        encrypt_file(self.src, container, TEST_PASSWORD, **CRYPTO_ITER)
        with open(container, "ab") as f:
            f.write(b"junk")
        target = self.root / "doc2.fgc"
        with self.assertRaises(CorruptContainerError):
            rekey_file(container, target, TEST_PASSWORD, "new-password-123", **CRYPTO_ITER)
        self.assertFalse(target.exists())

    def test_keyfile_generation_never_overwrites(self) -> None:
        path = self.root / "guard.key"
        generate_keyfile(path)
        original = path.read_bytes()
        results: list[Path] = []
        errors: list[Exception] = []

        def worker() -> None:
            try:
                results.append(generate_keyfile(path))
            except FileGuardError as exc:
                errors.append(exc)

        threads = [threading.Thread(target=worker) for _ in range(4)]
        for thread in threads:
            thread.start()
        for thread in threads:
            thread.join()
        self.assertEqual(results, [])  # 已存在 → 全部拒绝
        self.assertEqual(len(errors), 4)
        self.assertEqual(path.read_bytes(), original)  # 原密钥一个字节都不能变


class BackupHardeningTest(unittest.TestCase):
    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory()
        self.root = Path(self._tmp.name)
        self.src = self.root / "report.txt"
        self.src.write_text("版本一", encoding="utf-8")
        self.store = BackupStore(self.root / "backups", TEST_PASSWORD, **BACKUP_ITER)

    def tearDown(self) -> None:
        self._tmp.cleanup()

    def test_version_number_not_reused_after_purge(self) -> None:
        store = BackupStore(self.root / "retained", TEST_PASSWORD, keep_versions=1, **BACKUP_ITER)
        store.backup_file(self.src)
        self.src.write_text("版本二", encoding="utf-8")
        store.backup_file(self.src)
        self.src.write_text("版本三", encoding="utf-8")
        result = store.backup_file(self.src)
        self.assertEqual(result["version"], 3)  # 旧实现会复用 2
        versions = [row["version"] for row in store.list(self.src)]
        self.assertEqual(versions, [3])
        self.assertEqual(len(set(versions)), len(versions))

    def test_backup_detects_source_change_via_plaintext_hash(self) -> None:
        """旧实现传 hash_plaintext=False，却去读 result["sha256"]，检查永远不生效。"""
        self.store.sha256_file = lambda path: "0" * 64  # 模拟哈希计算与加密内容不一致
        with self.assertRaises(IntegrityError):
            self.store.backup_file(self.src)
        leftover = list((self.root / "backups" / "objects").rglob("*.fgc"))
        self.assertEqual(leftover, [])

    def test_object_name_does_not_leak_plaintext_hash(self) -> None:
        self.store.backup_file(self.src)
        content_sha = BackupStore.sha256_file(self.src)
        objects = list((self.root / "backups" / "objects").rglob("*.fgc"))
        self.assertEqual(len(objects), 1)
        self.assertNotIn(content_sha, objects[0].name)
        self.assertNotIn(content_sha, objects[0].relative_to(self.root / "backups").as_posix())

    def test_malicious_index_cannot_escape_objects_dir(self) -> None:
        """备份目录可能被别人拷贝/伪造：索引里的对象路径不能把恢复写到任意位置。"""
        self.store.backup_file(self.src)
        self.store.index.append(
            {
                "event": "backup",
                "path": str(self.src),
                "version": 99,
                "sha256": "a" * 64,
                "size": 1,
                "mtime": 0,
                "object": "../../evil.fgc",
                "ts": "",
            }
        )
        # 重新打开备份库，模拟"拿到一份被伪造过的备份目录"
        tampered = BackupStore(self.root / "backups", TEST_PASSWORD, **BACKUP_ITER)
        with self.assertRaises(IntegrityError):
            tampered.restore(self.src, version=99, overwrite=True)
        for bad in ("../../../windows/x.fgc", "C:/abs.fgc", "aa/nothex.fgc"):
            with self.assertRaises(IntegrityError):
                tampered._safe_object_path(bad)


class ScanIsolationTest(unittest.TestCase):
    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory()
        self.root = Path(self._tmp.name)
        self.scan_root = self.root / "authorized"
        self.scan_root.mkdir()
        (self.scan_root / "normal.txt").write_text("hello", encoding="utf-8")
        self.outside = self.root / "outside"
        self.outside.mkdir()
        (self.outside / "id.txt").write_text(f"身份证 {make_id_card()}", encoding="utf-8")

    def tearDown(self) -> None:
        for link in (self.scan_root / "link", self.scan_root / "filelink"):
            _remove_junction(link)
        self._tmp.cleanup()

    def test_directory_junction_is_not_traversed(self) -> None:
        """os.walk 的 followlinks=False 挡不住 Windows 目录联结，旧实现会扫到目录外。"""
        link = self.scan_root / "link"
        if not _make_junction(link, self.outside):
            self.skipTest("当前环境无法创建目录联结")
        collected = SensitiveScanner().collect([self.scan_root])
        self.assertEqual([p.name for p in collected], ["normal.txt"])
        self.assertEqual([p.name for p in iter_files(self.scan_root)], ["normal.txt"])
        report = scan_paths([self.scan_root])
        self.assertEqual(report.summary["high"], 0)  # 目录外的身份证号不该被发现

    def test_symlinked_file_is_skipped(self) -> None:
        link = self.scan_root / "filelink"
        try:
            os.symlink(self.outside / "id.txt", link)
        except (OSError, NotImplementedError):
            self.skipTest("当前环境无法创建符号链接")
        collected = SensitiveScanner().collect([self.scan_root])
        self.assertNotIn(link.name, [p.name for p in collected])

    def test_office_zip_bomb_is_skipped(self) -> None:
        """旧实现先 zf.read() 全量解压、再检查大小：小文件即可撑爆内存。"""
        docx = self.root / "bomb.docx"
        with zipfile.ZipFile(docx, "w", zipfile.ZIP_DEFLATED) as zf:
            zf.writestr("word/document.xml", "<x>" + ("A" * (6 << 20)) + "</x>")
        text = SensitiveScanner._read_office_text(docx)
        self.assertEqual(text, "")
        risk = SensitiveScanner().scan_file(docx)
        self.assertEqual(risk.risk, "none")


class WatchIsolationTest(unittest.TestCase):
    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory()
        self.root = Path(self._tmp.name)
        self.watch_root = self.root / "authorized"
        self.watch_root.mkdir()
        self.outside = self.root / "outside"
        self.outside.mkdir()
        (self.outside / "secret.txt").write_text("secret", encoding="utf-8")

    def tearDown(self) -> None:
        _remove_junction(self.watch_root / "link")
        self._tmp.cleanup()

    def test_watcher_does_not_follow_junction(self) -> None:
        link = self.watch_root / "link"
        if not _make_junction(link, self.outside):
            self.skipTest("当前环境无法创建目录联结")
        snapshot = DirectoryWatcher([self.watch_root]).take_snapshot()
        self.assertNotIn("secret.txt", " ".join(snapshot))

    def _make_idle_file(self, name: str, content: str) -> Path:
        target = self.watch_root / name
        target.write_text(content, encoding="utf-8")
        # 显式把 mtime 拨回 1 分钟前：idle_seconds=0 时"刚写的文件"可能因时间戳精度
        # 出现 now-mtime<0 而被误判成"还没空闲"，导致用例偶发不执行
        stamp = time.time() - 60
        os.utime(target, (stamp, stamp))
        return target

    def test_auto_encrypt_keeps_source_when_verify_fails(self) -> None:
        """自动加密删明文前必须能解密回来：否则容器写坏/密钥不对会永久丢数据。"""
        target = self._make_idle_file("notes.txt", "重要内容")
        watcher = DirectoryWatcher([self.watch_root])
        with mock.patch("file_guard.watch.verify_file", side_effect=FileGuardError("容器写坏了")):
            results = auto_encrypt_idle(
                watcher,
                password=TEST_PASSWORD,
                idle_seconds=0,
                iterations=TEST_ITERATIONS,
                chunk_size=TEST_CHUNK_SIZE,
                remove_source=True,
            )
        self.assertTrue(target.exists(), "校验失败时必须保留明文")
        self.assertTrue(any(item.get("error") for item in results), results)

    def test_auto_encrypt_deletes_source_after_verification(self) -> None:
        target = self._make_idle_file("notes.txt", "重要内容")
        results = auto_encrypt_idle(
            DirectoryWatcher([self.watch_root]),
            password=TEST_PASSWORD,
            idle_seconds=0,
            iterations=TEST_ITERATIONS,
            chunk_size=TEST_CHUNK_SIZE,
            remove_source=True,
        )
        self.assertFalse(target.exists())
        self.assertTrue((self.watch_root / "notes.txt.fgc").exists())
        self.assertTrue(any(item.get("action") == "encrypted" for item in results), results)


class ServiceHardeningTest(unittest.TestCase):
    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory()
        self.root = Path(self._tmp.name)
        self.cfg = Config(home=self.root / "home", iterations=TEST_ITERATIONS, log_iterations=TEST_ITERATIONS)
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

    def _request(self, headers: dict, body: dict | None = None, method: str = "POST") -> tuple[int, dict]:
        connection = http.client.HTTPConnection("127.0.0.1", self.port, timeout=10)
        raw = json.dumps(body or {}).encode("utf-8")
        connection.request(method, "/rpc", body=raw, headers=headers)
        response = connection.getresponse()
        payload = json.loads(response.read().decode("utf-8") or "{}")
        connection.close()
        return response.status, payload

    def test_refuses_non_loopback_bind(self) -> None:
        with self.assertRaises(ServiceError):
            create_server(self.session, self.token, host="0.0.0.0", port=0)
        # 显式 opt-in 才允许
        server = create_server(self.session, self.token, host="0.0.0.0", port=0, allow_remote=True)
        server.server_close()

    def test_short_token_refused(self) -> None:
        with self.assertRaises(ServiceError):
            create_server(self.session, "short", port=0)

    def test_rejects_foreign_host_header(self) -> None:
        """DNS rebinding：恶意域名解析到 127.0.0.1 后，浏览器发来的 Host 不是本机名。"""
        status, payload = self._request(
            {"X-FG-Token": self.token, "Host": "evil.example.com"},
            {"method": "ping", "params": {}},
        )
        self.assertEqual(status, 403)
        self.assertFalse(payload["ok"])

    def test_rejects_browser_origin(self) -> None:
        status, payload = self._request(
            {"X-FG-Token": self.token, "Origin": "https://evil.example.com"},
            {"method": "ping", "params": {}},
        )
        self.assertEqual(status, 403)
        self.assertFalse(payload["ok"])

    def test_bad_params_return_400(self) -> None:
        status, _ = self._request(
            {"X-FG-Token": self.token, "Host": f"127.0.0.1:{self.port}"},
            {"method": "scan", "params": {"paths": 123}},
        )
        self.assertEqual(status, 400)
        status, _ = self._request(
            {"X-FG-Token": self.token, "Host": f"127.0.0.1:{self.port}"},
            {"method": "scan", "params": {"paths": ["."], "min_risk": "unknown"}},
        )
        self.assertEqual(status, 400)

    def test_valid_request_still_works(self) -> None:
        status, payload = self._request(
            {"X-FG-Token": self.token, "Host": f"127.0.0.1:{self.port}"},
            {"method": "ping", "params": {}},
        )
        self.assertEqual(status, 200)
        self.assertTrue(payload["ok"])


class ConfigAndManifestHardeningTest(unittest.TestCase):
    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory()
        self.root = Path(self._tmp.name)
        self.home = self.root / "home"
        self.home.mkdir()

    def tearDown(self) -> None:
        self._tmp.cleanup()

    def test_out_of_range_config_is_rejected(self) -> None:
        (self.home / "config.json").write_text(json.dumps({"iterations": 5}), encoding="utf-8")
        with self.assertRaises(ConfigError):
            Config.load(self.home)

    def test_non_integer_config_is_rejected(self) -> None:
        (self.home / "config.json").write_text(json.dumps({"iterations": "abc"}), encoding="utf-8")
        with self.assertRaises(ConfigError):
            Config.load(self.home)

    def test_unknown_and_property_keys_are_ignored(self) -> None:
        """旧实现在配置里塞 audit_dir 会 setattr 到 property 上 → AttributeError 崩。"""
        (self.home / "config.json").write_text(
            json.dumps({"audit_dir": "x", "unknown_key": 1, "iterations": TEST_ITERATIONS}),
            encoding="utf-8",
        )
        cfg = Config.load(self.home)
        self.assertEqual(cfg.iterations, TEST_ITERATIONS)

    def test_absurd_manifest_iterations_are_rejected_fast(self) -> None:
        pkg = self.root / "pkg"
        pkg.mkdir()
        (pkg / "mod.py").write_text("x = 1\n", encoding="utf-8")
        manifest = build_manifest(pkg, TEST_PASSWORD)
        manifest["iterations"] = 1_000_000_000  # 旧实现会真的去跑这么多次 PBKDF2
        result = verify_manifest(pkg, manifest, TEST_PASSWORD)
        self.assertFalse(result["ok"])
        self.assertTrue(any("迭代" in p for p in result["problems"]), result["problems"])

    def test_malformed_manifest_salt_is_rejected(self) -> None:
        pkg = self.root / "pkg2"
        pkg.mkdir()
        (pkg / "mod.py").write_text("x = 1\n", encoding="utf-8")
        manifest = build_manifest(pkg, TEST_PASSWORD)
        manifest["salt"] = "!!!not-base64!!!"
        result = verify_manifest(pkg, manifest, TEST_PASSWORD)
        self.assertFalse(result["ok"])


if __name__ == "__main__":
    unittest.main(verbosity=2)
