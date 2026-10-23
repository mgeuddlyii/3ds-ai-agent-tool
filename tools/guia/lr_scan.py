#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""
lr_scan.py — 学习率扫描：确认 loss 上升是不是 lr 过大造成的

怎么读这个表
------------
固定同一批训练数据、同一个随机种子，只改 lr。
每档都在**同一组固定序列**上评 loss（不是训练时那个随机 batch），
所以数字之间可比。

判据：
  - 某个 lr 的固定评估 loss 全程单调下降  -> 那个 lr 至少是安全的
  - 某个 lr 先降后升 -> 发散，说明它是峰值区
  - 最优 lr 明显低于 3e-3 -> 当前配置就是元凶
"""
import os
import sys
import time
import importlib.util

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

spec = importlib.util.spec_from_file_location("tg", os.path.join(HERE, "train_gpt.py"))
tg = importlib.util.module_from_spec(spec)
spec.loader.exec_module(tg)


def ce_on(model, seqs, B=32):
    """固定序列上的平均每 token 交叉熵。"""
    tot, ntok = 0.0, 0
    for i in range(0, len(seqs), B):
        chunk = seqs[i:i + B]
        ml = max(len(s) for s in chunk)
        xb = np.zeros((len(chunk), ml), np.int32)
        yb = np.full((len(chunk), ml), -100, np.int32)
        for r, s in enumerate(chunk):
            n = len(s)
            xb[r, :n - 1] = s[:n - 1]
            yb[r, :n - 1] = s[1:n]
        logits = np.asarray(model.forward(xb)["logits"], dtype=np.float64)
        mx = logits.max(-1, keepdims=True)
        lse = np.log(np.exp(logits - mx).sum(-1)) + mx[..., 0]
        safe = np.maximum(yb, 0)
        valid = yb >= 0
        picked = np.take_along_axis(logits, safe[..., None], axis=-1)[..., 0]
        tot += float(((lse - picked) * valid).sum())
        ntok += int(valid.sum())
    return tot / max(1, ntok)


def run(lr_peak, steps, seqs, probe, log):
    model = tg.GPT(tg.CFG)
    rng = np.random.default_rng(7)          # 各档完全相同的批次序列
    it = tg.make_batches(seqs, 16, rng)
    log("--- lr 峰值 %.1e ---" % lr_peak)
    row = []
    for step in range(1, steps + 1):
        xb, yb = next(it)
        save = model.forward(xb)
        logits = save["logits"]
        m = logits.shape[-1]
        mx = logits.max(-1, keepdims=True)
        lse = np.log(np.exp(logits - mx).sum(-1)) + mx[..., 0]
        safe = np.maximum(yb, 0)
        valid = yb >= 0
        picked = np.take_along_axis(logits, safe[..., None], axis=-1)[..., 0]
        loss = float(((lse - picked) * valid).sum() / max(1, int(valid.sum())))
        g, _ = model.backward(save, yb)
        # 固定 peak 的常数 lr（不做 warmup / cosine），
        # 这样各档之间的对比是干净的：要看的就是这个常数能不能稳住。
        model.adam(g, lr_peak)
        if step % 50 == 0 or step == 1:
            pv = ce_on(model, probe)
            row.append((step, pv))
            log("  step %4d  固定集 loss %7.4f  ppl %8.2f"
                % (step, pv, float(np.exp(min(20, pv)))))
    return row


def main():
    steps = int(sys.argv[1]) if len(sys.argv) > 1 else 300
    peaks = [float(x) for x in (sys.argv[2].split(",") if len(sys.argv) > 2
                                else ["3e-3", "1e-3", "5e-4", "2e-4"])]

    seqs, _ = tg.build_dataset()
    # 固定评估集：取前 96 条（顺序固定，不随机）
    probe = seqs[:96]
    logf = open(os.path.join(HERE, "ckpt", "lr_scan.log"), "w", encoding="utf-8")

    def log(s):
        print(s, flush=True)
        logf.write(s + "\n")
        logf.flush()

    log("扫描：%d 步 / batch 16 / 固定评估集 %d 条" % (steps, len(probe)))
    log("token/参数 = %.4f（Chinchilla 建议 20，差 %.0f 倍）"
        % (sum(len(s) for s in seqs) / (tg.CFG["vocab"] * tg.CFG["d_model"]
           + 6 * (4 * tg.CFG["d_model"] ** 2 + 2 * tg.CFG["d_model"] * tg.CFG["d_ff"])),
           20 * (tg.CFG["vocab"] * tg.CFG["d_model"]
           + 6 * (4 * tg.CFG["d_model"] ** 2 + 2 * tg.CFG["d_model"] * tg.CFG["d_ff"]))
           / sum(len(s) for s in seqs)))
    log("")

    allrows = {}
    for p in peaks:
        t0 = time.time()
        allrows[p] = run(p, steps, seqs, probe, log)
        log("  用时 %.1f 分钟" % ((time.time() - t0) / 60))
        log("")

    log("=== 汇总（固定评估集 loss，越低越好）===")
    pts = sorted({s for r in allrows.values() for s, _ in r})
    log("%8s" % "step" + "".join("%12.2e" % p for p in peaks))
    for s in pts:
        line = "%8d" % s
        for p in peaks:
            v = dict(allrows[p]).get(s)
            line += "%12s" % ("-" if v is None else "%.4f" % v)
        log(line)
    log("")
    log("最终固定集 loss：")
    for p in peaks:
        log("  lr %.1e -> %.4f" % (p, allrows[p][-1][1]))
    logf.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())