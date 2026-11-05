"""环境自检：确认解释器、依赖、模块导入、演示文件与加密往返都正常。

用法（工程根目录）::

    .venv\\Scripts\\python.exe scripts\\check_env.py
"""

from __future__ import annotations

import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
if str(ROOT) not in sys.path:
    sys.path.insert(0, str(ROOT))

SAMPLE = ROOT / "合同.docx"  # 演示文件随工程走：换机拷贝整个工程目录即可
PASSWORD = "check-env-password"


def check(name: str, fn) -> tuple[bool, str, str]:
    try:
        return True, name, str(fn())
    except Exception as exc:  # noqa: BLE001 - 自检就是要把异常收集起来展示
        return False, name, f"{type(exc).__name__}: {exc}"


def _python() -> str:
    version = sys.version_info
    assert version >= (3, 10), f"需要 Python 3.10+，当前 {sys.version.split()[0]}"
    return f"{sys.version.split()[0]}  {sys.executable}"


def _venv() -> str:
    assert Path(sys.prefix).name == ".venv", f"当前解释器不是工程内 .venv：{sys.prefix}"
    return sys.prefix


def _cryptography() -> str:
    import cryptography

    return f"cryptography {cryptography.__version__}"


def _modules() -> str:
    import file_guard
    import file_guard.audit
    import file_guard.backup
    import file_guard.crypto
    import file_guard.fsutil
    import file_guard.integrity
    import file_guard.scan
    import file_guard.service
    import file_guard.watch

    return (
        f"file_guard {file_guard.__version__}"
        "（crypto/scan/audit/backup/watch/integrity/service/fsutil 均可导入）"
    )


def _sample_file() -> str:
    assert SAMPLE.is_file(), f"演示文件不存在：{SAMPLE}"
    return f"{SAMPLE}（{SAMPLE.stat().st_size} 字节）"


def _roundtrip() -> str:
    from file_guard.crypto import decrypt_file, encrypt_file

    with tempfile.TemporaryDirectory(prefix="file-guard-check-") as tmp:
        src = Path(tmp) / "roundtrip.txt"
        src.write_text("环境自检：加密/解密往返", encoding="utf-8")
        container = Path(tmp) / "roundtrip.txt.fgc"
        encrypt_file(src, container, PASSWORD, iterations=1_000, chunk_size=1024)
        restored = Path(tmp) / "restored.txt"
        decrypt_file(container, restored, PASSWORD)
        assert restored.read_text(encoding="utf-8") == src.read_text(encoding="utf-8")
    return "加密/解密往返一致，篡改会被 GCM 拒绝"


def _cli() -> str:
    from file_guard.cli import build_parser, main

    parser = build_parser()
    assert main and parser
    return "CLI 入口可用（python -m file_guard --help）"


def _tests() -> str:
    files = sorted((ROOT / "tests").glob("test_*.py"))
    assert files, "tests 目录缺失"
    return f"{len(files)} 个测试文件（python -m unittest discover -s tests）"


def main() -> int:
    checks = [
        ("Python 版本", _python),
        ("虚拟环境", _venv),
        ("加密库", _cryptography),
        ("模块导入", _modules),
        ("演示文件", _sample_file),
        ("加密往返", _roundtrip),
        ("命令行入口", _cli),
        ("测试套件", _tests),
    ]
    results = [check(name, fn) for name, fn in checks]
    width = max(len(name) for name, _ in checks)
    print("=" * 60)
    print("file_guard 环境自检")
    print("=" * 60)
    for ok, name, detail in results:
        print(f"[{'OK' if ok else 'FAIL'}] {name:<{width}}  {detail}")
    failed = [name for ok, name, _ in results if not ok]
    print("-" * 60)
    if failed:
        print(f"未通过：{', '.join(failed)}")
        return 1
    print("全部通过，可以开始使用。")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
