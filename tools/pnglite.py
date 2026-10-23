#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
pnglite.py -- 只够用的 PNG 读写（无第三方依赖）

为什么不用 Pillow：装它要下载，而这台机器上"能本机做完就别联网"。
PNG 的解码无非是 zlib 解压 + 反滤波（5 种 filter），编码是反过来 ——
标准库里 zlib 与 binascii 都在，几十行就够，而这里只需要 RGBA8 与
几种常见颜色类型。

支持读：color type 0/2/3/4/6，位深 8（16 位会丢掉低字节并警告）。
支持写：color type 6（RGBA8）。
"""

import struct
import zlib


def _paeth(a, b, c):
    p = a + b - c
    pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
    if pa <= pb and pa <= pc:
        return a
    if pb <= pc:
        return b
    return c


def read_png(path):
    """读 PNG，返回 (w, h, [(r,g,b,a), ...]) 行优先。"""
    d = open(path, "rb").read()
    if d[:8] != b"\x89PNG\r\n\x1a\n":
        raise ValueError("不是 PNG")

    pos = 8
    idat = b""
    w = h = depth = ctype = None
    plte = None
    trns = None

    while pos < len(d):
        ln = struct.unpack(">I", d[pos:pos + 4])[0]
        typ = d[pos + 4:pos + 8]
        body = d[pos + 8:pos + 8 + ln]
        if typ == b"IHDR":
            w, h, depth, ctype, comp, filt, inter = struct.unpack(">IIBBBBB", body)
            if depth != 8:
                raise ValueError("只支持位深 8（这份是 %d）" % depth)
            if inter != 0:
                raise ValueError("只支持非隔行")
        elif typ == b"PLTE":
            plte = body
        elif typ == b"tRNS":
            trns = body
        elif typ == b"IDAT":
            idat += body
        elif typ == b"IEND":
            break
        pos += 12 + ln

    if ctype is None or idat == b"":
        raise ValueError("缺 IHDR 或 IDAT")

    nch = {0: 1, 2: 3, 3: 1, 4: 2, 6: 4}[ctype]
    raw = zlib.decompress(idat)
    stride = w * nch
    out = []
    prev = bytearray(stride)
    p = 0
    for _ in range(h):
        ft = raw[p]
        p += 1
        line = bytearray(raw[p:p + stride])
        p += stride
        if ft == 1:
            for i in range(nch, stride):
                line[i] = (line[i] + line[i - nch]) & 0xFF
        elif ft == 2:
            for i in range(stride):
                line[i] = (line[i] + prev[i]) & 0xFF
        elif ft == 3:
            for i in range(stride):
                a = line[i - nch] if i >= nch else 0
                line[i] = (line[i] + ((a + prev[i]) >> 1)) & 0xFF
        elif ft == 4:
            for i in range(stride):
                a = line[i - nch] if i >= nch else 0
                c = prev[i - nch] if i >= nch else 0
                line[i] = (line[i] + _paeth(a, prev[i], c)) & 0xFF
        elif ft != 0:
            raise ValueError("未知 filter %d" % ft)
        prev = line

        row = []
        for x in range(w):
            v = line[x * nch:(x + 1) * nch]
            if ctype == 6:
                row.append((v[0], v[1], v[2], v[3]))
            elif ctype == 2:
                row.append((v[0], v[1], v[2], 255))
            elif ctype == 0:
                row.append((v[0], v[0], v[0], 255))
            elif ctype == 4:
                row.append((v[0], v[0], v[0], v[1]))
            elif ctype == 3:
                i = v[0]
                r, g, b = (plte[i * 3], plte[i * 3 + 1], plte[i * 3 + 2]) if plte else (0, 0, 0)
                a = trns[i] if (trns and i < len(trns)) else 255
                row.append((r, g, b, a))
        out.append(row)
    return w, h, out


def write_png(path, w, h, rows):
    """写 RGBA8 的 PNG。"""
    raw = bytearray()
    for y in range(h):
        raw.append(0)                      # filter 0
        row = rows[y]
        for x in range(w):
            r, g, b, a = row[x]
            raw += bytes((r & 0xFF, g & 0xFF, b & 0xFF, a & 0xFF))

    def chunk(typ, body):
        c = struct.pack(">I", len(body)) + typ + body
        return c + struct.pack(">I", zlib.crc32(typ + body) & 0xFFFFFFFF)

    png = b"\x89PNG\r\n\x1a\n"
    png += chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(bytes(raw), 9))
    png += chunk(b"IEND", b"")
    open(path, "wb").write(png)


def crop(rows, w, x0, y0, cw, ch):
    return [rows[y][x0:x0 + cw] for y in range(y0, y0 + ch)]


def resize_nn(rows, w, h, nw, nh):
    """最近邻缩放。图标这种硬边图形不需要插值 ——
    插值反而会把像素边界糊掉，那正是小尺寸图标最怕的。"""
    out = []
    for y in range(nh):
        sy = min(h - 1, y * h // nh)
        src = rows[sy]
        out.append([src[min(w - 1, x * w // nw)] for x in range(nw)])
    return out


def resize_box(rows, w, h, nw, nh):
    """面积平均降采样（box filter）。

    为什么不能只用最近邻：200 → 24 是 8 倍多，最近邻会**丢掉 7/8 的信息**，
    而丢掉的正是小图标最需要的东西（笔画之间的空隙）。
    面积平均把一个源像素区域的颜色取均值，等价于"这个输出像素该是什么颜色"。

    为什么不用 LANCZOS（现在有 PIL 时用的那个）：它假设源图是连续图像，
    会做插值；而像素风图标**本来就该保持硬边缘**，
    插值反而把锯齿糊掉。两者的取舍不同，这里只做 box。
    """
    out = []
    for y in range(nh):
        y0 = y * h // nh
        y1 = max(y0 + 1, (y + 1) * h // nh)
        row = []
        for x in range(nw):
            x0 = x * w // nw
            x1 = max(x0 + 1, (x + 1) * w // nw)
            r = g = b = a = n = 0
            for yy in range(y0, y1):
                src = rows[yy]
                for xx in range(x0, x1):
                    p = src[xx]
                    r += p[0]; g += p[1]; b += p[2]; a += p[3]
                    n += 1
            row.append((r // n, g // n, b // n, a // n))
        out.append(row)
    return out


def bbox_nonbg(rows, bg=(0, 0, 0), tol=12):
    """找出与 bg 差别够大的像素范围（用来从对比图里切出一格）。"""
    h = len(rows)
    w = len(rows[0]) if h else 0
    x0, y0, x1, y1 = w, h, -1, -1
    for y in range(h):
        for x in range(w):
            r, g, b = rows[y][x][:3]
            if abs(r - bg[0]) + abs(g - bg[1]) + abs(b - bg[2]) > tol:
                if x < x0: x0 = x
                if x > x1: x1 = x
                if y < y0: y0 = y
                if y > y1: y1 = y
    return x0, y0, x1 + 1, y1 + 1


if __name__ == "__main__":
    import sys
    w, h, rows = read_png(sys.argv[1])
    print("%s  %dx%d" % (sys.argv[1], w, h))
