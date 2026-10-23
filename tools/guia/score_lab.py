#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""
score_lab.py — 打分函数选型实验台

为什么需要这个
--------------
BM25 在中文短查询上有经典失效：**稀有的无信息碎片词会压过实体词**。

实测「大师之剑在哪里拿」：
    大师之剑  df=3   idf=5.01  -> 段落 503,504,508（正确）
    在哪里    df=2   idf=5.35  -> 段落 290,451（无关，马力欧卡丁车）
    拿        df=20  idf=3.24  -> 段落 290 也命中
纯 BM25 求和 -> 290 靠 2 个碎片词堆到 4.53，压过只命中 1 个实体词的 503。

对策（下面逐个实测，用 12 条用例的命中率打分）：
    A 纯 BM25                     基线
    B BM25 x 覆盖度^alpha         命中 query 词种类越多，奖励越高
    C 去掉单字/极低 df 词         清洗查询
    D B + C                       组合
    E 稀有词优先（idf 加权覆盖）   对高 idf 词的命中给额外权重
"""
import os
import sys
import math
import pickle
from collections import defaultdict

HERE = os.path.dirname(os.path.abspath(__file__))
CKPT = os.path.join(HERE, "ckpt")
sys.path.insert(0, HERE)

CASES = [
    ("沙之杖怎么用", "沙之杖"),
    ("心之碎片怎么收集", "心之碎片"),
    ("大师之剑在哪里拿", "大师之剑"),
    ("怪物猎人4G 弱点是什么", "弱点"),
    ("宝可梦XY 御三家选哪个", "御三家"),
    ("火焰纹章觉醒 转职怎么转", "转职"),
    ("马力欧卡丁车7 漂移技巧", "漂移"),
    ("路易吉洋馆2 吸尘器怎么用", "吸尘器"),
    ("时之笛 钩索箭哪里拿", "钩索箭"),
    ("梅祖拉的假面 三天循环怎么过", "循环"),
    ("战斗命中率怎么提高", "命中"),
    ("boss战第二阶段怎么办", "阶段"),
]

K1, B = 2.2, 0.75


def load_all():
    import test_kb as T
    from train_bpe import BPE
    with open(os.path.join(CKPT, "bpe.pkl"), "rb") as f:
        d = pickle.load(f)
    b = BPE(vocab_size=len(d["inv"]))
    b.vocab, b.inv, b.merges = d["vocab"], d["inv"], d["merges"]
    b._train_ranks()
    return T.load(), b


def prep(kb, query, drop_solo=False, min_df=1):
    """把查询变成 [(tid, idf, df)]，可清洗。"""
    V, N = kb["V"], kb["n"]
    out = []
    for tid in kb["_ids"](query) if False else bpe_encode(query):
        if tid >= V - 3:
            continue
        s, e = kb["post_off"][tid], kb["post_off"][tid + 1]
        df = e - s
        if df == 0:
            continue
        if drop_solo and df < min_df:
            continue
        idf = math.log(1.0 + (N - df + 0.5) / (df + 0.5))
        out.append((tid, idf, df))
    return out


_bpe = None


def bpe_encode(q):
    return _bpe.encode(q)


def run(kb, query, game_filter=-1, alpha=0.0, drop_solo=False,
        min_df=1, rare_boost=0.0, k=4):
    toks = prep(kb, query, drop_solo, min_df)
    if not toks:
        return []
    acc = defaultdict(float)
    hit_cnt = defaultdict(int)
    max_idf = max(t[1] for t in toks) or 1.0
    for tid, idf, df in toks:
        s, e = kb["post_off"][tid], kb["post_off"][tid + 1]
        for j in range(s, e):
            pid = kb["post_ids"][j]
            if game_filter >= 0 and kb["pass_game"][pid] != game_filter:
                continue
            tf = kb["post_tf"][j]
            L = kb["pass_off"][pid + 1] - kb["pass_off"][pid]
            num = tf * K1
            den = tf + K1 * (1 - B + B * L / max(1.0, kb["avg_len"]))
            acc[pid] += idf * num / den
            hit_cnt[pid] += 1
            if rare_boost > 0:
                # 稀有词的命中额外加分（idf 越接近 max 权重越大）
                acc[pid] += rare_boost * (idf / max_idf) * (num / den)
    nq = len(toks)
    scored = []
    for pid, sc in acc.items():
        cov = hit_cnt[pid] / nq
        if alpha > 0:
            sc *= (cov ** alpha)
        scored.append((pid, sc))
    scored.sort(key=lambda kv: -kv[1])
    return scored[:k]


def ptext(kb, pid):
    a, b2 = kb["pass_off"][pid], kb["pass_off"][pid + 1]
    return kb["text"][a:b2].decode("utf-8", "replace")


def evaluate(name, **kw):
    kb = _KB
    ok, miss = 0, []
    for q, kw2 in CASES:
        top = run(kb, q, **kw)
        if not top:
            miss.append((q, "无命中"))
            continue
        if kw2 in ptext(kb, top[0][0]):
            ok += 1
        else:
            miss.append((q, "#%d %s" % (top[0][0], ptext(kb, top[0][0])[:26])))
    print("[%-3s] %-34s %2d/%d" % ("OK" if ok >= 10 else "..",
                                   name, ok, len(CASES)))
    for m in miss:
        print("        miss: %-22s %s" % m)
    return ok


_KB = None

if __name__ == "__main__":
    kb, _b = load_all()
    _KB = kb
    _bpe = _b
    print("基线段落 %d，平均长 %.1f\n" % (kb["n"], kb["avg_len"]))
    evaluate("A 纯 BM25（基线）")
    evaluate("B 覆盖度^0.5", alpha=0.5)
    evaluate("B 覆盖度^1.0", alpha=1.0)
    evaluate("B 覆盖度^1.5", alpha=1.5)
    evaluate("C 丢掉 df<3 的词", drop_solo=True, min_df=3)
    evaluate("C 丢掉 df<5 的词", drop_solo=True, min_df=5)
    evaluate("D 覆盖^1.0 + df>=3", alpha=1.0, drop_solo=True, min_df=3)
    evaluate("D 覆盖^1.5 + df>=3", alpha=1.5, drop_solo=True, min_df=3)
    evaluate("E 稀有词加权 1.0", rare_boost=1.0)
    evaluate("E 稀有词加权 3.0", rare_boost=3.0)
    evaluate("E+覆盖 稀有2.0/覆盖^1.0", rare_boost=2.0, alpha=1.0)
    evaluate("E+清洗 稀有3.0/覆盖^1.0/df>=3", rare_boost=3.0, alpha=1.0,
             drop_solo=True, min_df=3)