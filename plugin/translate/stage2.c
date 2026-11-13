/*
 * stage2.c —— 架构 + 绘制 一起验的第二版探针（不接 UI，只画一条洋红色带）
 *
 * ── 这一版要一次性验掉的三件事 ──
 * 1. 入口契约：main() 能安全返回（boot.s 已改成照抄 CTRComposer 的写法）
 * 2. 工作线程：裸 svcCreateThread 起的线程，自己补 TLS 后能调 newlib
 * 3. 绘制通路：**不碰 GSP**，直接读 LCD 寄存器拿帧缓冲地址写像素
 *    （这是 22:10 那次 `gspInit()` 卡死的正面解法）
 *
 * ── 期望结果 ──
 * 游戏正常启动、不崩，下屏底部出现一条**闪烁的洋红色带**
 * （游戏每帧会擦掉它，我们 4ms 重刷一次，所以看起来是闪的，这是正常的）。
 * 痕迹文件最后一行应该是 `B4 drawing`。
 *
 * ── 为什么只画可见缓冲 ──
 * 双缓冲下隐藏缓冲会被游戏在翻转前重写，往那里画纯属白费；
 * CTRComposer 的结论是"只重刷可见缓冲，间隔 4ms"，闪断率约 12%，足够稳。
 */

#include <3ds.h>
#include <reent.h>
#include <stdio.h>

#include "lcd.h"

void *__service_ptr     = NULL;
u32   __system_runflags = 0;
u32   __apt_appid       = 0;

static struct _reent g_reent_store;
static u32           g_worker_stack[8192];   /* 32KB */
static Handle        g_worker;

/* ---------------- 不留 newlib 依赖的小工具 ---------------- */

static unsigned slen(const char *s)
{
    unsigned n = 0;
    while (s[n]) n++;
    return n;
}

static char *put_hex(char *p, unsigned v)
{
    static const char H[] = "0123456789ABCDEF";
    for (int i = 28; i >= 0; i -= 4) *p++ = H[(v >> i) & 0xF];
    return p;
}

static void mark(const char *what)
{
    Handle  fh;
    u32     nw = 0;
    FS_Path p  = fsMakePath(PATH_ASCII, "/3ds/aiconsole/plugin_boot.txt");

    if (R_FAILED(FSUSER_OpenFileDirectly(&fh, ARCHIVE_SDMC,
                                         fsMakePath(PATH_EMPTY, ""), p,
                                         FS_OPEN_WRITE | FS_OPEN_CREATE, 0)))
        return;
    FSFILE_Write(fh, &nw, 0, what, slen(what), FS_WRITE_FLUSH);
    FSFILE_Close(fh);
}

static void mark3(const char *tag, const char *k1, unsigned v1,
                  const char *k2, unsigned v2, const char *k3, unsigned v3)
{
    char  b[110];
    char *p = b;

    while (*tag) *p++ = *tag++;
    *p++ = ' ';
    while (*k1) *p++ = *k1++;
    *p++ = '=';
    p = put_hex(p, v1);

    if (k2) { *p++ = ' '; while (*k2) *p++ = *k2++; *p++ = '='; p = put_hex(p, v2); }
    if (k3) { *p++ = ' '; while (*k3) *p++ = *k3++; *p++ = '='; p = put_hex(p, v3); }
    *p = 0;
    mark(b);
}

/* ---------------- 工作线程 ---------------- */

/* 裸 svcCreateThread 建出来的线程没有 libctru 的 ThreadVars：
 *   [0]=魔数  [2]=reent  [4]=fs_magic(0 → 用全局 fs 会话)
 * 少一项，线程里第一次 snprintf/fopen 就在 __syscall_getreent 里 svcBreak。 */
static int worker_tls(void)
{
    u32 *tv = NULL;

    __asm__ volatile ("mrc p15, 0, %0, c13, c0, 3" : "=r"(tv));
    if (!tv || (uintptr_t)tv < 0x00100000u)
        return -1;

    tv[0] = 0x21545624u;
    tv[1] = 0u;
    tv[2] = (u32)(uintptr_t)&g_reent_store;
    tv[3] = 0xFFFFFFF8u;
    tv[4] = 0u;
    ((unsigned char *)tv)[24] = 0;
    return 0;
}

static void worker_main(void *arg)
{
    LcdFb f;

    (void)arg;

    if (worker_tls() != 0) {
        mark("B2x tls seed failed");
        for (;;) svcSleepThread(1000000000LL);
    }
    mark("B2 worker tls ok");

    /* 先只取一次，把参数记下来 —— 万一后面画崩了，这一行就是证据 */
    if (lcd_visible(1, &f) != 0) {
        mark("B3 no framebuffer");
        for (;;) svcSleepThread(1000000000LL);
    }
    mark3("B3 fb", "fb", f.fb, "stride", f.stride, "bpp", f.bpp);

    /* 参数明显不对就别画 —— 宁可什么都不显示，也不要写坏内存把游戏打崩 */
    if (f.stride < (u32)f.h || f.stride > 4096u || f.bpp < 2u || f.bpp > 4u) {
        mark3("B3x fb params bad", "fb", f.fb, "stride", f.stride, "bpp", f.bpp);
        for (;;) svcSleepThread(1000000000LL);
    }

    mark("B4 drawing");

    /* 每 4ms 往【当前可见】的下屏缓冲重刷一条洋红色带。
     * 不能只取一次地址：游戏会翻转缓冲，Select 位随时在变。 */
    for (;;) {
        if (lcd_visible(1, &f) == 0 && f.stride >= (u32)f.h)
            lcd_fill(&f, 0, 170, f.w, 70, 0xFF00FFu);   /* 洋红 */

        svcSleepThread(4000000LL);   /* 4ms */
    }
}

int main(void)
{
    Result rc;

    srvInit();
    fsInit();
    mark("B0 srv+fs ok");

    rc = svcCreateThread(&g_worker, (ThreadFunc)worker_main, 0,
                         g_worker_stack + sizeof(g_worker_stack) / 4,
                         0x30, -1);
    mark3("B1 thread", "rc", (unsigned)rc, NULL, 0, NULL, 0);

    /* ★ 必须立刻返回：Luma 的加载器在等这个返回，它还要跳去游戏真正的入口。
     *   返回路径的现场还原在 boot.s 里（push {r0-r12,lr} + CPSR + pop {…,pc}）。 */
    return 0;
}
