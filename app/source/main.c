/*
 * 3DS AI Console -- 入口
 *
 * 里程碑进度见 设计规划.md §6：
 *   M0 ✅ 工具链 + CIA 骨架
 *   M1 ✅ 中文渲染 + 渲染抽象层
 *   M2 ✅ 首启 3 屏 + config 落盘   ← 本文件当前对应
 *
 * 本文件保持"薄"：只做初始化、主循环、全局按键（START / SELECT），
 * 以及无人值守验收的收尾。屏幕与状态在 ui/，渲染在 gfx/，配置在 shared/。
 *
 * 全局按键约定：START = 退出程序，SELECT = 截图。这两件和"当前在哪一屏"
 * 无关，所以放在这里而不是各屏的 handle() 里。屏幕内的返回一律是 B。
 *
 * 界面文案全部是**编译期常量**。将来确认弹窗的文案也必须是常量 ——
 * 任何把 AI 生成文本渲染进确认框的做法都会让模型有机会伪造 UI
 * （见 SAFETY_DESIGN.md §3）。
 */

#include <3ds.h>
#include <stdio.h>
#include <stdlib.h>
#include <malloc.h>   /* memalign —— newlib 把它放这儿，不在 stdlib.h 里 */
#include <string.h>

#include "render.h"
#include "font.h"
#include "shot.h"

#include "app.h"
#include "sysstat.h"
#include "autorun.h"
#include "bootlog.h"
#include "httpx.h"
#include "splash.h"
#include "widgets.h"
#include "perf_lvl.h"

/* 把启动期的失败写成文件。黑屏 + 无输出是最难查的，宁可先在卡上留个证据。 */
static void write_error(const char *what, int rc, const char *detail)
{
    shot_mkdirs(SHOT_DIR);

    FILE *f = fopen(SHOT_DIR "/error.txt", "wb");
    if (!f)
        return;

    fprintf(f, "%s failed rc=%d\n", what, rc);
    fprintf(f, "expected top=%dx%d bottom=%dx%d\n", R_TOP_W, R_TOP_H, R_BOT_W, R_BOT_H);
    if (detail)
        fprintf(f, "detail: %s\n", detail);
    fclose(f);
}

/* 截图名净化在 gfx/shot.c（shot_sanitize）—— 流式期间的连拍也要用同一套规则 */

/* 启动/运行探针 boot_mark 已挪到 test/bootlog.c —— sc_chat 等深层代码
 * 也要在关键路径（HTTP 发送、进屏）上打点，黑匣子得全局可见：
 *   boot.txt = 每次覆写（最后现场），boot.log = 追加（完整轨迹） */

/* ── 栈独立化（Azahar 16KB 主线程栈的治本方案）────────────────────────
 * Azahar 的 3dsx 加载器把主线程栈硬编码为 Kernel::DEFAULT_STACK_SIZE
 * = 0x4000（16KB），且完全忽略 3dsx/ELF 的 __stacksize__（见 azahar
 * src/core/loader/3dsx.cpp: process->Run(48, DEFAULT_STACK_SIZE)）。
 * 真机 Homebrew Launcher 默认给 256KB —— 这纯属模拟器侧的坑。
 * 16KB 装不下本 app 的调用深度 + newlib stdio（svfprintf 单帧 1~2KB，
 * dtoa 2~3KB）：栈被压穿到 0x07FFFFF0（栈底 0x08000000 之下），
 * 返回地址成垃圾 → PC=0 → Undefined Instruction。M1 调用链浅没炸，
 * M2 的 keystore/聊天/更深 UI 一进来就爆。
 * 解法：把整个 app 搬进自建 256KB 栈的工作线程（与真机默认一致），
 * 主线程只负责创建并等它结束。
 * 注：r_init/字体/SD/httpc/APT 都基于进程级服务，从工作线程用没问题。 */
#define APP_THREAD_STACK 0x40000   /* 256KB */

static void app_thread_main(void *arg)
{
    (void)arg;

    boot_mark("enter", "worker thread running");
    int rrc = r_init();
    if (rrc != 0) {
        write_error("r_init", rrc, "帧缓冲布局与 render.h 的假设不符");
        gfxExit();
        return;   /* 线程函数是 void：rc 已写进 error.txt */
    }

    /* ★ 启动进度屏的第 1 帧：这时帧缓冲已就绪但字体还没好 ——
     *   splash 里要画字，所以它必须排在 font_init 之后。
     *   这里先不画，等字体就绪再画第一帧（见下方）。 */

    int frc = font_init();
    if (frc != 0) {
        write_error("font_init", frc,
                    "内嵌字库校验失败（-1..-7 见 font.c；-8 = 内嵌数据太短）");
        r_exit();
        return;
    }

    boot_mark("font", "ok");

    /* ★ 字体好了 —— 立刻画第一帧启动进度。
     *   这一帧的意义：用户从"黑屏"变成"看到了界面"，哪怕后面还要干活。
     *   之后每完成一个阶段画一帧，进度条持续推进。 */
    splash_frame(0, "初始化显示");
    boot_mark("splash", "step=0");

    /* ── 网络栈 ────────────────────────────────────────────────────────
     * 必须在**任何 HTTP 之前**：curl 走的是 3DS 的 socket 层，soc 没起来时
     * curl_easy_perform 会失败，而且报出来的错指向 TLS/证书，
     * 完全看不出真正原因是"网络栈根本没初始化"。
     *
     * ⚠️ 这里以前**没有 socInit**，而 chatapi.c 直接调 httpcInit(0)。
     * 那条路在真机上本来就不成立（httpc 要求 soc 已起），只是没人发现 ——
     * 因为整套自动验收用的是 demo provider，它压根不联网。
     * 接豆包（TLS 1.2）才把这个洞照出来：不补这一段，豆包一定连不上。
     *
     * 失败不拦启动：本来就是个 AI 助手，没网时离线攻略/演示仍要能用。
     * 只留一条痕迹，让"为什么联网功能不好使"有据可查。 */
    splash_frame(1, "启动网络栈");
    boot_mark("splash", "step=1");
    {
        u32 *soc_buf = (u32 *)memalign(0x1000, 0x10000);

        if (soc_buf && R_SUCCEEDED(socInit(soc_buf, 0x10000))) {
            boot_mark("soc", "ok");
            /* curl 的全局初始化跟着 soc 走 —— 它没有 soc 也用不了。 */
            if (hx_init()) {
                boot_mark("curl", "%s", hx_version());
                app_note("net=soc ok · %s", hx_version());
            } else {
                boot_mark("curl", "init failed");

                app_note("net=soc ok 但 curl 起不来（HTTPS 全不可用）");
            }
        } else {
            free(soc_buf);
            boot_mark("soc", "failed");
            app_note("net=soc FAILED —— 联网功能全都不可用（离线功能不受影响）");
        }
    }

    /* 先读脚本再读配置：脚本能决定这一趟要看哪一屏，
     * 而起始屏是 app_init 根据配置定的，顺序不能反。 */
    splash_frame(2, "读取配置");
    boot_mark("splash", "step=2");
    bool scripted = autorun_init();
    boot_mark("autorun", "scripted=%d steps=%d err=%s",
              (int)scripted, autorun_steps(), autorun_error());
    app_init();
    boot_mark("app_init", "ok");

    app_note("autorun=%d steps=%d err=%s", (int)scripted,
             autorun_steps(), autorun_error());
    app_note("font glyphs=%d cell=%dx%d baseline=%d",
             font_glyph_count(), font_cell_w(), font_cell_h(), font_baseline());

    /* CPU 性能档位探测。放在 app_init 之后 —— 那时服务已经起来了，
     * 但**必须在主循环之前**：perf_init 会发 ptm:sysm 和 APT 的 IPC，
     * 而 aptMainLoop 一旦进去就只 poll 一次事件，不再给我们插入点。
     *
     * 顺序也保证失败是静默的：探测早于任何绘制和任何按键处理，
     * 就算退化成 L1（什么都不做），后面的流程一个字都不用改。
     *
     * 插件场景（.3gx 由 Luma 链进游戏）额外注意：Luma 会在写 exheader 时
     * 带上 core2 权限（0xFF002109）和 core1 的 89% 配额
     *（reslimits[0] = BIT(7)|89），所以这里的两条路在插件形态也是通的。
     * 我们只要 60% 而不是 89% —— 插件的第一服务对象是游戏，不是我们。*/
    splash_frame(3, "探测性能档位");
    boot_mark("splash", "step=3");
    {
        PerfLevel plv = perf_init();
        int p_n3 = 0, p_boost = 0, p_dual = 0, p_tick = 0;
        perf_detail(NULL, &p_n3, &p_boost, &p_dual, &p_tick);
        boot_mark("perf", "%s new3ds=%d boost=%d dual=%d ticks=%d limit=%d",
                  perf_level_name(plv), p_n3, p_boost, p_dual, p_tick,
                  perf_worker_cpu_limit());
        /* ⚠️ **boost 的 0 不代表「已验证升频成功」**。
         * osSetSpeedupEnable 返回 void，拿不到 svc 的 Result，
         * 所以 0 只表示「已调用，没抛」；真实的升频是否生效**只能靠
         * 实测速度**判断（真机上跑一次 matvec 比基准时间）。
         * dual=0 才是硬结论 —— 那意味着线程真的被调度了，
         * 因为它必须等一个 volatile 标志被写上才算成功。*/
        app_note("perf=%s new3ds=%d boost=%d(unverified) dual=%d ticks=%d",
                 perf_level_name(plv), p_n3, p_boost, p_dual, p_tick);
    }

    /* 版面溢出计数：这一趟画过的每一行都会被量一遍，超宽就记。
     * 重置放循环前，这样 status.txt 里报的是本趟渲染的真实情况。 */
    splash_frame(4, "准备界面");
    boot_mark("splash", "step=4");
    w_overflow_reset();

    /* 最后一步：画"就绪"帧。之后主循环接管，下一帧就是真正的界面 ——
     * 因为 app_init 已经把起始屏 push 好了，splash_done 只是把进度条走满，
     * 真正的界面紧随其后出现，中间没有黑屏空档。 */
    splash_done();
    boot_mark("splash", "done");

    boot_mark("loop", "begin");

    int frames = 0;

    /* ★ 帧耗时探针（2026-10-22 加，用于定位"卡顿"）。
     *   目的：把"这一帧慢在哪一段"变成可读的数字，而不是靠猜。
     *   三段分别记累计毫秒：
     *     t_in   输入采集 + autorun 合成（hidScanInput .. synth 替换完）
     *     t_hdl  app_handle（含状态机、屏幕切换、按键处理）
     *     t_rdr  app_render + r_end（绘制 + 清缓冲 + 交换 + 等 vblank）
     *   ⚠️ 只在**前 400 帧**累计，之后停手 —— 探针本身也要花时间
     *   （osGetTime 每次是一次 IPC），常驻会自己污染测量。
     *   ⚠️ r_end 里的 gspWaitForVBlank 是**设计内的等待**，它会被算进
     *   t_rdr；所以判读时要看 t_rdr - 1000/fps 才是"真正的绘制开销"。
     *   （60fps 下 vblank 一帧 16.7ms，判读时按 frames 折算掉。） */
    u64 t_in = 0, t_hdl = 0, t_rdr = 0, t_swap = 0, t_hid = 0;
    u64 t_start = osGetTime();
    int probe_frames = 0;

    while (aptMainLoop()) {
        /* 探针：前 8 帧逐帧记录，之后每 32 帧一次。
         * 若连 iter 都没有 = 阻塞在 aptMainLoop；
         * 若只有 iter 0 = 阻塞在循环体内（大概率 r_end 的 vblank 等待）。 */
        if (frames < 8 || (frames & 31) == 0)
            boot_mark("iter", "frames=%d", frames);

        u64 t_h0 = osGetTime();
        hidScanInput();
        u32 down = hidKeysDown();
        u32 held = hidKeysHeld();
        if (probe_frames < 400)
            t_hid += osGetTime() - t_h0;

        /* 触摸采集：held 期间一直给坐标（画"按下"的样子），
         * pressed 只在按下沿为真（触发动作）。坐标进黑匣子 ——
         * "点了没反应"和"按了没反应"是同一类问题，都要留证据。 */
        touchPosition tpos;
        hidTouchRead(&tpos);
        const bool touching = (held & KEY_TOUCH) != 0 || (down & KEY_TOUCH) != 0;
        app_touch_frame(touching ? (int)tpos.px : -1,
                        touching ? (int)tpos.py : -1,
                        (down & KEY_TOUCH) != 0);
        if (down & KEY_TOUCH)
            boot_mark("touch", "x=%d y=%d frame=%d", tpos.px, tpos.py, frames);

        /* 摇杆采集（2026-10-23 新增）。
         * 用户定的分工：十字键管下屏内容，摇杆上下管上屏文字滚动。
         * 摇杆回中时 hidCircleRead 也会吐出 ±3~6 的抖动值，
         * 所以这里**不做**死区 —— 死区留给用它那一屏判（各屏灵敏度不同）。
         * 回放脚本推的是按键，摇杆只对真实硬件有效；这也是它必须
         * 经 app 层转一手、而不是在屏幕里直接 hidCircleRead 的原因。 */
        circlePosition cpos;
        hidCircleRead(&cpos);
        app_circle_frame((int)cpos.dx, (int)cpos.dy);

        /* 按键探针：任何 down/held 都记录 —— 「按了没反应」类问题
         * 用它一锤定音：guest 到底有没有看见键盘。
         *
         * ★ 位置在 autorun **之后**（2026-10-16 挪的）。
         *
         * 早先它在合成替换**之前**，于是记的是"真实硬件按键" ——
         * 而回放（脚本喂键）不碰真实硬件，这一行**永远不写**。
         * 也就是说：最需要按键轨迹的那个场景（脚本按了键、程序没反应），
         * 恰恰是它瞎掉的场景。2026-10-16 查"拒绝之后 SELECT/A 有没有
         * 到达对话屏"就是这么卡住的 —— 翻了半天 boot.log，一行按键都没有。
         *
         * 挪到后面记的是**这一帧真正交给屏幕的**按键，回放与真机一视同仁。
         * 只在真有按键的帧写（wait 帧不写），所以不会把 boot.log 灌满。 */
        u32         synth_keys = 0;
        int         synth_tx = -1, synth_ty = -1;
        int         synth_stick_y = 0;
        const char *synth_shot = NULL;
        bool        synth_quit = false;

        if (autorun_tick(&synth_keys, &synth_tx, &synth_ty,
                         &synth_stick_y, &synth_shot, &synth_quit)) {
            /* 合成输入替换真实输入。held 也清空 —— 回放里没有"按住"的概念，
             * 留着会触发出真实按键重复的效果。 */
            down = synth_keys;
            held = 0;

            /* 合成摇杆：只有脚本真的推了（非 0）才覆盖。
             * 不判非 0 的话，回放的每一帧都会把摇杆"按回中位"——
             * 真机上用户推着摇杆、脚本却在跑别的趟次时，滚动会失灵。
             * 这条规则与"合成触摸覆盖真实"不同：触摸那条是**每一帧
             * 都要有明确答案**（-1 表示松开），而摇杆没有"松开"指令，
             * 回中就是 0，两者语义不同，不能共用一套写法。 */
            if (synth_stick_y != 0)
                app_circle_frame(0, synth_stick_y);

            if (down & KEY_TOUCH)
                app_touch_frame(synth_tx, synth_ty, true);   /* 合成触摸覆盖真实 */

            if (down || held || synth_stick_y)
                boot_mark("key", "down=%08x held=%08x stick=%d frame=%d (synth)",
                          down, held, synth_stick_y, frames);
        } else if (down || held) {
            boot_mark("key", "down=%08x held=%08x frame=%d", down, held, frames);
        }

        if (synth_quit)
            break;

        const char *shot_name = synth_shot;

        if (down & KEY_START)
            break;
        /* 手动截图热键 = ZR。原先在 SELECT 上，但对话屏把 SELECT 用作
         * 快捷短语了 —— 全局热键不能抢屏幕键位，谁后做谁挪。 */
        if (down & KEY_ZR)
            shot_name = "manual";

        app_handle(down, held);
        /* 按钮按下动画推进：必须在 render 之前 ——
         * render 时 w_button 读动画值决定下沉量/配色。 */
        w_anim_frame();
        /* 屏幕探针：死时正停在哪一屏，boot.txt 最后一行直接可见。
         * 每帧只覆写 boot.txt、不进 boot.log —— 轨迹留给关键事件。 */
        boot_mark_scr(screen_name(app_current()), frames);
        {
            u64 t1 = osGetTime();
            app_render();
            u64 t2 = osGetTime();
            /* 状态面板的采样：**复用这两个已有时间戳**，不额外调 osGetTime ——
             * 这一句每帧都跑（不像上面的探针只跑前 400 帧），
             * 观测本身要尽量不花钱。 */
            sysstat_tick((unsigned)t_start, (unsigned)t2);
            if (probe_frames < 400) {
                t_hdl += t1 - t_start;   /* 输入采集 + autorun + app_handle + anim */
                t_rdr += t2 - t1;        /* app_render（含 r_end 的 vblank 等待） */
                ++probe_frames;
            }
        }

        if (shot_name) {
            char name[48];
            shot_mkdirs(SHOT_DIR);
            snprintf(name, sizeof name, "%s", shot_name);
            shot_sanitize(name);
            shot_save_both(SHOT_DIR, name);
        }

        {
            u64 t3 = osGetTime();
            r_end();
            if (probe_frames <= 400)
                t_swap += osGetTime() - t3;   /* 清缓冲 + 交换 + 等 vblank */
            t_start = osGetTime();            /* 下一帧的起点 */
        }
        ++frames;
        if ((frames & 31) == 0)
            boot_mark("frame", "%d", frames);
    }

    app_shutdown();
    /* ★ 帧耗时探针的结果（见循环上方那段说明）。
     *   报的是**前 probe_frames 帧的平均值**，四段分开：
     *     hdl  = 输入采集 + autorun + app_handle + 按钮动画
     *     rdr  = app_render（纯绘制，不含交换）
     *     swap = r_end（清缓冲 + gfxFlush + gfxSwap + 等 vblank）
     *     tot  = 三者之和
     *   ⚠️ swap 里含**设计内的 vblank 等待**（60fps 下约 16.7ms/帧）。
     *   判读：tot - 16.7 ≈ 真实的每帧计算开销；若 tot 远大于 16.7
     *   而 swap 很小，瓶颈就在 rdr（绘制太重）；若 swap 占大头而
     *   hdl/rdr 都小，那是 r_end 里阻塞得比一帧还久（交换排队）。
     *   ×100 是整数化，避免 %f 拉进 newlib dtoa（栈炸元凶）。 */
    if (probe_frames > 0) {
        app_note("fpsprobe: n=%d hid=%d hdl=%d rdr=%d swap=%d tot=%d (x100 us)",
                 probe_frames,
                 (int)(t_hid  * 100 / (u64)probe_frames),
                 (int)(t_hdl  * 100 / (u64)probe_frames),
                 (int)(t_rdr  * 100 / (u64)probe_frames),
                 (int)(t_swap * 100 / (u64)probe_frames),
                 (int)((t_hdl + t_rdr + t_swap) * 100 / (u64)probe_frames));
    }

    app_write_status(frames);

    /* 关网络的顺序与开相反：先收 curl 再收 soc。
     * 反过来的话 curl 还可能持着 socket 句柄，而 soc 已经被关掉了。 */
    hx_exit();
    socExit();

    font_exit();
    r_exit();
}

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    /* 与主线程同优先级；主线程随后阻塞在等待里，不抢 CPU。 */
    s32 prio = 0x30;
    svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);

    Thread t = threadCreate(app_thread_main, NULL, APP_THREAD_STACK,
                            prio, -2, false);
    if (t) {
        /* ⚠️ **不用 threadJoin(t, U64_MAX)** ——
         * 这版 libctru 的 threadJoin 有 bug：它是 (Thread, u64 timeout_ns)，
         * timeout 是**栈上**的第二个参数，但它重载 r0 当 handle 之后
         * 直接 `b svcWaitSynchronization`，**从没把 timeout 放进 r1:r2**
         * （r1 被一条 ldrb 占着）。于是传下去的是那个标志字节，不是超时值。
         * 反汇编见 shared/nnet/perf_lvl.c 里 perf_wait_worker 上方的注释。
         *
         * ✅ 这里用 svc 是**安全的**，而且是刻意的 ——
         *   与 perf_lvl.c 刻意避开它相反，差别只在**想要什么**：
         *   · 这里要的是「一直等到 app 线程结束」——
         *     它是本进程的主体，跑完就该退出，不存在"超时"这个概念。
         *     即便这版 svc 把 timeout 也一并忽略（Azahar 上观察到的正是
         *     如此：对一个仍在跑的线程，给 1ms 也**不返回**，
         *     说明它等到线程真的结束为止），那也正是这里想要的语义。
         *   · perf_lvl.c 要的是「等不及就放弃」——
         *     而**这恰恰是 svc 给不了的那一半**。它只会等到线程真的结束，
         *     于是"带超时"三个字在它身上和 threadJoin 一样不成立。
         *
         * 所以判据很简单：**凡是需要"超时就放弃"的地方，一律不许用它。**
         * 3dbrew 的 "Timeout occurred ... when timeout is not ~0"
         * 在这版 libctru + Azahar 上实测不成立，别赌。 */
        svcWaitSynchronization(threadGetHandle(t), U64_MAX);
        threadFree(t);
        return 0;
    }

    /* 线程创建失败（正常不会发生）：退回主线程直接跑，栈小但聊胜于无。 */
    app_thread_main(NULL);
    return 0;
}
