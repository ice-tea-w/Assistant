# -*- coding: utf-8 -*-
"""Assistant —— 分支进展汇报 PPT（13 页）

覆盖：主分支（Phase 1–3 + A1–A4）、feature/plugin-market、qianju、file_guard，
含完成度、实测数据、合并路线，以及一页留给汇报人插入实际效果图。

用法： python make_deck_branches.py
"""
import os

from pptx import Presentation
from pptx.chart.data import CategoryChartData
from pptx.dml.color import RGBColor
from pptx.enum.chart import XL_CHART_TYPE, XL_LEGEND_POSITION
from pptx.enum.dml import MSO_LINE_DASH_STYLE
from pptx.enum.shapes import MSO_SHAPE
from pptx.enum.text import MSO_ANCHOR, PP_ALIGN
from pptx.oxml.ns import qn
from pptx.util import Inches, Pt

FONT = "Microsoft YaHei"
NAVY = RGBColor(0x14, 0x22, 0x38)
NAVY2 = RGBColor(0x1E, 0x30, 0x4C)
BLUE = RGBColor(0x4A, 0x90, 0xD9)
BLUE_D = RGBColor(0x2C, 0x5F, 0x9E)
BLUE_L = RGBColor(0xE8, 0xF1, 0xFB)
INK = RGBColor(0x1A, 0x1A, 0x1A)
MUTED = RGBColor(0x5E, 0x6A, 0x7A)
MUTED_L = RGBColor(0x8E, 0x9A, 0xAD)
LINE = RGBColor(0xD8, 0xDE, 0xE6)
WHITE = RGBColor(0xFF, 0xFF, 0xFF)
GREEN = RGBColor(0x2E, 0x8B, 0x57)
GREEN_L = RGBColor(0xE7, 0xF4, 0xEC)
AMBER = RGBColor(0xB5, 0x6E, 0x00)
AMBER_L = RGBColor(0xFD, 0xF3, 0xE2)
RED = RGBColor(0xC0, 0x39, 0x2B)
RED_L = RGBColor(0xFB, 0xEC, 0xEA)
VIOLET = RGBColor(0x5B, 0x4B, 0x8A)
VIOLET_L = RGBColor(0xEE, 0xEB, 0xF6)
BG = RGBColor(0xF7, 0xF9, 0xFC)
ZEBRA = RGBColor(0xF1, 0xF5, 0xFA)

C_M1, C_M2, C_M3, C_M4 = BLUE, GREEN, RED, VIOLET

W, H = 13.333, 7.5
M, CW = 0.75, 11.833


def style_font(run, size=None, bold=None, color=None, name=FONT, italic=None):
    run.font.name = name
    rPr = run._r.get_or_add_rPr()
    for tag in ("a:ea", "a:cs"):
        el = rPr.find(qn(tag))
        if el is None:
            el = rPr.makeelement(qn(tag), {})
            rPr.append(el)
        el.set("typeface", name)
    if size is not None:
        run.font.size = Pt(size)
    if bold is not None:
        run.font.bold = bold
    if italic is not None:
        run.font.italic = italic
    if color is not None:
        run.font.color.rgb = color


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
    p.space_before = Pt(before)
    p.space_after = Pt(after)
    p.line_spacing = line
    r = p.add_run()
    r.text = text
    style_font(r, size=size, bold=bold, color=color, italic=italic, name=font)
    return p


def rich(tf, segs, size=13, first=False, after=5, line=1.25, align=PP_ALIGN.LEFT):
    p = tf.paragraphs[0] if first else tf.add_paragraph()
    p.alignment = align
    p.space_before = Pt(0)
    p.space_after = Pt(after)
    p.line_spacing = line
    for t, b, c in segs:
        r = p.add_run()
        r.text = t
        style_font(r, size=size, bold=b, color=c)
    return p


def rect(slide, x, y, w, h, fill=None, line_color=None, line_w=0.75, radius=None):
    shp = slide.shapes.add_shape(
        MSO_SHAPE.ROUNDED_RECTANGLE if radius else MSO_SHAPE.RECTANGLE,
        Inches(x), Inches(y), Inches(w), Inches(h))
    if fill is None:
        shp.fill.background()
    else:
        shp.fill.solid()
        shp.fill.fore_color.rgb = fill
    if line_color is None:
        shp.line.fill.background()
    else:
        shp.line.color.rgb = line_color
        shp.line.width = Pt(line_w)
    shp.shadow.inherit = False
    if radius:
        shp.adjustments[0] = radius
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
            cell.margin_left = Inches(0.11)
            cell.margin_right = Inches(0.08)
            cell.margin_top = Inches(0.03)
            cell.margin_bottom = Inches(0.03)
            cell.vertical_anchor = MSO_ANCHOR.MIDDLE
            cell.fill.solid()
            cell.fill.fore_color.rgb = NAVY if r == 0 else (WHITE if r % 2 else ZEBRA)
            tf = cell.text_frame
            tf.word_wrap = True
            p = tf.paragraphs[0]
            p.line_spacing = 1.06
            p.alignment = aligns[c] if aligns else PP_ALIGN.LEFT
            run = p.add_run()
            run.text = str(val)
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


def notes(slide, text):
    slide.notes_slide.notes_text_frame.text = text


prs = Presentation()
prs.slide_width, prs.slide_height = Inches(W), Inches(H)
PG = [1]


def nxt(s, dark=False):
    PG[0] += 1
    pageno(s, PG[0], dark)
    return s


# ================================================================ 01 封面
s = blank(prs, NAVY)
rect(s, 0, 0, 0.17, H, fill=BLUE)
para(textbox(s, 1.05, 1.90, 7.6, 0.36), "ASSISTANT · 分支进展汇报", size=14, bold=True,
     color=BLUE, first=True, after=0)
para(textbox(s, 1.05, 2.40, 8.4, 1.15), "三条支线，一个主模块", size=40, bold=True,
     color=WHITE, first=True, after=0)
para(textbox(s, 1.05, 3.70, 8.2, 0.9), "插件市场 · 设备同步 · 文件安全\n各自做到哪一步、怎么合并、还差什么",
     size=16, color=RGBColor(0xB9, 0xC6, 0xD8), first=True, after=0, line=1.35)
rect(s, 1.05, 5.05, 3.0, 0.03, fill=RGBColor(0x35, 0x4A, 0x66))
para(textbox(s, 1.05, 5.35, 8.2, 0.4), "软件工程设计项目 · 2026 年 10 月", size=13,
     color=MUTED_L, first=True, after=0)

rect(s, 9.45, 2.10, 3.05, 2.95, fill=NAVY2, radius=0.05)
para(textbox(s, 9.80, 2.34, 2.4, 0.32), "今天只讲三件事", size=12.5, bold=True,
     color=BLUE, first=True, after=0)
tf = textbox(s, 9.80, 2.76, 2.45, 2.0)
for i, t in enumerate(["已经完成了什么（可验收）",
                       "三条支线各自缺什么",
                       "按什么顺序并回 main"]):
    para(tf, "·  " + t, size=11.5, color=RGBColor(0xC7, 0xD4, 0xE6), first=(i == 0),
         after=14, line=1.3)
notes(s, "开场：这次汇报不讲愿景，只讲四个提交点的实际状态——主分支到 A4，三条支线各自完成度不同，"
         "重点是给出可执行的合并顺序。")

# ================================================================ 02 汇报地图
s = nxt(blank(prs))
header(s, "MAP", "四个提交点，一张地图")

items = [
    ("主分支 main", "M1 文件管理", "9 个提交待推 · A2–A4", "已落地", C_M1, BLUE_L),
    ("feature/plugin-market", "M4 插件", "7 提交 · 35 文件 · +11840 行", "已完成待合并", C_M4, VIOLET_L),
    ("qianju", "M2 传输", "6 提交 · 31 文件 · +6592 行", "通道完成", C_M2, GREEN_L),
    ("file_guard", "M3 安全", "1 提交 · 62 文件 · +9121 行", "Python 原型", C_M3, RED_L),
]
x0, wcard, gap = 0.75, 2.85, 0.14
for i, (name, mod, meta, state, accent, bg) in enumerate(items):
    x = x0 + i * (wcard + gap)
    rect(s, x, 1.95, wcard, 2.58, fill=WHITE, line_color=LINE, radius=0.05)
    rect(s, x, 1.95, wcard, 0.10, fill=accent)
    para(textbox(s, x + 0.24, 2.24, wcard - 0.45, 0.30), name, size=12, bold=True,
         color=accent, first=True, after=0)
    para(textbox(s, x + 0.24, 2.66, wcard - 0.45, 0.55), mod, size=17, bold=True,
         color=INK, first=True, after=0)
    para(textbox(s, x + 0.24, 3.32, wcard - 0.45, 0.75), meta, size=10.5, color=MUTED,
         first=True, after=0, line=1.25)
    chip(s, x + 0.24, 3.86, 1.95, 0.40, state, bg, accent, 11)

para(textbox(s, M, 5.30, CW, 0.40), "判定标准只有一条：能不能在真机上跑出可复现的结果，而不是写了多少行。",
     size=15, bold=True, color=INK, first=True, after=0)
rich(textbox(s, M, 5.86, CW, 0.9),
     [("三条支线不是三个平行功能：", True, INK),
      ("插件市场 = M4，设备同步 = M2 的“渠道目录通道”，文件安全 = M3 的参考实现；", False, MUTED)],
     size=12.5, first=True, line=1.35)
notes(s, "点题：三条支线分别对应四模块里的 M4、M2、M3，主分支承载 M1。正因为是不同模块，"
         "合并顺序才不能随便排。")

# ================================================================ 03 主分支已完成
s = nxt(blank(prs))
header(s, "MAIN", "主分支：从 Hello 窗口到 A4 查询引擎")

phases = [
    ("Phase 1", "MVP 骨架", ["托盘 + 全局热键", "QML 窗口 + 配置", "主题切换"], C_M1),
    ("Phase 2", "v0.2.0", ["日志 / 历史库", "五级模糊匹配", "计算器 + 应用索引"], C_M1),
    ("Phase 3", "v0.3.0", ["主面板四页", "热键自定义", "开机自启动"], C_M1),
    ("A1–A3", "v0.4.0", ["止血：不跟随 junction", "Win32 扫描 + 差量库", "迁移框架"], C_M1),
    ("A4", "v0.4.0", ["QueryParser / ScoreEngine", "top-K 最小堆", "基线落盘"], C_M1),
]
x0, wcard, gap = 0.75, 2.24, 0.13
for i, (tag, name, lines, accent) in enumerate(phases):
    x = x0 + i * (wcard + gap)
    rect(s, x, 2.05, wcard, 2.12, fill=WHITE, line_color=LINE, radius=0.05)
    chip(s, x + 0.18, 2.18, 0.95, 0.30, tag, accent, WHITE, 10)
    para(textbox(s, x + 0.18, 2.54, wcard - 0.36, 0.32), name, size=14, bold=True,
         color=INK, first=True, after=0)
    tf = textbox(s, x + 0.18, 2.94, wcard - 0.34, 1.20)
    for j, ln in enumerate(lines):
        para(tf, "· " + ln, size=9.5, color=MUTED, first=(j == 0), after=5, line=1.2)
    if i < len(phases) - 1:
        arrow(s, x + wcard + 0.005, 3.00, color=LINE, size=12)

rect(s, 0.75, 4.62, 5.75, 1.62, fill=WHITE, line_color=LINE, radius=0.05)
para(textbox(s, 1.05, 4.80, 5.2, 0.32), "现在能演示的", size=13, bold=True, color=BLUE,
     first=True, after=0)
tf = textbox(s, 1.05, 5.18, 5.2, 0.90)
para(tf, "Alt+Space 唤起 → 输入 → 应用 / 文件 / 计算器三类结果 → 回车执行",
     size=11.5, color=MUTED, first=True, after=4, line=1.3)
para(tf, "主面板：待办 / 笔记 / 剪贴板 / 设置四页，热键与开机启动可改", size=11.5,
     color=MUTED, after=0, line=1.3)

rect(s, 6.80, 4.62, 5.78, 1.62, fill=BLUE_L, line_color=BLUE_L, radius=0.05)
para(textbox(s, 7.10, 4.80, 5.2, 0.32), "版本口径", size=13, bold=True, color=BLUE_D,
     first=True, after=0)
para(textbox(s, 7.10, 5.18, 5.2, 0.95),
     "v0.3.0 的四个页面已冻结；v0.4.0 只做文件子系统 A 阶段，A5 增量与 A6 冷启动尚未开始。",
     size=11.5, color=BLUE_D, first=True, after=0, line=1.3)
notes(s, "主分支的完成度最高：Phase 1–3 可演示，A1–A4 有独立验收程序（golden / a3 / bench）。")

# ================================================================ 04 索引链路
s = nxt(blank(prs))
header(s, "M1", "文件索引：一条链路，三个不变量")

steps = [
    ("FileScanner", "Win32 句柄栈 DFS\n目录级剪枝、不跟 junction", C_M1),
    ("差量写库", "dirs / files / roots\n三方差集 + 批量 UPSERT", C_M1),
    ("FileSnapshot", "不可变列式快照\n构建完即冻结", GREEN),
    ("SnapshotManager", "atomic<shared_ptr>\n一次发布，替换旧的", GREEN),
    ("QueryEngine", "Parser → Score → top-K\n全程零分配", AMBER),
]
x0, wcard, gap = 0.75, 2.14, 0.20
for i, (t, sub, accent) in enumerate(steps):
    x = x0 + i * (wcard + gap)
    node(s, x, 2.05, wcard, 1.28, t, sub, accent)
    if i < len(steps) - 1:
        arrow(s, x + wcard + 0.02, 2.55, color=MUTED_L, size=11)

para(textbox(s, M, 3.62, CW, 0.34), "三个不变量（也是三条验收依据）", size=13, bold=True,
     color=INK, first=True, after=0)
blocks = [
    ("快照不可变", "写侧永远构建新快照再原子发布；读侧拿到的对象整轮查询都不会变。", BLUE_L, BLUE_D),
    ("查询零锁", "读侧只取一份 shared_ptr，不加读写锁，也不会读到半成品。", GREEN_L, GREEN),
    ("顺序一致", "DFS 前序与旧实现逐条相同——硬上限截断按顺序进行，顺序变了结果集就变。", AMBER_L, AMBER),
]
for i, (t, d, bg, fg) in enumerate(blocks):
    x = 0.75 + i * 3.95
    rect(s, x, 4.05, 3.78, 1.28, fill=bg, line_color=bg, radius=0.05)
    para(textbox(s, x + 0.28, 4.22, 3.3, 0.30), t, size=14, bold=True, color=fg,
         first=True, after=0)
    para(textbox(s, x + 0.28, 4.60, 3.3, 0.62), d, size=10.5, color=INK, first=True,
         after=0, line=1.25)

para(textbox(s, M, 5.62, CW, 1.1),
     "为什么值得这么拆：如果把扫描、写库、查询放进同一个对象，锁的粒度必然退化到“整份索引”，"
     "重建期间界面就会卡。不可变快照把“写”和“读”彻底分开，代价只是一次指针替换。",
     size=12, color=MUTED, first=True, after=0, line=1.45)
notes(s, "这一页讲设计动机：不是追求时髦的数据结构，而是为了让重建索引时 UI 不卡。")

# ================================================================ 05 性能
s = nxt(blank(prs))
header(s, "DATA", "实测：一次扫描 0.5 秒，一次查询 4 毫秒")

rows = [
    ["指标", "旧实现", "当前实现", "提升"],
    ["2 万条 1–3 字符查询", "18.7 ms", "3.9 ms", "4.8×"],
    ["20 万条 1–3 字符查询", "275 ms", "35.4 ms", "7.8×"],
    ["3.2 万文件全量扫描", "—", "459–502 ms", "—"],
    ["冷启动到可查询", "—", "28.7 ms", "达标"],
    ["目录级剪枝", "—", "97.7%", "—"],
]
table(s, 0.75, 1.95, 5.65, rows, [1.95, 1.15, 1.35, 1.20], row_h=0.52, head_h=0.46,
      fs=11.5, head_fs=11.5,
      aligns=[PP_ALIGN.LEFT, PP_ALIGN.CENTER, PP_ALIGN.CENTER, PP_ALIGN.CENTER],
      colors=[[None, None, GREEN, GREEN], [None, None, GREEN, GREEN],
              [None, None, None, None], [None, None, None, None], [None, None, None, None]])

data = CategoryChartData()
data.categories = ["2 万条", "20 万条"]
data.add_series("旧实现", (18.7, 275.0))
data.add_series("当前实现", (3.9, 35.4))
gf = s.shapes.add_chart(XL_CHART_TYPE.COLUMN_CLUSTERED, Inches(6.70), Inches(1.90),
                        Inches(5.90), Inches(3.55), data)
chart = gf.chart
chart.has_legend = True
chart.legend.position = XL_LEGEND_POSITION.BOTTOM
chart.legend.include_in_layout = False
chart.font.size = Pt(11)
chart.font.name = FONT
plot = chart.plots[0]
plot.gap_width = 90
plot.has_data_labels = True
dl = plot.data_labels
dl.font.size = Pt(10)
dl.font.name = FONT
series_colors = [MUTED_L, BLUE]
for i, ser in enumerate(chart.series):
    ser.format.fill.solid()
    ser.format.fill.fore_color.rgb = series_colors[i]
chart.value_axis.has_major_gridlines = True
chart.value_axis.major_gridlines.format.line.color.rgb = LINE
chart.value_axis.tick_labels.font.size = Pt(10)
chart.category_axis.tick_labels.font.size = Pt(11)

para(textbox(s, 6.70, 5.55, 5.9, 0.32), "查询耗时（ms，越低越好）", size=10.5, color=MUTED,
     first=True, after=0)
footnote(s, "Release 构建 · 同一台机器 · 同一份语料；Debug 同口径约慢 30 倍，所有结论以 Release 为准。")
notes(s, "强调两点：性能结论只在 Release 成立；20 万条仍未达到 3 ms 的目标，下一步是位图预筛和目录字典编码。")

# ================================================================ 06 插件市场
s = nxt(blank(prs))
header(s, "M4 · feature/plugin-market", "插件市场：从安装到出现在导航里")

milestones = [
    ("M0", "本地插件管理", True),
    ("M1", "市场目录 + 种子索引", True),
    ("M2", "本地 zip / 在线安装", True),
    ("M3", "启用、禁用、卸载", True),
    ("M4", "插件贡献页面进入导航", True),
]
for i, (tag, name, done) in enumerate(milestones):
    y = 1.95 + i * 0.66
    chip(s, 0.75, y, 0.62, 0.34, tag, C_M4 if done else LINE, WHITE, 10)
    para(textbox(s, 1.50, y + 0.02, 3.4, 0.32), name, size=12.5, bold=False,
         color=INK, first=True, after=0)
    chip(s, 4.55, y, 0.72, 0.30, "完成", VIOLET_L, C_M4, 9.5)

para(textbox(s, 0.75, 5.32, 4.6, 0.30), "7 提交 · 35 文件 · +11840 行 · 2681 行测试", size=10.5,
     color=MUTED, first=True, after=0)

para(textbox(s, 5.75, 1.92, 6.6, 0.32), "安装流水线（失败即清理，不留半安装）", size=12.5,
     bold=True, color=INK, first=True, after=0)
flow = ["inspect", "download", "extract", "validate", "commit"]
for i, step in enumerate(flow):
    x = 5.75 + i * 1.32
    rect(s, x, 2.30, 1.18, 0.52, fill=WHITE, line_color=C_M4, line_w=1.0, radius=0.10)
    para(textbox(s, x, 2.30, 1.18, 0.52, anchor=MSO_ANCHOR.MIDDLE), step, size=10.5,
         bold=True, color=C_M4, first=True, align=PP_ALIGN.CENTER, after=0)
    if i < 4:
        pass  # 相邻卡片不再画箭头，避免与边框重叠

bullets = [
    ("zip-slip 防护", "解压时逐条校验目标路径，拒绝跳出插件目录的相对路径。"),
    ("sha256 校验", "包与清单双重校验，校验不过直接终止并清理临时目录。"),
    ("导航 key 化", "页面以 key 标识，插件页运行时注入——加页面不再影响既有索引判断。"),
    ("宿主面收窄", "每个插件只拿到一个 PluginHost，看不到引擎、注册表、管理器。"),
]
for i, (t, d) in enumerate(bullets):
    y = 3.10 + i * 0.86
    rect(s, 5.75, y, 0.05, 0.66, fill=C_M4)
    para(textbox(s, 5.95, y, 6.3, 0.28), t, size=12.5, bold=True, color=INK, first=True, after=0)
    para(textbox(s, 5.95, y + 0.30, 6.3, 0.36), d, size=10.5, color=MUTED, first=True, after=0)

chip(s, 10.25, 1.88, 2.10, 0.34, "已完成 · 待在 P1 合并", VIOLET_L, C_M4, 10.5)
notes(s, "插件市场是三条支线里完成度最高的：M0–M4 全通，有 2681 行独立测试；缺的是能力校验与签名策略。")

# ================================================================ 07 设备同步
s = nxt(blank(prs))
header(s, "M2 · qianju", "设备同步：先做通道，不做网络")

rect(s, 0.75, 1.95, 7.15, 2.30, fill=WHITE, line_color=LINE, radius=0.05)
para(textbox(s, 1.02, 2.12, 6.6, 0.30), "传输抽象", size=12.5, bold=True, color=INK,
     first=True, after=0)
node(s, 1.02, 2.52, 2.05, 1.20, "ITransport", "probe / listRemote\nfetch / push / remove", C_M2)
arrow(s, 3.12, 2.98, color=MUTED_L, size=12)
node(s, 3.48, 2.52, 2.05, 1.20, "FolderTransport", "投递区 = 共享文件夹\n清单 + 原子写", C_M2)
arrow(s, 5.58, 2.98, color=MUTED_L, size=12)
node(s, 5.94, 2.52, 1.80, 1.20, "LanTransport", "设计中\n未实现", MUTED_L)

para(textbox(s, 0.75, 4.42, 7.2, 0.30), "同步引擎", size=12.5, bold=True, color=INK,
     first=True, after=0)
engine = [
    ("三方差量", "本地 / 对端 / 基线"),
    ("冲突副本", "按设备名 + 时间命名"),
    ("PathGuard", "穿越、保留名、junction"),
    ("SyncDatabase", "基线 / 冲突 / 接收记录"),
]
for i, (t, d) in enumerate(engine):
    x = 0.75 + i * 1.82
    rect(s, x, 4.78, 1.72, 1.05, fill=GREEN_L, line_color=GREEN_L, radius=0.06)
    para(textbox(s, x + 0.16, 4.92, 1.45, 0.28), t, size=11.5, bold=True, color=GREEN,
         first=True, after=0)
    para(textbox(s, x + 0.16, 5.22, 1.45, 0.52), d, size=9.5, color=INK, first=True,
         after=0, line=1.2)

rect(s, 8.20, 1.95, 4.38, 3.88, fill=AMBER_L, line_color=AMBER_L, radius=0.05)
para(textbox(s, 8.48, 2.12, 3.9, 0.32), "必须说清的一件事", size=12.5, bold=True,
     color=AMBER, first=True, after=0)
tf = textbox(s, 8.48, 2.55, 3.85, 3.1)
for i, ln in enumerate([
    "代码里没有任何 QTcpSocket / QSslSocket，",
    "LanTransport 只存在于设计文档。",
    "",
    "所以它现在的定位是 M2 的“②渠道目录通道”，",
    "不是 LAN 传输，也不能宣称能传文件到另一台电脑。",
    "",
    "单机双实例（--instance）演示的是差量与冲突逻辑，不是网络。",
    "",
    "对外统一表述：通道已通，网络未做。",
]):
    para(tf, ln, size=10.5, color=INK, first=(i == 0), after=2, line=1.35)

para(textbox(s, 0.75, 6.02, 11.6, 0.6), "合并后定位：作为“实验通道”进入 main，作业内核（分块 / 最终哈希 / 隔离区）另开分支补齐。",
     size=11.5, bold=True, color=INK, first=True, after=0)
notes(s, "这一页要主动说清边界：它是通道，不是网络传输。否则答辩被问“怎么证明两台机器传成功了”会答不上来。")

# ================================================================ 08 安全
s = nxt(blank(prs))
header(s, "M3 · file_guard", "文件安全：一个能跑的 Python 原型")

para(textbox(s, M, 1.90, CW, 0.32), "原型做了四件事（都不是 C++ 实现）", size=12.5, bold=True,
     color=INK, first=True, after=0)
caps = [
    ("保险箱", "FGC1 分块 AES-256-GCM\n每块 AAD 绑定序号与末块标记", C_M3),
    ("审计链", "HMAC 链 + next_seq 高水位\n删行 / 回滚可检出", C_M3),
    ("敏感扫描", "正则 + 校验位 + Office 文本\n按分值给出风险等级", C_M3),
    ("增量备份", "内容寻址 + 加密索引\n对象名由密钥派生", C_M3),
]
for i, (t, d, c) in enumerate(caps):
    x = 0.75 + i * 2.98
    rect(s, x, 2.30, 2.82, 1.42, fill=WHITE, line_color=LINE, radius=0.05)
    rect(s, x, 2.30, 0.05, 1.42, fill=c)
    para(textbox(s, x + 0.24, 2.44, 2.4, 0.30), t, size=13, bold=True, color=INK,
         first=True, after=0)
    para(textbox(s, x + 0.24, 2.78, 2.45, 0.80), d, size=9.5, color=MUTED, first=True,
         after=0, line=1.25)

para(textbox(s, M, 3.98, CW, 0.32), "可以直接吸收的设计（写 C++ 时用）", size=12.5, bold=True,
     color=INK, first=True, after=0)
absorb = [
    "容器头 + 每块 AAD(index, is_final)：防截断、防换块序、防追加",
    "审计链 HMAC(chain, h-1 || nonce || ct) + 高水位 + 外部锚点",
    "DPAPI 缓存数据密钥（与提案 §7 选的路径一致）",
    "junction 统一跳过（和 A1 踩过的 NoSymLinks 坑同源）",
]
for i, t in enumerate(absorb):
    y = 4.38 + i * 0.46
    rect(s, 0.78, y + 0.13, 0.07, 0.07, fill=C_M3)
    para(textbox(s, 1.00, y, 7.4, 0.36), t, size=11, color=MUTED, first=True, after=0)

rect(s, 8.55, 3.98, 4.05, 2.42, fill=RED_L, line_color=RED_L, radius=0.05)
para(textbox(s, 8.82, 4.12, 3.6, 0.30), "必须对表的四处", size=12, bold=True, color=RED,
     first=True, after=0)
tf = textbox(s, 8.82, 4.50, 3.55, 1.75)
for i, ln in enumerate(["无恢复码（提案要求）",
                        "文件名不加密（提案要求名称加密）",
                        "账本要落 security.db，不是加密 JSONL",
                        "scan 读正文，与“默认关闭”决策冲突"]):
    para(tf, "· " + ln, size=9.5, color=INK, first=(i == 0), after=5, line=1.25)

chip(s, 10.55, 6.42, 2.05, 0.36, "设计参考 · 不进构建", RED_L, RED, 10.5)
notes(s, "安全模块是三条支线里离产品最远的：Python 原型、宿主假设是 PyQt，只能借设计。")

# ================================================================ 09 对比矩阵
s = nxt(blank(prs))
header(s, "MATRIX", "四分支横向对比")

rows = [
    ["分支", "模块", "提交", "规模", "状态", "合并难度"],
    ["main", "M1 文件管理", "9", "+11840（A2–A4）", "已落地", "—"],
    ["feature/plugin-market", "M4 插件", "7", "35 文件 +11840 行", "已完成", "低（0 冲突）"],
    ["qianju", "M2 传输", "6", "31 文件 +6592 行", "通道完成", "中（5 文件冲突）"],
    ["file_guard", "M3 安全", "1", "62 文件 +9121 行", "Python 原型", "高（会顶掉 README）"],
]
table(s, 0.75, 1.92, 11.60, rows, [2.65, 1.85, 0.85, 2.35, 1.75, 2.15],
      row_h=0.52, head_h=0.46, fs=11.5, head_fs=11.5,
      aligns=[PP_ALIGN.LEFT, PP_ALIGN.LEFT, PP_ALIGN.CENTER, PP_ALIGN.LEFT,
              PP_ALIGN.CENTER, PP_ALIGN.CENTER],
      colors=[[C_M1, None, None, None, GREEN, None],
              [C_M4, None, None, None, C_M4, GREEN],
              [C_M2, None, None, None, AMBER, AMBER],
              [C_M3, None, None, None, RED, RED]])

para(textbox(s, M, 4.92, CW, 0.32), "冲突不是文本问题，是三件“谁说了算”", size=12.5, bold=True,
     color=INK, first=True, after=0)
trio = [
    ("导航机制", "StackLayout + index → Loader + key\n由插件市场分支定义，其余 rebase 到它之上"),
    ("CMake 基线", "两套 VCPKG_INSTALLED_DIR 写法\n取 ${VCPKG_TARGET_TRIPLET} + FATAL_ERROR 守卫"),
    ("测试入口", "ctest / QTest / 独立工程三套并存\n统一为 tests/ + Qt6::Test + add_test"),
]
for i, (t, d) in enumerate(trio):
    x = 0.75 + i * 3.98
    rect(s, x, 5.30, 3.80, 1.30, fill=WHITE, line_color=LINE, radius=0.05)
    para(textbox(s, x + 0.26, 5.44, 3.3, 0.30), t, size=12.5, bold=True, color=BLUE_D,
         first=True, after=0)
    para(textbox(s, x + 0.26, 5.78, 3.3, 0.72), d, size=10, color=MUTED, first=True,
         after=0, line=1.3)
notes(s, "横向对比的结论：插件市场可以先合，同步要 rebase，file_guard 只归档。")

# ================================================================ 10 效果图占位
s = nxt(blank(prs))
header(s, "SCREENSHOTS", "实际效果（请在此插入截图）")

para(textbox(s, M, 1.62, CW, 0.32),
     "四个虚线框已排好版：把真机截图拖进去，等比缩放铺满框即可（保持 16:10 更整齐）。",
     size=11.5, color=MUTED, first=True, after=0)

frames = [
    ("搜索浮窗", "Alt+Space → 输入 → 结果列表", 0.75, 2.06, 5.80, 2.24),
    ("主面板", "待办 / 笔记 / 剪贴板 / 设置", 6.78, 2.06, 5.80, 2.24),
    ("插件市场", "已安装列表 + 市场目录", 0.75, 4.44, 5.80, 2.24),
    ("设备同步", "同步根 / 通道 / 冲突副本", 6.78, 4.44, 5.80, 2.24),
]
for name, sub, x, y, w, h in frames:
    shp = rect(s, x, y, w, h, fill=WHITE, line_color=BLUE, line_w=1.25, radius=0.03)
    shp.line.dash_style = MSO_LINE_DASH_STYLE.DASH
    para(textbox(s, x, y + h / 2 - 0.40, w, 0.34), name, size=13, bold=True,
         color=MUTED_L, first=True, align=PP_ALIGN.CENTER, after=0)
    para(textbox(s, x, y + h / 2 + 0.00, w, 0.34), sub, size=10, color=MUTED_L,
         first=True, align=PP_ALIGN.CENTER, after=0)

para(textbox(s, M, 6.86, 9.6, 0.30),
     "提示：插入后在“图片格式”里选“裁剪 → 填充”，框不会变形；四个框可整组复制到其他页。",
     size=9.5, color=MUTED_L, first=True, after=0)
notes(s, "这一页留给汇报人自己贴图，交付时是空的虚线框。")

# ================================================================ 11 完成度
s = nxt(blank(prs))
header(s, "STATUS", "完成度：M1 最深，M3 最浅")

mods = [
    ("M1 文件管理", 0.72, C_M1, "A2–A4 已落地；A5/A6 未开始"),
    ("M2 传输与共享", 0.30, C_M2, "通道可用；内核/LAN 未开始"),
    ("M3 安全", 0.12, C_M3, "Python 原型；C++ 零实现"),
    ("M4 插件", 0.62, C_M4, "M0–M4 全通；能力校验待补"),
]
for i, (name, pct, c, note) in enumerate(mods):
    y = 1.95 + i * 1.18
    para(textbox(s, 0.75, y, 2.55, 0.34), name, size=13.5, bold=True, color=INK,
         first=True, after=0)
    rect(s, 3.50, y + 0.06, 6.05, 0.24, fill=RGBColor(0xE6, 0xEC, 0xF4), radius=0.5)
    rect(s, 3.50, y + 0.06, max(0.25, 6.05 * pct), 0.24, fill=c, radius=0.5)
    para(textbox(s, 9.70, y, 0.75, 0.34), f"{int(pct * 100)}%", size=13, bold=True, color=c,
         first=True, align=PP_ALIGN.RIGHT, after=0)
    para(textbox(s, 10.60, y + 0.02, 2.0, 0.34), note, size=9.5, color=MUTED, first=True,
         after=0, line=1.2)

rect(s, 0.75, 6.28, 11.83, 0.52, fill=BLUE_L, line_color=BLUE_L, radius=0.05)
para(textbox(s, 0.95, 6.28, 11.45, 0.52, anchor=MSO_ANCHOR.MIDDLE),
     "百分比是“可验收范围”的估计，不是代码行数：能跑、能测、能复现才算。",
     size=10.5, color=BLUE_D, first=True, after=0)
notes(s, "不要把百分比说成精确值，它是按验收项估的。")

# ================================================================ 12 合并路线
s = nxt(blank(prs))
header(s, "NEXT", "合并路线：先立规矩，再合代码")

steps = [
    ("P0", "仓库卫生", "推 main · .gitignore · 删残留分支 · 落 C1 契约骨架", MUTED_L),
    ("P1", "合并插件市场", "带 CMake 基线修复与导航 key 化；前置 vcpkg install miniz", C_M4),
    ("P2", "rebase 同步分支", "冲突 7 个文件；顺带统一测试入口", C_M2),
    ("P3", "归档 file_guard", "docs/reference + tools/，不进构建", C_M3),
    ("P4", "补内核与判定", "M2 作业内核、M3-p1 判定与账本", C_M1),
]
for i, (tag, name, desc, c) in enumerate(steps):
    y = 1.92 + i * 0.92
    chip(s, 0.75, y + 0.10, 0.72, 0.40, tag, c if c != MUTED_L else MUTED, WHITE, 12)
    para(textbox(s, 1.66, y + 0.02, 3.2, 0.34), name, size=13.5, bold=True, color=INK,
         first=True, after=0)
    para(textbox(s, 1.66, y + 0.38, 8.2, 0.34), desc, size=10.5, color=MUTED, first=True,
         after=0)
    if i < len(steps) - 1:
        rect(s, 1.00, y + 0.60, 0.02, 0.34, fill=LINE)

rect(s, 9.55, 1.92, 3.05, 2.78, fill=WHITE, line_color=LINE, radius=0.05)
para(textbox(s, 9.85, 2.08, 2.5, 0.30), "为什么是这个顺序", size=11.5, bold=True, color=BLUE,
     first=True, after=0)
tf = textbox(s, 9.85, 2.46, 2.45, 2.7)
for i, ln in enumerate(["插件市场决定导航模型",
                        "同步分支必须 rebase 到它上面",
                        "CMake / 测试只在第二个分支进来时才有确定答案",
                        "file_guard 语言不同，只能归档"]):
    para(tf, "· " + ln, size=9.5, color=MUTED, first=(i == 0), after=8, line=1.3)

para(textbox(s, 0.75, 6.55, 11.8, 0.40),
     "完成 P1–P2 之后，main 才第一次同时具备 M1、M2、M4 三个门面；M3 仍只有设计。",
     size=11, bold=True, color=INK, first=True, after=0)
notes(s, "这一页是行动项，讲完要落到“下周做什么”。")

# ================================================================ 13 结尾
s = nxt(blank(prs, NAVY), dark=True)
rect(s, 0, 0, 0.17, H, fill=BLUE)
para(textbox(s, 1.05, 1.85, 9.6, 0.40), "小结", size=14, bold=True, color=BLUE,
     first=True, after=0)
para(textbox(s, 1.05, 2.30, 10.2, 1.10), "一条主线，三条支线，按依赖合", size=34,
     bold=True, color=WHITE, first=True, after=0)

rows = [
    ("已交付", "搜索浮窗 + 主面板 + 文件索引 A2–A4，可演示、有验收程序", GREEN),
    ("已完成待并", "插件市场 M0–M4，0 冲突，P1 即可进入 main", C_M4),
    ("部分完成", "设备同步的通道层；作业内核与 LAN 仍未开始", C_M2),
    ("仅原型", "文件安全只有 Python 设计与原型，C++ 需要重写", C_M3),
]
for i, (tag, text, c) in enumerate(rows):
    y = 3.62 + i * 0.62
    para(textbox(s, 1.05, y, 1.55, 0.34), tag, size=11.5, bold=True, color=c, first=True,
         after=0)
    para(textbox(s, 2.65, y, 9.4, 0.34), text, size=11.5,
         color=RGBColor(0xC7, 0xD4, 0xE6), first=True, after=0)

para(textbox(s, 1.05, 6.30, 9.0, 0.40), "谢谢 · 欢迎提问", size=13, color=MUTED_L,
     first=True, after=0)
notes(s, "收尾：把“能合的先合、不能合的说清楚”作为结论，避免被追问进度时含糊。")

out = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                   "分支进展汇报-Assistant.pptx")
prs.save(out)
print("saved", out, len(prs.slides._sldIdLst), "slides")
