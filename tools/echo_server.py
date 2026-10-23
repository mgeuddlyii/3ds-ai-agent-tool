#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""echo_server.py -- 本地抓请求服务器：把 3DS app 的 POST 原样记录下来。

python tools/echo_server.py [port]   # 默认 18081
响应固定返回一个合法的 chat completion JSON，让 app 走"成功"路径。
抓到的请求写在 logs/echo_capture.txt（追加）。
"""
import http.server
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
CAPTURE = os.path.join(ROOT, "logs", "echo_capture.txt")

RESP = (b'{"choices":[{"index":0,"message":{"role":"assistant",'
        b'"content":"echo-ok"}},{"index":1,"message":{"role":"assistant",'
        b'"content":"second"}}]}')


class Handler(http.server.BaseHTTPRequestHandler):
    def do_POST(self):
        n = int(self.headers.get("Content-Length", 0) or 0)
        body = self.rfile.read(n) if n else b""

        os.makedirs(os.path.dirname(CAPTURE), exist_ok=True)
        with open(CAPTURE, "ab") as f:
            f.write(b"=== REQUEST ===\n")
            f.write(("POST %s\n" % self.path).encode())
            f.write(str(self.headers).encode())
            f.write(b"\nBODY(%d bytes): " % len(body))
            f.write(body[:2000])
            f.write(b"\n\n")

        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(RESP)))
        self.end_headers()
        self.wfile.write(RESP)

    def log_message(self, *args):          # 静音，全靠 capture 文件
        pass


if __name__ == "__main__":
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 18081
    print("echo server on 127.0.0.1:%d -> %s" % (port, CAPTURE), flush=True)
    http.server.HTTPServer(("127.0.0.1", port), Handler).serve_forever()
