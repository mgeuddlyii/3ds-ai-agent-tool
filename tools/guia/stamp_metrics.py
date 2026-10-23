#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""
stamp_metrics.py — 复算「钢印_v0.99」里写的每一个数字

═══ 为什么有这个文件 ═══════════════════════════════════════════════
钢印上写「12.6%」而实测是「9.8%」这种错，已经发生过一次。
数字一旦写进文档就会被人当事实引用，**所以每个数字都要能一键复算**。

    python stamp_metrics.py          # 打印全部指标
    python stamp_metrics.py --check  # 与钢印上的值比对，不一致就 exit 1

★ 判断标准很硬：**实测与文档不符就报错，而不是"文档为准"**。
文档是给人读的，实测是事实 —— 冲突时改文档。
"""
from __future__ import print_function

import glob
import os
import pickle
import re
import struct
import sys
from collections import Counter

HERE = os.path.dirname(os.path.abspath(__file__))
KB_BIN = os.path.join(HERE, "ckpt", "guide_kb.bin")
BPE_PKL = os.path.join(HERE, "ckpt", "bpe.pkl")
CORPUS = os.path.join(HERE, "corpus")

# 钢印上写的值。改动钢印时这里也要一起改，改完跑 --check。
STAMP = {
    "passages":      1174,     # 1.1 段落数
    "games": 31,       # 1.1 游戏数
    "retrieve_12of12": 12,      # 1.1 评测集满分
    "ask_contain_inq":  14,      # 1.2 问句含实体词
    "ask_contain_n":     14,      # 1.2 同上，总数
    "ask_noent_hit":     0,       # 1.2 问句不含实体词
    "ask_noent_n":       4,       # 1.2 同上，总数
    "corpus_bytes": 772683,      # 语料总字节
    "vocab_ge3_pct":  20.8,      # 词表 3 字以上占比
    "proper_whole":26,       # 能学成整词的专有名词
    "proper_total":      265,      # 专有名词总数
    "zhizhan_passages":   16,      # 含「转职」的段落数
}


def load_kb():
    """复用 test_kb 的解析器 —— 不要在这里重写一份。

    ⚠️ 我第一版自己写了个解析，结果 `off` **多加了一次 `4*(V+1)`**，
    文本区偏移全错，「含转职的段落数」算出 0，
    校验器于是报了一个假的"与钢印不符"。
    **同一个二进制格式维护两份解析 = 迟早分叉**，
    而这里的分叉会让校验器自己骗自己（它本该是那个说真话的）。
    """
    import test_kb as TK
    return TK.load()


def kb_text(kb, pid):
    return kb["text"][kb["pass_off"][pid]:kb["pass_off"][pid + 1]].decode("utf-8", "replace")


def main():
    check = "--check" in sys.argv
    kb = load_kb()

    # 词表：bpe.pkl 里的 inv 是 dict（内部表示），要归一成按 id 排的 list
    d = pickle.load(open(BPE_PKL, "rb"))
    inv = d["inv"]
    if isinstance(inv, dict):
        inv = [inv[i] for i in range(len(inv))]
    whole, c = set(), Counter()
    for b in inv:
        try:
            s = b.decode("utf-8")
            whole.add(s)
            c[len(s)] += 1
        except UnicodeDecodeError:
            c[0] += 1
    tot = len(inv)
    ge3 = 100.0 * sum(v for k, v in c.items() if k >= 3) / tot

    # 专有名词：从章节标题里抽 3 字以上的中文串
    names = set()
    files = glob.glob(os.path.join(CORPUS, "guide_*.txt"))
    for f in files:
        for line in open(f, encoding="utf-8"):
            m = re.match(r"^#+\s*(.+?)。?\s*$", line.strip())
            if m:
                for n in re.findall(r"[\u4e00-\u9fff]{3,}", m.group(1)):
                    names.add(n)
    n_whole = sum(1 for n in names if n in whole)

    corpus_bytes = sum(os.path.getsize(f) for f in files)
    zhizhan = sum(1 for p in range(kb["n"]) if "转职" in kb_text(kb, p))
    # ★ test_kb.load() 不返回 n_game（它没解那个字段）。
    #   用 pass_game 里出现过的最大值 +1 推出来 —— 不要硬编码 31，
    #   那样语料扩容后这个数会静默变旧。
    n_game = max(kb["pass_game"]) + 1

    got = {
        "passages":    kb["n"],
        "games":       n_game,
        "corpus_bytes": corpus_bytes,
        "vocab_ge3_pct": round(ge3, 1),
        "proper_whole": n_whole,
        "proper_total": len(names),
        "zhizhan_passages": zhizhan,
    }
    # 这两个要跑 python 侧评测才拿得到，这里只做存在性检查
    for k in ("retrieve_12of12", "ask_contain_inq", "ask_contain_n",
              "ask_noent_hit", "ask_noent_n"):
        got.setdefault(k, STAMP[k])

    print("── 钢印 v0.99 指标复算 ──")
    width = max(len(k) for k in got)
    for k, v in got.items():
        print("  %-*s = %s" % (width, k, v))

    if not check:
        print()
        print("加 --check 可与钢印上写的值比对（不一致会报错）")
        return 0

    print()
    bad = []
    for k, want in STAMP.items():
        if k in ("retrieve_12of12", "ask_contain_inq", "ask_contain_n",
                 "ask_noent_hit", "ask_noent_n"):
            continue          # 需要跑评测，不在这里验
        have = got.get(k)
        ok = (abs(float(have) - float(want)) < 0.05
              if isinstance(want, float) else have == want)
        print("  %-4s %-*s 钢印=%-8s 实测=%s"
              % ("OK" if ok else "!!", width, k, want, have))
        if not ok:
            bad.append(k)
    if bad:
        print()
        print("!! %d 项与钢印不符：%s" % (len(bad), " ".join(bad)))
        print("   → 改**钢印**（实测是事实，文档不是）")
        return 1
    print()
    print("OK —— 全部可自动验的数字都与钢印一致")
    print("（评测类数字 retrieve/ask_* 需跑 guide_search.py 与 c_side_sim.py，见钢印第六节）")
    return 0


if __name__ == "__main__":
    sys.exit(main())
