#!/usr/bin/env python3
# render_preview.py -- manual_pages 渲染预览器（PC 侧复刻，非真机）
#
# 复刻链（2026-10-17 逐行核对）：
#   app/source/gfx/html.c      标签 -> 行类型（H1/H2/H3/ITEM/QUOTE/TEXT）
#   app/source/ui/sc_viewer.c  各类型的缩进与宽（H1:0 / H2:4 / H3:8 /
#                               ITEM:8+12*indent 且带 "· " 前缀 / QUOTE:0）
#   app/source/gfx/text.c      first_line_bytes 折行：
#                               全角（0x2E80-0xFF60）16px，其余 8px；
#                               拉丁词优先空格处断；禁则标点不留行尾。
#   容量：上屏 usable 384px（W_TOP_USABLE），内容区 174px，行高 18px
#         => 一屏 9 行。单页超过 3 屏（27 行）判红——翻页太多。
#
# 用法：python render_preview.py [目录]           # 每页屏数总览
#       python render_preview.py [目录] --text   # 附折行后纯文本预览

import os
import re
import sys

# ---- 容量常量（widgets.h / text.c）----
SCREEN_W = 384          # W_TOP_USABLE
LINES_PER_SCREEN = 9    # 174px 内容区 / 18px 行高
MAX_SCREENS = 3         # 单页屏数上限，超过判红

WIDE_LO, WIDE_HI = 0x2E80, 0xFF60
# is_no_line_start（text.c）：不允许出现在行首的收尾标点
NO_LINE_START = set("、。，．：；！？』」〉》】）］｝…·—)]}")


def is_wide(cp):
    return WIDE_LO <= ord(cp) <= WIDE_HI


def glyph_advance(cp):
    return 16 if is_wide(cp) else 8


def first_line(s, max_w):
    """复刻 text.c first_line_bytes：返回第一行占的字符数。"""
    w = 0
    last_space = 0
    for idx, cp in enumerate(s):
        if cp == "\n":
            return idx + 1
        adv = glyph_advance(cp)
        if idx > 0 and w + adv > max_w:
            if not is_wide(cp) and 0 < last_space <= idx:
                return last_space
            if cp not in NO_LINE_START:
                return idx
            # 宽字符 + 禁则标点：允许本行溢出一点，下一轮再断
        w += adv
        if cp == " ":
            last_space = idx + 1
    return len(s)


def line_count(s, max_w):
    """复刻 text.c text_line_count。"""
    if max_w <= 0:
        return 0
    lines = 0
    i = 0
    while i < len(s):
        n = first_line(s[i:], max_w)
        if n == 0:
            break
        i += n
        lines += 1
    return lines


# ---- html.c 标签 -> 行类型 ----
BLOCK_TAGS = {"h1", "h2", "h3", "p", "div", "li", "blockquote"}
KIND_OF = {"h1": "H1", "h2": "H2", "h3": "H3",
           "p": "TEXT", "div": "TEXT", "li": "ITEM", "blockquote": "QUOTE"}


def parse_html(text):
    """返回 [(kind, indent, text)]，复刻 html.c 的流式累积。"""
    lines = []
    cur_kind, cur_indent, buf = "TEXT", 0, []
    depth = 0

    def flush():
        nonlocal buf, cur_kind, cur_indent
        s = "".join(buf).strip()
        if s:
            lines.append((cur_kind, cur_indent, s))
        buf = []

    for m in re.finditer(r"<\s*(/?)\s*([a-zA-Z][a-zA-Z0-9]*)[^>]*>|([^<]+)",
                         text):
        closing, name, chunk = m.group(1), m.group(2), m.group(3)
        if chunk is not None:
            buf.append(chunk)
            continue
        name = name.lower()
        if name in BLOCK_TAGS:
            flush()
            if not closing:
                cur_kind = KIND_OF[name]
                cur_indent = depth if name == "li" else (1 if name == "blockquote" else 0)
        elif name in ("ul", "ol"):
            flush()
            depth = depth + 1 if not closing else max(0, depth - 1)
        # 其余标签忽略（页面包白名单保证不会出现）
    flush()
    return lines


def render_lines(parsed):
    """按 sc_viewer 缩进与宽度，返回折行后的预览行列表。"""
    out = []
    for kind, indent, text in parsed:
        if kind == "H1":
            x, w, shown = 0, SCREEN_W, text
        elif kind == "H2":
            x, w, shown = 4, SCREEN_W - 4, text
        elif kind == "H3":
            x, w, shown = 8, SCREEN_W - 8, text
        elif kind == "ITEM":
            x = 8 + indent * 12
            w = SCREEN_W - x - 2
            shown = "· " + text
        else:  # QUOTE / TEXT
            x, w, shown = 0, SCREEN_W, text
        if w <= 0:
            out.append(("FATAL", kind, "宽度<=0：%s" % text[:20]))
            continue
        s = shown
        while s:
            n = first_line(s, w)
            out.append((kind, indent, s[:n]))
            s = s[n:]
    return out


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    show_text = "--text" in sys.argv
    here = os.path.abspath(args[0]) if args else os.path.dirname(os.path.abspath(__file__))
    pages = sorted(f for f in os.listdir(here) if f.endswith(".html"))
    if not pages:
        print("!! %s 下没有 .html" % here)
        return 1

    bad = 0
    for name in pages:
        with open(os.path.join(here, name), "r", encoding="utf-8") as f:
            text = f.read()
        parsed = parse_html(text)
        rendered = render_lines(parsed)
        n = len(rendered)
        screens = (n + LINES_PER_SCREEN - 1) // LINES_PER_SCREEN
        flag = ""
        if screens > MAX_SCREENS:
            flag = "  <== RED：%d 屏，拆页或精简" % screens
            bad += 1
        fatal = [r for r in rendered if r[0] == "FATAL"]
        for f_ in fatal:
            print("!! %s: %s" % (name, f_[2]))
            bad += 1
        print("%-22s %3d 行 = %d 屏%s" % (name, n, screens, flag))
        if show_text:
            for kind, indent, s in rendered:
                pad = "  " * indent
                print("    [%-5s] %s%s" % (kind, pad, s))

    if bad:
        print("\n%d 处判红。改页面，别放宽判据。" % bad)
        return 1
    print("\n%d 页全部在 %d 屏内。" % (len(pages), MAX_SCREENS))
    return 0


if __name__ == "__main__":
    sys.exit(main())
