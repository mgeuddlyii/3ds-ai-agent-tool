#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""
watch.py — 训练进度观察

用法：
    python watch.py          # 打印最后 15 行 + 当前 checkpoint 的生成样例
    python watch.py --quiet  # 只打印进度，不做生成（生成很慢）

为什么要这个
------------
上次训练 22 分钟一个字都看不到（`| tail -60` 把输出全攒在管道缓冲区），
被误判成卡死。现在 train_gpt.py 已经**直接写ckpt/train.log 并 fsync**，
这个脚本只负责读取，不再经过管道。
"""
import os
import sys
import math
import pickle
import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
CKPT = os.path.join(HERE, "ckpt")
sys.path.insert(0, HERE)


def tail(n=15):
    p = os.path.join(CKPT, "train.log")
    if not os.path.exists(p):
        print("还没有train.log，训练还没启动")
        return
    with open(p, encoding="utf-8") as f:
        lines = [x.rstrip() for x in f if x.strip()]
    print("--- 进度 ---")
    for l in lines[-n:]:
        print("  " + l)


def sample():
    from train_gpt import GPT, CFG
    from gen_test import load_bpe, load_special, generate
    p = os.path.join(CKPT, "model_fp32.npz")
    if not os.path.exists(p):
        print("还没有 checkpoint")
        return
    d = dict(np.load(p))
    m = GPT(CFG, seed=1)
    m.p = d
    bpe = load_bpe()
    bos, eos, sep = load_special()
    rng = np.random.default_rng(0)
    print("--- 当前 checkpoint 的生成样例 ---")
    for q in ("火焰纹章觉醒 转职怎么转", "沙之杖怎么用",
              "马力欧卡丁车7 漂移技巧", "宝可梦黑 御三家选哪个"):
        txt, ids = generate(m, bpe, bos, sep, eos, q,
                            max_new=32, temp=0.6, rng=rng)
        print("  Q: %s" % q)
        print("  A: %s" % txt[:70])
    # 训练集 loss
    from train_gpt import build_dataset, softmax
    seqs, _ = build_dataset()
    tot, n = 0.0, 0
    for ids2 in seqs[:120]:
        x = np.array([ids2[:-1]], dtype=np.int32)
        y = np.array([ids2[1:]], dtype=np.int32)
        if x.shape[1] == 0:
            continue
        lg = np.asarray(m.forward(x)["logits"], dtype=np.float64)
        pr = softmax(lg, -1)
        pt = np.maximum(pr[0, np.arange(y.shape[1]), y[0]], 1e-9)
        tot += float(-np.log(pt).mean())
        n += 1
    if n:
        avg = tot / n
        print("--- 训练集 loss %.4f  ppl %.2f  (%d 条) ---"
              % (avg, math.exp(min(20, avg)), n))


if __name__ == "__main__":
    tail()
    if "--quiet" not in sys.argv:
        print()
        sample()
