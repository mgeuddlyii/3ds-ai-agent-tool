/*
 * orch.h — 非攻略问题的统一路由（编排层 M1）
 *
 * 为什么要单独一层，而不是在 do_search 里依次 if：
 * **优先级顺序本身是需要被测试的东西。** 散在 UI 函数里的顺序改错了
 * 只有真机能发现。这里把它收成一个纯函数，PC 侧可以逐条判。
 *
 * 当前优先级（从高到低）：
 *   1. 工具层   算式 / 日期  —— 精确、无歧义，必须先判
 *   2. 闲聊层   整句寒暄    —— 判不准就放行
 *   3. 攻略检索  兜底        —— 真正的游戏问题
 *
 * 顺序不是随意的：算式**绝不**该被当成寒暄（"1+1" 里没有寒暄词，但反过来
 * "你好1+1" 呢？工具先判能保证它算出来而不是被寒暄吃掉），
 * 而闲聊绝不能抢攻略问题（由 chitchat 自己的防护栏保证）。
 */
#ifndef ORCH_H
#define ORCH_H

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    ORCH_GUIDE = 0,   /* 没被接管：交给攻略检索 */
    ORCH_TOOL,        /* 工具层答了（算式 / 日期） */
    ORCH_CHITCHAT     /* 闲聊层答了 */
} OrchKind;

/*
 * 试着接住这一句。接住就把答案写进 out 并返回对应 Kind；
 * 返回 ORCH_GUIDE 时 out 不会被写，调用方应继续走攻略检索。
 */
OrchKind orch_route(const char *q, char *out, int out_sz);

#ifdef __cplusplus
}
#endif
#endif /* ORCH_H */
