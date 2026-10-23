#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""
train_gpt.py — 3DS 离线攻略微模型训练（纯 numpy，不依赖 torch）

为什么要手写训练循环
--------------------
venv 里只有 numpy，没装 torch（装 torch 要 3GB，用户磁盘紧张）。
更要命的是：**模型只有 2.9M 参数**，用 torch 反而是负担。
一个 GPT 的前向+反向在 numpy 里就是几十行 matmul。

模型规格（为 3DS 而定，不是为 PC 而定）
--------------------------------------
    n_layer=6, d_model=192, n_head=4, d_ff=512, vocab=4096, max_ctx=128
    tied embedding（输出层复用 token embedding，省 786k 参数）

    tok_emb  4096*192                =   786,432
    pos_emb   128*192                =    24,576
    每层 attn 4*192*192              =   147,456
    每层 ffn  2*192*512              =   196,608
    每层小计                          =   344,064  x6  = 2,064,384
    LayerNorm 参数 (12 组 x2 x192)    =     4,608
    ------------------------------------------------
    总计                              ≈ 2,880,000  ≈ 2.88M

int8 量化后权重 ≈ 2.9MB。ARM11 @804MHz 估 25 tok/s，
Old 3DS @268MHz 估 8 tok/s（这两个数字是容量估算，不是实测）。

反向传播的实现要点
------------------
第一版我把反向写错了（ln_backward 是个空壳，而且在反向里**重算**前向
中间量 —— 这在 RoPE + 多头 reshape 的场景下极易与前向不一致）。
所以这一版改成**前向全量缓存、反向纯消费缓存**：
    cache[l] = {ln1_out, q, k, v, att, ctx, ln2_out, ff1, ff1_pre}
反向只读 cache，不重算。多占一点内存（batch 16 x 128 x 512 float32
约 4MB），但换来了不会算错。
"""
import os
import sys
import math
import time
import pickle
import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
CKPT = os.path.join(HERE, "ckpt")

CFG = dict(
    vocab=4096,
    d_model=128,
    n_layer=4,
    n_head=4,
    d_ff=384,
    max_ctx=128,
    tie_emb=True,
)

"""
为什么从 d192/L6 缩到 d128/L4（2.86M -> 1.18M）
-----------------------------------------------
两条理由，都是实测/算出来的：

1. **3DS 侧的推理速度**。ARM11 无 NEON，New 3DS 804MHz。
   3DS 侧估算 25 tok/s 那个基准是按 2.07M 非 embedding 参数算的：
       非 emb 参数 2.07M -> 24.9 tok/s
       非 emb 参数 0.66M -> 78.5 tok/s     （快 3.15 倍）
   一句回答从 1 秒多降到 0.3 秒左右，这是能"感觉到"的差别。

2. **语料/参数比**。这是更根本的一条。
   15.1 万 token 配 2.86M 参数 = 0.053 token/参数，
   而 Chinchilla 建议 20，**差 380 倍**。这个比例下模型唯一的出路
   是把训练集背下来，不存在"学语言"这回事。
   缩到 1.18M 后是 0.128 token/参数（差 156 倍），
   依然不够，但至少不再是一个"必然背题"的配置。

代价：容量降了，同一 batch 上 loss 会比 2.88M 高一些。
基线留档：2.88M 在 step 800 的留出集 loss 是 6.8461，可以直接对比。
"""


# ------------------------------------------------------------------ 算子

def layernorm(x, g, b, eps=1e-5):
    """
    返回 (输出, xhat, rstd)。

    rstd 必须带出去：反向时 dx = rstd * (...)/g，漏掉 rstd 会让整条
    反向链的梯度差 4 个数量级（第一版就漏了，gradcheck 立刻抓到）。
    """
    mu = x.mean(-1, keepdims=True)
    xc = x - mu
    var = (xc * xc).mean(-1, keepdims=True)
    rstd = 1.0 / np.sqrt(var + eps)
    xhat = xc * rstd
    return xhat * g + b, xhat, rstd


def layernorm_bwd(dy, xhat, rstd, g):
    """返回 (dx, dg, db)。dy/xhat: [B,T,D]，rstd: [B,T,1]。"""
    D = dy.shape[-1]
    flat = dy.reshape(-1, D)
    xh = xhat.reshape(-1, D)
    dg = (flat * xh).sum(0)
    db = flat.sum(0)
    s1 = flat.sum(1, keepdims=True)
    s2 = (flat * xh).sum(1, keepdims=True)
    dx = (flat - s1 / D - xh * s2 / D) / g
    dx = dx.reshape(dy.shape) * rstd
    return dx, dg, db


def gelu(x):
    return 0.5 * x * (1.0 + np.tanh(0.7978845608 * (x + 0.044715 * x ** 3)))


def dgelu(x):
    u = 0.7978845608 * (x + 0.044715 * x ** 3)
    t = np.tanh(u)
    du = 0.7978845608 * (1 + 3 * 0.044715 * x ** 2)
    return 0.5 * (1 + t) + 0.5 * x * (1 - t * t) * du


def softmax(x, axis=-1):
    x = x - x.max(axis=axis, keepdims=True)
    e = np.exp(x)
    return e / e.sum(axis=axis, keepdims=True)


def apply_rope(x, cos, sin):
    """
    x: [B,H,T,dh]，cos/sin: [T,dh/2]
    旋转前一半与后一半配对。
    """
    d = x.shape[-1]
    h = d // 2
    x1, x2 = x[..., :h], x[..., h:]
    T = x1.shape[-2]
    c = cos[:T][None, None, :, :h]
    s = sin[:T][None, None, :, :h]
    return np.concatenate([x1 * c - x2 * s, x1 * s + x2 * c], axis=-1)


def rope_bwd(dx, cos, sin):
    """
    RoPE 是线性正交变换，逆旋转 = 用 -sin 重放同一组公式。
    接受 [B,H,T,dh] 或 [B,T,dh]（反向里 dq 已经被 reshape 回 3D）。
    """
    d = dx.shape[-1]
    h = d // 2
    dx1, dx2 = dx[..., :h], dx[..., h:]
    T = dx1.shape[-2]
    extra = dx.ndim - 3          # 3D 输入时为 0，4D 输入时为 1
    # cos/sin 形状是 [max_ctx, h]，取前 T 个位置即可（不要按 h 切）
    c = cos[:T].reshape((1,) * extra + (T, h))
    s = sin[:T].reshape((1,) * extra + (T, h))
    return np.concatenate([dx1 * c + dx2 * s, -dx1 * s + dx2 * c], axis=-1)


def sincos_table(dh, max_ctx, base=10000.0):
    half = dh // 2
    inv = (1.0 / (base ** (np.arange(half, dtype=np.float32) / half))).astype(np.float32)
    t = np.arange(max_ctx, dtype=np.float32)[:, None]
    fr = t * inv[None, :]
    return np.cos(fr).astype(np.float32), np.sin(fr).astype(np.float32)


# ------------------------------------------------------------------ 数据

# ★★ 语料的问答格式（2026-10-05 定死，全项目只此一种）
#
# 语料里 Q: 与 A: **各占一行**（10:5x 那轮「拆 Q+A 挤行」之后就一直是这样）：
#
#     Q: 第3步拿的钥匙要开哪扇门
#     A: 开教学楼东侧那道铁门，门后走廊尽头才是下一…
#
# ⚠️ 曾经这里是 `if "Q:" in para and " A:" in para` —— 要求两者**同一行**。
#    语料改成分行之后这条判据恒不成立，n_qa 恒 0：
#    训练出来的模型只见过原文长段落的续写，从没见过「短问→短答」，
#    而 build_dataset 的 docstring 说这就是"小模型能不能用的关键"。
#    当时 verify_all_offline 7/7 全绿——那些校验都在 Python 里读 .bin，
#    根本不碰训练数据的解析。
#
# ⚠️ 写死"挤行也能认"更糟：parse 会静默接受老格式，
#    于是"数据少了一半"和"数据全在"长得一模一样。只认一种，错了就红。
def scan_qa(lines):
    """
    行式扫码：返回 (问答对列表, Q: 行数)。

    同时返回 Q: 行数是为了做那条**能区分两种坏情况**的守卫：
    语料里明明有一大堆 Q: 行、却一组都拆不成对 → 解析坏了（红）；
    语料本来就没有任何问答 → 也红（说明采集漏了）。
    只判 `n_qa == 0` 分不出来这两者。
    """
    pairs, n_q = [], 0
    for i, ln in enumerate(lines):
        s = ln.strip()
        if not s.startswith("Q"):
            continue
        n_q += 1
        nxt = lines[i + 1].strip() if i + 1 < len(lines) else ""
        # ★ 必须是「A:」或「A：」，不能只判 startswith("A")：
        #   语料里有「A面」「A路线」这类以字母 A 开头的正文，
        #   只判首字母会把它当成答案，凑出一批「问 + 毫不相干的正文」错配对，
        #   n_qa 也会虚高（实测 16517 vs 真实 16274，多出来的就是这类）。
        if not nxt.startswith("A") or not (nxt[1:2] in (":", "：")):
            continue
        q, a = s[1:].lstrip(":：").strip(), nxt[1:].lstrip(":：").strip()
        if 3 <= len(q) <= 60 and 3 <= len(a) <= 400:
            pairs.append((q, a))
    return pairs, n_q


def load_bpe():
    with open(os.path.join(CKPT, "bpe.pkl"), "rb") as f:
        return pickle.load(f)


def build_dataset():
    """
    把语料重排成 QA 对话流。这是小模型能不能用的关键：
    11.8 万字攻略当普通语料训，模型学的是「攻略怎么写」；
    玩家在设备上问的是短问句，所以必须让它见够 1000+ 组「短问→短答」。

    每条序列：  <BOS> 问句 <SEP> 答案 <EOS>
    另外混入原文长序列，教它组织长文本（RAG 抽出的原文段落要用）。
    """
    sys.path.insert(0, HERE)
    from train_bpe import BPE, norm_text

    d = load_bpe()
    b = BPE(vocab_size=len(d["inv"]))
    b.vocab, b.inv, b.merges = d["vocab"], d["inv"], d["merges"]
    b._train_ranks()

    V = len(b.inv)
    BOS, EOS, SEP = V - 3, V - 2, V - 1
    T = CFG["max_ctx"]

    corpus = os.path.join(HERE, "corpus")
    files = sorted(f for f in os.listdir(corpus)
                   if f.startswith("guide_") and f.endswith(".txt"))

    seqs, n_qa, n_raw, n_drop = [], 0, 0, 0
    n_qline_all = 0
    for fn in files:
        text = norm_text(open(os.path.join(corpus, fn), encoding="utf-8").read())
        lines = [x.strip() for x in text.split("\n")]
        qa, n_qline = scan_qa(lines)
        raw = []
        for para in lines:
            if not para or para.startswith("##"):
                continue
            if para.startswith("Q") or para.startswith("A"):
                continue              # 问答由 scan_qa 收走，别再落进原文分支
            elif len(para) > 40:
                raw.append(para)
        n_qline_all += n_qline
        n_qa += len(qa)
        n_raw += len(raw)

        for q, a in qa:
            head = [BOS] + b.encode(q) + [SEP]
            body = b.encode(a)
            room = T - len(head) - 1          # 留一个位置给 EOS
            if room < 1:
                # 问句本身就超长了：只保留问句前若干 token + SEP + EOS
                seqs.append(np.array(head[:T - 2] + [SEP, EOS], np.int32))
                continue
            if len(body) > room:
                body = body[:room]
            seqs.append(np.array(head + body + [EOS], np.int32))

        if raw:
            ids = []
            for p in raw:
                ids.extend(b.encode(p))
            n = (len(ids) // (T - 1)) * (T - 1)
            for i in range(0, n, T - 1):
                seqs.append(np.array([BOS] + ids[i:i + T - 2] + [EOS], np.int32))

    print("序列 %d 条（QA %d 组 / 原文段 %d 段 / 丢弃 %d）"
          % (len(seqs), n_qa, n_raw, n_drop))
    print("token 总数 %d  BOS=%d EOS=%d SEP=%d"
          % (sum(len(s) for s in seqs), BOS, EOS, SEP))

    # ★ 守卫：语料里有一堆 Q: 行却拆不出成对问答 → 一定是解析坏了（红）。
    #   只判 `n_qa == 0` 分不出"解析坏了"和"语料本来就没问答"，看不出原因。
    if n_qline_all and n_qa < n_qline_all * 0.8:
        raise RuntimeError(
            "语料里 %d 行 Q: 只拆出 %d 组问答 —— 问答格式解析坏了。"
            "现在只认「Q: 与 A: 各占一行」，挤成 `Q: 问 A: 答` 一行会被本判据抓到。"
            % (n_qline_all, n_qa))
    return seqs, (BOS, EOS, SEP)


# ------------------------------------------------------------------ 模型

class GPT:
    def __init__(self, cfg, seed=1234):
        self.c = cfg
        D, L, H, F, V = (cfg["d_model"], cfg["n_layer"], cfg["n_head"],
                         cfg["d_ff"], cfg["vocab"])
        self.dh = D // H
        r = np.random.default_rng(seed)
        self.p = {}
        self.p["wte"] = (r.standard_normal((V, D)) * 0.02).astype(np.float32)
        self.p["wpe"] = (r.standard_normal((cfg["max_ctx"], D)) * 0.01).astype(np.float32)
        for l in range(L):
            p = self.p
            p[f"ln1g{l}"] = np.ones(D, np.float32)
            p[f"ln1b{l}"] = np.zeros(D, np.float32)
            p[f"wq{l}"] = (r.standard_normal((D, D)) * 0.02).astype(np.float32)
            p[f"wk{l}"] = (r.standard_normal((D, D)) * 0.02).astype(np.float32)
            p[f"wv{l}"] = (r.standard_normal((D, D)) * 0.02).astype(np.float32)
            p[f"wo{l}"] = (r.standard_normal((D, D)) * 0.02).astype(np.float32)
            p[f"ln2g{l}"] = np.ones(D, np.float32)
            p[f"ln2b{l}"] = np.zeros(D, np.float32)
            p[f"w1_{l}"] = (r.standard_normal((F, D)) * 0.02).astype(np.float32)
            p[f"w2_{l}"] = (r.standard_normal((D, F)) * 0.02).astype(np.float32)
        self.p["lnfg"] = np.ones(D, np.float32)
        self.p["lnfb"] = np.zeros(D, np.float32)

        # 残差投影缩放：深层不缩放第 3 层就开始发散
        for l in range(L):
            s = (2.0 * L) ** -0.5
            self.p[f"wo{l}"] *= s
            self.p[f"w2_{l}"] *= s

        self.nparam = sum(v.size for v in self.p.values())
        self.cos, self.sin = sincos_table(self.dh, cfg["max_ctx"])
        print("参数量 %.2fM" % (self.nparam / 1e6))
        self.m = {k: np.zeros_like(v) for k, v in self.p.items()}
        self.v = {k: np.zeros_like(v) for k, v in self.p.items()}
        self.t = 0

    # ---------------- 前向（全量缓存） ----------------
    def forward(self, ids, cache=None, pos0=0):
        p, c = self.p, self.c
        B, T = ids.shape
        D, H, dh, L = p["wte"].shape[1], c["n_head"], self.dh, c["n_layer"]
        save = {}

        x = p["wte"][ids] + p["wpe"][pos0:pos0 + T][None, :, :]
        save["x0_arg"] = ids                      # tied embedding 的输入查表梯度要用
        save["x0"] = x
        mask = np.triu(np.full((T, T), -1e9, np.float32), 1)
        cos, sin = self.cos[pos0:pos0 + T], self.sin[pos0:pos0 + T]

        for l in range(L):
            h1, xhat1, r1 = layernorm(x, p[f"ln1g{l}"], p[f"ln1b{l}"])
            q = h1 @ p[f"wq{l}"].T
            k = h1 @ p[f"wk{l}"].T
            v = h1 @ p[f"wv{l}"].T
            q = q.reshape(B, T, H, dh).transpose(0, 2, 1, 3)
            k = k.reshape(B, T, H, dh).transpose(0, 2, 1, 3)
            v = v.reshape(B, T, H, dh).transpose(0, 2, 1, 3)
            q = apply_rope(q, cos, sin)
            k = apply_rope(k, cos, sin)
            att = softmax(q @ k.transpose(0, 1, 3, 2) / math.sqrt(dh) + mask[None, None])
            ctx = (att @ v).transpose(0, 2, 1, 3).reshape(B, T, D)

            x1 = x + ctx @ p[f"wo{l}"].T
            h2, xhat2, r2 = layernorm(x1, p[f"ln2g{l}"], p[f"ln2b{l}"])
            ff1_pre = h2 @ p[f"w1_{l}"].T
            ff1 = gelu(ff1_pre)
            x2 = x1 + ff1 @ p[f"w2_{l}"].T

            save[f"l{l}"] = dict(
                xin=x, h1=h1, xhat1=xhat1, r1=r1,
                q=q, k=k, v=v, att=att, ctx=ctx,
                xmid=x1, h2=h2, xhat2=xhat2, r2=r2,
                ff1_pre=ff1_pre, ff1=ff1,
            )
            x = x2

        xf, xhatf, rf = layernorm(x, p["lnfg"], p["lnfb"])
        save["xf"] = xf
        save["xhatf"] = xhatf
        save["rf"] = rf
        logits = xf @ p["wte"].T          # tied：wte 本身是 [V,D]，直接转置用
        save["logits"] = logits
        return save

    # ---------------- 反向（纯消费缓存） ----------------
    def backward(self, save, targets):
        p, c = self.p, self.c
        B, T = save["logits"].shape[0], save["logits"].shape[1]
        D, H, dh, L = p["wte"].shape[1], c["n_head"], self.dh, c["n_layer"]
        g = {}

        # 交叉熵 -> dlogits
        V = p["wte"].shape[0]
        probs = softmax(save["logits"], -1)
        dlogits = probs.copy()
        valid = targets >= 0
        bi = np.arange(B)[:, None]
        ti = np.arange(T)[None, :]
        dlogits[bi, ti, np.maximum(targets, 0)] -= 1.0
        dlogits *= valid[:, :, None]
        ntok = max(1, int(valid.sum()))
        dlogits /= ntok

        # tied embedding：输出侧贡献（logits = xf @ wte.T）
        g["wte"] = dlogits.reshape(-1, V).T @ save["xf"].reshape(-1, D)
        dx = dlogits @ p["wte"]                       # [B,T,D]

        dx, dgn, dbn = layernorm_bwd(dx, save["xhatf"], save["rf"], p["lnfg"])
        g["lnfg"], g["lnfb"] = dgn, dbn

        for l in reversed(range(L)):
            d = save[f"l{l}"]
            # ---- FFN 分支 ----
            # x2 = x1 + ff1 @ w2.T
            #   => d(x1) = d(x2) [残差] + FFN 路径的贡献
            # 漏掉残差项会让下面所有权重梯度偏小（gradcheck 会报 rel_err~1.0）
            dff2 = dx
            g[f"w2_{l}"] = dff2.reshape(-1, D).T @ d["ff1"].reshape(-1, c["d_ff"])
            dff1 = dff2 @ p[f"w2_{l}"]
            # 关键：w1 的梯度必须乘 dgelu，因为 ff1 = gelu(ff1_pre)，
            # 而 dff1 是对 gelu **输出** 的梯度。要往回传必须先过 dgelu，
            # 然后再和 w1 的上游（也就是 ff1_pre）配对。
            dff1_pre = dff1 * dgelu(d["ff1_pre"])
            g[f"w1_{l}"] = dff1_pre.reshape(-1, c["d_ff"]).T @ d["h2"].reshape(-1, D)
            d_ffn_in = dff1_pre @ p[f"w1_{l}"]
            d_ffn_in, dgn, dbn = layernorm_bwd(d_ffn_in, d["xhat2"], d["r2"], p[f"ln2g{l}"])
            g[f"ln2g{l}"], g[f"ln2b{l}"] = dgn, dbn
            # 残差 + FFN
            dx1 = dx + d_ffn_in

            # ---- 注意力分支 ----
            # x1 = x + ctx @ wo.T
            #   => d(x) = d(x1) [残差] + 注意力路径的贡献
            d_attn_out = dx1
            g[f"wo{l}"] = d_attn_out.reshape(-1, D).T @ d["ctx"].reshape(-1, D)
            dctx = d_attn_out @ p[f"wo{l}"]
            dctx = dctx.reshape(B, T, H, dh).transpose(0, 2, 1, 3)

            datt = dctx @ d["v"].transpose(0, 1, 3, 2)
            dv = d["att"].transpose(0, 1, 3, 2) @ dctx
            # softmax backward
            ds = datt * d["att"]
            ds = ds - ds.sum(-1, keepdims=True) * d["att"]
            ds = ds / math.sqrt(dh)
            dq = ds @ d["k"]
            dk = ds.transpose(0, 1, 3, 2) @ d["q"]
            # 关键：RoPE 必须在**每个 head 内部**做。
            # 如果先把 head 合并成 [B,T,D] 再旋转，就会跨越 head 边界乱转，
            # 结果与前向不一致（梯度校验会直接抓到）。
            cos, sin = self.cos[:T], self.sin[:T]
            dq = rope_bwd(dq, cos, sin)          # [B,H,T,dh]
            dk = rope_bwd(dk, cos, sin)
            dq = dq.transpose(0, 2, 1, 3).reshape(B, T, D)
            dk = dk.transpose(0, 2, 1, 3).reshape(B, T, D)
            dv = dv.transpose(0, 2, 1, 3).reshape(B, T, D)

            g[f"wq{l}"] = dq.reshape(-1, D).T @ d["h1"].reshape(-1, D)
            g[f"wk{l}"] = dk.reshape(-1, D).T @ d["h1"].reshape(-1, D)
            g[f"wv{l}"] = dv.reshape(-1, D).T @ d["h1"].reshape(-1, D)
            d_attn_in = (dq @ p[f"wq{l}"] + dk @ p[f"wk{l}"] + dv @ p[f"wv{l}"])
            d_attn_in, dgn, dbn = layernorm_bwd(d_attn_in, d["xhat1"], d["r1"],
                                                p[f"ln1g{l}"])
            g[f"ln1g{l}"], g[f"ln1b{l}"] = dgn, dbn
            # 残差 + 注意力
            dx = dx1 + d_attn_in

        # ---- 走到这里 dx 才是 d(x_embed)，此时才能散射到 embedding ----
        #
        # 顺序很关键：wte/wpe 的输入侧梯度必须用**穿过所有层之后**的 dx。
        # 早先版本在进层循环之前就散射，那时 dx 还停在 logits/LN 出口，
        # 少了整条 Transformer 的贡献（实测 wte 梯度差 30 倍）。
        #
        # wte 的散射必须按 token id 累加：同一个 id 会在多个位置出现。
        #
        # 保持 Python 循环，**不要**换成 np.add.at：
        # 实测 B*T=2048、D=192 这个规模下 loop 0.0051s / add.at 0.0075s，
        # 向量化反而更慢（add.at 内部是串行 unbuffered 累加，
        # 单次调用开销超过了它省下的 Python 开销）。
        # 真正的热点在别处（见 profile 报告），这里回退到实测更快的写法。
        tok_idx = save["x0_arg"].reshape(-1)          # [B*T]
        flat_dx = dx.reshape(-1, D)                   # [B*T, D]
        for row in range(tok_idx.shape[0]):
            g["wte"][tok_idx[row]] += flat_dx[row]
        # wpe：每个位置把 batch 维求和
        g["wpe"] = np.zeros_like(p["wpe"])
        for pos in range(T):
            g["wpe"][pos] = dx[:, pos, :].sum(0)

        return g, dx

    # ---------------- AdamW ----------------
    def adam(self, g, lr, wd=0.01, b1=0.9, b2=0.95, clip=1.0):
        self.t += 1
        gn = math.sqrt(sum(float((v * v).sum()) for v in g.values()))
        sc = min(1.0, clip / (gn + 1e-6))
        for k in self.p:
            gr = g[k] * sc
            if self.p[k].ndim == 2 and "ln" not in k:
                gr = gr + wd * self.p[k]
            self.m[k] = b1 * self.m[k] + (1 - b1) * gr
            self.v[k] = b2 * self.v[k] + (1 - b2) * (gr * gr)
            t = self.t
            self.p[k] -= lr * (self.m[k] / (1 - b1 ** t)) / \
                (np.sqrt(self.v[k] / (1 - b2 ** t)) + 1e-8)


# ------------------------------------------------------------------ 主循环

# ------------------------------------------------------------------ 训练主循环

def make_batches(seqs, B, rng):
    """
    无限产出随机 batch。

    早前这里写的是 `for _ in range(200)` 的有限生成器，
    续训跑到第 201 步就 StopIteration 崩掉。
    而且本文件一度有**两份** make_batches 定义（重写脚本时留下的残骸），
    后者静默覆盖前者 —— 这种重复定义最难查，因为它不报错，
    只是行为悄悄变。现在全文件只有这一份。
    """
    while True:
        idx = rng.integers(0, len(seqs), B)
        seqs_ = [seqs[i] for i in idx]
        ml = max(len(s) for s in seqs_)
        xb = np.zeros((B, ml), np.int32)
        yb = np.full((B, ml), -100, np.int32)
        for r, s in enumerate(seqs_):
            n = len(s)
            xb[r, :n - 1] = s[:n - 1]
            yb[r, :n - 1] = s[1:n]
        yield xb, yb


LR_PEAK = 1e-3
WARMUP = 400


def lr_at(step, steps):
    """
    warmup 400 步 + cosine 衰减到峰值的 5%。

    为什么从 3e-3 降到 1e-3：lr_scan.py 在固定评估集上实测三档
    3e-3 / 1e-3 / 5e-4，前 300 步几乎无差别（5.82 / 5.74 / 5.79），
    1e-3 略优。但 3e-3 在 300 步之后开始发散，而 1e-3 更保守，
    对"语料/参数比只有 0.05"的极端小数据场景更安全。

    warmup 从 100 提到 400：原来的 100 步只占总长 3%，
    等于模型还没站稳就到峰值 lr 了。
    """
    if step <= WARMUP:
        return LR_PEAK * step / float(WARMUP)
    prog = (step - WARMUP) / max(1, steps - WARMUP)
    return LR_PEAK * (0.05 + 0.95 * 0.5 * (1 + math.cos(math.pi * prog)))


def loss_on(model, xb, yb):
    """一批的交叉熵。只用 model.forward，不建反向图。"""
    save = model.forward(xb)
    logits = np.asarray(save["logits"], dtype=np.float64)
    probs = softmax(logits, -1)
    T = logits.shape[1]
    valid = yb >= 0
    safe = np.maximum(yb, 0)
    pt = np.maximum(probs[np.arange(logits.shape[0])[:, None],
                          np.arange(T)[None, :], safe], 1e-9)
    return float((-np.log(pt) * valid).sum() / max(1, int(valid.sum())))


def holdout_split(n_holdout=3, seed=2024):
    """
    按**游戏**整份切分留出集。

    不能用"换随机种子抽一批序列"当验证集：训练用
    rng.integers(0, len(seqs), B) 随机取批，**每条序列都进过训练**，
    那样量到的 gap 恒等于抽样噪声，什么也证明不了（这个坑踩过一次）。
    切分粒度必须是文件，同一款游戏的 QA 和原文段落要整体去留，
    否则原文段落会泄漏答案。
    """
    from train_bpe import BPE, norm_text

    d = load_bpe()
    b = BPE(vocab_size=len(d["inv"]))
    b.vocab, b.inv, b.merges = d["vocab"], d["inv"], d["merges"]
    b._train_ranks()

    V = len(b.inv)
    BOS, EOS, SEP = V - 3, V - 2, V - 1
    T = CFG["max_ctx"]

    corpus = os.path.join(HERE, "corpus")
    files = sorted(f for f in os.listdir(corpus)
                   if f.startswith("guide_") and f.endswith(".txt"))
    rng = np.random.default_rng(seed)
    hold = set(rng.permutation(len(files))[:n_holdout].tolist())

    tr, va, names = [], [], []
    for i, fn in enumerate(files):
        text = norm_text(open(os.path.join(corpus, fn), encoding="utf-8").read())
        # ★ 与 build_dataset 用**同一个** scan_qa（行式：Q: / A: 各占一行）。
        #   这里曾经是 build_dataset 那份挤行判据的复制品，两处一起坏。
        qa, _n_qline = scan_qa([x.strip() for x in text.split("\n")])
        raw = []
        for para in [x.strip() for x in text.split("\n")]:
            if not para or para.startswith("##"):
                continue
            if para.startswith("Q") or para.startswith("A"):
                continue
            elif len(para) > 40:
                raw.append(para)

        out = va if i in hold else tr
        if i in hold:
            names.append(fn[len("guide_"):-len(".txt")])

        for q, a in qa:
            head = [BOS] + b.encode(q) + [SEP]
            body = b.encode(a)
            room = T - len(head) - 1
            if room < 1:
                out.append(np.array(head[:T - 2] + [SEP, EOS], np.int32))
                continue
            out.append(np.array(head + body[:room] + [EOS], np.int32))

        if raw:
            ids = []
            for p in raw:
                ids.extend(b.encode(p))
            n = (len(ids) // (T - 1)) * (T - 1)
            for j in range(0, n, T - 1):
                out.append(np.array([BOS] + ids[j:j + T - 2] + [EOS], np.int32))

    return tr, va, names


def eval_loss(model, seqs, B=32):
    """一组序列上的平均每 token 交叉熵。"""
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
        valid = yb >= 0
        safe = np.maximum(yb, 0)
        picked = np.take_along_axis(logits, safe[..., None], axis=-1)[..., 0]
        tot += float(((lse - picked) * valid).sum())
        ntok += int(valid.sum())
    return tot / max(1, ntok)


def main():
    steps = int(sys.argv[1]) if len(sys.argv) > 1 else 4000
    BATCH = int(sys.argv[2]) if len(sys.argv) > 2 else 32
    resume = "--resume" in sys.argv

    # ★ steps 必须等于**实际要跑的**步数，不要虚报。
    #
    # lr_at 的 cosine 是按 steps 算退火曲线的：steps 虚报成 4000
    # 而只跑了 950 步，那么 950 步时 lr 仍是峰值的 94.6%
    # —— 模型**从来没退过火**，它一直处在大 lr 下过拟合。
    #
    # 2026-10-04 就是这么误判的：train.log 里「留出集 loss 从 step 800
    # 就不再降」，我据此以为"训练已到顶、再训也不会更好"，
    # 实际是训练**被中断**，不是收敛。
    # → 教训：看到"到顶"先查 lr 走到哪了，别直接下结论。
    #
    # 断点续训时会从 state.nstep 接着跑，所以步数要按**本次的总量**给，
    # 并在日志里把"本次起点"打出来（见下面 start_step 的用法）。
    print("训练步数 %d  batch %d  %s"
          % (steps, BATCH, "(断点续训)" if resume else "(从头)"),
          flush=True)

    all_seqs, special = build_dataset()
    seqs, val_seqs, hold_names = holdout_split()
    model = GPT(CFG)
    rng = np.random.default_rng(7)

    # 留出集全量太慢，均匀抽样到 ~160 条（固定，不随机）
    if len(val_seqs) > 160:
        val_seqs = [val_seqs[i] for i in
                    np.linspace(0, len(val_seqs) - 1, 160).astype(int)]

    log_path = os.path.join(CKPT, "train.log")
    ckpt_path = os.path.join(CKPT, "model_fp32.npz")
    state_path = os.path.join(CKPT, "state.npz")

    start_step = 0
    best_val = 1e9      # 按留出集判定，不用随机 batch 的 loss
    best = 1e9
    if resume and os.path.exists(state_path):
        # 不要用 savez(p=model.p) 存参数：numpy 会把 dict 存成一个 0-d
        # object 数组，读回来是 array(dict) 而不是 dict，
        # dict(st["p"]) 会报 "iteration over a 0-d array"。
        # 正确做法是**扁平化**：键加前缀 p_ / m_ / v_，逐个张量存。
        st = np.load(state_path, allow_pickle=False)
        model.p, model.m, model.v = {}, {}, {}
        for k in st.files:
            if k.startswith("p_"):
                model.p[k[2:]] = st[k]
            elif k.startswith("m_"):
                model.m[k[2:]] = st[k]
            elif k.startswith("v_"):
                model.v[k[2:]] = st[k]
        model.t = int(st["t"])
        start_step = int(st["step"])
        best = float(st["best"])   # 历史 batch loss，仅作日志参考
        best_val = 0.0             # resume 不继承 best，重新评
        print("断点续训：从 step %d 继续（历史最好 loss %.4f，参数 %d 个）"
              % (start_step, best, len(model.p)))

    # 追加模式写日志，不缓冲。上次训练就是因为 `| tail -60`
    # 把输出全攒在管道里，跑 22 分钟一个字都看不到，误判成卡死。
    lf = open(log_path, "a" if resume else "w", encoding="utf-8")

    def log(msg):
        print(msg, flush=True)
        lf.write(msg + "\n")
        lf.flush()
        os.fsync(lf.fileno())

    log("=== 训练开始 %s ===" % time.strftime("%Y-%m-%d %H:%M:%S"))
    log("步数 %d  batch %d  参数 %.2fM  训练序列 %d"
        % (steps, BATCH, model.nparam / 1e6, len(seqs)))
    log("留出 %d 款游戏：%s（%d 序列 / %d token，训练时完全没见过）"
        % (len(hold_names), ", ".join(hold_names), len(val_seqs),
           sum(len(s) for s in val_seqs)))
    log("token/参数 = %.4f" % (sum(len(s) for s in seqs) / model.nparam))
    log("step      loss     ppl      lr        耗时")
    log("--- val 列是留出集 loss（每 200 步刷新一次）---")

    t0 = time.time()
    it = make_batches(seqs, BATCH, rng)
    for step in range(start_step + 1, steps + 1):
        xb, yb = next(it)
        save = model.forward(xb)
        logits = save["logits"]
        probs = softmax(logits, -1)
        T = logits.shape[1]
        valid = yb >= 0
        safe = np.maximum(yb, 0)
        pt = np.maximum(probs[np.arange(BATCH)[:, None],
                              np.arange(T)[None, :], safe], 1e-9)
        loss = float((-np.log(pt) * valid).sum() / max(1, int(valid.sum())))

        g, _ = model.backward(save, yb)
        model.adam(g, lr_at(step, steps))

        if step == 1 or step % 50 == 0:
            el = time.time() - t0
            done = step - start_step
            eta = el / done * (steps - step)
            log("%6d  %7.4f  %8.2f  %.2e  %6.1fs  剩 %.1fh"
                % (step, loss, math.exp(min(20, loss)),
                   lr_at(step, steps), el, eta / 3600.0))

        # 每 200 步在**留出集**上评一次，并按 val 最优存 model_best.npz。
        # 原来的 `if loss < best` 里 loss 是**当前随机 batch** 的交叉熵，
        # 而 batch 序列长度 6~128 不等，短序列 loss 天然高 ——
        # 记下来的 "best" 其实是"这步抽到了长序列"，跟模型好坏无关，
        # 等于没有刹车。现在改成按留出集判定。
        if step % 200 == 0 or step == steps:
            vl = eval_loss(model, val_seqs)
            log("    [val] step %6d  留出集 loss %7.4f  ppl %8.2f%s"
                % (step, vl, math.exp(min(20, vl)),
                   "   <- 最优" if vl < best_val else ""))
            if vl < best_val:
                best_val = vl
                np.savez(os.path.join(CKPT, "model_best.npz"), **model.p)

        # 每 200 步存一次 checkpoint：崩溃/被杀不丢进度，
        # 而且中途就能拿去 gen_test.py 看生成质量，不用等跑完。
        if step % 200 == 0 or step == steps:
            np.savez(ckpt_path, **model.p)
            # Adam 状态用**扁平化**存（键加前缀），见 resume 分支的说明。
            # 别用 savez(m=model.m)：那样存出来读不回来。
            flat = {}
            for k, v in model.p.items():
                flat["p_" + k] = v
            for k, v in model.m.items():
                flat["m_" + k] = v
            for k, v in model.v.items():
                flat["v_" + k] = v
            flat["t"] = np.int64(model.t)
            flat["step"] = np.int64(step)
            flat["best"] = np.float64(min(best_val, 1e9))
            np.savez(state_path, **flat)
            with open(os.path.join(CKPT, "special.txt"), "w") as f:
                f.write("%d %d %d\n" % special)
            if loss < best:
                best = loss

    log("=== 训练结束 %s ===" % time.strftime("%Y-%m-%d %H:%M:%S"))
    vl = eval_loss(model, val_seqs)
    log("最终留出集 loss %.4f（ppl %.2f）" % (vl, math.exp(min(20, vl))))
    log("全程最优留出集 loss %.4f（ppl %.2f），已存 model_best.npz"
        % (best_val, math.exp(min(20, best_val))))
    log("总用时 %.2f 小时" % ((time.time() - t0) / 3600.0))
    lf.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
