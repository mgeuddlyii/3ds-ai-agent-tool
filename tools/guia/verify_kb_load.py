#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""
verify_kb_load.py —— **卡上能不能 load 起来**（离线版 gkb_load 前置判据）

═══ 为什么单独有这个脚本（2026-10-05 的血案）══════════════════════
索引 `ckpt/guide_kb.bin` 里 n_passage = **4496**，而 guide_kb.c 的
`gkb_load()` 里是 `if (n > GKB_MAX_PASSAGES) return -3;`——
上限当时是 4096。**知识库整个加载失败，3DS 上的攻略检索一点都出不来。**

而 verify_all_offline.py 当时 7/7 全绿：那些校验全是在 Python 里
直接解析 .bin 的，**根本不走 gkb_load**。于是"数据全对"和"卡上跑不起来"
这两个结论同时成立，谁也没证伪谁 —— 典型的永远绿的守卫。

这个脚本只干一件事：**按 gkb_load 的同源判据，逐个字段验一遍。**
不看内容对不对，只看卡上那几行 `return -N` 会不会被触发。

判据逐条对齐 guide_kb.c:762-798（错误码注释就是那边那几个 -1..-6）：
    -1 raw_len < 20            -2 magic 不是 GKB1
     -3 n<=0 || n>上限 || V<=0 || text_len<=0
     -4 n_game != GAME_N       -5 GAME_N > 255
     -6 n_post 越界

⚠️ 本机无 native gcc，验不了 C 代码本身，验的是**喂给 C 的那份数据**。
   结论一律要标「未经模拟器/真机验证」。

用法：
    python verify_kb_load.py
"""
from __future__ import print_function

import os
import re
import struct
import sys
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
HDR = os.path.join(ROOT, "shared", "nnet", "guide_kb.h")
KB_BIN = os.path.join(HERE, "ckpt", "guide_kb.bin")
KB_C = os.path.join(ROOT, "shared", "nnet", "nnet_kb_data.c")


def macro(name):
    """从 guide_kb.h 抠宏值（同源，绝不写死；抠不到就报错退出）。"""
    src = open(HDR, encoding="utf-8").read()
    m = re.search(r"#define\s+%s\s+(\S+)" % re.escape(name), src)
    if not m:
        raise RuntimeError("guide_kb.h 里找不到 %s" % name)
    tok = m.group(1)
    if not tok.isdigit():
        raise RuntimeError("guide_kb.h 里的 %s 不是数字（=%r），没法比" % (name, tok))
    return int(tok)


def gkz_payload():
    """
    从 nnet_kb_data.c 里抠出 gkb_data[] 并解 GKZ。
    ⚠️ 不能照抄"找到 'gkb_data[' 到下一个 '}'"：那个正则会跨过数组末尾
       去匹配很远的地方（这个项目里已经因此删错过一次代码）。
       这里数的是 **逗号个数** 之外的信号：先定位数组名，再找该行结尾的 ';'。
    """
    src = open(KB_C, encoding="utf-8").read()
    i = src.find("gkb_data[]")
    if i < 0:
        raise RuntimeError("nnet_kb_data.c 里没有 gkb_data[]")
    # 数组声明以 " = {" 开始；找它后面的 "};"
    s = src.find("= {", i)
    if s < 0:
        raise RuntimeError("gkb_data[] 声明里找不到 '= {'")
    s = src.index("{", s) + 1
    e = src.find("};", s)
    if e < 0:
        raise RuntimeError("gkb_data[] 数组没有以 '};' 正常结尾（文件被截断？）")
    body = src[s:e]
    blob = bytes(int(x) for x in re.findall(r"\d+", body))
    if blob[:3] != b"GKZ":
        raise RuntimeError("gkb_data[] 开头不是 GKZ（=%r），不是本脚本预期的产物"
                           % blob[:4])
    comp_len = int.from_bytes(blob[7:11], "little")
    return zlib.decompress(blob[11:11 + comp_len])


def header(raw):
    """按 gkb_load 的字段顺序（小端）解头。"""
    if len(raw) < 20:
        raise RuntimeError("raw_len %d < 20（gkb_load 会 return -1）" % len(raw))
    if raw[:4] != b"GKB1":
        raise RuntimeError("magic 不是 GKB1（=%r，-2）" % raw[:4])
    # 字段顺序严格照 guide_kb.c:767-771：n, n_game, V, text_len 四个 i32，
    # 后面才是 avg(f32)。多解一个 float 会把偏移顶歪。
    n, n_game, V, text_len = struct.unpack("<4i", raw[4:20])
    return dict(n=n, n_game=n_game, V=V, text_len=text_len)


def check(raw, which, max_pass, game_n, errs):
    h = header(raw)
    n, ng, V, tl = h["n"], h["n_game"], h["V"], h["text_len"]
    print("  %-10s n_passage=%-6d n_game=%-4d vocab=%-5d text_len=%d"
          % (which, n, ng, V, tl))

    def bad(code, msg):
        errs.append("%s: %s  → gkb_load 会 return %s" % (which, msg, code))

    if n <= 0:
        bad("-3", "n_passage=%d <= 0" % n)
    elif n > max_pass:
        bad("-3", "n_passage=%d > GKB_MAX_PASSAGES=%d" % (n, max_pass))
    if V <= 0:
        bad("-3", "vocab=%d <= 0" % V)
    if tl <= 0:
        bad("-3", "text_len=%d <= 0" % tl)
    if ng != game_n:
        bad("-4", "n_game=%d != GAME_N=%d（索引与 C 侧别名表不同步）" % (ng, game_n))
    if game_n > 255:
        bad("-5", "GAME_N=%d > 255（N 侧编码放不下）" % game_n)

    # -6：post_off[V] 也就是倒排条目总数
    #
    # ⚠️ 偏移算错过一次（2026-10-05），而且**是沉默的**：
    #    v1 写成 `4+4+4+4+4 + 4*(n+1) + n`，三处错 —— 少了 avg f32 的 4 字节、
    #    post_off 长度用 n+1 而不是 V+1、还多加了 n。它照样能解出一个数，
    #    只是那个数是 post 区中间的乱码；碰巧 < 2^24 就绿着（语料 5208 段时绿的）。
    #    语料一涨到 5253 段，解出的乱码变 1728079616 > 2^24，才暴露。
    #    「永远绿的守卫」又一个实例 —— 判据算错偏移，还假装自己在测倒排条目数。
    #    正确偏移对照 build_kb.py:254-265 的写出顺序：
    #      magic(4) | 4i(16) | avg f32(4) | pass_off[n+1]i | pass_game[i16 × n] |
    #      post_off[V+1]i | post[u16] | tf[i8] | text
    #    ⚠️ 中间那段 pass_game（int16 x n）最容易漏 —— 漏了它解出来是个负数。
    if n > 0 and tl > 0:
        base = 4 + 16 + 4 + 4 * (n + 1) + 2 * n      # 到 post_off[0] 之前
        try:
            arr = struct.unpack_from("<%di" % (V + 1), raw, base)
        except struct.error:
            bad("-6", "解不到 post_off[]（文件被截断，base=%d）" % base)
            return h
        off = arr[V]
        if off < 0 or off > (1 << 24):
            bad("-6", "倒排条目 %d 越界（应落在 0..%d）" % (off, (1 << 24) - 1))
        else:
            print("              倒排条目 %d（<= 2^24，OK）" % off)
        # 自断言：post_off 必须单调不减（每个词的 posting 数只增不减）。
        # 偏移算错时数组必然乱跳 —— 上一版偏移漏了 pass_game，
        # 解出的是 post 区中间的数据，单调性就是被这条抓住的。
        if any(arr[i + 1] < arr[i] for i in range(len(arr) - 1)):
            bad("-6", "post_off 数组单调性被破坏，前 4 项 %s —— 偏移算错，"
                      "解出来的不是倒排起点表" % (list(arr[:4]),))
        else:
            print("              post_off 单调性 OK（%d 项）" % len(arr))
    return h


def selftest():
    """
    证明判据会红 —— 没有这一步，本脚本可能永远绿（本项目已踩过两次）。

    做法：拿一份**真的**内嵌索引，逐条把 gkb_load 判据对应的字段改坏，
    看 check() 有没有抓到、抓到的是不是**对的那一条**（抓错一条也是假绿）。
    """
    max_pass, game_n = macro("GKB_MAX_PASSAGES"), macro("GKB_GAME_N")
    raw = bytearray(gkz_payload())
    ok = True

    def run(label, mut, want):
        # ★ 每条 fixture 都从**重新解出来的一份**开始：就地改 bytearray
        #   会把后面所有 fixture 一起污染（我第一次就栽在这，
        #   最后是"只有第一条红"，看着最像真红，其实全是假象）。
        buf = bytearray(gkz_payload())
        out = mut(buf)
        # mut 分两种：就地改（返回 None）与直接返回新字节（如截短）。
        # 返回 None 时**必须传回 buf 本身**，传 gkz_payload() 会把改动丢掉
        # —— 那正好造出"fixture 明明改了却一点没变"的假绿。
        buf = out if out is not None else buf
        buf = bytes(buf)
        errs = []
        try:
            check(bytes(buf), "fixture", max_pass, game_n, errs)
        except Exception as exc:          # noqa: BLE001
            errs = ["抛异常: %s" % exc]
        got = " ; ".join(errs)
        hit = want in got
        nonlocal ok
        ok = ok and hit
        print("SELFTEST %s %-14s 抓到 %r  %s"
              % ("ok  " if hit else "FAIL", label, want, got[:70]))

    # ★ 偏移必须照 header() 里的顺序：GKB1(0:4) n(4:8) n_game(8:12) V(12:16)
    #   text_len(16:20) avg(20:24)。写错一个就会改到别的字段上 ——
    #   那样 fixture 看着"红了"，其实红的是另一条判据（比全绿更骗人）。
    def mut_n(buf):                       # 段落数超顶 → -3
        struct.pack_into("<i", buf, 4, max_pass + 1)

    def mut_game(buf):                    # 游戏数与别名表不同步 → -4
        struct.pack_into("<i", buf, 8, game_n + 1)

    def mut_magic(buf):                   # magic 错 → -2
        buf[0] = ord("X")

    def mut_len(buf):                     # raw_len < 20 → -1
        return bytes(buf)[:8]             # 截短，触发长度判据

    def mut_tlen(buf):                    # text_len <= 0 → -3
        struct.pack_into("<i", buf, 16, 0)

    # 逐条必须命中**对应那条**判据（错一条也算假绿）
    run("段落超顶", mut_n, "-3")
    run("游戏数不同步", mut_game, "-4")
    run("magic 错", mut_magic, "-2")
    run("raw_len 过小", mut_len, "-1")
    run("text_len<=0", mut_tlen, "-3")

    # 合格样本不许被误杀（同样用一份新的，别让上面那条 fixture 污染它）
    errs = []
    check(gkz_payload(), "fixture", max_pass, game_n, errs)
    if errs:
        ok = False
        print("SELFTEST FAIL 合格样本被误杀：%s" % errs)
    else:
        print("SELFTEST ok   合格样本通过（未被误杀）")
    print("SELFTEST %s" % ("ALL PASS" if ok else "FAILED"))
    return 0 if ok else 1


def main():
    if "--selftest" in sys.argv:
        return selftest()
    max_pass = macro("GKB_MAX_PASSAGES")
    game_n = macro("GKB_GAME_N")
    print("=== gkb_load 前置判据（同源抠 guide_kb.h）===")
    print("GKB_MAX_PASSAGES = %d    GKB_GAME_N = %d" % (max_pass, game_n))
    print()

    errs = []
    if not os.path.exists(KB_BIN):
        print("缺 %s" % KB_BIN)
        return 1
    disk = header(open(KB_BIN, "rb").read())

    if not os.path.exists(KB_C):
        print("缺 %s（卡上内嵌的那份）" % KB_C)
        return 1
    emb = header(gkz_payload())

    a = check(open(KB_BIN, "rb").read(), "磁盘 .bin", max_pass, game_n, errs)
    print()
    b = check(gkz_payload(), "卡上内嵌", max_pass, game_n, errs)

    print()
    # 两份不同代 = 卡上跑的还是旧索引（等于没装新语料）
    if (a["n"], a["n_game"], a["V"]) != (b["n"], b["n_game"], b["V"]):
        errs.append("磁盘 .bin（段落 %d/游戏 %d）与卡上内嵌（段落 %d/游戏 %d）"
                    "不同代 —— 漏跑 export_kb_zlib.py，卡上还是旧索引"
                    % (a["n"], a["n_game"], b["n"], b["n_game"]))

    print("=" * 66)
    if errs:
        print("FAIL：卡上 gkb_load 会被判定失败（攻略检索出不来）")
        for e in errs:
            print("  - %s" % e)
        print("=" * 66)
        print("⚠️ 未经模拟器/真机验证（本机无 native gcc，验的是喂给 C 的数据）")
        return 1
    print("PASS：磁盘 .bin 与卡上内嵌两份都能过 gkb_load 的字段判据，且同代")
    print("=" * 66)
    print("⚠️ 未经模拟器/真机验证：这只能证明「卡上加载得了」，证明不了检索结果对。")
    return 0


if __name__ == "__main__":
    sys.exit(main())
