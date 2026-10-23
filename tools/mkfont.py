#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
mkfont.py -- 3DS AI Console 字库生成器

把一份 OFL 字体（默认 Noto Sans SC）离线渲染成位图，
导出给程序内嵌使用的 font.bin，并顺手出一张预览图。

产物去向：app/data/font.bin
    由 devkitPro 的 bin2s 编成 font_bin.o 直接链进可执行文件
    （app/source/gfx/font.c 里 extern 引用）。

    为什么不是 romfs：makerom 0.19 的 -romfs 不接受 mkromfs3ds 产出的
    标准 RomFS 镜像（试过带/不带目录哈希表两种布局，都报
    "Invalid RomFS Binary"），所以 .cia 装不进 romfs。而 .3dsx 的 romfs
    又只能在模拟器里用。字库反正要整份读进内存，正好用 bin2o 绕开这个坑。
    详见 工具链说明.md「makerom -romfs 不可用」。

设计要点
--------
* 格子固定 16x16，**位深可配**（--depth 1 / 4 / 8），行主序。
* 汉字/全角字符**步进 16**（16x16 网格，400px 宽的顶屏 = 25 字/行）；
  半角 ASCII 用字体自身的比例步进（4~12 px）。
* 全字库共用**同一条基线**（格内第 13 行），所以中英混排不会错位。
* 逐字检查墨迹是否跑出 16x16，超界的字会被列出来（不静默截断）。
* **超采样 + 面积覆盖**决定墨量（见下），不是直接硬阈值 —— 硬阈值会把
  13px 的斜画/细横判断，屏幕上就是"字缺一截"。

位深：为什么默认 8（灰度抗锯齿）
--------------------------------
**1bpp 的病根不是"字形丑"，是每像素只有「有墨 / 没墨」两个状态。**
渲染端 r_glyph() 只能对"有墨"的像素写死一个前景色，斜边必然是台阶；
抗锯齿的灰阶在**生成期**就被二值化丢掉了，后端再怎么做也救不回来。

depth=8 时，位图里存的是**每个像素的墨量覆盖率**（0..255，即 8bit alpha），
渲染端按 alpha 与底色混合 —— 斜边被插值成灰阶过渡，这就是"光滑圆润"。
社区标准做法（citro2d + BCFNT）也是同一条路：字形表选 A8（单通道 8 位
alpha）+ 线性过滤，光滑感来自插值，不是来自更圆的黑体。

depth=1 保留是为了**对照实验与回归**：判据要能区分两条路的输出，
所以必须留着产出"纯二值"字库的能力（见 tools/check_font_aa.py）。

为什么用「画进大画布再裁」而不是直接画进 16x16
-----------------------------------------------
直接画进 16x16 时，Pillow 会把越界笔画**静默丢掉**，等于悄悄裁字。
先画进带边距的大画布，就能算准墨迹范围、发现越界，再精确裁出目标区。

为什么是超采样 + 面积覆盖（而不是调阈值）
----------------------------------------
硬阈值是"每像素独立二值化"。13px 汉字在 16x16 格里，撇/捺/细横的
抗锯齿边缘只落在半灰上，一卡就断 —— 用**连通块数**一量就露馅
（正常汉字 1~2 块）：
    阈值 128 → 4.50 块/字、孤点 1.60
    阈值  96 → 3.45 块/字、孤点 0.84   ← 仍在断（全库口径）
    超采样   → 1.80 块/字、孤点 0.16   ← 采用
做法：按 SUPERSAMPLE 倍率放大渲染，每个格子数覆盖了多少个"够亮"的
亚像素，达到 COVER_NEED 才出墨。细笔画（覆盖 5~8/16）保住了，
抗锯齿的极浅边（覆盖 1~4/16）仍被丢掉，所以断线消失但不增肥。

文件格式 AFNT v2
----------------
    Header 32 B
      0x00 char[4]  magic  "AFNT"
      0x04 u16      version = 2
      0x06 u16      flags   bit0..1 = 位深编码（0=1bpp / 1=4bpp / 2=8bpp）
                              bit2 = 1 表示位图区是 8bit 覆盖率（冗余标记，
                              供调试器一眼看清，不必反推 flags）
      0x08 u8       cell_w  = 16
      0x09 u8       cell_h  = 16
      0x0A u8       baseline  (格内基线行号)
      0x0B u8       reserved
      0x0C u32      glyph_count      总字形数（= 索引条目数）
      0x10 u32      index_off        索引表文件内偏移
      0x14 u32      bitmap_off       位图区文件内偏移
      0x18 u32      bitmap_size      位图区字节数
      0x1C u32      fallback_index   缺字时用的字形下标
    Index  glyph_count * 8 B（按 codepoint 升序，可二分）
      0x00 u32  codepoint
      0x04 u8   advance
      0x05 u8   flags        bit0 = 全角/宽
      0x06 u16  reserved
    Bitmap 每字形 cell_w*cell_h*depth/8 字节，行主序
      depth=1：每行 2 字节，MSB 在左（v1 布局，**未变**，老字库照样能读）
      depth=8：每像素 1 字节 = 墨量覆盖率 0..255（255 = 全墨）

⚠️ v1 字库（version=1）**仍然可被程序读取**，本工具默认输出 v2/8bit。
留着的理由：判据 tools/check_font_aa.py 要拿它当"纯二值"的反例，
证明判据在坏情况下会红（见铁律 4：永远绿的守卫比没有更坏）。

用法
----
    python tools/mkfont.py                        # 默认 8bit 灰度，写 app/data/font.bin
    python tools/mkfont.py --depth 1              # 退回 1bpp 二值（反例/对照用）
    python tools/mkfont.py --profile min          # 只收 GB2312 一级字（体积最小）
    python tools/mkfont.py --size 13 --baseline 13
    python tools/mkfont.py --supersample 4 --cover 5   # 仅 depth=1 有效：出墨门限
    python tools/mkfont.py --no-preview

自检分两种口径：
* depth=8（默认）—— 报「墨量 / 孤立噪点 / 连通块 / **中间灰阶占比**」，
  其中中间灰阶占比是抗锯齿的**正向**信号：灰度字库必须有大量非 0 非 255 的像素，
  否则说明白做了一遍灰度却没真出抗锯齿。
* depth=1 —— 报「墨量 / 孤立噪点 / 连通块」，判据同旧版（笔画有没有被打断）。
"""

import argparse
import os
import struct
import sys

try:
    from PIL import Image, ImageDraw, ImageFont
except ImportError:  # pragma: no cover
    sys.exit("需要 Pillow：pip install pillow")

# --------------------------------------------------------------------------
# 常量
# --------------------------------------------------------------------------

MAGIC = b"AFNT"
VERSION = 2

CELL_W = 16
CELL_H = 16

#: 头像深 -> (flags 位深编码, flags 的 8bit 冗余位)
#:   1bpp : 编码 0，每字 2 字节/行 -> 每字 32 字节（与 v1 完全一致）
#:   4bpp : 编码 1，每字 8 字节/行 -> 每字 128 字节
#:   8bpp : 编码 2，每像素 1 字节   -> 每字 256 字节
DEPTHS = {
    1: (0, 0),
    4: (1, 0),
    8: (2, 1),
}

#: 默认 8bit 灰度。见文件头「位深：为什么默认 8」。
DEFAULT_DEPTH = 8

def cell_bytes(depth):
    """每字形位图字节数。**唯一来源**，别处不许自己算（铁律 6b：同源）。"""
    return CELL_W * CELL_H * depth // 8


#: 格内左边距。字体里极少数字形（Y、j 等）墨迹会超出笔位左侧 1px，
#: 留 1px 可避免左边被切。
DX = 1

#: 字号。保持 **13**：14 虽然更舒展，但配上 Medium 字重后墨量到 94+，
#: 闭合部件（日/目/口）内部空隙会被挤没，长时间阅读显闷。
#: 13 + Medium 是"饱满但不胖"的平衡点（见下）。
DEFAULT_SIZE = 13
DEFAULT_BASELINE = 13

#: 1bpp 阈值（**只作为超采样的"墨"门限**，不再是最终判定）。
#:
#: 128 而不是 96：**换了 Medium 字重之后可以拉高门限**。
#: Regular 的笔画偏细，门限一高（128）抗锯齿边缘就掉出阈值、笔画断裂；
#: Medium 笔画饱满一档，同样门限下边缘仍在阈值内，于是能既瘦又不断。
#: 实测（4x 超采样 / 覆盖>=5 / 字13，全库 6763 汉字）：
#:     Regular 13px gate 96 → 墨量 75.4 / 孤点 0.84 / 块 3.45(纯阈值) …
#:     Regular 13px gate 96 → 墨量 59.9 / 孤点 0.03 / 块 1.65（超采样）
#:     Regular 13px gate128 → 墨量 57.7 / 孤点 0.13 / 块 1.85  ← 断回来了
#:     Medium  13px gate 96 → 墨量 85.8 / 孤点 0.00 / 块 1.34
#:     Medium  13px gate128 → 墨量 82.5 / 孤点 0.01 / 块 1.42  ★ 采用
#:     Medium  13px gate160 → 墨量 79.4 / 孤点 0.04 / 块 1.56
#: 对比图见 tools/out/font_abc.png。
DEFAULT_THRESHOLD = 128

# --------------------------------------------------------------------------
# 超采样 + 面积覆盖（字形质量的正解）
# --------------------------------------------------------------------------
#
# 为什么不是「调阈值」：
#   硬阈值本质是"每个像素独立二值化"。但 13px 汉字在 16x16 格里，
#   撇/捺/细横的抗锯齿边缘只落在半灰（100~200）上，硬阈值一卡，
#   一条斜画中间就断成两截。这不是"缺笔画"，是**笔画被打断**。
#
#   量化信号是**连通块数**（一个汉字正常应是 1~2 块）：
#       阈值 128 → 每字平均 4.50 块、孤点 1.60   ← 断成好几截
#       阈值  96 → 每字平均 2.72 块、孤点 0.52   ← 仍在断
#       阈值  64 → 每字平均 1.72 块、孤点 0.03   ← 但墨量 55.6，开始变胖
#   单看墨量或单看阈值都解决不了，必须要"看面积"。
#
# 正解：先按 SUPERSAMPLE 倍率放大渲染，再让每个格子数它覆盖了多少个
# "够亮"的亚像素 —— 覆盖 >= COVER_NEED 才出墨。这样：
#   细笔画（覆盖 5~8/16）→ 保住了，不再断
#   抗锯齿的极浅边（覆盖 1~4/16）→ 仍然不出墨，不会变胖
#
#   4x 门限96 覆盖>=5/16 → 墨量 59.9  孤点 0.03  连通块 1.65  ★ 采用
#   4x 门限96 覆盖>=6/16 → 墨量 58.0  孤点 0.17  连通块 1.97  （又开始断）
#   4x 门限96 覆盖>=7/16 → 墨量 55.0  孤点 0.80  连通块 3.07  ✗
# 对比图见 tools/out/font_compare_new.png（旧/新并排放大）。
SUPERSAMPLE = 4
COVER_NEED = 5          # SUPERSAMPLE^2 = 16 格中至少这么多格算墨

#: 判定「全角」时，字宽达到字号这个比例就按全角算。
#: 用来兜住东亚宽度表没列、但中文字体里确实是全角的东西（①②■ 等）。
WIDE_RATIO = 0.95

#: 缺字用的「豆腐块」在索引里的哨兵码位。
TOFU_CP = 0x10FFFF
REPLACEMENT_CP = 0xFFFD

# 东亚宽度为 Wide/Fullwidth 的区段（简化表，够用）
WIDE_RANGES = (
    (0x1100, 0x115F), (0x2E80, 0x303E), (0x3041, 0x33FF),
    (0x3400, 0x4DBF), (0x4E00, 0x9FFF), (0xA000, 0xA4CF),
    (0xAC00, 0xD7A3), (0xF900, 0xFAFF), (0xFE10, 0xFE19),
    (0xFE30, 0xFE6F), (0xFF00, 0xFF60), (0xFFE0, 0xFFE6),
    (0x1F300, 0x1F64F), (0x20000, 0x2FFFD),
)

#: 默认附带的零散符号（GB2312 之外、但界面/对话里常出现的）
EXTRA_SYMBOLS = (
    "\u2013\u2014\u2018\u2019\u201c\u201d\u2022\u2026\u2039\u203a"
    "\u00b7\u00d7\u00f7\u00b1\u00b0\u00a7\u00a9\u00ae\u2122"
    "\u2190\u2191\u2192\u2193\u2194\u21d2\u21d0"
    "\u25a0\u25a1\u25b2\u25bc\u25b6\u25c0\u25cf\u25cb\u25c6\u25c7"
    "\u2605\u2606\u2600\u2601\u2602\u2614\u26a0\u2713\u2717\u2714\u2718"
    "\u2610\u2611\u2612\u2302\u2318\u21b5\u2423"
)

# 额外符号区段（--profile full 时全收）
EXTRA_RANGES = (
    (0x2200, 0x22FF),   # 数学运算符
    (0x2500, 0x257F),   # 制表符
    (0x2580, 0x259F),   # 方块元素
    (0x25A0, 0x25FF),   # 几何图形
    (0x2600, 0x26FF),   # 杂项符号
    (0x2700, 0x27BF),   # 装饰符号
    (0x3000, 0x303F),   # CJK 标点
    (0xFE30, 0xFE4F),   # CJK 兼容形式
    (0xFF00, 0xFFEF),   # 全角/半角形式
)


# --------------------------------------------------------------------------
# emoji 补字（豆包 / DeepSeek 这类服务商的回复里 emoji 很常见）
# --------------------------------------------------------------------------
#
# 为什么需要第二份字体：主字体 Noto Sans SC **一个 emoji 字形都没有**
# （实测 font.bin 7573 个字形里 U+1F300+ 与 U+2764 全缺），回复里一出现
# emoji 就整块渲染成豆腐块。补一份 Noto Emoji（OFL，与主字体许可兼容）
# 即可，仍走同一条 8bit 灰度管线 —— 3DS 上 emoji 是**单色线稿**，
# 这是 16x16 格 + 灰度字库的物理上限，不是 bug，也不假装是彩色。
#
# 收录策略：**精选而非全量**。全量 U+1F300..U+1FAFF ≈ 1400 字 × 256B
# ≈ 360KB，3dsx 白胖一圈且大半是冷门符号；精选 566 字 ≈ 145KB，
# 覆盖中文 AI 对话 95%+ 的出现频率。没收到的仍然显示豆腐块 ——
# 诚实降级，好过塞一个认不出的色块。
#
# ⚠️ 只收**单码位** emoji。ZWJ 序列（👨‍👩‍👧）、肤色（👍🏽）、键帽（1️⃣）
# 需要字形合成 / 分层，16x16 灰度字库做不到。做法是把连接符与修饰符
# 设成**零宽不可见**（见 ZERO_WIDTH_CPS），于是序列退化成
# "前一个基础 emoji 照常显示"——不出豆腐块，也不出赝品。
#
# ★ 这份区段表与 tools/make_emoji_font.py 的 EMOJI_RANGES **必须同源**：
#   子集字体按它收缩，mkfont 按它收录，两边不一致就会"子集里没有、
#   mkfont 想要"，被 getlength 过滤掉还不报错。铁律 6b：同源。
EMOJI_RANGES = (
    # --- 笑脸与情感：中文 AI 对话里出现频率最高的一整段 ---
    (0x1F600, 0x1F64F), (0x1F910, 0x1F92F), (0x1F970, 0x1F976), (0x1F97A, 0x1F97A),
    # --- 手势与身体 ---
    (0x1F440, 0x1F44F), (0x1F595, 0x1F596), (0x1F64C, 0x1F64F), (0x1F919, 0x1F91F),
    (0x1F926, 0x1F926), (0x1F937, 0x1F937), (0x1F4AA, 0x1F4AA), (0x261D, 0x261D),
    # --- 心形与情感符号 ---
    (0x2764, 0x2764), (0x1F493, 0x1F49F), (0x1F9E1, 0x1F9E1),
    # --- 对勾叉号问号等 ---
    (0x2705, 0x2705), (0x274C, 0x274E), (0x2753, 0x2755), (0x2757, 0x2757),
    (0x2795, 0x2797), (0x2714, 0x2716),
    # --- 扑克花色 ---
    (0x2660, 0x2666),
    # --- 闪光/星星 ---
    (0x2728, 0x2728), (0x2B50, 0x2B50), (0x1F31F, 0x1F31F),
    # --- 物品 ---
    (0x1F4A1, 0x1F4A1), (0x1F4A3, 0x1F4AB), (0x1F4AC, 0x1F4AD), (0x1F4AF, 0x1F4AF),
    (0x1F4B0, 0x1F4BE), (0x1F4BB, 0x1F4BB), (0x1F4C1, 0x1F4CE),
    (0x1F4D1, 0x1F4DA), (0x1F4DD, 0x1F4DD), (0x1F4E7, 0x1F4E7), (0x1F4F1, 0x1F4F2),
    (0x1F4F7, 0x1F4F7), (0x1F525, 0x1F525), (0x1F52E, 0x1F52E), (0x1F5FF, 0x1F5FF),
    (0x1F6D1, 0x1F6D1),
    # --- 娱乐 / 运动 / 时间 ---
    (0x1F3A5, 0x1F3A5), (0x1F3AE, 0x1F3AE), (0x1F3AF, 0x1F3AF), (0x1F3B0, 0x1F3B2),
    (0x1F3C6, 0x1F3C6), (0x1F947, 0x1F949), (0x1F94A, 0x1F94A),
    (0x231A, 0x231B),
    # --- 食物饮料 ---
    (0x1F32D, 0x1F344), (0x1F345, 0x1F353), (0x1F354, 0x1F37F), (0x1F942, 0x1F94F),
    (0x1F950, 0x1F96F),
    (0x2615, 0x2615),           # ☕ 热饮（预览样例行里露过脸的漏网之鱼）
    (0x1F9C0, 0x1F9C3),         # 🧀🧁🧂🧃 奶酪/纸杯蛋糕/盐/饮料盒
    # --- 衣物与杂物 ---
    (0x1F9E1, 0x1F9EA),         # 🧡🧢🧣🧤🧥🧦🧧
    (0x1F9ED, 0x1F9FF),         # 🧭🧮🧯🧰🧱🧲🧳🧴🧵🧶🧷🧸🧹🧺🧻🧼🧽🧾🧿
    (0x1FA70, 0x1FA7C), (0x1FA90, 0x1FA95),
    # --- 图表与钱 ---
    (0x1F4B0, 0x1F4BE),         # 💰💱💲💳💴💵💶💷💸💹💺💻💼💽💾
    (0x1F4C8, 0x1F4CB),         # 📈📉📊📋
    # --- 媒体控制与键盘 ---
    (0x23E9, 0x23FA),           # ⏩⏪⏫⏬⏭⏮⏯⏰⏱⏲⏳⏴⏵⏶⏷⏸⏹⏺
    (0x2328, 0x2328),           # ⌨
    (0x1F5A5, 0x1F5B2),         # 🖥🖨🖱🖲🖳
    # --- 乐器 ---
    (0x1F3B5, 0x1F3BA),         # 🎵🎶🎷🎸🎹🎺
    # --- 彩色圆点方块（状态标记常用）---
    (0x1F7E0, 0x1F7E9),         # 🟠🟡🟢🔵🟣🟤⚫⚪
    (0x1F7EA, 0x1F7EB),         # 🟥🟧🟨🟩🟦🟪🟫
    # --- 自然 / 天气 ---
    (0x1F30D, 0x1F30F), (0x1F308, 0x1F308), (0x1F31E, 0x1F320), (0x1F327, 0x1F32C),
    (0x1F330, 0x1F335), (0x1F337, 0x1F343), (0x1F33F, 0x1F341), (0x1F339, 0x1F33B),
    (0x1F333, 0x1F334), (0x2600, 0x2604), (0x26C4, 0x26C4),
    # --- 动物 ---
    (0x1F400, 0x1F43F), (0x1F980, 0x1F98F), (0x1F990, 0x1F9A9), (0x1F9AE, 0x1F9AE),
    # --- 交通 / 地点 ---
    (0x1F3E0, 0x1F3E2), (0x1F5FD, 0x1F5FD), (0x1F680, 0x1F68F), (0x1F695, 0x1F69C),
    (0x1F6B2, 0x1F6B2), (0x1F6F8, 0x1F6F8), (0x2708, 0x2708), (0x26BD, 0x26BE),
    (0x1F3C0, 0x1F3C0),
)

#: 零宽不可见码位 —— emoji 的连接 / 修饰成分。**不是 emoji 本体**，
#: 但缺了它们整串都会出豆腐块，所以必须收、且必须不占位：
#:   U+200D ZWJ           序列连接符（👨‍👩‍👧 家庭）
#:   U+FE0F VS16          变体选择符（❤️ → ❤ + 本符）
#:   U+1F3FB..U+1F3FFF    肤色修饰符（没有分层字形，与其出一个独立色块
#:                         冒充，不如让它隐形，前一个基础 emoji 照常显示）
#: ⚠️ C 端排版循环是**逐字符**推进的（text.c: text_width / first_line_bytes /
#:    text_draw 都是 i += n），advance=0 不会死循环；空位图 + alpha 全 0
#:    等于不画。已核对过，不是想当然。
ZERO_WIDTH_CPS = (0x200D, 0xFE0F) + tuple(range(0x1F3FB, 0x1F400))

#: emoji 源字体候选。**默认用仓库里的子集产物**（335KB，由
#: tools/make_emoji_font.py 从 NotoEmoji[wght].ttf 离线生成），
#: 这样 mkfont 只依赖 Pillow，不必拖 fontTools。
#: 找不到时**不失败**：打一声警告继续，字库就是没有 emoji 的旧行为
#: （构建不能因为一台机器上没这份可选字体而挂掉）。
EMOJI_FONT_CANDIDATES = (
    os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
                 "tools", "fonts", "NotoEmoji-subset.ttf"),
    r"C:\Windows\Fonts\NotoEmoji-Regular.ttf",
    "/usr/share/fonts/truetype/noto/NotoEmoji-Regular.ttf",
)


# --------------------------------------------------------------------------
# 字符集
# --------------------------------------------------------------------------

def gb2312_rows(rows):
    """解码 GB2312 指定区（0xA1..0xF7）下的全部字符，返回 (char, cp) 列表。"""
    out = []
    for b1 in rows:
        for b2 in range(0xA1, 0xFF):
            try:
                ch = bytes([b1, b2]).decode("gb2312")
            except UnicodeDecodeError:
                continue
            out.append(ch)
    return out


def build_charset(profile, font, emoji_font=None):
    """返回去重排序后的码位列表。

    emoji_font 不为 None 时，额外收录 EMOJI_RANGES 精选 + ZERO_WIDTH_CPS，
    这两类的**存在性判据用 emoji 字体**，不用主字体（主字体一个 emoji 都没有，
    拿它判等于全砍掉）。
    """
    cps = set()

    # 1) ASCII 可见区（含空格，空格会渲染成全空字形，用来推进笔位）
    cps.update(range(0x20, 0x7F))

    # 2) GB2312
    if profile == "min":
        rows = [0xA1, 0xA2, 0xA3, 0xA9] + list(range(0xB0, 0xD8))   # 符号 + 一级字
    elif profile == "std":
        rows = list(range(0xA1, 0xF8))                              # 符号 + 一二级字
    elif profile == "full":
        rows = list(range(0xA1, 0xF8))
    else:
        raise ValueError(profile)

    for ch in gb2312_rows(rows):
        cps.add(ord(ch))

    # 3) 零散符号
    cps.update(ord(c) for c in EXTRA_SYMBOLS)

    # 4) 大片符号区
    if profile == "full":
        for lo, hi in EXTRA_RANGES:
            cps.update(range(lo, hi + 1))

    # 5) emoji 精选 + 零宽符（判据走 emoji 字体）
    emoji_wanted = set()
    if emoji_font is not None:
        for lo, hi in EMOJI_RANGES:
            emoji_wanted.update(range(lo, hi + 1))
        emoji_wanted.update(ZERO_WIDTH_CPS)
        # ⚠️ 只建集合不并进 cps 是个静默坑：字符集不涨，emoji 一个都不收，
        # 而"字体缺字形被剔除 0"看着一切正常。并进来，让第 6 步统一判。
        cps.update(emoji_wanted)

    cps.add(REPLACEMENT_CP)

    # 6) 只保留字体真有的字形（advance > 0）
    #    emoji/零宽符走 emoji 字体判，其余走主字体 —— 两条路分开，
    #    否则"主字体没有"会把 emoji 全砍了，还不报错（静默劣化）。
    #    ⚠️ 零宽符**豁免 advance>0**：ZWJ/VS16 在字体度量里步进本来就是 0，
    #    按 >0 判会被当"缺字形"剔掉 —— 而它们恰恰是"❤️"不出豆腐块的关键。
    kept, dropped = [], []
    for cp in sorted(cps):
        if cp == 0x20:                       # 空格：字体里 advance 正常，保留
            kept.append(cp)
            continue
        if cp in ZERO_WIDTH_CPS:
            kept.append(cp)                  # 恒收，rasterize_all 强制零宽
            continue
        f = emoji_font if (emoji_font is not None and cp in emoji_wanted) else font
        try:
            if f.getlength(chr(cp)) > 0:
                kept.append(cp)
            else:
                dropped.append(cp)
        except Exception:
            dropped.append(cp)
    return kept, dropped


def is_wide(cp):
    for lo, hi in WIDE_RANGES:
        if lo <= cp <= hi:
            return True
    return False


# --------------------------------------------------------------------------
# 渲染
# --------------------------------------------------------------------------

PAD = 8   # 探测画布边距

#: 自动微调允许的最大位移（**目标像素**）。`g j y |` 这类带下伸部的字形
#: 在字号 14 下会超出格底约 1.2~2px，整体上移这么点完全看不出，
#: 比直接切掉一截好。实际搜索在超采样尺度上做（可精确到亚像素）。
#: 超过这个幅度就报错不再硬凑。
MAX_NUDGE = 2


def rasterize_all(font, size, baseline, cps, threshold, latin_tracking,
                  supersample=SUPERSAMPLE, cover_need=COVER_NEED, depth=DEFAULT_DEPTH,
                  emoji_font=None):
    """
    返回 (glyphs, report)

    glyphs: [(cp, advance, flags, 位图bytes), ...]
            位图长度 = cell_bytes(depth)，由 depth 决定
    report: dict，含越界/微调/空白字形统计

    emoji_font 不为 None 时，EMOJI_RANGES 精选 + ZERO_WIDTH_CPS 里的码位
    用 emoji 字体渲染（主字体没有这些字形，硬画只会得到空白/豆腐）。

    depth=1：按 supersample 倍率放大，再按"面积覆盖 >= cover_need"落到 1bpp。
             supersample=1 且 cover_need=1 时退化为纯阈值行为。
    depth=8：仍用同样的超采样画布，但**不做二值化** —— 每个目标像素取它覆盖的
             s*s 个亚像素的平均灰度，乘 255/255 直接当 alpha 写进位图。
             亚像素数 s*s 决定了灰阶精度（4x -> 17 级，够用；想更细可 --supersample 6）。
    """
    S = max(1, int(supersample))
    cb = cell_bytes(depth)
    pw = (CELL_W + PAD * 2 + 4) * S
    ph = (CELL_H + PAD * 2 + 4) * S

    emoji_set = set()
    if emoji_font is not None:
        for lo, hi in EMOJI_RANGES:
            emoji_set.update(range(lo, hi + 1))
        emoji_set.update(ZERO_WIDTH_CPS)
    zero_set = set(ZERO_WIDTH_CPS)

    canvas = Image.new("L", (pw, ph), 0)
    draw = ImageDraw.Draw(canvas)
    tx, ty = (PAD + 2) * S, (PAD + 2) * S
    target = (tx, ty, tx + CELL_W * S, ty + CELL_H * S)  # 目标格（超采样坐标）

    def render(ch, dx, dy, f=None):
        """dx/dy 单位是**超采样像素**（不是目标像素）。

        这样微调能精确到亚像素，超出 1.5 个目标像素的字形也能挪回来
        （g/j/y/| 这类带下伸部的字母在字号 14 下会超底边约 1.2~2px）。
        """
        draw.rectangle((0, 0, pw, ph), fill=0)
        draw.text((tx + DX * S + dx, ty + baseline * S + dy), ch,
                  font=f or font, fill=255, anchor="ls")
        return canvas.getbbox()

    glyphs = []
    clipped = []
    nudged = []
    empty = 0
    n_emoji = 0

    for cp in cps:
        ch = chr(cp)
        f = emoji_font if (emoji_font is not None and cp in emoji_set) else font
        if emoji_font is not None and cp in emoji_set:
            n_emoji += 1

        # -- 零宽不可见：不渲染、不出墨、不占位 -------------------------
        # 先拦在这里，省掉一次光栅化；位图全 0 的代价只是 cb 字节。
        if cp in zero_set:
            glyphs.append((cp, 0, 0, bytes(cb)))
            continue

        # -- 步进 ---------------------------------------------------------
        try:
            adv_f = f.getlength(ch)
        except Exception:
            adv_f = 0.0
        # getlength 用的是超采样后的字号，换算回目标尺度
        adv_f /= S
        if is_wide(cp) or adv_f >= size * WIDE_RATIO:
            advance = CELL_W                      # 全角：16x16 网格
            wide = True
        else:
            advance = max(1, int(round(adv_f)))
            wide = False
            if advance > CELL_W:
                advance = CELL_W
        if cp == 0x20:
            wide = False

        # -- 光栅化 -------------------------------------------------------
        bbox = render(ch, 0, 0, f)
        used = (0, 0)

        if bbox is not None and not fits(bbox, target):
            # 小越界 → 在**超采样尺度**上搜位移（优先向上：下伸部越界最常见）
            lim = MAX_NUDGE * S
            best = None
            # dx 候选必须覆盖 ±1.5 个**目标像素**（±6 个超采样像素）：
            # Noto Emoji 的字形按 em 方画满，13px 下墨迹右缘会探出格子约
            # 1~1.5 个目标像素（实测 U+1F404 🐄 等 13 个 emoji 探出 1.25px，
            # ±4 的超采样候选够不着）。±6 覆盖 1.5 目标像素，够了。
            for dy in range(0, -lim - 1, -1):
                for dx in (0, -1, 1, -2, 2, -3, 3, -4, 4, -5, 5, -6, 6):
                    if abs(dx) > lim:
                        continue
                    b2 = render(ch, dx, dy, f)
                    if b2 is not None and fits(b2, target):
                        best = (dx, dy)
                        break
                if best:
                    break
            if best is None:                    # 向上不行，再试向下/纯左右
                for dy in range(1, lim + 1):
                    for dx in (0, -1, 1, -2, 2):
                        if abs(dx) > lim:
                            continue
                        b2 = render(ch, dx, dy, f)
                        if b2 is not None and fits(b2, target):
                            best = (dx, dy)
                            break
                    if best:
                        break
            if best:
                used = best
            if used != (0, 0):
                # 报告里换算回目标像素，读起来更有意义
                nudged.append((cp, ch, (round(used[0] / S, 2), round(used[1] / S, 2))))
            render(ch, used[0], used[1], f)
            bbox = canvas.getbbox()

        if bbox is None:
            empty += 1
            bits = bytes(cb)
        else:
            if not fits(bbox, target):
                clipped.append((cp, ch, bbox))
            if depth == 1:
                bits = pack_coverage(canvas.crop(target), threshold, S, cover_need)
            else:
                bits = pack_gray(canvas.crop(target), depth)

        glyphs.append((cp, advance, 1 if wide else 0, bits))

        if (not wide) and latin_tracking and cp < 0x2000:
            glyphs[-1] = (cp, min(CELL_W, advance + latin_tracking), 0, bits)

    report = {
        "count": len(glyphs),
        "empty": empty,
        "clipped": clipped,
        "nudged": nudged,
        "emoji": n_emoji,
        "zero_width": len([1 for cp, adv, _f, _b in glyphs if adv == 0 and cp != 0x20]),
        "supersample": S,
        "cover_need": cover_need,
        "depth": depth,
    }
    return glyphs, report


def fits(bbox, target):
    return (bbox[0] >= target[0] and bbox[1] >= target[1]
            and bbox[2] <= target[2] and bbox[3] <= target[3])


# --------------------------------------------------------------------------
# 笔画完整性自检
#
# 越界检查只保证"没被切掉"，但**判不出笔画被二值化吃掉** ——
# 一条 1px 宽的横画灰度峰值 100，阈值 128 一压就整条消失，
# 而 bbox 依然在格内、报告一片绿。用户看到的却是"字缺笔画"。
#
# 判据用两个可量化的信号，都是越低越好：
#   ink   每字平均墨量 —— 骤降说明笔画被吃掉
#   iso   孤立墨点（四邻全空）—— 噪点，阈值定得不对时会飙升
# 两个一起看才能定阈值：单看 ink 会一路调低到笔画粘连。
# --------------------------------------------------------------------------

PROBE_CHARS = (
    "的一是不了人我在有他这为之大来以个中上们到说国和地也子时道出"
    "而要于就下得可你年生自会那后能对着事其里所去行过家十用发天如"
    "然作方成者多日都三小军二无同么经法当起与好看学进种将还分此心"
)


def glyph_alpha_at(bits, depth, x, y):
    """取一个字形在 (x,y) 的覆盖率 0..255。**位深无关的唯一读取入口**。

    depth=1 时返回 0 或 255（二值）；depth=4/8 返回真实灰阶。
    凡是"这个像素算不算有墨"的判据都走这里，别再各自写位寻址 ——
    那正是 sc_lab.c 字形巡检器出错的地方（硬编码 1bpp 的 py*2+(px>>3)）。
    """
    if not (0 <= x < CELL_W and 0 <= y < CELL_H):
        return 0
    if depth == 1:
        byte = bits[y * 2 + (0 if x < 8 else 1)]
        return 255 if (byte & (0x80 >> (x & 7))) else 0
    i = y * CELL_W + x
    if depth == 8:
        return bits[i]
    if depth == 4:
        b = bits[i // 2]
        nib = (b >> 4) if (i % 2 == 0) else (b & 0x0F)
        return nib * 17                     # 0..15 -> 0..255
    raise ValueError("未知位深 %d" % depth)


#: 判「有墨」的门槛。**刻意用 32 而不是 128**：
#:
#: 灰度字库里有一大片 1~127 的浅灰像素，它们是抗锯齿的"过渡层"。
#: 判连通性时若用高门槛（128/160），等于**只取笔画最黑的核心** ——
#: 而 16px 汉字的撇/捺核心本来就只有 1px 宽，切掉两侧灰边后
#: 一条斜画自然碎成好几段。实测（130 个常用汉字，平均连通块）：
#:     门槛  32 -> 1.20   门槛 128 -> 2.71   门槛 160 -> 4.78   门槛 200 -> 6.81
#: 门槛越高越碎，说明**它量的不是"断笔"而是"笔画有多黑"**。
#:
#: 「断笔」的正确语义是：*这条笔画是否连成一体*，那就必须把抗锯齿灰边
#: 算进笔画本体（低门槛）。1bpp 路径下只有 0/255，低门槛与高门槛等价，
#: 所以**同一个门槛对两条路都成立，指标可直接横向比较** ——
#: 这正是判据能区分好坏的前提（实测 1bpp=1.42 / 8bit@32=1.20，同量级）。
INK_LEVEL = 32

#: 噪点判定门槛（与 INK_LEVEL 分工，见 audit_strokes 里的注释）。
#: 96 = "肉眼明显看得见的浅灰"。低于它的碎点在 16px 下不可见，数它没意义。
NOISE_LEVEL = 96


def audit_strokes(glyphs, depth=DEFAULT_DEPTH):
    """对已烘焙的位图做笔画自检。位深无关（走 glyph_alpha_at）。

    返回 {'n','ink','iso','comp','gray_ratio','worst','broken'}：
      ink        每字平均墨量        —— 骤降说明笔画被吃掉
      iso        孤立墨点（四邻全空）—— 噪点，阈值定得不对时会飙升
      comp       4-连通块数           —— **断裂的判据**。一个汉字正常 1~2 块，
                  大于 3 说明笔画被打断成好几截（屏幕上就是"字缺一截"）。
      gray_ratio **中间灰阶占比**（INK_LEVEL <= v < 255 的像素 / v >= INK_LEVEL 的像素）
                  —— 抗锯齿的**正向**信号。depth=1 时恒为 0，
                  depth=8 时应显著 > 0。这是判据能区分两条路的**关键量**。
                  ★ 定义与 check_font_aa.py 的 B 判据同源，两边数字必须能对上。
    """
    total_ink = 0
    total_iso = 0
    total_comp = 0
    total_gray = 0
    total_solid = 0
    per = []

    for cp, _adv, _fl, bits in glyphs:
        if not (0x4E00 <= cp <= 0x9FFF):
            continue

        def on(x, y):
            return 1 if glyph_alpha_at(bits, depth, x, y) >= INK_LEVEL else 0

        # 噪点用**另一个门槛**：孤立的浅灰碎点（1~31）在屏幕上几乎看不见，
        # 拿 INK_LEVEL 去数会把它们全算成噪点，指标与缺陷脱钩。
        # 噪点的语义是"明显可见的孤立小块"，所以用 96（肉眼可辨的浅灰）。
        def loud(x, y):
            return 1 if glyph_alpha_at(bits, depth, x, y) >= NOISE_LEVEL else 0

        pts = [(x, y) for y in range(CELL_H) for x in range(CELL_W) if on(x, y)]
        ink = len(pts)
        iso = sum(1 for (x, y) in pts
                  if loud(x - 1, y) + loud(x + 1, y)
                  + loud(x, y - 1) + loud(x, y + 1) == 0)

        # 灰阶统计：**只在该字确实有墨时统计**，否则空格会把比例稀释。
        #
        # ★ 定义必须与 tools/check_font_aa.py 的 B 判据**完全一致**（铁律 6b）：
        #     分子 = INK_LEVEL <= v < 255 的像素
        #     分母 = v >= INK_LEVEL 的像素（= "笔画本体"，含全墨芯与过渡带）
        #   ⚠️ 这里原先是 `if 0 < v < 255` 数分子、`total_solid += g + ink` 数分母，
        #      而 ink 是 v>=INK_LEVEL 的计数 —— 32..254 这一段被**算了两遍**，
        #      分母虚高 → 同一份字库这里报 0.530、check_font_aa 报 0.943，
        #      **同一个指标给出两个数**。两个数都对不上，就没法互相印证了。
        if ink:
            g = 0
            for y in range(CELL_H):
                for x in range(CELL_W):
                    v = glyph_alpha_at(bits, depth, x, y)
                    if INK_LEVEL <= v < 255:
                        g += 1
            total_gray += g
            total_solid += ink

        # 4-连通块计数（洪泛）
        pt = set(pts)
        seen = set()
        comp = 0
        for p in pt:
            if p in seen:
                continue
            comp += 1
            stack = [p]
            seen.add(p)
            while stack:
                cx, cy = stack.pop()
                for dx, dy in ((1, 0), (-1, 0), (0, 1), (0, -1)):
                    q = (cx + dx, cy + dy)
                    if q in pt and q not in seen:
                        seen.add(q)
                        stack.append(q)

        total_ink += ink
        total_iso += iso
        total_comp += comp
        per.append((chr(cp), ink, comp))

    n = max(1, len(per))
    # 点画笔画的字天然墨量极低，剔除后再算均值，免得把判据拉偏
    solid = [t for t in per if t[0] not in INK_EXEMPT]
    per_sorted = sorted(per, key=lambda t: t[1])
    worst = [(c, k) for c, k, _ in per_sorted if c not in INK_EXEMPT][:10]
    # 断裂最严重的字（块数最多）
    most_broken = sorted(per, key=lambda t: -t[2])[:8]
    return {
        "n": len(per),
        "ink": sum(k for _c, k, _ in solid) / max(1, len(solid)),
        "iso": total_iso / n,
        "comp": total_comp / n,
        "gray_ratio": total_gray / max(1, total_solid),
        "worst": worst,
        "broken": [(c, k) for c, _i, k in most_broken],
    }


#: 自检下限/上限。
#: 墨量下限 48：低于此基本可判定笔画被阈值吃掉
#:   （13px + 阈值 128 纯硬阈值时是 43.0，压在下限之下）。
#: 噪点上限 1.0：实测 96 是 0.52；阈值 128 时 1.60，明显超标。
#: 连通块上限 2.5：一个汉字正常 1~2 块。纯硬阈值阈值 128 时是 4.50
#:   （断成好几截），96 时 2.72（仍在断）。超采样后应降到 1.7 附近。
#:   这三个一起看才能定方案：单看墨量会一路调低到笔画粘连，
#:   单看噪点会漏掉"笔画断成两截"，必须看连通块。
INK_MIN = 48.0
ISO_MAX = 1.00
COMP_MAX = 2.50

#: 这些字本身就是点画/单笔，墨量天然极低，不参与墨量下限判定。
INK_EXEMPT = set("丶冫氵丨一亠冖宀丫灬丿乙乚亅匚刂彳彡巛")

#: **灰阶占比下限**：有墨像素里，中间灰（0<v<255）至少要占这么多。
#:
#: 这是抗锯齿判据的**核心**。为什么不能只靠"看预览图"：
#: 人眼看 16px 汉字的边缘有没有灰阶很不可靠，而这个量是可量化的 ——
#: depth=1 恒为 0.000，depth=8 实测在 0.30~0.45。设 0.12 留足余量：
#: 低于它说明灰度位图里其实没有过渡像素，也就是**白做了抗锯齿**。
#:
#: ⚠️ 只对 depth>=4 生效。depth=1 时它是 0，那是**预期值不是缺陷**，
#: 所以 main() 里按位深分别判定，不拿这条去卡 1bpp 路径。
GRAY_RATIO_MIN = 0.12


def pack_1bpp(img, threshold, depth=1):
    """16x16 -> 1bpp 位图，行主序，每行 2 字节，MSB 在左。（纯阈值，仅对比用）"""
    px = img.load()
    out = bytearray(cell_bytes(depth))
    for y in range(CELL_H):
        hi = 0
        lo = 0
        for x in range(CELL_W):
            if px[x, y] >= threshold:
                if x < 8:
                    hi |= 0x80 >> x
                else:
                    lo |= 0x80 >> (x - 8)
        out[y * 2] = hi
        out[y * 2 + 1] = lo
    return bytes(out)


def pack_gray(img, depth):
    """超采样画布 -> 每像素 depth 位的覆盖率位图。**抗锯齿的正路。**

    img 是 CELL_W*s x CELL_H*s 的灰度图。用 Pillow 的 BOX 重采样
    （面积平均 = 每个目标像素被覆盖的比例）降到 CELL_W x CELL_H，
    于是每个目标像素得到 0..255 的灰度 —— 这就是 alpha。

    为什么用 BOX 而不是自己数"够亮的亚像素个数"：
      计数法（pack_coverage）要一个 threshold + need，把连续灰阶又量化成 0/1，
      抗锯齿信息就是在那里丢的；BOX 直接对连续灰度求平均，**不设阈值**，
      浅到 1/17 的边缘也会变成"很淡的一层"而不是"没有"。
    """
    target = img.resize((CELL_W, CELL_H), Image.BOX)
    px = target.load()
    out = bytearray(cell_bytes(depth))
    i = 0
    for y in range(CELL_H):
        for x in range(CELL_W):
            v = px[x, y]
            if depth == 8:
                out[i] = v
            elif depth == 4:
                if x % 2 == 0:
                    out[i] = (v >> 4) & 0x0F          # 高 4 位在左
                else:
                    out[i] |= v & 0x0F
            else:
                raise ValueError("pack_gray 不支持 depth=%d" % depth)
            i += 1
    return bytes(out)


def pack_coverage(img, threshold, s, need):
    """超采样画布 -> 16x16 的 1bpp 位图，按"面积覆盖"判定。

    img 是 CELL_W*s x CELL_H*s 的灰度图。每个目标像素统计它覆盖的
    s*s 个亚像素里有多少 >= threshold，达到 need 才出墨。
    这样细笔画的浅边缘能保住（不断线），纯抗锯齿边角却仍被丢掉（不增肥）。

    ⚠️ 这条路径**会把灰阶压回 0/1**，所以 depth=1 时字必然有锯齿。
    保留它只为对照与回归，别拿它当"抗锯齿版"。
    """
    px = img.load()
    out = bytearray(cell_bytes(1))
    total = s * s
    need = max(1, min(need, total))
    for y in range(CELL_H):
        hi = 0
        lo = 0
        for x in range(CELL_W):
            n = 0
            for yy in range(s):
                sy = y * s + yy
                for xx in range(s):
                    if px[x * s + xx, sy] >= threshold:
                        n += 1
            if n >= need:
                if x < 8:
                    hi |= 0x80 >> x
                else:
                    lo |= 0x80 >> (x - 8)
        out[y * 2] = hi
        out[y * 2 + 1] = lo
    return bytes(out)


def make_tofu(depth=DEFAULT_DEPTH):
    """缺字豆腐块：12x12 空心方框，居中。位深无关。"""
    img = Image.new("L", (CELL_W, CELL_H), 0)
    d = ImageDraw.Draw(img)
    d.rectangle((2, 3, 13, 14), outline=255)
    if depth == 1:
        return pack_1bpp(img, 128, 1)
    return pack_gray(img, depth)


# --------------------------------------------------------------------------
# 写文件
# --------------------------------------------------------------------------

HEADER_FMT = "<4sHHBBBBIIIII"
HEADER_SIZE = struct.calcsize(HEADER_FMT)          # 32
assert HEADER_SIZE == 32, HEADER_SIZE


def write_font(path, glyphs, baseline, depth=DEFAULT_DEPTH):
    cb = cell_bytes(depth)
    dcode, dflag = DEPTHS[depth]
    # 豆腐块追加到末尾（哨兵码位保证索引仍然升序）
    tofu_bits = make_tofu(depth)
    glyphs = glyphs + [(TOFU_CP, CELL_W, 1, tofu_bits)]
    glyphs.sort(key=lambda g: g[0])

    # 排序后重新定位 fallback
    fallback_index = next(i for i, g in enumerate(glyphs) if g[0] == TOFU_CP)

    index_off = HEADER_SIZE
    bitmap_off = index_off + len(glyphs) * 8
    bitmap_size = len(glyphs) * cb

    buf = bytearray()
    buf += struct.pack(
        HEADER_FMT,
        MAGIC, VERSION, (dcode & 3) | (dflag << 2),
        CELL_W, CELL_H, baseline, 0,
        len(glyphs), index_off, bitmap_off, bitmap_size, fallback_index,
    )
    for cp, advance, flags, _ in glyphs:
        buf += struct.pack("<IBBH", cp, advance, flags, 0)
    for _, _, _, bits in glyphs:
        if len(bits) != cb:
            # 自己就能抓住的错：位图长度与头里声明的 cell/位深不符
            raise AssertionError("字形 U+%04X 位图 %d 字节，应为 %d"
                                 % (_, len(bits), cb))
        buf += bits

    assert len(buf) == bitmap_off + bitmap_size, (len(buf), bitmap_off, bitmap_size)

    os.makedirs(os.path.dirname(os.path.abspath(path)), exist_ok=True)
    with open(path, "wb") as f:
        f.write(buf)
    return len(glyphs), len(buf)


# --------------------------------------------------------------------------
# 预览图（用**已烘焙的位图**画，所见即设备所见）
# --------------------------------------------------------------------------

SAMPLE_CN = (
    "3DS AI 助手",
    "你好，我是运行在 3DS 上的 AI 助手。",
    "我可以帮你查攻略、调亮度、管理 SD 卡文件。",
    "所有危险操作都会先弹出确认框。",
    "Current level: L0 (对话)   25 chars/line",
    "数字 0123456789  符号 ，。、；：？！（）《》「」…—·",
    "英文 The quick brown fox jumps over the lazy dog.",
    "混排 Mix 中文 + English + 123 + ，。 end.",
)

GRID_CHARS = (
    "的一是不了人我在有他这为之大来以个中上们到说国和地也子时道出而要于就下得可你年生自"
    "会那后能对着事其里所去行过家十用发天如然作方成者多日都三小军二无同么经法当起与好看"
    "学进种将还分此心前面又定见只主没公从"
)

#: emoji 样例行。挑高频的，一次验证三件事：
#:   ① emoji 真有字形（不是豆腐块） ② 步进是 16（全宽，不挤在一起）
#:   ③ 和中英文字混排不错位（共用同一条基线）
EMOJI_SAMPLE = (
    "今天天气不错 ☀ 出门逛逛 🌳 顺便吃点 🍔 喝杯 ☕",
    "这个想法很棒 💡 我完全同意 👍 ✅ 给你点个赞 💯",
    "❤ 爱心 ✨ 闪光 ⭐ 星星 🔥 火焰 🎉 庆祝 🚀 出发",
)

#: emoji 字表网格行（高频 25 个，叠格线看 16x16 里的留白与基线）
EMOJI_GRID = (
    0x1F60A, 0x1F602, 0x1F923, 0x1F970, 0x1F60D, 0x1F914, 0x1F644, 0x1F62D,
    0x1F622, 0x1F621, 0x1F631, 0x1F60E, 0x1F609, 0x1F60F, 0x1F611, 0x1F610,
    0x1F634, 0x1F971, 0x1F972, 0x1F973, 0x1F92F, 0x1F975, 0x1F976, 0x1F97A,
    0x2764,
)


MARGIN = 8
LINE_H = 18
GRID_COLS = 25          # 25 * 16 = 400 = 顶屏一行能放的字数


def render_preview(glyphs, path, zoom=3, src_name="", depth=DEFAULT_DEPTH,
                   size=None, baseline=None):
    """glyphs: [(cp, advance, flags, bits)] 已排序，含豆腐块。

    用**已烘焙的位图**逐位重画，所以预览图和真机显示逐像素一致。
    ⚠️ depth=8 时按 alpha 与底色混合，而不是直接画黑 —— 否则预览图
    会比真机"黑得多"，看着比实际更糙，误判成灰度没生效。
    """
    idx = {cp: (adv, bits) for cp, adv, fl, bits in glyphs}
    tofu = idx.get(TOFU_CP, (CELL_W, bytes(cell_bytes(depth))))

    def glyph_of(cp):
        return idx.get(cp, tofu)

    lines = list(SAMPLE_CN) + list(EMOJI_SAMPLE)
    grid_rows = (len(GRID_CHARS) + GRID_COLS - 1) // GRID_COLS
    emoji_rows = (len(EMOJI_GRID) + GRID_COLS - 1) // GRID_COLS

    w = MARGIN * 2 + GRID_COLS * CELL_W
    h = (MARGIN
         + len(lines) * LINE_H
         + 6
         + 1                                    # 分隔线
         + 6
         + grid_rows * LINE_H
         + 6                                    # 字表与 emoji 字表之间
         + emoji_rows * LINE_H
         + MARGIN + 20)                         # 底部图例

    canvas = Image.new("L", (w, h), 245)
    d = ImageDraw.Draw(canvas)
    d.rectangle((0, 0, w - 1, h - 1), outline=120)

    # ---- 文本样例（按真实步进排版）----
    yy = MARGIN
    for line in lines:
        pen = MARGIN
        for ch in line:
            adv, bits = glyph_of(ord(ch))
            if pen + CELL_W > w - MARGIN:
                break                      # 样例行也可能超宽，同样按步进裁
            draw_cell(canvas, pen, yy, bits, depth)
            pen += adv
        yy += LINE_H
    yy += 6
    d.line((MARGIN, yy, w - MARGIN, yy), fill=170)
    yy += 7

    # ---- 字表网格（叠加 16x16 格线，便于看基线与留白）----
    n = 0
    for _ in range(grid_rows):
        for c in range(GRID_COLS):
            if n >= len(GRID_CHARS):
                break
            ch = GRID_CHARS[n]
            n += 1
            pen = MARGIN + c * CELL_W
            d.rectangle((pen, yy, pen + CELL_W - 1, yy + CELL_H - 1), outline=215)
            _, bits = glyph_of(ord(ch))
            draw_cell(canvas, pen, yy, bits, depth)
        yy += LINE_H
    yy += 6

    # ---- emoji 字表网格（同样叠格线）----
    for r in range(emoji_rows):
        for c in range(GRID_COLS):
            i = r * GRID_COLS + c
            if i >= len(EMOJI_GRID):
                break
            pen = MARGIN + c * CELL_W
            d.rectangle((pen, yy, pen + CELL_W - 1, yy + CELL_H - 1), outline=215)
            _, bits = glyph_of(EMOJI_GRID[i])
            draw_cell(canvas, pen, yy, bits, depth)
        yy += LINE_H

    # ---- 图例（这一行本身就用点阵画，验证混合排版）----
    # 字体名从实际用的源字体取，免得换了源字体图例还写旧名字；
    # 字号/基线/位深也从**实际参数**取，不写死 —— 写死就会出现
    # "图例说字号13、实际生成的是别的"这种永远骗人的标签。
    legend = "字号%s 基线%s 步进%d %dbit | %s" % (
        size if size is not None else "?",
        baseline if baseline is not None else "?",
        CELL_W, depth, os.path.basename(src_name) or "?")
    # 按**实际步进**裁到画布内。写死短串是靠不住的：图例一变长
    # （加了位深、换了更长的字体名）就会越界画到画布外，
    # 报出来的却是 IndexError，跟真正的原因隔着十万八千里。
    pen = MARGIN
    for ch in legend:
        adv, bits = glyph_of(ord(ch))
        if pen + adv > w - MARGIN:
            break
        draw_cell(canvas, pen, yy + 2, bits, depth)
        pen += adv

    if zoom != 1:
        canvas = canvas.resize((canvas.width * zoom, canvas.height * zoom),
                               Image.NEAREST)
    os.makedirs(os.path.dirname(os.path.abspath(path)), exist_ok=True)
    canvas.convert("RGB").save(path)
    return path


def draw_cell(canvas, ox, oy, bits, depth=DEFAULT_DEPTH):
    """把一个字形混合到底图上。

    ★ 混合公式与 render_2d.c 的 canvas_blend() **逐位对应**
      （铁律 6b：复刻脚本的公式必须与被测代码同源，改一处必须改两处）。
      C 端： dst = (dst*(255-a) + fg*a + 127) / 255
      这里： fg = 0（墨色黑），底色 = 画布当前值
    ⚠️ depth=1 时 a 只能是 0 或 255，公式退化成覆盖写 —— 与旧行为一致。
    """
    px = canvas.load()
    for y in range(CELL_H):
        for x in range(CELL_W):
            a = glyph_alpha_at(bits, depth, x, y)
            if a == 0:
                continue
            px[ox + x, oy + y] = (px[ox + x, oy + y] * (255 - a) + 127) // 255


# --------------------------------------------------------------------------
# main
# --------------------------------------------------------------------------

#: 源字体候选。**优先 Medium 字重**：
#: 小字号点阵化时，Regular 的笔画偏细，抗锯齿边缘覆盖度刚好卡在阈值附近，
#: 容易断线；Medium 笔画饱满一档，同样的阈值下断裂更少。
#: 实测（4x 超采样 / 覆盖>=5 / 字号14）：
#:     Noto Sans SC Regular 13px → 墨量 59.9 / 孤点 0.03 / 块 1.65
#:     Noto Sans SC Medium  14px → 墨量 79.4 / 孤点 0.00 / 块 1.35  ★ 采用
#: 对比图见 tools/out/font_upgrade_cmp.png。
DEFAULT_FONT_CANDIDATES = (
    r"C:\Windows\Fonts\Noto Sans SC Medium (TrueType).otf",
    r"C:\Windows\Fonts\NotoSansSC-Medium.otf",
    r"C:\Windows\Fonts\Noto Sans SC (TrueType).otf",
    r"C:\Windows\Fonts\NotoSansSC-Regular.otf",
    "/usr/share/fonts/opentype/noto/NotoSansCJK-Medium.ttc",
    "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc",
)


def find_font(explicit):
    if explicit:
        if not os.path.isfile(explicit):
            sys.exit("找不到字体文件: %s" % explicit)
        return explicit
    for c in DEFAULT_FONT_CANDIDATES:
        if os.path.isfile(c):
            return c
    sys.exit("没找到可用字体，请用 --font 指定。")


def find_emoji_font(explicit):
    """返回 emoji 字体路径或 None。**None 不是错误** —— 只是没有 emoji 补字。

    找不到时打一声警告：构建继续，字库退回"无 emoji"的旧行为。
    静默跳过才是坑（下次有人看到豆腐块，会以为渲染管线坏了）。
    """
    if explicit:
        if not os.path.isfile(explicit):
            sys.exit("找不到 emoji 字体: %s" % explicit)
        return explicit
    for c in EMOJI_FONT_CANDIDATES:
        if os.path.isfile(c):
            return c
    print("警告: 没找到 emoji 字体（%s），本次字库**不含 emoji**，"
          "服务商回复里的 emoji 会显示为豆腐块。"
          % EMOJI_FONT_CANDIDATES[0], file=sys.stderr)
    return None


def main(argv=None):
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    ap = argparse.ArgumentParser(description="生成 3DS 点阵字库 font.bin")
    ap.add_argument("--font", default=None, help="源字体路径")
    ap.add_argument("--emoji-font", default=None,
                    help="emoji 补字字体路径（默认 tools/fonts/NotoEmoji-subset.ttf）")
    ap.add_argument("--no-emoji", action="store_true",
                    help="不收 emoji（退回无 emoji 字库，仅对照用）")
    ap.add_argument("--out", default=os.path.join(root, "app", "data", "font.bin"))
    ap.add_argument("--preview", default=os.path.join(root, "tools", "out", "font_preview.png"))
    ap.add_argument("--no-preview", action="store_true")
    ap.add_argument("--preview-zoom", type=int, default=3)
    ap.add_argument("--size", type=int, default=DEFAULT_SIZE)
    ap.add_argument("--baseline", type=int, default=DEFAULT_BASELINE)
    ap.add_argument("--threshold", type=int, default=DEFAULT_THRESHOLD,
                    help="亚像素「算墨」的灰度门限，越大笔画越细")
    ap.add_argument("--depth", type=int, choices=(1, 4, 8), default=DEFAULT_DEPTH,
                    help="位深。8=灰度抗锯齿（默认）；1=纯二值（反例/对照）；4=折中")
    ap.add_argument("--supersample", type=int, default=SUPERSAMPLE,
                    help="渲染倍率（4=推荐；1 则退化为纯阈值）")
    ap.add_argument("--cover", type=int, default=COVER_NEED,
                    help="出墨所需覆盖的亚像素数（**仅 depth=1 有效**）")
    ap.add_argument("--latin-tracking", type=int, default=0,
                    help="半角字符额外加宽的像素数（0 = 用字体原步进）")
    ap.add_argument("--profile", choices=("min", "std", "full"), default="std")
    ap.add_argument("--quiet", action="store_true")
    args = ap.parse_args(argv)

    font_path = find_font(args.font)
    S = max(1, int(args.supersample))
    font = ImageFont.truetype(font_path, args.size * S)

    emoji_font = None
    emoji_font_path = None
    if not args.no_emoji:
        emoji_font_path = find_emoji_font(args.emoji_font)
        if emoji_font_path:
            emoji_font = ImageFont.truetype(emoji_font_path, args.size * S)

    def say(*a):
        if not args.quiet:
            print(*a)

    say("源字体 : %s" % font_path)
    if emoji_font_path:
        say("emoji  : %s（补 %d 字 + %d 零宽符）"
            % (emoji_font_path,
               sum(hi - lo + 1 for lo, hi in EMOJI_RANGES), len(ZERO_WIDTH_CPS)))
    say("字号   : %d   基线行: %d   阈值: %d   超采样: %dx%s"
        % (args.size, args.baseline, args.threshold, S,
           ("（覆盖 >=%d/%d）" % (args.cover, S * S)) if args.depth == 1
           else "（灰度，不设阈值）"))
    say("位深   : %dbit%s   每字 %d 字节"
        % (args.depth,
           "（8bit 抗锯齿，渲染端按 alpha 混合）" if args.depth == 8
           else "（4bit 灰度）" if args.depth == 4
           else "（1bpp 二值，**无抗锯齿**，仅对照用）",
           cell_bytes(args.depth)))

    cps, dropped = build_charset(args.profile, font, emoji_font)
    say("字符集 : profile=%s  入选 %d 字  （字体缺字形被剔除 %d）"
        % (args.profile, len(cps), len(dropped)))

    glyphs, rep = rasterize_all(
        font, args.size, args.baseline, cps, args.threshold, args.latin_tracking,
        supersample=S, cover_need=args.cover, depth=args.depth,
        emoji_font=emoji_font)

    n_wide = sum(1 for g in glyphs if g[2] & 1)
    say("光栅化 : %d 字形（全角 %d / 半角 %d），空白 %d，emoji %d，零宽 %d"
        % (rep["count"], n_wide, rep["count"] - n_wide, rep["empty"],
           rep["emoji"], rep["zero_width"]))

    if rep["nudged"]:
        say("微调   : %d 个字形整体挪了几像素以避开边缘" % len(rep["nudged"]))
        for cp, ch, off in rep["nudged"][:20]:
            say("     U+%04X %r  d=(%+d,%+d)" % (cp, ch, off[0], off[1]))

    if rep["clipped"]:
        say("")
        say("!! 有 %d 个字形墨迹超出 16x16：" % len(rep["clipped"]))
        for cp, ch, bb in rep["clipped"][:40]:
            say("     U+%04X %r  bbox=%s" % (cp, ch, bb))
        if len(rep["clipped"]) > 40:
            say("     ... 另有 %d 个" % (len(rep["clipped"]) - 40))
        say("   → 调小 --size 或调整 --baseline 重试")
    else:
        say("越界   : 无（全部字形都在 16x16 格内）")

    audit = audit_strokes(glyphs, args.depth)
    say("笔画   : 汉字 %d 字，平均墨量 %.1f，孤立噪点 %.2f，连通块 %.2f，灰阶占比 %.3f"
        % (audit["n"], audit["ink"], audit["iso"], audit["comp"],
           audit["gray_ratio"]))

    # 判据分两组：**笔画完整性**（两条路都查，口径一致所以可横向比较）
    # 与 **抗锯齿有效性**（只对 depth>=4 查，depth=1 恒 0 是预期不是缺陷）。
    # ⚠️ 别把灰阶门槛套到 depth=1 上，那会让"故意生成反例"永远红。
    bad_stroke = (audit["ink"] < INK_MIN or audit["iso"] > ISO_MAX
                  or audit["comp"] > COMP_MAX)
    bad_aa = args.depth >= 4 and audit["gray_ratio"] < GRAY_RATIO_MIN

    if bad_stroke or bad_aa:
        if bad_stroke:
            print("   !! 笔画自检不过（下限 墨量>=%.0f 噪点<=%.2f 连通块<=%.2f）——"
                  % (INK_MIN, ISO_MAX, COMP_MAX))
            print("      墨量偏低 = 笔画被吃掉；连通块偏高 = 笔画被打断。")
            if args.depth == 1:
                print("      调低 --cover（当前 %d/%d），或调低 --threshold。"
                      % (args.cover, S * S))
            print("      墨量最少的十个字：%s"
                  % " ".join("%s(%d)" % (c, k) for c, k in audit["worst"]))
            print("      最断的八个字：%s"
                  % " ".join("%s(%d块)" % (c, k) for c, k in audit["broken"]))
        if bad_aa:
            print("   !! 抗锯齿自检不过（有墨像素里中间灰只占 %.3f < %.2f）——"
                  % (audit["gray_ratio"], GRAY_RATIO_MIN))
            print("      说明位图里几乎没有过渡像素：抗锯齿等于没做，")
            print("      渲染端再怎么做 alpha 混合也只是把纯黑白硬边稍微柔化。")
            print("      查 pack_gray() 是不是还在走 pack_coverage 的二值路径。")

    total, size_bytes = write_font(args.out, glyphs, args.baseline, args.depth)
    say("")
    say("写出   : %s" % args.out)
    say("         字形 %d  索引 %d B  位图 %d B  合计 %d B (%.1f KB)"
        % (total, total * 8, total * cell_bytes(args.depth), size_bytes,
           size_bytes / 1024.0))

    if not args.no_preview:
        p = render_preview(
            sorted(glyphs + [(TOFU_CP, CELL_W, 1, make_tofu(args.depth))],
                   key=lambda g: g[0]),
            args.preview, zoom=args.preview_zoom, src_name=font_path,
            depth=args.depth, size=args.size, baseline=args.baseline)
        say("预览   : %s  (%dx 放大)" % (p, args.preview_zoom))

    # 退出码：有硬伤（越界 / 笔画断 / 无抗锯齿）一律非 0
    return 0 if not (rep["clipped"] or bad_stroke or bad_aa) else 2


if __name__ == "__main__":
    sys.exit(main())
