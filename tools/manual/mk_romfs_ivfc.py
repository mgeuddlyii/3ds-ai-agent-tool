#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
mk_romfs_ivfc.py —— 把目录打成 **IVFC 包裹的 RomFS**（NCCH 要的那种形态）

── 这个脚本为什么必须存在 ──
devkitPro 的 `mkromfs3ds` 只产出 **裸 level-3 RomFS**（第一字节就是 0x28 头），
而 `makerom -romfs` 要的是**套了 IVFC 哈希树外壳**的镜像 —— 喂裸的它会报

    [ROMFS ERROR] Invalid RomFS Binary.
    [NCCH ERROR] Failed to setup RomFs

（2026-10-18 本机实测复核，与 docs/内置说明书方案.md §3.2 的结论一致。）
所以要自己做两件事：调 mkromfs3ds 出 level-3，再按 IVFC 规范套壳。

── 物理布局（NO$FUN: 3DS Files - NCCH RomFS，2026-10-18 核对）──
    0x0000  0x5C  IVFC 头
    0x005C  0x04  补到 0x20 边界
    0x0060  0x20*M  master hashes（Level 1 每个 0x1000 块一个）
    ...           补到 0x1000
    0x1000  ...   Level 3 = 裸 RomFS 数据（补到 0x1000）
    XX000   ...   Level 1（Level 2 每个 0x1000 块一个 SHA-256）
    YY000   ...   Level 2（Level 3 每个 0x1000 块一个 SHA-256）

★ 注意物理顺序是 **L3 → L1 → L2**，不是 1/2/3 顺序。因为 L2 是 L3 的哈希、
  L1 是 L2 的哈希、master 是 L1 的哈希；写入时从"被哈希方"往"哈希方"铺。

── 每个哈希覆盖多少字节 ──
**永远是完整的 0x1000 块（含补零）**。原文："0x060h 20h*N SHA256's on the
1000h-byte block(s) at xx000h"。所以每个数组都先补零到 0x1000 再算。

── 头的字段（偏移按 0x5C 版）──
    0x00 0x08  "IVFC" + 版本 0x00010000
    0x08 0x04  master hash 数组大小（0x20*M）
    0x0C 0x08  Level 1 逻辑偏移
    0x14 0x08  Level 1 数组大小（0x20*N1）
    0x1C 0x04  Level 1 块大小 log2 = 0x0C
    0x20 0x04  保留 0
    0x24 0x08  Level 2 逻辑偏移
    0x2C 0x08  Level 2 数组大小
    0x34 0x04  Level 2 块大小 log2 = 0x0C
    0x38 0x04  保留 0
    0x3C 0x08  Level 3 逻辑偏移
    0x44 0x08  Level 3 数据大小（已补到 0x1000）
    0x4C 0x04  Level 3 块大小 log2 = 0x0C
    0x50 0x04  保留 0
    0x54 0x04  头大小 = 0x5C
    0x58 0x04  可选信息大小 = 0
    0x5C 0x04  补到 0x20 边界

⚠️ "逻辑偏移"与物理偏移无关（原文：completely unrelated to the physical
   offsets）。这里按 NO$FUN 给的例子取"累计对齐后的尺寸"：
   L1=0 / L2=align(L1 size) / L3=L2高+align(L2 size)。
"""

import argparse
import hashlib
import os
import struct
import subprocess
import sys

BLOCK = 0x1000
BLOCK_LOG2 = 0x0C
HDR_SIZE = 0x5C
HDR_PADDED = 0x60


def sha256(b):
    return hashlib.sha256(b).digest()


def pad_to(b, n):
    """补零到 n 的整数倍（已对齐则原样返回）。用于**数组长度**。"""
    if n <= 0:
        return b
    rem = len(b) % n
    return b if rem == 0 else b + b"\x00" * (n - rem)


def pad_abs(buf, n):
    """把缓冲区补到**绝对长度**是 n 的整数倍。

    ★ 这和 pad_to 不是一回事：master hash 从 0x60 开始，
      要补到 **0x1000 这个绝对地址**，而不是"再接一整块 0x1000"
      （那样 L3 会落到 0x1060，而规范要求落在 0x1000）。
    """
    if n <= 0:
        return buf
    rem = len(buf) % n
    if rem:
        buf += b"\x00" * (n - rem)
    return buf


def hash_blocks(data):
    """对每个 0x1000 块算 SHA-256。data 必须已补到 0x1000 整数倍。"""
    assert len(data) % BLOCK == 0, "内部错误：待哈希数据没对齐到 0x1000"
    return b"".join(sha256(data[i:i + BLOCK]) for i in range(0, len(data), BLOCK))


def build_level3(root, mkromfs, out_romfs):
    """调 mkromfs3ds 出裸 level-3 RomFS。返回其字节。"""
    r = subprocess.run([mkromfs, root, out_romfs],
                       capture_output=True, text=True)
    if r.returncode != 0 or not os.path.exists(out_romfs):
        sys.stderr.write("!! mkromfs3ds 失败 rc=%s\n%s\n%s\n"
                         % (r.returncode, r.stdout, r.stderr))
        return None
    with open(out_romfs, "rb") as f:
        return f.read()


def wrap_ivfc(level3):
    """把裸 level-3 数据包成 IVFC。返回完整镜像字节。"""
    l3 = pad_to(level3, BLOCK)

    # Level 2 = 对 L3 每个 0x1000 块算哈希
    l2 = hash_blocks(l3)
    l2_padded = pad_to(l2, BLOCK)

    # Level 1 = 对 L2（补零后）每个 0x1000 块算哈希
    l1 = hash_blocks(l2_padded)
    l1_padded = pad_to(l1, BLOCK)

    # master = 对 L1（补零后）每个 0x1000 块算哈希
    master = hash_blocks(l1_padded)

    # 逻辑偏移：累计"对齐后的尺寸"（NO$FUN 例子口径：L1=0, L2=0x1000, L3=0x4000）
    l1_off = 0
    l2_off = l1_off + len(pad_to(l1, BLOCK))
    l3_off = l2_off + len(pad_to(l2, BLOCK))

    hdr = bytearray(HDR_PADDED)
    hdr[0x00:0x04] = b"IVFC"
    struct.pack_into("<I", hdr, 0x04, 0x00010000)       # 版本
    struct.pack_into("<I", hdr, 0x08, len(master))      # master hash 数组大小
    struct.pack_into("<Q", hdr, 0x0C, l1_off)
    struct.pack_into("<Q", hdr, 0x14, len(l1))
    struct.pack_into("<I", hdr, 0x1C, BLOCK_LOG2)
    struct.pack_into("<I", hdr, 0x20, 0)                # 保留
    struct.pack_into("<Q", hdr, 0x24, l2_off)
    struct.pack_into("<Q", hdr, 0x2C, len(l2))
    struct.pack_into("<I", hdr, 0x34, BLOCK_LOG2)
    struct.pack_into("<I", hdr, 0x38, 0)                # 保留
    struct.pack_into("<Q", hdr, 0x3C, l3_off)
    struct.pack_into("<Q", hdr, 0x44, len(l3))
    struct.pack_into("<I", hdr, 0x4C, BLOCK_LOG2)
    struct.pack_into("<I", hdr, 0x50, 0)                # 保留
    struct.pack_into("<I", hdr, 0x54, HDR_SIZE)         # 头大小
    struct.pack_into("<I", hdr, 0x58, 0)                # 可选信息大小

    # 物理顺序：头(0x60) → master(补到 0x1000) → L3 → L1 → L2
    # 每一段都补到**绝对 0x1000 边界**。
    out = bytearray(hdr)
    out += master
    out = pad_abs(out, BLOCK)          # → 0x1000
    out += l3                          # 已是 0x1000 整数倍
    out = pad_abs(out, BLOCK)
    out += l1
    out = pad_abs(out, BLOCK)
    out += l2
    out = pad_abs(out, BLOCK)
    return bytes(out)


def verify(blob):
    """把产出的镜像**自己解析一遍**，逐级重算哈希。

    不信自己的拼装 —— 这类"结构看起来对、哈希差一个字节"的错，
    makerom 只会报一句 Invalid RomFS Binary，根本不知道错在哪。
    """
    ok = True

    def bad(msg):
        nonlocal ok
        ok = False
        print("   ✗ " + msg)

    if blob[0:4] != b"IVFC":
        bad("magic 不是 IVFC，实际 %r" % blob[0:4])
        return False

    ver, msz = struct.unpack_from("<II", blob, 0x04)
    l1_off, l1_sz = struct.unpack_from("<QQ", blob, 0x0C)
    l1_log2, = struct.unpack_from("<I", blob, 0x1C)
    l2_off, l2_sz = struct.unpack_from("<QQ", blob, 0x24)
    l2_log2, = struct.unpack_from("<I", blob, 0x34)
    l3_off, l3_sz = struct.unpack_from("<QQ", blob, 0x3C)
    l3_log2, = struct.unpack_from("<I", blob, 0x4C)
    hdr_sz, = struct.unpack_from("<I", blob, 0x54)

    print("   版本 0x%08X · 头 0x%X · master %d 字节" % (ver, hdr_sz, msz))
    print("   L1 off=0x%X sz=0x%X log2=%d" % (l1_off, l1_sz, l1_log2))
    print("   L2 off=0x%X sz=0x%X log2=%d" % (l2_off, l2_sz, l2_log2))
    print("   L3 off=0x%X sz=0x%X log2=%d" % (l3_off, l3_sz, l3_log2))

    if ver != 0x00010000:
        bad("版本应为 0x00010000")
    if hdr_sz != HDR_SIZE:
        bad("头大小应为 0x%X" % HDR_SIZE)
    for nm, v in (("L1", l1_log2), ("L2", l2_log2), ("L3", l3_log2)):
        if v != BLOCK_LOG2:
            bad("%s 块大小 log2 应为 %d" % (nm, BLOCK_LOG2))

    # 物理切片（顺序：头 0x60 → master → L3 → L1 → L2，各段补到绝对 0x1000）
    off = HDR_PADDED
    master_phys = blob[off:off + msz]
    off = ((off + msz + BLOCK - 1) // BLOCK) * BLOCK
    l3_phys_off = off
    l3 = blob[l3_phys_off:l3_phys_off + l3_sz]
    off = ((l3_phys_off + l3_sz + BLOCK - 1) // BLOCK) * BLOCK
    l1_phys_off = off
    l1 = blob[l1_phys_off:l1_phys_off + l1_sz]
    off = ((l1_phys_off + l1_sz + BLOCK - 1) // BLOCK) * BLOCK
    l2_phys_off = off
    l2 = blob[l2_phys_off:l2_phys_off + l2_sz]

    expect_len = ((l2_phys_off + l2_sz + BLOCK - 1) // BLOCK) * BLOCK
    if len(blob) != expect_len:
        bad("总长度与布局推算不一致：实际 0x%X，推算 0x%X"
            % (len(blob), expect_len))
    print("   物理: master@0x%X L3@0x%X L1@0x%X L2@0x%X"
          % (HDR_PADDED, l3_phys_off, l1_phys_off, l2_phys_off))

    # 逐级重算
    l2_calc = hash_blocks(pad_to(l3, BLOCK))
    if l2_calc != l2:
        bad("Level 2 哈希与 Level 3 数据不符")
    l1_calc = hash_blocks(pad_to(l2, BLOCK))
    if l1_calc != l1:
        bad("Level 1 哈希与 Level 2 不符")
    master_calc = hash_blocks(pad_to(l1, BLOCK))
    if master_calc != master_phys:
        bad("master hash 与 Level 1 不符")

    # Level-3 内部：应该是合法 RomFS（0x28 头）
    if l3[0:4] != struct.pack("<I", 0x28):
        bad("Level 3 不是合法 RomFS（首个 u32 应为 0x28）")

    if ok:
        print("   ✓ 逐级哈希、长度、RomFS 头全部自洽")
    return ok


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("root", help="要打包的目录")
    ap.add_argument("out", help="输出的 IVFC RomFS 镜像")
    ap.add_argument("--mkromfs", default=None, help="mkromfs3ds 路径")
    ap.add_argument("--verify-only", metavar="IMG", help="只校验已有镜像")
    args = ap.parse_args()

    if args.verify_only:
        with open(args.verify_only, "rb") as f:
            return 0 if verify(f.read()) else 1

    mk = args.mkromfs or r"E:\devkitPro\tools\bin\mkromfs3ds.exe"
    if not os.path.exists(mk):
        sys.stderr.write("!! 找不到 mkromfs3ds：%s\n" % mk)
        return 1
    if not os.path.isdir(args.root):
        sys.stderr.write("!! 不是目录：%s\n" % args.root)
        return 1

    tmp = args.out + ".level3"
    l3 = build_level3(args.root, mk, tmp)
    if l3 is None:
        return 1
    print("   level3 : %d 字节（%d 个 0x1000 块）"
          % (len(l3), (len(pad_to(l3, BLOCK)) // BLOCK)))

    blob = wrap_ivfc(l3)
    with open(args.out, "wb") as f:
        f.write(blob)
    print("   ivfc   : %s（%d 字节）" % (args.out, len(blob)))

    if not verify(blob):
        sys.stderr.write("!! 自校验失败 —— 不要拿这个镜像去喂 makerom\n")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
