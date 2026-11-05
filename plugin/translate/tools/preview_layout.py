"""
preview_layout.py —— 用 PC 把插件的两块界面按**同一套坐标**画出来

为什么值得做：3gx 只能在真机跑，Azahar 不加载。而"文字撞在一起/超边界"
是这类自绘 UI 最常见的错，跟 libctru 一点关系都没有 —— 用 PIL 按 hud.c /
main.c 里那几行坐标重画一遍，就能提前看见。

它验的是**版面**，不是插件能不能加载。这个区别要记住。

用法：python tools/preview_layout.py
输出：preview_settings.png / preview_panel.png
"""

import os

from PIL import Image, ImageDraw, ImageFont

HERE = os.path.dirname(os.path.abspath(__file__))
FONT = r"C:\Windows\Fonts\simsun.ttc"

# 与 hud.c 的配色保持一致
BG      = (10, 12, 16)
BAR     = (40, 70, 80)
SEL     = (26, 62, 72)
TITLE   = (120, 220, 210)
FG      = (190, 200, 210)
FG_SEL  = (240, 245, 250)
VAL     = (150, 220, 190)
HINT    = (120, 150, 160)
FRAME   = (90, 200, 190)
PANEL   = (24, 28, 34)
PANEL_FG = (235, 240, 245)

ROW_LABEL = ["显示在哪块屏", "翻译屏位置", "宽度（字符）",
             "高度（字符）", "背板深浅", "触发键"]
ROW_VALUE = ["下屏", "左下", "34", "6", "中", "SELECT"]


def settings(w=320, h=240):
    img = Image.new("RGB", (w, h), BG)
    d = ImageDraw.Draw(img)
    f = ImageFont.truetype(FONT, 16)

    d.text((10, 8), "翻译插件 · 设置", font=f, fill=TITLE)
    d.rectangle([10, 30, w - 11, 30], fill=BAR)

    for i in range(6):
        y = 42 + i * 26
        if i == 2:                     # 模拟光标停在第三行
            d.rectangle([6, y - 5, w - 7, y - 5 + 24], fill=SEL)
        d.text((14, y - 2), ROW_LABEL[i], font=f,
               fill=FG_SEL if i == 2 else FG)
        vw = d.textlength(ROW_VALUE[i], font=f)
        d.text((w - 14 - vw, y - 2), ROW_VALUE[i], font=f, fill=VAL)

    d.rectangle([10, h - 44, w - 11, h - 44], fill=BAR)
    d.text((10, h - 42), "↑↓ 选择    ←→ 修改", font=f, fill=HINT)
    d.text((10, h - 22), "A 保存      B 退出", font=f, fill=HINT)
    img.save(os.path.join(HERE, "..", "preview_settings.png"))
    print("设置页 →", "preview_settings.png")


def panel(w=320, h=240, cols=34, rows=6):
    """翻译屏：cfg_panel_rect 的 BL 锚点，34 列 × 6 行。"""
    pw, ph = cols * 8, rows * 16
    if pw > w: pw = w
    if ph > h: ph = h
    x, y = 8, h - ph - 8           # 左下角

    img = Image.new("RGB", (w, h), (70, 90, 120))   # 假装是游戏画面
    d = ImageDraw.Draw(img)
    d.rectangle([x, y, x + pw - 1, y + ph - 1], fill=PANEL)
    d.rectangle([x, y, x + pw - 1, y + ph - 1], outline=FRAME)

    f = ImageFont.truetype(FONT, 16)
    lines = ["抓屏成功：320 x 240（225 KB）",
             "AI 上传还没接上",
             "第二行示意：这是一段很长的译文，用来",
             "检查会不会顶到右边框外面去。"]
    ty = y + 6
    for ln in lines:
        if ty + 16 > y + ph - 6:
            break
        # 按宽度裁，模拟 hud.c 里 x + 16 <= max_x 的判定
        cut, tw = "", 0
        for ch in ln:
            cw = d.textlength(ch, font=f)
            if tw + cw > pw - 12:
                break
            cut += ch
            tw += cw
        d.text((x + 6, ty - 2), cut, font=f, fill=PANEL_FG)
        ty += 18

    img.save(os.path.join(HERE, "..", "preview_panel.png"))
    print("翻译屏 →", "preview_panel.png  （%dx%d @ %d,%d）" % (pw, ph, x, y))


HELP_TEXT = """这是什么
游戏运行时按一个键，把当前画面截下来发给 AI，译文显示在一块可以自己摆位置的翻译屏上。

按键
短按 SELECT：截屏并翻译
长按 SELECT 约一秒：打开设置
设置里 ↑↓ 选行，←→ 改值
A 保存并退出，B 不保存退出

设置项
显示在哪块屏：上屏画面大，下屏不挡手
翻译屏位置：九宫格，贴着哪个角
宽/高：以字符为单位，1 字符 = 8×16 像素
背板深浅：3DS 没有半透明，只能调底色
触发键：SELECT 或 ZL（ZL 只有新三有）

译文放不下怎么办
先调高、再调宽。宽到 40 字符就顶满下屏了，再宽会被屏幕裁掉。

现在还没做好的
截屏已经能用了，面板上会显示抓到的分辨率。把画面传给 AI 这一步还没接 —— 所以你现在按下去，看到的是“抓屏成功”，不是译文。

真机出问题先看这里
面板不出现：多半是游戏自己管着显示服务，换个游戏试试，能出来的话就是这个原因。
按键没反应：确认 Luma 的 Plugin Loader 开着。
截出来的图是黑的：说明抓到的是插件自己的空画面，这条要靠改抓屏方式解决，不是设置问题。
游戏直接崩：插件占的内存太多，这个要改插件打包时的内存设置。

设置文件
存在 sdmc:/3ds/aiconsole/plugin.cfg，也能用电脑记事本直接改，下次进游戏生效。

版本
翻译插件 0.2.0　作者 AI Console"""


def wrap(text, cols=18):
    out = []
    for para in text.split("\n"):
        if not para:
            out.append("")
            continue
        line = ""
        for ch in para:
            if len(line) >= cols:
                out.append(line)
                line = ""
            line += ch
        out.append(line)
    return out


def help_page(idx=0, cols=18, per=9, w=320, h=240):
    lines = wrap(HELP_TEXT, cols)
    img = Image.new("RGB", (w, h), BG)
    d = ImageDraw.Draw(img)
    f = ImageFont.truetype(FONT, 16)

    d.text((10, 6), "使用说明", font=f, fill=TITLE)
    d.rectangle([10, 28, w - 11, 28], fill=BAR)

    y = 34
    for i in range(per):
        k = idx * per + i
        if k >= len(lines):
            break
        if y + 16 > h - 24:
            break
        head = (k + 1 < len(lines) and lines[k + 1] == "" and lines[k] != "")
        d.text((10, y - 2), lines[k], font=f,
               fill=(235, 240, 245) if head else (195, 205, 215))
        y += 18

    d.rectangle([10, h - 22, w - 11, h - 22], fill=BAR)
    d.text((10, h - 20), "↑↓ 滚动   ←→ 翻页   %d/%d   B 返回"
           % (idx + 1, (len(lines) + per - 1) // per), font=f, fill=HINT)
    img.save(os.path.join(HERE, "..", "preview_help.png"))
    print("说明页 → preview_help.png（共 %d 行，%d 页）"
          % (len(lines), (len(lines) + per - 1) // per))


if __name__ == "__main__":
    settings()
    panel()
    help_page(0)
