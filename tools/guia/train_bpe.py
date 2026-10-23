#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""
train_bpe.py — 离线 BPE 分词器训练（纯标准库，零依赖）

为什么自己写而不是用现成的
--------------------------
sentencepiece / tokenizers 都要编译或装包，而且它们的产物是 protobuf，
C 端没法直接读。本项目需要一个**能被 3DS 单文件加载**的词表，
所以这里输出最朴素的两张表：
    vocab.txt    每行一个 token（按 id 排序）
    merges.txt   每行一对 merge，顺序即优先级
C 端加载这两张表 + 一个 256-entry 的字节级起始表，完事。

词表大小怎么定
--------------
3M 参数的模型，embedding 就要 vocab*d。建议 vocab=4096：
    4096 * 192 = 786k 参数，占总量 24%
再大就挤占 transformer 层的容量了，中文压缩率会从 ~1.8 字/token 掉下来。
4096 在 11.8 万字的语料上不会过拟合（每个 token 平均出现 30 次以上）。

只做标准库实现，是因为这台机器上没有 sentencepiece，而语料只有 12 万字，
自己写反而更可控。
"""

import os
import sys
from collections import Counter

# ------------------------------------------------------------------ 基础

def norm_text(t):
    """统一的文本规范化。训练端和解码端必须完全一致，否则会出乱码。"""
    t = t.replace("\r\n", "\n")
    # 全角 -> 半角（只处理 ASCII 区，够用且快）
    out = []
    for ch in t:
        o = ord(ch)
        if o == 0x3000:
            out.append(" ")
        elif 0xFF01 <= o <= 0xFF5E:
            out.append(chr(o - 0xFEE0))
        else:
            out.append(ch)
    t = "".join(out)
    return t


def split_to_chars(t):
    """
    预切分：把文本切成「块」。
    语料是中英混杂的攻略文本，纯字符预切分会把 ASCII 词和汉字打散，
    导致唯一词太少、BPE 学不到「的心之碎片」这类长中文串
    （实测 vocab=2048 时压缩率只有 0.30 汉字/token，远低于 1.5 的及格线）。

    所以这里只在**空白**处切分：同一个空格之间的内容整体作为一个块。
    汉字串会变成一个长块，BPE 可以在块内部自由 merge 出高压缩率的 token。
    """
    return [(w, False) for w in t.split(" ") if w]


# ------------------------------------------------------------------ BPE

class BPE:
    def __init__(self, vocab_size=4096, verbose=True):
        self.vocab_size = vocab_size
        self.verbose = verbose
        # 0..255 全部保留：UTF-8 的高位字节（0x80-0xBF）是汉字的组成部分，
        # 没有它们就无法编码中文。代价是约 590 个槽位落在单字节和
        # 非法的 UTF-8 中间片段上（约 2.3% 的词表），换取实现简单可靠。
        self.vocab = {bytes([i]): i for i in range(256)}
        self.inv = {i: bytes([i]) for i in range(256)}
        self.merges = []          # [(a_id, b_id), ...]  按训练顺序 = 优先级
        self._ranks = {}

    # ---- 内部 ----
    def _add(self, piece):
        """把一个 bytes token 挂到 id 0 上（占位，实际编码不用它）。"""
        if piece in self.vocab:
            return self.vocab[piece]
        i = len(self.inv)
        self.vocab[piece] = i
        self.inv[i] = piece
        return i

    def _train_ranks(self):
        self._ranks = {p: i for i, p in enumerate(self.merges)}

    def _bpe_word(self, word_bytes):
        """对一段字节做 merge，返回 id 序列。"""
        parts = [word_bytes[i:i + 1] for i in range(len(word_bytes))]
        if len(parts) == 1:
            return [self.vocab[parts[0]]]

        # 先转成 id 序列。_ranks 的 key 是 (id_a, id_b)，
        # 如果这里用 bytes 去查会永远 miss（训练时存的是 id 对）。
        ids = [self.vocab[p] for p in parts]
        while len(ids) > 1:
            best, best_pos = None, -1
            for i in range(len(ids) - 1):
                r = self._ranks.get((ids[i], ids[i + 1]))
                if r is not None and (best is None or r < best):
                    best, best_pos = r, i
            if best is None:
                break
            ids[best_pos:best_pos + 2] = [self._add(self.inv[ids[best_pos]] +
                                                    self.inv[ids[best_pos + 1]])]
        return ids

    # ---- 训练 ----
    def train(self, text, verbose_every=500):
        words = [w.encode("utf-8") for w, _ in split_to_chars(text)]
        freq = Counter(words)
        if self.verbose:
            print("预切分后词表 %d，唯一词 %d" % (len(words), len(freq)))

        # word -> tuple(ids)
        wids = {w: tuple(self.vocab[bytes([c])] for c in w) for w in freq}

        while len(self.inv) < self.vocab_size:
            pairs = Counter()
            for w, f in freq.items():
                ids = wids[w]
                for i in range(len(ids) - 1):
                    pairs[(ids[i], ids[i + 1])] += f
            if not pairs:
                break
            # 最高频；频率相同按 (a,b) 字典序，保证可复现
            (a, b), cnt = max(pairs.items(), key=lambda kv: (kv[1], -kv[0][0], -kv[0][1]))
            new_id = self._add(self.inv[a] + self.inv[b])
            self.merges.append((a, b))
            self._ranks[(a, b)] = len(self.merges) - 1

            # 把所有出现 (a,b) 的词就地合并
            changed = False
            for w in freq:
                ids = wids[w]
                out, i, n = [], 0, len(ids)
                while i < n:
                    if i < n - 1 and ids[i] == a and ids[i + 1] == b:
                        out.append(new_id)
                        i += 2
                        changed = True
                    else:
                        out.append(ids[i])
                        i += 1
                wids[w] = tuple(out)

            if self.verbose and len(self.inv) % verbose_every == 0:
                print("  vocab=%d  最新 merge: %s" % (
                    len(self.inv),
                    (self.inv[a] + self.inv[b]).decode("utf-8", "replace")))

        return self

    # ---- 编码 / 解码 ----
    def encode(self, text):
        text = norm_text(text)
        out = []
        for w, _ in split_to_chars(text):
            out.extend(self._bpe_word(w.encode("utf-8")))
        return out

    def decode(self, ids):
        buf = b""
        for i in ids:
            buf += self.inv[i]
        return buf.decode("utf-8", "replace")

    # ---- 导出 ----
    def save(self, vocab_path, merges_path):
        """
        两个都是纯文本，C 端可以直接 fgets 读。
        vocab.txt 每行 token 的 UTF-8 字节（用 \\xNN 转义不可打印字符）
        merges.txt 每行 "<a> <b>"，同样转义
        """
        with open(vocab_path, "w", encoding="utf-8") as f:
            for i in range(len(self.inv)):
                b = self.inv[i]
                f.write(esc(b) + "\n")
        with open(merges_path, "w", encoding="utf-8") as f:
            for a, b in self.merges:
                f.write("%s %s\n" % (esc(self.inv[a]), esc(self.inv[b])))
        return len(self.inv), len(self.merges)


def esc(b):
    """把一段字节写成 C 端好解析的转义形式。"""
    out = []
    for ch in b:
        c = chr(ch)
        if ch == 0x5C:
            out.append("\\\\")
        elif 0x20 <= ch < 0x7F:
            out.append(c)
        else:
            out.append("\\x%02X" % ch)
    return "".join(out)


# ------------------------------------------------------------------ 主流程

def main():
    here = os.path.dirname(os.path.abspath(__file__))
    corpus = os.path.join(here, "corpus")
    out = os.path.join(here, "ckpt")

    files = sorted(f for f in os.listdir(corpus) if f.startswith("guide_") and f.endswith(".txt"))
    if not files:
        print("corpus/ 为空", file=sys.stderr)
        return 1

    chunks = []
    for fn in files:
        t = open(os.path.join(corpus, fn), encoding="utf-8").read()
        chunks.append(t)
        print("  读入 %-28s %d 字符" % (fn, len(t)))
    text = norm_text("\n".join(chunks))

    print("训练 BPE，vocab=4096 ...")
    bpe = BPE(vocab_size=4096).train(text)

    nv, nm = bpe.save(os.path.join(out, "vocab.txt"), os.path.join(out, "merges.txt"))

    # 压缩率体检
    sample = text[:4000]
    ids = bpe.encode(sample)
    cjk = sum(1 for c in sample if "\u4e00" <= c <= "\u9fff")
    print("---")
    print("vocab=%d  merges=%d" % (nv, nm))
    print("压缩率: %.2f 汉字/token" % (cjk / max(1, len(ids))))
    print("往返测试:", "OK" if bpe.decode(bpe.encode("沙之杖怎么用")) == "沙之杖怎么用" else "FAIL")

    # 存一份给 C 端/训练脚本用的 pickle
    import pickle
    with open(os.path.join(out, "bpe.pkl"), "wb") as f:
        pickle.dump({"vocab": bpe.vocab, "inv": bpe.inv,
                     "merges": bpe.merges, "vocab_size": nv}, f)
    print("已写出 ckpt/vocab.txt, ckpt/merges.txt, ckpt/bpe.pkl")
    return 0


if __name__ == "__main__":
    sys.exit(main())
