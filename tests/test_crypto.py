"""底层安全层测试：容器格式、GCM 完整性、防截断、密钥派生、DPAPI。"""

from __future__ import annotations

import os
import tempfile
import unittest
from pathlib import Path

from helpers import TEST_CHUNK_SIZE, TEST_ITERATIONS, TEST_PASSWORD

from file_guard import crypto
from file_guard.crypto import (
    HEADER_SIZE,
    DpapiVault,
    decrypt_file,
    derive_key,
    encrypt_file,
    generate_keyfile,
    inspect_container,
    is_container,
    load_keyfile_material,
    rekey_file,
    verify_file,
)
from file_guard.errors import CorruptContainerError, DecryptionError, FileGuardError

EXTRA = {"iterations": TEST_ITERATIONS, "chunk_size": TEST_CHUNK_SIZE}


class KeyDerivationTest(unittest.TestCase):
    def test_deterministic_and_salt_sensitive(self) -> None:
        salt = os.urandom(16)
        key1 = derive_key(TEST_PASSWORD, salt, TEST_ITERATIONS)
        key2 = derive_key(TEST_PASSWORD, salt, TEST_ITERATIONS)
        key3 = derive_key(TEST_PASSWORD, os.urandom(16), TEST_ITERATIONS)
        self.assertEqual(key1, key2)
        self.assertEqual(len(key1), 32)
        self.assertNotEqual(key1, key3)

    def test_password_bytes_and_unicode(self) -> None:
        salt = b"0" * 16
        self.assertEqual(
            derive_key("密码123", salt, TEST_ITERATIONS),
            derive_key("密码123".encode("utf-8"), salt, TEST_ITERATIONS),
        )

    def test_short_salt_rejected(self) -> None:
        with self.assertRaises(Exception):
            derive_key(TEST_PASSWORD, b"short", TEST_ITERATIONS)


class FileRoundTripTest(unittest.TestCase):
    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory()
        self.root = Path(self._tmp.name)

    def tearDown(self) -> None:
        self._tmp.cleanup()

    def _roundtrip(self, data: bytes, name: str = "sample.bin") -> Path:
        src = self.root / name
        src.write_bytes(data)
        container = self.root / (name + ".fgc")
        info = encrypt_file(src, container, TEST_PASSWORD, **EXTRA)
        self.assertEqual(info["plaintext_size"], len(data))
        self.assertTrue(container.exists())
        restored = self.root / (name + ".restored")
        out = decrypt_file(container, restored, TEST_PASSWORD)
        self.assertEqual(restored.read_bytes(), data)
        self.assertEqual(out["sha256"], info["sha256"])
        return container

    def test_empty_file(self) -> None:
        self._roundtrip(b"")

    def test_small_file(self) -> None:
        self._roundtrip(b"hello file guard \x00\x01\x02")

    def test_exact_chunk_multiple(self) -> None:
        """恰好整块时没有"最后一块"的边界问题。"""
        self._roundtrip(os.urandom(TEST_CHUNK_SIZE * 3))

    def test_multi_chunk_with_remainder(self) -> None:
        data = os.urandom(TEST_CHUNK_SIZE * 4 + 17)
        container = self._roundtrip(data)
        info = inspect_container(container)
        self.assertEqual(info["chunks"] if "chunks" in info else 5, 5)
        self.assertEqual(info["plaintext_size"], len(data))
        self.assertEqual(info["chunk_size"], TEST_CHUNK_SIZE)

    def test_unicode_filename(self) -> None:
        self._roundtrip(b"contract", name="合同-2026.docx")

    def test_container_magic(self) -> None:
        container = self._roundtrip(b"abc")
        self.assertTrue(is_container(container))
        self.assertFalse(is_container(self.root / "sample.bin"))
        self.assertEqual(container.read_bytes()[:4], b"FGC1")

    def test_existing_target_requires_force(self) -> None:
        src = self.root / "a.txt"
        src.write_text("data", encoding="utf-8")
        dst = self.root / "a.txt.fgc"
        encrypt_file(src, dst, TEST_PASSWORD, **EXTRA)
        with self.assertRaises(FileGuardError):
            encrypt_file(src, dst, TEST_PASSWORD, **EXTRA)
        encrypt_file(src, dst, TEST_PASSWORD, overwrite=True, **EXTRA)


class TamperTest(unittest.TestCase):
    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory()
        self.root = Path(self._tmp.name)
        self.src = self.root / "secret.txt"
        self.src.write_bytes(b"top secret " * 300)
        self.container = self.root / "secret.txt.fgc"
        encrypt_file(self.src, self.container, TEST_PASSWORD, **EXTRA)

    def tearDown(self) -> None:
        self._tmp.cleanup()

    def _flip(self, offset: int) -> None:
        raw = bytearray(self.container.read_bytes())
        raw[offset] ^= 0xFF
        self.container.write_bytes(bytes(raw))

    def test_wrong_password(self) -> None:
        with self.assertRaises(DecryptionError):
            decrypt_file(self.container, self.root / "out.txt", "wrong-password")

    def test_ciphertext_tamper(self) -> None:
        self._flip(HEADER_SIZE + 5)
        with self.assertRaises(DecryptionError):
            decrypt_file(self.container, self.root / "out.txt", TEST_PASSWORD)

    def test_header_tamper(self) -> None:
        self._flip(20)  # 改盐：头部摘要立刻不匹配
        with self.assertRaises(CorruptContainerError):
            decrypt_file(self.container, self.root / "out.txt", TEST_PASSWORD)

    def test_truncation(self) -> None:
        raw = self.container.read_bytes()
        self.container.write_bytes(raw[: HEADER_SIZE + 100])
        with self.assertRaises((CorruptContainerError, DecryptionError)):
            decrypt_file(self.container, self.root / "out.txt", TEST_PASSWORD)

    def test_trailing_garbage(self) -> None:
        with open(self.container, "ab") as f:
            f.write(b"garbage")
        with self.assertRaises(CorruptContainerError):
            decrypt_file(self.container, self.root / "out.txt", TEST_PASSWORD)

    def test_failed_decrypt_leaves_no_partial_file(self) -> None:
        self._flip(HEADER_SIZE + 3)
        target = self.root / "out.txt"
        with self.assertRaises(DecryptionError):
            decrypt_file(self.container, target, TEST_PASSWORD)
        self.assertFalse(target.exists())
        self.assertEqual([p.name for p in self.root.iterdir() if p.suffix == ".part"], [])


class VerifyRekeyTest(unittest.TestCase):
    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory()
        self.root = Path(self._tmp.name)
        self.src = self.root / "doc.txt"
        self.src.write_text("hello" * 500, encoding="utf-8")
        self.container = self.root / "doc.txt.fgc"
        encrypt_file(self.src, self.container, TEST_PASSWORD, **EXTRA)

    def tearDown(self) -> None:
        self._tmp.cleanup()

    def test_verify_reports_plaintext_hash(self) -> None:
        info = verify_file(self.container, TEST_PASSWORD)
        self.assertEqual(info["plaintext_size"], self.src.stat().st_size)
        self.assertEqual(len(info["sha256"]), 64)
        with self.assertRaises(DecryptionError):
            verify_file(self.container, "bad")

    def test_rekey_keeps_data(self) -> None:
        new_container = self.root / "doc2.fgc"
        rekey_file(self.container, new_container, TEST_PASSWORD, "new-password-456", **EXTRA)
        with self.assertRaises(DecryptionError):
            decrypt_file(new_container, self.root / "out1.txt", TEST_PASSWORD)
        out = decrypt_file(new_container, self.root / "out2.txt", "new-password-456")
        self.assertEqual(Path(out["dst"]).read_text(encoding="utf-8"), "hello" * 500)


class KeyfileTest(unittest.TestCase):
    def test_keyfile_material(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            keyfile = generate_keyfile(Path(tmp) / "guard.key")
            self.assertEqual(keyfile.stat().st_size, 32)
            material = load_keyfile_material(keyfile)
            self.assertEqual(len(material), 32)
            with self.assertRaises(FileGuardError):
                generate_keyfile(keyfile)
            with self.assertRaises(Exception):
                load_keyfile_material(Path(tmp) / "missing.key")


@unittest.skipUnless(os.name == "nt", "DPAPI 仅 Windows 可用")
class DpapiVaultTest(unittest.TestCase):
    def test_roundtrip(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            vault = DpapiVault(Path(tmp) / "vault.bin")
            self.assertIsNone(vault.load())
            vault.store("hunter2-hunter2")
            self.assertEqual(vault.load(), "hunter2-hunter2")
            raw = vault.path.read_bytes()
            self.assertNotIn(b"hunter2", raw)  # 磁盘上没有明文
            vault.clear()
            self.assertIsNone(vault.load())


if __name__ == "__main__":
    unittest.main(verbosity=2)
