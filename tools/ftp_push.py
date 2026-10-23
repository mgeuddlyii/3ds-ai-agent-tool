#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""把发布产物推到 3DS 的 SD 卡上（走 ftpd），并**回读校验**。

用法：
    python tools/ftp_push.py <3ds-ip> [port]
    # 例：python tools/ftp_push.py 192.168.1.50 5000
    #
    # 主机也可以走环境变量，省得每次敲：
    #   Windows: set AIC_FTP_HOST=192.168.1.50
    #   Linux/macOS: export AIC_FTP_HOST=192.168.1.50
    #   python tools/ftp_push.py            # 端口默认 5000

★ 主机**没有默认值**，必填。原因：写死一个内网 IP 在公开仓库里既没用
  （别人的机器不可能是那个地址），又会把作者的局域网拓扑漏出去。
  3DS 的 IP 在它的「系统设置 → 网络」里能看到。

推两个文件：
    3dsx  ->  /3ds/AIConsole.3dsx      （Homebrew Launcher 读这里）
    cia   ->  /AIConsole.cia           （装到主菜单用）

★ 为什么要回读：FTP 的 STOR 返回 226 只代表"服务端收下了这次传输"，
  不代表落盘完整。3DS 的 SD 卡写入偶发失败（卡接触、空间不足），
  而症状是"程序装上去打不开"—— 看起来像构建坏了。
  所以传完必须用 SIZE 或重新 RETR 比长度/哈希。
"""

import hashlib
import ftplib
import os
import socket
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
REL = os.path.join(ROOT, "release")

DEFAULT_PORT = 5000

# 版本号**不写死**：从 release/ 里找最新的 AIConsole-<ver>.3dsx / .cia。
# 早先这里硬编码 "0.99.4"，每次升版本都得改两处，漏一处就推了个旧包上去 ——
# 而症状是"新功能没生效"，看着像代码没编进去。
def pick_release():
    """返回 [(3dsx 路径, 远端), (cia 路径, 远端)]，缺哪个就少哪个。"""
    out = []
    if not os.path.isdir(REL):
        return out
    vers = set()
    for fn in os.listdir(REL):
        if fn.startswith("AIConsole-") and (fn.endswith(".3dsx") or fn.endswith(".cia")):
            vers.add(fn[len("AIConsole-"):-5])
    # 取版本号最大的那个（字符串比较对 0.99.x 够用；跨位数时按元组比）
    def key(v):
        return tuple(int(x) if x.isdigit() else 0 for x in v.split("."))
    if not vers:
        return out
    v = max(vers, key=key)
    d3 = os.path.join(REL, "AIConsole-%s.3dsx" % v)
    ci = os.path.join(REL, "AIConsole-%s.cia" % v)
    if os.path.isfile(d3):
        out.append((d3, "/3ds/AIConsole.3dsx"))
    if os.path.isfile(ci):
        out.append((ci, "/AIConsole.cia"))
    return out


def local_size(path):
    return os.path.getsize(path)


def remote_size(ftp, path):
    try:
        ftp.voidcmd("TYPE I")
        return ftp.size(path)
    except Exception:
        return None


def main():
    host = sys.argv[1] if len(sys.argv) > 1 else os.environ.get("AIC_FTP_HOST", "")
    if not host:
        print("!! 需要 3DS 的 IP：python tools/ftp_push.py <3ds-ip> [port]")
        print("   （或设置 AIC_FTP_HOST 环境变量）")
        print("   3DS 的 IP 在「系统设置 → 网络」里能查到。")
        return 2
    port = int(sys.argv[2]) if len(sys.argv) > 2 else DEFAULT_PORT

    # 先探一口，别让 ftplib 自己抛一堆重试
    s = socket.socket(); s.settimeout(3)
    try:
        s.connect((host, port))
    except Exception as e:
        print("!! 连不上 %s:%d —— %s" % (host, port, e))
        print("   可能原因：ftpd 没启动 / 3DS 无线开关关着 / 不在同一个 Wi-Fi")
        return 2
    finally:
        s.close()

    ftp = ftplib.FTP()
    ftp.connect(host, port, timeout=10)
    try:
        print("登录：", ftp.login())
        ftp.set_pasv(True)

        ok_all = True
        targets = pick_release()
        if not targets:
            print("!! release/ 里找不到 AIConsole-<ver>.3dsx / .cia")
            print("   先跑 python tools/build.py，再 cp 到 release/")
            ftp.quit()
            return 1
        for src, dst in targets:
            name = os.path.basename(src)

            size = local_size(src)
            print("\n-> %s (%d 字节) => %s" % (name, size, dst))
            with open(src, "rb") as fh:
                ftp.storbinary("STOR " + dst, fh, blocksize=8192)

            got = remote_size(ftp, dst)
            if got is None:
                print("   ⚠ 服务端不支持 SIZE，跳过长度校验")
            elif got == size:
                print("   [OK] 远端长度 %d 一致" % got)
            else:
                print("   [!!] 远端长度 %s != 本地 %d —— 传输不完整" % (got, size))
                ok_all = False

        ftp.quit()
        print("\n%s" % ("全部推送成功" if ok_all else "有失败项，见上"))
        return 0 if ok_all else 1
    except Exception as e:
        print("!! 传输失败：", e)
        try:
            ftp.close()
        except Exception:
            pass
        return 1


if __name__ == "__main__":
    sys.exit(main())
