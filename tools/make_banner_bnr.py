#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
make_banner_bnr.py — 把「AI 对话框」3D 模型做成 3DS 主菜单浮起横幅的 .bnr 容器。

═══════════════════════════════════════════════════════════════════════════
为什么这个脚本存在
─────────────────────────────────────────────────────────────────────────
3DS 主菜单里那个会转的 3D 模型横幅，本质是 ExeFS 里的 "banner" 文件，
格式叫 **CBMD**（CTR Banner Model Data，3dbrew 核实）：
    +0x00  4B  magic "CBMD"
    +0x04  4B  zero
    +0x08  4B  默认(通用) CGFX 偏移
    +0x0C 12*4B 12 个区域专属 CGFX 偏移（不用就全 0）
    +0x40 0x44B 填充(0)
    +0x84  4B  CWAV 音频偏移（0 = 无音频）
    +0x88  ...  [u32 解压后大小][LZ11 压缩的 CGFX]

CGFX 必须 **LZ11 压缩**，解压后 ≤ 0x80000(512KiB)。CWAV 音频**可选**，
官网/GBAtemp 实战都确认无音频横幅是合法的（音频最长 3 秒、要 2 声道，
容易出错且没必要，这里直接省掉 -> CWAV 偏移置 0，不做 DSP-ADPCM 编码）。

正常流程用 bannertool 造这个 .bnr，但本机 devkitPro 已不带 bannertool、
且 github 下载被代理挡死。本脚本**纯 Python 复刻** CBMD 容器 + LZ11，
彻底绕开 bannertool：
  · LZ11 用「纯字面量」编码（每 ≤16 字节一个控制字节），这是任何标准
    LZ11 解码器都能解的最简形态，不冒险写回溯匹配（避免写错冻屏）。
  · CGFX 由 skyfloogle/pycgfx 把 glTF/GLB 转出来（tools/pycgfx/，已落地）。

产出：app/banner/banner_model.bnr  （makerom -banner 直接吃）

═══════════════════════════════════════════════════════════════════════════
用法
────
    python tools/make_banner_bnr.py            # 缺/旧则重转 CGFX 再打 .bnr
    python tools/make_banner_bnr.py --force     # 强制重转 CGFX
    python tools/make_banner_bnr.py --check     # 只校验现有 .bnr 结构
"""
from __future__ import print_function

import os
import struct
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
APP_BANNER = os.path.join(ROOT, "app", "banner")
GLB = os.path.join(APP_BANNER, "banner_model.glb")
CGFX = os.path.join(APP_BANNER, "banner_model.cgfx")
BNR = os.path.join(APP_BANNER, "banner_model.bnr")
PYCGFX_MAIN = os.path.join(HERE, "pycgfx", "main.py")
VENV_PY = r"C:/Users/Kevin/.workbuddy/binaries/python/envs/pycgfx/Scripts/python.exe"

CBMD_HDR_SIZE = 0x88
MAX_DECOMPRESSED = 0x80000  # 512 KiB，3dbrew 硬上限


def lz11_literal_only(data):
    """LZ11 压缩：纯字面量编码（控制字节高 4 位=0，低 4 位+1=字面量字节数 1..16）。

    这是 LZ11 的合法子集，标准解码器逐包拷贝字面量直到凑够 4 字节头声明的
    解压大小。不写任何回溯匹配，零冻屏风险。
    """
    out = bytearray(struct.pack("<I", len(data)))  # 解压后大小（小端 u32）
    i = 0
    n = len(data)
    while i < n:
        chunk = data[i : i + 16]
        out.append(len(chunk) - 1)  # 控制字节 0x00..0x0F
        out += chunk
        i += len(chunk)
    return bytes(out)


def build_cbmd(cgfx_lz11):
    hdr = bytearray(CBMD_HDR_SIZE)
    hdr[0:4] = b"CBMD"
    # hdr[4:8] 保持 0
    struct.pack_into("<I", hdr, 0x08, CBMD_HDR_SIZE)  # 默认 CGFX 偏移 = 头部之后
    # 12 个区域偏移 + 0x40 填充 默认 0
    # 0x84 CWAV 偏移 = 0（无音频）
    return bytes(hdr) + cgfx_lz11


def regen_cgfx(force=False):
    if (not force) and os.path.exists(CGFX) and os.path.exists(GLB) and \
       os.path.getmtime(CGFX) >= os.path.getmtime(GLB):
        print("CGFX : 已是最新，跳过 pycgfx 转换", flush=True)
        return True
    if not os.path.exists(VENV_PY):
        print("!! 找不到 pycgfx venv python: %s" % VENV_PY, flush=True)
        return False
    if not os.path.exists(PYCGFX_MAIN):
        print("!! 找不到 pycgfx: %s" % PYCGFX_MAIN, flush=True)
        return False
    if not os.path.exists(GLB):
        print("!! 找不到 GLB: %s  （先跑 tools/make_banner_model.py 用 Blender 生成）"
              % GLB, flush=True)
        return False
    cmd = [VENV_PY, PYCGFX_MAIN, GLB, CGFX]
    print("CGFX : pycgfx 转换 %s -> %s" % (GLB, CGFX), flush=True)
    r = subprocess.run(cmd, cwd=os.path.join(HERE, "pycgfx"))
    return r.returncode == 0 and os.path.exists(CGFX)


def validate_cgfx(path):
    d = open(path, "rb").read()
    ok = True
    if d[:4] != b"CGFX":
        print("   ! CGFX magic 不对", flush=True); ok = False
    if len(d) > MAX_DECOMPRESSED:
        print("   ! CGFX 解压后 %d > 0x80000 上限" % len(d), flush=True); ok = False
    if b"COMMON" not in d:
        print("   ! CGFX 里没有 COMMON 模型名（主菜单只认 COMMON）", flush=True); ok = False
    if b"JOINTS_0" in d or b"WEIGHTS_0" in d:
        print("   ! CGFX 含软蒙皮(JOINTS/WEIGHTS)，真机会冻屏", flush=True); ok = False
    return ok, len(d)


def check_bnr(path):
    if not os.path.exists(path):
        print("BNR  : 不存在 %s" % path, flush=True)
        return False
    d = open(path, "rb").read()
    if d[:4] != b"CBMD":
        print("   ! 魔数不是 CBMD", flush=True); return False
    cgfx_off = struct.unpack_from("<I", d, 0x08)[0]
    cwav_off = struct.unpack_from("<I", d, 0x84)[0]
    if cgfx_off != CBMD_HDR_SIZE:
        print("   ! 默认 CGFX 偏移 = 0x%X（期望 0x%X）" % (cgfx_off, CBMD_HDR_SIZE),
              flush=True); return False
    dec_size = struct.unpack_from("<I", d, cgfx_off)[0]
    lz11_start = cgfx_off + 4
    print("   CBMD 魔数 OK, 大小 %d 字节" % len(d))
    print("   默认 CGFX 偏移 0x%X, 声明解压大小 0x%X (%d)"
          % (cgfx_off, dec_size, dec_size))
    print("   CWAV 偏移 0x%X (%s)" % (cwav_off, "无音频" if cwav_off == 0 else "有音频"))
    if dec_size > MAX_DECOMPRESSED:
        print("   ! 解压大小超过 512KiB 上限", flush=True); return False
    if cwav_off != 0 and cwav_off < len(d):
        print("   CWAV 块 %d 字节" % (len(d) - cwav_off), flush=True)
    print("BNR  : 结构 OK（真机/模拟器为准）", flush=True)
    return True


def main():
    args = sys.argv[1:]
    force = "--force" in args
    check_only = "--check" in args

    if check_only:
        return 0 if check_bnr(BNR) else 1

    if not regen_cgfx(force):
        print("!! CGFX 生成失败，无法继续", flush=True)
        return 1

    ok, cgfx_size = validate_cgfx(CGFX)
    if not ok:
        print("!! CGFX 校验未过，拒绝打包（避免造出冻屏横幅）", flush=True)
        return 1

    raw = open(CGFX, "rb").read()
    lz11 = lz11_literal_only(raw)
    bnr = build_cbmd(lz11)
    with open(BNR, "wb") as f:
        f.write(bnr)
    print("BNR  : 写出 %s" % BNR)
    print("   原始 CGFX %d B -> LZ11 %d B -> CBMD .bnr %d B"
          % (cgfx_size, len(lz11), len(bnr)))
    print("   解压后 %d B (上限 0x80000=%d)，%s"
          % (cgfx_size, MAX_DECOMPRESSED,
             "OK" if cgfx_size <= MAX_DECOMPRESSED else "超限!"))
    print("   下一步：python tools/build.py cia  （脚本检测到 .bnr 会自动 -banner）")
    return 0


if __name__ == "__main__":
    sys.exit(main())
