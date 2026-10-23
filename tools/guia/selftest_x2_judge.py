#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""
selftest_x2_judge.py — 验证 X2 趟的判据在**四种场景**下都会给出正确结论

═══ 为什么需要它 ═══════════════════════════════════════════════════
X2 判据自己出过错，而且是「探针全绿、判据报红」那种：
    fails = [l for l in enotes if "FAIL" in l]
命中了探针的**汇总行** `汇总 OK=8 FAIL=0 跳过=0` ——
一条FAIL 都没有，判据反而报「有 FAIL 的用例」。

★ 铁律：**判据必须能区分两条路的不同输出**。
所以这里不只验「全绿时判绿」，还要验：
    ① 全绿        ->判绿
    ② 有FAIL     -> 判红，且**报的是那条用例**而不是汇总行
    ③ 有跳过      -> 判红（跳过不是OK，X2 要求 0 跳过）
    ④ 探针漏报（汇总说 0 FAIL 但逐条有 FAIL 行）-> **判红**
       ↑ 这一条最关键：判据不能只信探针自己的汇总
    ⑤ 汇总与逐条不一致 -> 判红（说明有一类被归错桶）

用法：python selftest_x2_judge.py     退出码 0 = 判据可信
"""
from __future__ import print_function

import os
import re
import sys

# 与 app/source/test/exttest.c 的 CASES[] 一致
QS = ["火焰纹章觉醒 转职怎么转", "时之笛 钩索箭哪里拿", "怪物猎人4G 弱点是什么",
      "路易吉洋馆2 吸尘器怎么用", "马力欧卡丁车7 漂移技巧", "大师之剑在哪里拿",
      "沙之杖怎么用", "梅祖拉的假面 三天循环怎么过"]


def make(bad_idx=None, skip_idx=None, lie_summary=None):
    """造一份 exttest: 输出。格式**逐字照抄** exttest.c 的 app_note 格式串。"""
    out = ["exttest: ===== 开始 =====",
           "exttest: 词表 ok v=4096 merges=3840",
           "exttest: 索引 ok 段落=1174 游戏=31"]
    for i, q in enumerate(QS):
        if i == skip_idx:
            out.append("exttest: [%d] %s  检索 0 命中  gid=2 rest=[ %s]" % (i, q, q))
        elif i == bad_idx:
            out.append("exttest: [%d] %s  FAIL 答案里没有「X」" % (i, q))
        else:
            out.append("exttest: [%d] %s  OK  %d 字节  %s答案" % (i, q, 100 + i, q))
    n_ok = 8 - (1 if bad_idx is not None else 0) - (1 if skip_idx is not None else 0)
    n_bad = 1 if bad_idx is not None else 0
    n_skip = 1 if skip_idx is not None else 0
    if lie_summary is not None:
        n_ok, n_bad, n_skip = lie_summary
    out.append("exttest: 汇总 OK=%d FAIL=%d 跳过=%d / 共 8" % (n_ok, n_bad, n_skip))
    out.append("exttest: RESULT=%s" % ("FAIL" if n_bad else "OK"))
    out.append("exttest: ===== 结束 =====")
    return out


# ---- 判据（与 tools/azahar_test.py 的 X2 分支保持一致）----
def judge(enotes):
    """返回 (problems, stats)。与 azahar_test.py 里那段逐行对应。"""
    problems = []
    case = [l for l in enotes if re.match(r"^exttest:\s*\[\d+\]", l)]
    # ★ 只认用例行，且要求 "  OK  "/"  FAIL " 的**前置两空格 + 词 + 空白**。
    #   不能用 "FAIL" in l —— 汇总行 `FAIL=0` 也含它。
    oks = [l for l in case if re.search(r"\s\sOK\s", l)]
    fails = [l for l in case if re.search(r"\s\sFAIL\s", l)]
    skipped = [l for l in case if "检索 0 命中" in l]
    stats = dict(case=len(case), ok=len(oks), fail=len(fails), skip=len(skipped))

    summ = next((l for l in enotes if "exttest: 汇总" in l), None)
    if not summ:
        problems.append("没有汇总行")
        return problems, stats
    m = re.search(r"OK=(\d+)\s+FAIL=(\d+)\s+跳过=(\d+)", summ)
    if not m:
        problems.append("汇总行格式变了，判据抓不住：" + summ)
        return problems, stats
    s = tuple(int(x) for x in m.groups())
    if s != (len(oks), len(fails), len(skipped)):
        problems.append("汇总%s与逐条(OK %d/FAIL %d/跳过 %d)对不上"
                        % (s, len(oks), len(fails), len(skipped)))
    if fails:
        problems.append("有 FAIL 的用例：" + fails[0])
    if len(oks) != 8:
        problems.append("OK 只有 %d 条（应 8）" % len(oks))
    if skipped:
        problems.append("跳过 %d 条" % len(skipped))
    return problems, stats


CASES = [
    ("① 全绿（8 条 OK）", make(), False),
    ("② 有 1 条 FAIL", make(bad_idx=3), True),
    ("③ 有 1 条检索 0 命中", make(skip_idx=1), True),
    ("④ 探针漏报：汇总说 0 FAIL 但逐条有 FAIL", make(bad_idx=5, lie_summary=(7, 0, 0)), True),
    ("⑤ 汇总与逐条不一致（跳过被算成 FAIL）", make(skip_idx=2, lie_summary=(7, 1, 0)), True),
]


def main():
    print("X2 判据自测（判据必须能区分不同输出）\n")
    allok = True
    for name, enotes, should_fail in CASES:
        probs, stats = judge(enotes)
        got_fail = bool(probs)
        mark = "OK " if got_fail == should_fail else "!! "
        if got_fail != should_fail:
            allok = False
        print("%s%-42s 期望%s 实际%s  统计=%s"
              % (mark, name,
                 "红" if should_fail else "绿",
                 "红" if got_fail else "绿", stats))
        for p in probs:
            print("      · " + p)
    print()
    if not allok:
        print("!! 判据不可信 —— 至少一个场景的结论与预期相反")
        return 1
    print("OK —— 5 个场景的判据结论全部符合预期")
    return 0


if __name__ == "__main__":
    sys.exit(main())
