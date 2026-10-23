"""Is the anonymous `ttwid` cookie the missing piece for the chat endpoint?

Observation that motivated this probe
-------------------------------------
  * with anonymous cookies (ttwid + hook_slardar_session_id) -> 710022002 block
  * with no cookie at all                                    -> gateway-error

If an anonymous ttwid is enough to get a *structured* refusal instead of a
gateway error, then every non-secret ingredient of a Doubao request is
obtainable by a 3DS without any server:

  ttwid          <- GET /            (Set-Cookie, anonymous)
  web_id         <- GET /            (in the SSR payload)
  device_id / tea_uuid  <- not validated (proven by doubao_devprobe)
  x-flow-trace   <- random uuids
  login cookie   <- QR login, plain HTTP

That leaves exactly one thing a 3DS cannot do alone: the scan itself.
"""

import http.cookiejar
import json
import re
import ssl
import time
import urllib.error
import urllib.request
import uuid

BASE = "https://www.doubao.com"
AID = "497858"
UA = ("Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
      "(KHTML, like Gecko) Chrome/135.0.0.0 Safari/537.36")
CTX = ssl.create_default_context()
CTX.check_hostname = False
CTX.verify_mode = ssl.CERT_NONE


def req(url, body=None, headers=None, timeout=40):
    data = None
    method = "GET"
    if body is not None:
        data = json.dumps(body, ensure_ascii=False).encode() if isinstance(
            body, (dict, list)) else body.encode()
        method = "POST"
    h = {"User-Agent": UA, "Accept": "*/*", "Accept-Language": "zh-CN,zh;q=0.9"}
    if headers:
        h.update(headers)
    r = urllib.request.Request(url, data=data, method=method, headers=h)
    try:
        with urllib.request.urlopen(r, timeout=timeout, context=CTX) as resp:
            return resp.status, dict(resp.headers), resp.read()
    except urllib.error.HTTPError as e:
        return e.code, dict(e.headers), e.read()
    except Exception as e:
        return -1, {}, ("EXC %s" % e).encode()


def anon_bootstrap():
    """GET / and keep whatever cookies it hands out."""
    jar = http.cookiejar.CookieJar()
    op = urllib.request.build_opener(
        urllib.request.HTTPCookieProcessor(jar),
        urllib.request.HTTPSHandler(context=CTX))
    r = urllib.request.Request(BASE + "/", headers={"User-Agent": UA})
    with op.open(r, timeout=40) as resp:
        html = resp.read().decode("utf-8", "replace")
    cks = {c.name: c.value for c in jar}
    m = re.search(r'"web_id"\s*:\s*"(\d{6,25})"', html)
    return cks, (m.group(1) if m else "")


def call(cookie, web_id, label):
    dev = {"web_id": web_id,
           "device_id": "7691730000000000000",
           "tea_uuid": "%s_%d" % (uuid.uuid4(), int(time.time() * 1000))}
    qs = "&".join([
        "aid=" + AID, "device_id=" + dev["device_id"], "device_platform=web",
        "language=zh", "pc_version=3.17.3", "pkg_type=release_version",
        "real_aid=" + AID, "region=CN", "samantha_web=1", "sys_region=CN",
        "tea_uuid=" + dev["tea_uuid"], "use-olympus-account=1",
        "version_code=20800", "web_id=" + dev["web_id"],
    ])
    body = {
        "messages": [{
            "content": json.dumps({"text": "你好"}, ensure_ascii=False),
            "content_type": 2001, "attachments": [], "references": []}],
        "local_conversation_id": "local_%d" % int(time.time()),
        "stream": False,
    }
    h = {
        "content-type": "application/json",
        "accept": "text/event-stream",
        "agw-js-conv": "str",
        "origin": BASE,
        "referer": BASE + "/chat/",
        "user-agent": UA,
        "x-flow-trace": json.dumps({"trace_id": uuid.uuid4().hex,
                                    "span_id": uuid.uuid4().hex}),
    }
    if cookie:
        h["cookie"] = cookie
    st, hdr, b = req(BASE + "/samantha/chat/completion?" + qs, body, h)
    txt = b.decode("utf-8", "replace")
    codes = re.findall(r'"code":?\s*"?(\d+)', txt)
    msgs = re.findall(r'"(?:message|msg)":\s*"([^"]{0,40})"', txt)
    print("  %-30s http=%-4s codes=%-13s %s"
          % (label, st, ",".join(codes[:2]) or "-", msgs[0] if msgs else txt[:70]))
    return st, tuple(codes), msgs[:1]


def main():
    cks, web_id = anon_bootstrap()
    print("anonymous cookies:", ", ".join(sorted(cks)) or "(none)")
    print("web_id:", web_id or "(none)")
    cookie = "; ".join("%s=%s" % (k, v) for k, v in cks.items())

    print("\n=== chat: cookie vs none ===")
    r_with = call(cookie, web_id, "with anon cookie (ttwid)")
    time.sleep(2)
    r_without = call(None, web_id, "no cookie")
    time.sleep(2)
    r_ttwid = None
    if "ttwid" in cks:
        r_ttwid = call("ttwid=" + cks["ttwid"], web_id, "ttwid only")
        time.sleep(2)

    print("\n=== history: cookie vs none (control) ===")
    for lbl, ck in (("with anon cookie", cookie), ("no cookie", None)):
        h = {"content-type": "application/json", "agw-js-conv": "str",
             "origin": BASE, "referer": BASE + "/chat/", "user-agent": UA,
             "x-flow-trace": json.dumps({"trace_id": uuid.uuid4().hex,
                                         "span_id": uuid.uuid4().hex})}
        if ck:
            h["cookie"] = ck
        st, _, b = req(BASE + "/samantha/conversation/list",
                       {"cursor": 0, "count": 20}, h)
        txt = b.decode("utf-8", "replace")
        codes = re.findall(r'"code":\s*(\d+)', txt)
        msgs = re.findall(r'"msg":\s*"([^"]{0,30})"', txt)
        print("  %-30s http=%-4s codes=%-13s %s"
              % (lbl, st, ",".join(codes[:2]) or "-", msgs[0] if msgs else txt[:70]))

    print("\n=== verdict ===")
    if r_with[1] and not r_without[1]:
        print("  anon cookie changes chat from gateway-error to a coded refusal.")
        print("  -> every ingredient except the login session is self-obtainable.")
    elif r_with[1] == r_without[1]:
        print("  cookie made no difference to chat; both give the same class.")
    print("  chat codes   : with-cookie=%s  no-cookie=%s%s"
          % (r_with[1] or "-", r_without[1] or "-",
             "  ttwid-only=%s" % (r_ttwid[1] or "-") if r_ttwid else ""))
    return 0


if __name__ == "__main__":
    main()
