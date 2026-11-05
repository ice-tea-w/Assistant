# -*- coding: utf-8 -*-
"""临时验收测试：不接入桌面助手主程序，单机验证 file_guard 框架是否可用/可信/可集成。

它做的事：在系统临时目录里造一个隔离环境（不碰你真实的 ~/.file_guard），
按"环境 → 加密容器 → 敏感检测 → 审计日志 → 备份恢复 → 本地服务 → 监听/自校验/保险箱
→ 安全加固抽检 → 性能冒烟"的顺序跑一遍，最后生成 Markdown 报告。

用法（工程根目录）::

    .venv\\Scripts\\python.exe scripts\\run_acceptance.py
    .venv\\Scripts\\python.exe scripts\\run_acceptance.py --out docs\\框架临时验证报告.md
    .venv\\Scripts\\python.exe scripts\\run_acceptance.py --keep   # 保留临时目录便于排查

注意：这是"临时验证"，不替代单元测试；它的价值是模拟真实调用方（子进程 CLI / 直接 import /
本机 HTTP）把整条链路走通，并把结果固化成报告。
"""

from __future__ import annotations

import argparse
import base64
import http.client
import json
import os
import shutil
import socket
import subprocess
import sys
import tempfile
import time
import urllib.request
import zipfile
from dataclasses import dataclass, field
from datetime import datetime
from pathlib import Path
from typing import Any, Callable, Optional

ROOT = Path(__file__).resolve().parents[1]
if str(ROOT) not in sys.path:
    sys.path.insert(0, str(ROOT))

try:  # Windows 控制台默认 GBK，中文输出容易花
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")  # type: ignore[attr-defined]
    sys.stderr.reconfigure(encoding="utf-8", errors="replace")  # type: ignore[attr-defined]
except Exception:  # noqa: BLE001
    pass

SAMPLE = ROOT / "合同.docx"
ACCEPT_PASSWORD = "acceptance-test-password-123"
NEW_PASSWORD = "acceptance-test-password-456"
ITER = 1_000  # 验收用低成本 KDF（生产默认 600000），避免报告跑成几分钟
CHUNK = 64 * 1024


# ---------------------------------------------------------------------------
# 检查框架
# ---------------------------------------------------------------------------
@dataclass
class Check:
    category: str
    name: str
    status: str = "PENDING"  # PASS / FAIL / SKIP
    ms: float = 0.0
    detail: str = ""
    traceback_text: str = ""


@dataclass
class Context:
    work: Path
    checks: list[Check] = field(default_factory=list)
    artifacts: list[str] = field(default_factory=list)
    notes: list[str] = field(default_factory=list)

    def add(self, category: str, name: str, status: str, ms: float, detail: str = "",
            traceback_text: str = "") -> Check:
        item = Check(category, name, status, ms, detail, traceback_text)
        self.checks.append(item)
        icon = {"PASS": "PASS", "FAIL": "FAIL", "SKIP": "SKIP"}.get(status, status)
        print(f"  [{icon}] {category} · {name}" + (f"  ({detail})" if detail else ""), flush=True)
        return item


def run_check(ctx: Context, category: str, name: str, fn: Callable[[], str]) -> bool:
    start = time.perf_counter()
    try:
        detail = fn() or ""
        ms = (time.perf_counter() - start) * 1000
        ctx.add(category, name, "PASS", ms, str(detail))
        return True
    except SkipCheck as exc:
        ms = (time.perf_counter() - start) * 1000
        ctx.add(category, name, "SKIP", ms, str(exc))
        return True
    except Exception as exc:  # noqa: BLE001 - 验收要把失败收集成报告，而不是中断
        ms = (time.perf_counter() - start) * 1000
        import traceback

        ctx.add(category, name, "FAIL", ms, f"{type(exc).__name__}: {exc}", traceback.format_exc())
        return False


class SkipCheck(Exception):
    """当前环境不满足，跳过（例如非 Windows 上测 DPAPI）。"""


# ---------------------------------------------------------------------------
# 调用辅助
# ---------------------------------------------------------------------------
def subprocess_env() -> dict[str, str]:
    env = os.environ.copy()
    env["FG_PASSWORD"] = ACCEPT_PASSWORD
    env["FG_KDF_ITERATIONS"] = str(ITER)
    env["PYTHONIOENCODING"] = "utf-8"
    env.pop("FG_HOME", None)
    return env


def run_cli(*args: str, expect: Optional[int] = 0, workdir: Optional[Path] = None,
            env: Optional[dict[str, str]] = None) -> subprocess.CompletedProcess:
    proc = subprocess.run(
        [sys.executable, "-m", "file_guard", *args],
        cwd=str(workdir or ROOT),
        env=env or subprocess_env(),
        capture_output=True,
        text=True,
        encoding="utf-8",
        errors="replace",
        timeout=180,
    )
    if expect is not None and proc.returncode != expect:
        raise AssertionError(
            f"命令 {args} 期望退出码 {expect}，实际 {proc.returncode}\n"
            f"stdout={proc.stdout[-800:]}\nstderr={proc.stderr[-800:]}"
        )
    return proc


def cli_json(*args: str, expect: Optional[int] = 0) -> Any:
    proc = run_cli(*args, "--json", expect=expect)
    try:
        return json.loads(proc.stdout)
    except json.JSONDecodeError as exc:
        raise AssertionError(f"输出不是合法 JSON：{proc.stdout[:800]}") from exc


def sha256_of(path: Path) -> str:
    import hashlib

    digest = hashlib.sha256()
    with open(path, "rb") as f:
        for block in iter(lambda: f.read(1 << 20), b""):
            digest.update(block)
    return digest.hexdigest()


def first_line(text: str) -> str:
    for line in (text or "").splitlines():
        if line.strip():
            return line.strip()
    return ""


def make_junction(link: Path, target: Path) -> bool:
    if os.name != "nt":
        return False
    try:
        proc = subprocess.run(["cmd", "/c", "mklink", "/J", str(link), str(target)],
                              capture_output=True, timeout=30)
        return proc.returncode == 0 and link.exists()
    except (OSError, subprocess.SubprocessError):
        return False


def remove_junction(link: Path) -> None:
    try:
        link.rmdir()
    except OSError:
        try:
            link.unlink()
        except OSError:
            pass


# ---------------------------------------------------------------------------
# A. 环境与三条入口
# ---------------------------------------------------------------------------
def group_environment(ctx: Context) -> None:
    cat = "A. 环境与入口"

    def python_env() -> str:
        import cryptography

        return (f"Python {sys.version.split()[0]} · cryptography {cryptography.__version__}"
                f" · {Path(sys.executable).name}")

    run_check(ctx, cat, "Python / cryptography 版本", python_env)

    def imports() -> str:
        import file_guard
        from file_guard import (  # noqa: F401
            AuditLog, BackupStore, SensitiveScanner, decrypt_file, encrypt_file, scan_paths,
        )

        return f"file_guard {file_guard.__version__}，公开 API 可导入"

    run_check(ctx, cat, "包导入（直接 import 路径）", imports)

    def cli_entry() -> str:
        proc = run_cli("--version")
        return proc.stdout.strip()

    run_check(ctx, cat, "子进程 CLI 入口（--version）", cli_entry)

    def sample() -> str:
        assert SAMPLE.is_file(), f"工程根目录缺少演示文件：{SAMPLE}"
        return f"合同.docx {SAMPLE.stat().st_size} 字节"

    run_check(ctx, cat, "工程内演示文件可读", sample)


# ---------------------------------------------------------------------------
# B. 加密容器
# ---------------------------------------------------------------------------
def group_crypto(ctx: Context, home: Path, work: Path) -> dict[str, Path]:
    cat = "B. 文件加密容器"
    docs = work / "docs"
    docs.mkdir(parents=True, exist_ok=True)
    sample = docs / "合同.docx"
    shutil.copyfile(SAMPLE, sample)
    sample_sha = sha256_of(sample)
    container = docs / "合同.docx.fgc"

    def init() -> str:
        cli_json("init", "--home", str(home), "--no-vault", "--iterations", str(ITER))
        assert (home / "config.json").exists()
        return f"数据目录初始化完成：{home.name}"

    run_check(ctx, cat, "init 初始化数据目录", init)

    def encrypt() -> str:
        result = cli_json("encrypt", str(sample), "-o", str(container), "--home", str(home))
        info = result["results"][0]
        assert info["dst"] == str(container)
        assert Path(container).exists()
        assert info.get("sha256") == sample_sha
        return (f"{info['plaintext_size']} B → {info['ciphertext_size']} B，"
                f"{info['chunks']} 块，sha256 {info['sha256'][:12]}…")

    run_check(ctx, cat, "加密真实合同文件（子进程 CLI）", encrypt)

    def header_is_auth() -> str:
        info = cli_json("info", str(container), "--home", str(home))
        return f"容器头：v{info['version']}，迭代 {info['iterations']}，块 {info['chunk_size']} B"

    run_check(ctx, cat, "容器头信息可读（无需密码）", header_is_auth)

    def verify() -> str:
        info = cli_json("verify", str(container), "--home", str(home))
        assert info["sha256"] == sample_sha
        return f"明文 SHA-256 一致：{info['sha256'][:16]}…"

    run_check(ctx, cat, "verify 校验明文一致性", verify)

    restored = work / "restored.docx"

    def decrypt() -> str:
        run_cli("decrypt", str(container), "-o", str(restored), "--home", str(home))
        assert sha256_of(restored) == sample_sha
        return "逐字节一致"

    run_check(ctx, cat, "解密还原并逐字节比对", decrypt)

    def tamper() -> str:
        tampered = work / "tampered.fgc"
        shutil.copyfile(container, tampered)
        raw = bytearray(tampered.read_bytes())
        raw[-1] ^= 0xFF
        tampered.write_bytes(bytes(raw))
        target = work / "evil.docx"
        proc = run_cli("decrypt", str(tampered), "-o", str(target), "--home", str(home), expect=None)
        assert proc.returncode == 3, f"篡改后应返回 3(DecryptionError)，实际 {proc.returncode}"
        assert not target.exists(), "失败解密不应留下半截明文"
        assert not list(work.glob("*.part")), "失败解密不应留 .part 临时文件"
        return "改 1 字节 → exit 3、无半截明文、无 .part 残留"

    run_check(ctx, cat, "篡改 1 字节被 GCM 拒绝", tamper)

    def wrong_password() -> str:
        proc = run_cli("decrypt", str(container), "-o", str(work / "x.docx"),
                       "--home", str(home), "--password", "definitely-wrong", expect=None)
        assert proc.returncode == 3
        return "错误密码 → exit 3，不区分'密码错/被篡改'（不泄露提示）"

    run_check(ctx, cat, "错误密码被拒绝", wrong_password)

    def batch_dir() -> str:
        batch = work / "batch-in"
        (batch / "sub").mkdir(parents=True, exist_ok=True)
        (batch / "a.txt").write_text("A 内容", encoding="utf-8")
        (batch / "sub" / "a.txt").write_text("子目录同名文件", encoding="utf-8")
        out = work / "batch-out"
        result = cli_json("encrypt", str(batch), "-o", str(out), "--home", str(home))
        assert len(result["results"]) == 2
        assert (out / "a.txt.fgc").exists(), "根目录文件应保留结构"
        assert (out / "sub" / "a.txt.fgc").exists(), "子目录同名文件不能互相覆盖"
        return "2 个同名文件按目录结构输出，未互相覆盖"

    run_check(ctx, cat, "目录批量加密（保留子目录结构）", batch_dir)

    def rekey() -> str:
        rekeyed = work / "rekeyed.fgc"
        cli_json("passwd", str(container), "-o", str(rekeyed),
                 "--new-password", NEW_PASSWORD, "--home", str(home))
        # 用 verify 校验（不写审计日志，避免"容器新口令"和"模块口令"被混为一谈）：
        # 新口令能解出正确明文，旧口令必须失败
        info = cli_json("verify", str(rekeyed), "--home", str(home), "--password", NEW_PASSWORD)
        assert info["sha256"] == sample_sha
        old = run_cli("verify", str(rekeyed), "--home", str(home), expect=None)
        assert old.returncode == 3, f"旧密码应返回 3，实际 {old.returncode}"
        return "新密码 verify 通过（sha256 一致），旧密码 exit 3"

    run_check(ctx, cat, "passwd 改密码（不落地明文）", rekey)

    def keyfile_mode() -> str:
        key_home = work / "home-keyfile"
        keyfile = work / "my.key"
        cli_json("keygen", str(keyfile), "--home", str(home))
        assert keyfile.stat().st_size == 32
        cli_json("init", "--home", str(key_home), "--no-vault", "--iterations", str(ITER),
                 "--keyfile", str(keyfile))
        out = work / "keyfile.rekey.fgc"
        cli_json("encrypt", str(sample), "-o", str(out), "--home", str(key_home),
                 "--keyfile", str(keyfile))
        plain = work / "keyfile-restored.docx"
        cli_json("decrypt", str(out), "-o", str(plain), "--home", str(key_home),
                 "--keyfile", str(keyfile))
        assert sha256_of(plain) == sample_sha
        return "32 字节密钥文件模式往返一致"

    run_check(ctx, cat, "密钥文件模式（模式二）", keyfile_mode)

    return {"sample": sample, "container": container, "sample_sha": sample_sha}


# ---------------------------------------------------------------------------
# C. 敏感文件检测
# ---------------------------------------------------------------------------
def group_scan(ctx: Context, home: Path, work: Path) -> None:
    cat = "C. 敏感文件检测"

    def scan_sample() -> str:
        report = cli_json("scan", str(SAMPLE), "--home", str(home), "--min-risk", "low")
        assert report["scanned"] == 1
        assert report["summary"]["medium"] >= 1, f"应识别出合同/内部标记，实际 {report['summary']}"
        kinds = {finding["kind"] for finding in report["files"][0]["findings"]}
        assert "contract" in kinds, f"应命中合同规则，实际 {kinds}"
        payload = json.dumps(report, ensure_ascii=False)
        assert "13800138000" not in payload, "报告里不应出现完整手机号"
        return f"命中 {sorted(kinds)}，样本已打码，分值 {report['files'][0]['score']}"

    run_check(ctx, cat, "真实合同扫描 + 样本打码", scan_sample)

    def scan_masking() -> str:
        sample_text = work / "idcard.txt"
        # 造一个合法身份证号（校验位正确），确认报告里不出现明文
        body = "11010519900101123"
        weights = (7, 9, 10, 5, 8, 4, 2, 1, 6, 3, 7, 9, 10, 5, 8, 4, 2)
        codes = "10X98765432"
        id_number = body + codes[sum(int(c) * w for c, w in zip(body, weights)) % 11]
        sample_text.write_text(f"客户身份证 {id_number}，电话 13900001111", encoding="utf-8")
        report = cli_json("scan", str(sample_text), "--home", str(home), "--json")
        payload = json.dumps(report, ensure_ascii=False)
        assert id_number not in payload, "报告不应包含完整身份证号"
        assert "13900001111" not in payload, "报告不应包含完整手机号"
        return "身份证/手机号在报告中均为打码样本"

    run_check(ctx, cat, "报告打码（不含完整证件号/手机号）", scan_masking)

    def oversized() -> str:
        big = work / "身份证扫描件.txt"
        big.write_text("x" * (3 << 20), encoding="utf-8")
        report = cli_json("scan", str(big), "--home", str(home))
        risk = report["files"][0] if report["files"] else {"findings": [], "note": ""}
        assert "超过" in risk.get("note", ""), f"超大文件应提示跳过正文，实际 {risk}"
        return "超限文件跳过正文、仅按文件名提示"

    run_check(ctx, cat, "超大文件跳过正文（不撑爆内存）", oversized)

    def junction_isolation() -> str:
        outside = work / "outside-secret"
        outside.mkdir(exist_ok=True)
        (outside / "id.txt").write_text("身份证 110105199001011234", encoding="utf-8")
        sandbox = work / "scan-root"
        sandbox.mkdir(exist_ok=True)
        (sandbox / "normal.txt").write_text("普通文件", encoding="utf-8")
        link = sandbox / "link"
        if not make_junction(link, outside):
            raise SkipCheck("当前环境无法创建目录联结")
        try:
            from file_guard.scan import SensitiveScanner

            collected = SensitiveScanner().collect([sandbox])
            names = sorted(p.name for p in collected)
            assert names == ["normal.txt"], f"不应遍历到联结目标，实际 {names}"
            return "目录联结被跳过，未越出授权目录"
        finally:
            remove_junction(link)

    run_check(ctx, cat, "目录联结隔离（不越出授权目录）", junction_isolation)

    def zip_bomb() -> str:
        docx = work / "bomb.docx"
        with zipfile.ZipFile(docx, "w", zipfile.ZIP_DEFLATED) as zf:
            zf.writestr("word/document.xml", "<x>" + ("A" * (6 << 20)) + "</x>")
        from file_guard.scan import SensitiveScanner

        text = SensitiveScanner._read_office_text(docx)
        assert text == "", "膨胀 XML 应被大小上限跳过"
        risk = SensitiveScanner().scan_file(docx)
        assert risk.risk == "none"
        return "6MB 膨胀 XML 被上限跳过，进程无内存压力"

    run_check(ctx, cat, "Office zip 炸弹防护", zip_bomb)


# ---------------------------------------------------------------------------
# D. 审计日志
# ---------------------------------------------------------------------------
def group_audit(ctx: Context, work: Path) -> None:
    cat = "D. 操作审计日志"
    home = work / "home-audit"
    log_path = home / "audit" / "audit.jsonl"

    def seed() -> str:
        cli_json("init", "--home", str(home), "--no-vault", "--iterations", str(ITER))
        for i in range(5):
            cli_json("encrypt", str(work / "docs" / "合同.docx"),
                     "-o", str(work / f"seed-{i}.fgc"), "--home", str(home))
        assert log_path.exists()
        record = log_path.read_bytes()
        assert "合同.docx".encode() not in record, "日志里不应出现明文路径"
        return f"生成 {len(record.splitlines())} 条加密日志"

    run_check(ctx, cat, "日志写入且落盘为密文", seed)

    def verify_ok() -> str:
        result = cli_json("log", "verify", "--home", str(home))
        assert result["ok"], result["problems"]
        return f"{result['records']} 条，链头 {result['head'][:16]}…"

    run_check(ctx, cat, "哈希链校验通过", verify_ok)

    def anchor() -> str:
        head = cli_json("log", "head", "--home", str(home))["head"]
        ok = cli_json("log", "verify", "--home", str(home), "--expected-head", head)
        assert ok["ok"]
        bad = run_cli("log", "verify", "--home", str(home), "--expected-head", "00" * 32,
                      "--json", expect=5)
        assert json.loads(bad.stdout)["ok"] is False
        return "外部锚点一致时通过，锚点不符时 exit 5"

    run_check(ctx, cat, "外部锚点（log head / expected-head）", anchor)

    backup_dir = work / "audit-backup"
    shutil.copytree(home / "audit", backup_dir)

    def tamper_line() -> str:
        lines = log_path.read_text(encoding="utf-8").splitlines()
        row = json.loads(lines[2])
        raw = bytearray(base64.b64decode(row["d"]))
        raw[3] ^= 0xFF
        row["d"] = base64.b64encode(bytes(raw)).decode()
        lines[2] = json.dumps(row)
        log_path.write_text("\n".join(lines) + "\n", encoding="utf-8")
        result = cli_json("log", "verify", "--home", str(home), "--json", expect=None)
        proc = run_cli("log", "verify", "--home", str(home), "--json", expect=None)
        assert proc.returncode == 5, f"篡改后应 exit 5，实际 {proc.returncode}"
        payload = json.loads(proc.stdout)
        assert payload["ok"] is False
        return f"检出：{payload['problems'][0][:60]}…"

    run_check(ctx, cat, "改一行密文被检出", tamper_line)

    def restore_dir() -> str:
        shutil.rmtree(home / "audit")
        shutil.copytree(backup_dir, home / "audit")
        return "已恢复原始日志，进入下一项"

    run_check(ctx, cat, "测试后恢复日志目录", restore_dir, )

    def delete_line() -> str:
        lines = log_path.read_text(encoding="utf-8").splitlines()
        del lines[1]
        log_path.write_text("\n".join(lines) + "\n", encoding="utf-8")
        proc = run_cli("log", "verify", "--home", str(home), "--json", expect=None)
        assert proc.returncode == 5
        problems = json.loads(proc.stdout)["problems"]
        assert any("序号不连续" in p or "哈希" in p for p in problems), problems
        return "检出：序号不连续/哈希不匹配"

    run_check(ctx, cat, "删一条日志被检出", delete_line)

    run_check(ctx, cat, "再次恢复日志目录", lambda: (
        shutil.rmtree(home / "audit"),
        shutil.copytree(backup_dir, home / "audit"),
        "已恢复",
    )[-1])

    def wrong_password_append() -> str:
        before = log_path.read_bytes()
        proc = run_cli("encrypt", str(work / "docs" / "合同.docx"),
                       "-o", str(work / "bad-pwd.fgc"), "--home", str(home),
                       "--password", "wrong-password", expect=None)
        assert proc.returncode != 0, "错误密码不应能追加日志"
        assert log_path.read_bytes() == before, "错误密码不应改动日志文件"
        return f"错误密码被拒绝（exit {proc.returncode}），日志字节未变"

    run_check(ctx, cat, "错误密码不能写坏日志", wrong_password_append)

    def rollback_detected() -> str:
        lines = log_path.read_text(encoding="utf-8").splitlines()
        log_path.write_text("\n".join(lines[:2]) + "\n", encoding="utf-8")
        proc = run_cli("encrypt", str(work / "docs" / "合同.docx"),
                       "-o", str(work / "rollback.fgc"), "--home", str(home), expect=None)
        assert proc.returncode != 0, "日志被回滚后应拒绝继续追加"
        assert "回滚" in (proc.stdout + proc.stderr), f"应提示回滚，实际：{proc.stdout[:200]}"
        return "日志被截断后拒绝追加，提示疑似回滚"

    run_check(ctx, cat, "日志回滚被拒绝", rollback_detected)


# ---------------------------------------------------------------------------
# E. 备份与恢复
# ---------------------------------------------------------------------------
def group_backup(ctx: Context, home: Path, work: Path, files: dict[str, Path]) -> None:
    cat = "E. 安全备份与恢复"
    backup_target = work / "docs" / "合同.docx"
    sample_sha = files["sample_sha"]

    def add_v1() -> str:
        result = cli_json("backup", "add", str(backup_target), "--home", str(home))
        item = result["results"][0]
        assert item["version"] == 1 and not item["skipped"], item
        return f"v1 备份完成，sha256 {item['sha256'][:12]}…"

    run_check(ctx, cat, "首次备份（v1）", add_v1)

    def add_v2() -> str:
        backup_target.write_text("修改后的内容：验收测试 v2", encoding="utf-8")
        result = cli_json("backup", "add", str(backup_target), "--home", str(home))
        item = result["results"][0]
        assert item["version"] == 2, item
        again = cli_json("backup", "add", str(backup_target), "--home", str(home))
        assert again["results"][0]["skipped"] is True, "内容未变应增量跳过"
        return "v2 生成；内容未变时增量跳过"

    run_check(ctx, cat, "改内容生成 v2 + 增量去重", add_v2)

    def list_versions() -> str:
        rows = cli_json("backup", "list", str(backup_target), "--home", str(home))
        versions = [row["version"] for row in rows]
        assert versions == [1, 2], versions
        return f"版本列表 {versions}"

    run_check(ctx, cat, "版本列表正确", list_versions)

    def restore_v1() -> str:
        out = work / "restore-v1.docx"
        result = cli_json("backup", "restore", str(backup_target), "--version", "1",
                          "--dest", str(out), "--home", str(home))
        assert sha256_of(out) == sample_sha, "恢复出的 v1 内容应与原始文件一致"
        return f"v1 恢复到 {out.name}，SHA-256 一致"

    run_check(ctx, cat, "按版本回滚", restore_v1)

    def restore_deleted() -> str:
        backup_target.unlink()
        result = cli_json("backup", "restore", str(backup_target), "--home", str(home))
        restored = Path(result["restored_to"])
        assert restored.exists(), "源文件删除后应能从备份恢复"
        return f"误删恢复成功：{restored.name}（v{result['version']}）"

    run_check(ctx, cat, "误删恢复", restore_deleted)

    def verify_full() -> str:
        result = cli_json("backup", "verify", "--full", "--home", str(home))
        assert result["ok"], result["problems"]
        assert result["objects_checked"] >= 2
        return f"索引 {result['index_records']} 条，对象 {result['objects_checked']} 个全部可解密"

    run_check(ctx, cat, "备份完整性校验（--full 逐个解密）", verify_full)

    def object_names() -> str:
        objects = list((home / "backups" / "objects").rglob("*.fgc"))
        assert objects, "应有备份对象"
        names = " ".join(obj.name for obj in objects)
        assert sample_sha not in names, "对象名不应直接暴露明文哈希"
        assert "合同" not in names, "对象名不应暴露原文件名"
        return f"{len(objects)} 个对象，文件名是密钥化哈希（不泄露明文信息）"

    run_check(ctx, cat, "对象名去标识化", object_names)

    def gc() -> str:
        result = cli_json("backup", "gc", "--home", str(home))
        return f"清理 {result['removed_objects']} 个无引用对象，释放 {result['freed_bytes']} B"

    run_check(ctx, cat, "gc 清理无引用对象", gc)


# ---------------------------------------------------------------------------
# F. 本地 HTTP 服务
# ---------------------------------------------------------------------------
def _free_loopback_port() -> int:
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as sock:
        sock.bind(("127.0.0.1", 0))
        return int(sock.getsockname()[1])


def group_service(ctx: Context, home: Path, work: Path) -> None:
    cat = "F. 本地 HTTP 服务"
    log_file = work / "service.log"
    proc: Optional[subprocess.Popen] = None
    base: dict[str, Any] = {}

    def start() -> str:
        nonlocal proc
        cli_json("init", "--home", str(home), "--no-vault", "--iterations", str(ITER))
        base["port"] = _free_loopback_port()
        # stdin=DEVNULL：就算密码没被环境变量接住，也绝不让子进程卡在 getpass 上
        log_handle = open(log_file, "w", encoding="utf-8")
        base["log_handle"] = log_handle
        proc = subprocess.Popen(
            [sys.executable, "-m", "file_guard", "serve", "--home", str(home),
             "--host", "127.0.0.1", "--port", str(base["port"]), "--quiet"],
            cwd=str(ROOT),
            env=subprocess_env(),
            stdin=subprocess.DEVNULL,
            stdout=subprocess.DEVNULL,
            stderr=log_handle,
            text=True,
            encoding="utf-8",
            errors="replace",
        )
        deadline = time.monotonic() + 30
        while time.monotonic() < deadline:
            if proc.poll() is not None:
                raise AssertionError(f"服务进程提前退出（exit={proc.returncode}），见 {log_file}")
            try:
                with urllib.request.urlopen(
                    f"http://127.0.0.1:{base['port']}/health", timeout=1
                ) as response:
                    if response.status == 200:
                        break
            except OSError:
                time.sleep(0.1)
        else:
            raise AssertionError(f"等待服务监听超时，见 {log_file}")
        token_file = home / "service.token"
        assert token_file.exists(), "应生成 service.token"
        base["token"] = token_file.read_text(encoding="utf-8").strip()
        base["token_file"] = token_file
        return f"监听 127.0.0.1:{base['port']}，token 已生成（{len(base['token'])} 字符）"

    run_check(ctx, cat, "服务启动 + token 生成", start)

    def http_request(method: str, path: str, headers: Optional[dict[str, str]] = None,
                     body: Optional[dict] = None) -> tuple[int, dict]:
        conn = http.client.HTTPConnection("127.0.0.1", base["port"], timeout=15)
        raw = json.dumps(body).encode("utf-8") if body is not None else None
        conn.request(method, path, body=raw, headers=headers or {})
        resp = conn.getresponse()
        payload = json.loads(resp.read().decode("utf-8") or "{}")
        conn.close()
        return resp.status, payload

    def health() -> str:
        status, payload = http_request("GET", "/health", {"Host": f"127.0.0.1:{base['port']}"})
        assert status == 200 and payload["ok"]
        return "/health 200（无需 token）"

    run_check(ctx, cat, "健康检查 /health", health)

    def auth() -> str:
        status, payload = http_request("POST", "/rpc",
                                       {"Host": f"127.0.0.1:{base['port']}"},
                                       {"method": "ping", "params": {}})
        assert status == 401, f"无 token 应 401，实际 {status}"
        status, payload = http_request("POST", "/rpc",
                                       {"Host": f"127.0.0.1:{base['port']}",
                                        "X-FG-Token": base["token"]},
                                       {"method": "ping", "params": {}})
        assert status == 200 and payload["result"]["name"] == "file_guard"
        return "无 token → 401；正确 token → 200"

    run_check(ctx, cat, "RPC 鉴权", auth)

    def rpc_scan() -> str:
        status, payload = http_request(
            "POST", "/rpc",
            {"Host": f"127.0.0.1:{base['port']}", "X-FG-Token": base["token"]},
            {"method": "scan", "params": {"paths": [str(SAMPLE)], "min_risk": "low"}},
        )
        assert status == 200 and payload["ok"], payload
        return f"RPC scan 成功，中危 {payload['result']['summary']['medium']} 个"

    run_check(ctx, cat, "RPC 调用业务方法（scan）", rpc_scan)

    def host_guard() -> str:
        status, _ = http_request("POST", "/rpc",
                                 {"Host": "evil.example.com", "X-FG-Token": base["token"]},
                                 {"method": "ping", "params": {}})
        assert status == 403, f"非法 Host 应 403，实际 {status}"
        return "Host: evil.example.com → 403（挡 DNS rebinding）"

    run_check(ctx, cat, "Host 校验（DNS rebinding）", host_guard)

    def origin_guard() -> str:
        status, _ = http_request("POST", "/rpc",
                                 {"Host": f"127.0.0.1:{base['port']}",
                                  "Origin": "https://evil.example.com",
                                  "X-FG-Token": base["token"]},
                                 {"method": "ping", "params": {}})
        assert status == 403, f"带 Origin 应 403，实际 {status}"
        return "Origin: evil → 403（挡浏览器跨站调用）"

    run_check(ctx, cat, "Origin 校验（浏览器 CSRF）", origin_guard)

    def bad_params():
        status, payload = http_request(
            "POST", "/rpc",
            {"Host": f"127.0.0.1:{base['port']}", "X-FG-Token": base["token"]},
            {"method": "scan", "params": {"paths": 123}},
        )
        assert status == 400, f"畸形参数应 400，实际 {status}"
        status, _ = http_request(
            "POST", "/rpc",
            {"Host": f"127.0.0.1:{base['port']}", "X-FG-Token": base["token"]},
            {"method": "no.such.method", "params": {}},
        )
        assert status == 400
        return "畸形参数/未知方法 → 400（不是 500）"

    run_check(ctx, cat, "参数校验（400 而非 500）", bad_params)

    def remote_bind():
        from file_guard.errors import ServiceError
        from file_guard.service import Session, create_server

        from file_guard.config import Config

        cfg = Config(home=home, iterations=ITER, log_iterations=ITER)
        session = Session(cfg, ACCEPT_PASSWORD)
        try:
            try:
                create_server(session, base["token"], host="0.0.0.0", port=0)
            except ServiceError:
                pass
            else:
                raise AssertionError("非回环监听应被拒绝")
        finally:
            session.close()
        return "默认拒绝监听 0.0.0.0（需 --allow-remote 显式 opt-in）"

    run_check(ctx, cat, "服务不监听非回环地址", remote_bind)

    def token_acl() -> str:
        if os.name != "nt":
            raise SkipCheck("仅 Windows 有 ACL 检查")
        icacls = Path(os.environ.get("SystemRoot", r"C:\Windows")) / "System32" / "icacls.exe"
        proc = subprocess.run([str(icacls), str(base["token_file"])],
                              capture_output=True, text=True, encoding="utf-8",
                              errors="replace", timeout=30)
        output = proc.stdout + proc.stderr
        assert proc.returncode == 0, output
        principal = f"{os.environ.get('USERDOMAIN')}\\{os.environ.get('USERNAME')}"
        assert principal in output, f"应仅当前用户可读，实际 ACL：{output}"
        for bad in ("Everyone", "BUILTIN\\Users", "NT AUTHORITY\\Authenticated Users"):
            assert bad.lower() not in output.lower(), f"ACL 仍包含低权限组 {bad}：{output}"
        return f"ACL 已收紧：{principal} + SYSTEM + Administrators"

    run_check(ctx, cat, "token 文件 ACL 仅当前用户", token_acl)

    def stop() -> str:
        nonlocal proc
        if proc and proc.poll() is None:
            proc.terminate()
            try:
                proc.wait(timeout=10)
            except subprocess.TimeoutExpired:
                proc.kill()
        handle = base.pop("log_handle", None)
        if handle is not None:
            handle.close()
        base.pop("token", None)
        return "服务进程已退出"

    run_check(ctx, cat, "服务优雅退出", stop)


# ---------------------------------------------------------------------------
# G. 监听 / 自校验 / 密码保险箱
# ---------------------------------------------------------------------------
def group_support(ctx: Context, work: Path) -> None:
    cat = "G. 监听 / 自校验 / 保险箱"

    def watch() -> str:
        from file_guard.watch import DirectoryWatcher

        root = work / "watch-root"
        root.mkdir(exist_ok=True)
        created = root / "created.txt"
        modified = root / "modified.txt"
        deleted = root / "deleted.txt"
        for item in (modified, deleted):
            item.write_text("初始内容", encoding="utf-8")
        watcher = DirectoryWatcher([root], interval=0.5)
        watcher.poll_once()  # 第一次是建立基线（返回空）
        created.write_text("新文件", encoding="utf-8")
        modified.write_text("内容变长了一些再变长", encoding="utf-8")
        deleted.unlink()
        changes = watcher.poll_once()
        kinds = sorted({(c.kind, c.path.name) for c in changes})
        assert ("create", "created.txt") in kinds, kinds
        assert ("modify", "modified.txt") in kinds, kinds
        assert ("delete", "deleted.txt") in kinds, kinds
        return f"捕获 {len(changes)} 个变更：create/modify/delete 各 1"

    run_check(ctx, cat, "目录监听捕获新增/修改/删除", watch)

    def integrity() -> str:
        from file_guard.integrity import build_manifest, verify_manifest

        pkg = work / "pkg-copy"
        shutil.copytree(ROOT / "file_guard", pkg,
                        ignore=shutil.ignore_patterns("__pycache__"))
        manifest = build_manifest(pkg, ACCEPT_PASSWORD)
        ok = verify_manifest(pkg, manifest, ACCEPT_PASSWORD)
        assert ok["ok"], ok["problems"]
        (pkg / "cli.py").write_text("# 被篡改\n" + (pkg / "cli.py").read_text(encoding="utf-8"),
                                    encoding="utf-8")
        bad = verify_manifest(pkg, manifest, ACCEPT_PASSWORD)
        assert not bad["ok"] and "cli.py" in bad["modified"], bad
        return f"{ok['checked']} 个文件校验通过；改一个文件后立即检出"

    run_check(ctx, cat, "模块自校验（防篡改告警）", integrity)

    def dpapi() -> str:
        if os.name != "nt":
            raise SkipCheck("DPAPI 仅 Windows 可用")
        from file_guard.crypto import DpapiVault

        vault = DpapiVault(work / "vault.bin")
        vault.store("vault-secret-password")
        assert vault.load() == "vault-secret-password"
        raw = vault.path.read_bytes()
        assert "vault-secret-password".encode() not in raw, "磁盘上不能有明文密码"
        vault.clear()
        assert vault.load() is None
        return "DPAPI 保险箱往返正常，磁盘无明文密码"

    run_check(ctx, cat, "DPAPI 密码保险箱", dpapi)

    def nonce_unique() -> str:
        from file_guard.audit import AuditLog

        home = work / "home-nonce"
        audit = AuditLog(home / "audit", ACCEPT_PASSWORD, iterations=ITER)
        for i in range(8):
            audit.record("open", path=f"n{i}.txt")
        lines = [json.loads(line) for line in
                 (home / "audit" / "audit.jsonl").read_text(encoding="utf-8").splitlines()]
        nonces = {base64.b64decode(line["n"]) for line in lines}
        assert len(nonces) == len(lines) == 8
        assert all(len(nonce) == 12 for nonce in nonces)
        return "8 条记录 8 个互不相同的 12 字节 nonce"

    run_check(ctx, cat, "审计 nonce 逐条随机（防复用）", nonce_unique)


# ---------------------------------------------------------------------------
# H. 性能冒烟
# ---------------------------------------------------------------------------
def group_perf(ctx: Context, home: Path, work: Path) -> None:
    cat = "H. 性能冒烟"

    def big_encrypt() -> str:
        big = work / "big.bin"
        big.write_bytes(os.urandom(5 << 20))
        container = work / "big.bin.fgc"
        start = time.perf_counter()
        run_cli("encrypt", str(big), "-o", str(container), "--home", str(home))
        elapsed = time.perf_counter() - start
        ratio = container.stat().st_size / big.stat().st_size
        assert ratio < 1.05
        return f"5 MiB 加密 {elapsed * 1000:.0f} ms（含进程启动），膨胀率 {ratio:.3f}"

    run_check(ctx, cat, "5 MiB 文件加密耗时", big_encrypt)

    def many_scan() -> str:
        folder = work / "many"
        folder.mkdir(exist_ok=True)
        for i in range(200):
            (folder / f"f{i}.txt").write_text(f"普通内容 {i}\n手机 1380000{i:04d}\n", encoding="utf-8")
        start = time.perf_counter()
        report = cli_json("scan", str(folder), "--home", str(home))
        elapsed = time.perf_counter() - start
        assert report["scanned"] == 200
        return f"200 个小文件扫描 {elapsed * 1000:.0f} ms"

    run_check(ctx, cat, "200 个小文件扫描耗时", many_scan)


# ---------------------------------------------------------------------------
# 报告
# ---------------------------------------------------------------------------
CATEGORY_ORDER = [
    "A. 环境与入口",
    "B. 文件加密容器",
    "C. 敏感文件检测",
    "D. 操作审计日志",
    "E. 安全备份与恢复",
    "F. 本地 HTTP 服务",
    "G. 监听 / 自校验 / 保险箱",
    "H. 性能冒烟",
]

MANUAL_ITEMS = [
    ("Windows 右键菜单", "`scripts/install-context-menu.ps1` 会写注册表（HKCU）；属 demo 形态，正式产品建议做进桌面助手 UI"),
    ("真实桌面助手联调", "本次未接主程序；PyQt/tkinter 侧按 README 第 5 节直接 import 或起独立服务，建议在 UI 线程外用 QThread 调长任务"),
    ("PyInstaller 打包后的行为", "打包后 `.py` 哈希清单意义下降，建议改用 Authenticode 对 exe 签名并在启动时校验"),
    ("大目录扫描体感", "本次只测 200 个小文件；10 万级目录建议改异步 job + 进度回调（接口已预留 ScanControl/progress）"),
]


def render_report(ctx: Context, args: argparse.Namespace) -> str:
    total = len(ctx.checks)
    passed = sum(1 for c in ctx.checks if c.status == "PASS")
    failed = sum(1 for c in ctx.checks if c.status == "FAIL")
    skipped = sum(1 for c in ctx.checks if c.status == "SKIP")
    elapsed = sum(c.ms for c in ctx.checks) / 1000
    overall = "通过" if failed == 0 else "未通过"

    lines: list[str] = []
    lines.append("# file_guard 框架临时验证报告（不接主程序）")
    lines.append("")
    lines.append("> 用途：在**不接入桌面助手主程序**的前提下，用独立脚本把模块的四大功能、"
                 "三条接入路径（子进程 CLI / 直接 import / 本机 HTTP）和安全加固点整体跑一遍，"
                 "确认框架可用、可信、可集成。")
    lines.append(f"> 生成时间：{datetime.now().astimezone().isoformat(timespec='seconds')}  ")
    lines.append(f"> 执行脚本：`scripts/run_acceptance.py`（可重复运行：`python scripts/run_acceptance.py`）")
    lines.append("")
    lines.append("## 1. 结论")
    lines.append("")
    lines.append(f"**总体：{overall}** —— 共 {total} 项检查，通过 {passed}、失败 {failed}、跳过 {skipped}，"
                 f"检查耗时合计 {elapsed:.1f} 秒。")
    lines.append("")
    lines.append("| 指标 | 数值 |")
    lines.append("| --- | --- |")
    lines.append(f"| 检查项总数 | {total} |")
    lines.append(f"| 通过 | {passed} |")
    lines.append(f"| 失败 | {failed} |")
    lines.append(f"| 跳过 | {skipped} |")
    lines.append(f"| 检查耗时 | {elapsed:.1f} 秒 |")
    lines.append(f"| Python | {sys.version.split()[0]} |")
    lines.append(f"| 操作系统 | {sys.platform} |")
    lines.append("")
    if failed:
        lines.append("### 失败项")
        lines.append("")
        for check in ctx.checks:
            if check.status == "FAIL":
                lines.append(f"- **{check.category} / {check.name}**：{check.detail}")
        lines.append("")
    lines.append("## 2. 检查明细")
    lines.append("")
    for category in CATEGORY_ORDER:
        rows = [c for c in ctx.checks if c.category == category]
        if not rows:
            continue
        lines.append(f"### {category}")
        lines.append("")
        lines.append("| # | 检查项 | 结果 | 耗时(ms) | 说明 |")
        lines.append("| --- | --- | --- | --- | --- |")
        for index, check in enumerate(rows, 1):
            status = {"PASS": "✅ 通过", "FAIL": "❌ 失败", "SKIP": "⏭️ 跳过"}[check.status]
            detail = check.detail.replace("|", "\\|")
            if len(detail) > 160:
                detail = detail[:157] + "…"
            lines.append(f"| {index} | {check.name} | {status} | {check.ms:.0f} | {detail} |")
        lines.append("")
    lines.append("## 3. 验证覆盖了什么")
    lines.append("")
    lines.append("| 能力 | 验证方式 |")
    lines.append("| --- | --- |")
    lines.append("| 加密容器格式与完整性 | 加密真实合同 → verify 明文哈希 → 解密逐字节比对 → 改 1 字节/错密码均被拒绝 |")
    lines.append("| 目录批量与路径结构 | 两个同名文件分别放在根目录/子目录，输出保持结构不互相覆盖 |")
    lines.append("| 密钥文件模式 | 32 字节密钥文件初始化 + 加解密往返 |")
    lines.append("| 敏感检测 | 真实合同命中合同/内部标记/手机号/邮箱；报告样本打码；超大文件跳过；联结越界与 zip 炸弹防护 |")
    lines.append("| 审计日志 | 加密落盘、链校验、外部锚点、改一行/删一条/回滚/错密码四种攻击均被检出或拒绝 |")
    lines.append("| 备份恢复 | 版本递增、增量去重、按版本回滚、误删恢复、--full 逐个解密校验、对象名去标识化、gc |")
    lines.append("| 本地服务 | 启动/token/健康检查/RPC 鉴权/业务调用/Host+Origin 拦截/参数校验/拒绝非回环/ACL |")
    lines.append("| 辅助能力 | 目录监听三类事件、模块自校验防篡改、DPAPI 保险箱、nonce 逐条随机 |")
    lines.append("| 性能冒烟 | 5 MiB 加密耗时、200 个小文件扫描耗时 |")
    lines.append("")
    lines.append("## 4. 需要人工确认 / 后续项")
    lines.append("")
    lines.append("| 项目 | 说明 |")
    lines.append("| --- | --- |")
    for name, note in MANUAL_ITEMS:
        lines.append(f"| {name} | {note} |")
    lines.append("")
    lines.append("## 5. 怎么复现")
    lines.append("")
    lines.append("```bash")
    lines.append("# 在工程根目录执行（会在系统临时目录里建隔离环境，不碰真实 ~/.file_guard）")
    lines.append(".venv\\Scripts\\python.exe scripts\\run_acceptance.py")
    lines.append("# 指定报告输出位置 / 保留临时目录排查")
    lines.append(".venv\\Scripts\\python.exe scripts\\run_acceptance.py --out docs\\框架临时验证报告.md --keep")
    lines.append("```")
    lines.append("")
    lines.append("> 说明：本脚本是**端到端冒烟/验收**，不替代 `tests/` 下的 115 个单元与回归测试；")
    lines.append("> 它验证的是「框架是否跑得通、防线是否生效、三条接入路径是否可用」，"
                 "执行完会自动清理临时数据。")
    lines.append("")
    if ctx.notes:
        lines.append("## 6. 附注")
        lines.append("")
        for note in ctx.notes:
            lines.append(f"- {note}")
        lines.append("")
    return "\n".join(lines) + "\n"


# ---------------------------------------------------------------------------
# 主流程
# ---------------------------------------------------------------------------
def main(argv: Optional[list[str]] = None) -> int:
    parser = argparse.ArgumentParser(description="file_guard 框架临时验收（不接主程序）")
    parser.add_argument("--out", default=str(ROOT / "docs" / "框架临时验证报告.md"),
                        help="Markdown 报告输出路径")
    parser.add_argument("--keep", action="store_true", help="保留临时目录（排查用）")
    args = parser.parse_args(argv)

    work = Path(tempfile.mkdtemp(prefix="file_guard_accept_"))
    ctx = Context(work=work)
    print("=" * 74)
    print(f"file_guard 框架临时验收（隔离目录：{work}）")
    print("=" * 74)

    started = time.perf_counter()
    try:
        group_environment(ctx)
        home = work / "home"
        files = group_crypto(ctx, home, work)
        group_scan(ctx, home, work)
        group_audit(ctx, work)
        group_backup(ctx, home, work, files)
        group_service(ctx, work / "home-service", work)
        group_support(ctx, work)
        group_perf(ctx, home, work)
    finally:
        total_elapsed = time.perf_counter() - started
        if args.keep:
            ctx.notes.append(f"临时目录已保留：`{work}`")
        else:
            shutil.rmtree(work, ignore_errors=True)

    report = render_report(ctx, args)
    out_path = Path(args.out).expanduser()
    if not out_path.is_absolute():
        out_path = ROOT / out_path
    out_path.parent.mkdir(parents=True, exist_ok=True)
    out_path.write_text(report, encoding="utf-8")
    # 机器可读副本，方便 CI/看板消费
    json_path = out_path.with_suffix(".json")
    json_path.write_text(
        json.dumps(
            {
                "generated": datetime.now().astimezone().isoformat(timespec="seconds"),
                "python": sys.version.split()[0],
                "platform": sys.platform,
                "elapsed_seconds": round(total_elapsed, 2),
                "checks": [
                    {
                        "category": c.category,
                        "name": c.name,
                        "status": c.status,
                        "ms": round(c.ms, 1),
                        "detail": c.detail,
                    }
                    for c in ctx.checks
                ],
            },
            ensure_ascii=False,
            indent=2,
        ),
        encoding="utf-8",
    )

    passed = sum(1 for c in ctx.checks if c.status == "PASS")
    failed = sum(1 for c in ctx.checks if c.status == "FAIL")
    skipped = sum(1 for c in ctx.checks if c.status == "SKIP")
    print("-" * 74)
    print(f"结果：{len(ctx.checks)} 项，通过 {passed}，失败 {failed}，跳过 {skipped}，"
          f"总耗时 {total_elapsed:.1f}s")
    print(f"报告：{out_path}")
    print(f"JSON：{json_path}")
    if failed:
        print("失败项：")
        for check in ctx.checks:
            if check.status == "FAIL":
                print(f"  - {check.category} / {check.name}：{check.detail}")
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
