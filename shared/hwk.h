/*
 * hwk.h — 3DS 硬件知识库检索
 *
 * 存在的理由：cupd.c 能装 hw_knowledge.txt，但 2026-10-04 之前
 * **没有任何代码读它** —— 装上去等于没装。这条「假更新」只有把每类
 * 可更新内容的实际读者逐条查一遍才会暴露（见 cupd.h 里的记录）。
 *
 * ══ 为什么不用攻略库那套 BPE + 倒排索引 ═══════════════════════════
 *
 * 攻略库用 BPE 是因为它有 836 段、40 万字节、4096 词表，扛得住。
 * 硬件库是 25 条、6KB，**规模差两个数量级**，上倒排索引纯属浪费。
 *
 * 更要紧的是**词表不匹配**：BPE 词表是从 31 款游戏攻略里学出来的，
 * 里面没有 `ldrsb`、`smlabb`、`PICA200`、`ptm:sysm` 这些词 ——
 * 它们会被切成碎片，而碎片全是高频词（df 高 → idf 低），
 * 真正的实体词反而被压过去。这正是 guide_kb.h 顶部描述的那个问题，
 * 在硬件词上会更严重（不是 5 个碎片，是 6 个且全无意义）。
 *
 * → 所以这里用**字符 bigram 重叠**：不需要词表、不需要训练、对
 * 专有名词和未登录词都有效。代价是索引不是倒排、是逐条线性打分；
 * 25 条 × 每次几微秒，与 3DS 的性能预算比可以忽略。
 *
 * ══ 覆盖优先于排序 ══════════════════════════════��════════════════
 *
 * 打分用「问句的 bigram 有多少比例出现在候选问句里」（覆盖率），
 * 再加两个调整项：候选问句更短时给长度惩罚（避免长条目靠字数取胜），
 * 问句被候选问句**完整包含**时给一个大加分（那是明确命中）。
 */
#ifndef HWK_H
#define HWK_H

#include <stdbool.h>
#include <stddef.h>

/* 条目数上限。实测 25 条，512 留 20 倍余量。
 * ⚠️ 改这个数要同时看下面的 HWK_MAX_ANSWER —— 答案缓冲按最长的那条定。 */
#define HWK_MAX_ENTRY  512

/* 单条答案的字节上限。实测最长 421 字节。 */
#define HWK_MAX_ANSWER 768

/* 单条问句的字节上限。实测最长 34 字节（"怎么知道3ds有没有升频成功"）。 */
#define HWK_MAX_ASK    96

/* 打分结果。best 需要 HWK_MAX_ANSWER + 1 字节。 */
typedef struct {
    int  n;                 /* 一共多少条 */
    int  hit;               /* 命中的条目下标，-1 = 没有 */
    int  score;             /* 命中率 ×100（整数，避免把 dtoa 拖进 UI） */
    int  over;              /* 得分超过它的条目数，用于 UI 说"第几条" */
    char best[HWK_MAX_ANSWER + 1];
    char ask[HWK_MAX_ASK];  /* 命中的那条问句，用于显示出处 */
} HwkResult;

/*
 * 加载知识库。SD 卡优先（sdmc:/3ds/aiconsole/hw_knowledge.txt），
 * 失败回退编译期内嵌的那份。返回条目数，负数是错误码：
 *   -1 文件都读不到（两条来源都没了）  -2 一条也解析不出来
 *
 * 幂等：重复调用会先释放上一次的内容。**不返回时 HwkResult 不会被填**，
 * 调用方必须自己看返回值 —— 这与 gkb_bind 一样，不设"传 NULL 就跳过"的分支。
 */
int  hwk_load(void);
void hwk_free(void);

/* 当前是否可用（hwk_load 成功过）。 */
bool hwk_ready(void);

/* 当前用的是哪一份：1 = SD 卡，0 = 内嵌。给诊断用。 */
int      hwk_from_sd(void);
const char *hwk_sd_path(void);

/* 检索。out->hit == -1 表示没找到（score 会是 0）。 */
void hwk_search(const char *query, HwkResult *out);

#endif /* HWK_H */
