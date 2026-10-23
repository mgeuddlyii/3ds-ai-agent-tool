#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""
verify_vocab.py — 断言「卡上内嵌的 BPE 词表」与「索引/模型用的词表」是同一代

═══ 为什么这个脚本必须存在 ═══════════════════════════════════════════
2026-10-04 踩的坑（本项目"各写各的表"系列的第 4 次）：

    app/data/guide_vocab.txt   10-03 20:19 导出   ← 内嵌进 .3dsx
    tools/guia/ckpt/bpe.pkl   10-03 22:51 训练   ← guide_kb.bin / 模型用这份

  BPE 重训过一次（22:51 那次），但**没人重新跑export_vocab.py**。
  于是卡上跑的是**旧一代词表**，而 guide_kb.bin 里的倒排表是按**新一代**
  的 token id 建的 —— 4096 个 id 里 3815 个对不上。

  症状极其恶劣：
    · 2 条用例「检索 0 命中」（token 编出来了，post_off 查空）
    · 4 条用例**命中了错误的段落**（比0 命中更隐蔽 —— 有输出，看着像答案）
  全程**不报错、不崩、退出码正常**。

  ★ 关键教训：id 是**位置**，不是内容。两份表"看起来一样"（都是 4096 个
    token、集合甚至完全不相交），但只要 id 错位，检索就全错。
    所以校验必须比 **id -> bytes 的逐条映射**，不能只比 V 和 merges 数。

用法：
    python verify_vocab.py           # 比对，不一致就 exit 1
    python verify_vocab.py --fix     # 不一致就自动跑 export_vocab.py 再比对
"""
from __future__ import print_function

import os
import pickle
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
PROJ = os.path.abspath(os.path.join(HERE, "..", ".."))
CARD_VOCAB = os.path.join(PROJ, "app", "data", "guide_vocab.txt")
BPE_PKL = os.path.join(HERE, "ckpt", "bpe.pkl")
KB_BIN = os.path.join(HERE, "ckpt", "guide_kb.bin")


def load_card(path=None):
    """解析 GVB1 文本 -> list[bytes]，按 id 顺序。格式见 export_vocab.py。

    ⚠️ `path=None` 而不是 `path=CARD_VOCAB`：默认参数在**函数定义时**就把值
    绑死了，之后改模块变量 `V.CARD_VOCAB = ...` 不起作用 ——
    我做故障注入时就是这么被骗的：判据明明该红，它却报了 OK。
    「永远绿的守卫比没有更坏」这条铁律，连校验脚本自己也照样踩。
    """
    if path is None:
        path = CARD_VOCAB
    lines = open(path, encoding="ascii").read().split("\n")
    if lines[0] != "GVB1":
        raise ValueError("magic 不对：%r" % lines[0])
    hdr = lines[1].split()
    V, M, bos, eos, sep = (int(x) for x in hdr[:5])
    inv, p = [], 2
    for i in range(V):
        parts = lines[p].split()
        n, raw = int(parts[0]), bytes.fromhex(parts[1])
        if len(raw) != n:
            raise ValueError("第 %d 行：声明 %d 字节实际 %d" % (p, n, len(raw)))
        inv.append(raw)
        p += 1
    merges = []
    for _ in range(M):
        a, b = lines[p].split()
        merges.append((int(a), int(b)))
        p += 1
    return inv, merges, V, M, bos, eos, sep


def load_pkl(path=BPE_PKL):
    d = pickle.load(open(path, "rb"))
    inv = d["inv"]
    # ★ bpe.pkl 里的 inv 是 **{id: bytes}**（BPE 类内部用的字典），
    #   而 export_vocab.py 写出去的是按 id 排序的 list。
    #   两种形态都可能出现 —— 归一化成 list，否则会自己把自己骗过去。
    if isinstance(inv, dict):
        inv = [inv[i] for i in range(len(inv))]
    return list(inv), d["merges"]


def kb_vocab_size(path=KB_BIN):
    """索引头里的 V（第 3 个 int32）。它必须等于词表大小 —— 不等就是
    又一份"各写各的"。"""
    import struct
    with open(path, "rb") as f:
        blob = f.read(20)
    if blob[:4] != b"GKB1":
        raise ValueError("索引 magic 不对：%r" % blob[:4])
    n, n_game, V, text_len = struct.unpack_from("<4i", blob, 4)
    return n, n_game, V, text_len


def main():
    fix = "--fix" in sys.argv

    p_inv, p_merges = load_pkl()
    c_inv, c_merges, cV, cM, bos, eos, sep = load_card()
    n, n_game, kbV, text_len = kb_vocab_size()

    print("bpe.pkl        V=%-6d merges=%d" % (len(p_inv), len(p_merges)))
    print("guide_vocab.txt V=%-6d merges=%d  (BOS=%d EOS=%d SEP=%d)"
          % (cV, cM, bos, eos, sep))
    print("guide_kb.bin    段落=%d 游戏=%d V=%d" % (n, n_game, kbV))
    print()

    bad = []

    # ---- 判据 1：索引的 V 必须同时等于两份词表的大小 ----
    # 少了这条，词表换代而索引没重建时V 相同，检查会误判为一致。
    if not (kbV == len(p_inv) == cV):
        bad.append("V 三方不一致：索引=%d bpe.pkl=%d 卡上=%d" % (kbV, len(p_inv), cV))

    # ---- 判据 2：merges 条数 ----
    if len(p_merges) != cM:
        bad.append("merges 条数不一致：bpe.pkl=%d 卡上=%d" % (len(p_merges), cM))

    # ---- 判据 3（★核心）：id -> bytes 逐条映射 ----
    # 只比 V / merges 数会漏掉这次的真故障。必须逐条比。
    mism = [i for i in range(min(len(p_inv), cV)) if p_inv[i] != c_inv[i]]
    if mism:
        bad.append("词表 id 映射错位：%d/%d 个 id 对不上" % (len(mism), min(len(p_inv), cV)))
        print("!! 词表 id 映射错位：%d/%d 个 id 对不上" % (len(mism), min(len(p_inv), cV)))
        print("   前 10 个错位的 id：")
        for i in mism[:10]:
            print("     id=%-5d bpe.pkl=%-14r 卡上=%r" % (i, p_inv[i], c_inv[i]))
        # 顺带给出「集合是否相同」—— 区分「重训了」与「纯错位」
        s_p, s_c = set(p_inv), set(c_inv)
        print("   token 集合是否相同：%s（bpe.pkl 独有 %d / 卡上独有 %d）"
              % (s_p == s_c, len(s_p - s_c), len(s_c - s_p)))
        if s_p == s_c:
            print("   → 集合相同但 id 不同 = **纯顺序错位**，"
                  "通常是重训 BPE 后没重新跑 export_vocab.py")
        else:
            print("   → 集合本身不同 = **BPE 又训过一代**，"
                  "必须重新导出，且索引/模型也要跟着重建")
        print()

    # ---- 判据 4：BOS/EOS/SEP 位置 ----
    if (bos, eos, sep) != (cV - 3, cV - 2, cV - 1):
        bad.append("特殊 token 位置不对：应为 %d/%d/%d"
                   % (cV - 3, cV - 2, cV - 1))

    if bad:
        print("FAIL —— %d 项不一致：" % len(bad))
        for b in bad:
            print("   · " + b)
        if fix:
            print()
            print("--fix：重新导出 ...")
            subprocess.check_call([sys.executable,
                                   os.path.join(HERE, "export_vocab.py")])
            print("已重新导出，请再跑一次本脚本确认。")
        return 1

    print("OK —— 卡上词表 / 索引 / bpe.pkl 三方一致（%d 个 token 逐条对齐）" % cV)
    return 0


if __name__ == "__main__":
    sys.exit(main())
