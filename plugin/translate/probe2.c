/*
 * probe2.c —— 分步留痕探针（不是产品代码，只用来定位真机死在哪一步）
 *
 * ── 为什么需要它 ──
 * 之前那个 4.3KB 的 probe.3gx 能写出 "1 bootstrap ok"，于是我们以为
 * "加载 + TLS + 服务"这一层没问题。**这个结论是错的**：
 * 那个探针里 `strlen("字面量")` 被 GCC 折成常量，全程没调用任何 newlib
 * 函数，也就完全没验证 TP 嫁接到底成不成立。
 * 所以真正的 main.c 可能在 bootstrap 里静默死掉，而探针看不出来。
 *
 * 这个探针的做法：**每走一步覆盖写一次卡上的文件**，
 * 读出来的那一行就是"最后活着走到的步骤"。
 *
 * ── 关键设计 ──
 *   1. mark 全程不依赖 newlib（自己算长度、自己转十六进制）
 *      —— 否则"留痕迹"这件事本身会跟着死
 *   2. TP 的值、gspInit 的返回值都写进文件，直接从数据判定，不靠推测
 *   3. 故意调用一次 snprintf：这是**真正**验证 reent 可用的唯一办法
 *   4. 卡在 gspInit 的话，文件会停在 D4 —— 那就能确定病根是
 *      "gsp::Gpu 已经被游戏独占"，而不是我们的代码
 */

#include <3ds.h>
#include <reent.h>
#include <stdio.h>   /* 只为 D4 那一步故意调 snprintf，验证 reent 真的可用 */

void *__service_ptr    = NULL;
u32   __system_runflags = 0;
u32   __apt_appid      = 0;

static struct _reent g_reent_store;
static uint32_t      g_tls_block[32];

/* ---------------- 不碰 newlib 的小工具 ---------------- */

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

static char *put_dec(char *p, unsigned v)
{
    char t[12];
    int  n = 0;

    if (!v) { *p++ = '0'; return p; }
    while (v) { t[n++] = (char)('0' + v % 10); v /= 10; }
    while (n) *p++ = t[--n];
    return p;
}

/* ---------------- 留痕 ---------------- */

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

/* 组装一行 "Dx ..." 再写。不带 printf，避免还没验证就依赖它 */
static void mark_kv(const char *tag, const char *k, unsigned v)
{
    char b[80];
    char *p = b;

    while (*tag) *p++ = *tag++;
    *p++ = ' ';
    while (*k) *p++ = *k++;
    *p++ = '=';
    p = put_hex(p, v);
    *p = 0;
    mark(b);
}

int main(void)
{
    u32      *tp = NULL;
    Result    rc;

    /* ① 服务框架。libctru 的 srv/fs 不碰 newlib，先拿它们才有办法留痕 */
    srvInit();
    fsInit();
    mark("D0 srv+fs ok");

    /* ② 读出现有 TP。真机转储里它是 0x1FFB1000（不是我们的块） */
    __asm__ volatile ("mrc p15, 0, %0, c13, c0, 3" : "=r"(tp));
    mark_kv("D1", "tp", (unsigned)(uintptr_t)tp);
    mark_kv("D1b", "tp0", tp ? tp[0] : 0xDEADBEEFu);

    /* ③ 把魔数和 reent 指针填进 TP 指向的那块内存 */
    if (!tp || (uintptr_t)tp < 0x00100000u) {
        tp = g_tls_block;
        __asm__ volatile ("mcr p15, 0, %0, c13, c0, 3" :: "r"(tp) : "memory");
        mark("D2 tp was bad, used own block");
    } else {
        mark("D2 tp was usable");
    }
    tp[0] = 0x21545624u;
    tp[1] = 0u;
    tp[2] = (uint32_t)&g_reent_store;
    tp[3] = 0xFFFFFFF8u;
    ((unsigned char *)tp)[24] = 0;
    _REENT_INIT_PTR(&g_reent_store);
    mark("D3 tls grafted");

    /* ④ 真刀真枪调一次 newlib：能过这步，reent 才算真的可用 */
    {
        char buf[32];
        int  n = snprintf(buf, sizeof(buf), "newlib %d", 42);
        mark_kv("D4", "snprintf", (unsigned)n);
    }

    /* ⑤ 拿 GPU 服务。游戏正握着它 —— 这一步就是最可能卡死的地方 */
    rc = gspInit();
    mark_kv("D5", "gspInit", (unsigned)rc);
    if (R_FAILED(rc)) {
        mark("D5b gspInit FAILED -> stop (game owns gsp::Gpu)");
        for (;;) svcSleepThread(1000000000LL);
    }

    /* ⑥ 到这里才可能真的画出东西 */
    gfxInitDefault();
    mark("D6 gfxInit ok");

    /* ⑦ 顶住：每 600 帧（约 10 秒）把计数写一次，
     *    这样"插件活着但画面被游戏盖掉"和"插件根本没跑"就能分开 */
    {
        unsigned n = 0;
        for (;;) {
            u8  *raw = gfxGetFramebuffer(GFX_BOTTOM, GFX_LEFT, NULL, NULL);
            u32 *fb  = (u32 *)raw;
            if (fb) {
                for (int i = 0; i < 240 * 320; ++i) fb[i] = 0xFF00FFFFu; /* 洋红 */
                gfxFlushBuffers();
            }
            if ((++n % 600u) == 0) {
                char b[40];
                char *p = b;
                const char *t = "D7 alive ";
                while (*t) *p++ = *t++;
                p = put_dec(p, n / 600u);
                *p = 0;
                mark(b);
            }
            svcSleepThread(16000000LL);
        }
    }
}
