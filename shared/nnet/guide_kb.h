/*
 * guide_kb.h — 离线攻略知识库（检索层）
 *
 * 这个模块解决 2.88M 参数模型的最大短板：**它记不住事实**。
 * 十几万字的攻略语料不可能塞进 2.9M 参数里。
 *
 * 所以架构是：检索负责事实，模型负责组织语言。
 *   问句 -> 拆游戏名定范围 -> BPE 分词 -> BM25 打分 -> top-k 段落
 *        -> 神经网络逐 token 组织语言 -> 命中段落即为事实来源
 *
 * 全离线。检索层不涉及任何网络。
 *
 * 为什么游戏名要单独处理（本项目最关键的设计）
 * --------------------------------------------
 * BPE 词表是从语料里学出来的，**游戏名恰好没被学成完整 token**：
 *     火焰纹章觉醒   -> 火焰/纹/章/觉/醒（5 个碎片）
 *     马力欧卡丁车 7 -> 马/力/欧/卡丁车/7
 *     boss战         -> os/s/战（boss 切成两个字节）
 * 而道具动作词都是完整 token：大师之剑 / 漂移 / 转职 / 钩索箭 / 御三家。
 *
 * 后果：查询里的碎片都是高频词（df=21~24），五个碎片加起来把真正的
 * 实体词（转职 df=9）压过去，实测「火焰纹章觉醒 转职怎么转」会答成
 * 另一个游戏的宝可梦进化石。**所以游戏名只用来定范围，不参与打分。**
 * 这也正好是产品上该有的交互：先选游戏，再提问。
 */
#ifndef GUIDE_KB_H
#define GUIDE_KB_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * 段落数上限。
 *
 * ★ 2026-10-05 从 4096 抬到 16384 —— 但**不是**靠"改个数字"抬的：
 *   gkb_search 里 `float score[GKB_MAX_PASSAGES]` 与 `uint8_t hitc[...]`
 *   原本是**栈上数组**，4096 时就是 20KB 栈，16384 会变成 80KB 栈
 *   （3DS 主线程栈远不够），光改这里只会把报错换成栈溢出。
 *   已把那两个数组改成 gkb_search 内的 static（本程序单线程、不重入，
 *   见 guide_kb.c 里那句注释），代价是 80KB .bss，对 3DS 的 RAM 可忽略。
 *   抬完的收益：索引 n_passage 从 4496 涨到 8000 量级也不再 load 失败。
 *
 * ★★ 必须配 build_kb.py 那侧同源的断言：n_passage > 这个上限时
 *    gkb_load 直接 return -3（见 guide_kb.c），整个知识库废掉、检索全挂。
 *    2026-10-05 就是这么中招的：索引 n_passage=4496 > 4096，
 *    而离线校验全在 Python 里直接读 .bin、压根不走 gkb_load，
 *    于是 verify_all_offline.py 7/7 全绿、卡上其实一点都检索不出来。
 */
#define GKB_MAX_PASSAGES 16384     /* 段落数上限 */
/*
 * 每个词最多索引多少段落。**必须与 build_kb.py 的 MAX_POSTING 一致**。
 *
 * 为什么是 64（2026-10-04 按实测定的）：
 *   31 款语料 1174 段下，真实 df 16~60 的**实体词**区若按原来的 24 截断，
 *   会丢掉 8307 条 —— 也就是大部分游戏的段落被整段切掉，
 *   症状是「限定游戏检索 0 命中」（候选词表 19 处失效全是这个）。
 *   提到 64 后该区零丢弃，索引 563 → 607 KB。
 *   C 端倒排约 300 KB，占可写 784 KB 的 38%，可承受。
 *
 * ⚠️ 不要无脑提到"不丢任何段落"：那要 1153（「的」在 827 段里），
 * 而「的」idf 本来就接近 0，存下 827 条既占内存又不带来检索价值。
 * df>120 的区间仍会丢，**那是应该丢的**。
 *
 * ⚠️ 两边不一致时的症状：Python 侧多存了、C 端读到越界
 * → 检索结果随机错乱，**不报错**。所以改这个值必须两边同时改。
 *
 * ★ 2026-10-05 重标（语料 31 款 -> 211 款、段数 1174 -> 2934）：df 分布
 * 整体右移，原来落在 df 64 以下的中等频实体词（「宝可梦」这类）漂到了
 * 65~120 这一档（682 词，已成最大实体档）。沿用 64 会开始截它们，
 * 症状仍是「限定游戏检索 0 命中」。取 128 让 df<=128 区间零丢弃；
 * 标定脚本 `tools/guia/tmp/calib_posting.py`，改语料规模后先跑它再定值。
 */
#define GKB_MAX_POSTING  128       /* 每个词最多索引多少段落 */
#define GKB_MAX_QUERY    64        /* 查询最多取多少个 token */
#define GKB_HIT          4         /* 取 top-4 段落 */

/* 输出层候选集上限。实测 top-4 并集最大 248，开 1024 留 2 倍余量 ——
 * 语料扩了之后段落会长，这个数要跟着重跑 cand_probe.py 量，别拍。
 * 它同时决定打点表大小：1024*8 = 1KB bit表，即 128 字节。
 * 越界不是静默截断成「前 1024 个」，而是靠这个上限在循环里
 * 提前收口（见 guide_kb.c 里的 max_out-4 判据）。 */
#define GKB_CAND_MAX     1024

/* 候选集构造时单段原文的临时缓冲。语料里最长一段 633 字节，
 * 768 留够。段落原文要按 tok_len 显式拷贝（kb->text 里段末无 NUL）。 */
#define GKB_CAND_TEXT    768

typedef struct {
    int32_t id;         /* 段落 id                                     */
    float   score;      /* BM25 得分（已含覆盖度与概述段降权）        */
    int32_t tok_off;    /* 该段落文本在 kb->text 里的字节偏移          */
    int32_t tok_len;    /* 字节长度（字段名沿用历史叫法，单位是字节） */
    int32_t game;       /* 所属游戏，用于「换个游戏问」过滤           */
} GkbHit;

typedef struct {
    /* --- 静态内容（解压缩后的缓冲区，或直接指向只读段） --- */
    const char    *text;       /* 全部段落首尾相接，UTF-8，无 NUL 结尾 */
    int32_t        text_len;
    const int32_t *pass_off;   /* [n_passage+1] 段落在 text 的字节范围  */
    /* [n_passage] 所属游戏。
     * ⚠️ 这里**必须**是 int16：语料扩到 211 款后 game_id 到 210，
     * int8 在 128 就回绕成负数，限定游戏过滤会静默漏掉/hit 错游戏。
     * 早先这个字段是 int8，且 gkb_bind 里的守卫只判 `GAME_N > 255`
     * —— 判据比真实上限（127）松，128~255 那一段**放行错误数据**，
     * 属于"永远绿的守卫比没有更坏"，现已改成 int16 并把判据对齐。 */
    const int16_t *pass_game;  /* [n_passage] 所属游戏                 */
    const int32_t *post_off;   /* [vocab+1] 倒排表在 post[] 的起点      */
    const int16_t *post;       /* 倒排表：段落 id                        */
    const int8_t  *post_tf;    /* 与 post[] 等长：词频（封顶 8）         */
    int32_t        n_post;     /* 倒排条目总数                          */
    int32_t        n_passage;
    int32_t        vocab;
    int32_t        n_game;
    float          avg_len;    /* 平均段落字节数（文件头原值，仅记录）  */

    /* --- 运行期（gkb_bind 分配，gkb_free 释放） --- */
    unsigned char *blob;       /* zlib 解压后的整个 GKB1 映像           */
    int16_t       *pass_tlen;  /* [n_passage] 每段 token 估计长度       */
    float          avg_tlen;   /* 平均 token 长度，BM25 归一化用        */
} Gkb;

/*
 * 绑定知识库。数据来源有两条，**SD 卡优先、失败自动回退内嵌**：
 *   1) sdmc:/3ds/aiconsole/guide_kb.bin（内容更新通道装的那份，裸 GKB1）
 *   2) 编译期内嵌的 nnet_kb_data.c / gkb_data[]（GKZ 压缩）
 * 内嵌那份永远在二进制里，所以 SD 拔了照样能用。
 *
 * 成功返回段落数；负数是错误码：
 *   -1 数据太短  -2 magic 不对  -3 头部非法  -4 游戏数与别名表不同步
 *   -5 游戏数超出 int16 表示  -6 倒排长度非法  -7 数据被截断
 *   -8 内存分配失败  -9 两条来源都拿不到数据
 *
 * ⚠️ -4 值得单说：SD 上那份来自别的构建时，游戏集可能与本二进制里的
 * g_alias[] 不同步。此时**必须用内嵌那份**，否则检索在一个和 UI 说法
 * 不一致的编号空间里打分，答非所问而且不报错。所以 -4 属于
 * 「回退」而不是「失败」。
 *
 * 关于 -5：历史上这条的注释写的是「倒排长度非法」，而代码里它其实拦的是
 * GAME_N > 255；-6 才是倒排表长度。注释与实现对不上时，读注释的人会去查
 * 完全无关的地方，所以按实现改。
 */
int  gkb_bind(Gkb *kb);
void gkb_free(Gkb *kb);

/*
 * 上一次成功绑定用的是哪一份：1 = SD 卡那份，0 = 编译期内嵌那份。
 *
 * 存在的理由：SD 上「有文件但用不了」是**静默回退**的（见上面 -4 的说明），
 * 用户界面上看不出异常。诊断页要能回答"到底在用哪份"，
 * 才查得清"我装了更新为什么没变化"。
 */
int         gkb_source_is_sd(void);
const char *gkb_sd_path(void);

/* 游戏数 / 游戏显示名（UI 选游戏列表用）。id 越界返回 ""。 */
int         gkb_game_count(void);
const char *gkb_game_name(int id);

/*
 * **编译期**的游戏数。gkb_game_count() 是函数，sizeof 里用不了，
 * 而"UI 那张按游戏索引的定长表有多少行"必须在编译期就能断言 ——
 * 历史上正是这��表脱节过（19 行 vs 31 款），越界读合法元素，
 * gcc 一声不吭，设备上表现为「那款游戏下面什么词都没有」。
 *
 * 这里的值必须与 guide_kb.c 里的 g_game_names 行数一致，
 * 并与 build_kb.py 按 sorted(corpus) 分配的 game_id 范围一致。
 */
#define GKB_GAME_N 211


/*
 * 拆游戏名。
 *   query 问句；out 剩余问句（需 strlen(query)+2 字节）
 *   返回 game_id（-1 = 没认出游戏），此时 out == query 原文。
 * 最长匹配优先，避免「宝可梦」抢走「宝可梦太阳」。
 */
int  gkb_split_game(const char *query, char *out, int out_sz);

/*
 * 检索。query 应当是**已剔除游戏名**的剩余问句（见 gkb_split_game）。
 * game_filter >= 0 时只在该游戏内检索（-1 = 全部）。
 * 返回命中数（<= GKB_HIT）。
 *
 * 返回 0 不一定是 bug：词表没加载时 BPE 编码失败（见 nnet_text.c），
 * 或者查询的每个 token 都落在停用词表里。调用方要能区分「没命中」
 * 与「检索根本跑不起来」，所以另有 gkb_query_tokens 可查 token 数。
 *
 * 栈占用约 20KB（score[4096] + hitc[4096]）。本工程把 app 整个搬进了
 * 256KB 的工作线程栈（见 app/source/main.c），所以这里是安全的；
 * 拿它去跑在 3dsx 默认的 16KB 主线程栈上会直接爆。
 */
int  gkb_search(const Gkb *kb, const char *query, int game_filter,
                GkbHit *out, int max_out);

/* 查询 -> token id（停用词过滤之后）。返回 token 数，0 = 没有可用的 token。 */
int  gkb_query_tokens(const Gkb *kb, const char *query,
                      int32_t *toks, int max_tok);

/* ---- 输出层候选集 ---------------------------------------------------
 *
 * 把 top-n 命中段落里出现过的 token 收成去重集合，交给
 * nnet_set_candidates() 把输出层从全词表 4096 缩到这一小撮。
 * 调用方在 nnet_push 之前设置，**不要**在生成循环里反复重设。
 *
 * 为什么需要：输出层是 tok_emb 复用（tied），形状 [4096][128]，
 * 单这一层就占单 token 总算力的 43%（524288 / 1212416 MAC），
 * 比 4 层主干（655360）还接近。而 4096 是为通用文本设计的词表，
 * 这个模型只会吐语料里出现过的东西。
 *
 * 实测规模（tools/guia/cand_probe.py，12 条用例）：
 *   top-1 段落 token 数 37 / 57 / 95（min/中位/max）
 *   top-4 段落并集      57 / 169 / 248
 *   → 输出层省 96.3%，单 token 总算力省 41.6%
 *
 * 附带收益更重要：**模型没法说出命中原文里没有的字**，胡编从
 * 「靠提示词去求」变成结构性保证。
 *
 * 三个容易踩的点，实现里都处理了（改动前请先读）：
 *   1. kb->text 里的段落**末尾没有 NUL**，必须按 tok_len 显式拷贝。
 *   2. UTF-8 不能腰斩，且对齐判据要看**源**字节。
 *   3. **EOS 必须在候选集里** —— 模型靠采到 EOS 才知道停。
 *      漏了它会一路采到 max_gen 上限，屏上多出一串胡话。
 *
 * 返回写入的候选数（已去重，含 BOS/EOS）。0 = 一条可用候选都没有，
 * 此时上层应当**不设候选集**（走全词表）而不是传空 ——
 * 「不限制」和「限制成空集」在 nnet_push 里是两件完全不同的事：
 * 前者能生成，后者采样空间为空会一路撞上限。
 *
 * max_out 传 GKB_CAND_MAX 即可；返回值不会超过它。
 */
int  gkb_candidate_tokens(const Gkb *kb, const GkbHit *hits, int n_hit,
                          int32_t *out, int max_out);

#ifdef __cplusplus
}
#endif
#endif /* GUIDE_KB_H */
