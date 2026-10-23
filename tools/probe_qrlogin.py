#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""豆包「扫码登录」接口实测 —— 验证中转方案的第一环能不能成。

这一步只用 stdlib urllib，不需要浏览器、不需要 Playwright。
如果能拿到一张真二维码，说明「3DS → 中转 → 豆包」这条路的第一段是活的。

流程（照 doubao2api 的 qr_login 思路重写，只跑到"拿到二维码"为止，
**不会真的等用户扫码**）：
  1. GET https://www.doubao.com/                  -> 基础 cookie(ttwid 等)
  2. GET /passport/safe/csrf_token/?aid=497858    -> passport_csrf_token
  3. GET /passport/web/get_qrcode/?next=...&aid=  -> token + base64 PNG
  4. 把 PNG 存下来（可选），并**用纯 Python 解出模块矩阵**
     —— 矩阵才是 3DS 想要的：几十字节，不用在 3DS 上解码 PNG。

用法：
    python tools/probe_qrlogin.py            # 只验证，不存图
    python tools/probe_qrlogin.py --save     # 存 PNG 到 tools/out/ 供肉眼确认
"""

import base64
import http.cookiejar
import json
import os
import sys
import urllib.error
import urllib.parse
import urllib.request
import zlib

BASE = "https://www.doubao.com"
AID = 497858
UA = ("Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
      "(KHTML, like Gecko) Chrome/131.0.0.0 Safari/537.36")

jar = http.cookiejar.CookieJar()
opener = urllib.request.build_opener(
    urllib.request.HTTPCookieProcessor(jar))


def get(url, extra=None):
    req = urllib.request.Request(url, method="GET")
    req.add_header("User-Agent", UA)
    req.add_header("Accept", "application/json, text/plain, */*")
    req.add_header("Referer", "https://www.doubao.com/chat/login")
    req.add_header("Origin", BASE)
    for k, v in (extra or {}).items():
        req.add_header(k, v)
    try:
        with opener.open(req, timeout=20) as r:
            return r.status, r.read().decode("utf-8", "replace")
    except urllib.error.HTTPError as e:
        body = ""
        try:
            body = e.read().decode("utf-8", "replace")
        except Exception:
            pass
        return e.code, body


def cookies_str():
    return "; ".join("%s=%s" % (c.name, c.value) for c in jar)


def png_to_matrix(png_bytes):
    """纯 stdlib 解 PNG -> 黑白矩阵。二维码一定是小图，够用就行。

    只处理 8bit 灰度（colortype 0）与 8bit RGB（colortype 2）。
    返回 (n, [[0/1]*n, ...])；失败返回 (0, [])。
    """
    if png_bytes[:8] != b"\x89PNG\r\n\x1a\n":
        return 0, []
    pos = 8
    w = h = depth = ctype = 0
    idat = b""
    while pos + 8 <= len(png_bytes):
        ln = int.from_bytes(png_bytes[pos:pos + 4], "big")
        typ = png_bytes[pos + 4:pos + 8]
        data = png_bytes[pos + 8:pos + 8 + ln]
        if typ == b"IHDR":
            w = int.from_bytes(data[0:4], "big")
            h = int.from_bytes(data[4:8], "big")
            depth = data[8]
            ctype = data[9]
        elif typ == b"IDAT":
            idat += data
        elif typ == b"IEND":
            break
        pos += 12 + ln

    if not w or not h or depth != 8 or ctype not in (0, 2):
        return 0, []

    try:
        raw = zlib.decompress(idat)
    except Exception:
        return 0, []

    bpp = 1 if ctype == 0 else 3
    stride = w * bpp
    prev = bytearray(stride)
    rows = []
    p = 0
    for _ in range(h):
        if p >= len(raw):
            return 0, []
        ft = raw[p]
        p += 1
        line = bytearray(raw[p:p + stride])
        p += stride
        if len(line) < stride:
            return 0, []
        # PNG 行滤波还原
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
                pa, pb, pc = abs(b - c), abs(a - c), abs(a + b - 2 * c)
                pr = a if (pa <= pb and pa <= pc) else (b if pb <= pc else c)
                line[i] = (line[i] + pr) & 0xFF
        prev = line

        row = []
        for x in range(w):
            px = line[x * bpp]
            row.append(0 if px < 128 else 1)   # 深色=1
        rows.append(row)
    return w, rows


def shrink_matrix(rows, n):
    """把像素图采样成 n x n 的模块矩阵（二维码有静默区，按块取中心点）。"""
    h = len(rows)
    if not h:
        return []
    w = len(rows[0])
    out = []
    for j in range(n):
        line = []
        for i in range(n):
            y = int((j + 0.5) * h / n)
            x = int((i + 0.5) * w / n)
            line.append(rows[y][x])
        out.append(line)
    return out


def main():
    save = "--save" in sys.argv
    print("=" * 70)
    print("豆包扫码登录接口实测（只到拿到二维码，不等扫码）")
    print("=" * 70)

    print("\n[1] GET /  取基础 cookie")
    code, body = get(BASE + "/")
    print("    HTTP %s   cookie: %s" % (code, cookies_str()[:120] or "(空)"))

    print("\n[2] GET /passport/safe/csrf_token")
    code2, body2 = get("%s/passport/safe/csrf_token/?aid=%d" % (BASE, AID))
    csrf = ""
    try:
        j = json.loads(body2)
        csrf = j.get("data", {}).get("passport_csrf_token", "")
    except Exception:
        pass
    if not csrf:
        for c in jar:
            if c.name in ("passport_csrf_token", "passport_csrf_token_default"):
                csrf = c.value
                break
    print("    HTTP %s   csrf=%s" % (code2, (csrf[:24] + "...") if csrf else "(空)"))
    if not csrf:
        print("    拿不到 csrf，后面基本没戏。")
        return 2

    print("\n[3] GET /passport/web/get_qrcode")
    nxt = urllib.parse.quote("https://www.doubao.com", safe="")
    url = "%s/passport/web/get_qrcode/?next=%s&aid=%d" % (BASE, nxt, AID)
    code3, body3 = get(url, {"x-tt-passport-csrf-token": csrf})
    print("    HTTP %s" % code3)
    print("    %s" % body3[:260])

    try:
        j = json.loads(body3)
    except Exception:
        print("    JSON 解析失败 —— 接口可能已改版。")
        return 2

    data = j.get("data", {})
    token = data.get("token", "")
    b64 = data.get("qrcode", "") or data.get("qrcode_url", "")
    print("\n    token      : %s" % (token[:40] + "..." if token else "(无)"))
    print("    qrcode 字段: %d 字符，开头 %s" % (len(b64), b64[:24]))

    if not token:
        print("\n    => 没拿到 token，接口已失效或需要额外参数。")
        return 2

    png = b""
    if b64.startswith("data:"):
        png = base64.b64decode(b64.partition(",")[2])
    elif b64.startswith("http"):
        png = urllib.request.urlopen(b64, timeout=15).read()
    elif b64:
        png = base64.b64decode(b64)
    print("    PNG 字节数 : %d" % len(png))

    if png:
        if save:
            out = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                               "out", "qrcode_probe.png")
            os.makedirs(os.path.dirname(out), exist_ok=True)
            with open(out, "wb") as f:
                f.write(png)
            print("    已存图     : %s" % out)

        w, rows = png_to_matrix(png)
        print("    解出图像   : %dx%d" % (w, len(rows)))
        if w:
            m = shrink_matrix(rows, 29)
            print("    采样矩阵   : %d x %d" % (len(m), len(m[0])))
            # 粗略统计黑白比例，二维码应当接近一半一半
            blacks = sum(sum(r) for r in (rows or []))
            total = w * len(rows) if rows else 1
            print("    深色像素占比: %.1f%%  （二维码通常 40~60%%）"
                  % (100.0 * blacks / total))
            print("\n    前 12 行预览（# = 深色）：")
            for r in m[:12]:
                print("      " + "".join("#" if v else "." for v in r))

    print("\n" + "=" * 70)
    print("判读")
    print("=" * 70)
    if token and png:
        print("  ✅ 二维码接口活着：拿到了 token 和真二维码图。")
        print("  => 「3DS → 局域网中转 → 豆包扫码登录」这条路的第一段成立。")
        print("  => 3DS 端只需要矩阵（29x29 的 0/1），不必在 3DS 上解码 PNG。")
    else:
        print("  ❌ 没拿到完整二维码，这条路的登录环节要先解决。")
    return 0


if __name__ == "__main__":
    sys.exit(main())
