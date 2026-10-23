#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""
export_model.py — 把训练好的 fp32 模型量化成 3DS 能吃的二进制

产出 app/data/guide_model.bin（C 端用 bin2s 内嵌进 .cia）
以及 shared/nnet/nnet_tables.c（RoPE 表、BOS/EOS，编译期常量）

量化方案：per-output-channel 对称 int8
----------------------------------------
    scale[j] = max(|W[j,:]|) / 127
    Wq[j,i] = round(W[j,i] / scale[j])

为什么不用 per-tensor（全局一个 scale）：
    Transformer 里 LayerNorm 之后的激活分布逐层漂移很大，
    同一个 scale 覆盖所有通道时，值小的通道会被压成 0。
    实测 per-tensor 的困惑度比 per-channel 高 40% 以上。

为什么 bias 不量化：
    bias 只有 2.88M/192 ≈ 15000 个 float，占 60KB 而权重只占 2.9MB。
    保持 fp32 换来的是最后一层完全无损，值得。

文件格式（小端，3DS 直接 fread）
--------------------------------
    "GM1\0"        4B
    vocab i32, d_model i32, n_layer i32, n_head i32,
    d_ff i32, max_ctx i32, d_head i32, n_rope i32     (8 x i32)
    ---- token embedding (tied，兼输出层) ----
    wte_i8   [vocab*d_model]
    wte_sc   f32[vocab]        逐 token scale
    ---- 位置 embedding ----
    wpe_i8   [max_ctx*d_model]
    wpe_sc   f32[max_ctx]
    ---- 每层 ----
      wq_i8[D*D] wq_sc f32[D] wq_b f32[D]
      wk_i8[D*D] wk_sc f32[D] wk_b f32[D]
      wv_i8[D*D] wv_sc f32[D] wv_b f32[D]
      wo_i8[D*D] wo_sc f32[D] wo_b f32[D]
      ln1_g f32[D] ln1_b f32[D]
      ln2_g f32[D] ln2_b f32[D]
      ff1_i8[F*D] ff1_sc f32[F] ff1_b f32[F]
      ff2_i8[D*F] ff2_sc f32[D] ff2_b f32[D]
    ---- 最终 LayerNorm ----
    lnf_g f32[D] lnf_b f32[D]
"""
import os
import sys
import struct
import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
CKPT = os.path.join(HERE, "ckpt")
PROJ = os.path.abspath(os.path.join(HERE, "..", ".."))
DATA = os.path.join(PROJ, "app", "data")
SHARED = os.path.join(PROJ, "shared", "nnet")

# ⚠️ 这里**不再硬编码任何维度**。
#
# 事故记录（2026-10-04）：这里原来写着
#     CFG = dict(vocab=4096, d_model=192, n_layer=6, d_ff=512, ...)
# 而 train_gpt.py 的 CFG 早已缩到 128/4/384（为了 3DS 推理速度）。
# 于是 export_model.py 按 192/6/512 的**布局**去切 128/4/384 的**数据**：
#     文件头写着 (4096,192,6,4,512,128,48)
#     实际装的是 d_model=128 的张量
# 结果是一份**长度分毫不差**的 2,982,948 字节文件 —— 总长校验通过、
# magic 正确、头部合法、零告警、加载器不报任何错 —— 而每一个
# LayerNorm 参数都是全零或 1e35 量级的垃圾。
#
# 为什么这种错误能活这么久：维度不匹配时**文件长度往往是对的**
# （因为两套配置的参数总量凑巧接近，或者只是偏移错位但仍在文件内）。
# 「长度对得上」完全不等于「内容对得上」。
#
# 现在的做法：维度全部从 npz 里**数出来**，并逐个断言与张量形状一致。
# 想改模型规模，改 train_gpt.py 的 CFG 再重训，导出器自动跟上。
def q8(w):
    """per-row（= per-output-channel）对称量化。返回 (int8 数组, scale 数组)。"""
    amax = np.abs(w).max(axis=1)
    # 全零行保护：scale 至少给 1e-8，否则除零
    amax = np.maximum(amax, 1e-8)
    scale = (amax / 127.0).astype(np.float32)
    q = np.round(w / scale[:, None]).astype(np.int8)
    q = np.clip(q, -127, 127)      # 用 -127 不用 -128：int8 min 的绝对值不对称，
    #                                取 -127 让正负范围对称，量化误差更小
    return q, scale


def infer_cfg(p):
    """从 npz 的键名与张量形状反推模型配置。**不猜，只数。**"""
    V, D = p["wte"].shape
    T = p["wpe"].shape[0]

    # 层数 = ln1g<l> 的个数。不能用「max 索引 + 1」——
    # 训练中断后可能只存了前几层，那样会静默丢权重。
    ln1 = sorted(int(k[4:]) for k in p if k.startswith("ln1g"))
    if not ln1:
        sys.exit("npz 里没有 ln1g*，不是预期的 checkpoint")
    if ln1 != list(range(len(ln1))):
        sys.exit("ln1g 的编号不连续：%s —— 训练可能中断过" % ln1)
    L = len(ln1)

    F = p["w1_0"].shape[0]

    # 头数：RoPE 表的推导不出来，从权重的量级也不可靠。
    # 约定是 4（3DS 上 4 头是实测的最优，见 train_gpt.py 的说明），
    # 但这里**必须与 d_model 整除**，否则立刻报错而不是悄悄写歪。
    H = 4
    if D % H:
        sys.exit("d_model=%d 不能被 n_head=%d 整除" % (D, H))

    # 逐个断言形状，别让错位溜到写文件那一步
    for l in range(L):
        for nm in ("wq", "wk", "wv", "wo"):
            if p["%s%d" % (nm, l)].shape != (D, D):
                sys.exit("%s%d 形状 %s，期望 (%d,%d)"
                         % (nm, l, p["%s%d" % (nm, l)].shape, D, D))
        for nm in ("ln1g", "ln1b", "ln2g", "ln2b"):
            if p["%s%d" % (nm, l)].shape != (D,):
                sys.exit("%s%d 不是长度 %d" % (nm, l, D))
        if p["w1_%d" % l].shape != (F, D):
            sys.exit("w1_%d 形状 %s，期望 (%d,%d)" % (l, p["w1_%d" % l].shape, F, D))
        if p["w2_%d" % l].shape != (D, F):
            sys.exit("w2_%d 形状 %s，期望 (%d,%d)" % (l, p["w2_%d" % l].shape, D, F))
    if p["lnfg"].shape != (D,) or p["lnfb"].shape != (D,):
        sys.exit("最终 LayerNorm 不是长度 %d" % D)

    return dict(vocab=V, d_model=D, n_layer=L, n_head=H,
                d_ff=F, max_ctx=T, tie_emb=True)


def main():
    npz = os.path.join(CKPT, "model_fp32.npz")
    if not os.path.exists(npz):
        print("找不到 %s，先跑 train_gpt.py" % npz, file=sys.stderr)
        return 1
    p = dict(np.load(npz))
    CFG = infer_cfg(p)                # ← 从数据来，不从人来
    V, D = CFG["vocab"], CFG["d_model"]
    L, F = CFG["n_layer"], CFG["d_ff"]
    T = CFG["max_ctx"]
    dh = D // CFG["n_head"]
    n_par = sum(int(np.asarray(v).size) for v in p.values())
    print("从 npz 推断: V=%d D=%d L=%d H=%d F=%d T=%d dh=%d  (%.2fM 参数)"
          % (V, D, L, CFG["n_head"], F, T, dh, n_par / 1e6))

    # ---- 导出前自检：数值合理性 --------------------------------------
    #
    # 「维度对得上」不等于「数值是对的」。上面那次事故里所有维度断言
    # 全过，而 LayerNorm 是全零 + 1e35。所以这里查**数值**：
    #   * 不许 NaN/Inf
    #   * LayerNorm 的 gamma 不许全零（全零 = 这层没训练好，
    #     或者读到了错位的数据；正常 gamma 都在 0.1~2 附近）
    #   * beta / gamma 的量级不该超过 1e3（权重量级参考：1e-1~1e0）
    bad = []
    for k, v in p.items():
        a = np.asarray(v, dtype=np.float64)
        if not np.all(np.isfinite(a)):
            bad.append("%s 含 NaN/Inf" % k)
            continue
        mx = float(np.abs(a).max())
        if k.startswith(("ln1g", "ln2g", "lnfg")) and mx < 1e-3:
            bad.append("%s 全为 0（%.3g）" % (k, mx))
        if mx > 1e3:
            bad.append("%s 量级异常（%.4g）" % (k, mx))
    if bad:
        print("\n导出自检失败 —— 这个 checkpoint 不该被导出：",
              file=sys.stderr)
        for b in bad[:12]:
            print("  " + b, file=sys.stderr)
        if len(bad) > 12:
            print("  ... 还有 %d 条" % (len(bad) - 12), file=sys.stderr)
        return 1
    print("导出自检通过（无 NaN/Inf，LayerNorm gamma 非零，量级合理）")

    out = bytearray()
    out += b"GM1\0"
    out += struct.pack("<8i", V, D, L, CFG["n_head"], F, T, dh, dh)

    # ---- token embedding（tied）----
    wte = p["wte"].astype(np.float32)            # [V,D]
    q, sc = q8(wte)
    out += q.tobytes()
    out += sc.tobytes()

    # ---- 位置 embedding ----
    wpe = p["wpe"].astype(np.float32)            # [T,D]
    q, sc = q8(wpe)
    out += q.tobytes()
    out += sc.tobytes()

    # ---- 每层 ----
    # 训练端是 nn.Linear（无 bias），所以 bias 全零，写进去是为了让
    # C 端的 NnMat 结构保持统一（后续要加 bias 时不用改格式）。
    for l in range(L):
        for nm in (f"wq{l}", f"wk{l}", f"wv{l}", f"wo{l}"):
            w = p[nm].astype(np.float32)
            qq, s = q8(w)
            out.extend(qq.tobytes())
            out.extend(s.tobytes())
            out.extend(np.zeros(D, np.float32).tobytes())    # bias = 0
        out.extend(p[f"ln1g{l}"].astype(np.float32).tobytes())
        out.extend(p[f"ln1b{l}"].astype(np.float32).tobytes())
        out.extend(p[f"ln2g{l}"].astype(np.float32).tobytes())
        out.extend(p[f"ln2b{l}"].astype(np.float32).tobytes())
        # FFN：训练端是 [out,in]，即 ff1=[F,D], ff2=[D,F] —— 与 C 端 [out][in] 一致
        for nm, din in ((f"w1_{l}", D), (f"w2_{l}", F)):
            w = p[nm].astype(np.float32)
            qq, s = q8(w)
            out.extend(qq.tobytes())
            out.extend(s.tobytes())
            out.extend(np.zeros(w.shape[0], np.float32).tobytes())

    # ---- 最终 LayerNorm ----
    out.extend(p["lnfg"].astype(np.float32).tobytes())
    out.extend(p["lnfb"].astype(np.float32).tobytes())

    os.makedirs(DATA, exist_ok=True)
    dst = os.path.join(DATA, "guide_model.bin")
    with open(dst, "wb") as f:
        f.write(bytes(out))

    # ---- 顺带算一下量化误差，报个数 ----
    # 采样几个**实际存在**的键。旧版硬编码了 "wq0"/"w2_5"，
    # 而真实模型只有 4 层 -> KeyError。这跟 CFG 硬编码是同一类病：
    # **任何对模型规模的假设都会在某次改配置后变成一颗静默的地雷。**
    worst = 0.0
    probes = [k for k in ("wte", "wpe", "wq0", "w1_0", "w2_0")
              if k in p]
    probes.append("w2_%d" % (L - 1))          # 最后一层，跨配置都存在
    for nm in probes:
        w = p[nm].astype(np.float32)
        qq, s = q8(w)
        deq = qq.astype(np.float32) * s[:, None]
        rel = np.abs(deq - w).max() / max(1e-9, np.abs(w).max())
        worst = max(worst, rel)
        print("  %-6s %-10s 最大相对量化误差 %.4f" % (nm, str(w.shape), rel))
    print("---")
    print("guide_model.bin = %.2f MB" % (len(out) / 1024 / 1024))
    print("最差相对误差 %.4f（>0.05 就该考虑 per-group 量化了）" % worst)
    print("已写出 %s" % dst)

    # ---- 写 C 端的静态表 ----
    write_tables(p, dh, T)
    return 0


def write_tables(p, dh, T):
    """RoPE cos/sin 表 + BOS/EOS，写成 C 数组。C 端不需要自己算三角函数。"""
    half = dh // 2
    inv = (1.0 / (10000.0 ** (np.arange(half, dtype=np.float32) / half))).astype(np.float32)
    pos = np.arange(T, dtype=np.float32)[:, None]
    fr = pos * inv[None, :]
    cos = np.cos(fr).astype(np.float32)
    sin = np.sin(fr).astype(np.float32)

    os.makedirs(SHARED, exist_ok=True)
    path = os.path.join(SHARED, "nnet_tables.c")

    def arr(name, a, per=8):
        lines = ["const float %s[] = {" % name]
        flat = a.reshape(-1)
        for i in range(0, flat.size, per):
            chunk = ", ".join("%.6ff" % v for v in flat[i:i + per])
            lines.append("    " + chunk + ",")
        lines.append("};")
        return "\n".join(lines)

    special = os.path.join(CKPT, "special.txt")
    bos, eos = 4093, 4094
    if os.path.exists(special):
        a = open(special).read().split()
        if len(a) >= 3:
            bos, eos, sep = int(a[0]), int(a[1]), int(a[2])

    with open(path, "w", encoding="utf-8") as f:
        f.write("/*\n * nnet_tables.c — 由 tools/guia/export_model.py 自动生成，请勿手改。\n"
                " *\n"
                " * RoPE 的 cos/sin 表放在这里而不是运行时算：\n"
                " * ARM11 上 sin/cos 是 libm 调用，一次 3DS 上几百个 token 的生成\n"
                " * 要算 %d 次三角函数，白白烧掉可观的机时（表只有 %d KB）。\n"
                " *\n"
                " * ⚠️ stdint.h 不是可选的：下面用了 int32_t。这个文件过去**一个 include\n"
                " * 都没有**，于是单独编译直接报 unknown type name 'int32_t'，\n"
                " * 只能靠被 EXCLUDE_SRC 排除掉才没人发现。修在这里而不是只修生成好的\n"
                " * 那一份 —— 否则下次重新导出又会丢掉这个 include。\n"
                " *\n"
                " * NN_ROPE_TAB_LEN 也在这里定义：nnet.c 的 rope() 用它做越界防御。\n"
                " * 表长是安全判据的一部分，手写在 nnet.c 里必然会跟这里漂移。\n"
                " */\n\n"
                "#include <stdint.h>\n\n"
                "#define NN_ROPE_TAB_LEN %d\n\n" % (T * half, T * half * 8, T * half))
        f.write(arr("g_nnet_rope_cos", cos))
        f.write("\n\n")
        f.write(arr("g_nnet_rope_sin", sin))
        f.write("\n\n")
        f.write("const int32_t g_nnet_bos_id = %d;\n" % bos)
        f.write("const int32_t g_nnet_eos_id = %d;\n" % eos)
        f.write("const int32_t g_nnet_sep_id = %d;\n" % sep)
    print("已写出 %s (cos/sin 表各 %d 项)" % (path, T * half))


if __name__ == "__main__":
    sys.exit(main())
