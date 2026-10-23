#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
verify_cia_banner.py — 验收 CIA 里到底有没有真的把 3D 横幅打进去。

本机没 ctrtool。实测发现：这个 homebrew CIA 的 ExeFS 是**明文**的
（"banner"/"icon" 文件名表、CBMD、SMDH 数据都直接可见），
makerom 只是没写 "ExeFS" 魔数，所以逐字节找魔数即可验证：

  · 搜 b"CBMD" -> 验 CBMD 容器（默认 CGFX 偏移、声明解压大小 ≤512KiB）。
  · 搜 b"SMDH" -> 验 SMDH，读 flags@0x2028 的 0x0010(Extended Banner) /
    0x0002(图标 3D)。home-menu 靠这两者配合才会渲染浮起旋转的 AI 对话框。

只读检查，不改任何文件。
"""
import os
import struct
import sys

CIA = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
                   "app", "AIConsole.cia")
MAX_DEC = 0x80000  # 512 KiB


def main():
    if not os.path.exists(CIA):
        print("!! 找不到 %s" % CIA)
        return 1
    blob = open(CIA, "rb").read()
    print("CIA  : %d 字节  (%s)\n" % (len(blob), CIA))

    ok = True

    # ---- 1) CBMD 横幅 ----
    print("=== 3D 横幅 (CBMD) ===")
    cgfx_in = False
    i = blob.find(b"CBMD")
    while i != -1:
        # CBMD: +0x08 默认 CGFX 偏移（应=0x88），+0x88 u32 解压大小
        if i + 0x8C <= len(blob):
            cgfx_off = struct.unpack_from("<I", blob, i + 0x08)[0]
            dec = struct.unpack_from("<I", blob, i + 0x88)[0]
            cwav = struct.unpack_from("<I", blob, i + 0x84)[0]
            good = (cgfx_off == 0x88) and (dec <= MAX_DEC)
            print("  CBMD @0x%X : 默认CGFX偏移=0x%X(%s) 解压=%d B(%s) CWAV=%s"
                  % (i, cgfx_off, "OK" if cgfx_off == 0x88 else "错",
                     dec, "≤512KiB OK" if dec <= MAX_DEC else "超限!",
                     "有音频" if cwav else "无音频"))
            if good:
                cgfx_in = True
        i = blob.find(b"CBMD", i + 1)
    if not cgfx_in:
        print("  !! 没找到合法的 CBMD 横幅")
        ok = False
    else:
        print("  -> 横幅 CBMD 已在 CIA 内 ✓")

    # ---- 2) SMDH 标志 ----
    print("\n=== SMDH 标志 (Extended Banner) ===")
    any_ext = False
    j = blob.find(b"SMDH")
    while j != -1:
        if j + 0x202C <= len(blob):
            flags = struct.unpack_from("<I", blob, j + 0x2028)[0]
            has3d = bool(flags & 0x0002)
            has_ext = bool(flags & 0x0010)
            any_ext = any_ext or has_ext
            print("  SMDH @0x%X : flags=0x%04X  3D(icon)=%s  extBanner=%s"
                  % (j, flags, has3d, has_ext))
        j = blob.find(b"SMDH", j + 1)
    if not any_ext:
        print("  !! 没有任何 SMDH 置了 Extended Banner(0x0010) 位")
        ok = False
    else:
        print("  -> 至少一个 SMDH 已声明 Extended Banner ✓")

    print("\n结论: %s"
          % ("横幅已正确打进 CIA 且 SMDH 标志齐 ✓，可装真机验收"
             if ok else "存在问题，见上 ⚠"))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
