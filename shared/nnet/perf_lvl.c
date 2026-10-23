/*
 * perf_lvl.c — CPU 性能档位实现。设计说明见 perf_lvl.h 顶部。
 *
 * 本文件的第一职责不是"跑得快"，是 **绝不把宿主游戏带崩**。
 * 任何一步失败都只体现为"档位低一档"，没有一处会 abort / 死循环 /
 * 无限等待 / 往 stderr 喷东西（喷了也会被游戏看到，白底黑字，很难看）。
 */
#include "perf_lvl.h"

#include <3ds.h>
#include <stddef.h>

/* ══ 状态 ═══════════════════════════════════════════════════════════
 * 只有一个写者（perf_init），发生在任何读者之前。
 * 所以不需要锁 —— volatile 只保证"编译器别把它优化进寄存器"。
 */
static volatile int     g_lv;              /* PerfLevel */
static volatile int     g_is_new3ds;
static volatile int     g_boost_rc;
static volatile int     g_dual_rc;
static volatile int     g_sync_ticks;

static Thread   g_worker;                   /* NULL = 没有 worker */
static int       g_worker_core = -1;        /* 实际用上的核号 */
static int       g_cpu_limit;               /* 已申请的 CPU 配额 */

/* perf_init 握手**确认过可用**的核号；-1 = 还没确认过任何核。
 *
 * 为什么单独记一个，而不每次 perf_worker_start 都重新试：
 *   · 「重新试」的实现是"建一个探针线程 + 等它应答 + free"。
 *     而"等它应答"必须**确认它真的跑完了**才能 free ——
 *     一个建得出但拿不到时间片的线程，它的 Thread 结构被 free 之后，
 *     trampoline 会在某个时刻从已释放内存里读入口函数指针。
 *   · 探一次的代价也就几毫秒，缓存结果没有性能损失。
 */
static int       g_verified_core = -1;

/* worker 跑完时由 worker 自己置 1。
 * 这是"它结束了"的唯一判据 —— 不查任何 svc（见 perf_wait_worker）。 */
static volatile int g_worker_done;

/* perf_verify_core 累计等待的 tick 数（诊断用）。
 * 单独记而不是让它做返回值，是因为「等到了」和「等了多久」是两件事：
 * 前者决定档位，后者只在排查时有用。 */
static int       g_verify_ticks;

/* 等待循环的一步。20ms：细到不会在边界上白等 3 秒，
 * 粗到不会因为反复唤醒而拖累主线程。 */
#define PERF_WAIT_STEP_MS      20u


/* ══ system tick 的真实尺度 ═══════════════════════════════════════
 *
 * ⚠️⚠️ **两个量纲，两次都踩错了。这里的每个数字都是实测出来的。**
 *
 * 【错误 1】我曾以为 system tick = 60Hz。
 *   实测（Azahar，perffault 的 ruler 场景，A 段）：
 *       sleep 1ms   = 1,000,000 ns  ->    269,414 tick
 *       sleep 10ms  =10,000,000 ns  ->  2,682,421 tick
 *       sleep 100ms =100,000,000 ns -> 26,812,488 tick
 *   三组都收敛到 **268 tick/微秒**。
 *
 * 【错误 1b】修完 60Hz 之后，我又把系数写成 268000 并注释"268 kHz"——
 *   **还是错的，差 1000 倍。** 268 是 tick/微秒，不是 tick/毫秒。
 *   两个互相独立的测量交叉验证了这一点：
 *       A 段（sleep 100ms）  26,812,488 tick / 100 ms = 268,125 tick/ms
 *       D 段（join 600ms）  160,896,234 tick / 600 ms = 268,160 tick/ms
 *   两条路径差 0.013%，所以真实频率 = **268 MHz**（tick 计数器跑在 CPU 时钟上）。
 *
 *   ⚠️ 为什么这个错**没让超时失效**（这是它危险的地方）：
 *     超时逻辑里 MS2TICKS 只出现在**比值**中 ——
 *         deadline / step = MS2TICKS(600) / MS2TICKS(20) = 30 步 = 30×20ms
 *     任何线性常数都会约掉，所以 join(600ms) 实测**恰好**在 600ms 返回 -2。
 *     常数只在「把 tick 换算成人话」的地方露馅（本次就错了一次：
 *     ruler D 段把 600ms 显示成 600359ms）。
 *     → 教训：**只验证相对量时，常数错了也测不出来。**
 *       必须有一条「绝对量」判据（ruler A 段就是这个角色）。
 *
 *   所以下面这个常数**必须**配 A 段那条绝对判据一起看。
 *   60Hz 那个数差了 4400 倍 —— 而当时所有超时都基于它，
 *   于是「2 秒超时」实际是 0.0075 秒，「3 秒」是 0.011 秒。
 *   错的方向还是最坏的那种：**超时变得极短**，
 *   于是慢一点的机器上会频繁误判成「线程没起来」。
 *
 * 【错误 2】我曾以为 svcSleepThread / threadJoin 的时间参数是 tick。
 *   错。svc.h 的注释写得很清楚：
 *       svcSleepThread:  "@param ns The minimum number of **nanoseconds**"
 *       threadJoin:     "@param timeout_ns Timeout in **nanoseconds**"
 *   所以**时间参数一律是纳秒，只有 svcGetSystemTick 才是 tick**。
 *   这两个量纲混用时 compilers 不会报错、运行时不崩，只是超时全错 ——
 *   最难发现的一类 bug。
 *
 * 【tick 怎么换算】MS2TICKS **只用于比值**（deadline/step），
 *   所以这个常数对超时行为**没有影响**（见【错误 1b】）。
 *   但诊断输出里要做「tick -> 人话」的换算，那里必须用真值。
 *   保留成宏是为了「以后要打时间的时候不至于又随手写个 60」。
 */
#define PERF_TICK_HZ        268000000ull   /* 实测 268 MHz（两条路径交叉验证） */
#define PERF_TICKS_PER_MS   (PERF_TICK_HZ / 1000ull)

/* 纳秒 -> tick。只用于「量一段时间」，不用于「等一段时间」。 */
#define MS2TICKS(ms)  ((u64)((ms) * PERF_TICKS_PER_MS))

/* tick -> 毫秒（整数，截断）。**只给诊断打印用**。 */
#define TICKS2MS(t)   ((u64)((t) / PERF_TICKS_PER_MS))

/* 探测等待：累计 2 秒。每步睡 250ms —— 足够细到不死等，
 * 又粗到不会因为反复唤醒而拖累主线程。 */
#define PERF_PROBE_TIMEOUT_MS   2000u
#define PERF_PROBE_STEP_MS      250u

/* 探测线程的栈。它只写一个 volatile 标志就退出，1KB 都嫌多，
 * 给 4KB 是为了让 threadCreate 的分配一定成功（栈算下来要向系统要一页）。 */
#define PERF_PROBE_STACK        4096u

/* 探针线程的优先级。与主线程同级（0x30），
 * **刻意不更高** —— 探测不该比游戏还急。 */
#define PERF_PROBE_PRIO         0x30

/* ════════════════════════════════════════════════════════════════════
 * 等一个 worker 结束 —— **只靠轮询一个 volatile 标志**
 * ════════════════════════════════════════════════════════════════════
 *
 * 返回 0 = worker 确实跑完了（可以安全 free）
 * 返回负数 = 没等到（worker 可能还在跑，**调用方绝不能 free**）
 *
 * ══ 为什么不能用 threadJoin，也不直接用 svcWaitSynchronization ════
 *
 * 两者在这个环境里都**不可信**，而且是实测出来的：
 *
 * 【threadJoin 的 timeout 参数根本没被传下去】
 *   devkitARM gcc 16.1.0 / libctru.a 的 thread.o 反汇编：
 *       threadJoin:
 *          0:  cmp  r0, #0          ; r0 = thread
 *          4:  beq  14
 *          8:  ldrb r1, [r0, #17]  ; ← 顺手占了 r1
 *          c:  cmp  r1, #0
 *         10:  beq  1c
 *         14:  mov  r0, #0
 *         18:  bx   lr
 *         1c:  ldr  r0, [r0]       ; r0 = handle
 *         20:  b    svcWaitSynchronization
 *   threadJoin(Thread, u64 timeout_ns) 的 timeout 在栈上（第二个参数），
 *   而 svcWaitSynchronization(Handle, s64 timeout_ns) 需要 r1:r2=timeout。
 *   这段代码只重载了 r0，**从没往 r1:r2 写 timeout**（r1 被 ldrb 占着）。
 *   实测：对一个**确认在跑**的死循环线程，threadJoin(th, 1ms) 返回
 *   **rc=0（成功）** —— 超时完全没生效。
 *
 * 【svcWaitSynchronization 也给不了"超时就放弃"】
 *   换成一个直接对**确认在跑**的线程调 svcWaitSynchronization(h, 1ms)，
 *   程序在 ruler 场景里直接**卡死不返回**（status.txt 空、
 *   boot.txt 停在 f=74）。
 *
 *   ⚠️ 顺便修正一个我自己的误读：当时我写的是"svc 在模拟器上坏了"。
 *   不对 —— 更可能是它**根本没在 timeout 上超时**，而是一直等到
 *   线程真的结束（对活线程来说那就是永远）。也就是说：
 *   **它不是不能用，是它的 timeout 不可信** —— 和 threadJoin 一个毛病。
 *   这也解释了为什么 app/source/main.c 里 `svcWaitSynchronization(h, U64_MAX)`
 *   是安全的：那里要的就是"等到结束"，而它恰好只会做这一件事。
 *   反过来，**凡是需要"超时就放弃"的地方，它和 threadJoin 一样不能用**。
 *
 *   两条证据指向同一句话：这一版 libctru + Azahar 上，
 *   **没有任何 wait/join 能给你"等不及就撒手"**。
 *
 * ══ 所以只用两个被实测证明可靠的原语 ══════════════════════════════
 *
 *   · svcGetSystemTick  —— 纯读寄存器，不进内核，ruler A 段实测 268 tick/ms
 *   · svcSleepThread    —— 纳秒，ruler A 段实测三档（1/10/100ms）全部线性
 *
 * 加上一个 volatile int。**不依赖任何 wait/join svc 的语义**，
 * 也不依赖「建出来的线程一定会被调度」—— 它要是永远不来，
 * 我们只是超时而已，而超时正是我们要的行为。
 *
 * 为什么这样反而更安全：唯一能让我们卡死的因素变成了
 * 「svcGetSystemTick 或 svcSleepThread 本身坏了」，而这两个
 * 是最简单、最不可能有语义分歧的调用。
 */
static int perf_wait_worker(uint32_t timeout_ms)
{
    const uint64_t deadline = MS2TICKS(timeout_ms);
    const uint64_t step     = MS2TICKS(PERF_WAIT_STEP_MS);
    uint64_t waited = 0;

    while (!g_worker_done) {
        if (waited >= deadline)
            return -1;               /* 超时。**不 free** */
        svcSleepThread((s64)(PERF_WAIT_STEP_MS * 1000000ull));  /* 纳秒 */
        waited += step;
    }
    return 0;
}

/* perf_worker_start 里收拾上一个 worker 的等待上限。
 * 3 秒足够跑完一次 matvec；超时即视为"上一个卡住了"，直接放弃。
 *
 * ⚠️ 单位是**毫秒**。这个宏传进 perf_wait_worker 后会被 MS2TICKS
 *    换算成 tick；这里绝不自己写 1000*1000 之类的东西 —— 那个坑踩过。 */
#define PERF_WORKER_JOIN_MS     3000u

/* 真正的 worker 的优先级。仍是 0x30 —— 与主线程同级，不抢游戏。 */
#define PERF_WORKER_PRIO        0x30

/* ══ 探测用的握手变量 ═══════════════════════════════════════════
 *
 * 这两个**只**用于 perf_verify_core 探测核，不参与真 worker 的生命周期。
 * 真 worker 的完成判据是 g_worker_done（见 perf_wait_worker）。
 * 分成两套的原因见 perf_verify_core 上方的注释：探测要证明的是
 * "这个核能不能调度我"，真 worker 要证明的是"活干完了"，
 * 混用一个标志会让其中一个的失败污染另一个。
 */
static volatile int      g_worker_ready;
static volatile int      g_worker_arg_seen;

/* 探测用的极简 worker：只写标志就退出，不碰任何全局状态。 */
static void perf_probe_worker(void *arg)
{
    (void)arg;
    g_worker_arg_seen = 1;
    g_worker_ready    = 1;
}

/* perf_verify_core 定义在下方（它依赖 perf_probe_worker 与握手变量），
 * 但 perf_init 要用它。加一条前向声明，把「实现集中在一处」和
 * 「调用顺序自然」两个诉求都满足。 */
static int perf_verify_core(int core);

/* ════════════════════════════════════════════════════════════════════
 * perf_init
 * ════════════════════════════════════════════════════════════════════ */
PerfLevel perf_init(void)
{
    /* 幂等：已经探过就直接返回，绝不重复发 IPC。 */
    if (g_lv != PERF_UNKNOWN)
        return (PerfLevel)g_lv;

    int rc_boost = 0;
    int rc_dual  = 0;
    int new3ds   = 0;

    /* ---- 第 0 步：这是不是 New 3DS -------------------------------
     * 老 3DS 上调 PTMSYSM_ConfigureNew3DSCPU 是无意义的（那命令只在新机有意义），
     * 而且在老机上它可能返回错误。所以先问一句。
     *
     * APT_CheckNew3DS 需要 8.0.0-18 才有这个命令。老机上它会返回错误，
     * 那正好 —— 我们把它当作"不是 New 3DS"。
     */
    {
        bool isNew = false;
        if (R_SUCCEEDED(APT_CheckNew3DS(&isNew)) && isNew)
            new3ds = 1;
        g_is_new3ds = new3ds;
    }

    /* ---- 第 1 步：升频到 804MHz ----------------------------------
     * osSetSpeedupEnable(true) 内部：
     *   ptmSysmInit() → PTMSYSM_ConfigureNew3DSCPU(3) → ptmSysmExit()
     * value 3 = 804MHz + L2C-310。
     *
     * 它返回 void，不给 Result —— 所以 rc_boost 只能靠"之后能不能
     * 拿到 CPU 频率"来判断。这里我们保守处理：
     *   · 不是 New 3DS → 根本不调，直接记失败
     *   · 是 New 3DS   → 调一次，之后不再碰
     *
     * ⚠️ **绝不在这里调 osSetSpeedupEnable(false)** ——
     *   降频会影响到整个进程，而进程可能还带着别人（链式加载）。
     *   一旦把频率降下去，接下来几个 token 全是 3 倍慢。
     */
    if (!new3ds) {
        rc_boost = -1;
    } else {
        /* 这一版 libctru 里 osSetSpeedupEnable 返回 void，
         * 拿不到 svc 的 Result。所以只能"调用即视为成功"，
         * 真实的失败会表现为"跑起来还是很慢"，不是崩溃。 */
        osSetSpeedupEnable(true);
        rc_boost = 0;
    }
    g_boost_rc = rc_boost;

    /* ---- 第 2 步：申请 core1 配额 + 建第二核线程 -----------------
     * 顺序很重要：core1 的 CPU 时间配额**必须先申请**，
     * 否则建出来的线程拿不到时间片（会成功，但永远不跑）。
     *
     * Luma 在 hbldr.c 里给的是 `reslimits[0] = BIT(7) | 89`，
     * 注释明确写"requires a APT_SetAppCpuTimeLimit call"，
     * 所以这一步就是那个 call。我们申请一个保守值：
     *
     *   89   = Luma 默认给的量，照抄
     *   60   = 我们自己降一档，给游戏多留点余量
     *
     * 选 60 的理由：**插件的第一服务对象是游戏，不是我们**。
     * 我们只是搭便车，不该去抢它的时间片。剩下的量够跑 matvec 了。
     */
    if (new3ds) {
        Result r = APT_SetAppCpuTimeLimit(60);
        if (R_SUCCEEDED(r)) {
            g_cpu_limit = 60;
        } else {
            g_cpu_limit = 0;
        }
    }

    /* ---- 第 3 步：核可用性探测 -----------------------------------
     * 走 perf_verify_core（唯一实现，带握手）。
     * core2 优先（New 3DS 独占，Luma 的 0xFF002109 给了权限），
     * 退 core1（系统核，配额刚申请过）。
     *
     * ⚠️ 只有 new3ds 才探。老 3DS 上根本没有第二核，
     *    而 threadCreate(core=2) 在老机上**可能返回成功**
     *    （svc 层不校验 core 编号是否合法），于是我们会白等 2 秒
     *    一次握手 —— 在插件场景里那是白占游戏两秒。
     */
    if (new3ds) {
        if (perf_verify_core(2)) {
            g_verified_core = 2;
            rc_dual = 0;
        } else if (perf_verify_core(1)) {
            g_verified_core = 1;
            rc_dual = 0;
        } else {
            /* 两个都不可用。区分不了是「建不出」还是「建得出但不跑」——
             * 插件场景下这个区别没有价值（都要退回单核），不值得
             * 为了区分它再冒一次泄漏的风险。两个码都留着：
             * -3 = 至少尝试过；具体原因看 g_sync_ticks（0 = 一次都没成）。 */
            rc_dual = -3;
            g_verified_core = -1;
        }
    } else {
        rc_dual = -1;          /* 老 3DS：只有 core0 一核，不算失败也不假装 */
        g_verified_core = -1;
    }

    g_dual_rc     = rc_dual;
    g_sync_ticks  = g_verify_ticks;

    /* ---- 定档 ---------------------------------------------------- */
    {
        PerfLevel lv;
        if (rc_boost < 0)
            lv = PERF_L1_BASE;                 /* 连 804 都没拿到，别想两核 */
        else if (rc_dual < 0)
            lv = PERF_L2_BOOST;                /* 升频成了，两核没成 */
        else
            lv = PERF_L3_DUAL;
        g_lv = (int)lv;
    }

    return (PerfLevel)g_lv;
}

PerfLevel perf_level(void)
{
    if (g_lv == PERF_UNKNOWN)
        return perf_init();
    return (PerfLevel)g_lv;
}

const char *perf_level_name(PerfLevel lv)
{
    switch (lv) {
    case PERF_L1_BASE:  return "L1 base(1core/268MHz)";
    case PERF_L2_BOOST: return "L2 boost(1core/804MHz)";
    case PERF_L3_DUAL:  return "L3 dual(2core/804MHz)";
    case PERF_UNKNOWN:  break;
    }
    return "?";
}

int perf_detail(PerfLevel *lv, int *is_new3ds, int *boost_rc,
                int *dual_rc, int *sync_ticks)
{
    /* 不自己 init —— 避免调用方只想看个字段却触发一堆 IPC。
     * 没 init 过就照实报告 UNKNOWN。 */
    if (lv)         *lv         = (PerfLevel)g_lv;
    if (is_new3ds)  *is_new3ds  = g_is_new3ds;
    if (boost_rc)   *boost_rc   = g_boost_rc;
    if (dual_rc)    *dual_rc    = g_dual_rc;
    if (sync_ticks) *sync_ticks = g_sync_ticks;

    if (g_lv == PERF_UNKNOWN) return -1;
    if (g_boost_rc < 0)       return -1;
    if (g_dual_rc  < 0)       return g_dual_rc;
    return 0;
}

/* ════════════════════════════════════════════════════════════════════
 * worker
 * ════════════════════════════════════════════════════════════════════ */

/*
 * threadCreate 只给一个 void* 参数，但我们要同时传"函数"和"用户参数"。
 * 打包成一个结构体传指针。
 *
 * 为什么可以用**静态**变量而不是 malloc：
 *   perf_worker_start 保证同一时刻只有一个 worker（它会先 join 旧的），
 *   而 join 是带硬超时的 —— 拿不到锁。所以静态变量不构成数据竞争。
 *   反过来，malloc 会在插件场景多一个失败点（内存被游戏吃了大半），
 *   **多一个失败点就多一份崩溃风险**，不划算。
 */
typedef struct {
    PerfWorkerFn fn;
    void        *arg;
} PerfJob;

static PerfJob g_job;

/* 真正干活的 worker 入口。
 *
 * ⚠️ 最后那行 `g_worker_done = 1` 是整个等待机制里**唯一的完成信号**，
 *    不是可选的收尾。少了它，perf_wait_worker 只会一直等到超时，
 *    于是每次 join 都必然走"放弃"分支 —— 而那条分支**故意不 free**，
 *    于是每跑一次推理泄漏一个 worker。
 *    （线程自己退出是另一回事：libctru 的 trampoline 退出时会自己清理，
 *    泄漏的只是我们这边的 Thread 结构。）
 */
static void perf_worker_entry(void *arg)
{
    PerfJob *job = (PerfJob *)arg;
    PerfWorkerFn fn = job->fn;
    void        *ua = job->arg;

    if (fn) fn(ua);

    /* 无论 fn 是不是 NULL 都要置位 —— "没干活"也是"干完了"，
     * 否则调用方会白等一个 PERF_WORKER_JOIN_MS。 */
    g_worker_done = 1;
}

/* ════════════════════════════════════════════════════════════════════
 * perf_verify_core —— 「这个核真的能用吗」的**唯一**实现
 * ════════════════════════════════════════════════════════════════════
 *
 * 返回 1 = 这个核确认可用（线程真的被调度过并跑完了）
 * 返回 0 = 不可用（**已经清理干净，没有残留**）
 *
 * ── 这里为什么必须带握手，不能只看 threadCreate 的返回值 ──────────
 *
 * 「建得出线程」和「线程会被调度」是两件完全不同的事。
 * 第二核（core2）在 exheader 没有 0x2000 位时，svcCreateThread
 * **照样返回成功**，但这个线程永远等不到时间片 —— 它躺在那儿，
 * 栈和内核对象都占着，永远不退出。
 *
 * 于是「建得出就 free」是一个**必炸**的写法：
 *   · 此刻线程还没跑，_thread_begin trampoline 也还没执行；
 *   · 我们把 Thread 结构 free 掉了；
 *   · 之后某个时刻系统调度了它，trampoline 从**已释放的内存**里
 *     读入口函数指针和参数（见 libctru 的 _thread_begin：
 *     `ldr r3,[r0,#4]` / `ldr r2,[r0,#8]`）——
 *   · 那是从堆里读一个函数指针然后 blx 它。
 * 在插件场景里这等于**把宿主游戏带崩**。
 *
 * 而 threadJoin 的超时**不能**当成这个问题的安全网 —— 实测证明它
 * 压根没把 timeout 传下去（同上，svcWaitSynchronization 也一样）。
 * 所以我们不赌它们，改成**只有确认线程自己跑完了才 free**：
 *
 *   · 握手成功  → 线程已经执行完 perf_probe_worker 并返回，
 *                 trampoline 之后只碰自己栈上的东西，不再读 Thread
 *                 结构 → 这时 free 才安全。
 *   · 握手超时  → **绝不 free**。泄漏一个 4KB 栈的线程，
 *                 比 use-after-free 可接受得多（见文件头的取舍原则）。
 *
 * 这也是为什么 g_verified_core 要缓存：**这条危险路径只该走一次**。
 */
static int perf_verify_core(int core)
{
    g_worker_ready    = 0;
    g_worker_arg_seen = 0;
    g_worker_done     = 0;

    Thread th = threadCreate(perf_probe_worker, NULL, PERF_PROBE_STACK,
                             PERF_PROBE_PRIO, core, false);
    if (!th)
        return 0;   /* 建不出来：干净，什么都没残留 */

    const uint64_t TIMEOUT = MS2TICKS(PERF_PROBE_TIMEOUT_MS);
    const uint64_t STEP    = MS2TICKS(PERF_PROBE_STEP_MS);
    uint64_t waited = 0;

    while (!g_worker_ready && waited < TIMEOUT) {
        /* ⚠️ 纳秒，不是 tick。见上面「system tick 的真实尺度」的【错误 2】。
         * 这里传 STEP（= 250ms 对应的 67000）的话，实际只睡了 67 微秒 ——
         * 探测会慢 3700 倍，而且慢的机器上干脆就变成"看起来像没起来"。 */
        svcSleepThread((s64)(PERF_PROBE_STEP_MS * 1000000ull));
        waited += STEP;
    }
    g_verify_ticks += (int)waited;

    if (!g_worker_ready) {
        /* 没应答 —— 放弃，且**故意泄漏 th**。理由见上面的注释。
         * 探测线程只写两个 volatile 就退出，所以它即便真的被调度了，
         * 泄漏的也只是 4KB 栈 —— 而 free 的代价是 UAF。 */
        return 0;
    }

    /* 到这里线程确认跑完了：g_worker_ready 是 worker 自己写的，
     * 它写完就 return，之后 trampoline 不再读 Thread 结构。
     * 所以**直接 free，不用再等一次**。
     *
     * （曾经在这里多调一次 perf_wait_thread 做"双重确认"，那是多余的：
     *  唯一能改变结论的判据就是 g_worker_ready，再等一次不会更安全，
     *  只会把一个已经确定的判断变成带超时的等待 ——
     *  而"带超时的等待"正是我们刚刚才发现不可信的那个东西。） */
    threadFree(th);
    return 1;
}

/*
 * perf_worker_start 内部用的核心号选择。
 *
 * ⚠️ **这里不做任何探测。** 只读 perf_init 缓存下来的 g_verified_core。
 *
 * 这一条是本轮修掉的一个真 bug 的核心：原来的实现每次 start 都
 * 「建探针线程 → threadJoin → threadFree」，而 join 超时被忽略了。
 * 探针万一没被调度（这正是它要探测的那种情况！），
 * free 掉的就是一个**还在跑**的线程的控制块 ——
 * trampoline 随后从已释放内存里读入口函数指针。
 * 插件场景下这是必崩，而且崩在宿主游戏里。
 *
 * 现在改成：探测只在 perf_init 做一次，结果缓存，
 * 之后热路径只读一个 int。这样这条危险路径每进程只走一次。
 */
static int perf_pick_core(void)
{
    if (g_lv == PERF_UNKNOWN) perf_init();   /* 保证探测过 */

    if (g_verified_core < 0) return -1;      /* 已知没有可用核，单核跑 */

    /* 配额可能是在 perf_init 里申请失败过（比如那次 apt 正忙）。
     * 这里再补一次 —— 但**只在还没拿到配额时**才发，避免重复申请。
     * 仍然不发任何线程，所以不引入 UAF 风险。 */
    if (g_cpu_limit == 0 && g_is_new3ds) {
        if (R_SUCCEEDED(APT_SetAppCpuTimeLimit(60)))
            g_cpu_limit = 60;
    }

    return g_verified_core;
}


int perf_worker_start(PerfWorkerFn fn, void *arg, size_t stack_size)
{
    if (!fn) return 0;

    /* 先收拾上一个。
     * 刻意**带超时**：万一它没结束，我们宁可放弃它，也不能被它拖死 ——
     * 插件卡住 = 整个游戏卡住，这是最不能接受的结果。
     *
     * ⚠️ 用 perf_wait_worker 而不是 threadJoin：后者的 timeout 是坏的
     *    （见 perf_wait_worker 上方的反汇编与实测）。"带超时"三个字
     *    在 threadJoin 身上不成立 —— 它只会一直等下去。 */
    if (g_worker) {
        if (perf_wait_worker(PERF_WORKER_JOIN_MS) < 0) {
            /* 上一个卡住了。放弃它，但**绝不 free** ——
             * 线程还在跑，free 它的控制块就是 use-after-free。
             * 同时也不在此时再起一个：状态不明的时候叠加新线程，
             * 是把一个"卡住"变成"两个卡住 + 一份坏内存"。 */
            g_worker = NULL;
            g_worker_core = -1;
            return 0;
        }
        threadFree(g_worker);
        g_worker = NULL;
        g_worker_core = -1;
    }

    int core = perf_pick_core();
    if (core < 0) return 0;

    /* 栈至少给 8KB。给多了浪费（插件的内存池被游戏占着），
     * 给少了 nnet 的 matvec 递归会爆。 */
    size_t ss = stack_size ? stack_size : PERF_WORKER_STACK_DEFAULT;
    if (ss < 8192) ss = 8192;

    g_job.fn  = fn;
    g_job.arg = arg;

    /* ⚠️ 清完成标志的时机很讲究：
     *   · 必须在上面那个"收拾上一个"循环**之后**、建线程**之前**。
     *     早了会被上一个 worker 的迟到写入覆盖（worker 还在跑时
     *     就把标志清了 → 我们会误以为它没跑完，白等一轮）。
     *   · 必须在 threadCreate **之前**。反过来的话，
     *     「建线程失败」这条路径上标志是上一轮的 1，
     *     调用方会以为刚发的活已经做完了。
     */
    g_worker_done = 0;

    /* PERF_WORKER_PRIO = 0x30，与主线程同级。刻意不更高（0x18 更快）：
     * 插件的 worker 不该比游戏还急。 */
    Thread th = threadCreate(perf_worker_entry, &g_job, ss,
                             PERF_WORKER_PRIO, core, false);
    if (!th) {
        /* 建线程失败 —— 这通常意味着内存不够。
         * 静默退回单核，绝不报错。
         * 标志留着 0：调用方 join 时会超时并放弃，
         * 那正是"什么都没发出去"该有的表现。 */
        g_job.fn = NULL;
        return 0;
    }

    g_worker      = th;
    g_worker_core = core;
    return 1;
}

int perf_worker_join(uint32_t timeout_ms)
{
    if (!g_worker) return -1;

    /* timeout_ms == 0 意为"立刻看一眼"，不是"无限等"。
     * 想要真正的无限等 —— 那是 svcWaitSynchronization 的 U64_MAX 语义，
     * 但**插件里不该那么用**，那等于把游戏押在这一个线程上。 */
    if (perf_wait_worker(timeout_ms) < 0) {
        /* 超时：worker 还活着。
         *
         * ⚠️⚠️ **必须把句柄置空** —— 这一行是本轮修掉的第二个真 bug。
         *
         * 原来的写法是 `return -2;` 什么都不做，于是 g_worker 一直非空。
         * 后果不是"下次推理慢一点"，而是**永久降级**：
         *   下次 perf_worker_start 看到 g_worker 非空
         *     -> perf_wait_worker(3000) -> 又是 3 秒死等 -> 又超时
         *     -> start 返回 0，推理失败
         *   再下次，同样。
         * 也就是说**一次卡死就让 AI 永久停在"每次推理白等 3 秒然后失败"**，
         * 而游戏那边一切正常 —— 这种故障最难被用户归因，
         * 因为"游戏没崩"反而让人以为插件没事。
         *
         * 取舍：这里放弃的是「我还能安全回收它」的机会，换来
         * 「后续调用立刻恢复正常」。插件场景下这笔交易非常划算：
         * 泄漏一个 16KB 栈的线程，是**一次性**代价；
         * 而卡住句柄是**每次调用**都要付 3 秒。
         *
         * 绝不能做的：threadFree(g_worker)。它还在跑，
         * free 掉它的控制块就是 use-after-free（见上面的注释）。
         */
        g_worker      = NULL;
        g_worker_core = -1;
        g_job.fn      = NULL;
        return -2;   /* 已放弃；调用方应据 -2 走单核回退 */
    }

    threadFree(g_worker);
    g_worker      = NULL;
    g_worker_core = -1;
    g_job.fn      = NULL;
    return 0;
}

void *perf_worker_handle(void)
{
    return (void *)g_worker;
}

int perf_worker_cpu_limit(void)
{
    return g_cpu_limit;
}