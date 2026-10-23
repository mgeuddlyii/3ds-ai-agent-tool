/*
 * conv_state.c — 多轮上下文槽位（编排层 M0）
 *
 * 单线程使用，不需要加锁。状态就一个结构体，static 放 .bss。
 *
 * 判据纪律：本文件每条分支都必须能被单独测到。尤其最后两条
 * 「有指代词+有槽位 -> 用槽位」和「有指代词+无槽位 -> CLARIFY」——
 * 后者正是「静默答成别的游戏」这个 bug 的正解，判据写松了就等于没修。
 */
#include <string.h>
#include <stdio.h>

#include "conv_state.h"

static ConvState g_cs = { -1, 0, 0 };

/*
 * 指代词表。刻意短：只放「在这套交互里确实指代上一款游戏」的词。
 *
 * 「其他」「他们」里**不含** U+5B83「它」，所以按字节子串匹配「它」
 * 是安全的，不会被这两个常用词误触发。
 *
 * 「这个」「那个」在通用文本里很常见，但在这个界面里用户问的几乎
 * 全是攻略，「这个游戏」几乎总是指当前讨论的那款。这是有意的宽松，
 * 不是疏忽 —— 真要更严可以要求指代词出现在问句前若干字节内，
 * 但目前没有数据证明那样更准，所以不猜。
 */
static const char *g_pron[] = {
    "它", "这款", "那款", "这个", "那个",
    "这作", "那作", "这部", "那部",
    "上面", "刚才", "刚刚", "之前"
};
#define PRON_N ((int)(sizeof g_pron / sizeof g_pron[0]))

void conv_reset(void)
{
    g_cs.game = -1;
    g_cs.turn = 0;
    g_cs.last_from_text = 0;
}

const ConvState *conv_get(void)
{
    return &g_cs;
}

int conv_remember_game(int gid)
{
    if (gid >= 0) {
        g_cs.game = gid;
        g_cs.last_from_text = 1;
    }
    /* gid < 0 时**刻意不动** g_cs.game。
     * 把「这句没认出游戏」当成「忘掉上一轮」会让多轮彻底不可用：
     * 追问「它的道具呢」认不出游戏是正常的，不该因此丢槽位。 */
    return g_cs.game;
}

int conv_has_pronoun(const char *q)
{
    int i;
    if (!q) return 0;
    for (i = 0; i < PRON_N; i++) {
        if (strstr(q, g_pron[i]))
            return 1;
    }
    return 0;
}

int conv_resolve_game(const char *q, int g_from_query)
{
    g_cs.turn++;

    /* 问句里写明了游戏名（或用户手选了）：显式优先，槽位让位。
     * 这一条必须排在最前 —— 用户从「塞尔达」切到「火焰纹章」再问
     * 「它的boss」，要跟的是**这句**，不是上一句。 */
    if (g_from_query >= 0) {
        g_cs.game = g_from_query;
        g_cs.last_from_text = 1;
        return g_from_query;
    }

    /* 没有指代词 = 用户没在引用上一款，那就不要擅自收窄范围。
     * 用户手选的 g_game 由调用方提前并进 g_from_query，不走这里。 */
    if (!conv_has_pronoun(q))
        return CONV_GAME_NONE;

    /* 到这里：引用了某款游戏，但槽位里没有 -> 必须反问，不许瞎搜。 */
    if (g_cs.game >= 0)
        return g_cs.game;
    return CONV_GAME_CLARIFY;
}

void conv_clarify_prompt(char *out, int out_sz)
{
    if (!out || out_sz <= 0) return;
    /* 带上一个可照抄的例句：只问「你是指哪款」而不教用户怎么问，
     * 用户会重复原来的问法，然后我们再拒一次 —— 死循环。 */
    snprintf(out, (size_t)out_sz,
             "这句话没说是哪一款游戏。请在句子里带上游戏名，"
             "比如「火焰纹章觉醒 它的boss怎么打」。");
}
