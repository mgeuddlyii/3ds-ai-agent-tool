#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
probe_activ.py — 量出前向每一步中间张量的真实量级

    python tools/guia/probe_activ.py

═══ 为什么需要它 ═══════════════════════════════════════════════════

发现 checkpoint 里 wq/wk 的 absmax 只有 1.9e-5，而 wv/wo/w1/w2 是
1e-2 量级 —— 差三个数量级（训练时权重衰减把 q/k 压得很小，
这是训练结果本身，不是 bug）。

那么 C 端 `quant_one(matvec_f32(...))` 之后：
    dot(inbuf, wq_row) ~ 128 * 5 * 127 * 0.5 ≈ 4e4
    乘 wq 的 scale 1.5e-7  ->  6e-3
    quant_one(6e-3) -> int8(0.006) -> **0**

也就是说 **q 和 k 会被整体清零**，注意力退化成均匀分布，
位置信息全部丢失。程序照样跑、照样出字、零告警。

本脚本把每一步的 min/max/rms 打出来，用来回答两个问题：
  1. 哪些中间量在 int8 的动态范围内，哪些不在
  2. 不在的那些，缺一个什么因子才能落进 int8

判据是**量级**，不是「跑通了」—— 前者能预测，后者只说明没崩。
"""
import os
import sys

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
PROJ = os.path.abspath(os.path.join(HERE, "..", ".."))
NPZ = os.path.join(HERE, "ckpt", "model_fp32.npz")


def layernorm(x, g, b, eps=1e-5):
    mu = x.mean()
    return (x - mu) / np.sqrt(x.var() + eps) * g + b


def gelu(x):
    return 0.5 * x * (1.0 + np.tanh(0.7978845608 * (x + 0.044715 * x ** 3)))


def rep(name, a):
    a = np.asarray(a, np.float64).ravel()
    amax = np.abs(a).max()
    # int8 直接量化（隐含 scale 1）会保留多少信息
    q = np.clip(np.trunc(np.where(a >= 0, a + 0.5, a - 0.5)), -128, 127)
    kept = np.count_nonzero(q) / a.size
    print("  %-22s absmax=%-12.4g rms=%-12.4g  直接int8后非零率=%5.1f%%  %s"
          % (name, amax, np.sqrt((a * a).mean()), 100.0 * kept,
             "★ 会被清零" if kept < 0.01 else ("△ 损失大" if kept < 0.5 else "")))
    return amax


def main():
    if not os.path.exists(NPZ):
        sys.exit("找不到 %s" % NPZ)
    p = dict(np.load(NPZ))
    V, D = p["wte"].shape
    T = p["wpe"].shape[0]
    ln1 = sorted(int(k[4:]) for k in p if k.startswith("ln1g"))
    L = len(ln1)
    F = p["w1_0"].shape[0]
    H = 4
    dh = D // H
    print("模型 V=%d D=%d L=%d H=%d F=%d T=%d dh=%d" % (V, D, L, H, F, T, dh))
    print()

    ids = [1, 300, 700, 1200]
    x = p["wte"][ids[0]] + p["wpe"][0]
    print("=== 残差流 ===")
    rep("x0 = wte+wpe", x)

    worst = {"q": 0.0, "k": 0.0, "v": 0.0, "ctx": 0.0, "ff1": 0.0}
    for l in range(L):
        print()
        print("=== 第 %d 层 ===" % l)
        h1 = layernorm(x, p[f"ln1g{l}"], p[f"ln1b{l}"])
        rep("h1 = LN(x)", h1)
        h1q = np.clip(np.trunc(np.where(h1 >= 0, h1 + 0.5, h1 - 0.5)), -128, 127)

        q = h1 @ p[f"wq{l}"].T
        k = h1 @ p[f"wk{l}"].T
        v = h1 @ p[f"wv{l}"].T
        rep("q = h1@wq.T", q)
        rep("k = h1@wk.T", k)
        rep("v = h1@wv.T", v)
        worst["q"] = max(worst["q"], np.abs(q).max())
        worst["k"] = max(worst["k"], np.abs(k).max())
        worst["v"] = max(worst["v"], np.abs(v).max())

        ctx = v                       # pos=0 时注意力退化为取自己
        rep("ctx (=v, pos=0)", ctx)
        x1 = x + ctx @ p[f"wo{l}"].T
        h2 = layernorm(x1, p[f"ln2g{l}"], p[f"ln2b{l}"])
        rep("h2 = LN(x1)", h2)
        f1 = gelu(h2 @ p[f"w1_{l}"].T)
        rep("ff1 = gelu(...)", f1)
        worst["ff1"] = max(worst["ff1"], np.abs(f1).max())
        x = x1 + f1 @ p[f"w2_{l}"].T

    print()
    print("=== 全程峰值 ===")
    for kk, v_ in worst.items():
        print("  %-6s absmax = %.4g" % (kk, v_))
    print()
    print("结论：")
    for kk, v_ in sorted(worst.items(), key=lambda t: t[1]):
        need = v_ / 127.0
        if need < 1.0 / 127.0:
            print("  %-4s 量级 %.3g -> 必须**逐张量重标定**（乘 %.4g 才能用满 int8）"
                  % (kk, v_, need))
        elif v_ > 127:
            print("  %-4s 量级 %.3g -> **溢出 int8**，必须除一个因子" % (kk, v_))
        else:
            print("  %-4s 量级 %.3g -> 落在 int8 动态范围内，但只用到 %.1f%% 的码位"
                  % (kk, v_, 100.0 * v_ / 127.0))
    return 0


if __name__ == "__main__":
    sys.exit(main())
