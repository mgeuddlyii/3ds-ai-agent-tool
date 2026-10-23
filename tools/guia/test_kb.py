#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""
test_kb.py — 检索层自测

为什么要单独测
--------------
索引写错了，训练再久也没用 —— 而且索引的错不会报错，
只会让检索「看起来能跑但答非所问」。所以必须有一组带标准答案的问句。

判据：top-1 命中的段落必须包含问句里的**关键实体**
（游戏名或道具名），否则算失败。
"""
import os
import sys
import math
import struct
import pickle
from collections import defaultdict

HERE = os.path.dirname(os.path.abspath(__file__))
CKPT = os.path.join(HERE, "ckpt")

# (问句, 必须出现在命中段落里的关键词)
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


def load():
    with open(os.path.join(CKPT, "guide_kb.bin"), "rb") as f:
        blob = f.read()
    assert blob[:4] == b"GKB1", "magic 不对"
    off = 4
    n, n_game, V, text_len = struct.unpack_from("<4i", blob, off); off += 16
    avg_len, = struct.unpack_from("<f", blob, off); off += 4
    pass_off = struct.unpack_from("<%di" % (n + 1), blob, off); off += 4 * (n + 1)
    # ⚠️ pass_game 是 int16（2026-10-05 由 int8 扩的：语料超过 127 款后
    #    int8 会回绕成负数，限定游戏过滤把 >=128 的游戏永远过滤成 0 命中）。
    #    这里曾经写成 "<%db"，解析出来的 gid>=128 全是负数 ——
    #    gen_cand_rows 全量验证时所有高 gid 的 own 恒为 0（假 FAIL）。
    pass_game = struct.unpack_from("<%dh" % n, blob, off); off += 2 * n
    post_off = struct.unpack_from("<%di" % (V + 1), blob, off); off += 4 * (V + 1)
    n_post = post_off[V]
    post_ids = struct.unpack_from("<%dh" % n_post, blob, off); off += 2 * n_post
    post_tf = struct.unpack_from("<%db" % n_post, blob, off); off += n_post
    text = blob[off:off + text_len]
    return dict(n=n, V=V, avg_len=avg_len, pass_off=pass_off, pass_game=pass_game,
                post_off=post_off, post_ids=post_ids, post_tf=post_tf, text=text)


def search(kb, bpe, query, k=4, game_filter=-1):
    V = kb["V"]
    ids = bpe.encode(query)
    score = defaultdict(float)
    N = kb["n"]
    for tid in ids:
        if tid >= V - 3:
            continue
        s, e = kb["post_off"][tid], kb["post_off"][tid + 1]
        if e <= s:
            continue
        df = e - s
        idf = math.log(1.0 + (N - df + 0.5) / (df + 0.5))
        for j in range(s, e):
            pid = kb["post_ids"][j]
            if game_filter >= 0 and kb["pass_game"][pid] != game_filter:
                continue
            tf = kb["post_tf"][j]
            L = kb["pass_off"][pid + 1] - kb["pass_off"][pid]
            # BM25
            num = tf * 2.2
            den = tf + 1.2 * (0.25 + 0.75 * L / max(1.0, kb["avg_len"]))
            score[pid] += idf * num / den
    top = sorted(score.items(), key=lambda kv: -kv[1])[:k]
    return top


def passage_text(kb, pid):
    a, b = kb["pass_off"][pid], kb["pass_off"][pid + 1]
    return kb["text"][a:b].decode("utf-8", "replace")


def main():
    sys.path.insert(0, HERE)
    from train_bpe import BPE
    with open(os.path.join(CKPT, "bpe.pkl"), "rb") as f:
        d = pickle.load(f)
    bpe = BPE(vocab_size=len(d["inv"]))
    bpe.vocab, bpe.inv, bpe.merges = d["vocab"], d["inv"], d["merges"]
    bpe._train_ranks()

    kb = load()
    print("载入 %d 段落 / 词表 %d / 平均长 %.1f" % (kb["n"], kb["V"], kb["avg_len"]))
    print("-" * 68)
    ok = 0
    for q, kw in CASES:
        top = search(kb, bpe, q)
        if not top:
            print("[MISS] %-24s 无命中" % q)
            continue
        pid, sc = top[0]
        txt = passage_text(kb, pid)
        hit = kw in txt
        ok += 1 if hit else 0
        flag = "OK  " if hit else "MISS"
        print("[%s] %-24s -> #%d (%.2f) %s" % (flag, q, pid, sc, txt[:34]))
    print("-" * 68)
    print("命中率 %d/%d" % (ok, len(CASES)))

    # 顺带看看 top-4 的内容，人工判断质量
    print()
    print("=== 样例：'沙之杖怎么用' 的 top-4 ===")
    for pid, sc in search(kb, bpe, "沙之杖怎么用", k=4):
        print("  #%-4d %.2f  %s" % (pid, sc, passage_text(kb, pid)[:70]))
    return 0 if ok >= len(CASES) * 0.8 else 1


if __name__ == "__main__":
    sys.exit(main())
