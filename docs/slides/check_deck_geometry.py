# -*- coding: utf-8 -*-
"""PPT 几何自检：越界 / 文字溢出 / 同层元素重叠

用法： python check_deck_geometry.py [pptx路径 ...]
不带参数则检查当前目录下 _drafts/*.pptx

为什么需要：python-pptx 不渲染，写脚本时看不见成品。
"文字超出文本框"是这类生成脚本最典型的翻车点，靠几何估算能提前抓住。
"""
import glob
import os
import sys

from pptx import Presentation
from pptx.util import Emu

W_IN, H_IN = 13.333, 7.5
EMU = 914400.0


def inch(v):
    return None if v is None else v / EMU


def est_text_height(tf, width_in):
    """估算一个文本框实际需要的高度（英寸）。

    中文字符按 1.0 em 宽、英文数字按 0.55 em 宽估算，行高 = 字号*行距*1.22。
    """
    total = 0.0
    for p in tf.paragraphs:
        text = "".join(r.text for r in p.runs)
        sizes = [r.font.size.pt for r in p.runs if r.font.size is not None]
        size = max(sizes) if sizes else 12.0
        ls = p.line_spacing if isinstance(p.line_spacing, float) else 1.2
        before = p.space_before.pt if p.space_before is not None else 0.0
        after = p.space_after.pt if p.space_after is not None else 0.0

        if not text.strip():
            total += size * 1.22 / 72.0
            continue

        cjk = sum(1 for ch in text if ord(ch) > 0x2E80)
        other = len(text) - cjk
        text_w_pt = cjk * size + other * size * 0.55
        avail_pt = max(width_in * 72.0, 1.0)
        lines = max(1, int(text_w_pt / avail_pt) + (1 if text_w_pt % avail_pt else 0))
        total += lines * size * ls * 1.22 / 72.0
        total += (before + after) / 72.0
    return total


def check(path):
    prs = Presentation(path)
    name = os.path.basename(path)
    issues = []

    for si, slide in enumerate(prs.slides, 1):
        boxes = []
        for sh in slide.shapes:
            x, y = inch(sh.left), inch(sh.top)
            w, h = inch(sh.width), inch(sh.height)
            if None in (x, y, w, h):
                continue
            boxes.append((sh, x, y, w, h))

            # --- 越界 ---
            if x < -0.01 or y < -0.01 or x + w > W_IN + 0.01 or y + h > H_IN + 0.01:
                issues.append(
                    f"  第{si}页 越界: {sh.shape_type}, 位置=({x:.2f},{y:.2f}) "
                    f"尺寸=({w:.2f}x{h:.2f}) → 右下角=({x+w:.2f},{y+h:.2f})")

            # --- 文字溢出 ---
            if sh.has_text_frame and sh.text_frame.text.strip():
                need = est_text_height(sh.text_frame, w)
                if need > h + 0.06:
                    snippet = sh.text_frame.text.strip().replace("\n", " / ")[:34]
                    issues.append(
                        f"  第{si}页 文字可能溢出: 框高{h:.2f}\" 需要≈{need:.2f}\" "
                        f"| \"{snippet}\"")

        # --- 同层卡片重叠（白底卡片是主要内容载体，互相压住必是 bug） ---
        cards = [(sh, x, y, w, h) for (sh, x, y, w, h) in boxes
                 if sh.has_text_frame is False and w > 1.5 and h > 0.8]
        for i in range(len(cards)):
            for j in range(i + 1, len(cards)):
                _, x1, y1, w1, h1 = cards[i]
                _, x2, y2, w2, h2 = cards[j]
                ox = min(x1 + w1, x2 + w2) - max(x1, x2)
                oy = min(y1 + h1, y2 + h2) - max(y1, y2)
                if ox > 0.05 and oy > 0.05:
                    a = ox * oy
                    if a > 0.12:
                        issues.append(
                            f"  第{si}页 大块重叠: "
                            f"({x1:.2f},{y1:.2f},{w1:.2f}x{h1:.2f}) 与 "
                            f"({x2:.2f},{y2:.2f},{w2:.2f}x{h2:.2f}) "
                            f"重叠 {ox:.2f}x{oy:.2f}={a:.2f}平方英寸")

    print(f"=== {name} ({len(prs.slides._sldIdLst)} 页) ===")
    if issues:
        for it in issues:
            print(it)
        print(f"  → {len(issues)} 项待确认")
    else:
        print("  OK：无越界 / 无文字溢出 / 无大块重叠")
    return len(issues)


if __name__ == "__main__":
    args = sys.argv[1:]
    if not args:
        base = os.path.join(os.path.dirname(os.path.abspath(__file__)), "_drafts")
        args = sorted(glob.glob(os.path.join(base, "*.pptx")))
    if not args:
        print("没有找到 pptx")
        sys.exit(1)
    for p in args:
        try:
            print()
            check(p)
        except Exception as exc:                       # noqa: BLE001
            print(f"  检查失败 {p}: {exc}")
