#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""
pc_chat_test.py — 在 **PC 上**验证对话链路（不要用模拟器试）

══ 为什么要有这个脚本 ══════════════════════════════════════════════

对话功能要在 3DS 上验收，但**每次跑模拟器要 10 分钟**，
而一次「能不能连通」的问题在 PC 上 3 秒就能答。

而且有个**更硬的理由**��在 Azahar 上测 HTTPS 会给出**假阳性**：
它自带的 httpc 无条件关掉了证书校验，所以握手几乎必成；
真机上 curl+mbedtls 那条路才需要 CA bundle。
PC 上用系统证书库，行为更接近真机。

⚠️ 本脚本**只测网络与协议**，不测 UI。
   请求体字段与 shared/chatapi.c 的 build_request 保持一致
   （model / messages / stream / max_tokens），所以它通过就意味着
   「3DS 上那条路也通」—— TLS 实现不同而已。

用法：
    python tools/guia/pc_chat_test.py                     # 只测连通性（不要 key）
    ZHIPU_KEY=xxx python tools/guia/pc_chat_test.py zhipu   # 测真实对话
"""
import json
import os
import ssl
import sys
import time
import urllib.error
import urllib.request

# 与 shared/provider.c 逐条一致
PROVIDERS = {
    "deepseek": ("https://api.deepseek.com/v1",  "deepseek-chat"),
    "zhipu":    ("https://open.bigmodel.cn/api/paas/v4", "glm-4-flash"),
    "volc":     ("https://ark.cn-beijing.volces.com/api/v3", "doubao-pro"),
    "moonshot": ("https://api.moonshot.cn/v1",  "moonshot-v1-8k"),
    "dashscope": ("https://dashscope.aliyuncs.com/compatible-mode/v1", "qwen-plus"),
    "doubao":   ("https://www.doubao.com", ""),
}


def probe(name, timeout=8):
    """只测 TLS 握手 + 是否有证书问题。发一个空 POST 看状态码。"""
    url, model = PROVIDERS[name]
    req = urllib.request.Request(url + "/chat/completions",
                                 data=b"{}", method="POST")
    req.add_header("Content-Type", "application/json")
    t0 = time.time()
    try:
        with urllib.request.urlopen(req, timeout=timeout) as r:
            code, ms = r.status, (time.time() - t0) * 1000
        return "%-9s %3d  %6.0fms  %s" % (name, code, ms,
                                           "OK" if code < 500 else "服务端错")
    except urllib.error.HTTPError as e:
        code, ms = e.code, (time.time() - t0) * 1000
        # 4xx 是预期内的：说明**网络通、证书没问题**，只是没带 key
        return "%-9s %3d  %6.0fms  %s" % (name, code, ms,
                                           "网络通（缺鉴权）" if code in (401, 403)
                                           else "网络通")
    except Exception as e:
        ms = (time.time() - t0) * 1000
        return "%-9s ERR %6.0fms  %s: %s" % (name, ms, type(e).__name__, e)


def chat(name, key, user_text, timeout=40):
    """真实发一轮。请求体字段与 chatapi.c 的 build_request 对齐。"""
    url, model = PROVIDERS[name]
    body = json.dumps({
        "model": model,
        "messages": [{"role": "user", "content": user_text}],
        "stream": False,
        "max_tokens": 200,
    }).encode("utf-8")
    req = urllib.request.Request(url + "/chat/completions", data=body,
                                 method="POST")
    req.add_header("Content-Type", "application/json")
    req.add_header("Authorization", "Bearer " + key)
    t0 = time.time()
    try:
        with urllib.request.urlopen(req, timeout=timeout) as r:
            d = json.loads(r.read().decode("utf-8"))
            txt = d["choices"][0]["message"]["content"]
            usage = d.get("usage", {})
            return True, txt, (time.time() - t0), usage
    except urllib.error.HTTPError as e:
        detail = e.read().decode("utf-8", "replace")[:300]
        return False, "HTTP %d: %s" % (e.code, detail), time.time() - t0, {}
    except Exception as e:
        return False, "%s: %s" % (type(e).__name__, e), time.time() - t0, {}


def chat_multi(name, key, user_text, history, timeout=60):
    """带多轮上下文的请求 —— 与 3DS 上 chat_completion 的语义一致。"""
    url, model = PROVIDERS[name]
    msgs = [{"role": h["role"], "content": h["content"]} for h in history]
    msgs.append({"role": "user", "content": user_text})
    body = json.dumps({
        "model": model,
        "messages": msgs,
        "stream": False,
        "max_tokens": 400,
    }).encode("utf-8")
    req = urllib.request.Request(url + "/chat/completions", data=body,
                                 method="POST")
    req.add_header("Content-Type", "application/json")
    req.add_header("Authorization", "Bearer " + key)
    t0 = time.time()
    try:
        with urllib.request.urlopen(req, timeout=timeout) as r:
            d = json.loads(r.read().decode("utf-8"))
            txt = d["choices"][0]["message"]["content"]
            u = d.get("usage", {})
            usage = "tokens 总 %s" % u.get("total_tokens") if u else ""
            return True, txt, time.time() - t0, usage
    except urllib.error.HTTPError as e:
        detail = e.read().decode("utf-8", "replace")[:300]
        return False, "HTTP %d: %s" % (e.code, detail), time.time() - t0, ""
    except Exception as e:
        return False, "%s: %s" % (type(e).__name__, e), time.time() - t0, ""


def main():
    if len(sys.argv) > 1 and sys.argv[1] in PROVIDERS:
        name = sys.argv[1]
        env = {"zhipu": "ZHIPU_KEY", "deepseek": "DEEPSEEK_KEY",
               "moonshot": "MOONSHOT_KEY", "dashscope": "DASHSCOPE_KEY",
               "volc": "VOLC_KEY", "doubao": "DOUBAO_KEY"}
        key = os.environ.get(env[name], "")
        if not key:
            print("没给 key。设置环境变量 %s=<你的 API key> 再跑。" % env[name])
            return 1
        model = PROVIDERS[name][1]
        print("真实对话：%s / %s" % (name, model))
        print("（先问一轮固定问题确认链路，再进交互模式；空行退出）\n")

        # 先用固定问题确认链路 —— 因为要区分"key 错"与"模型不行"
        ok, txt, sec, usage = chat(name, key,
                                   "用一句话回答：3DS 的 homebrew 是什么？")
        print("  [自检] %.1fs" % sec)
        if not ok:
            print("  失败：%s" % txt)
            return 1
        print("  回复：%s" % txt[:200])
        if usage:
            print("  用量：%s" % usage)

        print("\n=== 交互模式（跟 3DS 上同样的多轮）===")
        turns = []
        while True:
            try:
                q = input("你> ").strip()
            except (EOFError, KeyboardInterrupt):
                print()
                return 0
            if not q:
                return 0
            ok, txt, sec, usage = chat_multi(name, key, q, turns)
            if not ok:
                print("AI> [失败] %s" % txt[:300])
                continue
            turns.append({"role": "user", "content": q})
            turns.append({"role": "assistant", "content": txt})
            print("AI> %s" % txt)
            print("     (%.1fs%s)" % (sec,
                                     "  %s" % usage if usage else ""))

    print("=== 连通性（不需要 key）===")
    print("判据：4xx = 网络与证书都通（只是没带鉴权）；"
          "SSL/timeout = 真有问题\n")
    for n in ("zhipu", "deepseek", "moonshot", "dashscope"):
        print("  " + probe(n))
    return 0


if __name__ == "__main__":
    sys.exit(main())
