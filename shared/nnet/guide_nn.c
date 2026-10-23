/*
 * guide_nn.c — 把 BM25 检索与微模型生成接起来
 *
 * 这个文件是「离线攻略 AI」的大脑与神经的接合处：
 *     gkb_search()  ->  命中段落
 *     gkb_candidate_tokens()  ->  合法答案的 token 集合
 *     nnet_session_generate()  ->  在这个集合里组织语言
 *
 * 为什么要分权：微模型只有 1.20M 参数，记不住 12 万字攻略。
 * 让它硬记的结果是胡编。分工是——
 *     **BM25 提供事实，模型只负责把事实组织成句子。**
 *
 * ══ 三道闸（与联网通道的「四道闸」是同一套思路）══════════════════
 *
 * ① **候选集就是物理约束**，不是提示词。
 *    模型在物理上算不出候选集之外的 token（输出层只遍历候选），
 *    所以「胡编」从概率问题变成结构问题。
 *
 * ② **路径与词表都不来自模型**。模型只吐 id，id 的含义由
 *    nnet_text_decode 决定，而那张表是编译期内嵌的常量。
 *
 * ③ **生成有上限**（GNN_MAX_GEN），且必须采到 EOS 才算完整。
 *    不给上限的话，一个训练不足的模型会一路说下去。
 *
 * ══ 失败策略：一律回退到检索直出 ═══════════════════════════════
 *
 * 这个文件里每一个可能失败的地方都**不返回错误给 UI**，
 * 而是让 do_guide() 走「直接把检索命中的段落显示出来」这条路。
 * 理由：检索层是可靠的（纯查表，零数值计算），模型层是不可靠的
 * （量化、训练不足、文件可能损坏）。让不可靠的一层失败时
 * 拖垮可靠的一层，是本末倒置。
 */
#include "guide_nn.h"

#include <stdio.h>
#include <string.h>

#include "guide_kb.h"
#include "nnet_load.h"
#include "nnet_text.h"

/* ------------------------------------------------------------------ */
/* 全局会话                                                            */
/* ------------------------------------------------------------------ */

/*
 * 单例。这个模块不做重入 —— 3DS 上同时只有一份模型内存有意义
 * （1.2MB 权重 + 132KB KV cache，再开一份就把可写内存吃光）。
 * 真要做多会话，得先把「按需加载/换出」做出来，那是另一件事。
 *
 * ⚠️ 状态缓冲用**静态数组**而不是 malloc：
 *   - NNLoader 里有 layers[32] + tok_emb，sizeof 约 1.3KB；
 *   - 推理状态（nnet_state_bytes）实测约 30KB。
 *   两者都放在这里，一处分配都没有，插件场景下少两个失败点。
 *   代价是这 30KB 常驻 —— 占可写内存的 3.8%，而它换来的
 *   是「打开失败时不需要 free 任何东西」，这笔账划算。
 *
 *   上限 64KB 是按 nnet_state_bytes 的实际构成留的余量：
 *   D=128/F=384/V=4096/C=128 时约 30KB，翻倍配置（256 维）
 *   约 60KB。超了会在 nnet_state_bind 里被 16MB 上限挡下，
 *   不会踩内存 —— 但那时会返回 NNLM_ERR_STATE 而不是静默溢出。
 *
 * ⚠️ 曾经用 `union { NNLoader ld; unsigned char align[16]; }` 来
 * 强制对齐，gcc 直接报 incompatible pointer type ——
 * union 的首成员类型是 NNLoader，但取出时退化成 union*，
 * 必须强转才匹配。union 在这里没有任何好处：
 * NNLoader 里第一个字段是 NnModel（含指针），静态存储器的
 * 16 字节对齐由链接器保证，__attribute__((aligned)) 更直接。
 */
#define GNN_STATE_MAX  (64 * 1024)

static NNLoader g_ld __attribute__((aligned(16)));

static unsigned char g_state_work[GNN_STATE_MAX] __attribute__((aligned(16)));

static struct {
    int        inited;      /* 0=没试过 1=可用 -1=已失败（不再重试） */
    int        last_err;    /* NNLM_* 错误码，给诊断用 */
    NNSession  ss;
    char       path[GNN_PATH_MAX];
} G;

/* ------------------------------------------------------------------ */
/* 生命周期                                                            */
/* ------------------------------------------------------------------ */

int gnn_available(void)
{
    return G.inited == 1;
}

const char *gnn_last_error(void)
{
    if (G.inited == 1) return "";
    if (G.inited == -1) return nnet_load_strerror(G.last_err);
    return "尚未初始化";
}

/*
 * 打开模型。path 通常是 sdmc 里的模型文件；不存在时试编译期内嵌那份。
 *
 * ⚠️ 失败**不是**灾难：把 inited 记成 -1 并**记住**，
 * 之后每次调用都直接返回，不再重试。理由是 gnn_open() 在
 * guide_enter() 里被调，而那是用户每次进攻略页都会走一遍 ——
 * 一个缺失的模型文件如果每次都去 stat+open，SD 卡 IO 会明显卡手。
 */
int gnn_open(const char *path)
{
    int rc;

    if (G.inited == 1)  return GNN_OK;
    if (G.inited == -1) return GNN_ERR_LOAD;

    if (!path || !*path) {
        G.last_err = NNLM_ERR_IO;
        G.inited = -1;
        return GNN_ERR_LOAD;
    }

    nnet_init();
    /* ld 由我们提供（静态数组，见上面的注释）。
     * 之前这里写的是「先设 ss.ld 再调 nnet_session_open」，
     * 而那个函数开头 memset(ss,0,...) 会把指针清掉 ——
     * 于是 nnet_load_file(NULL, path) 段错误。
     * 根因是 nnet_session_open 的签名没给 ld 参数（死设计），
     * 已改成由调用方传入。 */
    rc = nnet_session_open(&G.ss, &g_ld, path, g_state_work);
    if (rc != NNLM_OK) {
        /* 半成品必须清干净：nnet_session_open 内部已回滚自己分配的部分，
         * 这里再关一次是防它漏回滚 —— 在插件场景里漏一次就是
         * 一次无法回收的分配，而反复重试会把它累积成内存耗尽。 */
        nnet_session_close(&G.ss);
        G.last_err = rc;
        G.inited = -1;
        return GNN_ERR_LOAD;
    }

    G.inited = 1;
    strncpy(G.path, path, sizeof(G.path) - 1);
    G.path[sizeof(G.path) - 1] = '\0';
    return GNN_OK;
}

void gnn_close(void)
{
    if (G.inited != 1) return;
    nnet_session_close(&G.ss);
    G.inited = 0;      /* 注意是 0 不是 -1：允许重新打开 */
}

/* ------------------------------------------------------------------ */
/* 生成                                                                */
/* ------------------------------------------------------------------ */

/*
 * sink 的默认实现：把 token 追加解码到 out。
 *
 * 为什么要「追加解码」而不是「攒 id 最后一次性解码」：
 * BPE 的解码不是逐 token 独立的 —— 一个 UTF-8 字符可能横跨
 * 几个 token，攒齐再解没问题，但边生成边解就会在字符中间截断，
 * 屏上出现半个汉字。所以这里用 nnet_text_decode 的
 * 「追加到已有缓冲」模式，并在末尾补 NUL。
 */
struct sink_ctx {
    char  *out;
    int    cap;
    int    len;
    int    overflow;    /* 溢出了多少字节，供 UI 决定要不要显示提示 */
};

static bool sink_append(void *ud, int32_t token, int step)
{
    struct sink_ctx *c = (struct sink_ctx *)ud;
    int before = c->len;

    (void)step;
    if (!c->out || c->cap <= 1) return true;
    if (c->len < 0) c->len = 0;
    c->out[c->len] = '\0';
    if (c->len < c->cap - 1)
        nnet_text_decode(&token, 1, c->out + c->len, c->cap - c->len);
    c->len = (int)strlen(c->out);
    if (c->len >= c->cap) c->len = c->cap - 1;   /* 夹住，别让下轮越界写 */
    if (c->len == c->cap - 1 && before >= c->len)
        c->overflow++;      /* 这一轮没写进去 */
    return true;                                /* 一直生成到 max_gen */
}

/*
 * 核心：检索命中 -> 组织成句子。
 *
 * 返回 GNN_OK 时 out 里是 UTF-8 文本（NUL 结尾）。
 * 返回错误时 **out 仍然被写成空串** —— 调用方可以无条件显示它，
 * 而失败的情况要靠返回值区分（去显示检索原文）。
 *
 * ⚠️ 温度 0.7 不是调出来的偏好，是这套架构下的合理默认：
 * 候选集已经把「说什么」限死了，模型只决定「怎么排列」，
 * 太低会反复吐同一个词，太高会排出语序错乱的句子。
 */
int gnn_generate(const Gkb *kb, const char *query, int game_filter,
                 const GkbHit *hits, int n_hit,
                 char *out, int out_cap,
                 float temperature, uint32_t *seed)
{
    struct sink_ctx sc;
    int32_t cand[GKB_CAND_MAX];
    int32_t prompt[GNN_PROMPT_MAX];
    int n_cand, n_prompt, rc;

    if (!out || out_cap < 2) return GNN_ERR_ARG;
    out[0] = '\0';
    if (G.inited != 1) return GNN_ERR_LOAD;
    if (!kb || !query)    return GNN_ERR_ARG;

    /* ① 候选集：命中段落里出现过的 token（+EOS）。
     *    返回 0 表示「一条可用候选都没有」，此时**不设候选集**，
     *    走全词表 —— 「不限制」与「限制成空集」是两件不同的事，
     *    后者会让采样空间为空，一路撞到 max_gen 上限。 */
    n_cand = gkb_candidate_tokens(kb, hits, n_hit, cand, GKB_CAND_MAX);
    if (n_cand > 0)
        nnet_set_candidates(&G.ss.st, cand, n_cand);

    /* ② prompt = [BOS] + 游戏名 + SEP + 查询的 token。
     *
     * 为什么把游戏名喂进去：调用方（UI）已经用 gkb_split_game
     * 从查询里剔掉了游戏名再送去检索 —— 对检索来说那是降噪
     * （「宝可梦」这个词在每个宝可梦帖里都出现，IDF 接近 0）。
     * 但对**生成**来说它是强约束：不喂，模型会把相邻游戏的
     * 攻略串起来（候选集来自正确的段落，但 prompt 没告诉它
     * 现在在讲哪个游戏）。
     *
     * SEP 是训练时就有的分隔符（vocab 末尾第三个特殊 token），
     * 训练语料里「游戏名 SEP 问句」就是这个形状，所以喂它
     * 等于回到模型见过的分布里。
     *
     * game_filter < 0（全部游戏）时不加游戏名 —— 这时
     * prompt 的形状变成 [BOS] + 问句，与训练的 QA 形态一致。 */
    n_prompt = 0;
    if (game_filter >= 0) {
        const char *gname = gkb_game_name(game_filter);
        if (gname) {
            int n = nnet_text_encode(gname, prompt + n_prompt + 1,
                                     GNN_PROMPT_MAX - n_prompt - 2);
            if (n > 0) {
                n_prompt += n;
                prompt[n_prompt] = g_nnet_sep_id;
                n_prompt += 1;
            }
        }
    }
    prompt[0] = g_nnet_bos_id;                 /* 永远在位置 0 */

    {
        int n = nnet_text_encode(query, prompt + n_prompt + 1,
                                 GNN_PROMPT_MAX - n_prompt - 1);
        if (n > 0) n_prompt += n;
    }
    if (n_prompt <= 0) {
        /* 查询整个编码不出来（全是停用词，或词表没加载）。
         * 只喂 BOS 让模型自由生成 —— 候选集仍然生效，
         * 所以它顶多说出一段命中原文里的话，不会跑偏。 */
        n_prompt = 1;
    }

    /* ③ 生成。seed 可空 -> 用固定种子，可复现（诊断时需要）。 */
    sc.out = out;
    sc.cap = out_cap;
    sc.len = 0;
    sc.overflow = 0;

    rc = nnet_session_generate(&G.ss, prompt, n_prompt,
                               GNN_MAX_GEN, temperature,
                               seed ? seed : &g_gnn_default_seed,
                               (n_cand > 0) ? cand : NULL, n_cand,
                               sink_append, &sc);
    if (rc < 0) {
        out[0] = '\0';
        return GNN_ERR_GEN;
    }
    return GNN_OK;
}

/*
 * 一次到位：查询 -> 文本。内部自己做检索。
 * 拆成两步（gnn_search_then_generate）是因为 UI 常常需要
 * **先显示命中段落再显示生成结果**，检索那一步的结果要单独拿到。
 */
int gnn_answer(const Gkb *kb, const char *query, int game_filter,
               char *out, int out_cap, float temperature, uint32_t *seed)
{
    GkbHit hits[GKB_HIT];
    int n_hit;

    if (!out || out_cap < 2) return GNN_ERR_ARG;
    out[0] = '\0';
    if (G.inited != 1) return GNN_ERR_LOAD;
    if (!kb || !query)    return GNN_ERR_ARG;

    n_hit = gkb_search(kb, query, game_filter, hits, GKB_HIT);
    if (n_hit <= 0) {
        /* 检索没命中**不是错误**：可能是纯停用词，也可能是词表没加载。
         * 两种都不该由这一层判断，所以只回一个明确的「无命中」。 */
        return GNN_ERR_NO_HIT;
    }
    return gnn_generate(kb, query, game_filter, hits, n_hit,
                        out, out_cap, temperature, seed);
}

uint32_t g_gnn_default_seed = 0x5EED1234u;
