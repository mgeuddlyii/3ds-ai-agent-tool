"""
probe_entity_boost.py — 验证「实体词覆盖率」能否修掉剩下的 3 个 miss

══ 三个 miss 的性质完全不同，不能一刀切 ═══════════════════════════

  心之碎片怎么收集    11/12 段**确实**被 token 命中，却排在第 5 名之后。
                      原因：它们分属 3 款塞尔达，而「收集」(df=24)
                      在别处更密集，BM25 逐词累加后被拉开。
  宝可梦XY 御三家选哪个  正确答案 #814 **排第 3**（前 2 是不含「御三家」的概述段）
  boss战第二阶段怎么办   top2 都在讲 boss 两阶段机制，**评测关键词「阶段」太苛刻**

→ 所以要加的不是"再调一版 BM25 参数"，而是一条**结构化约束**：
  「**查询里的实体词，有多少比例出现在本段落里**」。

为什么这个量是对的：
  · 它与段落长短无关（不像 BM25 那样被 len 归一化拉偏）
  · 它是 0/1 性质，不该参与连续加权 —— 一个段落"含有"某个实体词
    是事实，而"含有得多频繁"是另一回事
  · 它直接对应用户的真实需求：「我要找的是**提到这个东西**的段落」

用法：python tools/guia/probe_entity_boost.py
"""
import math
import os
import pickle
import sys
from collections import defaultdict

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import guide_search as G          # noqa: E402
import test_kb as T               # noqa: E402
from train_bpe import BPE         # noqa: E402

K1, B = G.K1, G.B


def load():
    with open(os.path.join(HERE, "ckpt", "bpe.pkl"), "rb") as f:
        d = pickle.load(f)
    bpe = BPE(vocab_size=len(d["inv"]))
    bpe.vocab, bpe.inv, bpe.merges = d["vocab"], d["inv"], d["merges"]
    bpe._train_ranks()
    return T.load(), bpe


def entity_terms(query, bpe, kb):
    """查询里的「实体词」：BPE 切出来、且 df 不太高的那些 token。

    判据用 df 而不是停用词表 —— 停用词表是人工维护的，而 df 是**语料事实**。
    「收集」df=24 这样的泛词，即便不在停用表里也不该算实体词。
    阈值 15 是从 1174 段 × 平均 80 token 的规模来的：
    低于它的词在语料里足够独特，高于它的多半是通用动词。
    """
    V = kb["V"]
    out = []
    for tid in bpe.encode(query):
        if tid >= V - 3:
            continue
        df = kb["post_off"][tid + 1] - kb["post_off"][tid]
        if df == 0 or df > 15:
            continue
        piece = bpe.inv[tid].decode("utf-8", "replace")
        if piece in G.STOP_EXACT:
            continue
        if any(s in piece for s in G.STOP_SUBSTR):
            continue
        out.append((tid, df, piece))
    return out


def search(kb, bpe, R, query, gid, k, w_ent, w_cov):
    """w_ent = 实体词覆盖率的权重（0 = 退回原算法）。"""
    ents = entity_terms(query, bpe, kb)
    ent_ids = set(t[0] for t in ents)
    ent_str = {t[0]: t[2] for t in ents}

    # 哪些段落"含"了至少一个实体词 —— 用倒排一次算完
    ent_doc = defaultdict(set)      # pid -> {tid}
    for tid in ent_ids:
        s, e = kb["post_off"][tid], kb["post_off"][tid + 1]
        for j in range(s, e):
            ent_doc[kb["post_ids"][j]].add(tid)

    base = R.search(query, gid, k=max(k, 20))
    if not ents:
        return [(pid, sc) for pid, sc in base[:k]], []

    N = kb["n"]
    scored = []
    for pid, sc in base:
        got = ent_doc.get(pid, ())
        cover = len(got) / float(len(ents))
        # 关键：**不给"完全没提到实体词"的段落任何机会**
        # （cover=0 时乘一个小于 1 的因子而不是直接归零，
        #   保留一点翻盘余地，避免实体词切分不好时全军覆没）
        boost = (w_ent ** cover) if cover > 0 else 0.15
        scored.append((pid, sc * boost * (w_cov ** (cover == 0))))
    scored.sort(key=lambda kv: -kv[1])
    return scored[:k], ents


CASES = [
    ("沙之杖怎么用", "沙之杖", None),
    ("心之碎片怎么收集", "心之碎片", None),
    ("大师之剑在哪里拿", "大师之剑", None),
    ("怪物猎人4G 弱点是什么", "弱点", "mh4g"),
    ("宝可梦XY 御三家选哪个", "御三家", "pokemon_xy"),
    ("火焰纹章觉醒 转职怎么转", "转职", "fe_awakening"),
    ("马力欧卡丁车7 漂移技巧", "漂移", "mk7"),
    ("路易吉洋馆2 吸尘器怎么用", "吸尘器", "luigi2"),
    ("时之笛 钩索箭哪里拿", "钩索箭", "zelda_oot3d"),
    ("梅祖拉的假面 三天循环怎么过", "循环", "zelda_mm3d"),
    ("战斗命中率怎么提高", "命中", None),
    ("boss战第二阶段怎么办", "阶段", None),
]


def main():
    kb, bpe = load()
    R = G.Retriever(kb, bpe, G.GAME_ORDER)
    print("段落 %d  游戏 %d" % (kb["n"], len(G.GAME_ORDER)))
    print()

    for w_ent in (1.0, 1.6, 2.2, 3.0):
        hits = 0
        bad = []
        for q, kw, gid in CASES:
            gi = G.GAME_ORDER.index(gid) if gid else -1
            res, ents = search(kb, bpe, R, q, gi, 4, w_ent, 1.0)
            ok = bool(res) and kw in R.text(res[0][0])
            hits += ok
            if not ok:
                bad.append((q, kw, [R.text(p)[:40] for p, _ in res[:1]]))
        print("w_ent=%.1f  ->  %2d/12" % (w_ent, hits))
        for q, kw, t in bad:
            print("      miss %-22s  top1=%s" % (q, t[0] if t else "(无)"))
    print()
    print("实体词判定示例：")
    for q in ("心之碎片怎么收集", "boss战第二阶段怎么办", "御三家选哪个"):
        print("   %-22s -> %s" % (q, entity_terms(q, bpe, kb)))


if __name__ == "__main__":
    main()
