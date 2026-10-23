#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""二维码 PNG -> 模块矩阵。给中转用，也用来自检。

为什么不用"按比例采样"那种懒办法
--------------------------------
二维码在 PNG 里的静默区宽度、图片尺寸都不固定（这次是 512x512，
模块数 33），按 W/n 硬切会在边缘累积误差，最后几列整个错位 ——
错一列，手机就扫不出来。

正确做法是**量出模块尺寸**：二维码三个角上的定位图案是 1:1:3:1:1 的
黑白交替（总宽 7 个模块），在穿过它中心的那一行上量出这五段的像素宽度，
除以 7 就是每模块多少像素。有了它，模块数 = 内容宽 / 模块尺寸，且可以用
三个定位图案做交叉校验。

3DS 端拿到的是 **n x n 的 0/1 矩阵**（33x33 = 1089 bit），自己画方块，
每块 6~7 px，正好填满 320x240 底屏 —— 不用在 3DS 上解码 PNG。
"""

import sys

try:
    from PIL import Image
except ImportError:                                   # pragma: no cover
    Image = None


def qr_to_matrix(png_bytes, threshold=128):
    """返回 (n, matrix)。matrix[i][j] 为 1 表示深色模块。失败返回 (0, [])."""
    if Image is None:
        raise RuntimeError("需要 Pillow：pip install Pillow")
    import io
    im = Image.open(io.BytesIO(png_bytes)).convert("L")
    w, h = im.size
    px = im.load()

    # 二值化：1 = 深色
    def dark(x, y):
        return 1 if px[x, y] < threshold else 0

    # 内容边界（去掉外围静默区）
    x0, y0, x1, y1 = w, h, -1, -1
    for y in range(h):
        for x in range(w):
            if dark(x, y):
                if x < x0:
                    x0 = x
                if x > x1:
                    x1 = x
                if y < y0:
                    y0 = y
                if y > y1:
                    y1 = y
    if x1 < 0:
        return 0, []

    # 用左上定位图案量模块尺寸。
    # 取穿过定位图案中心的一行（距内容顶 ~3.5 个模块），扫 1:1:3:1:1。
    # 先粗估一个行位置：定位图案中心 = y0 + 3 个模块。
    # 模块尺寸未知，所以用"从 y0 往下逐行找第一个 1:1:3:1:1"的办法。
    def runs_in_row(y):
        out = []
        cur = dark(x0, y)
        n = 1
        for x in range(x0 + 1, x1 + 1):
            d = dark(x, y)
            if d == cur:
                n += 1
            else:
                out.append((cur, n))
                cur, n = d, 1
        out.append((cur, n))
        return out

    mod = 0
    for y in range(y0, min(y0 + 40, y1)):
        rs = runs_in_row(y)
        # 跳过开头的白段（静默区）
        i = 0
        while i < len(rs) and rs[i][0] == 0:
            i += 1
        seg = rs[i:i + 5]
        if len(seg) < 5:
            continue
        pat = [s[0] for s in seg]
        if pat != [1, 0, 1, 0, 1]:
            continue
        widths = [s[1] for s in seg]
        total = sum(widths)
        unit = total / 7.0
        # 校验 1:1:3:1:1（中心段约 3 倍宽）
        if unit < 1:
            continue
        if not (0.5 * unit <= widths[0] <= 1.6 * unit and
                2.2 * unit <= widths[2] <= 3.8 * unit):
            continue
        mod = unit
        break

    if mod <= 0:
        # 退路：按内容宽度除以标准模块数候选，挑最接近整数的
        span = x1 - x0 + 1
        best, bestd = 0, 1e9
        for n in range(21, 58, 4):
            d = abs(span / n - round(span / n))
            if d < bestd:
                bestd, best = d, n
        if not best:
            return 0, []
        mod = span / best

    n = int(round((x1 - x0 + 1) / mod))
    # 校正到合法模块数（版本 1..40 -> 21,25,...,177 步长 4）
    legal = [21 + 4 * k for k in range(40)]
    n = min(legal, key=lambda L: abs(L - n))

    step = (x1 - x0 + 1) / float(n)
    mat = []
    for j in range(n):
        row = []
        yy = int(y0 + (j + 0.5) * step)
        yy = min(max(yy, 0), h - 1)
        for i in range(n):
            xx = int(x0 + (i + 0.5) * step)
            xx = min(max(xx, 0), w - 1)
            row.append(dark(xx, yy))
        mat.append(row)
    return n, mat


def check_finder(mat, n):
    """用三个定位图案校验矩阵是否正确。返回吻合度 0~1。"""
    if n < 21:
        return 0.0
    ok = tot = 0
    for bi, bj in ((0, 0), (n - 7, 0), (0, n - 7)):
        for a in range(7):
            for b in range(7):
                exp = 1 if (a in (0, 6) or b in (0, 6) or
                            (2 <= a <= 4 and 2 <= b <= 4)) else 0
                if mat[bj + b][bi + a] == exp:
                    ok += 1
                tot += 1
    return ok / float(tot)


if __name__ == "__main__":
    import base64
    import os
    import urllib.request

    p = sys.argv[1] if len(sys.argv) > 1 else ""
    if p.startswith("http"):
        data = urllib.request.urlopen(p, timeout=15).read()
    elif p.endswith(".txt"):
        data = base64.b64decode(open(p).read().strip())
    else:
        data = open(p, "rb").read()

    n, m = qr_to_matrix(data)
    print("模块数 = %d   校验吻合度 = %.1f%%" % (n, 100 * check_finder(m, n)))
    if n:
        print("矩阵字节数（打包成 0/1）= %d，每行 %d" % (n * n, n))
        for r in m[:10]:
            print("  " + "".join("#" if v else "." for v in r))
