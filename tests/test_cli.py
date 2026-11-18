"""命令行端到端测试：init / encrypt / decrypt / scan / log / backup / integrity / 密钥文件。"""

from __future__ import annotations

import json
import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

from helpers import PROJECT_ROOT, TEST_PASSWORD, make_id_card

CLI_PASSWORD = "cli-test-password-123"


class CliTest(unittest.TestCase):
    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory()
        self.root = Path(self._tmp.name)
        self.home = self.root / "home"
        self.work = self.root / "work"
        self.work.mkdir()
        self.src = self.work / "客户名单.txt"
        self.src.write_text(
            f"姓名,身份证号,手机号\n张三,{make_id_card()},13812345678\n甲方：某某公司\n",
            encoding="utf-8",
        )
        self.env = os.environ.copy()
        self.env.pop("FG_HOME", None)
        self.env["FG_PASSWORD"] = CLI_PASSWORD
        self.env["PYTHONIOENCODING"] = "utf-8"

    def tearDown(self) -> None:
        self._tmp.cleanup()

    # ---- 执行辅助 -------------------------------------------------
    def run_cli(self, *args: str, expect: int = 0, env: dict | None = None) -> subprocess.CompletedProcess:
        run_env = dict(self.env)
        if env:
            run_env.update(env)
        proc = subprocess.run(
            [sys.executable, "-m", "file_guard", *args],
            cwd=str(PROJECT_ROOT),
            env=run_env,
            capture_output=True,
            text=True,
            encoding="utf-8",
            timeout=180,
        )
        self.assertEqual(
            proc.returncode,
            expect,
            msg=f"命令 {args} 返回 {proc.returncode}\nstdout={proc.stdout}\nstderr={proc.stderr}",
        )
        return proc

    def run_json(self, *args: str, expect: int = 0, env: dict | None = None) -> dict:
        proc = self.run_cli(*args, expect=expect, env=env)
        try:
            return json.loads(proc.stdout)
        except json.JSONDecodeError as exc:
            self.fail(f"输出不是合法 JSON：{exc}\nstdout={proc.stdout}\nstderr={proc.stderr}")

    # ---- 测试 -----------------------------------------------------
    def test_help_and_version(self) -> None:
        self.assertIn("file_guard", self.run_cli("--version").stdout)
        self.assertIn("加密", self.run_cli("--help").stdout)

    def test_full_workflow(self) -> None:
        # 1) 初始化
        init = self.run_json("init", "--home", str(self.home), "--no-vault", "--iterations", "1000", "--json")
        self.assertTrue((self.home / "config.json").exists())
        self.assertFalse(init["vault"])

        # 2) 加密
        encrypted = self.run_json("encrypt", str(self.src), "--home", str(self.home), "--json")
        container = Path(encrypted["results"][0]["dst"])
        self.assertTrue(container.exists())
        self.assertEqual(container.suffix, ".fgc")
        self.assertTrue(self.src.exists())  # 默认保留原文件

        # 3) 容器信息 + 校验
        info = self.run_json("info", str(container), "--home", str(self.home), "--json")
        self.assertEqual(info["plaintext_size"], self.src.stat().st_size)
        verify = self.run_json("verify", str(container), "--home", str(self.home), "--json")
        self.assertEqual(verify["sha256"], encrypted["results"][0]["sha256"])

        # 4) 解密（原文件存在 → 需要 --force）
        self.run_cli("decrypt", str(container), "--home", str(self.home), expect=1)
        decrypted = self.run_json("decrypt", str(container), "--home", str(self.home), "--force", "--json")
        self.assertEqual(Path(decrypted["results"][0]["dst"]).read_text(encoding="utf-8"), self.src.read_text(encoding="utf-8"))

        # 5) 错误密码 → 退出码 3（DecryptionError）
        self.run_cli(
            "decrypt", str(container), "--home", str(self.home), "--force", "--password", "wrong-password",
            expect=3,
        )

        # 6) 扫描
        report = self.run_json("scan", str(self.work), "--home", str(self.home), "--json")
        self.assertEqual(report["summary"]["high"], 1)
        self.assertTrue(report["files"][0]["findings"])

        # 7) 审计日志
        log = self.run_json("log", "list", "--home", str(self.home), "--json", "--limit", "50")
        events = {row["event"] for row in log}
        self.assertTrue({"encrypt", "decrypt", "scan"} <= events, events)
        self.assertTrue(self.run_json("log", "verify", "--home", str(self.home), "--json")["ok"])

        # 8) 备份 + 版本回滚
        self.src.write_text("版本二", encoding="utf-8")
        backup = self.run_json("backup", "add", str(self.src), "--home", str(self.home), "--json")
        self.assertFalse(backup["results"][0]["skipped"])
        versions = self.run_json("backup", "list", str(self.src), "--home", str(self.home), "--json")
        self.assertEqual(len(versions), 1)
        restore = self.run_json(
            "backup", "restore", str(self.src), "--version", "1", "--force", "--home", str(self.home), "--json"
        )
        self.assertEqual(Path(restore["restored_to"]).read_text(encoding="utf-8"), "版本二")
        self.assertTrue(self.run_json("backup", "verify", "--home", str(self.home), "--json")["ok"])

        # 9) 模块完整性
        self.run_json("integrity", "init", "--home", str(self.home), "--json")
        check = self.run_json("integrity", "check", "--home", str(self.home), "--json")
        self.assertTrue(check["ok"], check["problems"])

    def test_keyfile_mode(self) -> None:
        # 密钥文件模式用独立数据目录：审计日志/配置都是按"一份口令材料"加密的，
        # 同一个 home 里混用密码和密钥文件会被拒绝（否则旧实现会静默写坏日志）。
        keyfile = self.root / "my.key"
        self.run_json("keygen", str(keyfile), "--home", str(self.home), "--json")
        self.assertEqual(keyfile.stat().st_size, 32)
        home = self.root / "home-keyfile"
        home.mkdir()
        self.run_cli(
            "init", "--home", str(home), "--no-vault", "--iterations", "1000", "--keyfile", str(keyfile)
        )
        self.run_json("encrypt", str(self.src), "--home", str(home), "--keyfile", str(keyfile), "--json")
        container = self.src.with_name(self.src.name + ".fgc")
        # 不给密钥文件 → 密码环境变量不匹配，解密失败
        self.run_cli("decrypt", str(container), "--home", str(home), "--force", expect=3)
        out = self.root / "restored.txt"
        result = self.run_json(
            "decrypt", str(container), "--home", str(home), "--keyfile", str(keyfile),
            "--out", str(out), "--force", "--json",
        )
        self.assertEqual(out.read_text(encoding="utf-8"), self.src.read_text(encoding="utf-8"))
        del result

    def test_scan_out_file_and_min_risk(self) -> None:
        self.run_cli("init", "--home", str(self.home), "--no-vault", "--iterations", "1000")
        report_path = self.root / "report.json"
        proc = self.run_cli(
            "scan", str(self.work), "--home", str(self.home), "--min-risk", "medium",
            "--out", str(report_path), "--quiet",
        )
        self.assertEqual(proc.stdout.strip(), "")
        payload = json.loads(report_path.read_text(encoding="utf-8"))
        self.assertEqual(payload["summary"]["high"], 1)
        self.assertIn("duration_seconds", payload)

    def test_log_clear_and_head(self) -> None:
        self.run_cli("init", "--home", str(self.home), "--no-vault", "--iterations", "1000")
        head = self.run_json("log", "head", "--home", str(self.home), "--json")
        self.assertEqual(len(head["head"]), 64)
        self.run_cli("log", "clear", "--yes", "--home", str(self.home))
        cleared = self.run_json("log", "verify", "--home", str(self.home), "--json")
        self.assertTrue(cleared["ok"])
        self.assertEqual(cleared["records"], 0)
        archived = list((self.home / "audit").glob("audit-*.jsonl"))
        self.assertEqual(len(archived), 1)

    def test_encrypt_directory(self) -> None:
        self.run_cli("init", "--home", str(self.home), "--no-vault", "--iterations", "1000")
        (self.work / "sub").mkdir()
        (self.work / "sub" / "b.txt").write_text("b", encoding="utf-8")
        result = self.run_json("encrypt", str(self.work), "--home", str(self.home), "--json")
        self.assertEqual(len(result["results"]), 2)  # 客户名单.txt + sub/b.txt
        self.assertTrue((self.work / "sub" / "b.txt.fgc").exists())


if __name__ == "__main__":
    unittest.main(verbosity=2)
