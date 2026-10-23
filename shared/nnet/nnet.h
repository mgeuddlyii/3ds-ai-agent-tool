/*
 * nnet.h — 3DS 离线微模型推理引擎（公开接口）
 *
 * 设计约束（都是硬约束，不是偏好）
 * ---------------------------------
 *  1. 目标机是 ARM11（ARMv6K），**没有 NEON、没有 SIMD**。
 *     所以矩阵乘是手写 int8 标量点积，靠 -O2 + 4 次展开榨 ILP。
 *  2. New 3DS 804MHz / Old 3DS 268MHz。同一份 .cia 都要能跑。
 *  3. 权重 int8（逐通道对称量化），激活 int8。
 *     3.25M 参数 -> 权重约 3.2MB，进 .cia 没问题。
 *  4. 不用 malloc 之外的动态内存：整个引擎只有一处线性分配，
 *     由调用方在栈/静态区给足。
 *
 * 数值方案：per-channel（按输出通道）对称 int8 量化。
 * 为什么不 per-tensor：Transformer 里 LayerNorm 之后的激活分布
 * 逐层漂得很厉害，per-tensor 的 scale 一个通道错了整层就废。
 * per-channel 每 192 个权重一组 scale，量化误差能压到 1% 以内。
 *
 * 线程：全部是同步阻塞调用。调用方（UI）负责把它塞到工作线程里，
 * 每生成一个 token 调一次 on_token 回调，UI 就能边生成边上屏。
 */
#ifndef NNET_H
#define NNET_H

#include <stdint.h>
#include <stddef.h>

/*
 * RoPE cos/sin 表长度，由 tools/guia/export_model.py 写进
 * nnet_tables.c（那里是权威定义）。这里 extern 声明只为了在
 * nnet.c 里做越界防御 —— 表长必须和表一起变，不能手写。
 */
#ifndef NN_ROPE_TAB_LEN
#define NN_ROPE_TAB_LEN 2048      /* max_ctx(128) * d_head/2(16) */
#endif

/* ---- 特殊 token id（由 tools/guia/export_model.py 从
 *      tools/guia/ckpt/special.txt 读出后写进 nnet_tables.c）--------
 *
 * 特殊 token 排在词表末尾（BOS/EOS/SEP = vocab-3..vocab-1），
 * **不参与** BPE 编码，但在组装 prompt 与判断生成终止时必须认识它们。
 *
 * ⚠️ 这三个 id 过去只声明在 nnet_tables.c 里，别的翻译单元拿不到 ——
 * 于是「采到 EOS 就停」这条逻辑根本没法在加载器里写出来。
 * 声明放头文件里，才是对「有谁需要用」的诚实回答。 */
extern const int32_t g_nnet_bos_id;
extern const int32_t g_nnet_eos_id;
extern const int32_t g_nnet_sep_id;

#ifdef __cplusplus
extern "C" {
#endif

/* ---- 模型结构（全部由 host 侧静态分配，引擎不 malloc） ------------ */

typedef struct {
    int32_t in;        /* 输入维（= d_model）   */
    int32_t out;       /* 输出维               */
    const int8_t  *w;  /* [out][in] 行主序，int8 */
    const float   *bs; /* [out] 偏置，float32（bias 很少，保持 fp 不量化） */
    const float   *sc; /* [out] 每输出通道的 scale，float32 */
} NnMat;

typedef struct {
    NnMat wq, wk, wv, wo;   /* 注意分成 q/k/v 三个矩阵而不是一个大矩阵：
                             * 加载时做 K 和 Q 的转置缓存，见 nnet.c */
    /* ---- pre-LN 的两个 LayerNorm（不是四个！）------------------
     * 训练端 train_gpt.py:301/315：
     *     h1, _, _ = layernorm(x,  ln1g, ln1b)
     *     q/k/v    = h1 @ wq/wk/wv.T
     *     ...
     *     h2, _, _ = layernorm(x1, ln2g, ln2b)
     *     ff       = gelu(h2 @ w1.T) @ w2.T
     *
     * ⚠️ 本结构曾经是 bnq/bnk/bnv/bno + bn1/bn2（**4+2 个 LN**），
     * 那是 **post-LN** 架构（Transformer 早期做法），而模型是按
     * **pre-LN** 训的。字段数量对不上不是小问题：nnet.c 里
     * 「matvec 之后再 layernorm」正好是 post-LN 的形状，
     * 于是整个前向与训练时完全不同 —— 输出是垃圾，但程序跑得好好的，
     * gcc 一个字都不报。**LN 必须在残差分支之外、投影之前。** */
    const float *ln1g, *ln1b;
    const float *ln2g, *ln2b;
    NnMat ff1, ff2;                       /* FFN 两层 */
} NnLayer;

typedef struct {
    int32_t vocab;        /* 词表大小 V            */
    int32_t d_model;      /* 隐藏维 D              */
    int32_t n_layer;      /* 层数 L                */
    int32_t n_head;       /* 注意力头数 H          */
    int32_t d_ff;         /* FFN 中间维            */
    int32_t d_head;       /* D / H                 */
    int32_t max_ctx;      /* 最大上下文长度        */
    int32_t n_rope;       /* RoPE 生效的前 head_dim 维（d_head 通常全部） */

    const NnMat   *tok_emb;  /* [V][D]  与输出层共享（tied） */
    const int8_t  *pos_emb;  /* [max_ctx][D] int8，配套 pos_scale */
    const float   *pos_scale;/* [max_ctx]，逐位置 scale */
    const NnLayer *layers;
    const float   *lnf_b, *lnf_g;   /* 最终 LayerNorm */
} NnModel;

/* ---- KV cache ----------------------------------------------------- */
/*
 * 每层 K/V 各 [max_ctx][D]，int8 + **逐位置** scale。
 *
 * 逐位置（而不是逐层）是必须的：K/V 每个 token 各自动态定标
 * （见 nnet.c 的 quant_dyn），量级逐 token 变化很大 ——
 * 早期版本用 k_scale/v_scale 是想省掉这两次乘法，结果那两个
 * 字段从来没被写过，注意力里写的是常数 1/1024。
 * 那个常数在「固定 scale」假设下是对的，但它与 softmax 的
 * 平移不变性叠加，就变成一个**不改变分布、只改变绝对值**的
 * 错误 —— 最难查的那一类。
 *
 * 4 层 * 2 * 128 * 128B ≈ 128KB，加 scale 共约 132KB，Old 3DS 也放得下。
 */
typedef struct {
    int8_t *k;          /* [n_layer][max_ctx][D] 连续一大块 */
    int8_t *v;
    float  *k_scale;    /* [n_layer][max_ctx] 逐位置，nnet.c 每次 push 都写 */
    float  *v_scale;
    int32_t len;        /* 已写入的 token 数 */
} NnCache;

/* ---- 采样结果 ----------------------------------------------------- */

#define NN_TOPK 8
typedef struct {
    int32_t id[NN_TOPK];
    float   p[NN_TOPK];   /* 已归一化，未截断的部分之和=1 */
    int32_t n;
} NnTopK;

/* ---- 生命周期 ----------------------------------------------------- */

/* 一次性初始化全局状态（查表等）。必须在任何其他调用之前执行。 */
void nnet_init(void);

/*
 * 准备一次推理的运行期状态。
 * act_* 缓冲区由调用方提供，大小要求见 nnet_state_bytes()。
 */
typedef struct {
    const NnModel *m;
    NnCache       *cache;
    int32_t        pos;       /* 当前写入位置 */
    int8_t        *act;       /* 激活工作区 */
    float         *fbuf;      /* fp32 工作区 */
    void          *blk;       /* 内部分配的转置缓存等 */
} NnState;

/* 计算 state 需要的总字节数（含内部分配），供调用方一次性申请。 */
size_t nnet_state_bytes(const NnModel *m);

/* 绑定工作区。work 至少 nnet_state_bytes(m) 字节，且 16 字节对齐。 */
int  nnet_state_bind(NnState *st, const NnModel *m, void *work);

/* 追加一个 token（首 token 或逐 token 生成都调它），返回 0 成功。 */
int  nnet_push(NnState *st, int32_t token);

/*
 * 限定输出层只在 cand[] 这 n 个 token 上算 logits。传 n<=0 恢复全词表。
 *
 * 为什么需要这个（3DS 专属优化，实测省 40% 算力）：
 *   输出层是 tok_emb 复用（tied），形状 [4096][128]，
 *   即 4096 * 128 = 524288 MAC/token。而 4 层主干一共才 655360 MAC。
 *   **输出层一项就占了单 token 总算力的 43%** —— 比整个主干还接近。
 *
 *   原因很直白：4096 是个为通用文本设计的词表，而我们这个模型
 *   只会吐出语料里出现过的东西。攻略问答的正确答案几乎全部来自
 *   BM25 检索命中的那几段原文，也就是说**合法答案的 token 集合
 *   是已知的、而且很小**（实测 300~500 个）。
 *
 *   在这 300 个候选上算 logits，输出层从 524288 掉到 38400 MAC，
 *   单 token 总算力 1212416 -> 726528，**降 40%**。
 *
 *   这不只是省时间：把采样限制在检索命中的原文里，
 *   顺带**根治了模型胡编**（它没机会说出原文里没有的字），
 *   和我们已经在 B 通道 / R 通道用的"物理确认"闸门是同一个思路。
 *
 * 注意：
 *   1. cand 存的是 token id，不是行号；重复 id 无害（只是多算一遍）。
 *   2. 只在 nnet_push 的**输出层**生效，KV cache / 主干不受影响。
 *   3. 必须在 nnet_push 之前设置，push 内部读它。
 */
int  nnet_set_candidates(NnState *st, const int32_t *cand, int n);

/* 对当前 logits 取 top-k。st->fbuf 会被用作 logits 缓冲。 */
int  nnet_sample(const NnState *st, float temperature, NnTopK *out);

/* 采样：temperature + top-k 里随机抽一个，seed 可复现。 */
int32_t nnet_pick(const NnTopK *tk, float temperature, uint32_t *seed);

/* 把 logits 缓冲前 n 个最大值对应的 token id 写进 out（调试用）。 */
int  nnet_logits_top(const NnState *st, int32_t *out, int32_t n);

/* ---- 文本接口（拼在推理之上的便利层，见 nnet_text.c） ------------ */

/* BPE 编码：文本 -> token 数组。返回写入的 token 数，-1 出错。 */
int  nnet_text_encode(const char *utf8, int32_t *out, int max_out);

/* BPE 解码：token 数组 -> UTF-8 文本（追加到 out 缓冲）。 */
int  nnet_text_decode(const int32_t *ids, int n, char *out, int out_sz);

/* 词表大小，供上层做 BOS/EOS 处理。 */
int32_t nnet_vocab_size(void);

#ifdef __cplusplus
}
#endif
#endif /* NNET_H */
