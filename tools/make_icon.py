#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""
make_icon.py — 生成 3DS AI 控制台（AIConsole）的 home-menu 图标。

═══ 为什么这个文件存在 ══════════════════════════════════════════════

3DS 桌面上显示的是 **SMDH 图标**，而 SMDH 图标是 **RGB565 不透明方块**
（见 3dbrew SMDH：0x2040 起 small 24x24 / 0x24C0 起 large 48x48，都只有
RGB565，没有 alpha 通道）。所以「桌面 3D 模型」的真相是：home menu 用立体
视差让图标浮起来，而 SMDH 里有个 **3D 允许标志**（`flags` @ offset 0x2028，
bit 0x0002）。`patch_smdh_3d.py` 负责把 3D 标志打开。

这个脚本负责把 SMDH 那张"该有却一直用 citro3d 默认图标"的图，换成能代表
软件特色的吉祥物。3DS 图标在 48x48 / 24x24 下必须**剪影清楚、对比强、没有
细碎部件**（细耳朵/小星星在小尺寸会糊掉），所以这里提供几种**风格迥异**的方案，
`--all` 一次出图让你挑，挑中后再 `--style X` 落到 app/icon.png 供构建使用。

═══ 用法 ══════════════════════════════════════════════════════════════
    python tools/make_icon.py --style mono     # 写 app/icon.png (默认 mono)
    python tools/make_icon.py --style bubble   # 聊天气泡风
    python tools/make_icon.py --style robot    # 机器人头风
    python tools/make_icon.py --all            # 出三版候选 + app/icon_candidates.png 对比图
"""
from __future__ import print_function

import argparse
import os
import sys

from PIL import Image, ImageDraw, ImageFilter, ImageFont

HERE = os.path.dirname(os.path.abspath(__file__))
PROJ = os.path.abspath(os.path.join(HERE, ".."))
OUT_DEFAULT = os.path.join(PROJ, "app", "icon.png")
STYLES = ["mono", "bubble", "robot", "bubble3d"]

# ---- 配色（teal 主调 + navy 字 + 暖色点缀）----
TEAL_TOP = (40, 198, 214)      # 左上受光：亮青
TEAL_BOT = (8, 92, 110)        # 右下背光：深青（体积感来源）
PANEL = (255, 255, 255)        # 白色机身/气泡
INK = (14, 32, 52)             # navy 文字 / 眼睛
RIM = (173, 247, 255)          # 边缘高光：极亮青
ACCENT = (255, 209, 74)        # 暖黄：天线灯 / 点缀


def _lerp(a, b, t):
    return tuple(int(round(a[i] + (b[i] - a[i]) * t)) for i in range(3))


def _find_font(size):
    """挑一个能用的粗体 TrueType；失败就退 Pillow 自带。"""
    for p in (r"C:\Windows\Fonts\msyhbd.ttf",
              r"C:\Windows\Fonts\seguibl.ttf",
              r"C:\Windows\Fonts\arialbd.ttf"):
        if os.path.exists(p):
            try:
                return ImageFont.truetype(p, size)
            except Exception:
                pass
    return ImageFont.load_default()


def _bg(img, d, size):
    """整张画布铺青色渐变（左上亮 -> 右下暗），制造体积感。"""
    for y in range(size):
        t = y / float(size - 1)
        d.line([(0, y), (size, y)], fill=_lerp(TEAL_TOP, TEAL_BOT, t))


def _ai_text(d, cx, cy, size, font_size, fill=INK):
    font = _find_font(int(size * font_size))
    bb = d.textbbox((0, 0), "AI", font=font)
    tw, th = bb[2] - bb[0], bb[3] - bb[1]
    d.text((cx - tw / 2 - bb[0], cy - th / 2 - bb[1]), "AI", fill=fill, font=font)


# ---- 风格 1：纯字标（最干净，app 图标感）----
def build_mono(size):
    img = Image.new("RGB", (size, size), TEAL_BOT)
    d = ImageDraw.Draw(img)
    _bg(img, d, size)
    # 中心柔光，让 "AI" 像发光
    glow = Image.new("L", (size, size), 0)
    ImageDraw.Draw(glow).ellipse([size * 0.16, size * 0.14, size * 0.84, size * 0.92], fill=55)
    glow = glow.filter(ImageFilter.GaussianBlur(size * 0.07))
    img = Image.composite(Image.new("RGB", (size, size), (120, 235, 250)), img, glow)
    d = ImageDraw.Draw(img)
    # 极亮青描边把图标从背景抠出来
    d.rounded_rectangle([4, 4, size - 4, size - 4], radius=int(size * 0.16),
                        outline=RIM, width=max(3, int(size * 0.022)))
    # 粗体 AI 居中
    _ai_text(d, size / 2, size / 2, size, 0.52)
    return img


# ---- 风格 2：聊天气泡（一眼是「AI 助手/对话」）----
def build_bubble(size):
    img = Image.new("RGB", (size, size), TEAL_BOT)
    d = ImageDraw.Draw(img)
    _bg(img, d, size)
    pad = int(size * 0.15)
    bx0, bx1 = pad, size - pad
    by0, by1 = int(size * 0.12), int(size * 0.70)
    rad = int(size * 0.15)
    # 气泡主体（白面板）
    d.rounded_rectangle([bx0, by0, bx1, by1], radius=rad, fill=PANEL)
    # 气泡尾巴（朝下小三角）
    tw = int(size * 0.18)
    d.polygon([(size / 2 - tw / 2, by1), (size / 2 + tw / 2, by1),
               (size / 2, by1 + int(size * 0.16))], fill=PANEL)
    # 亮青描边（只描主体，不描尾巴避免双线）
    d.rounded_rectangle([bx0, by0, bx1, by1], radius=rad, outline=RIM,
                        width=max(2, int(size * 0.016)))
    # AI 在气泡里
    _ai_text(d, (bx0 + bx1) / 2, (by0 + by1) / 2, size, 0.40)
    return img


# ---- 风格 2b：立体质感气泡（玻璃球感：落影 + 左上高光 + 右下暗面）----
def build_bubble3d(size):
    img = Image.new("RGB", (size, size), TEAL_BOT)
    d = ImageDraw.Draw(img)
    _bg(img, d, size)
    pad = int(size * 0.14)
    bx0, bx1 = pad, size - pad
    by0, by1 = int(size * 0.12), int(size * 0.70)
    rad = int(size * 0.18)
    # 落影：整块气泡的投影，让它"浮"在桌面上
    sh = Image.new("L", (size, size), 0)
    ImageDraw.Draw(sh).rounded_rectangle(
        [bx0 + size * 0.03, by0 + size * 0.05, bx1 - size * 0.03, by1 + size * 0.10],
        radius=rad, fill=120)
    sh = sh.filter(ImageFilter.GaussianBlur(size * 0.035))
    img = Image.composite(Image.new("RGB", (size, size), (2, 26, 36)), img, sh)
    # 气泡层（独立 RGBA，方便叠加光影）
    layer = Image.new("RGBA", (size, size), (0, 0, 0,0))
    ld = ImageDraw.Draw(layer)
    ld.rounded_rectangle([bx0, by0, bx1, by1], radius=rad, fill=(255, 255, 255, 255))
    tw = int(size * 0.18)
    ld.polygon([(size / 2 - tw / 2, by1), (size / 2 + tw / 2, by1),
                (size / 2, by1 + int(size * 0.16))], fill=(255, 255, 255, 255))
    # 顶部高光（柔白），底部暗面（让球有体积）
    hi = Image.new("L", (size, size), 0)
    ImageDraw.Draw(hi).ellipse(
        [bx0 + size * 0.10, by0 + size * 0.06, bx0 + size * 0.62, by0 + size * 0.42], fill=235)
    hi = hi.filter(ImageFilter.GaussianBlur(size * 0.05))
    lo = Image.new("L", (size, size), 0)
    ImageDraw.Draw(lo).ellipse(
        [bx0 + size * 0.28, by0 + size * 0.45, bx1 - size * 0.02, by1 + size * 0.06], fill=120)
    lo = lo.filter(ImageFilter.GaussianBlur(size * 0.05))
    lr, hr, lor = layer.load(), hi.load(), lo.load()
    for y in range(size):
        for x in range(size):
            r, g, b, a = lr[x, y]
            if a <= 0:
                continue
            f = lor[x, y] / 255.0          # 压暗底部
            r = int(r * (1 - 0.38 * f)); g = int(g * (1 - 0.38 * f)); b = int(b * (1 - 0.38 * f))
            f2 = hr[x, y] / 255.0          # 提亮顶部
            r = int(r + (255 - r) * 0.55 * f2); g = int(g + (255 - g) * 0.55 * f2); b = int(b + (255 - b) * 0.55 * f2)
            lr[x, y] = (r, g, b, a)
    ImageDraw.Draw(layer).rounded_rectangle(
        [bx0, by0, bx1, by1], radius=rad, outline=(173, 247, 255, 255),
        width=max(2, int(size * 0.016)))
    img = Image.alpha_composite(img.convert("RGBA"), layer).convert("RGB")
    d = ImageDraw.Draw(img)
    _ai_text(d, (bx0 + bx1) / 2, (by0 + by1) / 2 - size * 0.02, size, 0.40)
    return img


# ---- 风格 3：机器人头（友好、有「AI 生物」感但不细碎）----
def build_robot(size):
    img = Image.new("RGB", (size, size), TEAL_BOT)
    d = ImageDraw.Draw(img)
    _bg(img, d, size)
    ax = size / 2
    # 天线（粗线 + 暖黄灯），比细耳朵在小尺寸更立得住
    d.line([(ax, int(size * 0.09)), (ax, int(size * 0.23))], fill=INK,
           width=max(3, int(size * 0.028)))
    d.ellipse([ax - int(size * 0.045), int(size * 0.04),
               ax + int(size * 0.045), int(size * 0.13)], fill=ACCENT)
    # 头（白圆角方块）
    hx0, hx1 = int(size * 0.19), int(size * 0.81)
    hy0, hy1 = int(size * 0.22), int(size * 0.84)
    rad = int(size * 0.12)
    d.rounded_rectangle([hx0, hy0, hx1, hy1], radius=rad, fill=PANEL)
    d.rounded_rectangle([hx0, hy0, hx1, hy1], radius=rad, outline=RIM,
                        width=max(2, int(size * 0.016)))
    # 两只眼睛
    ey = int(size * 0.40)
    for ex in (size * 0.38, size * 0.62):
        r = size * 0.05
        d.ellipse([ex - r, ey - r, ex + r, ey + r], fill=INK)
    # 下方 "AI" 当嘴/铭牌
    _ai_text(d, ax, int(size * 0.63), size, 0.28)
    return img


BUILDERS = {"mono": build_mono, "bubble": build_bubble, "robot": build_robot, "bubble3d": build_bubble3d}


def build(style="mono", size=256):
    return BUILDERS[style](size)


def _montage():
    """把三版候选按 256 + 48 实际 拼成对比图，供肉眼挑。"""
    from PIL import Image
    cols = len(STYLES)
    cell = 200
    W = cols * cell + (cols + 1) * 20
    H = 460
    cv = Image.new("RGB", (W, H), (28, 34, 42))
    dd = ImageDraw.Draw(cv)

    def label(xy, text, sz=18):
        try:
            f = ImageFont.truetype(r"C:\Windows\Fonts\msyhbd.ttf", sz)
        except Exception:
            f = ImageFont.load_default()
        dd.text(xy, text, fill=(220, 230, 235), font=f)

    for i, s in enumerate(STYLES):
        x = 20 + i * (cell + 20)
        big = build(s, 256).resize((cell, cell), Image.LANCZOS)
        cv.paste(big, (x, 20))
        sm = build(s, 48)
        cv.paste(sm.resize((cell, cell), Image.NEAREST), (x, 20 + cell + 30))
        label((x, 20 + cell + 10), s, 18)
        label((x, 20 + 2 * cell + 40), "48px 实", 14)
    cv.save(os.path.join(PROJ, "app", "icon_candidates.png"))
    print("candidates -> app/icon_candidates.png")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default=OUT_DEFAULT)
    ap.add_argument("--size", type=int, default=256)
    ap.add_argument("--style", choices=STYLES, default="mono")
    ap.add_argument("--all", action="store_true")
    args = ap.parse_args()

    if args.all:
        for s in STYLES:
            im = build(s, args.size)
            out = os.path.join(PROJ, "app", "icon_%s.png" % s)
            im.save(out)
            print("icon[%s] -> %s (%dx%d)" % (s, out, im.width, im.height))
        _montage()
        return 0

    img = build(args.style, args.size)
    out = os.path.abspath(args.out)
    img.save(out)
    print("icon -> %s (%dx%d, style=%s)" % (out, img.width, img.height, args.style))
    return 0


if __name__ == "__main__":
    sys.exit(main())
