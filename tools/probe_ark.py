#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""火山方舟（豆包官方 API）通道探测 —— 不需要真 Key 就能验的东西先验完。

思路
----
拿一把**假 Key** 去打真实端点。看服务端回什么：

  * 回 401「鉴权失败」  => 域名、端口、路径、协议、JSON 格式**全对**，
                          端到端只剩「换一把真 Key」这一件事。
  * 回 DNS/连接失败      => 网络层不通。
  * 回证书错误           => TLS 层挂了（3DS 老根证书的死法）。
  * 回 404               => 路径错了。
  * 回 400 且抱怨 model  => 端点对了，但 model 字段的写法不对。

顺带把 model 字段的两种写法各打一遍，看服务端怎么说 ——
方舟的 model 到底该填模型名还是推理接入点 ID，让服务端自己回答最准。

可选：真 Key 从环境变量 ARK_API_KEY 读进来，就再跑一次真请求。

用法：
    python tools/probe_ark.py
    ARK_API_KEY=xxxx python tools/probe_ark.py
"""

import json
import os
import sys
import urllib.error
import urllib.request

BASE = "https://ark.cn-beijing.volces.com/api/v3"
FAKE = "fake-key-for-probe-only"


def post(path, key, body, stream=False, timeout=40):
    url = BASE + path
    data = json.dumps(body, ensure_ascii=False).encode("utf-8")
    headers = {
        "Content-Type": "application/json",
        "Authorization": "Bearer " + key,
    }
    if stream:
        headers["Accept"] = "text/event-stream"
        headers["Cache-Control"] = "no-cache"
    req = urllib.request.Request(url, data=data, headers=headers,
                                 method="POST")
    try:
        with urllib.request.urlopen(req, timeout=timeout) as r:
            raw = r.read(2000).decode("utf-8", "replace")
            return True, r.status, raw, dict(r.headers)
    except urllib.error.HTTPError as e:
        raw = ""
        try:
            raw = e.read(2000).decode("utf-8", "replace")
        except Exception:
            pass
        return False, e.code, raw, dict(e.headers)
    except Exception as e:
        return False, 0, "%s: %s" % (type(e).__name__, e), {}


def shrink(s, n=420):
    s = (s or "").strip().replace("\n", " ")
    return s[:n] + (" ..." if len(s) > n else "")


def try_model(label, model, key):
    body = {
        "model": model,
        "messages": [{"role": "user", "content": "说一个字：好"}],
        "stream": False,
    }
    ok, code, raw, hdr = post("/chat/completions", key, body)
    print("  %-34s -> HTTP %s" % (label, code))
    print("      %s" % shrink(raw))
    return ok, code, raw


def main():
    print("=" * 70)
    print("火山方舟（豆包官方 API）通道探测")
    print("=" * 70)
    print("端点：%s/chat/completions" % BASE)

    real = os.environ.get("ARK_API_KEY", "").strip()

    print("\n" + "-" * 70)
    print("[1] 假 Key 打真实端点 —— 验的是「这条路修到哪了」")
    print("-" * 70)
    ok, code, raw = try_model("基准确认", "doubao-seed-1.6", FAKE)

    print("\n" + "-" * 70)
    print("[2] model 字段的几种写法（同一把假 Key，看服务端挑哪个毛病）")
    print("-" * 70)
    try_model("model=模型名 doubao-seed-1.6", "doubao-seed-1.6", FAKE)
    try_model("model=带日期 doubao-seed-1-6-250615",
              "doubao-seed-1-6-250615", FAKE)
    try_model("model=接入点 ep-fake-00000000-abcde",
              "ep-fake-00000000-abcde", FAKE)

    print("\n" + "-" * 70)
    print("[3] 路径对不对：故意打一个错路径做对照")
    print("-" * 70)
    ok4, code4, raw4, _ = post("/chat/completion", FAKE,      # 少个 s
                               {"model": "x", "messages": []})
    print("  /chat/completion (错路径) -> HTTP %s" % code4)
    print("      %s" % shrink(raw4))

    print("\n" + "-" * 70)
    print("[4] 流式(SSE)能不能开 —— 3DS 端默认走流式")
    print("-" * 70)
    body = {"model": "doubao-seed-1.6",
            "messages": [{"role": "user", "content": "说一个字：好"}],
            "stream": True}
    oks, codes, raws, hdrs = post("/chat/completions", FAKE, body, stream=True)
    print("  stream=true -> HTTP %s" % codes)
    print("      %s" % shrink(raws))

    print("\n" + "=" * 70)
    print("[1] 这一步的判读")
    print("=" * 70)
    if code == 401 or code == 403:
        print("  假 Key 被服务端**正确拒绝**了（HTTP %d）。" % code)
        print("  => 域名、TLS、路径、POST、JSON、鉴权头：全通。")
        print("  => 端到端只剩一件事：换一把真 Key。")
    elif code == 0:
        print("  连不上：%s" % shrink(raw))
        print("  => 网络层就没通，先解决网络。")
    elif code == 404:
        print("  404 —— 路径不对。")
    else:
        print("  HTTP %d，看上面返回体判断。" % code)

    if real:
        print("\n" + "=" * 70)
        print("[5] 真 Key 实测")
        print("=" * 70)
        b = {"model": os.environ.get("ARK_MODEL", "doubao-seed-1.6"),
             "messages": [{"role": "user", "content": "用一句话介绍你自己"}],
             "stream": False}
        o, c, r, _ = post("/chat/completions", real, b)
        print("  HTTP %s" % c)
        print("  %s" % shrink(r, 900))
        if c == 200:
            try:
                j = json.loads(r)
                print("\n  content: %s" %
                      j["choices"][0]["message"]["content"])
                print("  usage  : %s" % j.get("usage"))
            except Exception as e:
                print("  (解析不全：%s)" % e)
        else:
            print("\n  真 Key 也没成 —— 多半是 model 写法。")
            print("  方舟的 model 通常要填**推理接入点 ID**（ep- 开头），")
            print("  在控制台「在线推理」里创建接入点后才拿得到。")
    else:
        print("\n（没给真 Key。想跑真实请求：ARK_API_KEY=xxx python tools/probe_ark.py）")

    return 0


if __name__ == "__main__":
    sys.exit(main())
