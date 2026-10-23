"""
Build driver for the 3DS AI Console app (P0).

Why this exists next to the Makefile: it drives devkitARM directly, so the
project builds even before MSYS2's `make` is installed. Both paths produce the
same artefacts.

Usage
-----
    python build.py             compile + link + .smdh + .3dsx
    python build.py cia         ... and additionally package a .cia
    python build.py clean       delete the build directory

Artefacts land in  app/  (same location the Makefile uses):
    AIConsole.elf    AIConsole.smdh    AIConsole.3dsx    AIConsole.cia
Object files stay in app/build/.

⚠️ 两个产物形态，权限不同，别混淆
----------------------------------
    3dsx  -> app/AIConsole.3dsx   **开发与自动验收用**
             没有 exheader。Luma3DS 加载它时走 hbldr.c: hbldrPatchExHeaderInfo(),
             那里 memset 清零后 memcpy 一张写死的 34 项服务表（无 am:u）。
             所以 AIConsole.rsf 里声明的服务对它是**无效**的。
             tools/azahar_test.py 的 A–S 十九趟全部跑这个形态。

    cia   -> app/AIConsole.cia    **能装到主菜单的形态**
             自带 exheader。Luma3DS 的判据 hbldrIs3dsxTitle(title_id) 比的是
             可配置的 hbldr_tid，我们的 0xA1C01 不等于它 -> 走普通 title 分支
             -> 用 **AIConsole_cia.rsf** 里声明的服务（含 am:u）。
             这个形态**不能被 azahar_test.py 脚本驱动**（装到主菜单后没有
             可注入按键的入口），验证靠 --keep + 手工点按。

    两份 RSF 分开是故意的：往 3dsx 那份加 am:u 不生效只会误导人，
    往 cia 那份漏掉 am:u 则"AI 装程序"在真机上直接权限失败。
"""

import os
import shutil
import subprocess
import sys

# 桌面图标要开「允许立体 3D」位（smdhtool --create 不暴露这个开关，
# 见 tools/patch_smdh_3d.py）。SMDH 的 flags@0x2028 bit 0x0002 = Allow 3D，
# 不置位的话 home-menu 图标只是张平面方块，推 3D 滑杆没反应 ——
# 用户要的「桌面上那个 3D 模型」特色就出不来。这里在每次 smdhtool 之后
# 自动把位拨开，幂等，且不会影响 smdhtool 已经写好的其它标志位。
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
try:
    from patch_smdh_3d import set_3d_flag as _set_smdh_3d
    from patch_smdh_3d import set_extended_banner_flag as _set_ext_banner
except Exception:
    _set_smdh_3d = None
    _set_ext_banner = None

# 标题承重墙：SMDH 写完立刻按 UTF-16LE 逐字节验 16 个语言槽。
# 为什么必须有这一步：smdhtool 写坏字节能一样返回 rc=0，而"能不能解出可读字符"
# 这种宽松判据对空标题/截断都返回 True（= 永远绿的守卫）。check_title 比的是
# **与期望 UTF-16LE 的逐字节相等**，四种坏法（latin-1 乱码/截断/单槽错/图标空）
# 都被 `python tools/smdhdump.py --selftest` 证过会红。
try:
    from smdhdump import check_title as _smdh_check
except Exception:
    _smdh_check = None

# --------------------------------------------------------------------------
DKP    = r"E:\devkitPro"
GCC    = os.path.join(DKP, "devkitARM", "bin", "arm-none-eabi-gcc.exe")
TOOLS  = os.path.join(DKP, "tools", "bin")
CTRU   = os.path.join(DKP, "libctru")
PORTLIBS = os.path.join(DKP, "portlibs", "3ds")

# this script lives in <project root>/tools/
ROOT   = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
APP    = os.path.join(ROOT, "app")
SHARED = os.path.join(ROOT, "shared")

# 中间产物目录。**可被环境变量覆盖**（AIC_BUILD_DIR）。
#
# 为什么要留这个口子：两个会话同时在这个仓库上干活时，共用 app/build/
# 会互相拆台 —— 一方在 link，另一方在重编同一个 .o，于是出现
# "reopening xxx.o: Permission denied" 或者"cannot find xxx.o"。
# 更坏的情况是链接到**一半旧一半新**的 .o，产出一个没人能解释的二进制。
# 各用各的目录就彻底隔离了。
# 不设该变量时行为与从前完全一致（默认还是 app/build），
# 所以这个口子对单会话照常透明。
BUILD  = os.environ.get("AIC_BUILD_DIR") or os.path.join(APP, "build")
OUT    = APP                          # final artefacts - same place as the Makefile

ARCH   = ["-march=armv6k", "-mtune=mpcore", "-mfloat-abi=hard", "-mtp=soft"]
CFLAGS = ["-g", "-Wall", "-O2", "-mword-relocations", "-ffunction-sections"] + ARCH

# 允许从环境注入额外的编译宏（空格分隔）。
# 用途：验收时把 OTA 更新源临时指到本机的测试服务，**不必改源码再改回来** ——
# 那种"改回去忘了"的事故在配置文件里几乎必然发生一次。
for _m in os.environ.get("AIC_EXTRA_CFLAGS", "").split():
    CFLAGS.append(_m)

# AGENTDOC_EMBEDDED 只在 shared/agentdoc.c 这一个文件上加（见下面第 2 步）。
# 不加这个宏时 agentdoc.c 照样编译，只是走 sdmc 读卡上文件那条路 ——
# 所以两条构建路径（build.py / Makefile）不会因为缺宏而挂。

# ---- 推理内核的可切换实现（做 A/B 性能对比用）------------------------
#
# AIC_DOT_VIA_LDR=1 → shared/nnet/nnet.c 的 dot_i8_i8 走「ldr + sxtb」：
#   每 4 个 MAC 里 w 侧从 4 条 ldrsb（4 次访存）变成 1 条 ldr（1 次访存），
#   访存次数 -75%。x 侧保持 ldrsb —— x 只有 128 字节且常驻 L1，
#   改它省不到 RAM 流量。
#
# 不设（或 =0）→ 走原来的逐字节 ldrsb，4 路展开。
#
# ⚠️ 为什么用环境变量而不是改源码再改回来：
#   改源码 → 构建 → 测 → 改回 → 再构建 → 再测，这两次构建之间
#   其它东西也会变（对齐、链接顺序、.o 的生成顺序），
#   于是差值里混进了一堆与被测改动无关的差异。
#   同一份源码、同一个构建流程、只切一个宏，才是能下结论的对比。
#
# ⚠️ 口径必须与 app/Makefile 一致 —— 两条构建路径的分叉在
#   本项目已经咬过一次（EXCLUDE_SRC 分叉那次）。
def _env_flag(name):
    v = os.environ.get(name, "").strip().lower()
    return v not in ("", "0", "false", "no")

if _env_flag("AIC_DOT_VIA_LDR"):
    CFLAGS.append("-DNNET_DOT_VIA_LDR")
    print("DOT  : ldr+sxtb 版（每 4 MAC 的 w 侧访存 4->1）", flush=True)
else:
    print("DOT  : ldrsb 版（默认，4 路展开）", flush=True)

GFX    = os.path.join(APP, "source", "gfx")
UI     = os.path.join(APP, "source", "ui")
TEST   = os.path.join(APP, "source", "test")
NNET   = os.path.join(SHARED, "nnet")
DATA   = os.path.join(APP, "data")
INC    = ["-I" + os.path.join(CTRU, "include"),
          # portlibs/3ds：zlib.h，给 guide_kb.c 的 uncompress() 用。
          # 口径与 app/Makefile 的 LIBDIRS 一致，两条构建路径不能分叉。
          "-I" + os.path.join(PORTLIBS, "include"),
          "-I" + SHARED,
          "-I" + NNET,
          "-I" + os.path.join(APP, "source"),
          "-I" + GFX,
          "-I" + UI,
          "-I" + TEST,
          "-I" + BUILD]          # build/ 里是 bin2s 生成的 font_bin.h

TARGET = "AIConsole"

# --------------------------------------------------------------------------
# shared/nnet 里的取舍
#
# 推理内核已于 2026-10-04 全部接入构建（EXCLUDE_SRC 清空）。
# 此前排除的两个文件，理由都写在这里，因为这些理由本身也是教训：
#
#  1. 「nnet.c 引用的权重还没有导出产物」—— **这是误判**。
#     app/data/guide_model.bin 一直都在（2,982,948 字节，magic GM1，
#     头部 (4096,192,6,4,512,128,48) 与 CFG 完全吻合，布局逐字段
#     推算总长与实际文件**零字节差**）。真正缺的不是权重，是**加载器**
#     —— 见 shared/nnet/nnet_load.c（2026-10-04 新增）。
#     教训：说「某文件不存在」之前，先把它的字节读出来看一眼。
#
#  2. 「nnet_tables.c 自身不完整」—— **是真的**，且已修。
#     生成器模板和生成物都缺 #include <stdint.h>（用了 int32_t）。
#     治本的做法是修生成器，否则下次重新导出又丢掉这个 include。
#
#  接入前还修掉了三个静默错误（gcc 一个字都没报，见 nnet.c 注释）：
#     RoPE 配对方式错 + 漏多头循环 / post-LN vs pre-LN 架构不匹配 /
#     nnet_state_bytes 少算 5 层 KV cache（122880 字节越界）。
EXCLUDE_SRC = set()


def discover_sources():
    """main.c + shared/*.c + shared/nnet/*.c + gfx/*.c + ui/*.c + test/*.c

    与 app/Makefile 的 SOURCES 口径一致（Makefile 里已有 ../shared/nnet）。
    对象名扁平化到 build/ 下，所以不同目录不能有同名 .c。
    """
    out = [(os.path.join(APP, "source", "main.c"), "main.o")]
    for d in (SHARED, NNET, GFX, UI, TEST):
        for name in sorted(os.listdir(d)):
            if name.endswith(".c") and name not in EXCLUDE_SRC:
                out.append((os.path.join(d, name), name[:-2] + ".o"))
    return out


SOURCES = discover_sources()

# mkfont.py 需要 Pillow。它装在 WorkBuddy 的隔离 venv 里，不污染系统 Python，
# 所以调 mkfont 时要挑一个真的能 import PIL 的解释器。
_VENV_PY = os.path.join(os.path.expanduser("~"), ".workbuddy", "binaries",
                        "python", "envs", "default", "Scripts", "python.exe")
TOOL_PY = _VENV_PY if os.path.exists(_VENV_PY) else sys.executable

# ★ 标题**可以**是中文 —— 这一点是实测推翻旧结论后改的，别再退回 ASCII。
#
#   旧注释说"smdhtool 把 UTF-8 当 latin-1 塞进 UTF-16 字段，中文会变乱码"，
#   据此要求标题必须 ASCII。**实测不成立**（smdhtool 0.7.0 / devkitPro）：
#   传 "测试" 生成的 SMDH，short desc 区字节是 `4b 6d d5 8b`，
#   正是 '测试'.encode('utf-16-le') —— 编码正确，16 个语言槽全部一致。
#   验证工具：tools/smdhdump.py（按 UTF-16LE 严格解码 + 标替换符/控制符）。
#
#   ⚠️ 别把这条当成"以后可以随便传字符串"的许可：**判据是 smdhdump 的解码
#   结果，不是 smdhtool 的退出码**。smdhtool 写坏字节能一样 exit 0。
#   构建后请跑 `python tools/smdhdump.py app/AIConsole.smdh app/AIConsole_cia.smdh`。
#
#   仍然要守的两条：
#   · 标题在 SMDH 里的容量是 short 64 码元 / long 128 码元，超了会被截断
#     （不是报错，是静默截半），所以别把说明文字塞进标题。
#   · 版本号保留在标题末尾 —— 桌面上要能一眼看出装的是哪一版。
#     ⚠️ SMDH 短标题容量只有 8 字符（"测试 0.99.2" = 9 个会被静默截断），
#     所以标题只放「主.次」；应用内"关于"页读 ota.h 的全版本号。
# ★ 2026-10-23 改名 + 署名（用户要求）：
#   对外名称 = **3DS AI Agent Tool**；acrac 设计，DeepSeek V4.1-Flash 制作。
#   短标题只能 8 码元（见上面那条注释），所以缩成 "AI Agent" ——
#   "3DS AI Agent" 是 12 码元，会被**静默截断**成 "3DS AI A"。
#   全名放在描述里（short desc 上限 64 码元）。
APP_TITLE  = "AI Agent"
APP_DESC   = "3DS AI Agent Tool 测试版 0.99.5 · acrac 设计，DeepSeek V4.1-Flash 制作"
# 作者字段：SMDH 的 publisher 是 64 码元，这条 28 码元有余量。
APP_AUTHOR = "acrac / DeepSeek V4.1-Flash"

# CIA 形态在主菜单里单独一个名字 —— 它比 3dsx 版多一项能力（能装程序），
# 两个图标长一样会造成"点错了以为坏了"的困惑。见第 6 步的说明。
# 标题同样允许中文（依据同 APP_TITLE 上面的实测结论）。
APP_TITLE_CIA = "AI Agent"                # 与 3dsx 版同名（桌面显示名一致），
                                         # 区分靠描述文字，不靠标题
APP_DESC_CIA  = "3DS AI Agent Tool 测试版 0.99.5 · 支持安装游戏"

# Icon used for the HOME-menu entry. Drop an app/icon.png in to override.
ICON = os.path.join(APP, "icon.png")
if not os.path.exists(ICON):
    ICON = os.path.join(CTRU, "default_icon.png")


# --------------------------------------------------------------------------
def run(cmd, label):
    print("--- %s" % label, flush=True)
    r = subprocess.run(cmd, capture_output=True, text=True, errors="replace")
    out = (r.stdout or "").strip()
    err = (r.stderr or "").strip()
    if out:
        print(out[:4000], flush=True)
    if err:
        print("STDERR: " + err[:4000], flush=True)
    print("    rc=%d" % r.returncode, flush=True)
    return r.returncode == 0


def need(path, what):
    if not os.path.exists(path):
        print("MISSING %s: %s" % (what, path))
        return False
    return True


def bin2s_asm(src, hdr, asm):
    """把一个二进制/文本文件编成可编译的汇编 + 头文件。

    bin2s 只往 stdout 写，所以走临时 .s 再编译。
    它生成的汇编没带 .note.GNU-stack，链接器会为它打一条
    "missing .note.GNU-stack section implies executable stack" 的弃用告警，
    所以补上这一段，构建就干净了。
    """
    with open(asm, "wb") as fh:
        r = subprocess.run([os.path.join(TOOLS, "bin2s.exe"),
                            "-a", "4", "-H", hdr, src],
                           stdout=fh, stderr=subprocess.PIPE)
    if r.returncode != 0 or not os.path.exists(hdr):
        print("bin2s failed: " + (r.stderr or b"").decode("utf-8", "replace"))
        return None
    with open(asm, "ab") as fh:
        fh.write(b'\n.section .note.GNU-stack,"",%progbits\n')
    return asm


AGENTDOC_HDR_TMPL = """/* generated by tools/build.py -- do not edit */
#pragma once
#include <stddef.h>
#include <stdint.h>
extern const uint8_t %(txt)s[];
extern const size_t %(size)s;
"""


def _agentdoc_max_bytes():
    """从 shared/agentdoc.h 里抠 AGENTDOC_MAX，**不在这里另写一份**。

    同源纪律：手册上限是设备端行为（超了整份被拒），
    这里若自己写个 24576，改了头文件就会两边不一致，
    而症状是"构建说没问题、运行时模型拿不到规则"。
    """
    import re as _re
    p = os.path.join(ROOT, "shared", "agentdoc.h")
    try:
        txt = open(p, encoding="utf-8", errors="replace").read()
    except OSError:
        raise SystemExit("!! 读不到 %s，无法确定手册上限" % p)
    m = _re.search(r"#define\s+AGENTDOC_MAX\s+\(\s*(\d+)\s*\*\s*1024\s*\)", txt)
    if m:
        return int(m.group(1)) * 1024
    m = _re.search(r"#define\s+AGENTDOC_MAX\s+\(?\s*(\d+)\s*\)?", txt)
    if m:
        return int(m.group(1))
    raise SystemExit("!! 在 agentdoc.h 里找不到 AGENTDOC_MAX —— 不给兜底值")


AGENTDOC_MAX_BYTES = None      # main() 里初始化（ROOT 要先确定）


def emit_agentdoc_asm(agent_md, hdr, asm,
                      txt_sym="agentdoc_txt", size_sym="agentdoc_txt_size"):
    """把一份手册 .md 编成 .s + 对应的 extern 头。

    单独成一个函数是为了能被**不跑整条构建**的方式验证（这台机器没有工具链）：
    生成完把 .byte 列表抠回来和源文件逐字节比，对不上就是这里生成错了。
    内嵌的手册喂给模型的是规则和上限，错一个字节就是让模型照错的规矩干活。

    ★ 符号名是**参数**，不是写死的 —— 现在有两份手册（中文 / 英文），
      两份不能撞符号。agentdoc.c 按界面语言挑一份，见该文件注释。
    """
    with open(hdr, "w", encoding="ascii") as fh:
        fh.write(AGENTDOC_HDR_TMPL % {"txt": txt_sym, "size": size_sym})
    if not os.path.exists(agent_md):
        # 没有手册也得给一份空的 extern 声明：agentdoc.c 无条件 include 这个头，
        # 缺了头文件整个编译就挂 —— 而"手册缺失"本来就该走到 sdmc 兜底那条路。
        print("WARN  %s 不存在 -> 内嵌副本为空，运行时改读卡上文件"
              % os.path.basename(agent_md), flush=True)
        with open(asm, "w", encoding="ascii") as fh:
            fh.write('.section .rodata\n.global %s\n'
                     '.type %s, %%object\n.align 2\n%s:\n.zero 0\n'
                     '.type %s, %%object\n.align 2\n'
                     '%s:\n.word 0\n'
                     % (txt_sym, txt_sym, txt_sym, size_sym, size_sym))
        return True
    with open(agent_md, "rb") as fh:
        payload = fh.read()
    with open(asm, "w", encoding="ascii") as fh:
        fh.write('.section .rodata\n')
        # 注意这一句**没有走 % 格式化**，所以这里只能写单个 %；
        # 下面那句走 % 格式化的是 %% —— 同一个"百分号转义"规则，写错了
        # 产物里就会留下一行 `.type xxx, %%object`，gas 直接报语法错。
        fh.write('.global %s\n.type %s, %%object\n.align 4\n' % (txt_sym, txt_sym))
        fh.write('%s:\n' % txt_sym)
        for i in range(0, len(payload), 16):
            fh.write("\t.byte " + ",".join("0x%02x" % b
                                           for b in payload[i:i + 16]) + "\n")
        fh.write('.size %s, %d\n'
                 '.global %s\n'
                 '.type %s, %%object\n.align 2\n'
                 '%s:\n\t.word %d\n'
                 '.section .note.GNU-stack,"",%%progbits\n'
                 % (txt_sym, len(payload), size_sym, size_sym, size_sym,
                    len(payload)))
    print("AGENT: %d bytes -> %s (embedded, sym=%s)"
          % (len(payload), os.path.basename(asm), txt_sym), flush=True)
    return True


def do_clean():
    if os.path.isdir(BUILD):
        shutil.rmtree(BUILD)
        print("removed %s" % BUILD)
    return 0


def gen_smdh_icons():
    """把 app/icon.png（256 设计稿）降采样成 SMDH 要的 48x48 + 24x24。

    smdhtool --create **不缩放**图标：给它 256x256 会报 "Icon size is
    incorrect" 并直接跳过图标写入，产出 8256 字节的截断 SMDH（home-menu
    上图标变空块）。所以必须先把图标降到精确尺寸再喂进去。
    失败（比如 Pillow 不在）就退回直接用 256 原图，并打印告警，
    不让整个构建因为"图标不好看"而挂掉。
    """
    out48 = os.path.join(BUILD, "icon_48.png")
    out24 = os.path.join(BUILD, "icon_24.png")
    r = subprocess.run([TOOL_PY, os.path.join(ROOT, "tools", "make_smdh_icons.py"),
                        "--src", ICON, "--out48", out48, "--out24", out24],
                       capture_output=True, text=True, errors="replace")
    if r.returncode != 0 or not (os.path.exists(out48) and os.path.exists(out24)):
        print("WARN  SMDH 图标降采样失败，退回直接用 256 原图 "
              "（smdhtool 可能丢图标 → 桌面图标变空块）", flush=True)
        if r.stderr.strip():
            print("      " + r.stderr.strip()[:800], flush=True)
        return ICON, None
    return out48, out24


def main():
    global AGENTDOC_MAX_BYTES
    argv = [a.lower() for a in sys.argv[1:]]

    if "clean" in argv:
        return do_clean()

    AGENTDOC_MAX_BYTES = _agentdoc_max_bytes()

    want_cia = "cia" in argv

    for p, what in ((GCC, "devkitARM gcc"),
                    (os.path.join(TOOLS, "3dsxtool.exe"), "3dsxtool"),
                    (os.path.join(TOOLS, "smdhtool.exe"), "smdhtool"),
                    (os.path.join(TOOLS, "bin2s.exe"), "bin2s"),
                    (os.path.join(CTRU, "include", "3ds.h"), "libctru")):
        if not need(p, what):
            return 1
    if want_cia and not need(os.path.join(TOOLS, "makerom.exe"), "makerom"):
        return 1

    # ---- 0. 内嵌数据 ------------------------------------------------------
    # app/data/font.bin 是点阵字库，缺了就先生成一份。mkfont.py 幂等。
    fontbin = os.path.join(DATA, "font.bin")
    if not os.path.exists(fontbin):
        print("--- font.bin missing, generating with mkfont.py", flush=True)
        subprocess.run([TOOL_PY, os.path.join(ROOT, "tools", "mkfont.py")],
                       check=False)
        if not os.path.exists(fontbin):
            print("FAILED: 字库生成失败，先单独跑 tools/mkfont.py 看报错")
            return 1
    print("FONT : %s (%d bytes)" % (fontbin, os.path.getsize(fontbin)), flush=True)

    # 离线攻略的 BPE 词表。由 tools/guia/export_vocab.py 导出（GVB1 纯文本）。
    # 同样内嵌而不是放 SD 卡：检索是这功能的核心，词表缺了会静默变成
    # "命中 0 条"，而用户没道理知道自己少了张卡。
    vocabtxt = os.path.join(DATA, "guide_vocab.txt")
    if not need(vocabtxt, "guide_vocab.txt（跑 tools/guia/export_vocab.py 重新导出）"):
        return 1
    print("VOCAB: %s (%d bytes)" % (vocabtxt, os.path.getsize(vocabtxt)), flush=True)

    # ★ 构建期断言：卡上这份词表必须与 guide_kb.bin / 模型用的同一代。
    #
    # 2026-10-04 踩过：重训过一次 BPE（ckpt/bpe.pkl 更新了），
    # 但**没人重新跑 export_vocab.py** →卡上跑的是旧一代词表，
    # 而 guide_kb.bin 的倒排表按新一代 token id 建立。
    # 症状：2 条用例"检索 0 命中" + 4 条**命中错误段落**，
    # 全程不报错、不崩、退出码正常 —— 抽取式 X2 趟 1/8 查了一整轮。
    #
    # 为什么必须在这里挡：`id`是**位置**，两份表"看起来一样"
    #（V 和 merges 数都相等），只有逐条比 id->bytes 才抓得到。
    # 见 tools/guia/verify_vocab.py 的文件头。
    ver = os.path.join(ROOT, "tools", "guia", "verify_vocab.py")
    if os.path.exists(ver):
        r = subprocess.run([TOOL_PY, ver], capture_output=True)
        out = (r.stdout or b"").decode("utf-8", "replace").strip()
        if out:
            print(out, flush=True)
        if r.returncode != 0:
            print("FAILED: 词表与索引不同代 —— 跑 "
                  "`python tools/guia/export_vocab.py` 重新导出"
                  "（只差顺序时 `verify_vocab.py --fix` 可自动修）")
            return 1

    # ★ 先跑文案判据，再生成表。
    # check_i18n.py 查的是"机器能查、眼睛查不了"的三件事：
    # 格式占位符中英一致（少一个 %s 在 3DS 上就是读越界）、
    # 动作协议名没被翻译、译文里没残留中文。
    # 它带 --selftest，判据自己假绿的话那一轮会先报出来。
    chk = os.path.join(ROOT, "tools", "check_i18n.py")
    if os.path.exists(chk):
        r = subprocess.run([TOOL_PY, chk], capture_output=True, text=True,
                           errors="replace")
        out = (r.stdout or "").strip()
        if out:
            print(out, flush=True)
        if r.returncode != 0:
            print("FAILED: 文案判据不通过（见上面）—— 译文问题，先别编译")
            return 1

    # 界面文案表（中英）。由 tools/i18n_table.py 从两份清单生成：
    #   tools/i18n_strings.json  编号 → 中文（i18n_extract.py 抽自源码）
    #   tools/i18n_en.json       编号 → 英文（译文）
    # 它**每次都重生成**（不判 mtime）：漏翻一条就会在编译期失败，
    # 这比"真机跑到那一屏发现是中文"早得多，也便宜得多。
    i18n = os.path.join(ROOT, "tools", "i18n_table.py")
    if os.path.exists(i18n):
        r = subprocess.run([TOOL_PY, i18n], capture_output=True, text=True,
                           errors="replace")
        out = (r.stdout or "").strip()
        if out:
            print(out, flush=True)
        if r.returncode != 0:
            print("FAILED: 文案表生成失败 —— 见上面提示"
                  "（缺译文就补 tools/i18n_en.json）")
            return 1

    # TLS 根证书（Mozilla bundle）。**必须内嵌**，不能要求用户往卡上放 ——
    # 出厂就要能连 HTTPS。curl 用 CURLOPT_CAINFO_BLOB 读它（见 shared/httpx.c）。
    # 本机开发时卡巴斯基做 HTTPS 中间人，SD 卡上放一份带它根证书的
    # sdmc:/config/ssl/cacert.pem 即可覆盖，真机不需要那个文件。
    cacert = os.path.join(DATA, "cacert.pem")
    if not need(cacert, "cacert.pem（Mozilla CA bundle，放 app/data/ 下）"):
        return 1
    print("CA   : %s (%d bytes)" % (cacert, os.path.getsize(cacert)), flush=True)

    # 3DS 硬件知识库（TF 纯文本）。**同样双路径**：SD 卡上那份可通过
    # 内容更新通道替换（shared/cupd.c 的 CU_HARDWARE），内嵌这份兜底。
    # 内嵌的理由与词表一样 —— 但更硬：这份东西是「我们实测过的硬件结论」，
    # 是这套软件敢回答硬件问题的底气。丢了它，用户问「3ds 能超频吗」
    # 就只能得到沉默，而沉默会被理解成「不支持」。
    hwknow = os.path.join(DATA, "hw_knowledge.txt")
    if not need(hwknow, "hw_knowledge.txt（3DS 硬件知识库）"):
        return 1
    print("HWKNOW: %s (%d bytes)" % (hwknow, os.path.getsize(hwknow)), flush=True)

    os.makedirs(BUILD, exist_ok=True)

    # ---- 1. 内嵌数据 -> 汇编 -> .o ----------------------------------------
    # 必须排在源码编译之前：font.c 要 include 生成的 font_bin.h，
    # nnet_text.c 要 include 生成的 guide_vocab_bin.h。
    #
    # 产物名与 devkitPro 默认的 bin2o 规则（`%.bin.o %_bin.h: %.bin`）保持一致，
    # 这样两条构建路径（build.py / Makefile）共用同一批中间文件，来回切换不用重编。
    binobjs = []
    # ★ pinyin_tbl.bin 由 tools/gen_pinyin_table.py 生成（拼音输入法的音节→候选表）。
    #   不内嵌的话，输入法在卡上找不到数据就是"按了没反应"，
    #   而那种失败在真机上没有任何提示。
    pinyin_bin = os.path.join(DATA, "pinyin_tbl.bin")
    for src, stem in ((fontbin, "font"), (vocabtxt, "guide_vocab"),
                      (pinyin_bin, "pinyin_tbl"),
                      (cacert, "cacert"), (hwknow, "hw_knowledge")):
        hdr = os.path.join(BUILD, stem + "_bin.h")
        asm = os.path.join(BUILD, stem + ".bin.s")
        obj = os.path.join(BUILD, stem + ".bin.o")
        if not bin2s_asm(src, hdr, asm):
            return 1
        if not run([GCC] + CFLAGS + INC + ["-c", asm, "-o", obj],
                   "compile %s.bin.s" % stem):
            return 1
        binobjs.append(obj)
        print("%-5s: %s.bin.o (%d bytes)" % (stem, stem, os.path.getsize(obj)),
              flush=True)

    # ---- 1b. 内嵌 AGENT.md（喂给模型的那份运行手册）------------------------
    # 别的资源走 bin2s，这份**故意不走**：
    #   ① bin2s 的符号名是按源文件名推的（hw_knowledge.txt → hw_knowledge_txt），
    #      AGENT.md 会推出什么没人敢打包票，而这台机器上根本没有工具链可以试；
    #   ② 这里自己生成 .s，符号名是我们定的，改了 AGENT.md 也不用猜。
    # 不内嵌的话，用户得每次手动往卡上拷一份 AGENT.md，
    # 拷了旧版 / 拷错位置都会静默变成"模型按过期的规矩干活"。
    # ★ 两份手册都要内嵌：中文（默认）+ 英文（界面语言切成 en 时用）。
    #   agentdoc.c 按 uistr_lang() 挑一份。两份的符号名必须不同，
    #   所以这里成对传进去，别指望函数里写死。
    docs = [
        ("AGENT.md",    "agentdoc_md.s",    "agentdoc_md_bin.h",
         "agentdoc_txt",    "agentdoc_txt_size"),
        ("AGENT.en.md", "agentdoc_en_md.s", "agentdoc_en_md_bin.h",
         "agentdoc_en_txt", "agentdoc_en_txt_size"),
    ]
    payload_len = 0
    for fname, sname, hname, tsym, ssym in docs:
        src_md = os.path.join(ROOT, fname)
        h      = os.path.join(BUILD, hname)
        s      = os.path.join(BUILD, sname)
        o      = os.path.join(BUILD, sname.replace(".s", ".o"))
        if not emit_agentdoc_asm(src_md, h, s, tsym, ssym):
            return 1
        if os.path.exists(src_md):
            sz = os.path.getsize(src_md)
            if fname == "AGENT.md":
                payload_len = sz
            # 超限判据在编译前就报，别等运行时整份被拒（agentdoc.c 的行为是
            # "超限就整份不用"，那时模型拿不到任何规则，症状是"AI 开始乱编动作"）。
            if sz > AGENTDOC_MAX_BYTES:
                print("!! %s 有 %d 字节，超过 AGENTDOC_MAX(%d) —— 运行时会被**整份拒绝**"
                      % (fname, sz, AGENTDOC_MAX_BYTES), flush=True)
                return 1
            if AGENTDOC_MAX_BYTES - sz < 512:
                print("   ! %s 距上限只剩 %d 字节，再加内容就要超了"
                      % (fname, AGENTDOC_MAX_BYTES - sz), flush=True)
        if not run([GCC] + CFLAGS + INC + ["-c", s, "-o", o],
                   "compile " + sname):
            return 1
        binobjs.append(o)

    # ---- 2. compile -------------------------------------------------------
    objs = list(binobjs)
    agdoc_src = os.path.join(ROOT, "shared", "agentdoc.c")
    for src, objname in SOURCES:
        if not need(src, "source"):
            return 1
        obj = os.path.join(BUILD, objname)
        # agentdoc.c 单独多带一个 -DAGENTDOC_EMBEDDED：只有它认那个宏，
        # 别的源文件不认也用不上 —— 避免把宏撒得到处都是，看不出谁真用了。
        extra = ["-DAGENTDOC_EMBEDDED"] if os.path.samefile(src, agdoc_src) else []
        if run([GCC] + CFLAGS + INC + ["-D__3DS__"] + extra + ["-c", src, "-o", obj],
               "compile " + os.path.basename(src)):
            objs.append(obj)
        else:
            print("compile failed, aborting")
            return 1

    # ---- 3. link ----------------------------------------------------------
    # -Wl,-z,noexecstack：devkitARM 自己的 crtn.o 没带 .note.GNU-stack，
    # 不显式声明就会打出 "implies executable stack" 的弃用告警。
    elf = os.path.join(OUT, TARGET + ".elf")
    # -lz + portlibs/3ds：guide_kb.c 要 uncompress() 解开压缩内嵌的知识库
    # （tools/guia/export_kb_zlib.py，408KB -> 165KB）。
    # 网络栈现在接了 curl + mbedtls（TLS 1.2）。**静态库，链进去不占 SD 卡。**
    #
    # ⚠️ 顺序是**实测定下来的**，不要"整理"：
    #     curl → mbedtls → mbedx509 → mbedcrypto → z → **ctru** → m
    # 静态链接一遍过，被依赖的必须排在后面。两个具体的坑：
    #   · curl 调 mbedtls_*，mbedtls 调 x509/crypto —— 顺序反了报
    #     "curl 里找不到 mbedtls_xxx"，看着像 portlibs 没装全。
    #   · **libctru 必须排在 curl 后面**：curl 的 socket 层（poll/inet_pton/
    #     getsockname…）是 libctru 提供的，ctru 排在前面时那些符号还没被
    #     引出来，链接器就报 undefined reference to `poll` ——
    #     看着像"3DS 不支持 poll"，其实纯粹是顺序。
    # 这个顺序与 tools/tls12/Makefile、tools/doubao/Makefile 完全一致
    # （那两个探针是实测跑通的），口径不能分叉。
    if not run([GCC, "-specs=3dsx.specs", "-g"] + ARCH + objs +
               ["-L" + os.path.join(PORTLIBS, "lib"),
                "-lcurl", "-lmbedtls", "-lmbedx509", "-lmbedcrypto", "-lz",
                "-L" + os.path.join(CTRU, "lib"), "-lctru", "-lm",
                "-Wl,-z,noexecstack", "-o", elf],
               "link " + TARGET + ".elf"):
        return 1
    print("ELF  : %s (%d bytes)" % (elf, os.path.getsize(elf)), flush=True)

    # ---- 4. SMDH (title/icon metadata) ------------------------------------
    # 3dsxtool needs this, so it must be built BEFORE the .3dsx step.
    smdh = os.path.join(OUT, TARGET + ".smdh")
    icon48, icon24 = gen_smdh_icons()
    smdh_cmd = [os.path.join(TOOLS, "smdhtool.exe"), "--create",
                APP_TITLE, APP_DESC, APP_AUTHOR, icon48, smdh]
    if icon24:
        smdh_cmd.append(icon24)
    if not run(smdh_cmd, "smdhtool -> " + TARGET + ".smdh"):
        return 1
    print("SMDH : %s (%d bytes)" % (smdh, os.path.getsize(smdh)), flush=True)
    # 桌面图标开立体 3D（smdhtool 不暴露该开关，见 tools/patch_smdh_3d.py）
    if _set_smdh_3d is not None and _set_smdh_3d(smdh):
        print("3D   : 已置位 SMDH 立体 3D 标志 (flags@0x2028 bit 0x0002)", flush=True)

    # 标题**当场**验，别等以后想起来。
    # 为什么必须在这里自动验：smdhtool 写坏字节能一样返回 rc=0，
    # 而 smdhdump 的判据（按 UTF-16LE 严格解码）是唯一能区分
    # 「标题对」与「标题花掉」的东西。挂进构建期 = 改标题后忘了手验也不会漏。
    # 历史教训：本文件曾有条"标题必须 ASCII"的注释，那是**误判**（实测
    # smdhtool 正确支持 UTF-16LE 中文），一条没人验过的假守卫把中文封死了。
    if _smdh_check is not None and not _smdh_check(smdh, APP_TITLE):
        print("!! SMDH 标题校验失败 —— 桌面标题会花掉，已中止构建", flush=True)
        return 1

    # ---- 5. .3dsx ---------------------------------------------------------
    s3dsx = os.path.join(OUT, TARGET + ".3dsx")
    run([os.path.join(TOOLS, "3dsxtool.exe"), elf, s3dsx, "--smdh=" + smdh],
        "3dsxtool -> " + TARGET + ".3dsx")
    if os.path.exists(s3dsx):
        print("3DSX : %s (%d bytes)" % (s3dsx, os.path.getsize(s3dsx)), flush=True)

    # ---- 6. .cia ----------------------------------------------------------
    # 注意：没有 -romfs。makerom 0.19 不接受 mkromfs3ds 产出的标准 RomFS
    # 镜像（带/不带目录哈希表都被判 Invalid RomFS Binary），字库改走 bin2o。
    #
    # ⚠️ 这里用 **AIConsole_cia.rsf**，不是 AIConsole.rsf。
    # 差别只有一行 am:u，但那正是"能不能自己装程序"的那一行。
    # 之所以有这个区别：3dsx 的 ServiceAccessControl 会被 Luma 的 hbldr
    # 覆盖，只有 .cia 的才会被真正读。详见文件头说明。
    # 用错 RSF 的表现是：cia 能装上、能开机，但调用 AM_StartCiaInstall 时
    # 报权限错误 —— 而这是**真机才会暴露**的问题，模拟器不查 exheader，
    # 照样给你 rc=0（Azahar 假阳性，老坑了）。
    if want_cia:
        rsf = os.path.join(APP, TARGET + "_cia.rsf")
        if not need(rsf, "RSF (CIA 形态专用，含 am:u)"):
            return 1
        # ⚠️ CIA 形态的 SMDH 独立生成（描述文字与 3dsx 版不同，便于在
        #   "关于"页分辨自己装的是哪一版）。标题与 3dsx 版**同名**：
        #   用户明确要求主菜单显示「测试」，两个形态都叫这个；
        #   区分靠描述，不靠标题。注意这只影响主菜单显示名，
        #   不影响 RSF / title id / 权限。
        smdh_cia = os.path.join(OUT, TARGET + "_cia.smdh")
        cia_cmd = [os.path.join(TOOLS, "smdhtool.exe"), "--create",
                   APP_TITLE_CIA, APP_DESC_CIA, APP_AUTHOR, icon48, smdh_cia]
        if icon24:
            cia_cmd.append(icon24)
        if run(cia_cmd, "smdhtool -> " + TARGET + "_cia.smdh (仅 CIA 用)"):
            icon_for_cia = smdh_cia
            # 桌面图标开立体 3D（CIA 形态同样要浮起来）
            if _set_smdh_3d is not None and _set_smdh_3d(smdh_cia):
                print("3D   : 已置位 CIA 版 SMDH 立体 3D 标志", flush=True)
            # ★★ Extended Banner 位 (0x0010)：告诉 home-menu ExeFS 里有 CBMD
            # 3D 模型横幅。这个位 ≠ 图标 3D 位(0x0002)，必须单独置。
            # 不置的话 home-menu 横幅页不会去读我们打进 CIA 的 banner 文件，
            # 用户要的"浮起旋转的 AI 对话框"就出不来。两者配套：横幅文件
            # 由下面 makerom 的 -banner 打进 ExeFS，这个位是它的"开关"。
            if _set_ext_banner is not None and _set_ext_banner(smdh_cia):
                print("BNR  : 已置位 CIA 版 SMDH Extended Banner(0x0010) 标志",
                      flush=True)
            # CIA 版的标题同样当场验（与 3dsx 版各写一份 SMDH，各验各的）
            if _smdh_check is not None and not _smdh_check(smdh_cia, APP_TITLE_CIA):
                print("!! CIA 版 SMDH 标题校验失败 —— 已中止构建", flush=True)
                return 1
            print("       ^ 主菜单里显示为 %r" % APP_TITLE_CIA, flush=True)
        else:
            icon_for_cia = smdh      # 退回共用，不因为改名字而让整个构建失败
            print("       ! 专用 SMDH 生成失败，退回共用图标；功能不受影响",
                  flush=True)
        cia = os.path.join(OUT, TARGET + ".cia")

        # ★★ 标题版本**必须显式给**，不能留默认值。
        #
        # 2026-10-10 真机（FBI）装这个 CIA 报：
        #   Result 0xD8E08027 / Level Permanent(27) / Summary Invalid argument(7)
        #   Module AM(32) / Desc **Invalid or outdated title version (39)**
        # 而本脚本此前调 makerom **一直没传 `-ver`** ——
        # TMD 与 NCCH 里的标题版本于是都是 0。
        # 「invalid **or outdated**」是个**比较/校验**结果：AM 装包时会看这个字段。
        #
        # 版本取 4096 = v4.0.0（3DS 的编码是 6bit 主 . 6bit 次 . 4bit 微，
        # 4096 = 0b000100_000000_0000）。
        #
        # 为什么不用 1024 这种"老实"的值：报错的措辞是
        # 「invalid **or outdated**」——**outdated 是个比较结果**，
        # 意味着系统里可能已有一个同 TitleID、版本更高的标题
        # （我们查过 hblauncher_loader 的 UniqueId 是 0xd921e，不是我们，
        #  但用户实际装过什么无法从本地确认）。
        # 取一个**明显高于常见 homebrew v1.x** 的值，两种情况一次覆盖：
        #   · 若真因是"版本不能是 0" → 任何非 0 都行
        #   · 若真因是"比不过既有标题" → 4096 大于绝大多数 homebrew 版本
        # ⚠️ 以后发新版**只往上加**，别往回退 —— 退回会被判成 outdated 装不上去。
        # ★★ Extended Banner 横幅：把「AI 对话框」3D 模型打进 ExeFS 的 banner 文件。
        #
        # 流程：tools/make_banner_bnr.py 负责（GLB -> pycgfx -> CGFX -> LZ11
        # -> CBMD .bnr），纯 Python 绕开本机缺失的 bannertool。它会先校验
        # CGFX 不含软蒙皮、含 COMMON、解压 ≤512KiB，任一不过都拒绝打包 ——
        # 避免造出真机冻屏的横幅。
        #
        # 只有 .bnr 真的产出且合法，才给 makerom 加 -banner；否则**降级**为
        # 不带横幅的 CIA（图标 3D 位仍在），不让"横幅"把整个 CIA 构建拖垮。
        banner_arg = []
        _bnr_maker = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                  "make_banner_bnr.py")
        _bnr_file = os.path.join(ROOT, "app", "banner", "banner_model.bnr")
        try:
            _r = subprocess.run([sys.executable, _bnr_maker],
                                cwd=os.path.dirname(os.path.abspath(__file__)))
            if _r.returncode == 0 and os.path.exists(_bnr_file):
                banner_arg = ["-banner", _bnr_file]
                print("BNR  : 横幅 .bnr 就绪，将随 CIA 打进 ExeFS", flush=True)
            else:
                print("   ! 横幅 .bnr 生成失败（rc=%s），本趟 CIA 不带 3D 横幅"
                      % _r.returncode, flush=True)
        except Exception as _e:
            print("   ! 横幅生成异常：%s，本趟 CIA 不带 3D 横幅" % _e, flush=True)

        CIA_VER = "4096"

        # ---- 系统说明书（content index 1）------------------------------
        # HOME 菜单选中标题后左下角的「?」读的就是 content 0001（一个 CFA）。
        # 只有 .cia 能有，.3dsx 永远不可能（3DSX 不是 title content）。
        # 详见 docs/内置说明书方案.md。
        #
        # ⚠️ 真 Manual.bcma 还没做（BCLYT 页面要从零写生成器）。现在打进
        #   去的是**占位文件**，它唯一的作用是回答一个 go/no-go 问题：
        #   自制 test-signed title 装上去之后，HOME 到底显不显示「?」。
        #   点开「?」报错/白屏是预期行为，不是 bug。
        #
        # 打包失败时**降级**为单 content CIA（不强求），但要明确说出来 ——
        # 静默降级会让"说明书没生效"变成一个查不出原因的现象。
        manual_cfa = os.path.join(APP, "manual.cfa")
        manual_ok = False

        # ★★ 2026-10-22 起**默认不挂系统说明书**（用户真机反馈：点「?」闪退）。
        #
        #   原因很直接：`app/manual/root/Manual.bcma` 至今是一个 **259 字节的
        #   纯文本占位文件**（内容就是 "AI CONSOLE MANUAL PLACEHOLDER..."），
        #   不是合法的 BCMA。系统 applet 拿去解析必然失败 ——
        #   在真机上不是"白屏"，而是**闪退**。
        #
        #   挂一个必崩的「?」比没有这个按钮糟得多：
        #   用户会以为程序坏了，而真正的原因是"这个功能从来没做出来"。
        #
        #   等真 BCMA/BCLYT 生成器做完，用 AIC_MANUAL=1 重新打开这条路。
        if os.environ.get("AIC_MANUAL") != "1":
            print("   · 系统说明书：未挂载（Manual.bcma 仍是占位文件，挂了会闪退）",
                  flush=True)
        else:
            _mhelper = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                    "manual", "build_manual.py")
            try:
                _r = subprocess.run([sys.executable, _mhelper],
                                    cwd=os.path.dirname(os.path.abspath(__file__)))
                manual_ok = (_r.returncode == 0 and os.path.exists(manual_cfa))
                if not manual_ok:
                    print("   ! 说明书 CFA 打包失败（rc=%s），本趟 CIA **不含**说明书"
                          % _r.returncode, flush=True)
            except Exception as _e:
                print("   ! 说明书打包异常：%s，本趟 CIA 不含说明书" % _e, flush=True)

        if manual_ok:
            # 双 content 必须走两步：makerom 的 `-elf` 只产单 content，
            # 要加第二个只能用 `-content <file>:<index>:<id>`。
            #   步骤1  ELF -> CXI（content 0）
            #   步骤2  CXI + CFA -> CIA
            cxi = os.path.join(OUT, TARGET + ".cxi")
            step1 = [os.path.join(TOOLS, "makerom.exe"),
                     "-f", "ncch", "-target", "t",
                     "-o", cxi,
                     "-elf", elf,
                     "-rsf", rsf,
                     "-icon", icon_for_cia,
                     "-exefslogo"] + banner_arg
            if not run(step1, "makerom -> " + TARGET + ".cxi (content 0)"):
                return 1
            makerom_cmd = [os.path.join(TOOLS, "makerom.exe"),
                      "-f", "cia", "-target", "t",
                      "-o", cia,
                      "-content", cxi + ":0:0",
                      "-content", manual_cfa + ":1:1",
                      "-rsf", rsf,
                      "-icon", icon_for_cia,
                      "-ver", CIA_VER]
            ok = run(makerom_cmd, "makerom -> " + TARGET + ".cia"
                     + "  (双 content：主程序 + 系统说明书)")
        else:
            makerom_cmd = [os.path.join(TOOLS, "makerom.exe"),
                      "-f", "cia", "-target", "t",
                      "-o", cia,
                      "-elf", elf,
                      "-rsf", rsf,
                      "-icon", icon_for_cia,
                      "-ver", CIA_VER,
                      "-exefslogo"] + banner_arg
            ok = run(makerom_cmd, "makerom -> " + TARGET + ".cia"
                     + ("  (含 3D 横幅)" if banner_arg else ""))

        # ★★ 构建期断言：**标题版本必须真的写进去了，且非 0**。
        #
        # 2026-10-10 真机（FBI）装这个 CIA 报
        #   0xD8E08027 / AM / "Invalid or outdated title version"。
        # 而本脚本此前**一直没传 `-ver`** → TMD 与 NCCH 里的标题版本都是 0。
        #
        # 两个偏移是**实测定位**的，不是猜的：
        #   用 `-ver 1024` 与 `-ver 4096` 各打一份再做字节差异，
        #   只有两处短区间按 0x0400→0x1000 的高字节变化（04→10），
        #   其余变化的都是签名/哈希块（TMD 被签名、NCCH 头哈希在 TMD 里）。
        #   再与"不传 -ver"那份比 → 这两处原值都是 00。
        #   → 0x2C26 在 TMD 里，0x2F9C 在 NCCH 头里，都是**大端 u16 的高字节**。
        # ⚠️ 这两个偏移是 makerom 0.19 的布局。**换 makerom 版本后若这条断言失败，
        #   不要直接删掉它** —— 照上面的办法重新 diff 出偏移再改。
        if ok and os.path.exists(cia):
            _v = int(CIA_VER)
            _want_hi = (_v >> 8) & 0xFF
            with open(cia, "rb") as _f:
                _blob = _f.read()
            _bad = []
            for _off, _what in ((0x2C26, "TMD"), (0x2F9C, "NCCH 头")):
                if _off >= len(_blob) or _blob[_off] != _want_hi:
                    _bad.append("%s@0x%06X 实测 0x%02X 期望 0x%02X"
                                % (_what, _off,
                                   _blob[_off] if _off < len(_blob) else -1,
                                   _want_hi))
            if _bad:
                print("!! CIA 标题版本没写对（-ver %s 没有生效）：" % CIA_VER,
                      flush=True)
                for _b in _bad:
                    print("     " + _b, flush=True)
                print("   → 装到真机会报 0xD8E08027（Invalid or outdated title "
                      "version）。先确认 -ver 传进了 makerom 参数表；",
                      flush=True)
                print("     若 makerom 版本变了，按注释里的办法重新 diff 定位偏移。",
                      flush=True)
                return 1
            print("CIA 版本: v%d（TMD 与 NCCH 头两处都已写对）" % _v, flush=True)

        # ★★ 构建期断言：说明书 content 真的进了 CIA。
        # makerom 是**静默**的 —— 参数写错（比如索引写重、路径笔误）它会
        # 照常产出 CIA，只是里面少一个 content，而"少了说明书"这件事
        # 在模拟器上完全看不出来，只有真机点「?」才发现。
        # 判据用"content 1 的前 0x200 字节在 CIA 里逐字节出现"：
        # CIA 的 content 区是原样存放的，这是最直接的存在性证据。
        if ok and manual_ok and os.path.exists(cia):
            try:
                _mb = open(manual_cfa, "rb").read()
                _cb = open(cia, "rb").read()
                _probe = _mb[:0x200]
                if _cb.find(_probe) < 0:
                    print("!! 说明书没进 CIA —— makerom 没报错但 content 1 缺失",
                          flush=True)
                    print("   → 检查 -content 参数是否为 \"%s:1:1\"" % manual_cfa,
                          flush=True)
                    return 1
                print("说明书: content 1 已进 CIA（%d 字节，逐字节存在性已验）"
                      % len(_mb), flush=True)
            except Exception as _e:
                print("!! 说明书存在性校验异常：%s" % _e, flush=True)
                return 1

        # ★★ 构建期断言：CIA 必须比 ELF **新**。
        #
        # 2026-10-07 实测踩到：app/AIConsole.cia 是 08:12 的，而
        # app/AIConsole.elf 已被另一个会话在 19:18 重编 —— CIA 没跟着重建。
        # 交付出去的是「11 小时前的旧代码」，而**所有检查都显示通过**：
        # 3dsx 是 19:18 的（新）、cia 是 08:12 的（旧），两者都能构建成功，
        # makerom 也 rc=0。唯一露馅的是 mtime 先后。
        #
        # 为什么这条值得写死： CIA 与 3dsx **走两条不同的打包路径**，
        # 而 3dsx 那条每次 `build.py` 都会跑、CIA 那条要 `build.py cia`。
        # 于是"改了代码忘了带 cia 参数"会静默产出一个**内容与文件名不匹配**
        # 的交付物 —— 装到机器上跑的是旧代码，而排查方向会全部带偏
        # （用户会怀疑权限、怀疑签名、怀疑版本号）。
        if ok and os.path.exists(cia):
            mt_cia = os.path.getmtime(cia)
            mt_elf = os.path.getmtime(elf)
            if mt_cia < mt_elf - 1:
                import time as _t
                print("!! CIA 比 ELF 旧 —— 那是上一次构建的产物，本次代码没进去", flush=True)
                print("   ELF %s / CIA %s" % (_t.strftime("%m-%d %H:%M:%S", _t.localtime(mt_elf)),
                                              _t.strftime("%m-%d %H:%M:%S", _t.localtime(mt_cia))),
                      flush=True)
                print("   已中止。请跑 `python tools/build.py cia`（带 cia 参数）", flush=True)
                return 1
            print("CIA  : %s (%d bytes)" % (cia, os.path.getsize(cia)), flush=True)
            print("       ^ 这份含 am:u，可装到主菜单；验证要真机 + 手工点按",
                  flush=True)
        else:
            print("CIA build failed", flush=True)
            return 1

    print("\nBUILD OK", flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
