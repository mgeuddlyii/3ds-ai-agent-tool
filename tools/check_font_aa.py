#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
check_font_aa.py -- 字库抗锯齿判据（独立于 mkfont.py 的自检）

存在的意义（铁律 4：永远绿的守卫比没有更坏）
-------------------------------------------
mkfont.py 自带的自检**只能验生成侧**：它知道自己刚算出来的覆盖率对不对。
但它验不了两件更要紧的事：

  1. **落盘的 font.bin 里到底是不是灰度**（自检算的是内存里的对象，
     write_font 之后有没有被写坏、头里的位深字段有没有写错，它看不见）
  2. **渲染端读得对不对** —— 若 C 端仍按 1bpp 位寻址去读 8bit 位图，
     会读出"看似合理的乱码字形"：不崩、不报错、预览图还挺像字，
     但边缘是错的。这类错误只有从**产物 + C 侧读取逻辑**两头对才能抓到。

本脚本只读 app/data/font.bin 这个**最终产物**，按 C 端 font.c 的读法
（font_glyph_alpha 的同款逻辑）复刻一遍，然后判三件事：
  A. 头部位深字段与位图实际大小自洽
  B. 汉字字形里存在足量中间灰阶（抗锯齿真出来了）
  C. 灰度不是"假灰"：随机抽样若干字形，逐像素与 1bpp 阈值化结果对比，
     确认灰度版**确实多出**了过渡像素（而不是把 1bpp 填成 0/255）

用法：
    python tools/check_font_aa.py                     # 验 app/data/font.bin
    python tools/check_font_aa.py --bin X.bin         # 验指定字库
    python tools/check_font_aa.py --selftest          # 反向验证：拿 1bpp 当输入，必须判红

--selftest 是**这个脚本自己的守卫**：它证明"坏输入会红"。
没有它，本脚本可能永远绿。
"""

import argparse
import os
import struct
import sys

CELL_FALLBACK = 16

#: 抗锯齿判据阈值（与 mkfont.py 的 GRAY_RATIO_MIN 同源，改一处要改两处）
GRAY_RATIO_MIN = 0.12

#: 与 C 端 font.c 里 INK/NOISE 无关 —— 这里只判"有没有中间灰"，
#: 所以用最朴素的 0<v<255，不引入门槛常量。
HEADER_FMT = "<4sHHBBBBIIIII"
HEADER_SIZE = struct.calcsize(HEADER_FMT)
assert HEADER_SIZE == 32


def read_font(path):
    with open(path, "rb") as f:
        d = f.read()
    if len(d) < HEADER_SIZE:
        raise ValueError("文件太短：%d 字节" % len(d))
    magic = d[:4]
    if magic != b"AFNT":
        raise ValueError("magic 不是 AFNT：%r" % magic)
    (_, ver, flags, cw, ch, bl, _rsv, count, ioff, boff, bsize, fb) = \
        struct.unpack_from(HEADER_FMT, d, 0)
    depth_code = flags & 3
    depth = {0: 1, 1: 4, 2: 8, 3: None}[depth_code]
    return dict(data=d, ver=ver, flags=flags, depth=depth, cw=cw, ch=ch,
                bl=bl, count=count, ioff=ioff, boff=boff, bsize=bsize, fb=fb)


def cell_bytes(cw, ch, depth):
    return cw * ch * depth // 8


def alpha_at(f, glyph_index, x, y):
    """复刻 C 端 font_glyph_alpha()。**必须与 font.c 逐行对应**（铁律 6b）。"""
    cw, ch, depth = f["cw"], f["ch"], f["depth"]
    if not (0 <= x < cw and 0 <= y < ch):
        return 0
    base = f["boff"] + glyph_index * cell_bytes(cw, ch, depth)
    d = f["data"]
    if depth == 1:
        byte = d[base + y * 2 + (0 if x < 8 else 1)]
        return 255 if (byte & (0x80 >> (x & 7))) else 0
    i = y * cw + x
    if depth == 8:
        return d[base + i]
    b = d[base + (i >> 1)]
    nib = (b >> 4) if (i % 2 == 0) else (b & 0x0F)
    return nib * 17


def cps_of(f):
    return [struct.unpack_from("<I", f["data"], f["ioff"] + i * 8)[0]
            for i in range(f["count"])]


def ascii_fail_reasons(cpset, adv, ink_of, sample="AagW1@"):
    """英文可用性的三个失败清单：缺字 / 零宽 / 空字模。

    抽成函数是为了**能自测**：selftest 拿一份"故意少了 A"的输入喂它，
    必须报出缺 A —— 否则这条判据就是永远绿的假守卫（铁律 4）。
    """
    missing  = [c for c in range(0x20, 0x7F) if c not in cpset]
    # ⚠️ 只判**在字库里**的那些：缺字已经在上一条报过了，
    #    再顺手当成"零宽"报一遍，同一个字符会红两次，看着像两处问题。
    zero_adv = [c for c in range(0x21, 0x7F) if c in cpset and adv.get(c, 0) == 0]
    blank    = [c for c in sample if c in cpset and ink_of(c) == 0]
    return missing, zero_adv, blank


def check(path, expect_depth=8, quiet=False):
    """返回 (ok, report_lines, stats)。ok=False 表示判据判红。

    ★ expect_depth 是**判据成立的前提**：我们的产品要求字库带抗锯齿，
      也就是"期望位深 >= 4"。若实际位深低于它（例如拿到一份 1bpp），
      那就是**缺陷**，必须判红 —— 而不是"1bpp 没有中间灰是正常的，放行"。

      ⚠️ 第一版就是写成"depth<4 则本判据不适用、直接通过"，
         结果 1bpp 字库一路绿灯 —— 典型的永远绿的假守卫（铁律 4）。
         selftest 当场把它抓了出来。判据必须表达**期望**，
         不能只描述**现状**。
    """
    rep = []
    f = read_font(path)
    d = f["data"]
    rep.append("字库   : %s  (%d 字节)" % (os.path.basename(path), len(d)))
    rep.append("头     : ver=%d flags=0x%04X -> %s  cell=%dx%d  glyphs=%d"
               % (f["ver"], f["flags"],
                  ("%dbit" % f["depth"]) if f["depth"] else "**未知位深**",
                  f["cw"], f["ch"], f["count"]))
    rep.append("期望   : >= %dbit（带抗锯齿）" % expect_depth)

    fail = []

    # ---- 0. 位深必须达到期望。这是最先要判的，别被后面的细节带跑偏 ----
    if f["depth"] is None:
        fail.append("0 头里位深编码 3 是保留值，程序会拒绝加载（font.c 返回 -9）")
    elif f["depth"] < expect_depth:
        fail.append("0 位深不足：实际 %dbit < 期望 %dbit。"
                    "这份字库没有抗锯齿 —— 渲染端再怎么混合也只是硬边。"
                    % (f["depth"], expect_depth))

    # ---- A. 结构性自洽：声明的大小 vs 按位深算出来的大小 ----
    if f["depth"] is not None:
        cb = cell_bytes(f["cw"], f["ch"], f["depth"])
        need = f["count"] * cb
        off_end = f["boff"] + need
        rep.append("A 位图 : 每字 %d 字节  需要 %d  文件末尾停在 %d / 实有 %d"
                   % (cb, need, off_end, len(d)))
        if off_end > len(d):
            fail.append("A 位图区越界：需要 %d 字节，实际只有 %d"
                        % (off_end, len(d)))
        if f["bsize"] != need:
            fail.append("A 头里 bitmap_size=%d 与按位深算出的 %d 不符"
                        % (f["bsize"], need))

    if fail:
        return False, rep + ["判红 :"] + ["    - " + x for x in fail], {}

    # ---- B / C. 灰阶统计（走到这里 depth 必然 >= expect_depth >= 4）----
    cps = cps_of(f)
    han = [i for i, cp in enumerate(cps) if 0x4E00 <= cp <= 0x9FFF]
    stats = {}
    gray = solid = 0
    for i in han:
        hasink = False
        for y in range(f["ch"]):
            for x in range(f["cw"]):
                if alpha_at(f, i, x, y) >= 32:
                    hasink = True
                    break
            if hasink:
                break
        if not hasink:
            continue
        for y in range(f["ch"]):
            for x in range(f["cw"]):
                v = alpha_at(f, i, x, y)
                if v >= 32:
                    solid += 1
                    if v < 255:
                        gray += 1
    ratio = gray / max(1, solid)
    stats["gray_ratio"] = ratio
    rep.append("B 灰阶 : 汉字 %d 字，笔画像素里中间灰占 %.3f（门槛 >=%.2f）"
               % (len(han), ratio, GRAY_RATIO_MIN))
    if ratio < GRAY_RATIO_MIN:
        fail.append("B 抗锯齿没出来：中间灰只占 %.3f < %.2f。"
                    "位图里几乎全是 0/255，说明生成的其实是二值图。"
                    % (ratio, GRAY_RATIO_MIN))

    # C. 抽样自洽：灰度版必须存在过渡像素
    step = max(1, len(han) // 400)
    extra = 0
    sampled = 0
    for i in han[::step]:
        sampled += 1
        for y in range(f["ch"]):
            for x in range(f["cw"]):
                v = alpha_at(f, i, x, y)
                if 32 <= v < 224:          # 既非笔画芯也非背景，只能是过渡
                    extra += 1
    stats["edge_px"] = extra
    rep.append("C 过渡 : 抽样 %d 字，边缘过渡像素 %d 个（>0 才算真有灰阶）"
               % (sampled, extra))
    if extra == 0:
        fail.append("C 抽样字形的边缘没有任何过渡像素 —— 灰度是假的")

    # ---- D. 英文（ASCII 可打印区）可用性 ----
    #
    # ★ 为什么要专门判这一条（2026-10-16 加）：
    #   这份字库是**为汉字**做的（16×16 点阵、全角 + 比例半角），英文是靠
    #   mkfont.py 里那句 `cps.update(range(0x20, 0x7F))` 顺带收进来的。
    #   哪天有人调字符集档位、换源字体、或者为了省体积裁掉一段，
    #   **最先丢的就是英文** —— 而界面上到处是英文（卡上路径、模型名、
    #   服务商名、动作名 file.write、HTTP 状态码、思考内容很可能是英文）。
    #   丢了以后**不崩、不报错**，屏幕上一片豆腐块，只会"看着怪怪的"，
    #   所以必须做成判据，不能靠人去发现。
    cps     = cps_of(f)
    cpset   = set(cps)
    ascii_n = 0x7F - 0x20                       # 0x20..0x7E 共 95 个
    adv     = {cp: d[f["ioff"] + i * 8 + 4]     # 索引项第 5 字节 = advance
               for i, cp in enumerate(cps)}

    def ink_of(cp):
        if f["depth"] is None:
            return 1                            # 位深未知时跳过"空字模"这条
        return sum(1 for y in range(f["ch"]) for x in range(f["cw"])
                   if alpha_at(f, cps.index(cp), x, y) >= 32)

    missing, zero_adv, blank = ascii_fail_reasons(cpset, adv, ink_of)

    rep.append("D 英文 : ASCII 0x20–0x7E 共 %d 个 · 缺 %d 个 · advance=0 的 %d 个"
               % (ascii_n, len(missing), len(zero_adv)))
    if missing:
        fail.append("D 字库缺这些 ASCII 字符：%s —— 界面上的路径/模型名/动作名"
                    "会变成豆腐块" % "".join(chr(c) for c in missing[:24]))
    if zero_adv:
        fail.append("D 这些 ASCII 字符 advance=0（画出来零宽，字会叠在一起）：%s"
                    % "".join(chr(c) for c in zero_adv[:24]))

    # 有 advance 不等于有字：字模全白的话，屏幕上是一片空白而不是豆腐。
    if blank:
        fail.append("D 这些英文/数字字符的字模是全空的：%s —— 有 advance 没内容，"
                    "屏幕上是个空白而不是字" % "".join(blank))

    ok = not fail
    if fail:
        rep.append("判红 :")
        rep += ["    - " + x for x in fail]
    else:
        rep.append("结论 : 通过 —— 位深达标，灰度真实存在，英文可显示。")
    return ok, rep, stats


def selftest():
    """反向验证：拿一份 1bpp 字库喂给判据，**必须判红**。

    没有这个，判据可能永远绿 —— 那它比没有更坏：它会给人"已经验过了"的错觉。
    做法：现场用 mkfont 生成一份 depth=1 字库，跑 check()，断言 ok=False。
    """
    import subprocess
    import tempfile
    here = os.path.dirname(os.path.abspath(__file__))
    root = os.path.dirname(here)
    py = sys.executable
    tmp = os.path.join(tempfile.gettempdir(), "aa_selftest_1bpp.bin")
    r = subprocess.run([py, os.path.join(here, "mkfont.py"),
                        "--depth", "1", "--out", tmp, "--no-preview",
                        "--quiet"],
                       cwd=root, capture_output=True)
    if not os.path.exists(tmp):
        print("  自测跳过：没能生成 1bpp 样本\n  %s" % r.stderr.decode("utf-8", "replace")[-300:])
        return False
    ok, rep, _ = check(tmp, expect_depth=8)
    print("\n".join(rep))
    if ok:
        print("\n  !! 自测失败：1bpp 字库竟然通过了抗锯齿判据 —— 判据是永远绿的假守卫")
        return False
    print("\n  ✓ 自测 1 通过：1bpp 样本被正确判红（判据真的能区分两条路）")

    # ---- 自测 2：英文判据必须能判红 ----
    # 拿一份"故意少了 A、且 g 的 advance=0"的输入喂进去，
    # 两条都要被报出来。只验"好输入全绿"的判据等于没有判据。
    gutted = set(range(0x20, 0x7F)) - {ord("A")}
    advs   = {c: 8 for c in gutted}
    advs[ord("g")] = 0
    miss, zero, _blank = ascii_fail_reasons(gutted, advs, lambda c: 1)
    if ord("A") not in miss or ord("g") not in zero:
        print("  !! 自测 2 失败：缺 A / g 零宽 都没被报出来（英文判据是假的）")
        return False
    print("  ✓ 自测 2 通过：缺字与零宽都被正确判红（英文判据不是永远绿的）")
    return True


def main():
    ap = argparse.ArgumentParser(description="字库抗锯齿判据")
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    ap.add_argument("--bin", default=os.path.join(root, "app", "data", "font.bin"))
    ap.add_argument("--expect-depth", type=int, default=8,
                    help="期望位深（产品要求带抗锯齿，默认 8）")
    ap.add_argument("--selftest", action="store_true")
    args = ap.parse_args()

    if args.selftest:
        return 0 if selftest() else 1

    try:
        ok, rep, _ = check(args.bin, expect_depth=args.expect_depth)
    except Exception as e:
        print("判据无法运行：%s" % e)
        return 1
    print("\n".join(rep))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
