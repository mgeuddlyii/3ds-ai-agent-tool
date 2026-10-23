#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
api_selftest.py —— 用真实 API 跑一次最小请求，验证「Agent 能不能连上模型」

── 为什么单独做这一步 ──
3DS 上的 AI 能不能用，取决于一条很长的链路：
    键盘/语音 → 拼 JSON → TLS → 服务商 → 流式解析 → 上屏
其中**任何一环**坏掉，症状都是"没有回复"，从表面分不清是哪一层。
这个脚本把最后两环（网络 + 服务商 + 解析）在 PC 上单独跑一次，
先确认"API 这把钥匙是好的、网络是通的、返回格式没变"。

确认之后，3DS 上如果还是没回复，问题就一定在**前面几环**，
不用再怀疑 Key 或网络。

── 它做什么 ──
1. 从密钥文件里挑一把 Key（按标签选，默认 3ds）
2. 打一次最小 chat/completions 请求
3. 把回复原文写进 out/api_selftest.txt
4. 打印结构化的诊断（**绝不打印 Key 本身**）

── 用法 ──
    python tools/api_selftest.py                    # 用默认 Key 文件 + deepseek
    python tools/api_selftest.py --provider glm     # 换一家
    python tools/api_selftest.py --model deepseek-chat --prompt "你好"
"""

import argparse
import io
import json
import os
import re
import ssl
import sys
import time
import urllib.error
import urllib.request

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT_DIR = os.path.join(ROOT, "out")

# 密钥文件：一行 Key、下一行标签（"3ds" / "桌面" 之类）。
# 路径来自项目 memory：唯一落点是 SD 卡，PC 侧的母本是这个文件。
KEY_FILES = [
    r"E:\Gudyi\密码\deepseek api.txt",
    r"E:\Gudyi\密码\glm.txt",
]

# 服务商表：与 shared/provider.h 的编译期地址保持一致（同源，别各写一份）
PROVIDERS = {
    "deepseek": {
        "url": "https://api.deepseek.com/v1/chat/completions",
        "model": "deepseek-chat",
        "keyfile": KEY_FILES[0],
    },
    "glm": {
        "url": "https://open.bigmodel.cn/api/paas/v4/chat/completions",
        "model": "glm-4-flash",
        "keyfile": KEY_FILES[1],
    },
}


def mask(s):
    """任何要打印的 Key 都必须先过这里。"""
    if not s:
        return "(空)"
    if len(s) <= 8:
        return "*" * len(s)
    return s[:4] + "…" + s[-3:] + "(len=%d)" % len(s)


def load_keys(path):
    """解析「一行 Key、一行标签」的格式。返回 [(key, label)]。"""
    if not os.path.exists(path):
        return []
    text = io.open(path, encoding="utf-8", errors="replace").read()
    lines = [l.strip() for l in text.splitlines() if l.strip()]
    out = []
    i = 0
    while i < len(lines):
        ln = lines[i]
        if ln.startswith("sk-") or re.fullmatch(r"[A-Za-z0-9._\-]{20,}", ln):
            label = lines[i + 1] if i + 1 < len(lines) and not lines[i + 1].startswith("sk-") else ""
            out.append((ln, label))
            i += 2 if label else 1
        else:
            i += 1
    return out


def pick_key(path, want_label):
    keys = load_keys(path)
    if not keys:
        return None, "文件里没找到形如 sk-... 的 Key：%s" % path
    for k, lab in keys:
        if want_label and want_label in lab:
            return k, "按标签 %r 选中 %s" % (want_label, mask(k))
    k, lab = keys[0]
    return k, "没有标签 %r，退回第一条 %s（标签=%r）" % (want_label, mask(k), lab)


def call_api(url, key, model, prompt, timeout=60):
    body = json.dumps({
        "model": model,
        "messages": [{"role": "user", "content": prompt}],
        "stream": False,
        "max_tokens": 300,
    }).encode("utf-8")

    req = urllib.request.Request(url, data=body, method="POST")
    req.add_header("Content-Type", "application/json")
    req.add_header("Authorization", "Bearer " + key)

    # 显式用系统信任库（本机卡巴斯基做 HTTPS 中间人，Python 走系统根证书是对的；
    # 若这里失败，说明是证书链问题，不是 Key 问题 —— 两者的报错完全不同）。
    ctx = ssl.create_default_context()

    t0 = time.time()
    with urllib.request.urlopen(req, timeout=timeout, context=ctx) as r:
        raw = r.read()
        dt = time.time() - t0
        return r.status, raw, dt


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--provider", default="deepseek", choices=list(PROVIDERS))
    ap.add_argument("--model", default=None)
    ap.add_argument("--label", default="3ds", help="密钥文件里的标签，默认 3ds")
    ap.add_argument("--prompt", default="用一句中文说明你是什么模型，不要超过三十个字。")
    args = ap.parse_args()

    pv = PROVIDERS[args.provider]
    model = args.model or pv["model"]

    print("== API 自检 ==")
    print("  服务商 : %s" % args.provider)
    print("  地址   : %s" % pv["url"])
    print("  模型   : %s" % model)

    key, msg = pick_key(pv["keyfile"], args.label)
    print("  密钥   : %s" % msg)
    if not key:
        print("!! 没有可用 Key，自检无法进行")
        return 1

    try:
        status, raw, dt = call_api(pv["url"], key, model, args.prompt)
    except urllib.error.HTTPError as e:
        detail = ""
        try:
            detail = e.read().decode("utf-8", "replace")[:400]
        except Exception:
            pass
        print("!! HTTP %s —— 服务端拒绝了请求" % e.code)
        print("   响应：%s" % detail.replace("\n", " "))
        print("   判读：401/402/403 多为 Key 无效或余额不足；429 是限流；")
        print("         400 通常是 model 名不对。")
        return 1
    except ssl.SSLError as e:
        print("!! TLS 握手失败：%s" % e)
        print("   判读：这是证书链问题（本机卡巴斯基做 HTTPS 中间人），**不是 Key 问题**。")
        return 1
    except urllib.error.URLError as e:
        print("!! 连不上：%s" % e.reason)
        print("   判读：DNS / 网络 / 代理问题，也不是 Key 问题。")
        return 1

    print("  HTTP   : %s（%.2f 秒，%d 字节）" % (status, dt, len(raw)))

    # 解析：只要能取到 content 就算通
    try:
        obj = json.loads(raw.decode("utf-8"))
    except Exception as e:
        print("!! 返回不是合法 JSON：%s" % e)
        print("   前 300 字节：%r" % raw[:300])
        return 1

    content = ""
    usage = obj.get("usage") or {}
    try:
        content = obj["choices"][0]["message"]["content"]
    except Exception:
        print("!! 返回里没有 choices[0].message.content")
        print("   顶层键：%s" % list(obj.keys()))
        return 1

    print("  usage  : prompt=%s completion=%s total=%s"
          % (usage.get("prompt_tokens", "?"),
             usage.get("completion_tokens", "?"),
             usage.get("total_tokens", "?")))
    print("  回复   : %s" % content.strip().replace("\n", " ")[:120])

    # ---- 落盘 ----
    os.makedirs(OUT_DIR, exist_ok=True)
    stamp = time.strftime("%Y-%m-%d %H:%M:%S")
    out = os.path.join(OUT_DIR, "api_selftest.txt")
    with io.open(out, "w", encoding="utf-8") as f:
        f.write("API 自检结果\n")
        f.write("=" * 40 + "\n")
        f.write("时间     : %s\n" % stamp)
        f.write("服务商   : %s\n" % args.provider)
        f.write("地址     : %s\n" % pv["url"])
        f.write("模型     : %s\n" % model)
        f.write("Key      : %s（已脱敏，不落盘）\n" % mask(key))
        f.write("HTTP     : %s\n" % status)
        f.write("耗时     : %.2f 秒\n" % dt)
        f.write("usage    : prompt=%s completion=%s total=%s\n"
                % (usage.get("prompt_tokens", "?"),
                   usage.get("completion_tokens", "?"),
                   usage.get("total_tokens", "?")))
        f.write("-" * 40 + "\n")
        f.write("提问：%s\n\n" % args.prompt)
        f.write("回复：\n%s\n" % content)
    print("  落盘   : %s" % out)
    print("\n== 自检通过：Key 有效、网络可达、返回格式未变 ==")
    return 0


if __name__ == "__main__":
    sys.exit(main())
