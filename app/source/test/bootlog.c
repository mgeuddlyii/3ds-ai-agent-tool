/*
 * bootlog.c -- 见 bootlog.h
 *
 * 从 main.c 拆出来：sc_chat 等深层代码也要在关键路径（HTTP 发送、进屏）
 * 上打点，黑匣子必须是全局可见的，而不是入口文件的私有函数。
 */

#include "bootlog.h"

#include <3ds.h>
#include <stdarg.h>
#include <stdio.h>

#define BOOTLOG_MAX (64 * 1024)   /* 轨迹上限：滚掉最旧的，保住最近的现场 */

static void stamp(FILE *f, const char *name, const char *detail)
{
    fprintf(f, "%s %s (t=%llu ms)\n", name, detail,
            (unsigned long long)osGetTime());
}

void boot_mark(const char *name, const char *fmt, ...)
{
    char detail[96];
    va_list ap;
    FILE *f;

    detail[0] = '\0';
    if (fmt) {
        va_start(ap, fmt);
        (void)vsnprintf(detail, sizeof detail, fmt, ap);
        va_end(ap);
    }

    f = fopen("sdmc:/3ds/aiconsole/boot.txt", "wb");
    if (f) {
        stamp(f, name, detail);
        fclose(f);
    }

    /* 追加轨迹。★ 2026-10-22：**不再每次都打开看大小**。
     *
     * 原来每次 boot_mark 都要 fopen(boot.log,"rb") → fseek → ftell → fclose
     * 来问"文件多大了"，再 fopen 一次追加 —— 一个打点 = **4 次 SD 卡操作**。
     * 而 boot_mark 在每 32 帧就被 "iter"/"frame" 各叫一次（60fps 下
     * 每秒各 2 次），累计起来和 boot_mark_scr 那个每帧一次是同一类浪费。
     *
     * 改成：用一个静态计数器自己记着"大概写了多少行"，
     * 只有超过阈值才真去问一次文件大小并可能滚动。
     * 代价是滚动会**晚一点**发生（最多多出 ROWS_CHECK 行），
     * 而 boot.log 的作用是黑匣子 —— 晚几十行滚掉旧内容不影响任何结论。
     * 判据同样不受影响：脚本只读 status.txt。 */
    {
        /* 每行约 48 字节；BOOTLOG_MAX / 48 ≈ 1365 行。
         * 每 128 次打点才查一次真实大小，省掉 127/128 的 fopen。 */
        static int rows_since_check = 0;

        if (rows_since_check++ >= 128) {
            rows_since_check = 0;
            f = fopen("sdmc:/3ds/aiconsole/boot.log", "rb");
            if (f) {
                (void)fseek(f, 0, SEEK_END);
                long sz = ftell(f);
                fclose(f);
                if (sz > BOOTLOG_MAX)
                    f = fopen("sdmc:/3ds/aiconsole/boot.log", "wb");  /* 滚动 */
                else
                    f = NULL;
            }
        } else {
            f = NULL;
        }

        if (!f)
            f = fopen("sdmc:/3ds/aiconsole/boot.log", "a");
    }
    if (f) {
        stamp(f, name, detail);
        fclose(f);
    }
}

void boot_mark_scr(const char *screen, int frames)
{
    /* ★★ 2026-10-22：**只在屏幕真的变了（或每 300 帧）才写盘**。
     *
     * 原来每帧无条件 fopen/fprintf/fclose 一次 —— 这是本应用**最大的
     * 单项卡顿来源**，实测每帧 hdl 段（输入采集+handle+动画）要 122ms，
     * 而纯绘制只要 13.6ms。SD 卡上一次 open+write+close 就是几十毫秒，
     * 一帧一次 = 把 60fps 直接拽到 6.8fps。
     *
     * 这个文件的作用是「死机后看一眼最后停在哪屏」，它**只在换屏这个
     * 时刻**才有诊断价值；同一屏连写 60 次是 0 信息量的重复。
     * 每 300 帧补写一次，是为了让时间戳还能反映"它一直活着"
     * （死机时能看到停住的那一秒）+ 帧号能看出跑到多快。
     *
     * 判据不受影响：boot.txt **不参与任何验收断言**（脚本一次都没读过它）。 */
    static const char *last = NULL;
    static int last_frames = -1;

    if (screen == last && last_frames >= 0 && frames - last_frames < 300)
        return;

    last        = screen;
    last_frames = frames;

    /* 刻意**不做**「写 tmp 再 rename」：这个每帧都调，rename 是元数据操作，
     * 每帧一次在 SD 卡上既慢又费。而 boot.txt 也**不参与任何断言**
     * （脚本一次都没读过它）—— 它是给人看的事后诊断，所以
     * 「刚好读到 0 字节」只影响我排查时的观感，不影响任何结论。
     * 真正被断言依赖的 status.txt 走的是原子写，见 app.c 的 app_write_status。 */
    FILE *f = fopen("sdmc:/3ds/aiconsole/boot.txt", "wb");
    if (f) {
        fprintf(f, "scr %s f=%d (t=%llu ms)\n", screen, frames,
                (unsigned long long)osGetTime());
        fclose(f);
    }
}
