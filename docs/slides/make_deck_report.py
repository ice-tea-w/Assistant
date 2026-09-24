# -*- coding: utf-8 -*-
"""Assistant 文件检索模块 —— 【汇报版】PPT 生成脚本（11 页，要点式）
用法： python make_deck_report.py
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
            cell.margin_left = Inches(0.12); cell.margin_right = Inches(0.09)
            cell.margin_top = Inches(0.03);  cell.margin_bottom = Inches(0.03)
            cell.vertical_anchor = MSO_ANCHOR.MIDDLE
            cell.fill.solid()
            cell.fill.fore_color.rgb = NAVY if r == 0 else (WHITE if r % 2 else ZEBRA)
            tf = cell.text_frame; tf.word_wrap = True
            p = tf.paragraphs[0]; p.line_spacing = 1.08
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
         num=None):
    rect(slide, x, y, w, h, fill=WHITE, line_color=LINE, radius=0.05)
    rect(slide, x, y + 0.16, 0.055, h - 0.32, fill=accent)
    ty = y + 0.22
    if num:
        chip(slide, x + 0.3, y + 0.22, 0.46, 0.42, num, accent, WHITE, 13)
        para(textbox(slide, x + 0.9, y + 0.24, w - 1.15, 0.4), title, size=tsize,
             bold=True, color=INK, first=True, after=0)
    else:
        para(textbox(slide, x + 0.32, ty, w - 0.6, 0.4), title, size=tsize,
             bold=True, color=INK, first=True, after=0)
    tf = textbox(slide, x + 0.32, y + 0.78, w - 0.62, h - 0.95)
    for i, ln in enumerate(lines):
        para(tf, ln, size=bsize, color=MUTED, first=(i == 0), after=7, line=1.3)

prs = Presentation()
prs.slide_width, prs.slide_height = Inches(W), Inches(H)
PG = [0]
def nxt(s, dark=False):
    PG[0] += 1; pageno(s, PG[0], dark); return s

# ================================================================ 01 封面
s = blank(prs, NAVY)
rect(s, 0, 0, 0.17, H, fill=BLUE)
para(textbox(s, 1.05, 2.35, 7.0, 0.36), "ASSISTANT · 阶段汇报", size=14, bold=True,
     color=BLUE, first=True, after=0)
para(textbox(s, 1.05, 2.85, 8.0, 1.0), "文件检索模块", size=44, bold=True,
     color=WHITE, first=True, after=0)
para(textbox(s, 1.05, 4.05, 7.6, 0.5), "已经做了什么 · 发现了什么问题 · 接下来做什么",
     size=17, color=RGBColor(0xB9, 0xC6, 0xD8), first=True, after=0)
rect(s, 1.05, 4.85, 3.0, 0.03, fill=RGBColor(0x35, 0x4A, 0x66))
para(textbox(s, 1.05, 5.15, 7.6, 0.4), "2026-09-24", size=13, color=MUTED_L,
     first=True, after=0)

rect(s, 9.15, 2.15, 3.35, 3.2, fill=NAVY2, radius=0.05)
para(textbox(s, 9.55, 2.5, 2.6, 0.32), "本次汇报三点", size=12.5, bold=True,
     color=BLUE, first=True, after=0)
tf = textbox(s, 9.55, 2.95, 2.6, 2.2)
for i, t in enumerate(["搜索已经可用", "A1 止血已落地并通过验收",
                       "发现一个更严重的问题并排入计划", "后续分两步、共七个阶段"]):
    para(tf, "·  " + t, size=11.5, color=RGBColor(0xC7, 0xD4, 0xE6), first=(i == 0),
         after=11, line=1.3)

# ================================================================ 02 一页总览
s = nxt(blank(prs))
header(s, "OVERVIEW", "一页看懂")
items = [
    ("已完成", "搜索链路已经跑通", ["文件 / 应用 / 命令统一搜索，输入即出结果",
                                    "A1 止血落地：4 个问题修复，验收全部通过"], GREEN, GREEN_L),
    ("关键发现", "索引其实「名不副实」", ["桌面 3.5 万个文件就吃满了索引额度",
                                          "文档、下载等目录一个文件都没进来"], RED, RED_L),
    ("接下来", "两步走、七个阶段", ["先做搜索基建，把它从「能用」做成「可靠」",
                                    "再做文件治理：可控范围 + 不丢文件"], BLUE, BLUE_L),
]
for i, (tag, t, lines, ac, al) in enumerate(items):
    x = M + i * (CW / 3)
    w = CW / 3 - 0.25
    rect(s, x, BODY_TOP, w, 3.05, fill=WHITE, line_color=LINE, radius=0.05)
    rect(s, x, BODY_TOP, w, 0.06, fill=ac)
    chip(s, x + 0.32, BODY_TOP + 0.32, 1.15, 0.36, tag, al, ac, 12)
    para(textbox(s, x + 0.32, BODY_TOP + 0.9, w - 0.64, 0.5), t, size=16.5,
         bold=True, color=INK, first=True, after=0, line=1.2)
    tf = textbox(s, x + 0.32, BODY_TOP + 1.55, w - 0.64, 1.3)
    for j, ln in enumerate(lines):
        para(tf, "·  " + ln, size=11.5, color=MUTED, first=(j == 0), after=8, line=1.3)

rect(s, M, 5.25, CW, 1.05, fill=BLUE_L, radius=0.05)
rich(textbox(s, M + 0.4, 5.25, CW - 0.8, 1.05, anchor=MSO_ANCHOR.MIDDLE),
     [("一句话结论：", True, BLUE_D),
      ("搜索的主干功能已经完成并可演示；真正拖后腿的不是性能，而是"
       "「索引覆盖不全」这类正确性问题 —— 所以下一步先修地基，再往上加治理能力。",
       False, INK)], size=13.5, first=True, after=0, line=1.35)

# ================================================================ 03 已完成
s = nxt(blank(prs))
header(s, "已完成 · 能力", "现在能做什么")
cards = [
    ("输入即搜", ["全局热键唤起，输入文件名即可搜",
                  "模糊匹配 + 五级打分，拼错也能命中"], BLUE),
    ("多源统一排序", ["内置命令、已安装应用、文件放在一起排序",
                      "输入算式直接出结果，回车复制"], BLUE),
    ("覆盖常用目录", ["桌面、文档、下载、图片、音乐、视频",
                      "启动时建立索引，随打随搜"], BLUE),
    ("主面板", ["待办、笔记、剪贴板、设置四个页面",
                "上一阶段交付，与搜索浮窗并存"], GREEN),
]
for i, (t, lines, ac) in enumerate(cards):
    x = M + (i % 2) * (CW / 2 + 0.12)
    y = BODY_TOP + (i // 2) * 2.35
    card(s, x, y, CW / 2 - 0.12, 2.15, t, lines, accent=ac, tsize=16, bsize=12.5)

footnote(s, "已完成部分对应用户可直接体验的功能；技术上对应上一阶段的搜索链路与主面板。")

# ================================================================ 04 A1 止血
s = nxt(blank(prs))
header(s, "已完成 · 本次交付", "A1 止血：修掉 4 个每天都在发生的问题")
chip(s, M, BODY_TOP - 0.04, 1.35, 0.38, "已通过验收", GREEN_L, GREEN, 12)
para(textbox(s, M + 1.55, BODY_TOP - 0.04, 9.5, 0.38),
     "已提交为独立补丁，可单独回滚，不影响其它功能", size=11.5, color=MUTED,
     first=True, after=0)
rows = [
    ["问题（用户能感知到的）", "怎么修的", "效果"],
    ["某些目录会让索引重复甚至无限递归", "不再跟随这类特殊目录", "重复条目 126 → 0"],
    ["逐个文件做路径判断，拖慢启动", "命中就直接跳过整个目录", "剪掉 160 个目录"],
    ["每打一个字就写一次数据库", "改成「真正打开结果」时才记录", "历史表不再被垃圾刷屏"],
    ["每打一个字就全量搜索一次", "加入 40ms 防抖", "连打 8 个字符只搜 1 次"],
]
table(s, M, 2.35, CW, rows, [4.6, 4.4, 2.833], row_h=0.62, head_h=0.46,
      fs=12.5, head_fs=12.5)
footnote(s, "三条验收均有实测数据：重复条目归零、防抖次数、打字期间数据库零写入。")

# ================================================================ 05 关键发现
s = nxt(blank(prs))
header(s, "关键发现", "索引其实「名不副实」：用户搜不到自己的文档")
nums = [("35607", "桌面上的文件数", RED), ("20000", "索引上限", RED),
        ("0", "文档目录进入索引的文件数", RED)]
for i, (v, lab, c) in enumerate(nums):
    x = M + i * (CW / 3)
    w = CW / 3 - 0.25
    rect(s, x, BODY_TOP, w, 1.5, fill=WHITE, line_color=LINE, radius=0.06)
    para(textbox(s, x + 0.35, BODY_TOP + 0.2, w - 0.7, 0.7), v, size=32, bold=True,
         color=c, first=True, after=0)
    para(textbox(s, x + 0.35, BODY_TOP + 0.96, w - 0.7, 0.4), lab, size=11.5,
         color=MUTED, first=True, after=0)

rect(s, M, 3.65, CW, 1.5, fill=WHITE, line_color=LINE, radius=0.05)
rect(s, M, 3.65, 0.055, 1.5, fill=RED)
tf = textbox(s, M + 0.4, 3.88, CW - 0.8, 1.1)
rich(tf, [("问题现象：", True, RED),
          ("日志显示「索引 20000 个文件」一切正常，但用户搜索"
           "「文档 / 下载 / 图片」里的文件，永远搜不到。", False, INK)],
     size=13, first=True, after=8)
rich(tf, [("原因：", True, RED),
          ("索引有 2 万条上限，而桌面排在最前面、单个目录就超过 2 万，"
           "遍历到它就把额度用光了，后面的目录根本没开始。", False, INK)],
     size=13, after=0)

rect(s, M, 5.4, CW, 1.05, fill=AMBER_L, radius=0.05)
rich(textbox(s, M + 0.4, 5.4, CW - 0.8, 1.05, anchor=MSO_ANCHOR.MIDDLE),
     [("已列入计划：", True, AMBER),
      ("这类「看起来正常、实际没生效」的问题优先级高于性能优化，"
       "在后续的索引重建阶段一并解决 —— 宁可少收文件，也不能让搜索漏掉用户真的有的文件。",
       False, INK)], size=13, first=True, after=0, line=1.35)

# ================================================================ 06 计划总览
s = nxt(blank(prs))
header(s, "后续计划", "两步走：先把搜索做稳，再让文件可控")
card(s, M, BODY_TOP, CW / 2 - 0.15, 3.9, "第一步：搜索基建",
     ["目标：把搜索从「能用」做成「可靠基础设施」",
      "",
      "·  建立持久化索引，启动即用、不再每次重扫",
      "·  后台扫描 + 增量更新，新建或改名立刻可见",
      "·  独立查询引擎，并先量性能基准再谈优化",
      "·  统一结果归并，为后续插件化留出接口"],
     accent=BLUE, tsize=17, bsize=12.5)
card(s, M + CW / 2 + 0.15, BODY_TOP, CW / 2 - 0.15, 3.9, "第二步：文件治理",
     ["目标：让搜索「范围可控」、让文件「不丢」",
      "",
      "·  种类过滤：只要文档 / 只要图片，一键切换",
      "·  工程排除：自动跳过 node_modules 之类目录",
      "·  下载中转：下载完成自动进「收件箱」",
      "·  去向追踪：文件被移走改名，也能指出它现在在哪"],
     accent=AMBER, tsize=17, bsize=12.5)
rect(s, M, 5.95, CW, 0.78, fill=BLUE_L, radius=0.05)
rich(textbox(s, M + 0.4, 5.95, CW - 0.8, 0.78, anchor=MSO_ANCHOR.MIDDLE),
     [("为什么是这个顺序：", True, BLUE_D),
      ("治理能力全部建立在「索引 + 查询」之上；地基不稳就加功能，"
       "后面要返工两次。", False, INK)], size=13, first=True, after=0)

# ================================================================ 07 路线图
s = nxt(blank(prs))
header(s, "后续计划 · 路线图", "五个阶段，一页看完")
stages = [
    ("A", "搜索基建", "索引 · 查询\n实时更新", "v0.4.0", BLUE, BLUE_L),
    ("B", "过滤与排除", "种类过滤\n工程排除", "v0.4.x", BLUE, BLUE_L),
    ("C", "下载中转", "下载收件箱\n一键归档", "v0.4.x\n–v0.5.x", AMBER, AMBER_L),
    ("D", "去向追踪", "移走改名\n也能找到", "v0.5.x", AMBER, AMBER_L),
    ("E", "自动治理", "归档建议\n查重 · 多来源", "v0.5.x", GREEN, GREEN_L),
]
bw = (CW - 4 * 0.24) / 5
for i, (code, name, desc, ver, ac, al) in enumerate(stages):
    x = M + i * (bw + 0.24)
    rect(s, x, BODY_TOP + 0.35, bw, 3.5, fill=WHITE, line_color=LINE, radius=0.05)
    rect(s, x, BODY_TOP + 0.35, bw, 0.06, fill=ac)
    chip(s, x + 0.3, BODY_TOP + 0.65, 0.5, 0.5, code, ac, WHITE, 17)
    para(textbox(s, x + 0.3, BODY_TOP + 1.35, bw - 0.6, 0.4), name, size=15,
         bold=True, color=INK, first=True, after=0)
    para(textbox(s, x + 0.3, BODY_TOP + 1.88, bw - 0.6, 0.85), desc, size=11.5,
         color=MUTED, first=True, after=0, line=1.3)
    para(textbox(s, x + 0.3, BODY_TOP + 3.15, bw - 0.6, 0.5), ver, size=11,
         bold=True, color=ac, first=True, after=0, line=1.15)
    if i < 4:
        arrow(s, x + bw + 0.005, BODY_TOP + 1.95, size=13)
footnote(s, "A 阶段完成即是一个可用的完整产品；B–E 才是「文件治理」，逐层加复杂度。")

# ================================================================ 08 Phase A 步骤
s = nxt(blank(prs))
header(s, "第一步拆解", "搜索基建：7 个步骤")
rows = [
    ["步骤", "做什么", "做完的标志"],
    ["A1 ✅", "止血：修掉索引重复、写库时机、输入防抖", "已完成并通过验收"],
    ["A2–A3", "搭架构骨架 + 重建索引\n（持久化、后台扫描、差量更新）", "启动即用；顺带解决\n「只覆盖桌面」的问题"],
    ["A4", "独立查询引擎 + 性能基准", "先量出基准数据，\n再决定要不要优化"],
    ["A5–A6", "实时更新 + 冷启动优化", "新建 / 改名立刻可搜；\n启动不再卡顿"],
    ["A7", "收尾：结果归并、快捷键、删除旧实现", "旧代码删净后仍能编译运行"],
]
table(s, M, BODY_TOP, CW, rows, [1.45, 6.15, 4.233], row_h=0.72, head_h=0.46,
      fs=12.5, head_fs=12.5)

# ================================================================ 09 Phase B-E 步骤
s = nxt(blank(prs))
header(s, "第二步拆解", "文件治理：4 个阶段")
rows = [
    ["阶段", "要解决的问题", "用户会看到什么"],
    ["B  过滤与排除", "搜索结果太杂，工程目录刷屏", "「只要文档 / 只要图片」一键筛选；\nnode_modules 不再出现"],
    ["C  下载中转", "下载完的文件散落各处、找不到", "下载完成自动进「收件箱」，\n一键归档或撤销"],
    ["D  去向追踪", "文件被移走、改名后就失去线索", "直接告诉你它现在在哪个文件夹"],
    ["E  自动治理", "重复文件、来源分散、没人整理", "归档建议（可选自动）、\n重复文件查重、接入聊天软件来源"],
]
table(s, M, BODY_TOP, CW, rows, [2.2, 4.6, 5.033], row_h=0.86, head_h=0.46,
      fs=12.5, head_fs=12.5)
footnote(s, "原则：默认只给建议、不自动移动文件；所有移动都可撤销。")

# ================================================================ 10 里程碑
s = nxt(blank(prs))
header(s, "里程碑", "版本节奏")
ms = [("v0.3.0", "上一阶段收尾", "主面板 · 设置 · 热键", "待真机走查", GREEN, GREEN_L),
      ("v0.4.0", "第一步：搜索基建", "A1–A7 七个步骤", "A1 已完成", BLUE, BLUE_L),
      ("v0.4.x", "第二步：过滤与排除", "种类过滤 · 工程排除", "计划中", BLUE, BLUE_L),
      ("v0.5.x", "下载中转 · 去向追踪 · 自动治理", "C · D · E 三个阶段", "计划中", AMBER, AMBER_L),
      ("v0.6.0", "插件系统 + 发布", "开放搜索接口给插件", "计划中", MUTED, ZEBRA)]
y = BODY_TOP
for i, (ver, t, d, st, ac, al) in enumerate(ms):
    rect(s, M, y, CW, 0.92, fill=WHITE, line_color=LINE, radius=0.05)
    rect(s, M, y, 0.055, 0.92, fill=ac)
    para(textbox(s, M + 0.35, y, 1.5, 0.92, anchor=MSO_ANCHOR.MIDDLE), ver,
         size=15, bold=True, color=ac, first=True, after=0)
    para(textbox(s, M + 2.0, y + 0.16, 5.6, 0.4), t, size=14, bold=True,
         color=INK, first=True, after=0)
    para(textbox(s, M + 2.0, y + 0.55, 5.6, 0.32), d, size=11, color=MUTED,
         first=True, after=0)
    para(textbox(s, M + 8.0, y, 3.4, 0.92, anchor=MSO_ANCHOR.MIDDLE), st,
         size=12, bold=True, color=ac, first=True, align=PP_ALIGN.RIGHT, after=0)
    y += 1.0

# ================================================================ 11 下一步
s = nxt(blank(prs, NAVY), dark=True)
rect(s, 0, 0, 0.17, H, fill=BLUE)
header(s, "NEXT", "下一步", dark=True)
steps = [("现在", "合并 A1 补丁", "编码与验收已完成，等评审合并", GREEN),
         ("接着", "上一阶段真机走查 → 打版本", "确认四页交互没问题后打 v0.3.0", BLUE),
         ("然后", "进入第一步 A2–A3", "搭架构骨架并重建索引，\n顺便解决「只覆盖桌面」这个关键问题", AMBER),
         ("之后", "按路线图推进 B–E", "搜索可控 → 下载中转 → 去向追踪 → 自动治理", BLUE)]
y = 1.95
for tag, t, d, ac in steps:
    h = 1.18 if "\n" in d else 1.0
    rect(s, M, y, CW, h, fill=NAVY2, radius=0.05)
    rect(s, M, y, 0.055, h, fill=ac)
    chip(s, M + 0.35, y + (h - 0.38) / 2, 0.9, 0.38, tag, ac, WHITE, 12)
    para(textbox(s, M + 1.5, y + 0.16, 10.0, 0.42), t, size=16, bold=True,
         color=WHITE, first=True, after=0)
    para(textbox(s, M + 1.5, y + 0.62, 10.0, h - 0.72), d, size=11.5,
         color=MUTED_L, first=True, after=0, line=1.3)
    y += h + 0.16

OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "文件检索模块-汇报.pptx")
prs.save(OUT)
print("saved:", OUT, os.path.getsize(OUT), "bytes,", len(prs.slides._sldIdLst), "slides")
