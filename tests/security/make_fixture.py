"""生成 C++ 移植互操作 fixture（用 Python 原型做"参考实现"）。

产物（tests/security/data/）：
  plain.bin        —— 5 KB 明文
  plain.sha256     —— 明文 SHA-256（hex）
  python_made.fgc  —— Python 加密的 FGC1 容器（iterations=1000, chunk=1024，多块）
C++ 测试用它验证：C++ 能解密 Python 容器；C++ 生成的 cpp_made.fgc 再由本脚本校验。
"""

from __future__ import annotations

import hashlib
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))

from file_guard.crypto import encrypt_file, verify_file  # noqa: E402

PASSWORD = "cpp-interop-pass"


def build() -> None:
    data_dir = Path(__file__).resolve().parent / "data"
    data_dir.mkdir(parents=True, exist_ok=True)

    text = "安全模块 C++ 互操作测试\n"
    plain = (text * 200).encode("utf-8") + bytes(range(256)) * 4
    (data_dir / "plain.bin").write_bytes(plain)
    (data_dir / "plain.sha256").write_text(hashlib.sha256(plain).hexdigest(), encoding="utf-8")

    container = data_dir / "python_made.fgc"
    if container.exists():
        container.unlink()
    encrypt_file(
        data_dir / "plain.bin",
        container,
        PASSWORD,
        iterations=1000,
        chunk_size=1024,
        hash_plaintext=True,
    )
    print(f"fixture 已生成：{data_dir}")
    print(f"  plain.bin      {len(plain)} bytes")
    print(f"  python_made.fgc {container.stat().st_size} bytes")


def check_cpp_container() -> None:
    data_dir = Path(__file__).resolve().parent / "data"
    made = data_dir / "cpp_made.fgc"
    if not made.exists():
        print("未找到 cpp_made.fgc：先运行 C++ 测试")
        raise SystemExit(2)
    result = verify_file(made, PASSWORD)
    expected = (data_dir / "plain.sha256").read_text(encoding="utf-8").strip()
    ok = result["sha256"] == expected
    print(f"Python 校验 C++ 容器：{'通过' if ok else '失败'}（sha256={result['sha256'][:16]}…）")
    raise SystemExit(0 if ok else 1)


if __name__ == "__main__":
    if len(sys.argv) > 1 and sys.argv[1] == "--check-cpp":
        check_cpp_container()
    else:
        build()
