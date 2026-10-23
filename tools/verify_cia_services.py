"""
verify_cia_services.py -- 检查 AIConsole.cia 的 exheader 里到底声明了哪些服务

为什么需要这个工具
------------------
"RSF 里写了 am:u" 和 "打出来的 .cia 里的 exheader 真的带 am:u" 是两件事。
makerom 可能因为名字拼错、或者某个内部白名单而**静默忽略**一个服务项 ——
它不会报错，你只会拿到一个少了权限的包。而那个包在模拟器里
**照样能跑**（Azahar 不查 exheader），问题要到真机上才暴露。

所以：改完 RSF、打完包之后，跑一次这个。它直接读二进制，不猜。

用法
----
    python tools/verify_cia_services.py                 # 默认 app/AIConsole.cia
    python tools/verify_cia_services.py path/to.cia

判据
----
    · 期望 11 个服务，最后一个非空的是 am:u
    · 如果 am:u 不在里面 -> 退出码 1（可接进构建脚本当闸）

exheader 在 CIA 里出现两次（NCCH 的 exheader 与其副本），所以同一个服务名
通常会命中 2 次。命中 0 次才是问题。

⚠️ 一个已经踩过的坑：ServiceAccessControl 每个槽位是**定长 8 字节**，
   所以**恰好 8 个字符的服务名不带结尾符**（`hid:USER`、`gsp::Gpu` 都是）。
   一开始用 `名字 + \\x00` 去搜，这两个就报"找不到"，而包其实是对的。
   现在按槽位对齐读，不再依赖结尾符 —— 见 read_service_slots()。
"""

import os
import sys
# RSF (app/AIConsole_cia.rsf) 里声明的那 11 个，顺序即 exheader 里的顺序。
# 改 RSF 就要改这里 —— 两边不一致时本脚本会报出来，那正是它存在的意义。
EXPECTED = [
    "cfg:u", "fs:USER", "gsp::Gpu", "hid:USER", "ndm:u",
    "soc:U", "ssl:C", "http:C", "APT:U", "ptm:u",
    "am:u",
]

# 3dsx 形态会拿到的那张 Luma 硬编码表里的特征项。它出现在包里 = 包被
# 当成了 3dsx 处理，或者我们误把 3dsx 的 RSF 用在了 cia 上。
HBLDR_ONLY = ["am:net", "cam:u", "mic:u", "csnd:SND", "y2r:u"]

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def count_slot(data: bytes, name: str) -> int:
    """数一下 name 在包的 ServiceAccessControl 槽位里出现几次。

    为什么不能写成 data.count(name + b"\\x00")：
      槽位定长 8 字节，**恰好 8 字符的服务名没有结尾符**。
      `hid:USER` 和 `gsp::Gpu` 都属于这种，用带 \\x00 的模式搜它们
      会得到 0 —— 那是**工具的假阴性**，不是包的问题（踩过一次）。
      正确做法是：在 8 字节对齐的槽位上做「槽位内容 == name」的比较。
    """
    nb = name.encode()
    hits = 0
    # 槽位起点一定在 8 的倍数上（exheader 里 service_access 数组
    # 相对结构体起点对齐，而结构体起点也是 8 对齐）。用切片窗口
    # 扫一遍，不依赖任何固定偏移 —— 包结构变了也不会失效。
    for i in range(0, len(data) - 8, 8):
        slot = data[i:i + 8]
        # 槽位里要么正好是 name，要么是 name 后面补 \x00
        if slot.rstrip(b"\x00") == nb or slot == nb.ljust(8, b"\x00"):
            hits += 1
    return hits


def main():
    path = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, "app", "AIConsole.cia")
    if not os.path.exists(path):
        print("FAIL: 找不到 %s —— 先跑 python tools/build.py cia" % path)
        return 1

    data = open(path, "rb").read()
    print("包     : %s" % path)
    print("大小   : %d 字节" % len(data))
    print()

    missing = []
    print("%-10s %s" % ("服务", "在包里的出现次数"))
    print("-" * 34)
    for svc in EXPECTED:
        n = count_slot(data, svc)
        mark = "OK " if n else "!! "
        print("%s%-10s %d" % (mark, svc, n))
        if not n:
            missing.append(svc)

    print()
    # hbldr 表里的项**不该**出现在我们的 cia 里（我们的 exheader 由
    # AIConsole_cia.rsf 生成，不含它们）。出现 = 用错了 RSF。
    leaked = [s for s in HBLDR_ONLY if count_slot(data, s)]
    if leaked:
        print("!! 发现只属于 Luma 3dsx 表的服务：%s" % ", ".join(leaked))
        print("   这说明打包时用的不是 AIConsole_cia.rsf。")
        print("   检查 tools/build.py 第 6 步用的 rsf 变量。")

    print()
    if missing:
        print("FAIL: 少了 %d 个服务：%s" % (len(missing), ", ".join(missing)))
        print("      makerom 不会为这个报错 —— 它只是默默不打进去。")
        return 1

    if leaked:
        return 1

    print("PASS: %d 个服务全部就位，am:u 在包里。" % len(EXPECTED))
    print("      注意这只证明**包里声明了**，不证明**真机上系统会给**。")
    print("      后者要真机实测，见 真机验证_CIA安装.md。")
    return 0


if __name__ == "__main__":
    sys.exit(main())
