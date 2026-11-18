"""保护目录监听：把"打开/修改/删除"操作记进审计日志。

设计稿要求记录"文件打开、修改、删除、加密、解密"轨迹。Python 侧不可能
无侵入地拿到所有"打开"事件（那需要文件系统过滤驱动/ETW/Shell 扩展），
因此这里用**轮询快照**实现修改/新增/删除的可靠捕获；"打开"事件由桌面助手
在调用文件时主动 ``audit.record(EVENT_OPEN, ...)`` 上报，两者拼成完整轨迹。

生产环境可以换成 ``watchdog``（ReadDirectoryChangesW）或 Windows ETW，
接口保持 ``poll_once()`` 不变即可。
"""

from __future__ import annotations

import os
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Callable, Iterable, Optional, Union

from .audit import (
    EVENT_BACKUP,
    EVENT_CREATE,
    EVENT_DELETE,
    EVENT_MODIFY,
    AuditLog,
)
from .crypto import SUFFIX, encrypt_file, verify_file
from .errors import FileGuardError
from .fsutil import is_reparse_point


@dataclass
class Change:
    kind: str  # created / modified / deleted
    path: Path


class DirectoryWatcher:
    def __init__(
        self,
        roots: Iterable[Union[str, Path]],
        *,
        interval: float = 5.0,
        exclude_names: Optional[set[str]] = None,
        on_change: Optional[Callable[[Change], None]] = None,
    ) -> None:
        self.roots = [Path(r).expanduser() for r in roots]
        self.interval = max(0.5, interval)
        self.exclude_names = exclude_names or {"desktop.ini", "thumbs.db", ".ds_store"}
        self.on_change = on_change
        self._snapshot: dict[str, tuple[float, int]] = {}
        #: 用独立标志表示"基线已建立"。不能拿 `not self._snapshot` 判断：
        #: 目录初始为空时快照就是 {}（假值），会永远被当成首次轮询 → 空目录里
        #: 新建的文件永远检测不到。
        self._baseline_ready = False

    def _iter_files(self) -> Iterable[Path]:
        for root in self.roots:
            if not root.exists():
                continue
            if root.is_file():
                yield root
                continue
            for dirpath, dirnames, filenames in os.walk(root):
                current = Path(dirpath)
                # 跳过目录联结/符号链接：否则监听会跟着链接跑到授权目录之外
                dirnames[:] = [
                    d
                    for d in dirnames
                    if d.lower() not in {"$recycle.bin", "system volume information"}
                    and not is_reparse_point(current / d)
                ]
                for name in filenames:
                    if name.lower() in self.exclude_names or name.endswith(SUFFIX):
                        continue
                    path = current / name
                    if is_reparse_point(path):
                        continue
                    yield path

    #: 对外公开的只读遍历接口（供 auto_encrypt_idle 等复用）
    iter_files = _iter_files

    def take_snapshot(self) -> dict[str, tuple[float, int]]:
        snapshot: dict[str, tuple[float, int]] = {}
        for path in self._iter_files():
            try:
                stat = path.stat()
            except OSError:
                continue
            snapshot[str(path)] = (stat.st_mtime, stat.st_size)
        return snapshot

    def poll_once(self) -> list[Change]:
        current = self.take_snapshot()
        changes: list[Change] = []
        if not self._baseline_ready:
            self._snapshot = current
            self._baseline_ready = True
            return changes
        for path, meta in current.items():
            old = self._snapshot.get(path)
            if old is None:
                changes.append(Change(EVENT_CREATE, Path(path)))
            elif old != meta:
                changes.append(Change(EVENT_MODIFY, Path(path)))
        for path in self._snapshot:
            if path not in current:
                changes.append(Change(EVENT_DELETE, Path(path)))
        self._snapshot = current
        for change in changes:
            if self.on_change:
                self.on_change(change)
        return changes

    def run(
        self,
        audit: AuditLog,
        *,
        stop_event: Optional[object] = None,
        max_cycles: Optional[int] = None,
        on_changes: Optional[Callable[[list[Change]], None]] = None,
    ) -> int:
        """轮询并把变化写入审计日志，返回处理的变更数。

        ``on_changes`` 在每轮记录完审计后被调用，可用于"监听到改动就自动备份"等联动
        （见 :func:`auto_backup_changes`）。
        """
        cycles = 0
        total = 0
        self._snapshot = self.take_snapshot()  # 基线，避免把已有文件全部当成新增
        self._baseline_ready = True
        try:
            while True:
                if stop_event is not None and getattr(stop_event, "is_set", lambda: False)():
                    break
                changes = self.poll_once()
                for change in changes:
                    audit.record(change.kind, path=change.path, detail="目录监听到的变化")
                    total += 1
                if changes and on_changes is not None:
                    on_changes(changes)
                cycles += 1
                if max_cycles is not None and cycles >= max_cycles:
                    break
                time.sleep(self.interval)
        except KeyboardInterrupt:
            pass
        return total


def auto_encrypt_idle(
    watcher: DirectoryWatcher,
    *,
    password: Union[str, bytes],
    idle_seconds: float,
    iterations: int,
    chunk_size: int,
    audit: Optional[AuditLog] = None,
    remove_source: bool = True,
    dry_run: bool = False,
) -> list[dict]:
    """把"空闲超过 N 秒的新文件"自动加密（对应"定时/超时自动加密"）。

    只处理 watcher 根目录下的普通文件；``remove_source`` 为 True 时加密成功后
    删除明文（这是设计稿"超时自动重新加密"的简化版）。
    """
    now = time.time()
    results: list[dict] = []
    for path in list(watcher.iter_files()):
        if path.name.endswith(SUFFIX):
            continue
        try:
            stat = path.stat()
        except OSError:
            continue
        if now - stat.st_mtime < idle_seconds:
            continue
        if dry_run:
            results.append({"path": str(path), "action": "would-encrypt"})
            continue
        try:
            info = encrypt_file(path, password=password, iterations=iterations, chunk_size=chunk_size)
        except (FileGuardError, OSError) as exc:
            results.append({"path": str(path), "error": str(exc)})
            continue
        if remove_source:
            # 删明文前先真正解密校验一遍：容器写坏/密钥不对时，删源 = 数据永久丢失
            try:
                check = verify_file(info["dst"], password)
            except (FileGuardError, OSError) as exc:
                results.append(
                    {"path": str(path), "encrypted": info["dst"], "error": f"加密结果校验失败，已保留明文：{exc}"}
                )
                continue
            if check.get("sha256") and info.get("sha256") and check["sha256"] != info["sha256"]:
                results.append(
                    {"path": str(path), "encrypted": info["dst"], "error": "加密结果与源文件哈希不一致，已保留明文"}
                )
                continue
            try:
                path.unlink()
            except OSError as exc:
                results.append({"path": str(path), "encrypted": info["dst"], "error": f"删除明文失败：{exc}"})
                continue
        if audit:
            audit.record("encrypt", path=path, detail="空闲超时自动加密", container=info["dst"])
        results.append({"path": str(path), "action": "encrypted", "dst": info["dst"]})
    return results


def auto_backup_changes(
    watcher: DirectoryWatcher,
    store: "object",
    *,
    changes: Optional[list[Change]] = None,
    note: str = "监听自动备份",
    audit: Optional[AuditLog] = None,
) -> list[dict]:
    """监听到"新增/修改"就自动做一次增量备份（新增功能，对应设计稿"操作留痕 + 可恢复"）。

    * 直接用 ``BackupStore.backup_file``：内容没变会自动跳过（增量去重），不会把库撑大；
    * ``changes`` 可以不传，函数自己 ``poll_once()``；``run(on_changes=...)`` 里则把本轮
      变更列表传进来，避免重复轮询/丢事件；
    * 单个文件失败（占用、权限）只记进结果，不影响其它文件；
    * ``audit`` 传入时，真正产生新版本的文件会额外记一条 ``backup`` 审计事件。

    返回每个变更的处理结果，可直接打印或交给 UI。
    """
    pending = changes if changes is not None else watcher.poll_once()
    results: list[dict] = []
    for change in pending:
        if change.kind not in (EVENT_CREATE, EVENT_MODIFY):
            continue  # 删除事件交给审计/恢复流程，这里不做"删备份"
        path = change.path
        if not path.is_file() or path.name.endswith(SUFFIX):
            continue
        try:
            item = store.backup_file(path, note=note)
        except (FileGuardError, OSError) as exc:
            results.append({"path": str(path), "event": change.kind, "error": str(exc)})
            continue
        skipped = bool(item.get("skipped"))
        results.append(
            {
                "path": str(path),
                "event": change.kind,
                "skipped": skipped,
                "version": item.get("version"),
                "created": item.get("created", False),
            }
        )
        if audit is not None and not skipped:
            audit.record(EVENT_BACKUP, path=path, detail=f"监听自动备份 v{item.get('version')}")
    return results
