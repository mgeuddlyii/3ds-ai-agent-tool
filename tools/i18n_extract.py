#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
i18n_extract.py —— 把界面文案从 C 源码里抽成字符串表

── 这个脚本为什么必须存在 ──
全项目界面文案是**硬编码在 C 字面量里**的（约 1600 处，散在 20 个界面文件）。
手工一条条改既不现实也必然漏 —— 漏一条的表现是"界面上忽然冒出一句中文"，
而那在截图里很难被当成 bug。

它只做**机械**的那一步：找字面量 → 发 ID → 换成 S(ID) → 导出清单。
翻译是人的活（不在这个脚本里猜译）。

── ★ 什么必须跳过（这条最重要）──
**诊断输出不许翻译。** 以下调用里的字符串是给验收脚本看的：

    app_note()      —— status.txt 的 notes，几十条断言直接匹配中文子串
    boot_mark()     —— boot.log
    actions_audit() —— action.log（ALLOW/DENY/FAIL 那条流水）
    hwtest/actprobe/featprobe... 等探针的 printf —— 同理

把它们翻成英文，判据会全红，而红的**原因不是功能坏了**。
所以本脚本按"调用上下文"跳过它们：字面量前面最近的那个标识符在
EXCLUDE_FUNCS 里就跳过。

同理，整个 app/source/test/*.c 与 sc_guide.c（攻略库是**数据**不是文案）
直接不进处理列表。
"""

import io
import json
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

CJK = re.compile(r'[一-鿿]')

# 诊断/审计类调用：里面的字符串是判据的输入，不许动
EXCLUDE_FUNCS = {
    "app_note", "boot_mark", "actions_audit", "printf", "snprintf_info",
    "fprintf", "actlog", "audit",
}

# 这些文件整个不处理：
#   sc_guide.c     —— 攻略库：游戏名/正文是**数据**，不是界面文案
#   *probe*/*test* —— 测试探针输出，判据直接匹配
SKIP_FILES = {"sc_guide.c"}
SKIP_PAT   = re.compile(r"(probe|test|fault)\.c$", re.I)


def literals(src):
    """返回 [(start, end, raw_including_quotes)]，按出现顺序。"""
    out = []
    i = 0
    n = len(src)
    while i < n:
        if src[i] == '"':
            j = i + 1
            while j < n:
                if src[j] == '\\':
                    j += 2
                    continue
                if src[j] == '"':
                    break
                j += 1
            out.append((i, j, src[i:j + 1]))
            i = j + 1
            continue
        if src[i] == "'":           # 字符常量，跳过（含转义）
            j = i + 1
            while j < n and src[j] != "'":
                j += 2 if src[j] == '\\' else 1
            i = j + 1
            continue
        # 注释：整段跳过，注释里的中文不是文案
        if src.startswith("/*", i):
            k = src.find("*/", i + 2)
            i = n if k < 0 else k + 2
            continue
        if src.startswith("//", i):
            k = src.find("\n", i)
            i = n if k < 0 else k
            continue
        i += 1
    return out


def caller_of(src, pos):
    """pos 之前最近的 '(' 前面那个标识符 —— 用来判断调用的是谁。"""
    head = src[:pos]
    # 找最后一个未闭合的 '('
    depth = 0
    idx = -1
    for k in range(len(head) - 1, -1, -1):
        c = head[k]
        if c == ')':
            depth += 1
        elif c == '(':
            if depth == 0:
                idx = k
                break
            depth -= 1
    if idx < 0:
        return ""
    m = re.search(r"([A-Za-z_][A-Za-z0-9_]*)\s*$", head[:idx])
    return m.group(1) if m else ""


def insert_include(src):
    """在最后一个 #include 之后插入 uistr.h。

    ★ 判据是"这一行**只**是 #include"：行内带 /* 或 // 的**不算** ——
    那种是注释被截断的续行，插进去就成了注释的一部分（见调用处注释）。
    找不到合适的行就插在文件第一行之后（总比不插好）。
    """
    lines = src.split("\n")
    last = -1
    for i, l in enumerate(lines[:120]):
        st = l.strip()
        if not st.startswith("#include"):
            continue
        if "/*" in st or "//" in st:
            continue                      # ★ 带注释的续行，不能当锚点
        last = i
    if last < 0:
        last = 0
    lines.insert(last + 1, '#include "uistr.h"')
    return "\n".join(lines)


def main():
    import argparse
    ap = argparse.ArgumentParser()
    ap.add_argument("--apply", action="store_true",
                    help="真的改写源码（默认是 dry-run，只报数量）")
    ap.add_argument("--only", default="", help="只处理这些文件（逗号分隔的文件名）")
    ap.add_argument("--out", default=os.path.join(ROOT, "tools", "i18n_strings.json"))
    args = ap.parse_args()

    only = set(x.strip() for x in args.only.split(",") if x.strip())

    files = []
    for d in (os.path.join(ROOT, "app", "source", "ui"),):
        for f in sorted(os.listdir(d)):
            if not f.endswith(".c"):
                continue
            if f in SKIP_FILES or SKIP_PAT.search(f):
                continue
            if only and f not in only:
                continue
            files.append(os.path.join(d, f))
    # 动作确认屏也在 ui/ 里；其余界面文件同样按上面的规则收进来

    mapping = {}          # 中文原文 -> ID
    per_file = {}
    total = 0
    skipped = 0

    # ---- 第一趟：收集所有待抽字面量（只收集，不发号）----
    # ★ 必须两趟：编号按**全局字典序**发（与 i18n_table.py 同口径），
    #   所以必须先看完所有文件才知道每句话的号。
    #   早先是一趟里"边扫边发号"，号依赖文件顺序 ——
    #   改一个文件的字面量，后面所有文件的号都会变。
    collected = []        # [(path, src, [(start,end,body)...])]
    for path in files:
        src = io.open(path, encoding="utf-8").read()
        edits = []
        for (a, b, raw) in literals(src):
            body = raw[1:-1]
            if not CJK.search(body):
                continue
            if caller_of(src, a) in EXCLUDE_FUNCS:
                skipped += 1
                continue
            # 相邻字面量会被 C 自动拼接（"aa" "bb"），抽出来会破坏拼接语义
            # —— 这里只处理"整句就是一条"的情况：后面紧跟另一个字面量就跳过
            nxt = src[b + 1:b + 40].lstrip()
            if nxt.startswith('"'):
                skipped += 1
                continue
            edits.append((a, b + 1, body))
            total += 1
        if edits:
            collected.append((path, src, edits))

    # ---- 发号：按中文原文字典序，全局唯一且稳定 ----
    bodies = sorted({b for (_, _, es) in collected for (_, _, b) in es})
    for i, body in enumerate(bodies, 1):
        mapping[body] = "S%04d" % i

    # ---- 第二趟：改写源码 ----
    for (path, src, edits) in collected:
        out = src
        for (a, b, body) in reversed(edits):
            out = out[:a] + "S(%s)" % mapping[body] + out[b:]
        if '#include "uistr.h"' not in out:
                # ★ 插在最后一个**完整行**的 #include 之后。
                # 早先的实现用 rfind('\n#include') + find('\n') 定位，
                # 会插到"半行注释"里 —— 比如
                #     #include <strings.h>   /* strcasecmp —— 判 .cia 用（
                #     #include "uistr.h"          ← 插进了这段注释中间
                #         * 这里**必须再判一次**：… */
                # 结果：include 变成注释的一部分，整个 uistr 都不生效，
                # 编译期报的是 'S0175' undeclared，指着使用处，真因在上面几十行。
                ins = insert_include(out)
                out = ins
        io.open(path, "w", encoding="utf-8").write(out)
        per_file[os.path.basename(path)] = len(edits)

    print("待抽取字面量：%d 条（去重后 %d 条）· 跳过的诊断输出 %d 条"
          % (total, len(mapping), skipped))
    for f, n in sorted(per_file.items(), key=lambda kv: -kv[1]):
        if n:
            print("   %4d  %s" % (n, f))

    if not args.apply:
        print("\n（dry-run，没有改写任何文件。加 --apply 才会真的改。）")

    with io.open(args.out, "w", encoding="utf-8") as fh:
        json.dump(mapping, fh, ensure_ascii=False, indent=1, sort_keys=True)
    print("清单已写入：%s（%d 条）" % (args.out, len(mapping)))
    print("★ 编号按**中文原文字典序**发，与 tools/i18n_table.py 同口径 ——"
          "改一个字面量的位置不会牵动别的号。")
    return 0


if __name__ == "__main__":
    sys.exit(main())
