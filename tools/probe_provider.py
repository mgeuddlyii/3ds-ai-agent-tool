#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""服务商通道体检：TLS 版本 + 真实请求两件事一起验。

为什么要测 TLS 版本
--------------------
libctru 的 sslc.h 里只有两个 TLS 选项：
    SSLCOPT_Default = 0        -> 用 TLSv1.1（3DS 的上限）
    SSLCOPT_TLSv10  = BIT(11)  -> 降到 TLSv1.0
**没有任何 TLS 1.2 的开关**。3dbrew 也是这么写的：
"highest supported TLS protocol version is v1.1"。

所以一个端点只要拒了 TLS1.0/1.1，3DS 真机在握手阶段就出局 ——
跟 Key、跟代码、跟 JSON 全都没关系，改软件也救不回来。

用法： python tools/probe_provider.py
可选： 带上真 Key 会顺带发一次真实请求（用和 chatapi.c 一样的请求体）
"""

import json
import os
import socket
import ssl
import sys
import urllib.error
import urllib.request

# 和 provider.c 保持一致
ENDPOINTS = [
    ("deepseek",  "https://api.deepseek.com/v1",
     "deepseek-chat"),
    ("zhipu",     "https://open.bigmodel.cn/api/paas/v4",
     "glm-4-flash"),
    ("ark",       "https://ark.cn-beijing.volces.com/api/v3",
     "doubao-seed-1.6"),
    ("moonshot",  "https://api.moonshot.cn/v1",
     "moonshot-v1-8k"),
    ("dashscope", "https://dashscope.aliyuncs.com/compatible-mode/v1",
     "qwen-plus"),
]

# 本地已存的 Key（logs/credbak），只为验证链路，用完不落盘
KEY_FILES = [
    r"E:\Gudyi\WORK5\3ds-ai\logs\credbak\keys.json",
    r"E:\Gudyi\WORK5\3ds-ai\logs\credbak\key.txt",
]


def tls_floor(host):
    """测端点最低接受哪个 TLS 版本。返回 (tls10, tls11, tls12)。"""
    out = {}
    for label, lo, hi in (("1.0", ssl.TLSVersion.TLSv1, ssl.TLSVersion.TLSv1),
                          ("1.1", ssl.TLSVersion.TLSv1_1, ssl.TLSVersion.TLSv1_1),
                          ("1.2", ssl.TLSVersion.TLSv1_2, ssl.TLSVersion.TLSv1_2)):
        c = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
        c.minimum_version = lo
        c.maximum_version = hi
        c.check_hostname = False
        c.verify_mode = ssl.CERT_NONE
        try:
            c.set_ciphers("DEFAULT:@SECLEVEL=0")
        except Exception:
            pass
        try:
            with socket.create_connection((host, 443), timeout=12) as s:
                with c.wrap_socket(s, server_hostname=host):
                    out[label] = True
        except Exception:
            out[label] = False
    return out


def load_keys():
    """从本地备份里读已有 Key。**只在验证时用，不写回任何地方。**"""
    got = {}
    for p in KEY_FILES:
        if not os.path.exists(p):
            continue
        try:
            d = open(p, encoding="utf-8").read()
        except Exception:
            continue
        if p.endswith(".json"):
            try:
                j = json.loads(d)
                for k in j.get("keys", []):
                    got[k.get("provider", "?")] = (k.get("key", ""),
                                                   k.get("model", ""))
            except Exception:
                pass
        else:
            prov = mdl = key = ""
            for line in d.splitlines():
                if line.startswith("provider="):
                    prov = line[9:].strip()
                elif line.startswith("model="):
                    mdl = line[6:].strip()
                elif line.startswith("key="):
                    key = line[4:].strip()
            if prov and key:
                got.setdefault(prov, (key, mdl))
    return got


def build_body(model, text, stream):
    """复刻 chatapi.c 的 build_request 输出，保证测的就是真请求体。"""
    return json.dumps({
        "model": model,
        "messages": [{"role": "user", "content": text}],
        "stream": stream,
    }, ensure_ascii=False).encode("utf-8")


def real_call(base, model, key, stream=False):
    url = base + "/chat/completions"
    headers = {"Content-Type": "application/json",
               "Authorization": "Bearer " + key}
    if stream:
        headers["Accept"] = "text/event-stream"
        headers["Cache-Control"] = "no-cache"
    req = urllib.request.Request(url,
                                 data=build_body(model, "只回复一个字：好",
                                                 stream),
                                 headers=headers, method="POST")
    try:
        with urllib.request.urlopen(req, timeout=45) as r:
            return r.status, r.read(1200).decode("utf-8", "replace")
    except urllib.error.HTTPError as e:
        raw = ""
        try:
            raw = e.read(1200).decode("utf-8", "replace")
        except Exception:
            pass
        return e.code, raw
    except Exception as e:
        return 0, "%s: %s" % (type(e).__name__, e)


def main():
    print("=" * 74)
    print("服务商通道体检 —— 3DS 真机的硬门槛是 TLS 版本")
    print("=" * 74)
    print("3DS sslc.h 只有 SSLCOPT_Default(TLS1.1) 和 SSLCOPT_TLSv10，没有 TLS1.2。")
    print("端点若拒了 1.0/1.1，真机握手阶段就出局。\n")

    print("%-10s %-8s %-8s %-8s %s" % ("服务商", "TLS1.0", "TLS1.1", "TLS1.2",
                                       "3DS真机判定"))
    print("-" * 74)
    verdict = {}
    for name, base, mdl in ENDPOINTS:
        host = base.split("//")[1].split("/")[0]
        t = tls_floor(host)
        ok3ds = t.get("1.1", False) or t.get("1.0", False)
        verdict[name] = ok3ds
        print("%-10s %-8s %-8s %-8s %s" % (
            name,
            "OK" if t.get("1.0") else "拒",
            "OK" if t.get("1.1") else "拒",
            "OK" if t.get("1.2") else "拒",
            "有戏" if ok3ds else "**握手就死**"))
    print()

    keys = load_keys()
    print("=" * 74)
    print("用本地已有 Key 做真实请求（复刻 chatapi.c 的请求体）")
    print("=" * 74)
    if not keys:
        print("没读到本地 Key。")
    for prov, (key, mdl) in keys.items():
        base = None
        for n, b, m in ENDPOINTS:
            if n == prov:
                base, mdl = b, (mdl or m)
        if not base:
            continue
        print("\n--- %s  model=%s" % (prov, mdl))
        print("    非流式 ...")
        code, raw = real_call(base, mdl, key, stream=False)
        print("    HTTP %s" % code)
        print("    %s" % raw.strip().replace("\n", " ")[:300])
        if code == 200:
            try:
                j = json.loads(raw)
                print("    content: %s" % j["choices"][0]["message"]["content"])
            except Exception:
                pass
        print("    流式(SSE) ...")
        code2, raw2 = real_call(base, mdl, key, stream=True)
        print("    HTTP %s" % code2)
        print("    %s" % raw2.strip().replace("\n", " | ")[:300])

    print("\n" + "=" * 74)
    print("结论速览")
    print("=" * 74)
    for name, base, mdl in ENDPOINTS:
        print("  %-10s %s" % (name, "真机可能连上" if verdict.get(name)
                              else "真机连不上（TLS 版本不够）"))
    return 0


if __name__ == "__main__":
    sys.exit(main())
