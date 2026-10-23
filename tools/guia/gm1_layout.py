#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
gm1_layout.py — GM1 文件布局的**独立核算器**

    python tools/guia/gm1_layout.py

═══ 这个文件为什么存在 ═══════════════════════════════════════════

forward_parity.py 一度报「权重消费 1210788 / 文件 1254948，差 44160 字节未解释」。
而手算「embedding 段无 bias」的总长恰好 = 1254948，与文件分毫不差。
两个结论互斥 —— 说明**至少有一个是错的**，而当时两边都是「看起来很可信」的。

这类矛盾的解法只有一个：**换一个更小的判据**。
所以本文件不复用任何前向逻辑，只做一件事 ——
把文件按「段」切开，逐段打印偏移、长度、以及该段头几个值的合理性。

合理性判据（这些是从训练端已知事实推的，不是拍脑袋）：
  * int8 权重段：不该整段为 0；抽样应在 ±127 附近有分布
  * f32 的 scale 段：per-channel 对称量化下 scale = amax/127，
    所以 scale 应**全为正**，且 max(scale)*127 应等于该行 amax
  * f32 的 bias 占位段：**必须整段为 0**（训练端 nn.Linear 无 bias）
  * LayerNorm 的 gamma 段：应在 0.1~2 量级（**全 0 = 读到错位**）
  * LayerNorm 的 beta 段：应在 ±1 量级

只靠「总长对得上」不能定位，就靠这些逐段的**值域指纹**。
"""
import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
PROJ = os.path.abspath(os.path.join(HERE, "..", ".."))
MODEL_BIN = os.path.join(PROJ, "app", "data", "guide_model.bin")


def f32at(d, off, n):
    return np.frombuffer(d, np.float32, n, off)


def i8at(d, off, n):
    return np.frombuffer(d, np.int8, n, off)


def fingerprint_i8(a):
    a = np.asarray(a, np.float64)
    return "min=%d max=%d 均值=%.1f 零占比=%.1f%%" % (
        a.min(), a.max(), a.mean(), 100.0 * (a == 0).mean())


def fingerprint_f32(a):
    a = np.asarray(a, np.float64)
    if not np.all(np.isfinite(a)):
        return "★ 含 NaN/Inf  值域 %g .. %g" % (np.nanmin(a[np.isfinite(a)]) if np.any(np.isfinite(a)) else 0,
                                                np.nanmax(a[np.isfinite(a)]) if np.any(np.isfinite(a)) else 0)
    return "min=%.6g max=%.6g 均值=%.6g" % (a.min(), a.max(), a.mean())


def main():
    if not os.path.exists(MODEL_BIN):
        sys.exit("找不到 %s —— 先跑 export_model.py" % MODEL_BIN)
    d = open(MODEL_BIN, "rb").read()
    flen = len(d)
    print("文件 %s" % MODEL_BIN)
    print("长度 %d 字节   magic=%r" % (flen, d[:4]))
    if d[:4] != b"GM1\0":
        sys.exit("不是 GM1")

    V, D, L, H, F, T, dh, n_rope = struct.unpack("<8i", d[4:36])
    print("头部: V=%d D=%d L=%d H=%d F=%d T=%d dh=%d n_rope=%d"
          % (V, D, L, H, F, T, dh, n_rope))
    print("自洽检查: dh*H=%d vs D=%d -> %s"
          % (dh * H, D, "OK" if dh * H == D else "★ 不一致"))

    # ----------------------------------------------------------
    # 总长的两种假设，先用算术判掉一种
    # ----------------------------------------------------------
    emb_bias = 4 * (V + T)          # embedding 段若带 bias，多出这么多字节
    per_layer = (4 * (D * D + 4 * D + 4 * D)      # wq wk wv wo
                 + 4 * 4 * D                        # ln1g ln1b ln2g ln2b
                 + (F * D + 4 * F + 4 * F)         # ff1 = [F,D]
                 + (D * F + 4 * D + 4 * D))        # ff2 = [D,F]
    base = 36
    len_nb = base + (V * D + 4 * V) + (T * D + 4 * T) + L * per_layer + 2 * 4 * D
    len_b = len_nb + emb_bias
    print()
    print("=== 总长算术（先判掉一种假设，不用读文件）===")
    print("  假设 A「embedding 无 bias」 = %d   %s"
          % (len_nb, "★ 与文件吻合" if len_nb == flen else "不符"))
    print("  假设 B「embedding 有 bias」 = %d   %s"
          % (len_b, "★ 与文件吻合" if len_b == flen else "不符"))
    if len_nb != flen and len_b != flen:
        print("  ★★ 两种假设都不吻合 —— 头部的维度本身可能就不对，"
              "别急着讨论 bias，先查 npz 与头是否同源")
        return 1

    # ----------------------------------------------------------
    # 按通过的假设切段，逐段打指纹
    # ----------------------------------------------------------
    with_bias = (len_b == flen)
    print()
    print("=== 逐段指纹（按假设 %s 切）===" % ("B" if with_bias else "A"))
    off = 36
    problems = []

    def take(n, what):
        nonlocal off
        if off + n > flen:
            problems.append("%s 越界（off=%d n=%d flen=%d）" % (what, off, n, flen))
            return None
        a = d[off:off + n]
        print("  off=%-9d %-12s len=%-8d %s"
              % (off, what, n, fingerprint_i8(np.frombuffer(a, np.int8, n))
                 if what.endswith("_i8") else fingerprint_f32(np.frombuffer(a, np.float32, n // 4))))
        off += n
        return a

    def take_i8(n, what):
        nonlocal off
        if off + n > flen:
            problems.append("%s 越界" % what)
            off += n
            return None
        a = np.frombuffer(d, np.int8, n, off)
        fp = fingerprint_i8(a)
        if a.max() == 0 and a.min() == 0:
            problems.append("%s 整段为 0 —— 几乎必然是错位" % what)
        elif a.max() < 8:
            problems.append("%s 量级过小（max=%d），可疑" % (what, a.max()))
        print("  off=%-9d %-12s len=%-8d %s" % (off, what, n, fp))
        off += n
        return a

    def take_f32(n, what, expect=None):
        """n 是元素个数。expect: 'zero' | 'pos' | 'norm' | None"""
        nonlocal off
        if off + 4 * n > flen:
            problems.append("%s 越界" % what)
            off += 4 * n
            return None
        a = np.frombuffer(d, np.float32, n, off)
        fp = fingerprint_f32(a)
        if not np.all(np.isfinite(a)):
            problems.append("%s 含 NaN/Inf（%s）—— 读到的不是这一段" % (what, fp))
        elif expect == "zero" and np.abs(a).max() != 0.0:
            problems.append("%s 应为全零占位，实测 max=%.4g" % (what, np.abs(a).max()))
        elif expect == "pos" and a.min() <= 0:
            problems.append("%s scale 应全为正，实测 min=%.4g" % (what, a.min()))
        elif expect == "norm" and (np.abs(a).max() < 1e-3 or np.abs(a).max() > 1e3):
            problems.append("%s 量级异常（%.4g）" % (what, np.abs(a).max()))
        print("  off=%-9d %-12s len=%-8d %s" % (off, what, 4 * n, fp))
        off += n * 4
        return a

    take_i8(V * D, "wte_i8")
    take_f32(V, "wte_sc", "pos")
    if with_bias:
        take_f32(V, "wte_b", "zero")
    take_i8(T * D, "wpe_i8")
    take_f32(T, "wpe_sc", "pos")
    if with_bias:
        take_f32(T, "wpe_b", "zero")

    for l in range(L):
        for nm in ("wq", "wk", "wv", "wo"):
            take_i8(D * D, "%s%d_i8" % (nm, l))
            take_f32(D, "%s%d_sc" % (nm, l), "pos")
            take_f32(D, "%s%d_b" % (nm, l), "zero")
        take_f32(D, "ln1g%d" % l, "norm")
        take_f32(D, "ln1b%d" % l, "norm")
        take_f32(D, "ln2g%d" % l, "norm")
        take_f32(D, "ln2b%d" % l, "norm")
        take_i8(F * D, "ff1_%d_i8" % l)
        take_f32(F, "ff1_%d_sc" % l, "pos")
        take_f32(F, "ff1_%d_b" % l, "zero")
        take_i8(D * F, "ff2_%d_i8" % l)
        take_f32(D, "ff2_%d_sc" % l, "pos")
        take_f32(D, "ff2_%d_b" % l, "zero")

    take_f32(D, "lnfg", "norm")
    take_f32(D, "lnfb", "norm")

    print()
    if off != flen:
        problems.append("读完所有段后 off=%d，文件长 %d，仍差 %d 字节"
                        % (off, flen, flen - off))
    else:
        print("★ 游标正好落在文件末尾（off=%d），布局完全确定" % off)

    print()
    if problems:
        print("发现 %d 个问题：" % len(problems))
        for p in problems[:30]:
            print("  " + p)
        if len(problems) > 30:
            print("  ... 还有 %d 条" % (len(problems) - 30))
        return 1
    print("逐段指纹全部合理。")
    print()
    print("结论：embedding 段在文件里%s bias。"
          % ("**有**（全零占位）" if with_bias else "**没有**"))
    return 0


if __name__ == "__main__":
    try:
        import numpy as np
    except ImportError:
        sys.stderr.write("需要 numpy\n")
        raise SystemExit(2)
    sys.exit(main())
