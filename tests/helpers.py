"""文件安全模块测试公共辅助。"""

from __future__ import annotations

import os
import sys
from pathlib import Path

# 允许在未安装包的情况下直接 `python -m unittest discover -s tests`
PROJECT_ROOT = Path(__file__).resolve().parents[1]
if str(PROJECT_ROOT) not in sys.path:
    sys.path.insert(0, str(PROJECT_ROOT))

from file_guard.crypto import DEFAULT_CHUNK_SIZE  # noqa: E402

#: 测试用低成本 KDF 参数（生产默认 600000，测试 1000）
TEST_ITERATIONS = 1_000
TEST_CHUNK_SIZE = 1024
TEST_PASSWORD = "correct-horse-battery-staple"


def luhn_check_digit(prefix: str) -> str:
    """给定前 n-1 位，计算 Luhn 校验位。"""
    digits = [int(c) for c in prefix]
    # 假设最后一位为校验位，因此从右往左对已有位按"偶数位×2"规则处理
    total = 0
    for i, digit in enumerate(reversed(digits)):
        if i % 2 == 0:  # 校验位占一位，故已有位的奇偶与完整串相反
            digit *= 2
            if digit > 9:
                digit -= 9
        total += digit
    return str((10 - total % 10) % 10)


def make_bank_card(bin_prefix: str = "622202123456789") -> str:
    return bin_prefix + luhn_check_digit(bin_prefix)


def make_id_card() -> str:
    body = "11010519900101123"
    weights = (7, 9, 10, 5, 8, 4, 2, 1, 6, 3, 7, 9, 10, 5, 8, 4, 2)
    codes = "10X98765432"
    total = sum(int(c) * w for c, w in zip(body, weights))
    return body + codes[total % 11]


def tmp_home(tmpdir: str) -> Path:
    home = Path(tmpdir) / "home"
    home.mkdir(parents=True, exist_ok=True)
    os.environ.pop("FG_PASSWORD", None)
    return home


__all__ = [
    "DEFAULT_CHUNK_SIZE",
    "PROJECT_ROOT",
    "TEST_CHUNK_SIZE",
    "TEST_ITERATIONS",
    "TEST_PASSWORD",
    "luhn_check_digit",
    "make_bank_card",
    "make_id_card",
    "tmp_home",
]
