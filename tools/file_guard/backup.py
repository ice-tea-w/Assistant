"""安全备份与恢复（业务层：备份）。

对应设计稿"安全备份与恢复"和"数据存储方案"：

* 增量备份：内容哈希相同则跳过，不重复占空间；
* 备份包本身加密：复用 FGC1 容器格式，密文落盘，文件名/路径不进明文索引；
* 误删可恢复：索引按"原始绝对路径 + 版本号"组织，源文件删除后仍可还原；
* 支持历史版本回滚、完整性校验、按保留策略清理；
* 默认存本地，是否上云由用户自己决定（本模块不联网）。

目录结构::

    <backup_root>/
        index.jsonl        # 加密 + HMAC 哈希链的索引（每行一条事件）
        index.meta.json    # 索引的盐/迭代次数/随机 nonce 前缀 + 高水位
        objects/ab/<keyed-hash>.fgc

对象命名用**密钥化哈希**：``HMAC(子密钥, SHA-256(明文))``，而不是明文 SHA-256。
明文哈希当文件名会泄露"某内容是否在备份里"（可确认、可离线暴力猜低熵文件、可定向
删对象）；密钥化后仍然按内容去重，但不知道密码就算不出对象名。旧备份里的对象名
是明文 SHA-256，继续按索引里记录的名字读取，不影响恢复。
"""

from __future__ import annotations

import hashlib
import hmac
import os
import re
from dataclasses import asdict, dataclass
from datetime import datetime
from pathlib import Path
from typing import Any, Iterable, Optional, Union

from .audit import DEDUP_LABEL, EncryptedLog
from .crypto import (
    DEFAULT_CHUNK_SIZE,
    DEFAULT_ITERATIONS,
    SUFFIX,
    Password,
    decrypt_file,
    encrypt_file,
    verify_file,
)
from .errors import FileGuardError, IntegrityError
from .fsutil import is_reparse_point

EVENT_BACKUP = "backup"
EVENT_PURGE = "purge"

#: 索引里的对象相对路径必须长这样，防止被篡改/导入的索引把恢复写到别处
_OBJECT_REL_RE = re.compile(r"^[0-9a-f]{2}/[0-9a-f]{64}\.fgc$")
_HEX64_RE = re.compile(r"^[0-9a-f]{64}$")


@dataclass
class BackupEntry:
    path: str
    version: int
    sha256: str
    size: int
    mtime: float
    object: str
    ts: str
    note: str = ""

    def to_dict(self) -> dict[str, Any]:
        return asdict(self)


class BackupStore:
    def __init__(
        self,
        root: Union[str, Path],
        password: Password,
        *,
        iterations: int = DEFAULT_ITERATIONS,
        chunk_size: int = DEFAULT_CHUNK_SIZE,
        log_iterations: int = 200_000,
        keep_versions: int = 0,
        fsync: bool = False,
    ) -> None:
        self.root = Path(root)
        self.objects_dir = self.root / "objects"
        self.objects_dir.mkdir(parents=True, exist_ok=True)
        self.password = password
        self.iterations = iterations
        self.chunk_size = chunk_size
        self.keep_versions = keep_versions
        self.fsync = fsync
        self.index = EncryptedLog(
            self.root, password, name="index", iterations=log_iterations, fsync=fsync
        )
        self._cache: Optional[dict[str, list[BackupEntry]]] = None
        self._object_key_cache: Optional[bytes] = None

    # ---- 基础工具 -------------------------------------------------
    @staticmethod
    def sha256_file(path: Union[str, Path]) -> str:
        digest = hashlib.sha256()
        with open(path, "rb") as f:
            for block in iter(lambda: f.read(1 << 20), b""):
                digest.update(block)
        return digest.hexdigest()

    def _object_key(self) -> bytes:
        if self._object_key_cache is None:
            self._object_key_cache = self.index.subkey(DEDUP_LABEL)
        return self._object_key_cache

    def object_name(self, sha256: str) -> str:
        """内容寻址的对象名：HMAC(密钥, 明文 SHA-256)，去重但不泄露明文哈希。"""
        return hmac.new(self._object_key(), sha256.encode("ascii"), hashlib.sha256).hexdigest()

    def object_path(self, sha256: str) -> Path:
        name = self.object_name(sha256)
        return self.objects_dir / name[:2] / f"{name}{SUFFIX}"

    def _object_rel(self, sha256: str) -> str:
        name = self.object_name(sha256)
        return f"{name[:2]}/{name}{SUFFIX}"

    def _legacy_object_path(self, sha256: str) -> Path:
        """旧版本用明文 SHA-256 命名对象；新写入不再使用，但读取要兼容。"""
        return self.objects_dir / sha256[:2] / f"{sha256}{SUFFIX}"

    def _locate_object(self, sha256: str) -> tuple[str, Path]:
        """返回 (索引里的相对路径, 绝对路径)：已有旧对象就复用，避免重复占空间。"""
        legacy = self._legacy_object_path(sha256)
        if legacy.exists():
            return legacy.relative_to(self.objects_dir).as_posix(), legacy
        return self._object_rel(sha256), self.objects_dir / self._object_rel(sha256)

    def _safe_object_path(self, rel: str) -> Path:
        """把索引里的相对路径还原成绝对路径，并校验它没有跑出 objects/。"""
        if not _OBJECT_REL_RE.match(str(rel)):
            raise IntegrityError(f"备份索引里的对象路径非法：{rel!r}")
        path = (self.objects_dir / rel).resolve()
        if not path.is_relative_to(self.objects_dir.resolve()):
            raise IntegrityError(f"备份对象路径越界：{rel!r}")
        return path

    # ---- 索引重放（带缓存，避免目录备份时 O(n²) 重放）-----------------
    @staticmethod
    def _entry_from_row(row: dict[str, Any]) -> BackupEntry:
        try:
            return BackupEntry(
                path=str(row["path"]),
                version=int(row["version"]),
                sha256=str(row["sha256"]),
                size=int(row.get("size", 0)),
                mtime=float(row.get("mtime", 0)),
                object=str(row["object"]),
                ts=str(row.get("ts", "")),
                note=str(row.get("note", "")),
            )
        except (KeyError, TypeError, ValueError) as exc:
            raise IntegrityError(f"备份索引条目字段损坏：{exc}") from exc

    @classmethod
    def _apply_row(cls, current: dict[str, list[BackupEntry]], row: dict[str, Any]) -> None:
        event = row.get("event")
        if event not in (EVENT_BACKUP, EVENT_PURGE):
            return
        path = str(row.get("path"))
        try:
            version = int(row.get("version"))
        except (TypeError, ValueError) as exc:
            raise IntegrityError("备份索引条目版本号损坏") from exc
        if event == EVENT_PURGE:
            versions = current.get(path)
            if versions:
                kept = [entry for entry in versions if entry.version != version]
                if kept:
                    current[path] = kept
                else:
                    current.pop(path, None)
            return
        entry = cls._entry_from_row(row)
        versions = current.setdefault(path, [])
        versions.append(entry)
        versions.sort(key=lambda item: item.version)

    def _entries(self) -> list[dict[str, Any]]:
        return self.index.query(limit=10**9)

    def _current_entries(self) -> dict[str, list[BackupEntry]]:
        """把索引重放成 {原始路径: 仍有效的版本列表}（排除已清理版本）。"""
        if self._cache is None:
            cache: dict[str, list[BackupEntry]] = {}
            for row in self._entries():
                self._apply_row(cache, row)
            self._cache = cache
        return self._cache

    def _append_index(self, row: dict[str, Any]) -> None:
        self.index.append(row)
        if self._cache is not None:
            self._apply_row(self._cache, row)

    # ---- 备份 -----------------------------------------------------
    def backup_file(self, path: Union[str, Path], *, note: str = "") -> dict[str, Any]:
        path = Path(path).resolve()
        if not path.is_file():
            raise FileGuardError(f"要备份的文件不存在：{path}")
        stat = path.stat()
        sha = self.sha256_file(path)
        versions = self._current_entries().get(str(path), [])
        if versions and versions[-1].sha256 == sha:
            return {
                "skipped": True,
                "reason": "内容未变化（增量备份）",
                "path": str(path),
                "version": versions[-1].version,
                "sha256": sha,
            }
        # 保留策略下版本号会变小，用 max+1 而不是 len+1，避免版本号被重复使用
        version = max((entry.version for entry in versions), default=0) + 1
        rel, obj = self._locate_object(sha)
        created = False
        if not obj.exists():
            obj.parent.mkdir(parents=True, exist_ok=True)
            result = encrypt_file(
                path,
                obj,
                self.password,
                iterations=self.iterations,
                chunk_size=self.chunk_size,
                overwrite=True,
                hash_plaintext=True,
            )
            # 加密时同步计算明文哈希，才能真正发现"备份过程中源文件被改"
            if result.get("sha256") != sha:
                obj.unlink(missing_ok=True)
                raise IntegrityError("备份过程中源文件被修改，已中止（请重试）")
            created = True
        entry = BackupEntry(
            path=str(path),
            version=version,
            sha256=sha,
            size=stat.st_size,
            mtime=stat.st_mtime,
            object=rel,
            ts=datetime.now().astimezone().isoformat(timespec="seconds"),
            note=note,
        )
        self._append_index({"event": EVENT_BACKUP, **entry.to_dict()})
        pruned: list[int] = []
        if self.keep_versions > 0:
            pruned = self._apply_retention(str(path))
        return {
            "skipped": False,
            "created": created,
            "path": str(path),
            "version": entry.version,
            "sha256": sha,
            "size": entry.size,
            "object": entry.object,
            "pruned_versions": pruned,
        }

    def backup_paths(
        self, paths: Iterable[Union[str, Path]], *, recursive: bool = True, note: str = ""
    ) -> list[dict[str, Any]]:
        results: list[dict[str, Any]] = []
        for item in paths:
            item = Path(item).expanduser()
            if item.is_file():
                results.append(self.backup_file(item, note=note))
            elif item.is_dir():
                if not recursive:
                    results.append({"skipped": True, "reason": "目录未启用递归", "path": str(item)})
                    continue
                for file_path in self._iter_dir(item):
                    try:
                        results.append(self.backup_file(file_path, note=note))
                    except (FileGuardError, OSError) as exc:
                        results.append({"skipped": True, "reason": str(exc), "path": str(file_path)})
            else:
                results.append({"skipped": True, "reason": "路径不存在", "path": str(item)})
        return results

    def _iter_dir(self, root: Path) -> Iterable[Path]:
        """递归列出普通文件；跳过链接/联结，避免把备份范围扩到授权目录之外。"""
        for dirpath, dirnames, filenames in os.walk(root):
            current = Path(dirpath)
            dirnames[:] = [d for d in dirnames if not is_reparse_point(current / d)]
            for name in filenames:
                path = current / name
                if path.name.endswith(SUFFIX) or path.name.startswith("."):
                    continue
                if is_reparse_point(path):
                    continue
                yield path

    def _apply_retention(self, path: str) -> list[int]:
        versions = self._current_entries().get(path, [])
        if len(versions) <= self.keep_versions:
            return []
        pruned: list[int] = []
        for entry in versions[: len(versions) - self.keep_versions]:
            self._append_index({"event": EVENT_PURGE, "path": path, "version": entry.version})
            pruned.append(entry.version)
        return pruned

    # ---- 查询/恢复 ------------------------------------------------
    def list(self, path: Optional[Union[str, Path]] = None) -> list[dict[str, Any]]:
        current = self._current_entries()
        if path is not None:
            key = str(Path(path).expanduser().resolve())
            return [e.to_dict() for e in current.get(key, [])]
        return [entry.to_dict() for versions in current.values() for entry in versions]

    def restore(
        self,
        path: Union[str, Path],
        *,
        version: Optional[int] = None,
        dest: Optional[Union[str, Path]] = None,
        overwrite: bool = False,
    ) -> dict[str, Any]:
        key = str(Path(path).expanduser())
        current = self._current_entries()
        versions = current.get(key) or current.get(str(Path(path).resolve()))
        if not versions:
            raise FileGuardError(f"没有该文件的备份：{path}")
        if version is not None:
            versions = [e for e in versions if e.version == version]
            if not versions:
                raise FileGuardError(f"该文件没有第 {version} 个备份版本")
        entry = versions[-1]
        if not _HEX64_RE.match(entry.sha256):
            raise IntegrityError("备份索引里的内容哈希非法")
        original = Path(entry.path)
        if not original.is_absolute():
            # 索引只能由本模块写入；相对路径说明索引被伪造/来自不可信来源
            raise IntegrityError(f"备份索引里的原始路径不是绝对路径：{entry.path!r}")
        obj = self._safe_object_path(entry.object)
        if not obj.exists():
            raise FileGuardError(f"备份对象缺失：{obj}")
        target = Path(dest).expanduser() if dest else Path(entry.path)
        if target.exists() and target.is_dir():
            raise FileGuardError(f"还原目标是一个目录：{target}")
        if target.exists() and not overwrite:
            raise FileGuardError(f"还原目标已存在：{target}（如需覆盖请用 --force）")
        result = decrypt_file(
            obj,
            target,
            self.password,
            overwrite=overwrite,
            hash_plaintext=True,
        )
        if result.get("sha256") and result["sha256"] != entry.sha256:
            target.unlink(missing_ok=True)
            raise IntegrityError("还原后哈希与备份记录不一致，已删除还原文件")
        if entry.mtime:
            try:
                os.utime(target, (entry.mtime, entry.mtime))
            except OSError:
                pass
        return {
            "path": entry.path,
            "version": entry.version,
            "restored_to": str(target),
            "sha256": entry.sha256,
            "size": entry.size,
        }

    # ---- 完整性/清理 ----------------------------------------------
    def verify(self, *, full: bool = False) -> dict[str, Any]:
        """校验索引哈希链，并逐个解密校验备份对象（full=True 时）。"""
        index_check = self.index.verify()
        problems: list[str] = list(index_check.get("problems", []))
        checked = 0
        if full:
            try:
                entries = self._entries()
            except FileGuardError as exc:
                problems.append(f"索引无法解析：{exc}")
                entries = []
            seen: set[str] = set()
            for row in entries:
                if row.get("event") != EVENT_BACKUP:
                    continue
                rel = str(row.get("object", ""))
                if rel in seen:
                    continue
                seen.add(rel)
                try:
                    obj = self._safe_object_path(rel)
                except IntegrityError as exc:
                    problems.append(str(exc))
                    continue
                if not obj.exists():
                    problems.append(f"对象缺失：{rel}")
                    continue
                try:
                    info = verify_file(obj, self.password)
                except Exception as exc:  # noqa: BLE001 - 汇总所有异常给用户看
                    problems.append(f"对象损坏：{rel}（{exc}）")
                    continue
                checked += 1
                if info["sha256"] != row.get("sha256"):
                    problems.append(f"对象内容与索引不一致：{rel}")
        return {
            "ok": not problems,
            "index_records": index_check.get("records", 0),
            "objects_checked": checked,
            "problems": problems[:50],
        }

    def gc(self) -> dict[str, Any]:
        """删除索引不再引用的对象文件（先解除保留策略后调用）。"""
        referenced: set[str] = set()
        for versions in self._current_entries().values():
            for entry in versions:
                referenced.add(entry.object)
        removed = 0
        freed = 0
        for obj in self.objects_dir.rglob(f"*{SUFFIX}"):
            if is_reparse_point(obj):
                continue
            rel = obj.relative_to(self.objects_dir).as_posix()
            if rel not in referenced:
                freed += obj.stat().st_size
                obj.unlink(missing_ok=True)
                removed += 1
        for folder in sorted(self.objects_dir.rglob("*"), reverse=True):
            if folder.is_dir() and not any(folder.iterdir()):
                folder.rmdir()
        return {"removed_objects": removed, "freed_bytes": freed}

    def stats(self) -> dict[str, Any]:
        objects = [o for o in self.objects_dir.rglob(f"*{SUFFIX}") if o.is_file()]
        return {
            "root": str(self.root),
            "files": len(self._current_entries()),
            "versions": sum(len(v) for v in self._current_entries().values()),
            "objects": len(objects),
            "object_bytes": sum(o.stat().st_size for o in objects),
        }

    def export_index(self, path: Union[str, Path]) -> Path:
        return self.index.export(path)
