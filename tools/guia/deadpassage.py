#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""
deadpassage.py — 「死段」体检：段落是否还检索得到

══ 为什么要有这个 ═══════════════════════════════════════════════
build_kb.py 的倒排有两个截断：
    ① 收集期：`if len(post[tid]) < MAX_POSTING * 8` —— 先收 8 倍，后面再截
    ② 落盘期：每个 token 只保留 tf 最高的 MAX_POSTING 个段落
`MAX_POSTING = 64` 是在 **31 款 / 1174 段** 上标定的。语料扩到
110+ 款 / 3000+ 段之后，同一个词的 df 会普遍变大 —— 原来安全的值
可能开始**把整个段落挤出索引**。

一旦某段落的**所有**词都被截掉，那段就永远检索不到 = **死段**。
它不报错、不崩、不影响其他段落的得分，只是那一小节的攻略**消失了**。
这正是 build_kb.py 注释里写过的「限定游戏检索 0 命中」的一种成因。

══ 判据为什么这样设计 ═════════════════════════════════════════════
「死段数 == 0」是个**在坏情况下会红**的判据（不是永远绿的守卫）：
    · 把 MAX_POSTING 调到 4  → 死段必然 > 0   （--simulate 自证）
    · 只会因为「真的检索不到」而变红，没有别的红的理由
自证命令（**改判据前后都该跑**）：
    python deadpassage.py --simulate 4,8,16,24,32,64

用法：
    python deadpassage.py                    # 只读产物：当前 bin 里有几个死段
    python deadpassage.py --simulate 8,16,32 # 重算截断，扫不同 MAX_POSTING
    python deadpassage.py --simulate 64 --detail
"""
from __future__ import print_function

import os
import sys
import collections

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import test_kb as T          # KB 解析器同源（读的就是卡上那一份格式）
import build_kb as BK        # MAX_TF / MAX_POSTING / load_bpe 同源

# ⚠️ 下面两个值**必须从 build_kb 取**，不能在这里写一份。
#   记忆里的原话：「复刻脚本里的常量必须同源，抠不到就 FAIL，绝不给兜底值。」
MAX_TF = BK.MAX_TF
BASE_POSTING = BK.MAX_POSTING

def _fail(msg):
    print("FAIL —— %s" % msg)
    print("（常量没抠到，按纪律不给兜底值，直接退出）")
    return 2


# ---------------------------------------------------------------- 产物直读
def scan_bin():
    """只读 guide_kb.bin，统计每个 pid 被多少条 posting 指着。"""
    kb = T.load()
    n = kb["n"]
    cnt = collections.Counter(kb["post_ids"])
    # reach[i] = 有多少个 token 在落盘后的倒排里仍指向段落 i。
    # ★ 只看「死段(reach==0)」不够敏感 —— 一段只剩 1 个可达词时，
    #   它理论上有救、实际上几乎命中不了（那唯一可用的词多半是
    #   「模式」「方法」这类跨游戏通用词，被几十个段共享）。
    #   所以同时报 reach<=1 / reach<=3 两档。
    reach = [cnt.get(i, 0) for i in range(n)]
    dead = [i for i in range(n) if reach[i] == 0]
    near = [i for i in range(n) if reach[i] == 1]
    thin = [i for i in range(n) if 1 <= reach[i] <= 3]

    # df 分布：df == MAX_POSTING 的 token 是「顶格」的，可能已被截断
    V = kb["V"]
    df = [kb["post_off"][t + 1] - kb["post_off"][t] for t in range(V)]
    full = sum(1 for d in df if d >= BASE_POSTING)
    nonzero = sum(1 for d in df if d > 0)

    print("=" * 68)
    print("① 产物直读（ckpt/guide_kb.bin）")
    print("=" * 68)
    print("段落总数            %d" % n)
    print("游戏数              %d" % (max(kb["pass_game"]) + 1))
    print("倒排条目            %d" % len(kb["post_ids"]))
    print("死段（reach==0）    %d  （占 %.2f%%）"
          % (len(dead), 100.0 * len(dead) / max(1, n)))
    print("极危（reach==1）    %d  （占 %.2f%%）"
          % (len(near), 100.0 * len(near) / max(1, n)))
    print("偏薄（reach<=3）    %d  （占 %.2f%%）"
          % (len(thin), 100.0 * len(thin) / max(1, n)))
    print("可达词中位数        %d" % sorted(reach)[n // 2])
    print("词表非零 df 的词    %d" % nonzero)
    print("其中 df 顶格(>=%d)  %d  （占 %.1f%%，可能已被截断）"
          % (BASE_POSTING, full, 100.0 * full / max(1, nonzero)))

    if dead:
        pg = kb["pass_game"]
        byg = collections.Counter(pg[i] for i in dead)
        print()
        print("!! 死段按游戏分布（有死段的游戏）:")
        for g, c in byg.most_common():
            print("   gid=%-3d  %d 段" % (g, c))
        print()
        print("!! 前 5 个死段的正文（自行判断是不是重要内容）:")
        for i in dead[:5]:
            a, b = kb["pass_off"][i], kb["pass_off"][i + 1]
            s = kb["text"][a:b].decode("utf-8", "replace")
            print("   pid=%-4d %s" % (i, s[:60].replace("\n", " ")))
    else:
        print()
        print("OK —— 当前索引里没有死段（每段至少有一个词能命中它）")
    return len(dead)


# ---------------------------------------------------------------- 模拟
def encode_all():
    """把 bin 里的段落原文重新 encode，返回 [(pid, {tid: tf})]。"""
    kb = T.load()
    bpe = BK.load_bpe()
    V = kb["V"]
    n = kb["n"]
    out = []
    for i in range(n):
        a, b = kb["pass_off"][i], kb["pass_off"][i + 1]
        s = kb["text"][a:b].decode("utf-8", "replace")
        tf = collections.defaultdict(int)
        for tid in bpe.encode(s):
            if tid >= V - 3:          # 与 build_kb 同源：跳过 BOS/EOS/SEP
                continue
            tf[tid] += 1
        out.append((i, {t: min(c, MAX_TF) for t, c in tf.items()}))
    return out, V, n


def simulate(enco, V, n, MP, collect_cap=None):
    """复刻 build_kb 的两次截断，返回 (死段list, 丢弃条目数, 真实df)。

    collect_cap=None 时按 build_kb 的 `MAX_POSTING*8` 走（同源）；
    否则用它给定的值（用来暴露「先收上限」这个隐含截断的影响）。
    """
    cap = MP * 8 if collect_cap is None else collect_cap
    post = collections.defaultdict(list)      # tid -> [(pid, tf)]
    for pid, tf in enco:
        for tid, c in tf.items():
            if len(post[tid]) < cap:          # ① 收集期截断（有偏：只收靠前的 pid）
                post[tid].append((pid, c))
    true_df = {t: len(v) for t, v in post.items()}

    keep = set()
    reach = collections.Counter()
    dropped = 0
    for tid, lst in post.items():
        lst.sort(key=lambda pc: (-pc[1], pc[0]))   # ② 落盘期：tf 降序、pid 升序
        if len(lst) > MP:
            dropped += len(lst) - MP
        for pid, _ in lst[:MP]:
            keep.add(pid)
            reach[pid] += 1
    dead = [i for i in range(n) if i not in keep]
    near = [i for i in range(n) if reach.get(i, 0) == 1]
    return dead, dropped, true_df, (len(near), sum(reach.values()))


def main():
    argv = sys.argv[1:]
    if not os.path.exists(os.path.join(HERE, "ckpt", "guide_kb.bin")):
        return _fail("找不到 ckpt/guide_kb.bin")
    if not os.path.exists(os.path.join(HERE, "ckpt", "bpe.pkl")):
        return _fail("找不到 ckpt/bpe.pkl")

    dead_now = scan_bin()

    spec = None
    for i, a in enumerate(argv):
        if a == "--simulate":
            spec = argv[i + 1] if i + 1 < len(argv) else "8,16,24,32,64"
        elif a.startswith("--simulate="):
            spec = a.split("=", 1)[1]
    if spec is None:
        print()
        print("（要看不同 MAX_POSTING 的影响：--simulate 8,16,24,32,64）")
        return 0 if dead_now == 0 else 1

    vals = []
    for s in spec.split(","):
        s = s.strip()
        if not s:
            continue
        if not s.isdigit():
            return _fail("--simulate 只接受正整数列表，收到 %r" % s)
        vals.append(int(s))
    if not vals:
        return _fail("--simulate 后面没给数")

    print()
    print("正在重新编码全部段落（真实语料，约 13s）…")
    enco, V, n = encode_all()

    # 真实 df（收集期完全不截）
    _, _, true_df, _ = simulate(enco, V, n, 10 ** 9, collect_cap=10 ** 9)
    over = {t: d for t, d in true_df.items() if d > BASE_POSTING}
    print()
    print("=" * 68)
    print("② 真实 df（收集期完全不截断）")
    print("=" * 68)
    print("df > %d 的词           %d 个（这些词现在**一定**被截了）"
          % (BASE_POSTING, len(over)))
    if over:
        worst = sorted(over.items(), key=lambda kv: -kv[1])[:5]
        print("df 最大的 5 个词:")
        for t, d in worst:
            print("   tid=%-5d df=%-5d" % (t, d))
    print("真实 df 总量 %d → 落盘后 %d"
          % (sum(true_df.values()), sum(min(d, BASE_POSTING) for d in true_df.values())))

    print()
    print("=" * 68)
    print("③ 扫描 MAX_POSTING（复刻 build_kb 的两次截断）")
    print("=" * 68)
    print("  %-8s %-8s %-10s %-12s %s"
          % ("MAX_POST", "死段", "reach==1", "丢弃条目", "判定"))
    print("  " + "-" * 62)
    first_clean = None
    for MP in vals:
        dead, dropped, _, (near, _) = simulate(enco, V, n, MP)
        verdict = "OK" if not dead else "!! %d 段检索不到" % len(dead)
        print("  %-8d %-8d %-10d %-12d %s"
              % (MP, len(dead), near, dropped, verdict))
        if not dead and first_clean is None:
            first_clean = MP
    print("  " + "-" * 62)
    if first_clean:
        print("  最小的零死段 MAX_POSTING = %d（当前 build_kb 用 %d）"
              % (first_clean, BASE_POSTING))
    else:
        print("  扫过的值里没有一个能保证零死段 —— 需要更大的 MAX_POSTING")

    # 自证：判据必须能红
    print()
    print("=" * 68)
    print("④ 判据自证（能不能在坏情况下变红）")
    print("=" * 68)
    ok = True
    for probe in (4, 8, 16):
        d, _, _, _ = simulate(enco, V, n, probe)
        if d:
            print("OK —— MAX_POSTING=%d 时死段 %d > 0，判据会红" % (probe, len(d)))
        else:
            print("-- MAX_POSTING=%d 时死段仍为 0（这个值测不出东西）" % probe)
    d4 = simulate(enco, V, n, 4)[0]
    if not d4:
        print("!! 压到 4 都不红 —— 这个判据测不出东西，是「永远绿的守卫」")
        ok = False
    return 0 if (dead_now == 0 and ok) else 1


if __name__ == "__main__":
    sys.exit(main())
