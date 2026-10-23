#!/usr/bin/env python3
# check_pages.py -- manual_pages 页面包守卫
#
# 判据（复刻自 app/source/gfx/html.c 的标签处理，2026-10-17 核对）：
#   1. 只允许 h1/h2/h3/p/div/li/ul/ol/blockquote 八种标签；
#      img/b/i/strong/em/a/style/script 一律判红（查看器不渲染）。
#   2. 每页行数（h*+p+div+li 计一行）不得超过 MAX_LINES——
#      3DS 一屏约 13 行，留 60% 余量给翻页与页眉页脚。
#   3. 必须以 <h1> 开头（每页一个标题）。
#
# 用法：python check_pages.py          # 全绿退出 0，有问题退出 1
#       python check_pages.py --list   # 逐页列出行数

import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
MAX_LINES = 20

ALLOWED = {"h1", "h2", "h3", "p", "div", "li", "ul", "ol", "blockquote"}
FORBIDDEN = {"img", "b", "i", "strong", "em", "a", "style", "script",
             "br", "hr", "table", "span", "code", "pre", "font"}

TAG_RE = re.compile(r"<\s*(/?)([a-zA-Z][a-zA-Z0-9]*)[^>]*>")
LINE_RE = re.compile(r"<\s*(h1|h2|h3|p|div|li)\b", re.IGNORECASE)


def check_file(path):
    """返回 (错误列表, 行数)"""
    errs = []
    with open(path, "r", encoding="utf-8") as f:
        text = f.read()

    tags = TAG_RE.findall(text)
    if not tags:
        errs.append("空文件或无标签")
        return errs, 0

    # 规则 1：标签白名单
    for close, name in tags:
        n = name.lower()
        if n in FORBIDDEN:
            errs.append("禁用标签 <%s>（html.c 不渲染）" % n)
        elif n not in ALLOWED:
            errs.append("白名单外标签 <%s>" % n)

    # 规则 3：<h1> 开头
    if not re.match(r"\s*<h1\b", text, re.IGNORECASE):
        errs.append("未以 <h1> 开头")

    # 规则 2：行数预算
    lines = len(LINE_RE.findall(text))
    if lines > MAX_LINES:
        errs.append("行数 %d 超过预算 %d，拆页" % (lines, MAX_LINES))

    return errs, lines


def main():
    list_mode = "--list" in sys.argv
    pages = sorted(f for f in os.listdir(HERE)
                   if f.endswith(".html"))
    if not pages:
        print("!! 没有找到任何 .html 页面")
        return 1

    bad = 0
    for name in pages:
        errs, lines = check_file(os.path.join(HERE, name))
        if list_mode:
            print("%-24s %2d 行 %s" % (name, lines,
                                        "OK" if not errs else "RED"))
        for e in errs:
            print("!! %s: %s" % (name, e))
            bad += 1

    if bad:
        print("\n%d 处违规。改页面，别放宽判据。" % bad)
        return 1
    print("manual_pages: %d 页全绿（标签白名单 + h1 开头 + 行数<=%d）"
          % (len(pages), MAX_LINES))
    return 0


if __name__ == "__main__":
    sys.exit(main())
