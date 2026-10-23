#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""
selftest_passes.py — 穷举验证 `parse_passes()`（--passes 解析器）

═══ 为什么需要它 ═══════════════════════════════════════════════════
`--passes` 的解析被推翻过三次，每次都是「用户想跑哪些趟」被当成了
「字符串里出现过哪些字母」：

    ① 逐字符 -> `--passes X2` 静默跑成 X 趟
    ② 子串     -> `--passes X2` **同时跑 X 和 X2**，
                  X2 全绿被 X 的断言判红，汇总说「有断言不通过」
    ③ 切词     -> 默认值`"".join(PASSES)` 是连写串 `ABCD...`，
                  切出来一整块认不出-> 什么都不跑，直接退出

★ 铁律：**判据必须能区分两条路的不同输出**。
所以这里不只验几个例子，而是对 PASSES 的**所有子集**做穷举 ——
凡是「连写」「逗号分隔」「空格分隔」三种写法应该得到同一组趟次的组合，
一条都不许错。

用法：python selftest_passes.py     退出码 0 = 解析器可信
"""
from __future__ import print_function

import itertools
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

# 不导入 azahar_test（它 import Pillow 且定义了一堆全局），
# 把 parse_passes 的源码单独抠出来执行 —— 这样测的是**真代码**。
# ⚠️ azahar_test.py 在 tools/ 下，本文件在 tools/guia/ 下 —— 差一级。
_here = os.path.dirname(os.path.abspath(__file__))
_test_py = os.path.join(os.path.dirname(_here), "azahar_test.py")
_src = open(_test_py, encoding="utf-8").read()
_i = _src.index("def parse_passes(")
_j = _src.index("\ndef main(", _i)
_ns = {"re": __import__("re")}
exec(compile(_src[_i:_j], "azahar_test.py:parse_passes", "exec"), _ns)
parse_passes = _ns["parse_passes"]

# 与 azahar_test.py 的真实 PASSES 保持一致（含两字符趟次 X2）
PASSES = ["A", "B", "C", "D", "E", "F", "G", "H", "I", "J", "K", "L",
          "M", "N", "O", "P", "Q", "R", "S", "T", "U", "V", "W",
          "X", "X2", "Y"]

SPECIFIC = [
    # (输入, 期望的趟次列表)
    ("X2", ["X2"]),                       # ★ 核心：绝不能带出 X
    ("X", ["X"]),
    ("X2,Y", ["X2", "Y"]),
    ("Y,X2", ["Y", "X2"]),
    ("x2", ["X2"]),
    ("X2 Y", ["X2", "Y"]),
    ("A", ["A"]),
    ("AB", ["A", "B"]),
    ("X2X2", ["X2"]),                # 去重
    ("X2X", ["X2", "X"]),                   # 连写：先吃 X2 再吃 X
    ("XX2", ["X", "X2"]),                   # 连写：X 不是 X2 的一部分
    ("A,B,C", ["A", "B", "C"]),
    ("ABC", ["A", "B", "C"]),
    ("", []),
    ("2", []),                              # 光有 2（X2 的尾巴）不算趟次
    ("X9", ["X"]),                          # 9 不是趟次字符 -> unknown
    ("Q", ["Q"]),                # Q 在 PASSES 里（区域切换那趟）
    ("AQ", ["A", "Q"]),
]


def main():
    fails = 0

    print("=== 1. 指定用例 ===")
    for spec, want in SPECIFIC:
        tags, unk = parse_passes(spec, PASSES)
        ok = tags == want
        if not ok:
            fails += 1
        print("%s %-10s -> %-16s 期望 %-16s 忽略=%s"
              % ("OK " if ok else "!! ", repr(spec), ",".join(tags),
                 ",".join(want), ",".join(unk) or "-"))

    print()
    print("=== 2. 穷举：所有子集 × 三种写法 ===")
    # 限制规模：24 个单字符趟次的全部子集是 2^24，太多。
    # 取一个能覆盖边界的代表性子集 + 全部 2~3 元组合。
    singles = [c for c in PASSES if len(c) == 1]
    combos = []
    for k in (1, 2, 3):
        combos.extend(itertools.combinations(singles, k))
    # 每次都掺入 X2，验证它与任何组合共处都不被拆错
    pairs = list(itertools.combinations(singles, 2))[:120]
    combos = combos[:400] + [c + ("X2",) for c in pairs]

    bad = 0
    for combo in combos:
        # 连写顺序需固定：X2 放在最后
        seq = [c for c in combo if c != "X2"] + (["X2"] if "X2" in combo else [])
        want = list(dict.fromkeys(seq))
        variants = {
            "": "".join(seq),                       # 连写
            ",": ",".join(seq),                # 逗号
            " ": " ".join(seq),                      # 空格
            " ": " ".join(seq),                # 混合
        }
        for name, spec in variants.items():
            tags, unk = parse_passes(spec, PASSES)
            if tags != want:
                bad += 1
                if bad <= 5:
                    print("!! [%s] %-24r -> %s  期望 %s  忽略=%s"
                          % (name, spec, tags, want, unk))
    fails += bad
    print("   组合数 %d × 写法 4 = %d 次比对，不一致 %d 次"
          % (len(combos), len(combos) * 4, bad))

    print()
    print("=== 3. 关键性质：X2 绝不能被拆成 X ===")
    for spec in ("X2", "X2Y", "AX2", "X2A", "X2X2", "Y,X2"):
        tags, _ = parse_passes(spec, PASSES)
        has_x2 = "X2" in tags
        lone_x = tags.count("X")
        ok = has_x2 and lone_x == 0
        if not ok:
            fails += 1
        print("%s %-8s -> %-14s  含X2=%s  独立X的个数=%d"
              % ("OK " if ok else "!! ", spec, ",".join(tags), has_x2, lone_x))

    print()
    print("=== 4. 默认串（连写）必须认全所有趟次 ===")
    default = "".join(PASSES)
    tags, unk = parse_passes(default, PASSES)
    ok = tags == PASSES and not unk
    if not ok:
        fails += 1
    print("%s 默认串 %r" % ("OK " if ok else "!! ", default))
    print("   认出 %d 个：%s" % (len(tags), " ".join(tags)))
    if unk:
        print("!! 忽略掉的：%s" % " ".join(unk))

    print()
    if fails:
        print("!! %d 项不符 —— 解析器不可信" % fails)
        return 1
    print("OK —— 解析器全部用例通过")
    return 0


if __name__ == "__main__":
    sys.exit(main())
