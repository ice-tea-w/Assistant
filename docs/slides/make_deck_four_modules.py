# -*- coding: utf-8 -*-
"""Assistant 文件系统 —— 【四模块：实现与交互关系】PPT 生成脚本（19 页）

内容来源：docs/architecture/文件系统四模块-关联与方案.md（提案 v2，已合并架构评审修订）
视角：不是"有哪些功能"，而是"四个模块各自实现什么、彼此靠什么连、为什么这么连"。

用法： python make_deck_four_modules.py
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
VIOLET = RGBColor(0x5B, 0x4B, 0x8A)
VIOLET_L = RGBColor(0xEE, 0xEB, 0xF6)
BG     = RGBColor(0xF7, 0xF9, 0xFC)
ZEBRA  = RGBColor(0xF1, 0xF5, 0xFA)

# 四个模块的视觉编码（贯穿全程）
C_M1, C_M2, C_M3, C_M4 = BLUE, GREEN, RED, VIOLET

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


def para(tf, text, size=14, bold=False, color=INK, first=False, before=0, after=5,
         align=PP_ALIGN.LEFT, line=1.2, italic=False, font=FONT):
    p = tf.paragraphs[0] if first else tf.add_paragraph()
    p.alignment = align
    p.space_before = Pt(before); p.space_after = Pt(after); p.line_spacing = line
    r = p.add_run(); r.text = text
    style_font(r, size=size, bold=bold, color=color, italic=italic, name=font)
    return p


def rich(tf, segs, size=13, first=False, after=5, line=1.25, align=PP_ALIGN.LEFT):
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
    para(textbox(slide, M, 0.76, CW, 0.66), title, size=27, bold=True,
         color=WHITE if dark else INK, first=True, after=0)
    rect(slide, M, 1.50, 1.05, 0.045, fill=BLUE)


def footnote(slide, text, dark=False):
    para(textbox(slide, M, 6.98, CW - 1.3, 0.32), text, size=10.5,
         color=MUTED_L if dark else MUTED, first=True, after=0)


def pageno(slide, n, dark=False):
    para(textbox(slide, W - 1.15, 6.98, 0.5, 0.32), f"{n:02d}", size=11, bold=True,
         color=MUTED_L if dark else MUTED, first=True, align=PP_ALIGN.RIGHT, after=0)


def table(slide, x, y, w, rows, col_w, row_h=0.5, head_h=0.44, fs=13, head_fs=12.5,
          colors=None, aligns=None):
    n_r, n_c = len(rows), len(rows[0])
    gt = slide.shapes.add_table(n_r, n_c, Inches(x), Inches(y), Inches(w),
                                Inches(head_h + row_h * (n_r - 1))).table
    gt.first_row = False; gt.horz_banding = False
    for i, cw in enumerate(col_w): gt.columns[i].width = Inches(cw)
    gt.rows[0].height = Inches(head_h)
    for r in range(1, n_r): gt.rows[r].height = Inches(row_h)
    for r, row in enumerate(rows):
        for c, val in enumerate(row):
            cell = gt.cell(r, c)
            cell.margin_left = Inches(0.11); cell.margin_right = Inches(0.08)
            cell.margin_top = Inches(0.03);  cell.margin_bottom = Inches(0.03)
            cell.vertical_anchor = MSO_ANCHOR.MIDDLE
            cell.fill.solid()
            cell.fill.fore_color.rgb = NAVY if r == 0 else (WHITE if r % 2 else ZEBRA)
            tf = cell.text_frame; tf.word_wrap = True
            p = tf.paragraphs[0]; p.line_spacing = 1.06
            p.alignment = aligns[c] if aligns else PP_ALIGN.LEFT
            run = p.add_run(); run.text = str(val)
            col = WHITE if r == 0 else INK
            if colors and r > 0 and colors[r - 1][c] is not None:
                col = colors[r - 1][c]
            style_font(run, size=head_fs if r == 0 else fs, bold=(r == 0), color=col)
    return gt


def arrow(slide, x, y, color=BLUE, size=15):
    para(textbox(slide, x, y, 0.32, 0.3, anchor=MSO_ANCHOR.MIDDLE), "▶",
         size=size, color=color, first=True, align=PP_ALIGN.CENTER, after=0)


def card(slide, x, y, w, h, title, lines, accent=BLUE, tsize=15, bsize=12.5,
         num=None, tcolor=INK):
    rect(slide, x, y, w, h, fill=WHITE, line_color=LINE, radius=0.05)
    rect(slide, x, y + 0.16, 0.055, h - 0.32, fill=accent)
    if num:
        chip(slide, x + 0.30, y + 0.22, 0.46, 0.42, num, accent, WHITE, 13)
        para(textbox(slide, x + 0.90, y + 0.24, w - 1.15, 0.40), title, size=tsize,
             bold=True, color=tcolor, first=True, after=0)
    else:
        para(textbox(slide, x + 0.32, y + 0.22, w - 0.60, 0.40), title, size=tsize,
             bold=True, color=tcolor, first=True, after=0)
    tf = textbox(slide, x + 0.32, y + 0.80, w - 0.62, h - 0.96)
    for i, ln in enumerate(lines):
        para(tf, ln, size=bsize, color=MUTED, first=(i == 0), after=7, line=1.3)


def node(slide, x, y, w, h, title, sub, accent, fill=WHITE, tsize=12):
    rect(slide, x, y, w, h, fill=fill, line_color=LINE, radius=0.08)
    rect(slide, x, y + 0.10, 0.05, h - 0.20, fill=accent)
    para(textbox(slide, x + 0.20, y + 0.08, w - 0.34, 0.28), title, size=tsize,
         bold=True, color=INK, first=True, after=0)
    if sub:
        para(textbox(slide, x + 0.20, y + 0.34, w - 0.34, h - 0.42), sub, size=10,
             color=MUTED, first=True, after=0, line=1.10)


prs = Presentation()
prs.slide_width, prs.slide_height = Inches(W), Inches(H)
PG = [0]
def nxt(s, dark=False):
    PG[0] += 1; pageno(s, PG[0], dark); return s


# ================================================================ 01 封面
s = blank(prs, NAVY)
rect(s, 0, 0, 0.17, H, fill=BLUE)
para(textbox(s, 1.05, 2.16, 7.6, 0.36), "ASSISTANT · 架构说明 v2", size=14, bold=True,
     color=BLUE, first=True, after=0)
para(textbox(s, 1.05, 2.66, 9.2, 1.05), "文件系统四模块：实现与交互关系", size=38,
     bold=True, color=WHITE, first=True, after=0)
para(textbox(s, 1.05, 3.86, 8.6, 0.5), "各自实现什么 · 彼此靠什么连 · 为什么必须这么连",
     size=16, color=RGBColor(0xB9, 0xC6, 0xD8), first=True, after=0)
rect(s, 1.05, 4.66, 3.0, 0.03, fill=RGBColor(0x35, 0x4A, 0x66))
para(textbox(s, 1.05, 4.96, 8.2, 0.4), "M1 文件管理 · M2 传输与共享 · M3 安全 · M4 插件",
     size=13, color=MUTED_L, first=True, after=0)

rect(s, 9.55, 2.00, 2.95, 3.45, fill=NAVY2, radius=0.05)
para(textbox(s, 9.90, 2.30, 2.4, 0.32), "一页结论", size=12.5, bold=True,
     color=BLUE, first=True, after=0)
tf = textbox(s, 9.90, 2.72, 2.35, 2.6)
for i, t in enumerate(["四个模块不是四个并列功能",
                       "是一条受控流水线 + 一个共享身份",
                       "跨模块只剩 4 个契约",
                       "其余耦合一律驳回"]):
    para(tf, "·  " + t, size=11, color=RGBColor(0xC7, 0xD4, 0xE6), first=(i == 0),
         after=11, line=1.3)

para(textbox(s, 9.55, 5.72, 3.0, 0.5), "依据：架构评审修订后的方案 v2", size=10.5,
     color=MUTED_L, first=True, after=0)

# ================================================================ 02 一页看懂
s = nxt(blank(prs))
header(s, "OVERVIEW", "一页看懂")

blocks = [
    ("职责怎么切", "一个问题只归一个模块",
     ["M1 看得见 · M2 出得去进得来",
      "M3 该不该、准不准、要不要保护",
      "M4 谁来加功能",
      "落不进这四行的，先别当新模块"], C_M1, BLUE_L),
    ("靠什么连", "4 个契约 + 公开 API",
     ["C1 FileRef · C2 事件",
      "C3 网关 · C4 插件入口",
      "模块门面上批准过的公开 API",
      "应用层编排作为组合根"], C_M2, GREEN_L),
    ("边界在哪", "网关只授权，执行器动手",
     ["只管经本应用发起的受控操作",
      "完整性是「发现」，不是「阻止」",
      "capability 不是进程内沙箱",
      "跨库不假设原子性，用恢复状态机"], C_M3, RED_L),
]
for i, (tag, t, lines, ac, al) in enumerate(blocks):
    x = M + i * (CW / 3)
    w = CW / 3 - 0.25
    rect(s, x, BODY_TOP, w, 3.05, fill=WHITE, line_color=LINE, radius=0.05)
    rect(s, x, BODY_TOP, w, 0.06, fill=ac)
    chip(s, x + 0.32, BODY_TOP + 0.32, 1.35, 0.36, tag, al, ac, 12)
    para(textbox(s, x + 0.32, BODY_TOP + 0.90, w - 0.64, 0.45), t, size=16,
         bold=True, color=INK, first=True, after=0, line=1.2)
    tf = textbox(s, x + 0.32, BODY_TOP + 1.52, w - 0.64, 1.40)
    for j, ln in enumerate(lines):
        para(tf, "·  " + ln, size=11.5, color=MUTED, first=(j == 0), after=8, line=1.3)

rect(s, M, 5.22, CW, 1.20, fill=BLUE_L, radius=0.05)
rich(textbox(s, M + 0.40, 5.22, CW - 0.80, 1.20, anchor=MSO_ANCHOR.MIDDLE),
     [("一句话结论：", True, BLUE_D),
      ("四个模块能各自长大，不靠约定俗成，而靠把耦合压到 4 个契约上。"
       "谁想加一条新的跨模块交互，先证明它落得进「七条边」之一 —— 落不进，"
       "就是职责重叠或契约漏了，回头改设计，而不是就地加接口。",
       False, INK)], size=13.5, first=True, after=0, line=1.35)
footnote(s, "本稿所有契约、边界与顺序均取自 docs/architecture/文件系统四模块-关联与方案.md（提案 v2）")

# ================================================================ 03 四模块职责
s = nxt(blank(prs))
header(s, "职责划分", "四模块：各自回答一个问题")

rows = [
    ["模块", "回答的问题", "主要输出", "明确不回答"],
    ["M1 文件管理", "盘上有什么、在哪、怎么找到",
     "索引快照 · 查询结果 · 分类/排除 · box 登记与去向", "该不该被操作、怎么送出去"],
    ["M2 传输与共享", "文件怎么出去、怎么进来",
     "传输作业（状态机/分块/校验/重试/恢复）· 配对 · 通道", "文件是什么、能不能发"],
    ["M3 安全", "该不该允许、有没有被动过、怎么保护",
     "判定 Allow/Ask/Deny · 完整性账本 · 审计 · 保险箱", "文件在哪、怎么发"],
    ["M4 插件", "谁来加新能力、加的能力能碰什么",
     "扩展点实现 · 能力声明 · 插件生命周期", "以上任何一件事本身"],
]
table(s, M, BODY_TOP + 0.02, CW, rows, [1.62, 2.85, 4.25, 3.11],
      row_h=0.90, fs=11.5, head_fs=12.5,
      colors=[[C_M1, None, None, None],
              [C_M2, None, None, None],
              [C_M3, None, None, None],
              [C_M4, None, None, None]])

rect(s, M, 6.02, CW, 0.62, fill=AMBER_L, radius=0.06)
rich(textbox(s, M + 0.32, 6.02, CW - 0.64, 0.62, anchor=MSO_ANCHOR.MIDDLE),
     [("验收式判据：", True, AMBER),
      ("任何新需求先往这四行里放。放不进去，说明它是第五个模块 —— 停下来讨论，"
       "而不是塞进现有模块。", False, INK)], size=12, first=True, after=0)
footnote(s, "M3 内部还要再分三档（发现 / 判定 / 保护），三者的验收方式完全不同 —— 见第 14 页")

# ================================================================ 04 分层与依赖
s = nxt(blank(prs))
header(s, "结构", "四层结构：依赖只许向下")

bands = [
    ("接入层", "QML 搜索浮窗 · 主面板 · 托盘 · 插件（只经 IHostApi，不碰模块）", MUTED, RGBColor(0xEC, 0xEF, 0xF4)),
    ("模块层", "M1 FileIndex · M2 TransferManager · M3 SecurityCenter · M4 PluginManager", C_M1, BLUE_L),
    ("契约层", "C1 FileRef · C2 FileSystemEvent · C3 FileOpGateway · C4 IHostApi + 能力", C_M3, RED_L),
    ("基座层", "FileIdentity · Snapshot · 事件分发器 · 各库访问组件 · Migrator · Config · Logger", C_M2, GREEN_L),
]
y = BODY_TOP
for name, txt, ac, bgc in bands:
    rect(s, M, y, CW, 0.78, fill=bgc, radius=0.05)
    rect(s, M, y + 0.14, 0.055, 0.50, fill=ac)
    para(textbox(s, M + 0.20, y, 1.35, 0.78, anchor=MSO_ANCHOR.MIDDLE), name,
         size=13, bold=True, color=ac, first=True, after=0)
    para(textbox(s, M + 1.62, y, CW - 1.80, 0.78, anchor=MSO_ANCHOR.MIDDLE), txt,
         size=11.5, color=INK, first=True, after=0, line=1.2)
    y += 0.94

for yy in (BODY_TOP + 0.60, BODY_TOP + 1.54, BODY_TOP + 2.48):
    para(textbox(s, M + CW / 2 - 0.15, yy, 0.3, 0.26), "▼", size=9, color=MUTED_L,
         first=True, align=PP_ALIGN.CENTER, after=0)

rect(s, M, 5.58, CW, 1.22, fill=WHITE, line_color=LINE, radius=0.05)
para(textbox(s, M + 0.32, 5.70, 3.0, 0.3), "三条组织规则", size=12, bold=True,
     color=BLUE_D, first=True, after=0)
rules = [
    "可以调公开业务 API，不许依赖别人的内部实现头 / 私有表 / 私有协作者",
    "应用层是组合根：构造、注入、编排都在这里，别散到各处",
    "不许为消除每次调用而加临时抽象，也不许把协调逻辑收进一个新的全局门面",
]
for i, r in enumerate(rules):
    para(textbox(s, M + 0.32 + i * 3.75, 6.06, 3.55, 0.66), f"{i+1}. " + r, size=10.5,
         color=MUTED, first=True, after=0, line=1.25)
footnote(s, "唯一的两处「向上」是接口倒置：IFilePolicy 由 M3 实现、IHostApi 由应用层实现，基座只持指针")

# ================================================================ 05 受控流水线
s = nxt(blank(prs))
header(s, "核心机制", "受控操作：全应用只有一条路")

groups = [
    ("① 受理", C_M4, ["请求提交", "调用主体校验", "目标对象重新解析"]),
    ("② 决策", C_M3, ["策略判定 Allow/Ask/Deny", "Ask = 尚未授权", "必要时等待审批"]),
    ("③ 收口", C_M2, ["执行前再次验证", "执行 / 创建异步作业", "记录结果 → 更新状态"]),
]
gw = 3.35
for i, (t, ac, steps) in enumerate(groups):
    x = M + i * (gw + 0.72)
    rect(s, x, BODY_TOP, gw, 2.28, fill=WHITE, line_color=LINE, radius=0.05)
    rect(s, x, BODY_TOP, gw, 0.06, fill=ac)
    para(textbox(s, x + 0.30, BODY_TOP + 0.20, gw - 0.6, 0.34), t, size=14, bold=True,
         color=ac, first=True, after=0)
    tf = textbox(s, x + 0.30, BODY_TOP + 0.68, gw - 0.6, 1.44)
    for j, st in enumerate(steps):
        para(tf, "·  " + st, size=11.5, color=MUTED, first=(j == 0), after=9, line=1.25)
    if i < 2:
        para(textbox(s, x + gw + 0.06, BODY_TOP + 0.82, 0.6, 0.5), "▶", size=18,
             color=MUTED_L, first=True, align=PP_ALIGN.CENTER, after=0)

rect(s, M, 4.30, CW, 0.92, fill=WHITE, line_color=LINE, radius=0.05)
rect(s, M, 4.30, 0.055, 0.92, fill=RED)
rich(textbox(s, M + 0.32, 4.30, CW - 0.64, 0.92, anchor=MSO_ANCHOR.MIDDLE),
     [("网关的职责是「授权 + 协调」，不是「替所有人做 I/O」：", True, RED),
      ("实际文件操作由受控执行组件完成，传输作业交给 M2。\n"
       "不要求每读一个数据块都重跑一次策略求值，但必须在校验边界检查源文件身份、版本与授权有效性。",
       False, INK)], size=12, first=True, after=0, line=1.3)

hard = [
    ("失败关闭", "策略异常 / 超时 / 无效结果 / 审批失效，一律 Deny；长作业遇到规则更新或信任撤销，按作业状态机处理"),
    ("覆盖范围", "只管经本应用发起的受控操作；资源管理器、别的程序直接动手，网关拦不住，也不承诺拦"),
    ("双源去重", "网关返回结果与文件系统通知是两路信息，必须用操作 id 关联，不能当成两次独立变更"),
]
for i, (t, d) in enumerate(hard):
    x = M + i * (CW / 3)
    w = CW / 3 - 0.22
    rect(s, x, 5.42, w, 1.36, fill=WHITE, line_color=LINE, radius=0.05)
    para(textbox(s, x + 0.24, 5.54, w - 0.48, 0.30), t, size=12, bold=True,
         color=RED, first=True, after=0)
    para(textbox(s, x + 0.24, 5.88, w - 0.48, 0.82), d, size=10.5, color=MUTED,
         first=True, after=0, line=1.28)
footnote(s, "这条流水线同时是插件的越权防线：插件没有文件系统权限，落盘动作都变成带 actor 的受控请求")

# ================================================================ 06 C1 FileRef
s = nxt(blank(prs))
header(s, "契约 ①", "C1 FileRef：轻量值句柄，不是授权凭证")

rect(s, M, BODY_TOP, 5.30, 3.62, fill=NAVY, radius=0.05)
tf = textbox(s, M + 0.34, BODY_TOP + 0.28, 4.62, 3.10)
lines = [
    ("struct FileRef {", WHITE, False),
    ("  quint64 id      = 0;   // 不透明句柄", RGBColor(0xA8, 0xD8, 0xA8), False),
    ("  QString path;          // 可失效的定位提示", RGBColor(0xA8, 0xD8, 0xA8), False),
    ("  qint64  size, mtime;   // 变化提示", RGBColor(0xA8, 0xD8, 0xA8), False),
    ("  quint32 catMask;       // M1 派生", RGBColor(0xF0, 0xC8, 0x8C), False),
    ("  quint8  flags;", RGBColor(0xF0, 0xC8, 0x8C), False),
    ("  quint8  secLevel;      // 仅展示/粗筛", RGBColor(0xF0, 0xC8, 0x8C), False),
    ("};", WHITE, False),
]
for i, (ln, c, b) in enumerate(lines):
    para(tf, ln, size=11.5, color=c, bold=b, first=(i == 0), after=4, line=1.42,
         font="Consolas")

para(textbox(s, M, 5.58, 5.30, 0.7),
     "字段可以随实现调整；下面三条语义不可省略。",
     size=10.5, color=MUTED, first=True, after=0, line=1.25)

right = [
    ("不是身份凭证", C_M3, ["进程内不透明句柄 ≠ 物理身份（卷序列号 + FileId）",
                        "路径是定位提示，不是稳定主键；文件可在两次调用之间被替换"]),
    ("不是快照保证", BLUE, ["size / mtime 只是快速变化提示，不是内容完整性证明",
                        "id 必须定义进程重启、文件删除、句柄失效时的行为"]),
    ("不是授权凭证", AMBER, ["secLevel 只能展示与粗筛，授权时必须重新判定",
                         "敏感操作尽可能用已打开的句柄验证实际对象，避免 TOCTOU"]),
]
x = M + 5.62
for i, (t, ac, ls) in enumerate(right):
    y = BODY_TOP + i * 1.24
    rect(s, x, y, CW - 5.62, 1.12, fill=WHITE, line_color=LINE, radius=0.05)
    rect(s, x, y + 0.14, 0.05, 0.84, fill=ac)
    para(textbox(s, x + 0.24, y + 0.12, 4.2, 0.30), t, size=12.5, bold=True,
         color=ac, first=True, after=0)
    tf = textbox(s, x + 0.24, y + 0.46, CW - 6.20, 0.60)
    for j, ln in enumerate(ls):
        para(tf, "· " + ln, size=10, color=MUTED, first=(j == 0), after=2, line=1.2)

rect(s, M, 4.42, 5.30, 1.02, fill=RED_L, radius=0.05)
para(textbox(s, M + 0.26, 4.54, 4.90, 0.80),
     "发送不变量：发送前校验源文件身份 · 传输中检测源文件变化 · 接收端分块校验 + 最终内容哈希",
     size=10, color=INK, first=True, after=0, line=1.28)
footnote(s, "要求「发出去的内容与授权时严格一致」时，必须用可验证快照或受控句柄 —— 只看 size/mtime 不够")

# ================================================================ 07 C2 FileSystemEvent
s = nxt(blank(prs))
header(s, "契约 ②", "C2 事件：变化通知，不是审计日志")

rect(s, M, BODY_TOP, 5.55, 1.52, fill=WHITE, line_color=LINE, radius=0.05)
para(textbox(s, M + 0.28, BODY_TOP + 0.16, 4.9, 0.3), "它只做一件事", size=12,
     bold=True, color=BLUE_D, first=True, after=0)
rich(textbox(s, M + 0.28, BODY_TOP + 0.52, 5.00, 0.86),
     [("告诉订阅者「盘上变了」，用于增量处理。", False, INK),
      ("\n它可能丢失、重复、乱序、溢出；它不能当审计日志，也不能当「没收到就等于没变」的依据。",
       False, MUTED)], size=11, first=True, after=0, line=1.3)

rect(s, M, 3.52, 5.55, 2.32, fill=WHITE, line_color=LINE, radius=0.05)
rect(s, M, 3.52, 0.055, 2.32, fill=AMBER)
para(textbox(s, M + 0.28, 3.64, 4.9, 0.3), "协议必须先写死这 5 件事", size=12,
     bold=True, color=AMBER, first=True, after=0)
musts = [
    "seq 的作用域、生成方、比较规则（全局单调还是按根单调？）",
    "Added / Removed / Renamed 的路径语义（目录重命名时子树怎么表达）",
    "可获得的物理身份信息，以及拿不到时怎么办",
    "Overflow 对应哪个监控根 / 受影响范围",
    "重复事件、乱序事件、消费者失败的处理方式",
]
tf = textbox(s, M + 0.28, 4.00, 5.05, 1.72)
for j, m in enumerate(musts):
    para(tf, f"{j+1}. " + m, size=10, color=MUTED, first=(j == 0), after=5, line=1.24)

rows = [
    ["消费者", "收到 Overflow / 中断 / 失败时"],
    ["M1 索引", "重扫受影响范围，修复索引"],
    ["M3 账本", "该范围标「待复核」——不许因没收到后续事件就认定没变"],
    ["M2 作业", "对源 / 目标文件主动校验，不靠通知决定作业状态"],
    ["其他", "必须写出自己的恢复 / 重试 / 降级策略，写不出不许接入"],
]
table(s, 6.55, BODY_TOP, 6.03, rows, [1.28, 4.75], row_h=0.86, fs=10.5, head_fs=11.5,
      colors=[[C_M1, None], [C_M3, None], [C_M2, None], [MUTED, None]])
footnote(s, "消费者由应用层显式组装；固定数量只是当前约定，不是长期不变量 —— 要加就走显式依赖，不许开隐式全局订阅")

# ================================================================ 08 C3 FileOpGateway
s = nxt(blank(prs))
header(s, "契约 ③", "C3 网关：授什么权、谁动手")

rows = [
    ["组件", "负责", "不负责"],
    ["FileOpGateway", "请求校验 · 主体识别 · 策略调用 · 审批协调 · 执行前验证 · 结果关联", "底层文件 I/O、传输"],
    ["IFilePolicy（M3）", "同步、纯内存的规则求值", "审批、落库、哈希、传输准备"],
    ["受控执行组件", "执行受控的本地文件操作", "策略判定"],
    ["TransferManager（M2）", "异步传输作业与通道", "绕过授权边界"],
    ["审计组件", "按风险分级持久化请求 / 判定 / 结果", "阻塞前台"],
]
table(s, M, BODY_TOP, CW, rows, [2.15, 6.30, 3.15], row_h=0.56, fs=10.5, head_fs=11.5,
      colors=[[C_M4, None, None], [C_M3, None, None], [C_M2, None, None],
              [C_M2, None, None], [MUTED, None, None]])

rect(s, M, 5.16, 5.72, 1.52, fill=WHITE, line_color=LINE, radius=0.05)
para(textbox(s, M + 0.26, 5.28, 5.1, 0.3), "失败关闭（比 v1 更完整）", size=12,
     bold=True, color=RED, first=True, after=0)
tf = textbox(s, M + 0.26, 5.64, 5.2, 0.94)
for j, t in enumerate(["策略异常 / 超时 / 无效结果 → Deny",
                       "审批失效、规则更新、信任撤销 → 按作业状态机处理",
                       "用户取消 → 后续块级操作立即失效"]):
    para(tf, "· " + t, size=10.5, color=MUTED, first=(j == 0), after=4, line=1.25)

rect(s, 6.72, 5.16, 5.86, 1.52, fill=WHITE, line_color=LINE, radius=0.05)
para(textbox(s, 6.98, 5.28, 5.3, 0.3), "静态检查的真实语义", size=12, bold=True,
     color=BLUE_D, first=True, after=0)
tf = textbox(s, 6.98, 5.64, 5.42, 0.94)
for j, t in enumerate(["覆盖 Qt / Win32 / Shell / 标准库全部变更 API",
                       "放行受控执行组件，拒绝 QML 层、插件、其他模块私自调用",
                       "验收不是「没命中字符串」，而是「所有路径都过边界」；例外逐条登记"]):
    para(tf, "· " + t, size=10.5, color=MUTED, first=(j == 0), after=4, line=1.25)
footnote(s, "网关实现为「注入依赖的具体协调类」；未来确有多种实现再抽接口 —— 不因形式统一而加一层空虚接口")

# ================================================================ 09 C4 IHostApi
s = nxt(blank(prs))
header(s, "契约 ④", "C4 插件入口：能力声明，不是沙箱")

rect(s, M, BODY_TOP, 6.10, 2.42, fill=NAVY, radius=0.05)
tf = textbox(s, M + 0.32, BODY_TOP + 0.22, 5.5, 2.0)
code = [
    ("class IHostApi {", WHITE),
    ("  search(text, limit);        // index.read", RGBColor(0xA8, 0xD8, 0xA8)),
    ("  resolve(id);               // file.read", RGBColor(0xA8, 0xD8, 0xA8)),
    ("  submitIngest(path, src);   // ingest.write", RGBColor(0xA8, 0xD8, 0xA8)),
    ("  requestSend(id, device);   // transfer.send → 必过授权", RGBColor(0xF0, 0xC8, 0x8C)),
    ("  showPanel(qmlUrl);         // ui.panel", RGBColor(0xA8, 0xD8, 0xA8)),
    ("};", WHITE),
]
for i, (ln, c) in enumerate(code):
    para(tf, ln, size=11, color=c, first=(i == 0), after=4, line=1.40, font="Consolas")

para(textbox(s, M, 4.34, 6.10, 0.34), "v1 能力清单（8 个，名称集中定义，不许各处拼写）",
     size=11, bold=True, color=MUTED, first=True, after=0)
caps = ["index.read", "file.read", "file.write", "op.execute",
        "ingest.write", "transfer.send", "security.policy.read", "ui.panel"]
for i, c in enumerate(caps):
    x = M + (i % 4) * 1.55
    y = 4.72 + (i // 4) * 0.52
    chip(s, x, y, 1.42, 0.40, c, VIOLET_L, VIOLET, 9.5)

rect(s, 7.10, BODY_TOP, 5.48, 2.42, fill=RED_L, radius=0.05)
para(textbox(s, 7.40, BODY_TOP + 0.20, 4.9, 0.34), "必须说清的一句话", size=12.5,
     bold=True, color=RED, first=True, after=0)
tf = textbox(s, 7.40, BODY_TOP + 0.62, 4.95, 1.62)
para(tf, "进程内插件能直接调 Win32、读进程内存、影响宿主。所以 manifest、签名、"
         "capability 都只是「契约」，不是「边界」。", size=11, color=INK, first=True,
     after=8, line=1.3)
para(tf, "签名只证明来源与完整性，不证明行为可信 —— 这一点必须写在安装提示里，"
         "而不是靠用户自己发现。", size=10.5, color=MUTED, after=0, line=1.3)

rect(s, M, 5.42, CW, 1.26, fill=WHITE, line_color=LINE, radius=0.05)
para(textbox(s, M + 0.30, 5.54, 5.0, 0.3), "宿主必须自己做的两件事", size=12, bold=True,
     color=VIOLET, first=True, after=0)
tf = textbox(s, M + 0.30, 5.90, 5.20, 0.70)
para(tf, "· 由宿主解析实际调用主体，插件自填 actorId 一律无效\n"
         "· 每次宿主 API 调用都校验能力（默认拒绝）", size=10.5, color=MUTED,
     first=True, after=0, line=1.28)
tf = textbox(s, 6.60, 5.90, 5.55, 0.70)
para(tf, "· API 版本兼容与能力授权分开判断（两个错误码，不许混）\n"
         "· 生命周期：加载位置可信 → 校验 manifest → 初始化 → 失败干净回滚",
     size=10.5, color=MUTED, first=True, after=0, line=1.28)

# ================================================================ 10 依赖规则
s = nxt(blank(prs))
header(s, "依赖规则", "合法的三条路，和非法的三条路")

rect(s, M, BODY_TOP, 5.62, 2.86, fill=GREEN_L, radius=0.05)
rect(s, M, BODY_TOP, 5.62, 0.06, fill=GREEN)
para(textbox(s, M + 0.30, BODY_TOP + 0.18, 5.0, 0.34), "合法：三者之一", size=13,
     bold=True, color=GREEN, first=True, after=0)
oks = [("契约类型", "include/contract/ 里的 C1–C4"),
       ("公开业务 API", "模块门面上明确批准过的方法，如 FileIndex::upsert()"),
       ("应用层编排", "组合根串起来的调用顺序，不落在任何模块内部")]
tf = textbox(s, M + 0.30, BODY_TOP + 0.66, 5.05, 2.05)
for j, (t, d) in enumerate(oks):
    para(tf, f"{j+1}. {t}", size=11.5, bold=True, color=INK, first=(j == 0), after=2)
    para(tf, "    " + d, size=10, color=MUTED, after=9, line=1.22)

rect(s, 6.72, BODY_TOP, 5.86, 2.86, fill=RED_L, radius=0.05)
rect(s, 6.72, BODY_TOP, 5.86, 0.06, fill=RED)
para(textbox(s, 7.02, BODY_TOP + 0.18, 5.2, 0.34), "驳回：典型的三种", size=13,
     bold=True, color=RED, first=True, after=0)
bas = [("include 别人的内部实现头", "#include \"core/TransferManager.h\" 出现在 M3 里"),
       ("直接查别人的私有表", "M3 去读 M1 的 files 表 —— 表结构一改就崩"),
       ("绕过授权边界", "M4 直调 M2 内部通道，等于自己给自己授权")]
tf = textbox(s, 7.02, BODY_TOP + 0.66, 5.30, 2.05)
for j, (t, d) in enumerate(bas):
    para(tf, f"{j+1}. {t}", size=11.5, bold=True, color=INK, first=(j == 0), after=2)
    para(tf, "    " + d, size=10, color=MUTED, after=9, line=1.22)

rect(s, M, 4.86, CW, 1.72, fill=WHITE, line_color=LINE, radius=0.05)
para(textbox(s, M + 0.30, 4.98, 6.0, 0.32), "契约数量：是审查提示，不是正确性指标", size=12.5,
     bold=True, color=BLUE_D, first=True, after=0)
para(textbox(s, M + 0.30, 5.36, CW - 0.60, 1.10),
     "v1 写的是「只能有四个，多一个都不加」。v2 改成：四个是当前目标集合，新增必须说得清"
     "「独立职责、必要性、依赖方向、失败语义」四件事。说得清就不必因为超过四个而绕道；"
     "说不清，一个也不该加。机械冻结数量只会逼出畸形绕过。",
     size=11, color=MUTED, first=True, after=0, line=1.32)
footnote(s, "判据可以背下来：依赖的是契约类型、门面上的批准方法、还是应用层编排？三者之外一律驳回")

# ================================================================ 11 七条边
s = nxt(blank(prs))
header(s, "交互关系", "七条边：加交互前先落到某一条上")

rows = [
    ["边", "传递什么", "走什么", "失败模式与对策"],
    ["M1 → M2", "待发送的 FileRef 集合", "TransferManager::enqueue()",
     "排队期间文件被改 → 重新解析验身份，不匹配就跳过，绝不发半截"],
    ["M2 → M1", "新落地文件 + 来源信息", "FileIndex::upsert() / remove()",
     "索引未就绪 → 排队不丢；恢复后补提交，不重收文件"],
    ["M1 → M3", "查询视野 + 变更事件", "query() + 订阅 C2",
     "Overflow → 标待复核，巡检兜底；M3 不许自己扫盘"],
    ["M3 → M1", "密级回写 secLevel", "快照重建（同 catMask）",
     "rulesVersion 驱动重建；授权时仍必须重新判定"],
    ["M3 → M2", "OpDecision + obligations", "网关判定 → 创建受控作业",
     "判定异常/审批失效 → Deny；执行中规则变更按作业状态机"],
    ["M2 → M3", "外来文件校验请求", "SecurityCenter::verifyIncoming()",
     "校验不符 → 留隔离区、不入库、不进索引"],
    ["M4 → 全部", "能力调用", "IHostApi → 公开 API / 网关",
     "能力不足给明确错误码；插件异常见第 15 页"],
]
table(s, M, BODY_TOP, CW, rows, [1.02, 2.42, 2.85, 5.54], row_h=0.62, fs=10, head_fs=11.5,
      colors=[[C_M1, None, None, None], [C_M2, None, None, None], [C_M1, None, None, None],
              [C_M3, None, None, None], [C_M3, None, None, None],
              [C_M2, None, None, None], [C_M4, None, None, None]])
footnote(s, "七条边之外的新交互＝新增一条边，需要重新评审：多半是职责重叠，或某个契约漏了一句话")

# ================================================================ 12 M1
s = nxt(blank(prs))
header(s, "M1 实现", "文件管理：只补接缝，不重写")

para(textbox(s, M, 1.62, CW, 0.34),
     "已有主体不动：FileIndex 门面 / FileSnapshot / SnapshotManager / A3–A8 清单 / 治理 B–E。"
     "M1 不承担 M2 的传输执行，也不承担 M3 的安全授权。",
     size=11, color=MUTED, first=True, after=0, line=1.25)

rows = [
    ["候选字段", "加的条件", "必须同时明确的语义"],
    ["sec_level", "作为 M3 密级结果的派生缓存，且能识别规则版本过期",
     "授权时必须重新判定；只用于展示 / 筛选 / 粗筛，不能作为授权依据"],
    ["prov_id", "来源关联、查询性能或现有数据模型确有需要时，才进索引库",
     "来源记录的权威存储位置、关联失效时的行为、删除与迁移策略"],
]
table(s, M, 2.10, CW, rows, [1.55, 4.55, 5.73], row_h=0.92, fs=10.5, head_fs=11.5,
      colors=[[C_M3, None, None], [C_M2, None, None]])

para(textbox(s, M, 4.50, 6.0, 0.32), "v1 → v2 的关键收紧：这两个字段不再「顺手一起加」",
     size=12, bold=True, color=RED, first=True, after=0)

cn = [
    ("薄门面", "M2 / M3 是客户，不是成员：只经 upsert / remove / query 通信"),
    ("跨库不假设外键", "来源记录在独立库时，用稳定标识 + 可恢复应用逻辑关联，不靠跨库外键"),
    ("字段要有出处", "新增字段必须服务明确的查询或恢复需求，不为「将来可能需要」无限扩"),
]
for i, (t, d) in enumerate(cn):
    x = M + i * (CW / 3)
    w = CW / 3 - 0.22
    rect(s, x, 4.86, w, 1.30, fill=WHITE, line_color=LINE, radius=0.05)
    rect(s, x, 4.86, w, 0.05, fill=C_M1)
    para(textbox(s, x + 0.24, 5.00, w - 0.48, 0.30), t, size=11.5, bold=True,
         color=C_M1, first=True, after=0)
    para(textbox(s, x + 0.24, 5.34, w - 0.48, 0.76), d, size=10, color=MUTED,
         first=True, after=0, line=1.26)
footnote(s, "secLevel 若进快照：uint8 向量，20 万条约 0.2 MB，按 rulesVersion 重建，不重扫盘")

# ================================================================ 13 M2
s = nxt(blank(prs))
header(s, "M2 实现", "传输：一套作业内核，三个交付单元")

units = [
    ("①", "传输作业内核", ["状态机 · 分块 · 校验 · 重试 · 取消 · 恢复",
                     "先用本地模拟通道验证", "不碰网络就能验收"], C_M2),
    ("②", "渠道目录通道", ["复用 box / 追踪 / 归档机制",
                     "投递到渠道客户端监听的目录", "验收入库与索引一致性"], C_M1),
    ("③", "LAN 通道", ["配对 · 验证过的 TLS · 接收隔离区",
                   "分块续传 · 最终提交 · 崩溃恢复", "①② 稳定后才做"], C_M4),
]
for i, (num, t, lines, ac) in enumerate(units):
    x = M + i * (CW / 3)
    card(s, x, BODY_TOP, CW / 3 - 0.22, 2.06, t, lines, accent=ac, tsize=14,
         bsize=10, num=num)

rect(s, M, 4.06, 7.05, 2.42, fill=WHITE, line_color=LINE, radius=0.05)
rect(s, M, 4.06, 0.055, 2.42, fill=RED)
para(textbox(s, M + 0.28, 4.18, 6.4, 0.3), "接收端 6 条硬要求（都是安全用例，不是可选项）",
     size=12, bold=True, color=RED, first=True, after=0)
reqs = [
    ("路径", "远端不能决定最终落盘路径；拒绝穿越、ADS、保留设备名、危险 reparse point"),
    ("入库", "未完成校验的文件不得进入正式索引；默认不覆盖同名文件"),
    ("恢复", "接收 / 验证 / 入库 / 索引更新必须能在崩溃后恢复"),
    ("状态", "源文件变化、重复块、任务取消、提交失败都要有明确状态转移"),
]
tf = textbox(s, M + 0.28, 4.58, 6.55, 1.82)
for j, (k, d) in enumerate(reqs):
    rich(tf, [(k + "：", True, RED), (d, False, MUTED)], size=10, first=(j == 0),
         after=6, line=1.26)

rect(s, 7.98, 4.06, 4.60, 2.42, fill=AMBER_L, radius=0.05)
para(textbox(s, 8.24, 4.18, 4.1, 0.3), "TLS 红线（v1 已修订）", size=12, bold=True,
     color=AMBER, first=True, after=0)
tf = textbox(s, 8.24, 4.58, 4.12, 1.80)
para(tf, "目标环境无法提供「经过验证」的 TLS 后端 → 暂停 LAN 发布。", size=10.5,
     color=INK, first=True, after=8, line=1.28)
para(tf, "不允许自动退化成自研加密协议。应用层 AES-GCM 不是 TLS 的替代品 —— "
         "两者解决的是不同层面的问题。", size=10, color=MUTED, after=8, line=1.26)
para(tf, "→ 这是一道前置门槛，不是可以留到发布后再补的项。", size=10, bold=True,
     color=AMBER, after=0, line=1.26)
footnote(s, "mDNS 不进首版：手动地址 + 配对码起步；后续按真实发现体验与依赖成本再评估，但不永久禁止")

# ================================================================ 14 M3
s = nxt(blank(prs))
header(s, "M3 实现", "安全：发现 / 判定 / 保护，三种能力分开验收")

caps3 = [
    ("发现", ["订阅事件 + 快速签名 + 周期复核", "只承诺「在监控范围内发现变化」",
            "不承诺阻止所有外部修改"], C_M2),
    ("判定", ["对经本应用的受控操作求值", "返回 Allow / Ask / Deny",
            "异常与超时一律失败关闭"], C_M3),
    ("保护", ["保险箱 + 密钥管理", "受控的临时明文生命周期",
            "只承诺静止态加密"], C_M4),
]
for i, (t, lines, ac) in enumerate(caps3):
    x = M + i * (CW / 3)
    rect(s, x, BODY_TOP, CW / 3 - 0.22, 1.58, fill=WHITE, line_color=LINE, radius=0.05)
    rect(s, x, BODY_TOP, CW / 3 - 0.22, 0.06, fill=ac)
    para(textbox(s, x + 0.30, BODY_TOP + 0.16, 3.2, 0.32), t, size=13.5, bold=True,
         color=ac, first=True, after=0)
    tf = textbox(s, x + 0.30, BODY_TOP + 0.56, CW / 3 - 0.82, 0.94)
    for j, ln in enumerate(lines):
        para(tf, "· " + ln, size=9.5, color=MUTED, first=(j == 0), after=3, line=1.2)

rect(s, M, 3.60, 5.58, 2.58, fill=WHITE, line_color=LINE, radius=0.05)
para(textbox(s, M + 0.28, 3.72, 5.0, 0.3), "账本：状态必须能说出「不知道」", size=12,
     bold=True, color=C_M3, first=True, after=0)
sts = ["正常", "待复核", "已变化", "不可验证"]
for i, st in enumerate(sts):
    chip(s, M + 0.28 + i * 1.28, 4.10, 1.16, 0.36, st,
         RED_L if i >= 2 else GREEN_L, RED if i >= 2 else GREEN, 10.5)
tf = textbox(s, M + 0.28, 4.58, 5.02, 1.50)
para(tf, "监控中断、事件溢出、文件读取失败 → 显式标记，不许静默当成正常。",
     size=10, color=MUTED, first=True, after=6, line=1.26)
para(tf, "快速签名只做变化筛查；要判「内容是否一致」必须全量哈希，或分块哈希 + 最终哈希。",
     size=10, color=MUTED, after=0, line=1.26)

rect(s, 6.70, 3.60, 5.88, 2.58, fill=WHITE, line_color=LINE, radius=0.05)
para(textbox(s, 6.96, 3.72, 5.3, 0.3), "保险箱 v1 必须明确的 5 件事", size=12,
     bold=True, color=C_M4, first=True, after=0)
vt = ["数据密钥的生成与管理（AES-256-GCM + DPAPI 保护）",
      "nonce 不重复的保证（随机碰撞论证或计数器方案）",
      "文件名 / 元数据与密文之间的认证关系（AAD 绑什么）",
      "恢复码的生成、保存、验证与密钥恢复语义",
      "临时明文的创建 / 清理 / 异常恢复；改文件后如何重加密"]
tf = textbox(s, 6.96, 4.14, 5.34, 1.96)
for j, t in enumerate(vt):
    para(tf, f"{j+1}. " + t, size=10, color=MUTED, first=(j == 0), after=6, line=1.24)

rect(s, M, 6.32, CW, 0.50, fill=AMBER_L, radius=0.06)
rich(textbox(s, M + 0.28, 6.32, CW - 0.56, 0.50, anchor=MSO_ANCHOR.MIDDLE),
     [("擦除的诚实边界：", True, AMBER),
      ("删掉临时文件不等于 SSD / 日志 / 备份 / 分页文件里的明文副本都消失 —— "
       "只承诺「受控的临时明文生命周期」，不承诺无法验证的绝对擦除。", False, INK)],
     size=10.5, first=True, after=0)

# ================================================================ 15 M4
s = nxt(blank(prs))
header(s, "M4 实现", "插件：进程内完全信任，先把边界说清")

rect(s, M, BODY_TOP, 4.90, 2.30, fill=RED_L, radius=0.05)
rect(s, M, BODY_TOP, 0.055, 2.30, fill=RED)
para(textbox(s, M + 0.28, BODY_TOP + 0.16, 4.2, 0.32), "v1 的信任模型", size=12,
     bold=True, color=RED, first=True, after=0)
tf = textbox(s, M + 0.28, BODY_TOP + 0.56, 4.30, 1.60)
para(tf, "面向「用户主动安装并明确授权」的可信代码，进程内加载。", size=10.5,
     color=INK, first=True, after=6, line=1.26)
para(tf, "它能直接调 Win32、读进程内存、影响宿主 —— 签名只用来识别来源与完整性，"
         "不用来证明行为可信。", size=10, color=MUTED, after=6, line=1.26)
para(tf, "承认：插件崩溃可能拖垮主进程。这是 v1 的已知代价，不是缺陷遗漏。",
     size=10, bold=True, color=RED, after=0, line=1.26)

para(textbox(s, 6.10, BODY_TOP + 0.02, 6.4, 0.32), "宿主必须做的 6 件事", size=12,
     bold=True, color=VIOLET, first=True, after=0)
duties = [
    "从可信加载位置加载，明确安装与启用流程",
    "校验 manifest、API 版本、能力声明",
    "由宿主识别插件身份，不信任自填 actorId",
    "每次宿主 API 调用都校验能力（默认拒绝）",
    "管理停用 / 卸载 / 初始化失败 / 宿主关闭",
    "异常给出明确错误信息，不静默吞掉",
]
tf = textbox(s, 6.10, BODY_TOP + 0.44, 6.45, 1.92)
for j, d in enumerate(duties):
    para(tf, f"{j+1}. " + d, size=10.5, color=MUTED, first=(j == 0), after=4, line=1.22)

rect(s, M, 4.34, CW, 1.10, fill=WHITE, line_color=LINE, radius=0.05)
para(textbox(s, M + 0.30, 4.46, 5.0, 0.3), "两类扩展点之外，还要三个稳定面", size=11.5,
     bold=True, color=BLUE_D, first=True, after=0)
for i, (t, d) in enumerate([("ISearchProvider", "往搜索结果里加一类结果"),
                            ("IFileActionProvider", "给 FileRef 加动作（右键 / 命令）"),
                            ("IIngestSource", "提供新的文件来源")]):
    x = M + 0.30 + i * 3.90
    para(textbox(s, x, 4.82, 3.7, 0.26), t, size=10.5, bold=True, color=VIOLET,
         first=True, after=0)
    para(textbox(s, x, 5.08, 3.7, 0.26), d, size=9.5, color=MUTED, first=True, after=0)

rect(s, M, 5.60, CW, 1.10, fill=WHITE, line_color=LINE, radius=0.05)
rich(textbox(s, M + 0.30, 5.60, CW - 0.60, 1.10, anchor=MSO_ANCHOR.MIDDLE),
     [("版本与能力分开判断：", True, VIOLET),
      ("apiVersion 不兼容 → 拒绝加载；能力不足 → 拒绝调用。两个错误码不许混，"
       "否则用户只会看到「插件不工作」。", False, INK),
      ("\n进程外插件与 IPC 隔离属于后续候选，与 mDNS 是两件独立的事，不互相绑定。",
       False, MUTED)], size=10.5, first=True, after=0, line=1.32)

# ================================================================ 16 存储与恢复
s = nxt(blank(prs))
header(s, "存储边界", "5 个存储区：谁是权威，失败怎么办")

rows = [
    ["存储区", "性质", "唯一写者", "迁移失败"],
    ["file_index.db", "派生（可从磁盘重建）", "FileIndexDatabase", "备份后重建，代价是重扫"],
    ["downloads.db", "权威（box / 去向链重建不出来）", "DownloadsDatabase", "保留数据，停止相关功能，拒绝启动"],
    ["security.db", "权威（账本 / 审计 / 密钥清单）", "SecurityDatabase", "同上，且比 downloads 更硬"],
    ["transfer.db", "半权威（作业可弃，配对信任不可弃）", "TransferDatabase", "降级启动：禁传输，提示重新配对"],
    ["plugins/<id>/", "插件私有（命名空间隔离）", "插件自己", "隔离该插件，不破坏主程序"],
]
table(s, M, BODY_TOP, CW, rows, [1.92, 3.05, 2.15, 4.71], row_h=0.54, fs=10, head_fs=11,
      colors=[[MUTED, None, None, None], [C_M1, None, None, None], [C_M3, None, None, None],
              [C_M2, None, None, None], [C_M4, None, None, None]])

rect(s, M, 5.02, 6.05, 1.80, fill=WHITE, line_color=LINE, radius=0.05)
para(textbox(s, M + 0.28, 5.14, 5.4, 0.3), "跨库状态机的四问（每个状态都要答）", size=11.5,
     bold=True, color=C_M3, first=True, after=0)
tf = textbox(s, M + 0.28, 5.50, 5.5, 1.22)
for j, t in enumerate(["哪些持久化操作已经完成？", "哪些可以安全重试？",
                       "哪些必须补偿或回滚？", "哪些只能交给用户处理？"]):
    para(tf, f"{j+1}. " + t, size=10, color=MUTED, first=(j == 0), after=3, line=1.2)

rect(s, 7.00, 5.02, 5.58, 1.80, fill=RED_L, radius=0.05)
para(textbox(s, 7.26, 5.14, 5.0, 0.3), "两个必须写成用例的场景", size=11.5, bold=True,
     color=RED, first=True, after=0)
tf = textbox(s, 7.26, 5.50, 5.06, 1.22)
para(tf, "· 已写入 quarantine 但未入库 → 重启后续验，或安全清理，不许停在半入库",
     size=10, color=INK, first=True, after=6, line=1.24)
para(tf, "· 来源登记成功但索引更新失败 → 补提交索引，不重收整个文件",
     size=10, color=INK, after=0, line=1.24)
footnote(s, "独立数据库 ≠ 天然一致：跨库流程必须有恢复入口和验收用例，这是 v1 只写「靠应用逻辑维护」补不上的")

# ================================================================ 17 实施顺序
s = nxt(blank(prs))
header(s, "落地", "依赖阶梯：能并行的并行，不能被同一闭环卡住的先卡死")

cols = [
    ("现在", [("内核 A3–A6", "身份 · 事件恢复 · 受控操作边界", C_M1)]),
    ("可并行", [("B 治理过滤", "分类 · 排除 · 查询语法", C_M1),
              ("M3-p1 安全", "边界 + 判定 + 账本骨架", C_M3)]),
    ("本地闭环", [("C/D box + 追踪", "入库恢复闭环", C_M2),
                ("E 归档多源", "不被 LAN 待定阻塞", C_M2)]),
    ("传输三级", [("M2① 作业内核", "本地模拟通道", C_M2),
                ("M2② 渠道通道", "复用 box / 追踪", C_M2),
                ("M2③ LAN 通道", "TLS 实测通过才做", C_M4)]),
]
xs = [M, 3.10, 5.75, 8.55]
ws = [1.95, 2.35, 2.55, 3.30]
for ci, ((title, nodes), x, w) in enumerate(zip(cols, xs, ws)):
    para(textbox(s, x, 1.66, w, 0.3), title, size=11, bold=True, color=MUTED,
         first=True, align=PP_ALIGN.CENTER, after=0)
    for ni, (t, d, ac) in enumerate(nodes):
        node(s, x, 2.04 + ni * 0.92, w, 0.80, t, d, ac, tsize=11)
    if ci < 3:
        para(textbox(s, x + w + 0.02, 2.30, 0.36, 0.3), "▶", size=13, color=MUTED_L,
             first=True, align=PP_ALIGN.CENTER, after=0)

rect(s, M, 5.02, 5.72, 1.20, fill=VIOLET_L, radius=0.05)
para(textbox(s, M + 0.26, 5.14, 5.2, 0.3), "旁支：M3-p2 保险箱", size=11.5, bold=True,
     color=VIOLET, first=True, after=0)
para(textbox(s, M + 0.26, 5.48, 5.2, 0.68),
     "独立验证加密 / 密钥 / 恢复码 / 临时明文；可复用验证过的加密工具，"
     "但不依赖 M2 业务模块，因此不必等传输。", size=9.5, color=MUTED, first=True,
     after=0, line=1.26)

rect(s, 6.70, 5.02, 5.88, 1.20, fill=VIOLET_L, radius=0.05)
para(textbox(s, 6.96, 5.14, 5.4, 0.3), "条件触发：M4 插件", size=11.5, bold=True,
     color=VIOLET, first=True, after=0)
para(textbox(s, 6.96, 5.48, 5.4, 0.68),
     "挂在「核心公开 API 定型」这个条件上，不挂在某条实现链的末尾 —— "
     "所以它不看传输做没做完，看接口稳没稳。", size=9.5, color=MUTED, first=True,
     after=0, line=1.26)
footnote(s, "并行与串行的界线：互不依赖的实现可以并行；被同一闭环依赖的接口必须先各自验收")

# ================================================================ 18 决策 · 待验证 · 不做
s = nxt(blank(prs))
header(s, "收口", "已拍板的、还没测的、明确不做的")

rect(s, M, BODY_TOP, 3.85, 4.22, fill=WHITE, line_color=LINE, radius=0.05)
rect(s, M, BODY_TOP, 3.85, 0.06, fill=GREEN)
para(textbox(s, M + 0.26, BODY_TOP + 0.16, 3.3, 0.3), "四项决策（已定）", size=12,
     bold=True, color=GREEN, first=True, after=0)
ds = [("操作边界", "做，但网关只授权协调"),
      ("mDNS", "首版不做，也不永久禁止"),
      ("插件信任", "进程内完全信任，无沙箱"),
      ("保险箱", "只承诺静止态加密")]
tf = textbox(s, M + 0.26, BODY_TOP + 0.58, 3.34, 3.50)
for j, (k, v) in enumerate(ds):
    para(tf, k, size=10.5, bold=True, color=GREEN, first=(j == 0), after=1)
    para(tf, v, size=10, color=MUTED, after=9, line=1.22)

rect(s, 4.75, BODY_TOP, 3.85, 4.22, fill=WHITE, line_color=LINE, radius=0.05)
rect(s, 4.75, BODY_TOP, 3.85, 0.06, fill=AMBER)
para(textbox(s, 5.01, BODY_TOP + 0.16, 3.3, 0.3), "待实测（不是设计待决）", size=12,
     bold=True, color=AMBER, first=True, after=0)
ws2 = [("Qt TLS 后端", "LAN 发布的前置门槛"),
       ("prov_id 是否入索引", "看查询需求与性能实测"),
       ("mDNS 是否引入", "看真实发现体验与成本"),
       ("插件进程外化", "与 mDNS 无关，独立决策")]
tf = textbox(s, 5.01, BODY_TOP + 0.58, 3.34, 3.50)
for j, (k, v) in enumerate(ws2):
    para(tf, k, size=10.5, bold=True, color=AMBER, first=(j == 0), after=1)
    para(tf, v, size=10, color=MUTED, after=9, line=1.22)

rect(s, 8.75, BODY_TOP, 3.83, 4.22, fill=WHITE, line_color=LINE, radius=0.05)
rect(s, 8.75, BODY_TOP, 3.83, 0.06, fill=RED)
para(textbox(s, 9.01, BODY_TOP + 0.16, 3.3, 0.3), "v1 明确不做", size=12, bold=True,
     color=RED, first=True, after=0)
tf = textbox(s, 9.01, BODY_TOP + 0.58, 3.30, 3.50)
for j, t in enumerate(["文件系统过滤驱动级拦截", "全系统外部操作强制控制",
                       "进程内插件沙箱", "运行期完全无明文的保险箱",
                       "自研 TLS 替代协议", "NAT 穿透 / 公网直连"]):
    para(tf, "· " + t, size=10, color=MUTED, first=(j == 0), after=6, line=1.22)
para(textbox(s, 9.01, 5.42, 3.30, 0.8),
     "这些不是「以后再说」，是「说了也做不到」——写进文档是为了不给自己留假承诺的余地。",
     size=9, color=MUTED_L, first=True, after=0, line=1.24)
footnote(s, "所有性能数字都是目标值（待实测），不是已实现能力的声明")

# ================================================================ 19 小结
s = nxt(blank(prs, NAVY), dark=True)
rect(s, 0, 0, 0.17, H, fill=BLUE)
para(textbox(s, 1.05, 1.55, 8.0, 0.36), "小结", size=13, bold=True, color=BLUE,
     first=True, after=0)
para(textbox(s, 1.05, 1.98, 10.6, 1.30),
     "四个模块的关系，是「一段流水线 + 一个身份 + 少数契约」",
     size=30, bold=True, color=WHITE, first=True, after=0, line=1.24)
rect(s, 1.05, 3.42, 3.0, 0.03, fill=RGBColor(0x35, 0x4A, 0x66))

lines = [
    ("M1 管看得见，M2 管出得去进得来，M3 管该不该 / 准不准 / 要不要保护，M4 管谁来加功能。",
     RGBColor(0xC7, 0xD4, 0xE6)),
    ("四者共用一个身份（FileRef / FileIdentity）、一条受控流水线、一小组契约与批准过的公开 API。",
     RGBColor(0xC7, 0xD4, 0xE6)),
    ("其余一切耦合都是设计失误 —— 包括「就这一次，先直接调一下」。", WHITE),
]
tf = textbox(s, 1.05, 3.82, 10.2, 1.90)
for i, (t, c) in enumerate(lines):
    para(tf, t, size=14, color=c, first=(i == 0), after=14, line=1.40)

rect(s, 1.05, 5.90, 5.2, 0.60, fill=NAVY2, radius=0.06)
para(textbox(s, 1.28, 5.90, 4.8, 0.60, anchor=MSO_ANCHOR.MIDDLE),
     "下一步：落 M3-p1 的契约头 + 网关骨架 + 边界检查", size=11,
     color=RGBColor(0x9F, 0xB6, 0xD4), first=True, after=0)

OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                   "文件系统四模块-实现与交互关系.pptx")
prs.save(OUT)
print("saved:", OUT, "slides:", len(prs.slides))
