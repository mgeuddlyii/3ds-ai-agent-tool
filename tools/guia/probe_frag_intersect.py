"""
probe_frag_intersect.py — 验证「碎片交集」能不能补上 BPE 切分的损失

══ 结论：不需要加这一层。实测 12/12 全部命中，与原版无差异。═══════

原因：BPE 切分**不等于**检索失败。
切开的词它的碎片仍然是倒排里的 token，而**同时**含这些碎片的段落
几乎必然就是那个词的段落 —— 这正是原算法里
`覆盖度 = (cnt/nq)^0.5` 那一项在做的事。

实测（限定游戏，问被切开的专有名词，看 top1 是否含该词）：

    科学搜查        切 4 块   OK
    女神武器        切 3 块   OK
    剑与回旋镖      切 3 块   OK
    宝石与收集要素  切 2 块   OK
    卡丁车组装      切 2 块   OK
    属性克制要点    切 2 块   OK
    初始宝可梦      切 2 块   OK
    黑暗世界        切 1 块   OK

w_frag 扫 0.0 / 1.0 / 2.0 / 3.0 / 5.0 / 8.0 → **全是 12/12**，
所以额外加权既没有收益也没有害处，但**没有理由加**。

⚠️ 唯一真正的代价在别处：切开的词在**候选词表**里不能用 ——
底屏上给用户的一键提问词必须能整词切分，否则那一下是白点
（见 tools/guia/fix_cand.py 的三道判据）。而那不是检索算法的问题。

⚠️ 还有一个坑记在这里，因为它让人多花了一轮：
**测检索改进时，口径必须与 guide_search 逐字对齐**。
本脚本第一版漏了「游戏名剔除」，测出 9/12，
而 guide_search 自己是 12/12 —— 差的 3 个全是「限定游戏」的用例。
→ 9/12 与 12/12 **不是两个算法的好坏，而是口径对不对。**

所以：BPE 词表（4096，3 字以上整词只占 12.6%）确实是数据质量的
根本约束，但**换词表的收益在检索侧几乎已经吃满了** ——
它真正的价值在**生成**侧（模型能直接输出「科学搜查」而不是
拼出来的碎片），而那要重训模型（几小时级 + 重验留出集）。

══ 问题的性质 ══════════════════════════════════════════════════════

BPE 只把**高频 2 字词**学成整词，3~4 字的攻略专有名词几乎全被切开
（实测：章节标题里 309 个专有名词，91% 被切；
词表里 3 字以上的整词只占 12.6%）。

于是查询「科学搜查」变成 4 个碎片 科/学/搜/查，各自都是高频噪声：

    碎片      df
    科        4
    学        18
    搜        21
    查        20
    任一命中   51 段    ← 全是噪声
    全部命中   3 段     ← 正是答案

→ **交集**才是信号：这些段落同时含「科 学 搜 查」四个字，
   几乎必然是在讲「科学搜查」。这与 BPE 的问题正相反 ——
   切开导致噪声，而**碎片必须同时出现**这一点又恰好把噪声收敛了。

⚠️ 这不是"用 C 端字符 bigram"那种另起炉灶的做法：
   碎片本来就是索引里的 token，所以这一层**不需要新索引、不需要重训**。

══ 为什么这条比重训词表划算 ══════════════════════════════════════════

换 BPE 词表会让模型 embedding 与所有 token id 全部失效，
必须重训整个模型（几小时级，且要重新验证留出集）。
而这一层是**纯检索侧的后处理**，可独立验证、独立回退。

用法：python tools/guia/probe_frag_intersect.py
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

# 碎片交集的权重。不是 1.0 —— 见下面的实测。
W_FRAG = 3.0


def load():
    with open(os.path.join(HERE, "ckpt", "bpe.pkl"), "rb") as f:
        d = pickle.load(f)
    bpe = BPE(vocab_size=len(d["inv"]))
    bpe.vocab, bpe.inv, bpe.merges = d["vocab"], d["inv"], d["merges"]
    bpe._train_ranks()
    return T.load(), bpe


def search(kb, bpe, R, query, gid, k, w_frag):
    """在原算法上加「碎片交集」一项。w_frag=0 则退回原行为。"""
    toks = R.query_tokens(query)
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

    # ---- 碎片交集 ----
    #
    # ⚠️ 判据是「**同时命中几种查询 token**」，不是「原始词被切了几块」——
    # 前者能从倒排直接算出来（cnt[pid]），后者需要重建查询的切分。
    # 而两者在效果上等价：被切成 k 块的长词，k 块会分别成为
    # k 个 token 同时出现在查询里；能同时命中这 k 个 token 的段落，
    # 就是那个词的段落。
    #
    # 用 cnt/nq 的**平方**而不是线性：k 块里命中 j 个，
    # 偶然命中的概率随 j 指数下降 —— 平方是对这个概率的粗略近似。
    if w_frag and nq >= 2:
        for pid in acc:
            acc[pid] += w_frag * (cnt[pid] / float(nq)) ** 2

    scored = []
    for pid, sc in acc.items():
        sc *= (cnt[pid] / float(nq)) ** 0.5
        if R.text(pid).startswith(("游戏概述", "游戏概要", "概述")):
            sc *= 0.35
        scored.append((pid, sc))
    scored.sort(key=lambda kv: -kv[1])
    return scored[:k]


CASES = G.CASES


def split_game(query):
    """游戏名剔除。**必须与 guide_search.split_game 同一套判据**。

    ⚠️ 漏掉这一步时我测出 9/12，而 guide_search 自己是 12/12 ——
    差的那 3 个全是「限定游戏」的用例。原因是：
      · 问句里的游戏名（BPE 切成碎片，df 很高）会压过真正的实体词；
      · 不剔除范围时，「怪物猎人4G 弱点」会检索到别的游戏去。
    → 也就是说 9/12 与 12/12 **不是两个算法的好坏**，
    而是「口径对不对」。测检索改进时口径必须先对齐。
    """
    best = None
    for gname, aliases in G.GAME_ALIAS.items():
        gid = G.GAME_ORDER.index(gname)
        for a in aliases:
            p = query.find(a)
            if p >= 0 and (best is None or len(a) > best[0]):
                best = (len(a), gid, p, a)
    if not best:
        return -1, query
    ln, gid, p, a = best
    return gid, (query[:p] + " " + query[p + ln:])


def main():
    kb, bpe = load()
    R = G.Retriever(kb, bpe, G.GAME_ORDER)
    print("段落 %d" % kb["n"])
    print()

    for w_frag in (0.0, 1.0, 2.0, 3.0, 5.0, 8.0):
        hits = 0
        bad = []
        for q, kw, exp in CASES:
            gid, rest = split_game(q)
            if exp is not None:              # 用例指定的游戏优先
                gid = G.GAME_ORDER.index(exp)
            res = search(kb, bpe, R, rest, gid, 4, w_frag)
            if res and kw in R.text(res[0][0]):
                hits += 1
            else:
                bad.append(q)
        print("w_frag=%.1f  %2d/12   %s"
              % (w_frag, hits, "" if hits == 12 else "miss: " + " ".join(bad)))

    print()
    print("对照 guide_search.py 自己的口径：")
    G._RETRIEVER = R
    ok = 0
    for q, kw, exp in CASES:
        gname, rest, hits = G.answer(q, force_game=exp)
        if hits and kw in R.text(hits[0][0]):
            ok += 1
    print("  guide_search 原版：%d/12" % ok)


if __name__ == "__main__":
    main()
