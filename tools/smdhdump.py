#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""
smdhdump.py — 只读地打印 SMDH 的标题/图标/标志位，并**按 UTF-16LE 严格解码**。

═══ 为什么要严格解码 ═══════════════════════════════════════════════════
SMDH 的标题区是 UTF-16LE。smdhtool 收命令行参数时把字符串当 latin-1 逐字节
塞进这个 UTF-16 字段 —— 于是传中文进去，桌面上就是乱码（build.py 里那条
"标题必须 ASCII"的注释记的就是这件事）。

所以**验证标题必须真的按 UTF-16LE 解**：
  · 传 ASCII 时，"字节看起来对"和"解码出来对"是一致的 → 假绿；
  · 传中文时两者会打架。
本脚本用 errors='replace' + 转义回显 + 「可打印 / 有替换符」两个标记，
让乱码在报告里**肉眼可见**，而不是被 'utf-16-le' 的容错悄悄吃掉。

只读，不写盘。

    python tools/smdhdump.py app/AIConsole.smdh app/AIConsole_cia.smdh
"""
from __future__ import print_function

import os
import struct
import sys

# ---- SMDH 布局（3dbrew + 实物双向核实）----
# 0x0000 magic 'SMDH'(4)
# 0x0004 version(u16) + reserved(u16)
# 0x0008 titles[16]，每份 0x200：
#         +0x000 short description  0x80 字节 (64 码元)
#         +0x080 long  description  0x100 字节(128 码元)
#         +0x180 publisher         0x80 字节 (64 码元)
# 0x2008 settings / 0x2028 flags(u32)
# 0x2040 small icon 24x24 RGB565 (0x480 字节)
# 0x24C0 large icon 48x48 RGB565 (0x1200 字节)
# 合计 0x36C0 = 14016 字节 —— 与产物大小对不上就说明不是 SMDH。
OFF_MAGIC   = 0x00
OFF_TITLES  = 0x08
TITLE_STRIDE = 0x200
N_LANG      = 16
OFF_FLAGS   = 0x2028
OFF_ICON_S  = 0x2040
OFF_ICON_L  = 0x24C0
SZ_ICON_S   = 24 * 24 * 2
SZ_ICON_L   = 48 * 48 * 2
TOTAL_SZ    = OFF_ICON_L + SZ_ICON_L
FLAG_VISIBLE = 0x0001
FLAG_3D      = 0x0002

# 3DS 的语言编号（3dbrew CFG_Language）。中文 3DS 常见系统语言是
# SimplifiedChinese(6)；TraditionalChinese(11) 是繁体。
LANG_NAMES = {
    0: "Japanese", 1: "English", 2: "French", 3: "German", 4: "Italian",
    5: "Spanish", 6: "SimplifiedChinese", 7: "Korean", 8: "Dutch",
    9: "Portuguese", 10: "Russian", 11: "TraditionalChinese",
}
# 桌面上真正会被读到的那几个（smdhtool 通常把同一份写进所有语言槽）
LANGS_OF_INTEREST = (1, 6, 11, 0)


def decode_field(raw, label):
    """按 UTF-16LE 解一个标题字段，并把"像不像乱码"标出来。

    返回 (文本, 是否可疑)。可疑 = 出现 U+FFFD 替换符或不可打印控制符。
    'utf-16-le' 默认会静默替换非法序列 → 必须显式查 replacement。
    """
    txt = raw.decode("utf-16-le", errors="replace").rstrip("\x00")
    bad = "\ufffd" in txt
    ctrl = [c for c in txt if ord(c) < 0x20 and c not in "\t"]
    if ctrl:
        bad = True
    flag = "  <== 可疑(有替换符/控制符)" if bad else ""
    return txt, bad, len(ctrl)


def show_field(fh_label, txt, bad, nctrl):
    print("        %-8s %r%s" % (fh_label, txt,
          "  <== 可疑" if bad else ""))


def dump(path):
    print("=" * 72)
    print("SMDH %s" % path)
    if not os.path.exists(path):
        print("  ! 文件不存在")
        return False
    b = open(path, "rb").read()
    if b[OFF_MAGIC:OFF_MAGIC + 4] != b"SMDH":
        print("  ! magic 不是 'SMDH'，不是 SMDH 文件")
        return False
    print("  size   : %d 字节 (期望 %d)" % (len(b), TOTAL_SZ))
    if len(b) != TOTAL_SZ:
        print("  ! 大小对不上 —— 桌面图标会变空块（见 build.py: smdhtool 不缩放）")
    print("  version: 0x%04X" % struct.unpack_from("<H", b, 4)[0])
    flags = struct.unpack_from("<I", b, OFF_FLAGS)[0]
    print("  flags  : 0x%04X  visible=%s  3D=%s"
          % (flags, bool(flags & FLAG_VISIBLE), bool(flags & FLAG_3D)))

    # 图标区是否全 0（全 0 = 空图标，桌面上是空块）
    def icon_stat(off, sz, px):
        raw = b[off:off + sz]
        nz = sum(1 for x in raw if x != 0)
        uniq = len(set(raw))
        return nz, uniq
    nzS, uS = icon_stat(OFF_ICON_S, SZ_ICON_S, 24)
    nzL, uL = icon_stat(OFF_ICON_L, SZ_ICON_L, 48)
    print("  icon24 :非零字节 %d/%d 唯一值 %d %s"
          % (nzS, SZ_ICON_S, uS, "<== 疑似空图标" if nzS == 0 else ""))
    print("  icon48 : 非零字节 %d/%d 唯一值 %d %s"
          % (nzL, SZ_ICON_L, uL, "<== 疑似空图标" if nzL == 0 else ""))

    any_bad = False
    # 逐语言槽扫一遍，找出「哪些槽是坏的」和「哪些槽有内容」
    for lang in range(N_LANG):
        base = OFF_TITLES + lang * TITLE_STRIDE
        short, bad_s, _ = decode_field(b[base:base + 0x80], "short")
        long_, bad_l, _ = decode_field(b[base + 0x80:base + 0x180], "long")
        pub, bad_p, _ = decode_field(b[base + 0x180:base + 0x200], "pub")
        has = bool(short or long_ or pub)
        bad = bad_s or bad_l or bad_p
        if bad:
            any_bad = True
        if lang in LANGS_OF_INTEREST:
            name = LANG_NAMES.get(lang, "lang%d" % lang)
            print("  -- slot %d (%s) %s" % (lang, name,
                                            "有内容" if has else "空"))
            if has:
                show_field("short", short, bad_s, 0)
                show_field("long", long_, bad_l, 0)
                show_field("pub", pub, bad_p, 0)
        elif has and bad:
            # 非关注语言里也发现了坏内容 —— 换个语种启动就会花屏
            print("  -- slot %d (%s) 有内容且**乱码**: short=%r"
                  % (lang, LANG_NAMES.get(lang, "?"), short))
    if any_bad:
        print("  >>> 结论：有标题字段按 UTF-16LE 解是乱码（smdhtool 的 latin-1 坑）")
    return not any_bad


def check_title(path, expect_short):
    """构建期承重墙：验 path 的 16 个语言槽标题是否**逐字节**等于 expect_short。

    返回 True/False，并把失败原因说清楚（不吞错）。

    ★ 为什么判据必须是"逐字节等于期望的 UTF-16LE"，而不是"能不能解出可打印字符"：
      后者对**空标题**也返回 True（空串全对），对**截断**也返回 True
      （截半仍是合法 UTF-16），于是会变成一条永远绿的守卫。
      逐字节比才能同时挡住：编码错位、latin-1 混入、长度被截、多语言槽不一致。
    """
    if not os.path.exists(path):
        print("  ! SMDH 不存在: %s" % path)
        return False
    b = open(path, "rb").read()
    if len(b) != TOTAL_SZ:
        print("  ! %s 大小 %d != %d（桌面图标会变空块）"
              % (os.path.basename(path), len(b), TOTAL_SZ))
        return False
    if b[OFF_MAGIC:OFF_MAGIC + 4] != b"SMDH":
        print("  ! %s 的 magic 不是 'SMDH'" % os.path.basename(path))
        return False

    want = expect_short.encode("utf-16-le")
    if len(want) > 0x80:
        print("  ! 标题过长：%d 字节 > short desc 容量 128 字节"
              "（SMDH 会**静默截半**不报错）" % len(want))
        return False

    bad = []
    for lang in range(N_LANG):
        off = OFF_TITLES + lang * TITLE_STRIDE
        got = b[off:off + len(want)]
        if got != want:
            name = LANG_NAMES.get(lang, "lang%d" % lang)
            bad.append((lang, name, got.decode("utf-16-le", errors="replace")))
    if bad:
        print("  ! 标题校验失败（期望 %r）:" % expect_short)
        for lang, name, got in bad[:4]:
            print("      slot %d (%-19s) 实际 %r" % (lang, name, got))
        if len(bad) > 4:
            print("      ... 另有 %d 个语言槽也不一致" % (len(bad) - 4))
        return False

    # 顺带确认图标区不是空的（smdhtool 不缩放，给大图会静默跳过 → 空块）
    for off, sz, px in ((OFF_ICON_S, SZ_ICON_S, 24), (OFF_ICON_L, SZ_ICON_L, 48)):
        if all(x == 0 for x in b[off:off + sz]):
            print("  ! icon%d 全 0 —— 桌面上是空块（smdhtool 不缩放，"
                  "先用 tools/make_smdh_icons.py 降到 48/24）" % px)
            return False

    print("  OK 标题 16/16 语言槽 == %r（UTF-16LE 逐字节相等）" % expect_short)
    return True


def selftest():
    """自检：证明 check_title **能红**。不做这一步，上面就只是一条永远绿的守卫。

    构造 3 个反例，期望全部被 check_title 判红：
      ① latin-1 逐字节塞进去（就是旧注释担心的那个坑）
      ② 标题被截断（短一半）
      ③ 只有一个语言槽是错的
    再验正例（正确的中文标题）判绿。
    """
    good = bytearray(b"SMDH" + b"\x00" * (TOTAL_SZ - 4))
    want = "测试 0.99".encode("utf-16-le")
    for lang in range(N_LANG):
        off = OFF_TITLES + lang * TITLE_STRIDE
        good[off:off + len(want)] = want
    # 图标区填点非零，否则会先被 icon 判据拦下（测不到标题判据本身）
    for i in range(OFF_ICON_S, OFF_ICON_L + SZ_ICON_L):
        good[i] = 0xAB

    cases = []

    # ① latin-1 乱码：把每个字节当 latin-1 展开成 UTF-16
    bad1 = bytearray(good)
    for lang in range(N_LANG):
        off = OFF_TITLES + lang * TITLE_STRIDE
        moji = "测试".encode("utf-8").decode("latin-1").encode("utf-16-le")
        bad1[off:off + len(moji)] = moji
    cases.append(("latin-1 乱码", bytes(bad1), False))

    # ② 截断：只写前一半
    bad2 = bytearray(good)
    for lang in range(N_LANG):
        off = OFF_TITLES + lang * TITLE_STRIDE
        bad2[off + len(want) // 2:off + len(want)] = b"\x00" * (len(want) // 2)
    cases.append(("标题被截断", bytes(bad2), False))

    # ③ 只有槽 6 错（模拟"只对了一种语言"，最容易蒙混过关的那种）
    bad3 = bytearray(good)
    off6 = OFF_TITLES + 6 * TITLE_STRIDE
    bad3[off6:off6 + len(want)] = b"\x00" * len(want)
    cases.append(("单个语言槽为空", bytes(bad3), False))

    # ④ 全 0 图标（顺便证明图标判据会红）
    bad4 = bytearray(good)
    for i in range(OFF_ICON_S, OFF_ICON_L + SZ_ICON_L):
        bad4[i] = 0
    cases.append(("图标全 0", bytes(bad4), False))

    # 正例
    cases.append(("正确的中文标题", bytes(good), True))

    import tempfile
    ok = True
    print("---- check_title 自检（证明判据能红）----")
    for name, data, expect_ok in cases:
        fd, tmp = tempfile.mkstemp(suffix=".smdh")
        os.close(fd)
        with open(tmp, "wb") as fh:
            fh.write(data)
        got = check_title(tmp, "测试 0.99")
        os.unlink(tmp)
        verdict = "OK" if got == expect_ok else "!! 判据失效"
        if got != expect_ok:
            ok = False
        print("  %-18s 期望 %-5s 实得 %-5s  %s"
              % (name, expect_ok, got, verdict))
    print("自检结果: %s" % ("全部符合预期" if ok else "有反例没被抓住"))
    return ok


def main():
    args = sys.argv[1:]
    if args and args[0] == "--selftest":
        return 0 if selftest() else 1
    if not args:
        args = ["app/AIConsole.smdh", "app/AIConsole_cia.smdh"]
    allok = True
    for p in args:
        if not dump(p):
            allok = False
    print("=" * 72)
    print("RESULT: %s" % ("全部标题解码正常" if allok else "存在问题，见上"))
    return 0 if allok else 1


if __name__ == "__main__":
    sys.exit(main())
