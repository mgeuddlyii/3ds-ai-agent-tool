#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
gen_pinyin_table.py —— 生成拼音输入法用的「音节 → 候选汉字」表

── 为什么要在 PC 上生成、再内嵌 ──
3DS 上没有拼音数据，也不可能在卡上放一份 GB 级的词库再现场解析。
所以把"哪些字读这个音"这件事**在构建机上算好**，压成一张定长表编进程序。
运行时只剩"按音节查一段连续字节"，没有解析、没有内存分配。

── 字从哪来 ──
用 **GB2312 一级汉字（3755 字）**：
  · 一级字是按**拼音排序**的常用字，正好是输入法最该有的那一批；
  · 二级字（次常用）覆盖率低但体积翻倍，不划算，本轮不收。
  · 多音字会在多个音节下各出现一次 —— 这是**对的**：
    "行" 既该在 hang 下也该在 xing 下，输入法的候选本来就该这样。

── 同一音节里怎么排序（★ 踩过坑，别改回去）──
第一版按 **GB2312 码位升序**，理由是"一级字表大致按常用度排"。
实测**不成立**：码位序在同一个音节内是按声调/笔画排的，于是
    ni  -> 呢 妮 霓 倪 泥 尼   （"你" 根本不在前几名）
    hao -> 皋 镐 壕 嚎 豪 毫   （"好" 不在）
—— 一个候选里没有"你"和"好"的拼音输入法是不能用的。

第二版按 **项目自己的中文语料统计字频**（tools/guia/corpus/ 下 53MB 攻略 + AGENT.md）。
用自建语料而不是通用字频库，有两个理由：
  · 通用字频库（wordfreq）装不上、且要额外数据；本机试过，pip 直接超时。
  · 这个输入法主要服务"聊游戏、查攻略、问操作"，用攻略语料算出来的高频字
    恰好就是用户要打的那批字（"装""档""卡""键""屏" 排在前面）。
  · 语料里没出现的字排在后面，仍可输入 —— 不是白名单，只是排序。

── 二进制格式（全部小端）──
    u16  n_syl
    n_syl × { char name[8]; u32 off; u16 n; }
    pool:  UTF-8 汉字连续排列，每个 3 字节（BMP 常用字都是 3 字节）

  · name 是无声调小写拼音（"zhong"），NUL 补齐到 8 字节；
  · off/n 指向 pool 里的区间：off 是字节偏移，n 是**字数**不是字节数。
  · 运行时查表就是 strcmp + 取 pool[off .. off+n*3)。

用法：
    python tools/gen_pinyin_table.py            # 写到 app/data/pinyin_tbl.bin
    python tools/gen_pinyin_table.py --stats    # 只打印统计
"""

import argparse
import io
import os
import re
import struct
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, "app", "data", "pinyin_tbl.bin")

MAX_PER_SYL = 16      # 每个音节最多收几个候选（面板一屏显示 6 个，可翻）


def level1_hanzi():
    """GB2312 一级汉字：区 16..55，位 1..94。按码位序返回。"""
    out = []
    for hi in range(0xB0, 0xD8):          # 16..55 区
        for lo in range(0xA1, 0xFF):
            try:
                ch = bytes([hi, lo]).decode("gb2312")
            except UnicodeDecodeError:
                continue
            if len(ch) == 1 and "\u4e00" <= ch <= "\u9fff":
                out.append(ch)
    return out


CORPUS_DIR = os.path.join(ROOT, "tools", "guia", "corpus")


def corpus_freq():
    """统计中文语料里的字频。返回 {char: count}。

    只数 CJK 统一表意文字（\\u4e00-\\u9fff），标点/数字/英文一律不计 ——
    它们本来就不会成为候选。读不动的文件直接跳过：语料是构建机上的一份
    工作集，缺几个文件不影响"常用字排序"这个用途。
    """
    from collections import Counter

    cnt = Counter()
    sources = []

    d = CORPUS_DIR
    if os.path.isdir(d):
        for name in sorted(os.listdir(d)):
            if name.endswith(".txt"):
                sources.append(os.path.join(d, name))
    sources.append(os.path.join(ROOT, "AGENT.md"))

    for path in sources:
        try:
            txt = io.open(path, encoding="utf-8", errors="ignore").read()
        except OSError:
            continue
        cn = [c for c in txt if "\u4e00" <= c <= "\u9fff"]
        cnt.update(cn)

    return cnt


def build():
    try:
        from pypinyin import pinyin, Style
    except ImportError:
        print("!! 需要 pypinyin：pip install pypinyin", file=sys.stderr)
        return None

    chars = level1_hanzi()
    syl = {}                              # syllable -> [chars]

    for ch in chars:
        try:
            py = pinyin(ch, style=Style.NORMAL, heteronym=True)
        except Exception:
            continue
        if not py or not py[0]:
            continue
        for s in py[0]:
            s = re.sub(r"[^a-z]", "", s.lower())
            # 过长的读音（"ng" 之类）也留着 —— 用户会打；但要能放进 name[8]
            if not s or len(s) > 7:
                continue
            lst = syl.setdefault(s, [])
            if ch not in lst:
                lst.append(ch)

    # 按语料字频降序排，同频再按码位 —— 见文件头「同一音节里怎么排序」
    freq = corpus_freq()
    for k in syl:
        syl[k].sort(key=lambda c: (-freq.get(c, 0), c))
        syl[k] = syl[k][:MAX_PER_SYL]

    return chars, syl


def emit(chars, syl, path):
    names = sorted(syl.keys())
    pool = bytearray()
    entries = []
    for name in names:
        off = len(pool)
        for ch in syl[name]:
            pool += ch.encode("utf-8")
        entries.append((name, off, len(syl[name])))

    hdr = struct.pack("<H", len(entries))
    idx = bytearray()
    for name, off, n in entries:
        nb = name.encode("ascii")
        idx += nb + b"\x00" * (8 - len(nb))
        idx += struct.pack("<IH", off, n)

    blob = hdr + bytes(idx) + bytes(pool)
    with open(path, "wb") as f:
        f.write(blob)
    return len(entries), len(pool), len(blob)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--stats", action="store_true")
    ap.add_argument("--out", default=OUT)
    a = ap.parse_args()

    r = build()
    if not r:
        return 1
    chars, syl = r

    n, pool, total = emit(chars, syl, a.out) if not a.stats else (len(syl), 0, 0)

    print("一级汉字  : %d 个" % len(chars))
    print("音节数    : %d" % len(syl))
    print("候选总数  : %d" % sum(len(v) for v in syl.values()))
    if not a.stats:
        print("写出      : %s（%d 字节 = 头 %d + 索引 %d + 字池 %d）"
              % (a.out, total, 2, len(syl) * 14, pool))
        # 抽几个音节核对，避免"生成成功但内容错位"
        for probe in ("ni", "hao", "zhong", "shi"):
            if probe in syl:
                print("  %-6s -> %s" % (probe, "".join(syl[probe][:6])))
    return 0


if __name__ == "__main__":
    sys.exit(main())
