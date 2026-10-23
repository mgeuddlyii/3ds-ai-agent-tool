/*
 * hwtest.c -- 见 hwtest.h
 *
 * 口径与其它探针一致：**每条结论都要能被独立证据支持**。
 * 所以这里不打"硬件知识库 OK"这种话，而是打出
 * 「问句 -> 命中了哪一条（问句原文）-> 覆盖率多少 -> 答案开头几个字」，
 * 三样都能在 status.txt 里被人逐条核对。
 *
 * ⚠️ PASS/FAIL 计数由 ck() 自己维护，**不是让验收脚本去数 notes 里的行**。
 * 原因是 notes 有 6144 字节上限，而这里 40 来条用例的记录很可能填满它；
 * 一旦填满，后面的行被静默丢掉（缓冲区满的现象和崩溃一模一样，
 * 后面的行凭空消失），而汇总那行写在最后反而保住了 ——
 * 于是脚本数出来是"全过"，实际上一半用例根本没跑。
 * 计数必须由写记录的函数自己做，不能由读记录的脚本推。
 */

#include "hwtest.h"

#include "app.h"
#include "hwk.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static int g_pass, g_fail;

/* 记一条结论。消息由调用方拼好，这里只决定前缀并计数。 */
static void ck(bool ok, const char *fmt, ...)
{
    va_list ap;
    char    msg[176];

    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);

    if (ok) g_pass++; else g_fail++;
    app_note("hwtest: %s %s", ok ? "PASS" : "FAIL", msg);
}

/* 期望命中的用例。
 *
 * `expect` 是那条**问句**的原文（hw_knowledge.txt 的问句列），
 * 不是答案里的关键词 —— 用关键词做断言会引入一个真实的歧义：
 * 「为什么放弃线程后要把句柄置空」和「线程放弃能带超时吗」
 * 两条的答案里都出现"句柄"，而它们是不同条目。
 * 按问句全文比，才能确定命中的确实是那一条。 */
typedef struct {
    const char *q;        /* 用户会怎么问 */
    const char *expect;   /* 期望命中的那条的问句原文 */
} HwCase;

static const HwCase g_cases[] = {
    { "3ds能超频吗",                 "3ds能超频吗" },
    { "3ds的cpu是几核",              "3ds的cpu是几核" },
    { "3ds的gpu能用来算东西吗",      "3ds的gpu能用来算东西吗" },
    { "3ds的dsp能用吗",              "3ds的dsp能用吗" },
    { "3ds的时钟单位是什么",         "3ds的时钟单位是什么" },
    { "3ds的缓存有多大",             "3ds的缓存有多大" },
    { "3ds支持simd吗",               "3ds支持simd吗" },
    { "3ds支持双精度吗",             "3ds支持双精度吗" },
    { "3ds有硬件乘加指令吗",         "3ds有硬件乘加指令吗" },
    /* 故意换个说法（多几个字/换词序），验它不是靠死记硬背整句 */
    { "请问3ds能不能超频呢",         "3ds能超频吗" },
    { "3ds cpu 有几个核心",          "3ds的cpu是几核" },
    { "3ds 缓存 容量",               "3ds的缓存有多大" },
};

#define CASE_N ((int)(sizeof(g_cases) / sizeof(g_cases[0])))

/* 答案里必须出现的关键片段。
 *
 * ⚠️ 片段长度必须 >= 4 字节。2~3 字节的中文片段容易在
 * 「答案被 UTF-8 边界截断」时也刚好匹配上，
 * 那样这条断言就变成了永远绿 —— 永远绿的守卫比没有更坏。 */
typedef struct {
    const char *q;        /* 问句 */
    const char *needle;   /* 答案里该出现的片段 */
} HwContent;

static const HwContent g_content[] = {
    { "3ds的cpu是几核",         "core0" },
    { "3ds能超频吗",             "804MHz" },
    { "3ds的gpu能用来算东西吗", "PICA200" },
    { "3ds支持simd吗",           "NEON" },
    { "3ds的时钟单位是什么",     "268MHz" },
    { "3ds的缓存有多大",         "512KB" },
    { "3ds支持双精度吗",         "f64" },
};

#define CONTENT_N ((int)(sizeof(g_content) / sizeof(g_content[0])))

static void check_cases(void)
{
    HwkResult r;
    int i;

    app_note("hwtest: ===== 检索：问句 -> 命中哪条 =====");

    for (i = 0; i < CASE_N; i++) {
        memset(&r, 0, sizeof r);
        r.hit = -1;
        hwk_search(g_cases[i].q, &r);

        if (r.hit < 0) {
            /* 没命中**本身是合法结果**（问得太短或用词太偏），
             * 但对下面这批精心挑的问句它不该发生，
             * 所以这里记 FAIL 而不是跳过 ——
             * 跳过会让"覆盖率门槛被调太高"这种回归完全看不出来。 */
            ck(false, "问「%s」没有任何命中", g_cases[i].q);
            continue;
        }
        ck(strcmp(r.ask, g_cases[i].expect) == 0,
           "问「%s」-> 命中「%s」 覆盖 %d%%%s",
           g_cases[i].q, r.ask, r.score,
           strcmp(r.ask, g_cases[i].expect) == 0 ? ""
                                                 : "（期望的其实是另一条）");
    }
}

/* 反向用例：**不该有命中**。
 *
 * 这条比正向用例更重要。正向全绿只说明"能对上"，
 * 反向才是"没有乱答" —— 用户问一句游戏攻略却拿到一条硬件知识，
 * 看起来完全像个答案，比"没找到"糟得多。 */
static void check_negative(void)
{
    static const char *const noise[] = {
        "火焰纹章觉醒转职怎么转",
        "大师之剑在哪里拿",
        "马力欧卡丁车7的赛道有几条",
        "宝可梦绿宝石的精灵球",
        "雷曼的疯狂兔子怎么玩",
        "道具怎么用",
        "沙之杖在哪里拿",
        "火焰纹章觉醒 值得入手吗",
    };
    HwkResult r;
    unsigned i;

    app_note("hwtest: ===== 反向：游戏问句不该命中硬件知识 =====");

    for (i = 0; i < sizeof(noise) / sizeof(noise[0]); i++) {
        memset(&r, 0, sizeof r);
        r.hit = -1;
        hwk_search(noise[i], &r);
        if (r.hit < 0)
            ck(true, "游戏问句「%s」没命中硬件知识", noise[i]);
        else
            ck(false, "游戏问句「%s」命中了「%s」（%d%%）",
               noise[i], r.ask, r.score);
    }
}

/* 答案内容核对：命中的那条**答对了内容**。
 *
 * 为什么要单独查：命中判定只看"问的是哪一条"，
 * 而答串可能因为缓冲区/对齐/编码出问题变成乱码 —— 那时命中判定
 * 依然全绿。所以必须另查一遍答案里有没有可辨认的关键片段。 */
static void check_content(void)
{
    HwkResult r;
    int i;

    app_note("hwtest: ===== 答案内容核对（不是只看命中了哪条）=====");

    for (i = 0; i < CONTENT_N; i++) {
        memset(&r, 0, sizeof r);
        r.hit = -1;
        hwk_search(g_content[i].q, &r);
        if (r.hit < 0) {
            ck(false, "问「%s」没命中，验不了内容", g_content[i].q);
            continue;
        }
        ck(r.best[0] && strstr(r.best, g_content[i].needle) != NULL,
           "问「%s」答案含「%s」 答案开头=「%.24s」",
           g_content[i].q, g_content[i].needle, r.best);
    }
}

/* 短问句必须返回"没找到"而不是"勉强找一条"。
 *
 * bigram 至少要 **2 个**（3 个字符）才检索。1 个 bigram 时覆盖率
 * 要么 0 要么 100%，而 100% 等于「那条问句里有这两个字就算命中」——
 * 「3ds」在半数条目里都有，于是问「3ds」会随机命中一条。
 * 答非所问比答不出来糟得多。
 *
 * ⚠️ 这一组第一次写成"2 字问句（「几核」）不该命中"，
 * 而「几核」其实是 1 个 bigram → 判据成立。
 * 但当时我把它归因成"4 字节以下直接 return"，
 * 于是**测试通过、原因错了**。后来真正的原因是分母算错
 * （bigram 步长 2 导致 1/4 = 25% > 20% 门槛），
 * 而那才是会让「大师之剑在哪里拿」误命中的那个 bug。
 * → 教训：断言对了不代表对的原因对了，要单独看**失败时的报法**。 */
static void check_short(void)
{
    HwkResult r;

    app_note("hwtest: ===== 边界：太短的问句 =====");

    /* 1 个 bigram（2 个字符）。"几核"看起来该能答，但它只有 1 个
     * bigram，在这条判据下就是查不动 —— 这是本方案的真实边界。 */
    memset(&r, 0, sizeof r);
    r.hit = -1;
    hwk_search("几核", &r);
    ck(r.hit < 0, "2 字问句「几核」（1 个 bigram）返回「没找到」");

    /* 「3ds」是这条判据的**由来**：它在半数条目里都有，
     * 而用户问「3ds」时想要的大概不是随机那一条。 */
    memset(&r, 0, sizeof r);
    r.hit = -1;
    hwk_search("3ds", &r);
    ck(r.hit < 0, "「3ds」返回「没找到」（否则会随机命中半数条目之一）");

    /* 1 个字符：连 bigram 都凑不出一个。 */
    memset(&r, 0, sizeof r);
    r.hit = -1;
    hwk_search("3", &r);
    ck(r.hit < 0, "1 字问句「3」返回「没找到」");

    /* 3 个字符（2 个 bigram）必须**能**查到 —— 确认下限不是设过头。
     * 只测上限不测下限的话，把下限写成 99 个 bigram 也能全绿。 */
    memset(&r, 0, sizeof r);
    r.hit = -1;
    hwk_search("能超频吗", &r);
    ck(r.hit >= 0 && strcmp(r.ask, "3ds能超频吗") == 0,
       "3 字问句「能超频吗」能查到（2 个 bigram，够用了）");
}

/* 空指针与越界的防御。
 * 这一组不算功能，但它是唯一能在真机上崩掉我们的那一组。 */
static void check_defensive(void)
{
    HwkResult r;

    app_note("hwtest: ===== 防御：空指针 / 越界 =====");

    memset(&r, 0, sizeof r);
    r.hit = -1;
    hwk_search(NULL, &r);
    ck(r.hit < 0, "hwk_search(NULL) 返回没找到而不是崩");

    /* out 传 NULL：文档里说"会崩"吗没说，但一个只读的检索函数
     * 因为查询结果没地方放就崩，是很难接受的。 */
    hwk_search("3ds能超频吗", NULL);
    ck(true, "hwk_search 结果指针为 NULL 时安全返回");
}

void hwtest_run(void)
{
    int n;

    g_pass = 0;
    g_fail = 0;

    app_note("hwtest: ===== 开始 =====");

    n = hwk_load();
    if (n <= 0) {
        /* 这一条挂了后面全部免谈 —— 不是"跳过"，
         * 因为下面每一条都建立在"库加载成功"之上。 */
        app_note("hwtest: FAIL hwk_load 失败 rc=%d（内嵌那份也拿不到）", n);
        app_note("hwtest: RESULT=FAIL pass=0 fail=1");
        app_note("hwtest: ===== 结束 =====");
        return;
    }
    app_note("hwtest: PASS hwk_load ok 条目=%d 来源=%s（1=SD卡 0=内嵌）", n,
             hwk_from_sd() ? "SD卡" : "内嵌");

    /* 这一趟刻意**不做** cupdtest 那种 cu_set_root 隔离 ——
     * 硬件知识库要验的就是"内嵌那份能不能用"，
     * 而隔离之后 SD 卡上那份会覆盖内嵌，
     * 于是"内嵌能不能用"这个问题反而验不到。
     * 代价是它**只读**，不写任何文件（hwk.c 里没有落盘路径）。 */
    if (hwk_from_sd())
        app_note("hwtest: 注意 卡上有 hw_knowledge.txt，这一趟验的是 SD 那份；"
                 "内嵌那份要等把卡上文件挪走才验得到");

    check_cases();
    check_negative();
    check_content();
    check_short();
    check_defensive();

    app_note("hwtest: RESULT=%s pass=%d fail=%d",
             g_fail == 0 ? "PASS" : "FAIL", g_pass, g_fail);
    app_note("hwtest: ===== 结束 =====");
}
