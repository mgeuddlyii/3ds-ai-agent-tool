"""
stage_cia.py -- 把一个 .cia 放进虚拟 SD 的 pkgs/ 目录，并算出 sha256

用途
----
真机验证（真机验证_CIA安装.md）和 Azahar 里的手动排查都要先做这一步：
把一个包放进 sdmc:/3ds/aiconsole/pkgs/<名字>.cia。

它只做三件事，没有玄机：
  1. 拷文件（目标目录不存在就建）
  2. 算源文件的 sha256 —— 这是**给护栏用的期望值**
  3. 打印出来，顺便提醒怎么故意造一个"摘要不符"

为什么值得写成一个脚本而不是让人手动拷：
  · 包名有规矩（不能带路径分隔符），拷错了目录会得到"找不到这个包"，
    而那个错误信息不会告诉你"你拷到别处了"。
  · sha256 要手算，容易抄错一位 —— 而抄错一位的结果恰好是
    "护栏正确地拒绝了正确的包"，看起来像 bug。

用法
----
    python tools/stage_cia.py app/AIConsole.cia                # 名字取 selftest
    python tools/stage_cia.py app/AIConsole.cia --name mygame  # 指定名字
    python tools/stage_cia.py app/AIConsole.cia --sdmc D:/sdmc # 指定 SD 根

退出码：0 成功；1 参数/文件问题。
"""

import argparse
import hashlib
import os
import shutil
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# 与 shared/pkg_cia.c 的 CIA_DIR 保持一致。这里**故意重复一遍**而不是
# 从 C 源码里读：读源码要正则解析，反而更脆；而这个路径是产品行为的一部分，
# 改它就要同时改两处 —— 让"改一处忘了另一处"变成一个显式的动作。
PKGS_REL = os.path.join("3ds", "aiconsole", "pkgs")

# 与 pkg_cia_name_ok 同判据：只防路径穿越。
BAD_CHARS = set('/\\:*?"<>|')


def default_sdmc():
    """猜一个 SD 根。先看 Azahar 的虚拟 SD，再看是不是插着真卡。"""
    cands = [
        os.path.join(r"E:\Azahar\azahar-windows-msvc-2126.1.1", "user", "sdmc"),
    ]
    # 让用户自己指定永远优先；这里只是提供一个"多半对"的默认值。
    for c in cands:
        if os.path.isdir(c):
            return c
    return None


def name_ok(n):
    if not n or len(n) > 60:
        return False
    if any(ch in BAD_CHARS or ord(ch) < 0x20 for ch in n):
        return False
    if n in (".", ".."):
        return False
    return True


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("cia", help="要放入的 .cia 文件路径")
    ap.add_argument("--name", default="selftest",
                    help="包名（不带 .cia 后缀），默认 selftest")
    ap.add_argument("--sdmc", default=None,
                    help="SD 卡根目录；默认自动猜（Azahar 的虚拟 SD）")
    ap.add_argument("--suffix", action="store_true",
                    help="允许 --name 里带 .cia 后缀")
    args = ap.parse_args()

    src = args.cia
    if not os.path.isfile(src):
        print("FAIL: 找不到 %s" % src)
        print("      先构建：python tools/build.py cia")
        return 1

    name = args.name
    if args.suffix and name.lower().endswith(".cia"):
        name = name[:-4]
    if not name_ok(name):
        print("FAIL: 包名不合法：%r" % args.name)
        print("      不能含 / \\ : * ? \" < > | 或控制字符，不能是 . / ..")
        return 1

    sdmc = args.sdmc or default_sdmc()
    if not sdmc:
        print("FAIL: 猜不到 SD 根目录，请用 --sdmc 指定")
        print("      （真机就是把卡的根目录指过来，比如 --sdmc E:/）")
        return 1
    if not os.path.isdir(sdmc):
        print("FAIL: SD 根目录不存在：%s" % sdmc)
        return 1

    dst_dir = os.path.join(sdmc, PKGS_REL)
    dst = os.path.join(dst_dir, name + ".cia")

    size = os.path.getsize(src)

    # 算 sha256 —— 这是护栏的期望值。
    h = hashlib.sha256()
    with open(src, "rb") as f:
        while True:
            blk = f.read(65536)
            if not blk:
                break
            h.update(blk)
    digest = h.hexdigest()

    os.makedirs(dst_dir, exist_ok=True)
    shutil.copyfile(src, dst)

    if not os.path.isfile(dst) or os.path.getsize(dst) != size:
        print("FAIL: 拷贝后大小不一致（源 %d）" % size)
        return 1

    print("源     : %s" % src)
    print("大小   : %d 字节 (%.2f MB)" % (size, size / 1048576.0))
    print("落点   : %s" % dst)
    print()
    print("sha256 : %s" % digest)
    print()
    print("这个摘要是**正确**的那个。两种用法：")
    print("  · 想验证「正常装」：让 AI 用这个摘要，或干脆不给摘要（走可选参数那条路）")
    print("  · 想验证「护栏拦得住」：把上面 64 位**改掉任意一位**再喂给 AI，")
    print("    正确行为是拒绝安装并说明摘要不符 —— 那才是护栏在工作。")
    print()
    print("真机：把 %s 整个目录拷到卡上同样位置即可。" % PKGS_REL)
    return 0


if __name__ == "__main__":
    sys.exit(main())
