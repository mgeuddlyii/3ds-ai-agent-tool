#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
forward_parity.py — C 端 nnet_push 前向 vs 训练端 forward 的逐层比对

    python tools/guia/forward_parity.py

═══ 为什么需要它 ═══════════════════════════════════════════════════

nnet.c 的前向曾经同时有**三个**错误，gcc 全程一个字都没报：

  1. RoPE 配对方式错（交错 vs 前后半分半）+ 只转head 0
  2. 架构不匹配：post-LN + 无残差，而模型按 pre-LN + 残差训练
  3. nnet_state_bytes 少算 5 层 KV cache（122880 字节越界）

第3 个是内存错误，编译器和运行都抓得住（段错误）。
**第 1、2 个不会** —— 程序跑得好好的，输出是纯垃圾。
「零告警」「不崩溃」「能出字」这三条判据全部通过。

所以唯一有效的判据是：**与训练端逐值比对**。
rope_parity.py 只覆盖 RoPE 那一段；本文件覆盖整个前向。

═══ 参照是谁 ═════════════════════════════════════════════════════

参照是**训练端那份 fp32 实现**（tools/guia/train_gpt.py 的 Model.forward），
用真实权重 guide_model.bin 反量化回 fp32 后跑。

注意这里有个**不可回避的量化损失**：C 端权重是 int8 逐通道量化，
所以 C 端结果不可能与 fp32 参照完全一致。判据因此是**相对误差**，
不是绝对误差 —— 而且阈值要按「量化理论下界」定，不是拍脑袋。

量化下界怎么来的：per-channel 对称 int8 的相对步进是
1/127 ≈ 0.79%，单层 matvec 累加 in=192 项时误差按 sqrt(192) 相对放大，
但相对误差不会超过 1% 量级。经过 LayerNorm 归一后放大的是形状不是幅度。
6 层累积 + 输出层，经验上 **相对误差 < 8%** 属于量化正常范围。
实测值打在下面，超了就是真 bug，不是量化。

═══ 免责声明 ═══════════════════════════════════════════════════

本文件是**离线验证工具**，不进 3DS 固件。它读 guide_model.bin
（PC 上的那份），而真机是从 SD 卡读同一份文件 —— 同一份数据，
两边的结论可以直接互相印证。
"""

import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
PROJ = os.path.abspath(os.path.join(HERE, "..", ".."))
MODEL_BIN = os.path.join(PROJ, "app", "data", "guide_model.bin")
TABLES_C = os.path.join(PROJ, "shared", "nnet", "nnet_tables.c")

TOL_REL = 0.08        # 相对误差上限，见上面「量化下界」

try:
    import numpy as np
except ImportError:
    sys.stderr.write("需要 numpy\n")
    raise SystemExit(2)


# --------------------------------------------------------------------
# 读权重
# --------------------------------------------------------------------
def load_model(path):
    d = open(path, "rb").read()
    if d[:4] != b"GM1\0":
        sys.exit("不是 GM1 模型文件")
    V, D, L, H, F, T, dh, n_rope = struct.unpack("<8i", d[4:36])
    off = 36

    def i8(n):
        nonlocal off
        a = np.frombuffer(d, np.int8, n, off); off += n; return a
    def f32(n):
        nonlocal off
        # ⚠️ 这里曾经写成 `off += n`，而 n 是**元素个数**不是字节数 ——
        # 于是每读一段 f32 就少前进 3n 字节。全文件 14720 个 f32
        #   -> 少前进 44160 字节
        # 而 44160 恰好接近文件总长的 3.5%，看起来像「有一段没解释的
        # 尾巴」，很容易被误判成「embedding 段多了一组 bias」。
        # 真实原因只是游标单位错了，**文件本身完全正常**
        # （tools/guia/gm1_layout.py 已独立核实：切段指纹全合理、
        #   游标正好落在文件末尾、embedding 段确实没有 bias）。
        a = np.frombuffer(d, np.float32, n, off); off += 4 * n; return a

    def mat(inp, outp):
        # 反量化回 fp32：W = q * scale[逐输出通道]
        # 文件里是 [out][in] 行主序
        w = i8(outp * inp).reshape(outp, inp).astype(np.float32)
        sc = f32(outp).astype(np.float32)
        return w * sc[:, None]

    m = dict(V=V, D=D, L=L, H=H, F=F, T=T, dh=dh, n_rope=n_rope)
    # embedding 段在文件里**没有 bias**（已由 gm1_layout.py 用逐段
    # 指纹 + 游标末尾对齐双重确认）。这里若用带 bias 的读法会从这里
    # 起整体错位，而总长仍可能吻合 —— 于是「看起来没问题」实则全盘垃圾。
    m["wte"] = mat(D, V)                       # [V,D] tied
    m["wpe"] = mat(D, T)                       # [T,D]
    layers = []
    for _ in range(L):
        ly = {}
        for nm in ("wq", "wk", "wv", "wo"):
            ly[nm] = mat(D, D)                 # [D_out, D_in]
            # 文件里的 bias 是全零占位（训练端 nn.Linear 无 bias）
            ly[nm + "_b"] = f32(D).astype(np.float32)
        ly["ln1g"] = f32(D).astype(np.float32)
        ly["ln1b"] = f32(D).astype(np.float32)
        ly["ln2g"] = f32(D).astype(np.float32)
        ly["ln2b"] = f32(D).astype(np.float32)
        # FFN 的方向：ff1=[F,D]（out=F, in=D），ff2=[D,F]（out=D, in=F）
        # 写反了 D/F 都是合法正数，不会报错，只是读到错位的权重。
        ly["ff1"] = mat(D, F)                   # [F, D]
        ly["ff1_b"] = f32(F).astype(np.float32)
        ly["ff2"] = mat(F, D)                   # [D, F]
        ly["ff2_b"] = f32(D).astype(np.float32)
        layers.append(ly)
    m["layers"] = layers
    m["lnfg"] = f32(D).astype(np.float32)
    m["lnfb"] = f32(D).astype(np.float32)
    m["consumed"] = off
    m["file_len"] = len(d)
    return m


def read_rope_tables():
    """从 nnet_tables.c 读回 C 端真正用的那张表（不复刻）。"""
    s = open(TABLES_C, encoding="utf-8", errors="replace").read()

    def grab(name):
        i = s.index(name); j = s.index("};", i)
        return np.array([float(x[:-1]) for x in
                         __import__("re").findall(r"-?\d+\.\d+f?", s[i:j])],
                        dtype=np.float32)
    return grab("g_nnet_rope_cos"), grab("g_nnet_rope_sin")


# --------------------------------------------------------------------
# 参照：训练端 pre-LN + 残差（train_gpt.py:288-322）
# --------------------------------------------------------------------
def layernorm(x, g, b, eps=1e-5):
    mu = x.mean()
    var = x.var()
    return (x - mu) / np.sqrt(var + eps) * g + b


def gelu(x):
    return 0.5 * x * (1.0 + np.tanh(0.7978845608 *
                                     (x + 0.044715 * x * x * x)))


def ref_forward(m, ids):
    """纯 fp32（无任何 int8），逐层保留中间量供比对。

    ⚠️ 必须对**每个 pos 都跑一遍**：早期版本只算 pos=0 就收工，
    于是 out 的键是 "x_0"（层号）而 C 端是 "x_0_0"（pos+层），
    比对表全空 -> 最大相对误差恒为 0 -> 报「PASS」。
    **一条都没比出来的 PASS 比 FAIL 更危险**，所以 main() 里
    加了「实际比对条数」的下限检查。"""
    D, H, dh, L = m["D"], m["H"], m["dh"], m["L"]
    cos, sin = read_rope_tables()
    half = dh // 2

    def rope_one(vec, p):
        """把长度 D 的向量按 [H, dh] 拆开，对**每个头**施加 RoPE。
        与训练端 apply_rope 一致：前后半分半，共用同一套频率。"""
        hd = np.asarray(vec, np.float32).reshape(H, dh).copy()
        c = cos[p * half: p * half + half]
        s = sin[p * half: p * half + half]
        if c.shape[0] != half:
            return hd.reshape(D)          # 越界就原样返回（与 C 端防御一致）
        for h in range(H):
            x1 = hd[h, :half].copy()
            x2 = hd[h, half:].copy()
            hd[h, :half] = x1 * c - x2 * s
            hd[h, half:] = x1 * s + x2 * c
        return hd.reshape(D)

    out = {}
    # ⚠️ KV 必须**跨 pos 累积**，所以这个字典要建在 pos 循环**外面**。
    # 早期版本把它建在循环体内，于是每个 pos 的参照只看到自己一个
    # token（注意力退化成「只看自己」），而 C 端看到全部历史 ——
    # 于是 pos>=1 全部对不上，误差接近 1.0。
    # 讽刺的是 pos=0 反而完全吻合：因为那时两边都只有一个 token。
    kv = {}                                # l -> (K[t,D], V[t,D])
    for pos, tok in enumerate(ids):
        x = m["wte"][tok] + m["wpe"][pos]
        out["emb_%d" % pos] = x.copy()

        for l in range(L):
            ly = m["layers"][l]
            h1 = layernorm(x, ly["ln1g"], ly["ln1b"])
            out["h1_%d_%d" % (pos, l)] = h1

            qp = rope_one(h1 @ ly["wq"].T + ly["wq_b"], pos)
            k_new = rope_one(h1 @ ly["wk"].T + ly["wk_b"], pos)
            v_new = h1 @ ly["wv"].T + ly["wv_b"]
            out["q_%d_%d" % (pos, l)] = qp.copy()

            if l in kv:
                K, V = kv[l]
                K = np.vstack([K, k_new[None, :]])
                V = np.vstack([V, v_new[None, :]])
            else:
                K, V = k_new[None, :], v_new[None, :]
            kv[l] = (K, V)
            out["K_%d_%d" % (pos, l)] = K[-1].copy()

            # 注意力：因果 mask 由「只看到 pos 为止」天然实现
            ctx = np.zeros(D, dtype=np.float32)
            for h in range(H):
                off = h * dh
                sc = (K[:, off:off + dh] @ qp[off:off + dh]) / np.sqrt(dh)
                e = np.exp(sc - sc.max())
                a = e / e.sum()
                ctx[off:off + dh] = a @ V[:, off:off + dh]
            out["ctx_%d_%d" % (pos, l)] = ctx

            x1 = x + ctx @ ly["wo"].T + ly["wo_b"]
            h2 = layernorm(x1, ly["ln2g"], ly["ln2b"])
            out["h2_%d_%d" % (pos, l)] = h2

            ff = gelu(h2 @ ly["ff1"].T + ly["ff1_b"]) @ ly["ff2"].T + ly["ff2_b"]
            x = x1 + ff
            out["x_%d_%d" % (pos, l)] = x.copy()

        xf = layernorm(x, m["lnfg"], m["lnfb"])
        out["xf_%d" % pos] = xf
        out["logits_%d" % pos] = m["wte"] @ xf
    return out


# --------------------------------------------------------------------
# 被测：C 端 nnet_push 的 numpy 复刻（逐行对照 shared/nnet/nnet.c）
# --------------------------------------------------------------------
def c_quant(v):
    """quant_one：NaN->0，round-half-away-from-zero，夹到 [-128,127]"""
    v = np.asarray(v, dtype=np.float32)
    out = np.where(v != v, 0.0, np.where(v >= 0, v + 0.5, v - 0.5))
    out = np.trunc(out)
    return np.clip(out, -128, 127).astype(np.int8)


def c_quant_dyn(x):
    """quant_dyn：逐张量动态定标。

    ⚠️ 这是**修好之后**的版本。老版对每个张量直接 quant_one
    （隐含 scale=1），而实测：
        q  absmax 2.6e-4 -> 非零率 0.0%
        v  absmax 1.6e-1 -> 非零率 0.0%
        ff1 absmax 1.4e-1 -> 非零率 0.0%
    整个注意力和 FFN 静默退化成 0。C 端已改成 per-tensor 动态
    定标（见 nnet.c 的 quant_dyn），这里必须同步复刻。
    """
    x = np.asarray(x, dtype=np.float32)
    amax = np.abs(x).max() if x.size else np.float32(0.0)
    amax = np.float32(0.0) if not np.isfinite(amax) else np.float32(amax)
    s = np.float32(amax / 127.0)
    if not (s > 1e-20):
        s = np.float32(1.0)
    q = c_quant(x / s)
    return q, s


def c_matvec_f32(w_q, sc, bias, x_int8, x_scale):
    """matvec_f32：dot(int8,int8) * per-channel scale * 输入 scale + bias"""
    s = w_q.astype(np.int32) @ x_int8.astype(np.int32)
    return s.astype(np.float32) * (sc * x_scale) + bias


def c_forward(m, ids, cand=None):
    """逐行复刻 nnet.c 的 nnet_push（pre-LN + 残差 + 动态定标版）"""
    D, H, dh, L, F = m["D"], m["H"], m["dh"], m["L"], m["F"]
    cos, sin = read_rope_tables()

    # 权重量化回 int8（文件里存的就是量化值）
    def qz(w):
        amax = np.abs(w).max(axis=1, keepdims=True)
        amax = np.maximum(amax, 1e-8)
        sc = (amax / 127.0).astype(np.float32).ravel()
        return np.clip(np.round(w / sc[:, None]), -127, 127).astype(np.int8), sc

    W = {}
    for l, ly in enumerate(m["layers"]):
        W[l] = {}
        for nm in ("wq", "wk", "wv", "wo", "ff1", "ff2"):
            W[l][nm], W[l][nm + "_sc"] = qz(ly[nm])

    wte_q, wte_sc = qz(m["wte"])                # [V,D]
    wpe_q, wpe_sc = qz(m["wpe"])                # [T,D]

    half = dh // 2

    def rope_c(vec, p):
        """C 端 rope()：int8 码值域就地旋转，逐头，前后半分半。

        ⚠️ **不要乘 scale**。RoPE 是线性正交变换，在码值域做
        量化后旋转 ≈ 旋转后量化（差一个舍入），而旋转矩阵保范数，
        所以码值天然守恒。曾在这里乘 scale「还原真实幅度」，
        结果 1e-5 量级的真实值被当码值截断成 0，K 全清零。
        越界（p*half+half > 表长）时原样返回，与 C 的防御一致。"""
        hd = vec.reshape(H, dh).copy()
        if p < 0 or p * half + half > len(cos):
            return hd.reshape(D)
        c = cos[p * half: p * half + half]
        s = sin[p * half: p * half + half]
        for h in range(H):
            a = hd[h, :half].astype(np.float32)
            b = hd[h, half:].astype(np.float32)
            hd[h, :half] = c_quant(a * c - b * s)
            hd[h, half:] = c_quant(a * s + b * c)
        return hd.reshape(D)

    out = {}
    Kcache = [[] for _ in range(L)]            # K cache（跨 token 累积）
    Vcache = [[] for _ in range(L)]
    Ksc = [[] for _ in range(L)]               # 逐位置 scale
    Vsc = [[] for _ in range(L)]

    for pos, tok in enumerate(ids):
        # --- embedding -> fp32 残差流 ---
        resid = (wte_q[tok].astype(np.float32) * wte_sc[tok]
                 + wpe_q[pos].astype(np.float32) * wpe_sc[pos])
        out["emb_%d" % pos] = resid.copy()

        for l in range(L):
            ly = m["layers"][l]
            # ① h1 = LN(resid)，动态定标
            h1 = layernorm(resid, ly["ln1g"], ly["ln1b"])
            inbuf, hs = c_quant_dyn(h1)
            # ② q/k/v = matvec_f32 后各自定标
            q, q_s = c_quant_dyn(c_matvec_f32(W[l]["wq"], W[l]["wq_sc"],
                                               ly["wq_b"], inbuf, hs))
            k, k_s = c_quant_dyn(c_matvec_f32(W[l]["wk"], W[l]["wk_sc"],
                                               ly["wk_b"], inbuf, hs))
            v, v_s = c_quant_dyn(c_matvec_f32(W[l]["wv"], W[l]["wv_sc"],
                                               ly["wv_b"], inbuf, hs))
            # ③ RoPE（int8 码值域旋转，不乘 scale）
            q = rope_c(q, pos)
            k = rope_c(k, pos)
            out["q_%d_%d" % (pos, l)] = q.astype(np.float32)

            # ④ 写 KV cache + 逐位置 scale
            Kcache[l].append(k)
            Vcache[l].append(v)
            Ksc[l].append(np.float32(k_s))
            Vsc[l].append(np.float32(v_s))
            Kp = np.array(Kcache[l], dtype=np.float32)
            Vp = np.array(Vcache[l], dtype=np.float32)
            ks = np.array(Ksc[l], dtype=np.float32)
            vs = np.array(Vsc[l], dtype=np.float32)
            out["K_%d_%d" % (pos, l)] = Kp[-1].copy()

            # ⑤ 注意力：score = dot * k_scale[t] * q_scale * rsqrt(dh)
            f3 = np.zeros(D, dtype=np.float32)
            for h in range(H):
                off = h * dh
                sc_ = ((Kp[:, off:off + dh]
                        @ q[off:off + dh].astype(np.float32))
                       * (ks * q_s) / np.sqrt(dh))
                e = np.exp(sc_ - sc_.max())
                a = e / e.sum()
                # v_scale 是逐位置的，必须在这里逐 t 乘
                f3[off:off + dh] = (a * vs) @ Vp[:, off:off + dh]
            out["ctx_%d_%d" % (pos, l)] = f3

            # ⑥⑦ resid += wo @ ctx
            cin, cs = c_quant_dyn(f3)
            resid = resid + c_matvec_f32(W[l]["wo"], W[l]["wo_sc"],
                                         ly["wo_b"], cin, cs)
            # ⑧⑨⑩ resid += ff2 @ gelu(ff1 @ LN(resid))
            h2 = layernorm(resid, ly["ln2g"], ly["ln2b"])
            hin, h2s = c_quant_dyn(h2)
            f1 = gelu(c_matvec_f32(W[l]["ff1"], W[l]["ff1_sc"],
                                   ly["ff1_b"], hin, h2s))
            qk, fs = c_quant_dyn(f1)
            resid = resid + c_matvec_f32(W[l]["ff2"], W[l]["ff2_sc"],
                                         ly["ff2_b"], qk, fs)
            out["x_%d_%d" % (pos, l)] = resid.copy()

        xf = layernorm(resid, m["lnfg"], m["lnfb"])
        out["xf_%d" % pos] = xf
        # tied 输出层：logits[j] = dot(wte_row_j, xf_q) * wte_sc[j] * x_scale
        xq, xs = c_quant_dyn(xf)
        logits = ((wte_q.astype(np.int32) @ xq.astype(np.int32)).astype(np.float32)
                  * (wte_sc * xs))
        out["logits_%d" % pos] = logits

    return out


def relerr(a, b):
    """相对误差：用 b 的 RMS 做分母（b 是参照）"""
    a = np.asarray(a, np.float64).ravel()
    b = np.asarray(b, np.float64).ravel()
    if not (np.all(np.isfinite(a)) and np.all(np.isfinite(b))):
        return float("inf")
    nb = np.sqrt(np.mean(b * b))
    if nb < 1e-12:
        nb = 1.0
    return np.sqrt(np.mean((a - b) ** 2)) / nb


# 逐层要比的关键帧。刻意覆盖三种量级：
#   emb   刚进网络（纯 embedding，没有任何矩阵乘）
#   x     残差流（每层累加后）—— 误差在这里累积，最能反映架构对不对
#   xf    最终 LayerNorm 之后
#   ctx   注意力输出 —— 单独看它能定位「错在注意力还是在 FFN」
KEYS = ("emb", "x", "xf", "ctx")

MIN_CMP = 8   # 至少要比出这么多条，否则视为「参照与被测没对齐」


def main():
    if not os.path.exists(MODEL_BIN):
        sys.exit("找不到 %s —— 先跑 tools/guia/export_model.py" % MODEL_BIN)

    m = load_model(MODEL_BIN)
    print("模型 V=%d D=%d L=%d H=%d F=%d T=%d dh=%d" %
          (m["V"], m["D"], m["L"], m["H"], m["F"], m["T"], m["dh"]))
    ok_len = (m["consumed"] == m["file_len"])
    print("权重消费 %d / 文件 %d 字节  %s" %
          (m["consumed"], m["file_len"],
           "完全吻合" if ok_len else
           "★ 有 %d 字节未解释" % (m["file_len"] - m["consumed"])))

    ids = [1, 300, 700, 1200]
    ref = ref_forward(m, ids)
    got = c_forward(m, ids)

    # ---- 先检查两侧的键是否真能对上 ----
    common = sorted(set(ref) & set(got))
    if not common:
        print()
        print("★ 参照与被测没有一个同名键 —— 比对无从谈起。"
              "这正是「空比出 PASS」的成因，所以先查键。")
        print("  ref 前 8 个键: %s" % sorted(ref)[:8])
        print("  got 前 8 个键: %s" % sorted(got)[:8])
        return 1

    print()
    print("=== 逐层相对误差（C 端 int8 vs 训练端 fp32）===")
    print("  可比对键 %d 个   阈值 %.2f" % (len(common), TOL_REL))
    print()
    print("  %-20s %10s" % ("帧", "相对误差"))
    worst = 0.0
    worst_at = ""
    n_cmp = 0
    per_layer_max = {}
    for pos in range(len(ids)):
        for kind in KEYS:
            if kind == "emb" or kind == "xf":
                ks = ["%s_%d" % (kind, pos)]
            elif kind == "x":
                ks = ["x_%d_%d" % (pos, l) for l in range(m["L"])]
            else:
                ks = ["ctx_%d_%d" % (pos, l) for l in range(m["L"])]
            for k in ks:
                if k not in ref or k not in got:
                    continue
                e = relerr(got[k], ref[k])
                n_cmp += 1
                per_layer_max[k] = max(per_layer_max.get(k, 0.0), e)
                if e > worst:
                    worst, worst_at = e, k
                if kind in ("emb", "xf"):
                    print("  %-20s %10.4f  %s"
                          % ("pos=%d %s" % (pos, kind), e,
                             "OK" if e < TOL_REL else "★ 超阈值"))

    print()
    print("  各帧最差值：")
    for pos in range(len(ids)):
        for kind in ("emb", "x", "xf", "ctx"):
            vals = [v for k, v in per_layer_max.items()
                    if (k == "%s_%d" % (kind, pos) if kind in ("emb", "xf")
                        else k.startswith("%s_%d_" % (kind, pos)))]
            if not vals:
                continue
            v = max(vals)
            print("    pos=%d %-4s %8.4f  %s"
                  % (pos, kind, v, "OK" if v < TOL_REL else "★ 超阈值"))

    print()
    last = len(ids) - 1
    lg = got["logits_%d" % last]
    lgr = ref["logits_%d" % last]
    top = [int(x) for x in np.argsort(-lg)[:5]]
    topr = [int(x) for x in np.argsort(-lgr)[:5]]
    ov = len(set(top) & set(topr))
    print("=== 最终位置 top-5 token ===")
    print("  C 端(int8): %s" % top)
    print("  参照(fp32): %s" % topr)
    print("  重合 %d/5  %s" % (ov, "OK" if ov >= 3 else "★ 偏离过大"))

    print()
    if n_cmp < MIN_CMP:
        print("★ 只比出 %d 条（要求 >=%d）—— 判据形同虚设，不给结论"
              % (n_cmp, MIN_CMP))
        return 1
    print("实际比对 %d 条，最大相对误差 = %.4f（%s）  阈值 %.2f  %s"
          % (n_cmp, worst, worst_at, TOL_REL, "PASS" if worst < TOL_REL else "FAIL"))
    print()
    print("读法：单层 0.02~0.06 是 int8 量化的正常水位（per-channel 步进")
    print("      1/127≈0.79%%，per-tensor 动态定标后误差略高于 per-channel，")
    print("      因为激活是逐张量而不是逐通道）。")
    print("      若某帧突然到 0.3 以上，那是架构/定标对不上，不是量化。")
    ok = worst < TOL_REL and ov >= 3 and ok_len
    print()
    print("总判定：%s" % ("PASS" if ok else "FAIL"))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
