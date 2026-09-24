# -*- coding: utf-8 -*-
"""Assistant 文件管理模块 —— 【设计理念篇】PPT 生成脚本
主线：从项目初期的一个朴素想法，到分阶段落地的文件子系统
用法： python make_deck_design.py
"""
import os
from pptx import Presentation
from pptx.util import Inches, Pt
from pptx.dml.color import RGBColor
from pptx.enum.text import PP_ALIGN, MSO_ANCHOR
from pptx.enum.shapes import MSO_SHAPE
from pptx.oxml.ns import qn

FONT   = "Microsoft YaHei"
NAVY   = RGBColor(0x14, 0x22, 0x38)
NAVY2  = RGBColor(0x1E, 0x30, 0x4C)
BLUE   = RGBColor(0x4A, 0x90, 0xD9)
BLUE_D = RGBColor(0x2C, 0x5F, 0x9E)
BLUE_L = RGBColor(0xE8, 0xF1, 0xFB)
INK    = RGBColor(0x1A, 0x1A, 0x1A)
MUTED  = RGBColor(0x5E, 0x6A, 0x7A)
MUTED_L= RGBColor(0x8E, 0x9A, 0xAD)
LINE   = RGBColor(0xD8, 0xDE, 0xE6)
WHITE  = RGBColor(0xFF, 0xFF, 0xFF)
GREEN  = RGBColor(0x2E, 0x8B, 0x57)
GREEN_L= RGBColor(0xE7, 0xF4, 0xEC)
AMBER  = RGBColor(0xB5, 0x6E, 0x00)
AMBER_L= RGBColor(0xFD, 0xF3, 0xE2)
RED    = RGBColor(0xC0, 0x39, 0x2B)
RED_L  = RGBColor(0xFB, 0xEC, 0xEA)
PURPLE = RGBColor(0x6B, 0x4E, 0xA8)
PURPLE_L = RGBColor(0xF0, 0xEB, 0xF9)
BG     = RGBColor(0xF7, 0xF9, 0xFC)
ZEBRA  = RGBColor(0xF1, 0xF5, 0xFA)

W, H = 13.333, 7.5
M, CW = 0.75, 11.833
BODY_TOP = 1.80

def style_font(run, size=None, bold=None, color=None, name=FONT, italic=None):
    run.font.name = name
    rPr = run._r.get_or_add_rPr()
    for tag in ('a:ea', 'a:cs'):
        el = rPr.find(qn(tag))
        if el is None:
            el = rPr.makeelement(qn(tag), {}); rPr.append(el)
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

def para(tf, text, size=13, bold=False, color=INK, first=False, before=0, after=5,
         align=PP_ALIGN.LEFT, line=1.25, italic=False, font=FONT):
    p = tf.paragraphs[0] if first else tf.add_paragraph()
    p.alignment = align
    p.space_before = Pt(before); p.space_after = Pt(after); p.line_spacing = line
    r = p.add_run(); r.text = text
    style_font(r, size=size, bold=bold, color=color, italic=italic, name=font)
    return p

def rich(tf, segs, size=13, first=False, after=5, line=1.3, align=PP_ALIGN.LEFT):
    p = tf.paragraphs[0] if first else tf.add_paragraph()
    p.alignment = align
    p.space_before = Pt(0); p.space_after = Pt(after); p.line_spacing = line
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

def chip(slide, x, y, w, h, text, fill, txt_color, size=12, bold=True):
    rect(slide, x, y, w, h, fill=fill, radius=0.4)
    para(textbox(slide, x, y, w, h, anchor=MSO_ANCHOR.MIDDLE), text, size=size,
         bold=bold, color=txt_color, first=True, align=PP_ALIGN.CENTER, after=0)

def blank(prs, bg=BG):
    s = prs.slides.add_slide(prs.slide_layouts[6])
    rect(s, 0, 0, W, H, fill=bg)
    return s

def header(slide, kicker, title, dark=False):
    para(textbox(slide, M, 0.42, CW, 0.32), kicker, size=12, bold=True,
         color=BLUE, first=True, after=0)
    para(textbox(slide, M, 0.76, CW, 0.66), title, size=26, bold=True,
         color=WHITE if dark else INK, first=True, after=0)
    rect(slide, M, 1.50, 1.05, 0.045, fill=BLUE)

def footnote(slide, text, dark=False):
    para(textbox(slide, M, 6.98, CW - 1.3, 0.32), text, size=10.5,
         color=MUTED_L if dark else MUTED, first=True, after=0)

def pageno(slide, n, dark=False):
    para(textbox(slide, W - 1.15, 6.98, 0.5, 0.32), f"{n:02d}", size=11, bold=True,
         color=MUTED_L if dark else MUTED, first=True, align=PP_ALIGN.RIGHT, after=0)

def arrow(slide, x, y, color=BLUE, size=15):
    para(textbox(slide, x, y, 0.34, 0.42, anchor=MSO_ANCHOR.MIDDLE), "▶",
         size=size, color=color, first=True, align=PP_ALIGN.CENTER, after=0)

def panel(slide, x, y, w, h, title, lines, accent=BLUE, tsize=15, bsize=12,
          fill=WHITE, gap=7):
    rect(slide, x, y, w, h, fill=fill, line_color=LINE, radius=0.05)
    rect(slide, x, y + 0.16, 0.055, h - 0.32, fill=accent)
    para(textbox(slide, x + 0.32, y + 0.24, w - 0.6, 0.4), title, size=tsize,
         bold=True, color=INK, first=True, after=0)
    tf = textbox(slide, x + 0.32, y + 0.82, w - 0.62, h - 1.0)
    for i, ln in enumerate(lines):
        para(tf, ln, size=bsize, color=MUTED, first=(i == 0), after=gap, line=1.32)
    return tf

prs = Presentation()
prs.slide_width, prs.slide_height = Inches(W), Inches(H)
PG = [0]
def nxt(s, dark=False):
    PG[0] += 1; pageno(s, PG[0], dark); return s

# ================================================================ 01 封面
s = blank(prs, NAVY)
rect(s, 0, 0, 0.17, H, fill=BLUE)
para(textbox(s, 1.05, 2.15, 7.2, 0.36), "ASSISTANT · 设计篇", size=14, bold=True,
     color=BLUE, first=True, after=0)
para(textbox(s, 1.05, 2.65, 8.4, 1.0), "文件管理模块", size=44, bold=True,
     color=WHITE, first=True, after=0)
para(textbox(s, 1.05, 3.82, 8.2, 0.5),
     "从「输入文件名就能打开」，到一个分五阶段落地的文件子系统",
     size=16, color=RGBColor(0xB9, 0xC6, 0xD8), first=True, after=0)
rect(s, 1.05, 4.62, 3.0, 0.03, fill=RGBColor(0x35, 0x4A, 0x66))
para(textbox(s, 1.05, 4.92, 8.2, 0.8), "设计理念  ·  设计想法  ·  模块特点",
     size=14, color=MUTED_L, first=True, after=0, line=1.4)

rect(s, 9.55, 2.15, 2.95, 3.4, fill=NAVY2, radius=0.05)
para(textbox(s, 9.9, 2.48, 2.3, 0.32), "时间线", size=12.5, bold=True,
     color=BLUE, first=True, after=0)
tf = textbox(s, 9.9, 2.92, 2.35, 2.5)
for i, (d, t) in enumerate([("2026-08-15", "第一版文件搜索"),
                            ("2026-08-25", "市场调研与构思"),
                            ("2026-09-21", "架构设计定稿"),
                            ("2026-09-24", "A1 止血落地"),
                            ("下一阶段", "搜索基建 A2–A7")]):
    rich(tf, [(d + "  ", True, BLUE), (t, False, RGBColor(0xC7, 0xD4, 0xE6))],
         size=10.5, first=(i == 0), after=9, line=1.25)

# ================================================================ 02 讲什么
s = nxt(blank(prs))
header(s, "CONTENTS", "这份 PPT 讲四件事")
items = [
    ("01", "起点", "文件搜索最初是怎么做的，\n当时定下了哪些取舍", BLUE, BLUE_L),
    ("02", "想法", "为什么它从一个搜索框，\n长成一个文件子系统", GREEN, GREEN_L),
    ("03", "理念", "三条核心设计取向：\n分层、分阶段、正确性优先", AMBER, AMBER_L),
    ("04", "特点", "与同类产品不同的地方，\n以及能力蓝图与落地节奏", PURPLE, PURPLE_L),
]
for i, (num, t, d, ac, al) in enumerate(items):
    x = M + i * (CW / 4)
    w = CW / 4 - 0.22
    rect(s, x, 2.2, w, 3.5, fill=WHITE, line_color=LINE, radius=0.05)
    rect(s, x, 2.2, w, 0.06, fill=ac)
    para(textbox(s, x + 0.3, 2.55, w - 0.6, 0.6), num, size=30, bold=True,
         color=ac, first=True, after=0)
    para(textbox(s, x + 0.3, 3.35, w - 0.6, 0.4), t, size=16, bold=True,
         color=INK, first=True, after=0)
    para(textbox(s, x + 0.3, 3.9, w - 0.6, 1.5), d, size=11.5, color=MUTED,
         first=True, after=0, line=1.4)

# ================================================================ 03 起点
s = nxt(blank(prs))
header(s, "起点", "项目初期的三步：文件搜索是「助手能用」的最后一块拼图")
stages = [("Phase 0", "环境搭建", "Hello 窗口跑起来\n工具链打通", "第 1 周", MUTED, ZEBRA),
          ("Phase 1", "MVP 原型", "托盘 + 全局热键 + 搜索浮窗\n先让「一键呼出」成立", "第 2–4 周", BLUE, BLUE_L),
          ("Phase 2", "助手能用", "日志 / 历史库 / 模糊匹配 /\n计算器 / 应用启动器 / 文件搜索", "第 5–8 周\n→ v0.2.0", GREEN, GREEN_L)]
bw = (CW - 2 * 0.3) / 3
for i, (ph, name, desc, wk, ac, al) in enumerate(stages):
    x = M + i * (bw + 0.3)
    rect(s, x, BODY_TOP + 0.2, bw, 3.25, fill=WHITE, line_color=LINE, radius=0.05)
    rect(s, x, BODY_TOP + 0.2, bw, 0.06, fill=ac)
    chip(s, x + 0.32, BODY_TOP + 0.5, 1.35, 0.4, ph, al, ac, 12.5)
    para(textbox(s, x + 0.32, BODY_TOP + 1.08, bw - 0.64, 0.42), name, size=17,
         bold=True, color=INK, first=True, after=0)
    para(textbox(s, x + 0.32, BODY_TOP + 1.6, bw - 0.64, 1.0), desc, size=12,
         color=MUTED, first=True, after=0, line=1.35)
    para(textbox(s, x + 0.32, BODY_TOP + 2.72, bw - 0.64, 0.6), wk, size=11,
         bold=True, color=ac, first=True, after=0, line=1.25)
    if i < 2:
        arrow(s, x + bw + 0.005, BODY_TOP + 1.5, size=14)

rect(s, M, 5.5, CW, 1.0, fill=BLUE_L, radius=0.05)
rich(textbox(s, M + 0.4, 5.5, CW - 0.8, 1.0, anchor=MSO_ANCHOR.MIDDLE),
     [("第一版文件搜索的目标非常朴素：", True, BLUE_D),
      ("输入文件名（比如 report.docx、照片）→ 模糊匹配用户目录 → 回车用默认程序打开。"
       "当时没人打算做「文件管理」，只是想让搜索框里多一路结果。", False, INK)],
     size=13, first=True, after=0, line=1.35)

# ================================================================ 04 最初的想法
s = nxt(blank(prs))
header(s, "设计想法 · 第一版", "当年的四个决定，和两条主动写下的取舍")
panel(s, M, BODY_TOP, 6.9, 2.35, "四个设计决定（2026-08-15）",
      ["①  内存索引：启动时建一次，之后每次按键只扫内存，不再递归磁盘",
       "②  只收六个常用目录：桌面 / 文档 / 下载 / 图片 / 音乐 / 视频",
       "③  跳过巨目录：node_modules、__pycache__ 等",
       "④  上限 2 万条；文件不进默认列表（否则空查询会拖死界面）"],
      accent=BLUE, tsize=14.5, bsize=11.5, gap=5)

rect(s, M + 7.15, BODY_TOP, 4.68, 2.35, fill=AMBER_L, radius=0.05)
rect(s, M + 7.15, BODY_TOP, 0.055, 2.35, fill=AMBER)
para(textbox(s, M + 7.45, BODY_TOP + 0.22, 4.1, 0.35), "当时就写下的两条取舍",
     size=14.5, bold=True, color=AMBER, first=True, after=0)
tf = textbox(s, M + 7.45, BODY_TOP + 0.72, 4.1, 1.5)
rich(tf, [("① 启动延迟：", True, INK),
          ("建索引在窗口显示前同步执行，目录很大时会慢。", False, MUTED)],
     size=11, first=True, after=9)
rich(tf, [("② 遍历浪费：", True, INK),
          ("QDirIterator 无法跳过子树 ——「若未来遇到大目录，可改为手写递归精确剪枝」。",
           False, MUTED)], size=11, after=0)

rect(s, M, 4.45, CW, 2.05, fill=WHITE, line_color=LINE, radius=0.05)
rect(s, M, 4.45, 0.055, 2.05, fill=GREEN)
para(textbox(s, M + 0.35, 4.65, CW - 0.7, 0.35), "为什么这些取舍值得单独记一笔",
     size=14.5, bold=True, color=INK, first=True, after=0)
tf = textbox(s, M + 0.35, 5.12, CW - 0.7, 1.2)
rich(tf, [("因为没有一条是「以后再说」——", False, MUTED),
          ("每条都写清了「为什么这么选」和「什么情况下会失效」", True, GREEN),
          ("。三个月后再回头看，这两条取舍直接变成了施工图。", False, MUTED)],
     size=12.5, first=True, after=0)

# ================================================================ 05 预判应验
s = nxt(blank(prs))
header(s, "设计想法 · 应验", "当初写下的两条取舍，后来都真的发生了")
rows = [
    ["当初写下的取舍", "后来实测到的情况", "怎么处理的"],
    ["「建索引在窗口显示前同步执行，\n目录很大时会慢」",
     "本机实测 0.7–2.4 秒，窗口要等索引建完\n才出现；桌面一个目录就有 3.5 万个文件",
     "列入后续阶段：\n后台扫描 + 冷启动先可用"],
    ["「QDirIterator 无法跳过子树，\n未来可改为手写递归精确剪枝」",
     "一个特殊目录（指向父目录的链接）造成\n126 条重复索引，路径叠了 63 层",
     "A1 已按当初设想的方向\n改成手写递归 + 剪枝"],
]
from pptx.util import Inches as In
n_r, n_c = len(rows), len(rows[0])
gt = s.shapes.add_table(n_r, n_c, In(M), In(BODY_TOP), In(CW), In(0.5 + 0.95 * (n_r - 1))).table
gt.first_row = False; gt.horz_banding = False
for i, cw in enumerate([4.0, 4.6, 3.233]):
    gt.columns[i].width = In(cw)
gt.rows[0].height = In(0.5)
for r in range(1, n_r): gt.rows[r].height = In(0.95)
for r, row in enumerate(rows):
    for c, val in enumerate(row):
        cell = gt.cell(r, c)
        cell.margin_left = In(0.14); cell.margin_right = In(0.1)
        cell.margin_top = In(0.04);  cell.margin_bottom = In(0.04)
        cell.vertical_anchor = MSO_ANCHOR.MIDDLE
        cell.fill.solid()
        cell.fill.fore_color.rgb = NAVY if r == 0 else (WHITE if r % 2 else ZEBRA)
        tf = cell.text_frame; tf.word_wrap = True
        p = tf.paragraphs[0]; p.line_spacing = 1.12
        run = p.add_run(); run.text = str(val)
        col = WHITE if r == 0 else (MUTED if c == 0 else INK)
        style_font(run, size=13 if r == 0 else 11.5, bold=(r == 0), color=col)

rect(s, M, 4.5, CW, 2.0, fill=GREEN_L, radius=0.05)
para(textbox(s, M + 0.4, 4.78, CW - 0.8, 0.5),
     "好的取舍记录，就是后来的施工图。", size=21, bold=True, color=GREEN,
     first=True, after=0)
tf = textbox(s, M + 0.4, 5.38, CW - 0.8, 0.9)
rich(tf, [("写下取舍的人，和三个月后来修它的人，往往是同一个 —— 但那时已经记不清当初为什么了。"
           "文档里那句「若未来遇到大目录，可改为手写递归」，让 A1 不用重新做一次决策，"
           "只需要执行它。", False, MUTED)], size=12.5, first=True, after=0, line=1.4)

# ================================================================ 06 认识升级
s = nxt(blank(prs))
header(s, "设计想法 · 升级", "想清楚一件事：文件搜索 ≠ 文件管理")
panel(s, M, BODY_TOP, 5.65, 2.5, "过去的边界",
      ["一句话：在搜索框里多加一路结果。",
       "文件只是 SearchResult 里的一种 type。",
       "能搜到就够 —— 搜不到，用户自己也没办法。"],
      accent=MUTED, tsize=15, bsize=12, gap=8)
arrow(s, M + 5.72, BODY_TOP + 1.05, size=18)
panel(s, M + 6.18, BODY_TOP, 5.65, 2.5, "现在的边界",
      ["一句话：一个独立的文件子系统。",
       "搜索框只是它对外的一个出口，不是全部。",
       "要同时保证两件相反的事：找得到 + 不漏掉。"],
      accent=GREEN, tsize=15, bsize=12, gap=8)

rect(s, M, 4.6, CW, 2.05, fill=WHITE, line_color=LINE, radius=0.05)
rect(s, M, 4.6, 0.055, 2.05, fill=PURPLE)
para(textbox(s, M + 0.35, 4.8, CW - 0.7, 0.35), "由此得到最核心的一条设计理念：把「面」分开",
     size=14.5, bold=True, color=INK, first=True, after=0)
row = [("检索面", "让用户「找到想要的」", "查询 · 打分 · 排序 · 融合", BLUE, BLUE_L),
       ("采集面", "保证「不漏掉文件」", "扫描 · 归类 · 排除 · 追踪", GREEN, GREEN_L)]
for i, (t, d, k, ac, al) in enumerate(row):
    x = M + 0.38 + i * 5.6
    rect(s, x, 5.25, 5.35, 1.15, fill=al, radius=0.06)
    para(textbox(s, x + 0.3, 5.38, 4.8, 0.36), t, size=15, bold=True, color=ac,
         first=True, after=0)
    rich(textbox(s, x + 0.3, 5.78, 4.8, 0.55),
         [(d + "　·　", False, INK), (k, True, MUTED)], size=10.5, first=True, after=0)

# ================================================================ 07 两种过滤
s = nxt(blank(prs))
header(s, "设计理念 ①", "两种「过滤」必须分清 —— 这是整个设计的枢纽")
blocks = [("采集过滤", "决定「什么进数据库」",
           ["根目录、目录级排除、工程目录识别",
            "改动代价：要重新扫盘，慢",
            "所以必须在界面上明确提示「正在重建索引」"],
           RED, RED_L, "慢 · 要扫盘"),
          ("检索过滤", "决定「内存快照装什么」",
           ["种类白名单：只要文档 / 只要图片 / 全都要",
            "改动代价：从数据库重建快照，数十毫秒",
            "所以可以做成界面上一个随时可点的开关"],
           GREEN, GREEN_L, "快 · 不扫盘")]
for i, (t, sub, lines, ac, al, tag) in enumerate(blocks):
    x = M + i * 6.13
    rect(s, x, BODY_TOP, 5.7, 2.85, fill=WHITE, line_color=LINE, radius=0.05)
    rect(s, x, BODY_TOP, 5.7, 0.06, fill=ac)
    para(textbox(s, x + 0.35, BODY_TOP + 0.28, 4.0, 0.4), t, size=17, bold=True,
         color=ac, first=True, after=0)
    chip(s, x + 4.35, BODY_TOP + 0.28, 1.05, 0.4, tag,
         al, ac, 10)
    para(textbox(s, x + 0.35, BODY_TOP + 0.82, 5.0, 0.35), sub, size=12.5,
         color=INK, first=True, after=0)
    tf = textbox(s, x + 0.35, BODY_TOP + 1.3, 5.0, 1.4)
    for j, ln in enumerate(lines):
        para(tf, "·  " + ln, size=11.5, color=MUTED, first=(j == 0), after=7, line=1.3)

rect(s, M, 4.95, CW, 1.62, fill=WHITE, line_color=LINE, radius=0.05)
rect(s, M, 4.95, 0.055, 1.62, fill=PURPLE)
tf = textbox(s, M + 0.35, 5.15, CW - 0.7, 1.3)
rich(tf, [("分界线是 ", False, MUTED), ("files 表", True, BLUE_D),
          ("：能进表的数据，都是「磁盘上真实存在」的全量结果（只受排除规则约束）；"
           "用户看到的列表，才是按白名单筛过的。", False, MUTED)], size=12.5,
     first=True, after=9)
rich(tf, [("一条重要例外：", True, RED),
          ("下载收件箱不服从白名单。否则用户把白名单设成「只要 PDF」，"
           "刚下载的安装包就会从收件箱里消失 —— 那是数据丢失级的事故，"
           "不是筛选问题。", False, MUTED)], size=12.5, after=0)

# ================================================================ 08 分阶段
s = nxt(blank(prs))
header(s, "设计理念 ②", "架构一次设计完整，功能分阶段落地")
panel(s, M, BODY_TOP, 5.7, 2.6, "接口先定死",
      ["门面：只做「持依赖 / 转发 / 持快照 / 暴露状态」四件事",
       "快照：查询层只能通过只读原语访问数据",
       "Provider：A7 之后插件也挂在同一个接口上"],
      accent=BLUE, tsize=15, bsize=11.5, gap=6)
panel(s, M + 6.13, BODY_TOP, 5.7, 2.6, "能力慢慢加",
      ["后面的位图预筛、缓存、SoA 存储都是可选优化",
       "加它们不改调用方 —— 因为表示藏在接口之后",
       "所以第一阶段可以只用最朴素、最不容易错的实现"],
      accent=GREEN, tsize=15, bsize=11.5, gap=6)

rect(s, M, 4.7, CW, 1.87, fill=WHITE, line_color=LINE, radius=0.05)
rect(s, M, 4.7, 0.055, 1.87, fill=AMBER)
para(textbox(s, M + 0.35, 4.9, CW - 0.7, 0.35), "配套的三条工程习惯",
     size=14.5, bold=True, color=INK, first=True, after=0)
tf = textbox(s, M + 0.35, 5.36, CW - 0.7, 1.1)
rich(tf, [("① 每步都能独立编译、独立提交、独立验收；", True, INK),
          ("② 旧实现始终留一条可用退路（一个开关就能回退）；", True, INK),
          ("③ 先量基准再谈优化 —— 不做没有实测支撑的性能改造。", True, INK)],
     size=12.5, first=True, after=0, line=1.5)

# ================================================================ 09 正确性优先
s = nxt(blank(prs))
header(s, "设计理念 ③", "正确性优先于资源：宁可不给答案，也不给错答案")
rules = [("索引满了也不驱逐", "早期想过「按使用频率淘汰」，已明确废弃 —— 被淘汰后用户搜不到自己存在的文件，\n"
                            "行为上等于「文件凭空消失」。改为：软预算告警、硬上限停止并提示。", RED, RED_L),
         ("去向不明就说不知道", "文件被移走后，索引只知道「它走了」，不知道「它去哪了」。\n"
                              "多候选一律标记为未知，绝不猜一个看起来像的位置。", AMBER, AMBER_L),
         ("默认只建议，不自动动手", "不自动删除任何文件；不自动移动（默认只给归档建议）；\n"
                                  "所有移动都留日志、可撤销、同名冲突零覆盖。", GREEN, GREEN_L)]
y = BODY_TOP
for t, d, ac, al in rules:
    rect(s, M, y, CW, 1.28, fill=WHITE, line_color=LINE, radius=0.05)
    rect(s, M, y, 0.055, 1.28, fill=ac)
    para(textbox(s, M + 0.4, y + 0.2, 5.0, 0.4), t, size=15.5, bold=True,
         color=ac, first=True, after=0)
    para(textbox(s, M + 0.4, y + 0.66, CW - 0.85, 0.55), d, size=11.5,
         color=MUTED, first=True, after=0, line=1.28)
    y += 1.4

rect(s, M, 6.02, CW, 0.7, fill=BLUE_L, radius=0.05)
rich(textbox(s, M + 0.4, 6.02, CW - 0.8, 0.7, anchor=MSO_ANCHOR.MIDDLE),
     [("为什么把它当第一原则：", True, BLUE_D),
      ("文件是用户的真实资产。搜索工具出错，用户只会觉得「不好用」；"
       "文件工具出错，用户会丢东西。", False, INK)], size=12.5, first=True, after=0)

# ================================================================ 10 特点
s = nxt(blank(prs))
header(s, "模块特点", "和同类工具不一样在哪")
feats = [("纯本地 · 无外部依赖",
          ["不引入全文检索引擎，不依赖云服务",
           "只用 Qt + SQLite + Win32，产物是一个可执行文件"], BLUE),
         ("查询不碰数据库",
          ["数据以不可变内存快照发布，读的一侧完全免锁",
           "数据库只负责「冷启动加速」和「使用频率存储」"], GREEN),
         ("键盘优先 · 中文友好",
          ["输入即搜，40ms 防抖；输入法预编辑阶段不触发检索",
           "后续可开启拼音首字母（默认关闭）"], AMBER),
         ("每一步都可退",
          ["新旧实现共存，一个配置项即可回退",
           "A1 是一个独立补丁，可单独回滚"], PURPLE)]
for i, (t, lines, ac) in enumerate(feats):
    x = M + (i % 2) * (CW / 2 + 0.12)
    y = BODY_TOP + (i // 2) * 2.35
    panel(s, x, y, CW / 2 - 0.12, 2.15, t, ["·  " + lines[0], "·  " + lines[1]],
          accent=ac, tsize=15, bsize=11.5, gap=8)

# ================================================================ 11 能力蓝图
s = nxt(blank(prs))
header(s, "能力蓝图", "用户视角的四层能力：找得到 → 可控 → 不丢 → 会打理")
layers = [("找得到", "文件名模糊匹配（已有）→ 路径参与、子序列、分词", GREEN, "v0.4.0"),
          ("可控", "种类过滤、工程目录排除、查询语法（只要文档 / ext:pdf）", BLUE, "v0.4.x"),
          ("不丢", "下载收件箱：下载完成自动收纳；文件移走改名也能找回", AMBER, "v0.4.x–v0.5.x"),
          ("会打理", "归档建议（可选自动）、重复文件查重、接入聊天软件来源", PURPLE, "v0.5.x")]
y = BODY_TOP + 0.15
for t, d, ac, ver in layers:
    rect(s, M, y, CW, 0.92, fill=WHITE, line_color=LINE, radius=0.05)
    rect(s, M, y, 0.055, 0.92, fill=ac)
    chip(s, M + 0.35, y + 0.25, 1.25, 0.42, t, ac, WHITE, 13)
    para(textbox(s, M + 1.85, y, 8.0, 0.92, anchor=MSO_ANCHOR.MIDDLE), d,
         size=13, color=INK, first=True, after=0, line=1.3)
    para(textbox(s, M + 9.9, y, 1.6, 0.92, anchor=MSO_ANCHOR.MIDDLE), ver,
         size=11.5, bold=True, color=ac, first=True, align=PP_ALIGN.RIGHT, after=0)
    y += 1.03

rect(s, M, 6.08, CW, 0.66, fill=BLUE_L, radius=0.05)
rich(textbox(s, M + 0.4, 6.08, CW - 0.8, 0.66, anchor=MSO_ANCHOR.MIDDLE),
     [("四层之间的关系：", True, BLUE_D),
      ("「找得到」是地基，「可控」让结果不吵，「不丢」和「会打理」才是文件管理真正的价值。",
       False, INK)], size=12.5, first=True, after=0)

# ================================================================ 12 落地节奏
s = nxt(blank(prs))
header(s, "落地节奏", "五个阶段：搜索基建 → 过滤排除 → 下载中转 → 去向追踪 → 自动治理")
stages = [("A", "搜索基建", "索引 · 查询\n实时更新", "v0.4.0\nA1 已完成", BLUE, BLUE_L),
          ("B", "过滤与排除", "种类过滤\n工程排除", "v0.4.x", BLUE, BLUE_L),
          ("C", "下载中转", "下载收件箱\n一键归档", "v0.4.x\n–v0.5.x", AMBER, AMBER_L),
          ("D", "去向追踪", "移走改名\n也能找到", "v0.5.x", AMBER, AMBER_L),
          ("E", "自动治理", "归档建议\n查重 · 多来源", "v0.5.x", GREEN, GREEN_L)]
bw = (CW - 4 * 0.24) / 5
for i, (code, name, desc, ver, ac, al) in enumerate(stages):
    x = M + i * (bw + 0.24)
    rect(s, x, BODY_TOP + 0.3, bw, 3.3, fill=WHITE, line_color=LINE, radius=0.05)
    rect(s, x, BODY_TOP + 0.3, bw, 0.06, fill=ac)
    chip(s, x + 0.3, BODY_TOP + 0.58, 0.5, 0.5, code, ac, WHITE, 17)
    para(textbox(s, x + 0.3, BODY_TOP + 1.26, bw - 0.6, 0.4), name, size=15,
         bold=True, color=INK, first=True, after=0)
    para(textbox(s, x + 0.3, BODY_TOP + 1.78, bw - 0.6, 0.85), desc, size=11.5,
         color=MUTED, first=True, after=0, line=1.3)
    para(textbox(s, x + 0.3, BODY_TOP + 2.9, bw - 0.6, 0.6), ver, size=10.5,
         bold=True, color=ac, first=True, after=0, line=1.2)
    if i < 4:
        arrow(s, x + bw + 0.005, BODY_TOP + 1.85, size=13)

rect(s, M, 5.85, CW, 0.85, fill=GREEN_L, radius=0.05)
rich(textbox(s, M + 0.4, 5.85, CW - 0.8, 0.85, anchor=MSO_ANCHOR.MIDDLE),
     [("当前进度：", True, GREEN),
      ("A 阶段的第一步（A1 止血）已经落地并通过验收；接下来的 A2–A3 会重建索引，"
       "同时解决「文档目录搜不到」这个问题。", False, INK)], size=12.5, first=True, after=0)

# ================================================================ 13 收尾
s = nxt(blank(prs, NAVY), dark=True)
rect(s, 0, 0, 0.17, H, fill=BLUE)
para(textbox(s, M, 0.85, CW, 0.35), "SUMMARY", size=12, bold=True, color=BLUE,
     first=True, after=0)
para(textbox(s, M, 1.3, 11.0, 1.6),
     "从一个搜索框里的一路结果，\n到一个分五阶段落地的文件子系统。",
     size=27, bold=True, color=WHITE, first=True, after=0, line=1.3)

points = [("变的", "范围：从「能搜到文件名」扩到「文件不丢、可控、会打理」", BLUE),
          ("不变的", "那条原则：宁可不给答案，也不给错答案", GREEN),
          ("沉淀下来的", "每条取舍都写清「为什么」和「何时失效」，它就成了后来的施工图", AMBER)]
y = 3.35
for t, d, ac in points:
    rect(s, M, y, CW, 0.86, fill=NAVY2, radius=0.05)
    rect(s, M, y, 0.055, 0.86, fill=ac)
    para(textbox(s, M + 0.4, y, 1.3, 0.86, anchor=MSO_ANCHOR.MIDDLE), t,
         size=13.5, bold=True, color=ac, first=True, after=0)
    para(textbox(s, M + 1.85, y, 9.6, 0.86, anchor=MSO_ANCHOR.MIDDLE), d,
         size=12.5, color=RGBColor(0xC7, 0xD4, 0xE6), first=True, after=0, line=1.3)
    y += 1.0

para(textbox(s, M, 6.72, CW - 1.3, 0.4),
     "依据：docs/devlog/2026-08-15.md ｜ docs/features/市场调研与功能构思.md ｜ "
     "docs/architecture/ 三份设计稿 ｜ docs/devlog/2026-09-24.md",
     size=10, color=RGBColor(0x6E, 0x7C, 0x92), first=True, after=0)

OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "文件管理模块-设计理念.pptx")
prs.save(OUT)
print("saved:", OUT, os.path.getsize(OUT), "bytes,", len(prs.slides._sldIdLst), "slides")
