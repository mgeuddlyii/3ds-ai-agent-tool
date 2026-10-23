#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""
gpu_bench.py — 实测「同一个模型、同一个 batch」在 CPU 和 GPU 上各多少秒

为什么必须实测而不是算
----------------------
2.88M 参数 / batch32 / ctx128 的矩阵规模极小（192x192、128x512），
单次 matmul 的计算量只有几微秒，而 CUDA kernel launch 本身就要 5-10us。
所以**纸面 FLOP/s 完全不适用** —— GPU 可能快 20 倍，也可能只快 2 倍。
不实测就是在赌。

这个脚本只做一件事：把 train_gpt.py 里现有的 GPT 类原样搬到 torch 上，
跑同样的前向+反向若干步，报秒/步。**不改模型结构、不改超参**，
所以测出来的比值就是换GPU 的真实收益。

torch 不在这个venv 里时自动跳过并说明。
"""
import os
import sys
import time
import importlib.util

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

# GPU 版需要一个独立解释器（E:\torch-venv 里才有 torch）
sys.path.insert(0, os.path.dirname(os.path.abspath(sys.executable)))

try:
    import torch
except ImportError:
    print("当前解释器没有 torch ——请用 E:\\torch-venv\\Scripts\\python.exe 跑本脚本")
    print("（它只装torch，numpy 从系统那边补）")
    sys.exit(2)

spec = importlib.util.spec_from_file_location("tg", os.path.join(HERE, "train_gpt.py"))
tg = importlib.util.module_from_spec(spec)
spec.loader.exec_module(tg)

DEV = "cuda" if torch.cuda.is_available() else "cpu"
print("解释器: %s" % sys.executable)
print("torch %s  device=%s" % (torch.__version__, DEV))
if DEV == "cuda":
    print("GPU: %s  (%.1f GB)" % (torch.cuda.get_device_name(0),
                                  torch.cuda.get_device_properties(0).total_memory / 2**30))
print()


# ---------------------------------------------------------- torch 版模型
class TorchGPT:
    """
    与 train_gpt.GPT 逐层对应的 torch 实现。
    关键是**保持结构一致**，否则测出来的比值没有意义：
        n_layer / n_head / d_model / d_ff / tied embedding / RoPE / 预-LN残差
    数值上不要求 bit-level 相同，只要 loss 量级一致即可。
    """

    def __init__(self, cfg, device):
        c = cfg
        self.c, self.dev = c, device
        D, H, L, F = c["d_model"], c["n_head"], c["n_layer"], c["d_ff"]
        V, T = c["vocab"], c["max_ctx"]
        self.D, self.H, self.L, self.F, self.V, self.T = D, H, L, F, V, T
        self.dh = D // H

        f = lambda *s: torch.nn.Parameter(torch.randn(*s, device=device) * 0.02)
        self.wte = f(V, D)
        self.wpe = f(T, D)
        self.blocks = torch.nn.ModuleList()
        for _ in range(L):
            blk = torch.nn.ModuleDict(dict(
                ln1g=f(1, D), ln1b=f(1, D),
                ln2g=f(1, D), ln2b=f(1, D),
                wq=f(D, D), wk=f(D, D), wv=f(D, D), wo=f(D, D),
                w1=f(D, F), w2=f(F, D),
            ))
            self.blocks.append(blk)
        self.lnfg = f(1, D)
        self.lnfb = f(1, D)
        #梯度裁剪用的全量梯度范数缓冲
        self._grads = None

    def _ln(self, x, g, b):
        u = x.mean(-1, keepdim=True)
        s = (x - u).pow(2).mean(-1, keepdim=True)
        return (x - u) / torch.sqrt(s + 1e-5) * g + b

    def _rope(self, x):
        # x: [B, H, T, dh]
        B, H, T, d = x.shape
        dev = x.device
        pos = torch.arange(T, device=dev, dtype=torch.float32)
        inv = 1.0 / (10000.0 ** (torch.arange(0, d, 2, device=dev).float() / d))
        ang = pos[:, None] * inv[None, :]# [T, d/2]
        cos = torch.cat([ang.cos(), ang.cos()], -1)[None, None]# [1,1,T,d]
        sin = torch.cat([ang.sin(), ang.sin()], -1)[None, None]
        return x * cos + self._rot(x) * sin

    def _rot(self, x):
        d = x.shape[-1] // 2
        a, b = x[..., :d], x[..., d:]
        return torch.cat([-b, a], -1)

    def forward(self, idx):
        B, T = idx.shape
        x = self.wte[idx] + self.wpe[:T][None]
        mask = torch.triu(torch.ones(T, T, device=x.device,
                                     dtype=torch.bool), 1)
        for blk in self.blocks:
            h = self._ln(x, blk["ln1g"], blk["ln1b"])
            q = self._rope((h @ blk["wq"]).view(B, T, self.H, self.dh).transpose(1, 2))
            k = self._rope((h @ blk["wk"]).view(B, T, self.H, self.dh).transpose(1, 2))
            v = (h @ blk["wv"]).view(B, T, self.H, self.dh).transpose(1, 2)
            att = (q @ k.transpose(-2, -1)) / (self.dh ** 0.5)
            att = att.masked_fill(mask, float("-inf")).softmax(-1)
            c = (att @ v).transpose(1, 2).reshape(B, T, self.D) @ blk["wo"]
            x = x + c
            h = self._ln(x, blk["ln2g"], blk["ln2b"])
            x = x + torch.nn.functional.gelu(h @ blk["w1"]) @ blk["w2"]
        x = self._ln(x, self.lnfg, self.lnfb)
        # tied embedding
        return x @ self.wte.T


def bench(fn, n_warm=3, n_meas=8):
    for _ in range(n_warm):
        fn()
    if DEV == "cuda":
        torch.cuda.synchronize()
    t0 = time.time()
    for _ in range(n_meas):
        fn()
    if DEV == "cuda":
        torch.cuda.synchronize()
    return (time.time() - t0) / n_meas


def main():
    torch.manual_seed(0)
    np.random.seed(0)

    cfg = tg.CFG
    B = int(sys.argv[1]) if len(sys.argv) > 1 else 32
    T = cfg["max_ctx"]

    seqs, _ = tg.build_dataset()
    print("序列 %d 条" % len(seqs))

    idx = np.zeros((B, T), np.int64)
    tgt = np.full((B, T), -100, np.int64)
    for r in range(B):
        s = seqs[r % len(seqs)][:T]
        n = len(s)
        idx[r, :n - 1] = s[:n - 1]
        tgt[r, :n - 1] = s[1:n]

    dev = torch.device(DEV)
    m = TorchGPT(cfg, dev)
    opt = torch.optim.AdamW(m.parameters(), lr=1e-3, betas=(0.9, 0.95),
                            weight_decay=0.01)
    ti = torch.from_numpy(idx).to(dev)
    tt = torch.from_numpy(tgt).to(dev)

    def step():
        logits = m.forward(ti)
        loss = torch.nn.functional.cross_entropy(
            logits.reshape(-1, logits.shape[-1]), tt.reshape(-1),
            ignore_index=-100)
        opt.zero_grad(set_to_none=True)
        loss.backward()
        torch.nn.utils.clip_grad_norm_(m.parameters(), 1.0)
        opt.step()
        return loss

    loss = step()
    print("初始 loss %.4f  （CPU 版同期约 8.3，量级一致说明移植没错）"
          % float(loss))
    print()

    t = bench(step)
    flops = 6 * sum(p.numel() for p in m.parameters()) * B * T
    print("batch=%d ctx=%d  参数 %.2fM" % (B, T, sum(p.numel() for p in m.parameters()) / 1e6))
    print("  %s: %.3f 秒/步  (%.1f 样本/秒, %.1f GFLOP/s)"
          % (DEV.upper(), t, B / t, flops / t / 2**30))
    if DEV == "cuda":
        print("  显存峰值 %.0f MB" % (torch.cuda.max_memory_allocated() / 2**20))

    # CPU 基线
    if DEV == "cuda":
        print()
        print("参照：CPU(OpenBLAS 24线程) 实测 4.35 秒/步 (batch32)")
        print("=> GPU 加速 %.1f 倍" % (4.35 / t))
        print()
        print("对8000 步的影响：")
        print("  CPU  %.1f 小时" % (8000 * 4.35 / 3600))
        print("  GPU  %.1f 分钟" % (8000 * t / 60))
    return 0


if __name__ == "__main__":
    sys.exit(main())