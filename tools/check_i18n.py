#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
check_i18n.py —— 文案表的判据（不靠肉眼）

── 为什么译文写完不能直接编译就算过 ──
译文是我逐条写的，而 3DS 上一个格式占位符写错的后果是**读越界**：
printf 少一个 %s 就把后面的栈内容当字符串吐出来，屏幕上是一串乱码，
严重时直接崩。这类错**编译期抓不到**、**不崩也能过**，只在真机上偶发。

所以这里查三件机器能查、眼睛查不了的事：

  A. **占位符一致**：中英两侧的 %s / %d / %ld / %.*s / %u 个数与**顺序**一致。
     顺序也要查：两个 %s 顺序反了不会崩，但内容会串（"把 a 移到 b" 变成
     "把 b 移到 a"），这种错比乱码更难发现。

  B. **动作协议名未被翻译**：file.write 这类 token 出现在译文里必须与
     原文逐字相同。翻译它们会让模型输出的命令对不上。

  C. **英文残留**：译文里还有 CJK 字符 = 没翻完，界面上会混进中文。
     唯一例外是文件名/命令（config.json、creds.enc、GodMode9…），
     那些是**不该翻**的专名 —— 用白名单放过。

  D. **反向自测**（--selftest）：故意造三条坏译文（少一个 %s、
     协议名被译、残留中文），判据必须判红。
     没有自测的判据是"永远绿的假守卫"，这是本项目的铁律。
"""

import io
import json
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

STRINGS = os.path.join(ROOT, "tools", "i18n_strings.json")
EN      = os.path.join(ROOT, "tools", "i18n_en.json")

# printf 占位符：%s %d %ld %u %.*s %c ... 以及 %% 不算占位符
SPEC = re.compile(r"%(?:\.\*)?(?:hh|h|ll|l|z|j|t)?[sudfFeEgGxXocsp]")
# 协议 token：小写下划线点分，且出现在动作语境里
TOKEN = re.compile(r"\b[a-z][a-z0-9_]*(?:\.[a-z][a-z0-9_]*)+\b")
# CJK
CJK = re.compile(r"[\u4e00-\u9fff\u3000-\u303f\uff00-\uffef]")

# 允许出现在英文里的 CJK：专有名词/文件名/命令，不该翻
CJK_ALLOW = (
    "config.json", "creds.enc", "key.txt", "status.txt", "action.log",
    "boot.log", "chat_", "GodMode9", "SysNAND", "Luma", "locale.txt",
    "NAND", "apt", "models", "pid", "tid",
)
# 协议 token 白名单：这些**不是**动作协议，别拿它们报"被翻译"
TOKEN_OK = {
    "e.g", "i.e", "etc",
}


def specs(s):
    """取出占位符序列（顺序敏感）。%% 会被当成字面百分号，不算。"""
    return SPEC.findall(re.sub(r"%%", "", s))


def tokens(s):
    return [t for t in TOKEN.findall(s) if t not in TOKEN_OK]


def check(zh_map, en_map, report):
    fail = []

    # ★ 清单的形状是 {中文原文: 编号}（key 是原文，value 是编号）——
    # 反查成 {编号: 原文} 才能按编号取到中文。方向搞反的话，
    # 每一条都会被报成"中文侧没有占位符"，而真因是判据自己读错了字段。
    by_id = {}
    for zh, sid in zh_map.items():
        by_id[sid] = zh

    ids = sorted(by_id)
    for sid in ids:
        zh = by_id[sid]
        en = en_map.get(sid)

        if en is None:
            fail.append("%s 没有英文译文" % sid)
            continue
        if not en.strip():
            fail.append("%s 英文译文是空串" % sid)
            continue

        # ---- A 占位符 ----
        zs, es = specs(zh), specs(en)
        if zs != es:
            fail.append("%s 占位符不一致：中文 %s / 英文 %s" % (sid, zs or "无", es or "无"))

        # ---- B 动作协议名 ----
        for t in set(tokens(zh)):
            if t in TOKEN_OK:
                continue
            if t not in en:
                fail.append("%s 协议名 '%s' 在英文里不见了（不能翻译）" % (sid, t))

        # ---- C 英文里残留中文 ----
        # 放过不该翻的专名（config.json、GodMode9…）：它们是**文件名/命令**，
        # 译了反而错。放行方式是先从译文里剔掉这些词，再看还有没有 CJK。
        tmp = en
        for allow in CJK_ALLOW:
            tmp = tmp.replace(allow, "")
        stray = sorted(set(CJK.findall(tmp)))
        if stray:
            fail.append("%s 英文里还有中文：%s" % (sid, "".join(stray)))

    ok = not fail
    report.append("C 文案 : %d 条 · 占位符/协议名/残留中文 检查%s"
                  % (len(ids), "通过" if ok else "发现 %d 个问题" % len(fail)))
    return ok, fail


def selftest():
    """反向自测：造三条坏译文，判据必须全判红。

    ★ 这里的输入形状必须和真实清单一致（{中文: 编号}），
    否则自测走的是一条判据平时不会走的路径，绿了也说明不了什么。
    """
    cases = [
        # 名字,           中文原文,         英文（故意写坏）,       期望出现的字样
        ("少一个占位符",   "已移动 %s 到 %s", "Moved %s",             "占位符"),
        # 这条要写对：原文里的 token 是 file.write，译文里换成了 "file save"，
        # token 整体不见了 → 抓"协议名不见"。若译文里**原样保留** file.write，
        # 那是正确的，不该判红（第一版用例写成那样，自测立刻暴露了它自己）。
        ("协议名被改写",   "已执行 file.write", "Done: file save",    "协议名"),
        ("残留中文",       "完成",           "Done 完成",            "中文"),
    ]
    ok = True
    for name, zh, en, expect in cases:
        rep = []
        _, fail = check({zh: "S0001"}, {"S0001": en}, rep)
        hit = any(expect in f for f in fail)
        print("   %-14s %s%s" % (name, "判红 ✓" if hit else "没判红 ✗",
                                 "" if hit else "  （实际：%s）" % (fail or "无")))
        if not hit:
            ok = False
    if ok:
        print("\n  ✓ 自测通过：三条坏译文都被抓住了")
    else:
        print("\n  !! 自测失败：判据抓不住某些坏译文 —— 它是永远绿的假守卫")
    return ok


def main():
    if "--selftest" in sys.argv:
        return 0 if selftest() else 1

    with io.open(STRINGS, encoding="utf-8") as fh:
        zh_map = json.load(fh)
    with io.open(EN, encoding="utf-8") as fh:
        en_map = json.load(fh)

    report = []
    ok, fail = check(zh_map, en_map, report)
    print("\n".join(report))
    if fail:
        print("判红 :")
        for f in fail[:40]:
            print("    - " + f)
        if len(fail) > 40:
            print("    ... 还有 %d 条" % (len(fail) - 40))
    else:
        print("结论 : 通过 —— 格式占位符、协议名、残留中文都对。")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())