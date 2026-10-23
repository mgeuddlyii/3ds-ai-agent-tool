/*
 * nnet_load.h — 把 app/data/guide_model.bin 绑成 NnModel 指针树
 *
 * ══ 为什么需要这个文件 ═══════════════════════════════════════════
 *
 * 推理引擎（nnet.c）从头到尾**没有做过文件 I/O**。它只接受一个
 * 已经填好的 NnModel：每个 NnMat 的 w / sc / bs 三个指针分别指向
 * 权重、per-channel scale、bias。引擎本身是纯计算，可重入、无状态。
 *
 * 这不是过度设计而是必要的：3DS 上一个 2.9MB 的权重文件，
 * 每次进游戏都重新解析一遍是浪费；而把解析逻辑塞进引擎会让
 * 引擎依赖 stdio，在「被宿主游戏加载的插件」场景里多一个
 * 失败来源（fd 耗尽、sdmc 未就绪、文件被占用）。
 *
 * ══ 文件格式（GM1，见 tools/guia/export_model.py）════════════════
 *
 *   "GM1\0"                              4B
 *   vocab d_model n_layer n_head
 *   d_ff max_ctx d_head n_rope           (8 x i32，共 32B)
 *   wte_i8 [vocab*d_model]               tied，兼输出层
 *   wte_sc f32[vocab]
 *   wpe_i8 [max_ctx*d_model]
 *   wpe_sc f32[max_ctx]
 *   每层 L：
 *     wq/wk/wv/wo: i8[D*D] + sc f32[D] + b f32[D]
 *     ln1g ln1b ln2g ln2b: f32[D] x4
 *     ff1: i8[F*D] + sc f32[F] + b f32[F]
 *     ff2: i8[D*F] + sc f32[D] + b f32[D]
 *   lnf_g lnf_b: f32[D] x2
 *
 * 全部小端。3DS 是小端 ARM，**不做字节序转换** ——
 * 写出端（x86 PC）和读出端（ARM）都是小端，转一次反而是引入 bug。
 *
 * ══ 内存策略 ═════════════════════════════════════════════════════
 *
 * 两种模式，取决于权重是内嵌（bin2s）还是从 SD 卡读：
 *
 *   NNLM_EMBEDDED  权重在 .rodata 里（编译期内嵌）
 *                  → **零分配**，指针直接指过去。
 *                  2.9MB 塞进 .cia 会让固件很大，且常驻内存，
 *                  但读 SD 卡要 1~2 秒。
 *
 *   NNLM_FROM_FILE 从 SD 卡读进 malloc 的缓冲
 *                  → 一次 malloc，之后和内嵌走同一条路径。
 *
 * 两种模式都**不拷贝**：NnMat 的指针直接指进那块缓冲。
 * 所以 NNModel 和那块缓冲必须同生共死 —— 由 NNLoader 持有。
 */
#ifndef NNET_LOAD_H
#define NNET_LOAD_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "nnet.h"

#ifdef __cplusplus
extern "C" {
#endif

#define NNLM_PATH_MAX 96

/* 错误码。全部是「拒绝加载」，调用方据此回退到检索直出。 */
enum {
    NNLM_OK          =  0,
    NNLM_ERR_IO      = -1,   /* 打不开 / 读不到 / 写不出 */
    NNLM_ERR_SHORT   = -2,   /* 文件比头部还短 */
    NNLM_ERR_MAGIC   = -3,   /* 不是 GM1 */
    NNLM_ERR_HEADER  = -4,   /* 维度非法（0 或负） */
    NNLM_ERR_TRUNC   = -5,   /* 声明的长度超过实际文件 */
    NNLM_ERR_ALIGN   = -6,   /* 文件偏移没对齐 4 字节 */
    NNLM_ERR_RANGE   = -7,   /* 维度超过编译期上限 */
    NNLM_ERR_MEM     = -8,   /* malloc 失败 */
    NNLM_ERR_DIM     = -9,   /* d_head * n_head != d_model */
    NNLM_ERR_ROPE    =-10,   /* n_rope 与 d_head/表长不匹配 */
    NNLM_ERR_STATE   =-11,   /* 状态缓冲申请失败 */
};

/*
 * 编译期上限。作用是**在分配之前**拦住 absurd 尺寸 ——
 * 一个损坏或截断的文件头可以声称 vocab=2^31，那一下 malloc
 * 就把 3DS 的内存吃光了，而且失败得很不体面（可能直接被
 * loader 杀进程，看起来像是我们崩了）。
 *
 * 真值来自 export_model.py 的 CFG。留了余量给以后换小模型，
 * 但不许放大到「反正是 int32 所以理论上能到 2^31」。
 */
#define NNLM_MAX_VOCAB   16384
#define NNLM_MAX_DMODEL  1024
#define NNLM_MAX_LAYER      32
#define NNLM_MAX_HEAD      128
#define NNLM_MAX_FF       16384
#define NNLM_MAX_CTX      4096

typedef struct {
    NnModel   model;
    NnLayer   layers[NNLM_MAX_LAYER];
    NnMat     tok_emb[1];        /* tied：物理上只有一份 */

    unsigned char *buf;          /* NNLM_FROM_FILE 时的自有缓冲 */
    size_t         buf_len;
    int            state;         /* 上一个错误码，便于诊断 */
} NNLoader;

/*
 * 从 SD 卡加载。path 通常是 "sdmc:/3ds/aiconsole/guide_model.bin"。
 * 成功返回 NNLM_OK，**loader 必须保持存活**直到推理结束。
 *
 * 会读两次文件：先 stat 出长度，再一次性读完。用 fseek/ftell
 * 而不是「边解析边读」，因为后者在中途发现截断时已经无法回退 ——
 * 而我们必须在任何指针被填进去之前就知道整个文件是否完整。
 */
int  nnet_load_file(NNLoader *ld, const char *path);

/*
 * 绑定到已在内存里的映像（编译期内嵌的权重走这条路）。
 * base 必须在整个推理期间保持有效，且 4 字节对齐。
 * len 是完整长度，会被用来做截断检查。
 */
int  nnet_load_mem(NNLoader *ld, const void *base, size_t len);

/* 释放自有缓冲。**不要**在推理中途调用。 */
void nnet_load_free(NNLoader *ld);

/* 人话版错误原因，给 UI 直接显示。 */
const char *nnet_load_strerror(int code);

/* ---- KV cache（nnet.c 的 st->cache 由调用方提供）------------------ */

/*
 * 算 cache 需要多少字节。layout 是 [n_layer][max_ctx][D] 的 int8，
 * k 和 v 各一份，另有逐位置 scale。
 * 这是**按层数算**的 —— 曾经有一版只算一份，
 * 于是第 2~6 层的 K 全部写越界（见 nnet.c 里的注释）。
 */
size_t nnet_cache_bytes(const NnModel *m);

/* 分配并清零 cache。失败返回 NULL。 */
NnCache *nnet_cache_new(const NnModel *m);
void     nnet_cache_free(NnCache *c);

/*
 * 一站式：加载 + 建 state + 建 cache。给上层唯一要调的入口。
 * state_work 至少 nnet_state_bytes(m) 字节（16 字节对齐）。
 * 任一步失败都回滚已分配的部分，不留半成品。
 *
 * ⚠️ ld 由调用方提供，且**必须在调用期间保持有效**
 * （NNSession 里只存它的指针）。原因见 nnet_session_open 的注释：
 * NNLoader 约 1.3KB，放栈上会挤爆 3dsx 的 16KB 主线程栈，
 * 放堆上就多一个 malloc 失败点 —— 交回给调用方决定。
 */
typedef struct {
    NNLoader *ld;      /* 调用方提供，本函数不分配也不释放它 */
    NnState   st;
    NnCache  *cache;
} NNSession;

int  nnet_session_open(NNSession *ss, NNLoader *ld, const char *path,
                       void *state_work);
void nnet_session_close(NNSession *ss);

/* 一次生成：喂完 prompt 后逐 token 出字，回调返回 false 可提前停。
 * 候选集 cand/cand_n 传 NULL/0 表示走全词表。 */
typedef bool (*NnetSink)(void *ud, int32_t token, int step);
int  nnet_session_generate(NNSession *ss, const int32_t *prompt, int n_prompt,
                           int max_gen, float temperature, uint32_t *seed,
                           const int32_t *cand, int cand_n, NnetSink sink,
                           void *ud);

#ifdef __cplusplus
}
#endif
#endif /* NNET_LOAD_H */
