#!/usr/bin/env python3
"""A/B 验证豆包的模式开关：联网搜索 与 agent(use_auto_cot)。

目标配置（本项目要用的）：
    ext = {"use_search": "1"}          -> 开联网搜索
    completion_option.use_auto_cot = False   -> 关 agent（不自主多步/不写代码执行）
    completion_option.use_deep_think = False -> 关深度思考

用法:
    python tools/doubao_modeprobe.py            # 跑三组对照
    python tools/doubao_modeprobe.py --verbose  # 打印原始 SSE
"""

import argparse
import json
import os
import random
import re
import ssl
import sys
import time
import urllib.error
import urllib.request
import uuid

HERE = os.path.dirname(os.path.abspath(__file__))
SESSION = os.path.join(HERE, "doubao_session.json")

UA = ("Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
      "(KHTML, like Gecko) Chrome/135.0.0.0 Safari/537.36")
BASE = "https://www.doubao.com"
CTX = ssl.create_default_context()

# A question that can only be answered from live data, so a search-capable
# config and a non-search one should look visibly different in the SSE stream.
QUESTION = "2026年诺贝尔物理学奖得主是谁？如果不知道就明确说不知道。"

SEARCH_HINTS = ("search", "search_result", "reference", "citation",
                "web_search", "plugin", "工具", "搜索")


def req(url, body, cookie, extra=None):
    h = {
        "content-type": "application/json",
        "cookie": cookie,
        "origin": BASE,
        "referer": BASE + "/chat/",
        "user-agent": UA,
        "x-flow-trace": json.dumps({"trace_id": uuid.uuid4().hex,
                                    "span_id": uuid.uuid4().hex}),
        "agw-js-conv": "str",
    }
    if extra:
        h.update(extra)
    r = urllib.request.Request(url, data=json.dumps(body).encode(),
                               headers=h, method="POST")
    try:
        with urllib.request.urlopen(r, timeout=90, context=CTX) as resp:
            return resp.status, resp.read()
    except urllib.error.HTTPError as e:
        return e.code, e.read()
    except Exception as e:
        return -1, ("EXC %s: %s" % (type(e).__name__, e)).encode()


def build(use_search, use_auto_cot, use_deep_think, dev, cid="0"):
    ext = {}
    if use_search:
        ext["use_search"] = "1"
    if use_deep_think:
        ext["use_deep_think"] = "1"
    return {
        "bot_id": "7338286299411103781",
        "completion_option": {
            "is_regen": False,
            "with_suggest": False,
            "need_create_conversation": cid == "0",
            "launch_stage": 1,
            "use_auto_cot": use_auto_cot,
            "use_deep_think": use_deep_think,
        },
        "conversation_id": cid,
        "local_conversation_id": "local_%d" % int(time.time()),
        "local_message_id": str(uuid.uuid4()),
        "messages": [{
            "content": json.dumps({"text": QUESTION}, ensure_ascii=False),
            "content_type": 2001,
            "attachments": [],
            "references": [],
        }],
        "ext": ext,
    }


def analyse(txt):
    """Pull out the answer text and any sign that a web search happened."""
    text = ""
    events = set()
    for frame in txt.replace("\r\n", "\n").split("\n"):
        if not frame.startswith("data:"):
            continue
        try:
            outer = json.loads(frame[5:].strip())
        except Exception:
            continue
        events.add(str(outer.get("event_type")))
        ev = outer.get("event_data", "")
        if isinstance(ev, str) and ev:
            try:
                ev = json.loads(ev)
            except Exception:
                ev = {}
        if not isinstance(ev, dict):
            continue
        msg = ev.get("message", {})
        if isinstance(msg, dict):
            c = msg.get("content", "")
            if isinstance(c, str) and c:
                try:
                    c = json.loads(c).get("text", c)
                except Exception:
                    pass
                if c:
                    text += c
    searchy = [h for h in SEARCH_HINTS if re.search(h, txt, re.I)]
    return text, sorted(events), searchy


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--verbose", action="store_true")
    ap.add_argument("--only", default="", help="只跑某一组: A / B / C")
    ap.add_argument("--gap", type=int, default=0, help="每组之间等待秒数")
    args = ap.parse_args()

    if not os.path.exists(SESSION):
        print("no session. run: python tools/qr_now.py --loop")
        return 1
    s = json.load(open(SESSION, encoding="utf-8"))
    cookie = s["cookie_header"]

    # A logged-in homepage visit yields a real web_id.
    st, raw = req(BASE + "/chat/", None, cookie) if False else (200, b"")
    r = urllib.request.Request(BASE + "/chat/", headers={"user-agent": UA,
                                                         "cookie": cookie})
    with urllib.request.urlopen(r, timeout=60, context=CTX) as resp:
        html = resp.read().decode("utf-8", "replace")
    m = re.search(r'["\']web_id["\']\s*[:=]\s*["\'](\d{10,25})["\']', html)
    web_id = m.group(1) if m else str(random.randint(10 ** 18, 10 ** 19 - 1))
    dev = {"device_id": str(random.randint(10 ** 18, 10 ** 19 - 1)),
           "tea_uuid": str(random.randint(10 ** 18, 10 ** 19 - 1)),
           "web_id": web_id}
    q = "&".join([
        "aid=497858", "device_id=" + dev["device_id"], "device_platform=web",
        "language=zh", "pc_version=3.17.3", "pkg_type=release_version",
        "real_aid=497858", "region=CN", "samantha_web=1", "sys_region=CN",
        "tea_uuid=" + dev["tea_uuid"], "use-olympus-account=1",
        "version_code=20800", "web_id=" + dev["web_id"],
    ])
    url = BASE + "/samantha/chat/completion?" + q

    only = args.only.upper()
    all_cases = [
        ("A  纯聊天（都不开）", build(False, False, False, dev)),
        ("B  联网开 + agent 关  ← 我们要的", build(True, False, False, dev)),
        ("C  联网开 + agent 开（专家模式）", build(True, True, True, dev)),
    ]
    cases = ([c for c in all_cases if c[0].startswith(only)]
             if only in ("A", "B", "C") else all_cases)
    if args.gap and len(cases) > 1:
        print("(间隔 %ds 以防触发限流)" % args.gap)
    print("问题: %s\n" % QUESTION)
    for name, body in cases:
        st, raw = req(url, body, cookie)
        txt = raw.decode("utf-8", "replace")
        text, events, searchy = analyse(txt)
        print("=" * 64)
        print("%s   http=%s  %d bytes" % (name, st, len(raw)))
        print("  event_type: %s" % ",".join(events))
        print("  搜索痕迹  : %s" % (",".join(searchy) if searchy else "无"))
        print("  回答      : %s" % (text[:220].replace("\n", " ") or "(空)"))
        if args.verbose:
            print("  RAW: %s" % txt[:1200])
        if args.gap:
            time.sleep(args.gap)
    print("=" * 64)
    print("\n判读：B 组应当有搜索痕迹而 C 组更多事件类型；"
          "A 组应最少。差别不明显则说明服务端对这几个开关不敏感。")
    return 0


if __name__ == "__main__":
    sys.exit(main())
