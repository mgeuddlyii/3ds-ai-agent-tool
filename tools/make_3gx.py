#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
make_3gx.py —— 把插件 ELF 打成 .3gx（纯 Python，不需要 C++ 编译器）

── 为什么要有这个 ──
官方工具 3gxtool 是 C++ 程序，要 PC 编译器才能编。本机只有 ARM 交叉编译器
（arm-none-eabi），装 mingw 又要几百 MB。而 3gx 的**不加密路径**其实只是
"读 ELF 段 → 拼一个固定头 → 顺序写出"，用 Python 一百多行就能干完，
而且比装工具链更可控（不污染 devkitPro 环境）。

格式来源：3gxtool 的 includes/3gx.hpp + sources/ElfConvert.cpp（逐字段对照）。
所有结构都是 PACKED（无对齐填充），头固定 **148 字节**。

── 不加密路径做什么 ──
  · checksum = 三段(代码/只读/数据) u32 求和
  · 三段"解密/交换载荷"都填同一段 36 字节 ARM 桩（源码里的 defaultFunc）
  · 因此不需要 --enclib 那个私有加密库

用法：
    python tools/make_3gx.py plugin/aiconsole/aiconsole.elf \
        --out dist/AIConsolePlugin.3gx \
        --title "AI Console" --author "AIConsole" \
        --summary "In-game AI assistant"
"""

import argparse
import io
import os
import struct
import sys

MAGIC = 0x3230303024584733        # "3GX$0002"

# 源码 ElfConvert.cpp 的 defaultFunc，36 字节（9 个 u32，最后一个 0xE320F000 是 NOP 终止符）
DEFAULT_FUNC = bytes([
    0xC0, 0x40, 0x2D, 0xE9, 0x00, 0x70, 0xA0, 0xE3,
    0x04, 0x60, 0x90, 0xE4, 0x06, 0x70, 0x87, 0xE0,
    0x01, 0x00, 0x50, 0xE1, 0xFB, 0xFF, 0xFF, 0x1A,
    0x07, 0x00, 0xA0, 0xE1, 0xC0, 0x80, 0xBD, 0xE8,
    0x00, 0xF0, 0x20, 0xE3,
])
assert len(DEFAULT_FUNC) == 36

# _3gx_Infos::MemorySize 枚举：0=5MiB 1=2MiB 2=10MiB
MEM = {"5MiB": 0, "2MiB": 1, "10MiB": 2}


def parse_elf(path):
    """读 ELF32 的程序头，按 p_flags 分出 code/rodata/data/bss。

    这三类的判定抄自 ElfConvert.cpp 的 switch (s.flags)：5/4/6。
    顺序也有约束（code 必须最先、data 必须最后），这里一并检查 ——
    顺序错了 3gxtool 会 die，而它给的错是 "Code segment must be the first"，
    看不出是哪一段，所以在这里报得更具体。
    """
    img = io.open(path, "rb").read()
    if img[:4] != b"\x7fELF":
        raise SystemExit("不是 ELF：%s" % path)
    if img[4] != 1:
        raise SystemExit("不是 ELF32（ei_class=%d）" % img[4])

    e_entry, e_phoff = struct.unpack_from("<II", img, 0x18)
    e_phentsize, e_phnum = struct.unpack_from("<HH", img, 0x2A)

    code = rodata = data = None
    bss = 0
    base = None                         # 第一段的 vaddr（3gx 的加载基址）

    for i in range(e_phnum):
        off = e_phoff + i * e_phentsize
        p_type, p_offset, p_vaddr, p_paddr, p_filesz, p_memsz, p_flags, p_align = \
            struct.unpack_from("<IIIIIIII", img, off)
        if p_type != 1:                     # PT_LOAD
            continue

        if base is None:
            base = p_vaddr

        seg = img[p_offset:p_offset + p_filesz]
        if p_flags == 5:                    # R+X = code
            if code is not None:
                raise SystemExit("有两段 code，3gx 只允许一段")
            if rodata is not None or data is not None:
                raise SystemExit("code 段必须是第一段")
            code = seg
        elif p_flags == 4:                  # R = rodata
            if rodata is not None:
                raise SystemExit("有两段 rodata")
            if data is not None:
                raise SystemExit("rodata 必须在 data 之前")
            rodata = seg
        elif p_flags == 6:                  # RW = data + bss
            if data is not None:
                raise SystemExit("有两段 data")
            data = seg
            bss = p_memsz - p_filesz
        else:
            raise SystemExit("不认识的段 flags=%d（只认 5/4/6）" % p_flags)

    if code is None:
        raise SystemExit("没有 code 段")
    return {
        "code": code, "rodata": rodata or b"", "data": data or b"",
        "bss": bss, "entry": e_entry, "base": base or 0,
    }


def checksum(blob):
    """defaultFunc 算的就是"所有 u32 之和"（见 ElfConvert.cpp 的注释汇编）。
    长度不是 4 的倍数时尾部按小端补齐 —— 与那段 ARM 桩的循环一致。"""
    if len(blob) % 4:
        blob = blob + b"\0" * (4 - len(blob) % 4)
    return sum(struct.unpack("<%dI" % (len(blob) // 4), blob)) & 0xFFFFFFFF


def build_3gx(elf, title, author, summary, description, mem, targets,
              version, swap_not_needed, private_mem, out_path):
    seg = parse_elf(elf)
    code, rodata, data = seg["code"], seg["rodata"], seg["data"]

    blob = code + rodata + data
    cks = checksum(blob)

    # ---- flags 位域（与 _3gx_Infos 的 anonymous struct 同序）----
    flags = 0
    flags |= 1 << 0                     # embeddedExeDecryptFunc = 1
    flags |= 1 << 1                     # embeddedSwapEncDecFunc = 1
    flags |= (MEM[mem] & 3) << 2        # memoryRegionSize
    flags |= (0 & 3) << 4               # compatibility = 0 (CONSOLE)
    flags |= 0 << 6                     # eventsSelfManaged = 0
    flags |= (1 if swap_not_needed else 0) << 7
    flags |= (1 if private_mem else 0) << 8

    # ---- 先算长度，再排布 ----
    HDR = 148
    parts = []                          # (name, bytes) 顺序即文件顺序
    off = HDR

    def add(name, b):
        nonlocal off
        start = off
        parts.append((name, b))
        off += len(b)
        return start

    # 三个载荷（不加密路径下都是同一段桩）
    exe_dec_off = add("decExePayload", DEFAULT_FUNC)
    swap_enc_off = add("encSwapPayload", DEFAULT_FUNC)
    swap_dec_off = add("decSwapPayload", DEFAULT_FUNC)

    # 字符串区（len 含结尾 \0；偏移是文件绝对偏移）
    def add_str(text):
        if not text:
            return 0, 0
        b = text.encode("utf-8") + b"\0"
        o = add("str", b)
        return len(b), o

    author_len, author_off = add_str(author)
    title_len, title_off = add_str(title)
    summ_len, summ_off = add_str(summary)
    desc_len, desc_off = add_str(description)

    # 目标 Title ID 列表（0 = 所有游戏）
    if targets:
        tgt_bytes = b"".join(struct.pack("<I", t) for t in targets)
        tgt_off = add("targets", tgt_bytes)
    else:
        tgt_bytes = b""
        tgt_off = 0

    # 代码段 16 字节对齐（源码里就是 padding = 16 - (offset & 0xF)）
    pad = 16 - (off & 0xF)
    add("pad", b"\0" * pad)

    code_off = add("code", code)
    rodata_off = add("rodata", rodata)
    data_off = add("data", data)

    # ---- 组头 ----
    h = bytearray()
    h += struct.pack("<QII", MAGIC, version, 0)          # magic/version/reserved
    h += struct.pack("<8I", author_len, author_off,
                     title_len, title_off,
                     summ_len, summ_off,
                     desc_len, desc_off)
    h += struct.pack("<I", flags)
    h += struct.pack("<I", cks)
    h += struct.pack("<4I", 0, 0, 0, 0)                  # builtInDecExeArgs
    h += struct.pack("<4I", 0, 0, 0, 0)                  # builtInSwapEncDecArgs
    h += struct.pack("<10I", code_off, rodata_off, data_off,
                     len(code), len(rodata), len(data), seg["bss"],
                     exe_dec_off, swap_enc_off, swap_dec_off)
    h += struct.pack("<2I", len(targets) if targets else (1 if tgt_off == 0 else 0),
                     0)                                  # targets.count/titles(保留)
    h += struct.pack("<3I", 0, 0, 0)                     # symtable（不写符号）
    assert len(h) == HDR, "头长度算错了：%d != %d" % (len(h), HDR)

    os.makedirs(os.path.dirname(os.path.abspath(out_path)) or ".", exist_ok=True)
    with io.open(out_path, "wb") as fh:
        fh.write(h)
        for _, b in parts:
            fh.write(b)

    return {
        "size": os.path.getsize(out_path),
        "code": len(code), "rodata": len(rodata), "data": len(data),
        "bss": seg["bss"], "checksum": cks, "entry": seg["entry"],
        "base": seg["base"],
        "flags": flags,
    }


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("elf")
    ap.add_argument("--out", required=True)
    ap.add_argument("--title", default="AI Console")
    ap.add_argument("--author", default="AIConsole")
    ap.add_argument("--summary", default="In-game AI assistant")
    ap.add_argument("--description", default="")
    ap.add_argument("--mem", default="5MiB", choices=list(MEM))
    ap.add_argument("--target", action="append", type=lambda s: int(s, 0),
                    default=None, help="Title ID（可重复）；不给 = 所有游戏")
    ap.add_argument("--version", type=int, default=2)
    ap.add_argument("--swap-not-needed", action="store_true", default=True)
    ap.add_argument("--private-mem", action="store_true", default=False)
    args = ap.parse_args()

    r = build_3gx(args.elf, args.title, args.author, args.summary,
                  args.description, args.mem, args.target, args.version,
                  args.swap_not_needed, args.private_mem, args.out)

    print("%-14s %s" % ("输出", args.out))
    print("%-14s %d 字节" % ("文件大小", r["size"]))
    print("%-14s code=%d rodata=%d data=%d bss=%d" %
          ("段", r["code"], r["rodata"], r["data"], r["bss"]))
    print("%-14s 0x%08X（三段 u32 之和）" % ("校验和", r["checksum"]))
    print("%-14s flags=0x%02X entry=0x%08X" % ("标志", r["flags"], r["entry"]))

    # 自检：入口必须等于**首段基址**。
    #   3gxtool 那行检查写的是  —— 报错文案说 zero，实际比的是基址。
    #   我们的链接脚本把基址定在 0x07000100，所以 entry 也是这个值；
    #   早先我按字面意思断言 "entry 必须为 0"，是自己把自己拦下来了。
    if r["entry"] != r["base"]:
        print("!! 入口(0x%08X) != 首段基址(0x%08X) —— 3gxtool 会拒收" %
              (r["entry"], r["base"]))
        return 1
    if r["code"] + r["rodata"] + r["data"] >= 0x200000:
        print("!! 三段合计 >= 2MiB，插件可能不稳（3gxtool 同样会给这条警告）")
    return 0


if __name__ == "__main__":
    sys.exit(main())