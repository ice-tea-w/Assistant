"""底层安全层：PBKDF2 密钥派生 + AES-256-GCM 分块加密容器。

对应设计稿"底层安全层（AES/哈希/密钥管理）"与"数据存储方案"：

* 密码永不明文落盘：每个容器自带随机盐，用 PBKDF2-HMAC-SHA256 派生 32 字节密钥；
* 大文件不整体读入内存：按 1 MiB 分块，每块独立 nonce + GCM tag（STREAM 构造）；
* 防截断/防重排/防篡改：每块 AAD 绑定"头部 + 块序号 + 是否最后一块"；
* 解密先写临时文件，校验通过后原子替换，失败不留半截明文。

容器格式（FGC1，全部大端）::

    offset size  field
    0      4     magic  b"FGC1"
    4      1     version = 1
    5      1     kdf_id = 1 (PBKDF2-HMAC-SHA256)
    6      1     cipher_id = 1 (AES-256-GCM)
    7      1     flags（保留，当前 0）
    8      4     iterations
    12     4     chunk_size
    16     16    salt
    32     8     nonce_prefix（每块 nonce = prefix || counter(4)）
    40     8     plaintext_size
    48     32    header_digest = SHA-256(前 48 字节)
    80     ...   分块密文：ciphertext(chunk) || tag(16)
"""

from __future__ import annotations

import base64
import hashlib
import json
import os
import struct
import tempfile
from pathlib import Path
from typing import Any, BinaryIO, Callable, Optional, Union

from cryptography.exceptions import InvalidTag
from cryptography.hazmat.primitives import hashes
from cryptography.hazmat.primitives.ciphers.aead import AESGCM
from cryptography.hazmat.primitives.kdf.pbkdf2 import PBKDF2HMAC

from .errors import (
    CorruptContainerError,
    DecryptionError,
    FileGuardError,
    PasswordError,
    UnsupportedPlatformError,
)

Password = Union[str, bytes]
Progress = Optional[Callable[[int, int], None]]

MAGIC = b"FGC1"
FORMAT_VERSION = 1
KDF_PBKDF2_SHA256 = 1
CIPHER_AES256_GCM = 1
HEADER_STRUCT = struct.Struct(">4sBBBBII16s8sQ")
HEADER_SIZE = HEADER_STRUCT.size + 32  # 48 + 32 = 80
TAG_SIZE = 16
SALT_SIZE = 16
NONCE_PREFIX_SIZE = 8
KEY_SIZE = 32
SUFFIX = ".fgc"

DEFAULT_ITERATIONS = 600_000
MIN_ITERATIONS = 1_000
MAX_ITERATIONS = 5_000_000  # 防止恶意文件用超大迭代次数拖死解密
DEFAULT_CHUNK_SIZE = 1 << 20
MIN_CHUNK_SIZE = 256
MAX_CHUNK_SIZE = 64 << 20
MAX_CHUNKS = 0xFFFFFFFF


# ---------------------------------------------------------------------------
# 密钥派生
# ---------------------------------------------------------------------------
def normalize_password(password: Password) -> bytes:
    """统一密码字节表示：str→UTF-8，bytes 原样。空密码直接拒绝。"""
    if isinstance(password, str):
        if not password:
            raise PasswordError("密码不能为空")
        return password.encode("utf-8")
    if isinstance(password, (bytes, bytearray)) and len(password) > 0:
        return bytes(password)
    raise PasswordError("密码必须是非空的 str 或 bytes")


def derive_key(password: Password, salt: bytes, iterations: int, length: int = KEY_SIZE) -> bytes:
    """PBKDF2-HMAC-SHA256 派生密钥（C 实现，600k 次约 0.2~0.4 秒）。"""
    if not MIN_ITERATIONS <= iterations <= MAX_ITERATIONS:
        raise FileGuardError(f"KDF 迭代次数超出允许范围：{iterations}")
    if len(salt) < 16:
        raise PasswordError("盐长度不足 16 字节")
    kdf = PBKDF2HMAC(
        algorithm=hashes.SHA256(),
        length=length,
        salt=salt,
        iterations=iterations,
    )
    return kdf.derive(normalize_password(password))


def _nonce(prefix: bytes, index: int) -> bytes:
    return prefix + struct.pack(">I", index)


def _aad(header: bytes, index: int, final: bool) -> bytes:
    return header + struct.pack(">IB", index, 1 if final else 0)


def _validate_chunk_size(chunk_size: int) -> int:
    if not MIN_CHUNK_SIZE <= chunk_size <= MAX_CHUNK_SIZE:
        raise FileGuardError(f"分块大小需在 [{MIN_CHUNK_SIZE}, {MAX_CHUNK_SIZE}] 之间")
    return chunk_size


def _build_header(
    iterations: int,
    chunk_size: int,
    salt: bytes,
    nonce_prefix: bytes,
    plaintext_size: int,
) -> bytes:
    fields = HEADER_STRUCT.pack(
        MAGIC,
        FORMAT_VERSION,
        KDF_PBKDF2_SHA256,
        CIPHER_AES256_GCM,
        0,
        iterations,
        chunk_size,
        salt,
        nonce_prefix,
        plaintext_size,
    )
    return fields + hashlib.sha256(fields).digest()


def _parse_header(raw: bytes) -> dict[str, Any]:
    """校验并解析 80 字节头部（不需要密码）。"""
    if len(raw) != HEADER_SIZE:
        raise CorruptContainerError("文件过短，不是有效的加密容器")
    body = raw[: HEADER_STRUCT.size]
    if hashlib.sha256(body).digest() != raw[HEADER_STRUCT.size :]:
        raise CorruptContainerError("容器头部校验失败（可能已损坏或被篡改）")
    (
        magic,
        version,
        kdf_id,
        cipher_id,
        flags,
        iterations,
        chunk_size,
        salt,
        nonce_prefix,
        plaintext_size,
    ) = HEADER_STRUCT.unpack(body)
    if magic != MAGIC:
        raise CorruptContainerError("文件魔数不匹配，不是 FGC1 加密容器")
    if version != FORMAT_VERSION:
        raise CorruptContainerError(f"不支持的容器版本：{version}")
    if kdf_id != KDF_PBKDF2_SHA256 or cipher_id != CIPHER_AES256_GCM:
        raise CorruptContainerError("不支持的算法组合")
    if not MIN_ITERATIONS <= iterations <= MAX_ITERATIONS:
        raise CorruptContainerError(f"容器声明的 KDF 迭代次数异常：{iterations}")
    if not MIN_CHUNK_SIZE <= chunk_size <= MAX_CHUNK_SIZE:
        raise CorruptContainerError(f"容器声明的分块大小异常：{chunk_size}")
    return {
        "version": version,
        "iterations": iterations,
        "chunk_size": chunk_size,
        "salt": salt,
        "nonce_prefix": nonce_prefix,
        "plaintext_size": plaintext_size,
        "flags": flags,
        "header": raw,
    }


# ---------------------------------------------------------------------------
# 流式加解密（字节级核心，文件/内存共用）
# ---------------------------------------------------------------------------
def _encrypt_stream(
    fin: BinaryIO,
    fout: BinaryIO,
    plaintext_size: int,
    password: Password,
    *,
    iterations: int,
    chunk_size: int,
    progress: Progress = None,
    hash_plaintext: bool = False,
) -> dict[str, Any]:
    chunk_size = _validate_chunk_size(chunk_size)
    chunks = (plaintext_size + chunk_size - 1) // chunk_size if plaintext_size else 0
    if chunks > MAX_CHUNKS:
        raise FileGuardError("文件过大，请增大 chunk_size")
    salt = os.urandom(SALT_SIZE)
    nonce_prefix = os.urandom(NONCE_PREFIX_SIZE)
    header = _build_header(iterations, chunk_size, salt, nonce_prefix, plaintext_size)
    key = derive_key(password, salt, iterations)
    aes = AESGCM(key)
    digest = hashlib.sha256() if hash_plaintext else None

    fout.write(header)
    done = 0
    index = 0
    while done < plaintext_size:
        want = min(chunk_size, plaintext_size - done)
        data = fin.read(want)
        if len(data) != want:
            raise FileGuardError("读取源数据时长度不足（文件被修改或流提前结束）")
        final = done + want == plaintext_size
        if digest is not None:
            digest.update(data)
        fout.write(aes.encrypt(_nonce(nonce_prefix, index), data, _aad(header, index, final)))
        done += want
        index += 1
        if progress:
            progress(done, plaintext_size)
    fout.flush()
    result = {
        "plaintext_size": plaintext_size,
        "ciphertext_size": HEADER_SIZE + plaintext_size + chunks * TAG_SIZE,
        "chunks": chunks,
        "iterations": iterations,
        "chunk_size": chunk_size,
    }
    if digest is not None:
        result["sha256"] = digest.hexdigest()
    return result


def _decrypt_stream(
    fin: BinaryIO,
    fout: BinaryIO,
    password: Password,
    *,
    progress: Progress = None,
    hash_plaintext: bool = False,
) -> dict[str, Any]:
    raw_header = fin.read(HEADER_SIZE)
    info = _parse_header(raw_header)
    header = info["header"]
    chunk_size = info["chunk_size"]
    total = info["plaintext_size"]
    key = derive_key(password, info["salt"], info["iterations"])
    aes = AESGCM(key)
    digest = hashlib.sha256() if hash_plaintext else None

    done = 0
    index = 0
    while done < total:
        want = min(chunk_size, total - done)
        chunk = fin.read(want + TAG_SIZE)
        if len(chunk) != want + TAG_SIZE:
            raise CorruptContainerError("容器被截断：密文分块长度不足")
        final = done + want == total
        try:
            plain = aes.decrypt(_nonce(info["nonce_prefix"], index), chunk, _aad(header, index, final))
        except InvalidTag as exc:
            # 不区分"密码错"和"被篡改"：两者在 GCM 下不可区分，也避免给攻击者提示
            raise DecryptionError("GCM 校验失败：密码错误，或密文已损坏/被篡改") from exc
        if digest is not None:
            digest.update(plain)
        fout.write(plain)
        done += want
        index += 1
        if progress:
            progress(done, total)
    if fin.read(1):
        raise CorruptContainerError("容器尾部存在多余数据")
    chunks = (total + chunk_size - 1) // chunk_size if total else 0
    result = {
        "plaintext_size": total,
        "ciphertext_size": HEADER_SIZE + total + chunks * TAG_SIZE,
        "chunks": chunks,
        "iterations": info["iterations"],
        "chunk_size": chunk_size,
    }
    if digest is not None:
        result["sha256"] = digest.hexdigest()
    return result


def _encrypt_bytes(data: bytes, password: Password, *, iterations: int, chunk_size: int) -> bytes:
    import io

    out = io.BytesIO()
    _encrypt_stream(io.BytesIO(data), out, len(data), password, iterations=iterations, chunk_size=chunk_size)
    return out.getvalue()


def _decrypt_bytes(blob: bytes, password: Password) -> bytes:
    import io

    out = io.BytesIO()
    _decrypt_stream(io.BytesIO(blob), out, password)
    return out.getvalue()


# ---------------------------------------------------------------------------
# 文件级 API
# ---------------------------------------------------------------------------
def _open_temp(target: Path) -> tuple[Path, BinaryIO]:
    """在目标目录创建临时文件，返回 (路径, 已打开的写句柄)。

    安全要点：直接用 ``mkstemp`` 返回的 fd 打开文件，**不要**先建空文件再按名字
    ``open()``——后者在共享目录下存在 TOCTOU：同目录攻击者可以在两步之间把临时
    文件换成符号链接，让解密明文写到他想写的位置。
    """
    # 文件名过长时截断前缀，避免 mkstemp 因 NAME_MAX 失败
    prefix = f".{target.name[:80]}."
    fd, name = tempfile.mkstemp(prefix=prefix, suffix=".part", dir=str(target.parent))
    return Path(name), os.fdopen(fd, "wb")


def _prepare_dest(dst: Path, overwrite: bool) -> None:
    dst.parent.mkdir(parents=True, exist_ok=True)
    if dst.exists() and not overwrite:
        raise FileGuardError(f"目标文件已存在：{dst}（如需覆盖请用 --force）")


def encrypt_file(
    src: Union[str, Path],
    dst: Union[str, Path, None] = None,
    password: Password = "",
    *,
    iterations: int = DEFAULT_ITERATIONS,
    chunk_size: int = DEFAULT_CHUNK_SIZE,
    progress: Progress = None,
    overwrite: bool = False,
    hash_plaintext: bool = True,
) -> dict[str, Any]:
    """加密单个文件，默认输出 ``<原名>.fgc``。"""
    src = Path(src)
    if not src.is_file():
        raise FileGuardError(f"源文件不存在：{src}")
    dst = Path(dst) if dst else src.with_name(src.name + SUFFIX)
    if dst.resolve() == src.resolve():
        raise FileGuardError("加密输出不能覆盖源文件本身")
    _prepare_dest(dst, overwrite)
    tmp, fout = _open_temp(dst)
    size = src.stat().st_size
    try:
        with open(src, "rb") as fin, fout:
            result = _encrypt_stream(
                fin,
                fout,
                size,
                password,
                iterations=iterations,
                chunk_size=chunk_size,
                progress=progress,
                hash_plaintext=hash_plaintext,
            )
            # 读完声明的长度后必须正好到文件尾：源文件在加密过程中变长时，
            # 多出来的内容会被静默丢弃（数据丢失），这里直接报错让用户重试。
            if fin.read(1):
                raise FileGuardError("源文件在加密过程中被修改（变大），已中止，请重试")
            os.fsync(fout.fileno())
        os.replace(tmp, dst)
    except Exception:
        tmp.unlink(missing_ok=True)
        raise
    result.update({"src": str(src), "dst": str(dst)})
    return result


def decrypt_file(
    src: Union[str, Path],
    dst: Union[str, Path, None] = None,
    password: Password = "",
    *,
    progress: Progress = None,
    overwrite: bool = False,
    hash_plaintext: bool = True,
) -> dict[str, Any]:
    """解密单个容器；默认去掉 ``.fgc`` 后缀还原原文件名。"""
    src = Path(src)
    if not src.is_file():
        raise FileGuardError(f"容器不存在：{src}")
    if dst is None:
        dst = src.with_name(src.name[: -len(SUFFIX)] if src.name.endswith(SUFFIX) else src.name + ".dec")
    dst = Path(dst)
    _prepare_dest(dst, overwrite)
    tmp, fout = _open_temp(dst)
    try:
        with open(src, "rb") as fin, fout:
            result = _decrypt_stream(fin, fout, password, progress=progress, hash_plaintext=hash_plaintext)
            os.fsync(fout.fileno())
        os.replace(tmp, dst)
    except Exception:
        tmp.unlink(missing_ok=True)
        raise
    result.update({"src": str(src), "dst": str(dst)})
    return result


def verify_file(src: Union[str, Path], password: Password = "", *, progress: Progress = None) -> dict[str, Any]:
    """只校验不落盘：流式解密并计算明文 SHA-256（用于"解密后校验原文件"）。"""
    src = Path(src)
    with open(src, "rb") as fin:
        sink = _HashSink()
        result = _decrypt_stream(fin, sink, password, progress=progress, hash_plaintext=False)
        result["sha256"] = sink.hexdigest()
    result["src"] = str(src)
    return result


def rekey_file(
    src: Union[str, Path],
    dst: Union[str, Path, None],
    old_password: Password,
    new_password: Password,
    *,
    iterations: int = DEFAULT_ITERATIONS,
    chunk_size: int = DEFAULT_CHUNK_SIZE,
    overwrite: bool = False,
) -> dict[str, Any]:
    """修改密码：逐块"解密旧口令 → 用新口令重加密"，中间不落地明文。"""
    src = Path(src)
    dst = Path(dst) if dst else src
    if dst.exists() and dst.resolve() != src.resolve() and not overwrite:
        raise FileGuardError(f"目标文件已存在：{dst}")
    tmp, fout = _open_temp(dst)
    try:
        with open(src, "rb") as fin:
            raw_header = fin.read(HEADER_SIZE)
            info = _parse_header(raw_header)
            total = info["plaintext_size"]
            if (total + info["chunk_size"] - 1) // info["chunk_size"] > MAX_CHUNKS:
                raise CorruptContainerError("容器声明的分块数量异常")
            old_key = derive_key(old_password, info["salt"], info["iterations"])
            new_salt = os.urandom(SALT_SIZE)
            new_prefix = os.urandom(NONCE_PREFIX_SIZE)
            new_header = _build_header(
                iterations, info["chunk_size"], new_salt, new_prefix, total
            )
            new_key = derive_key(new_password, new_salt, iterations)
            old_aes, new_aes = AESGCM(old_key), AESGCM(new_key)
            with fout:
                fout.write(new_header)
                done = 0
                index = 0
                while done < total:
                    want = min(info["chunk_size"], total - done)
                    chunk = fin.read(want + TAG_SIZE)
                    if len(chunk) != want + TAG_SIZE:
                        raise CorruptContainerError("容器被截断")
                    final = done + want == total
                    try:
                        plain = old_aes.decrypt(
                            _nonce(info["nonce_prefix"], index), chunk, _aad(info["header"], index, final)
                        )
                    except InvalidTag as exc:
                        raise DecryptionError("旧密码错误，无法修改密码") from exc
                    fout.write(
                        new_aes.encrypt(_nonce(new_prefix, index), plain, _aad(new_header, index, final))
                    )
                    done += want
                    index += 1
                if fin.read(1):
                    # 与 decrypt 保持一致：尾部垃圾数据说明容器被篡改，不能悄悄丢掉
                    raise CorruptContainerError("容器尾部存在多余数据")
                fout.flush()
                os.fsync(fout.fileno())
        os.replace(tmp, dst)
    except Exception:
        tmp.unlink(missing_ok=True)
        raise
    return {"src": str(src), "dst": str(dst), "iterations": iterations}


class _HashSink:
    """只计算哈希的"文件对象"，用于 verify_file。"""

    def __init__(self) -> None:
        self._h = hashlib.sha256()
        self.size = 0

    def write(self, data: bytes) -> int:
        self._h.update(data)
        self.size += len(data)
        return len(data)

    def flush(self) -> None:  # 兼容 _decrypt_stream 的 fout.flush()
        pass

    def hexdigest(self) -> str:
        return self._h.hexdigest()


def is_container(path: Union[str, Path]) -> bool:
    try:
        with open(path, "rb") as f:
            return f.read(4) == MAGIC
    except OSError:
        return False


def inspect_container(path: Union[str, Path]) -> dict[str, Any]:
    """读取容器头部信息（不需要密码，用于 UI 展示）。"""
    with open(path, "rb") as f:
        info = _parse_header(f.read(HEADER_SIZE))
    info.pop("header", None)
    info.pop("salt", None)
    info.pop("nonce_prefix", None)
    info["file"] = str(path)
    info["file_size"] = Path(path).stat().st_size
    return info


# ---------------------------------------------------------------------------
# 密钥文件模式 & Windows DPAPI 密码保险箱
# ---------------------------------------------------------------------------
def load_keyfile_material(path: Union[str, Path]) -> bytes:
    """加载"本地密钥文件"：任意 ≥32 字节的二进制文件，取 SHA-256 作为口令材料。

    对应设计稿"两种模式：密码加密 / 本地密钥文件加密"。密钥文件是 256bit 随机数，
    即使被看到也无法反推；配合 PBKDF2 的盐，仍然一次一密。
    """
    data = Path(path).read_bytes()
    if len(data) < 32:
        raise PasswordError("密钥文件至少需要 32 字节随机内容")
    return hashlib.sha256(data).digest()


def generate_keyfile(path: Union[str, Path]) -> Path:
    """生成 32 字节随机密钥文件（模式二）。

    用 ``O_CREAT | O_EXCL`` 原子创建：即使两个进程同时执行 ``keygen``，也绝不会
    覆盖已存在的密钥文件（覆盖 = 旧密文永久打不开）。

    注意 Windows 上必须带 ``O_BINARY``：CRT 默认文本模式会把随机字节里的 0x0A
    翻译成 0x0D 0x0A，写出来的"32 字节密钥文件"会变成 33/34 字节且内容被改。
    """
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    flags = os.O_CREAT | os.O_EXCL | os.O_WRONLY | getattr(os, "O_BINARY", 0)
    try:
        fd = os.open(path, flags, 0o600)
    except FileExistsError as exc:
        raise FileGuardError(f"密钥文件已存在：{path}") from exc
    try:
        data = os.urandom(KEY_SIZE)
        written = os.write(fd, data)
        if written != len(data):
            raise FileGuardError("写入密钥文件不完整，请重试")
    finally:
        os.close(fd)
    return path


class DpapiVault:
    """Windows DPAPI 密码保险箱（自动/定时加密时免重复输入密码）。

    写入的是 ``CryptProtectData`` 密文，绑定当前 Windows 用户账户；
    换用户或换机器无法解开，也不会以明文形式出现在磁盘上。
    设计稿要求"密码永不明文保存"——这里保存的是操作系统加密后的密文。
    """

    MAGIC = b"FGV1\x00\x00\x00\x00"

    def __init__(self, path: Union[str, Path]) -> None:
        self.path = Path(path)

    @staticmethod
    def available() -> bool:
        return os.name == "nt"

    def store(self, password: str) -> None:
        from .fsutil import restrict_acl

        self._require_windows()
        blob = self.MAGIC + _dpapi_protect(password.encode("utf-8"))
        self.path.parent.mkdir(parents=True, exist_ok=True)
        tmp = self.path.with_suffix(".tmp")
        tmp.write_bytes(blob)
        os.replace(tmp, self.path)
        # 保险箱内容是 DPAPI 密文（换账户解不开），但仍收紧 ACL 减少暴露面
        restrict_acl(self.path)

    def load(self) -> Optional[str]:
        if not self.path.exists():
            return None
        raw = self.path.read_bytes()
        if not raw.startswith(self.MAGIC):
            raise FileGuardError("密码保险箱格式不正确")
        return _dpapi_unprotect(raw[len(self.MAGIC) :]).decode("utf-8")

    def clear(self) -> None:
        self.path.unlink(missing_ok=True)

    @staticmethod
    def _require_windows() -> None:
        if os.name != "nt":
            raise UnsupportedPlatformError("密码保险箱依赖 Windows DPAPI，仅在 Windows 可用")


def _dpapi_protect(data: bytes) -> bytes:
    import ctypes
    from ctypes import wintypes

    class DATA_BLOB(ctypes.Structure):
        _fields_ = [("cbData", wintypes.DWORD), ("pbData", ctypes.POINTER(ctypes.c_char))]

    def blob(b: bytes) -> tuple[DATA_BLOB, ctypes.Array]:
        buf = ctypes.create_string_buffer(b, len(b))
        return DATA_BLOB(len(b), ctypes.cast(buf, ctypes.POINTER(ctypes.c_char))), buf

    crypt32 = ctypes.WinDLL("crypt32", use_last_error=True)
    kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
    crypt32.CryptProtectData.argtypes = [
        ctypes.POINTER(DATA_BLOB),
        wintypes.LPCWSTR,
        ctypes.POINTER(DATA_BLOB),
        ctypes.c_void_p,
        ctypes.c_void_p,
        wintypes.DWORD,
        ctypes.POINTER(DATA_BLOB),
    ]
    crypt32.CryptProtectData.restype = wintypes.BOOL
    blob_in, keep = blob(data)
    blob_out = DATA_BLOB()
    ok = crypt32.CryptProtectData(
        ctypes.byref(blob_in), None, None, None, None, 0x1, ctypes.byref(blob_out)
    )
    if not ok:
        raise FileGuardError(f"DPAPI 加密失败（错误码 {ctypes.get_last_error()}）")
    try:
        return ctypes.string_at(blob_out.pbData, blob_out.cbData)
    finally:
        kernel32.LocalFree(blob_out.pbData)


def _dpapi_unprotect(data: bytes) -> bytes:
    import ctypes
    from ctypes import wintypes

    class DATA_BLOB(ctypes.Structure):
        _fields_ = [("cbData", wintypes.DWORD), ("pbData", ctypes.POINTER(ctypes.c_char))]

    def blob(b: bytes) -> tuple[DATA_BLOB, ctypes.Array]:
        buf = ctypes.create_string_buffer(b, len(b))
        return DATA_BLOB(len(b), ctypes.cast(buf, ctypes.POINTER(ctypes.c_char))), buf

    crypt32 = ctypes.WinDLL("crypt32", use_last_error=True)
    kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
    crypt32.CryptUnprotectData.argtypes = [
        ctypes.POINTER(DATA_BLOB),
        ctypes.c_void_p,
        ctypes.c_void_p,
        ctypes.c_void_p,
        ctypes.c_void_p,
        wintypes.DWORD,
        ctypes.POINTER(DATA_BLOB),
    ]
    crypt32.CryptUnprotectData.restype = wintypes.BOOL
    blob_in, keep = blob(data)
    blob_out = DATA_BLOB()
    ok = crypt32.CryptUnprotectData(
        ctypes.byref(blob_in), None, None, None, None, 0x1, ctypes.byref(blob_out)
    )
    if not ok:
        raise FileGuardError(f"DPAPI 解密失败（错误码 {ctypes.get_last_error()}）")
    try:
        return ctypes.string_at(blob_out.pbData, blob_out.cbData)
    finally:
        kernel32.LocalFree(blob_out.pbData)


# ---------------------------------------------------------------------------
# 便捷函数：目录一键加密（设计稿"单文件/文件夹一键加密"）
# ---------------------------------------------------------------------------
def iter_files(root: Path, *, skip_suffixes: tuple[str, ...] = (SUFFIX,)) -> list[Path]:
    """递归列出目录里的普通文件（跳过隐藏文件/已有容器/链接）。

    用 os.walk 而不是 ``Path.rglob``：Windows 上 ``os.walk`` 默认不跟符号链接，
    再用 ``is_reparse_point`` 挡掉目录联结，避免"一键加密"把范围扩到目录之外。
    """
    from .fsutil import is_reparse_point

    found: list[Path] = []
    for dirpath, dirnames, filenames in os.walk(root):
        current = Path(dirpath)
        dirnames[:] = [d for d in dirnames if not is_reparse_point(current / d)]
        for name in filenames:
            path = current / name
            if path.name.startswith(".") or path.name.endswith(skip_suffixes):
                continue
            if is_reparse_point(path):
                continue
            found.append(path)
    return sorted(found)


def container_info_json(info: dict[str, Any]) -> str:
    return json.dumps(info, ensure_ascii=False, indent=2)


__all__ = [
    "DEFAULT_CHUNK_SIZE",
    "DEFAULT_ITERATIONS",
    "HEADER_SIZE",
    "SUFFIX",
    "DpapiVault",
    "container_info_json",
    "decrypt_file",
    "derive_key",
    "encrypt_file",
    "generate_keyfile",
    "inspect_container",
    "is_container",
    "iter_files",
    "load_keyfile_material",
    "normalize_password",
    "rekey_file",
    "verify_file",
]
