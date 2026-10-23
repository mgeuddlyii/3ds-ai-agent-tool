#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""
build_kb.py — 离线攻略知识库打包器

产出 guide_kb.bin：段落原文 + 倒排索引，3DS 端一个文件读入即可。

为什么要有这个
--------------
2.88M 参数的模型**记不住事实**。12 万字攻略要塞进 2.9M 参数，
需要 40:1 的压缩比 —— 做不到，强行训练的结果是每句话都胡编。
（这就是为什么很多人在 3DS 上试过小模型之后都放弃了：
 它们能学到语感，学不到知识。）

所以事实交给索引，语言交给模型。这个文件就是索引的来源。

段落切分规则
------------
按 `## ` 小节标题切，一段 = 一个小节下的全部正文。
再把过长的段落按句号切成 <= 200 字的子段，
因为 BM25 的长度归一化假设文档长度分布相对均匀。

索引构建
--------
用 BPE token（与模型共用词表）：
    for 每个段落 -> 编码 -> 统计词频 -> 建 posting (token -> [(passage, tf)])
词频封顶（tf 取 min(tf, 8)）并按段落去重，索引体积才可控。

文件格式（全部小端，3DS 端直接 fread）
----------------------------------------
    magic   "GKB1"          4B
    n_passage               i32
    n_game                  i32
    vocab                   i32
    avg_len                 f32
    text_len                i32
    pass_off[n_passage+1]   i32   每段落在 text[] 的字节范围
    pass_game[n_passage]    i16   /* ⚠️ 曾为 i8，语料 128 款以上回绕成负数 */
    post_off[vocab+1]       i32   倒排起点（用 i32，C 端读着省事）
    post[]                  i16   段落 id
    post_tf[]               i8    词频（封顶 8）
    text[text_len]          UTF-8 全部段落首尾相接
"""
import os
import sys
import struct
import pickle
import math
import re
from collections import defaultdict

HERE = os.path.dirname(os.path.abspath(__file__))
CKPT = os.path.join(HERE, "ckpt")

MAX_TF = 8
# 每个词最多索引多少段落（按 tf 取前 N）。
#
# ⚠️ 这个值**必须与 C 端 GKB_MAX_POSTING 一致**（guide_kb.h）。
# 两边不一致时的症状：一边多存了、一边读到越界 -> 检索结果随机错乱，
# 不报错。两边都是编译期常量，所以下面用注释提醒；真正的保险是
# guide_kb.h 里那条编译期断言（它卡的是 GKB_GAME_N，不是这个 ——
# 这个值不进任何 sizeof，所以只能靠这里与那边的注释互相提醒）。
#
# 为什么是 64（2026-10-04 实测定，不是猜的）：
#   31 款语料 1174 段下，真实 df 16~60 的**实体词**区若按 24 截断，
#   会丢掉 8307 条 —— 也就是大部分游戏的段落被整段切掉，
#   表现为「限定游戏检索 0 命中」（候选词表 19 处失效全是这个）。
#   提到 64 后该区**零丢弃**，索引 563 -> 607 KB（+8%）。
#
#   ⚠️ 不要无脑提到不丢任何段落：那要 1153（因为「的」在 827 段里），
#   而「的」idf 本来就接近 0，存下 827 条既占内存又不带来检索价值。
#   df>120 那个区间仍丢 12117 条，**那是应该丢的**。
#
# ★ 2026-10-05 重标（语料 31 款 -> 211 款、段数 1174 -> 2934）：
#   重跑 `tools/guia/tmp/calib_posting.py` 后，**df 分布整体右移**了：
#       旧：实体词集中在 df 16~60
#       新：df 17~32 = 460 词 / 33~64 = 603 词 / **65~120 = 682 词（最大一档）**
#   也就是说「宝可梦」「技能」这类**中等频实体词**从 64 以下漂到了 64 以上，
#   沿用 64 就开始截它们 —— 症状和 10-04 那次一模一样：
#   限定游戏检索会因为候选词被掐掉而 0 命中，不报错、只是答案错。
#
#   取 128 的依据：让 **df ≤ 128 的区间全部零丢弃**（覆盖住新增的
#   65~120 这档），实测 128 vs 64 的倒排体积 775 KB vs 493 KB，
#   在 3DS 的内存预算（程序 + 索引总共几 MB 量级）内可以接受。
#
#   ⚠️ 这个数**必须与 guide_kb.h 的 GKB_MAX_POSTING 一致**，见那边注释。
MAX_POSTING = 128
MAX_PASSAGE_CHARS = 200


def _kb_max_passages():
    """
    从 guide_kb.h 抠 GKB_MAX_PASSAGES（**同源，绝不写死**）。

    写死会犯这个错：C 侧哪天调了上限、这边的断言还按老数字判，
    于是"超顶"被当成正常、或者"没超"被误拦。
    抠不到就报错退出 —— 宁可停，也不给一个自己都不信的兜底值。
    """
    h = os.path.join(HERE, "..", "..", "shared", "nnet", "guide_kb.h")
    src = open(h, encoding="utf-8").read()
    m = re.search(r"#define\s+GKB_MAX_PASSAGES\s+(\d+)", src)
    if not m:
        raise RuntimeError("在 %s 里没找到 GKB_MAX_PASSAGES，"
                           "没法做卡上 load 的硬顶断言" % h)
    return int(m.group(1))

# 派生问答的小节名。这些段落在索引里是**噪声**：
# 它们由模板批量生成，「怎么用/要点是什么/有什么技巧」这类问法
# 出现在几十个不相关的段落里，会把真正的答案挤下去。
# 实测：不排除它们，命中率 3/12；排除后 12/12。
SKIP_SECTIONS = ("常见问题（衍生）",)


def load_bpe():
    sys.path.insert(0, HERE)
    from train_bpe import BPE
    with open(os.path.join(CKPT, "bpe.pkl"), "rb") as f:
        d = pickle.load(f)
    b = BPE(vocab_size=len(d["inv"]))
    b.vocab, b.inv, b.merges = d["vocab"], d["inv"], d["merges"]
    b._train_ranks()
    return b


def strip_qa(body):
    """
    去掉段落里的 `Q: ... A: ...` 行，只留叙述性正文。

    为什么：派生问答是**训练**用的（教模型问答格式），
    但**索引**里它们是噪声 —— 一个「Q: 游戏难度大吗 A: ...」的片段
    会被「游戏」这个词命中，把真正讲操作的段落挤下去。
    训练要格式，检索要事实，两者必须分开。
    """
    out = []
    for line in body.split("\n"):
        s = line.strip()
        if s.startswith("Q:") or s.startswith("A:"):
            continue
        out.append(line)
    return "\n".join(out).strip()


def split_long(text, limit=MAX_PASSAGE_CHARS):
    """把长段按中文句末标点切开，避免单段过长稀释 BM25。"""
    if len(text) <= limit:
        return [text]
    out, buf = [], ""
    for ch in text:
        buf += ch
        if ch in "。！？；\n" and len(buf) >= limit // 2:
            out.append(buf.strip())
            buf = ""
    if buf.strip():
        # 尾巴接上去
        if out and len(out[-1]) + len(buf.strip()) <= limit * 3 // 2:
            out[-1] += buf.strip()
        else:
            out.append(buf.strip())
    return [o for o in out if len(o) >= 10]


def main():
    bpe = load_bpe()
    V = len(bpe.inv)
    corpus = os.path.join(HERE, "corpus")
    files = sorted(f for f in os.listdir(corpus)
                   if f.startswith("guide_") and f.endswith(".txt"))

    passages = []           # (text, game_id)
    game_id = 0
    for fn in files:
        cur_title, buf = None, []
        blocks = []

        def flush():
            if cur_title is None:
                return
            body = "\n".join(buf).strip()
            if body:
                blocks.append((cur_title, body))

        for line in open(os.path.join(corpus, fn), encoding="utf-8"):
            line = line.rstrip("\n")
            if line.startswith("## "):
                flush()
                cur_title, buf = line[3:].strip(), []
            else:
                buf.append(line)
        flush()

        for title, body in blocks:
            if any(s in title for s in SKIP_SECTIONS):
                continue
            body = strip_qa(body)
            if len(body) < 20:
                continue
            # 标题单独作为上下文拼在段首，检索命中标题词时更有用
            text = title + "。" + body
            for piece in split_long(text):
                passages.append((piece, game_id))
        game_id += 1
        print("%-28s 累计段落 %d" % (fn, len(passages)))

    n = len(passages)
    n_game = game_id
    text_blob = []
    pass_off = [0]
    total = 0
    for t, _ in passages:
        b = t.encode("utf-8")
        text_blob.append(b)
        total += len(b)
        pass_off.append(total)
    text_all = b"".join(text_blob)

    # ---- 建倒排 ----
    post = defaultdict(list)          # token -> [(passage, tf)]
    lens = []
    for i, (t, _) in enumerate(passages):
        ids = bpe.encode(t)
        lens.append(len(ids))
        tf = defaultdict(int)
        for tid in ids:
            # 跳过 BOS/EOS/SEP 和单字节噪声，避免污染索引
            if tid >= V - 3:
                continue
            tf[tid] += 1
        for tid, c in tf.items():
            if len(post[tid]) < MAX_POSTING * 8:   # 先多收，后面按 tf 截断
                post[tid].append((i, min(c, MAX_TF)))

    # 每个 token 只保留 tf 最高的 MAX_POSTING 个段落
    post_final = {}
    dropped = 0
    for tid, lst in post.items():
        lst.sort(key=lambda pc: (-pc[1], pc[0]))
        if len(lst) > MAX_POSTING:
            dropped += len(lst) - MAX_POSTING
        post_final[tid] = lst[:MAX_POSTING]

    # ---- 写文件 ----
    post_off = [0] * (V + 1)
    post_ids, post_tfs = [], []
    for tid in range(V):
        for (pid, tf) in post_final.get(tid, []):
            post_ids.append(pid)
            post_tfs.append(tf)
        post_off[tid + 1] = len(post_ids)

    out = os.path.join(CKPT, "guide_kb.bin")
    avg_len = sum(lens) / max(1, len(lens))
    header_size = 4 + 4 * 4 + 4 * 4 + 4 * (n + 1) + n + 4 * (V + 1)
    with open(out, "wb") as f:
        f.write(b"GKB1")
        # n_passage, n_game, vocab, text_len
        f.write(struct.pack("<4i", n, n_game, V, total))
        f.write(struct.pack("<f", avg_len))
        f.write(struct.pack("<%di" % (n + 1), *pass_off))
        # ★ 必须是 i16 不是 i8：game_id 是语料文件名字母序下标，
        #   211 款时最大 210，i8 在 128 回绕成负数 → 限定游戏过滤全错。
        f.write(struct.pack("<%dh" % n, *[g for _, g in passages]))
        f.write(struct.pack("<%di" % (V + 1), *post_off))
        f.write(struct.pack("<%dh" % len(post_ids), *post_ids))
        f.write(struct.pack("<%db" % len(post_tfs), *post_tfs))
        f.write(text_all)

    # ★★ 卡上能不能 load 的硬顶（同源：从 guide_kb.h 抠，绝不写死）。
    #    gkb_load 里 `n > GKB_MAX_PASSAGES` 直接 return -3，
    #    知识库整个废掉、检索全挂；而离线校验读的是 .bin，根本不走 gkb_load，
    #    所以这儿不拦，校验器照样全绿（2026-10-05 就是这么漏过去的）。
    cap = _kb_max_passages()
    if n > cap:
        print("★ 构建中止：段落 %d 超过 GKB_MAX_PASSAGES=%d"
              % (n, cap))
        print("  产出的索引在卡上 gkb_load 会 return -3（检索全废）。")
        print("  要么抬高 shared/nnet/guide_kb.h 的 GKB_MAX_PASSAGES"
              "（同时注意 gkb_search 里那两个数组是 static，见该处注释），")
        print("  要么让语料薄一些。")
        return 1

    size = os.path.getsize(out)
    print("---")
    print("段落 %d  游戏 %d  词表 %d" % (n, n_game, V))
    print("平均段落长度 %.1f token" % avg_len)
    print("倒排条目 %d（截断丢弃 %d）" % (len(post_ids), dropped))
    print("guide_kb.bin = %.1f KB" % (size / 1024))
    print("  text %d B / post_off %d B / post %d B"
          % (total, 4 * (V + 1), len(post_ids) * 3))
    return 0


if __name__ == "__main__":
    sys.exit(main())
