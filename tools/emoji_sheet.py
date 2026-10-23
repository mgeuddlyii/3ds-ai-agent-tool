# -*- coding: utf-8 -*-
"""把 emoji 子集字体按 mkfont 同参数（13px / 4x 超采样 / 16x16 格 / 8bit 灰度）
逐字烘焙，拼一张接触表，用 Read 直接看效果。"""
import os
import sys
from PIL import Image, ImageDraw, ImageFont

S = 4
SIZE = 13
CELL = 16
PAD = 8
BASELINE = 13

FONT = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                    "fonts", "NotoEmoji-subset.ttf")

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from make_emoji_font import wanted  # noqa: E402

f = ImageFont.truetype(FONT, SIZE * S)
cps = wanted()

COLS = 32
ZOOM = 3
rows = (len(cps) + COLS - 1) // COLS
W = COLS * CELL * ZOOM
H = rows * CELL * ZOOM

canvas = Image.new("L", (W, H), 245)
d = ImageDraw.Draw(canvas)

pw = (CELL + PAD * 2 + 4) * S
ph = (CELL + PAD * 2 + 4) * S
big = Image.new("L", (pw, ph), 0)
bd = ImageDraw.Draw(big)
tx, ty = (PAD + 2) * S, (PAD + 2) * S

for i, cp in enumerate(cps):
    bd.rectangle((0, 0, pw, ph), fill=0)
    try:
        bd.text((tx + 1 * S, ty + BASELINE * S), chr(cp), font=f, fill=255,
                anchor="ls")
    except Exception:
        pass
    small = big.resize((CELL, CELL), Image.BOX)     # 面积平均 = pack_gray 同源
    gx = (i % COLS) * CELL * ZOOM
    gy = (i // COLS) * CELL * ZOOM
    for y in range(CELL):
        for x in range(CELL):
            a = small.getpixel((x, y))
            if a:
                canvas.paste(255 - a, (gx + x * ZOOM, gy + y * ZOOM,
                                       gx + x * ZOOM + ZOOM,
                                       gy + y * ZOOM + ZOOM))

out = os.path.join(os.path.dirname(os.path.abspath(__file__)), "out",
                   "emoji_sheet.png")
os.makedirs(os.path.dirname(out), exist_ok=True)
canvas.save(out)
print("saved", out, canvas.size, "glyphs", len(cps))
