#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
parity_all.py — 数值一致性的**单一入口**

    python tools/guia/parity_all.py

跑三个判据，任何一个非零就整体失败：

  1. gm1_layout.py     文件布局与逐段值域指纹
  2. rope_parity.py    RoPE 段与训练端逐值比对
  3. forward_parity.py 整个前向与训练端逐层比对

═══ 为什么要一个入口 ═══════════════════════════════════════════════

这三个脚本修的是**同一类病**：C 端与训练端不一致，而 gcc 零告警、
程序不崩溃、照样能出字。它们必须**一起跑**才算一个判据 ——
只跑 rope 的话，前向里的定标错误就漏了；只跑 forward 的话，
rope 的配对方式错了会被量化误差掩盖。

分开跑的历史教训：每次都是「跑了一个 → 以为过了 → 下次改动又坏」。

退出码直接给 CI 用（0=全过）。
"""
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
SCRIPTS = ("gm1_layout.py", "rope_parity.py", "forward_parity.py")


def main():
    py = sys.executable
    failed = []
    for s in SCRIPTS:
        path = os.path.join(HERE, s)
        print("=" * 72)
        print("== %s" % s)
        print("=" * 72)
        r = subprocess.run([py, path], cwd=os.path.abspath(os.path.join(HERE, "..", "..")))
        print()
        if r.returncode != 0:
            failed.append((s, r.returncode))

    print("=" * 72)
    if failed:
        print("判定：FAIL —— %s" % "  ".join("%s(rc=%d)" % f for f in failed))
        print()
        print("注意：在这三个全过之前，不要碰性能优化。")
        print("      分不清「优化带来的收益」和「数值回归」，")
        print("      优化的每一次改动都会变成盲改。")
        return 1
    print("判定：全部通过（%d 项）" % len(SCRIPTS))
    return 0


if __name__ == "__main__":
    sys.exit(main())
