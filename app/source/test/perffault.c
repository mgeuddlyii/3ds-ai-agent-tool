/*
 * perffault.c -- 故障注入验收：证明「坏情况下只会降级，不会崩」
 *
 * ══ 这个文件为什么存在 ══════════════════════════════════════════════
 *
 * perf_lvl.c 的设计约束全是否定式的：「失败静默」「带硬超时」
 * 「绝不 free 一个还在跑的线程」。但这些约束**写在注释里不等于成立** ——
 * 上一轮就真的藏了一个必炸的写法（perf_pick_core 丢弃 threadJoin
 * 的返回值就 free，探针没被调度时就是 use-after-free）。
 * 那个 bug 是「读代码读出来的」，不是「测出来的」——
 * 而读代码只能发现你想到的那一个。
 *
 * 所以这里把每一条约束都变成**一个能主动触发的场景**，
 * 让「不崩」有实测支撑，而不是靠注释担保。
 *
 * ══ 三条设计原则 ═══════════════════════════════════════════════════
 *
 * ① **只注入「无法在真机上制造」的故障。**
 *    内存不足、线程卡死、重复调用这些都能真造（见下面各场景），
 *    就不要用桩去模拟 —— 桩只能证明桩自己是对的。
 *
 * ② **判据必须是「还活着」，不是「结果正确」。**
 *    每个场景跑完都必须能继续执行下一个 app_note()。
 *    一旦某个场景把进程带崩，status.txt 里后面的行就全都不在了 ——
 *    这本身就是判据（宿主脚本按行数与末行内容断言）。
 *
 * ③ **每个场景都要能单独触发。**
 *    崩一个不该连累别的。autorun 的 `perffault <名>` 就是这个粒度。
 *
 * ══ 关于"能不能造出真故障"的一处硬事实 ════════════════════════════
 *
 * threadCreate 失败时**返回 NULL，不 abort**（已反汇编确认：
 * memalign 失败 -> beq 到 mov r4,#0；svcCreateThread < 0 -> free 后
 * 同样返回 0）。所以「申请一个荒谬的栈大小」就能真实触发
 * threadCreate 失败 —— 不需要任何桩。
 * 这一条让场景 oom / bigstack 成为**真故障**而非模拟。
 */

#include "perffault.h"

#include "app.h"
#include "perf_lvl.h"

#include <3ds.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

/* ── tick 换算（与 perf_lvl.c 内部一致）──────────────────────────────
 * 这里独立写一份而不是去 include perf_lvl.c 的内部宏 ——
 * 那个宏是 static 的、且刻意不做成对外接口。
 *
 * ⚠️⚠️ **系数是 268000000（268 MHz），不是 268000，更不是 60。**
 *   这个数我错了两次：
 *     ① 以为 system tick = 60Hz            -> 差 4400 倍
 *     ② 改成 268000 并注释"268 kHz"        -> 还是差 1000 倍
 *        （268 是 tick/微秒，不是 tick/毫秒）
 *   两条互相独立的路径交叉验证（见 ruler A 段与 D 段的对比）：
 *       sleep 100ms  -> 26,812,488 tick / 100ms = 268,125 tick/ms
 *       join(600ms)  -> 160,896,234 tick / 600ms = 268,160 tick/ms
 *   差 0.013%，所以 268 MHz 是真的。
 *
 *   两处各写一份的代价就是这种漂移，所以硬约束是：
 *   **若哪天又测出别的系数，perf_lvl.c 的 PERF_TICK_HZ 和这里都要改。** */
#define PF_TICK_HZ          268000000ull
#define PF_TICKS_PER_MS     (PF_TICK_HZ / 1000ull)
#define PF_MS2TICKS(ms)     ((u64)((ms) * PF_TICKS_PER_MS))
#define PF_TICKS2MS(t)      ((u64)((t) / PF_TICKS_PER_MS))
#define PF_MSREM(t)         ((u64)(((t) % PF_TICKS_PER_MS) * 1000ull \
                                      / PF_TICKS_PER_MS))

/* ══ 工具 ═══════════════════════════════════════════════════════════ */

/* 场景之间共用的收尾：把这一趟的结论记进 status.txt。
 * 它同时是「我还活着」的证据 —— 崩了的场景不会有这一行。
 *
 * ⚠️ buf 是 192 字节时，本文件里最长的一行（hang 的耗时那条，实测 126 字节）
 *    正好被截断，表现为「记录里有 join=-2 但没有耗时字段」。
 *    宿主脚本据此判"没有耗时字段 = 没超时"，把一次**成功的超时**
 *    误判成失败 —— 比没有这条记录更坏，因为它看起来像有结论。
 *    → 现在给 256，并且下面每条消息都刻意控制在 ~140 字节内。
 *      中文按 UTF-8 每字 3 字节算，一行 40 个汉字就 120 字节了。
 */
static void pf_note(const char *fmt, ...)
{
    char    buf[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);

    /* 截断是**静默**的：vsnprintf 不告诉你它丢了尾巴。
     * 而宿主脚本会把"字段缺失"读成"结论相反"。
     * 所以这里自己判一次 —— 一旦截了就留一个显式标记，
     * 让"证据被截断"和"结论为否"在文件里可区分。 */
    if (strlen(buf) >= sizeof buf - 1)
        snprintf(buf + sizeof buf - 24, 24, " ...[截断!见源码]");

    app_note("perffault: %s", buf);
}

/* worker 的行为模式。必须在 pf_worker 之前定义。 */
enum {
    PF_MODE_NORMAL = 0,   /* 立刻返回，线程正常结束 */
    PF_MODE_SPIN   = 1,   /* 死循环，join 必然超时 */
};

/* 一个会在**别的核**上跑的、可控的 worker。
 * 行为由 g_pf_mode 决定，参数原样回写（验证参数确实传到了）。 */
static volatile int g_pf_mode;
static volatile int g_pf_ran;
static int          g_pf_arg_seen;

static void pf_worker(void *arg)
{
    g_pf_arg_seen = *(int *)arg;
    g_pf_ran      = 1;

    if (g_pf_mode == PF_MODE_SPIN) {
        /* 死循环。这是最恶劣的情况：join 必然超时。
         * 它同时也在核上抢 CPU —— 所以这个场景跑完必须确保那个线程
         * 还在跑（我们故意不 join 成功），但也**只能**这样：
         * 我们没有安全的方式去杀掉一个已经启动的线程。
         *
         * ⚠️ svcSleepThread 的单位是**纳秒**（svc.h 的注释：
         * "@param ns The minimum number of nanoseconds to sleep for"）。
         * 我第一版在这里传了 PF_MS2TICKS(50) = 3 —— 那是 3 **纳秒**，
         * 等于不让出，把一个"死循环"写成了"忙等"。
         * 3ns 连一次线程切换都不到，于是它把整个核钉死在一个
         * 极紧的自旋里 —— 那不是我要测的东西。
         * 1ms 才是"占着一个核但让出"的真实卡死。 */
        for (;;)
            svcSleepThread(1000000ll);   /* 1ms */
    }
    /* 其他模式：直接返回，线程正常结束。 */
}

/* ══ 场景 0：量尺（其它所有场景的基准）══════════════════════════
 *
 * 写在最前面，因为它测的是**这台机器的时基**本身。
 *
 * 起因：perf_lvl.c 里所有超时都经过 MS2TICKS() 换算，而那个换算的
 * 前提是「system tick = 60Hz」。这个前提是**猜的**，而且是错的
 * —— 见下面实测出来的数字。
 *
 * ⚠️ 本场景在修复前后的**行为不同**，这不是 bug，是修复的结果。
 *   修复前：拿 threadJoin / svcWaitSynchronization 当量尺，
 *           于是量到的是"这两个 API 的 timeout 到底怎么解释"。
 *   修复后：perf_wait_worker 是纯 volatile 轮询，20ms 一步。
 *           所以 B 段量到的**必然 >= 20ms** —— 那是轮询的量化误差，
 *           不是 timeout 的解释。
 *   → A 段是唯一还有意义的时基测量，C/D 段保留了"为什么不能那么写"的证据。
 */
static void pf_scn_ruler(void)
{
    /* ---- A. sleep 纳秒数 -> tick 数（**本场景唯一有效的测量**）----
     *
     * 三档差了 100 倍，如果换算是错的，三行不会收敛到同一个值。
     *
     * ⚠️ 输出的是 **tick/微秒**（`dt / (ns/1000)`）。
     *    上一版这里写的是 `dt*1000/ns` 并标成 "tick/ms" —— 那个式子
     *    算出来的其实是 tick/微秒，标签错了 1000 倍，于是把
     *    268 MHz 读成了 268 kHz，还顺手把 perf_lvl.c 的常数带歪了。
     *    现在两个单位都打出来，让标签和数字互相盯着 ——
     *    标签再错一次，一眼就能看出来（两者差 1000 倍）。
     *
     * 实测（Azahar）：
     *     sleep 1ms   = 1,000,000 ns  ->    269,414 tick  => 269 tick/us
     *     sleep 10ms  =10,000,000 ns  ->  2,682,421 tick  => 268 tick/us
     *     sleep 100ms =100,000,000 ns -> 26,812,488 tick  => 268 tick/us
     * → tick 频率 ≈ 268 MHz（不是 60Hz，差 4400 倍）。
     */
    struct { u64 ns; const char *what; } cases[] = {
        { 1000000ull,   "1ms"   },
        { 10000000ull,  "10ms"  },
        { 100000000ull, "100ms" },
    };
    for (unsigned i = 0; i < sizeof cases / sizeof *cases; ++i) {
        u64 t0 = svcGetSystemTick();
        svcSleepThread(cases[i].ns);
        u64 dt = svcGetSystemTick() - t0;
        u64 per_us = dt / (cases[i].ns / 1000ull);   /* tick / 微秒 */
        u64 per_ms = dt * 1000000ull / cases[i].ns;  /* tick / 毫秒 */
        pf_note("scn=ruler sleep %-5s = %10llu ns -> %9llu tick"
                "  => %llu tick/us = %llu tick/ms",
                cases[i].what, (unsigned long long)cases[i].ns,
                (unsigned long long)dt,
                (unsigned long long)per_us, (unsigned long long)per_ms);
    }

    /* ---- B. 轮询等待的量化误差 --------------------------------------
     *
     * perf_wait_worker 每 PERF_WAIT_STEP_MS(20ms) 醒一次。
     * 所以哪怕 timeout 给 1ms，实际也会睡满一步 —— **这是设计如此**：
     * 一个"带超时"却只能精确到 20ms 的等待，在插件里完全够用
     * （我们等的对象是毫秒级的 matvec）。
     *
     * 这里量出来是为了**把量化误差写进日志**：以后如果哪天有人把
     * PERF_WAIT_STEP_MS 调小，这里会立刻显示耗时跟着变，
     * 而不是变成一个" mysteriously 总是 20ms"的常量。
     *
     * 对照组：一个还没跑完的线程（超时路径），见 D 段。 */
    {
        int m = 0x7A7A;
        g_pf_mode = PF_MODE_NORMAL;
        g_pf_ran  = 0;

        u64 t0 = svcGetSystemTick();
        int rc = perf_worker_start(pf_worker, &m, 16384);
        u64 t1 = svcGetSystemTick();
        pf_note("scn=ruler 起一个立刻返回的 worker: start=%d 建线程耗时=%llu tick",
                rc, (unsigned long long)(t1 - t0));
        if (rc) {
            u64 t2 = svcGetSystemTick();
            int jr = perf_worker_join(1);      /* 1 毫秒 */
            u64 t3 = svcGetSystemTick();
            pf_note("scn=ruler join(timeout=1ms) rc=%d 耗时=%llu tick"
                    "（轮询步长 20ms，所以这值必然 >= %llu tick，"
                    "即量到的是量化误差不是 timeout）",
                    jr, (unsigned long long)(t3 - t2),
                    (unsigned long long)PF_MS2TICKS(20));
        }
    }

    /* ---- C. 一个巨大的 timeout 不会变成"真的等很久" ----------------
     *
     * 旧实现里 0xFFFFFFFF 被当成 tick 传进 threadJoin，
     * 折算出来是 248 天。现在它是毫秒（4294967295ms ≈ 49 天），
     * 而 worker 早已结束，所以轮询一次就该返回。
     *
     * 验的是：**timeout 再离谱，也不会让"已完成"变成"等很久"** ——
     * 因为判据是 g_worker_done（一个 volatile int），
     * 不是"时间到了没有"。这是刻意的设计：
     * 时间只用来判超时，不用来判完成。
     */
    {
        int m2 = 0x7B7B;
        g_pf_mode = PF_MODE_NORMAL;
        g_pf_ran  = 0;
        int rc = perf_worker_start(pf_worker, &m2, 16384);
        pf_note("scn=ruler 大 timeout 档: start=%d", rc);
        if (rc) {
            u64 t0 = svcGetSystemTick();
            int jr = perf_worker_join(0xFFFFFFFFu);  /* 32 位全 1，非 U64_MAX */
            u64 dt = svcGetSystemTick() - t0;
            pf_note("scn=ruler join(0xFFFFFFFF) rc=%d 耗时=%llu tick"
                    "（若这值很小，说明判据是完成标志而不是时间，"
                    "符合设计；旧实现会在这里耗掉 248 天或直接挂死）",
                    jr, (unsigned long long)dt);
        }
    }

    /* ---- D. 死循环线程：验证"超时即放弃"真的发生 -------------------
     *
     * 这是整个文件的核心。上面 C 段里一个**已结束**的线程 join 成功了，
     * 本身什么都不能证明 —— 因为对一个已经结束的线程，
     * "成功"是唯一合理的结果。
     *
     * 真正能区分的是**永远不会结束**的线程：
     *   · 如果等待机制可信 -> 在 timeout 内返回 -2（超时），
     *     句柄被置空、线程被放弃，进程继续活着。
     *   · 如果等待机制不可信 -> 要么永久阻塞（插件把游戏按死），
     *     要么谎报成功（然后我们 free 一个还在跑的线程 = UAF）。
     *
     * ⚠️ 这里**不再**直接调 svcWaitSynchronization 做对照了。
     *    上一版试过，结果是程序在 Azahar 上**卡死不返回**
     *    （status.txt 空、boot.txt 停在 f=74）——
     *    也就是说在这版模拟器上"带超时的等待"连测都测不了。
     *    那个实验的结论已经写进 perf_lvl.c 的注释（连同 threadJoin
     *    丢掉 timeout 参数的反汇编），这里只保留可靠的那一半。 */
    {
        int m3 = 0x7C7C;
        g_pf_mode = PF_MODE_SPIN;
        g_pf_ran  = 0;

        int rc = perf_worker_start(pf_worker, &m3, 16384);
        pf_note("scn=ruler 死循环 worker: start=%d ran=%d", rc, g_pf_ran);
        if (!rc) {
            g_pf_mode = PF_MODE_NORMAL;
            pf_note("scn=ruler 死循环 worker 起不来，跳过 D 段");
            return;
        }
        /* 多等一会儿，确认它**真的**没结束（否则 join 成功是正常的）。
         * 这个等待用 svcSleepThread，参数是纳秒。 */
        svcSleepThread(50000000ull);   /* 50ms */
        pf_note("scn=ruler 等了 50ms 后 ran=%d（若仍为 0，线程没起来）",
                g_pf_ran);

        u64 t0 = svcGetSystemTick();
        int j1 = perf_worker_join(600);        /* 600 毫秒 */
        u64 d1 = svcGetSystemTick() - t0;
        pf_note("scn=ruler D 死循环 join(600ms) rc=%d（期望 -2 超时）"
                " 耗时=%llu tick = %llu.%03llu ms",
                j1, (unsigned long long)d1,
                (unsigned long long)PF_TICKS2MS(d1),
                (unsigned long long)PF_MSREM(d1));
        pf_note("scn=ruler D 超时后 handle=%p（期望 NULL —— 已放弃，"
                "**没 free**，那个线程还在核上跑）",
                perf_worker_handle());

        g_pf_mode = PF_MODE_NORMAL;   /* 让它退出（如果它还在跑） */
    }
}

/* ══ 场景 1：内存不足 / 栈申请失败 ═══════════════════════════════
 *
 * 真造，不打桩：请求一个荒谬的栈（768MB），memalign 必然失败。
 *
 * 要验的：perf_worker_start 必须**返回 0**（而不是崩、不是死循环）。
 * 另外 g_job.fn 要被清掉 —— 否则下一次 start 会用上一个的陈旧函数指针。
 */
static void pf_scn_alloc_fail(void)
{
    int marker = 0x5A5A;

    g_pf_mode = PF_MODE_NORMAL;
    g_pf_ran  = 0;

    pf_note("scn=alloc_fail 试一个 768MB 栈的 worker（必然失败）");
    int rc = perf_worker_start(pf_worker, &marker, 768u * 1024u * 1024u);
    pf_note("scn=alloc_fail start=%d（期望 0）handle=%p（期望 NULL）",
            rc, perf_worker_handle());

    /* 失败之后必须还能再试一次小的 —— 证明失败没留下坏状态。 */
    g_pf_ran = 0;
    int rc2 = perf_worker_start(pf_worker, &marker, 0);   /* 0 = 用默认 32KB */
    pf_note("scn=alloc_fail 失败后重试 start=%d（期望 1）", rc2);

    if (rc2)
        pf_note("scn=alloc_fail join=%d（期望 0）ran=%d arg=%04x（期望 1 5a5a）",
                perf_worker_join(3000), g_pf_ran, g_pf_arg_seen);
    else
        pf_note("scn=alloc_fail 重试也失败了 → 栈回收有问题");
}

/* ══ 场景 2：worker 卡死不返回 ═══════════════════════════════════
 *
 * 真造：worker 进死循环。这是「join 超时」这条约束的唯一检验方式。
 *
 * 要验的三件事，缺一不可：
 *   ① join 必须在超时内返回 -2（不能无限等 —— 卡死游戏是最坏结果）
 *   ② 超时之后句柄必须被**置空**（g_worker=NULL），
 *      否则下一次 start 会去 join 一个已经放弃的线程
 *   ③ **绝不能 free** 那个线程 —— 这正是本轮修掉的 UAF。
 *      ③没法直接观测「没 free」，但可以观测它的**后果**：
 *      那个线程还在核上跑，如果它的控制块被释放了，
 *      后续任何堆操作都可能踩到它。本场景跑完再跑一次
 *      alloc + 若干次启动，进程还活着就是间接证据
 *      （真崩了的话，后面的 note 就不见了）。
 */
static void pf_scn_hang(void)
{
    int marker = 0x1234;

    g_pf_mode = PF_MODE_SPIN;    /* 死循环，join 必然超时 */
    g_pf_ran  = 0;

    pf_note("scn=hang 起一个永不返回的 worker");
    int rc = perf_worker_start(pf_worker, &marker, 16384);
    pf_note("scn=hang start=%d ran=%d", rc, g_pf_ran);
    if (!rc) {
        pf_note("scn=hang 竟然没起来（内存不够？）—— 本场景失去意义");
        return;
    }

    u64 t0 = svcGetSystemTick();

    /* 给 600ms。线程在死循环，所以必然超时。
     * 注意这里传的是**毫秒**：传 tick 的话 600ms 会变成 10 秒。 */
    int jr = perf_worker_join(600);

    u64 dt = svcGetSystemTick() - t0;

    pf_note("scn=hang join=%d（期望 -2 超时）耗时=%llu tick = %llu.%03llu ms"
            "（期望约 600ms，上界 3000ms）",
            jr, (unsigned long long)dt,
            (unsigned long long)PF_TICKS2MS(dt),
            (unsigned long long)PF_MSREM(dt));

    pf_note("scn=hang 超时后 handle=%p（期望 NULL —— 已被放弃）",
            perf_worker_handle());

    /* 那个死循环线程还在核上跑，我们管不了它（也不该去 kill）。
     * 之后所有场景都必须在它还活着的状态下继续跑通 ——
     * 这才是「放弃一个卡住的线程是可接受的」这句话的实测依据。 */
    g_pf_mode = PF_MODE_NORMAL;   /* 下次 start 不再死循环 */
}

/* ══ 场景 3：重复 start（线程复用） ══════════════════════════════
 *
 * 连着 start 两次，第二次必须先收拾第一个。
 * 第一次正常结束 -> 第二次应该拿到干净的 worker。
 * 两次都该成功；两次的 arg_seen 都得对（证明没有串用 g_job）。
 */
static void pf_scn_repeat(void)
{
    int m1 = 0x1111, m2 = 0x2222;

    g_pf_mode = PF_MODE_NORMAL;

    pf_note("scn=repeat 第一次 start=%d", perf_worker_start(pf_worker, &m1, 0));
    pf_note("scn=repeat 第一次 join=%d arg=%04x（期望 0 1111）",
            perf_worker_join(3000), g_pf_arg_seen);

    g_pf_ran = 0;
    pf_note("scn=repeat 第二次 start=%d", perf_worker_start(pf_worker, &m2, 0));
    pf_note("scn=repeat 第二次 join=%d arg=%04x（期望 0 2222 —— 不能是 1111）",
            perf_worker_join(3000), g_pf_arg_seen);
}

/* ══ 场景 4：空参数与零超时 ═════════════════════════════════════
 *
 * 边界输入。这类 bug 不会崩，只会静默错，所以更该测：
 *   · fn = NULL        -> 必须返回 0，且不建线程
 *   · join 时无 worker -> 必须返回 -1
 *   · join(0)          -> 语义是"立刻看一眼"，不是"无限等"
 */
static void pf_scn_edges(void)
{
    pf_note("scn=edges start(NULL)=%d（期望 0）",
            perf_worker_start(NULL, NULL, 0));
    /* 这两条走的是同一个早退分支：g_worker == NULL。
     * 所以第二个 UINT32_MAX **测的不是超时**（压根没进等待循环），
     * 它测的是"timeout 再离谱也不会走到 svc 层"。
     * 真正的"大 timeout + 有活线程"组合在 ruler 的 C 段里测。 */
    pf_note("scn=edges 无 worker 时 join=%d（期望 -1）",
            perf_worker_join(100));
    pf_note("scn=edges 无 worker 时 join(UINT32_MAX)=%d"
            "（期望 -1 —— 早退，**不进等待循环**，不能卡住）",
            perf_worker_join(0xFFFFFFFFu));

    /* 栈大小 1 字节：必须被抬到下限（8KB），而不是照着 1 字节建
     * —— 1 字节的栈连 trampoline 都放不下。 */
    int m = 0x3333;
    g_pf_mode = PF_MODE_NORMAL;
    g_pf_ran  = 0;
    int rc = perf_worker_start(pf_worker, &m, 1);
    pf_note("scn=edges 1 字节栈 start=%d（期望 1）", rc);
    if (rc) {
        int jr = perf_worker_join(3000);
        /* ran 必须是 1：栈被抬到 8KB 才可能跑完。
         * 如果这里 ran=0，说明栈真的只有 1 字节，trampoline 崩了。 */
        pf_note("scn=edges join=%d ran=%d arg=%04x（期望 0 1 3333）",
                jr, g_pf_ran, g_pf_arg_seen);
    }
}

/* ══ 场景 5：极端栈 + 反复起停（压力） ═══════════════════════════
 *
 * 连做 8 轮「起 → 跑 → 收」，其中一轮故意用大栈（128MB，
 * 在 32MB 池子上必然失败）。
 * 验的是**累积效应**：单次都对，但 8 轮下来有没有把线程对象、
 * 栈、内核句柄漏光 —— 漏光的表现是后面某轮突然失败。
 */
static void pf_scn_churn(void)
{
    int m = 0x4444;
    int ok = 0, fail = 0;

    g_pf_mode = PF_MODE_NORMAL;

    for (int i = 0; i < 8; ++i) {
        g_pf_ran = 0;
        size_t ss = (i == 4) ? (128u * 1024u * 1024u)   /* 故意失败 */
                             : 12288;
        int rc = perf_worker_start(pf_worker, &m, ss);
        if (!rc) {
            ++fail;
            pf_note("scn=churn 第 %d 轮 start=0（%s，期望）",
                    i, (i == 4) ? "大栈" : "小栈也失败了 → 有问题");
            continue;
        }
        int jr = perf_worker_join(3000);
        if (jr == 0 && g_pf_ran && g_pf_arg_seen == 0x4444)
            ++ok;
        else
            pf_note("scn=churn 第 %d 轮 join=%d ran=%d arg=%04x（有问题）",
                    i, jr, g_pf_ran, g_pf_arg_seen);
    }
    pf_note("scn=churn 8 轮：ok=%d fail=%d（期望 ok=7 fail=1）", ok, fail);
}

/* ══ 场景 6：探测本身的幂等 ═════════════════════════════════════
 *
 * perf_init 声明自己是幂等的。连调 5 次必须返回同一个值、
 * 且不重新发 IPC（否则在游戏运行中反复 init 会刷一串 ptm:sysm）。
 * 顺便验 perf_detail 在已 init 的状态下能读出全部字段。
 */
static void pf_scn_idem(void)
{
    PerfLevel first = perf_level();   /* 触发 init（若还没） */

    int same = 1;
    for (int i = 0; i < 4; ++i)
        if (perf_level() != first) same = 0;
    perf_init();
    if (perf_level() != first) same = 0;

    PerfLevel lv = PERF_UNKNOWN;
    int n3 = 0, bo = 0, du = 0, tk = 0;
    int rc = perf_detail(&lv, &n3, &bo, &du, &tk);

    pf_note("scn=idem 连调 5 次一致=%d lv=%d rc=%d new3ds=%d dual=%d limit=%d",
            same, lv, rc, n3, du, perf_worker_cpu_limit());
    pf_note("scn=idem 档位名=%s", perf_level_name(lv));

    /* perf_detail 在**没** init 时的行为也是契约：返回 -1，
     * 且不自己触发 init。但这里已经 init 了，所以只查字段完整性。 */
    if (rc == 0)
        pf_note("scn=idem perf_detail 三项 rc 全 >= 0（正常路径）");
}

/* ══ 场景 7：卡死线程还活着的前提下，再正常起停 ═════════════════
 *
 * 这是整个文件里最重要的一条，也是「放弃一个卡住的线程」
 * 这个设计取舍的**唯一**实测依据。
 *
 * 前面 hang 场景留下了一个死循环线程（我们故意不 free 它）。
 * 这一条在它还在核上跑的情况下，正常地起停 3 次普通 worker。
 *
 * 如果放弃策略写错了（比如偷偷 free 了那个线程），
 * 它的 trampoline 会在某个时刻从已释放内存读入口指针 -> 崩。
 * 崩了的话，本场景后面的 note 就不存在，宿主脚本按行数断言就会红。
 *
 * 注意：Azahar 上这条只能证明「我们的逻辑没崩」，
 * 真机上 use-after-free 的表现可能更晚更飘 —— 但至少这里能抓住
 * 「立刻就崩」的那一类。
 */
static void pf_scn_coexist(void)
{
    int m = 0x5555;
    int bad = 0;

    g_pf_mode = PF_MODE_NORMAL;

    for (int i = 0; i < 3; ++i) {
        g_pf_ran = 0;
        int rc = perf_worker_start(pf_worker, &m, 16384);
        int jr = rc ? perf_worker_join(3000) : -99;
        if (jr != 0 || !g_pf_ran || g_pf_arg_seen != 0x5555) {
            ++bad;
            pf_note("scn=coexist 第 %d 轮 start=%d join=%d ran=%d arg=%04x",
                    i, rc, jr, g_pf_ran, g_pf_arg_seen);
        }
        /* 每次之间做一次堆上的活（snprintf 会用 malloc）——
         * UAF 的经典后果就是踩坏 malloc 的元数据。 */
        char tmp[96];
        snprintf(tmp, sizeof tmp, "coexist %d %d %d", i, rc, jr);
    }
    pf_note("scn=coexist 3 轮全部正常=%d（期望 1）handle=%p（期望 NULL）",
            bad == 0, perf_worker_handle());
}

/* ══ 场景表 ═══════════════════════════════════════════════════════ */

static const struct {
    const char *name;
    void      (*fn)(void);
} PF_SCENARIOS[] = {
    /* ruler 必须第一：它量的是这台机器的时基，后面所有涉及
     * 「多久算超时」的判据都要拿它的结果当尺子。 */
    { "ruler",   pf_scn_ruler       },
    /* 顺序有讲究：hang 要在 coexist 之前（后者依赖前者留下的活线程），
     * 而 hang 必须在 edges/repeat 之前 —— 因为它会占住一个核，
     * 放最后会让前面的场景被它干扰（那不算 bug，只是噪音）。 */
    { "alloc",   pf_scn_alloc_fail },
    { "hang",    pf_scn_hang        },
    { "coexist", pf_scn_coexist     },
    { "repeat",  pf_scn_repeat      },
    { "edges",   pf_scn_edges       },
    { "churn",   pf_scn_churn       },
    { "idem",    pf_scn_idem        },
};
#define PF_N ((int)(sizeof PF_SCENARIOS / sizeof(*PF_SCENARIOS)))

void perffault_run(const char *scenario)
{
    if (!scenario || !*scenario) {
        app_note("perffault: 没给场景名。可用：ruler alloc hang coexist "
                 "repeat edges churn idem");
        return;
    }

    for (int i = 0; i < PF_N; ++i) {
        if (strcmp(scenario, PF_SCENARIOS[i].name) == 0) {
            app_note("perffault: === 场景 %s 开始 ===", scenario);
            PF_SCENARIOS[i].fn();
            /* 这一行是「这个场景跑完了而且进程还活着」的证据。
             * 断言脚本靠它确认「最后一行是 DONE 而不是半路断掉」。 */
            app_note("perffault: === 场景 %s DONE ===", scenario);
            return;
        }
    }

    app_note("perffault: 不认识的场景 '%s'（不执行任何东西）", scenario);
}
