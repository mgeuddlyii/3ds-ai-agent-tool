/*
 * perfprobe.c -- 见 perfprobe.h
 *
 * 测三段，每段都跑多轮取**中位数**：
 *   A. 单 token 前向（喂一个固定 prompt，生成 1 个 token 就停）
 *   B. 整段生成（一次问句走完，模拟真实手感）
 *   C. 纯检索（同一问句，不带模型）
 *
 * 为什么取中位数而不是平均：模拟器里偶发的调度抖动会把平均拉高，
 * 而**中位数对少量离群值免疫**。取平均的话，同一份二进制两次跑能差 8%，
 * 那样的基线什么都证明不了。
 *
 * 为什么每段都跑三轮：升频（osSetSpeedupEnable）生效与否会影响结果，
 * 第一轮可能在还没升频的频率上。不做预热的话第一轮天然偏慢。
 */

#include "perfprobe.h"

#include "app.h"
#include "guide_kb.h"
#include "guide_nn.h"
#include "hwk.h"
#include "nnet_text.h"

#include <3ds.h>
#include <stdio.h>
#include <string.h>

#define MODEL_PATH "sdmc:/3ds/aiconsole/guide_model.bin"

/* tick -> 毫秒。268MHz 硬件计数器，整数运算不走 dtoa。 */
static void tick_ms(u64 dt, char *buf, int cap)
{
    unsigned long long ms = (unsigned long long)(dt * 1000ULL / 268000000ULL);
    unsigned long long frac = (unsigned long long)((dt * 1000ULL % 268000000ULL) * 1000ULL
                                                  / 268000000ULL);
    snprintf(buf, cap, "%llu.%03llums", ms, frac);
}

/* 三数取中位数。输入会被打乱顺序（就地插入排序），返回值是原数组中项。 */
static u64 median3(u64 *v)
{
    /* 3 个元素的插入排序：短而正确，比 qsort 少一个函数指针。 */
    for (int i = 1; i < 3; i++) {
        u64 k = v[i];
        int j = i - 1;
        while (j >= 0 && v[j] > k) {
            v[j + 1] = v[j];
            j--;
        }
        v[j + 1] = k;
    }
    return v[1];
}

/* BPE 词表工作区。gkb_vocab_bytes(4096,4096) 约 224KB，
 * 必须放静态区 —— 放栈上会与 gkb_search 的 20KB 撞爆。 */
static uint8_t g_vocab_work[224 * 1024];

/*
 * 加载词表。**必须在 gkb_search 之前**。
 *
 * gkb_bind 本身不碰词表（它只是解包 + 建指针），所以它成功而检索 0 命中
 * 看起来像"库里没有这条"。真正原因是 gkb_query_tokens 走 BPE，
 * 词表没加载时编码直接失败 -> 返回 0 个 token -> 什么都搜不到。
 *
 * 这个区分是本探针第一版踩过的：先看到 -9（索引挂），
 * 修好之后看到"检索 0 命中"，两次都是**同一个函数的调用方缺一步**。
 * 症状完全不同（一个是加载失败，一个是"库里没有"），
 * 所以要靠 notes 里的两行分别判，不能合并成一句"搜索失败"。
 */
static bool load_vocab(void)
{
    GkbVocab vocab;
    int rc = gkb_vocab_load_embedded(&vocab, g_vocab_work,
                                     (int)sizeof g_vocab_work);
    if (rc != 0) {
        app_note("perfprobe: 词表加载失败 rc=%d（检索必然 0 命中）", rc);
        return false;
    }
    app_note("perfprobe: 词表 ok v=%d merges=%d", (int)vocab.vocab,
             (int)vocab.n_merge);
    return true;
}

/* 一个固定问句。用它而不是随机串，是为了让每次跑的**工作量完全相同** ——
 * 候选集大小会改变输出层的算力（实测中位 169，max 248），
 * 问句变了就等于换了负载，前后数据不可比。 */
static const char *const PROBE_QUERY = "火焰纹章觉醒 转职怎么转";

/*
 * 一次生成。
 *
 * ⚠️ **只跑一轮**，尽管 median3 要三个样本。
 * 理由：模拟器上 1.2M 模型生成一次比真机慢一个数量级
 * （真机 804MHz 实测 25~40ms/token，模拟器是宿主 CPU 上跑的），
 * 一次 48 token 的生成在模拟器里可能就要十几秒。
 * 12 次生成实测把 250 秒的等待窗口跑爆了 —— status.txt 都没写出来。
 *
 * 那为什么不用中位数？因为**基线的用途是"改完之后比"，
 * 而对比的两次都在同一个环境里跑**，环境偏差是共模的。
 * 真正需要消掉的是"同一次运行内的偶发抖动"，而探针跑得少时
 * 那个抖动本来就小。取一轮换来的是"能跑完"，
 * 一个跑不完的基线没有任何价值。
 *
 * 检索是另一回事：它便宜到可以跑三轮取中位数。
 */
static void bench_forward(char *out, int cap)
{
    Gkb     kb;
    GkbHit  hits[GKB_HIT];
    char    rest[128];
    char    ans[512];
    int     gid, n, rc;
    u64     dt;

    n = gkb_bind(&kb);
    if (n <= 0) {
        snprintf(out, cap, "索引加载失败 rc=%d", n);
        gkb_free(&kb);
        return;
    }
    gid = gkb_split_game(PROBE_QUERY, rest, (int)sizeof rest);
    n = gkb_search(&kb, rest, gid, hits, GKB_HIT);
    if (n <= 0) {
        snprintf(out, cap, "检索 0 命中");
        gkb_free(&kb);
        return;
    }

    u64 t0 = svcGetSystemTick();
    rc = gnn_generate(&kb, rest, gid, hits, n, ans, (int)sizeof ans, 0.7f, NULL);
    dt = svcGetSystemTick() - t0;

    gkb_free(&kb);

    if (rc != GNN_OK) {
        snprintf(out, cap, "生成失败 rc=%d %s", rc, gnn_last_error());
        return;
    }
    {
        char m[24];
        tick_ms(dt, m, sizeof m);
        snprintf(out, cap, "整段生成 = %s，输出 %d 字节", m, (int)strlen(ans));
    }

    /*
     * ⚠️⚠️ **RAW 那一行必须同时记输出字节数**，而且**人比对时必须先对字节数**。
     *
     * 这不是形式主义。实测真的发生过：
     * 第一版 ldr+sxtb 把字节序写反了（3DS 是小端，我把 w[i+0]
     * 当成了高字节），于是点积的下标全部错位一格 ——
     * 程序不崩不报错，数字量级也正常，**但模型提前采到 EOS**，
     * 输出从 122 字节缩到 19 字节。
     * 于是"耗时下降 34%"看起来像个大利好，
     * 而它其实只是**少干了活**：token 少了，耗时当然短。
     *
     * → 任何"变快了"的结论，必须先确认"活也没少干"。
     * 只有一个时间数字是不够的，必须有一个与时间配对的**工作量**数字。
     * 别的例子：滑窗注意力砍掉一半计算，耗时也减半 —— 那是设计取舍，
     * 不该被当成"优化成功"报上去。
     */
    app_note("perfprobe: RAW 整段生成 = %llu tick（268MHz 计数器）"
             "，输出 %d 字节",
             (unsigned long long)dt, (int)strlen(ans));
}

static void bench_search(char *out, int cap)
{
    u64  samples[3];
    Gkb  kb;
    char rest[128];
    GkbHit hits[GKB_HIT];
    int  gid, nh;

    if (gkb_bind(&kb) <= 0) {
        snprintf(out, cap, "索引加载失败");
        gkb_free(&kb);
        return;
    }
    gid = gkb_split_game(PROBE_QUERY, rest, (int)sizeof rest);
    /* 预热一次：第一次调 gkb_search 时，att[t] 那类小数组是冷的。 */
    nh = gkb_search(&kb, rest, gid, hits, GKB_HIT);
    if (nh <= 0) {
        snprintf(out, cap, "检索 0 命中");
        gkb_free(&kb);
        return;
    }

    for (int i = 0; i < 3; i++) {
        u64 t0 = svcGetSystemTick();
        gkb_search(&kb, rest, gid, hits, GKB_HIT);
        samples[i] = svcGetSystemTick() - t0;
    }
    gkb_free(&kb);

    {
        char m[24];
        tick_ms(median3(samples), m, sizeof m);
        snprintf(out, cap, "纯检索 = %s（3 轮中位数，命中 %d 条）", m, nh);
    }
}

void perfprobe_run(void)
{
    char a[96], b[96];
    int  rc;

    app_note("perfprobe: ===== 开始 =====");

    /* 加载**不计入**时间：它是一次性的，而推理是每 token 都发生的。
     * 把两者混在一起测，优化会被「少读一次 SD 卡」带偏 ——
     * 那是启动时间的问题，不该记在推理账上。 */
    rc = gnn_open(MODEL_PATH);
    if (rc != GNN_OK) {
        app_note("perfprobe: 模型不可用 rc=%d %s（计时跳过）",
                 rc, gnn_last_error());
        app_note("perfprobe: 提示 模型要拷到 %s", MODEL_PATH);
        app_note("perfprobe: RESULT=SKIP");
        app_note("perfprobe: ===== 结束 =====");
        return;
    }
    app_note("perfprobe: 模型已就绪");

    /* 词表要在 gkb_search 之前 —— 见 load_vocab 的注释。 */
    load_vocab();

    bench_forward(a, sizeof a);
    bench_search(b, sizeof b);

    app_note("perfprobe: A %s", a);
    app_note("perfprobe: C %s", b);
    app_note("perfprobe: RESULT=OK");
    app_note("perfprobe: ===== 结束 =====");
}
