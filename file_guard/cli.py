"""命令行入口：交互层/右键菜单/主程序调用的统一入口。

示例（在工程根目录执行；演示文件是根目录下的 ``合同.docx``）::

    python -m file_guard init
    python -m file_guard encrypt "合同.docx" --remove-source
    python -m file_guard decrypt "合同.docx.fgc"
    python -m file_guard scan "合同.docx" --min-risk medium --out report.json
    python -m file_guard log list --limit 20
    python -m file_guard backup add "合同.docx"
    python -m file_guard backup restore "合同.docx" --version 1
    python -m file_guard serve --port 8765

密码优先级：--password > --keyfile > 环境变量 FG_PASSWORD > DPAPI 密码保险箱 > 交互输入。
"""

from __future__ import annotations

import argparse
import getpass
import json
import os
import sys
import time
from pathlib import Path
from typing import Any, Optional, Sequence, Union

from . import __version__
from .audit import (
    EVENT_BACKUP,
    EVENT_DECRYPT,
    EVENT_ENCRYPT,
    EVENT_RESTORE,
    EVENT_SCAN,
    EVENT_SETTINGS,
    AuditLog,
)
from .backup import BackupStore
from .config import ENV_PASSWORD, Config
from .crypto import (
    DpapiVault,
    SUFFIX,
    decrypt_file,
    encrypt_file,
    generate_keyfile,
    inspect_container,
    is_container,
    iter_files,
    load_keyfile_material,
    normalize_password,
    rekey_file,
    verify_file,
)
from .errors import FileGuardError, PasswordError
from .integrity import build_manifest, load_manifest, save_manifest, verify_manifest
from .scan import SEVERITY_LABEL, SensitiveScanner
from .watch import DirectoryWatcher, auto_encrypt_idle

PROG = "file_guard"
PASS_HINT = "密码"


# ---------------------------------------------------------------------------
# 通用工具
# ---------------------------------------------------------------------------
def _human_size(num: int) -> str:
    value = float(num)
    for unit in ("B", "KB", "MB", "GB", "TB"):
        if value < 1024 or unit == "TB":
            return f"{value:.1f}{unit}"
        value /= 1024
    return f"{num}B"


def _load_config(args: argparse.Namespace) -> Config:
    home = Path(args.home).expanduser() if getattr(args, "home", None) else None
    cfg = Config.load(home)
    if getattr(args, "iterations", None):
        cfg.iterations = int(args.iterations)
        cfg.log_iterations = max(1_000, int(args.iterations) // 3)
    cfg.validate()  # 命令行覆盖后同样要校验范围，别让 --iterations 5 静默削弱安全强度
    return cfg


def _vault(cfg: Config) -> DpapiVault:
    return DpapiVault(cfg.vault_file)


def resolve_password(
    args: argparse.Namespace,
    cfg: Config,
    *,
    create: bool = False,
    confirm: bool = False,
    allow_vault: bool = True,
) -> Union[str, bytes]:
    keyfile = getattr(args, "keyfile", None)
    if keyfile:
        return load_keyfile_material(keyfile)
    explicit = getattr(args, "password", None)
    if explicit:
        print(
            "警告：--password 会留在命令历史与进程列表里，建议改用 FG_PASSWORD 环境变量或 --keyfile",
            file=sys.stderr,
        )
        return explicit
    env_name = getattr(args, "password_env", None) or ENV_PASSWORD
    env_value = os.environ.get(env_name)
    if env_value:
        return env_value
    vault = _vault(cfg)
    if allow_vault and not create and vault.available():
        try:
            cached = vault.load()
        except FileGuardError:
            cached = None
        if cached:
            return cached
    if not sys.stdin.isatty():
        raise PasswordError(
            f"非交互环境无法输入密码：请设置环境变量 {env_name} 或使用 --password/--keyfile"
        )
    prompt = "设置文件安全模块密码：" if create else "请输入文件安全模块密码："
    password = getpass.getpass(prompt)
    if confirm:
        again = getpass.getpass("请再次输入密码：")
        if password != again:
            raise PasswordError("两次输入的密码不一致")
    if len(password) < 8:
        print("提示：密码长度小于 8 位，建议使用更长的口令（设计稿：防暴力破解）", file=sys.stderr)
    return password


def _emit(args: argparse.Namespace, payload: Any, text: str) -> None:
    if getattr(args, "json", False):
        print(json.dumps(payload, ensure_ascii=False, indent=2))
    elif not getattr(args, "quiet", False):
        print(text)


def _make_audit(cfg: Config, password: Union[str, bytes]) -> AuditLog:
    return AuditLog(cfg.audit_dir, password, iterations=cfg.log_iterations)


def _make_backups(cfg: Config, password: Union[str, bytes]) -> BackupStore:
    return BackupStore(
        cfg.backup_dir,
        password,
        iterations=cfg.iterations,
        chunk_size=cfg.chunk_size,
        log_iterations=cfg.log_iterations,
        keep_versions=cfg.backup_keep,
    )


def _set_low_priority() -> None:
    """把当前进程降到"低于正常"优先级（Windows），对应"扫描不抢 CPU"。"""
    if os.name != "nt":
        return
    try:
        import ctypes

        BELOW_NORMAL_PRIORITY_CLASS = 0x00004000
        handle = ctypes.windll.kernel32.GetCurrentProcess()
        ctypes.windll.kernel32.SetPriorityClass(handle, BELOW_NORMAL_PRIORITY_CLASS)
    except Exception:  # noqa: BLE001 - 权限不足时静默失败
        pass


# ---------------------------------------------------------------------------
# 命令实现
# ---------------------------------------------------------------------------
def cmd_init(args: argparse.Namespace) -> int:
    cfg = _load_config(args)
    cfg.ensure_dirs()
    cfg.save()
    password = resolve_password(args, cfg, create=True, confirm=True, allow_vault=False)
    normalize_password(password)
    vault = _vault(cfg)
    stored = False
    if vault.available() and not args.no_vault:
        vault.store(password if isinstance(password, str) else password.decode("utf-8", "ignore"))
        stored = True
    audit = _make_audit(cfg, password)
    audit.record(EVENT_SETTINGS, detail="初始化文件安全模块")
    _emit(
        args,
        {"home": str(cfg.home), "vault": stored, "version": __version__},
        f"初始化完成：{cfg.home}\n密码保险箱：{'已启用（DPAPI）' if stored else '未启用'}",
    )
    return 0


def _collect_encrypt_targets(paths: Sequence[str]) -> list[tuple[Path, Optional[Path]]]:
    """返回 [(待加密文件, 它所属的授权根目录)]；根为 None 表示用户直接指定的文件。"""
    targets: list[tuple[Path, Optional[Path]]] = []
    for raw in paths:
        path = Path(raw).expanduser()
        if path.is_file():
            if path.name.endswith(SUFFIX):
                raise FileGuardError(f"已经是加密容器：{path}")
            targets.append((path, None))
        elif path.is_dir():
            for file_path in iter_files(path):
                targets.append((file_path, path))
        else:
            raise FileGuardError(f"路径不存在：{path}")
    return targets


def _iter_containers(root: Path) -> list[Path]:
    """递归列出目录下的 .fgc 容器（跳过链接/联结，避免跨出授权目录）。"""
    from .fsutil import is_reparse_point

    found: list[Path] = []
    for dirpath, dirnames, filenames in os.walk(root):
        current = Path(dirpath)
        dirnames[:] = [d for d in dirnames if not is_reparse_point(current / d)]
        for name in filenames:
            if not name.endswith(SUFFIX):
                continue
            path = current / name
            if is_reparse_point(path):
                continue
            found.append(path)
    return sorted(found)


def _resolve_outputs(
    targets: list[tuple[Path, Optional[Path]]], out: Optional[Path], *, decrypt: bool
) -> list[Optional[Path]]:
    """计算每个目标文件的输出路径。

    多文件/目录批量时把 ``--out`` 当目录，并**保留相对子目录结构**：
    旧实现把所有文件平铺到输出目录，两个不同子目录里的同名文件会互相覆盖。
    """
    if out is None:
        return [None] * len(targets)
    mirror = len(targets) > 1 or any(base is not None for _, base in targets)
    if not mirror:
        return [out]
    if out.exists() and not out.is_dir():
        raise FileGuardError(f"多文件场景下 --out 必须是目录：{out}")
    out.mkdir(parents=True, exist_ok=True)
    results: list[Optional[Path]] = []
    for path, base in targets:
        relative = path.relative_to(base) if base is not None else Path(path.name)
        name = relative.name
        if decrypt:
            name = name[: -len(SUFFIX)] if name.endswith(SUFFIX) else name
        else:
            name = name + SUFFIX
        results.append(out / relative.parent / name)
    return results


def cmd_encrypt(args: argparse.Namespace) -> int:
    cfg = _load_config(args)
    password = resolve_password(args, cfg)
    audit = _make_audit(cfg, password)
    targets = _collect_encrypt_targets(args.paths)
    if not targets:
        raise FileGuardError("没有可加密的文件")
    out = Path(args.out).expanduser() if args.out else None
    outputs = _resolve_outputs(targets, out, decrypt=False)
    results = []
    for (path, _base), dst in zip(targets, outputs):
        info = encrypt_file(
            path,
            dst,
            password,
            iterations=cfg.iterations,
            chunk_size=cfg.chunk_size,
            overwrite=args.force,
        )
        audit.record(EVENT_ENCRYPT, path=path, detail=f"生成 {info['dst']}", container=info["dst"])
        if args.remove_source:
            path.unlink()
        results.append(info)
        if not args.json and not args.quiet:
            print(f"[OK] {path} -> {info['dst']}（{_human_size(info['plaintext_size'])}）")
    _emit(args, {"results": results}, f"完成：{len(results)} 个文件已加密")
    return 0


def cmd_decrypt(args: argparse.Namespace) -> int:
    cfg = _load_config(args)
    password = resolve_password(args, cfg)
    audit = _make_audit(cfg, password)
    targets: list[tuple[Path, Optional[Path]]] = []
    for raw in args.paths:
        path = Path(raw).expanduser()
        if path.is_file():
            if not is_container(path):
                raise FileGuardError(f"不是加密容器：{path}")
            targets.append((path, None))
        elif path.is_dir():
            found = _iter_containers(path)
            if not found:
                raise FileGuardError(f"目录下没有 .fgc 容器：{path}")
            targets.extend((item, path) for item in found)
        else:
            raise FileGuardError(f"路径不存在：{path}")
    out = Path(args.out).expanduser() if args.out else None
    outputs = _resolve_outputs(targets, out, decrypt=True)
    results = []
    for (path, _base), dst in zip(targets, outputs):
        info = decrypt_file(path, dst, password, overwrite=args.force)
        audit.record(EVENT_DECRYPT, path=path, detail=f"还原到 {info['dst']}")
        if args.remove_source:
            path.unlink()
        results.append(info)
        if not args.json and not args.quiet:
            print(f"[OK] {path} -> {info['dst']}（sha256={info.get('sha256', '')[:16]}...）")
    _emit(args, {"results": results}, f"完成：{len(results)} 个文件已解密")
    return 0


def cmd_keygen(args: argparse.Namespace) -> int:
    """生成 32 字节随机密钥文件（设计稿"本地密钥文件加密"模式）。"""
    path = generate_keyfile(args.file)
    _emit(
        args,
        {"keyfile": str(path), "size": path.stat().st_size},
        f"密钥文件已生成：{path}\n请妥善保存：丢失后加密文件无法恢复（设计稿：不设后门）",
    )
    return 0


def cmd_info(args: argparse.Namespace) -> int:
    info = inspect_container(args.file)
    _emit(args, info, json.dumps(info, ensure_ascii=False, indent=2))
    return 0


def cmd_verify(args: argparse.Namespace) -> int:
    cfg = _load_config(args)
    password = resolve_password(args, cfg)
    info = verify_file(args.file, password)
    _emit(args, info, f"校验通过：{args.file}（sha256={info['sha256']}）")
    return 0


def cmd_passwd(args: argparse.Namespace) -> int:
    """修改单个加密容器的密码（不落地明文）。"""
    cfg = _load_config(args)
    old = resolve_password(args, cfg)
    new = args.new_password or os.environ.get("FG_NEW_PASSWORD")
    if not new:
        if not sys.stdin.isatty():
            raise PasswordError("请用 --new-password 或 FG_NEW_PASSWORD 提供新密码")
        new = getpass.getpass("请输入新密码：")
        again = getpass.getpass("请再次输入新密码：")
        if new != again:
            raise PasswordError("两次输入的新密码不一致")
    info = rekey_file(args.file, args.out or None, old, new, iterations=cfg.iterations)
    _emit(args, info, f"密码已更新：{info['dst']}")
    return 0


def cmd_scan(args: argparse.Namespace) -> int:
    cfg = _load_config(args)
    password = resolve_password(args, cfg)
    audit = _make_audit(cfg, password)
    if args.low_priority:
        _set_low_priority()
    progress = None
    if not args.json and not args.quiet and sys.stderr.isatty():

        def progress(done: int, total: int, current: Optional[str]) -> None:
            print(f"\r扫描中 {done}/{total} ...", end="", file=sys.stderr, flush=True)

    scanner = SensitiveScanner(
        max_file_size=args.max_size,
        workers=args.workers,
        throttle_bps=args.throttle,
        skip_paths=[cfg.home],
        progress=progress,
    )
    started = time.monotonic()
    try:
        report = scanner.scan(args.paths, min_risk=args.min_risk)
    except KeyboardInterrupt:
        print("\n已中断扫描", file=sys.stderr)
        return 130
    if progress:
        print("\r" + " " * 40 + "\r", end="", file=sys.stderr)
    duration = time.monotonic() - started
    audit.record(
        EVENT_SCAN,
        detail=f"扫描 {report.scanned} 个文件，高危 {report.summary.get('high', 0)} 个",
        roots=[str(p) for p in args.paths],
    )
    payload = report.to_dict()
    payload["duration_seconds"] = round(duration, 2)
    if args.out:
        out = Path(args.out).expanduser()
        out.parent.mkdir(parents=True, exist_ok=True)
        out.write_text(json.dumps(payload, ensure_ascii=False, indent=2), encoding="utf-8")
    if args.json:
        print(json.dumps(payload, ensure_ascii=False, indent=2))
        return 0
    if args.quiet:
        return 0
    print(f"扫描完成：{report.scanned} 个文件，用时 {duration:.1f}s，跳过 {report.skipped} 个")
    summary = report.summary
    print(
        f"风险分布：高危 {summary.get('high', 0)} / 中危 {summary.get('medium', 0)} / "
        f"低危 {summary.get('low', 0)} / 无风险 {summary.get('none', 0)}"
    )
    if report.files:
        print("\n风险文件（按分值排序，最多显示 20 条）：")
        for risk in report.files[:20]:
            labels = "、".join(sorted({f.label for f in risk.findings}))
            print(f"  [{SEVERITY_LABEL[risk.risk]}] {risk.score:>3}  {risk.path}  —— {labels}")
    else:
        print("未发现敏感信息。")
    if args.out:
        print(f"\n报告已写入：{args.out}")
    return 0


def cmd_log(args: argparse.Namespace) -> int:
    cfg = _load_config(args)
    password = resolve_password(args, cfg)
    audit = _make_audit(cfg, password)
    if args.log_action == "list":
        rows = audit.query(event=args.event, contains=args.contains, limit=args.limit)
        if args.json:
            print(json.dumps(rows, ensure_ascii=False, indent=2))
        else:
            for row in rows:
                print(f"{row.get('ts', '')}  {row.get('event', ''):<8} {row.get('path') or '-'}")
                if row.get("detail"):
                    print(f"    {row['detail']}")
    elif args.log_action == "verify":
        result = audit.verify(expected_head=args.expected_head)
        _emit(
            args,
            result,
            ("[OK] 日志完整性校验通过" if result["ok"] else "[X] 日志校验失败")
            + f"：{result['records']} 条，链头 {result['head'][:16]}..."
            + ("\n  " + "\n  ".join(result["problems"]) if result["problems"] else ""),
        )
        return 0 if result["ok"] else 5
    elif args.log_action == "head":
        head = audit.head()
        _emit(args, {"head": head}, f"{head}\n（请把该哈希抄到可信位置，用于检测日志被整体截断）")
    elif args.log_action == "export":
        path = audit.export(args.out)
        _emit(args, {"exported": str(path)}, f"已导出日志：{path}")
    elif args.log_action == "clear":
        if not args.yes:
            answer = input("确认清空审计日志？会先归档到同目录（y/N）：").strip().lower()
            if answer != "y":
                print("已取消")
                return 0
        result = audit.clear(archive=not args.no_archive)
        _emit(args, result, f"日志已清空，归档：{result['archived'] or '（无）'}")
    return 0


def cmd_backup(args: argparse.Namespace) -> int:
    cfg = _load_config(args)
    password = resolve_password(args, cfg)
    store = _make_backups(cfg, password)
    audit = _make_audit(cfg, password)
    action = args.backup_action
    if action == "add":
        results = store.backup_paths(args.paths, recursive=not args.no_recursive)
        for item in results:
            if item.get("skipped"):
                if not args.json and not args.quiet:
                    print(f"- 跳过 {item.get('path')}：{item.get('reason')}")
                continue
            audit.record(EVENT_BACKUP, path=item["path"], detail=f"备份版本 {item['version']}")
            if not args.json and not args.quiet:
                print(f"[OK] 已备份 {item['path']} -> 版本 {item['version']}（{_human_size(item['size'])}）")
        if args.json:
            print(json.dumps({"results": results}, ensure_ascii=False, indent=2))
        elif not args.quiet:
            ok = sum(1 for item in results if not item.get("skipped"))
            print(f"完成：备份 {ok} 项，跳过 {len(results) - ok} 项")
        return 0
    if action == "list":
        rows = store.list(args.path)
        if args.json:
            print(json.dumps(rows, ensure_ascii=False, indent=2))
        else:
            for row in rows:
                print(
                    f"v{row['version']:<3} {row['ts']}  {_human_size(row['size']):>9}  "
                    f"{row['sha256'][:12]}...  {row['path']}"
                )
            if not rows:
                print("暂无备份记录")
        return 0
    if action == "restore":
        result = store.restore(
            args.path,
            version=args.version,
            dest=args.dest,
            overwrite=args.force,
        )
        audit.record(EVENT_RESTORE, path=args.path, detail=f"还原版本 {result['version']} 到 {result['restored_to']}")
        _emit(args, result, f"[OK] 已还原 {result['path']}（版本 {result['version']}）-> {result['restored_to']}")
        return 0
    if action == "verify":
        result = store.verify(full=args.full)
        _emit(
            args,
            result,
            ("[OK] 备份索引与对象校验通过" if result["ok"] else "[X] 备份校验失败")
            + f"：索引 {result['index_records']} 条，校验对象 {result['objects_checked']} 个"
            + ("\n  " + "\n  ".join(result["problems"]) if result["problems"] else ""),
        )
        return 0 if result["ok"] else 5
    if action == "gc":
        result = store.gc()
        _emit(args, result, f"已清理 {result['removed_objects']} 个无用对象，释放 {_human_size(result['freed_bytes'])}")
        return 0
    if action == "stats":
        result = store.stats()
        _emit(args, result, json.dumps(result, ensure_ascii=False, indent=2))
        return 0
    raise FileGuardError(f"未知的 backup 子命令：{action}")


def cmd_watch(args: argparse.Namespace) -> int:
    cfg = _load_config(args)
    password = resolve_password(args, cfg)
    audit = _make_audit(cfg, password)
    watcher = DirectoryWatcher(args.paths, interval=args.interval)
    audit.record(EVENT_SETTINGS, detail="启动目录监听", roots=[str(p) for p in args.paths])
    if not args.json and not args.quiet:
        print(f"开始监听 {len(args.paths)} 个目录（Ctrl+C 退出）...")
    if args.auto_encrypt is not None:
        idle_seconds = args.auto_encrypt * 60
        results = auto_encrypt_idle(
            watcher,
            password=password,
            idle_seconds=idle_seconds,
            iterations=cfg.iterations,
            chunk_size=cfg.chunk_size,
            audit=audit,
            remove_source=not args.keep_source,
        )
        if not args.json and not args.quiet:
            for item in results:
                print(json.dumps(item, ensure_ascii=False))
    total = watcher.run(audit, max_cycles=args.cycles)
    _emit(args, {"changes": total}, f"监听结束，共记录 {total} 个变更")
    return 0


def cmd_serve(args: argparse.Namespace) -> int:
    from .service import Session, ensure_token, serve

    cfg = _load_config(args)
    cfg.ensure_dirs()
    password = resolve_password(args, cfg)
    token = ensure_token(cfg.home)
    session = Session(cfg, password)
    if not args.json and not args.quiet:
        print(f"监听地址：http://{args.host}:{args.port}/rpc")
        print(f"访问 token：{token}（见 {cfg.token_file}，已收紧为仅当前用户可读）")
        if args.allow_remote:
            print("警告：已允许非回环监听，局域网内其他机器在拿到 token 后可调用本模块，请谨慎")
        print("按 Ctrl+C 停止服务")
    serve(
        session,
        host=args.host,
        port=args.port,
        token=token,
        allow_remote=bool(args.allow_remote),
    )
    return 0


def cmd_integrity(args: argparse.Namespace) -> int:
    cfg = _load_config(args)
    package_root = Path(__file__).resolve().parent
    if args.integrity_action == "init":
        password = resolve_password(args, cfg)
        manifest = build_manifest(package_root, password)
        path = save_manifest(manifest, cfg.integrity_file)
        _emit(
            args,
            {"manifest": str(path), "files": len(manifest["files"])},
            f"完整性清单已生成：{path}（{len(manifest['files'])} 个文件）",
        )
        return 0
    if args.integrity_action == "check":
        password = resolve_password(args, cfg)
        manifest = load_manifest(cfg.integrity_file)
        result = verify_manifest(package_root, manifest, password)
        _emit(
            args,
            result,
            ("[OK] 模块完整性校验通过" if result["ok"] else "[X] 模块可能被篡改")
            + f"：{result['checked']} 个文件"
            + ("\n  " + "\n  ".join(result["problems"]) if result["problems"] else ""),
        )
        return 0 if result["ok"] else 5
    raise FileGuardError("未知的 integrity 子命令")


# ---------------------------------------------------------------------------
# argparse
# ---------------------------------------------------------------------------
def _common() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(add_help=False)
    parser.add_argument("--home", metavar="DIR", help="数据目录（默认 ~/.file_guard 或 FG_HOME）")
    parser.add_argument("--password", help="密码（不推荐：会留在命令历史，建议用 FG_PASSWORD）")
    parser.add_argument("--password-env", default=ENV_PASSWORD, help="读取密码的环境变量名")
    parser.add_argument("--keyfile", metavar="FILE", help="本地密钥文件（设计稿的模式二）")
    parser.add_argument("--iterations", type=int, help="PBKDF2 迭代次数（默认 600000）")
    parser.add_argument("--json", action="store_true", help="以 JSON 输出（便于主程序解析）")
    parser.add_argument("--quiet", action="store_true", help="静默模式")
    return parser


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        prog=PROG,
        description="桌面助手 · 文件安全模块（加密 / 检测 / 审计 / 备份）",
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    parser.add_argument("--version", action="version", version=f"{PROG} {__version__}")
    sub = parser.add_subparsers(dest="command", required=True)
    common = _common()

    p = sub.add_parser("init", parents=[common], help="初始化数据目录并设置密码")
    p.add_argument("--no-vault", action="store_true", help="不启用 DPAPI 密码缓存")
    p.set_defaults(func=cmd_init)

    p = sub.add_parser("encrypt", parents=[common], help="加密文件/目录（生成 .fgc 容器）")
    p.add_argument("paths", nargs="+")
    p.add_argument("-o", "--out", help="输出文件或目录")
    p.add_argument("--remove-source", action="store_true", help="加密成功后删除原文件")
    p.add_argument("--force", action="store_true", help="覆盖已存在的目标文件")
    p.set_defaults(func=cmd_encrypt)

    p = sub.add_parser("decrypt", parents=[common], help="解密 .fgc 容器")
    p.add_argument("paths", nargs="+")
    p.add_argument("-o", "--out", help="输出目录")
    p.add_argument("--remove-source", action="store_true", help="解密成功后删除容器")
    p.add_argument("--force", action="store_true", help="覆盖已存在的目标文件")
    p.set_defaults(func=cmd_decrypt)

    p = sub.add_parser("keygen", parents=[common], help="生成随机密钥文件（模式二）")
    p.add_argument("file")
    p.set_defaults(func=cmd_keygen)

    p = sub.add_parser("info", parents=[common], help="查看加密容器头部信息")
    p.add_argument("file")
    p.set_defaults(func=cmd_info)

    p = sub.add_parser("verify", parents=[common], help="校验容器完整性并输出明文哈希")
    p.add_argument("file")
    p.set_defaults(func=cmd_verify)

    p = sub.add_parser("passwd", parents=[common], help="修改某个容器的密码（重新加密）")
    p.add_argument("file")
    p.add_argument("--new-password")
    p.add_argument("-o", "--out")
    p.set_defaults(func=cmd_passwd)

    p = sub.add_parser("scan", parents=[common], help="主动扫描敏感文件（不做后台全盘扫描）")
    p.add_argument("paths", nargs="+")
    p.add_argument("--min-risk", choices=["high", "medium", "low"], default="low")
    p.add_argument("--workers", type=int, default=4)
    p.add_argument("--max-size", type=int, default=2 << 20, help="单文件最大读取字节数")
    p.add_argument("--throttle", type=int, default=0, help="限速（字节/秒），0 表示不限")
    p.add_argument("--low-priority", action="store_true", help="降低进程优先级（Windows）")
    p.add_argument("--out", help="把 JSON 报告写入文件")
    p.set_defaults(func=cmd_scan)

    p = sub.add_parser("log", parents=[common], help="审计日志：list/verify/head/export/clear")
    log_sub = p.add_subparsers(dest="log_action", required=True)
    q = log_sub.add_parser("list", parents=[common])
    q.add_argument("--event")
    q.add_argument("--contains")
    q.add_argument("--limit", type=int, default=50)
    q.set_defaults(func=cmd_log)
    q = log_sub.add_parser("verify", parents=[common])
    q.add_argument("--expected-head", help="外部锚定的链头哈希")
    q.set_defaults(func=cmd_log)
    q = log_sub.add_parser("head", parents=[common])
    q.set_defaults(func=cmd_log)
    q = log_sub.add_parser("export", parents=[common])
    q.add_argument("out")
    q.set_defaults(func=cmd_log)
    q = log_sub.add_parser("clear", parents=[common])
    q.add_argument("--yes", action="store_true")
    q.add_argument("--no-archive", action="store_true")
    q.set_defaults(func=cmd_log)

    p = sub.add_parser("backup", parents=[common], help="加密备份：add/list/restore/verify/gc/stats")
    bsub = p.add_subparsers(dest="backup_action", required=True)
    q = bsub.add_parser("add", parents=[common])
    q.add_argument("paths", nargs="+")
    q.add_argument("--no-recursive", action="store_true", help="目录不递归")
    q.set_defaults(func=cmd_backup)
    q = bsub.add_parser("list", parents=[common])
    q.add_argument("path", nargs="?")
    q.set_defaults(func=cmd_backup)
    q = bsub.add_parser("restore", parents=[common])
    q.add_argument("path")
    q.add_argument("--version", type=int)
    q.add_argument("--dest")
    q.add_argument("--force", action="store_true")
    q.set_defaults(func=cmd_backup)
    q = bsub.add_parser("verify", parents=[common])
    q.add_argument("--full", action="store_true", help="逐个解密校验备份对象")
    q.set_defaults(func=cmd_backup)
    q = bsub.add_parser("gc", parents=[common])
    q.set_defaults(func=cmd_backup)
    q = bsub.add_parser("stats", parents=[common])
    q.set_defaults(func=cmd_backup)

    p = sub.add_parser("watch", parents=[common], help="监听保护目录（记录修改/新增/删除）")
    p.add_argument("paths", nargs="+")
    p.add_argument("--interval", type=float, default=5.0)
    p.add_argument("--auto-encrypt", type=float, metavar="MIN",
                   help="空闲超过 N 分钟后自动加密（需配合密码/保险箱）")
    p.add_argument("--keep-source", action="store_true", help="自动加密后保留明文（默认删除）")
    p.add_argument("--cycles", type=int, help="只轮询 N 轮（调试用）")
    p.set_defaults(func=cmd_watch)

    p = sub.add_parser("serve", parents=[common], help="启动本地 HTTP 服务供桌面助手调用")
    p.add_argument("--host", default="127.0.0.1", help="监听地址（默认只允许回环地址）")
    p.add_argument("--port", type=int, default=8765)
    p.add_argument(
        "--allow-remote",
        action="store_true",
        help="允许监听非回环地址（默认拒绝：本模块会代用户加解密，暴露风险高）",
    )
    p.set_defaults(func=cmd_serve)

    p = sub.add_parser("integrity", parents=[common], help="模块自身哈希校验")
    isub = p.add_subparsers(dest="integrity_action", required=True)
    q = isub.add_parser("init", parents=[common])
    q.set_defaults(func=cmd_integrity)
    q = isub.add_parser("check", parents=[common])
    q.set_defaults(func=cmd_integrity)

    return parser


def main(argv: Optional[Sequence[str]] = None) -> int:
    parser = build_parser()
    args = parser.parse_args(argv)
    try:
        return int(args.func(args))
    except FileGuardError as exc:
        if getattr(args, "json", False):
            print(json.dumps({"ok": False, "error": {"type": type(exc).__name__, "message": str(exc)}}, ensure_ascii=False))
        else:
            print(f"错误：{exc}", file=sys.stderr)
        return getattr(exc, "exit_code", 1)
    except KeyboardInterrupt:
        print("\n已取消", file=sys.stderr)
        return 130
    except OSError as exc:
        print(f"系统错误：{exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
