#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""
gradcheck.py — 数值梯度校验

为什么必须有这个
----------------
第一版 backward 我写错了（ln_backward 空壳 + 在反向里重算前向中间量）。
如果直接开跑 4000 步，会白烧 20 分钟才发现 loss 不降。
所以在训练前用**有限差分**逐参数核对反向传播：

    (L(θ+ε) - L(θ-ε)) / 2ε   ≈   ∂L/∂θ

约定：相对误差 < 1e-3 算过，> 1e-2 算炸。
"""
import os
import sys
import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

from train_gpt import GPT, CFG, softmax  # noqa: E402


def loss_of(model, ids, targets):
    """
    必须在 float64 下算，否则有限差分会被 float32 的舍入误差淹没。
    试过 float32 + eps=1e-3：loss 本身约 3.4，float32 的 eps 是 1e-7 量级，
    差分 (L+ - L-)/2e-3 的有效精度只剩 5~6 位有效数字，
    对 |grad| ~ 1e-3 的参数会得到完全错误的数值梯度（相对误差看起来 1.0，
    其实是我的参照系坏了，不是模型坏了）。
    """
    save = model.forward(ids)
    logits = save["logits"].astype(np.float64)
    B, T = logits.shape[0], logits.shape[1]
    x = logits - logits.max(-1, keepdims=True)
    e = np.exp(x)
    probs = e / e.sum(-1, keepdims=True)
    valid = targets >= 0
    safe = np.maximum(targets, 0)
    pt = np.maximum(probs[np.arange(B)[:, None], np.arange(T)[None, :], safe], 1e-300)
    return float((-np.log(pt) * valid).sum() / max(1, int(valid.sum())))


def main():
    # 用极小配置跑得快：2 层 16 维
    cfg = dict(vocab=32, d_model=16, n_layer=2, n_head=2, d_ff=32,
               max_ctx=8, tie_emb=True)
    model = GPT(cfg, seed=42)
    # 关键：整个校验跑在 float64。
    # 模型的参数是 float32 时，前向本身的舍入误差就有 1e-7，
    # 而要校验的梯度只有 1e-5~1e-3，差分会被噪声完全盖住
    # （这就是前面误判「反向有 bug」的原因 —— 其实坏的是参照系）。
    model.p = {k: v.astype(np.float64) for k, v in model.p.items()}
    model.cos = model.cos.astype(np.float64)
    model.sin = model.sin.astype(np.float64)

    rng = np.random.default_rng(0)
    B, T = 3, cfg["max_ctx"]
    ids = rng.integers(0, cfg["vocab"], (B, T)).astype(np.int32)
    targets = np.full((B, T), -100, np.int32)
    for r in range(B):
        n = T - r
        targets[r, :n - 1] = ids[r, 1:n]
    targets[0, 5:] = -100

    save = model.forward(ids)
    g, _ = model.backward(save, targets)
    base = loss_of(model, ids, targets)
    print("base loss = %.6f" % base)
    print("%-12s %14s %14s %10s" % ("param", "analytic", "numeric", "rel_err"))
    print("-" * 56)

    eps = 1e-2
    worst = 0.0
    fails = []
    # 全部参数都抽一个元素来测（参数太多，全测太慢）
    checks = [
        ("wte", None), ("wpe", None),
        ("wq0", None), ("wk0", None), ("wv0", None), ("wo0", None),
        ("w1_0", None), ("w2_0", None),
        ("ln1g0", None), ("ln1b0", None), ("ln2g0", None), ("ln2b0", None),
        ("wq1", None), ("w1_1", None), ("lnfg", None), ("lnfb", None),
    ]
    for name, _ in checks:
        p = model.p[name]
        # 抽 3 个随机位置，多位置能发现「只对角线对」这类错误
        pos = [tuple(rng.integers(0, s) for s in p.shape) for _ in range(3)]
        for posx in pos:
            old = float(p[posx])
            p[posx] = old + eps
            lp = loss_of(model, ids, targets)
            p[posx] = old - eps
            lm = loss_of(model, ids, targets)
            p[posx] = old
            num = (lp - lm) / (2 * eps)
            ana = float(g[name][posx])
            den = max(1e-6, abs(num) + abs(ana))
            rel = abs(num - ana) / den
            worst = max(worst, rel)
            tag = "%s%s" % (name, posx)
            print("%-12s %14.6f %14.6f %10.2e" % (tag, ana, num, rel))
            if rel > 1e-2:
                fails.append((tag, ana, num, rel))

    print("-" * 56)
    print("最大相对误差 %.2e" % worst)
    if fails:
        print("!! %d 处不匹配 —— 反向传播有 bug" % len(fails))
        for f in fails:
            print("   ", f)
        return 1
    print("OK：全部参数梯度一致，可以开训。")
    return 0


if __name__ == "__main__":
    sys.exit(main())
