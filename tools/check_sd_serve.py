#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
check_sd_serve.py -- 验收 tools/sd_serve.py + sd_serve/page.html（**本机跑，零积分**）

为什么给它写判据：
  3DS 那边的传输是**分块**的，而分块对服务端有三条硬要求 ——
    ① 必须支持 Range（否则 3DS 的 206 检查会中止；而更糟的是某些服务器
       回 200 + 整文件，被追加写出几倍大的坏文件，**且不报错**）
    ② 越界请求必须回**真正的错误码**，不是断连
    ③ 配对必须**真的**生效（"返回成功但状态没改"这种最阴 ——
       网页上看着一切正常，可 3DS 就是取不到）
  这三条都在服务端，而服务端**在电脑上** —— 所以它能在本机验，
  不该占用 3DS 模拟器那点积分。
"""

import json
import os
import re
import shutil
import socket
import subprocess
import sys
import tempfile
import time
import urllib.error
import urllib.parse
import urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
SERVE = os.path.join(HERE, "sd_serve.py")
PAGE = os.path.join(HERE, "sd_serve", "page.html")

ok = 0
bad = []


def expect(name, cond, extra=""):
    global ok
    if cond:
        ok += 1
        print("  PASS  %s %s" % (name, extra))
    else:
        bad.append(name)
        print("  FAIL  %s %s" % (name, extra))


def expect_safe(name, fn, extra=""):
    """跑一段可能会抛的检查，抛了就记 FAIL 而不是让整个判据崩掉。

    ★ 为什么必须有它：破坏测试（临时拆掉 Range 或 unquote）时，
      判据自己抛了 HTTPError → 整个脚本带着 traceback 死掉 →
      退出码 1 看着像"抓到了"，其实**没有输出任何一条失败项**。
      那种退出码会骗人：它分不清"功能坏了"与"检查本身崩了"。
      判据自己崩掉时必须仍然报出**是哪一条坏了**。"""
    try:
        cond = fn()
    except Exception as e:
        cond = False
        extra = "%s 抛了 %s" % (extra, type(e).__name__)
    expect(name, bool(cond), extra)


def free_port():
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    p = s.getsockname()[1]
    s.close()
    return p


def code_lines(text):
    """去掉注释行与行尾注释 —— 判据必须只扫真代码。"""
    out = []
    for ln in text.splitlines():
        s = ln.lstrip()
        if s.startswith("#"):
            continue
        out.append(ln.split("  #", 1)[0])
    return out


def main():
    for f in (SERVE, PAGE):
        if not os.path.isfile(f):
            print("  找不到 %s，判据无从跑起" % os.path.basename(f))
            return 1

    tmp = tempfile.mkdtemp(prefix="sdserve_")
    try:
        open(os.path.join(tmp, "big.cia"), "wb").write(b"B" * (1024 * 1024))
        open(os.path.join(tmp, "small.3dsx"), "wb").write(b"A" * (300 * 1024))
        open(os.path.join(tmp, "readme.txt"), "wb").write(b"ignore me")
        open(os.path.join(tmp, ".hidden.3dsx"), "wb").write(b"X" * 10)

        port = free_port()
        proc = subprocess.Popen([sys.executable, SERVE, tmp, "--port", str(port)],
                                stdout=subprocess.DEVNULL,
                                stderr=subprocess.DEVNULL)
        base = "http://127.0.0.1:%d" % port
        try:
            for _ in range(40):
                try:
                    urllib.request.urlopen(base + "/ask", timeout=1).read()
                    break
                except Exception:
                    time.sleep(0.2)
            else:
                print("  服务没起来")
                return 1

            # ---- 网页 ----
            pg = urllib.request.urlopen(base + "/", timeout=5).read().decode("utf-8")
            expect("网页能打开", "给 3DS 传文件" in pg)
            expect("网页有拖拽区", 'id="drop"' in pg and "dragover" in pg)
            expect("网页有四位码输入", 'id="code"' in pg and 'maxlength="4"' in pg)
            expect("网页是裸传（不依赖 multipart）",
                   "FormData" not in pg and "xhr.send(file)" in pg)
            expect("网页有分段进度条",
                   'class="bar2"' in pg or "className = 'bar2'" in pg)

            # ---- 3DS 取配对码（未配对时也要能取，否则没法开始配）----
            info = json.loads(urllib.request.urlopen(base + "/ask", timeout=5).read())
            code = info.get("code", "")
            expect("3DS 能取到四位码", len(code) == 4 and code.isdigit(),
                   "码=%s" % code)
            expect("初始是未配对状态", info.get("paired") is False)
            # 3DS 侧用纯文本那个（它不该为四个数字引一个 JSON 解析器）
            txt = urllib.request.urlopen(base + "/ask.txt", timeout=5).read().decode()
            expect("ask.txt 给出同样的四位码",
                   txt.strip() == code, "JSON=%s 文本=%s" % (code, txt.strip()))

            # ---- 未配对：清单与文件都拿不到 ----
            for u, name in (("/list.txt", "未配对不给清单"),
                            ("/get/x.cia", "未配对不给文件")):
                try:
                    urllib.request.urlopen(base + u, timeout=5)
                    expect(name, False, "被放行了")
                except urllib.error.HTTPError as e:
                    expect("%s（403）" % name, e.code == 403, "实际 %s" % e.code)

            # ---- 上传（模拟网页拖拽；裸传，名字在 query）----
            payload = b"C" * (512 * 1024)
            rq = urllib.request.Request(
                base + "/upload?name=" + urllib.parse.quote("我的游戏.cia"),
                data=payload, method="POST")
            j = json.loads(urllib.request.urlopen(rq, timeout=20).read().decode())
            expect("裸传上传成功", bool(j.get("ok")) and j.get("bytes") == len(payload),
                   "%s 字节" % j.get("bytes"))
            # ★ 读盘要兜住"文件根本不在"：上传坏了（没改名成正名）时
            #   open() 会 FileNotFoundError 把判据带崩 ——
            #   而崩掉的判据只会说"这里有异常"，不会说"上传没落盘"。
            def _landed():
                return os.path.isfile(os.path.join(tmp, "我的游戏.cia")) and \
                       open(os.path.join(tmp, "我的游戏.cia"), "rb").read() == payload
            expect_safe("落盘内容逐字节一致", _landed)
            expect_safe("没留 .part 残留",
                        lambda: not os.path.exists(
                            os.path.join(tmp, "我的游戏.cia.part")))
            try:
                urllib.request.urlopen(
                    urllib.request.Request(base + "/upload?name=x.txt",
                                           data=b"x", method="POST"), timeout=5)
                expect("拒绝非游戏后缀", False, "被受理了")
            except urllib.error.HTTPError as e:
                expect("拒绝非游戏后缀（400）", e.code == 400, "实际 %s" % e.code)

            # ---- 配对 ----
            def pair(c):
                return json.loads(urllib.request.urlopen(
                    urllib.request.Request(
                        base + "/pair", method="POST",
                        data=json.dumps({"code": c}).encode(),
                        headers={"Content-Type": "application/json"}),
                    timeout=5).read().decode())

            expect("错误码被拒", not pair("0000").get("ok"))
            expect("正确码配对返回成功", pair(code).get("ok") is True)

            # ★ 这一条才是"配对真的生效"。漏 global 的症状正是：
            #   上一条绿、这一条 403 —— 只查返回值等于没验。
            try:
                body = urllib.request.urlopen(base + "/list.txt",
                                             timeout=5).read().decode()
                expect("★ 配对后真能读清单", "我的游戏.cia|" in body, body.strip())
            except urllib.error.HTTPError as e:
                expect("★ 配对后真能读清单", False,
                       "仍回 %d —— 配对状态没真正落下来" % e.code)

            expect("清单带字节数", "|524288" in body)
            expect("清单不含非游戏后缀", "readme.txt" not in body)
            expect("清单不含隐藏文件", ".hidden" not in body)

            # ---- 取文件 + Range ----
            _whole = {}

            def _getall():
                _whole["b"] = urllib.request.urlopen(
                    base + "/get/" + urllib.parse.quote("我的游戏.cia"),
                    timeout=15).read()
                return True
            expect_safe("配对后能整取文件", _getall,
                        "%d 字节" % len(_whole.get("b", b"")))
            expect("  ↳ 整取内容一致", _whole.get("b") == payload)

            # ★ Range 用 **big.cia（1 MB）** 做，不用上面那个 512 KB 的 ——
            #   512 KB 只够两块，而判据要三块，越界的那个会拿到 416。
            #   而 **416 恰恰是正确响应** —— 第一版判据在这里炸了，
            #   报错是"Range 失败"，看着像代码坏了，其实是我自己算错了块数。
            big = b"B" * (1024 * 1024)
            segs = []
            all206 = True

            def _rng():
                nonlocal all206
                for off in range(0, 3 * 262144, 262144):
                    rq = urllib.request.Request(base + "/get/big.cia")
                    rq.add_header("Range", "bytes=%d-%d" % (off, off + 262143))
                    r = urllib.request.urlopen(rq, timeout=5)
                    if r.status != 206:
                        all206 = False
                    segs.append(r.read())
                return True
            expect_safe("Range 三块都能取到", _rng,
                        ("中途回的不是 206" if not all206 else ""))
            expect("每个 Range 都回 206（不是 200）", all206)
            expect("每块长度正确", all(len(s) == 262144 for s in segs))
            expect("三块拼接 == 原文件前 786432 字节",
                   b"".join(segs) == big[:3 * 262144])

            # 收尾那一点也要真的能取到（最后一块常常短于整块）
            def _tail():
                rq = urllib.request.Request(base + "/get/big.cia")
                rq.add_header("Range", "bytes=786432-")
                r = urllib.request.urlopen(rq, timeout=5)
                return r.status == 206 and r.read() == big[786432:]
            expect_safe("最后一块（不满一整块）也对", _tail)

            try:
                rq = urllib.request.Request(base + "/get/big.cia")
                rq.add_header("Range", "bytes=99999999-")
                urllib.request.urlopen(rq, timeout=5)
                expect("越界回 416", False, "居然没报错")
            except urllib.error.HTTPError as e:
                expect("越界回 416", e.code == 416, "实际 %s" % e.code)

            # ---- 中文与空格的文件名（URL 百分号编码）----
            cn = "测试 游戏.cia"
            body_cn = b"Y" * 4096
            urllib.request.urlopen(
                urllib.request.Request(
                    base + "/upload?name=" + urllib.parse.quote(cn),
                    data=body_cn, method="POST"), timeout=5)
            _got = {}

            def _cn():
                _got["b"] = urllib.request.urlopen(
                    base + "/get/" + urllib.parse.quote(cn), timeout=5).read()
                return True
            expect_safe("中文与空格的文件名能上传也能取回", _cn,
                        "实际 %d 字节" % len(_got.get("b", b"")))
            expect("  ↳ 内容一致", _got.get("b") == body_cn)
            # ★ 必须**重新取一次清单**：上面那个 body 是上传中文名**之前**取的，
            #   拿它判"清单里有没有这个名"当然是 False —— 那是判据自己没跟上时序。
            expect_safe("配对后还能再读一次清单（取中文名那个）",
                        lambda: cn in urllib.request.urlopen(
                            base + "/list.txt", timeout=5).read().decode())
            os.remove(os.path.join(tmp, cn))

            # ---- 目录穿越必须回错误码，而不是断连 ----
            for evil, want in (("/get/..%2Fsd_serve.py", 400),
                               ("/get/nope.cia", 404),
                               ("/get/%2e%2e%2fpage.html", 400)):
                try:
                    urllib.request.urlopen(base + evil, timeout=5)
                    expect("挡 %s" % evil, False, "被放行了")
                except urllib.error.HTTPError as e:
                    expect("挡 %s（%d）" % (evil, want), e.code == want,
                           "实际 %s" % e.code)
                except Exception as e:
                    expect("挡 %s（%d）" % (evil, want), False,
                           "连接被断（%s）" % type(e).__name__)

            # ---- 只读 ----
            try:
                urllib.request.urlopen(base + "/list.txt", data=b"x", timeout=5)
                expect("POST 到清单被拒 405", False, "被受理了")
            except urllib.error.HTTPError as e:
                expect("POST 到清单被拒 405", e.code == 405, "实际 %s" % e.code)

            # ---- 源码层面 ----
            src = open(SERVE, encoding="utf-8").read()
            expect("支持 Range", "RANGE_RE" in src and "206" in src)
            expect("解了 URL 百分号编码（中文名才取得到）", "unquote" in src)
            # ★ 只取 send_error 的**第二个参数**：那是进状态行的。
            #   中文进状态行 → UnicodeEncodeError → 连接被断，
            #   客户端一个错误码都收不到。第三个参数是响应体（该是 UTF-8）。
            msgs = [m.group(1) for ln in code_lines(src)
                    for m in re.finditer(r'send_error\(\s*\d+\s*,\s*"([^"]*)"', ln)]
            badmsg = [m for m in msgs if any(ord(c) > 127 for c in m)]
            expect("send_error 的 message 里没有非 ASCII", not badmsg,
                   ("挂的：%s" % badmsg[:1]) if badmsg else "")
            # ★ 函数体里 global 前面可能有注释，正则不能贴死"下一行就是 global"。
            _co = re.search(r"def code_ok\(c\):(.*?)(?=\ndef |\nclass )", src, re.S)
            expect("配对状态是全局的（漏 global 会配而不生效）",
                   bool(_co) and "global _paired" in _co.group(1))
        finally:
            proc.terminate()
            try:
                proc.wait(timeout=5)
            except Exception:
                proc.kill()
    finally:
        shutil.rmtree(tmp, ignore_errors=True)

    print()
    if bad:
        print("== %d ok, %d problem(s) ==" % (ok, len(bad)))
        for b in bad:
            print("   挂的：%s" % b)
        return 1
    print("== %d ok, 0 problem(s) ==" % ok)
    return 0


if __name__ == "__main__":
    sys.exit(main())
