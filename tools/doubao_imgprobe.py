#!/usr/bin/env python3
"""端到端验证「3DS 拍照 -> 上传豆包 -> 识图提问」链路。

这条链路最难的一环是火山引擎 ImageX 的 AWS4-HMAC-SHA256 签名。
本脚本用纯 stdlib (hashlib + hmac) 实现它，目的是证明：
算法本身只有 SHA256 + HMAC-SHA256，两者 mbedtls 都提供，3DS 上可原样移植。

用法:
    python tools/doubao_imgprobe.py            # 跑完整链路
    python tools/doubao_imgprobe.py --verbose  # 打印每一步的请求/响应
"""

import argparse
import binascii
import hashlib
import hmac
import json
import os
import ssl
import sys
import time
import urllib.parse
import urllib.request
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
SESSION = os.path.join(HERE, "doubao_session.json")

UA = ("Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
      "(KHTML, like Gecko) Chrome/135.0.0.0 Safari/537.36")
BASE = "https://www.doubao.com"
IMAGEX_HOST = "imagex.bytedanceapi.com"
TOS_HOST = "tos-d-x-hl.snssdk.com"
REGION = "cn-north-1"
SERVICE = "imagex"

CTX = ssl.create_default_context()


# --------------------------------------------------------------------------
# AWS Signature Version 4 — stdlib only.
# Only SHA256 + HMAC-SHA256 are needed, both of which mbedtls provides,
# so this exact algorithm is portable to the 3DS.
# --------------------------------------------------------------------------
def _sha256_hex(b):
    return hashlib.sha256(b).hexdigest()


def _hmac(key, msg):
    return hmac.new(key, msg.encode("utf-8"), hashlib.sha256).digest()


def _sign_key(secret, datestamp):
    k = _hmac(("AWS4" + secret).encode("utf-8"), datestamp)
    k = _hmac(k, REGION)
    k = _hmac(k, SERVICE)
    return _hmac(k, "aws4_request")


def aws4_sign(method, host, path, query_pairs, headers, payload,
              access_key, secret_key, session_token=None):
    """Return the headers dict with Authorization/x-amz-date added."""
    now = time.gmtime()
    amzdate = time.strftime("%Y%m%dT%H%M%SZ", now)
    datestamp = time.strftime("%Y%m%d", now)

    h = {k.lower(): str(v).strip() for k, v in headers.items()}
    h["host"] = host
    h["x-amz-date"] = amzdate
    if session_token:
        h["x-amz-security-token"] = session_token

    # Canonical query string: sorted by key, then by value, URI-encoded.
    q = sorted(((urllib.parse.quote(str(k), safe="-_.~"),
                 urllib.parse.quote(str(v), safe="-_.~"))
                for k, v in query_pairs if v is not None))
    canonical_qs = "&".join("%s=%s" % kv for kv in q)

    signed = sorted(h.keys())
    canonical_headers = "".join("%s:%s\n" % (k, h[k]) for k in signed)
    signed_headers = ";".join(signed)

    canonical_req = "\n".join([
        method.upper(),
        path or "/",
        canonical_qs,
        canonical_headers,
        signed_headers,
        _sha256_hex(payload),
    ])

    scope = "%s/%s/%s/aws4_request" % (datestamp, REGION, SERVICE)
    sts = "\n".join(["AWS4-HMAC-SHA256", amzdate, scope,
                     _sha256_hex(canonical_req.encode("utf-8"))])

    sig = hmac.new(_sign_key(secret_key, datestamp),
                   sts.encode("utf-8"), hashlib.sha256).hexdigest()

    out = dict(headers)
    out["host"] = host
    out["x-amz-date"] = amzdate
    if session_token:
        out["x-amz-security-token"] = session_token
    out["Authorization"] = (
        "AWS4-HMAC-SHA256 Credential=%s/%s, SignedHeaders=%s, Signature=%s"
        % (access_key, scope, signed_headers, sig))
    return out, canonical_req


# --------------------------------------------------------------------------
# Minimal PNG writer — no Pillow dependency. Produces a distinctive test
# pattern so we can tell from Doubao's reply whether it really saw the image.
# --------------------------------------------------------------------------
def make_png(w, h):
    """Solid-colour PNG with a white block, so the answer is verifiable."""
    raw = bytearray()
    for y in range(h):
        raw.append(0)  # filter type 0
        for x in range(w):
            # white cross on a red field: unmistakable to a vision model
            band = (abs(x - w // 2) < w // 8) or (abs(y - h // 2) < h // 8)
            if band:
                raw += b"\xff\xff\xff"
            else:
                raw += b"\xc0\x20\x20"

    def chunk(tag, data):
        c = tag + data
        return (len(data).to_bytes(4, "big") + c +
                binascii.crc32(c).to_bytes(4, "big"))

    ihdr = (w.to_bytes(4, "big") + h.to_bytes(4, "big") +
            bytes([8, 2, 0, 0, 0]))  # 8-bit, truecolour RGB
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", ihdr) +
            chunk(b"IDAT", zlib.compress(bytes(raw), 9)) +
            chunk(b"IEND", b""))


# --------------------------------------------------------------------------
def http(url, method="GET", body=None, headers=None, verbose=False):
    req = urllib.request.Request(url, data=body, method=method,
                                 headers=headers or {})
    if verbose:
        print("    -> %s %s" % (method, url[:120]))
    try:
        with urllib.request.urlopen(req, timeout=60, context=CTX) as r:
            raw = r.read()
            if verbose:
                print("    <- %s  %d bytes" % (r.status, len(raw)))
            return r.status, raw
    except urllib.error.HTTPError as e:
        raw = e.read()
        if verbose:
            print("    <- HTTP %s  %d bytes" % (e.code, len(raw)))
        return e.code, raw
    except Exception as e:
        if verbose:
            print("    <- ERR %s: %s" % (type(e).__name__, str(e)[:120]))
        return -1, str(e).encode()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--verbose", action="store_true")
    ap.add_argument("--size", default="320x240", help="test image WxH")
    args = ap.parse_args()

    if not os.path.exists(SESSION):
        print("!! no session. run: python tools/qr_now.py --loop")
        return 1
    sess = json.load(open(SESSION, encoding="utf-8"))
    cookie = sess["cookie_header"]

    w, h = (int(v) for v in args.size.split("x"))
    png = make_png(w, h)
    print("test image: %dx%d PNG, %d bytes  md5=%s"
          % (w, h, len(png), hashlib.md5(png).hexdigest()))

    common = {
        "origin": BASE,
        "referer": BASE + "/chat/",
        "user-agent": UA,
    }
    url_params = "&".join([
        "aid=497858", "device_id=0", "device_platform=web", "language=zh",
        "pc_version=2.20.0", "pkg_type=release_version", "real_aid=497858",
        "region=CN", "samantha_web=1", "sys_region=CN", "tea_uuid=0",
        "use-olympus-account=1", "version_code=20800",
    ])

    # -- step 1: get temporary credentials -------------------------------
    print("\n[1] prepare_upload  (get AWS credentials)")
    hdr = dict(common)
    hdr["content-type"] = "application/json"
    hdr["cookie"] = cookie
    st, raw = http(BASE + "/alice/resource/prepare_upload?" + url_params,
                   "POST", json.dumps({"resource_type": 2, "scene_id": "5",
                                       "tenant_id": "5"}).encode(), hdr,
                   args.verbose)
    d = json.loads(raw.decode("utf-8", "replace"))
    if d.get("code") != 0:
        print("    FAILED code=%s  %s" % (d.get("code"), raw[:300]))
        return 1
    info = d["data"]
    tok = info["upload_auth_token"]
    service_id = info["service_id"]
    ak, sk, sess_tok = tok["access_key"], tok["secret_key"], tok["session_token"]
    print("    OK  service_id=%s  ak=%s...  ttl=%s"
          % (service_id, ak[:12], tok.get("expired_time", "?")[:19]))

    # -- step 2: ApplyImageUpload (AWS4 signed) --------------------------
    print("\n[2] ApplyImageUpload  (AWS4-HMAC-SHA256 — the hard part)")
    qs = [("Action", "ApplyImageUpload"), ("Version", "2018-08-01"),
          ("ServiceId", service_id), ("NeedFallback", "true"),
          ("FileSize", len(png)), ("FileExtension", ".png")]
    signed_hdr, canon = aws4_sign(
        "GET", IMAGEX_HOST, "/", qs, common, b"", ak, sk, sess_tok)
    if args.verbose:
        print("    canonical request:\n      " +
              canon.replace("\n", "\n      "))
    url = "https://%s/?%s" % (IMAGEX_HOST, urllib.parse.urlencode(qs))
    st, raw = http(url, "GET", None, signed_hdr, args.verbose)
    d = json.loads(raw.decode("utf-8", "replace"))
    addr = d.get("Result", {}).get("UploadAddress", {})
    stores = addr.get("StoreInfos", [])
    if not stores:
        print("    FAILED  %s" % raw[:400])
        return 1
    store_uri = stores[0]["StoreUri"]
    store_auth = stores[0]["Auth"]
    session_key = addr.get("SessionKey")
    print("    OK  store_uri=%s...  session_key=%s..."
          % (store_uri[:40], str(session_key)[:24]))

    # -- step 3: PUT the bytes -------------------------------------------
    print("\n[3] upload binary to TOS")
    up = {
        "authorization": store_auth,
        "origin": BASE,
        "referer": BASE,
        "host": TOS_HOST,
        "content-type": "application/octet-stream",
        "content-disposition": 'attachment; filename="undefined"',
        "content-crc32": "%08x" % (binascii.crc32(png) & 0xFFFFFFFF),
    }
    st, raw = http("https://%s/upload/v1/%s" % (TOS_HOST, store_uri),
                   "POST", png, up, args.verbose)
    d = json.loads(raw.decode("utf-8", "replace"))
    if d.get("message") != "Success":
        print("    FAILED  %s" % raw[:400])
        return 1
    print("    OK  %s" % d.get("message"))

    # -- step 4: CommitImageUpload (AWS4 signed) --------------------------
    print("\n[4] CommitImageUpload  (AWS4 signed)")
    qs2 = [("Action", "CommitImageUpload"), ("Version", "2018-08-01"),
           ("ServiceId", service_id)]
    payload = json.dumps({"SessionKey": session_key}).encode()
    ch = dict(common)
    ch["content-type"] = "application/json"
    signed_hdr, _ = aws4_sign("POST", IMAGEX_HOST, "/", qs2, ch, payload,
                              ak, sk, sess_tok)
    url2 = "https://%s/?%s" % (IMAGEX_HOST, urllib.parse.urlencode(qs2))
    st, raw = http(url2, "POST", payload, signed_hdr, args.verbose)
    d = json.loads(raw.decode("utf-8", "replace"))
    results = d.get("Result", {}).get("PluginResult", [])
    if not results:
        print("    FAILED  %s" % raw[:400])
        return 1
    r0 = results[0]
    image_uri = r0.get("ImageUri")
    print("    OK  ImageUri=%s" % image_uri)
    print("        %dx%d  %d bytes" % (r0.get("ImageWidth", 0),
                                       r0.get("ImageHeight", 0),
                                       r0.get("ImageSize", len(png))))

    # -- step 5: send it in a chat and see if Doubao can actually see it --
    print("\n[5] chat/completion WITH the image attached")
    import uuid
    import random
    # The chat endpoint needs the full device-parameter query string; without it
    # the gateway answers 710022002 "block" even though the cookie is valid.
    dev = {"device_id": str(random.randint(10 ** 18, 10 ** 19 - 1)),
           "tea_uuid": str(random.randint(10 ** 18, 10 ** 19 - 1)),
           "web_id": ""}
    # Ask the logged-in homepage for a real web_id first.
    st_home, raw_home = http(BASE + "/chat/", "GET", None,
                             {"user-agent": UA, "cookie": cookie})
    m = __import__("re").search(
        r'["\']web_id["\']\s*[:=]\s*["\'](\d{10,25})["\']',
        raw_home.decode("utf-8", "replace"))
    dev["web_id"] = m.group(1) if m else str(
        random.randint(10 ** 18, 10 ** 19 - 1))

    attachment = {
        "key": image_uri,
        "name": "shot.png",
        "type": "image",
        "file_review_state": 3,
        "file_parse_state": 3,
        "identifier": str(uuid.uuid4()),
        "option": {"height": r0.get("ImageHeight", h),
                   "width": r0.get("ImageWidth", w)},
        "md5": hashlib.md5(png).hexdigest(),
        "size": len(png),
    }
    body = {
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
            "content": json.dumps({"text": "这张图里是什么图案？只回答颜色和形状。"},
                                  ensure_ascii=False),
            "content_type": 2001,
            "attachments": [attachment],
            "references": [],
        }],
        "ext": {},
    }
    q = "&".join([
        "aid=497858", "device_id=" + dev["device_id"],
        "device_platform=web", "language=zh", "pc_version=3.17.3",
        "pkg_type=release_version", "real_aid=497858", "region=CN",
        "samantha_web=1", "sys_region=CN", "tea_uuid=" + dev["tea_uuid"],
        "use-olympus-account=1", "version_code=20800",
        "web_id=" + dev["web_id"],
    ])
    ch2 = dict(common)
    ch2["content-type"] = "application/json"
    ch2["cookie"] = cookie
    ch2["x-flow-trace"] = json.dumps({"trace_id": uuid.uuid4().hex,
                                      "span_id": uuid.uuid4().hex})
    ch2["agw-js-conv"] = "str"
    st, raw = http(BASE + "/samantha/chat/completion?" + q, "POST",
                   json.dumps(body).encode(), ch2, args.verbose)
    txt = raw.decode("utf-8", "replace")
    print("    http=%s  %d bytes" % (st, len(raw)))

    # pull the assistant text out of the SSE frames
    answer = ""
    for frame in txt.replace("\r\n", "\n").split("\n"):
        if not frame.startswith("data:"):
            continue
        try:
            outer = json.loads(frame[5:].strip())
        except Exception:
            continue
        ev = outer.get("event_data", "")
        if isinstance(ev, str) and ev:
            try:
                ev = json.loads(ev)
            except Exception:
                ev = {}
        msg = (ev or {}).get("message", {})
        if not isinstance(msg, dict):
            msg = {}
        c = msg.get("content", "")
        if isinstance(c, str) and c:
            try:
                c = json.loads(c).get("text", c)
            except Exception:
                pass
        if c and msg.get("content_type") in (2001, None):
            answer += c
    if answer:
        print("    豆包的回答: %s" % answer[:400])
        print("\n>>> 图片上传 + 识图 全链路通过")
        return 0
    print("    (no assistant text found)")
    print("    %s" % txt[:1500])
    return 1


if __name__ == "__main__":
    sys.exit(main())
