#!/usr/bin/env python3
"""Doubao QR login driver that runs on the PC.

Why this exists: the 3DS probe inside Azahar stops polling the moment the
emulator exits, and a passport QR whose client goes silent gets rejected by
the server with "登录失败，请重新扫码". This driver keeps the conversation
alive, logs every server answer, and refreshes the code when it expires so a
human always has a live one to scan.

It also produces the credential file (sessionid + cookies) that the endpoint
probes need, so the whole login -> chat -> history chain can be validated
without ever touching the 3DS.

Usage:
    python tools/doubao_login.py            # run for up to REFRESH_LIMIT minutes

Writes:
    tools/out/doubao_qr.png      current QR (rewritten on refresh)
    tools/out/doubao_login.log   every poll answer, append-only
    tools/out/doubao_state.txt   one-line status for the outside world
    tools/doubao_session.json    credentials, only on success
"""

import base64
import http.cookiejar
import json
import os
import ssl
import sys
import time
import urllib.parse
import urllib.request

BASE = "https://www.doubao.com"
AID = 497858
UA = (
    "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
    "(KHTML, like Gecko) Chrome/148.0.0.0 Safari/537.36"
)
BASE_HEADERS = {
    "User-Agent": UA,
    "Accept": "application/json, text/plain, */*",
    "Accept-Language": "zh-CN,zh;q=0.9",
    "Referer": BASE + "/chat/login",
    "Origin": BASE,
}

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
OUT = os.path.join(ROOT, "tools", "out")
QR_PNG = os.path.join(OUT, "doubao_qr.png")
QR_META = os.path.join(OUT, "doubao_qr.json")
QR_JS = os.path.join(OUT, "doubao_qr.js")
LOG = os.path.join(OUT, "doubao_login.log")
STATE = os.path.join(OUT, "doubao_state.txt")
SESSION = os.path.join(ROOT, "tools", "doubao_session.json")

POLL_INTERVAL = 1.5
TOTAL_BUDGET = 25 * 60  # keep handing out fresh codes for this long

CTX = ssl.create_default_context()


def log(msg):
    line = "[%s] %s" % (time.strftime("%H:%M:%S"), msg)
    print(line, flush=True)
    with open(LOG, "a", encoding="utf-8") as fh:
        fh.write(line + "\n")


def set_state(s):
    with open(STATE, "w", encoding="utf-8") as fh:
        fh.write(s)


class Session:
    def __init__(self):
        self.jar = http.cookiejar.CookieJar()
        self.op = urllib.request.build_opener(
            urllib.request.HTTPCookieProcessor(self.jar),
            urllib.request.HTTPSHandler(context=CTX),
        )

    def get(self, url, extra=None, timeout=30):
        h = dict(BASE_HEADERS)
        if extra:
            h.update(extra)
        req = urllib.request.Request(url, headers=h)
        try:
            r = self.op.open(req, timeout=timeout)
            return r.status, r.read().decode("utf-8", "replace")
        except urllib.error.HTTPError as e:
            return e.code, e.read().decode("utf-8", "replace")
        except Exception as e:
            return -1, str(e)

    def cookie_header(self):
        return "; ".join("%s=%s" % (c.name, c.value) for c in self.jar)

    def cookies_dict(self):
        return {c.name: c.value for c in self.jar}


def bootstrap(s):
    """Homepage + csrf. Returns csrf token or ''."""
    st, _ = s.get(BASE + "/")
    log("GET /            -> %s" % st)
    csrf = ""
    st, body = s.get(BASE + "/passport/safe/csrf_token/?aid=%d" % AID)
    if st == 200:
        try:
            csrf = json.loads(body)["data"]["passport_csrf_token"]
        except Exception:
            log("   csrf parse failed: %s" % body[:200])
    log("GET csrf_token   -> %s  csrf=%s..." % (st, csrf[:16]))
    return csrf


def fetch_qr(s, csrf):
    """Returns (token, png_bytes, expire_epoch)."""
    nxt = urllib.parse.quote(BASE, safe="")
    st, body = s.get(
        BASE + "/passport/web/get_qrcode/?next=%s&aid=%d" % (nxt, AID),
        {"x-tt-passport-csrf-token": csrf},
    )
    if st != 200:
        log("get_qrcode -> %s  %s" % (st, body[:200]))
        return None, None, 0
    d = json.loads(body).get("data", {})
    token = d.get("token", "")
    raw = d.get("qrcode", "")
    if raw.startswith("data:"):
        raw = raw.partition(",")[2]
    png = base64.b64decode(raw + "=" * (-len(raw) % 4)) if raw else b""
    return token, png, int(d.get("expire_time", 0) or 0)


def write_qr(png, expire):
    """Publish atomically: the viewer must never catch a half-written PNG."""
    os.makedirs(OUT, exist_ok=True)
    tmp = QR_PNG + ".tmp"
    with open(tmp, "wb") as fh:
        fh.write(png)
    os.replace(tmp, QR_PNG)
    meta = {"generated_at": int(time.time()), "expires_at": int(expire),
            "bytes": len(png)}
    tmp2 = QR_META + ".tmp"
    with open(tmp2, "w", encoding="utf-8") as fh:
        json.dump(meta, fh)
    os.replace(tmp2, QR_META)
    # A classic <script> tag is the one thing file:// pages may still load,
    # so the countdown travels as JS instead of JSON+fetch.
    tmp3 = QR_JS + ".tmp"
    with open(tmp3, "w", encoding="utf-8") as fh:
        fh.write("window.__QR = %s;\n" % json.dumps(meta))
    os.replace(tmp3, QR_JS)


def poll_once(s, token, csrf):
    """Returns (status_str, full_data_dict)."""
    nxt = urllib.parse.quote(BASE, safe="")
    url = "%s/passport/web/check_qrconnect/?next=%s&token=%s&aid=%d" % (
        BASE, nxt, token, AID)
    st, body = s.get(url, {"x-tt-passport-csrf-token": csrf}, timeout=20)
    if st != 200:
        return "http_%s" % st, {"raw": body[:300]}
    try:
        d = json.loads(body)
    except Exception:
        return "badjson", {"raw": body[:300]}
    data = d.get("data", {}) or {}
    status = str(data.get("status", d.get("status", "")))
    return status, d


def finish(s, d):
    """Follow redirect_url, then persist every cookie we ended up with."""
    redirect = (d.get("data") or {}).get("redirect_url", "")
    log("confirmed. redirect_url=%s" % (redirect[:120] or "(none)"))
    if redirect:
        st, _ = s.get(redirect, timeout=40)
        log("follow redirect -> %s" % st)
    # A second hit on the chat page finishes planting session cookies.
    st, _ = s.get(BASE + "/chat/", timeout=40)
    log("GET /chat/       -> %s" % st)

    ck = s.cookies_dict()
    with open(SESSION, "w", encoding="utf-8") as fh:
        json.dump({"cookies": ck, "cookie_header": s.cookie_header(),
                   "saved_at": time.time()}, fh, ensure_ascii=False, indent=1)
    keys = sorted(ck)
    log("saved %d cookies -> %s" % (len(ck), ", ".join(keys)))
    log("sessionid present: %s" % ("sessionid" in ck or "sessionid_ss" in ck))
    return ck


def main():
    os.makedirs(OUT, exist_ok=True)
    open(LOG, "w", encoding="utf-8").close()
    set_state("booting")

    s = Session()
    csrf = bootstrap(s)
    if not csrf:
        set_state("csrf_failed")
        log("no csrf token, aborting")
        return 1

    deadline = time.monotonic() + TOTAL_BUDGET
    generation = 0

    while time.monotonic() < deadline:
        generation += 1
        token, png, expire = fetch_qr(s, csrf)
        if not token or not png:
            set_state("qr_failed")
            log("failed to obtain a QR code")
            return 1

        now = int(time.time())
        ttl = expire - now if expire else -1
        write_qr(png, expire)
        log("QR #%d  token=%s  %d bytes  ttl=%ds  -> %s"
            % (generation, token[:16], len(png), ttl, QR_PNG))
        set_state("waiting_scan:new#%d" % generation)

        last = ""
        # Drop the code before the server does, so a human never scans a
        # corpse. TTL is ~60s, so this retires it at ~45s.
        code_deadline = min(deadline, time.monotonic() + max(ttl - 15, 30))

        while time.monotonic() < code_deadline:
            status, d = poll_once(s, token, csrf)
            if status != last:
                log("  poll status=%s  data=%s"
                    % (status, json.dumps(d, ensure_ascii=False)[:400]))
                last = status
                set_state("waiting_scan:%s#%d" % (status, generation))

            if status in ("confirmed", "success"):
                set_state("confirmed")
                finish(s, d)
                log("=== LOGIN COMPLETE ===")
                return 0
            if status in ("expired", "failed", "canceled", "cancel"):
                log("  code %s -> fetching a fresh one" % status)
                break
            if status in ("scanned",):
                # Keep going; the human is confirming on the phone.
                pass
            time.sleep(POLL_INTERVAL)

    set_state("timeout")
    log("gave up after %d minutes without a scan" % (TOTAL_BUDGET // 60))
    return 1


if __name__ == "__main__":
    sys.exit(main())
