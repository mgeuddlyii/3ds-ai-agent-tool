#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""TLS 可达性探测：3DS 能不能连上这些 AI 接口？

判定三件事，任何一件过不了，3DS 端就是连不上的：
  1. 服务端还接不接受 TLS 1.2（3DS 的 sslc 最高到 1.2）
  2. 证书链的**根**是不是 3DS 内置的那批（见 sslc.h 的 SSLC_DefaultRootCert）
  3. 本机到该域名通不通（顺便看卡巴斯基有没有中间人）

注意：本机装了卡巴斯基并开了「加密连接扫描」，所有出网 HTTPS 会被重签，
所以直接 openssl 看到的 issuer 可能是 Kaspersky 而不是真实根。
脚本会检测这一点并显式标注，避免拿中间人证书下结论。

用法： python tools/probe_tls.py
"""

import json
import os
import socket
import ssl
import sys

# 3DS sslc.h SSLC_DefaultRootCert —— 系统内置的那几个根（其它都不是）
N3DS_ROOTS = {
    "Nintendo CA", "Nintendo CA - G2", "Nintendo CA - G3",
    "Nintendo Class 2 CA", "Nintendo Class 2 CA - G2", "Nintendo Class 2 CA - G3",
    "GTE CyberTrust Global Root", "AddTrust External CA Root",
    "COMODO RSA Certification Authority", "USERTrust RSA Certification Authority",
    "DigiCert High Assurance EV Root CA",
}

TARGETS = [
    ("ark(火山方舟/豆包)", "ark.cn-beijing.volces.com", 443),
    ("deepseek",           "api.deepseek.com",          443),
    ("zhipu",              "open.bigmodel.cn",          443),
    ("moonshot",           "api.moonshot.cn",           443),
    ("dashscope",          "dashscope.aliyuncs.com",    443),
    ("doubao网页",          "www.doubao.com",            443),
]


def rdn_to_str(rdn):
    """把 getpeercert 的 RDN 元组拼成 CN=... 的可读串。"""
    parts = []
    for r in rdn or []:
        for k, v in r:
            parts.append("%s=%s" % (k, v))
    return ", ".join(parts)


def probe(name, host, port):
    print("=" * 70)
    print("%-22s %s" % (name, host))
    print("=" * 70)

    # ---- 1) 解析 + TCP ----
    try:
        ip = socket.gethostbyname(host)
    except Exception as e:
        print("  DNS 失败：%s" % e)
        return None
    print("  DNS        : %s" % ip)

    try:
        s = socket.create_connection((host, port), timeout=12)
    except Exception as e:
        print("  TCP 失败   ：%s" % e)
        return None
    s.close()
    print("  TCP 443    : 通")

    # ---- 2) TLS 1.2 能不能握上（3DS 的上限）----
    for label, maxv, minv in (("TLS1.2", ssl.TLSVersion.TLSv1_2,
                               ssl.TLSVersion.TLSv1_2),
                              ("TLS1.0-1.2", ssl.TLSVersion.TLSv1_2,
                               ssl.TLSVersion.TLSv1)):
        c = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
        c.minimum_version = minv
        c.maximum_version = maxv
        c.check_hostname = False
        c.verify_mode = ssl.CERT_NONE
        try:
            with socket.create_connection((host, port), timeout=12) as sk:
                with c.wrap_socket(sk, server_hostname=host) as ss:
                    print("  %-10s : 握手成功 (%s / %s)" % (label, ss.version(),
                                                            ss.cipher()[0]))
                    break
        except Exception as e:
            print("  %-10s : 失败 %s" % (label, type(e).__name__))
    else:
        print("  => 服务端不接受 TLS1.2，3DS 直接出局")
        return None

    # ---- 3) 证书链 ----
    c = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
    c.check_hostname = False
    c.verify_mode = ssl.CERT_NONE
    chain = []
    try:
        with socket.create_connection((host, port), timeout=12) as sk:
            with c.wrap_socket(sk, server_hostname=host) as ss:
                try:
                    chain = ss.getpeercert(chain=True) or []
                except TypeError:
                    chain = []
                if not chain:
                    d = ss.getpeercert()
                    if d:
                        chain = [d]
    except Exception as e:
        print("  抓链失败：%s" % e)
        return None

    print("  链长度     : %d" % len(chain))
    mitm = False
    for i, d in enumerate(chain):
        subj = rdn_to_str(d.get("subject"))
        iss = rdn_to_str(d.get("issuer"))
        tag = "leaf" if i == 0 else ("中间" if i < len(chain) - 1 else "根?")
        print("   [%d] %s" % (i, tag))
        print("       subject : %s" % subj)
        print("       issuer  : %s" % iss)
        if "Kaspersky" in iss or "Kaspersky" in subj:
            mitm = True
        print("       notAfter: %s" % d.get("notAfter"))

    if mitm:
        print("\n  *** 检测到卡巴斯基中间人重签名：这条链不是服务端原来那条，")
        print("      issuer/根 的判断不可信，只能当『通不通』用。 ***")

    root = chain[-1].get("subject") if chain else None
    root_cn = ""
    if root:
        for r in root:
            for k, v in r:
                if k in ("commonName", "CN"):
                    root_cn = v
    print("\n  链顶（根）  : %s" % (root_cn or "(未知)"))
    if root_cn in N3DS_ROOTS:
        print("  => 根在 3DS 内置列表里 —— 有希望")
    else:
        print("  => 根 **不在** 3DS 内置的 11 个根里")
    return {"host": host, "root": root_cn, "in_3ds": root_cn in N3DS_ROOTS,
            "mitm": mitm, "chain": len(chain)}


def main():
    print("\n3DS 内置根（sslc.h SSLC_DefaultRootCert）：")
    for r in sorted(N3DS_ROOTS):
        print("   - %s" % r)
    print()

    out = []
    for name, host, port in TARGETS:
        r = probe(name, host, port)
        if r:
            r["name"] = name
            out.append(r)
        print()

    print("=" * 70)
    print("汇总")
    print("=" * 70)
    print("%-22s %-40s %s" % ("通道", "根证书", "3DS内置?"))
    for r in out:
        print("%-22s %-40s %s%s" % (r["name"], (r["root"] or "?")[:40],
                                    "是" if r["in_3ds"] else "否",
                                    "  (被卡巴斯基重签)" if r["mitm"] else ""))
    p = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                     "tls_probe_result.json")
    with open(p, "w", encoding="utf-8") as f:
        json.dump(out, f, ensure_ascii=False, indent=1)
    print("\n结果已存 %s" % p)
    return 0


if __name__ == "__main__":
    sys.exit(main())
