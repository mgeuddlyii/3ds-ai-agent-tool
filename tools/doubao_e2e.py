"""Doubao end-to-end probe: scan-login -> session -> chat + history.

Goal: decide whether a 3DS can talk to Doubao *directly* (no server at all).

Why this matters (2026-10-02):
  * TLS 1.2 on 3DS is solved (libcurl + 3ds-mbedtls, see tools/tls12).
  * QR login is plain HTTP (no browser) - already proven.
  * The open question is the a_bogus signature. Three known approaches:
      B1  reimplement a_bogus in another language  -> infeasible (JSVMP)
      B2  Playwright, let the page sign it         -> triggers fingerprint checks
      B3  x-flow-trace bypass header               -> plain HTTP, no signing
    B3 reportedly works for /samantha/chat/completion only; other endpoints
    (conversation list) normally still need a_bogus. The /im/chain/* endpoints
    look like a plain-HTTP alternative for history - this script tests that.

Usage
-----
    python tools/doubao_e2e.py qr                 # fetch QR, save PNG, print token
    python tools/doubao_e2e.py poll <token>       # wait for scan, save credentials
    python tools/doubao_e2e.py probe              # test chat + history endpoints
    python tools/doubao_e2e.py import <file>      # import headers pasted from devtools

Notes
-----
  * Scan with the **Douyin app**, not the Doubao app (ByteDance unified passport).
  * x-flow-trace / tea_uuid / device_id / web_id have to be captured from a real
    logged-in browser session (F12 -> Network -> any /samantha/ request).
    `import` takes a JSON file with those headers so `probe` can try B3.
  * Credentials are written to tools/doubao_session.json - keep it private.
"""

import argparse
import base64
import http.cookiejar
import json
import os
import sys
import time
import urllib.parse
import urllib.request
import urllib.error

HERE = os.path.dirname(os.path.abspath(__file__))
SESSION = os.path.join(HERE, "doubao_session.json")
QR_PNG = os.path.join(HERE, "out", "doubao_qr.png")

BASE = "https://www.doubao.com"
AID = "497858"
UA = ("Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
      "(KHTML, like Gecko) Chrome/135.0.0.0 Safari/537.36")


class Net:
    """Cookie-keeping HTTP client. TLS verification is left to the OS store."""

    def __init__(self):
        self.cj = http.cookiejar.CookieJar()
        self.op = urllib.request.build_opener(
            urllib.request.HTTPCookieProcessor(self.cj))

    def get(self, url, headers=None):
        req = urllib.request.Request(url, headers=self._h(headers))
        try:
            with self.op.open(req, timeout=30) as r:
                return r.status, dict(r.headers), r.read()
        except urllib.error.HTTPError as e:
            return e.code, dict(e.headers), e.read()

    def post(self, url, body, headers=None):
        if isinstance(body, (dict, list)):
            body = json.dumps(body, ensure_ascii=False).encode("utf-8")
        elif isinstance(body, str):
            body = body.encode("utf-8")
        req = urllib.request.Request(url, data=body, method="POST",
                                     headers=self._h(headers))
        try:
            with self.op.open(req, timeout=60) as r:
                return r.status, dict(r.headers), r.read()
        except urllib.error.HTTPError as e:
            return e.code, dict(e.headers), e.read()

    def _h(self, extra):
        h = {"User-Agent": UA, "Accept": "*/*"}
        if extra:
            h.update(extra)
        return h

    def cookie_str(self):
        return "; ".join("%s=%s" % (c.name, c.value) for c in self.cj)


def load_session():
    if os.path.exists(SESSION):
        with open(SESSION, encoding="utf-8") as f:
            return json.load(f)
    return {}


def save_session(d):
    with open(SESSION, "w", encoding="utf-8") as f:
        json.dump(d, f, ensure_ascii=False, indent=2)
    print("saved ->", SESSION)


# --------------------------------------------------------------------------
# 1. QR login
# --------------------------------------------------------------------------

def cmd_qr(args):
    net = Net()
    net.get(BASE + "/")
    st, _, body = net.get(
        "%s/passport/safe/csrf_token/?aid=%s" % (BASE, AID))
    try:
        csrf = json.loads(body).get("token", "")
    except Exception:
        csrf = ""
    print("csrf:", (csrf or "(none)")[:40])

    nxt = urllib.parse.quote(BASE + "/chat/", safe="")
    url = ("%s/passport/web/get_qrcode/?next=%s&aid=%s" % (BASE, nxt, AID))
    st, _, body = net.get(url, {"x-tt-passport-csrf-token": csrf})
    d = json.loads(body)
    print("get_qrcode status:", st, "keys:", list(d.keys())[:8])

    token = d.get("token") or (d.get("data") or {}).get("token")
    img = d.get("image") or d.get("qrcode") or (d.get("data") or {}).get("image")
    if not token:
        print("!! no token:", json.dumps(d, ensure_ascii=False)[:400])
        return 1

    os.makedirs(os.path.dirname(QR_PNG), exist_ok=True)
    if img:
        raw = base64.b64decode(img.split(",")[-1])
        with open(QR_PNG, "wb") as f:
            f.write(raw)
        print("qr png -> %s (%d bytes)" % (QR_PNG, len(raw)))
    print("TOKEN=%s" % token)
    print("scan it with the Douyin app, then:")
    print("  python tools/doubao_e2e.py poll %s" % token)
    return 0


def cmd_poll(args):
    token = args.token
    net = Net()
    net.get(BASE + "/")
    deadline = time.time() + args.timeout
    status = None
    while time.time() < deadline:
        url = ("%s/passport/web/check_qrconnect/?token=%s&aid=%s"
               % (BASE, token, AID))
        st, _, body = net.get(url)
        try:
            d = json.loads(body)
        except Exception:
            time.sleep(2)
            continue
        status = d.get("status") or (d.get("data") or {}).get("status")
        print("  status =", status, flush=True)
        if status in ("confirmed", "success", 3):
            save_session({
                "cookie": net.cookie_str(),
                "cookie_list": [{"name": c.name, "value": c.value,
                                 "domain": c.domain} for c in net.cj],
                "captured_at": time.strftime("%Y-%m-%d %H:%M:%S"),
                "raw": d,
            })
            print("logged in. cookie stored.")
            return 0
        if status in ("expired", "canceled", "failed"):
            print("!! login", status)
            return 1
        time.sleep(2)
    print("!! timed out, last status:", status)
    return 1


def cmd_import(args):
    """Import headers captured from a logged-in browser (F12 -> Network).

    Expected file format - any subset is fine:
      {"cookie": "...", "x-flow-trace": "...",
       "tea_uuid": "...", "device_id": "...", "web_id": "...", "fp": "...",
       "room_id": "..."}
    """
    with open(args.file, encoding="utf-8") as f:
        d = json.load(f)
    s = load_session()
    s.update(d)
    save_session(s)
    print("imported keys:", list(d.keys()))
    return 0


# --------------------------------------------------------------------------
# 2. Endpoint probes
# --------------------------------------------------------------------------

def probe_chat(net, s, verbose):
    """Try /samantha/chat/completion with the B3 bypass header set."""
    url = BASE + "/samantha/chat/completion"
    qs = {}
    for k_src, k_dst in (("device_id", "device_id"), ("web_id", "web_id"),
                         ("tea_uuid", "tea_uuid"), ("fp", "fp")):
        if s.get(k_src):
            qs[k_dst] = s[k_src]
    if qs:
        url += "?" + urllib.parse.urlencode(qs)

    body = {
        "messages": [
            {"content_type": 2001,
             "content": json.dumps({"text": "你好"}, ensure_ascii=False)}
        ],
        "local_conversation_id": "local_%d" % int(time.time()),
        "stream": False,
    }

    variants = []
    base_h = {
        "Content-Type": "application/json",
        "Origin": BASE,
        "Referer": BASE + "/chat/",
        "agw-js-conv": "str",
        "Cookie": s.get("cookie", ""),
    }
    if s.get("x-flow-trace"):
        base_h["x-flow-trace"] = s["x-flow-trace"]
    variants.append(("as-captured (cookie+x-flow-trace)", dict(base_h)))

    h2 = dict(base_h)
    h2.pop("x-flow-trace", None)
    variants.append(("without x-flow-trace", h2))

    h3 = dict(base_h)
    h3["x-flow-trace"] = s.get("x-flow-trace", "") or "1"
    variants.append(("x-flow-trace fallback", h3))

    for label, h in variants:
        st, hdr, b = net.post(url, body, h)
        txt = b.decode("utf-8", "replace")
        print("  [%s] http=%s len=%d" % (label, st, len(b)))
        if verbose or st != 200:
            print("      %s" % txt[:300].replace("\n", " "))
        if st == 200:
            return True
    return False


def probe_history(net, s, verbose):
    """/im/chain/* is reported to be plain HTTP - no a_bogus needed."""
    cookie = s.get("cookie", "")
    h = {
        "Content-Type": "application/json",
        "Origin": BASE,
        "Referer": BASE + "/chat/",
        "Cookie": cookie,
        "agw-js-conv": "str",
    }
    if s.get("x-flow-trace"):
        h["x-flow-trace"] = s["x-flow-trace"]

    attempts = [
        (BASE + "/im/chain/recent_conv", {"cmd": 3200, "direction": 2, "count": 20}),
        (BASE + "/im/chain/recent_conv", {"cmd": 3200, "direction": 2, "limit": 20}),
        (BASE + "/samantha/conversation/list", {"cursor": 0, "count": 20}),
    ]
    ok = False
    for url, body in attempts:
        st, hdr, b = net.post(url, body, h)
        txt = b.decode("utf-8", "replace")
        print("  [%s] http=%s len=%d" % (url.split(".com")[1], st, len(b)))
        if verbose or st != 200:
            print("      %s" % txt[:300].replace("\n", " "))
        if st == 200 and len(b) > 20:
            ok = True
    return ok


def cmd_probe(args):
    s = load_session()
    if not s.get("cookie"):
        print("!! no session. run `qr` + `poll` (or `import`) first.")
        return 1
    net = Net()
    print("cookie fields:", len(s.get("cookie_list", [])),
          "| x-flow-trace:", "yes" if s.get("x-flow-trace") else "NO")

    print("\n--- chat (/samantha/chat/completion) ---")
    chat_ok = probe_chat(net, s, args.verbose)

    print("\n--- history (/im/chain + /samantha/conversation/list) ---")
    hist_ok = probe_history(net, s, args.verbose)

    print("\n=== verdict ===")
    print("chat   :", "OK" if chat_ok else "blocked")
    print("history:", "OK" if hist_ok else "blocked")
    if chat_ok and hist_ok:
        print("-> 3DS can reach Doubao directly. No server needed.")
    elif chat_ok:
        print("-> chat works direct; history needs a signed endpoint.")
    else:
        print("-> direct path blocked; a relay (or captured x-flow-trace) is required.")
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd")

    sub.add_parser("qr", help="fetch a login QR code").set_defaults(fn=cmd_qr,
                                                                    token=None,
                                                                    timeout=180,
                                                                    verbose=False,
                                                                    file=None)
    p = sub.add_parser("poll", help="wait for the QR scan")
    p.add_argument("token")
    p.add_argument("--timeout", type=int, default=180)
    p.set_defaults(fn=cmd_poll, verbose=False, file=None)

    p = sub.add_parser("import", help="import headers captured from a browser")
    p.add_argument("file")
    p.set_defaults(fn=cmd_import, token=None, timeout=180, verbose=False)

    p = sub.add_parser("probe", help="test chat + history endpoints")
    p.add_argument("-v", "--verbose", action="store_true")
    p.set_defaults(fn=cmd_probe, token=None, timeout=180, file=None)

    args = ap.parse_args()
    if not args.cmd:
        ap.print_help()
        return 2
    return args.fn(args)


if __name__ == "__main__":
    sys.exit(main())
