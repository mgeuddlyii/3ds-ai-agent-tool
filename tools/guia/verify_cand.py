#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""
verify_cand.py — 验证**卡上那张** g_cand_set[] 的候选词真的能检索到东西

══ 这个脚本为什么存在 ════════════════════════════════════════════════
候选词是底屏上一键提问的入口。词选得不好有两种失败：
  · 词太泛（「3DS」「简单说」）→ 点下去一堆弱相关段落
  · 词在该款语料里**检索不到** → 点下去 0 命中，用户以为功能坏了

══ 2026-10-05 重写 ═══════════════════════════════════════════════════
以前这份脚本里塞了一张**手抄的 31 款候选词表**（`CAND = {...}`），
与语料口径（89 款 → 211 款）一脱节，第一个检查
`set(CAND) != set(GAME_ORDER)` 就红，脚本干t做成一个"永远过期的抄本"。
而真正要验的东西另有其人 —— **已经落盘进 sc_guide.c 的 g_cand_set**。
从语料**重新算**一遍的是 gen_cand_rows.py，它负责"生成 + 自验"；
这里负责"生成完**读回磁盘**再验一遍"，才是对卡上产物真正的守门。

两条链路的差别：
  gen_cand_rows.py —— 从语料算出候选、逐条 verify，不落盘就报 FAIL
  verify_cand.py   —— 读回**已经写进源码**的 g_cand_set，验卡上那张表

⚠️ 同源要求：读的是 `app/source/ui/sc_guide.c` 里 `g_cand_set[]` 的**字面**。
   如果哪次写入没落盘成功、或落盘的是旧表，这里会红 —— 这正是它该干的。

用法：python tools/guia/verify_cand.py
"""
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import guide_search as G          # noqa: E402
import test_kb as T               # noqa: E402
from train_bpe import BPE         # noqa: E402

SC_GUIDE = os.path.join(os.path.dirname(os.path.dirname(HERE)),
                        "app", "source", "ui", "sc_guide.c")
ROW_RE = re.compile(r"(static\s+const\s+CandSet\s+g_cand_set\[\]\s*=\s*\{)(.*?)(\};)", re.S)
ALL_RE = re.compile(r"(static\s+const\s+char\s*\*\s*const\s+g_cand_all\[\s*CAND_N\s*\]\s*=\s*\{)(.*?)(\};)", re.S)


def load():
    with open(os.path.join(HERE, "ckpt", "bpe.pkl"), "rb") as f:
        d = pickle.load(f)
    bpe = BPE(vocab_size=len(d["inv"]))
    bpe.vocab, bpe.inv, bpe.merges = d["vocab"], d["inv"], d["merges"]
    bpe._train_ranks()
    return T.load(), bpe


def load_rows():
    """从 sc_guide.c 读回 g_cand_set：[[词...], ...]，顺序即 gid。"""
    if not os.path.exists(SC_GUIDE):
        raise RuntimeError("找不到 %s" % SC_GUIDE)
    src = open(SC_GUIDE, encoding="utf-8").read()
    ms = ROW_RE.findall(src)
    if len(ms) != 1:
        raise RuntimeError("g_cand_set[] 匹配到 %d 处，不敢验（预期 1）" % len(ms))
    rows = []
    for line in ms[0][1].splitlines():
        if not line.strip():
            continue
        words = re.findall(r'"([^"]*)"', line)
        if not words:
            continue
        rows.append(words)
    return rows


def load_all():
    """g_cand_all（「全部游戏」那组）同样读回源码再验。"""
    if not os.path.exists(SC_GUIDE):
        return []
    src = open(SC_GUIDE, encoding="utf-8").read()
    ms = ALL_RE.findall(src)
    if len(ms) != 1:
        raise RuntimeError("g_cand_all[] 匹配到 %d 处（预期 1）" % len(ms))
    return re.findall(r'"([^"]*)"', ms[0][1])


def main():
    kb, bpe = load()
    R = G.Retriever(kb, bpe, G.GAME_ORDER)

    rows = load_rows()
    if len(rows) != len(G.GAME_ORDER):
        print("!! g_cand_set 有 %d 行，GAME_ORDER 有 %d 款 —— 口径不一致" %
              (len(rows), len(G.GAME_ORDER)))
        return 1

    print("读回 sc_guide.c 的 g_cand_set：%d 行 × %d 词（与 GAME_ORDER 同口径）"
          % (len(rows), len(rows[0])))
    print("判据：限定该游戏时检索，top1 正文里含这个词本身 ——"
          " 那是用户按下去会看到的答案")
    print()

    bad_total = 0
    weak_rows = []
    for gi, words in enumerate(rows):
        marks = []
        for w in words:
            res = R.search(w, gi, k=1)
            if not res:
                marks.append("%s=0命中" % w)
                bad_total += 1
            elif w in R.text(res[0][0]):
                marks.append("%s=OK" % w)
            else:
                marks.append("%s=弱" % w)
                bad_total += 1
        # ⚠️ 判「弱」必须看 `=弱` 这个**后缀**，不能子串包含 "弱" ——
        #    候选词本身就叫「弱点」，`"弱" in "弱点=OK"` 恒真，
        #    于是 6 行全 OK 的游戏被我自己的判据列进"不合格"（假红）。
        if any("=0命中" in m or "=弱" in m for m in marks):
            weak_rows.append((gi, G.GAME_ORDER[gi], "  ".join(marks)))
        if gi < 3 or (gi + 1) % 40 == 0 or gi >= len(rows) - 3:
            print("  %s" % ("  ".join(marks)))

    print()
    print("  ...（中间 %d 行略）" % (len(rows) - 6))
    print()

    # 「全部游戏」那组：无 gid 过滤，只要求能检索到东西
    allw = load_all()
    allbad = []
    for w in allw:
        res = R.search(w, -1, k=1)
        if not res:
            allbad.append("%s=0命中" % w)
        elif w not in R.text(res[0][0]):
            # "boss战" 这种词 BPE 会切成 boss + 战，检索时根本没有完整
            # token，字面判据恒不成立 —— 那算 warn 不算 FAIL（见下面合计）
            allbad.append("%s=弱" % w)
    print("g_cand_all（全部游戏，%d 词）：%s" % (len(allw), "  ".join(allbad) if allbad else "全部 OK"))
    bad_total += len(allbad)

    if weak_rows:
        print()
        print("不合格明细（=0命中 点下去什么都查不到；=弱 查到了但不在这个词上）:")
        for gi, stem, m in weak_rows:
            print("   gid=%-3d %-30s %s" % (gi, stem[:30], m))
    print()
    if bad_total:
        print("合计 %d 处不合格" % bad_total)
        return 1
    print("全部合格。")
    return 0


import pickle  # noqa: E402
if __name__ == "__main__":
    sys.exit(main())
