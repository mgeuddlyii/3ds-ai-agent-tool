/*
 * perf_lvl.h/c — CPU 性能档位（New 3DS 专属，含三级降级）
 *
 * ══ 这个模块存在的唯一理由 ══════════════════════════════════════
 *
 * 它要装在 **游戏运行过程中**被打开。所以第一原则不是"跑得快"，
 * 是 **任何一步失败都不能把游戏带崩**。所有探测都满足：
 *   · 失败静默（不 printf、不弹窗、不 abort）
 *   · 幂等（只探一次，结果缓存）
 *   · 之后热路径只读一个 volatile int —— 零开销零风险
 *
 *
 * ══ 查证结论（2026-10-03，全部来自反汇编 + 源码，不是推测）══════
 *
 * ① **升频不走 APT，走 ptm:sysm。**
 *    我最初记的是「APT cmd 0x1D = aptSetCpuBoostMode」—— **这是错的**。
 *    3dbrew 的 APT 命令表里根本没有 boost 命令（表到 0x01050100 为止）。
 *    真正实现是：
 *        osSetSpeedupEnable(enable)              [os.c]
 *          └─ if (enable) value = 3 else value = 0
 *          └─ ptmSysmInit()  →  打开 ptm:sysm
 *          └─ PTMSYSM_ConfigureNew3DSCPU(value)  →  命令头 0x08180040
 *          └─ ptmSysmExit()
 *    value=3 的含义是 **804MHz + 启用 L2C-310**，value=0 是退回 268MHz。
 *    所以这层**不需要写任何 IPC**，一行 `osSetSpeedupEnable(true)` 完事。
 *
 * ② **双核也不需要 aptNewThread，直接传 threadCreate 的 core_id。**
 *    `threadCreate(entry, arg, stack, prio, core_id, detached)` 反汇编确认
 *    内部直接 `svcCreateThread(..., processor_id)`，**没有任何 APT 权限申请**。
 *    能不能用取决于 exheader 的两份授权，而 Luma 启动 3dsx 时会自己写。
 *    依据：`LumaTeam/Luma3DS` master 分支
 *          `sysmodules/loader/source/hbldr.c`
 *      · `kernelCaps[]` 里有一项 `0xFF002109`，注释原文：
 *        "Exflags: APPLICATION memtype + Shared page writing +
 *                    Allow debug + **Access core2**"
 *        —— 0x2000 那一位就是 `thread.h` 里说的 core2 开关，**Luma 给了**。
 *      · 同文件 `localcaps0->reslimits[0] = BIT(7) | 89`，注释原文：
 *        "Set mode1 preemption mode for core1, max. 89% of CPU time
 *         (**default 0, requires a APT_SetAppCpuTimeLimit call**)"
 *        —— core1 的配额 Luma 预授权了 89%，但**必须我们自己调一次
 *           APT_SetAppCpuTimeLimit 才会生效**。不调，core1 就是全 0。
 *      · 服务表 `serviceList[34][8]` 里有 `ptm:u` 和 `ptm:sysm`。
 *
 *    两条授权路径都由 Luma 写死，不依赖我们自己的 exheader，
 *    所以 **.3dsx / .3gx 插件形态同样有效** —— 这点很关键，
 *    因为直觉上「插件没有 exheader，所以插件用不了」，但这里恰恰相反：
 *    **我们要的授权全是运行时的，插件形态一样拿得到。**
 *
 * ③ **.cia 形态另有一条更早的路**：RSF 里 `CpuSpeed: 804mhz`
 *    （makerom 认这个 key；对应 exheader 的 use_cpu_clockrate_804MHz 位）。
 *    进程一起来就是满频，比运行时再调 osSetSpeedupEnable 更早。
 *    但**插件形态走 3dsx，exheader 被 Luma 整体覆盖，这条路无效** ——
 *    所以运行时那条（本模块）是唯一能覆盖插件形态的，必须有。
 *    两条并存不冲突：.cia 是「启动即满频 + 运行时确认」，插件只有运行时。
 *
 *
 * ══ 降级链 ════════════════════════════════════════════════════════
 *
 *     L3  dual    boost 804MHz + worker 跑在第二核（core2，退 core1）
 *         │ 任一步失败
 *     L2  boost   boost 804MHz，单核
 *         │ 任一步失败
 *     L1  base    什么都不做（等价于老样子，一定能跑）
 *
 * **失败必须是静默的，而且不重试。** 升频/建核失败绝大多数是
 * "这台机器/这个加载器不支持"，重试不会让它变支持，
 * 只会浪费启动时间，重复发还可能被系统视为滥用。
 *
 *
 * ══ 为什么不在游戏运行时长时间占着第二核 ═════════════════════════
 *
 * 插件的 worker 线程用 **detached + 一次性** 语义：
 * 建起来、跑完活、就退出。不常驻、不轮询、不抢游戏的时间片。
 * 这样即便游戏真的在跑，我们也只是在它需要 CPU 之外的间隙里搭个便车，
 * 不会把它挤掉。
 */
#ifndef PERF_LVL_H
#define PERF_LVL_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 性能档位。数值越大越快，但**不要**用于比较 —— 用名字。 */
typedef enum {
    PERF_UNKNOWN = 0,   /* 还没探测 */
    PERF_L1_BASE  = 1,   /* 单核 268MHz —— 一定能跑 */
    PERF_L2_BOOST = 2,   /* 单核 804MHz */
    PERF_L3_DUAL  = 3,   /* 804MHz + 第二核 worker */
} PerfLevel;

/*
 * 探测当前机器能到哪一档，并把能开的都开上。
 *
 * **返回实际达成的档位，一定 >= PERF_L1_BASE** ——
 * 即使不是 New 3DS、即使升频被拒，也一定返回 L1（什么都不做）。
 * 绝不会返回 PERF_UNKNOWN，也绝不会失败，更不会 abort。
 *
 * 幂等：重复调用只返回第一次的结果。
 * 耗时：一次 ptm:sysm IPC 往返 + 一次线程创建，毫秒级；Old 3DS 上几乎为零。
 */
PerfLevel perf_init(void);

/* 上一次 perf_init 的结果。不自己调 init。 */
PerfLevel perf_level(void);

/* 人类可读的档位名，进 app_note 用。 */
const char *perf_level_name(PerfLevel lv);

/*
 * 探测明细，供验收脚本断言。
 *
 * ⚠️ **boost_rc == 0 不代表「已验证升频成功」。**
 *   libctru 的 osSetSpeedupEnable 返回 **void**，拿不到 svc 层的 Result。
 *   所以 0 只表示「已调用，没抛异常」；真实的频率有没有真变成 804MHz
 *   **只能靠实测速度**判断（跑一次 matvec 计时，和 268MHz 基线比）。
 *   别把 0 当「验证通过」写进验收结论 —— 那是假阳性。
 *
 *   而 dual_rc == 0 是**硬结论**：它必须等探针线程真的在第二核上
 *   写了一个 volatile 标志才算成功（带 2 秒硬超时）。
 *   **建得出线程 != 线程真被调度过** —— 这个区别正是靠握手保证的，
 *   没握手就会把「拿到了一个永不结束的线程句柄」误判成双核可用。
 *
 * 返回 0 成功；负数是错误码（**都不致命**，perf_init 已降级）：
 *   -1  不是 New 3DS，或 APT_CheckNew3DS 失败（压根没调升频）
 *   -3  第二核线程**建不出来**（core2 和 core1 都试过了）
 *   -4  线程建出来了但**没在 2 秒内应答**（可能是建得出却拿不到时间片）
 *
 * 出参（都可传 NULL）：
 *   lv          实际档位
 *   is_new3ds   1/0，APT_CheckNew3DS 的结果
 *   boost_rc    见上面的警告
 *   dual_rc     0 = 第二核可用且确认被调度；<0 是具体原因
 *   sync_ticks  握手等待用掉的 tick 数（**60Hz，一 tick ≈ 16.67ms**）
 */
int perf_detail(PerfLevel *lv, int *is_new3ds, int *boost_rc,
                int *dual_rc, int *sync_ticks);

/*
 * ══ 第二核 worker 的最小封装 ═════════════════════════════════════
 *
 * 用法（两步，失败都不致命）：
 *
 *     if (perf_worker_start(my_func, my_arg, sizeof(my_stack))) {
 *         ...  稍后 perf_worker_join()   // 最多等 perf_worker_timeout_ticks()
 *     }
 *
 * 设计约束：
 *   · detached 线程，一次性，跑完就死，不常驻抢 CPU
 *   · 失败返回 false，**绝不 abort**（插件场景的硬要求）
 *   · worker 里**不允许**调 perf_* 以外的全局初始化（避免依赖环）
 *   · 栈默认 32KB，不够就自己传大 —— 但注意 New 3DS 上
 *     Luma 给的 App 内存只有 32MB 池子，别一次开太多线程
 */
typedef void (*PerfWorkerFn)(void *arg);

/* worker 线程栈的默认大小。32KB 够跑 nnet 的 matvec 热循环。 */
#define PERF_WORKER_STACK_DEFAULT (32 * 1024)

/*
 * 在第二核起一个 worker。
 *
 * core 的选择顺序：先试 core2（New3DS 独占，Luma 给了 0x2000），
 * 失败再试 core1（系统核，需 APT_SetAppCpuTimeLimit，Luma 给了 89% 配额）。
 * 两个都失败才返回 false。
 *
 * 成功返回 true。**重复调用会先 join 上一个**，不会泄漏线程。
 */
int perf_worker_start(PerfWorkerFn fn, void *arg, size_t stack_size);

/* 等 worker 结束，最多等 timeout_ms 毫秒。
 *
 * ⚠️ **单位是毫秒，不是 system tick。** 本文件里所有超时都用毫秒表达，
 *    内部才换算成 tick（3DS 的 tick 是 60Hz，一个 tick ≈ 16.67ms ——
 *    这个换算极易搞错，见 perf_lvl.c 顶部 PERF_TICKS_PER_SEC）。
 *
 * 返回 0 = worker 正常跑完；非 0 = 超时或没有 worker。
 * **绝不无限等待** —— 插件不能卡死游戏。传一个有限值。
 */
int perf_worker_join(uint32_t timeout_ms);

/* worker 线程句柄（诊断用）。NULL = 没有 worker。 */
void *perf_worker_handle(void);

/* 拿到的 CPU 时间配额（百分之一为单位，如 89 = 0.89% 的倒数关系）。
 * 没成功调用过 APT_SetAppCpuTimeLimit 就返回 0。 */
int perf_worker_cpu_limit(void);

#ifdef __cplusplus
}
#endif
#endif /* PERF_LVL_H */