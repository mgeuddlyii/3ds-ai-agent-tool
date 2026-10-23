#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
preview_screen.py -- 按 3DS 顶屏真实尺寸（400x240）渲染一屏 UI 文字，
用于在 PC 上直观对比 1bpp / 8bit 两种字库的观感。

★ 为什么用"已烘焙的 font.bin"而不是直接调 Pillow 排字：
  直接排字得到的是"字体本来长什么样"，验不到我们自己的位图与混合逻辑。
  读 font.bin 再按 render_2d.c 的读法/混合公式重画，才是"设备所见"。

★ 混合公式与 render_2d.c 的 canvas_blend()、mkfont.py 的 draw_cell()
  三处必须一致（铁律 6b）。

用法：
    python tools/preview_screen.py --a OLD.bin --b NEW.bin --out cmp.png
"""

import argparse
import os
import struct
import sys

from PIL import Image

HEADER_FMT = "<4sHHBBBBIIIII"
HEADER_SIZE = struct.calcsize(HEADER_FMT)

#: 与 app/source/gfx/render.h 的 R_TOP_W / R_TOP_H 同源
SCREEN_W = 400
SCREEN_H = 240

#: 与 app/source/gfx/text.h 的 TEXT_LINE_H 同源
LINE_H = 18

#: 与 app/source/gfx/render_2d.c 的 canvas_blend() 同源。
#: dst = (dst*(255-a) + src*a + 127) / 255
def blend(dst, src, a):
    if a >= 255:
        return src
    if a == 0:
        return dst
    inv = 255 - a
    return tuple((dst[i] * inv + src[i] * a + 127) // 255 for i in range(3))


class Font:
    def __init__(self, path):
        d = open(path, "rb").read()
        (magic, ver, flags, cw, ch, bl, _r, count, ioff, boff, bsize, fb) = \
            struct.unpack_from(HEADER_FMT, d, 0)
        if magic != b"AFNT":
            raise ValueError("不是 AFNT：%r" % magic)
        self.d, self.ver, self.flags = d, ver, flags
        self.cw, self.ch, self.bl = cw, ch, bl
        self.count, self.ioff, self.boff, self.fb = count, ioff, boff, fb
        self.depth = {0: 1, 1: 4, 2: 8}.get(flags & 3, 1)
        self.idx = {}
        for i in range(count):
            cp = struct.unpack_from("<I", d, ioff + i * 8)[0]
            adv = d[ioff + i * 8 + 4]
            fl = d[ioff + i * 8 + 5]
            self.idx[cp] = (adv, fl, i)

    def cell_bytes(self):
        return self.cw * self.ch * self.depth // 8

    def alpha(self, i, x, y):
        if not (0 <= x < self.cw and 0 <= y < self.ch):
            return 0
        base = self.boff + i * self.cell_bytes()
        if self.depth == 1:
            b = self.d[base + y * 2 + (0 if x < 8 else 1)]
            return 255 if (b & (0x80 >> (x & 7))) else 0
        k = y * self.cw + x
        if self.depth == 8:
            return self.d[base + k]
        b = self.d[base + (k >> 1)]
        nib = (b >> 4) if (k % 2 == 0) else (b & 0x0F)
        return nib * 17

    def draw(self, img, x, y, cp, fg):
        e = self.idx.get(cp) or self.idx.get(self.fb)
        if not e:
            return 0
        adv, _fl, i = e
        px = img.load()
        for gy in range(self.ch):
            sy = y + gy
            if not (0 <= sy < SCREEN_H):
                continue
            for gx in range(self.cw):
                sx = x + gx
                if not (0 <= sx < SCREEN_W):
                    continue
                a = self.alpha(i, gx, gy)
                if a:
                    px[sx, sy] = blend(px[sx, sy], fg, a)
        return adv

    def text(self, img, x, y, s, fg):
        for ch in s:
            x += self.draw(img, x, y, ord(ch), fg)
        return x


#: 模拟一屏主界面文字（内容取自项目真实 UI 用语）
DEMO = [
    ("AI 助手控制台", 0x1D9E75),
    ("", 0),
    ("开始对话       查攻略        设置", 0x2C2C2A),
    ("从电脑取文件   系统自检      语音输入", 0x2C2C2A),
    ("", 0),
    ("状态：已连接 · 区域：中国 · L0", 0x5F5E5A),
    ("所有危险操作都会先弹出确认框。", 0x5F5E5A),
    ("", 0),
    ("数字 0123456789  符号 ，。、；：？！（）《》「」", 0x2C2C2A),
    ("The quick brown fox jumps over the lazy dog.", 0x2C2C2A),
    ("混排 Mix 中文 + English + 123 + ，。 end.", 0x2C2C2A),
    ("我能帮你查攻略、调亮度、管理 SD 卡文件。", 0x5F5E5A),
]


def render(font, bg=(255, 255, 255)):
    img = Image.new("RGB", (SCREEN_W, SCREEN_H), bg)
    y = 8
    for s, col in DEMO:
        if s:
            fg = ((col >> 16) & 0xFF, (col >> 8) & 0xFF, col & 0xFF)
            font.text(img, 30, y, s, fg)
        y += LINE_H
    return img


def main():
    ap = argparse.ArgumentParser()
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    ap.add_argument("--a", default=os.path.join(root, "app", "data", "font.bin.v1bak"))
    ap.add_argument("--b", default=os.path.join(root, "app", "data", "font.bin"))
    ap.add_argument("--out", default=os.path.join(root, "tools", "out", "screen_aa_cmp.png"))
    ap.add_argument("--zoom", type=int, default=2)
    args = ap.parse_args()

    fa = Font(args.a)
    fb = Font(args.b)
    ia, ib = render(fa), render(fb)

    zoom = max(1, args.zoom)
    W, H = SCREEN_W * zoom, SCREEN_H * zoom
    ia = ia.resize((W, H), Image.NEAREST)
    ib = ib.resize((W, H), Image.NEAREST)

    # 每屏上方加一条标签带（高 BAND），上下叠放。
    # ⚠️ 别把标签画在屏幕内容上 —— 会盖住字、也看不清标签（第一版就踩了这个）。
    BAND = 22
    out = Image.new("RGB", (W, (H + BAND) * 2), (232, 232, 232))
    out.paste(ia, (0, BAND))
    out.paste(ib, (0, H + BAND * 2))

    from PIL import ImageDraw
    d = ImageDraw.Draw(out)
    d.rectangle((0, 0, W, BAND), fill=(60, 60, 60))
    d.text((8, 6), "1bpp (old)  %d B/glyph  -- jagged" % fa.cell_bytes(),
           fill=(255, 255, 255))
    d.rectangle((0, H + BAND, W, H + BAND * 2), fill=(15, 110, 86))
    d.text((8, H + BAND + 6), "%dbit (new)  %d B/glyph  -- antialiased"
           % (fb.depth, fb.cell_bytes()), fill=(255, 255, 255))

    os.makedirs(os.path.dirname(args.out), exist_ok=True)
    out.save(args.out)
    print("写出", args.out, out.size,
          "| a=%dbit b=%dbit" % (fa.depth, fb.depth))
    return 0


if __name__ == "__main__":
    sys.exit(main())
