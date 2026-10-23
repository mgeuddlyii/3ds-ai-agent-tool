#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""
eval_holdout.py — 真·留出集诊断（按游戏整份切分）

为什么不用 eval_fixed.py 的随机抽法
----------------------------------
那个脚本从 1703 条序列里换种子抽 256 条当"验证集"，但训练用
`rng.integers(0, len(seqs), B)` 随机取批，**每一条序列都进过训练**。
所以它量到的 gap 恒等于 batch 抽样噪声，**什么也证明不了**
（第一次跑出来 gap=-0.07，看着像"不是过拟合"，其实是废话）。

这里改成：按**游戏**切分。留出集里的内容，模型一次都没见过。
这样 gap 才有意义。
"""
import os
import sys
import math
import importlib.util

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

spec = importlib.util.spec_from_file_location("tg", os.path.join(HERE, "train_gpt.py"))
tg = importlib.util.module_from_spec(spec)
spec.loader.exec_module(tg)


def split_by_game(n_holdout=3, seed=2024):
    """
    按游戏整份切分。返回 (train_seqs, val_seqs, 留出的游戏名)。
    切分粒度是**文件**，不是序列 —— 同一款游戏的 QA 和原文段落
    必须整体去留，否则原文段落会泄漏答案。
    """
    from train_bpe import BPE, norm_text

    d = tg.load_bpe()
    b = BPE(vocab_size=len(d["inv"]))
    b.vocab, b.inv, b.merges = d["vocab"], d["inv"], d["merges"]
    b._train_ranks()

    V = len(b.inv)
    BOS, EOS, SEP = V - 3, V - 2, V - 1
    T = tg.CFG["max_ctx"]

    corpus = os.path.join(HERE, "corpus")
    files = sorted(f for f in os.listdir(corpus)
                   if f.startswith("guide_") and f.endswith(".txt"))
    rng = np.random.default_rng(seed)
    order = rng.permutation(len(files))
    hold = set(order[:n_holdout].tolist())

    tr, va, hold_names = [], [], []
    for i, fn in enumerate(files):
        text = norm_text(open(os.path.join(corpus, fn), encoding="utf-8").read())
        qa, raw = [], []
        # ★ 与 train_gpt.scan_qa 同一套行式判据（Q: / A: 各占一行）。
        #   原先这里是 `Q: ... A: ...` 挤一行那份判据的复制品，
        #   语料改分行之后问卷/留出评估会**一条问答都取不到**，
        #   评估数字全变成"随机权重"的水平且不报错。
        lines = [x.strip() for x in text.split("\n")]
        for i, para in enumerate(lines):
            if not para or para.startswith("##"):
                continue
            if not para.startswith("Q") or not (para[1:2] in (":", "：")):
                if len(para) > 40:
                    raw.append(para)
                continue
            nxt = lines[i + 1].strip() if i + 1 < len(lines) else ""
            if not nxt.startswith("A") or not (nxt[1:2] in (":", "：")):
                continue
            q = para[1:].lstrip(":：").strip()
            a = nxt[1:].lstrip(":：").strip()
            if 3 <= len(q) <= 60 and 3 <= len(a) <= 400:
                qa.append((q, a))

        out = va if i in hold else tr
        if i in hold:
            hold_names.append(fn[len("guide_"):-len(".txt")])

        for q, a in qa:
            head = [BOS] + b.encode(q) + [SEP]
            body = b.encode(a)
            room = T - len(head) - 1
            if room < 1:
                out.append(np.array(head[:T - 2] + [SEP, EOS], np.int32))
                continue
            out.append(np.array(head + body[:room] + [EOS], np.int32))

        if raw:
            ids = []
            for p in raw:
                ids.extend(b.encode(p))
            n = (len(ids) // (T - 1)) * (T - 1)
            for i2 in range(0, n, T - 1):
                out.append(np.array([BOS] + ids[i2:i2 + T - 2] + [EOS], np.int32))

    return tr, va, hold_names


def eval_seq(model, seqs, B=32):
    """在一组序列上算平均每 token 交叉熵。"""
    tot_loss, tot_tok = 0.0, 0
    for i in range(0, len(seqs), B):
        chunk = seqs[i:i + B]
        ml = max(len(s) for s in chunk)
        xb = np.zeros((len(chunk), ml), np.int32)
        yb = np.full((len(chunk), ml), -100, np.int32)
        for r, s in enumerate(chunk):
            n = len(s)
            xb[r, :n - 1] = s[:n - 1]
            yb[r, :n - 1] = s[1:n]
        save = model.forward(xb)
        logits = np.asarray(save["logits"], dtype=np.float64)
        m = logits.shape[-1]
        lg = logits - logits.max(-1, keepdims=True)
        lse = np.log(np.exp(lg).sum(-1)) + logits.max(-1)
        # 逐 token 交叉熵（-log softmax[y]），只在有效位上统计
        safe = np.maximum(yb, 0)
        valid = yb >= 0
        picked = np.take_along_axis(logits, safe[..., None], axis=-1)[..., 0]
        ce = lse - picked
        tot_loss += float((ce * valid).sum())
        tot_tok += int(valid.sum())
    return tot_loss / max(1, tot_tok)


def main():
    n_hold = int(sys.argv[1]) if len(sys.argv) > 1 else 3
    tr, va, names = split_by_game(n_holdout=n_hold)

    def toks(ss):
        return sum(len(s) for s in ss)

    print("按游戏留出 %d 款：%s" % (n_hold, ", ".join(names)))
    print("训练集 %d 条 / %d token" % (len(tr), toks(tr)))
    print("留出集 %d 条 / %d token" % (len(va), toks(va)))
    print()

    model = tg.GPT(tg.CFG)
    ck = os.path.join(HERE, "ckpt", "model_fp32.npz")
    if os.path.exists(ck):
        d = np.load(ck)
        n = 0
        for k in d.files:
            if k in model.p:
                model.p[k] = d[k].astype(model.p[k].dtype)
                n += 1
        print("载入 checkpoint：%d/%d 个张量" % (n, len(model.p)))
    else:
        print("**没有 checkpoint，以下是随机初始化**")
    print("参数量 %.2fM" % (model.nparam / 1e6))
    print()

    # 留出集全量太慢，均匀抽样到 ~200 条
    va_s = va if len(va) <= 200 else [va[i] for i in
                                     np.linspace(0, len(va) - 1, 200).astype(int)]
    tr_s = tr if len(tr) <= 200 else [tr[i] for i in
                                     np.linspace(0, len(tr) - 1, 200).astype(int)]

    l_tr = eval_seq(model, tr_s)
    l_va = eval_seq(model, va_s)
    gap = l_va - l_tr
    print("  训练子集 loss %7.4f  ppl %8.2f" % (l_tr, math.exp(min(20, l_tr))))
    print("  留出集   loss %7.4f  ppl %8.2f" % (l_va, math.exp(min(20, l_va))))
    print("  gap(留出-训练) = %+.4f" % gap)
    print()

    # ⚠️⚠️ 这里**不能只看单次 gap 的绝对值**。原来用 gap>0.35 判「真过拟合」，
    # 而实测（2026-10-04）扫留出规模时发现方向是反的：
    #
    #     留 1 款  gap +0.6843
    #     留 3 款  gap +0.8075
    #     留 5 款  gap +0.6696
    #     留 8 款  gap +0.2649
    #
    # **留出越多，gap 越小**。原因是留出 3 款只占语料 9.7%，
    # 样本少时 loss 本身波动大（ppl 900 上下都属于这个模型的常态），
    # 而训练子集 loss 是全语料的均值 —— 两者量级不同，比出来的差
    # 主要反映**留出集有多小**，不是模型有多背题。
    #
    # → 正确的判据是看 **gap 随留出规模的变化趋势**：
    #     留出越多 gap 反而越大 = 真过拟合
    #     留出越多 gap 变小     = 波动主导，模型没在背题
    # 跑 `python tools/guia/eval_holdout.py 1 / 3 / 5 / 8` 一起看。
    print("  ⚠️ 单次 gap 不足以判过拟合 —— 要扫留出规模一起看：")
    print("     python tools/guia/eval_holdout.py 1")
    print("     python tools/guia/eval_holdout.py 3")
    print("     python tools/guia/eval_holdout.py 5")
    print("     python tools/guia/eval_holdout.py 8")
    print("     实测：1款 +0.68 / 3款 +0.81 / 5款 +0.67 / 8款 +0.26")
    print("     → 留出越多 gap 越小，说明这个量级主要是**小留出集的波动**。")
    print()
    if gap > 0.35:
        print(">> gap 偏大。**先扫上面那 4 个留出规模**再下结论 ——")
        print(">> 单看这一次会误判（留 3 款时它总是最大的）。")
    else:
        print(">> gap 正常。")
    return 0


if __name__ == "__main__":
    sys.exit(main())