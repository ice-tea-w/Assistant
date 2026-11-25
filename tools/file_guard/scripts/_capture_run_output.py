# -*- coding: utf-8 -*-
"""抓取 file_guard 的真实终端输出（验收脚本 + CLI 实跑），供渲染截图使用。

产物（写入 docs/运行截图/）：
* ``01-验收原始输出.txt``      —— run_acceptance.py 的完整 stdout/stderr
* ``03-CLI实跑原始输出.txt``   —— 一串真实 CLI 命令 + 输出（带 $ 提示符）

用法（工程根目录）::

    .venv\\Scripts\\python.exe scripts\\_capture_run_output.py
"""
from __future__ import annotations

import os
import shutil
import subprocess
import sys
import tempfile
import time
from pathlib import Path

PROJECT = Path(__file__).resolve().parents[1]
PY = PROJECT / ".venv" / "Scripts" / "python.exe"
OUT = PROJECT / "docs" / "运行截图"
PASSWORD = "shot-password-123"


def run(cmd: list[str], env: dict[str, str]) -> tuple[int, str]:
    proc = subprocess.run(
        cmd,
        cwd=str(PROJECT),
        env=env,
        capture_output=True,
        text=True,
        encoding="utf-8",
        errors="replace",
        timeout=600,
    )
    return proc.returncode, (proc.stdout or "") + (proc.stderr or "")


def main() -> int:
    if not PY.exists():
        print(f"找不到工程虚拟环境：{PY}", file=sys.stderr)
        return 1
    OUT.mkdir(parents=True, exist_ok=True)
    work = Path(tempfile.mkdtemp(prefix="fg-shots-"))

    env = os.environ.copy()
    env["PYTHONIOENCODING"] = "utf-8"
    env["FG_PASSWORD"] = PASSWORD
    env["FG_KDF_ITERATIONS"] = "1000"
    env.pop("FG_HOME", None)

    try:
        # 1) 验收脚本完整输出
        started = time.time()
        code, text = run([str(PY), "-u", "scripts/run_acceptance.py"], env)
        (OUT / "01-验收原始输出.txt").write_text(text.rstrip() + "\n", encoding="utf-8")
        print(f"[capture] 验收脚本 exit={code}，{len(text.splitlines())} 行，{time.time() - started:.1f}s")

        # 2) CLI 实跑转录（带 $ 提示符，像真实终端会话）
        home = work / "home"
        run([str(PY), "-m", "file_guard", "encrypt", "合同.docx",
             "-o", str(work / "合同.docx.fgc"), "--home", str(home), "--quiet"], env)
        transcript: list[str] = []

        def record(argv: list[str]) -> None:
            shown = " ".join(
                ["python -m file_guard", argv[0]]
                + [a.replace(str(work), r".\tmp") if a.startswith(str(work)) else a for a in argv[1:]]
            )
            transcript.append("$ " + shown)
            code, out = run([str(PY), "-m", "file_guard", *argv], env)
            for line in out.rstrip().splitlines():
                transcript.append(line.replace(str(work), r".\tmp"))
            if code != 0:
                transcript.append(f"[exit code: {code}]")
            transcript.append("")

        (work / "note.txt").write_text("这是一份用于截图演示的笔记。\n", encoding="utf-8")
        for argv in (
            ["init", "--home", str(home), "--no-vault", "--iterations", "1000"],
            ["info", str(work / "合同.docx.fgc")],
            ["verify", str(work / "合同.docx.fgc"), "--home", str(home)],
            ["scan", "合同.docx", "--home", str(home), "--min-risk", "medium"],
            ["log", "list", "--limit", "5", "--home", str(home)],
            ["log", "verify", "--home", str(home)],
            ["backup", "add", str(work / "note.txt"), "--home", str(home)],
            ["backup", "list", str(work / "note.txt"), "--home", str(home)],
            ["backup", "restore", str(work / "note.txt"),
             "--dest", str(work / "note-还原.txt"), "--home", str(home)],
        ):
            record(argv)
        (OUT / "03-CLI实跑原始输出.txt").write_text(
            "\n".join(transcript).rstrip() + "\n", encoding="utf-8"
        )
        print(f"[capture] CLI 转录 {len(transcript)} 行")
        return 0
    finally:
        shutil.rmtree(work, ignore_errors=True)


if __name__ == "__main__":
    raise SystemExit(main())
