#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""
export_kb.py — 把 guide_kb.bin 转成 C 静态数组

产出 shared/nnet/nnet_kb_data.c：
    const unsigned char gkb_data[] = { ... };

为什么走 bin2s 内嵌而不是读 SD卡
--------------------------------
1. 3DS 的可用堆很紧张（老型号约 32-40MB，且要给 GPU/音频留）。
   guide_kb.bin 现在 400KB，将来扩到更多游戏会到 MB 级。
2. .cia 里已经有字体（font.bin 302KB）用了同样的机制，统一。
3. 离线要求：SD 卡拔了也得能用。

代价：换知识库要重新编译 .cia。这是离线单机应用可接受的。

注意：这个脚本**不重新生成索引**，索引由 build_kb.py 负责。
这里只做格式转换 + 校验，避免两个脚本都写 guide_kb.bin 造成职责重叠。
"""
import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
CKPT = os.path.join(HERE, "ckpt")
PROJ = os.path.abspath(os.path.join(HERE, "..", ".."))
OUT = os.path.join(PROJ, "shared", "nnet", "nnet_kb_data.c")


def parse(blob):
    """按 build_kb.py 写出的顺序拆字段，并做完整性校验。"""
    if blob[:4] != b"GKB1":
        raise ValueError("magic 不是 GKB1")
    off = 4
    n, n_game, V, text_len = struct.unpack_from("<4i", blob, off); off += 16
    avg_len, = struct.unpack_from("<f", blob, off); off += 4
    pass_off = struct.unpack_from("<%di" % (n + 1), blob, off)
    off += 4 * (n + 1)
    pass_game = struct.unpack_from("<%dh" % n, blob, off); off += n   # i16: 语料>127 款
    post_off = struct.unpack_from("<%di" % (V + 1), blob, off)
    off += 4 * (V + 1)
    n_post = post_off[V]
    post_id = struct.unpack_from("<%dh" % n_post, blob, off)
    off += 2 * n_post
    post_tf = struct.unpack_from("<%db" % n_post, blob, off)
    off += n_post
    text = blob[off:off + text_len]
    off += text_len
    # 校验：必须正好读完，不能有多余字节（说明解析漏了字段）
    if off != len(blob):
        raise ValueError("解析后剩余 %d 字节未消费，字段布局对不上" % (len(blob) - off))
    # 校验：pass_off 单调不减且末值 == text_len
    if pass_off[-1] != text_len:
        raise ValueError("pass_off 末值 %d != text_len %d" % (pass_off[-1], text_len))
    return dict(n=n, n_game=n_game, V=V, text_len=text_len, avg_len=avg_len,
                n_post=n_post)


def emit(blob, info):
    lines = []
    a = lines.append
    a("/* 自动生成，请勿手改。由 tools/guia/export_kb.py 从 ckpt/guide_kb.bin 转出。 */")
    a("/* 段落 %d / 游戏 %d / 词表 %d / 倒排 %d / 正文 %d 字节 */"
      % (info["n"], info["n_game"], info["V"], info["n_post"], info["text_len"]))
    a("const unsigned char gkb_data[] __attribute__((aligned(4))) = {")
    body = []
    for i in range(0, len(blob), 16):
        row = ", ".join("0x%02x" % b for b in blob[i:i + 16])
        body.append("    " + row)
    a(",\n".join(body))
    a("};")
    a("const unsigned int gkb_data_len = %dU;" % len(blob))
    a("")
    return "\n".join(lines)


def main():
    src = os.path.join(CKPT, "guide_kb.bin")
    if not os.path.exists(src):
        print("找不到 %s，先跑 build_kb.py" % src, file=sys.stderr)
        return 1
    with open(src, "rb") as f:
        blob = f.read()
    info = parse(blob)
    print("校验通过：段落 %d  游戏 %d  词表 %d  倒排 %d  正文 %d 字节"
          % (info["n"], info["n_game"], info["V"], info["n_post"], info["text_len"]))
    txt = emit(blob, info)
    with open(OUT, "w", encoding="utf-8") as f:
        f.write(txt)
    print("已写出 %s（%.1f KB C 源）" % (OUT, os.path.getsize(OUT) / 1024.0))
    return 0


if __name__ == "__main__":
    sys.exit(main())
