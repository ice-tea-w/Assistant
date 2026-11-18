"""操作审计日志：每条记录 AES-256-GCM 加密 + HMAC 哈希链防篡改。

对应设计稿"操作审计日志"与数据存储表：

* 记录文件打开/修改/删除/加密/解密/备份/恢复/扫描等事件；
* 日志条目**逐条加密**（AES-256-GCM），文件名、路径等敏感信息不落明文；
* 每条记录的哈希链接到上一条（``h_i = HMAC(chain_key, h_{i-1} || nonce_i || ct_i)``），
  改一行、删一行、调换顺序都会在 ``verify()`` 里暴露；
* 日志不上传、不联网；"手动清空"采用归档而非直接删除，防止误操作。

记录格式（v2，当前）::

    {"seq": i, "n": base64(12 字节随机 nonce), "h": 链哈希, "d": base64(密文)}
    AAD = b"FGL1" || seq(8) || nonce

v1（旧格式，只读兼容）：``{"seq","h","d"}``，nonce = ``meta.nonce_prefix || seq``，
AAD = ``b"FGL1" || seq``。**v2 的关键改进**：每条记录独立随机 nonce。v1 的 nonce 由
"固定前缀 + 序号"拼成，一旦日志被整体回滚/删除后继续追加（或 meta 被旧版本覆盖），
就会用同一密钥 + 同一 nonce 加密不同明文 —— AES-GCM 下这会同时泄露明文异或值和
GHASH 认证密钥（可伪造任意记录）。逐条随机 nonce 让回滚也无法造成复用；v1 记录仍可
读取，新写入一律用 v2。

已知边界：攻击者若把"日志 + meta"整体替换成**更早的完整快照**再继续写，本地文件
无法自证（任何防回滚都需要外部锚点）。这里做了两层缓解：

* ``meta.mac`` = 密码派生密钥的 HMAC，防"只改 meta 不碰日志"的篡改；
* ``meta.next_seq`` 记录高水位，日志比它短就报"被回滚/截断"；
* 需要更强保证时用 ``head()`` 把链头哈希抄到注册表/另一台设备，
  再用 ``verify(expected_head=...)`` 比对。
"""

from __future__ import annotations

import base64
import hashlib
import hmac
import json
import os
import socket
import struct
import time
from contextlib import contextmanager
from dataclasses import dataclass
from datetime import datetime
from getpass import getuser
from pathlib import Path
from typing import Any, Iterator, Optional, Union

from cryptography.exceptions import InvalidTag
from cryptography.hazmat.primitives.ciphers.aead import AESGCM

from .crypto import Password, derive_key
from .errors import DecryptionError, FileGuardError, PasswordError

LOG_FORMAT_VERSION = 2
LEGACY_LOG_FORMAT_VERSION = 1
GENESIS_LABEL = b"FG-AUDIT-GENESIS-v1"
AAD_LABEL = b"FGL1"
META_MAC_LABEL = b"FG-META-v1"
DEDUP_LABEL = b"FG-OBJ-v1"
NONCE_SIZE = 12
HASH_HEX_LEN = 64
MAX_SEQ = 0xFFFF_FFFF
TAIL_WINDOW = 1 << 20  # 追加/取尾哈希只读最后 1 MiB，大日志也接近 O(1)

EVENT_OPEN = "open"
EVENT_MODIFY = "modify"
EVENT_DELETE = "delete"
EVENT_CREATE = "create"
EVENT_ENCRYPT = "encrypt"
EVENT_DECRYPT = "decrypt"
EVENT_BACKUP = "backup"
EVENT_RESTORE = "restore"
EVENT_SCAN = "scan"
EVENT_PURGE = "purge"
EVENT_SETTINGS = "settings"
EVENT_INTEGRITY = "integrity"
EVENTS = {
    EVENT_OPEN,
    EVENT_MODIFY,
    EVENT_DELETE,
    EVENT_CREATE,
    EVENT_ENCRYPT,
    EVENT_DECRYPT,
    EVENT_BACKUP,
    EVENT_RESTORE,
    EVENT_SCAN,
    EVENT_PURGE,
    EVENT_SETTINGS,
    EVENT_INTEGRITY,
}


def _is_hex64(value: str) -> bool:
    if len(value) != HASH_HEX_LEN:
        return False
    try:
        bytes.fromhex(value)
    except ValueError:
        return False
    return True


def _b64decode(value: Any) -> bytes:
    if not isinstance(value, str):
        raise ValueError("不是 base64 字符串")
    return base64.b64decode(value.encode("ascii"), validate=True)


def _pid_alive(pid: int) -> bool:
    """判断进程是否存活（用于锁文件：只抢占"持有者已死"的过期锁）。"""
    if pid <= 0:
        return False
    if os.name == "nt":
        try:
            import ctypes

            PROCESS_QUERY_LIMITED_INFORMATION = 0x1000
            STILL_ACTIVE = 259
            kernel32 = ctypes.windll.kernel32
            handle = kernel32.OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, False, pid)
            if not handle:
                return False
            try:
                code = ctypes.c_ulong()
                if not kernel32.GetExitCodeProcess(handle, ctypes.byref(code)):
                    return True  # 查不到就当还活着，避免误删别人的锁
                return code.value == STILL_ACTIVE
            finally:
                kernel32.CloseHandle(handle)
        except Exception:  # noqa: BLE001 - 判断失败时保守处理
            return True
    try:
        os.kill(pid, 0)
    except ProcessLookupError:
        return False
    except PermissionError:
        return True
    except OSError:
        return True
    return True


@dataclass
class LogRecord:
    seq: int
    payload: dict[str, Any]
    ciphertext: bytes
    chain_hash: str


class _FileLock:
    """跨进程写锁：O_EXCL 锁文件 + 过期清理。

    过期判定不只看 mtime：还要确认锁文件里的 pid 确实已经退出，避免"另一个进程
    只是跑得慢"时被抢锁，导致两条记录用同一个 seq / 同一条链。
    """

    def __init__(self, path: Path, timeout: float = 10.0, stale: float = 300.0) -> None:
        self.path = path
        self.timeout = timeout
        self.stale = stale

    def _stale(self) -> bool:
        try:
            stat = self.path.stat()
        except FileNotFoundError:
            return True
        if time.time() - stat.st_mtime < self.stale:
            return False
        try:
            pid = int(self.path.read_text(encoding="utf-8").strip() or "0")
        except (OSError, ValueError):
            pid = 0
        return not _pid_alive(pid)

    @contextmanager
    def __call__(self) -> Iterator[None]:
        deadline = time.monotonic() + self.timeout
        fd: Optional[int] = None
        while True:
            try:
                fd = os.open(self.path, os.O_CREAT | os.O_EXCL | os.O_WRONLY, 0o600)
                os.write(fd, str(os.getpid()).encode("ascii"))
                break
            except FileExistsError:
                if self._stale():
                    self.path.unlink(missing_ok=True)
                    continue
                if time.monotonic() > deadline:
                    raise FileGuardError(f"日志正被其他进程占用：{self.path}")
                time.sleep(0.05)
        try:
            yield
        finally:
            if fd is not None:
                os.close(fd)
            self.path.unlink(missing_ok=True)


class EncryptedLog:
    """通用的"加密 + 哈希链"仅追加日志（审计日志、备份索引共用）。"""

    def __init__(
        self,
        directory: Union[str, Path],
        password: Password,
        *,
        name: str = "audit",
        iterations: int = 200_000,
        fsync: bool = False,
    ) -> None:
        self.directory = Path(directory)
        self.name = name
        self.log_path = self.directory / f"{name}.jsonl"
        self.meta_path = self.directory / f"{name}.meta.json"
        self._password = password
        self.iterations = iterations
        self.fsync = fsync
        self._meta: Optional[dict[str, Any]] = None
        self._key: Optional[bytes] = None
        self._aes: Optional[AESGCM] = None
        self._prefix_bytes: Optional[bytes] = None
        self._lock = _FileLock(self.directory / f"{name}.lock")

    # ---- 元数据 & 密钥 -------------------------------------------
    def _read_meta(self) -> Optional[dict[str, Any]]:
        if not self.meta_path.exists():
            return None
        try:
            meta = json.loads(self.meta_path.read_text(encoding="utf-8"))
        except (OSError, json.JSONDecodeError) as exc:
            raise FileGuardError(f"日志元数据损坏：{self.meta_path}") from exc
        if not isinstance(meta, dict):
            raise FileGuardError(f"日志元数据格式不正确：{self.meta_path}")
        version = meta.get("version")
        if version not in (LEGACY_LOG_FORMAT_VERSION, LOG_FORMAT_VERSION):
            raise FileGuardError(f"不支持的日志格式版本：{version}")
        try:
            salt = _b64decode(meta["salt"])
            prefix = _b64decode(meta["nonce_prefix"])
            iterations = int(meta["iterations"])
        except (KeyError, ValueError, TypeError) as exc:
            raise FileGuardError(f"日志元数据字段损坏：{self.meta_path}") from exc
        if len(salt) < 16 or len(prefix) != 8:
            raise FileGuardError(f"日志元数据字段长度异常：{self.meta_path}")
        if not 1_000 <= iterations <= 5_000_000:
            raise FileGuardError(f"日志元数据 KDF 迭代次数异常：{iterations}")
        if "next_seq" in meta:
            try:
                meta["next_seq"] = int(meta["next_seq"])
            except (TypeError, ValueError) as exc:
                raise FileGuardError("日志元数据 next_seq 非法") from exc
        # 旧版 meta 在内存里按新格式处理（缺的字段由 appender 补齐）
        meta["version"] = LOG_FORMAT_VERSION
        return meta

    def _new_meta(self) -> dict[str, Any]:
        return {
            "version": LOG_FORMAT_VERSION,
            "iterations": self.iterations,
            "salt": base64.b64encode(os.urandom(16)).decode(),
            "nonce_prefix": base64.b64encode(os.urandom(8)).decode(),
            "created": datetime.now().astimezone().isoformat(timespec="seconds"),
            "next_seq": 0,
            "head": None,
        }

    def _write_meta(self, meta: dict[str, Any], *, exclusive: bool) -> bool:
        """原子写 meta（tmp + replace）；exclusive=True 时用 O_EXCL 防并发首建。

        注意 meta 的持久化策略要和日志行一致（fsync=False 时都不强制落盘）：
        否则崩溃后可能"meta 记了 N 条、日志只剩 N-1 条"，被误判成回滚攻击。
        """
        self.meta_path.parent.mkdir(parents=True, exist_ok=True)
        body = json.dumps(meta, ensure_ascii=False, indent=2).encode("utf-8")
        target = self.meta_path
        binary = getattr(os, "O_BINARY", 0)  # Windows 文本模式会篡改 \n，必须显式二进制
        if exclusive:
            try:
                fd = os.open(target, os.O_CREAT | os.O_EXCL | os.O_WRONLY | binary, 0o600)
            except FileExistsError:
                return False
            try:
                os.write(fd, body)
            finally:
                os.close(fd)
            return True
        tmp = target.with_name(target.name + ".tmp")
        fd = os.open(tmp, os.O_CREAT | os.O_WRONLY | os.O_TRUNC | binary, 0o600)
        try:
            os.write(fd, body)
            if self.fsync:
                os.fsync(fd)
        finally:
            os.close(fd)
        os.replace(tmp, target)
        return True

    def _ensure_meta(self) -> dict[str, Any]:
        if self._meta is not None:
            return self._meta
        meta = self._read_meta()
        if meta is None:
            meta = self._new_meta()
            if not self._write_meta(meta, exclusive=True):
                # 并发：另一个进程先建好了，用它的
                meta = self._read_meta()
                if meta is None:
                    raise FileGuardError(f"日志元数据创建失败：{self.meta_path}")
        self._meta = meta
        return meta

    def _refresh_meta(self) -> dict[str, Any]:
        """持锁追加前重新读盘上的 meta。

        长期运行的进程（例如常驻服务）可能在别的进程执行过 ``log clear`` 之后仍持有
        旧缓存：旧盐派生出的密钥解不开新日志，会报"密码不对"，让人误以为口令丢了。
        这里发现盐/迭代次数变化就丢弃旧密钥重新派生。
        """
        meta = self._read_meta()
        if meta is None:
            self._reset_state()
            return self._ensure_meta()
        cached = self._meta
        if cached is not None and (
            cached.get("salt") != meta.get("salt")
            or cached.get("iterations") != meta.get("iterations")
        ):
            self._key = None
            self._aes = None
            self._prefix_bytes = None
        self._meta = meta
        return meta

    def _get_aes(self) -> AESGCM:
        if self._aes is None:
            meta = self._ensure_meta()
            self._key = derive_key(
                self._password,
                _b64decode(meta["salt"]),
                int(meta["iterations"]),
                length=32,
            )
            self._aes = AESGCM(self._key)
            self._prefix_bytes = _b64decode(meta["nonce_prefix"])
        return self._aes

    def _nonce(self, seq: int) -> bytes:
        """v1 旧格式 nonce（仅用于读旧记录；新记录用逐条随机 nonce）。"""
        self._ensure_meta()
        if self._prefix_bytes is None:
            self._prefix_bytes = _b64decode(self._meta["nonce_prefix"])  # type: ignore[index]
        return self._prefix_bytes + struct.pack(">I", seq)

    def _record_aad(self, seq: int, nonce: bytes, legacy: bool) -> bytes:
        head = AAD_LABEL + struct.pack(">Q", seq)
        return head if legacy else head + nonce

    def _chain_key(self) -> bytes:
        """哈希链用 HMAC（而不是裸 SHA-256）：没有密码就无法重算链，
        因此"删掉中间一条再重排"这种攻击在验证时必然暴露。"""
        self._get_aes()
        assert self._key is not None
        return hashlib.sha256(self._key + b"FG-CHAIN-v1").digest()

    def subkey(self, label: bytes) -> bytes:
        """从主密钥派生的用途隔离子密钥（备份对象命名去标识化等）。"""
        self._get_aes()
        assert self._key is not None
        return hmac.new(self._key, label, hashlib.sha256).digest()

    def _chain_hash(self, prev_hex: str, ciphertext: bytes, nonce: Optional[bytes] = None) -> str:
        data = bytes.fromhex(prev_hex)
        if nonce is not None:
            data += nonce
        return hmac.new(self._chain_key(), data + ciphertext, hashlib.sha256).hexdigest()

    def _genesis(self) -> str:
        meta = self._ensure_meta()
        return hmac.new(
            self._chain_key(), GENESIS_LABEL + _b64decode(meta["salt"]), hashlib.sha256
        ).hexdigest()

    def _meta_body(self, meta: dict[str, Any]) -> bytes:
        body = {k: v for k, v in meta.items() if k != "mac"}
        return json.dumps(body, sort_keys=True, ensure_ascii=False, separators=(",", ":")).encode("utf-8")

    def _meta_mac(self, meta: dict[str, Any]) -> str:
        return hmac.new(self.subkey(META_MAC_LABEL), self._meta_body(meta), hashlib.sha256).hexdigest()

    def _verify_meta_mac(self) -> Optional[bool]:
        """None 表示旧版 meta 没有签名；False 表示签名不匹配（密码错或 meta 被改）。"""
        meta = self._ensure_meta()
        stored = meta.get("mac")
        if not stored:
            return None
        try:
            return hmac.compare_digest(str(stored), self._meta_mac(meta))
        except (TypeError, ValueError):
            return False

    def _save_meta(self, *, next_seq: int, head: str) -> None:
        """更新 meta 高水位并重新签名（必须在持锁的 append 里调用）。"""
        meta = self._ensure_meta()
        meta["version"] = LOG_FORMAT_VERSION
        meta["next_seq"] = int(next_seq)
        meta["head"] = head
        meta["mac"] = self._meta_mac(meta)
        self._write_meta(meta, exclusive=False)

    # ---- 写入 -----------------------------------------------------
    def append(self, payload: dict[str, Any]) -> LogRecord:
        self.directory.mkdir(parents=True, exist_ok=True)
        with self._lock():
            self._ensure_meta()
            self._refresh_meta()  # 别的进程可能 clear 过：不能用过期缓存
            self._get_aes()  # 先派生密钥，meta 签名/密码校验都要用
            head, seq, last_raw, torn = self._read_tail()
            self._check_append_allowed(seq, last_raw)
            if torn:
                self._truncate_torn_tail()  # 崩溃留下的半截行：截掉后继续
            if seq > MAX_SEQ:
                raise FileGuardError("日志条目数量已达上限，请先归档/清空")
            data = json.dumps(payload, ensure_ascii=False, separators=(",", ":")).encode("utf-8")
            nonce = os.urandom(NONCE_SIZE)
            ct = self._get_aes().encrypt(nonce, data, self._record_aad(seq, nonce, legacy=False))
            chain = self._chain_hash(head, ct, nonce)
            line = json.dumps(
                {
                    "seq": seq,
                    "n": base64.b64encode(nonce).decode(),
                    "h": chain,
                    "d": base64.b64encode(ct).decode(),
                },
                separators=(",", ":"),
            )
            with open(self.log_path, "a", encoding="utf-8", newline="\n") as f:
                f.write(line + "\n")
                f.flush()
                if self.fsync:
                    os.fsync(f.fileno())
            self._save_meta(next_seq=seq + 1, head=chain)
        return LogRecord(seq=seq, payload=payload, ciphertext=ct, chain_hash=chain)

    def _check_append_allowed(self, next_seq: int, last_raw: Optional[dict[str, Any]]) -> None:
        """追加前的自检：密码对不对、meta 有没有被改、日志有没有被回滚。

        旧实现在密码错误时也能追加：那条记录立刻变成永远解不开的垃圾，还会让后续
        校验全部报错。这里先解密最后一条记录确认密码，再核对 meta 高水位。
        """
        if self._verify_meta_mac() is False:
            raise PasswordError(
                "密码或密钥与已有审计日志不匹配（meta 签名校验失败，也可能是 meta 被篡改）。"
                "请使用初始化时的密码/密钥；确实要换口令请先用 `log clear` 归档旧日志"
            )
        if last_raw is not None:
            seq, ct, nonce, legacy = self._parse_record(last_raw)
            try:
                self._get_aes().decrypt(nonce, ct, self._record_aad(seq, nonce, legacy))
            except InvalidTag as exc:
                raise PasswordError("密码不正确（无法解密日志最后一条记录），已拒绝追加，避免写坏日志") from exc
        meta = self._ensure_meta()
        meta_next = meta.get("next_seq")
        if meta_next is None:
            return
        expected = int(meta_next)
        if expected > 0 and last_raw is None:
            raise FileGuardError(
                f"日志文件缺失，但 meta 记录已写入 {expected} 条：疑似被删除/回滚。"
                "确认无误后可用 `file_guard log clear` 归档后重新开始"
            )
        # 正常追加是"先写日志行、再写 meta 高水位"；崩溃只会让 meta 落后（日志行没落盘），
        # 不会多写。所以 meta 比日志"超前"就意味着回滚/截断，直接拒绝追加。
        if next_seq < expected:
            raise FileGuardError(
                f"日志被回滚或截断：meta 记录下一条应为 seq={expected}，日志尾部却是 seq={next_seq}。"
                "请恢复日志，或用 `file_guard log clear` 归档后重新开始"
            )

    # ---- 尾部读取 ------------------------------------------------
    def _read_tail(self) -> tuple[str, int, Optional[dict[str, Any]], bool]:
        """返回 (链头哈希, 下一条序号, 最后一条完整记录, 是否有残缺尾行)。"""
        last, torn = self._tail_line()
        if last is None:
            return self._genesis(), 0, None, torn
        try:
            rec = json.loads(last.decode("utf-8"))
            seq, _ct, _nonce, _legacy = self._parse_record(rec)
        except (ValueError, KeyError, TypeError, UnicodeDecodeError) as exc:
            raise FileGuardError("日志尾部损坏，无法继续追加") from exc
        return str(rec["h"]), seq + 1, rec, torn
    def _tail_line(self) -> tuple[Optional[bytes], bool]:
        """返回 (最后一条**完整**记录的原始字节, 是否存在残缺尾行)。

        大日志只读最后 1 MiB；尾部找不到完整行时回退读全文（超长单条记录）。
        末尾没有换行的半截行视为"写入中断"的残行，单独标记，供 append 自愈。
        """
        if not self.log_path.exists():
            return None, False
        size = self.log_path.stat().st_size
        if size == 0:
            return None, False
        window = min(size, TAIL_WINDOW)
        while True:
            with open(self.log_path, "rb") as f:
                f.seek(size - window)
                data = f.read()
            ends_nl = data.endswith(b"\n")
            parts = data.split(b"\n")
            if ends_nl:
                parts = parts[:-1]
            while parts and not parts[-1].strip():
                parts.pop()
            torn = False
            if not ends_nl:
                if not parts:
                    if window < size:
                        window = size
                        continue
                    return None, True
                # 最后一段是残行，丢掉
                parts.pop()
                torn = True
                while parts and not parts[-1].strip():
                    parts.pop()
            if not parts:
                if window < size:
                    window = size
                    continue
                return None, torn
            last = parts[-1]
            if window < size:
                try:
                    self._parse_record(json.loads(last.decode("utf-8")))
                except (ValueError, KeyError, TypeError, UnicodeDecodeError):
                    window = size
                    continue
            return last, torn

    def _truncate_torn_tail(self) -> None:
        with open(self.log_path, "r+b") as f:
            f.seek(0, os.SEEK_END)
            pos = f.tell()
            chunk = 65536
            while pos > 0:
                start = max(0, pos - chunk)
                f.seek(start)
                data = f.read(pos - start)
                idx = data.rfind(b"\n")
                if idx >= 0:
                    f.truncate(start + idx + 1)
                    return
                pos = start
            f.truncate(0)

    def _tail_state(self) -> tuple[str, int, Optional[dict[str, Any]]]:
        """返回 (链头哈希, 下一条序号, 最后一条记录)（只读，不修复）。"""
        head, seq, last_raw, _torn = self._read_tail()
        return head, seq, last_raw

    def _parse_record(self, raw: dict[str, Any]) -> tuple[int, bytes, bytes, bool]:
        """校验并拆一条记录，返回 (seq, ciphertext, nonce, 是否旧格式)。"""
        if not isinstance(raw, dict):
            raise ValueError("记录不是对象")
        seq = int(raw["seq"])
        if not 0 <= seq <= MAX_SEQ:
            raise ValueError("seq 越界")
        stored = str(raw["h"])
        if not _is_hex64(stored):
            raise ValueError("哈希字段格式非法")
        ct = _b64decode(raw["d"])
        legacy = "n" not in raw
        if legacy:
            nonce = self._nonce(seq)
        else:
            nonce = _b64decode(raw["n"])
            if len(nonce) != NONCE_SIZE:
                raise ValueError("nonce 长度非法")
        return seq, ct, nonce, legacy

    # ---- 读取/校验 ------------------------------------------------
    def _raw_lines(self) -> tuple[list[dict[str, Any]], bool]:
        """逐行读日志（不整文件进内存），并判断文件是否以换行结尾。"""
        if not self.log_path.exists():
            return [], True
        out: list[dict[str, Any]] = []
        with open(self.log_path, "rb") as f:
            size = f.seek(0, os.SEEK_END)
            if size == 0:
                return [], True
            f.seek(size - 1)
            ends_nl = f.read(1) == b"\n"
            f.seek(0)
            for lineno, raw in enumerate(f, 1):
                line = raw.strip()
                if not line:
                    continue
                try:
                    out.append(json.loads(line.decode("utf-8")))
                except (json.JSONDecodeError, UnicodeDecodeError):
                    out.append({"__broken__": True, "lineno": lineno})
        return out, ends_nl

    def read_all(self, *, verify_chain: bool = True) -> tuple[list[LogRecord], list[str]]:
        """解密全部记录并校验哈希链，返回 (记录列表, 问题列表)。"""
        records: list[LogRecord] = []
        problems: list[str] = []
        head = self._genesis()
        aes = self._get_aes()
        if self._verify_meta_mac() is False:
            problems.append("日志元数据（meta.json）签名无效：密码不正确，或 meta 被篡改")
        raw_lines, ends_nl = self._raw_lines()
        for index, raw in enumerate(raw_lines):
            if raw.get("__broken__"):
                if index == len(raw_lines) - 1 and not ends_nl:
                    problems.append("日志末尾有一条没写完的记录（进程崩溃或写入中断）")
                else:
                    problems.append(f"第 {raw['lineno']} 行不是合法 JSON")
                break
            try:
                seq, ct, nonce, legacy = self._parse_record(raw)
            except (KeyError, ValueError, TypeError) as exc:
                problems.append(f"第 {index + 1} 条记录字段非法：{exc}")
                break
            stored = str(raw["h"])
            if seq != index:
                problems.append(f"序号不连续：期望 {index}，实际 {seq}（疑似删除了中间记录）")
            expected_h = self._chain_hash(head, ct, None if legacy else nonce)
            if verify_chain and stored != expected_h:
                problems.append(f"第 {seq} 条哈希校验失败（内容被修改、删除或顺序被调整）")
            try:
                plain = aes.decrypt(nonce, ct, self._record_aad(seq, nonce, legacy))
            except InvalidTag:
                problems.append(f"第 {seq} 条无法解密（密码错误，或该条被篡改）")
                plain = None
            payload: dict[str, Any] = {}
            if plain is not None:
                try:
                    payload = json.loads(plain.decode("utf-8"))
                except (json.JSONDecodeError, UnicodeDecodeError):
                    problems.append(f"第 {seq} 条明文不是合法 JSON")
            records.append(LogRecord(seq=seq, payload=payload, ciphertext=ct, chain_hash=stored))
            head = stored  # 用文件里记录的链值继续，避免一次篡改引发后续"雪崩"误报
        meta_next = self._ensure_meta().get("next_seq")
        if verify_chain and meta_next is not None and int(meta_next) > len(records):
            problems.append(
                f"meta 记录已写入 {int(meta_next)} 条，日志实际只有 {len(records)} 条"
                "（日志被回滚/截断，或崩溃丢失了末尾记录）"
            )
        return records, problems

    def payloads(self) -> list[dict[str, Any]]:
        records, problems = self.read_all()
        if problems:
            raise FileGuardError("日志完整性校验失败：" + "；".join(problems[:3]))
        return [r.payload for r in records]

    def head(self) -> str:
        head, _seq, _last = self._tail_state()
        return head

    def verify(self, expected_head: Optional[str] = None) -> dict[str, Any]:
        records, problems = self.read_all()
        head, seq, _last = self._tail_state()
        if expected_head and expected_head != head:
            problems.append("链头哈希与外部锚点不一致：日志被截断或整体替换")
        return {
            "ok": not problems,
            "records": len(records),
            "head": head,
            "next_seq": seq,
            "problems": problems[:20],
        }

    def tail(self, limit: int = 20) -> list[dict[str, Any]]:
        records, problems = self.read_all()
        if problems:
            raise FileGuardError("日志完整性校验失败：" + "；".join(problems[:3]))
        return [r.payload for r in records[-max(1, limit) :]]

    def query(
        self,
        *,
        event: Optional[str] = None,
        contains: Optional[str] = None,
        since: Optional[str] = None,
        limit: int = 200,
    ) -> list[dict[str, Any]]:
        rows = self.payloads()
        if event:
            rows = [r for r in rows if r.get("event") == event]
        if contains:
            needle = contains.lower()
            rows = [
                r
                for r in rows
                if needle in json.dumps(r, ensure_ascii=False).lower()
            ]
        if since:
            rows = [r for r in rows if str(r.get("ts", "")) >= since]
        return rows[-max(1, limit) :]

    # ---- 清空/归档 -------------------------------------------------
    def _archive_path(self, source: Path, stamp: str) -> Path:
        """归档文件名带上时间戳；同一秒内多次 clear 时自动加序号，避免覆盖旧归档。"""
        base = f"{self.name}-{stamp}"
        if source == self.meta_path:
            candidate = self.directory / f"{base}.meta.json"
            template = "{base}-{n}.meta.json"
        else:
            candidate = self.directory / f"{base}.jsonl"
            template = "{base}-{n}.jsonl"
        index = 1
        while candidate.exists():
            candidate = self.directory / template.format(base=base, n=index)
            index += 1
        return candidate

    def clear(self, *, archive: bool = True) -> dict[str, Any]:
        with self._lock():
            stamp = datetime.now().strftime("%Y%m%d-%H%M%S")
            archived: list[str] = []
            cleared = False
            if archive:
                for source in (self.log_path, self.meta_path):
                    if source.exists():
                        target = self._archive_path(source, stamp)
                        os.replace(source, target)
                        archived.append(str(target))
                        cleared = True
            else:
                for source in (self.log_path, self.meta_path):
                    if source.exists():
                        source.unlink()
                        cleared = True
            self._reset_state()
            return {"cleared": cleared, "archived": archived}

    def _reset_state(self) -> None:
        self._meta = None
        self._key = None
        self._aes = None
        self._prefix_bytes = None

    def export(self, path: Union[str, Path]) -> Path:
        rows = self.payloads()
        path = Path(path)
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(json.dumps(rows, ensure_ascii=False, indent=2), encoding="utf-8")
        return path


class AuditLog:
    """面向业务的审计日志（事件字段语义化）。"""

    def __init__(
        self,
        directory: Union[str, Path],
        password: Password,
        *,
        iterations: int = 200_000,
        fsync: bool = False,
    ) -> None:
        self.log = EncryptedLog(directory, password, name="audit", iterations=iterations, fsync=fsync)

    def record(
        self,
        event: str,
        *,
        path: Optional[Union[str, Path]] = None,
        detail: Optional[str] = None,
        **extra: Any,
    ) -> LogRecord:
        if event not in EVENTS:
            raise FileGuardError(f"未定义的事件类型：{event}")
        payload: dict[str, Any] = {
            "ts": datetime.now().astimezone().isoformat(timespec="seconds"),
            "event": event,
            "path": str(path) if path is not None else None,
            "detail": detail,
            "user": getuser(),
            "host": socket.gethostname(),
            "pid": os.getpid(),
        }
        if extra:
            payload.update(extra)
        return self.log.append(payload)

    def tail(self, limit: int = 20) -> list[dict[str, Any]]:
        return self.log.tail(limit)

    def query(self, **kwargs: Any) -> list[dict[str, Any]]:
        return self.log.query(**kwargs)

    def verify(self, expected_head: Optional[str] = None) -> dict[str, Any]:
        return self.log.verify(expected_head=expected_head)

    def head(self) -> str:
        return self.log.head()

    def clear(self, *, archive: bool = True) -> dict[str, Any]:
        return self.log.clear(archive=archive)

    def export(self, path: Union[str, Path]) -> Path:
        return self.log.export(path)
