"""Decode a QR PNG and re-render it larger, using only the stdlib.

The 3DS writes the QR as base64 so it never has to decode a PNG. The PC side
does that here, for two reasons:

  * to print/confirm the module matrix (the 3DS will draw from that matrix,
    not from pixels), and
  * to emit an upscaled copy, because a 29x29 pixel image is too small for
    a phone camera to lock onto from a screen.
"""

import struct
import zlib


def png_to_matrix(png):
    """8-bit grayscale or RGB PNG -> (n, [[0/1]*n]). Returns (0, []) on fail."""
    if png[:8] != b"\x89PNG\r\n\x1a\n":
        return 0, []
    pos, w, h, depth, ctype, idat = 8, 0, 0, 0, 0, b""
    while pos + 8 <= len(png):
        ln = int.from_bytes(png[pos:pos + 4], "big")
        typ = png[pos + 4:pos + 8]
        data = png[pos + 8:pos + 8 + ln]
        if typ == b"IHDR":
            w = int.from_bytes(data[0:4], "big")
            h = int.from_bytes(data[4:8], "big")
            depth, ctype = data[8], data[9]
        elif typ == b"IDAT":
            idat += data
        elif typ == b"IEND":
            break
        pos += 12 + ln

    if not w or not h or depth != 8 or ctype not in (0, 2) or w != h:
        return 0, []
    try:
        raw = zlib.decompress(idat)
    except zlib.error:
        return 0, []

    bpp = 1 if ctype == 0 else 3
    stride = w * bpp
    prev = bytearray(stride)
    rows, p = [], 0
    for _ in range(h):
        if p + 1 + stride > len(raw):
            return 0, []
        ft = raw[p]
        p += 1
        line = bytearray(raw[p:p + stride])
        p += stride
        for i in range(stride):
            a = line[i - bpp] if i >= bpp else 0
            b = prev[i]
            c = prev[i - bpp] if i >= bpp else 0
            if ft == 1:
                line[i] = (line[i] + a) & 0xFF
            elif ft == 2:
                line[i] = (line[i] + b) & 0xFF
            elif ft == 3:
                line[i] = (line[i] + ((a + b) >> 1)) & 0xFF
            elif ft == 4:
                pp = a + b - c
                pa, pb, pc = abs(pp - a), abs(pp - b), abs(pp - c)
                pred = a if (pa <= pb and pa <= pc) else (b if pb <= pc else c)
                line[i] = (line[i] + pred) & 0xFF
        rows.append(line)
        prev = line

    mid = w // 2
    out = []
    for r in rows:
        out.append([0 if r[x * bpp] < 128 else 1 for x in range(w)])
    return w, out if mid else out


def write_gray_png(path, w, h, rows):
    """rows: iterable of byte sequences, one byte per pixel."""
    raw = b"".join(b"\x00" + bytes(r) for r in rows)

    def chunk(tag, data):
        return (struct.pack(">I", len(data)) + tag + data +
                struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF))

    ihdr = struct.pack(">IIBBBBB", w, h, 8, 0, 0, 0, 0)
    png = (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", ihdr) +
           chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b""))
    with open(path, "wb") as fh:
        fh.write(png)
    return len(png)


def upscale(matrix, path, scale=12, quiet=4):
    """Render a 0/1 matrix as a big grayscale PNG with a quiet zone."""
    n = len(matrix)
    size = (n + quiet * 2) * scale
    rows = []
    for y in range(size):
        my = y // scale - quiet
        row = bytearray(size)
        for x in range(size):
            mx = x // scale - quiet
            on = 0 <= my < n and 0 <= mx < n and matrix[my][mx]
            row[x] = 0 if on else 255
        rows.append(row)
    return write_gray_png(path, size, size, rows)
