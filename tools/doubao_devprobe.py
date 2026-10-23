"""Does the Doubao gateway actually validate device_id / tea_uuid / web_id?

The B3 bypass needs three device params in the query string. On a real 3DS
there is no browser to mint them, so we need to know whether the server
cares. Method: send the *same* request with several param sets and see
whether the answer changes at all.

  * identical answers  -> params are decorative; a 3DS can invent them
  * different answers  -> they are checked; we must obtain them somehow

Also probes whether a fresh web_id can be minted by the 3DS itself, by
simply hitting the homepage (it is server-issued, no JS needed).
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
            return resp.status, resp.read()
    except urllib.error.HTTPError as e:
        return e.code, e.read()
    except Exception as e:
        return -1, ("EXC %s" % e).encode()


def mint_web_id():
    """The homepage SSRs a fresh web_id - no JS, no login."""
    st, b = req(BASE + "/")
    html = b.decode("utf-8", "replace")
    m = re.search(r'"web_id"\s*:\s*"(\d{6,25})"', html)
    return m.group(1) if m else "", st


def build_url(dev):
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
    return BASE + "/samantha/chat/completion?" + qs


def once(dev, label):
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
    url = build_url(dev)
    st, b = req(url, body, h)
    txt = b.decode("utf-8", "replace")
    # Pull the error code out so we can compare apples to apples.
    codes = re.findall(r'"code":\s*(\d+)', txt)
    msgs = re.findall(r'"message":\s*"([^"]{0,40})"', txt)
    print("  %-34s http=%-4s codes=%-14s %s"
          % (label, st, ",".join(codes[:2]) or "-", msgs[0] if msgs else txt[:60]))
    return (st, tuple(codes), msgs[:1])


def main():
    print("=== minting a web_id anonymously ===")
    web_id, st = mint_web_id()
    print("  http=%s  web_id=%s" % (st, web_id or "(none)"))

    fake_dev_id = str(7_000_000_000_000_000_000 +
                      int(time.time() * 1000) % 999_999_999_999_999)
    fake_tea = "%s_%d" % (uuid.uuid4(), int(time.time() * 1000))

    print("\n=== chat endpoint, four param sets ===")
    results = {}
    sets = [
        ("all empty", {}),
        ("real web_id only", {"web_id": web_id}),
        ("web_id + plausible device_id",
         {"web_id": web_id, "device_id": fake_dev_id}),
        ("all three plausible",
         {"web_id": web_id, "device_id": fake_dev_id, "tea_uuid": fake_tea}),
    ]
    for label, dev in sets:
        results[label] = once(dev, label)
        time.sleep(1)

    uniq = set((v[0], v[1]) for v in results.values())
    print("\n=== comparison ===")
    if len(uniq) == 1:
        print("  all four answered identically -> device params are NOT "
              "validated before login.")
        print("  a 3DS can invent them (or just mint web_id from /).")
    else:
        print("  answers differ -> params matter; they must be captured "
              "from a browser session.")
        for k, v in results.items():
            print("     %-34s %s %s" % (k, v[0], v[1]))

    print("\n=== history endpoint, same question ===")
    for label, dev in sets:
        url = build_url(dev).replace("/samantha/chat/completion",
                                     "/samantha/conversation/list")
        h = {"content-type": "application/json", "agw-js-conv": "str",
             "origin": BASE, "referer": BASE + "/chat/", "user-agent": UA,
             "x-flow-trace": json.dumps({"trace_id": uuid.uuid4().hex,
                                         "span_id": uuid.uuid4().hex})}
        st, b = req(url, {"cursor": 0, "count": 20}, h)
        txt = b.decode("utf-8", "replace")
        codes = re.findall(r'"code":\s*(\d+)', txt)
        msgs = re.findall(r'"msg":\s*"([^"]{0,30})"', txt)
        print("  %-34s http=%-4s codes=%-14s %s"
              % (label, st, ",".join(codes[:2]) or "-", msgs[0] if msgs else txt[:50]))
        time.sleep(1)
    return 0


if __name__ == "__main__":
    main()
