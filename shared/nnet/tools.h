/*
 * tools.h — 工具层（编排层 M6）
 *
 * 给 agent 装"工具"，不是为某道题写特判。覆盖：
 *   算式求值（1+1、3*(4+5)、2^10、50% …）
 *   单位换算（长度/重量/时间/数据量/温度）
 *   日期时间（今天几号、几点、星期几）
 *
 * 全离线、零网络、零依赖（除了 libc 的 time()）。
 *
 * ⚠️ 与神经模型的关键分工：**算术绝不能交给模型**。
 * 小模型做多位数乘除是"背"出来的，覆盖不全就会错，而且是静默错。
 * 规则求值 100% 准确。1+1 这种问题走这里，不走 nnet。
 */
#ifndef TOOLS_H
#define TOOLS_H

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    TOOL_NONE = 0,   /* 答不了，交给别处（检索/闲聊） */
    TOOL_CALC,       /* 算式   ✅ 本轮已实现 */
    TOOL_UNIT,       /* 单位换算 ✅ 已实现（长度/质量/容量/温度） */
    TOOL_TIME        /* 日期时间 ✅ 本轮已实现 */
} ToolKind;

/*
 * 试着回答 q，结果写进 out（短句，状态行只有 384px，别写长篇）。
 * 返回 TOOL_NONE 表示"不归我管"。
 *
 * 纪律：判不准就返回 TOOL_NONE。宁可漏答，不可抢答 ——
 * 抢答会把真正的攻略问题吃掉，答成一句算术，比不答更糟。
 */
ToolKind tools_try(const char *q, char *out, int out_sz);

/* 单项求值，判据与孪生测试用得到（tools_try 内部也走它）。
 * 返回 1 成功 / 0 不是算式 / -1 算式但算不出来（除零、溢出、括号不配对）。
 */
int tools_eval(const char *expr, float *out_val);

#ifdef __cplusplus
}
#endif
#endif /* TOOLS_H */
