#!/usr/bin/env python3
"""check_ui_text.py -- UI 文案判据：软件不许用聊天口吻

为什么要有这个：文案风格这种事没人会主动 review，改十条能记住，
改一百条就一定会漏。判据的作用是**让回退变红**，而不是提醒谁注意。

它检查的是「模式」不是「句子」：新写的文案只要不落进这些模式就放过，
所以它不会阻碍正常写作；但它拦住的那几类（反问、拟人、开发术语、
含糊指代）恰好是软件界面里最刺眼的一批。

判据清单（每条都给了为什么）：

1. 里程碑号漏给用户：`M1 回归屏`、`M11 立体渲染才会真正用上`
   —— 里程碑是排期信息，用户不需要知道"这个功能什么时候做"。
2. 错误码直出：`rc=-28`、`tb=-30`
   —— 用户拿到 `rc=-28` 无从下手，要翻代码才知道是超时。
3. 反问句：`保存失败：卡写不进去？`
   —— 界面在陈述状态，不是在跟用户对话。用问号会把报错变成疑问。
4. 含糊指代：`（原因看下面）`
   —— "下面"取决于当前布局，指代不到就等于没说。
5. 拟人化：`没听清`、`没采到声音`
   —— 机器不��"听"，是采集到/没采集到。
6. 辩解式：`本来就是关的，没有改动`
   —— 软件不需要为自己的行为辩解，陈述结果即可。
7. 祈使训人：`先按 Y 用键盘输入点什么`
   —— "先…吧"的语气像有人在催；软件应���只报状态，操作提示用中性句式。

例外（**必须放过**，否则会误报）：
- `app_note(...)` 里的调试日志不进用户界面，里面写多少开发术语都行。
- AGENT.md 与使用手册是给人和模型读的文档，不在本判据范围内。
- 代码注释不扫。

用法：
    python tools/check_ui_text.py            # 有问题返回 1
    python tools/check_ui_text.py --list     # 逐条列出，便于批量改
"""

import argparse
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)

# 只扫会进用户界面的源码。
SCAN_DIRS = [os.path.join(ROOT, "app", "source"), os.path.join(ROOT, "shared")]

# 文案出现在这四种调用里才算用户可见。
VISIBLE = re.compile(
    r'(?:app_status_set|push_msg|w_top_footer|w_bot_footer|w_wrap)\s*\('
)

# 逐条规则：(规则名, 说明, 正则)。正则针对**单个字符串字面量**。
RULES = [
    ("里程碑号",
     "排期信息不该出现在用户界面",
     re.compile(r'M\d+\s*[\u4e00-\u9fff]')),

    ("内部术语",
     "回归/里程碑这类排期词对用户没有意义",
     re.compile(r'(回归屏|里程碑|后续版本|开发中|未实测)')),

    ("错误码直出",
     "用户拿到 rc=-28 无从下手，应转成『网络超时』这类人话",
     # 形如 rc=%d（打印格式）或 tb=-30（真值），两种都算
     re.compile(r'(?:rc|tb|rt)\s*=\s*(?:%|-?\d)')),

    ("反问句",
     "界面在陈述状态，不是在发问",
     # 由 check_one 预处理好（挖掉「」内引用的原话后）再匹配
     re.compile(r'[\uff1f?]')),

    ("含糊指代",
     "『下面』取决于布局，指代不到等于没说",
     re.compile(r'(看下面|见下面|看对话里那条)')),

    ("拟人化",
     "机器不『听』也不『看』，是采集",
     re.compile(r'(没听清|没采到声音|听不清)')),

    ("辩解式",
     "软件不需要为自己的行为辩解",
     re.compile(r'(本来就是|没有改动)')),

    ("祈使训人",
     "『先…吧』像有人在催；软件只报状态，操作提示用中性句式",
     re.compile(r'(点什么|吧。|吧？|先按.*吧)')),
]


def strip_quoted(text):
    """挖掉「」与“”里的内容。

    必要性：对话流里会原样转述用户自己打的话（例如把"你叫什么名字？"
    当作引用回显），那是**用户的话不是软件的语气**，里面有问号是正常的。
    不挖掉就会把「软件在陈述事实」和「引述用户原话」混为一谈。
    """
    out = re.sub(r'「[^」]*」', "", text)
    out = re.sub(r'“[^”]*”', "", out)
    return out



def scan_texts(path):
    """抽出该文件里所有会进用户界面的字符串字面量。"""
    src = open(path, encoding="utf-8", errors="replace").read()
    # 逐个调用括号内，取其中的字符串字面量（覆盖拼接的第一段即可）
    out = []
    for m in VISIBLE.finditer(src):
        # 从调用处往后取到配对的右括号（最多 600 字符，够覆盖多行拼接）
        depth = 0
        buf = []
        for i in range(m.end() - 1, min(len(src), m.end() + 600)):
            ch = src[i]
            if ch == '(':
                depth += 1
            elif ch == ')':
                depth -= 1
                if depth == 0:
                    break
            buf.append(ch)
        call = "".join(buf)
        # 里面的字符串字面量（支持 \" 转义）
        for sm in re.finditer(r'"((?:[^"\\]|\\.)*)"', call):
            out.append((sm.group(1), call.replace("\n", " ")[:70]))
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--list", action="store_true", help="逐条列出")
    args = ap.parse_args()

    files = []
    for d in SCAN_DIRS:
        for root, _dirs, names in os.walk(d):
            for n in names:
                if n.endswith(".c") or n.endswith(".h"):
                    files.append(os.path.join(root, n))

    bad = []
    scanned = 0
    for f in sorted(files):
        rel = os.path.relpath(f, ROOT)
        for text, call in scan_texts(f):
            scanned += 1
            probe = strip_quoted(text)
            for rule, why, pat in RULES:
                if pat.search(probe):
                    bad.append((rel, rule, why, text, call))
                    break   # 一条文案只报一次，避免刷屏

    print("扫描 %d 个文件、%d 条界面文案" % (len(files), scanned))
    if not bad:
        print("全部通过：没有聊天口吻 / 开发术语 / 含糊指代")
        return 0

    print("发现 %d 条待改：" % len(bad))
    if args.list:
        for rel, rule, why, text, call in bad:
            print("\n  [%s] %s" % (rule, why))
            print("    %s" % rel)
            print("    文案: %s" % text[:90])
            print("    上下文: %s" % call)
    else:
        by_rule = {}
        for rel, rule, _why, _t, _c in bad:
            by_rule.setdefault(rule, []).append(rel)
        for rule, rels in sorted(by_rule.items()):
            print("  %-10s %2d 条   %s" % (rule, len(rels),
                                          ", ".join(sorted(set(os.path.basename(x) for x in rels)))))
    return 1


if __name__ == "__main__":
    sys.exit(main())
