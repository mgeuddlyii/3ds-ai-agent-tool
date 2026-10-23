#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""离线攻略语料（tools/guia/corpus/guide_*.txt）格式校验器。

校验的内容（每一条都必须是「坏数据会红」的真判据，不是走过场）：
  1  文件非空、体积不低于 MIN_BYTES
  2  至少有一个 `## ` 小节标题
  3  正文段落长度不低于 MIN_PARA（太短说明是凑行不是内容）
  4  每个小节至少一个 Q: 且 A: 非空
  5  同一文件内 Q: 提问文本不重复（复制粘贴 / 模板套娃）
  6  UTF-8 可解码、没有替换字符
  7  没有「作为 AI / 根据我的知识」这类暴露幻觉的措辞
  8  小节标题不重复
  9  流程小节（标题含 主线/流程/步骤/推进/顺序）必须真写成「逐步」：
     ≥ FLOW_MIN_STEPS 个渐进序号（第N步/第N天/第N关/Step N）、
     每条步骤正文 ≥ FLOW_MIN_STEP_CHARS 汉字、且该节 ≥ 6 组步级问答。
     ⚠️ 这条是 2026-10-05 补的。起因：扫「主线全流程完成度」时，
     宽泛判据（正文里出现「主线」「流程」四个字就算有）放行了 138 款——
     其中 66 款挂着一个 `## 主线流程` 标题，点进去通篇是没有序号的叙述段，
     72 款连标题都没有（如 mk7 八个小节一个流程都没有）。
     用户要的是「每一步他该干什么」，叙述段不算。
     把这条判据挪进常规校验，就不用每次靠手写扫描脚本自我欺骗了。

自带自测夹具（--selftest）：临时目录里造坏样本，逐条证明判据会红。
没有这一步，这个脚本可能永远绿 —— 那样它比没有更坏。

用法：
    python tools/guia/validate_corpus.py            # 校验 corpus 目录
    python tools/guia/validate_corpus.py --selftest # 只跑自测
    python tools/guia/validate_corpus.py --min 14000  # 调高体积门槛
"""

import os
import re
import sys
import tempfile

MIN_BYTES = 10000      # 低于这个体积基本是半成品（现有样本最小 14053）
# 门槛是按全库 1782 段实测分布标定的：最短 29、中位 69、90 分位 96。
# 早先拍脑袋写 80 → 命中 20% 的**正常**段落，是「永远红的守卫」；
# 40 也会误杀 41 段（看了内容，像「三只都能进化两次…」都是真提示）。
# 取 25 后正常数据最近 4 字余量，仍能把「凑行」抓出来。
MIN_PARA  = 25
MIN_ANS   = 10         # A: 答案最短长度
MAX_ANS   = 400        # A: 答案上限，超了说明段落混进来或生成串了

# ---- 第 9 条：流程小节必须写成「逐步」 ----
#
# ⚠️ 判据范围收窄过一次（2026-10-05）：
#    v1 把「只要标题里有 主线/流程/步骤/推进/顺序 就算流程小节」一律按硬判据要求 ≥8 步，
#    结果 `## 初期流程要点`、`## 通关流程与 BOSS 攻略` 这类本来就是「要点集合」的小节
#    被误杀（guide_zelda_oot3d 就是这么红的）——那是判据过严，不是数据坏了。
#    「要点」和「逐步流程」是两种东西，不该用同一把尺子量。
#    v2：标题里出现「逐步 / 全流程 / 序列」→ 硬判据（必须 ≥8 步）；
#        只出现「主线/流程/步骤/推进/顺序」→ 软判据（只提示，不计 FAIL）。
# 必须带 re.M：check_flow 里拿它去 `match(单行)` 时没差别，但 tmp/scan_flow2.py
# 拿它做 `finditer(全文)` 扫每个 `## ` 标题 —— 少了 re.M，`^` 只匹配整段开头，
# 于是 211 款全被报成「没有流程标题」（缺口被算成 210 款，明显假）。
FLOW_HEAD_RE = re.compile(r"^##\s*.*(主线|流程|步骤|推进|顺序)", re.M)
# ⚠️ 这个正则被修过一次（2026-10-05）：
#    v1 形如 `第\s*[0-9一二三四五六七八九十百]+\s*[步天关话幕章节]`，
#    结果正文里的「第一章」也会被当成序号 —— 于是 `第1步：从第一章开始…`
#    里「第1步」和「第一章」之间只剩一个「：」，判据报「该步只有 2 字（凑数）」，
#    而那一步明明写了四十多个字。判据自己把好数据判坏了。
#    v2 拆成两条：阿拉伯数字允许任意叙事后缀（第1关/第1章/第1幕都是步骤），
#    汉字数字只允许 步/天/轮/层 这类**动作序位**，「第一章」不再误伤。
#
#    ⚠️ v3（紧接着 v2 又踩一脚）：汉字数字分支一开始也放了「关」，
#       于是 `第2步：第一关先搞明白目标…` 里紧跟的「第一关」被当成下一个序号，
#       chunk 只剩一个「：」—— 又是一次误报（toki_tori_3d 全中招）。
#
#    ⚠️ v4（**误伤的真正根因，前两版都在治标**）：阿拉伯数字分支仍留着「关」，
#       `第3步 第1关先冲杂兵段…` 里的「第1关」被当成第 3 步的下一个序号，
#       chunk 被截成 0 字 —— dodonpachi 第 3 步明明写了六十多字却被判「凑数」。
#       一路砍后缀字符是治标（关/章/节/幕 每一个都可能再踩一次）。
#       真正的判据是**「渐进」= 同一后缀的序号严格递增**：
#       关卡游戏写「第1关→第2关→…→第10关」照样算逐步流程（那是关卡推进），
#       而 `第3步 第1关先冲…` 里「关」组只出现两次且重复（1,1），不构成序列被丢弃，
#       「步」组 1,2,3… 严格递增被保留。见 _flow_progressive()。
#    ⚠️ v5：**序号必须落在行首**。v4 只判「同后缀严格递增」，结果正文里引用
#       「所以第 4 步留下来…」「跟第 4 步那条线重合」这类**引用**被当成独立步骤，
#       和真步骤撞成重复序号 → 整组判废 → 明明写了 12 步却报「0 个序号」
#       （bokumono_3 12 步 / brunswick_pro_bowling 11 步都这么被误伤）。
#       真步骤一律 `第N步：` 独占一段的行首，引用永远夹在句子中间。
FLOW_STEP_RE = re.compile(
    r"^[ \t>]*第\s*(?:([0-9]{1,3})|([一二三四五六七八九十百]+))"
    r"\s*([步天关幕章节话回轮层阶])", re.M)
FLOW_STEP_ALT_RE = re.compile(
    r"^[ \t>]*[Ss][Tt][Ee][Pp]\s*[-_ ]?([0-9]{1,3})", re.M)
# 「第N步」这类序号后面允许的后缀字符；同一后缀下的序号必须严格递增，才算「渐进」
FLOW_ORD_SUFFIX = "步天关幕章节话回轮层阶"
FLOW_MIN_STEPS      = 8    # 少于 8 步达不到「全流程逐步」的密度
FLOW_MIN_STEP_CHARS = 30   # 单步正文太短就是一句话凑数
FLOW_MIN_STEP_QA    = 6    # 步级问答最少 6 组，不然问不出「XXX 步该干什么」

SECTION_RE = re.compile(r"^##\s+(\S.*?)\s*$")
QA_RE      = re.compile(r"^Q\s*[:：]\s*(.*?)(?:\s*A\s*[:：]\s*(.*))?$")

# 只在这些词出现在答案里才算「暴露 AI 幻觉」；命中即 FAIL
HALLU_RE = re.compile(
    r"(作为\s*AI|作为\s*一个\s*AI|作为大语言模型|作为语言模型|"
    r"根据我的知识库|根据我的训练数据|我无法访问|我不能保证准确)"
)


def parse(text):
    """把文本文件切成 (kind, payload) 序列。

    kind: 'section' | 'prose' | 'qa'
    """
    out = []
    for raw in re.split(r"\n\s*\n", text):
        block = raw.strip()
        if not block:
            continue
        first = block.split("\n")[0].strip()
        if first.startswith("##"):
            m = SECTION_RE.match(first)
            if m:
                out.append(("section", m.group(1)))
            else:
                out.append(("section", first[2:].strip()))
            continue
        if first.startswith("Q") and (":" in first or "：" in first):
            out.append(("qa", block))
            continue
        out.append(("prose", block))
    return out


def split_qa_block(block):
    """一个「块」里可能挤着多对 Q:/A:。

    guide_zelda_oot3d.txt 里出现过 `Q: 一 A: 一Q: 二 A: 二Q: 三 A: 三`
    这种一行挤好几对的写法（一行最多 4 个 Q），不切开会只数出第一对，
    后面那些问答在校验里就凭空消失了 —— 那是个静默的假绿。
    """
    parts = re.split(r"(?=Q\s*[:：])", block)
    return [p for p in parts if p.strip()]


def split_qa(block):
    """把 Q:/A: 块拆成 (q, a)。支持单行 `Q: ... A: ...` 与两行 `Q:` / `A:`。"""
    lines = [l.strip() for l in block.split("\n") if l.strip()]
    q = a = None
    for i, l in enumerate(lines):
        if l.startswith("Q") and (":" in l or "：" in l):
            rest = l[1:].lstrip(":：").strip()
            # 同行还带 A：
            m = re.match(r"^(.*?)\s*A\s*[:：]\s*(.*)$", rest)
            if m and m.group(2).strip():
                q, a = m.group(1).strip(), m.group(2).strip()
            else:
                q = rest
                nxt = lines[i + 1] if i + 1 < len(lines) else ""
                if nxt.startswith("A") and (":" in nxt or "：" in nxt):
                    a = nxt[1:].lstrip(":：").strip()
        elif l.startswith("A") and (":" in l or "：" in l) and q is not None and a is None:
            a = l[1:].lstrip(":：").strip()
    return q, a


_CN_ORD = {"零": 0, "一": 1, "二": 2, "三": 3, "四": 4, "五": 5,
           "六": 6, "七": 7, "八": 8, "九": 9, "十": 10, "百": 100}


def _cn2int(s):
    """把「一」「十二」「三十五」这类汉字数字转成整数。认不出来返回 None。"""
    total, cur = 0, 0
    for ch in s:
        v = _CN_ORD.get(ch)
        if v is None:
            return None
        if v in (10, 100):
            total += (cur if cur else 1) * v
            cur = 0
        else:
            cur = v
    return total + cur


def _flow_ordinal(m):
    """一个序号 match 的 (写法, 序号, 后缀)；不是序号返回 None。

    同时兼容 `Step 3`（写法 '_'、后缀 '_'）。
    """
    g = m.groups()
    if g[2]:                      # 第N步 / 第N关 …
        if g[0] is not None:
            return "ar", int(g[0]), g[2]
        n = _cn2int(g[1])
        return ("cn", n, g[2]) if n is not None else None
    g2 = FLOW_STEP_ALT_RE.match(m.group(0))
    if g2:
        return "_", int(g2.group(1)), "_"
    return None


def _flow_progressive(matches):
    """只保留「同写法同后缀、且序号严格递增」的序号 —— 这才是「渐进」。

    假序号（关卡正文里顺带出现的「第1关」、问答里提到的「第5步」）要么落进
    达不到递增的组里被丢掉，要么那一组根本凑不满，都不会污染「步数」和「每步字数」。
    返回 (保留的 match, 被丢弃的序号数)。

    ⚠️ 分组键必须带「写法」（阿拉伯 / 汉字 / Step）：同一小节里 `第1步` 和 `第一步`
       混着写（we_football 就这种现象），只按后缀分组会把它们算成「序号 1 出现两次」
       从而整组判为非序列 —— 明明写了十几步，反而报 0 个序号。**写法不同就是两串**。
    """
    groups = {}
    for m in matches:
        o = _flow_ordinal(m)
        if o is None:
            continue
        groups.setdefault((o[0], o[2]), []).append((o[1], m))
    keep, drop = [], 0
    for _key, items in groups.items():
        seq = sorted(items, key=lambda x: x[0])   # 只按序号升序（同号比 Match 会 TypeError）
        seen, last, ok = set(), None, True
        for n, _m in seq:
            if n in seen or (last is not None and n <= last):
                ok = False                        # 重复或回退 → 这一组不是序列
                break
            seen.add(n)
            last = n
        if ok and len(seq) > 1:
            keep.extend(m for _n, m in seq)
        else:
            drop += len(seq)
    keep.sort(key=lambda m: m.start())
    return keep, drop


def check_flow(text):
    """第 9 条判据：流程小节是否真写成了「逐步」。

    返回 (问题列表, 统计)。统计用于报表，即使没红也要报，
    否则「0 步」和「8 步 above 门槛」在输出里长得一样 —— 那是假绿。
    """
    bad = []
    warn = []          # 软判据：只提示，不计 FAIL（见 FLOW_HEAD_RE 上方的注释）
    lines = text.split("\n")
    n = len(lines)
    i = 0
    heads, steps_total, qa_total, dropped_total = 0, 0, 0, 0
    while i < n:
        if not FLOW_HEAD_RE.match(lines[i]):
            i += 1
            continue
        heads += 1
        title = lines[i].strip()
        # 标题没明说「逐步」→ 这节本来就可以是要点集合，走软判据：只提示，
        # **并且不再走下面的硬判据**（少了这层短路，「初期流程要点」会被判红，
        # 那是判据过严不是数据坏）。
        if not re.search(r"(逐步|全流程|序列)", lines[i]):
            warn.append("提示：%s 标题没写「逐步」但内容里也没有渐进序号"
                        "（本意是逐步流程就改名加序号；本来是要点集合就忽略）"
                        % title[:24])
            i += 1
            while i < n and not lines[i].lstrip().startswith("##"):
                i += 1
            continue
        i += 1
        body = []
        while i < n and not lines[i].lstrip().startswith("##"):
            body.append(lines[i])
            i += 1
        # 数序号只用**非问答行**：`Q: 第3步要做什么` 里的「第3步」是提问不是步骤，
        # 不排除的话它会被当成一步，然后把「步要做什么」当成该步正文（凑数误报）。
        body_steps = [l for l in body
                      if not (l.strip().startswith("Q") or l.strip().startswith("A"))]
        blob = "\n".join(body_steps)
        # 先收全部「第N后缀」和 `Step N`，再按「同后缀严格递增」滤掉假序号
        raw = list(FLOW_STEP_RE.finditer(blob)) + list(FLOW_STEP_ALT_RE.finditer(blob))
        raw.sort(key=lambda m: m.start())
        ms, dropped = _flow_progressive(raw)
        dropped_total += dropped
        # 每条渐进序号到下一个序号之间的文本长度 = 这一步写了多少
        steps_total += len(ms)
        for k, m in enumerate(ms):
            end = ms[k + 1].start() if k + 1 < len(ms) else len(blob)
            chunk = re.sub(r"\s+", "", blob[m.end():end])
            if len(chunk) < FLOW_MIN_STEP_CHARS:
                bad.append("[%s] 第 %d 个序号后只有 %d 字（< %d），是凑数不是步骤：%r"
                           % (title[:16], k + 1, len(chunk), FLOW_MIN_STEP_CHARS,
                              chunk[:24]))
        if len(ms) < FLOW_MIN_STEPS:
            bad.append("[%s] 只有 %d 个渐进序号（要求 >= %d）—— 这节是叙述段不是逐步流程"
                       % (title[:16], len(ms), FLOW_MIN_STEPS))
        n_q = sum(1 for l in body if l.strip().startswith("Q") and (":" in l or "：" in l))
        qa_total += n_q
        if n_q < FLOW_MIN_STEP_QA:
            bad.append("[%s] 步级问答只有 %d 组（要求 >= %d）"
                       % (title[:16], n_q, FLOW_MIN_STEP_QA))
    return bad, (heads, steps_total, qa_total, dropped_total), warn


def check(text, min_bytes=MIN_BYTES, min_para=MIN_PARA):
    """返回问题列表（空 = 通过）。text 是单个文件的全文。

    bad 里不含「提示：」开头的条目 —— 那些由 check_warns() 单独取，
    提示不计 FAIL。混在一起的话，提示会把好文件也判红（判据过严）。
    """
    bad, warn_out = [], []

    if not text.strip():
        # 必须返回元组：check() 只有这一个出口是「没有 header 时提前 return」，
        # 早先它返回裸字符串，而自测 mk() 会 `" ".join(got)` ——
        # 对 str 做 join 等于把每个字之间插空格（"文 件 是 空 的"），
        # 于是「空的」这条判据自己失效了。判据把判据自己骗了。
        return ["文件是空的"], []
    if len(text.encode("utf-8")) < min_bytes:
        bad.append("体积 %d 字节 < 门槛 %d（疑似半成品）"
                   % (len(text.encode("utf-8")), min_bytes))
    if "\ufffd" in text:
        bad.append("含 UTF-8 替换字符 \ufffd（编码坏了）")
    hits = HALLU_RE.findall(text)
    if hits:
        bad.append("命中 AI 幻觉措辞 %s（说明内容是编的不是查的）"
                   % sorted(set(h for h in hits if h)))

    nodes = parse(text)

    headers = [p for k, p in nodes if k == "section"]
    if not headers:
        # 必须一起返回 warn 元组，否则 check() 时好时坏（只有这条路径返回旧格式，
        # 自测里 mk() 拿到的是单个字符串而不是列表，判据就变成测了个寂寞）
        return bad + ["一个 `## ` 小节都没有"], []

    seen_h, dup_h = set(), []
    for h in headers:
        if h in seen_h:
            dup_h.append(h)
        seen_h.add(h)
    if dup_h:
        bad.append("小节标题重复 %s" % dup_h[:3])

    sec_qa, sec_idx = 0, 0
    all_q = []
    for kind, payload in nodes:
        if kind == "section":
            sec_idx += 1
            sec_qa = 0
            continue
        if kind == "qa":
            for sub in split_qa_block(payload):
                q, a = split_qa(sub)
                if not q:
                    bad.append("第 %d 节 Q: 提问空（块: %r）" % (sec_idx, sub[:24]))
                if not a:
                    bad.append("第 %d 节 Q: %r 没有 A: 答案" % (sec_idx, q[:24] if q else sub[:24]))
                else:
                    if len(a) < MIN_ANS:
                        bad.append("第 %d 节 A: 太短（%d 字）%r"
                                   % (sec_idx, len(a), a[:24]))
                    if len(a) > MAX_ANS:
                        bad.append("第 %d 节 A: 超长（%d 字，超过 %d）%r"
                                   % (sec_idx, len(a), MAX_ANS, a[:24]))
                sec_qa += 1
                if q:
                    all_q.append(q)
        else:  # prose
            flat = re.sub(r"\s+", "", payload)
            if len(flat) < min_para:
                bad.append("第 %d 节正文段太短（%d 字 < %d）: %r"
                           % (sec_idx, len(flat), min_para, payload[:30]))

    if sec_idx and sec_qa == 0 and "小节标题重复" not in "".join(bad):
        bad.append("最后一节（第 %d 个）一个 Q: 都没有" % sec_idx)

    dupq = {q for q in all_q if all_q.count(q) > 1}
    if dupq:
        bad.append("同文件 Q: 重复 %d 条，例：%s"
                   % (len(dupq), sorted(dupq)[:2]))

    flow_bad, (fh, fs, fq, fd), flow_warn = check_flow(text)
    bad.extend(flow_bad)
    # flow_warn 不入 bad：main() 会把它从「问题」里摘出来单独当提示统计。
    warn_out.extend(flow_warn)

    return bad, warn_out


def check_warns(text):
    """只取「提示：」条目（软判据）。"""
    return [b for b in check(text)[1] if b.startswith("提示：")]


# ---------------------------------------------------------------- 自测夹具
LONG_PROSE = (
    "这一段是刻意写满长度的正文段，用来越过最短段落这道门槛，"
    "里面讲的游戏机制是这一作的核心，玩家上手第一件事就是摸清楚它，"
    "后面所有关卡设计都围绕这条规则展开，理解不了会很吃力。")
SHORT_PROSE = "这段太短了凑不够字数。"


def _good_flow(n_step=8, n_qa=6, step_chars=60):
    """一份「合格」的流程小节：n_step 步、每步 step_chars 字、n_qa 组步级问答。

    step_chars 做成参数是为了自测能造「步数够但每句是凑数」的样本：
    固定长度写死的话，两种坏情况在判据里长一样，就分不出来了。
    """
    steps = ["第%d步：" % i + "这一步要把前面的关卡推进过去，把该拿的道具捡齐再往下走，"
             "顺序反了后面会缺材料要回头，是最容易漏的地方。"[:max(step_chars, 4)]
             for i in range(1, n_step + 1)]
    qas = ["Q: 第%d步要做什么" % i + "\n" +
           "A: 第%d步" % i + "先把前面的关卡推进过去，把该拿的道具捡齐再往下走，顺序反了后面会缺材料要回头。"[:step_chars]
           for i in range(1, n_qa + 1)]
    return ("## 主线全流程（逐步）\n\n" + "\n\n".join(steps) + "\n\n"
            + "\n".join(qas) + "\n")


def _good(n=1, prose=LONG_PROSE):
    """一份合格样本（非默认值：段落写满 / Q 带序号 / A 成对）。"""
    secs = []
    for i in range(n):
        secs.append(
            "## 第%d节 测试小节\n\n%s\n\n"
            "Q: 第%d节讲的是什么 A: 是测试小节%d的正文内容，用于给校验器当合格样本用。"
            % (i, prose, i, i))
    # 结尾带一行「挤三对」的 Q/A —— 让合格样本也走一遍同verse行拆分路径，
    # 证明解析器在正常数据上不会误判，坏数据才红。
    secs.append(
        "## 第9节 挤行样本\n\n" + LONG_PROSE + "\n\n"
        "Q: 挤行一 A: 挤行一的答案内容写得长一些用来绕过最短答案这道门槛。\n"
        "Q: 挤行二 A: 挤行二的答案内容也写长一点，同样用来绕过最短答案这道门槛。\n"
        "Q: 挤行三 A: 挤行三的答案内容一样写长一点用来绕过最短答案这道门槛。\n")
    return "\n\n".join(secs) + "\n"


def selftest():
    """每条判据都必须能在对应的坏样本上变红 —— 证明判据不是摆设。"""
    cases = []

    def mk(name, content, expect_kw, **kw):
        # 自测夹具刻意很小，体积这道门槛在自测里关掉，
        # 否则会误杀（判据过严）而不是真在验体积。
        got = check(content, **{**{"min_bytes": 0}, **kw})[0]
        cases.append((name, expect_kw, got))

    mk("空文件", "", ["空的"])
    # 这条必须开起体积门槛来验，否则就是在测一个被关掉的判据
    mk("体积过小", "## 只有标题\n\n" + SHORT_PROSE + "\n\nQ: 问 A: 答\n",
       ["体积"], min_bytes=MIN_BYTES)
    # 段落被砍到 80 字以下（非默认值：整段替换成短句）
    mk("正文段太短", _good(prose=SHORT_PROSE), ["正文段太短"])
    # 删掉 A: 的答案
    mk("Q 没有 A", _good().replace(
        " A: 是测试小节0的正文内容，用于给校验器当合格样本用。", ""),
        ["没有 A"])
    # 复制同一条 Q 两遍
    mk("Q 重复", _good() + "\n\nQ: 第0节讲的是什么 A: 另一段完全不同的答案内容。\n",
       ["重复"])
    # 塞一句幻觉措辞
    mk("幻觉措辞", _good().replace("是测试小节", "根据我的知识库，是测试小节"),
       ["幻觉"])
    # 一个 ## 都没有
    mk("没有小节", SHORT_PROSE + "\n\nQ: 问 A: 答\n", ["小节"])
    # UTF-8 替换字符
    mk("替换字符", _good().replace("测试小节", "\ufffd"), ["替换字符"])
    # 小节标题重复
    mk("标题重复", _good(2).replace("## 第0节 测试小节", "## 第1节 测试小节"),
       ["小节标题重复"])
    # 一行挤三对 Q:/A: —— 解析不切分的话，后两对会静默消失（假绿）
    mk("QA 挤行缺 A",
       _good() + "\n\nQ: 第一个问题 A: 第一个答案说得详细一点凑够字数。\n"
                 "Q: 第二个问题 A: 第二个答案Q: 第三个问题\n",
       ["没有 A"])
    # 第 9 条：挂个流程标题但只有 2 步 —— 标题党
    mk("流程标题太浅", _good_flow(n_step=2),
       ["渐进序号"])
    # 第 9 条：步数够但每步只有一句话（凑数）
    mk("流程步骤凑数", _good_flow(n_step=8, step_chars=12),
       ["凑数"])
    # 第 9 条：步数够但步级问答太少 —— 问不出「这一步该干什么」
    mk("流程步级问答太少", _good_flow(n_step=8, n_qa=1),
       ["步级问答"])
    # v4 反向：八个「第1步」重复出现，不是渐进序列 —— 必须判红。
    # 少了这条，_flow_progressive 就算恒等过滤（永远放行）也照样全绿。
    dup_num = "\n\n".join(
        "第1步 " + "这一步要把前面的关卡推进过去，把该拿的道具捡齐再往下走，顺序反了会缺材料。"
        for i in [1, 1, 1, 1, 1, 1, 1, 1])
    mk("序号不递增",
       "## 主线全流程（逐步）\n\n" + dup_num + "\n\n"
       + "\n".join("Q: 第%d步该做什么" % i for i in range(1, 7)) + "\n",
       ["渐进序号"])

    # v4 回归（判据不能误杀好数据）：步骤正文里自带「第N关」曾被 v2/v3 的正则
    # 当成下一个序号，把上一步截成 0 字（dodonpachi 第 3 步真实误报，那步有六十多字）。
    # 现在靠「同后缀严格递增」把「关」组（1,1）滤掉、「步」组（1..8）保留。
    inline = "\n\n".join(
        "第%d步 第1关先清杂兵段，边贴着弹幕缝隙走边持续输出别停手，停久了连锁会衰减归零，"
        "这一段攒连锁最划算，形态切普通弹覆盖最广。" % i for i in range(1, 9))
    flow_inline = ("## 主线全流程（逐步）\n\n" + inline + "\n\n"
                   + "\n".join("Q: 第%d步该干什么\nA: 第%d步先把杂兵段清干净再往下推。"
                               % (i, i) for i in range(1, 7)) + "\n")
    # 关卡游戏写「第1关→…→第10关」照样是逐步推进，不能因为后缀是「关」就判红
    flow_levels = ("## 主线全流程（逐步）\n\n" + "\n\n".join(
        "第%d关 %s" % (i, "进关先摸清地形和敌人布置，把该拿的道具捡齐再推，别硬冲踩陷阱。"
                    + "过关条件看清是清怪还是找钥匙。"[:70]) for i in range(1, 10))
        + "\n\n" + "\n".join("Q: 第%d关要做什么\nA: 第%d关把该拿的捡齐再推，别硬冲。"
                             % (i, i) for i in range(1, 7)) + "\n")
    ok = True
    # 合格样本必须是绿的（判据不能误杀好数据）
    for label, sample in [("普通样本", _good(2)), ("流程样本", _good_flow()),
                          ("步内自带第N关", flow_inline),
                          ("关卡序列第N关", flow_levels)]:
        g = check(sample, min_bytes=0)[0]
        if g:
            print("SELFTEST 合格样本被误杀（判据过严）: %s" % label)
            for x in g:
                print("   -", x)
            ok = False
        else:
            print("SELFTEST  ok  %s通过" % label)

    for name, kw, got in cases:
        joined = " ".join(got)
        if any(k in joined for k in kw):
            print("SELFTEST  ok  %-12s 按预期判红: %s" % (name, kw))
        else:
            print("SELFTEST  FAIL %-12s 没抓到 %r -> %s" % (name, kw, got))
            ok = False

    print("SELFTEST %s" % ("ALL PASS" if ok else "FAILED"))
    return 0 if ok else 1


# ---------------------------------------------------------------- 主流程
def main():
    if "--selftest" in sys.argv:
        return selftest()

    here = os.path.dirname(os.path.abspath(__file__))
    corpus = os.path.join(here, "corpus")

    min_bytes = MIN_BYTES
    if "--min" in sys.argv:
        min_bytes = int(sys.argv[sys.argv.index("--min") + 1])

    files = sorted(f for f in os.listdir(corpus)
                   if f.startswith("guide_") and f.endswith(".txt"))

    # 只想验自己写的那一个文件时用 --file xxx。
    # 全量扫 66+ 个文件很慢，而并行干活的几路 agent 各自只关心自己的产物；
    # 没有这个开关，它们就只能要么不管、要么把全库跑一遍（还会和别人的
    # 并发写撞上「文件写到一半在读」窗口）。
    if "--file" in sys.argv:
        want = sys.argv[sys.argv.index("--file") + 1]
        files = [f for f in files if f == want]
        if not files:
            print("--file %s 不存在（当前 %d 个文件）" % (want, len(files)))
            return 1

    if not files:
        print("corpus 目录里没有 guide_*.txt")
        return 1

    total_bad, total_sec, total_qa, total_warn = 0, 0, 0, 0
    for f in files:
        p = os.path.join(corpus, f)
        with open(p, "rb") as fh:
            raw = fh.read()
        try:
            text = raw.decode("utf-8")
        except UnicodeDecodeError as e:
            print("FAIL %-34s UTF-8 解码失败: %s" % (f, e))
            total_bad += 1
            continue

        nodes = parse(text)
        n_sec = sum(1 for k, _ in nodes if k == "section")
        # 数的是「问答对」不是「块」：一行挤四对时块数还是 1，
        # 报 1 会让人以为这两份语料没有问答 —— 那是数错了不是真没有。
        n_qa = sum(len(split_qa_block(p)) for k, p in nodes if k == "qa")
        total_sec += n_sec
        total_qa += n_qa

        bad, warns = check(text, min_bytes=min_bytes)
        hard = [b for b in bad if not b.startswith("提示：")]
        total_warn += len(warns)
        if hard:
            total_bad += 1
            print("FAIL %-34s %d 字节 %d 节 %d 问答" % (f, len(raw), n_sec, n_qa))
            for b in hard:
                print("      - %s" % b)
            for w in warns:
                print("      ~ %s" % w)
        elif warns:
            print("ok   %-34s %6d 字节 %3d 节 %3d 问答（%d 条提示）"
                  % (f, len(raw), n_sec, n_qa, len(warns)))
            for w in warns:
                print("      ~ %s" % w)
        else:
            print("ok   %-34s %6d 字节 %3d 节 %3d 问答"
                  % (f, len(raw), n_sec, n_qa))

    print("-" * 62)
    print("文件 %d / 有问题 %d / 提示 %d / 小节合计 %d / 问答合计 %d"
          % (len(files), total_bad, total_warn, total_sec, total_qa))
    print("（「提示」不计 FAIL：标题没写「逐步」的小节本来可以是要点集合，"
          "不算坏数据；只有标题写了 逐步/全流程 却没写满序号才算 FAIL）")
    if total_bad:
        print("不达标清单（先看这个，别被一大堆明细淹了）:")
        for f in files:
            with open(os.path.join(corpus, f), encoding="utf-8") as fh:
                b = check(fh.read(), min_bytes=min_bytes)[0]
            if b:
                kinds = {}
                for x in b:
                    k = re.sub(r"\(.*$", "", x).strip()
                    kinds[k] = kinds.get(k, 0) + 1
                print("  %-30s %s" % (f, kinds))
    print("%s" % ("ALL PASS" if total_bad == 0 else "有 %d 个文件不达标" % total_bad))
    return 0 if total_bad == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
