"""
probe_posting.py — 决定 MAX_POSTING 该设多大

══ 为什么这个值要用数据定，而不是"设大点保险" ═══════════════════════

MAX_POSTING 的作用是**限制单个词最多索引多少段落**（按 tf 取前 N）。
它有两头代价：

  · 设太小：词的段落被整段切掉 -> **限定游戏时 0 命中**
    （实测候选词表 19 处 0 命中，全部是这个原因）
  · 设太大：倒排条目线性增长 -> 索引变大、C 端读入变慢、内存吃紧

真实截断规模（1174 段 / 31 款语料实测）：

    真实 df 区间     token 数    丢弃条数    该不该管
    1-5                  750          0        -
    6-15               1447          0        -
    16-30                743        659        ← 实体词区，截断最亏
    31-60                419       7648        ← 实体词区，截断最亏
    61-120               124       7328        ← 边界
    121+                  47      11599        ← 虚词区（「的」「是」），存了也没用

关键：**虚词区丢掉的 11599 条是"应该丢的"** ——
「的」出现在 827 段里，存下全部 827 条既占内存又不带来检索价值
（idf 本来就接近 0，打分时几乎不参与）。

所以正确的目标不是"不丢任何段落"（那要 MAX_POSTING=1153，内存爆），
而是"**让 df 在 16~60 的实体词不被截断**"。

用法：python tools/guia/probe_posting.py
"""
import os
import pickle
import subprocess
import sys
from collections import Counter

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

BUILD = os.path.join(HERE, "..", "..", "tools", "build.py")
KB = os.path.join(HERE, "ckpt", "guide_kb.bin")
BAK = KB + ".bak_mp"

CANDIDATES = (24, 32, 40, 48, 64)


def run(cmd):
    return subprocess.run(cmd, capture_output=True, text=True)


def measure():
    """量一次：索引大小 + 各 df 区间的丢弃条数。"""
    import test_kb as T
    from train_bpe import BPE

    with open(os.path.join(HERE, "ckpt", "bpe.pkl"), "rb") as f:
        d = pickle.load(f)
    bpe = BPE(vocab_size=len(d["inv"]))
    bpe.vocab, bpe.inv, bpe.merges = d["vocab"], d["inv"], d["merges"]
    bpe._train_ranks()

    kb = T.load()
    N = kb["n"]
    lid = kb["pass_off"]
    loff = kb["post_off"]

    true_df = Counter()
    for pid in range(N):
        a, z = lid[pid], lid[pid + 1]
        t = bytes(kb["text"][a:z]).decode("utf-8", "replace")
        for tid in set(bpe.encode(t)):
            if tid < kb["V"] - 3:
                true_df[tid] += 1

    # 各区间丢了多少
    loss = {}
    for lo, hi in ((1, 15), (16, 30), (31, 60), (61, 120), (121, 99999)):
        tids = [t for t, c in true_df.items() if lo <= c <= hi]
        loss[(lo, hi)] = sum(max(0, true_df[t] - len_postings(kb, t))
                            for t in tids)

    size = os.path.getsize(KB)
    # 倒排条目数
    entries = loff[kb["V"]]
    return size, entries, loss


def len_postings(kb, tid):
    return kb["post_off"][tid + 1] - kb["post_off"][tid]


def main():
    if os.path.exists(KB) and not os.path.exists(BAK):
        with open(KB, "rb") as s, open(BAK, "wb") as d:
            d.write(s.read())
        print("已备份原索引到 %s\n" % os.path.basename(BAK))

    py = sys.executable
    print("%-6s %-10s %-10s %8s %8s %8s" %
          ("MP", "索引KB", "倒排条目", "丢1-15", "丢16-60", "丢61+"))
    print("-" * 58)

    rows = []
    for mp in CANDIDATES:
        # 改 build_kb.py 里的 MAX_POSTING
        p = os.path.join(HERE, "build_kb.py")
        src = open(p, encoding="utf-8").read()
        import re
        new = re.sub(r"MAX_POSTING = \d+", "MAX_POSTING = %d" % mp, src, count=1)
        with open(p, "w", encoding="utf-8") as f:
            f.write(new)
        r = run([py, os.path.join(HERE, "build_kb.py")])
        if r.returncode != 0:
            print("  MAX_POSTING=%d 构建失败" % mp)
            print(r.stderr[-400:])
            continue

        size, entries, loss = measure()
        l1 = loss[(1, 15)]
        l2 = loss[(16, 30)] + loss[(31, 60)]
        l3 = loss[(61, 120)] + loss[(121, 99999)]
        print("%-6d %-10.1f %-10d %8d %8d %8d"
              % (mp, size / 1024.0, entries, l1, l2, l3))
        rows.append((mp, size, entries, l1, l2, l3))

    # 还原 build_kb.py
    with open(os.path.join(HERE, "build_kb.py"), "w", encoding="utf-8") as f:
        f.write(src)

    print()
    # 选点：df 16~60 的实体词丢 0 条，且索引最小
    good = [r for r in rows if r[4] == 0]
    if good:
        best = min(good, key=lambda r: r[1])
        print("★ 实体词区（df 16~60）零丢弃的最小值：MAX_POSTING=%d" % best[0])
        print("  索引 %.1f KB / 倒排 %d 条" % (best[1] / 1024.0, best[2]))
        # C 端可写内存
        kb_bytes = best[2] * 5
        print("  C 端倒排约 %.1f KB（post_ids int32 + post_tf int8）"
              % (kb_bytes / 1024.0))
        print("  占可写 784KB 的 %.1f%%" % (kb_bytes / 1024.0 / 784 * 100))
    else:
        print("!! 候选值里没有一个能让 df 16~60 零丢弃")


if __name__ == "__main__":
    main()
