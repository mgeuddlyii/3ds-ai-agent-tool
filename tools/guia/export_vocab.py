#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""
export_vocab.py — 把 BPE 词表导出成 3DS 端能 fscanf 的纯文本

为什么要转成文本而不是二进制
----------------------------
C 端的加载器用 fscanf/fgetc 逐段读，省掉一套二进制解析，
而且出错时能直接看出「读到哪一行不对」，比二进制好排查。
体积差几百字节，对 3DS 无所谓。

格式（纯文本，3DS 用 fopen 读）：
    GVB1
    <vocab> <n_merge> <bos> <eos> <sep>
    每行一个 token：  <字节数> <十六进制字节，2 位，无空格>
    然后 n_merge 行： <a_id> <b_id>

注意 token 字节用十六进制而不是原文 —— 原文可能是任意 UTF-8
甚至半个码点（词表里确实有 128..255 的裸字节），直接写会乱码。
"""
import os
import sys
import pickle

HERE = os.path.dirname(os.path.abspath(__file__))
CKPT = os.path.join(HERE, "ckpt")
PROJ = os.path.abspath(os.path.join(HERE, "..", ".."))
DATA = os.path.join(PROJ, "app", "data")


def main():
    with open(os.path.join(CKPT, "bpe.pkl"), "rb") as f:
        d = pickle.load(f)
    inv = d["inv"]
    merges = d["merges"]
    V = len(inv)
    bos, eos, sep = V - 3, V - 2, V - 1

    os.makedirs(DATA, exist_ok=True)
    dst = os.path.join(DATA, "guide_vocab.txt")
    with open(dst, "w", encoding="ascii") as f:
        f.write("GVB1\n")
        f.write("%d %d %d %d %d\n" % (V, len(merges), bos, eos, sep))
        for i in range(V):
            b = inv[i]
            f.write("%d %s\n" % (len(b), b.hex()))
        for (a, c) in merges:
            f.write("%d %d\n" % (a, c))

    size = os.path.getsize(dst)
    # 自检：逆序读回来必须能还原
    lines = open(dst, encoding="ascii").read().split("\n")
    assert lines[0] == "GVB1"
    hdr = lines[1].split()
    assert int(hdr[0]) == V, "词表大小对不上"
    p = 2
    for i in range(V):
        parts = lines[p].split()
        n = int(parts[0])
        raw = bytes.fromhex(parts[1])
        assert len(raw) == n, "token %d 长度不符" % i
        assert raw == inv[i], "token %d 内容不符" % i
        p += 1
    for k in range(len(merges)):
        a, c = lines[p].split()
        assert (int(a), int(c)) == merges[k], "merge %d 对不上" % k
        p += 1

    print("词表 %d  merge %d  BOS=%d EOS=%d SEP=%d" % (V, len(merges), bos, eos, sep))
    print("guide_vocab.txt = %.1f KB，往返自检通过" % (size / 1024))
    print("已写出 %s" % dst)
    return 0


if __name__ == "__main__":
    sys.exit(main())
