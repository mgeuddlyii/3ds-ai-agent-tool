#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
sd_serve.py -- 把电脑开成一个"给 3DS 传文件"的网页。

怎么用
------
    双击这个文件，或者：
        python sd_serve.py                    # 用 share/ 目录
        python sd_serve.py D:\\3ds游戏         # 指定目录
    然后浏览器打开它打印的地址。

流程
----
    1. 网页上把 .cia / .3dsx 拖进去      -> 存进 share/
    2. 网页上填 3DS 显示的四位配对码      -> 这一对之后才能取
    3. 3DS 的互传页按 Y 读清单、按 A 开始取

为什么需要那四位码
------------------
    同一个 Wi-Fi 下**别人**也能打开这个网页。把文件拖进来不危险
    （那是他自己的文件），但**取走**就危险了 —— 他能把你的游戏拷走。
    所以：**上传自由，取文件要配对**。码由**这个服务**生成、
    显示在 3DS 上、由你敲进网页。局域网内 4 位（1 万种）够用。

为什么上传不用 multipart
------------------------
    multipart/form-data 要手工切边界才能拿到文件名，那段代码
    "已读了多少字节"的记账极易出错，而一个 800 MB 的游戏读错几个字节
    就是**一个坏掉的 .cia**。
    改成：文件名走 query（`POST /upload?name=xxx`），**body 就是文件本身**。
    服务端于是**零解析、纯流式**，几百 MB 也不会进内存。

为什么不用 `python -m http.server`
---------------------------------
    它**不支持 Range**，而 3DS 那边是**分块**传的
    （见 shared/xfer.c 的 xfer_pull_chunked）。
    服务器对 Range 请求会回"整个文件 + 200"，而 3DS 是**追加**写 ——
    结果是攒出一个几倍大的坏文件，**而且不报错**。
"""

import argparse
import http.server
import json
import locale
import os
import random
import re
import socket
import socketserver
import sys
import threading
import time
import urllib.parse

HERE = os.path.dirname(os.path.abspath(__file__))

# ---- 界面语言 ----
# 这是个**双击就跑**的小工具，所以不能要求用户先去改配置：
# 默认跟随环境（LANG / LC_ALL / 系统区域），Windows 中文系统就是中文，
# 英文系统就是英文；实在判断不出来时**回退中文**（作者与主要用户都是中文）。
# 想强制指定就传 --lang zh / --lang en。
_LANG = "zh"


def detect_lang(explicit=None):
    if explicit in ("zh", "en"):
        return explicit
    for k in ("LC_ALL", "LC_MESSAGES", "LANG"):
        v = (os.environ.get(k) or "").strip().lower()
        if not v:
            continue
        return "zh" if v.startswith("zh") else "en"
    try:
        loc = (locale.getdefaultlocale()[0] or "").lower()
    except Exception:                      # noqa: BLE001
        loc = ""
    return "zh" if loc.startswith("zh") else "en"


def T(zh, en):
    """按当前语言取一句。所有面向用户的输出都必须过这里 ——
    散着写中文字面量的话，以后加语言就要满文件找。"""
    return en if _LANG == "en" else zh
PAGE = os.path.join(HERE, "sd_serve", "page.html")

WANT_EXT = (".3ds", ".3dsx", ".cia", ".nds", ".cci")
MAX_LIST = 80
CHUNK = 262144

# Range 只支持单段 "bytes=a-b"（3DS 就这么发）。
# 多段（逗号分隔）当没给 —— "半支持"比"不支持"更难查。
RANGE_RE = re.compile(r"^bytes=(\d+)-(\d*)$")

# ---- 配对码 ----
# 只在内存里：重启服务就换码。落盘等于把"谁能取你文件"的凭据留在硬盘上。
_code = None
_code_born = 0.0
_paired = False
_lock = threading.Lock()

CODE_TTL = 600.0        # 码的寿命：10 分钟。
# 定寿命是因为"码一直有效"等于"配对形同虚设" —— 十分钟前在场的那个约定，
# 不代表现在站在机子前的是同一个人。


def current_code():
    global _code, _code_born
    with _lock:
        if _code is None or (time.time() - _code_born) > CODE_TTL:
            _code = "%04d" % random.randint(0, 9999)
            _code_born = time.time()
            _paired = False
        return _code


def is_paired():
    global _paired
    with _lock:
        if _code is None or (time.time() - _code_born) > CODE_TTL:
            return False
        return _paired


def code_ok(c):
    # ★ 这个 global 不是形式主义：漏了它，`_paired = True` 赋的是**局部变量**，
    #   函数照样 return True（所以网页上会显示"配对成功"），
    #   而全局状态从来没被改 —— 紧接着的 /list.txt 仍然 403。
    #   症状是"配对成功但还是取不到东西"，而真因在两行之外。
    #   模块级可变状态被多个函数改时，**每个函数都要 global**。
    global _paired
    want = current_code()
    if c == want:
        with _lock:
            _paired = True
        return True, ""
    if is_paired():
        return True, ""
    return False, "对不上（3DS 上显示的是四位数字）"


def local_ip():
    """本机在局域网里的 IP。

    ★ 不能打印 127.0.0.1 —— 那是回环，3DS 连不上。
      用 UDP 连一下 8.8.8.8 让系统选路（一个字节都不发），再读本地地址。
    """
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        s.connect(("8.8.8.8", 80))
        return s.getsockname()[0]
    except OSError:
        return "127.0.0.1"
    finally:
        s.close()


def collect(root):
    out = []
    try:
        names = os.listdir(root)
    except OSError:
        return out
    for n in names:
        if n.startswith(".") or n.endswith(".part"):
            continue
        p = os.path.join(root, n)
        if not os.path.isfile(p):
            continue
        if not n.lower().endswith(WANT_EXT):
            continue
        try:
            out.append((n, os.path.getsize(p)))
        except OSError:
            continue
    out.sort(key=lambda t: t[0].lower())
    return out[:MAX_LIST]


class Handler(http.server.BaseHTTPRequestHandler):
    # 状态行必须是 latin-1（协议规定）。不钉住的话，某个中文 message
    # 会在写响应时抛 UnicodeEncodeError，表现为"客户端连接被断" ——
    # 而那完全看不出是编码问题。
    protocol_version = "HTTP/1.0"

    def log_message(self, fmt, *args):
        # 3DS 每拉一块就一条记录，不静默的话一个 800 MB 文件能刷两百行，
        # 把真正的错误淹掉。
        pass

    def _err(self, code, msg_ascii, explain_cn):
        """★ msg 只能是 ASCII：它进的是 HTTP 状态行，而状态行是 latin-1。
        中文放 explain（响应体，UTF-8）。中文进状态行的后果不是回错误码，
        而是 UnicodeEncodeError 把连接直接断掉 —— 客户端什么都收不到，
        3DS 那边只会以为"连不上"，而真因是"文件名带了路径"。"""
        body = explain_cn.encode("utf-8")
        self.send_response(code, msg_ascii)
        self.send_header("Content-Type", "text/plain; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def _json(self, obj, code=200):
        body = json.dumps(obj, ensure_ascii=False).encode("utf-8")
        self.send_response(code)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_POST(self):
        if self.path.startswith("/upload"):
            self._upload()
        elif self.path == "/pair":
            self._pair()
        else:
            self._err(405, "Not allowed", "这里只收上传与配对")

    do_PUT = do_DELETE = do_POST

    def do_GET(self):
        name = self.path.split("?", 1)[0].lstrip("/")
        if name == "":
            self._page()
        elif name == "ask":
            # 3DS 第一次进来取配对码。**不要求已配对** ——
            # 不然它连"开始配对"这个动作都做不了。
            self._json({"code": current_code(),
                        "paired": is_paired(),
                        "ttl": int(CODE_TTL)})
        elif name == "ask.txt":
            # ★ 3DS 那边用这个而不是 /ask（JSON）：
            #   那边要为一个字段引一个 JSON 解析器不值得，
            #   而四位数字读出来只要找第一段非数字为止。
            body = current_code().encode("ascii")
            self.send_response(200)
            self.send_header("Content-Type", "text/plain; charset=ascii")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
        elif name == "list.txt":
            self._list()
        elif name.startswith("get/"):
            # ★ 路径里的百分号编码要解开，而且要按 **UTF-8** 解。
            #   http.server 给的 self.path 是**没解过**的，而且它按
            #   latin-1 理解的 —— 所以 "我的游戏.cia" 到这里是一串 %XX，
            #   直接拿去找文件必然 404（表现为"明明上传成功了却取不到"）。
            self._file(urllib.parse.unquote(name[4:], encoding="utf-8",
                                            errors="replace"))
        else:
            self._err(404, "Not found", "没这个页面")

    def do_HEAD(self):
        self.do_GET()

    # ---- 网页 ----
    def _page(self):
        try:
            body = open(PAGE, "rb").read()
        except OSError:
            self._err(500, "No page", "网页文件不见了（page.html）")
            return
        self.send_response(200)
        self.send_header("Content-Type", "text/html; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    # ---- 配对 ----
    def _pair(self):
        n = int(self.headers.get("Content-Length") or 0)
        if n <= 0 or n > 4096:
            self._json({"ok": False, "why": "没收到内容"}, 400)
            return
        try:
            data = json.loads(self.rfile.read(n).decode("utf-8"))
            code = str(data.get("code", ""))
        except Exception:
            self._json({"ok": False, "why": "看不懂你发的东西"}, 400)
            return
        ok, why = code_ok(code)
        self._json({"ok": ok, "why": why})

    # ---- 上传（body 就是文件本身，名字在 query 里）----
    def _upload(self):
        q = urllib.parse.urlparse(self.path).query
        name = urllib.parse.parse_qs(q).get("name", [""])[0]
        name = os.path.basename(name)

        if (not name) or ("/" in name) or ("\\" in name) or (".." in name):
            self._json({"ok": False, "why": "文件名不合法"}, 400)
            return
        if not name.lower().endswith(WANT_EXT):
            self._json({"ok": False,
                        "why": "不认识的类型：%s" % name}, 400)
            return
        try:
            n = int(self.headers.get("Content-Length") or 0)
        except ValueError:
            n = 0
        if n <= 0:
            self._json({"ok": False, "why": "空上传"}, 400)
            return

        os.makedirs(self.server.share_dir, exist_ok=True)
        final = os.path.join(self.server.share_dir, name)
        part = final + ".part"
        written = 0
        left = n
        try:
            with open(part, "wb") as f:
                while left > 0:
                    chunk = self.rfile.read(min(CHUNK, left))
                    if not chunk:
                        break
                    f.write(chunk)
                    left -= len(chunk)
                    written += len(chunk)
            # ★ 收齐了才把 .part 改名成正名。网页那边也这么干。
            #   传输中途断掉留下的 .part 不会被当成一个"完整的游戏"，
            #   而 collect() 也显式忽略 .part —— 两道都不认它。
            os.replace(part, final)
        except (BrokenPipeError, ConnectionResetError):
            try:
                os.remove(part)
            except OSError:
                pass
            self._json({"ok": False, "why": "你那边断开了"}, 400)
            return
        except OSError as e:
            try:
                os.remove(part)
            except OSError:
                pass
            self._json({"ok": False, "why": "写不了：%s" % e}, 500)
            return

        if left > 0:
            # Content-Length 说有 N 字节，实际只读到 N-left —— 这是网络问题，
            # 而收下的那份**不能**当成完整的文件留着。
            try:
                os.remove(final)
            except OSError:
                pass
            self._json({"ok": False,
                        "why": "没传完（少了 %d 字节），已丢弃" % left}, 400)
            return

        self._json({"ok": True, "name": name, "bytes": written})

    # ---- 清单（要配对）----
    def _list(self):
        if not is_paired():
            self._err(403, "Not paired",
                      "还没配对。3DS 上会显示四位码，填进网页那一栏。")
            return
        items = collect(self.server.share_dir)
        body = "".join("%s|%d\n" % (n, s) for n, s in items).encode("utf-8")
        self.send_response(200)
        self.send_header("Content-Type", "text/plain; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    # ---- 取文件（要配对 + 支持 Range）----
    def _file(self, name):
        if not is_paired():
            self._err(403, "Not paired", "还没配对")
            return
        # ★ 防目录穿越：只收纯文件名
        if (not name) or ("/" in name) or ("\\" in name) or (".." in name):
            self._err(400, "Bad file name", "文件名不合法")
            return

        path = os.path.join(self.server.share_dir, name)
        if not os.path.isfile(path):
            self._err(404, "Not found", "没这个文件")
            return
        try:
            size = os.path.getsize(path)
        except OSError:
            self._err(500, "Cannot read", "读不了这个文件")
            return

        start, end = 0, size - 1
        partial = False
        rng = self.headers.get("Range")
        if rng:
            mm = RANGE_RE.match(rng.strip())
            if mm:
                start = int(mm.group(1))
                if mm.group(2):
                    end = int(mm.group(2))
                if start >= size:
                    self.send_response(416)
                    self.send_header("Content-Range", "bytes */%d" % size)
                    self.send_header("Content-Length", "0")
                    self.end_headers()
                    return
                if end >= size:
                    end = size - 1
                partial = True

        length = end - start + 1
        self.send_response(206 if partial else 200)
        self.send_header("Content-Type", "application/octet-stream")
        self.send_header("Content-Length", str(length))
        self.send_header("Accept-Ranges", "bytes")
        if partial:
            self.send_header("Content-Range",
                             "bytes %d-%d/%d" % (start, end, size))
        self.end_headers()

        if self.command == "HEAD":
            return
        try:
            with open(path, "rb") as f:
                f.seek(start)
                left = length
                while left > 0:
                    chunk = f.read(min(CHUNK, left))
                    if not chunk:
                        break
                    self.wfile.write(chunk)
                    left -= len(chunk)
        except (BrokenPipeError, ConnectionResetError):
            # 3DS 那边按了"放弃"就会这样，属正常收场。
            pass
        except OSError:
            pass


class Server(socketserver.ThreadingTCPServer):
    # 3DS 是逐块来请求的，串行服务会让速度忽快忽慢。
    allow_reuse_address = True
    daemon_threads = True


def main():
    global _LANG

    ap = argparse.ArgumentParser(
        description="给 3DS 传文件的网页服务 / Web file server for 3DS")
    ap.add_argument("dir", nargs="?", default=os.path.join(HERE, "share"),
                    help="收文件的目录（默认：脚本同级的 share） / "
                         "receiving directory (default: share/ next to this script)")
    ap.add_argument("--port", type=int, default=8080)
    ap.add_argument("--lang", default="auto", choices=["auto", "zh", "en"],
                    help="界面语言 / UI language (default: follow the system)")
    args = ap.parse_args()
    _LANG = detect_lang(args.lang)

    root = os.path.abspath(args.dir)
    os.makedirs(root, exist_ok=True)
    if not os.path.isfile(PAGE):
        print(T("找不到网页文件：%s", "Page file not found: %s") % PAGE)
        sys.exit(1)

    code = current_code()
    ip = local_ip()

    print("=" * 58)
    print(T("  收文件的目录：%s", "  Receiving into: %s") % root)
    print("-" * 58)
    print(T("  1) 浏览器打开：  http://%s:%d",
          "  1) Open in a browser:  http://%s:%d") % (ip, args.port))
    print(T("  2) 把 .cia / .3dsx 拖进那个页面",
          "  2) Drag .cia / .3dsx onto that page"))
    print(T("  3) 3DS 进互传页，它会显示一个**四位码**",
          "  3) On the 3DS, open the transfer screen - it shows a **4-digit code**"))
    print(T("     （现在的码是 %s，3DS 上会显示它）",
          "     (current code: %s - the 3DS shows the same)") % code)
    print(T("     把它填进网页那一栏，再点「配对」",
          "     Type it into the field on the page, then click Pair"))
    print(T("  4) 回 3DS 按 Y 读清单、按 A 开始取",
          "  4) Back on the 3DS: Y reads the list, A starts fetching"))
    print("-" * 58)
    print(T("  这个码十分钟后会变。到那时重新配一次即可。",
          "  The code changes every 10 minutes; just pair again then."))
    print(T("  按 Ctrl+C 停", "  Press Ctrl+C to stop"))
    print("=" * 58)

    srv = Server(("0.0.0.0", args.port), Handler)
    srv.share_dir = root
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        print(T("\n停了。", "\nStopped."))


if __name__ == "__main__":
    try:
        main()
    except Exception:
        import traceback
        traceback.print_exc()
        try:
            input(T("\n出错了，按回车关掉…", "\nError. Press Enter to close..."))
        except Exception:
            pass
