#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""
score_lab2.py — 按游戏过滤的检索（第二轮实验）

第一轮结论
----------
纯调 BM25 参数（覆盖度/稀有词加权/丢低 df 词）全部卡在 4~5/12，
失败模式高度一致：**「游戏概述」段落霸榜**。
    怪物猎人4G 弱点是什么   -> #122 《怪物猎人 4G》游戏概述
    火焰纹章觉醒 转职怎么转 -> #0   《火焰纹章：觉醒》游戏概述
    马力欧卡丁车7 漂移技巧-> #207 《马力欧卡丁车 7》游戏概述

根因：查询里带游戏名（「怪物猎人4G」「火焰纹章觉醒」），
概述段是全语料里游戏名密度最高的段落，于是它总能赢。
调 BM25 治不了这个 —— 概述段**本来就该**包含游戏名。

真正的解法
----------
**先确定游戏范围，再在范围内检索。**这既修好了检索，
也是产品上该有的交互（离线攻略页 = 先选游戏 → 再提问）。

另外发现一个真bug
-----------------
build_kb.py 里 BM25 的长度归一化拿**字节长度**（pass_off 是字节偏移）
去比**token 平均长度**（avg_len=68.1）。中文 UTF-8 每字 3 字节，
所以 L/avg_len 系统性地偏大约 3 倍，把长度惩罚放大到不合理。
本文件用 token 长度重算。
"""
import os
import sys
import math
import pickle
from collections import defaultdict

HERE = os.path.dirname(os.path.abspath(__file__))
CKPT = os.path.join(HERE, "ckpt")
sys.path.insert(0, HERE)

K1, B = 2.2, 0.75

# 12 条用例；每条给出「这句话该不该被限定到某个游戏」
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

# 游戏短名 -> 出现在查询里的别名（长的排前面，避免「时之笛」吃掉「时之笛3D」）
GAME_ALIAS = {
    "fe_awakening": ["火焰纹章觉醒", "火焰纹章 觉醒", "FE觉醒", "觉醒"],
    "zelda_albw":  ["塞尔达众神三角力量2", "众神三角力量2", "三角力量2", "ALBW"],
    "mh4g":        ["怪物猎人4G", "怪物猎人 4G", "怪猎4G", "MH4G"],
    "mk7":         ["马力欧卡丁车7", "马力欧卡丁车 7", "马卡丁7", "MK7"],
    "pokemon_sm":  ["宝可梦太阳", "宝可梦 太阳", "宝可梦月亮", "太阳月亮", "SM"],
    "pokemon_xy":  ["宝可梦X", "宝可梦 X", "宝可梦Y", "宝可梦 Y", "宝可梦XY", "XY"],
    "luigi2":      ["路易吉洋馆2", "路易吉洋馆 2", "路易吉洋馆", "洋馆2"],
    "zelda_oot3d": ["时之笛3D", "时之笛 3D", "时之笛"],
    "zelda_mm3d":  ["梅祖拉的假面", "梅祖拉假面", "梅祖拉", "假面"],
}


def load_all():
    import test_kb as T
    from train_bpe import BPE
    with open(os.path.join(CKPT, "bpe.pkl"), "rb") as f:
        d = pickle.load(f)
    b = BPE(vocab_size=len(d["inv"]))
    b.vocab, b.inv, b.merges = d["vocab"], d["inv"], d["merges"]
    b._train_ranks()
    return T.load(), b


def game_of(kb, name):
    """短名 -> game_id（build_kb.py 按文件名字母序分配 0..n_game-1）。"""
    if name is None:
        return -1
    gid = list(GAME_ALIAS).index(name)      # 依赖字典顺序 == 文件名字母序
    assert gid < kb["n"], "game id 越界"
    return gid


def detect_game(query):
    """从问句里认出游戏名。认不出返回 None（= 全库检索）。"""
    best = None
    for name, aliases in GAME_ALIAS.items():
        for a in aliases:
            if a in query and (best is None or len(a) > len(best[1])):
                best = (name, a)
    return best[0] if best else None


def search(kb, bpe, query, gid, k=4, alpha=0.0):
    V, N = kb["V"], kb["n"]
    toks = []
    for tid in bpe.encode(query):
        if tid >= V - 3:
            continue
        s, e = kb["post_off"][tid], kb["post_off"][tid + 1]
        if e > s:
            toks.append((tid, math.log(1.0 + (N - (e - s) + 0.5) / ((e - s) + 0.5))))
    if not toks:
        return []
    acc = defaultdict(float)
    cnt = defaultdict(int)
    for tid, idf in toks:
        s, e = kb["post_off"][tid], kb["post_off"][tid + 1]
        for j in range(s, e):
            pid = kb["post_ids"][j]
            if gid >= 0 and kb["pass_game"][pid] != gid:
                continue
            tf = kb["post_tf"][j]
            L = kb["pass_tlen"][pid]                 # token 长度（不是字节）
            num = tf * K1
            den = tf + K1 * (1 - B + B * L / max(1.0, kb["avg_len"]))
            acc[pid] += idf * num / den
            cnt[pid] += 1
    nq = len(toks)
    out = []
    for pid, sc in acc.items():
        if alpha > 0:
            sc *= (cnt[pid] / nq) ** alpha
        out.append((pid, sc))
    out.sort(key=lambda kv: -kv[1])
    return out[:k]


def ptext(kb, pid):
    a, b2 = kb["pass_off"][pid], kb["pass_off"][pid + 1]
    return kb["text"][a:b2].decode("utf-8", "replace")


def evaluate(kb, bpe, use_detect, alpha, verbose=False):
    ok, miss = 0, []
    for q, kw, exp_gid in CASES:
        gid = -1
        if use_detect:
            d = detect_game(q)
            if d:
                gid = game_of(kb, d)
        top = search(kb, bpe, q, gid, alpha=alpha)
        if not top:
            miss.append((q, "无命中"))
            continue
        if kw in ptext(kb, top[0][0]):
            ok += 1
        else:
            miss.append((q, "#%d %s" % (top[0][0], ptext(kb, top[0][0])[:24].replace("\n", " "))))
    print("[%s] 自动识别游戏=%s覆盖^%.1f-> %2d/12"
          % ("OK " if ok >= 10 else ".. ", "是" if use_detect else "否", alpha, ok))
    if verbose:
        for m in miss:
            print("      miss %-22s %s" % m)
    return ok


if __name__ == "__main__":
    import test_kb as T
    import struct
    kb, bpe = load_all()
    # 补 token 长度（build_kb.bin 里没有，得重算）
    pass_tlen = []
    for i in range(kb["n"]):
        a, b2 = kb["pass_off"][i], kb["pass_off"][i + 1]
        pass_tlen.append(len(bpe.encode(kb["text"][a:b2].decode("utf-8", "replace"))))
    kb["pass_tlen"] = pass_tlen
    kb["avg_len"] = sum(pass_tlen) / max(1, len(pass_tlen))
    print("段落 %d，token 平均长 %.1f（此前误用字节长，比值偏 %.1fx）\n"
          % (kb["n"], kb["avg_len"],
             sum(kb["pass_off"][i + 1] - kb["pass_off"][i]
                 for i in range(kb["n"])) / kb["n"] / kb["avg_len"]))
    print("--- 修正长度归一化后的对比 ---")
    evaluate(kb, bpe, False, 0.0, verbose=True)
    print()
    evaluate(kb, bpe, True, 0.0, verbose=True)
    print()
    evaluate(kb, bpe, True, 0.5, verbose=True)
    print()
    evaluate(kb, bpe, True, 1.0, verbose=True)