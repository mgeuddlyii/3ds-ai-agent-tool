/*
 * probe3.c —— 根目录版探针（`/luma/plugins/default.3gx`）
 *
 * ── 为什么改用根目录，而不是游戏专属目录 ──
 * 事实链：①根目录的 default.3gx **确认会被加载**（那次把主菜单卡在开机
 * logo 上，就是它在 HOME 里跑到了 gfxInitDefault）；
 * ②而 `/luma/plugins/<TID>/default.3gx` 从 21:31 放上去到现在，**一次都没
 * 被加载过**（plugin_boot.txt 从没被重写，开机也不闪色）。
 * 所以先回到唯一被证实的那条路上取数据，把"按游戏目录加载"这个变量摘掉。
 *
 * ── 为什么不画任何东西 ──
 * default.3gx 会**加载进主菜单**。上一版就是在这里调 gfxInitDefault 把整个
 * 系统卡在开机 logo 上。这一版**全程不碰 gfx**，跑完写文件就睡，对 HOME
 * 零副作用，所以放根目录是安全的。
 *
 * ── 为什么加 APT_GetProgramID ──
 * 根目录插件对**所有 title** 都加载。不加这个的话，"文件里是游戏那次的结果"
 * 还是"主菜单那次的结果"分不出来，等于白跑。现在每次都会把当前程序的
 * 低 32 位 ID 写进去：
 *     00008F02 = 主菜单     00187000 = 那个游戏
 */

#include <3ds.h>
#include <reent.h>
#include <stdio.h>

void *__service_ptr     = NULL;
u32   __system_runflags = 0;
u32   __apt_appid       = 0;

static struct _reent g_reent_store;
static uint32_t      g_tls_block[32];

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

/* 写一行 "tag k=v k=v"，全部十六进制，不依赖 printf */
static void mark2(const char *tag, const char *k1, unsigned v1,
                  const char *k2, unsigned v2)
{
    char  b[96];
    char *p = b;

    while (*tag) *p++ = *tag++;
    *p++ = ' ';
    while (*k1) *p++ = *k1++;
    *p++ = '=';
    p = put_hex(p, v1);
    if (k2) {
        *p++ = ' ';
        while (*k2) *p++ = *k2++;
        *p++ = '=';
        p = put_hex(p, v2);
    }
    *p = 0;
    mark(b);
}

int main(void)
{
    u32    *tp = NULL;
    Result  rc;
    u64     pid = 0;

    /* ① 服务框架：mark 全靠它，必须先拿 */
    srvInit();
    fsInit();
    mark("R0 srv+fs ok");

    /* ② 当前 TP（真机转储里是 0x1FFB1000，不是我们自己的块） */
    __asm__ volatile ("mrc p15, 0, %0, c13, c0, 3" : "=r"(tp));
    mark2("R1", "tp", (unsigned)(uintptr_t)tp, "tp0", tp ? tp[0] : 0xDEADBEEFu);

    /* ③ 嫁接 TP 魔数 + reent 指针 */
    if (!tp || (uintptr_t)tp < 0x00100000u) {
        tp = g_tls_block;
        __asm__ volatile ("mcr p15, 0, %0, c13, c0, 3" :: "r"(tp) : "memory");
        mark("R2 tp bad, own block");
    } else {
        mark("R2 tp usable");
    }
    tp[0] = 0x21545624u;
    tp[1] = 0u;
    tp[2] = (uint32_t)&g_reent_store;
    tp[3] = 0xFFFFFFF8u;
    ((unsigned char *)tp)[24] = 0;
    _REENT_INIT_PTR(&g_reent_store);
    mark("R3 tls grafted");

    /* ④ 真调一次 newlib —— 这是唯一能证明 reent 可用的办法 */
    {
        char buf[32];
        int  n = snprintf(buf, sizeof(buf), "newlib %d", 42);
        mark2("R4", "snprintf", (unsigned)n, NULL, 0);
    }

    /* ⑤ 当前运行的是哪个 title？主菜单 00008F02 / 游戏 00187000 */
    mark("R7 apt begin");   /* 若最终停在 R7，说明 aptInit 卡住了 */
    rc = aptInit();
    if (R_SUCCEEDED(rc))
        rc = APT_GetProgramID(&pid);
    mark2("R8 title", "aptrc", (unsigned)rc, "pid", (unsigned)(pid & 0xFFFFFFFFu));

    /* ⑥ 游戏是不是已经独占 gsp::Gpu？这就是最大的嫌疑。
     *
     * ★★ 2026-11-13 22:10 真机实测：**gspInit() 不返回**。
     *   痕迹文件停在 R8（aptrc=0，说明前面的服务/TLS/newlib/APT 全好），
     *   R9 一行永远没写出来 → 卡在 gspInit 里面。
     *   机理：`gsp::Gpu` 是**单会话独占**服务，游戏进程已经握着它，
     *   同一进程再要一次 → srv 无限等待。
     *   ⇒ 结论：**插件里永远不能调 gfxInitDefault()/gspInit()**，
     *     所以"往帧缓冲直接画"这条路必须是别的方式。
     *   想跳过这一步编译：`-DPROBE_SKIP_GSP`。 */
#ifndef PROBE_SKIP_GSP
    rc = gspInit();
    mark2("R9 gsp", "rc", (unsigned)rc, "pid", (unsigned)(pid & 0xFFFFFFFFu));
#else
    mark2("RB no-gsp", "pid", (unsigned)(pid & 0xFFFFFFFFu), NULL, 0);
#endif

    /* ⑦ 不碰 gfx，原地睡。对主菜单零副作用。
     * ★ 这里**故意不再写任何 mark** —— 文件里剩下的那一行必须是 R9
     *   （它带着 gsp 返回值和 title ID，是这次要的两条数据）。
     *   多写一行 "done" 就把它们盖掉了，等于白跑。 */
    for (;;) svcSleepThread(1000000000LL);
}
