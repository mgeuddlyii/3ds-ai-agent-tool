/*
 * extract.h — 抽取式回答（见 extract.c 的实测依据）
 */
#ifndef GUIDE_EXTRACT_H
#define GUIDE_EXTRACT_H

#include <stdint.h>

#include "guide_kb.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 一句候选句的缓冲。
 *
 * 大小依据：语料里最长的一句实测 74 个汉字 = 222 字节。
 * 512 留一倍余量。⚠️ 句子会在这个缓冲里**整体比较**，
 * 所以太紧会导致半句被截掉、比较失真 —— 而"半句"看起来仍然像句话，
 * 症状是答案被腰斩而不是报错。 */
#define EXT_SENT_MAX   512

/* 候选句上限。实测每题 4~37 句，48 够用。 */
#define EXT_CAND_MAX   48

/* 取够几句拼成答案。3 是"够回答又不啰嗦"的折中。 */
#define EXT_PICK       3

/*
 * 从 hits 里挑句子，答案写进 out。
 * query 必须是**已剔除游戏名**的剩余问句（见 gkb_split_game）。
 * 返回写入 out 的字节数；0 = 一句都没取到（out 被置空串）。
 *
 * 算法（与 PC 侧 tools/guia/local_chat.py 的 extract() 逐条对应）：
 *   ① hits 的前若干段按 。！？与换行切句
 *   ② 滤掉"标题型"短句（它们是段首拼进去的章节名）
 *   ③ 句中含 query 的任一实体词窗口 -> 标为"命中"
 *   ④ 取命中的前 EXT_PICK 句按**原序**拼接；
 *      一句都没命中时退回"检索序的前 EXT_PICK 句"
 *
 * ④ 的原序很重要：BM25 已经把最相关的段落排在前 4 位，
 * 打乱顺序会让答案读起来像随机抽取。
 */
int gkb_extract(const Gkb *kb, const GkbHit *hits, int n_hit,
                const char *query, char *out, int out_cap);

#ifdef __cplusplus
}
#endif

#endif /* GUIDE_EXTRACT_H */
