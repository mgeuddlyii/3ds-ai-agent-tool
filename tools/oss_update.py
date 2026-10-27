#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""把 dist/update/ 推到阿里云 OSS，让 3DS 上的「检查更新」能拿到新版本。

为什么自己签（不用 oss2 SDK）
-----------------------------
这台机器上装 SDK 要走 pip（还可能被卡巴斯基的 HTTPS 扫描干扰），
而 OSS 的 V1 签名只有十几行：base64(HMAC-SHA1(secret, StringToSign))。
用标准库就能做，不引入依赖，脚本也更容易审。

密钥从哪来
----------
`E:/Gudyi/ai space/acrac.com/新建 文本文档.txt`（用户手写的记事本）。
**绝不写进本仓库**：这个脚本只在运行时读它。

用法
----
    python tools/oss_update.py --list                # 只列 aiconsole/ 下的文件
    python tools/oss_update.py --upload             # 上传 dist/update/ 全部
    python tools/oss_update.py --verify             # 拉一次公开 manifest 打印版本

    # 换版本目录 / 前缀
    python tools/oss_update.py --upload --dir dist/update --prefix aiconsole
"""

import argparse
import base64
import hashlib
import hmac
import mimetypes
import os
import sys
import urllib.error
import urllib.request
from datetime import datetime, timezone

# ---- 常量：桶与地域（与 shared/ota.c 的 kUpdateBases 第二源一致）----------
BUCKET = "acrachongkong"
ENDPOINT = "%s.oss-cn-hongkong.aliyuncs.com" % BUCKET
KEY_FILE = r"E:/Gudyi/ai space/acrac.com/新建 文本文档.txt"
DIST = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
                    "dist", "update")


def read_creds(path=KEY_FILE):
    """从记事本读 AK/SK。文件是手写的，容忍 ':' '：' '=' 与多余空格。"""
    ak = sk = None
    want = None                       # 上一行是标签时，这一行就是它的值
    with open(path, encoding="utf-8", errors="replace") as fh:
        for line in fh:
            s = line.strip()
            if not s:
                continue

            # 这个记事本是**标签一行、值下一行**的写法：
            #     AccessKey ID
            #     LTAI5t...
            #     （空行）
            #     AccessKey Secret
            #     oRGN3Q...
            # 也兼容「标签: 值」写在同一行。
            if want:
                if want == "ak":
                    ak = s
                else:
                    sk = s
                want = None
                continue

            for sep in (":", "：", "="):
                if sep in s:
                    k, v = s.split(sep, 1)
                    k = k.strip().lower().replace(" ", "")
                    v = v.strip()
                    if v:
                        if k in ("accesskeyid", "accesskey", "id"):
                            ak = v
                        elif k in ("accesskeysecret", "secret"):
                            sk = v
                        continue
                    s = k
                    break

            k = s.lower().replace(" ", "").replace(":", "").replace("：", "")
            if k in ("accesskeyid", "accesskey", "id"):
                want = "ak"
            elif k in ("accesskeysecret", "secret"):
                want = "sk"
    if not ak or not sk:
        sys.exit("没从 %s 里读到 AccessKey ID / Secret" % path)
    return ak, sk


def _sign(ak, sk, verb, key, content_type="", content_md5="", date=None):
    """OSS V1 签名。key 是不带前导斜杠的对象名（空 = 桶根）。"""
    date = date or datetime.now(timezone.utc).strftime("%a, %d %b %Y %H:%M:%S GMT")
    sts = "\n".join([verb, content_md5, content_type, date, "/%s/%s" % (BUCKET, key)])
    sig = base64.b64encode(
        hmac.new(sk.encode(), sts.encode(), hashlib.sha1).digest()).decode()
    return date, "OSS %s:%s" % (ak, sig)


def request(verb, key, data=None, content_type="", ak=None, sk=None):
    url = "https://%s/%s" % (ENDPOINT, key)
    md5 = base64.b64encode(hashlib.md5(data).digest()).decode() if data else ""
    date, auth = _sign(ak, sk, verb, key, content_type, md5)
    req = urllib.request.Request(url, data=data, method=verb)
    req.add_header("Date", date)
    req.add_header("Authorization", auth)
    if content_type:
        req.add_header("Content-Type", content_type)
    if md5:
        req.add_header("Content-MD5", md5)
    try:
        with urllib.request.urlopen(req, timeout=60) as r:
            return r.status, r.read()
    except urllib.error.HTTPError as e:
        return e.code, e.read()
    except Exception as e:                       # noqa: BLE001
        return 0, str(e).encode()


def list_prefix(prefix, ak, sk):
    """列对象：GET /?prefix=...&max-keys=100（V1 签名要把子资源算进 STS）。

    这里刻意手写 query 的签名串 —— 用 + 拼 querystring 时顺序必须和
    StringToSign 里的一致，否则会拿到 SignatureDoesNotMatch，
    而那个错误看起来像"密钥错了"，很容易误判。
    """
    # ⚠️ 这里原来写的是 ENDPOINT + "/" + q.lstrip("?") —— lstrip 把 '?' 去掉了，
    # 发出去的其实是 /max-keys=100&prefix=... 这种**没有问号**的路径，
    # OSS 当成对象名去签，于是必然 SignatureDoesNotMatch（看起来像密钥错）。
    q = "?max-keys=100&prefix=%s" % prefix
    url = "https://%s%s" % (ENDPOINT, q)
    date = datetime.now(timezone.utc).strftime("%a, %d %b %Y %H:%M:%S GMT")
    sts = "\n".join(["GET", "", "", date, "/%s/%s" % (BUCKET, q)])
    sig = base64.b64encode(
        hmac.new(sk.encode(), sts.encode(), hashlib.sha1).digest()).decode()
    req = urllib.request.Request(url, method="GET")
    req.add_header("Date", date)
    req.add_header("Authorization", "OSS %s:%s" % (ak, sig))
    try:
        with urllib.request.urlopen(req, timeout=60) as r:
            return r.status, r.read().decode("utf-8", "replace")
    except urllib.error.HTTPError as e:
        return e.code, e.read().decode("utf-8", "replace")
    except Exception as e:                       # noqa: BLE001
        return 0, str(e)


def guess_type(path):
    if path.endswith((".json",)):
        return "application/json"
    if path.endswith((".html", ".htm")):
        return "text/html; charset=utf-8"
    return mimetypes.guess_type(path)[0] or "application/octet-stream"


def upload_dir(local_dir, prefix, ak, sk):
    names = sorted(os.listdir(local_dir))
    if not names:
        sys.exit("%s 是空的（先跑 tools/make_update.py）" % local_dir)
    ok = 0
    for n in names:
        p = os.path.join(local_dir, n)
        if not os.path.isfile(p):
            continue
        body = open(p, "rb").read()
        key = "%s/%s" % (prefix, n) if prefix else n
        ct = guess_type(p)
        st, resp = request("PUT", key, body, ct, ak, sk)
        if st in (200, 201):
            print("  ✓ %-18s %8d bytes  %s" % (n, len(body), ct))
            ok += 1
        else:
            print("  ✗ %-18s HTTP %s %s" % (n, st, resp[:160]))
    return ok


def verify(prefix):
    """从**公网**拉 manifest —— 这才等于 3DS 看到的东西。"""
    url = "https://%s/%s/manifest.json" % (ENDPOINT, prefix)
    try:
        with urllib.request.urlopen(url, timeout=45) as r:
            body = r.read().decode("utf-8", "replace")
        import json
        m = json.loads(body)
        print("公开 manifest：version_name=%s version_code=%s 大小=%d bytes"
              % (m.get("version_name"), m.get("version_code"), len(body)))
        return True
    except Exception as e:                       # noqa: BLE001
        print("拉公开 manifest 失败：%s" % e)
        return False


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--dir", default=DIST)
    # ★ 前缀必须带 **channel 段**：程序取的是 `<base>/<channel>/manifest.json`
    #   （见 shared/ota.c 的 ota_fetch_remote：path = "%s/manifest.json", channel）。
    #   只传到 aiconsole/ 的话，设备照样读到旧的 stable/manifest.json ——
    #   表现为"我明明传了，机器还说线上是 0.99"。2026-10-27 就这么绕过一次。
    ap.add_argument("--prefix", default="aiconsole/stable")
    ap.add_argument("--list", action="store_true")
    ap.add_argument("--upload", action="store_true")
    ap.add_argument("--verify", action="store_true")
    a = ap.parse_args()

    ak, sk = read_creds()
    print("凭据：%s…（来自记事本，未写盘）" % ak[:6])

    if a.list:
        st, body = list_prefix(a.prefix + "/", ak, sk)
        print("LIST → HTTP %s" % st)
        if st == 200:
            import re
            for m in re.finditer(r"<Key>(.*?)</Key>.*?<Size>(\d+)</Size>", body, re.S):
                print("   %-28s %8s bytes" % (m.group(1), m.group(2)))
        else:
            print(body[:300])
    if a.upload:
        print("上传 %s → oss://%s/%s/" % (a.dir, BUCKET, a.prefix))
        n = upload_dir(a.dir, a.prefix, ak, sk)
        print("完成 %d 个文件" % n)
    if a.verify:
        verify(a.prefix)
    if not (a.list or a.upload or a.verify):
        ap.print_help()


if __name__ == "__main__":
    main()
