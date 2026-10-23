#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
build_manual.py —— 产出 **系统说明书 CFA**（HOME 菜单左下角「?」那个）

── 这一层做什么 ──
    输入：app/manual/root/Manual.bcma   （说明书内容本体）
    步骤：1. mkromfs3ds 打成裸 RomFS（level 3）
          2. mk_romfs_ivfc.py 套 IVFC 哈希树外壳
          3. makerom -f ncch 打成 CFA（RSF 里 ContentType: Manual）
          4. 逐字段校验产物
    输出：app/manual.cfa

── 为什么要自己套 IVFC ──
`mkromfs3ds` 只出裸 level-3，`makerom -romfs` 只收套了 IVFC 的镜像
（喂裸的报 `Invalid RomFS Binary`）。详见 mk_romfs_ivfc.py 文件头。

── 占位 BCMA（当前状态，务必读懂）──
**真正的 `Manual.bcma` 还没有。** 它是 DARC 归档 + LZ10 压缩的 DARC 子归档，
页面是 **BCLYT 二进制布局**，社区没有成熟编辑器（见 docs/内置说明书方案.md §2）。
所以这里先放一个**占位文件**，用途只有一个：

    **go/no-go 探针** —— 把带 content 0001 的 CIA 装到真机，
    看 HOME 菜单选中这个标题时，左下角到底出不出现「?」。

- 「?」出现 + 点开报错 → **go**：说明系统认了我们的说明书 content，
  值得投入去写 BCLYT 生成器。
- 「?」根本不出现 → **no-go**：自制 test-signed title 不被当成"有说明书的
  标题"，路线 A 到此为止，改做 App 内说明书屏（路线 B）。

⚠️ 占位 BCMA 被点开时**大概率会报错或白屏**，这是预期行为，不是 bug。
   要换成真内容时，把 `app/manual/root/Manual.bcma` 替换掉即可，
   本脚本其余部分不用改。

── 用法 ──
    python tools/manual/build_manual.py            # 产出 app/manual.cfa
    python tools/manual/build_manual.py --check    # 只校验已有产物
"""

import argparse
import os
import struct
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))          # 项目根 3ds-ai/
APP = os.path.join(ROOT, "app")

MANUAL_DIR = os.path.join(APP, "manual")
MANUAL_ROOT = os.path.join(MANUAL_DIR, "root")
BCMA = os.path.join(MANUAL_ROOT, "Manual.bcma")
IVFC = os.path.join(MANUAL_DIR, "manual_ivfc.bin")
CFA = os.path.join(APP, "manual.cfa")
RSF = os.path.join(APP, "manual.rsf")

MKROMFS = r"E:\devkitPro\tools\bin\mkromfs3ds.exe"
MAKEROM = r"E:\devkitPro\tools\bin\makerom.exe"

PLACEHOLDER = (b"AI CONSOLE MANUAL PLACEHOLDER\n"
               b"This file stands in for Manual.bcma so we can test whether\n"
               b"the HOME menu shows a manual button for a homebrew title.\n"
               b"It is NOT a valid BCMA archive and the manual applet will\n"
               b"almost certainly fail to render it. That is expected.\n")

# RSF 要点说明见 docs/内置说明书方案.md §1.2 / §3.3-5：
#   ContentType: Manual 必须写，makerom 会据此把 NCCH flags 的
#   Content Type 置成 2（Instruction Manual），不用事后补字节。
RSF_TEMPLATE = """\
# 系统说明书 CFA 的 RSF —— 由 tools/manual/build_manual.py 生成，别手改
#
# 与主程序的 AIConsole_cia.rsf 的关系：UniqueId 必须**完全相同**，
# 否则说明书 content 挂不到这个 title 上。改 title id 时两处一起改。
#
# ProductCode 固定 CTR-P-CTAP —— 零售说明书的标配产品码
# （见 3dbrew CFA 条目；makerom 会原样写进 NCCH 头 0x150）。

BasicInfo:
  Title                   : {title}
  CompanyCode             : "00"
  ProductCode             : CTR-P-CTAP
  ContentType             : Manual
  Logo                    : Homebrew

TitleInfo:
  Category                : Application
  UniqueId                : {unique_id}

Option:
  UseOnSD                 : true
  FreeProductCode         : true
  MediaFootPadding        : false
  EnableCrypt             : false

AccessControlInfo:
  CoreVersion             : 2
  DescVersion             : 2
  ReleaseKernelMajor      : "02"
  ReleaseKernelMinor      : "33"
  MemoryType              : Application
  SystemMode              : 64MB
  CpuSpeed                : 804mhz
  IdealProcessor          : 0
  AffinityMask            : 1
  Priority                : 16
  MaxCpu                  : 0
  HandleTableSize         : 0
  DisableDebug            : false
  EnableForceDebug        : false
  CanWriteSharedPage      : false
  CanUsePrivilegedPriority: false
  CanUseNonAlphabetAndNumber: true
  PermitMainFunctionArgument: true
  CanShareDeviceMemory    : false
  RunnableOnSleep         : false
  SpecialMemoryArrange    : false
  SystemCallAccess:
    ArbitrateAddress: 34
"""


def read_unique_id():
    """从 AIConsole_cia.rsf 里抠出 UniqueId —— **同源**，绝不在这里另写一份。

    ★ 两个 content 的 UniqueId 不一致 = 说明书挂不上；这类错构建期不报，
      只有真机点「?」没反应，排查成本极高。所以必须从唯一事实源读。
    """
    p = os.path.join(APP, "AIConsole_cia.rsf")
    if not os.path.exists(p):
        return None, "找不到 %s" % p
    import re
    for line in open(p, encoding="utf-8", errors="replace"):
        m = re.match(r"\s*UniqueId\s*:\s*(0x[0-9A-Fa-f]+|\d+)", line)
        if m:
            return m.group(1), None
    return None, "AIConsole_cia.rsf 里没有 UniqueId"


def ensure_rsf(unique_id):
    text = RSF_TEMPLATE.format(title="Manual", unique_id=unique_id)
    need = True
    if os.path.exists(RSF):
        try:
            old = open(RSF, encoding="utf-8").read()
            need = (old != text)
        except Exception:
            need = True
    if need:
        os.makedirs(os.path.dirname(RSF), exist_ok=True)
        with open(RSF, "w", encoding="utf-8", newline="\n") as f:
            f.write(text)
        print("   RSF  : 已生成 app/manual.rsf（UniqueId %s）" % unique_id)
    return True


def ensure_bcma():
    os.makedirs(MANUAL_ROOT, exist_ok=True)
    if os.path.exists(BCMA):
        sz = os.path.getsize(BCMA)
        if sz <= len(PLACEHOLDER):
            print("   BCMA : 占位文件（%d 字节）—— **只是 go/no-go 探针**，"
                  "点开「?」会报错属预期" % sz)
        else:
            print("   BCMA : %s（%d 字节）" % (os.path.basename(BCMA), sz))
        return True
    with open(BCMA, "wb") as f:
        f.write(PLACEHOLDER)
    print("   BCMA : 已写入占位文件 app/manual/root/Manual.bcma（%d 字节）"
          % len(PLACEHOLDER))
    print("          ⚠️ 真 Manual.bcma 尚未制作，这一趟只为验证「?」出不出现")
    return True


def run(cmd, what):
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0:
        print("!! %s 失败 rc=%s" % (what, r.returncode))
        for ln in (r.stdout + r.stderr).strip().splitlines()[-8:]:
            print("     " + ln)
        return False
    tail = (r.stdout + r.stderr).strip().splitlines()
    if tail:
        for ln in tail[-3:]:
            print("     " + ln)
    return True


def check_cfa(path):
    """校验 CFA 头：magic / 产品码 / Content Type / 程序 ID / IVFC。"""
    if not os.path.exists(path):
        print("!! 找不到 %s" % path)
        return False
    b = open(path, "rb").read()
    ok = True

    def bad(msg):
        nonlocal ok
        ok = False
        print("   ✗ " + msg)

    print("   CFA  : %s（%d 字节）" % (os.path.basename(path), len(b)))
    if b[0x100:0x104] != b"NCCH":
        bad("0x100 不是 NCCH magic（实际 %r）" % b[0x100:0x104])
    pc = b[0x150:0x160].rstrip(b"\x00").decode("ascii", "replace")
    if pc != "CTR-P-CTAP":
        bad("产品码应为 CTR-P-CTAP，实际 %r" % pc)
    else:
        print("   ✓ 产品码 CTR-P-CTAP")
    flag = b[0x18D]
    ctype = flag >> 2
    if ctype != 2:
        bad("ncchflag[5]@0x18D=0x%02X → ContentType=%d，应为 2（说明书）"
            % (flag, ctype))
    else:
        print("   ✓ ContentType=2（Instruction Manual），ncchflag[5]=0x%02X" % flag)
    off = b.find(b"IVFC")
    if off < 0:
        bad("找不到 IVFC 哈希树头 —— RomFS 没套壳")
    else:
        l3sz, = struct.unpack_from("<Q", b, off + 0x44)
        print("   ✓ IVFC @0x%X，Level3 size=0x%X" % (off, l3sz))
    return ok


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--check", action="store_true", help="只校验已有 app/manual.cfa")
    args = ap.parse_args()

    if args.check:
        return 0 if check_cfa(CFA) else 1

    print("手册 : 开始打包系统说明书 CFA")
    uid, err = read_unique_id()
    if uid is None:
        print("!! " + err)
        return 1
    ensure_rsf(uid)
    if not ensure_bcma():
        return 1

    if not os.path.exists(MKROMFS):
        print("!! 找不到 mkromfs3ds：%s" % MKROMFS)
        return 1
    if not os.path.exists(MAKEROM):
        print("!! 找不到 makerom：%s" % MAKEROM)
        return 1

    # 1+2) 目录 -> IVFC 包裹的 RomFS（mk_romfs_ivfc.py 内部会自校验）
    mk = os.path.join(HERE, "mk_romfs_ivfc.py")
    if not run([sys.executable, mk, MANUAL_ROOT, IVFC], "RomFS + IVFC"):
        return 1

    # 3) CFA
    if not run([MAKEROM, "-f", "ncch", "-target", "t",
                "-o", CFA, "-romfs", IVFC, "-rsf", RSF], "makerom -> manual.cfa"):
        return 1

    # 4) 校验
    if not check_cfa(CFA):
        print("!! 说明书 CFA 校验失败 —— 不要拿它去打包 CIA")
        return 1
    print("手册 : 就绪")
    return 0


if __name__ == "__main__":
    sys.exit(main())
