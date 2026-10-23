#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""
cand_probe.py — 量一下「输出层候选集」到底有多大

背景
----
nnet_set_candidates() 把输出层从全词表 4096 缩到「检索命中原文里出现过的
token」。省多少完全取决于这个并集有多大 —— 所以必须实测，不能估。

本脚本对 guide_search.py 的 12 条实测用例，逐条做一次真实检索，
把 top-4 命中段落的 BPE token 取并集，统计规模分布。

顺带算一下对应的输出层 MAC 数，与全词表对比。
"""
import os
import pickle
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)


def main():
    import test_kb as T
    from train_bpe import BPE
    import guide_search as G

    with open(os.path.join(HERE, "ckpt", "bpe.pkl"), "rb") as f:
        d = pickle.load(f)
    bpe = BPE(vocab_size=len(d["inv"]))
    bpe.vocab, bpe.inv, bpe.merges = d["vocab"], d["inv"], d["merges"]
    bpe._train_ranks()

    kb = T.load()
    R = G.Retriever(kb, bpe, G.GAME_ORDER)
    V = kb["V"]
    D = 128   # 1.18M 模型的 d_model

    print("词表 V=%d  d_model=%d" % (V, D))
    print()
    print("%-24s %5s %5s %5s %7s %7s %6s" %
          ("用例", "top1", "并集", "全4段", "MAC候选", "MAC全表", "占比"))
    print("-" * 78)

    rows = []
    for q, kw, exp in G.CASES:
        # 不走 G.answer()：它用的是模块级全局 _RETRIEVER，
        # 这里 R 是局部对象，绕开那条隐式依赖。
        gname, rest = G.split_game(q)
        gid = G.GAME_ORDER.index(gname) if gname in G.GAME_ORDER else -1
        hits = R.search(rest, gid)
        if not hits:
            print("%-24s  (无命中)" % q[:22])
            continue

        # top-1 段落的 token 集 —— 最激进的约束
        u1 = set(bpe.encode(R.text(hits[0][0])))
        # top-4 全部段落的 token 并集 —— 我们实际要用的
        u4 = set()
        for pid, _ in hits:
            u4 |= set(bpe.encode(R.text(pid)))

        mac_c = len(u4) * D
        mac_f = V * D
        rows.append((q, len(u1), len(u4)))
        print("%-24s %5d %5d %5d %7d %7d %5.1f%%" %
              (q[:22], len(u1), len(u4), len(hits),
               mac_c, mac_f, 100.0 * mac_c / mac_f))

    print("-" * 78)
    if rows:
        u1s = [r[1] for r in rows]
        u4s = [r[2] for r in rows]
        print("top-1  token 数：min=%d  中位=%d  max=%d"
              % (min(u1s), sorted(u1s)[len(u1s) // 2], max(u1s)))
        print("top-4  token 数：min=%d  中位=%d  max=%d"
              % (min(u4s), sorted(u4s)[len(u4s) // 2], max(u4s)))
        avg4 = sum(u4s) / len(u4s)
        print()
        print("按 top-4 平均 %d 个候选算：" % round(avg4))
        print("  输出层  %d -> %d MAC  (省 %.1f%%)"
              % (V * D, round(avg4) * D, 100.0 * (1 - avg4 / V)))
        # 单 token 总算力（4 层主干 + 输出层 + 满 ctx 注意力）
        trunk = 4 * (4 * 128 * 128 + 2 * 128 * 384)
        attn = 2 * 128 * 128
        tot_f = trunk + V * D + attn
        tot_c = trunk + round(avg4) * D + attn
        print("  单token 总算力 %d -> %d MAC  (省 %.1f%%)"
              % (tot_f, tot_c, 100.0 * (1 - tot_c / tot_f)))

        # 缓冲区要开多大：候选数不可能超过 vocab，但要看离上限多远
        print()
        print("候选数组上限建议：%d（top-4 实测最大 %d，留 2 倍余量）"
              % (1024, max(u4s)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
