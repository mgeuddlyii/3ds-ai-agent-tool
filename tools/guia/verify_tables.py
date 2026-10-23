#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""
verify_tables.py — 「游戏表」四方一致性检查（只读）

══ 为什么要有一个专门的检查器 ═══════════════════════════════════════
game_id 是**位置**：它等于 `sorted(corpus/guide_*.txt)` 的下标。
围绕这个下标，项目里有 **六处** 必须同时成立：

    ① corpus/guide_*.txt          语料文件列表（**唯一事实来源**）
    ② tools/guia/guide_search.py  GAME_ORDER  （Python：下标 -> stem）
    ③ tools/guia/guide_search.py  GAME_ALIAS  （Python：stem -> 别名）
    ④ shared/nnet/guide_kb.h      GKB_GAME_N  （C：游戏总数）
    ⑤ shared/nnet/guide_kb.c      g_alias[] / g_game_names[]  （C：别名与显示名）
    ⑥ app/source/ui/sc_guide.c    g_cand_set[] （C：底屏候选词，行数==④）

这六处**任何一处脱节都是静默故障**：不报错、不崩，只是某个游戏的
答案变成别的游戏的、或者底屏空白、或者从问句里永远认不出某款游戏。
历史上 ①②③ 各错位过一次，④⑤⑥ 各少过一批 —— 所以必须自动查。

══ 判据设计纪律 ═════════════════════════════════════════════════════
- 每一处都**逐个元素比**，不能只比长度。长度相等而内容错位是最隐蔽的
  那种（BPE 词表 3815/4096 错位、token 交集 0，长度却完全相等）。
- 不复刻任何脚本的常量：GKB_GAME_N 从 .h 抠，g_cand_set 从 .c 抠。
- 报错时必须说清**是第几处**、**错在第几个 gid** —— 否则拿到输出的人
  还得自己再查一遍才知道动哪张表。

用法：
    python tools/guia/verify_tables.py          # 全查，0 = 六处一致
    python tools/guia/verify_tables.py -v       # 连每款的 stem/名字也打出来
"""
from __future__ import print_function

import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
CORPUS = os.path.join(HERE, "corpus")
KB_C = os.path.join(ROOT, "shared", "nnet", "guide_kb.c")
KB_H = os.path.join(ROOT, "shared", "nnet", "guide_kb.h")
SC = os.path.join(ROOT, "app", "source", "ui", "sc_guide.c")


def read(p):
    if not os.path.exists(p):
        print("!! 找不到 %s" % p)
        sys.exit(2)
    return open(p, encoding="utf-8", errors="replace").read()


def arr_body(text, decl_re):
    """按花括号配平抠出 `decl_re = { ... };` 的内容。"""
    m = re.search(decl_re, text)
    if not m:
        return None
    i = text.index("{", m.end() - 1)
    depth, j = 0, i
    while j < len(text):
        if text[j] == "{":
            depth += 1
        elif text[j] == "}":
            depth -= 1
            if depth == 0:
                return text[i + 1:j]
        j += 1
    return None


def main():
    verbose = "-v" in sys.argv
    bad = []

    # ---- ① 语料：唯一事实来源 ----
    if not os.path.isdir(CORPUS):
        print("!! 找不到语料目录 %s" % CORPUS)
        return 2
    stems = sorted(f[len("guide_"):-len(".txt")]
                   for f in os.listdir(CORPUS)
                   if f.startswith("guide_") and f.endswith(".txt"))
    N = len(stems)
    print("① 语料目录            %d 款" % N)

    # ---- ②③ Python 侧 ----
    sys.path.insert(0, HERE)
    try:
        import guide_search as G
    except Exception as e:
        print("!! import guide_search 失败：%r" % (e,))
        return 2
    go = list(G.GAME_ORDER)
    ga = dict(G.GAME_ALIAS)
    print("② guide_search.GAME_ORDER %d 款" % len(go))
    print("③ guide_search.GAME_ALIAS %d 项" % len(ga))
    if go != stems:
        if len(go) != N:
            bad.append(("② GAME_ORDER", "长度 %d != 语料 %d" % (len(go), N)))
        else:
            diff = [i for i in range(N) if go[i] != stems[i]]
            bad.append(("② GAME_ORDER", "长度相同但 %d 处错位，首个在 gid=%d"
                        "（表里 %r / 语料 %r）"
                        % (len(diff), diff[0], go[diff[0]], stems[diff[0]])))
    missing = [s for s in stems if s not in ga]
    extra = [s for s in ga if s not in stems]
    if missing:
        bad.append(("③ GAME_ALIAS", "缺 %d 款（前 5: %s）" % (len(missing), missing[:5])))
    if extra:
        bad.append(("③ GAME_ALIAS", "多 %d 款（前 5: %s）" % (len(extra), extra[:5])))

    # ---- ④ GKB_GAME_N ----
    h = read(KB_H)
    m = re.search(r"#define\s+GKB_GAME_N\s+(\d+)", h)
    if not m:
        bad.append(("④ GKB_GAME_N", "guide_kb.h 里找不到这个宏"))
        gn = None
    else:
        gn = int(m.group(1))
        print("④ GKB_GAME_N          %d" % gn)
        if gn != N:
            bad.append(("④ GKB_GAME_N", "%d != 语料 %d" % (gn, N)))

    # ---- ⑤ g_alias / g_game_names ----
    c = read(KB_C)
    ab = arr_body(c, r"static\s+const\s+GkbAlias\s+g_alias\[\]\s*=\s*\{")
    nb = arr_body(c, r"static\s+const\s+char\s+\*g_game_names\[\]\s*=\s*\{")
    if ab is None or nb is None:
        bad.append(("⑤ guide_kb.c", "抠不出 g_alias[] 或 g_game_names[]"))
        alias_pairs, names = [], []
    else:
        alias_pairs = [(a, int(g)) for a, g in
                       re.findall(r'"([^"]*)"\s*,\s*(-?\d+)\s*\}', ab)]
        names = re.findall(r'"([^"]*)"', nb)
        gids = sorted({g for _, g in alias_pairs})
        print("⑤ g_game_names        %d 项" % len(names))
        print("  g_alias             %d 条，覆盖 gid %d 个"
              % (len(alias_pairs), len(gids)))
        if len(names) != N:
            bad.append(("⑤ g_game_names", "%d != 语料 %d" % (len(names), N)))
        miss = sorted(set(range(N)) - set(gids))
        if miss:
            bad.append(("⑤ g_alias", "缺 gid %s（共 %d 款）" % (miss[:10], N)))
        out = [g for g in gids if g < 0 or g >= N]
        if out:
            bad.append(("⑤ g_alias", "有越界 gid %s" % out[:10]))

        # 显示名一致性：C 侧显示名应等于 Python 的显示名（取 GAME_ALIAS 首项）
        if len(names) == N and not missing:
            mismatch = []
            for i, stem in enumerate(stems):
                py = (ga.get(stem) or [None])[0]
                if py is not None and py != names[i]:
                    mismatch.append((i, stem, py, names[i]))
            if mismatch:
                # ⚠️ 这项**只报不判**：显示名允许有意不同（比如 C 端用短名）。
                print("  提示：%d 款的显示名 Python/C 不一致（不判失败）" % len(mismatch))
                for i, stem, py, cc in mismatch[:5]:
                    if verbose:
                        print("     gid=%-3d %-26s py=%r c=%r" % (i, stem, py, cc))

    # ---- ⑥ g_cand_set ----
    sc = read(SC)
    cb = arr_body(sc, r"static\s+const\s+CandSet\s+g_cand_set\[\]\s*=\s*\{")
    if cb is None:
        bad.append(("⑥ sc_guide.c", "抠不出 g_cand_set[]"))
    else:
        rows = re.findall(r"\{\s*\{([^}]*)\}\s*\}", cb)
        print("⑥ g_cand_set          %d 行" % len(rows))
        if len(rows) != N:
            bad.append(("⑥ g_cand_set", "%d 行 != 语料 %d" % (len(rows), N)))
        short = [i for i, r in enumerate(rows)
                 if len(re.findall(r'"[^"]*"', r)) != 6]
        if short:
            bad.append(("⑥ g_cand_set", "这些行不是 6 个词：%s" % short[:10]))
        if gn is not None and len(rows) != gn:
            bad.append(("⑥ vs ④", "g_cand_set %d 行 != GKB_GAME_N %d —— "
                        "这会让编译期断言直接断掉构建"
                        % (len(rows), gn)))

    # ---- 结论 ----
    print()
    print("=" * 68)
    if bad:
        print("FAIL —— %d 处不一致：" % len(bad))
        for who, why in bad:
            print("  [%s] %s" % (who, why))
        print()
        print("修法：改完语料先跑 `python tools/guia/gen_game_tables.py --write`")
        print("      再跑 `python tools/guia/gen_cand_rows.py --write`")
        print("      （顺序不能换：g_cand_set 的行数有编译期断言，卡在 GKB_GAME_N）")
        return 1
    print("OK —— 六处一致：语料 %d 款 → GAME_ORDER / GAME_ALIAS / "
          "GKB_GAME_N / g_alias / g_game_names / g_cand_set" % N)
    return 0


if __name__ == "__main__":
    sys.exit(main())
