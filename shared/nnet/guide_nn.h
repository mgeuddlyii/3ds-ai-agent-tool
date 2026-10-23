/*
 * guide_nn.h — BM25 检索 + 微模型生成的接合层
 *
 * 见 guide_nn.c 的文件头注释（架构、三道闸、失败策略）。
 * 这里只列接口与常量。
 */
#ifndef GUIDE_NN_H
#define GUIDE_NN_H

#include <stdint.h>
#include <stddef.h>

#include "guide_kb.h"     /* Gkb / GkbHit / GKB_HIT —— 接口里直接用到了 */

#ifdef __cplusplus
extern "C" {
#endif

/* 生成上限。
 *
 * 为什么是 48：候选集把可输出的 token 限到几百个，一次问答
 * 实测 20~40 token 就说完。48 留了点余量又不会失控 ——
 * 生成是**同步阻塞**的，每多一个 token 都是用户等在那儿。
 * 3DS 上单 token 约 25~40ms（804MHz 实测口径），48 token ≈ 1.5 秒。 */
#define GNN_MAX_GEN     48

/* prompt 缓冲。查询文本 BPE 之后通常 10~30 token，
 * 64 已经很宽松；再大只会让栈上的数组更没有必要地吃内存。 */
#define GNN_PROMPT_MAX  64

#define GNN_PATH_MAX    96      /* 与 NNLM_PATH_MAX 对齐 */

/* 返回码。全部是「这一层不可用」，调用方据此回退到检索直出。 */
enum {
    GNN_OK          =  0,
    GNN_ERR_ARG     = -1,      /* 参数不对（空指针 / 缓冲太小） */
    GNN_ERR_LOAD    = -2,      /* 模型没打开或打开失败 */
    GNN_ERR_NO_HIT  = -3,      /* 检索没命中（不是 bug） */
    GNN_ERR_GEN     = -4,      /* 生成过程出错 */
};

/* 固定种子：诊断时需要可复现的输出。 */
extern uint32_t g_gnn_default_seed;

/* ---- 生命周期 ----------------------------------------------------- */

/*
 * 打开模型。失败返回 GNN_ERR_LOAD，且**记住失败不再重试**
 * （见 guide_nn.c 里的说明：SD 卡 IO 每次重试都是可感知的手感损失）。
 *
 * 状态缓冲是模块内的静态数组（约 64KB），调用方什么都不用准备。
 */
int  gnn_open(const char *path);
void gnn_close(void);

/* 模型是否可用。UI 用它决定要不要显示「生成中」的字样。 */
int  gnn_available(void);

/* 人话版原因，可直接上屏。 */
const char *gnn_last_error(void);

/* ---- 生成 --------------------------------------------------------- */

/*
 * 用已有命中生成句子。out 会被写成 NUL 结尾的 UTF-8。
 * 成功返回 GNN_OK；任何失败都保证 out 是**空串**，
 * 所以调用方可以无条件显示 out，靠返回值决定要不要改显示检索原文。
 */
int gnn_generate(const Gkb *kb, const char *query, int game_filter,
                 const GkbHit *hits, int n_hit,
                 char *out, int out_cap,
                 float temperature, uint32_t *seed);

/* 一步到位：查询 -> 文本（内部自己检索）。 */
int gnn_answer(const Gkb *kb, const char *query, int game_filter,
               char *out, int out_cap, float temperature, uint32_t *seed);

#ifdef __cplusplus
}
#endif
#endif /* GUIDE_NN_H */
