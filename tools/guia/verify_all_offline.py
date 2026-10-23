#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""
verify_all_offline.py — 一键跑完所有**不需要模拟器**的验证

═══ 为什么有这个脚本 ═══════════════════════════════════════════════
2026-10-04 18:52 起用户要求：**不许开 Azahar 模拟器测试**。
而本项目的验收趟（`tools/azahar_test.py`）全都依赖模拟器，
所以「不能开模拟器」等于「常规验收手段全部不可用」。

这个脚本把能离线做的验证收成一条命令，且**全部零积分**
（纯本地numpy/C 复刻/文件比对，不发任何网络请求）。

    python verify_all_offline.py

它验什么、**不**验什么（这个边界必须写清楚，否则就是自欺）：
┌────────────┬──────────────────────────┬────────────────────┐
│ 验证项│ 验的是什么                │ 能替代模拟器吗     │
├────────────┼──────────────────────────┼────────────────────┤
│ verify_vocab│ 词表 / 索引 / 模型同代   │ 能（就是查文件）   │
│ verify_kb_load│ 卡上 gkb_load 会不会  │ ★ 不能验C 代码本身 │
│             │ return -N（检索能不能 │ 只验"加载得了"     │
│             │ 起来）★ verify_vocab  │                    │
│             │ 查不到这一层            │                    │
│ c_side_sim  │ 检索 + 抽取的**逻辑**    │ ★ 不能验C 代码本身 │
│ selftest_*  │ 判据与解析器自身可信     │ 能（纯逻辑）       │
└────────────┴──────────────────────────┴────────────────────┘

⚠️ **c_side_sim 是复刻品**。它验的是「按C 的逻辑重写一遍的 Python」，
   不是 `shared/nnet/*.c` 本身 —— 本机没有 native gcc，C 代码无法在 PC
   上编译执行。所以：
   · 它能可靠地抓**数据层**问题（词表错位、索引不对、语料缺词）
   · 它能提示**逻辑层**问题（我复刻时就是这么发现 extract.c 的死循环洞）
   · 但它**证明不了**extract.c 在真机上跑出同样结果
   → 结论里必须标明「未经模拟器/真机验证」，别把8/8 说成验收通过。
"""
from __future__ import print_function

import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
PY = sys.executable

SUITES = [
    ("AGENT.md 与设备端代码对账", "../check_agentdoc.py",
     "喂给模型的 system 手册：动作名/上限/路径/协议标记，逐条从 C 源码抠基准值比对"),
    ("语音输入（采集/裁剪/状态行）", "../voice_offline_check.py",
     "RMS 抽样选型 + 只上传有声段 + 状态行宽度（解析真字库）"),
    ("卡上能不能 load 知识库", "verify_kb_load.py",
     "按 gkb_load 的同源判据验 磁盘.bin 与卡上内嵌GKZ：段落超顶/游戏数不同步/magic/倒排越界"),
    ("词表三方一致性", "verify_vocab.py",
     "卡上 guide_vocab.txt / ckpt bpe.pkl / guide_kb.bin 的 V 逐条对齐"),
    ("C 侧链路复刻（检索+抽取）", "c_side_sim.py",
     "8 条抽取式用例 + UTF-8 校验，复刻 guide_kb.c 与 extract.c"),
    ("卡上候选词 g_cand_set", "verify_cand.py",
     "读回 sc_guide.c 的 211 行×6 词，逐条验证限定该游戏能命中"),
    ("--passes 解析器", "selftest_passes.py",
     "520 个趟次组合 × 4 种写法 = 2080 次比对"),
    ("X2 判据", "selftest_x2_judge.py",
     "5 种场景：全绿/有FAIL/有跳过/探针漏报/桶分错"),
]


def main():
    print("离线验证（**不开模拟器**，零积分）")
    print("工作目录：%s" % HERE)
    print()

    results = []
    for name, script, what in SUITES:
        path = os.path.join(HERE, script)
        if not os.path.exists(path):
            results.append((name, "缺文件", ""))
            print("[跳过] %-24s —— %s 不存在" % (name, script))
            continue
        try:
            # ★ 跑 **path** 不是 **script**：script 是给人看的显示名（相对 HERE），
            #   把它原样塞给 subprocess 会让 Python 按**当前目录**去解析 ——
            #   换个目录跑这个脚本，所有带 "../" 的套件就都变成"找不到文件"，
            #   而且是 FAIL 不是"跳过"，看着像判据红了。
            r = subprocess.run([PY, path], capture_output=True, timeout=180)
        except subprocess.TimeoutExpired:
            results.append((name, "超时", ""))
            print("[超时] %-24s %s" % (name, script))
            continue
        out = (r.stdout or b"").decode("utf-8", "replace")
        err = (r.stderr or b"").decode("utf-8", "replace")
        status = "PASS" if r.returncode == 0 else "FAIL"
        results.append((name, status, out))
        print("[%s] %-24s %s" % (status, name, script))
        print("       %s" % what)
        if status == "FAIL":
            # 失败时把关键行打出来 —— 「失败时的报法」比「通过时的说明」重要
            for line in out.strip().split("\n")[-12:]:
                print("       | %s" % line)
            if err.strip():
                for line in err.strip().split("\n")[-6:]:
                    print("       ! %s" % line)
        print()

    print("=" * 68)
    npass = sum(1 for _n, s, _o in results if s == "PASS")
    for name, status, _out in results:
        print("  %-4s %s" % (status, name))
    print("=" * 68)
    print("%d / %d 通过" % (npass, len(results)))

    # 复刻器的 8/8 单独强调一次 —— 防止有人把它当成"验收通过"
    for name, status, out in results:
        if name.startswith("C 侧链路复刻") and status == "PASS":
            line = [l for l in out.split("\n") if l.strip().startswith("=> 抽取")]
            if line:
                print()
                print("提醒：%s" % line[0].strip())
                print("      ⚠️ 这是**复刻品**的结果，证明不了 extract.c 在真机上的行为。")
                print("      真正的 X2 趟结论仍以模拟器/真机实测为准。")
    return 0 if npass == len(results) else 1


if __name__ == "__main__":
    sys.exit(main())
