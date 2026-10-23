#!/usr/bin/env python3
"""Probe Doubao's chat + history endpoints WITH the QR-login session.

This is the decisive test. doubao_selfprobe.py already showed the gateway
lets these requests through while logged out; what it could not show is
whether they return real data once a session exists. Now that
tools/doubao_session.json holds a real sessionid, this answers it:

  * chat returns 200 with assistant text  -> 3DS can chat directly
  * history returns 200 with conversations -> 3DS can show the phone's history

Nothing here uses a_bogus; only the x-flow-trace header, which is two random
uuids.

Usage:
    python tools/doubao_probe_auth.py
    python tools/doubao_probe_auth.py -v
"""

import argparse
import json
import os
import random
import re
import ssl
import string
import sys
import time
import urllib.error
import urllib.parse
import urllib.request
import uuid

BASE = "https://www.doubao.com"
AID = "497858"
UA = ("Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
      "(KHTML, like Gecko) Chrome/148.0.0.0 Safari/537.36")

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
SESSION = os.path.join(ROOT, "tools", "doubao_session.json")
CTX = ssl.create_default_context()


def rand(n):
    return "".join(random.choice(string.digits) for _ in range(n))


def trace():
    """The whole 'signature': two random uuids. Not a signature."""
    return json.dumps({"trace_id": uuid.uuid4().hex,
                       "span_id": uuid.uuid4().hex})


def req(url, body=None, cookie="", timeout=50, stream=False):
    data = None
    method = "GET"
    if body is not None:
        data = (body if isinstance(body, bytes)
                else json.dumps(body, ensure_ascii=False).encode("utf-8"))
        method = "POST"
    h = {
        "User-Agent": UA,
        "Accept": "text/event-stream" if stream else "application/json, text/plain, */*",
        "Accept-Language": "zh-CN,zh;q=0.9",
        "Content-Type": "application/json",
        "Origin": BASE,
        "Referer": BASE + "/chat/",
        "agw-js-conv": "str",
        "x-flow-trace": trace(),
    }
    if cookie:
        h["Cookie"] = cookie
    r = urllib.request.Request(url, data=data, method=method, headers=h)
    try:
        with urllib.request.urlopen(r, timeout=timeout, context=CTX) as resp:
            return resp.status, resp.read()
    except urllib.error.HTTPError as e:
        return e.code, e.read()
    except Exception as e:
        return -1, ("EXC %s: %s" % (type(e).__name__, e)).encode()


def qs(dev):
    return "&".join([
        "aid=" + AID,
        "device_id=" + dev["device_id"],
        "device_platform=web",
        "language=zh",
        "pc_version=3.17.3",
        "pkg_type=release_version",
        "real_aid=" + AID,
        "region=CN",
        "samantha_web=1",
        "sys_region=CN",
        "tea_uuid=" + dev["tea_uuid"],
        "use-olympus-account=1",
        "version_code=20800",
        "web_id=" + dev["web_id"],
    ])


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("-v", "--verbose", action="store_true")
    args = ap.parse_args()

    if not os.path.exists(SESSION):
        print("no session file. run: python tools/qr_now.py --loop")
        return 1
    sess = json.load(open(SESSION, encoding="utf-8"))
    ck = sess["cookies"]
    cookie = sess.get("cookie_header") or "; ".join(
        "%s=%s" % (k, v) for k, v in ck.items())
    print("session loaded: %d cookies, sessionid=%s...\n"
          % (len(ck), ck.get("sessionid", "")[:10]))

    # A logged-in homepage visit refreshes web_id and confirms the session.
    st, body = req(BASE + "/chat/", cookie=cookie)
    html = body.decode("utf-8", "replace")
    print("GET /chat/           -> http=%s  %d bytes" % (st, len(body)))
    dev = {"device_id": rand(19), "tea_uuid": rand(19), "web_id": ""}
    m = re.search(r'["\']web_id["\']\s*[:=]\s*["\'](\d{10,25})["\']', html)
    if m:
        dev["web_id"] = m.group(1)
    if not dev["web_id"]:
        dev["web_id"] = ck.get("web_id") or rand(19)
    print("device params (self-generated, server does not validate them):")
    for k, v in dev.items():
        print("    %-10s = %s" % (k, v))
    print("logged-in marker in HTML: %s"
          % ("YES" if '"is_login":true' in html.replace(" ", "")
             or "isLogin" in html else "not found (page is client-rendered)"))

    q = qs(dev)
    results = {}

    print("\n=== 1) chat (non-streaming) ===")
    # Field-for-field the shape doubao-free-api sends. The first attempt
    # omitted bot_id / completion_option / conversation_id and the server
    # answered 710020702 "系统错误" - it validates the body, not a signature.
    chat_body = {
        "bot_id": "7338286299411103781",
        "completion_option": {
            "is_regen": False,
            "with_suggest": False,
            "need_create_conversation": True,
            "launch_stage": 1,
            "use_auto_cot": False,
            "use_deep_think": False,
        },
        "conversation_id": "0",
        "local_conversation_id": "local_%d" % int(time.time()),
        "local_message_id": str(uuid.uuid4()),
        "messages": [{
            "content": json.dumps({"text": "用一句话回答：1+1等于几？"},
                                  ensure_ascii=False),
            "content_type": 2001,
            "attachments": [],
            "references": [],
        }],
        "ext": {},
    }
    st, b = req(BASE + "/samantha/chat/completion?" + q, chat_body, cookie)
    txt = b.decode("utf-8", "replace")
    results["chat"] = st
    print("  http=%s  len=%d" % (st, len(b)))
    print("  %s" % txt[:600].replace("\n", " "))
    # Pull the assistant text straight out of the SSE frames.
    for m in re.finditer(r'data:\s*(\{.*?\})(?=\s*data:|\s*$)', txt, re.S):
        try:
            outer = json.loads(m.group(1))
        except Exception:
            continue
        inner = outer.get("event_data", "")
        if isinstance(inner, str) and inner:
            try:
                inner = json.loads(inner)
            except Exception:
                inner = {}
        msg = (inner or {}).get("message", {})
        c = msg.get("content", "")
        if c:
            try:
                c = json.loads(c).get("text", c)
            except Exception:
                pass
            print("  >> %s" % c[:300].replace("\n", " "))
            break
    if args.verbose:
        print("  FULL: %s" % txt[:4000])

    print("\n=== 2) history: samantha/thread/list ===")
    st, b = req(BASE + "/samantha/thread/list?" + q,
                {"cursor": 0, "count": 20}, cookie)
    txt = b.decode("utf-8", "replace")
    results["thread/list"] = st
    print("  http=%s  len=%d" % (st, len(b)))
    print("  %s" % txt[:600].replace("\n", " "))

    print("\n=== 3) history: im/chain/recent_conv ===")
    st, b = req(BASE + "/im/chain/recent_conv?" + q,
                {"cmd": 3200, "direction": 2, "count": 20}, cookie)
    txt = b.decode("utf-8", "replace")
    results["recent_conv"] = st
    print("  http=%s  len=%d" % (st, len(b)))
    print("  %s" % txt[:600].replace("\n", " "))

    print("\n=== 4) history: samantha/conversation/list ===")
    st, b = req(BASE + "/samantha/conversation/list?" + q,
                {"cursor": 0, "count": 20}, cookie)
    txt = b.decode("utf-8", "replace")
    results["conversation/list"] = st
    print("  http=%s  len=%d" % (st, len(b)))
    print("  %s" % txt[:600].replace("\n", " "))

    print("\n=== summary ===")
    for k, st in results.items():
        print("  %-22s http=%s" % (k, st))
    ok = [k for k, s in results.items() if s == 200]
    print("\n  200 responses: %s" % (", ".join(ok) if ok else "(none)"))
    return 0


if __name__ == "__main__":
    sys.exit(main())
