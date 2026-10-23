/*
 * conv_state.h — 多轮上下文槽位（编排层 M0）
 *
 * 它修的是一个具体到能复现的毛病：
 *
 *   用户：「火焰纹章觉醒」
 *   用户：「它的boss 怎么打」
 *
 * 第二句里没有游戏名，gkb_split_game() 返回 -1，检索于是放开到全库，
 * 结果大概率答成**别的游戏**。屏上看不出异常、退出码正常、不报错 ——
 * 只是答案错了。而用户以为「它」指的就是刚才那款。
 *
 * 根因不是检索弱，是**问句里出现过的游戏名没有被记住**。
 * 注意这和 g_game（用户手动选的）是两回事：g_game 要用户手点，
 * 问句里顺带提的游戏名从来不留存。
 *
 * 所以这里只做一件事：把每轮剥出来的 game_id 存下来，供下一轮补全。
 * 刻意不做的事：不猜「它」到底指什么。槽位为空时宁可反问，也不乱搜。
 */
#ifndef CONV_STATE_H
#define CONV_STATE_H

#ifdef __cplusplus
extern "C" {
#endif

/* conv_resolve_game 的返回值约定 */
#define CONV_GAME_NONE     (-1)  /* 不限定：正常全库检索              */
#define CONV_GAME_CLARIFY  (-2)  /* 指代却没有 antecedent：必须反问   */

typedef struct {
    int  game;           /* 记住的游戏 id；-1 = 没记住任何游戏 */
    int  turn;           /* 已经过的轮次，用于将来做上下文压缩   */
    int  last_from_text; /* 上一轮的游戏名是不是从问句里剥出来的 */
} ConvState;

/* 清空槽位（进屏 / 切换游戏时调）。 */
void conv_reset(void);

/* 取当前槽位（只读；不要直接改字段，改动走下面的函数）。 */
const ConvState *conv_get(void);

/*
 * 记下本轮剥出来的游戏名。
 *   gid >= 0  -> 记住它，返回 gid
 *   gid <  0  -> 不动槽位（**不因为这句没认出游戏就把上轮记住的清掉**），
 *                返回当前槽位里的 game
 */
int conv_remember_game(int gid);

/*
 * 问句里有没有指代词（它 / 这款 / 那个 / 上面说的 …）。
 *
 * 注意「其他」「他们」里**不含** U+5B83「它」，
 * 所以按字节子串匹配「它」是安全的，不会被这两个常用词误触发。
 */
int conv_has_pronoun(const char *q);

/*
 * 补全本轮该用哪个游戏去检索。**每问一轮调一次**。
 *
 *   g_from_query >= 0            -> 用它并记住（问句里写明了，或用户手选了）
 *   无指代词                      -> CONV_GAME_NONE（正常全库，不擅自收窄）
 *   有指代词 && 槽位里有游戏      -> 用槽位（这是多轮能接上的关键）
 *   有指代词 && 槽位里没有        -> CONV_GAME_CLARIFY（**不许瞎猜**）
 *
 * 轮次计数在这里 +1，调用方不必另外记得调用。
 *
 * 典型用法（放在 gkb_split_game 与「用户手选优先」之后）：
 *     gid = gkb_split_game(query, rest, sizeof rest);
 *     if (g_game >= 0) gid = g_game;
 *     r = conv_resolve_game(query, gid);
 *     if (r == CONV_GAME_CLARIFY) { 显示 conv_clarify_prompt(); return; }
 *     gid = r;
 */
int conv_resolve_game(const char *q, int g_from_query);

/*
 * 反问澄清的话术。槽位为空时用它告诉用户「我需要你说是哪款」。
 * 短句 —— 状态行只有 384px，长解释走对话流。
 */
void conv_clarify_prompt(char *out, int out_sz);

#ifdef __cplusplus
}
#endif
#endif /* CONV_STATE_H */
