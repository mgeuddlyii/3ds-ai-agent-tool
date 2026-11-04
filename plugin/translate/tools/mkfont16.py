"""
mkfont16.py —— 把中文字体烤成 16×16 1bpp 点阵，供 3gx 插件内嵌

为什么不用 citro2d 直接调系统字库：
    插件跑在**游戏进程**里，citro2d 初始化会接管 GPU 状态，和正在跑的游戏
    抢资源 —— 那是最容易把游戏搞崩的一条路。点阵 blit 只往帧缓冲写字节，
    不碰 GPU 状态机。

为什么只烤 GB2312 一级 + ASCII：
    一级汉字 3755 个覆盖日常文本 99% 以上，加 ASCII 大约 140KB。
    二级汉字（另 3000 多个）大多是生僻字，先把体积和验证成本压住。

产物：
    font16.c  —— 位图 + (码点, 序号) 有序索引，插件里二分查找
    font16_preview.png —— 抽样预览，用来肉眼检查有没有糊/缺字

用法：
    python tools/mkfont16.py            # 生成
    python tools/mkfont16.py --preview  # 只出预览
"""

import os
import sys

from PIL import Image, ImageDraw, ImageFont

HERE = os.path.dirname(os.path.abspath(__file__))
FONT_CANDIDATES = [
    # 宋体优先：Windows 的宋体在 16px 就是按点阵设计的，
    # 雅黑是矢量字体，缩到 16px 再二值化会断笔（两种都烤出来对比过）。
    r"C:\Windows\Fonts\simsun.ttc",
    r"C:\Windows\Fonts\msyh.ttc",
]
PX = 16
OUT_C = os.path.join(HERE, "..", "font16.c")
OUT_PNG = os.path.join(HERE, "..", "font16_preview.png")


def pick_font():
    for p in FONT_CANDIDATES:
        if os.path.isfile(p):
            return p
    raise SystemExit("找不到中文字体（msyh.ttc / simsun.ttc）")


def codepoints():
    """ASCII 可打印 + GB2312 全表（符号区 + 一级 + 二级首段）。

    GB2312 的区位到 Unicode 直接用 Python 的 gb2312 解码即可 ——
    不要手写映射表，那玩意儿写错一个字节就是整片乱码。
    """
    cps = list(range(0x20, 0x7F))
    for hi in range(0xA1, 0xF8):
        for lo in range(0xA1, 0xFF):
            try:
                ch = bytes([hi, lo]).decode("gb2312")
            except UnicodeDecodeError:
                continue
            cps.append(ord(ch))
    return sorted(set(cps))


def render_cp(font, cp):
    """把一个码点画成 16×16 的 0/1 点阵（1 = 亮）。

    阈值取 90：雅黑在 16px 下笔画偏细，阈值高了会断笔，低了会糊成一片。
    """
    img = Image.new("L", (PX, PX), 0)
    d = ImageDraw.Draw(img)
    try:
        d.text((0, 0), chr(cp), font=font, fill=255)
    except Exception:
        return None
    px = img.load()
    bits = bytearray(PX * 2)
    for y in range(PX):
        row = 0
        for x in range(PX):
            if px[x, y] >= 90:
                row |= 1 << (15 - x)
        bits[y * 2] = (row >> 8) & 0xFF
        bits[y * 2 + 1] = row & 0xFF
    return bytes(bits)


def main():
    only_preview = "--preview" in sys.argv
    font = ImageFont.truetype(pick_font(), PX)
    cps = codepoints()

    glyphs = []
    keep = []
    for cp in cps:
        b = render_cp(font, cp)
        if b is None:
            continue
        glyphs.append(b)
        keep.append(cp)
    print("码点 %d 个，位图 %d KB" % (len(keep), len(glyphs) * 32 // 1024))

    # ---- 预览：抽样排成一张图，肉眼检查 ----
    sample = [c for c in keep if c > 0x4E00][:96] + [c for c in keep if c < 0x80][:32]
    cols = 16
    rows = (len(sample) + cols - 1) // cols
    img = Image.new("L", (cols * (PX + 2), rows * (PX + 2)), 30)
    d = ImageDraw.Draw(img)
    for i, cp in enumerate(sample):
        b = glyphs[keep.index(cp)]
        cx = (i % cols) * (PX + 2)
        cy = (i // cols) * (PX + 2)
        for y in range(PX):
            row = (b[y * 2] << 8) | b[y * 2 + 1]
            for x in range(PX):
                if row & (1 << (15 - x)):
                    d.point((cx + x, cy + y), fill=255)
    img.save(OUT_PNG)
    print("预览：", OUT_PNG)

    if only_preview:
        return

    # ---- 生成 C ----
    with open(OUT_C, "w", encoding="utf-8", newline="\n") as f:
        f.write("/* 由 tools/mkfont16.py 生成，别手改。\n"
                " * %d 个字形，16x16 1bpp，每字形 32 字节，行优先、高位在左。 */\n\n"
                % len(glyphs))
        f.write("#include \"font16.h\"\n\n")
        f.write("const unsigned char g_font16_bits[] = {\n")
        for i, g in enumerate(glyphs):
            f.write("    " + ",".join("0x%02X" % b for b in g) + ",  /* %d */\n" % i)
        f.write("};\n\n")
        f.write("const Font16Entry g_font16_index[] = {\n")
        for i, cp in enumerate(keep):
            f.write("    {0x%04X,%d},\n" % (cp, i))
        f.write("};\n\n")
        f.write("const int g_font16_count = %d;\n" % len(keep))
    print("生成：", OUT_C, os.path.getsize(OUT_C) // 1024, "KB")


if __name__ == "__main__":
    main()
