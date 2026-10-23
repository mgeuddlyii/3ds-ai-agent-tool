#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""
corpus_expand.py — 攻略语料扩充器（纯离线，零依赖）

为什么要这个脚本
----------------
3M 参数的小模型最缺的不是知识，而是**问句到答案的映射格式**。
语料如果只有陈述式段落，模型会学着复述；它需要见过大量
「玩家这么问 → 攻略这么答」的对子，才会在推理时组织出问答语气。

所以这里不新写事实，而是从已有小节里**派生变体问答对**：
    · 同一段事实，换 6 种问法（怎么/在哪里/为什么/推荐/注意/能不能）
    · 同一问法，换不同的事实载体
这样语料的「句式多样性 × 事实覆盖率」矩阵就密起来了。

只用标准库，不依赖 numpy。
"""

import os
import re
import sys

# ---------------------------------------------------------------- 问句模板
# 每条：{q} 会被填入从正文里抽出的「话题短语」
ASK_TEMPLATES = [
    "{t}怎么弄",
    "{t}在哪里",
    "{t}怎么打",
    "{t}要点是什么",
    "{t}有什么技巧",
    "请问{t}",
    "关于{t}我该怎么做",
    "{t}难度大吗",
    "{t}要注意什么",
    "推荐一下{t}",
    "{t}的流程是什么",
    "能跳过{t}吗",
    "{t}值不值得练",
    "{t}有什么限制",
    "{t}的奖励是什么",
    "{t}的最佳时机",
    "{t}的代价",
    "{t}怎么搭配",
    "{t}会失败吗",
    "关于{t}还有什么要注意的",
]

# 答案开头模板：把同样的事实包装成不同语体的回答
ANS_OPENERS = [
    "{body}",
    "简单说，{body}",
    "基本上{body}",
    "按照攻略的说法，{body}",
    "要点是这样的：{body}",
    "通常情况下{body}",
    "推荐的做法是{body}",
    "{body}这一段是很多人卡住的地方。",
]


# ---------------------------------------------------------------- 工具函数

def split_sections(text):
    """把语料切成 [(标题, 正文)]，只处理 `## ` 标题。"""
    out = []
    cur_title, buf = None, []
    for line in text.split("\n"):
        if line.startswith("## "):
            if cur_title is not None:
                out.append((cur_title, "\n".join(buf).strip()))
            cur_title, buf = line[3:].strip(), []
        elif cur_title is not None:
            buf.append(line)
    if cur_title is not None:
        out.append((cur_title, "\n".join(buf).strip()))
    return [(t, b) for t, b in out if b]


def sentences(body):
    """把正文切成句子块（中文标点切分，保留信息完整度）。"""
    parts = re.split(r"(?<=[。！？；])", body)
    return [p.strip() for p in parts if len(p.strip()) >= 8]


def topic_of(title, body):
    """
    从小节里抽一个「话题短语」，用于填问句模板。
    优先用标题的核心名词部分，去掉「要点/流程/技巧」这类后缀。
    """
    t = re.sub(r"(要点|流程|技巧|是什么|怎么.*|.*指南|介绍|概述|.*建议|.*说明|.*要点)$", "", title)
    t = t.strip("：:· ")
    if len(t) < 2:
        # 标题太抽象就从正文首句里取主语
        s = sentences(body)
        if s:
            m = re.search(r"[《「]?([^，。、《」 ]{2,10})[》」]?", s[0])
            t = m.group(1) if m else title
    return t


def already_qa(text):
    """语料里已存在的 Q: 行，用来避免重复生成。"""
    return set(m.group(1).strip() for m in re.finditer(r"Q:\s*(.+?)\s*A:", text))


# ---------------------------------------------------------------- 主流程

def expand_one(path, per_section=14, seed=7):
    """
    读一个语料文件，给每个小节追加派生问答，追加到文件末尾的
    `## 常见问题（衍生）` 区里。已存在的问答不重复生成。
    """
    text = open(path, encoding="utf-8").read()
    secs = split_sections(text)
    if not secs:
        return 0, 0

    have = already_qa(text)
    # 简单可复现的伪随机，保证每次跑出来一样（便于复现训练数据）
    state = seed

    def rnd(n):
        nonlocal state
        state = (state * 1103515245 + 12345) & 0x7FFFFFFF
        return state % n

    lines = []
    made = 0
    for title, body in secs:
        sents = sentences(body)
        if not sents:
            continue
        topic = topic_of(title, body)
        for i in range(per_section):
            ask = ASK_TEMPLATES[rnd(len(ASK_TEMPLATES))].format(t=topic)
            if ask in have:
                continue
            have.add(ask)
            # 答案取该小节的 1~3 句原文，保持事实正确性
            k = 1 + rnd(min(3, len(sents)))
            picked = "".join(sents[(i + j) % len(sents)] for j in range(k))
            ans = ANS_OPENERS[rnd(len(ANS_OPENERS))].format(body=picked)
            # ★ 必须写成「Q: / A: 各占一行」。
            #   这里曾经是 `Q: %s A: %s` 挤一行，而 train_gpt.scan_qa 只认分行写法 ——
            #   跑一次就往语料里灌一批模型永远学不到的问答，还不报错。
            #   scan_qa 那条 n_qa < n_Q行*0.8 的守卫就是用来抓这个的。
            lines.append("Q: %s" % ask)
            lines.append("A: %s" % ans)
            made += 1

    if not lines:
        return 0, 0

    with open(path, "a", encoding="utf-8") as f:
        if "## 常见问题（衍生）" not in text:
            f.write("\n\n## 常见问题（衍生）\n\n")
        f.write("\n".join(lines) + "\n")
    return made, len(secs)


def main():
    base = os.path.dirname(os.path.abspath(__file__))
    corpus = os.path.join(base, "corpus")
    files = sorted(f for f in os.listdir(corpus) if f.startswith("guide_") and f.endswith(".txt"))
    if not files:
        print("corpus/ 下没有 guide_*.txt", file=sys.stderr)
        return 1

    total = 0
    for fn in files:
        p = os.path.join(corpus, fn)
        before = os.path.getsize(p)
        made, nsec = expand_one(p)
        after = os.path.getsize(p)
        total += made
        print("%-28s 小节 %2d  派生问答 %3d  +%dB" % (fn, nsec, made, after - before))
    print("---")
    print("合计派生问答 %d 条" % total)
    return 0


if __name__ == "__main__":
    sys.exit(main())
