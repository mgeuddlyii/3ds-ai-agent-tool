#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
fix_adjacent_literals.py —— 消除 "裸字面量 + S(...)" 相邻拼接

── 背景：抽取脚本为什么留下这些 ──
源码里有些句子是 C 的相邻字面量拼接：

        w_wrap(x, y, w,
               "装完之后它会出现在主菜单里。"
               S(S0190),                 ← 被抽成了 S()
               w_c(...), TEXT_LINE_H);

拼接形式**放不下 S()**：S() 是函数调用，不是字符串字面量，
编译器报 "expected ')' before 'uistr'"。

抽取脚本当时选择了"跳过拼接整段"（保守、不会破坏语义），
所以那个前半句留在了源码里，中文版看不出问题 —— **英文版才会
变成"半句中文接半句英文"**。

★ 这个脚本怎么修 ──
把相邻的裸字面量**合并进 S() 那一整句**，并给合并后的句子一个新编号。
它不猜语义，只做"拼起来"——所以改完**必须**人看一眼译文对不对。

用法：
    python tools/fix_adjacent_literals.py            # dry-run，报有多少处
    python tools/fix_adjacent_literals.py --apply    # 真的改
"""

import io
import json
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
UI = os.path.join(ROOT, "app", "source", "ui")
STRINGS = os.path.join(ROOT, "tools", "i18n_strings.json")
EN = os.path.join(ROOT, "tools", "i18n_en.json")

CJK = re.compile(r"[\u4e00-\u9fff]")
# 一行形如:  "中文……"  或  "中文……"(可能尾随逗号)
LIT = re.compile(r'^\s*("(?:[^"\\]|\\.)*")\s*,?\s*$')
# 一行形如:  S(S0190),
SID = re.compile(r'^\s*S\((S\d+)\)\s*,?\s*$')


def main():
    apply_ = "--apply" in sys.argv

    with io.open(STRINGS, encoding="utf-8") as fh:
        zh = json.load(fh)          # ★ 形状是 {中文: 编号}，不是 {编号: 中文}
    with io.open(EN, encoding="utf-8") as fh:
        en = json.load(fh)          # {编号: 英文}
    # 反查表：按编号拿中文原文。
    # ★ 早先直接 zh.get(sid) 拿到的是 None，于是 23 句全被判成"旧号已移走"，
    # 合并结果只剩前半句 —— 而表是合法的、判据全绿，只有真机看得出缺一截。
    zh_by_id = {}
    for text, sid in zh.items():
        zh_by_id[sid] = text
    next_id = max(int(k[1:]) for k in zh_by_id) + 1

    hits = []
    for name in sorted(os.listdir(UI)):
        if not name.endswith(".c"):
            continue
        p = os.path.join(UI, name)
        lines = io.open(p, encoding="utf-8").read().split("\n")

        i = 0
        while i < len(lines):
            m_lit, m_sid = LIT.match(lines[i]), None
            if not m_lit or not CJK.search(m_lit.group(1)):
                i += 1
                continue
            # 往下找最近的 S(...) 行（允许中间是别的实参行？不行，必须紧邻）
            j = i + 1
            m_sid = SID.match(lines[j]) if j < len(lines) else None
            if not m_sid:
                i += 1
                continue
            sid = m_sid.group(1)
            head = m_lit.group(1)[1:-1]          # 去掉两侧引号
            old = zh_by_id.get(sid)
            # ★ 旧号可能已经被前面的合并"搬走"过（zh 里不再有这个号，
            # 因为它的原文已经进了某个新号）。这时不能把 head 当成完整句 ——
            # 那会让界面只显示前半句。
            # 所以：取不到旧原文时，**只删前缀行、不编新号**，让人来补这句。
            if old is None:
                hits.append((name, i + 1, sid, "(合并后旧号已移走)", head))
                if apply_:
                    del lines[i]
                else:
                    i += 1
                continue
            merged = head + old
            new_id = "S%04d" % next_id
            next_id += 1          # ★ 必须自增。曾漏了这一行 → 14 句全撞在
                                  # 同一个编号上，界面上它们会显示成同一句话，
                                  # 而编译与判据全绿（表是合法的，只是内容错了）。
                                  # 这类错只有真机能看出来。
            hits.append((name, i + 1, sid, new_id, merged))
            if apply_:
                zh[merged] = new_id
                # ★ 标 TODO 而不是沿用旧译文：合并后的句子**多了前半句**，
                # 沿用旧译文等于界面只显示后半句（英文版会缺一截）。
                en[new_id] = "TODO-EN"
                lines[j] = " " * (len(lines[j]) - len(lines[j].lstrip())) + \
                           "S(%s)," % new_id
                del lines[i]
                i = j
            else:
                i = j + 1

        if apply_ and any(h[0] == name for h in hits):
            io.open(p, "w", encoding="utf-8").write("\n".join(lines))

    for name, ln, sid, nid, txt in hits:
        print("   %-14s:%-4d %s → %s（译文需人补）  %s..." % (name, ln, sid, nid, txt[:36]))
    print("共 %d 处%s" % (len(hits), "（已改）" if apply_ else "（dry-run，加 --apply 生效）"))
    if apply_:
        json.dump(zh, io.open(STRINGS, "w", encoding="utf-8"),
                  ensure_ascii=False, indent=1, sort_keys=True)
        json.dump(en, io.open(EN, "w", encoding="utf-8"),
                  ensure_ascii=False, indent=1, sort_keys=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())