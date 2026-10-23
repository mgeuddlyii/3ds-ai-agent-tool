#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""豆包网页版 Cookie 通道 —— 可行性探测。

背景
----
3DS 想用「自己的豆包账号 + 网页 Cookie」直连 doubao.com，成败取决于一件事：
服务端到底要不要校验那个 a_bogus 签名。

公开信息是矛盾的：
  * doubao-2api / doubao2API：必须开真浏览器执行 JS 才能算出签名
  * 2025doubao-free-api：声称只校验格式，随机串就能过

争不出来，那就实测。这个脚本在本机跑（不需要 3DS），把三种打法各试一遍，
结果直接告诉你这条路能不能走。

用法
----
1. 浏览器打开 https://www.doubao.com 登录
2. F12 -> Network -> 随便发一条消息 -> 点 chat/completion 那个请求
3. Request Headers 里找到 cookie:，整条复制出来
4. 粘贴到本目录的 doubao_cookie.txt（脚本会创建模板）
5. 跑：  python tools/probe_doubao.py

注意：Cookie 是敏感凭据，脚本只在本机使用，不联网报送任何东西。
      测完建议把 doubao_cookie.txt 删掉。
"""

import json
import os
import random
import shlex
import string
import subprocess
import sys
import urllib.error
import urllib.request

COOKIE_FILE = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                           "doubao_cookie.txt")

UA = ("Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
      "(KHTML, like Gecko) Chrome/131.0.0.0 Safari/537.36")

BASE = "https://www.doubao.com"


def http_post(path, cookie, query="", body=None, extra_headers=None,
              fake_sig=False):
    """打一次请求，返回 (ok, http_code, snippet)。"""
    url = BASE + path
    if query:
        url += "?" + query
    if fake_sig:
        # 2025doubao-free-api 声称的「只校验格式」打法
        a_bogus = "mf-" + rnd(34) + "-" + rnd(6)
        url += ("&" if "?" in url else "?") + "a_bogus=" + a_bogus

    headers = {
        "User-Agent": UA,
        "Accept": "application/json, text/plain, */*",
        "Accept-Language": "zh-CN,zh;q=0.9",
        "Referer": "https://www.doubao.com/chat/",
        "Origin": BASE,
        "agw-js-conv": "str, str",
    }
    if cookie:
        headers["Cookie"] = cookie
    if extra_headers:
        headers.update(extra_headers)

    data = json.dumps(body).encode("utf-8") if body is not None else None
    if data:
        headers["Content-Type"] = "application/json"

    req = urllib.request.Request(url, data=data, headers=headers,
                                 method="POST")
    try:
        with urllib.request.urlopen(req, timeout=30) as r:
            raw = r.read(4096).decode("utf-8", "replace")
            return True, r.status, raw
    except urllib.error.HTTPError as e:
        raw = ""
        try:
            raw = e.read(4096).decode("utf-8", "replace")
        except Exception:
            pass
        return False, e.code, raw
    except Exception as e:
        return False, 0, "%s: %s" % (type(e).__name__, e)


def sign_query(cmd, query, ua):
    """把 query 交给外部签名器，换回真的 a_bogus。

    为什么不在这里直接实现算法：那份去混淆的 JS 是第三方逆向产物，
    让它以文本文件形式放在你自己指定的位置、由你自己决定要不要跑，
    比我把不明代码塞进项目里更干净。

    cmd 里可以用 {query} 和 {ua} 两个占位符，例如：
        --sign-cmd "node tools/sign_abogus.js --query {query}"
    """
    line = cmd.format(query=shlex.quote(query), ua=shlex.quote(ua))
    try:
        p = subprocess.run(line, shell=True, capture_output=True,
                           timeout=30)
    except Exception as e:
        print("  签名器调用失败：%s" % e)
        return ""
    sig = p.stdout.decode("utf-8", "replace").strip()
    if not sig:
        print("  签名器没输出（stderr: %s）"
              % p.stderr.decode("utf-8", "replace").strip()[:200])
    return sig


def rnd(n):
    pool = string.ascii_letters + string.digits
    return "".join(random.choice(pool) for _ in range(n))


def device_fp():
    return "7" + "".join(random.choice(string.digits) for _ in range(18))


QUERY_TAIL = (
    "aid=497858&device_id={dev}&device_platform=web&language=zh"
    "&pc_version=2.44.0&pkg_type=release_version&region=CN&samantha_web=1"
    "&tea_uuid={dev}&use-olympus-account=1&version_code=20800"
    "&web_id={dev}&web_tab_id={tab}"
)


def build_query(with_tokens=True):
    dev = device_fp()
    tab = rnd(36)
    q = QUERY_TAIL.format(dev=dev, tab=tab)
    if with_tokens:
        q += "&msToken=" + rnd(128)
    return q


CHAT_BODY = {
    "messages": [
        {"content": "{\"text\":\"你好\"}", "content_type": 2001,
         "role": "user"},
    ],
    "completion_option": {
        "is_reasoning": False,
        "rule_options": [],
        "stream": False,
    },
    "conversation_id": "0",
    "local_conv_id": "local_conv_" + rnd(21),
    "extra": {"bot_id": "0"},
}


def main():
    sign_cmd = os.environ.get("DOUBAO_SIGN_CMD", "")
    argv = sys.argv[1:]
    for i, a in enumerate(argv):
        if a == "--sign-cmd" and i + 1 < len(argv):
            sign_cmd = argv[i + 1]

    print("=" * 68)
    print("豆包 Cookie 通道可行性探测")
    print("=" * 68)
    if sign_cmd:
        print("外部签名器   : %s" % sign_cmd)

    if not os.path.exists(COOKIE_FILE):
        with open(COOKIE_FILE, "w", encoding="utf-8") as f:
            f.write(
                "# 把浏览器里复制出来的 cookie 原样粘贴到这里（覆盖本行）\n"
                "# 获取：F12 -> Network -> chat/completion -> "
                "Request Headers -> cookie\n"
            )
        print("\n没有 cookie。我已经创建了模板：\n  %s\n" % COOKIE_FILE)
        print("请按上面的说明把 cookie 贴进去，再跑一次。")
        return 2

    cookie = ""
    with open(COOKIE_FILE, encoding="utf-8") as f:
        for line in f:
            line = line.strip()
            if line and not line.startswith("#"):
                cookie = line
                break

    if not cookie:
        print("文件里没有有效的 cookie（只有注释？）")
        return 2

    print("\ncookie 长度 = %d，前 24 字符 = %s..."
          % (len(cookie), cookie[:24]))

    has_sess = "sessionid" in cookie
    print("含 sessionid  : %s  （没有这个基本没戏）" % ("是" if has_sess else "否"))

    print("\n" + "-" * 68)
    print("[1/4] 空 cookie 基线 —— 确认「未登录」长什么样")
    print("-" * 68)
    ok, code, body = http_post("/passport/account/info/v2", "", "account_sdk_source=web")
    print("HTTP %s" % code)
    print(snip(body))

    print("\n" + "-" * 68)
    print("[2/4] 带真 cookie —— 确认会话是否有效")
    print("-" * 68)
    ok2, code2, body2 = http_post("/passport/account/info/v2", cookie,
                                  "account_sdk_source=web")
    print("HTTP %s" % code2)
    print(snip(body2))
    session_ok = code2 == 200 and '"user"' in body2.replace(" ", "")
    print("  => 会话%s" % ("有效" if session_ok else "无效（cookie 过期或复制不全）"))

    # 打法按成本从低到高排：先试不用签名的，再试只凑格式的，
    # 最后才动用真签名器（那一环要额外准备环境和 JS）。
    ways = []
    q = build_query(with_tokens=True)

    print("\n" + "-" * 68)
    print("[3] 带 cookie + 完整设备参数，但不带签名")
    print("-" * 68)
    _, code3, body3 = http_post("/samantha/chat/completion", cookie,
                                q, CHAT_BODY)
    print("HTTP %s" % code3)
    print(snip(body3))
    if code3 == 200:
        ways.append("无签名")

    print("\n" + "-" * 68)
    print("[4] 伪造 a_bogus（只凑格式，不是真签名）")
    print("-" * 68)
    _, code4, body4 = http_post("/samantha/chat/completion", cookie,
                                q, CHAT_BODY, fake_sig=True)
    print("HTTP %s" % code4)
    print(snip(body4))
    if code4 == 200:
        ways.append("伪造签名")

    if sign_cmd:
        sig = sign_query(sign_cmd, q, UA)
        if sig:
            print("\n" + "-" * 68)
            print("[5] 外部签名器算出的真 a_bogus")
            print("-" * 68)
            _, code5, body5 = http_post("/samantha/chat/completion", cookie,
                                        q + "&a_bogus=" + sig, CHAT_BODY)
            print("HTTP %s" % code5)
            print(snip(body5))
            if code5 == 200:
                ways.append("真签名")

            # 这才是真正想要的东西：账号里的历史会话
            print("\n" + "-" * 68)
            print("[6] 拉会话列表（你要的历史就在这里）")
            print("-" * 68)
            q6 = build_query(with_tokens=True)
            sig6 = sign_query(sign_cmd, q6, UA)
            if sig6:
                _, code6, body6 = http_post("/samantha/thread/list", cookie,
                                            q6 + "&a_bogus=" + sig6,
                                            {"limit": 20})
                print("HTTP %s" % code6)
                print(snip(body6, 600))
                if code6 == 200:
                    ways.append("拉会话列表")

    print("\n" + "=" * 68)
    print("判定")
    print("=" * 68)
    if not session_ok:
        print("会话本身没通过 —— 上面各步的失败都不可信，先解决 cookie。")
    elif ways:
        print("可行打法：%s" % "、".join(ways))
        print("记下这几种 —— 3DS 端就照它实现，不需要多余的绕弯。")
    else:
        print("都没成。看 [3][4] 的返回体：")
        print("  * 提到 a_bogus / 签名 => 必须真签名，")
        print("     那就是 JS 引擎那条路：QuickJS + bdms_novmp.js 跑在 3DS 本地。")
        print("  * 提到风控 / 滑块 / 频率 => 认了格式但把请求挡了，")
        print("     这条路不适合放进一个要发布给别人用的软件。")
    print("\n（Cookie 是敏感凭据，测完记得删 %s）" % os.path.basename(COOKIE_FILE))
    return 0


def snip(s, n=400):
    s = (s or "").strip().replace("\n", " ")
    return s[:n] + (" ..." if len(s) > n else "")


if __name__ == "__main__":
    sys.exit(main())
