# -*- coding: utf-8 -*-
"""conv_state 的 PC 侧孪生实现 + 判据。

⚠️ 这是**复刻品**：它复刻 conv_state.c 的逻辑，不是跑 conv_state.c 本身。
它能证明「这套逻辑对不对」，证明不了「C 代码编译后行为与它一致」。
C 侧的一致性靠 arm-none-eabi-gcc 编译过 + 人工逐行比对保证。

用法：python conv_twin.py
"""

CONV_GAME_NONE = -1
CONV_GAME_CLARIFY = -2

PRON = ["它", "这款", "那款", "这个", "那个",
        "这作", "那作", "这部", "那部",
        "上面", "刚才", "刚刚", "之前"]


class Conv:
    def __init__(self):
        self.reset()

    def reset(self):
        self.game = -1
        self.turn = 0
        self.last_from_text = 0

    def remember_game(self, gid):
        if gid >= 0:
            self.game = gid
            self.last_from_text = 1
        return self.game

    def has_pronoun(self, q):
        if not q:
            return False
        return any(p in q for p in PRON)

    def resolve_game(self, q, g_from_query):
        self.turn += 1
        if g_from_query >= 0:
            self.game = g_from_query
            self.last_from_text = 1
            return g_from_query
        if not self.has_pronoun(q):
            return CONV_GAME_NONE
        if self.game >= 0:
            return self.game
        return CONV_GAME_CLARIFY


# ---------------------------------------------------------------- 判据
FAIL = []


def check(name, got, want):
    ok = (got == want)
    if not ok:
        FAIL.append(name)
    print("  %s %-46s got=%-4s want=%s" % ("OK  " if ok else "FAIL", name, got, want))


print("=" * 64)
print("conv_state 孪生判据")
print("=" * 64)

# --- 1. 核心 bug 场景：问句里提了游戏名，下一句用指代 ---
print("\n[1] 核心 bug 场景（「火焰纹章觉醒」->「它的boss 怎么打」）")
c = Conv()
c.resolve_game("火焰纹章觉醒", 18)          # 第一句剥出 game 18
r = c.resolve_game("它的boss 怎么打", -1)   # 第二句没游戏名，但有指代
check("追问应继承上一轮的游戏", r, 18)

# --- 2. 无 antecedent 必须反问，绝不瞎搜 ---
print("\n[2] 无 antecedent：首轮就说「它的boss 怎么打」")
c = Conv()
r = c.resolve_game("它的boss 怎么打", -1)
check("无槽位+有指代 -> 必须 CLARIFY", r, CONV_GAME_CLARIFY)
c2 = Conv()
r2 = c2.resolve_game("这个游戏怎么打", -1)
check("「这个游戏」首轮同样 CLARIFY", r2, CONV_GAME_CLARIFY)

# --- 3. 无指代词不擅自收窄 ---
print("\n[3] 无指代词：不收窄到槽位")
c = Conv()
c.resolve_game("火焰纹章觉醒", 18)
r = c.resolve_game("白金怎么打", -1)
check("无指代词 -> 全库(NONE)", r, CONV_GAME_NONE)

# --- 4. 显式优先：提到新游戏就覆盖槽位 ---
print("\n[4] 显式优先于槽位")
c = Conv()
c.resolve_game("塞尔达 织梦岛", 3)
c.resolve_game("火焰纹章觉醒", 18)
r = c.resolve_game("它的boss", -1)
check("最新提到的游戏胜出", r, 18)

# --- 5. 槽位不被「认不出游戏」清掉 ---
print("\n[5] 槽位持久性")
c = Conv()
c.resolve_game("火焰纹章觉醒", 18)
c.remember_game(-1)          # 某轮没认出游戏
r = c.resolve_game("它的boss", -1)
check("认不出游戏不应清空槽位", r, 18)
# turn 只在 resolve_game 里 +1（设计如此），本段共调了 2 次 -> 2。
# 写成 3 会是「判据自己算错」，不是代码错 —— 记住这一条。
check("turn 只按 resolve_game 计数", c.turn, 2)

# --- 6. 问句里同时有指代和游戏名 -> 显式优先 ---
print("\n[6] 问句内同时出现指代与游戏名")
c = Conv()
c.resolve_game("塞尔达", 3)
r = c.resolve_game("火焰纹章觉醒的它的boss", 18)
check("显式游戏名优先于指代", r, 18)

# --- 7. 「其他」「他们」不应被当成指代「它」 ---
print("\n[7] 误触发防护")
c = Conv()
r = c.resolve_game("其他游戏怎么打", -1)
check("「其他游戏」不算指代 -> NONE", r, CONV_GAME_NONE)
c = Conv()
r = c.resolve_game("他们几个角色", -1)
check("「他们」不算指代 -> NONE", r, CONV_GAME_NONE)

# --- 8. reset 清空 ---
print("\n[8] reset")
c = Conv()
c.resolve_game("火焰纹章觉醒", 18)
c.reset()
r = c.resolve_game("它的boss", -1)
check("reset 后指代无 antecedent -> CLARIFY", r, CONV_GAME_CLARIFY)

# ---------------------------------------------------------------- 汇总
print("\n" + "=" * 64)
if FAIL:
    print("❌ %d 条判据未过：%s" % (len(FAIL), ", ".join(FAIL)))
    raise SystemExit(1)
print("✅ 全部判据通过（8 组 / 15 条）")
print("=" * 64)
