#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
fix_concat.py —— 消除源码里所有 "中文字面量 + S(...)" 的相邻拼接

── 为什么需要它 ──
抽取脚本把界面文案换成了 S(S0123)，而 C 的**相邻字面量拼接**里放不下
S()（它是函数调用，不是字符串字面量）。编译器报的是
    expected ')' before 'uistr'
指着一个看不出所以然的地方，而真因在上面几行的字面量。

这种拼接有三种形态，本脚本都要认：
    ① 跨行：  "前半句，"\n        ② 同行：  "前半句，" S(S0123),
              S(S0123),
    ③ 三元：  ? "前半句，"
                    S(S0123)
              : S(S0456),

★ 做法：把字面量前缀**合并进 S() 那一整句**，并给它一个新编号。
  不猜语义，只做"拼起来"—— 所以改完**必须**人核一遍译文。

用法：
    python tools/fix_concat.py             # dry-run
    python tools/fix_concat.py --apply     # 真改（同时给合并句发新号）
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

CJK = re.compile(r"[一-鿿]")

# 形态①②：字面量片段后面（可隔空白/换行）紧跟 S(Sxxxx)
P12 = re.compile(r'"((?:[^"\\]|\\.)*)"(\s*)S\((S\d{4})\)')
# 形态③：三元里的 ? "..." 后跟 S(...)
P3 = re.compile(r'(\?)(\s*)"((?:[^"\\]|\\.)*)"(\s*)S\((S\d{4})\)')


class Merger(object):
    """把拼接合并成一条，并发新号。

    ★ 编号自增必须放在这里。早先漏了自增，23 处全撞在同一个号上 ——
      界面上那些句子会显示成**同一句话**，而表合法、编译通过、
      判据全绿，只有真机截图才看得出来。
    """

    def __init__(self, zh_by_id, apply_):
        self.zh_by_id = zh_by_id
        self.apply_ = apply_
        self.next_id = max(int(k[1:]) for k in zh_by_id) + 1
        self.hits = []          # (file, old, new, merged)
        self.pending = []       # (merged, new)

    def _new_id(self):
        sid = "S%04d" % self.next_id
        self.next_id += 1
        return sid

    def _merge(self, file, head, sid):
        if not CJK.search(head):
            return None
        tail = self.zh_by_id.get(sid)
        if tail is None:
            return None                # 旧号不在清单里（人已改过）→ 放过
        new = self._new_id()
        merged = head + tail
        self.hits.append((file, sid, new, merged))
        if self.apply_:
            self.pending.append((merged, new))
        return new

    def sub12(self, file, m):
        new = self._merge(file, m.group(1), m.group(3))
        return m.group(0) if new is None else "S(%s)" % new

    def sub3(self, file, m):
        new = self._merge(file, m.group(3), m.group(5))
        if new is None:
            return m.group(0)
        return "%s%sS(%s)" % (m.group(1), m.group(2), new)


def main():
    apply_ = "--apply" in sys.argv

    with io.open(STRINGS, encoding="utf-8") as fh:
        zh = json.load(fh)              # ★ {中文原文: 编号}
    with io.open(EN, encoding="utf-8") as fh:
        en = json.load(fh)              # {编号: 英文}
    zh_by_id = {}
    for text, sid in zh.items():
        zh_by_id[sid] = text

    mg = Merger(zh_by_id, apply_)

    for name in sorted(os.listdir(UI)):
        if not name.endswith(".c"):
            continue
        p = os.path.join(UI, name)
        s = io.open(p, encoding="utf-8").read()
        orig = s
        s = P3.sub(lambda m: mg.sub3(name, m), s)
        s = P12.sub(lambda m: mg.sub12(name, m), s)
        if apply_ and s != orig:
            io.open(p, "w", encoding="utf-8").write(s)

    for (name, old, new, merged) in mg.hits:
        print("   %-14s %s -> %s   %s..." % (name, old, new, merged[:40]))
    print("共 %d 处%s" % (len(mg.hits),
                        "（已改，译文需人补）" if apply_ else "（dry-run）"))

    if apply_ and mg.hits:
        for (merged, new) in mg.pending:
            zh[merged] = new
            en[new] = "TODO-EN"     # ★ 标 TODO 不沿用旧译文：合并后多了前半句
        json.dump(zh, io.open(STRINGS, "w", encoding="utf-8"),
                  ensure_ascii=False, indent=1, sort_keys=True)
        json.dump(en, io.open(EN, "w", encoding="utf-8"),
                  ensure_ascii=False, indent=1, sort_keys=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())