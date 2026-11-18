"""保险箱测试：DEK / 恢复码 / 文件名加密 / 口令轮换 / 完整性校验。"""

from __future__ import annotations

import os
import tempfile
import unittest
from pathlib import Path

from helpers import TEST_ITERATIONS

from file_guard.errors import FileGuardError, PasswordError
from file_guard.vault import (
    RECOVERY_ALPHABET,
    Vault,
    format_recovery_code,
    normalize_recovery_code,
)

VAULT_ITER = {
    "iterations": TEST_ITERATIONS,
    "log_iterations": TEST_ITERATIONS,
    "chunk_size": 1024,
}
PASSWORD = "vault-test-password-123"
NEW_PASSWORD = "vault-test-password-456"


class VaultTest(unittest.TestCase):
    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory()
        self.root = Path(self._tmp.name)
        self.vault = Vault(self.root / "vault", **VAULT_ITER)
        self.password = PASSWORD
        info = self.vault.init(self.password, dpapi=False)
        self.recovery_code = info["recovery_code"]
        self.src = self.root / "合同.docx"
        self.src.write_text("这是一份测试合同\n", encoding="utf-8")

    def tearDown(self) -> None:
        self._tmp.cleanup()

    # ---- 初始化与解锁 ---------------------------------------------
    def test_init_creates_keys_without_plaintext_secrets(self) -> None:
        keys_path = self.root / "vault" / "vault.keys.json"
        self.assertTrue(keys_path.exists())
        raw = keys_path.read_text(encoding="utf-8")
        self.assertNotIn(self.password, raw)
        self.assertNotIn(self.recovery_code.replace("-", ""), raw)
        # 恢复码 = 32 个 Crockford Base32 字符，8 组
        normalized = normalize_recovery_code(self.recovery_code)
        self.assertEqual(len(normalized), 32)
        self.assertTrue(all(ch in RECOVERY_ALPHABET for ch in normalized))
        self.assertEqual(format_recovery_code(normalized), self.recovery_code)

    def test_wrong_password_rejected(self) -> None:
        with self.assertRaises(PasswordError):
            self.vault.unlock(password="wrong-password")
        # 用恢复码仍然可以解锁
        self.vault.unlock(recovery_code=self.recovery_code)

    def test_unlock_with_recovery_code_tolerates_formatting(self) -> None:
        raw = self.recovery_code.replace("-", "").lower()
        messy = "  " + " ".join(raw[i : i + 4] for i in range(0, len(raw), 4)) + "  "
        dek = self.vault.unlock(recovery_code=messy, use_dpapi=False)
        self.assertEqual(len(dek), 32)

    def test_lock_and_reunlock(self) -> None:
        self.vault.lock()
        self.assertFalse(self.vault.unlocked)
        self.vault.unlock(password=self.password, use_dpapi=False)
        self.assertTrue(self.vault.unlocked)

    # ---- 名称加密 -------------------------------------------------
    def test_container_name_is_opaque_and_original_name_not_on_disk(self) -> None:
        result = self.vault.add_file(self.src)
        container = self.root / "vault" / result["container"]
        self.assertTrue(container.exists())
        self.assertNotIn("合同", container.name)
        self.assertRegex(container.name, r"^[0-9a-f]{32}\.fgc$")
        # 整个保险箱目录的明文文件里不应出现原文件名
        for path in (self.root / "vault").rglob("*"):
            if path.is_file() and path.suffix != ".fgc":
                self.assertNotIn("合同.docx", path.read_text(encoding="utf-8", errors="ignore"))

    def test_list_shows_original_name_only_after_unlock(self) -> None:
        self.vault.add_file(self.src)
        files = self.vault.list_files()
        self.assertEqual([item["name"] for item in files], ["合同.docx"])
        self.assertEqual(files[0]["size"], self.src.stat().st_size)

    # ---- 取出 -----------------------------------------------------
    def test_extract_restores_name_content_and_mtime(self) -> None:
        stamp = 1_700_000_000.0
        os.utime(self.src, (stamp, stamp))
        self.vault.add_file(self.src)
        out = self.root / "restored"
        result = self.vault.extract(1, dest_dir=out)
        dest = out / "合同.docx"
        self.assertEqual(Path(result["restored_to"]), dest)
        self.assertEqual(dest.read_text(encoding="utf-8"), self.src.read_text(encoding="utf-8"))
        self.assertAlmostEqual(dest.stat().st_mtime, stamp, delta=2)

    def test_extract_by_name_and_container(self) -> None:
        added = self.vault.add_file(self.src)
        out = self.root / "restored"
        by_name = self.vault.extract("合同.docx", dest_dir=out)
        self.assertEqual(by_name["name"], "合同.docx")
        by_container = self.vault.extract(Path(added["container"]).name, dest_dir=out, overwrite=True)
        self.assertEqual(by_container["name"], "合同.docx")

    def test_extract_requires_force_when_target_exists(self) -> None:
        self.vault.add_file(self.src)
        out = self.root / "restored"
        self.vault.extract("合同.docx", dest_dir=out)
        with self.assertRaises(FileGuardError):
            self.vault.extract("合同.docx", dest_dir=out)
        self.vault.extract("合同.docx", dest_dir=out, overwrite=True)

    def test_remove_source_only_after_verify(self) -> None:
        result = self.vault.add_file(self.src, remove_source=True)
        self.assertTrue(result["removed_source"])
        self.assertFalse(self.src.exists())

    def test_extract_rejects_path_in_manifest_name(self) -> None:
        with self.assertRaises(Exception):
            Vault._safe_name("../evil.txt")
        with self.assertRaises(Exception):
            Vault._safe_name("a/b.txt")

    # ---- 口令轮换 -------------------------------------------------
    def test_change_password_does_not_reencrypt_containers(self) -> None:
        added = self.vault.add_file(self.src)
        container = self.root / "vault" / added["container"]
        before = container.read_bytes()
        self.vault.change_password(self.password, NEW_PASSWORD)
        self.vault.lock()
        with self.assertRaises(PasswordError):
            self.vault.unlock(password=self.password, use_dpapi=False)
        self.vault.unlock(password=NEW_PASSWORD, use_dpapi=False)
        self.assertEqual(container.read_bytes(), before)  # 容器一个字节都没变
        self.assertEqual(self.vault.extract("合同.docx", dest_dir=self.root / "out")["name"], "合同.docx")

    def test_show_recovery_code_after_password_change(self) -> None:
        self.vault.change_password(self.password, NEW_PASSWORD)
        code = self.vault.show_recovery_code(NEW_PASSWORD)
        self.assertEqual(normalize_recovery_code(code), normalize_recovery_code(self.recovery_code))

    # ---- 完整性 ---------------------------------------------------
    def test_verify_ok(self) -> None:
        self.vault.add_file(self.src)
        result = self.vault.verify(full=True)
        self.assertTrue(result["ok"], result["problems"])
        self.assertEqual(result["checked"], 1)

    def test_verify_detects_manifest_tamper(self) -> None:
        self.vault.add_file(self.src)
        names = self.root / "vault" / "names.jsonl"
        names.write_text(names.read_text(encoding="utf-8") + '{"seq":999}\n', encoding="utf-8")
        result = self.vault.verify(full=False)
        self.assertFalse(result["ok"])

    def test_verify_detects_corrupted_container(self) -> None:
        added = self.vault.add_file(self.src)
        container = self.root / "vault" / added["container"]
        raw = bytearray(container.read_bytes())
        raw[-1] ^= 0xFF
        container.write_bytes(bytes(raw))
        result = self.vault.verify(full=True)
        self.assertFalse(result["ok"])

    def test_info_reports_counts(self) -> None:
        self.vault.add_file(self.src)
        payload = self.vault.info()
        self.assertEqual(payload["containers"], 1)
        self.assertEqual(payload["files"], 1)
        self.assertFalse(payload["dpapi"])


@unittest.skipUnless(os.name == "nt", "DPAPI 仅 Windows 可用")
class VaultDpapiTest(unittest.TestCase):
    def test_dpapi_unlock_without_password(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            vault = Vault(Path(tmp) / "vault", **VAULT_ITER)
            info = vault.init(PASSWORD, dpapi=True)
            self.assertTrue(info["dpapi"])
            vault.lock()
            dek = vault.unlock(use_dpapi=True)  # 不给口令
            self.assertEqual(len(dek), 32)


if __name__ == "__main__":
    unittest.main(verbosity=2)
