#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
rope_parity.py — C 端 RoPE 与训练端 apply_rope 的逐值一致性检查。

    python tools/guia/rope_parity.py

## 为什么需要这个脚本

nnet.c 的 rope() 曾经同时有两个错误，而 gcc 对这两个错误的反应是
**只报一句 unused parameter**：

  1. 配对方式写成交错（x[i], x[i+1]），训练端是前后半分半
     （x[..., :h] 与 x[..., h:]）。旋转矩阵差了一整个置换。
  2. 只对 x[0..n_rope) 施加旋转 = **只有 head 0 有位置编码**，
     head 1/2/3 的 q/k 从来没被转过。

「零告警」在这里完全没有保护力。这个脚本就是那份保护。

## 判据

  新版 C 端 vs 训练端  最大偏差 < 1e-5   （float32 舍入级，PASS）
  旧版 C 端偏差 ~5.4，与随机输入同量级    （FAIL，且是静默的）

**不要**用「能不能编过」当判据，也不要用「跑起来不崩」当判据 ——
配对方式写错时模型输出的是纯垃圾，但程序跑得好好的。

## 复刻哪一段

  训练端 tools/guia/train_gpt.py:132-140  apply_rope
  训练端 tools/guia/train_gpt.py:159-164  sincos_table
  C 端    shared/nnet/nnet.c               rope()

C 端的 cos/sin 表直接从 shared/nnet/nnet_tables.c 里用正则读回来，
**不复刻、不重算** —— 一致性检查的前提是比对同一份数据。
"""

import os
import re
import sys

try:
    import numpy as np
except ImportError:                    # 明确的失败，别让后面报 NameError
    sys.stderr.write("需要 numpy：%s -m pip install numpy\n" % sys.executable)
    raise SystemExit(2)

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
TABLES_C = os.path.join(ROOT, "shared", "nnet", "nnet_tables.c")
MODEL_BIN = os.path.join(ROOT, "app", "data", "guide_model.bin")

# ⚠️ 这里**不从代码里写死维度**，而是从模型文件的头部读出来。
#
# 事故记录（2026-10-04）：这个文件原来写着 D_MODEL=192，
# 而模型早已缩到 d_model=128。于是：
#     训练端表 128×24 = 3072 项    C 端表 2048 项
#     np.abs(cc - cos.reshape(-1)) 直接 broadcast 失败
# 更糟的是假如当初两边碰巧同长，它会**测出一堆假偏差**，
# 然后有人去「修」本来正确的 nnet.c。
#
# 判据脚本自己写错维度，破坏性比没有判据更大 ——
# 它会让人去修一个没坏的东西。
N_HEAD  = 4        # 头数不在文件里，唯一来源是训练脚本的约定
TOL = 1e-5                         # float32 舍入级；实测新版权差 1.67e-06


def read_dims():
    """从 guide_model.bin 的头部读 (vocab, d_model, n_layer, n_head,
    d_ff, max_ctx, d_head, n_rope)。这是模型的唯一权威声明。"""
    import struct
    with open(MODEL_BIN, "rb") as f:
        d = f.read(36)
    if d[:4] != b"GM1\0":
        sys.exit("FAIL: %s 不是 GM1 模型文件" % MODEL_BIN)
    v = struct.unpack("<8i", d[4:36])
    names = ("vocab d_model n_layer n_head d_ff max_ctx d_head n_rope").split()
    return dict(zip(names, v))


DIM = read_dims()
D_MODEL = DIM["d_model"]
MAX_CTX = DIM["max_ctx"]
D_HEAD  = DIM["d_head"]


def sincos_table(dh, max_ctx, base=10000.0):
    """逐字复刻 train_gpt.py:159-164。"""
    half = dh // 2
    inv = (1.0 / (base ** (np.arange(half, dtype=np.float32) / half))).astype(np.float32)
    t = np.arange(max_ctx, dtype=np.float32)[:, None]
    fr = t * inv[None, :]
    return np.cos(fr).astype(np.float32), np.sin(fr).astype(np.float32)


def read_c_tables():
    """从 nnet_tables.c 读回 cos/sin，不重算 —— 这是比对，不是生成。"""
    s = open(TABLES_C, encoding="utf-8", errors="replace").read()

    m = re.search(r"#define\s+NN_ROPE_TAB_LEN\s+(\d+)", s)
    if not m:
        sys.exit("FAIL: nnet_tables.c 里没有 NN_ROPE_TAB_LEN —— "
                 "export_model.py 的 write_tables 可能没同步")

    def grab(name):
        i = s.index(name)
        j = s.index("};", i)
        nums = re.findall(r"-?\d+\.\d+f?", s[i:j])
        return np.array([float(x[:-1]) for x in nums], dtype=np.float32)

    return grab("g_nnet_rope_cos"), grab("g_nnet_rope_sin"), int(m.group(1))


def ref_rope(q, pos, cos, sin):
    """训练端参照：reshape 成 [B,H,T,dh] -> apply_rope -> 转回 [D]。"""
    qh = q.reshape(1, 1, N_HEAD, D_HEAD)          # B=1, T=1
    c = cos[pos][None, None, None, :]
    s = sin[pos][None, None, None, :]
    x1, x2 = qh[..., :D_HEAD // 2], qh[..., D_HEAD // 2:]
    out = np.concatenate([x1 * c - x2 * s, x1 * s + x2 * c], axis=-1)
    return out.reshape(D_MODEL)


def rope_fixed(x, pos, n_head, d_head, n_rope, cc, ss, tab_len):
    """修好后的 C 端 rope() 的 numpy 等价实现。

    ⚠️ 这里在**码值域**旋转，不乘任何 scale。C 端的 rope() 也一样 ——
    旋转矩阵保范数，所以码值天然守恒；反过来「先乘 scale 还原真实
    幅度、写回时忘了除回去」会把 1e-5 量级的真实值截断成 0，
    K 全清零。见 nnet.c 里 rope() 的注释。"""
    x = x.copy()
    half = n_rope // 2
    if n_rope <= 0 or (n_rope & 1) or pos < 0 or pos * half + half > tab_len:
        return x                                     # 越界防御：宁可不做位置编码
    for h in range(n_head):
        xh = x[h * d_head: h * d_head + d_head]
        for i in range(half):
            a = xh[i]
            b = xh[half + i]
            o = pos * half + i
            xh[i] = a * cc[o] - b * ss[o]
            xh[half + i] = a * ss[o] + b * cc[o]
    return x


def rope_old(x, pos, d_head, n_rope, cc, ss):
    """修复前的 C 端实现，保留作回归对照 —— 两个错误都在里面：
    ① 交错配对（x[i],x[i+1]）而非前后半分半
    ② 只转 head 0（签名里没有 n_head）"""
    x = x.copy()
    for i in range(0, n_rope, 2):
        a = x[i]
        b = x[i + 1]
        o = pos * (n_rope // 2) + i // 2
        x[i] = a * cc[o] - b * ss[o]
        x[i + 1] = a * ss[o] + b * cc[o]
    return x


def main():
    if N_HEAD * D_HEAD != D_MODEL:
        sys.exit("FAIL: n_head(%d) * d_head(%d) = %d != d_model(%d)"
                 % (N_HEAD, D_HEAD, N_HEAD * D_HEAD, D_MODEL))
    print("模型维度（从 guide_model.bin 头部读）: d_model=%d n_head=%d "
          "d_head=%d max_ctx=%d"
          % (D_MODEL, N_HEAD, D_HEAD, MAX_CTX))

    cos, sin = sincos_table(D_HEAD, MAX_CTX)
    cc, ss, tab_len = read_c_tables()

    print("训练端表 %dx%d = %d   C 端表 %d / %d   NN_ROPE_TAB_LEN=%d"
          % (cos.size // (D_HEAD // 2), D_HEAD // 2, cos.size,
             cc.size, ss.size, tab_len))
    if cc.size != cos.size or ss.size != sin.size:
        sys.exit("FAIL: 表长度对不上（训练端 %d / C 端 %d），模型维度与表不同步"
                 % (cos.size, cc.size))
    dcos = np.abs(cc - cos.reshape(-1)).max()
    dsin = np.abs(ss - sin.reshape(-1)).max()
    print("表本身最大偏差   cos %.3e   sin %.3e" % (dcos, dsin))
    if tab_len != cc.size:
        sys.exit("FAIL: NN_ROPE_TAB_LEN(%d) != 实际表长(%d) —— 越界防御会误判"
                 % (tab_len, cc.size))

    rng = np.random.default_rng(0)
    positions = [0, 1, 5, 17, 63, MAX_CTX - 1]
    worst_new = worst_old = 0.0
    per_head_old = np.zeros(N_HEAD)

    for pos in positions:
        q = rng.standard_normal(D_MODEL).astype(np.float32)
        ref = ref_rope(q, pos, cos, sin)
        new = rope_fixed(q, pos, N_HEAD, D_HEAD, D_HEAD, cc, ss, tab_len)
        old = rope_old(q, pos, D_HEAD, D_HEAD, cc, ss)
        worst_new = max(worst_new, np.abs(new - ref).max())
        worst_old = max(worst_old, np.abs(old - ref).max())
        for h in range(N_HEAD):
            sl = slice(h * D_HEAD, (h + 1) * D_HEAD)
            per_head_old[h] = max(per_head_old[h], np.abs(old[sl] - ref[sl]).max())

    print()
    print("新 C 端 vs 训练端  最大偏差 = %.3e   %s"
          % (worst_new, "PASS" if worst_new < TOL else "FAIL"))
    print("旧 C 端 vs 训练端  最大偏差 = %.3e   %s"
          % (worst_old, "PASS" if worst_old < TOL else "FAIL（预期，它是反例）"))
    print("旧版逐头偏差 head0..%d = %s" % (N_HEAD - 1,
                                            ["%.2e" % v for v in per_head_old]))
    print("  head0 也错 = 配对方式错；head1.. 也错 = 漏了多头循环")

    # 越界防御
    print()
    q = rng.standard_normal(D_MODEL).astype(np.float32)
    for name, over in [("pos=max_ctx", dict(pos=MAX_CTX)),
                       ("pos=-1", dict(pos=-1)),
                       ("n_rope 奇数", dict(pos=3, n_rope=D_HEAD + 1))]:
        kw = dict(pos=3, n_rope=D_HEAD)
        kw.update(over)
        got = rope_fixed(q, kw["pos"], N_HEAD, D_HEAD, kw["n_rope"],
                         cc, ss, tab_len)
        ok = np.array_equal(got, q)
        print("防御 %-12s -> %s" % (name, "PASS（原样返回）" if ok else "FAIL"))
        if not ok:
            return 1

    if worst_new >= TOL:
        print()
        print("FAIL: 修好后的 C 端仍与训练端不一致")
        return 1
    print()
    print("全部通过。")
    return 0


if __name__ == "__main__":
    sys.exit(main())
