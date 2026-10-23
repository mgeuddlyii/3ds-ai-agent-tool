#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
i18n_table.py —— 由清单生成 shared/uistr_ids.h + shared/uistr_table.c

── 它和 i18n_extract.py 的分工 ──
    i18n_extract.py   源码里的中文字面量 → 编号 → 换成 S(Sxxxx)   （动源码）
    i18n_table.py     编号 + 译文     → C 枚举 + C 表格            （只生成）
    人（我）           译文本身

把"发号"和"写表"分成两个脚本，是因为它们的**出错代价完全不同**：
发号错了 = 源码里编号对不上，编译期就炸（可发现）；
译文错了 = 界面上某句话别扭，只有真看得出来（难发现）。
所以前者交给机器，后者留给人。

★ ★ 三条纪律（每一条都是被坑出来的）★

1. **编号是位置，不是内容**（和 game_id 同一条纪律）。
   S0123 现在对应哪句中文，取决于编号规则。改文案 → 重跑抽取 → 号会变。

2. **编号空洞是合法的**。
   早先要求 1..N 严格连续，结果人工补录一条时忘了占下一个号，
   构建就一句"编号不连续"卡住 —— 而那条要求**不带来任何好处**：
   表与枚举都按下标一一对应即可，空洞只是浪费一个下标位。
   枚举项数（UISTR_LAST）才是唯一要对齐的东西。

3. **绝不在这里重排编号**。
   ★ 曾写过一版"按字典序自动重排 + 同步改源码"，看着聪明，
   实则危险：重排会让源码里所有 S(旧号) 失效，必须**同时**精确改对每一个；
   而改错一个的后果是"界面上某句话变成了**别的话**"——
   表合法、编译通过、判据全绿，只有真机截图才看得出来。
   那种错一旦发生，排查成本远高于"维护时手抄一下编号"。
   所以：**编号只由 i18n_extract.py 发，这里照着发。**
"""

import io
import json
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MANIFEST = os.path.join(ROOT, "tools", "i18n_strings.json")
EN_JSON  = os.path.join(ROOT, "tools", "i18n_en.json")
TABLE_C  = os.path.join(ROOT, "shared", "uistr_table.c")
TABLE_H  = os.path.join(ROOT, "shared", "uistr_ids.h")

TODO = "TODO-EN"


def c_str(s):
    """C 字符串字面量。

    要转义的不只是引号和反斜杠 —— **换行也是**。
    译文里写 "\\n" 时 Python 读出来是**真的换行符**，直接塞进 C 源码
    就是 "missing terminating \\" character"，而且编译器只报后面几行，
    真正的原因在上面 —— 这类错最费时间，必须在生成时就挡住。

    顺序也重要：先归一化换行，再逐字符转义。
    """
    s = s.replace("\r\n", "\n").replace("\r", "\n")
    out = []
    for ch in s:
        if ch == "\\":
            out.append("\\\\")
        elif ch == '"':
            out.append('\\"')
        elif ch == "\n":
            out.append("\\n")
        elif ch == "\t":
            out.append("\\t")
        elif ord(ch) < 0x20:
            out.append("\\%03o" % ord(ch))
        else:
            out.append(ch)
    return '"%s"' % "".join(out)


def load_tables():
    """读清单 + 译文。返回 (ids, by_id, en_map)；不合法返回 None。

    ids 是**按编号排序**的编号列表，by_id 是 {编号: 中文原文}。
    编号必须连续（1..N）—— 因为表按下标一一对应；
    这里不接受空洞，是为了让"枚举项数"与"数组长度"永远相等。
    """
    if not os.path.exists(MANIFEST):
        print("!! 找不到清单 %s —— 先跑 tools/i18n_extract.py" % MANIFEST)
        return None
    with io.open(MANIFEST, encoding="utf-8") as fh:
        zh_map = json.load(fh)          # ★ {中文原文: 编号}，方向别搞反

    if not os.path.exists(EN_JSON):
        print("!! 找不到译文 %s" % EN_JSON)
        return None
    with io.open(EN_JSON, encoding="utf-8") as fh:
        en_map = json.load(fh)          # {编号: 英文}

    by_id = {}
    for zh, sid in zh_map.items():      # ★ 反查：清单的 key 是原文
        by_id[sid] = zh

    nums = sorted(int(x[1:]) for x in by_id)
    if nums != list(range(1, len(nums) + 1)):
        holes = [i for i in range(1, nums[-1] + 1) if i not in set(nums)]
        print("!! 编号必须连续（1..%d），现在缺：%s"
              % (len(nums), holes[:12]))
        print("   补录文案时占用了哪个号？改 tools/i18n_strings.json "
              "里那条的编号。")
        return None

    ids = sorted(by_id)

    missing = [i for i in ids if i not in en_map]
    todo = [i for i in ids if i in en_map and en_map[i].strip() == TODO]
    if missing:
        print("!! 缺译文 %d 条，界面会露出占位符：%s"
              % (len(missing), " ".join(missing[:12])))
        return None
    if todo:
        print("!! 译文还是占位符 %d 条：%s" % (len(todo), " ".join(todo[:12])))
        return None

    return ids, by_id, en_map


def selftest():
    """反向自测：c_str 写错时必须自己抓得到。

    三种真实踩过的坑：
      1. 真换行没转义 → 编译器报 "missing terminating"，但错在几百行之前
      2. 反斜杠没转义 → 路径 `SD:\\3ds` 会变成 `\3`（八进制转义！）
      3. 正文里的引号没转义 → 字符串提前结束，后面全乱
    """
    cases = [
        ("换行未转义", "line1\nline2", '"line1\\nline2"'),
        ("反斜杠未转义", "a\\b", '"a\\\\b"'),
        ("引号未转义", 'say "hi"', '"say \\"hi\\""'),
        ("制表符未转义", "a\tb", '"a\\tb"'),
    ]
    ok = True
    for name, raw, want in cases:
        got = c_str(raw)
        good = (got == want)
        print("   %-12s %s" % (name, "✓" if good else
                                "✗ 得到 %s，应为 %s" % (got, want)))
        if not good:
            ok = False
    print("\n  %s" % ("✓ 自测通过" if ok else "!! 自测失败：转义函数有问题"))
    return ok


def write_files(ids, by_id, en_map):
    # ---- 头文件：编号枚举 ----
    H = []
    H.append("/*")
    H.append(" * uistr_ids.h —— **自动生成，不要手改**"
             "（生成器 tools/i18n_table.py）")
    H.append(" *")
    H.append(" * 枚举而不是宏：源码里写 S(S0160)，S0160 必须是**编译期常量**")
    H.append(" * 才能当数组下标。这么写的额外好处是 —— 写错编号（比如 S9999）")
    H.append(" * 编译器当场报 undefined，而不会等到运行时读越界、在屏上吐乱码。")
    H.append(" *")
    H.append(" * ★ 编号对应的是**位置**，不是内容：S0160 现在是「发送」，")
    H.append(" *   改了文案重跑抽取后可能变成别的话。所以**不要**在代码里")
    H.append(" *   按编号的**含义**写逻辑，只按它取字符串。")
    H.append(" */")
    H.append("")
    H.append("#pragma once")
    H.append("")
    H.append("enum {")
    for sid in ids:
        H.append("    %s = %d," % (sid, int(sid[1:])))
    H.append("    UISTR_LAST = %d" % len(ids))
    H.append("};")
    io.open(TABLE_H, "w", encoding="utf-8").write("\n".join(H) + "\n")

    # ---- 实现文件：两张表 + 取表函数 ----
    lines = []
    A = lines.append
    A("/*")
    A(" * uistr_table.c —— **自动生成，不要手改**")
    A(" *")
    A(" * 生成器：tools/i18n_table.py（由 tools/build.py 在编译前调用）")
    A(" * 清单：  tools/i18n_strings.json（中文原文 → 编号）")
    A(" * 译文：  tools/i18n_en.json（编号 → 英文）")
    A(" * 编号枚举在 uistr_ids.h")
    A(" *")
    A(" * 改文案的正确姿势：改源码里的中文 → 跑 i18n_extract.py --apply")
    A(" * → 在 i18n_en.json 里补译文 → 重新编译。**不要手编这个文件。**")
    A(" */")
    A("")
    A('#include "uistr.h"')
    A("")
    A("static const char *const g_zh[] = {")
    for sid in ids:
        A("    %s,  /* %s */" % (c_str(by_id[sid]), sid))
    A("};")
    A("")
    A("static const char *const g_en[] = {")
    for sid in ids:
        A("    %s,  /* %s */" % (c_str(en_map[sid]), sid))
    A("};")
    A("")
    A("/* 供 uistr.c 用（数组本体是 static，只有这几个能出去） */")
    A("const char *const *uistr_tbl(int lang)")
    A("{")
    A("    return lang ? g_en : g_zh;")
    A("}")
    A("")
    A("int uistr_count(void) { return UISTR_LAST; }")

    io.open(TABLE_C, "w", encoding="utf-8").write("\n".join(lines) + "\n")


def verify_table():
    """生成完把文件读回来扫一遍 —— 不信自己的转义函数。

    转义错了，C 编译器只会报后面那几行，真因在上面，
    人得自己往上翻几百行才知道是哪句话。

    判据按 C 的语义数引号：\\" 是**字面量里的一个字符**，不结束字符串。
    ★ 早先用 line.count('"') % 2 判红，把 S0460（正文里有个 \\"）
    误判成转义坏了 —— 假守卫比没守卫更坏，它让人去查没问题的地方。
    """
    bad = []
    for ln, line in enumerate(io.open(TABLE_C, encoding="utf-8"), 1):
        if not line.lstrip().startswith('"'):
            continue
        code = line.split("/*")[0]     # 注释里的引号不算
        n, i = 0, 0
        while i < len(code):
            if code[i] == "\\" and i + 1 < len(code):
                i += 2
                continue
            if code[i] == '"':
                n += 1
            i += 1
        if n % 2 != 0:
            bad.append(ln)
    if bad:
        print("!! 生成的表第 %s 行字符串没闭合 —— 转义函数有问题，"
              "这种错编译器只会报后面几行" % bad[:8])
        return False
    return True


def main():
    if "--selftest" in sys.argv:
        return 0 if selftest() else 1

    loaded = load_tables()
    if loaded is None:
        return 1
    ids, by_id, en_map = loaded

    write_files(ids, by_id, en_map)
    if not verify_table():
        return 1

    # 枚举项数必须与数组长度相等。两者不同步的表现是
    # "界面上某句话变成了别的话" —— 不崩、不报错，极难发现。
    if len(ids) != len(by_id) or len(ids) != len(en_map):
        print("!! 条数不一致：编号 %d / 中文 %d / 英文 %d"
              % (len(ids), len(by_id), len(en_map)))
        return 1

    print("   uistr_table : %d 条（枚举 + 双语表 + 引号自检）" % len(ids))
    return 0


if __name__ == "__main__":
    sys.exit(main())