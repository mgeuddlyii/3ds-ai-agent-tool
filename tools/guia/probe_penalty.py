"""
probe_penalty.py — 扫「概述段降权 + 泛化段降权」两个因子

══ 为什么这次能改 ════════════════════════════════════════════════

重建索引后 9/12，剩下 3 个 miss 的**正确答案其实已经在结果里**：

    宝可梦XY 御三家选哪个   #814（含「御三家」）排第 3
    火焰纹章觉醒 转职怎么转    #76（含「转职」）  排第 2
    怪物猎人4G 弱点是什么      #419（含「弱点」）排第 2

→ 问题不是"检索不到"，是**概述段/泛化段排在前面**。
所以要动的是**降权**，不是再加一个加权项
（加权项那版实测 9/12 -> 8/12，见 probe_entity_boost.py 的记录）。

两个待测因子
────────────
1. overview_f：概述段降权（现有 0.35）
   概述段是每篇语料的开场白，它对**任何**查询都像命中，
   因为它把游戏名、玩法、平台全说了一遍。

2. generic_f：**标题型泛化段**降权
   「G 级是集会所的最高难度」这种段也一样 ——
   它讲的是**一个通用话题**而不是某个具体操作。
   判据：段首是「XX。」「XX要点」这类**没有主语的具体对象**的标题。

   ⚠️ 但这个因子有风险：像「大型怪物弱点要点」这种段**标题里就有实体词**，
   降权它会伤到正确答案。所以判据必须是"标题里**没有**查询实体词"，
   而这需要在打分时才知道查询 ——
   那就不是"段落属性"而是"查询×段落"的性质了。

→ 所以第二个因子**先不测**，只测第一个 + 一个更稳的第二个：
   **按段落长度的降权**。实测里抢第一的短段（#822/#457/#172）
   长度明显短于含答案的长段，而 BM25 的长度归一化
   （B=0.75）对"短段"惩罚不够。

用法：python tools/guia/probe_penalty.py
"""
import os
import pickle
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import guide_search as G          # noqa: E402
import test_kb as T               # noqa: E402
from train_bpe import BPE         # noqa: E402
from collections import defaultdict  # noqa: E402
import math                        # noqa: E402

OV_PREFIX = ("游戏概述", "游戏概要", "概述")

# 与 guide_search.py 里写死的数值一致（那边是 inline 的，没有导出）
K1, B = G.K1, G.B
COV_ALPHA = 0.5


def load():
    with open(os.path.join(HERE, "ckpt", "bpe.pkl"), "rb") as f:
        d = pickle.load(f)
    bpe = BPE(vocab_size=len(d["inv"]))
    bpe.vocab, bpe.inv, bpe.merges = d["vocab"], d["inv"], d["merges"]
    bpe._train_ranks()
    return T.load(), bpe


def search(kb, bpe, R, query, gid, k, ov_f, len_f):
    """len_f：短段降权指数。sc *= (avg/L)^len_f
    len_f=0 退回原行为；越大越偏好长段。"""
    toks = R.query_tokens(query)
    if not toks:
        toks = [(t, 1) for t in bpe.encode(query)
                if t < kb["V"] - 3 and
                kb["post_off"][t + 1] > kb["post_off"][t]]
        if not toks:
            return []
    N = kb["n"]
    acc, cnt = defaultdict(float), defaultdict(int)
    for tid, df in toks:
        idf = math.log(1.0 + (N - df + 0.5) / (df + 0.5))
        w = idf * idf
        s, e = kb["post_off"][tid], kb["post_off"][tid + 1]
        for j in range(s, e):
            pid = kb["post_ids"][j]
            if gid >= 0 and kb["pass_game"][pid] != gid:
                continue
            tf = kb["post_tf"][j]
            L = R.pass_tlen[pid]
            num = tf * G.K1
            den = tf + G.K1 * (1 - G.B + G.B * L / max(1.0, R.avg_len))
            acc[pid] += w * num / den
            cnt[pid] += 1
    nq = len(toks)
    out = []
    for pid, sc in acc.items():
        sc *= (cnt[pid] / float(nq)) ** COV_ALPHA
        if R.text(pid).startswith(OV_PREFIX):
            sc *= ov_f
        if len_f:
            sc *= (R.avg_len / max(1.0, R.pass_tlen[pid])) ** len_f
        out.append((pid, sc))
    out.sort(key=lambda kv: -kv[1])
    return out[:k]


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

    print("%-6s %-6s  %s" % ("ov_f", "len_f", "命中"))
    print("-" * 52)
    best = None
    for ov_f in (0.35, 0.20, 0.10, 0.05, 0.0):
        for len_f in (0.0, 0.15, 0.30, 0.45):
            hits, bad = 0, []
            for q, kw, gid in CASES:
                gi = G.GAME_ORDER.index(gid) if gid else -1
                res = search(kb, bpe, R, q, gi, 1, ov_f, len_f)
                if res and kw in R.text(res[0][0]):
                    hits += 1
                else:
                    bad.append(q)
            print("%-6.2f %-6.2f  %2d/12   %s"
                  % (ov_f, len_f, hits,
                     "" if hits == 12 else "miss: " + " ".join(bad[:3])))
            if hits >= 10 and (best is None or hits > best[0]):
                best = (hits, ov_f, len_f)
    print()
    if best:
        print("最好：ov_f=%.2f len_f=%.2f -> %d/12" % (best[1], best[2], best[0]))
    else:
        print("没有组合达到 10/12")


if __name__ == "__main__":
    main()
