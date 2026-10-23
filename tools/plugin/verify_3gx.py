#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
verify_3gx.py —— 校验 .3gx 插件文件的结构

── 为什么需要这个 ──
`.3gx` 是给 Luma 的插件加载器吃的容器。它坏了的表现是
**游戏启动时直接退回 HOME / 黑屏**，而且没有任何日志 ——
现场既没有堆栈也没有提示，只能靠猜。
所以产物必须在下发前先在 PC 上验一遍结构。

判据（全部来自 Luma 的 `sysmodules/rosalina/include/plugin/3gx.h`
与 3gxtool 的 `includes/3gx.hpp`，两边字段一致）：

    0x00 u64 magic = "3GX$0002"
    0x08 u32 version
    0x0C u32 reserved
    0x10 ..0x30  info 字符串（长度 + 绝对偏移，四对）
    0x30 u32 infoFlags（位域）
    0x34 u32 exeDecChecksum
    0x38 4×u32 builtInDecExeArgs
    0x48 4×u32 builtInSwapEncDecArgs
    0x58 u32 codeOffset / 0x5C rodataOffset / 0x60 dataOffset
    0x64 u32 codeSize   / 0x68 rodataSize   / 0x6C dataSize
    0x70 u32 bssSize
    0x74 u32 exeDecOffset / 0x78 swapEncOffset / 0x7C swapDecOffset
    0x80 u32 targets.count / 0x84 u32 targets.titles
    0x88 u32 symtable.nbSymbols / 0x8C symbolsOffset / 0x90 nameTableOffset

★ 最关键的一条：`exeDecOffset` 必须非 0（flags.bit0=1）。
  Luma 的 `3gx.c` 里有 `if (!ctx->isExeLoadFunctionset) return RD_NO_DATA;`
  —— **没有内嵌解包函数的 .3gx 会被直接拒绝**，报
  "This plugin requires a loading function"。
  自己写打包器最容易漏这条，所以这里单独判。

用法：
    python tools/plugin/verify_3gx.py plugin/translate/translate.3gx
返回 0 = 通过。
"""

import os
import struct
import sys

HDR_SIZE = 0x94


def verify(path):
    if not os.path.exists(path):
        print("!! 找不到 %s" % path)
        return False
    b = open(path, "rb").read()
    ok = True

    def bad(msg):
        nonlocal ok
        ok = False
        print("   ✗ " + msg)

    print("文件 %s（%d 字节）" % (os.path.basename(path), len(b)))

    if len(b) < HDR_SIZE:
        bad("文件比头部还短")
        return False

    # ---- magic ----
    magic, version, reserved = struct.unpack_from("<QII", b, 0)
    if magic != 0x3230303024584733:
        bad("magic 不是 \"3GX$0002\"（实际 0x%016X）。"
            "若是 0x3130303024584733 = \"3GX$0001\"（v1），现行 Luma 会判 "
            "\"Outdated plugin file\"" % magic)
        return False
    print("   ✓ magic 3GX$0002")
    print("   版本 %d.%d.%d" % ((version >> 24) & 0xFF,
                               (version >> 16) & 0xFF,
                               (version >> 8) & 0xFF))

    # ---- info 字符串 ----
    a_len, a_off, t_len, t_off, s_len, s_off, d_len, d_off = \
        struct.unpack_from("<8I", b, 0x10)

    def getstr(n, off, name):
        if n == 0:
            print("   · %s：（空）" % name)
            return ""
        if off + n > len(b):
            bad("%s 越界（off=0x%X len=%d）" % (name, off, n))
            return ""
        raw = b[off:off + n].rstrip(b"\x00")
        try:
            txt = raw.decode("utf-8")
        except UnicodeDecodeError:
            txt = raw.decode("gbk", "replace")
        print("   ✓ %s：%s" % (name, txt))
        return txt

    getstr(t_len, t_off, "Title")
    getstr(a_len, a_off, "Author")
    getstr(s_len, s_off, "Summary")
    getstr(d_len, d_off, "Description")

    # ---- flags / 解包函数 ----
    info_flags, checksum = struct.unpack_from("<II", b, 0x30)
    has_dec = bool(info_flags & 1)
    mem_region = (info_flags >> 2) & 0x3
    compat = (info_flags >> 4) & 0x3
    mem_names = {0: "5MiB", 1: "2MiB", 2: "10MiB", 3: "?"}
    cmp_names = {0: "Console", 1: "Citra", 2: "Any", 3: "?"}
    print("   flags=0x%02X  内存区=%s  兼容=%s  embeddedDec=%s"
          % (info_flags, mem_names[mem_region], cmp_names[compat], has_dec))

    # ---- 段 ----
    code_off, ro_off, data_off = struct.unpack_from("<III", b, 0x58)
    code_sz, ro_sz, data_sz, bss_sz = struct.unpack_from("<IIII", b, 0x64)
    dec_off, swap_e, swap_d = struct.unpack_from("<III", b, 0x74)

    print("   code  @0x%06X size=0x%X" % (code_off, code_sz))
    print("   rodata@0x%06X size=0x%X" % (ro_off, ro_sz))
    print("   data  @0x%06X size=0x%X  bss=0x%X" % (data_off, data_sz, bss_sz))

    # 代码必须落在 0x07000100（Luma 硬编码跳转目标）
    if dec_off == 0:
        bad("exeDecOffset=0 —— 没有内嵌解包函数，Luma 会报 "
            "\"This plugin requires a loading function\" 并拒绝加载")
    else:
        print("   ✓ 内嵌解包函数 @0x%06X（checksum 0x%08X）"
              % (dec_off, checksum))

    for nm, off, sz in (("code", code_off, code_sz),
                        ("rodata", ro_off, ro_sz),
                        ("data", data_off, data_sz)):
        if off + sz > len(b):
            bad("%s 段越界：0x%X+0x%X > 文件长 %d" % (nm, off, sz, len(b)))

    # 段必须首尾相接（3gxtool 自己的硬要求，Luma 依赖这个布局）
    if ro_off != code_off + code_sz:
        bad("rodata 起点 0x%X ≠ code 结束 0x%X（段不连续）"
            % (ro_off, code_off + code_sz))
    if data_off != ro_off + ro_sz:
        bad("data 起点 0x%X ≠ rodata 结束 0x%X（段不连续）"
            % (data_off, ro_off + ro_sz))
    if ok:
        print("   ✓ 三段首尾相接")

    # ---- targets ----
    tcount, toff = struct.unpack_from("<II", b, 0x80)
    if tcount == 0:
        print("   · 兼容目标：全部 title（count=0）")
    else:
        print("   · 兼容目标：%d 个 title" % tcount)
        if toff + tcount * 4 > len(b):
            bad("targets 数组越界")

    # ---- 符号表（Luma 不读，仅提示）----
    nsym, symoff, nameoff = struct.unpack_from("<III", b, 0x88)
    print("   · 符号表：%d 项（Luma 不读，仅供调试）" % nsym)

    if ok:
        print("   ✓ 结构自洽")
    return ok


def main():
    if len(sys.argv) < 2:
        print("用法：python tools/plugin/verify_3gx.py <file.3gx>")
        return 2
    return 0 if verify(sys.argv[1]) else 1


if __name__ == "__main__":
    sys.exit(main())
