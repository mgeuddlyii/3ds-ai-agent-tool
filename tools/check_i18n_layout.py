#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
check_i18n_layout.py —— 英文版面体检：哪些英文串会被右边缘裁掉

── 为什么需要它 ──
中文是**全角**（advance 16px），英文是**半角**（约 8px）。所以"英文比中文长"
在**字符数**上成立，在**像素**上不一定 —— 英文要宽过中文，字符数得超过中文的
约 2 倍。凭感觉改文案会改错地方：有些句子字符数翻倍了但像素反而更窄。

真正的判据只有一条：**这条英文串画出来有多宽，容器有多宽。**

── 只查"单行绘制" ──
`w_wrap` 会折行，长文案不裁；被裁的只有**单行**绘制的：
    w_line / w_button / w_top_header / w_bot_header / w_bot_footer /
    w_top_footer / w_kv / w_bullet ...
所以只扫这些调用点里的 S(Sxxxx)，才是有意义的体检范围。
（正文类 w_wrap 不管 —— 它靠折行活着，量它只会得到一堆假警报。）

── 容器宽度怎么来 ──
从源码里**读常量**（W_TOP_USABLE / W_BOT_USABLE / 按钮 Rect 的 w 字段），
读不到就按调用名给一个保守的默认值，并**在报告里标明"用的是默认宽度"**——
猜测必须在输出里可见，否则又变成假判据。

用法：
    python tools/check_i18n_layout.py            # 报超宽的
    python tools/check_i18n_layout.py --all      # 连没超的也按宽度排序列出
    python tools/check_i18n_layout.py --lang en  # 只查英文（默认）
"""

import io
import json
import os
import re
import struct
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
UI = os.path.join(ROOT, "app", "source", "ui")
FONT = os.path.join(ROOT, "app", "data", "font.bin")
STRINGS = os.path.join(ROOT, "tools", "i18n_strings.json")
EN = os.path.join(ROOT, "tools", "i18n_en.json")

# 单行绘制：这些函数不折行，宽了就被右边缘裁掉
SINGLE_LINE = ("w_line", "w_button", "w_top_header", "w_bot_header",
               "w_bot_footer", "w_top_footer", "w_kv", "w_bullet",
               "w_tab", "w_chip", "w_badge")

# 没解析到宽度时用的保守默认（px）。★ 报告里会标出来
DEFAULT_W = {
    "w_button": 144,      # 对话屏按钮 144×2 = 300 已占满
    "w_top_header": 300,
    "w_bot_header": 300,
    "w_bot_footer": 300,
    "w_top_footer": 300,
    "w_line": 300,
    "w_kv": 300,
    "w_bullet": 280,
    "w_tab": 92,          # 主页格子
    "w_chip": 92,
    "w_badge": 92,
}


def load_font(path):
    """读 AFNT 的 advance 表：{codepoint: advance}。"""
    data = io.open(path, "rb").read()
    if data[:4] != b"AFNT":
        raise SystemExit("font.bin magic 不对：%r" % data[:4])
    ver, flags = struct.unpack_from("<HH", data, 4)
    count, idx_off = struct.unpack_from("<II", data, 12)
    adv = {}
    for i in range(count):
        cp, a, fl, _ = struct.unpack_from("<IBBH", data, idx_off + i * 8)
        adv[cp] = a
    return adv, ver, flags


def width_px(text, adv, fallback):
    """一段文本的像素宽度。缺字形按 fallback（保守：按全角算）。"""
    w = 0
    for ch in text:
        w += adv.get(ord(ch), fallback)
    return w


def const_widths():
    """从源码里把 W_*_USABLE 这类常量读出来（值是像素数就直接用）。"""
    out = {}
    pat = re.compile(r"#define\s+(W_[A-Z0-9_]+)\s+(\d+)")
    for p in (os.path.join(ROOT, "app", "source", "gfx", "widgets.h"),
              os.path.join(ROOT, "app", "source", "ui", "widgets.h"),
              os.path.join(ROOT, "shared", "widgets.h")):
        if not os.path.exists(p):
            continue
        for m in pat.finditer(io.open(p, encoding="utf-8").read()):
            out[m.group(1)] = int(m.group(2))
    return out


def calls(src):
    """找出单行绘制调用里的 S(Sxxxx)。返回 [(func, sid, 宽度表达式)]。"""
    out = []
    for fn in SINGLE_LINE:
        # fn( ... S(Sxxxx) ... )  —— 只取同一对括号内的
        for m in re.finditer(r"\b%s\s*\(" % fn, src):
            # 找配对的右括号
            i = m.end() - 1
            depth = 0
            while i < len(src):
                if src[i] == "(":
                    depth += 1
                elif src[i] == ")":
                    depth -= 1
                    if depth == 0:
                        break
                i += 1
            args = src[m.end():i]
            for sm in re.finditer(r"S\((S\d{4})\)", args):
                out.append((fn, sm.group(1), args))
    return out


def main():
    all_ = "--all" in sys.argv
    adv, ver, flags = load_font(FONT)
    consts = const_widths()
    full = adv.get(0x4E00, 16)     # 全角基准（实际逐字查表，这里只做兜底）

    with io.open(STRINGS, encoding="utf-8") as fh:
        zh_map = json.load(fh)
    with io.open(EN, encoding="utf-8") as fh:
        en_map = json.load(fh)
    by_id = {}
    for text, sid in zh_map.items():
        by_id[sid] = text

    rows = []
    guessed = set()
    for name in sorted(os.listdir(UI)):
        if not name.endswith(".c"):
            continue
        src = io.open(os.path.join(UI, name), encoding="utf-8").read()
        for (fn, sid, args) in calls(src):
            zh = by_id.get(sid)
            en = en_map.get(sid)
            if zh is None or en is None:
                continue
            w_en = width_px(en, adv, full)
            w_zh = width_px(zh, adv, full)
            cap = DEFAULT_W.get(fn, 300)
            # 试着从参数里认出 W_*_USABLE
            used_const = None
            for cname, cval in consts.items():
                if cname in args and cname.endswith("USABLE"):
                    cap = cval
                    used_const = cname
                    break
            if used_const is None:
                guessed.add(fn)
            rows.append((w_en - cap, fn, sid, w_en, w_zh, cap, en[:48], name))

    rows.sort(reverse=True)
    over = [r for r in rows if r[0] > 0]

    print("字库 : AFNT v%d flags=0x%02X · %d 字形" % (ver, flags, len(adv)))
    print("单行绘制调用点 : %d 处（同一个 S() 被多处引用会各算一次）" % len(rows))
    if guessed:
        print("⚠️ 这些函数没在参数里认出 W_*_USABLE，用的**默认宽度**（可能偏严）：%s"
              % ", ".join(sorted(guessed)))
    print("")

    if over:
        print("会超宽 %d 处（英文宽 > 容器宽）：" % len(over))
        for (d, fn, sid, w_en, w_zh, cap, txt, name) in over:
            print("   +%3dpx  %-14s %-6s %3d/%3d  %-16s %s"
                  % (d, fn, sid, w_en, cap, name, txt))
    else:
        print("✓ 没有任何单行文案超宽（英文全部放得下）")

    if all_:
        print("\n最宽的前 15 条（不论超没超）：")
        for (d, fn, sid, w_en, w_zh, cap, txt, name) in rows[:15]:
            print("   %4d/%3d  %-14s %-6s zh=%3d  %s"
                  % (w_en, cap, fn, sid, w_zh, txt))

    return 1 if over else 0


if __name__ == "__main__":
    sys.exit(main())