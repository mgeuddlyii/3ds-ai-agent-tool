# -*- coding: utf-8 -*-
"""chitchat + orch（优先级链）的 PC 侧孪生实现 + 判据。

⚠️ 复刻品：复刻 chitchat.c / orch.c 的逻辑，不是跑 C 本身。
算式层的判据在 tools_twin.py（那边已有 33 条），这里不重复。

用法：python orch_twin.py
"""

MAX_CHAT_LEN = 16

CHAT = [
    ("你好", "GREET", "你好。我能帮你查 3DS 游戏攻略。"),
    ("您好", "GREET", "你好。我能帮你查 3DS 游戏攻略。"),
    ("在吗", "GREET", "在的。问游戏攻略就行。"),
    ("早上好", "GREET", "早上好。今天想查哪款游戏？"),
    ("晚上好", "GREET", "晚上好。今天想查哪款游戏？"),
    ("谢谢", "THANKS", "不客气。"),
    ("多谢", "THANKS", "不客气。"),
    ("感谢", "THANKS", "不客气。"),
    ("再见", "BYE", "再见。"),
    ("拜拜", "BYE", "拜拜。"),
    ("走了", "BYE", "再见。"),
    ("好的", "ACK", "好。"),
    ("知道了", "ACK", "好。"),
    ("明白", "ACK", "好。"),
    ("哈哈", "LAUGH", "哈哈。"),
    ("嘿嘿", "LAUGH", "嘿嘿。"),
    ("抱歉", "SORRY", "没事。"),
    ("不好意思", "SORRY", "没事。"),
    ("你能做什么", "CAP", "我能查 3DS 游戏攻略：流程、boss、道具、职业、结局都能问。"),
    ("你会什么", "CAP", "我能查 3DS 游戏攻略：流程、boss、道具、职业、结局都能问。"),
    ("能干什么", "CAP", "我能查 3DS 游戏攻略：流程、boss、道具、职业、结局都能问。"),
    ("支持哪些游戏", "CAP", "支持的游戏在下面的选择条里，点一款就能问。"),
]

PARTICLE = ["啊", "呀", "哇", "吧", "呢", "吗", "哦", "噢", "嗯", "欸", "哈",
            "了", "啦", "咯",
            "，", "。", "！", "？", "、", "~", "!", "?", ".",
            "请", "请问", "麻烦", "你", "我", " ", "\t"]


def strip_particles(s):
    changed = True
    while changed:
        changed = False
        for p in PARTICLE:
            if p and s.startswith(p):
                s = s[len(p):]
                changed = True
                break
    return s


def chitchat_try(q):
    if q is None or q == "":
        return ("NONE", None)
    if len(q.encode("utf-8")) >= 128:
        return ("NONE", None)
    if len(q) > MAX_CHAT_LEN * 3:
        return ("NONE", None)

    found = None
    earliest = 10 ** 9
    for key, kind, rep in CHAT:
        pos = q.find(key)
        if pos < 0:
            continue
        if found is None or pos < earliest:
            earliest = pos
            found = (key, kind, rep)
    if not found:
        return ("NONE", None)

    key, kind, rep = found
    rest = q[:earliest] + q[earliest + len(key):]
    if len(strip_particles(rest)) > 0:
        return ("NONE", None)          # 还有实质内容 -> 放行给攻略
    return (kind, rep)


def orch_route(q, tool_pred):
    """tool_pred: 一个函数，输入问句返回 True/False，代表「工具层能答」。
    这里用简化判据（真实判据见 tools_twin.py 的 33 条）。"""
    if not q:
        return ("GUIDE", None)
    if tool_pred(q):
        return ("TOOL", "<算式结果>")
    kind, rep = chitchat_try(q)
    if kind != "NONE":
        return ("CHITCHAT", rep)
    return ("GUIDE", None)


# ------------------------------------------------------------------ 判据
FAIL = []


def check(name, got, want):
    ok = (got == want)
    if not ok:
        FAIL.append(name)
    print("  %s %-40s -> %-8s 期望 %s"
          % ("OK  " if ok else "FAIL", name[:38], got, want))


def is_calc(q):
    """简化：含数字且含算术运算符就算工具层能答（够测顺序了）"""
    return any(c.isdigit() for c in q) and any(c in "+-*/^%" for c in q)


print("=" * 68)
print("chitchat + orch 孪生判据")
print("=" * 68)

print("\n[1] 该接住的闲聊")
for q, want in [
    ("你好", "GREET"), ("您好", "GREET"), ("在吗", "GREET"),
    ("你好啊", "GREET"), ("早上好", "GREET"),
    ("谢谢", "THANKS"), ("谢谢你", "THANKS"), ("多谢了", "THANKS"),
    ("再见", "BYE"), ("拜拜", "BYE"),
    ("好的", "ACK"), ("知道了", "ACK"),
    ("哈哈", "LAUGH"),
    ("抱歉", "SORRY"),
    ("你能做什么", "CAP"), ("支持哪些游戏", "CAP"),
]:
    check(q, chitchat_try(q)[0], want)

print("\n[2] ★ 必须放行的（抢答就是 bug）")
for q in [
    "谢谢你，火焰纹章怎么打",
    "你好，请问雷顿教授3怎么过",
    "我不会玩怎么办",
    "哈哈，这个boss怎么打",
    "火焰纹章觉醒",
    "它的boss怎么打",
    "塞尔达 织梦岛",
    "3ds和2ds哪个好",
    "2周目怎么打",
    "再见火焰纹章",
    "好的火焰纹章觉醒怎么升级",
]:
    check(q, chitchat_try(q)[0], "NONE")

print("\n[3] 优先级顺序（这才是 M1 存在的理由）")
check("1+1", orch_route("1+1", is_calc)[0], "TOOL")
check("你好", orch_route("你好", is_calc)[0], "CHITCHAT")
check("火焰纹章觉醒", orch_route("火焰纹章觉醒", is_calc)[0], "GUIDE")
# 混合：既有寒暄又有算式 -> 工具层先接（不能被寒暄吃掉）
check("你好1+1", orch_route("你好1+1", is_calc)[0], "TOOL")
# 混合：寒暄 + 实质内容 -> 必须落到攻略（闲聊不许抢）
check("你好，火焰纹章怎么打", orch_route("你好，火焰纹章怎么打", is_calc)[0], "GUIDE")
check("谢谢，火焰纹章怎么打", orch_route("谢谢，火焰纹章怎么打", is_calc)[0], "GUIDE")

print("\n" + "=" * 68)
if FAIL:
    print("❌ %d 条未过：%s" % (len(FAIL), ", ".join(FAIL)))
    raise SystemExit(1)
print("✅ 全部判据通过（3 组 / 32 条）")
print("=" * 68)
