#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""
patch_smdh_3d.py — 把 SMDH 的「立体 3D」相关标志打开（或关掉）。

═══ 它在解决什么 ═══════════════════════════════════════════════════════
3DS home-menu 桌面图标（SMDH）里有个 flags 字段，offset 0x2028（u32）。
3dbrew 核实，这个字里**两个独立位**负责两套完全不同的"3D"：

  bit 0x0002 =「Allow use of 3D（立体视差）」—— 这是**图标**的开关。
      · smdhtool --create **不暴露**它，默认只置 Visible(bit0x0001)。
      · 没它，图标只是张平面方块，推 3D 滑杆没反应。
      · 打开后 home-menu 让图标随滑杆浮起（视差）。图标本身是 RGB565 平面，
        立体感靠 make_icon.py 在方块内的明暗 shading。

  bit 0x0010 =「Extended banner」—— 这是**3D 模型横幅**的开关。
      · 告诉 home-menu：ExeFS 里有一份 CBMD（"banner" 文件），是那个会
        转的 3D 模型横幅。不置这个位，home-menu 根本不会去读 banner 文件，
        横幅页只会显示纯文字/贴图（甚至空白），我们做的 AI 对话框 3D 模型
        就浮不出来。
      · 注意：这个位 ≠ 0x0002。两者是**两张不同的开关**，必须**分别**置。

所以用户要的"桌面上那个会浮起旋转的 3D 模型"，缺一不可：
  · 0x0002 让桌面图标本体允许立体；
  · 0x0010 让横幅页去加载我们打进 CIA 的 CBMD 模型。
本脚本两个位都能拨（--3d / --extbanner / --both）。

═══ 用法 ═══════════════════════════════════════════════════════════════
    python tools/patch_smdh_3d.py app/AIConsole_cia.smdh
        # 默认只开图标 3D 位 (0x0002)，保持与旧调用兼容
    python tools/patch_smdh_3d.py --extbanner app/AIConsole_cia.smdh
        # 只开 Extended Banner 位 (0x0010)
    python tools/patch_smdh_3d.py --both app/AIConsole_cia.smdh
        # 两个都开（CIA 横幅场景的标准姿势）
    python tools/patch_smdh_3d.py --disable --both app/AIConsole_cia.smdh
    python tools/patch_smdh_3d.py --check app/AIConsole_cia.smdh

幂等：已经是目标状态就不动、不报错。
build.py 在 smdhtool 之后会 import 这里的 set_3d_flag() / set_extended_banner_flag()
自动置位，所以这个脚本主要给「单独修一个已有 SMDH」或「验证」用。
"""
from __future__ import print_function

import os
import struct
import sys

# ---- SMDH 偏移（3dbrew 核实）----
OFF_MAGIC = 0x00           # 'SMDH' (4B)
OFF_FLAGS = 0x2028         # Application Settings 内的 flags (u32)
FLAG_VISIBLE = 0x0001      # 在 home menu 显示
FLAG_3D = 0x0002           # 允许立体 3D（图标本体视差开关）
FLAG_EXTBANNER = 0x0010    # Extended banner：ExeFS 里有 CBMD 3D 模型横幅


def set_flag(path, mask, enable=True):
    """打开/关闭 path 这个 SMDH 的**某一位**（mask）。返回 True 表示本次有改动。

    不做任何"假装成功"的事：magic 不对、文件太短、位已经是目标态，都会
    明确报出来；只有真正翻转了 bit 才写盘并返回 True。
    """
    if not os.path.exists(path):
        raise RuntimeError("SMDH 不存在: %s" % path)
    with open(path, "r+b") as fh:
        b = bytearray(fh.read())
    if len(b) < OFF_FLAGS + 4:
        raise RuntimeError("%s 太短(%d 字节)，不像 SMDH" % (path, len(b)))
    if b[OFF_MAGIC:OFF_MAGIC + 4] != b"SMDH":
        raise RuntimeError("%s 的 magic 不是 'SMDH'， refusing" % path)

    flags = struct.unpack_from("<I", b, OFF_FLAGS)[0]
    target = (flags | mask) if enable else (flags & ~mask)
    if target == flags:
        return False  # 已经是目标态，不需要写
    struct.pack_into("<I", b, OFF_FLAGS, target)
    with open(path, "r+b") as fh:
        fh.seek(0)
        fh.write(b)
    return True


def set_3d_flag(path, enable=True):
    """打开/关闭图标立体 3D 位 (0x0002)。兼容旧调用。"""
    return set_flag(path, FLAG_3D, enable)


def set_extended_banner_flag(path, enable=True):
    """打开/关闭 Extended Banner 位 (0x0010)：告诉 home-menu 去读 CBMD 横幅。

    这个位必须和 0x0002 分开置 —— 它管的是"3D 模型横幅"，不是图标视差。
    flag 置 1 但 ExeFS 没 banner 文件，home-menu 横幅页会异常；
    flag 置 0 但 banner 文件在，横幅页不会加载模型（白费）。两者要配套。
    """
    return set_flag(path, FLAG_EXTBANNER, enable)


def report(path):
    if not os.path.exists(path):
        print("  MISSING %s" % path)
        return
    b = open(path, "rb").read()
    if len(b) < OFF_FLAGS + 4 or b[OFF_MAGIC:OFF_MAGIC + 4] != b"SMDH":
        print("  ! %s 不是合法 SMDH" % path)
        return
    flags = struct.unpack_from("<I", b, OFF_FLAGS)[0]
    print("  %-28s flags=0x%04X  visible=%s  3D(icon)=%s  extBanner=%s"
          % (os.path.basename(path), flags,
             bool(flags & FLAG_VISIBLE),
             bool(flags & FLAG_3D),
             bool(flags & FLAG_EXTBANNER)))


def main():
    args = sys.argv[1:]
    if not args:
        print(__doc__)
        return 2
    enable = True
    # 默认只动图标 3D 位（0x0002），与旧行为兼容。
    # --extbanner 只动横幅位（0x0010）；--both 两个都动。
    which = "3d"
    paths = []
    for a in args:
        if a == "--disable":
            enable = False
        elif a == "--extbanner":
            which = "ext"
        elif a == "--both":
            which = "both"
        elif a == "--check":
            for p in paths or []:
                report(p)
            # --check 之后若没有具体文件，就 report 全部默认两个
            if not paths:
                report("app/AIConsole.smdh")
                report("app/AIConsole_cia.smdh")
            return 0
        else:
            paths.append(a)

    if not paths:
        # 没给路径就默认处理工程里两个 SMDH
        paths = ["app/AIConsole.smdh", "app/AIConsole_cia.smdh"]

    changed = 0
    for p in paths:
        before = None
        try:
            b = open(p, "rb").read()
            before = struct.unpack_from("<I", b, OFF_FLAGS)[0] if len(b) >= OFF_FLAGS + 4 else None
        except Exception:
            pass
        try:
            if which == "ext":
                did = set_extended_banner_flag(p, enable)
            elif which == "both":
                d1 = set_3d_flag(p, enable)
                d2 = set_extended_banner_flag(p, enable)
                did = d1 or d2
            else:
                did = set_3d_flag(p, enable)
        except RuntimeError as e:
            print("  ! %s" % e)
            continue
        after = struct.unpack_from("<I", open(p, "rb").read(), OFF_FLAGS)[0]
        verb = "set" if enable else "cleared"
        label = {"3d": "3D(icon) bit 0x0002",
                 "ext": "extBanner bit 0x0010",
                 "both": "3D+extBanner bits"}[which]
        print("  %-28s %s %s %s  (flags 0x%04X -> 0x%04X)"
              % (os.path.basename(p), label, verb,
                 "OK" if did else "(already)",
                 before if before is not None else 0, after))
        if did:
            changed += 1
    print("done (%d file(s) changed)" % changed)
    return 0


if __name__ == "__main__":
    sys.exit(main())
