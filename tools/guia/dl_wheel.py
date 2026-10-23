#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""
dl_wheel.py — 分块断点续传下载 torch CUDA wheel

为什么不用 pip
--------------
pip 直连在 53.7 MB / 2753 MB 处反复 `Tunnel connection failed: 502`
（WorkBuddy 沙箱的 HTTP 代理扛不住 2.7GB 的长连接）。
但 HEAD 请求能正常返回 200 和正确大小 —— 说明**通路是好的，
只是单条连接撑不住**。所以改成：Range 分块 + 每块独立重试，
失败只重传那一块。已经下好的块永远不重下。
"""
import os
import sys
import time
import http.client
import urllib.parse

URL = ("https://mirrors.aliyun.com/pytorch-wheels/cu128/"
       "torch-2.10.0%2Bcu128-cp313-cp313-win_amd64.whl")
DEST = r"E:\torch-venv\torch_cu128.whl"
UA = "Mozilla/5.0 (Windows NT 10.0; Win64; x64)"

CHUNK = 32 * 1024 * 1024      # 32 MB 一块
MAX_RETRY = 40


def size_of(url):
    p = urllib.parse.urlparse(url)
    c = http.client.HTTPSConnection(p.netloc, timeout=30)
    c.request("HEAD", p.path, headers={"User-Agent": UA})
    r = c.getresponse()
    n = int(r.getheader("Content-Length"))
    c.close()
    return n


def fetch_range(url, start, end, fh):
    """取[start, end] 写入 fh 的对应位置。成功返回 True。"""
    p = urllib.parse.urlparse(url)
    c = http.client.HTTPSConnection(p.netloc, timeout=120)
    c.request("GET", p.path, headers={
        "User-Agent": UA,
        "Range": "bytes=%d-%d" % (start, end),
    })
    r = c.getresponse()
    if r.status not in (200, 206):
        c.close()
        return False, "HTTP %d" % r.status
    got = 0
    want = end - start + 1
    fh.seek(start)
    while got < want:
        b = r.read(min(1 << 20, want - got))
        if not b:
            break
        fh.write(b)
        got += len(b)
    c.close()
    if got < want:
        return False, "短读 %d/%d" % (got, want)
    return True, ""


def main():
    total = size_of(URL)
    print("目标 %.2f GB (%d 字节)  分块 %d MB  共 %d 块"
          % (total / 2 ** 30, total, CHUNK >> 20,
             (total + CHUNK - 1) // CHUNK))
    nchunk = (total + CHUNK - 1) // CHUNK

    # 每块一个 .ok 标记文件：存在即该块已下好。
    # 用标记而不是靠文件长度判断，因为断流可能留下半个块。
    okdir = DEST + ".parts"
    os.makedirs(okdir, exist_ok=True)

    t0 = time.time()
    done_bytes = 0
    for i in range(nchunk):
        start = i * CHUNK
        end = min(start + CHUNK, total) - 1
        mark = os.path.join(okdir, "%04d.ok" % i)
        if os.path.exists(mark):
            done_bytes += end - start + 1
            continue

        fh = open(DEST, "r+b" if os.path.exists(DEST) else "wb")
        ok = False
        for attempt in range(MAX_RETRY):
            try:
                ok, err = fetch_range(URL, start, end, fh)
                if ok:
                    break
            except Exception as e:
                err = type(e).__name__ + ": " + str(e)[:60]
            time.sleep(min(8, 0.5 * (attempt + 1)))
        fh.close()
        if ok:
            open(mark, "w").close()
            done_bytes += end - start + 1
        else:
            print("块 %d 失败：%s" % (i, err))
            print("已下 %.1f MB / %.1f MB (%.1f%%)"
                  % (done_bytes / 1e6, total / 1e6, 100.0 * done_bytes / total))
            return 1

        if i % 10 == 0 or i == nchunk - 1:
            el = time.time() - t0
            rate = done_bytes / max(1, el)
            print("  %5.1f%%  %.0f MB / %.0f MB  %.1f MB/s  已用 %.0fs"
                  % (100.0 * done_bytes / total, done_bytes / 1e6,
                     total / 1e6, rate / 1e6, el), flush=True)

    got = os.path.getsize(DEST)
    if got != total:
        print("大小不符：%d != %d" % (got, total))
        return 1
    print("下载完成 %.2f GB  用时 %.0f 分钟" % (got / 2 ** 30, (time.time() - t0) / 60))
    return 0


if __name__ == "__main__":
    sys.exit(main())