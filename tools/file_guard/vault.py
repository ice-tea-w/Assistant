"""保险箱：DEK 模型 + 恢复码 + 文件名加密（对应 M3 安全模块"保护"能力的三处缺口）。

原 `crypto.py` 的容器是"口令直接派生密钥"，有三个局限：

1. **文件名明文**：`合同.docx.fgc` 一眼就知道里面是什么（M3 要求的 NameCipher 缺失）；
2. **没有恢复码**：忘记口令 = 数据永久不可读，用户不敢用；
3. **改口令要重加密全部容器**：大文件库轮换口令代价高。

本模块在**不改动 FGC1 容器格式**的前提下补齐这三项：

```
password ──PBKDF2──▶ KEK ──AES-GCM──▶ 包住 DEK ──▶ vault.keys.json
recovery code ─────▶ KEK ──AES-GCM──▶ 包住 DEK ──┘
DPAPI（可选）──────────────────────▶ 包住 DEK ──┘   ← Windows 当前用户免密解锁
                                 │
                                 ▼
                    DEK 作为"口令材料"加密每个容器
                 容器文件名 = 随机 32 hex，不含任何原文信息
                 原文件名/大小/mtime/哈希 → names.jsonl（DEK 加密 + HMAC 链）
```

要点：

* **改口令只重包 DEK**，容器一个字节都不动（`change_password`）；
* **恢复码是第二把钥匙**，不是后门：它和口令一样能解 DEK，泄露 = 泄露保险箱；
  丢失它且忘记口令 = 数据不可恢复（与 M3 的诚实边界一致）；
* 名称清单本身复用 `EncryptedLog`（AES-GCM + HMAC 链 + 随机 nonce + 高水位），
  改一行/删一行都会被 `verify` 发现；
* 容器名随机，所以**目录列表本身不泄露原文信息**（这是 NameCipher 的最小可行形态，
  真正的 M3 还应把名称密文写进容器头部；这里用加密清单达到同等隐蔽性）。
"""

from __future__ import annotations

import base64
import json
import os
import secrets
from dataclasses import dataclass, field
from datetime import datetime
from pathlib import Path
from typing import Any, Optional, Union

from cryptography.exceptions import InvalidTag
from cryptography.hazmat.primitives.ciphers.aead import AESGCM

from .audit import EncryptedLog
from .crypto import (
    DEFAULT_CHUNK_SIZE,
    DEFAULT_ITERATIONS,
    SUFFIX,
    decrypt_file,
    derive_key,
    dpapi_protect,
    dpapi_unprotect,
    encrypt_file,
    normalize_password,
    verify_file,
)
from .errors import (
    FileGuardError,
    IntegrityError,
    PasswordError,
    UnsupportedPlatformError,
)
from .fsutil import restrict_acl

VAULT_VERSION = 1
DEK_SIZE = 32
NONCE_SIZE = 12
#: 恢复码 = 20 字节随机数（160 bit）→ Crockford Base32 恰好 32 个字符
RECOVERY_CODE_BYTES = 20
#: Crockford Base32：去掉 I/L/O/U，避免手抄歧义
RECOVERY_ALPHABET = "0123456789ABCDEFGHJKMNPQRSTVWXYZ"

AAD_DEK_PASSWORD = b"FG-VAULT-DEK-PASSWORD-v1"
AAD_DEK_RECOVERY = b"FG-VAULT-DEK-RECOVERY-v1"
AAD_RECOVERY_CODE = b"FG-VAULT-RECOVERY-CODE-v1"

KEYS_FILE = "vault.keys.json"
CONTAINERS_DIR = "containers"
RESTORE_DIR = "restored"
NAMES_LOG = "names"
DEFAULT_LOG_ITERATIONS = 200_000


# ---------------------------------------------------------------------------
# 恢复码编解码
# ---------------------------------------------------------------------------
def _encode_b32(data: bytes) -> str:
    total_bits = len(data) * 8
    value = int.from_bytes(data, "big")
    chars = []
    for shift in range(total_bits - 5, -1, -5):
        chars.append(RECOVERY_ALPHABET[(value >> shift) & 31])
    return "".join(chars)


def _decode_b32(text: str) -> bytes:
    value = 0
    for char in text:
        value = (value << 5) | RECOVERY_ALPHABET.index(char)
    return value.to_bytes(RECOVERY_CODE_BYTES, "big")


def normalize_recovery_code(text: str) -> str:
    """把用户抄下来的恢复码规范化（大小写/分隔符/易混字符都容忍）。"""
    if not isinstance(text, str):
        raise PasswordError("恢复码必须是字符串")
    cleaned = "".join(ch for ch in text.upper() if ch.isalnum())
    cleaned = cleaned.translate(str.maketrans({"I": "1", "L": "1", "O": "0", "U": "V"}))
    if len(cleaned) != 32:
        raise PasswordError(f"恢复码长度不对（应为 32 个字符，实际 {len(cleaned)}）")
    for char in cleaned:
        if char not in RECOVERY_ALPHABET:
            raise PasswordError(f"恢复码包含非法字符：{char}")
    return cleaned


def format_recovery_code(text: str) -> str:
    """32 字符 → `XXXX-XXXX-...`，便于抄写。"""
    cleaned = normalize_recovery_code(text)
    return "-".join(cleaned[i : i + 4] for i in range(0, len(cleaned), 4))


# ---------------------------------------------------------------------------
# 保险箱
# ---------------------------------------------------------------------------
@dataclass
class VaultInfo:
    directory: str
    created: str = ""
    dpapi: bool = False
    containers: int = 0
    files: int = 0
    recovery_code: str = ""
    extra: dict[str, Any] = field(default_factory=dict)

    def to_dict(self) -> dict[str, Any]:
        return {
            "directory": self.directory,
            "created": self.created,
            "dpapi": self.dpapi,
            "containers": self.containers,
            "files": self.files,
            "recovery_code": self.recovery_code or None,
        }


class Vault:
    """一个目录 = 一个保险箱：`vault.keys.json` + `containers/` + `names.jsonl`。"""

    def __init__(
        self,
        directory: Union[str, Path],
        *,
        iterations: int = DEFAULT_ITERATIONS,
        log_iterations: int = DEFAULT_LOG_ITERATIONS,
        chunk_size: int = DEFAULT_CHUNK_SIZE,
        fsync: bool = False,
    ) -> None:
        self.directory = Path(directory)
        self.iterations = iterations
        self.log_iterations = log_iterations
        self.chunk_size = chunk_size
        self.fsync = fsync
        self.keys_path = self.directory / KEYS_FILE
        self.containers_dir = self.directory / CONTAINERS_DIR
        self._keys: Optional[dict[str, Any]] = None
        self._dek: Optional[bytes] = None
        self._log: Optional[EncryptedLog] = None

    # ---- 基础 -----------------------------------------------------
    @property
    def exists(self) -> bool:
        return self.keys_path.exists()

    @property
    def unlocked(self) -> bool:
        return self._dek is not None

    def lock(self) -> None:
        self._dek = None
        self._log = None
        self._keys = None

    def info(self) -> dict[str, Any]:
        if not self.exists:
            return VaultInfo(directory=str(self.directory)).to_dict()
        keys = self._load_keys()
        containers = (
            len(list(self.containers_dir.glob(f"*{SUFFIX}")))
            if self.containers_dir.exists()
            else 0
        )
        files = 0
        if self.unlocked:
            try:
                files = len(self.list_files())
            except FileGuardError:
                files = 0
        return VaultInfo(
            directory=str(self.directory),
            created=str(keys.get("created", "")),
            dpapi=bool(keys.get("dpapi")),
            containers=containers,
            files=files,
        ).to_dict()

    # ---- 初始化 ---------------------------------------------------
    def init(self, password: Union[str, bytes], *, dpapi: bool = True) -> dict[str, Any]:
        if self.exists:
            raise FileGuardError(f"保险箱已存在：{self.directory}")
        self.directory.mkdir(parents=True, exist_ok=True)
        self.containers_dir.mkdir(parents=True, exist_ok=True)

        dek = os.urandom(DEK_SIZE)
        password_salt = os.urandom(16)
        recovery_salt = os.urandom(16)
        code_bytes = os.urandom(RECOVERY_CODE_BYTES)

        password_kek = derive_key(normalize_password(password), password_salt, self.iterations)
        # 恢复码本身是高熵随机数，直接当口令材料派生 KEK
        recovery_kek = derive_key(code_bytes, recovery_salt, self.iterations)

        keys: dict[str, Any] = {
            "version": VAULT_VERSION,
            "created": datetime.now().astimezone().isoformat(timespec="seconds"),
            "kdf": {"salt": _b64(password_salt), "iterations": self.iterations},
            "recovery": {"salt": _b64(recovery_salt), "iterations": self.iterations},
            "wrapped": {
                "password": self._wrap(dek, password_kek, AAD_DEK_PASSWORD),
                "recovery": self._wrap(dek, recovery_kek, AAD_DEK_RECOVERY),
            },
            # 恢复码在 DEK 下再存一份：凭口令可以重新展示（丢失恢复码不至于只能靠备份）
            "recovery_code_under_dek": self._wrap(code_bytes, dek, AAD_RECOVERY_CODE),
            "dpapi": None,
        }
        if dpapi:
            try:
                keys["dpapi"] = _b64(dpapi_protect(dek))
            except UnsupportedPlatformError:
                keys["dpapi"] = None

        self._write_keys(keys)
        self._keys = keys
        self._dek = dek  # 初始化后直接处于解锁态，方便紧接着 add
        return {
            "directory": str(self.directory),
            "dpapi": bool(keys["dpapi"]),
            "recovery_code": format_recovery_code(_encode_b32(code_bytes)),
        }

    # ---- 解锁 -----------------------------------------------------
    def unlock(
        self,
        password: Union[str, bytes, None] = None,
        *,
        recovery_code: Optional[str] = None,
        use_dpapi: bool = True,
    ) -> bytes:
        """用口令 / 恢复码 / DPAPI 解出 DEK。"""
        keys = self._load_keys()
        if recovery_code is not None:
            code_bytes = _decode_b32(normalize_recovery_code(recovery_code))
            kek = derive_key(code_bytes, _unb64(keys["recovery"]["salt"]),
                             int(keys["recovery"].get("iterations", self.iterations)))
            dek = self._unwrap(keys["wrapped"]["recovery"], kek, AAD_DEK_RECOVERY)
        elif password is not None:
            kek = derive_key(normalize_password(password), _unb64(keys["kdf"]["salt"]),
                             int(keys["kdf"].get("iterations", self.iterations)))
            dek = self._unwrap(keys["wrapped"]["password"], kek, AAD_DEK_PASSWORD)
        elif use_dpapi and keys.get("dpapi"):
            try:
                dek = dpapi_unprotect(_unb64(str(keys["dpapi"])))
            except (FileGuardError, UnsupportedPlatformError) as exc:
                raise PasswordError(
                    "DPAPI 自动解锁失败（可能换了 Windows 用户或机器）；请改用口令或恢复码"
                ) from exc
        else:
            raise PasswordError("需要口令或恢复码才能解锁保险箱")
        if len(dek) != DEK_SIZE:
            raise IntegrityError("保险箱 DEK 长度异常")
        self._keys = keys
        self._dek = dek
        self._log = None
        return dek

    def change_password(
        self,
        old_password: Union[str, bytes],
        new_password: Union[str, bytes],
        *,
        recovery_code: Optional[str] = None,
    ) -> dict[str, Any]:
        """轮换口令：只重新包 DEK，容器与名称清单不重加密。"""
        dek = self.unlock(password=old_password, recovery_code=recovery_code, use_dpapi=False)
        keys = self._load_keys()
        new_salt = os.urandom(16)
        kek = derive_key(normalize_password(new_password), new_salt, self.iterations)
        keys["kdf"] = {"salt": _b64(new_salt), "iterations": self.iterations}
        keys["wrapped"]["password"] = self._wrap(dek, kek, AAD_DEK_PASSWORD)
        if keys.get("dpapi"):
            try:
                keys["dpapi"] = _b64(dpapi_protect(dek))
            except UnsupportedPlatformError:
                keys["dpapi"] = None
        self._write_keys(keys)
        self._keys = keys
        self._dek = dek
        return {
            "rotated": True,
            "containers_reencrypted": False,
            "directory": str(self.directory),
        }

    def show_recovery_code(self, password: Union[str, bytes]) -> str:
        """凭口令重新展示恢复码（恢复码本身在 DEK 下加密保存）。"""
        dek = self.unlock(password=password, use_dpapi=False)
        keys = self._load_keys()
        raw = self._unwrap(keys["recovery_code_under_dek"], dek, AAD_RECOVERY_CODE)
        return format_recovery_code(_encode_b32(raw))

    # ---- 文件操作 -------------------------------------------------
    def add_file(
        self,
        path: Union[str, Path],
        *,
        note: str = "",
        remove_source: bool = False,
        password: Union[str, bytes, None] = None,
        recovery_code: Optional[str] = None,
    ) -> dict[str, Any]:
        """把文件加密进保险箱：容器名随机，原文件名只存在于加密清单里。"""
        dek = self._require_dek(password, recovery_code)
        source = Path(path).expanduser()
        if not source.is_file():
            raise FileGuardError(f"要放进保险箱的文件不存在：{source}")
        source = source.resolve()
        self.containers_dir.mkdir(parents=True, exist_ok=True)

        container = self.containers_dir / f"{secrets.token_hex(16)}{SUFFIX}"
        info = encrypt_file(
            source,
            container,
            dek,
            iterations=self.iterations,
            chunk_size=self.chunk_size,
            hash_plaintext=True,
        )
        if not info.get("sha256"):
            container.unlink(missing_ok=True)
            raise IntegrityError("加密结果没有明文哈希，已中止")
        stat = source.stat()
        record = {
            "kind": "file",
            "container": self._rel_container(container),
            "name": source.name,
            "size": int(info["plaintext_size"]),
            "sha256": info["sha256"],
            "mtime": float(stat.st_mtime),
            "ts": datetime.now().astimezone().isoformat(timespec="seconds"),
            "note": note,
        }
        self._names(dek).append(record)

        removed = False
        if remove_source:
            # 删源文件前必须证明容器能完整解回来（与 auto_encrypt 同一原则）
            check = verify_file(container, dek)
            if check.get("sha256") != info["sha256"]:
                container.unlink(missing_ok=True)
                raise IntegrityError("加密结果校验失败，已保留源文件")
            source.unlink()
            removed = True
        return {
            "container": record["container"],
            "name": record["name"],
            "size": record["size"],
            "sha256": record["sha256"],
            "removed_source": removed,
        }

    def list_files(
        self,
        *,
        password: Union[str, bytes, None] = None,
        recovery_code: Optional[str] = None,
    ) -> list[dict[str, Any]]:
        dek = self._require_dek(password, recovery_code)
        files: list[dict[str, Any]] = []
        for row in self._names(dek).query(limit=10**9):
            if row.get("kind") != "file":
                continue
            files.append(
                {
                    "name": str(row.get("name", "")),
                    "container": str(row.get("container", "")),
                    "size": int(row.get("size", 0)),
                    "sha256": str(row.get("sha256", "")),
                    "ts": str(row.get("ts", "")),
                    "note": str(row.get("note", "")),
                }
            )
        return files

    def extract(
        self,
        target: Union[int, str],
        *,
        dest_dir: Optional[Union[str, Path]] = None,
        overwrite: bool = False,
        password: Union[str, bytes, None] = None,
        recovery_code: Optional[str] = None,
    ) -> dict[str, Any]:
        """按序号（1 起）/ 容器名 / 原文件名取出，并还原原文件名与 mtime。"""
        dek = self._require_dek(password, recovery_code)
        rows = [row for row in self._names(dek).query(limit=10**9) if row.get("kind") == "file"]
        entry = self._match_entry(rows, target)
        container = self._safe_container(entry.get("container"))
        name = self._safe_name(entry.get("name"))
        out_dir = Path(dest_dir).expanduser() if dest_dir else self.directory / RESTORE_DIR
        dest = out_dir / name
        if dest.exists() and not overwrite:
            raise FileGuardError(f"目标已存在：{dest}（如需覆盖请加 --force）")
        result = decrypt_file(container, dest, dek, overwrite=overwrite, hash_plaintext=True)
        if result.get("sha256") != entry.get("sha256"):
            dest.unlink(missing_ok=True)
            raise IntegrityError("还原后哈希与清单不一致，已删除还原文件")
        mtime = entry.get("mtime")
        if mtime:
            try:
                os.utime(dest, (float(mtime), float(mtime)))
            except (OSError, ValueError, TypeError):
                pass
        return {
            "name": name,
            "restored_to": str(dest),
            "size": int(entry.get("size", 0)),
            "sha256": str(entry.get("sha256", "")),
        }

    def verify(
        self,
        *,
        full: bool = True,
        password: Union[str, bytes, None] = None,
        recovery_code: Optional[str] = None,
    ) -> dict[str, Any]:
        """校验名称清单哈希链；``full`` 时逐个解密容器核对内容哈希。"""
        dek = self._require_dek(password, recovery_code)
        problems: list[str] = []
        try:
            rows = [row for row in self._names(dek).query(limit=10**9) if row.get("kind") == "file"]
        except FileGuardError as exc:
            return {"ok": False, "files": 0, "checked": 0, "problems": [str(exc)]}
        checked = 0
        if full:
            for row in rows:
                name = str(row.get("name", ""))
                try:
                    container = self._safe_container(row.get("container"))
                    info = verify_file(container, dek)
                    checked += 1
                    if info.get("sha256") != row.get("sha256"):
                        problems.append(f"{name}：容器内容与清单哈希不一致")
                except (FileGuardError, OSError) as exc:
                    problems.append(f"{name}：{exc}")
        return {
            "ok": not problems,
            "files": len(rows),
            "checked": checked,
            "problems": problems[:20],
        }

    # ---- 内部 -----------------------------------------------------
    def _names(self, dek: Optional[bytes] = None) -> EncryptedLog:
        if self._log is None:
            dek = dek or self._require_dek()
            self._log = EncryptedLog(
                self.directory,
                dek,
                name=NAMES_LOG,
                iterations=self.log_iterations,
                fsync=self.fsync,
            )
        return self._log

    def _require_dek(
        self,
        password: Union[str, bytes, None] = None,
        recovery_code: Optional[str] = None,
    ) -> bytes:
        if self._dek is not None:
            return self._dek
        if recovery_code is not None:
            return self.unlock(recovery_code=recovery_code)
        if password is not None:
            return self.unlock(password=password)
        return self.unlock(use_dpapi=True)

    def _load_keys(self) -> dict[str, Any]:
        if self._keys is not None:
            return self._keys
        if not self.keys_path.exists():
            raise FileGuardError(f"保险箱不存在：{self.directory}")
        try:
            keys = json.loads(self.keys_path.read_text(encoding="utf-8"))
        except (OSError, json.JSONDecodeError) as exc:
            raise IntegrityError(f"保险箱密钥文件损坏：{self.keys_path}") from exc
        if not isinstance(keys, dict) or keys.get("version") != VAULT_VERSION:
            raise IntegrityError("保险箱密钥文件版本不受支持")
        for field_name in ("kdf", "recovery", "wrapped"):
            if field_name not in keys:
                raise IntegrityError(f"保险箱密钥文件缺少字段：{field_name}")
        return keys

    def _write_keys(self, keys: dict[str, Any]) -> None:
        self.directory.mkdir(parents=True, exist_ok=True)
        tmp = self.keys_path.with_name(self.keys_path.name + ".tmp")
        body = json.dumps(keys, ensure_ascii=False, indent=2).encode("utf-8")
        flags = os.O_CREAT | os.O_WRONLY | os.O_TRUNC | getattr(os, "O_BINARY", 0)
        fd = os.open(tmp, flags, 0o600)
        try:
            os.write(fd, body)
            os.fsync(fd)
        finally:
            os.close(fd)
        os.replace(tmp, self.keys_path)
        restrict_acl(self.keys_path)

    @staticmethod
    def _wrap(secret: bytes, key: bytes, aad: bytes) -> str:
        nonce = os.urandom(NONCE_SIZE)
        return _b64(nonce + AESGCM(key).encrypt(nonce, secret, aad))

    @staticmethod
    def _unwrap(blob: str, key: bytes, aad: bytes) -> bytes:
        raw = _unb64(blob)
        if len(raw) <= NONCE_SIZE:
            raise IntegrityError("保险箱密钥块结构损坏")
        try:
            return AESGCM(key).decrypt(raw[:NONCE_SIZE], raw[NONCE_SIZE:], aad)
        except InvalidTag as exc:
            raise PasswordError("口令或恢复码不正确") from exc

    def _rel_container(self, path: Path) -> str:
        return path.relative_to(self.directory).as_posix()

    def _safe_container(self, rel: Any) -> Path:
        text = str(rel or "")
        if not text.startswith(CONTAINERS_DIR + "/") or ".." in Path(text).parts:
            raise IntegrityError(f"清单里的容器路径非法：{text!r}")
        path = (self.directory / text).resolve()
        if not path.is_relative_to(self.containers_dir.resolve()):
            raise IntegrityError(f"容器路径越界：{text!r}")
        return path

    @staticmethod
    def _safe_name(value: Any) -> str:
        name = str(value or "")
        if not name or name in (".", ".."):
            raise IntegrityError("清单里的文件名为空或非法")
        if "/" in name or "\\" in name or "\x00" in name:
            raise IntegrityError(f"清单里的文件名包含路径分隔符：{name!r}")
        if len(name) > 255:
            raise IntegrityError("清单里的文件名过长")
        return name

    @staticmethod
    def _match_entry(rows: list[dict[str, Any]], target: Union[int, str]) -> dict[str, Any]:
        if not rows:
            raise FileGuardError("保险箱里没有文件")
        if isinstance(target, int) or (isinstance(target, str) and target.isdigit()):
            index = int(target)
            if index < 1 or index > len(rows):
                raise FileGuardError(f"序号超出范围：{index}（共 {len(rows)} 个文件）")
            return rows[index - 1]
        text = str(target)
        matched = [
            row
            for row in rows
            if text in (row.get("name"), row.get("container"), str(row.get("container", "")).split("/")[-1])
        ]
        if not matched:
            raise FileGuardError(f"保险箱里没有匹配的文件：{text}")
        if len(matched) > 1:
            raise FileGuardError(f"匹配到 {len(matched)} 个文件，请用序号（1 起）指定")
        return matched[0]


def _b64(data: bytes) -> str:
    return base64.b64encode(data).decode("ascii")


def _unb64(text: str) -> bytes:
    try:
        return base64.b64decode(text.encode("ascii"), validate=True)
    except (ValueError, UnicodeEncodeError) as exc:
        raise IntegrityError("保险箱密钥块不是合法 base64") from exc


__all__ = [
    "DEK_SIZE",
    "RECOVERY_CODE_BYTES",
    "Vault",
    "VaultInfo",
    "format_recovery_code",
    "normalize_recovery_code",
]
