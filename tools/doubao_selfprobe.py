"""Self-contained Doubao probe that needs NO login and NO browser.

Why this exists
---------------
The whole "can a 3DS talk to Doubao without a server?" question reduces to
one thing: does the B3 bypass header actually get past the signature check?

  * If the server answers "signature missing / forbidden / risk"  -> signed
    endpoints, a_bogus is required, 3DS cannot do it alone.
  * If the server answers "not logged in / login required"        -> the
    request was ACCEPTED at the gateway and only the session is missing.
    Since QR login is plain HTTP (already proven), nothing else is needed.

That distinction is decidable while logged OUT, so this script decides it.

It also tries to harvest the device parameters (device_id / tea_uuid /
web_id / fp) from an anonymous homepage visit, because B3 needs them in the
query string. If they are obtainable anonymously, the 3DS can build the
whole request by itself.

Usage
-----
    python tools/doubao_selfprobe.py              # full run
    python tools/doubao_selfprobe.py -v           # print response bodies

No credentials are read or written by this script.
"""

import argparse
import http.cookiejar
import json
import os
import re
import sys
import time
import urllib.error
import urllib.parse
import urllib.request
import uuid

BASE = "https://www.doubao.com"
AID = "497858"
UA = ("Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
      "(KHTML, like Gecko) Chrome/135.0.0.0 Safari/537.36")

# Words that mean "your request never got past the anti-bot layer".
SIGN_WORDS = [
    "a_bogus", "abogus", "x-bogus", "signature", "签名", "verify", "captcha",
    "滑动", "risk", "风控", "forbidden", "403", "非法", "invalid request",
]
# Words that mean "the gateway accepted you, you are just not logged in".
LOGIN_WORDS = [
    "login", "登录", "未登录", "请登录", "unauthorized", "session",
    "passport", "not_login", "user not login", "no login",
]


def x_flow_trace():
    """B3 bypass header - a plain random value, NOT a signature.

    Confirmed from doubao-free-api sse.py:
        {"trace_id": uuid4().hex, "span_id": uuid4().hex}
    """
    return json.dumps({"trace_id": uuid.uuid4().hex,
                       "span_id": uuid.uuid4().hex})


class Net:
    def __init__(self):
        self.cj = http.cookiejar.CookieJar()
        self.op = urllib.request.build_opener(
            urllib.request.HTTPCookieProcessor(self.cj))
        self.last_set_cookie = []

    def _h(self, extra):
        h = {"User-Agent": UA,
             "Accept": "text/html,application/xhtml+xml,*/*",
             "Accept-Language": "zh-CN,zh;q=0.9"}
        if extra:
            h.update(extra)
        return h

    def req(self, url, body=None, headers=None, timeout=40):
        data = None
        method = "GET"
        if body is not None:
            if isinstance(body, (dict, list)):
                data = json.dumps(body, ensure_ascii=False).encode("utf-8")
            else:
                data = body.encode("utf-8")
            method = "POST"
        req = urllib.request.Request(url, data=data, method=method,
                                     headers=self._h(headers))
        try:
            with self.op.open(req, timeout=timeout) as r:
                sc = r.headers.get_all("Set-Cookie") or []
                self.last_set_cookie = sc
                return r.status, dict(r.headers), r.read()
        except urllib.error.HTTPError as e:
            sc = e.headers.get_all("Set-Cookie") or []
            self.last_set_cookie = sc
            return e.code, dict(e.headers), e.read()
        except Exception as e:
            return -1, {}, ("EXC %s: %s" % (type(e).__name__, e)).encode()

    def cookies(self):
        return {c.name: c.value for c in self.cj}


def harvest_device(net, verbose):
    """Visit the homepage anonymously and look for device parameters."""
    print("=== step 1: anonymous homepage ===")
    st, hdr, body = net.req(BASE + "/")
    html = body.decode("utf-8", "replace")
    print("  GET /            -> http=%s  %d bytes" % (st, len(body)))

    cks = net.cookies()
    print("  cookies set (%d): %s"
          % (len(cks), ", ".join(sorted(cks))[:120]))

    # Device params are usually rendered into the SSR payload / inline JS.
    found = {}
    for key in ("device_id", "tea_uuid", "web_id", "fp", "msToken",
                "room_id", "user_unique_id", "bd_ticket"):
        m = re.search(r'["\']%s["\']\s*[:=]\s*["\']([^"\']{4,80})["\']' % key,
                      html)
        if m:
            found[key] = m.group(1)
    # fallback: they may also sit in cookie values
    for key in ("device_id", "tea_uuid", "web_id", "msToken"):
        if key not in found and key in cks:
            found[key] = cks[key]

    if found:
        print("  device params harvested from HTML/cookies:")
        for k, v in sorted(found.items()):
            print("      %-14s = %s" % (k, v[:48]))
    else:
        print("  !! no device params found in HTML")
        if verbose:
            print(html[:1500])
    return found, cks


def probe_endpoint(net, dev, label, url_tpl, body, verbose):
    """Fire a B3-style request and classify the refusal."""
    qs = "&".join([
        "aid=" + AID,
        "device_id=" + dev.get("device_id", ""),
        "device_platform=web",
        "language=zh",
        "pc_version=3.17.3",
        "pkg_type=release_version",
        "real_aid=" + AID,
        "region=CN",
        "samantha_web=1",
        "sys_region=CN",
        "tea_uuid=" + dev.get("tea_uuid", ""),
        "use-olympus-account=1",
        "version_code=20800",
        "web_id=" + dev.get("web_id", ""),
    ])
    url = url_tpl + "?" + qs

    cookie = "; ".join("%s=%s" % (k, v) for k, v in net.cookies().items())
    h = {
        "content-type": "application/json",
        "accept": "text/event-stream",
        "agw-js-conv": "str",
        "cookie": cookie,
        "origin": BASE,
        "referer": BASE + "/chat/",
        "user-agent": UA,
        "x-flow-trace": x_flow_trace(),
    }
    st, hdr, b = net.req(url, body, h, timeout=40)
    txt = b.decode("utf-8", "replace")
    low = txt.lower()

    hit_sign = [w for w in SIGN_WORDS if w.lower() in low]
    hit_login = [w for w in LOGIN_WORDS if w.lower() in low]

    if st == 200:
        verdict = "ACCEPTED"
    elif hit_sign and not hit_login:
        verdict = "BLOCKED-BY-SIGNATURE"
    elif hit_login:
        verdict = "ACCEPTED-BUT-NEEDS-LOGIN"
    else:
        verdict = "OTHER"

    print("  [%-28s] http=%-4s %-24s len=%d"
          % (label, st, verdict, len(b)))
    if hit_sign:
        print("        sign-words:   %s" % ", ".join(hit_sign[:6]))
    if hit_login:
        print("        login-words:  %s" % ", ".join(hit_login[:6]))
    if verbose or verdict in ("OTHER", "ACCEPTED"):
        print("        %s" % txt[:400].replace("\n", " "))
    return verdict, st, txt


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("-v", "--verbose", action="store_true")
    args = ap.parse_args()

    net = Net()
    dev, cks = harvest_device(net, args.verbose)

    # Even without real device params we can still classify the refusal,
    # which is the whole point of this probe.
    if not dev.get("device_id"):
        print("  (using placeholder device params - refusal class still valid)")
        dev.setdefault("device_id", "0000000000")
        dev.setdefault("tea_uuid", "0000000000")
        dev.setdefault("web_id", "0000000000")

    chat_body = {
        "messages": [{
            "content": json.dumps({"text": "你好"}, ensure_ascii=False),
            "content_type": 2001,
            "attachments": [],
            "references": [],
        }],
        "local_conversation_id": "local_%d" % int(time.time()),
        "stream": False,
    }

    print("\n=== step 2: B3-style requests, logged OUT ===")
    print("(x-flow-trace generated locally, no a_bogus anywhere)\n")

    results = {}

    print("--- chat ---")
    v, st, txt = probe_endpoint(
        net, dev, "samantha/chat/completion",
        BASE + "/samantha/chat/completion", chat_body, args.verbose)
    results["chat"] = (v, st)

    print("\n--- history candidates ---")
    for label, url, body in [
        ("im/chain/recent_conv",
         BASE + "/im/chain/recent_conv", {"cmd": 3200, "direction": 2, "count": 20}),
        ("samantha/thread/list",
         BASE + "/samantha/thread/list", {"cursor": 0, "count": 20}),
        ("samantha/conversation/list",
         BASE + "/samantha/conversation/list", {"cursor": 0, "count": 20}),
    ]:
        v, st, txt = probe_endpoint(net, dev, label, url, body, args.verbose)
        results[label] = (v, st)

    print("\n=== verdict ===")
    for k, (v, st) in results.items():
        print("  %-26s http=%-4s %s" % (k, st, v))

    accepted = [k for k, (v, _) in results.items() if v.startswith("ACCEPTED")]
    blocked = [k for k, (v, _) in results.items() if v == "BLOCKED-BY-SIGNATURE"]
    print()
    if accepted:
        print("  -> gateway accepted: %s" % ", ".join(accepted))
        print("     signature bypass WORKS. Only a login cookie is missing,")
        print("     and QR login is plain HTTP => no server required.")
    if blocked:
        print("  -> blocked by signature: %s" % ", ".join(blocked))
        print("     these endpoints need a_bogus (or a relay).")
    if not accepted and not blocked:
        print("  -> inconclusive; inspect bodies with -v")
    return 0


if __name__ == "__main__":
    sys.exit(main())
