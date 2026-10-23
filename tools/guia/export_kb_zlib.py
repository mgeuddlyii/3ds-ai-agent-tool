#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""
export_kb_zlib.py — 把知识库压进 C 源（体积优化）

为什么要压
----------
CIA 里最大的一块是 gkb_data[]：2,550,259 字节（占 CIA 近 4 倍）。
它是 836 段中文攻略正文 + 倒排索引，全是自然语言文本，
zlib 实测能压到 **305,387 字节（省 88%）**。

诚实说明：**这不会让3DS 跑得更快**。CIA 装在 SD 卡上，不占运行内存，
SD 卡还有 1558 GB 空闲。省下的只是"装 CIA 快两秒"和一点 SD 占用。
做它的理由是体积本身有意义（要塞进 CIA 的话），不是速度。

怎么做
------
gkb_data[] 前面加一个小头：
    "GKZ" | u32 raw_len | u32 comp_len | zlib 数据
运行时 gkb_bind() 先用 zlib 的 uncompress 解到堆上，
之后所有指针逻辑**一行都不用改**（和现在指向 gkb_data 一样）。

为什么能这么写：gkb_bind() 拿到的是 (const unsigned char*)p，
只要 p 指向解出来的缓冲区，后面pass_off / post / text 那些
指针偏移全部照旧成立。

libz 不用额外装：3ds-curl 已经依赖它，本来就链进二进制了。
"""
import os
import sys
import zlib
import struct

HERE = os.path.dirname(os.path.abspath(__file__))
KB_BIN = os.path.join(HERE, "ckpt", "guide_kb.bin")
OUT_C = os.path.abspath(os.path.join(HERE, "..", "..", "shared", "nnet",
                                     "nnet_kb_data.c"))

MAGIC = b"GKZ"


def c_bytes(name, data):
    """把字节数组转成 C 源码，每行 16 个字节。"""
    out = ["const unsigned char %s[] __attribute__((aligned(4))) = {" % name]
    for i in range(0, len(data), 16):
        row = ",".join("%d" % b for b in data[i:i + 16])
        out.append("    " + row + ",")
    out.append("};")
    out.append("const unsigned int %s_len = %d;" % (name, len(data)))
    return "\n".join(out)


def main():
    if not os.path.exists(KB_BIN):
        print("找不到 %s，先跑 export_kb.py" % KB_BIN)
        return 1

    raw = open(KB_BIN, "rb").read()
    if raw[:4] != b"GKB1":
        print("源数据不是 GKB1（%r），导出中止" % raw[:4])
        return 2

    comp = zlib.compress(raw, 9)
    blob = MAGIC + struct.pack("<II", len(raw), len(comp)) + comp

    hdr = """/*
 * nnet_kb_data.c — 由 tools/guia/export_kb_zlib.py 自动生成，请勿手改。
 *
 * 布局（本文件自带头，不另设变量）：
 *     "GKZ" | u32 raw_len | u32 comp_len | zlib(raw_len 字节)
 * 运行时 guide_kb.c 的 gkb_bind() 先 uncompress 到堆上，
 * 之后全部指针偏移与未压缩版本完全一致。
 *
 * 压缩前 %d 字节 -> 压缩后 %d 字节（省 %.0f%%）。
 * 语料是中文攻略正文，zlib 对 UTF-8 中文压缩率很高。
 */
""" % (len(raw), len(blob), 100.0 * (1 - len(blob) / len(raw)))

    with open(OUT_C, "w", encoding="utf-8", newline="\n") as f:
        f.write(hdr)
        f.write("\n")
        f.write(c_bytes("gkb_data", blob))
        f.write("\n")

    #往返自检：压��再解，必须和原文逐字节相同。
    back = zlib.decompress(comp)
    if back != raw:
        print("往返自检失败：解压后 %d 字节 != 原始 %d 字节"
              % (len(back), len(raw)))
        return 3

    print("原始    %9d 字节" % len(raw))
    print("压缩后%9d 字节  (省 %.0f%%)" % (len(blob), 100.0 * (1 - len(blob) / len(raw))))
    print("写出    %s  (%d 字节 C 源)" % (OUT_C, os.path.getsize(OUT_C)))
    print("往返自检 OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())