# -*- coding: utf-8 -*-
"""PPT 对比度自检：找出深色底上看不清的文字

用法： python check_deck_contrast.py [pptx路径 ...]
不带参数则检查当前目录下 _drafts/*.pptx

为什么需要：python-pptx 不渲染，agent 看不到成品。
深色底 + 彩色小字（尤其琥珀/青绿）是这类生成脚本最隐蔽的翻车点：
肉眼在 RGB 直方图上不容易发现，但按 WCAG 公式一算就露馅。
"""
import glob
import os
import sys

from pptx import Presentation

EMU = 914400.0
W_IN, H_IN = 13.333, 7.5

# WCAG AA 门槛：正文 4.5:1；大号文字（>=18pt 或 >=14pt 粗体）3:1
AA_NORMAL = 4.5
AA_LARGE = 3.0


def inch(v):
    return None if v is None else v / EMU


def _lin(c):
    c = c / 255.0
    return c / 12.92 if c <= 0.03928 else ((c + 0.055) / 1.055) ** 2.4


def luminance(rgb):
    r, g, b = rgb
    return 0.2126 * _lin(r) + 0.7152 * _lin(g) + 0.0722 * _lin(b)


def contrast(fg, bg):
    l1, l2 = luminance(fg), luminance(bg)
    hi, lo = max(l1, l2), min(l1, l2)
    return (hi + 0.05) / (lo + 0.05)


def rgb_of(color_format):
    try:
        c = color_format.rgb
        return (c[0], c[1], c[2])
    except Exception:                                   # noqa: BLE001
        return None


def solid_fill_of(sh):
    try:
        if sh.fill.type is not None and sh.fill.type == 1:   # MSO_FILL.SOLID
            return rgb_of(sh.fill.fore_color)
    except Exception:                                   # noqa: BLE001
        pass
    return None


def shape_boxes(slide):
    out = []
    for sh in slide.shapes:
        x, y = inch(sh.left), inch(sh.top)
        w, h = inch(sh.width), inch(sh.height)
        if None in (x, y, w, h):
            continue
        out.append((sh, x, y, w, h))
    return out


def background_at(boxes, cx, cy, slide_bg):
    """找包含点 (cx,cy) 的最上层不透明实心填充，作为该点的背景色。"""
    best = slide_bg
    best_area = None
    for sh, x, y, w, h in boxes:
        if x <= cx <= x + w and y <= cy <= y + h:
            fill = solid_fill_of(sh)
            if fill is None:
                continue
            area = w * h
            if best_area is None or area < best_area:   # 取最小的包含者 = 最上层
                best, best_area = fill, area
    return best


def check(path):
    prs = Presentation(path)
    name = os.path.basename(path)
    issues = []

    for si, slide in enumerate(prs.slides, 1):
        boxes = shape_boxes(slide)

        # 整页铺底（blank() 铺的矩形）作为兜底背景
        slide_bg = (0xF7, 0xF9, 0xFC)
        for sh, x, y, w, h in boxes:
            if x <= 0.02 and y <= 0.02 and w >= W_IN - 0.05 and h >= H_IN - 0.05:
                f = solid_fill_of(sh)
                if f:
                    slide_bg = f
                    break

        for sh, x, y, w, h in boxes:
            if not sh.has_text_frame:
                continue
            for p in sh.text_frame.paragraphs:
                for run in p.runs:
                    if not run.text.strip():
                        continue
                    fg = rgb_of(run.font.color) if run.font.color is not None else None
                    if fg is None:
                        continue
                    size = run.font.size.pt if run.font.size is not None else 18.0
                    bold = bool(run.font.bold)
                    large = size >= 18 or (bold and size >= 14)
                    need = AA_LARGE if large else AA_NORMAL

                    # 用文字块中心点定位背景
                    bg = background_at(boxes, x + w / 2, y + h / 2, slide_bg)
                    ratio = contrast(fg, bg)
                    if ratio + 0.01 < need:
                        issues.append(
                            f"  第{si}页 对比度不足 {ratio:.2f}:1 (需 {need}:1) "
                            f"| {size:.0f}pt{'粗' if bold else ''} "
                            f"文字#{fg[0]:02X}{fg[1]:02X}{fg[2]:02X} "
                            f"在底#{bg[0]:02X}{bg[1]:02X}{bg[2]:02X} 上 "
                            f"| \"{run.text.strip()[:26]}\"")

    print(f"=== {name} ===")
    if issues:
        for it in issues:
            print(it)
        print(f"  → {len(issues)} 处对比度不足")
    else:
        print("  OK：全部文字达到 WCAG AA 对比度")
    return len(issues)


if __name__ == "__main__":
    args = sys.argv[1:]
    if not args:
        base = os.path.join(os.path.dirname(os.path.abspath(__file__)), "_drafts")
        args = sorted(glob.glob(os.path.join(base, "*.pptx")))
    if not args:
        print("没有找到 pptx")
        sys.exit(1)
    total = 0
    for p in args:
        print()
        try:
            total += check(p)
        except Exception as exc:                        # noqa: BLE001
            print(f"  检查失败 {p}: {exc}")
    print(f"\n合计 {total} 处待修")
