#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""
gen_test.py — 拿现有 ckpt 做贪心/采样生成，检验权重质量

用法：
    python gen_test.py              # 跑默认 6 条问句
    python gen_test.py --temp 0.9   # 提高多样性
    python gen_test.py --bench      # 只算验证集 loss（不生成）
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


def load_bpe():
    """bpe.pkl 存的是 state dict（vocab/inv/merges/vocab_size），这里还原成 BPE 对象。"""
    from train_bpe import BPE
    with open(os.path.join(CKPT, "bpe.pkl"), "rb") as f:
        st = pickle.load(f)
    b = BPE(vocab_size=st["vocab_size"], verbose=False)
    b.vocab = st["vocab"]
    b.inv = st["inv"]
    b.merges = st["merges"]
    b.vocab_size = st["vocab_size"]
    # _ranks 是 encode 的热路径查找表，必须由 merges 重建
    b._ranks = {tuple(m): i for i, m in enumerate(b.merges)}
    return b


def load_special():
    with open(os.path.join(CKPT, "special.txt")) as f:
        a, b, c = [int(x) for x in f.read().split()]
    return a, b, c


def generate(model, bpe, bos, sep, eos, prompt, max_new=60,
             temp=0.7, topk=8, rng=None):
    """单条生成。prompt 是问句（不含 <BOS>/<SEP>）。"""
    ids = [bos] + bpe.encode(prompt) + [sep]
    ctx = np.array([ids], dtype=np.int32)
    out = []
    pos = len(ids) - 1          # 下一个 token 的位置
    cur = np.array([[ids[-1]]], dtype=np.int32)

    for _ in range(max_new):
        save = model.forward(cur, pos0=pos)
        logits = np.array(save["logits"][0, -1], dtype=np.float32)
        if temp <= 1e-6:
            nxt = int(np.argmax(logits))
        else:
            lg = logits / temp
            if topk > 0:
                k = min(topk, lg.size)
                idx = np.argpartition(-lg, k - 1)[:k]
                m = np.full_like(lg, -1e30)
                m[idx] = lg[idx]
                lg = m
            p = np.exp(lg - lg.max())
            p /= p.sum()
            nxt = int(rng.choice(len(p), p=p)) if rng else int(np.argmax(p))
        if nxt == eos:
            break
        out.append(nxt)
        cur = np.array([[nxt]], dtype=np.int32)
        pos += 1
        if pos >= model.c["max_ctx"]:
            break
    return bpe.decode(out), out


QUERIES = [
    "火焰纹章觉醒 怎么提升好感",
    "沙之杖怎么用",
    "怪物猎人4G 弱点是什么",
    "马力欧卡丁车7 怎么漂移",
    "宝可梦太阳 初始精灵选哪个",
    "塞尔达时之笛3D 大师之剑在哪",
]


def main():
    npz = os.path.join(CKPT, "model_fp32.npz")
    if not os.path.exists(npz):
        print("没有权重，先跑 train_gpt.py", file=sys.stderr)
        return 1
    p = dict(np.load(npz))
    model = GPT(CFG)
    model.p = p
    print("载入权重 OK，%d 个张量" % len(p))

    bpe = load_bpe()
    bos, eos, sep = load_special()
    print("special: BOS=%d EOS=%d SEP=%d" % (bos, eos, sep))

    # ---- 基准 loss（训练集上，权重的直接质量指标）----
    seqs, _ = build_dataset()
    tot, n = 0.0, 0
    for ids in seqs[:200]:
        x = np.array([ids[:-1]], dtype=np.int32)
        y = np.array([ids[1:]], dtype=np.int32)
        if x.shape[1] == 0:
            continue
        lg = model.forward(x)["logits"]
        pr = softmax(np.array(lg, dtype=np.float64), -1)
        pt = np.maximum(pr[0, np.arange(y.shape[1]), y[0]], 1e-9)
        tot += float(-np.log(pt).mean())
        n += 1
    avg = tot / max(1, n)
    print("训练集 loss %.4f  ppl %.2f  （%d 条）"
          % (avg, math.exp(min(20, avg)), n))
    print("-" * 60)

    temp = 0.7
    if "--temp" in sys.argv:
        temp = float(sys.argv[sys.argv.index("--temp") + 1])
    rng = np.random.default_rng(0)
    for q in QUERIES:
        txt, ids = generate(model, bpe, bos, sep, eos, q,
                            max_new=50, temp=temp, rng=rng)
        print("Q: %s" % q)
        print("A: %s" % txt)
        print("   (%d token)" % len(ids))
        print("-" * 60)
    return 0


if __name__ == "__main__":
    sys.exit(main())