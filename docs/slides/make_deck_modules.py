# -*- coding: utf-8 -*-
"""Assistant 文件搜索模块 —— 【模块划分与连接】PPT 生成脚本（13 页）

视角：**现有代码**（v0.3.0 + A1 落地后）的模块切分，以及这些模块之间
「实际存在」的连接方式 —— 不是目标架构，目标是最后一页的对照。

所有事实均来自源码与运行日志，关键引用写进页脚。
用法： python make_deck_modules.py
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

# 三种连接方式的配色（贯穿全程的视觉编码）
C_SIG    = GREEN   # 信号 / 槽
C_INJECT = BLUE    # setter 注入
C_STATIC = AMBER   # 编译期静态调用

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


def node(slide, x, y, w, h, title, sub, accent, fill=WHITE):
    """一个有左色条的小方框（连线图用）。"""
    rect(slide, x, y, w, h, fill=fill, line_color=LINE, radius=0.08)
    rect(slide, x, y + 0.10, 0.05, h - 0.20, fill=accent)
    para(textbox(slide, x + 0.20, y + 0.09, w - 0.34, 0.28), title, size=12,
         bold=True, color=INK, first=True, after=0)
    if sub:
        para(textbox(slide, x + 0.20, y + 0.36, w - 0.34, h - 0.44), sub, size=10,
             color=MUTED, first=True, after=0, line=1.12)


prs = Presentation()
prs.slide_width, prs.slide_height = Inches(W), Inches(H)
PG = [0]
def nxt(s, dark=False):
    PG[0] += 1; pageno(s, PG[0], dark); return s


# ================================================================ 01 封面
s = blank(prs, NAVY)
rect(s, 0, 0, 0.17, H, fill=BLUE)
para(textbox(s, 1.05, 2.25, 7.6, 0.36), "ASSISTANT · 架构说明", size=14, bold=True,
     color=BLUE, first=True, after=0)
para(textbox(s, 1.05, 2.75, 8.6, 1.0), "文件搜索：模块划分与连接", size=40, bold=True,
     color=WHITE, first=True, after=0)
para(textbox(s, 1.05, 3.98, 8.2, 0.5), "现有代码里有哪些模块 · 它们靠什么连起来 · 哪里没连上",
     size=16, color=RGBColor(0xB9, 0xC6, 0xD8), first=True, after=0)
rect(s, 1.05, 4.78, 3.0, 0.03, fill=RGBColor(0x35, 0x4A, 0x66))
para(textbox(s, 1.05, 5.08, 7.6, 0.4), "基于 v0.3.0 + A1 落地后的代码现状", size=13,
     color=MUTED_L, first=True, after=0)

rect(s, 9.35, 2.05, 3.15, 3.3, fill=NAVY2, radius=0.05)
para(textbox(s, 9.72, 2.38, 2.5, 0.32), "一页结论", size=12.5, bold=True,
     color=BLUE, first=True, after=0)
tf = textbox(s, 9.72, 2.82, 2.45, 2.46)
for i, t in enumerate(["模块切分是清楚的：七层、职责单一",
                       "连接是不完整的：一半注入、一半静态直调",
                       "缺口集中在「编排层 → 采集层」这一跳",
                       "这正是「能跑，但改不动、测不了」的根源"]):
    para(tf, "·  " + t, size=11, color=RGBColor(0xC7, 0xD4, 0xE6), first=(i == 0),
         after=11, line=1.3)

# ================================================================ 02 一页总览
s = nxt(blank(prs))
header(s, "OVERVIEW", "一页看懂")

blocks = [
    ("模块划分", "七层，职责单一",
     ["触发 · 界面 · 编排 · 匹配",
      "采集 · 持久化 · 基础",
      "每一层都有明确的输入输出",
      "没有循环依赖"], GREEN, GREEN_L),
    ("连接方式", "三种并存",
     ["信号 / 槽：跨窗口与触发",
      "setter 注入：数据库类依赖",
      "编译期静态调用：算法与采集",
      "第三种正是问题所在"], BLUE, BLUE_L),
    ("连接缺口", "两处没接上",
     ["历史库只写不读，从未反馈到排序",
      "采集层无注入点，实现不可替换",
      "全局静态索引无法 mock",
      "直接决定 A2 的改造范围"], RED, RED_L),
]
for i, (tag, t, lines, ac, al) in enumerate(blocks):
    x = M + i * (CW / 3)
    w = CW / 3 - 0.25
    rect(s, x, BODY_TOP, w, 3.15, fill=WHITE, line_color=LINE, radius=0.05)
    rect(s, x, BODY_TOP, w, 0.06, fill=ac)
    chip(s, x + 0.32, BODY_TOP + 0.32, 1.2, 0.36, tag, al, ac, 12)
    para(textbox(s, x + 0.32, BODY_TOP + 0.92, w - 0.64, 0.45), t, size=16.5,
         bold=True, color=INK, first=True, after=0, line=1.2)
    tf = textbox(s, x + 0.32, BODY_TOP + 1.55, w - 0.64, 1.45)
    for j, ln in enumerate(lines):
        para(tf, "·  " + ln, size=11.5, color=MUTED, first=(j == 0), after=8, line=1.3)

rect(s, M, 5.30, CW, 1.15, fill=BLUE_L, radius=0.05)
rich(textbox(s, M + 0.4, 5.30, CW - 0.8, 1.15, anchor=MSO_ANCHOR.MIDDLE),
     [("一句话结论：", True, BLUE_D),
      ("搜索链路的每一步都能单独说清楚，模块边界也不含糊；但只要「谁拥有谁、"
       "谁能替换谁」没定下来，加功能就会变成改 AppBridge —— 所以下一步的重点"
       "不是加模块，而是把连接方式统一成可替换的那一种。",
       False, INK)], size=13.5, first=True, after=0, line=1.35)

# ================================================================ 03 模块划分总表
s = nxt(blank(prs))
header(s, "模块划分", "七层模块：按职责切，不按功能切")
rows = [
    ["层", "模块", "所在文件", "职责"],
    ["① 触发层", "HotkeyManager · TrayManager",
     "core/HotkeyManager · ui/TrayManager", "全局热键注册；托盘图标与菜单"],
    ["② 界面层", "MainWindow · MainPanel · Style",
     "resources/qml/*.qml", "搜索浮窗、主面板；主题单例"],
    ["③ 编排层", "AppBridge · PanelBridge",
     "ui/AppBridge · ui/PanelBridge", "搜索编排与执行分发；面板 CRUD"],
    ["④ 匹配层", "FuzzyMatcher · Calculator",
     "core/FuzzyMatcher · core/Calculator", "五级模糊打分；算式求值"],
    ["⑤ 采集层", "AppIndexer · FileSearcher",
     "core/AppIndexer · core/FileSearcher", "Start Menu 应用；文件系统遍历"],
    ["⑥ 持久化层", "HistoryDatabase · Todo/Notes/Clipboard",
     "database/*.cpp（4 个独立连接）", "搜索历史；三个功能各自的库"],
    ["⑦ 基础层", "Logger · ConfigManager",
     "core/Logger · core/ConfigManager", "双 sink 日志；JSON 配置与持久化"],
]
table(s, M, BODY_TOP, CW, rows, [1.35, 3.28, 3.55, 3.653],
      row_h=0.545, head_h=0.42, fs=11.5, head_fs=12.5)
footnote(s, "分层依据是「谁会调谁」，不是功能分类：④⑤两层都是无状态算法/采集，可被③层任意调用。")

# ================================================================ 04 采集层
s = nxt(blank(prs))
header(s, "模块划分 · 采集层", "两个采集器：同一个位置，完全不同的做法")
rows = [
    ["维度", "AppIndexer（应用）", "FileSearcher（文件）"],
    ["采集对象", "用户 + 全局 Start Menu 的 .lnk", "6 个标准用户目录下的文件"],
    ["实现手段", "IShellLink COM 解析快捷方式目标", "自控前序 DFS（walkDir 递归）"],
    ["接口形态", "scan() → QList<Entry>", "命名空间 + 文件级静态 s_index"],
    ["调用时机", "main.cpp:68 启动时一次", "main.cpp:72 启动时一次"],
    ["规模上限", "无上限（实际 31 条）", "硬上限 20000 条"],
    ["失败处理", "解析不出目标就静默跳过", "目录级剪枝；不跟随 junction"],
    ["实测开销", "133 ms", "888 ms（触顶截断）"],
]
table(s, M, BODY_TOP, CW, rows, [1.75, 5.0, 5.083],
      row_h=0.505, head_h=0.44, fs=11.5, head_fs=12.5)
footnote(s, "同为「采集层」，一个返回集合、一个持有全局状态 —— 这个差异决定了它们后面能不能被替换。")

# ================================================================ 05 匹配层
s = nxt(blank(prs))
header(s, "模块划分 · 匹配层", "一个共享叶子，两个互不相识的调用方")

para(textbox(s, M, BODY_TOP - 0.06, CW, 0.34),
     "FuzzyMatcher 的五级打分（分数即优先级）", size=13.5, bold=True,
     color=INK, first=True, after=0)
tiers = [("1000", "精确", GREEN, GREEN_L), ("900", "前缀", GREEN, GREEN_L),
         ("800", "词边界", BLUE, BLUE_L), ("600-", "包含·位置加权", BLUE, BLUE_L),
         ("400-", "编辑距离", AMBER, AMBER_L)]
bw = (CW - 4 * 0.18) / 5
for i, (v, lab, ac, al) in enumerate(tiers):
    x = M + i * (bw + 0.18)
    rect(s, x, BODY_TOP + 0.38, bw, 1.0, fill=WHITE, line_color=LINE, radius=0.07)
    rect(s, x, BODY_TOP + 0.38, bw, 0.05, fill=ac)
    para(textbox(s, x, BODY_TOP + 0.52, bw, 0.42), v, size=19, bold=True,
         color=ac, first=True, align=PP_ALIGN.CENTER, after=0)
    para(textbox(s, x, BODY_TOP + 0.99, bw, 0.3), lab, size=11.5, color=MUTED,
         first=True, align=PP_ALIGN.CENTER, after=0)

para(textbox(s, M, 3.62, CW, 0.34),
     "关键连接事实：它是被两条链路共用的叶子依赖", size=13.5, bold=True,
     color=RED, first=True, after=0)
rect(s, M, 4.02, CW, 1.55, fill=WHITE, line_color=LINE, radius=0.05)
rect(s, M, 4.02, 0.055, 1.55, fill=RED)
tf = textbox(s, M + 0.42, 4.22, CW - 0.85, 1.2)
rich(tf, [("调用方 A  ", True, BLUE_D),
          ("AppBridge.cpp:92  →  FuzzyMatcher::score(query, item.title)",
           False, INK),
          ("　（打分对象：内置命令与应用名）", False, MUTED)],
     size=12.5, first=True, after=9)
rich(tf, [("调用方 B  ", True, BLUE_D),
          ("FileSearcher.cpp:148  →  FuzzyMatcher::score(query, e.name)",
           False, INK),
          ("　（打分对象：文件名）", False, MUTED)],
     size=12.5, after=9)
rich(tf, [("影响  ", True, RED),
          ("改写打分逻辑会同时改变两条链路的结果；计划中的「小写形式预计算」"
           "必须两边一起改，否则排序出现分叉。", False, INK)],
     size=12.5, after=0)
footnote(s, "Calculator 也挂在同一层：AppBridge.cpp:143，门禁为「含数字 + 含运算符 + 长度≥3」。")

# ================================================================ 06 编排层 search
s = nxt(blank(prs))
header(s, "模块划分 · 编排层", "AppBridge::search：四步，一个排序")

steps = [
    ("01", "空查询短路", ["query 为空 → 直接返回 10 条内置命令",
                          "此时不会触碰文件搜索",
                          "保证空列表不会爆炸"], BLUE),
    ("02", "内置与应用打分", ["遍历 m_allItems（内置 10 + 应用 31）",
                              "逐条 FuzzyMatcher::score",
                              "保留 score > 0 的命中"], BLUE),
    ("03", "追加文件结果", ["FileSearcher::search(query, 20)",
                            "文件不进入 m_allItems，只在搜索时追加",
                            "priority=1，同分时排在后面"], GREEN),
    ("04", "整体排序 + 计算器", ["拼成一个大 list 后 std::sort",
                                 "score 降序 → priority 降序",
                                 "命中算式则 prepend score=1001"], AMBER),
]
for i, (num, t, lines, ac) in enumerate(steps):
    x = M + (i % 2) * (CW / 2 + 0.12)
    y = BODY_TOP + (i // 2) * 2.12
    card(s, x, y, CW / 2 - 0.12, 1.92, t, lines, accent=ac, tsize=15,
         bsize=11.5, num=num)

rect(s, M, 6.10, CW, 0.72, fill=AMBER_L, radius=0.05)
rich(textbox(s, M + 0.4, 6.10, CW - 0.8, 0.72, anchor=MSO_ANCHOR.MIDDLE),
     [("副作用：", True, AMBER),
      ("只记 m_lastQuery；A1 之后不再在 search() 里写库，",
       False, INK),
      ("真正的历史写入挪到了 executeCommand()", True, INK),
      ("。四类结果拼成一个大列表再全量排序，是 A7 要换成多路归并的地方。",
       False, INK)], size=12, first=True, after=0, line=1.3)

# ================================================================ 07 编排层 execute
s = nxt(blank(prs))
header(s, "模块划分 · 编排层", "executeCommand：按结果类型四分派")
rows = [
    ["分支", "判据", "动作", "写入的历史"],
    ["计算器", "type == Calculator", "结果复制到剪贴板（去掉 \"= \" 前缀）", "search_history"],
    ["应用", "id 以 app:// 开头", "openUrl，失败再退 QProcess::startDetached", "+ app_launch_history"],
    ["文件", "type == File", "openUrl，交给系统默认程序打开", "search_history"],
    ["内置命令", "其余", "executeAction 按 id 字符串逐个比对分发", "+ command_history"],
]
table(s, M, BODY_TOP, CW, rows, [1.55, 2.55, 5.2, 2.533],
      row_h=0.66, head_h=0.44, fs=11.5, head_fs=12.5)
rect(s, M, 5.02, CW, 1.05, fill=BLUE_L, radius=0.05)
rich(textbox(s, M + 0.42, 5.02, CW - 0.85, 1.05, anchor=MSO_ANCHOR.MIDDLE),
     [("连接要点：", True, BLUE_D),
      ("执行链路是 search 的镜像 —— 也是四条分支、也由 AppBridge 独占。"
       "AppBridge 同时承担「编排 + 打分 + 分发 + 文案」四种职责，"
       "是当前最接近上帝对象的模块（347 行，C++ 模块中最大）。", False, INK)],
     size=12.5, first=True, after=0, line=1.35)

# ================================================================ 08 连接① 组合根
s = nxt(blank(prs))
header(s, "连接 ①", "组合根：所有接线集中在 main.cpp")

BX1, BW1 = 0.75, 2.78
BX2, BW2 = 3.93, 2.62
BX3, BW3 = 6.95, 5.633

# 三条通道底色
rect(s, BX1, 1.92, BW1, 3.95, fill=RGBColor(0xF2, 0xF6, 0xFB), radius=0.04)
rect(s, BX2, 1.92, BW2, 3.95, fill=RGBColor(0xF2, 0xF6, 0xFB), radius=0.04)
rect(s, BX3, 1.92, BW3, 3.95, fill=RGBColor(0xF2, 0xF6, 0xFB), radius=0.04)
for bx, bw, t in ((BX1, BW1, "触发与界面"), (BX2, BW2, "编排层"),
                  (BX3, BW3, "叶子模块 / 数据")):
    para(textbox(s, bx, 2.02, bw, 0.3), t, size=12, bold=True, color=MUTED,
         first=True, align=PP_ALIGN.CENTER, after=0)

node(s, BX1 + 0.22, 2.40, BW1 - 0.44, 0.72, "HotkeyManager",
     "Alt+Space 全局热键", C_SIG)
node(s, BX1 + 0.22, 3.22, BW1 - 0.44, 0.72, "TrayManager",
     "托盘图标 + 右键菜单", C_SIG)
node(s, BX1 + 0.22, 4.04, BW1 - 0.44, 0.72, "MainWindow.qml",
     "搜索浮窗（绑定 backend）", C_INJECT)
node(s, BX1 + 0.22, 4.86, BW1 - 0.44, 0.72, "MainPanel.qml",
     "主面板（绑定 panel）", C_INJECT)

node(s, BX2 + 0.22, 2.95, BW2 - 0.44, 1.05, "AppBridge",
     "search / executeCommand\ntoggleWindow / openMainPanel", C_INJECT)
node(s, BX2 + 0.22, 4.30, BW2 - 0.44, 1.05, "PanelBridge",
     "待办 / 笔记 / 剪贴板 CRUD\napplyHotkey", C_INJECT)

node(s, BX3 + 0.24, 2.38, BW3 - 0.48, 0.66, "FuzzyMatcher  ·  静态调用",
     "AppBridge.cpp:92 打分", C_STATIC)
node(s, BX3 + 0.24, 3.10, BW3 - 0.48, 0.66, "Calculator  ·  静态调用",
     "AppBridge.cpp:143 算式门禁", C_STATIC)
node(s, BX3 + 0.24, 3.82, BW3 - 0.48, 0.66, "FileSearcher  ·  静态调用 + 全局态",
     "main.cpp:72 建索引 / AppBridge.cpp:105 查询", C_STATIC)
node(s, BX3 + 0.24, 4.54, BW3 - 0.48, 0.66, "HistoryDatabase  ·  setter 注入",
     "AppBridge::setHistoryDatabase", C_INJECT)
node(s, BX3 + 0.24, 5.26, BW3 - 0.48, 0.60, "AppIndexer  ·  静态调用（仅 main）",
     "main.cpp:68 → backend.addApp", C_STATIC)

for y in (3.10, 4.50):
    arrow(s, BX1 + BW1 + 0.03, y, color=C_SIG, size=12)
for y in (2.85, 3.55, 4.25, 4.95):
    arrow(s, BX2 + BW2 + 0.03, y, color=MUTED, size=12)

# 图例
lg = [("信号 / 槽", C_SIG), ("setter 注入", C_INJECT), ("编译期静态调用", C_STATIC)]
lx = M
for t, c in lg:
    rect(s, lx, 6.06, 0.26, 0.26, fill=c, radius=0.2)
    para(textbox(s, lx + 0.34, 6.06, 1.9, 0.26, anchor=MSO_ANCHOR.MIDDLE), t,
         size=11, color=MUTED, first=True, after=0)
    lx += 2.35
para(textbox(s, M, 6.45, CW, 0.3),
     "另有 2 个 lambda 连接：visibleChanged → 存窗口坐标；mainPanelRequested → 显示面板并隐藏浮窗。",
     size=10.5, color=MUTED, first=True, after=0)
footnote(s, "组合根的存在本身是好事：依赖关系集中可见。问题在于第三步「静态调用」绕过了它。")

# ================================================================ 09 连接② 两个桥
s = nxt(blank(prs))
header(s, "连接 ②", "两个窗口、两个引擎、两个桥")
rows = [
    ["", "搜索浮窗", "主面板"],
    ["入口 QML", "qrc:/qml/MainWindow.qml", "qrc:/qml/MainPanel.qml"],
    ["加载者", "main.cpp:87  engine", "main.cpp:140  panelEngine"],
    ["注入的桥", "backend = AppBridge", "panel = PanelBridge"],
    ["共同注入", "config · appVersion", "config · appVersion"],
    ["初始可见性", "绑定 backend.visible（初值 false）", "visible: false，由 🏠 / 托盘触发"],
    ["共享主题", "Style 单例（读 config.theme）", "Style 单例（读 config.theme）"],
]
table(s, M, BODY_TOP, CW, rows, [2.05, 4.9, 4.883],
      row_h=0.50, head_h=0.44, fs=11.5, head_fs=12.5)
rect(s, M, 5.40, CW, 1.15, fill=AMBER_L, radius=0.05)
rich(textbox(s, M + 0.42, 5.40, CW - 0.85, 1.15, anchor=MSO_ANCHOR.MIDDLE),
     [("隐含前提：", True, AMBER),
      ("两个 engine 各自独立，但 Style 单例会在每个 engine 里各实例化一份，"
       "而它的 isDark 依赖 config.theme —— 也就是说「每个 engine 都必须注入 config」"
       "是个未写进文档的硬约束，漏掉一个不会报错，只会静默取不到主题。",
       False, INK)], size=12.5, first=True, after=0, line=1.35)
footnote(s, "两套桥换来的是互不干扰（搜索浮窗不受面板影响）；代价是主题单例与缓存各一份。")

# ================================================================ 10 连接③ 注入现状
s = nxt(blank(prs))
header(s, "连接 ③", "依赖注入只做了一半")
rows = [
    ["依赖关系", "连接方式", "可替换 / 可 mock"],
    ["AppBridge → HistoryDatabase", "setter 注入（setHistoryDatabase）", "✅  可以"],
    ["PanelBridge → Todo / Notes / Clipboard", "setter 注入（三个 setter）", "✅  可以"],
    ["PanelBridge → Config / HotkeyManager", "setter 注入", "✅  可以"],
    ["AppBridge → FileSearcher", "编译期静态调用 + 全局静态索引", "❌  不行"],
    ["AppBridge → FuzzyMatcher / Calculator", "编译期静态调用（namespace）", "❌  不行"],
    ["main → AppIndexer", "编译期静态调用", "❌  不行"],
]
table(s, M, BODY_TOP, CW, rows,
      [4.35, 4.6, 2.883], row_h=0.50, head_h=0.44, fs=11.5, head_fs=12.5,
      colors=[(GREEN, GREEN, GREEN), (GREEN, GREEN, GREEN), (GREEN, GREEN, GREEN),
              (RED, RED, RED), (RED, RED, RED), (RED, RED, RED)])
rect(s, M, 5.38, CW, 1.20, fill=BLUE_L, radius=0.05)
rich(textbox(s, M + 0.42, 5.38, CW - 0.85, 1.20, anchor=MSO_ANCHOR.MIDDLE),
     [("这个分裂带来的直接后果：", True, BLUE_D),
      ("数据库类可以随时换实现（测试时给个假的就行），而算法与采集类"
       "一旦被调用就写死 —— 所以 A2 想加「legacy / v2」回退开关时，"
       "会发现 FileSearcher 根本没有挂载点，必须先立一个门面（FileIndex）出来。",
       False, INK)], size=12.5, first=True, after=0, line=1.35)

# ================================================================ 11 连接缺口
s = nxt(blank(prs))
header(s, "连接缺口", "两处没接上，一处接错了位置")

gaps = [
    ("缺口 ①", "历史库只写不读", [
        "HistoryDatabase 有 3 个读取接口：",
        "recentSearches / frequentApps / launchCount",
        "——全仓库零调用方（已实测）。",
        "databaseError 信号发出但无人 connect。",
        "→ 历史在积累，却从未反馈到排序。"], RED),
    ("缺口 ②", "采集层没有注入点", [
        "FileSearcher 是 namespace + 文件级",
        "静态 s_index：无实例、无构造、无接口。",
        "AppBridge 只能直接写 FileSearcher::search()。",
        "→ 换实现要改 AppBridge，回退开关没处挂。"], RED),
    ("缺口 ③", "采集阻塞在窗口之前", [
        "main.cpp:72 buildIndex() 同步执行，",
        "engine.load() 在 87 行 —— 阻塞在加载之前。",
        "实测 888 ms（占启动到热键就绪的 49%）。",
        "热键 123 行才注册，app.exec() 在 158 行。",
        "→ 启动后约 1.8 s 内按 Alt+Space 无反应。"], AMBER),
]
for i, (tag, t, lines, ac) in enumerate(gaps):
    x = M + i * (CW / 3)
    w = CW / 3 - 0.25
    rect(s, x, BODY_TOP, w, 4.15, fill=WHITE, line_color=LINE, radius=0.05)
    rect(s, x, BODY_TOP, w, 0.06, fill=ac)
    chip(s, x + 0.3, BODY_TOP + 0.28, 0.95, 0.34, tag, WHITE, ac, 11.5)
    para(textbox(s, x + 0.3, BODY_TOP + 0.76, w - 0.6, 0.4), t, size=15,
         bold=True, color=INK, first=True, after=0)
    tf = textbox(s, x + 0.3, BODY_TOP + 1.32, w - 0.6, 2.6)
    for j, ln in enumerate(lines):
        c = ac if ln.startswith("→") else MUTED
        para(tf, ln, size=11, color=c, first=(j == 0), after=7, line=1.28)
footnote(s, "①②属于架构债，③是它俩的运行时表现：采集层既不可替换，又被放在了错误的时机。")

# ================================================================ 12 现状 → A2
s = nxt(blank(prs))
header(s, "连接的重构方向", "现状 → A2：把「静态调用」换成「注入」")
rows = [
    ["方面", "现状（v0.3.0 + A1）", "A2 目标"],
    ["采集层形态", "namespace + 文件级静态 s_index",
     "FileIndex 门面 + FileScanner 后台线程"],
    ["索引发布", "无机制，建完即冻结",
     "SnapshotManager 以 COW 方式发布快照"],
    ["查询路径", "每次按键线性扫全量索引（2 万条）",
     "QueryEngine 独立成层，查只读快照"],
    ["编排层依赖", "AppBridge 直接写 FileSearcher::search()",
     "注入 FileIndex*，与 setHistoryDatabase 同风格"],
    ["回退能力", "无（旧实现是唯一实现）",
     "fileIndex.engine = legacy | v2，一行配置回退"],
]
table(s, M, BODY_TOP, CW, rows, [1.85, 5.0, 4.983],
      row_h=0.62, head_h=0.44, fs=11.5, head_fs=12.5)
rect(s, M, 5.48, CW, 1.10, fill=GREEN_L, radius=0.05)
rich(textbox(s, M + 0.42, 5.48, CW - 0.85, 1.10, anchor=MSO_ANCHOR.MIDDLE),
     [("判据：", True, GREEN),
      ("改造是否成功，不看新增了多少文件，而看 AppBridge 能不能在不改一行代码的前提下"
       "换掉文件搜索实现 —— 能换，连接方式才算真的统一了。", False, INK)],
     size=12.5, first=True, after=0, line=1.35)

# ================================================================ 13 小结
s = nxt(blank(prs, NAVY), dark=True)
rect(s, 0, 0, 0.17, H, fill=BLUE)
header(s, "SUMMARY", "小结", dark=True)
pts = [("划分", "七层，边界清楚", "触发 / 界面 / 编排 / 匹配 / 采集 / 持久化 / 基础", GREEN),
       ("连接", "三种方式并存", "信号槽 ✅ · setter 注入 ✅ · 编译期静态调用 ⚠", BLUE),
       ("缺口", "编排层 → 采集层这一跳", "无注入点 + 无实例 + 全局静态索引，占启动阻塞的 49%", RED),
       ("方向", "统一成「可替换」那一种", "A2 立 FileIndex 门面，legacy / v2 一行配置回退", AMBER)]
y = 1.95
for tag, t, d, ac in pts:
    rect(s, M, y, CW, 1.0, fill=NAVY2, radius=0.05)
    rect(s, M, y, 0.055, 1.0, fill=ac)
    chip(s, M + 0.35, y + 0.31, 0.9, 0.38, tag, ac, WHITE, 12)
    para(textbox(s, M + 1.5, y + 0.15, 10.0, 0.4), t, size=15.5, bold=True,
         color=WHITE, first=True, after=0)
    para(textbox(s, M + 1.5, y + 0.58, 10.0, 0.32), d, size=11.5,
         color=MUTED_L, first=True, after=0)
    y += 1.14

OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                   "文件搜索-模块划分与连接.pptx")
prs.save(OUT)
print("saved:", OUT)
print("size :", os.path.getsize(OUT), "bytes")
print("pages:", len(prs.slides._sldIdLst))
