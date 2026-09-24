# -*- coding: utf-8 -*-
"""Assistant 文件检索与治理模块 —— PPT 生成脚本
用法： python make_deck.py
依赖： python-pptx
数据来源： docs/architecture/{文件检索重构方案,文件治理功能设计,v0.4-实施清单}.md
           docs/devlog/2026-09-24.md
"""
import os
from pptx import Presentation
from pptx.util import Inches, Pt
from pptx.dml.color import RGBColor
from pptx.enum.text import PP_ALIGN, MSO_ANCHOR
from pptx.enum.shapes import MSO_SHAPE
from pptx.oxml.ns import qn

# ---------------------------------------------------------------- 设计常量
FONT   = "Microsoft YaHei"
NAVY   = RGBColor(0x14, 0x22, 0x38)
NAVY2  = RGBColor(0x1E, 0x30, 0x4C)
BLUE   = RGBColor(0x4A, 0x90, 0xD9)
BLUE_D = RGBColor(0x2C, 0x5F, 0x9E)
BLUE_L = RGBColor(0xE8, 0xF1, 0xFB)
INK    = RGBColor(0x1A, 0x1A, 0x1A)
MUTED  = RGBColor(0x60, 0x6B, 0x7C)
MUTED_L= RGBColor(0x8E, 0x9A, 0xAD)
LINE   = RGBColor(0xD8, 0xDE, 0xE6)
WHITE  = RGBColor(0xFF, 0xFF, 0xFF)
GREEN  = RGBColor(0x2E, 0x8B, 0x57)
GREEN_L= RGBColor(0xE7, 0xF4, 0xEC)
AMBER  = RGBColor(0xB5, 0x6E, 0x00)
AMBER_L= RGBColor(0xFD, 0xF3, 0xE2)
RED    = RGBColor(0xC0, 0x39, 0x2B)
RED_L  = RGBColor(0xFB, 0xEC, 0xEA)
BG     = RGBColor(0xF7, 0xF9, 0xFC)
ZEBRA  = RGBColor(0xF1, 0xF5, 0xFA)
CODEBG = RGBColor(0x11, 0x1C, 0x2E)

W, H = 13.333, 7.5
M    = 0.75                 # 左右边距
CW   = W - 2 * M            # 内容宽度 11.833
BODY_TOP, BODY_BOT = 1.78, 6.85

# ---------------------------------------------------------------- 基础工具
def style_font(run, size=None, bold=None, color=None, name=FONT, italic=None):
    run.font.name = name
    rPr = run._r.get_or_add_rPr()
    for tag in ('a:ea', 'a:cs'):
        el = rPr.find(qn(tag))
        if el is None:
            el = rPr.makeelement(qn(tag), {})
            rPr.append(el)
        el.set('typeface', name)
    if size   is not None: run.font.size  = Pt(size)
    if bold   is not None: run.font.bold  = bold
    if italic is not None: run.font.italic = italic
    if color  is not None: run.font.color.rgb = color

def textbox(slide, x, y, w, h, anchor=MSO_ANCHOR.TOP, wrap=True):
    tb = slide.shapes.add_textbox(Inches(x), Inches(y), Inches(w), Inches(h))
    tf = tb.text_frame
    tf.word_wrap = wrap
    tf.vertical_anchor = anchor
    tf.margin_left = tf.margin_right = tf.margin_top = tf.margin_bottom = 0
    return tf

def para(tf, text, size=14, bold=False, color=INK, first=False, before=0, after=4,
         align=PP_ALIGN.LEFT, line=1.15, italic=False, font=FONT):
    p = tf.paragraphs[0] if first else tf.add_paragraph()
    p.alignment = align
    p.space_before = Pt(before)
    p.space_after  = Pt(after)
    p.line_spacing = line
    r = p.add_run(); r.text = text
    style_font(r, size=size, bold=bold, color=color, italic=italic, name=font)
    return p

def rich(tf, segs, size=13, first=False, before=0, after=4, line=1.2,
         align=PP_ALIGN.LEFT):
    """segs = [(text, bold, color), ...] 同一段内混排"""
    p = tf.paragraphs[0] if first else tf.add_paragraph()
    p.alignment = align
    p.space_before = Pt(before); p.space_after = Pt(after); p.line_spacing = line
    for t, b, c in segs:
        r = p.add_run(); r.text = t
        style_font(r, size=size, bold=b, color=c)
    return p

def rect(slide, x, y, w, h, fill=None, line_color=None, line_w=0.75, radius=None):
    shp = slide.shapes.add_shape(
        MSO_SHAPE.ROUNDED_RECTANGLE if radius else MSO_SHAPE.RECTANGLE,
        Inches(x), Inches(y), Inches(w), Inches(h))
    if fill is None: shp.fill.background()
    else:            shp.fill.solid(); shp.fill.fore_color.rgb = fill
    if line_color is None: shp.line.fill.background()
    else:                  shp.line.color.rgb = line_color; shp.line.width = Pt(line_w)
    shp.shadow.inherit = False
    if radius: shp.adjustments[0] = radius
    return shp

def chip(slide, x, y, w, h, text, fill, txt_color, size=11, bold=True):
    rect(slide, x, y, w, h, fill=fill, radius=0.35)
    tf = textbox(slide, x, y, w, h, anchor=MSO_ANCHOR.MIDDLE)
    para(tf, text, size=size, bold=bold, color=txt_color, first=True,
         align=PP_ALIGN.CENTER, after=0)

def blank(prs, bg=BG):
    s = prs.slides.add_slide(prs.slide_layouts[6])
    rect(s, 0, 0, W, H, fill=bg)
    return s

def header(slide, kicker, title, dark=False):
    para(textbox(slide, M, 0.40, CW, 0.32), kicker, size=11.5, bold=True,
         color=BLUE, first=True, after=0)
    para(textbox(slide, M, 0.72, CW, 0.62), title, size=26, bold=True,
         color=WHITE if dark else INK, first=True, after=0)
    rect(slide, M, 1.46, 1.05, 0.045, fill=BLUE)

def footnote(slide, text, dark=False):
    para(textbox(slide, M, 6.98, CW - 0.6, 0.3), text, size=9.5,
         color=MUTED_L if dark else MUTED, first=True, after=0)

def pageno(slide, n, dark=False):
    para(textbox(slide, W - 1.15, 6.98, 0.5, 0.3), f"{n:02d}", size=10,
         bold=True, color=MUTED_L if dark else MUTED, first=True,
         align=PP_ALIGN.RIGHT, after=0)

def table(slide, x, y, w, rows, col_w, row_h=0.34, head_h=0.38,
          fs=11, head_fs=11, aligns=None, colors=None):
    """rows[0] 为表头；colors=None 或与 rows 等长的 'accent 色' 矩阵"""
    n_r, n_c = len(rows), len(rows[0])
    gt = slide.shapes.add_table(n_r, n_c, Inches(x), Inches(y), Inches(w),
                                Inches(head_h + row_h * (n_r - 1))).table
    gt.first_row = False
    gt.horz_banding = False
    for i, cw in enumerate(col_w):
        gt.columns[i].width = Inches(cw)
    gt.rows[0].height = Inches(head_h)
    for r in range(1, n_r):
        gt.rows[r].height = Inches(row_h)
    for r, row in enumerate(rows):
        for c, val in enumerate(row):
            cell = gt.cell(r, c)
            cell.margin_left = Inches(0.085); cell.margin_right = Inches(0.07)
            cell.margin_top = Inches(0.02);   cell.margin_bottom = Inches(0.02)
            cell.vertical_anchor = MSO_ANCHOR.MIDDLE
            cell.fill.solid()
            cell.fill.fore_color.rgb = NAVY if r == 0 else (WHITE if r % 2 else ZEBRA)
            tf = cell.text_frame; tf.word_wrap = True
            p = tf.paragraphs[0]
            p.line_spacing = 1.02
            p.alignment = (aligns[c] if aligns else PP_ALIGN.LEFT)
            run = p.add_run(); run.text = str(val)
            col = WHITE if r == 0 else INK
            if colors and r > 0 and colors[r - 1][c] is not None:
                col = colors[r - 1][c]
            style_font(run, size=head_fs if r == 0 else fs, bold=(r == 0), color=col)
    return gt

def codebox(slide, x, y, w, h, lines, size=10.5):
    rect(slide, x, y, w, h, fill=CODEBG, radius=0.05)
    tf = textbox(slide, x + 0.22, y + 0.14, w - 0.44, h - 0.28)
    for i, (txt, col) in enumerate(lines):
        para(tf, txt, size=size, color=col, first=(i == 0), after=1.5, line=1.12,
             font="Consolas")
    return tf

def bullets(tf, items, size=12.5, gap=5, line=1.22):
    """items = [(level, segs)]，segs 同 rich()"""
    for i, (lvl, segs) in enumerate(items):
        indent = [0, 0.22, 0.44][lvl]
        p = rich(tf, segs, size=size, first=(i == 0), after=gap, line=line)
        p.space_before = Pt(0)
        from pptx.util import Emu
        p._pPr = p._p.get_or_add_pPr()
        p._pPr.set('marL', str(int(indent * 914400)))
        p._pPr.set('indent', '0')

def arrow(slide, x, y, color=BLUE, size=16):
    para(textbox(slide, x, y, 0.34, 0.3, anchor=MSO_ANCHOR.MIDDLE),
         "▶", size=size, color=color, first=True, align=PP_ALIGN.CENTER, after=0)

def card(slide, x, y, w, h, title, body, accent=BLUE, tsize=14, bsize=11,
         fill=WHITE):
    rect(slide, x, y, w, h, fill=fill, line_color=LINE, radius=0.05)
    rect(slide, x, y + 0.16, 0.055, h - 0.32, fill=accent)
    tf = textbox(slide, x + 0.28, y + 0.19, w - 0.52, 0.4)
    para(tf, title, size=tsize, bold=True, color=INK, first=True, after=0)
    tf2 = textbox(slide, x + 0.28, y + 0.62, w - 0.52, h - 0.8)
    first = True
    for ln in (body if isinstance(body, list) else [body]):
        para(tf2, ln, size=bsize, color=MUTED, first=first, after=3.5, line=1.2)
        first = False

# ================================================================ 开始构建
prs = Presentation()
prs.slide_width, prs.slide_height = Inches(W), Inches(H)
PAGE = [0]
def nxt(s, dark=False):
    PAGE[0] += 1
    pageno(s, PAGE[0], dark)
    return s

# ---------------------------------------------------------------- 01 封面
s = blank(prs, NAVY)
rect(s, 0, 0, 0.17, H, fill=BLUE)
rect(s, 7.9, 1.5, 4.6, 4.4, fill=NAVY2, radius=0.04)
para(textbox(s, 8.3, 1.85, 3.9, 0.3), "本次范围", size=11, bold=True,
     color=BLUE, first=True, after=0)
tf = textbox(s, 8.3, 2.25, 3.9, 3.4)
for i, t in enumerate([
        "已完成：检索链路 + A1 止血",
        "实测：索引其实只覆盖 Desktop",
        "问题清单：P0 ×6 / P1 ×6 / P2 ×7",
        "目标架构：四层 + 反馈回路",
        "路线图：Phase A → B → C → D → E"]):
    para(tf, "·  " + t, size=12, color=RGBColor(0xC7, 0xD4, 0xE6), first=(i == 0),
         after=9, line=1.25)

para(textbox(s, 1.05, 2.15, 6.6, 0.34), "ASSISTANT · 技术分享", size=13,
     bold=True, color=BLUE, first=True, after=0)
para(textbox(s, 1.05, 2.6, 6.7, 1.3), "文件检索与治理模块", size=42, bold=True,
     color=WHITE, first=True, after=0)
para(textbox(s, 1.05, 3.85, 6.6, 0.5),
     "已完成的能力 · 实测暴露的问题 · Phase A–E 路线图",
     size=16, color=RGBColor(0xB9, 0xC6, 0xD8), first=True, after=0)
rect(s, 1.05, 4.62, 3.0, 0.03, fill=RGBColor(0x35, 0x4A, 0x66))
para(textbox(s, 1.05, 4.9, 6.6, 1.1),
     "v0.3.0 收尾 → v0.4.0 文件子系统 Phase A\n"
     "2026-09-24 ｜ 依据 docs/architecture/ 三份设计稿",
     size=11.5, color=MUTED_L, first=True, after=0, line=1.4)

# ---------------------------------------------------------------- 02 目录
s = nxt(blank(prs))
header(s, "AGENDA", "这份 PPT 讲四件事")
items = [
    ("01", "现在能做什么", "检索链路、五级模糊匹配、\nA1 止血的四项修复与实测数据", GREEN, GREEN_L),
    ("02", "实测暴露了什么", "P0/P1/P2 问题清单；\n以及「索引其实只覆盖 Desktop」这个真问题", RED, RED_L),
    ("03", "要建成什么样", "四层架构、关键接口、\n以及四个关键设计决策的取舍理由", BLUE, BLUE_L),
    ("04", "分几步走", "Phase A（搜索基建）→ B（治理过滤）\n→ C/D/E（Box / 追踪 / 自动化）", AMBER, AMBER_L),
]
for i, (num, t, d, ac, al) in enumerate(items):
    x = M + i * (CW / 4)
    w = CW / 4 - 0.22
    rect(s, x, 2.15, w, 3.6, fill=WHITE, line_color=LINE, radius=0.05)
    rect(s, x, 2.15, w, 0.055, fill=ac)
    para(textbox(s, x + 0.3, 2.5, w - 0.6, 0.6), num, size=30, bold=True,
         color=ac, first=True, after=0)
    para(textbox(s, x + 0.3, 3.28, w - 0.6, 0.4), t, size=15, bold=True,
         color=INK, first=True, after=0)
    para(textbox(s, x + 0.3, 3.8, w - 0.6, 1.7), d, size=11, color=MUTED,
         first=True, after=0, line=1.35)
footnote(s, "所有数字均来自本机实测或设计稿原文，未做估算加工。")

# ---------------------------------------------------------------- 03 定位
s = nxt(blank(prs))
header(s, "模块定位", "检索面与采集面：先把职责切开")
card(s, M, BODY_TOP, 5.7, 2.15, "文件检索（检索面）",
     ["让用户「找到想要的」：查询解析 → 候选筛选 → 打分 → top-K",
      "只读：内存快照 + COW 发布，查询路径不走 SQLite",
      "对应 Phase A（A1–A8）"], accent=BLUE)
card(s, M + 6.13, BODY_TOP, 5.7, 2.15, "文件治理（采集面）",
     ["保证「不漏文件」：种类过滤、工程排除、下载中转、去向追踪",
      "写：磁盘遍历全量入库，只受排除规则约束",
      "对应 Phase B–E"], accent=AMBER)

rect(s, M, 4.3, CW, 2.3, fill=WHITE, line_color=LINE, radius=0.05)
rect(s, M, 4.3, 0.055, 2.3, fill=GREEN)
para(textbox(s, M + 0.35, 4.52, CW - 0.7, 0.35),
     "核心设计决定：两种「过滤」必须分清", size=15, bold=True, color=INK,
     first=True, after=0)
tf = textbox(s, M + 0.35, 4.98, CW - 0.7, 1.4)
rich(tf, [("磁盘遍历全量入库", True, INK),
          ("（只受排除规则约束）→", False, MUTED),
          ("内存快照按当前白名单筛选构建", True, INK),
          ("。于是切换「只要文档 / 含代码 / 全都要」只需", False, MUTED),
          ("从 DB 重建快照（数十 ms），不必重新扫盘", True, GREEN),
          ("。", False, MUTED)], size=12.5, first=True, after=7)
rich(tf, [("分界线是 ", False, MUTED), ("files 表", True, BLUE_D),
          ("：排除规则（L2/L3）决定", False, MUTED), ("什么进 DB", True, INK),
          ("；种类白名单只决定", False, MUTED), ("内存快照装什么", True, INK),
          ("（可随时切换）。", False, MUTED)], size=12.5, after=7)
rich(tf, [("唯一例外：", True, RED),
          ("下载 box 不服从白名单 —— 否则白名单设成「只要 PDF」，刚下载的 .exe "
           "会从收件箱消失，属数据丢失级事故。", False, MUTED)], size=12.5, after=0)

# ---------------------------------------------------------------- 04 已完成：链路
s = nxt(blank(prs))
header(s, "已完成 ①", "检索链路：Phase 2 已经跑通的一条路")
stages = [("键盘输入", "QML TextInput"), ("40ms 防抖", "IME 预编辑拦截"),
          ("AppBridge", "三路候选 + 计算器"), ("排序截断", "score → priority"),
          ("结果列表", "ListView")]
bw, gap = 2.05, 0.35
for i, (t, d) in enumerate(stages):
    x = M + i * (bw + gap)
    fill = BLUE_L if i < 3 else WHITE
    rect(s, x, BODY_TOP, bw, 1.05, fill=fill, line_color=BLUE if i < 3 else LINE,
         radius=0.07)
    para(textbox(s, x + 0.14, BODY_TOP + 0.18, bw - 0.28, 0.32), t, size=12,
         bold=True, color=BLUE_D if i < 3 else INK, first=True, after=0,
         align=PP_ALIGN.CENTER)
    para(textbox(s, x + 0.14, BODY_TOP + 0.55, bw - 0.28, 0.34), d, size=9.5,
         color=MUTED, first=True, after=0, align=PP_ALIGN.CENTER)
    if i < len(stages) - 1:
        arrow(s, x + bw + 0.005, BODY_TOP + 0.37, size=13)

para(textbox(s, M, 3.15, CW, 0.3), "三路候选 + 计算器，统一按 score 排序",
     size=12.5, bold=True, color=INK, first=True, after=0)
rows = [
    ["来源", "实现", "进入条件", "结果优先级"],
    ["内置命令 / URL", "AppBridge::m_allItems（10 条）", "始终参与", "priority 6–10"],
    ["已索引应用", "AppIndexer 扫描开始菜单 .lnk（本机 31 个）", "始终参与", "priority 5"],
    ["文件", "FileSearcher 全量索引 + FuzzyMatcher", "仅非空查询，最多 20 条", "priority 1"],
    ["计算器", "Calculator 递归下降解析", "含数字且含运算符、长度 ≥3", "score 1001 强制置顶"],
]
table(s, M, 3.52, CW, rows, [2.0, 5.05, 2.85, 1.93], row_h=0.42, fs=10.5)

footnote(s, "落点：include/core/FileSearcher.h · src/ui/AppBridge.cpp · resources/qml/MainWindow.qml")

# ---------------------------------------------------------------- 05 五级打分
s = nxt(blank(prs))
header(s, "已完成 ②", "FuzzyMatcher：五级打分 + 一条排序规则")
rows = [
    ["分值", "层级", "含义", "例子（查询 → 命中）"],
    ["1000", "完全匹配", "大小写不敏感的全等", "cmd  →  cmd"],
    ["900", "前缀匹配", "以查询开头", "repo  →  report.docx"],
    ["800", "词边界匹配", "空格 / 连字符 / 下划线 / 点之后", "cfg  →  app_cfg.ini"],
    ["600–400", "包含匹配", "子串命中，位置越靠前分越高", "port  →  report.docx"],
    ["400–0", "编辑距离", "Levenshtein（仅短串启用）", "reprot  →  report"],
    ["0", "不匹配", "不进入结果集", "—"],
]
table(s, M, BODY_TOP, 7.6, rows, [1.0, 1.3, 3.1, 2.2], row_h=0.40, fs=10.5)

rect(s, M + 7.75, BODY_TOP, 4.08, 2.78, fill=WHITE, line_color=LINE, radius=0.05)
rect(s, M + 7.75, BODY_TOP, 4.08, 0.055, fill=GREEN)
para(textbox(s, M + 8.05, BODY_TOP + 0.26, 3.5, 0.35), "设计取舍", size=14,
     bold=True, color=INK, first=True, after=0)
tf = textbox(s, M + 8.05, BODY_TOP + 0.74, 3.5, 1.95)
for i, t in enumerate([
        "五级语义保留为 tier 快速判定，A4 的 DP 打分在它之后细化。",
        "文件 priority = 1：同分时排在应用与内置命令之后。",
        "文件只在非空查询出现，空查询不返回文件。",
        "A4 拟补子序列匹配：chr → chrome_installer.exe。"]):
    para(tf, "·  " + t, size=10, color=MUTED, first=(i == 0), after=8, line=1.25)

rect(s, M, 4.72, CW, 2.0, fill=WHITE, line_color=LINE, radius=0.05)
rect(s, M, 4.72, 0.055, 2.0, fill=AMBER)
para(textbox(s, M + 0.35, 4.92, CW - 0.7, 0.35),
     "排序规则（现状）", size=15, bold=True, color=INK, first=True, after=0)
tf = textbox(s, M + 0.35, 5.32, CW - 0.7, 1.3)
rich(tf, [("① score 降序", True, INK),
          ("（模糊分越高越靠前）；", False, MUTED),
          ("② 同分按 priority 降序", True, INK),
          ("（内置命令 > 应用 > 文件）；", False, MUTED),
          ("③ 计算器结果以 score = 1001 prepend", True, INK),
          ("，恒排第一。", False, MUTED)], size=12.5, first=True, after=7)
rich(tf, [("P1-3 的问题：现在是", False, MUTED),
          ("「收集全部命中 → std::sort 全排序 → erase 截断」", True, RED),
          ("，而只需要 top-K；20 万条量级下 O(M log M) + 大数组拷贝会被放大。",
           False, MUTED)], size=12.5, after=0)

# ---------------------------------------------------------------- 06 A1 止血
s = nxt(blank(prs))
header(s, "已完成 ③", "A1 止血：独立 patch，2026-09-24 落地")
chip(s, M, BODY_TOP - 0.02, 1.98, 0.32, "分支 fix/a1-stop-bleeding", BLUE_L, BLUE_D, 9.5)
chip(s, M + 2.10, BODY_TOP - 0.02, 0.95, 0.32, "已验证", GREEN_L, GREEN, 9.5)
para(textbox(s, M + 3.20, BODY_TOP - 0.02, 8.6, 0.32),
     "4 个文件、104 行新增；旧 FileSearcher 接口与语义不变，单 commit 可 revert",
     size=10, color=MUTED, first=True, after=0)

rows = [
    ["缺陷", "原计划做法", "实际做法", "结果"],
    ["P0-1\n跟随 reparse point",
     "QDirIterator 加\nQDir::NoSymLinks",
     "实测无效 → 自控前序 DFS +\nisSymLink() || isJunction()",
     "126 条 63 层叠罗汉路径 → 0"],
    ["P0-2\n热路径字符串分配",
     "删掉 toNativeSeparators()",
     "目录名命中即整棵子树不进\n（真的剪枝，不只是省分配）",
     "本机剪掉 160 个目录"],
    ["P1-5\n搜索写库时机",
     "从 search() 挪到\nexecuteCommand()",
     "只在「真的执行了」时写；\n补 debug 日志可观测",
     "历史表不再被前缀垃圾污染"],
    ["P1-6\n输入防抖 + IME",
     "Timer 40ms +\ninputMethodComposing",
     "同计划",
     "8 字符 → 1 次全量检索"],
]
table(s, M, 2.2, CW, rows, [2.35, 2.7, 4.35, 2.43], row_h=0.78, head_h=0.38, fs=10)

rect(s, M, 5.9, CW, 0.85, fill=RED_L, radius=0.05)
para(textbox(s, M + 0.3, 6.06, CW - 0.6, 0.6),
     "⚠  最重要的一条：计划里的 QDir::NoSymLinks 是个空操作 —— 编译通过、日志正常、"
     "看起来更好了，只有把索引 dump 出来才发现没生效。",
     size=11, bold=True, color=RED, first=True, after=0, line=1.25)

# ---------------------------------------------------------------- 07 A1 验收
s = nxt(blank(prs))
header(s, "已完成 ③ · 验收", "A1 三条验收：全部实测，不是推演")
rows = [
    ["#", "验收项", "方法", "结果"],
    ["①", "junction 不递归、不重复索引",
     "临时 env 门控仪器把索引全路径落盘 + grep", "含 /loop/ 的条目 126 → 0；探针文件恰好 2 条"],
    ["②", "连打 8 字符只触发 1 次 search",
     "PostMessage 投 WM_CHAR ×8，按 debug 日志统计", "只出现 1 行 'abcdefgh'，不是 8 行前缀"],
    ["③", "打字过程中 history.db 不新增行",
     "打字前/后各查一次 count(*)", "43 → 43 不变"],
    ["③+", "正向对照：执行时才写",
     "输 1+2 后回车（计算器恒排第一，副作用仅写剪贴板）", "43 → 44，新增行 query='1+2'"],
]
table(s, M, BODY_TOP, CW, rows, [0.6, 3.3, 4.35, 3.58], row_h=0.6, fs=10.5)

card(s, M, 4.4, 5.7, 2.2, "顺手做了两件事",
     ["清空 search_history 旧数据：44 → 0（清空前正是 1+ / 5e / isa / ope / de / del 这类前缀垃圾）；"
      "command_history 与 app_launch_history 未动。",
      "验收手法沉淀成可复用脚本（out/a1_typing_probe.ps1）："
      "走 Qt 真实消息链，不注入代码、不改被测程序。"],
     accent=GREEN, tsize=13.5, bsize=10.5)
card(s, M + 6.13, 4.4, 5.7, 2.2, "顺带量到的两个数",
     ["索引建库 ≈ 900 ms（修前同一台机 713–2379 ms），剪枝 160 个目录。",
      "小写化比较走 indexOf(..., CaseInsensitive)，全程零临时字符串 —— "
      "目录级剪枝让「省分配」变成了「少枚举」。"],
     accent=BLUE, tsize=13.5, bsize=10.5)

# ---------------------------------------------------------------- 08 P0
s = nxt(blank(prs))
header(s, "现状问题 · P0", "P0：正确性与稳定性（6 条）")
rows = [
    ["编号", "问题", "后果"],
    ["P0-1", "QDirIterator 默认过滤器含符号链接，Subdirectories 跟随 junction 递归",
     "重复索引甚至无限递归 ✅ A1 已修"],
    ["P0-2", "跳过判定逐文件做 toNativeSeparators() + 5 次 contains",
     "最热内循环里的设计错误 ✅ A1 已修"],
    ["P0-3", "20000 上限按遍历顺序任意截断，顺序取决于目录项返回顺序",
     "结果不可解释、两次启动内容可能不同、D 盘永远进不来 ← 本机已实际发生"],
    ["P0-4", "索引在主线程、engine.load() 之前同步构建",
     "冷启动白屏数百 ms ～ 数秒"],
    ["P0-5", "索引建完即冻结，无 watcher、无手动刷新入口",
     "新建/改名/删除的文件在本次运行中永远不反映"],
    ["P0-6", "namespace + 文件级静态 s_index，无实例无注入点",
     "不可单测 / mock / 多实例，与项目其他模块风格不一致"],
]
table(s, M, BODY_TOP, CW, rows, [1.0, 6.6, 4.23], row_h=0.66, fs=10.5)
footnote(s, "✅ 标记为本次 A1 已修复；P0-3/P0-4/P0-5/P0-6 分别由 A3 / A2 / A5 / A2 承接。")

# ---------------------------------------------------------------- 09 P1 P2
s = nxt(blank(prs))
header(s, "现状问题 · P1 / P2", "性能债与检索质量缺口")
para(textbox(s, M, BODY_TOP - 0.02, 5.7, 0.3), "P1 — 性能", size=14, bold=True,
     color=AMBER, first=True, after=0)
rows = [
    ["编号", "问题"],
    ["P1-1", "每按键全量线性扫 2 万条 + 完整五级打分（O(N·L)，约 5–15 ms，未实测）"],
    ["P1-2", "FuzzyMatcher 内反复 toLower()，每次分配新 QString → N 次堆分配/按键"],
    ["P1-3", "收集全部命中后全排序再截断，而只需要 top-K"],
    ["P1-4", "QString(UTF-16) 同时存 name / path，目录串高度重复 → 2 万条约数 MB"],
    ["P1-5", "每按键同步写 search_history ✅ A1 已修"],
    ["P1-6", "QML onTextChanged 无防抖 ✅ A1 已修"],
]
table(s, M, BODY_TOP + 0.34, 5.85, rows, [0.75, 5.1], row_h=0.46, fs=10)

para(textbox(s, M + 6.13, BODY_TOP - 0.02, 5.7, 0.3), "P2 — 检索质量 / 体验",
     size=14, bold=True, color=BLUE_D, first=True, after=0)
rows2 = [
    ["编号", "问题"],
    ["P2-1", "只匹配文件名，路径不参与；无空格分词"],
    ["P2-2", "无子序列匹配：chr 命中不到 chrome_installer.exe"],
    ["P2-3", "无 frecency / 使用频率权重"],
    ["P2-4", "空查询时文件完全不出现（应给「最近文件」）"],
    ["P2-5", "IME 预编辑触发检索；中文无法用拼音首字母命中"],
    ["P2-6", "无扩展名独立权重，docx 命中大量噪音"],
    ["P2-7", "结果动作单一：缺 Ctrl+Enter 打开所在文件夹、复制路径"],
]
table(s, M + 6.13, BODY_TOP + 0.34, 5.7, rows2, [0.75, 4.95], row_h=0.42, fs=10)
footnote(s, "另有 P3 能力缺口：索引无持久化（冷启动从零爬盘）、无可配置根目录与排除规则、无索引健康度可观测。")

# ---------------------------------------------------------------- 10 实测关键事实
s = nxt(blank(prs))
header(s, "实测暴露的真问题", "索引其实「名不副实」：用户搜不到自己的文档")
big = [("35607", "Desktop 文件数", RED), ("20000", "索引上限", RED),
       ("0", "Documents 进入索引的文件数", RED)]
for i, (v, lab, c) in enumerate(big):
    x = M + i * 3.95
    rect(s, x, BODY_TOP, 3.75, 1.28, fill=WHITE, line_color=LINE, radius=0.06)
    para(textbox(s, x + 0.28, BODY_TOP + 0.14, 3.2, 0.6), v, size=27, bold=True,
         color=c, first=True, after=0)
    para(textbox(s, x + 0.28, BODY_TOP + 0.76, 3.2, 0.35), lab, size=10.5,
         color=MUTED, first=True, after=0)

para(textbox(s, M, 3.3, CW, 0.3),
     "链式因果（P0-3 的具体后果）", size=13.5, bold=True, color=INK, first=True, after=0)
chain = [("roots 顺序\nDesktop 第一", "root[0]"), ("Desktop 单根\n就吃满 2 万", "35607 > 20000"),
         ("遍历提前 break", "Documents 未开始"),
         ("Documents / Downloads\n一个文件都没进", "1652 / 596 全部落空")]
for i, (t, sub) in enumerate(chain):
    x = M + i * 3.05
    rect(s, x, 3.68, 2.75, 1.05, fill=RED_L, line_color=RED, radius=0.07)
    para(textbox(s, x + 0.16, 3.82, 2.45, 0.5), t, size=10.5, bold=True,
         color=RED, first=True, after=0, align=PP_ALIGN.CENTER, line=1.15)
    para(textbox(s, x + 0.16, 4.36, 2.45, 0.3), sub, size=9.5, color=MUTED,
         first=True, after=0, align=PP_ALIGN.CENTER)
    if i < 3:
        arrow(s, x + 2.79, 4.05, color=RED, size=13)

rect(s, M, 5.0, CW, 1.7, fill=WHITE, line_color=LINE, radius=0.05)
rect(s, M, 5.0, 0.055, 1.7, fill=AMBER)
tf = textbox(s, M + 0.35, 5.2, CW - 0.7, 1.35)
rich(tf, [("这意味着功能「看起来有、实际没有」", True, INK),
          ("：日志显示 indexed 20000 files 一切正常，但用户搜 Documents 里的东西永远搜不到。",
           False, MUTED)], size=12, first=True, after=7)
rich(tf, [("结论：", True, AMBER),
          ("P0-3 的实际优先级高于原判断（实施清单原计划留给 A3）。"
           "修复方向已定：", False, MUTED),
          ("取消按 frecency 驱逐", True, INK),
          ("，改为", False, MUTED),
          ("「完整索引优先 + 软预算告警 + 硬上限停止」", True, INK),
          ("—— 对搜索而言，正确性远比内存重要，绝不能出现「文件凭空消失」。",
           False, MUTED)], size=12, after=0)

# ---------------------------------------------------------------- 11 架构
s = nxt(blank(prs))
header(s, "目标架构", "四层 + 一条反馈回路")
layers = [
    ("采集层  Indexer", "Scanner + Store", "Win32 迭代式 DFS，目录级剪枝；产出不可变 COW 快照", BLUE),
    ("查询层  QueryEngine", "Parser → Filter → Score → TopK",
     "归一化 → 位图预筛 → 分层打分 waterfall → top-K 最小堆", BLUE),
    ("融合层  ResultMerger", "ISearchProvider k-way merge",
     "各 provider 取 top-K 后归并，再叠加 frecency 综合分", BLUE),
    ("反馈层  Frecency", "只在「执行」时写", "异步写库、不重建快照；空查询返回 frecency top-8", GREEN),
]
y = BODY_TOP
for i, (t, sub, desc, ac) in enumerate(layers):
    rect(s, M, y, CW, 0.88, fill=WHITE, line_color=LINE, radius=0.05)
    rect(s, M, y, 0.055, 0.88, fill=ac)
    para(textbox(s, M + 0.3, y + 0.14, 3.2, 0.35), t, size=13.5, bold=True,
         color=INK, first=True, after=0)
    para(textbox(s, M + 0.3, y + 0.5, 3.2, 0.3), sub, size=9.5, color=BLUE_D,
         first=True, after=0, font="Consolas")
    para(textbox(s, M + 3.75, y, 7.85, 0.88, anchor=MSO_ANCHOR.MIDDLE), desc,
         size=11, color=MUTED, first=True, after=0, line=1.25)
    if i < 3:
        arrow(s, M + 1.0, y + 0.9, size=11)
    y += 1.02

rect(s, M, 6.0, CW, 0.72, fill=BLUE_L, radius=0.06)
rich(textbox(s, M + 0.3, 6.0, CW - 0.6, 0.72, anchor=MSO_ANCHOR.MIDDLE),
     [("硬约束：", True, BLUE_D),
      ("查询层只能通过 FileSnapshot 的只读原语访问数据 —— 物理表示（v1 的 "
       "vector<FileEntry>、后续的 SoA / 字典表）必须藏在快照接口之后，"
       "替换表示不改调用方。", False, INK)], size=11, first=True, after=0, line=1.3)

# ---------------------------------------------------------------- 12 接口
s = nxt(blank(prs))
header(s, "关键接口", "门面只做四件事，快照只给只读原语")
codebox(s, M, BODY_TOP, 5.75, 2.35, [
    ("// FileIndex —— 门面（薄）", MUTED_L),
    ("void start();", WHITE),
    ("void refresh();", WHITE),
    ("QList<Match> query(const Query&);", WHITE),
    ("IndexStats stats() const;", WHITE),
    ("void upsert(const QString& path);", WHITE),
    ("void remove(const QString& path);", WHITE),
    ("", WHITE),
    ("// 私有成员：Scanner / Store /", MUTED_L),
    ("// SnapshotManager / QueryEngine", MUTED_L),
], size=9.5)
para(textbox(s, M, 4.32, 5.75, 0.75),
     "判定标准：新逻辑能不能放进四个协作者之一？能，就不要加到门面上。",
     size=10.5, color=MUTED, first=True, after=0, line=1.3)

codebox(s, M + 6.08, BODY_TOP, 5.75, 2.35, [
    ("// FileSnapshot —— 唯一的查询表示", MUTED_L),
    ("int     size() const;", WHITE),
    ("QString nameAt(int i) const;", WHITE),
    ("QString lowerAt(int i) const;", WHITE),
    ("QString pathAt(int i) const;", WHITE),
    ("qint64  sizeAt(int i) const;", WHITE),
    ("qint64  mtimeAt(int i) const;", WHITE),
])
para(textbox(s, M + 6.08, 4.32, 5.75, 0.75),
     "v1 物理表示是 std::vector<FileEntry>（20 万条 ≈ 50–70 MB）；"
     "SoA / 字典表 / UTF-8 存储都是 gated 优化，先有 baseline 再决定。",
     size=10.5, color=MUTED, first=True, after=0, line=1.3)

codebox(s, M, 5.25, CW, 1.5, [
    ("// ISearchProvider —— A7 的稳定点，插件系统（GUIDE Phase 4）就挂在它上面", MUTED_L),
    ("class ISearchProvider {", WHITE),
    ("    virtual QString providerId() const = 0;", WHITE),
    ("    virtual QList<SearchResult> query(std::string_view text, int limit) = 0;", WHITE),
    ("};", WHITE),
])

# ---------------------------------------------------------------- 13 决策 1
s = nxt(blank(prs))
header(s, "关键决策 ①", "为什么不用 SQLite FTS5 做文件名检索")
rows = [
    ["理由", "依据"],
    ["本机根本不可用",
     "vcpkg sqlite3 3.53.3 只启用 json1；扫描 sqlite3.dll 未命中 FTS5/FTS3/rtree。"
     "要用得先 vcpkg install \"sqlite3[fts5]\"，可能触发 qtbase 重配"],
    ["trigram 解决不了主场景",
     "只解决子串，不解决子序列（chr → chrome）与 typo —— 而那才是启动器的核心卖点"],
    ["选择率太低",
     "2–3 字符查询的 postings 接近全量，却要多付一份索引体积"],
    ["仍要回表排序",
     "拿到 rowid 后还得回表取 path / frecency 再排序，省不掉关键路径"],
]
table(s, M, BODY_TOP, CW, rows, [3.0, 8.83], row_h=0.72, fs=10.5)
rect(s, M, 5.3, CW, 1.4, fill=WHITE, line_color=LINE, radius=0.05)
rect(s, M, 5.3, 0.055, 1.4, fill=BLUE)
tf = textbox(s, M + 0.35, 5.5, CW - 0.7, 1.05)
rich(tf, [("结论：", True, BLUE_D),
          ("走「内存快照索引 + SQLite 持久化」路线 —— 查询路径完全不碰 SQLite，"
           "SQLite 只负责", False, MUTED),
          ("冷启动加速与 frecency 存储", True, INK),
          ("。内容全文检索另立项，不在本次范围。", False, MUTED)],
     size=12, first=True, after=7)
rich(tf, [("同样被否掉的：", True, INK),
          ("Xapian / Tantivy(Rust) / Lucene（定位文档级全文、引外部运行时，与学习项目不符）；"
           "RocksDB / LevelDB（KV 不解决匹配算法，20 万级用不上 LSM）；"
           "自建 trigram 倒排（内存与快照方案收益重叠）。", False, MUTED)],
     size=11.5, after=0)

# ---------------------------------------------------------------- 14 决策 2
s = nxt(blank(prs))
header(s, "关键决策 ②", "并发、失效与「绝不静默驱逐」")
card(s, M, BODY_TOP, 3.75, 2.6, "COW 快照发布",
     ["构建完一次性发布不可变快照，读者整轮查询免锁。",
      "为什么不用 QReadWriteLock + 原地修改：原地改会迫使查询持锁、"
      "重建期阻塞 UI，正好和修 P0-4（冷启动白屏）冲突。",
      "优先 std::atomic<std::shared_ptr>（C++20），编译不过降级 std::shared_mutex。"
      "A2 先花 10 分钟验证 MSVC 是否支持。"],
     accent=BLUE, tsize=13.5, bsize=9.5)
card(s, M + 4.04, BODY_TOP, 3.75, 2.6, "不用 QFileSystemWatcher",
     ["Windows 后端基于 FindFirstChangeNotification，单句柄不递归子树。",
      "对 6 个根下成千上万子目录会退化成逐目录注册句柄 —— 句柄爆炸，"
      "且新增子目录还要重注册。",
      "改为 ReadDirectoryChangesW（bWatchSubtree=TRUE），每根一句柄。",
      "溢出（ERROR_NOTIFY_ENUM_DIR）→ 发 Overflow 事件 → 该根全量重扫，不静默丢事件。"],
     accent=AMBER, tsize=13.5, bsize=9.5)
card(s, M + 8.08, BODY_TOP, 3.75, 2.6, "绝不静默驱逐",
     ["早期「索引满按 frecency 驱逐」的设计已明确废弃。",
      "被驱逐后用户搜不到自己存在的文件 —— 行为变成「文件凭空消失」。",
      "对搜索而言正确性远比内存重要。",
      "改为三档显式可见：软预算 30 万条告警 / 硬上限 100 万条停止并提示；"
      "缩小规模靠 roots 与排除规则「少收」，而不是事后丢。"],
     accent=RED, tsize=13.5, bsize=9.5)

rect(s, M, 4.72, CW, 1.95, fill=WHITE, line_color=LINE, radius=0.05)
rect(s, M, 4.72, 0.055, 1.95, fill=GREEN)
para(textbox(s, M + 0.35, 4.92, CW - 0.7, 0.32),
     "最易踩的坑：细化缓存必须存「上一轮全部命中」", size=13.5, bold=True,
     color=INK, first=True, after=0)
tf = textbox(s, M + 0.35, 5.35, CW - 0.7, 1.2)
rich(tf, [("缓存里存 top-K，会出现", False, MUTED),
          ("「结果凭空消失」", True, RED),
          ("的 bug：用户删掉一个字符后，本该回到结果集的条目因为不在上一轮 "
           "top-K 里而永远不出现。规则：缓存深度 1 + 查询长度 ≤2 不吃缓存；"
           "且必须有「缓存命中路径与全量路径结果集逐条一致」的用例。", False, MUTED)],
     size=11.5, first=True, after=7)
rich(tf, [("位图预筛、细化缓存、charMask、SoA、并行分片 —— ", False, MUTED),
          ("全部 gated", True, AMBER),
          ("：A4 先出 baseline 数据，命中阈值才做。为没有实测支撑的 "
           "P50<3ms 去堆复杂算法，是最可能发生的浪费。", False, MUTED)],
     size=11.5, after=0)

# ---------------------------------------------------------------- 15 决策 3
s = nxt(blank(prs))
header(s, "关键决策 ③", "两个数据库的边界，与「迁移先于字段」")
rows = [
    ["", "file_index.db", "downloads.db"],
    ["性质", "派生数据 —— 可从磁盘重建", "权威数据 —— 磁盘上重建不出来"],
    ["写入方", "只有 FileIndexDatabase 可写", "只有 DownloadsDatabase 可写"],
    ["迁移失败", "直接丢弃重建（删文件重扫）", "必须备份、失败拒绝启动，绝不静默重建"],
    ["表", "dirs / files / roots / frecency", "downloads / move_journal / tracking_events /\n"
                                              "download_rules / sources / app_meta"],
]
table(s, M, BODY_TOP, CW, rows, [1.5, 5.15, 5.18], row_h=0.58, fs=10.5)

rect(s, M, 4.62, 5.75, 2.15, fill=WHITE, line_color=LINE, radius=0.05)
rect(s, M, 4.62, 0.055, 2.15, fill=BLUE)
para(textbox(s, M + 0.32, 4.82, 5.1, 0.32), "DatabaseMigrator（A3 必须先做出来）",
     size=13, bold=True, color=INK, first=True, after=0)
tf = textbox(s, M + 0.32, 5.18, 5.15, 1.52)
for i, t in enumerate([
        "版本号用原生 PRAGMA user_version，不额外建表。",
        "只做前向迁移，每步一事务；禁止 DROP TABLE + 重建。",
        "启动时序：打开 DB → 读 user_version → 逐版本迁移 → commit → 之后才允许其它访问。",
        "第一个真实用例：治理的 cat_mask / flags / root_id / origin_id + idx_files_sizemtime。"]):
    para(tf, "·  " + t, size=9.5, color=MUTED, first=(i == 0), after=6, line=1.22)

rect(s, M + 6.08, 4.62, 5.75, 2.15, fill=WHITE, line_color=LINE, radius=0.05)
rect(s, M + 6.08, 4.62, 0.055, 2.15, fill=GREEN)
para(textbox(s, M + 6.4, 4.82, 5.1, 0.32), "差量写入：三方差集",
     size=13, bold=True, color=INK, first=True, after=0)
tf = textbox(s, M + 6.4, 5.18, 5.15, 1.52)
for i, t in enumerate([
        "diskSet = 磁盘遍历 {(path,size,mtime)}；dbSet = SELECT path,size,mtime。",
        "added / removed / changed 三方差集（changed 按 size+mtime 判定）。",
        "单事务 + prepared statement 批量，每 1000 行提交一次 —— 避免每行一次 fsync。",
        "启动两阶段：先 loadFromDb() 灌满快照立即可查询，再后台差量校正。"]):
    para(tf, "·  " + t, size=9.5, color=MUTED, first=(i == 0), after=6, line=1.22)

# ---------------------------------------------------------------- 16 Phase A
s = nxt(blank(prs))
header(s, "路线图 · Phase A", "搜索基建（v0.4.0）：A1–A8，每步独立可编译可验收")
rows = [
    ["步", "内容", "交付 / 验收"],
    ["A1 ✅", "止血：junction / 目录级剪枝 / 写库时机 / 防抖+IME",
     "已落地 2026-09-24；独立 patch，三条验收全过"],
    ["A2", "结构：FileIndex 门面 + Snapshot + SnapshotManager（COW）+ 后台扫描线程",
     "立 fileIndex.engine = legacy|v2 开关，异常可一行配置退回；冷启动无可感白屏"],
    ["A3", "采集：FileScanner（Win32 DFS + 目录级剪枝）+ FileIndexDatabase + DatabaseMigrator + 差量事务",
     "扫描吞吐进 baseline；roots 生效；迁移框架先于字段增补落地"],
    ["A4", "查询：QueryEngine（Parser→Filter→Score→TopK）+ bench_file_index.cpp",
     "先出 baseline（N = 2/5/10/20 万），再决定要不要做优化候选"],
    ["A5", "增量：FileWatcher（ReadDirectoryChangesW）+ FileSystemEvent 分发 + 溢出降级重扫",
     "新建/改名/删除 ≤100 ms 可在搜索里看到；溢出用例"],
    ["A6", "冷启动与反馈：DB 载入 → 快照发布 → 后台差量；frecency 回路；空查询「最近文件」",
     "冷启动到可查询 < 300 ms（20 万条）；杀进程重启仍走 DB（日志可证）"],
    ["A7", "融合与收尾：ISearchProvider + k-way merge + 打分日志开关 + Ctrl+Enter/复制路径",
     "删除 FileSearcher 兼容层与 engine 开关后仍能编译运行；结果无回归"],
    ["A8", "（可选）拼音首字母：enablePinyinInitials，默认关闭",
     "开启后 bg 可命中「报告」；不进关键路径"],
]
table(s, M, BODY_TOP, CW, rows, [0.85, 6.15, 4.83], row_h=0.56, head_h=0.36, fs=9.5)
footnote(s, "分支节奏：A1 单独 patch → A2–A4 一个 feature 分支 → A5–A7 一个分支。A 阶段完成即是可用产品。")

# ---------------------------------------------------------------- 17 Phase B-E
s = nxt(blank(prs))
header(s, "路线图 · Phase B–E", "文件治理（v0.4.x → v0.5.x）")
rows = [
    ["阶段", "目标", "主要交付", "关键约束 / 验收"],
    ["B\nv0.4.x", "搜索 + 可控范围",
     "FileClassifier（16 位 FileKind 掩码）+ ExcludeRuleSet /\n"
     "ProjectGuard + QueryParser + 快照 catMask/flags + 过滤 Chip UI",
     "白名单切换 < 50 ms 且不扫盘；\nnode_modules 工程条目下降 ≥95%"],
    ["C\nv0.4.x–v0.5.x", "Download Box 中转",
     "DownloadsDatabase（pipe_state / track_state 双状态）+\n"
     "IngestEngine（A+B 主判定 + 退避）+ FileIdentity + BrowserSource",
     "下载完成 ≤3 s 可见；100 次端到端\n0 误拉；同名 0 覆盖；100% 可撤销"],
    ["D", "去向追踪",
     "FileId 同卷 → quickSig 跨卷 → 离线对账四级 →\nUnknown 兜底 UI；USN Journal 单独最后做",
     "同卷指纹命中 ≥99%、误判 0；\n移动链 ≥3 级正确；USN 默认关闭"],
    ["E", "自动治理与多源",
     "归档规则（默认 suggest）+ dry-run + 查重（quickSig/fullSig）+\n"
     "Todo/剪贴板联动 + IM 源适配器（QQ/微信/钉钉）",
     "自动移动必走 move_journal；\nIM 源最后做，不静默接管陌生目录"],
]
table(s, M, BODY_TOP, CW, rows, [1.35, 1.85, 5.05, 3.58], row_h=1.02,
      head_h=0.38, fs=9.5)
footnote(s, "B 必须先于 C：cat_mask/flags 就是 files 表 v1→v2 的第一个真实迁移用例，顺便把 DatabaseMigrator 立起来。")

# ---------------------------------------------------------------- 18 门槛
s = nxt(blank(prs))
header(s, "验收门槛", "性能预算（三档口径）与正确性硬门槛")
para(textbox(s, M, BODY_TOP - 0.02, CW, 0.28),
     "口径：Target / Budget / Fail —— 超过 Budget 就要解释，超过 Fail 必须停下来处理。",
     size=10.5, color=MUTED, first=True, after=0)
rows = [
    ["指标（N = 20 万条）", "Target", "Budget", "Fail"],
    ["查询 P50 / P99 —— 查询长 1–3", "< 3 / 8 ms", "< 6 / 15 ms", "P99 > 25 ms"],
    ["查询 P50 / P99 —— 查询长 4–8", "< 2 / 5 ms", "< 4 / 10 ms", "P99 > 20 ms"],
    ["快照内存", "≤ 30 MB", "≤ 40 MB", "> 60 MB（触发优化候选）"],
    ["冷启动到「可查询」", "< 150 ms", "< 300 ms", "> 600 ms"],
    ["首次全量扫描 20 万条（后台）", "< 8 s", "< 15 s", "> 30 s"],
    ["增量：watcher 事件 → 可查询", "< 100 ms", "< 300 ms", "> 1 s"],
]
table(s, M, BODY_TOP + 0.34, 7.6, rows, [3.2, 1.45, 1.5, 1.45], row_h=0.44, fs=10)

rect(s, M + 7.85, BODY_TOP + 0.34, 3.98, 3.10, fill=WHITE, line_color=LINE, radius=0.05)
rect(s, M + 7.85, BODY_TOP + 0.34, 3.98, 0.055, fill=GREEN)
para(textbox(s, M + 8.15, BODY_TOP + 0.62, 3.4, 0.32), "正确性硬门槛（不可让步）",
     size=13, bold=True, color=INK, first=True, after=0)
tf = textbox(s, M + 8.15, BODY_TOP + 1.08, 3.4, 2.2)
for i, t in enumerate([
        "golden 用例全命中：子序列 / 多词 / typo / 中文 / 短路路径。",
        "缓存命中路径与全量路径结果集逐条一致。",
        "种类白名单切换 < 50 ms 且不扫盘。",
        "浏览器下载 100 次：0 次误拉未完成文件。",
        "去向追踪同卷指纹命中 ≥99%，误判 0。",
        "同名冲突 0 覆盖，撤销成功率 100%。"]):
    para(tf, "·  " + t, size=9.5, color=MUTED, first=(i == 0), after=6, line=1.22)

rect(s, M, 5.3, CW, 1.4, fill=BLUE_L, radius=0.05)
rich(textbox(s, M + 0.35, 5.3, CW - 0.7, 1.4, anchor=MSO_ANCHOR.MIDDLE),
     [("注意：上表 Target 值是设计稿的预期，", False, INK),
      ("目前尚无实测支撑", True, RED),
      ("（20 万条 5–15 ms 也是估算）。所以 A4 的第一步是", False, INK),
      ("只建 baseline、不设断言", True, BLUE_D),
      ("：测 N = 2/5/10/20 万的查询分位、冷启动、全量扫描、快照内存，"
       "结果写进 docs/devlog/，再回头校准这张表。", False, INK)],
     size=11.5, first=True, after=0, line=1.35)

# ---------------------------------------------------------------- 19 风险
s = nxt(blank(prs))
header(s, "风险与边界", "最严重的风险，与「明确不做」的清单")
para(textbox(s, M, BODY_TOP - 0.02, 5.75, 0.3), "风险（按严重度）", size=13.5,
     bold=True, color=RED, first=True, after=0)
rows = [
    ["风险", "缓解"],
    ["误移用户文件（最严重）", "默认 suggest 不自动移动；autoOrganize 默认关；\n所有移动走 move_journal，先写 intent 再执行"],
    ["去向误判（比「不知道」更糟）", "多候选一律落 Unknown，绝不猜；\n指纹/签名唯一命中才确认"],
    ["索引 / box / 追踪三路重复处理同一变更", "200 ms 合并窗口 + 路径去重；\nbox 拉取完成后直接通知索引 upsert"],
    ["长路径 >260 字符", "全程 \\\\?\\ 前缀 + manifest longPathAware；\n本期至少保证不崩溃"],
    ["OneDrive 占位 / 网络盘", "只读元数据安全；mtime 频繁变靠 200 ms 去抖；\n网络盘默认不入 roots"],
]
table(s, M, BODY_TOP + 0.34, 7.3, rows, [3.0, 4.3], row_h=0.68, fs=9.5)

rect(s, M + 7.55, BODY_TOP - 0.02, 4.28, 4.55, fill=WHITE, line_color=LINE, radius=0.05)
rect(s, M + 7.55, BODY_TOP - 0.02, 4.28, 0.055, fill=AMBER)
para(textbox(s, M + 7.85, BODY_TOP + 0.26, 3.7, 0.32), "明确不做", size=13.5,
     bold=True, color=INK, first=True, after=0)
tf = textbox(s, M + 7.85, BODY_TOP + 0.72, 3.7, 3.6)
for i, t in enumerate([
        "不做全文检索（需另立项并启用 sqlite3[fts5]）",
        "不做下载加速 / 断点续传",
        "不做云盘双向同步 / 跨设备",
        "不接管「另存为」到任意目录",
        "不自动删除任何文件",
        "不猜测文件去向",
        "不做归档代理",
        "不做全局 EventBus（只加 FileSystemEvent + 极轻量分发器）"]):
    para(tf, "✕  " + t, size=10, color=MUTED, first=(i == 0), after=7, line=1.2)

rect(s, M, 5.98, 7.3, 0.76, fill=RED_L, radius=0.05)
rich(textbox(s, M + 0.3, 5.98, 6.7, 0.76, anchor=MSO_ANCHOR.MIDDLE),
     [("复杂度警告：", True, RED),
      ("① 目录级剪枝必须先判父目录再下探，天然要求自建迭代栈 —— "
       "这正是 A1/A2 必须先落地的原因；② 时序错（扫描未完就反查）会导致候选集为空、"
       "幽灵条目堆积。", False, INK)], size=10, first=True, after=0, line=1.28)

# ---------------------------------------------------------------- 20 下一步
s = nxt(blank(prs, NAVY), dark=True)
rect(s, 0, 0, 0.17, H, fill=BLUE)
header(s, "NEXT", "下一步", dark=True)
steps = [
    ("现在", "A1 已落地，等 review / PR",
     "分支 fix/a1-stop-bleeding：1 个 fix commit + 1 个 docs commit；\n"
     "已在隔离 worktree 单独编译 + 冒烟通过", GREEN),
    ("接着", "Phase 3 真机走查 → tag v0.3.0",
     "大改之前先有一个已知良好、可回退的 tag；\n"
     "顺带可插 1–2 天打包 spike（vcpkg Qt 部署 + NSIS）", BLUE),
    ("然后", "A2 结构：门面 + COW 快照 + 后台扫描",
     "先验证 MSVC 是否支持 std::atomic<std::shared_ptr>（10 分钟，别跳过），\n"
     "不支持就换 std::shared_mutex", AMBER),
    ("之后", "A3 采集 → A4 baseline → A5 增量 → A6 冷启动 → A7 融合",
     "A4 的 baseline 数据是后面所有优化决策的依据", BLUE),
]
y = 1.95
for tag, t, d, ac in steps:
    rect(s, M, y, CW, 1.08, fill=NAVY2, radius=0.05)
    rect(s, M, y, 0.055, 1.08, fill=ac)
    chip(s, M + 0.3, y + 0.38, 0.85, 0.32, tag, ac, WHITE, 10)
    para(textbox(s, M + 1.35, y + 0.16, 10.2, 0.38), t, size=14, bold=True,
         color=WHITE, first=True, after=0)
    para(textbox(s, M + 1.35, y + 0.56, 10.2, 0.46), d, size=10, color=MUTED_L,
         first=True, after=0, line=1.25)
    y += 1.2

para(textbox(s, M, 6.72, CW - 1.2, 0.4),
     "依据：docs/architecture/{文件检索重构方案, 文件治理功能设计, v0.4-实施清单}.md ｜ "
     "docs/devlog/2026-09-24.md ｜ docs/troubleshooting/qt-nosymlinks-挡不住junction.md",
     size=9.5, color=RGBColor(0x6E, 0x7C, 0x92), first=True, after=0)

OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                   "文件检索与治理模块.pptx")
prs.save(OUT)
print("saved:", OUT, os.path.getsize(OUT), "bytes,",
      len(prs.slides._sldIdLst), "slides")
