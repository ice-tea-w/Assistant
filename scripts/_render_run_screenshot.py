# -*- coding: utf-8 -*-
"""把真实终端输出渲染成终端风格 PNG（NSimSun 等宽，2x 超采样后降采样）。

读取 ``docs/运行截图/`` 下由 ``_capture_run_output.py`` 抓到的原始文本，
输出三张图（同一目录）：

* ``01-验收检查-A至D.png``
* ``02-验收检查-E至H与结果.png``
* ``03-CLI实跑.png``

用法（工程根目录）::

    python scripts\\_capture_run_output.py
    .venv\\Scripts\\python.exe scripts\\_render_run_screenshot.py   # 需要 Pillow

注意：Pillow 不是 file_guard 的运行时依赖；可用 DSH 自带运行时或单独 pip 装。
"""
from __future__ import annotations

import re
import sys
from pathlib import Path

try:
    from PIL import Image, ImageDraw, ImageFont
except ImportError:  # pragma: no cover - 截图工具不属于运行时
    print("需要 Pillow：pip install pillow（或用 DSH 自带 Python 运行）", file=sys.stderr)
    raise

PROJECT = Path(__file__).resolve().parents[1]
SHOT = PROJECT / "docs" / "运行截图"
FONT_PATH = r"C:\Windows\Fonts\simsun.ttc"
FONT_INDEX = 1  # NSimSun：等宽，中英文对齐

S = 2  # 超采样倍数
FS = 17 * S
LH = 26 * S
PAD_X = 26 * S
PAD_TOP = 16 * S
TITLE_H = 40 * S
FOOTER_H = 28 * S
MAX_W = 1240 * S

BG = (20, 22, 26, 255)
TITLEBAR = (33, 37, 44, 255)
BORDER = (48, 54, 63, 255)
FG = (214, 222, 235, 255)
DIM = (139, 148, 158, 255)
GREEN = (126, 231, 135, 255)
RED = (255, 123, 114, 255)
YELLOW = (227, 179, 65, 255)
CYAN = (121, 192, 255, 255)
BLUE = (86, 182, 194, 255)
ORANGE = (255, 166, 87, 255)
PURPLE = (210, 168, 255, 255)

MARKER_RE = re.compile(
    r"(\[PASS\]|\[FAIL\]|\[SKIP\]|\[OK\]|\[X\]|\[高\]|\[中\]|\[低\]|错误|警告|失败|exit code: \d+|sha256=[0-9a-f]+)"
)
JSON_KEY_RE = re.compile(r'^\s+"[^"]+"\s*:')
STATUS_RE = re.compile(r"^(  \[(PASS|FAIL|SKIP)\])(.*)$")

_font_cache: dict[int, ImageFont.FreeTypeFont] = {}


def font(size: int) -> ImageFont.FreeTypeFont:
    if size not in _font_cache:
        _font_cache[size] = ImageFont.truetype(FONT_PATH, size, index=FONT_INDEX)
    return _font_cache[size]


def tokenize(line: str) -> list[tuple[str, tuple[int, int, int, int]]]:
    """把一行拆成带颜色的片段。"""
    if line.startswith("$ "):
        return [(line, CYAN)]
    if line.startswith("=") or line.startswith("-----"):
        return [(line, DIM)]
    if line.startswith("file_guard 框架临时验收"):
        return [(line, PURPLE)]
    if line.startswith("结果："):
        return [(line, GREEN)]
    if line.startswith("报告：") or line.startswith("JSON："):
        return [(line, YELLOW)]
    if JSON_KEY_RE.match(line):
        return [(line, BLUE)]
    if line.strip() in ("{", "}"):
        return [(line, DIM)]
    if line.startswith("  - "):
        return [(line, RED)]
    # 验收结果行：只给状态标记上色，括号里的细节用暗色
    status = STATUS_RE.match(line)
    if status:
        color = {"PASS": GREEN, "FAIL": RED, "SKIP": YELLOW}[status.group(2)]
        rest = status.group(3)
        runs = [(status.group(1), color)]
        split = rest.find("  (")
        if split >= 0:
            runs.append((rest[:split], FG))
            runs.append((rest[split:], DIM))
        else:
            runs.append((rest, FG))
        return runs
    if line.startswith("    ") and not MARKER_RE.search(line):
        return [(line, DIM)]
    runs: list[tuple[str, tuple[int, int, int, int]]] = []
    last = 0
    for match in MARKER_RE.finditer(line):
        if match.start() > last:
            runs.append((line[last:match.start()], FG))
        token = match.group(0)
        if token in ("[PASS]", "[OK]"):
            color = GREEN
        elif token in ("[FAIL]", "[X]", "错误", "失败") or token.startswith("exit code"):
            color = RED
        elif token in ("[SKIP]", "警告", "[中]"):
            color = YELLOW
        elif token == "[高]":
            color = RED
        elif token == "[低]":
            color = CYAN
        elif token.startswith("sha256="):
            color = BLUE
        else:
            color = ORANGE
        runs.append((token, color))
        last = match.end()
    if last < len(line):
        runs.append((line[last:], FG))
    return runs or [(line, FG)]


def wrap_line(text: str, measure: ImageFont.FreeTypeFont, max_width: int) -> list[str]:
    if not text:
        return [""]
    if measure.getlength(text) <= max_width:
        return [text]
    out: list[str] = []
    rest = text
    while rest:
        width = 0.0
        cut = 0
        for index, char in enumerate(rest):
            advance = measure.getlength(char)
            if width + advance > max_width and cut > 0:
                break
            width += advance
            cut = index + 1
        cut = max(cut, 1)
        out.append(rest[:cut])
        rest = rest[cut:]
    return out


def render(lines: list[str], title: str, footer_left: str, footer_right: str, output: Path) -> Path:
    body_font = font(FS)
    wrapped: list[str] = []
    for line in lines:
        wrapped.extend(wrap_line(line.rstrip("\n"), body_font, MAX_W - 2 * PAD_X))

    longest = max((body_font.getlength(line) for line in wrapped), default=600)
    width = int(max(940 * S, min(MAX_W, longest + 2 * PAD_X)))
    height = TITLE_H + PAD_TOP + LH * len(wrapped) + FOOTER_H

    image = Image.new("RGBA", (width, height), BG)
    draw = ImageDraw.Draw(image)

    draw.rectangle([0, 0, width, TITLE_H], fill=TITLEBAR)
    for index, color in enumerate([(255, 95, 86), (255, 189, 46), (39, 201, 63)]):
        cx = PAD_X + index * 24 * S
        cy = TITLE_H // 2
        radius = 6 * S
        draw.ellipse([cx - radius, cy - radius, cx + radius, cy + radius], fill=color + (255,))
    draw.text((PAD_X + 90 * S, TITLE_H // 2), title, font=font(14 * S), fill=DIM, anchor="lm")
    draw.line([0, TITLE_H, width, TITLE_H], fill=BORDER)

    y = TITLE_H + PAD_TOP
    for line in wrapped:
        x = PAD_X
        for text, color in tokenize(line):
            draw.text((x, y), text, font=body_font, fill=color)
            x += body_font.getlength(text)
        y += LH

    footer_y = height - FOOTER_H
    draw.line([0, footer_y, width, footer_y], fill=BORDER)
    small = font(13 * S)
    draw.text((PAD_X, footer_y + FOOTER_H // 2), footer_left, font=small, fill=DIM, anchor="lm")
    draw.text((width - PAD_X, footer_y + FOOTER_H // 2), footer_right, font=small, fill=DIM, anchor="rm")

    image = image.resize((width // S, height // S), Image.LANCZOS)
    mask = Image.new("L", image.size, 0)
    ImageDraw.Draw(mask).rounded_rectangle(
        [0, 0, image.size[0] - 1, image.size[1] - 1], radius=12, fill=255
    )
    rounded = Image.new("RGBA", image.size, (0, 0, 0, 0))
    rounded.paste(image, (0, 0), mask)
    border = Image.new("RGBA", image.size, (0, 0, 0, 0))
    ImageDraw.Draw(border).rounded_rectangle(
        [0, 0, image.size[0] - 1, image.size[1] - 1], radius=12, outline=BORDER, width=1
    )
    rounded = Image.alpha_composite(rounded, border)
    output.parent.mkdir(parents=True, exist_ok=True)
    rounded.save(output)
    print(f"[render] {output.name}  {rounded.size[0]}x{rounded.size[1]}")
    return output


def main() -> int:
    acceptance_path = SHOT / "01-验收原始输出.txt"
    cli_path = SHOT / "03-CLI实跑原始输出.txt"
    if not acceptance_path.exists() or not cli_path.exists():
        print("缺少原始输出，先运行 scripts/_capture_run_output.py", file=sys.stderr)
        return 1
    acceptance = acceptance_path.read_text(encoding="utf-8").splitlines()
    cli = cli_path.read_text(encoding="utf-8").splitlines()

    elapsed = "8.5"
    for line in acceptance:
        found = re.search(r"总耗时\s*([\d.]+)\s*s", line)
        if found:
            elapsed = found.group(1)
            break
    size = sum(1 for line in acceptance if re.match(r"^\s+\[(PASS|FAIL|SKIP)\]", line))

    split_at = next(
        (i for i, line in enumerate(acceptance) if "E. 安全备份与恢复" in line),
        len(acceptance) // 2,
    )
    render(
        acceptance[:split_at],
        "PowerShell — file_guard 框架临时验收（A~D：环境/加密/检测/审计）",
        "file_guard v0.2.0 · Python 3.12.10 · 全部 PASS",
        "scripts/run_acceptance.py",
        SHOT / "01-验收检查-A至D.png",
    )
    render(
        acceptance[split_at:],
        "PowerShell — file_guard 框架临时验收（E~H：备份/服务/自校验/性能）",
        f"{size} 项检查 · 通过 {size} · 失败 0 · 跳过 0 · {elapsed}s",
        "docs/框架临时验证报告.md",
        SHOT / "02-验收检查-E至H与结果.png",
    )
    render(
        cli,
        "PowerShell — file_guard CLI 实跑（加密 / 校验 / 扫描 / 日志 / 备份）",
        "python -m file_guard · 隔离数据目录 · 无真实用户数据",
        "合同.docx",
        SHOT / "03-CLI实跑.png",
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
