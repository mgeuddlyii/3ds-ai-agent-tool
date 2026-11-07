"""
stt_server.py —— 在电脑上跑一个"OpenAI 兼容"的语音识别服务，给 3DS 用

为什么要有它：
    云端那几家免费 ASR 全挂了（硅基流动 402、模力方舟要计费、Groq 注册被墙）。
    最省事的替代不是再找一家，而是**让你自己的电脑干这件事**：
    3DS 录 5 秒音频 → 局域网 POST 过来 → 电脑上跑识别 → 返回文字。
    不花钱、不依赖外网、音频不出门。

两种模式：
    --echo   不装任何模型，把收到的音频字节数回显成一句话。
             用来**先验证"3DS 能不能把音频发过来"**这件事。
    （默认） 用 faster-whisper 真识别。需要 pip install faster-whisper。

用法：
    python stt_server.py --echo
    python stt_server.py                 # 需要 faster-whisper
    python stt_server.py --model small --port 8000

3DS 那边配 asr.txt：
    provider=custom
    base_url=http://<这台电脑的IP>:8000
    （本地服务不需要 key，留空即可）

⚠️ 防火墙：第一次启动 Windows 会弹窗问要不要放行，要选**允许**。
   没弹窗就去「Windows 安全中心 → 防火墙 → 允许应用通过防火墙」里放行 python.exe。
"""

import argparse
import io
import json
import re
import struct
import sys
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

MODEL = None          # faster-whisper 的模型，--echo 模式下为 None


def now():
    return time.strftime("%H:%M:%S")


def wav_info(data: bytes):
    """尽力读出采样率/声道/位深 —— 只为了在日志里好认，读不出也不报错。"""
    if len(data) < 44 or data[:4] != b"RIFF" or data[8:12] != b"WAVE":
        return None
    try:
        i, fmt = 12, None
        while i + 8 <= len(data):
            cid = data[i:i + 4]
            size = struct.unpack("<I", data[i + 4:i + 8])[0]
            body = data[i + 8:i + 8 + size]
            if cid == b"fmt ":
                fmt = struct.unpack("<HHIIHH", body[:16])
            if cid == b"data":
                break
            i += 8 + size + (size & 1)
        if not fmt:
            return None
        audio_fmt, ch, rate = fmt[0], fmt[1], fmt[2]
        bits = fmt[5]
        return dict(format=audio_fmt, channels=ch, rate=rate, bits=bits,
                    seconds=None)
    except Exception:
        return None


def parse_multipart(body: bytes, ctype: str):
    """从 multipart/form-data 里把 name="file" 那一段捞出来。

    自己写而不是用 cgi（3.13 里已经删了）/ 第三方库：
    这里只需要"找出一个字段的字节"，不值得引依赖。
    """
    m = re.search(r'boundary="?([^";]+)"?', ctype or "")
    if not m:
        return None, None
    boundary = b"--" + m.group(1).encode()
    parts = body.split(boundary)

    for part in parts:
        if b'name="file"' not in part:
            continue
        head_end = part.find(b"\r\n\r\n")
        if head_end < 0:
            continue
        data = part[head_end + 4:]
        if data.endswith(b"\r\n"):
            data = data[:-2]
        fname = b""
        fm = re.search(rb'filename="([^"]*)"', part[:head_end])
        if fm:
            fname = fm.group(1)
        return data, fname
    return None, None


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    # 让日志好看一点，默认的 BaseHTTPRequestHandler 日志太吵
    def log_message(self, fmt, *args):
        pass

    def _send(self, code, obj):
        body = json.dumps(obj, ensure_ascii=False).encode()
        self.send_response(code)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        if self.path.rstrip("/") in ("/health", ""):
            self._send(200, {"ok": True, "mode": "echo" if MODEL is None else "whisper"})
        else:
            self._send(404, {"error": "not found"})

    def do_POST(self):
        if not self.path.startswith("/v1/audio/transcriptions"):
            self._send(404, {"error": "not found"})
            return

        n = int(self.headers.get("Content-Length") or 0)
        body = self.rfile.read(n) if n else b""
        ctype = self.headers.get("Content-Type", "")
        audio, fname = parse_multipart(body, ctype)

        t0 = time.time()
        if not audio:
            print("%s  ← %s  收到的不是 multipart（%d 字节）"
                  % (now(), self.client_address[0], len(body)))
            self._send(400, {"error": "no file part"})
            return

        info = wav_info(audio)
        print("%s  ← %s  %s  %d 字节%s"
              % (now(), self.client_address[0],
                 (fname or b"audio").decode(errors="ignore"),
                 len(audio),
                 ("  %d Hz / %d ch / %d bit" % (info["rate"], info["channels"], info["bits"]))
                 if info else ""))

        if MODEL is None:
            # 回声模式：不识别，只证明"音频真的到了、格式能读"
            txt = "收到 %d 字节音频%s" % (
                len(audio),
                ("（%d Hz）" % info["rate"]) if info else "")
        else:
            try:
                # faster-whisper 能吃文件对象；3DS 发的是 wav
                segs, _ = MODEL.transcribe(
                    io.BytesIO(audio),
                    language=None,          # 中英混说交给模型判断
                    vad_filter=True,        # 去掉静音段，5 秒短句效果明显
                    beam_size=1,
                )
                txt = "".join(s.text for s in segs).strip()
            except Exception as e:
                print("      识别失败：%s" % e)
                self._send(500, {"error": str(e)})
                return

        dt = (time.time() - t0) * 1000
        print("       → %r（%.0f ms）" % (txt[:60], dt))
        # 形状照 OpenAI：{"text": "..."}。3DS 那边按这个字段解析。
        self._send(200, {"text": txt})


def main():
    global MODEL
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=8000)
    ap.add_argument("--host", default="0.0.0.0",
                    help="0.0.0.0 = 局域网内都能连（默认）")
    ap.add_argument("--echo", action="store_true", help="回声模式，不加载模型")
    ap.add_argument("--model", default="small",
                    help="faster-whisper 模型：tiny/base/small/medium")
    args = ap.parse_args()

    if not args.echo:
        try:
            from faster_whisper import WhisperModel
        except ImportError:
            print("没装 faster-whisper。可以先跑 --echo 验证链路：")
            print("    pip install faster-whisper")
            sys.exit(2)
        print("加载模型 %s …（第一次会下载，几百 MB）" % args.model)
        MODEL = WhisperModel(args.model, device="cpu", compute_type="int8")
        print("模型就绪")

    srv = ThreadingHTTPServer((args.host, args.port), Handler)
    print("听在 http://%s:%d  模式=%s"
          % (args.host, args.port, "echo" if MODEL is None else args.model))
    print("3DS 的 asr.txt 写：provider=custom / base_url=http://<本机IP>:%d" % args.port)
    print("按 Ctrl+C 停止")
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        print("\n停了")


if __name__ == "__main__":
    main()
