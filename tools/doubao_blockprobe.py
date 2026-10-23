"""What does the chat endpoint's 710022002 "block" actually mean?

Two candidate readings, with completely different consequences:

  A) "you are anonymous / your session is bad"  -> supplying a real cookie
     fixes it, and a 3DS needs nothing but QR login.
  B) "this IP / device is rate-limited or risk-controlled" -> a cookie won't
     help, and the whole approach needs a relay after all.

We cannot log in here, but we do not have to: we can compare an anonymous
request against one carrying a deliberately *invalid* sessionid.

  * anonymous   -> 710022002 block                     (already measured)
  * fake cookie -> 710012001 "登录已过期"  => A, the gateway sees the
                   session field and rejects it there: block was the
                   anonymous path, a real cookie is the only missing piece.
  * fake cookie -> 710022002 block          => B, indistinguishable from
                   anonymous: block is IP/device level, not session level.
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
    if body is not None:
        data = json.dumps(body, ensure_ascii=False).encode() if isinstance(
            body, (dict, list)) else body.encode()
    h = {"User-Agent": UA, "Accept": "*/*", "Accept-Language": "zh-CN,zh;q=0.9"}
    if headers:
        h.update(headers)
    r = urllib.request.Request(url, data=data, method="POST" if data else "GET",
                               headers=h)
    try:
        with urllib.request.urlopen(r, timeout=timeout, context=CTX) as resp:
            return resp.status, resp.read()
    except urllib.error.HTTPError as e:
        return e.code, e.read()
    except Exception as e:
        return -1, ("EXC %s" % e).encode()


def anon_bootstrap():
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


def call(cookie, web_id, label, path="/samantha/chat/completion"):
    qs = "&".join([
        "aid=" + AID, "device_id=7691730000000000000", "device_platform=web",
        "language=zh", "pc_version=3.17.3", "pkg_type=release_version",
        "real_aid=" + AID, "region=CN", "samantha_web=1", "sys_region=CN",
        "tea_uuid=%s_%d" % (uuid.uuid4(), int(time.time() * 1000)),
        "use-olympus-account=1", "version_code=20800", "web_id=" + web_id,
    ])
    body = {
        "messages": [{
            "content": json.dumps({"text": "你好"}, ensure_ascii=False),
            "content_type": 2001, "attachments": [], "references": []}],
        "local_conversation_id": "local_%d" % int(time.time()),
        "stream": False,
    }
    h = {"content-type": "application/json", "accept": "text/event-stream",
         "agw-js-conv": "str", "origin": BASE, "referer": BASE + "/chat/",
         "user-agent": UA,
         "x-flow-trace": json.dumps({"trace_id": uuid.uuid4().hex,
                                     "span_id": uuid.uuid4().hex})}
    if cookie:
        h["cookie"] = cookie
    st, b = req(BASE + path + "?" + qs, body, h)
    txt = b.decode("utf-8", "replace")
    codes = re.findall(r'"code":?\s*"?(\d+)', txt)
    msgs = re.findall(r'"(?:message|msg)":\s*"([^"]{0,40})"', txt)
    print("  %-32s http=%-4s codes=%-13s %s"
          % (label, st, ",".join(codes[:2]) or "-", msgs[0] if msgs else txt[:60]))
    return tuple(codes), tuple(msgs[:1]), txt


def main():
    cks, web_id = anon_bootstrap()
    anon = "; ".join("%s=%s" % (k, v) for k, v in cks.items())
    print("anon cookie:", anon[:70])
    print("web_id:", web_id)

    fake_variants = [
        ("fake sessionid", anon + "; sessionid=0000000000000000000000"),
        ("fake sessionid_ss", anon + "; sessionid_ss=0000000000000000000000"),
        ("fake uid_tt", anon + "; uid_tt=0000000000000000000000"),
    ]

    print("\n=== chat: anonymous vs fake-session ===")
    a = call(anon, web_id, "anonymous (ttwid only)")
    time.sleep(2)
    outs = {}
    for label, ck in fake_variants:
        outs[label] = call(ck, web_id, label)
        time.sleep(2)

    print("\n=== control: history endpoint, same cookies ===")
    call(anon, web_id, "anonymous", "/samantha/conversation/list")
    time.sleep(1)
    for label, ck in fake_variants[:1]:
        call(ck, web_id, label, "/samantha/conversation/list")

    print("\n=== verdict ===")
    anon_codes = a[0]
    changed = [(k, v[0]) for k, v in outs.items() if v[0] and v[0] != anon_codes]
    if changed:
        print("  a fake session CHANGED the answer -> the gateway inspects the")
        print("  session field. 710022002 is the anonymous path; a real cookie")
        print("  is what flips it. => no relay needed, QR login suffices.")
        for k, c in changed:
            print("     %-20s %s" % (k, c))
    else:
        print("  fake sessions behave exactly like anonymous -> 710022002 looks")
        print("  IP/device level, not session level. A cookie may not be enough.")
    print("  anon codes: %s" % (anon_codes or "-"))
    return 0


if __name__ == "__main__":
    main()
