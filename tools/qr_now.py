#!/usr/bin/env python3
"""One fresh Doubao QR, polled until it is scanned or dies.

Written for the case where a human is waiting on a phone: fetch a code, drop
it at tools/out/qr_now.png straight away so it can be handed over within
seconds, then keep polling so the confirm is not missed.

    python tools/qr_now.py          # keeps trying for one code, ~90s
    python tools/qr_now.py --loop   # hand out fresh codes for 20 minutes
"""

import base64
import http.cookiejar
import json
import os
import ssl
import sys
import time
import urllib.error
import urllib.parse
import urllib.request

BASE = "https://www.doubao.com"
AID = 497858
UA = ("Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
      "(KHTML, like Gecko) Chrome/148.0.0.0 Safari/537.36")
H = {"User-Agent": UA, "Accept": "application/json, text/plain, */*",
     "Accept-Language": "zh-CN,zh;q=0.9",
     "Referer": BASE + "/chat/login", "Origin": BASE}
NEXT = urllib.parse.quote(BASE, safe="")

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
OUT = os.path.join(ROOT, "tools", "out")
PNG = os.path.join(OUT, "qr_now.png")
STATE = os.path.join(OUT, "qr_now_state.txt")
SESSION = os.path.join(ROOT, "tools", "doubao_session.json")
CTX = ssl.create_default_context()


def say(m):
    print("[%s] %s" % (time.strftime("%H:%M:%S"), m), flush=True)


def put_state(s):
    with open(STATE, "w", encoding="utf-8") as fh:
        fh.write(s)


class S:
    def __init__(self):
        self.op = urllib.request.build_opener(
            urllib.request.HTTPCookieProcessor(http.cookiejar.CookieJar()),
            urllib.request.HTTPSHandler(context=CTX))

    def get(self, url, extra=None, timeout=25):
        h = dict(H)
        if extra:
            h.update(extra)
        try:
            r = self.op.open(urllib.request.Request(url, headers=h), timeout=timeout)
            return r.status, r.read().decode("utf-8", "replace")
        except urllib.error.HTTPError as e:
            return e.code, e.read().decode("utf-8", "replace")
        except Exception as e:
            return -1, str(e)

    def cookies(self):
        return {c.name: c.value for c in
                self.op.handlers[0].cookiejar} if False else \
               {c.name: c.value for c in
                [x for x in self.op.handlers
                 if isinstance(x, urllib.request.HTTPCookieProcessor)][0].cookiejar}


def one_round(s, budget):
    st, _ = s.get(BASE + "/")
    st, body = s.get(BASE + "/passport/safe/csrf_token/?aid=%d" % AID)
    csrf = ""
    if st == 200:
        try:
            csrf = json.loads(body)["data"]["passport_csrf_token"]
        except Exception:
            pass
    if not csrf:
        say("csrf failed")
        return None

    st, body = s.get(BASE + "/passport/web/get_qrcode/?next=%s&aid=%d" % (NEXT, AID),
                     {"x-tt-passport-csrf-token": csrf})
    if st != 200:
        say("get_qrcode -> %s" % st)
        return None
    d = json.loads(body).get("data", {})
    token, raw, expire = d.get("token", ""), d.get("qrcode", ""), int(d.get("expire_time", 0) or 0)
    if raw.startswith("data:"):
        raw = raw.partition(",")[2]
    png = base64.b64decode(raw + "=" * (-len(raw) % 4))
    os.makedirs(OUT, exist_ok=True)
    with open(PNG + ".tmp", "wb") as fh:
        fh.write(png)
    os.replace(PNG + ".tmp", PNG)
    say("QR ready  ttl=%ds  -> %s" % (expire - int(time.time()), PNG))
    put_state("waiting:new")

    last, end = "", time.monotonic() + budget
    while time.monotonic() < end:
        st, body = s.get(
            BASE + "/passport/web/check_qrconnect/?next=%s&token=%s&aid=%d"
            % (NEXT, token, AID), {"x-tt-passport-csrf-token": csrf}, timeout=20)
        if st != 200:
            time.sleep(1.5)
            continue
        try:
            j = json.loads(body)
        except Exception:
            time.sleep(1.5)
            continue
        status = str((j.get("data") or {}).get("status", ""))
        if status != last:
            say("status=%s  %s" % (status, json.dumps(j, ensure_ascii=False)[:300]))
            last = status
            put_state("waiting:%s" % status)
        if status in ("confirmed", "success"):
            redir = (j.get("data") or {}).get("redirect_url", "")
            say("confirmed, redirect=%s" % redir[:110])
            if redir:
                s.get(redir, timeout=40)
            s.get(BASE + "/chat/", timeout=40)
            ck = s.cookies()
            json.dump({"cookies": ck,
                       "cookie_header": "; ".join("%s=%s" % (k, v) for k, v in ck.items()),
                       "saved_at": time.time()},
                      open(SESSION, "w", encoding="utf-8"), ensure_ascii=False, indent=1)
            say("SAVED %d cookies: %s" % (len(ck), ", ".join(sorted(ck))))
            put_state("confirmed")
            return True
        if status in ("expired", "failed", "canceled", "cancel"):
            say("code %s" % status)
            return None
        time.sleep(1.5)
    return None


def main():
    loop = "--loop" in sys.argv
    deadline = time.monotonic() + (20 * 60 if loop else 95)
    s = S()
    while time.monotonic() < deadline:
        if one_round(s, budget=70):
            return 0
        if not loop:
            break
    put_state("timeout")
    say("no scan")
    return 1


if __name__ == "__main__":
    sys.exit(main())
