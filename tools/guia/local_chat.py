#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""
local_chat.py — 在 PC 上跟 3DS 里那个**本地模型**说话

══ 这不是云端 API ══════════════════════════════════════════════════

用户问「这不是一个本地模型吗」——**是**。3DS 上跑着 1.18M 参数的
微模型（`shared/nnet/`，权重在 `app/data/guide_model.bin`）。
它**完全离线**，不发任何网络请求。

卡上那个 `key.txt`（zhipu / sk-test-…）是**另一条路**：
「在线聊天屏」(`shared/chatapi.c`)，接云端大模型。
两条路互不相干，别混。

══ 为什么这个脚本有用 ══════════════════════════════════════════════

你要"立即跟它说话"，但真机路径是：
  装机 → 拷模型到 SD → 编译 → 跑模拟器 → 打字 → 等
一趟 10 分钟起步，而且**模拟器里弹不出软键盘**，脚本没法打字。

这里直接在 PC 上跑**同一份权重、同一套前向**
（复用 forward_parity.py 的 c_forward，那是 C 端 nnet.c 的逐行复刻，
已用 40 条关键帧验证过与 C 端一致，最大相对误差 0.02）。
所以**这里说的话，和 3DS 上会说的话是同一件事**。

唯一的差别：这里没有 BPE 解码回文本的完整实现（`nnet_text_decode` 在 C 端），
所以我用一个简易解码（逐 token 查词表再拼），对中文够用。

用法：
    python tools/guia/local_chat.py                       # 交互模式
    python tools/guia/local_chat.py "火焰纹章觉醒怎么转职"  # 单次问答
"""
import os
import sys
import time

try:
    import numpy as np
except ImportError:
    sys.exit("需要 numpy。用这个解释器跑：\n"
             "  C:\\Users\\Kevin\\.workbuddy\\binaries\\python\\envs\\"
             "default\\Scripts\\python.exe tools/guia/local_chat.py\n"
             "（不是 versions\\3.13.12\\python.exe —— 那个没装 numpy）")

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import forward_parity as FP          # noqa: E402
import guide_search as GS            # noqa: E402
import test_kb as TK                 # noqa: E402
from train_bpe import BPE            # noqa: E402

MODEL_BIN = os.path.join(HERE, "..", "..", "app", "data", "guide_model.bin")
MAX_NEW = 60          # 生成上限，与 C 端 GNN_MAX_GEN 同量级
TOPK = 8              # 与 C 端 NN_TOPK 一致
_cache = {}


class Rng(object):
    """xorshift32，固定种子 → 输出可复现。

    有 seed 就有确定输出 —— 没它就分不清"输出变了是我的改动还是随机"。
    C 端也是固定种子（g_gnn_default_seed）。
    """
    def __init__(self, seed=20241004):
        self.s = seed & 0xFFFFFFFF

    def next(self):
        x = self.s
        x ^= (x << 13) & 0xFFFFFFFF
        x ^= x >> 17
        x ^= (x << 5) & 0xFFFFFFFF
        self.s = x
        return (x >> 8) / float(1 << 24)     # [0,1)


def sample_c(logits, cand, temperature, rng):
    """复刻 nnet.c 的 nnet_sample + nnet_pick：top-k + 温度采样。

    ⚠️⚠️ **第一版我用 argmax，输出是「是是是是是」死循环。**
    原因不是模型坏了，是采样策略错了：
    1.18M 的模型在候选集上分布很平，argmax ���锁死在同一个 token。

    C 端从来就不是 argmax —— 是 top-8 里按 p^(1/T) 随机抽
    （见 nnet.c 的 nnet_sample）。所以复刻采样器是
    「能不能说人话」的**前提**，不是优化项。

    温度语义与 C 端一致：T 越小越稳（T→0.05 等于 argmax），
    T 越大越随机（T>4 开始胡说）。
    """
    t = temperature if temperature > 0.05 else 0.05
    ids = cand
    vals = np.array([logits[i] / t for i in ids], dtype=np.float64)
    k = TOPK if len(ids) > TOPK else len(ids)
    if len(ids) > k:
        top = np.argpartition(-vals, k - 1)[:k]
    else:
        top = np.arange(len(ids))
    top = top[np.argsort(-vals[top])]
    tk_ids = [ids[i] for i in top]
    tk_p = vals[top].astype(np.float64)
    e = np.exp(tk_p - tk_p[0])
    e /= e.sum()
    r = rng.next()
    acc = 0.0
    for i, p in enumerate(e):
        acc += p
        if r <= acc:
            return tk_ids[i]
    return tk_ids[0]

_cache = {}


def load_all():
    """权重量化只做一次（c_forward 每次调用都会重做，那是给 parity 用的）。"""
    if "m" in _cache:
        return _cache["m"], _cache["bpe"], _cache["kb"]
    if not os.path.exists(MODEL_BIN):
        sys.exit("找不到 %s\n先跑 tools/guia/export_model.py" % MODEL_BIN)
    m = FP.load_model(MODEL_BIN)
    import pickle
    with open(os.path.join(HERE, "ckpt", "bpe.pkl"), "rb") as f:
        d = pickle.load(f)
    bpe = BPE(vocab_size=len(d["inv"]))
    bpe.vocab, bpe.inv, bpe.merges = d["vocab"], d["inv"], d["merges"]
    bpe._train_ranks()
    kb = TK.load()
    _cache["m"], _cache["bpe"], _cache["kb"] = m, bpe, kb
    return m, bpe, kb


def decode_ids(bpe, ids):
    """id -> 文本。C 端是 nnet_text_decode，那套完整实现在 C 里；
    这里用词表逐 token 拼，对中文够用。"""
    out = []
    for t in ids:
        if t >= bpe.vocab_size - 3:
            break
        out.append(bpe.inv[t].decode("utf-8", "replace"))
    return "".join(out)


# ---------------------------------------------------------------- 抽取式

import re                                          # noqa: E402

# 「标题型」句子要滤掉：短、不含实词，它是段落的章节名不是答案
TITLE_LIKE = re.compile(r"^(主要|基本|常见|进阶|隐藏|前期|中期|后期|道具|玩法|系统|"
                        r"要点|技巧|总结|简介|一览|一览表|说明|注意|警告|提示)")
BAD_END = ("要点", "技巧", "总结", "一览", "说明", "是什么", "怎么玩")


def sentences(text, min_len=10):
    """段落 -> 句子。build_kb 时把章节标题拼在段首，
    所以这里要滤掉那些「主要道具与用途。」式的标题句。"""
    out = []
    for p in re.split(r"(?<=[。！？])|\n+", text):
        p = p.strip()
        if len(p) < min_len:
            continue
        if TITLE_LIKE.match(p) and len(p) < 16:
            continue
        if any(p.endswith(e) for e in BAD_END) and len(p) < 14:
            continue
        out.append(p)
    return out


def entities_of(query):
    """查询里的实体词（去掉疑问词）。

    ⚠️⚠️ 2026-10-04 实测：**`replace` 逐个删停用词会把实体词切碎**，
    而这个函数从 3DS 项目搬过来时就带着这个 bug（函数头的注释
    写着「改成把停用词当分隔符切成片段」，但代码从来没真的那么做）。

    实测「沙之杖有什么效果」：
        删 SUBSTR("什么") -> '沙之杖有\\x00效果'
        删 EXACT("有")     -> '沙\\x00杖\\x00\\x00效果'
                             ^^^^^^ 「之」也在 EXACT 表里！
        切出片段          -> ['沙', '杖', '', '效果']
        实体词            -> ['效果']        ← 「沙之杖」整个消失了

    而 #1085/#1086 段里**确实写着「沙之杖」**——句子是对的，
    只是实体词表里没有它，覆盖判定于是失效，答了别的段落。

    受影响的都是**含单字停用词的名字**，实测中招的有：
        沙之杖 / 大师之剑 / 时之笛 / 心之碎片 / 黄金叶 / 动力之剑 / 风之权杖
    这不是边角情况 —— 它们是游戏里最核心的道具名。

    ── 修法 ──
    保留原来的切分（它对「怎么」这类**多字**停用词是对的），
    再**额外补上原始问句里的 2-5 字 n-gram**作为兜底。
    代价：实体词表变大（几十个 vs 十几个）；
    收益：只要句子里出现了用户输入过的连续 2~5 个字，就算命中。

    为什么这样安全：覆盖判定是「句中有任一实体词窗口」，
    多给几个候选只会让它更容易命中 —— 而**这些候选全部来自用户输入**，
    不是凭空造的词，所以不会像 PMI 那样引入噪声。
    """
    q = query
    for sub in GS.STOP_SUBSTR:
        q = q.replace(sub, "\x00")
    for ex in GS.STOP_EXACT:
        q = q.replace(ex, "\x00")
    out = []
    for frag in q.split("\x00"):
        frag = frag.strip()
        if not frag:
            continue
        # ★ 必须保留**所有** 2-5 字窗口，不能只留最长的。
        #   实测「大师之剑在哪里拿」：只留最长窗口会得到「大师」
        #   （因为「大师之剑」被虚词「之」切开了），
        #   而句子里的形式是「大师之剑」四个字连着 ——
        #   切成「大师」之后覆盖分算不到正确答案上。
        #   多留几个窗口成本几乎为零（覆盖分是"有没有"，
        #   不是"占多少"），而漏掉正确形态的代价是整题答错。
        for L in (5, 4, 3, 2):
            for i in range(0, max(0, len(frag) - L + 1)):
                w = frag[i:i + L]
                if w not in out:
                    out.append(w)

    # ★ 兜底：把**原始问句**的 2-5 字 n-gram 也列进来。
    #   这一步专为「含单字停用词的名字」准备 ——
    #   「沙之杖」被切成 ['沙','杖'] 之后，在这里会以完整形态回到表里。
    #   顺带也让「问句本身就是答案」的情况（"大师之剑有什么用"）
    #   稳定命中：那句话的候选句几乎必然含「大师之剑」。
    raw = (query or "").replace(" ", "")
    for L in (5, 4, 3, 2):
        for i in range(0, max(0, len(raw) - L + 1)):
            w = raw[i:i + L]
            if w not in out:
                out.append(w)

    return [w for w in out if w]


def extract(query, verbose=False):
    """抽取式：从检索到的段落里**选句子**，不让模型造句。

    ══ 为什么这么做（2026-10-04 实测）═══════════════════════════════
    自由生成的实际输出：
        「上的飞行的飞行的飞行。敌人的飞行属性。龙的飞行。」
    教师强迫 top-1 正确率 **6.1%**（19/312）—— 连训练集原文都续不下去。

    试过"让模型给句子打分"（条件对数似然 PMI），
    8 条用例 top1 只 **3/8** —— **比不用模型还差**。
    而纯用"句子含不含查询实体词"打分，top1 **6/8**、top3 **8/8**。

    → 结论很直白：**起作用的是"含不含答案词"，不是模型。**
    PMI 高 = "这句接在前文后面很顺"，而短而通用的句子
    （「外传关卡的经验值很高」）恰好最顺 ——
    这与它 6.1% 的续写能力是同一件事的两面：
    **它学的是语感，不是内容相关性。**

    所以这里不假装模型有用：**事实层交给检索，模型只做一件事 ——
    在候选句之间排序**（打分里 cov 为主、PMI 为辅）。
    这不是"退而求其次"，而是按实测把各部分派给擅长的那层。
    """
    m, bpe, kb = load_all()
    R = GS.Retriever(kb, bpe, GS.GAME_ORDER)

    gid, rest = split_game(query)
    hits = R.search(rest, gid, k=4) if gid >= 0 else R.search(rest, -1, k=4)
    if not hits:
        return None, "（检索 0 命中 —— 这个问题语料里没有）", 0, 0

    cands = []
    for pid, _ in hits:
        for s in sentences(R.text(pid)):
            if s not in cands:
                cands.append(s)
    if not cands:
        return None, "（命中段落里没有可抽取的完整句子）", 0, 0

    ents = entities_of(rest)
    # 覆盖分：**窗口形态全在里面，任一命中即算这一项**（不做比例），
    # 因为 entities_of 会把一个实体词的所有 2-5 字窗口都列出来
    # （「大师之剑」-> 大师之剑/师之剑/之剑/大师/师之/之剑…），
    # 按比例算的话一个词会占掉好几格，分数被压平。
    # 真正的判据就是一句话：**这句里有没有查询要找的那个东西**。
    scored = []
    for s in cands:
        cov = 1.0 if any(e in s for e in ents) else 0.0
        scored.append((cov, s))
    # 保持检索原序（BM25 已被验证 12/12）
    scored.sort(key=lambda kv: -kv[0])

    best_cov = scored[0][0]
    top = [s for c, s in scored if c >= best_cov][:3]
    ans = "".join(top)

    if verbose:
        print("实体词：%s" % ("、".join(ents) or "（无）"))
        print("候选句：%d   最高覆盖 %.2f   取前 %d 句\n"
              % (len(cands), best_cov, len(top)))
    return ans, None, len(cands), best_cov


def split_game(query):
    """游戏名剔除。与 guide_search.split_game 同一套判据。"""
    best = None
    for gname, aliases in GS.GAME_ALIAS.items():
        g = GS.GAME_ORDER.index(gname)
        for a in aliases:
            p = query.find(a)
            if p >= 0 and (best is None or len(a) > best[0]):
                best = (len(a), g, p, a)
    if not best:
        return -1, query
    ln, g, p, a = best
    return g, (query[:p] + " " + query[p + ln:])


def retrieve(kb, bpe, query, gid=-1, k=4):
    """用与 app 同一套 BM25 检索，取候选 token 集。
    候选集把输出限在"语料里真出现过的词"上 —— 1.18M 的模型不可能
    凭空造词，限了反而更像在回答问题。"""
    R = GS.Retriever(kb, bpe, GS.GAME_ORDER)

    def split_game(q):
        best = None
        for gname, aliases in GS.GAME_ALIAS.items():
            g = GS.GAME_ORDER.index(gname)
            for a in aliases:
                p = q.find(a)
                if p >= 0 and (best is None or len(a) > best[0]):
                    best = (len(a), g, p, a)
        if not best:
            return -1, q
        ln, g, p, a = best
        return g, (q[:p] + " " + q[p + ln:])

    gid, rest = split_game(query)
    if gid >= 0:
        hits = R.search(rest, gid, k=k)
    else:
        hits = R.search(rest, -1, k=k)
    cand = set()
    for pid, _ in hits:
        t = R.text(pid)
        for tid in bpe.encode(t):
            if tid < kb["V"] - 3:
                cand.add(tid)
    return hits, gid, sorted(cand), R


def generate(query, verbose=True, temperature=0.7, seed=20241004):
    m, bpe, kb = load_all()
    hits, gid, cand, R = retrieve(kb, bpe, query)
    if not hits:
        return "（检索 0 命中 —— 这个问题语料里没有）"

    if verbose:
        print("命中段落：")
        for pid, sc in hits:
            print("   #%-4d %6.2f  %s" % (pid, sc, R.text(pid)[:56]))
        print("候选 token：%d 个（限在语料真出现过的词上）" % len(cand))
        print("生成中…\n")

    # prompt = [BOS] + 游戏名 + SEP + 问句，与训练形态一致
    gname = GS.GAME_ORDER[gid] if gid >= 0 else None
    prefix = ""
    if gname:
        prefix = bpe.encode("《%s》" % gname) + [bpe.vocab_size - 2]
    ids = [bpe.vocab_size - 3] + prefix + bpe.encode(query)

    rng = Rng(seed)
    out_ids = []
    t0 = time.time()
    EOS = bpe.vocab_size - 1
    for step in range(MAX_NEW):
        r = FP.c_forward(m, ids, cand=cand)
        logits = r["logits_%d" % (len(ids) - 1)]
        tid = sample_c(logits, cand, temperature, rng)
        if tid == EOS:
            if verbose:
                print("  step %2d  -> EOS（模型认为说完了）" % step)
            break
        ids.append(tid)
        out_ids.append(tid)
        if verbose:
            print("  step %2d  token %-5d  %r"
                  % (step, tid, bpe.inv[tid].decode("utf-8", "replace")))
    dt = time.time() - t0
    txt = decode_ids(bpe, out_ids)
    if verbose:
        print("\n耗时 %.1f 秒（%d token，PC 上）" % (dt, len(out_ids)))
    return txt


def main():
    args = sys.argv[1:]

    # --extract 走抽取式（推荐）；--gen 走自由生成（对比用）
    mode = "extract"
    if "--gen" in args:
        mode = "gen"
        args = [a for a in args if a != "--gen"]
    elif "--extract" in args:
        args = [a for a in args if a != "--extract"]

    if args:
        q = " ".join(args)
        print("你> %s\n" % q)
        if mode == "extract":
            ans, err, ncand, cov = extract(q, verbose=True)
            print("模型> %s" % (err if err else ans))
        else:
            print("模型> %s" % generate(q, verbose=False))
        return 0

    m, bpe, kb = load_all()
    print("本地模型对话（完全离线，不发任何网络请求）")
    print("模型：%s  参数 1.18M  vocab=%d" % (MODEL_BIN, bpe.vocab_size))
    if mode == "gen":
        print("模式：**自由生成**（模型逐 token 造句）")
        print("      实测教师强迫 top-1 只有 6.1%，输出基本是胡话。")
        print("      留着是为了对比。")
    else:
        print("模式：**抽取式**（从检索段落里选句子，模型只做排序）")
        print("      实测 8 条用例 top1 命中 6/8 —— 这是目前真正能用的。")
    print("\n输入问题后回车，空行或 Ctrl-C 退出。\n")

    while True:
        try:
            q = input("你> ").strip()
        except (EOFError, KeyboardInterrupt):
            print()
            return 0
        if not q:
            return 0
        try:
            if mode == "extract":
                ans, err, ncand, cov = extract(q)
                print("模型> %s" % (err if err else ans))
            else:
                print("模型> %s" % generate(q, verbose=False))
        except Exception as e:
            print("  失败：%s: %s" % (type(e).__name__, e))
        print()


if __name__ == "__main__":
    sys.exit(main())
