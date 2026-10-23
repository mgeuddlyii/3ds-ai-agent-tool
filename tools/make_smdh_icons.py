#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""
make_smdh_icons.py — 把 app/icon.png（256x256 设计稿）降采样成
SMDH 需要的精确尺寸：large 48x48 + small 24x24。

═══ 为什么必须有这个 ═══════════════════════════════════════════════════
3dbrew 的 SMDH 图标区是**定长**的：small 24x24 @0x2040，large 48x48 @0x24C0，
都只有 RGB565，没有 alpha。devkitPro 的 smdhtool --create **不缩放**图标——
给它 256x256 时它直接报错 "Icon size is incorrect (expected 48x48)" 并
**跳过图标写入**，产出一个 8256 字节（截断到 0x2040）的残废 SMDH，
home-menu 上图标会变成黑块/空块。

所以构建链路必须是：256 设计稿 -> 这里降采样到 48 / 24 -> 交给 smdhtool。
LANCZOS 降采样比让 smdhtool 自己处理更可控，也能保证 24x24 小图标
（"AI" 字 + 耳朵剪影）不至于糊成一团。

═══ 用法 ═══════════════════════════════════════════════════════════════
    python tools/make_smdh_icons.py            # 写 app/build/icon_48.png + icon_24.png
    python tools/make_smdh_icons.py --out48 X --out24 Y
build.py 在调 smdhtool 之前会调用本脚本，把 48 当主图标、24 当小图标传入。
"""
from __future__ import print_function

import argparse
import os
import sys

try:
    from PIL import Image
except ImportError:
    # 本机没装 Pillow。不在这里报错，下面 make() 会走 pnglite 的 box 降采样 ——
    # 因为**退回去用 256 原图的后果正是桌面图标变空块**（smdhtool 会跳过写入）。
    Image = None

HERE = os.path.dirname(os.path.abspath(__file__))
PROJ = os.path.abspath(os.path.join(HERE, ".."))
ICON_SRC = os.path.join(PROJ, "app", "icon.png")
BUILD = os.path.join(PROJ, "app", "build")
OUT48 = os.path.join(BUILD, "icon_48.png")
OUT24 = os.path.join(BUILD, "icon_24.png")


def make(src, out48, out24):
    if not os.path.exists(src):
        raise RuntimeError("找不到设计稿 %s（先跑 tools/make_icon.py）" % src)
    os.makedirs(os.path.dirname(out48), exist_ok=True)

    if Image is not None:
        im = Image.open(src).convert("RGB")
        # LANCZOS：缩小保细节，避免 24x24 的 AI 字糊掉
        im.resize((48, 48), Image.LANCZOS).save(out48)
        im.resize((24, 24), Image.LANCZOS).save(out24)
        how = "PIL/LANCZOS"
    else:
        # ★ 本机没装 Pillow。**绝不能就此退回去用 256 原图** ——
        #   smdhtool --create 拿到非 48x48 会**跳过图标写入**，
        #   产出一个截断的 SMDH，桌面上图标变成空块（这坑踩过）。
        #   所以就地用 pnglite 的 box 降采样顶上。
        import pnglite as P
        w, h, rows = P.read_png(src)
        for sz, dst in ((48, out48), (24, out24)):
            small = P.resize_box(rows, w, h, sz, sz)
            # SMDH 图标区没有 alpha，RGB565；写成 RGB PNG 即可
            P.write_png(dst, sz, sz,
                        [[(r, g, b, 255) for (r, g, b, _a) in row] for row in small])
        how = "pnglite/box（Pillow 未安装）"

    print("smdh-icons: %s (48x48) + %s (24x24)  [%s]"
          % (out48, out24, how))
    return out48, out24


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--src", default=ICON_SRC)
    ap.add_argument("--out48", default=OUT48)
    ap.add_argument("--out24", default=OUT24)
    args = ap.parse_args()
    try:
        make(args.src, args.out48, args.out24)
    except Exception as e:
        print("FAILED: %s" % e)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
