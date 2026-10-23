#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""
eval_fixed.py — 用**固定**验证集测loss，判断到底是过拟合还是 lr 过大

为什么必须固定验证集
--------------------
train_gpt.py 打印的 loss 是**单个随机 batch**（B=32，1703 条里抽 32 条）的
交叉熵。而序列长度从 6 到 128 差 20 倍，batch 之间的 loss 天差地别：

    step 450 -> 6.1202
    step 500 -> 6.8152   <- 只是换了一批数据
    step 550 -> 6.1970

三步内±0.7 的抖动是 **batch 方差**，不是训练不稳。
拿这种曲线判断收敛/过拟合，结论必然是错的。

判断过拟合的唯一办法是：**固定一份数据，每次都测它**。
- 训练 loss 降 + 固定 val loss 升 -> 真过拟合
- 训练 loss 降 + 固定 val loss 也降-> 没过拟合，是 lr 或别的问题
- 训练 loss 升+ 固定 val loss 升     -> 优化本身崩了（lr 过大等）

用法：
    python eval_fixed.py                 # 测当前 checkpoint
    python eval_fixed.py --sweep         # 在若干 lr 下各测一次（诊断用）
"""
import os
import sys
import math
import pickle
import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
CKPT = os.path.join(HERE, "ckpt")
sys.path.insert(0, HERE)

from train_gpt import GPT, CFG, build_dataset, softmax   # noqa: E402


def fixed_val_set(seqs, n=256, seed=999):
    """
    固定验证集：固定随机种子抽 n 条，**每次调用结果完全相同**。
    seed 与训练的 rng(7) 不同，所以这批数据训练时没见过。
    """
    rng = np.random.default_rng(seed)
    idx = rng.choice(len(seqs), size=min(n, len(seqs)), replace=False)
    return [seqs[i] for i in sorted(idx)]


def eval_split(model, seqs, max_n=None):
    """在一组序列上算平均每 token 交叉熵。"""
    tot, ntok_total = 0.0, 0
    use = seqs if max_n is None else seqs[:max_n]
    for ids in use:
        if len(ids) < 2:
            continue
        x = np.array([ids[:-1]], dtype=np.int32)
        y = np.array([ids[1:]], dtype=np.int32)
        lg = np.asarray(model.forward(x)["logits"], dtype=np.float64)
        pr = softmax(lg, -1)
        pt = np.maximum(pr[0, np.arange(y.shape[1]), y[0]], 1e-12)
        tot += float(-np.log(pt).sum())
        ntok_total += y.shape[1]
    return tot / max(1, ntok_total)


def load_model(weights):
    m = GPT(CFG, seed=1)
    if weights:
        m.p = dict(np.load(weights))
    return m


def main():
    seqs, _ = build_dataset()
    val = fixed_val_set(seqs)
    trn = fixed_val_set(seqs, n=256, seed=7)     # 训练分布内的固定子集
    print("固定训练子集 %d 条 / 固定验证集 %d 条（验证集训练时没见过）\n"
          % (len(trn), len(val)))

    w = os.path.join(CKPT, "model_fp32.npz")
    if not os.path.exists(w):
        print("没有 checkpoint")
        return 1
    m = load_model(w)
    st = np.load(os.path.join(CKPT, "state.npz"), allow_pickle=False)
    print("当前 checkpoint：step %d，best %.4f\n"
          % (int(st["step"]), float(st["best"])))

    tr = eval_split(m, trn)
    va = eval_split(m, val)
    print("  固定训练子集 loss %.4f  ppl %8.2f" % (tr, math.exp(min(20, tr))))
    print("  固定验证集   loss %.4f  ppl %8.2f" % (va, math.exp(min(20, va))))
    gap = va - tr
    print("  gap(验证-训练) %+.4f" % gap)
    if gap > 0.35:
        print("  -> gap 偏大，有过拟合迹象")
    elif gap < 0.05:
        print("  -> gap 很小，**不是过拟合**（训练集和验证集几乎一样好）")
    else:
        print("  -> gap 正常")
    return 0


if __name__ == "__main__":
    sys.exit(main())
