/*
 * stage1.c —— 只验「架构」的一版插件，不画任何东西
 *
 * ── 要验的两件事（22:10 真机定因后的假设）──
 * A. `main()` **必须返回**。Luma 用 `blx r5` 调进来，boot.s 原来在 `bl main`
 *    之后原地打转，等于焊死加载器线程 → 游戏卡在开机 logo。
 * B. 真正干活的必须是自己 `svcCreateThread` 起的线程，且**新线程要自己补 TLS**
 *    （裸 svcCreateThread 没有 libctru 的 ThreadVars，第一次 snprintf 就会
 *     在 __syscall_getreent 里 svcBreak）。
 *
 * ── 为什么这版完全不碰显示 ──
 * 目的就是先把 A/B 单独验干净。真机上显示要走 LCD 寄存器
 * （0x10400400/+0x68…），万一那块地址在游戏进程里没映射，会直接
 * data abort 把游戏打崩 —— 那会把 A/B 的结论一起污染掉。
 * 分两步走，每步只引入一个变量。
 *
 * 痕迹文件仍然是 sdcard:/3ds/aiconsole/plugin_boot.txt，覆盖写，
 * 最后剩下哪一行就是"最后活着走到的步骤"。
 */

#include <3ds.h>
#include <reent.h>
#include <stdio.h>

void *__service_ptr     = NULL;
u32   __system_runflags = 0;
u32   __apt_appid       = 0;

static struct _reent g_reent_store;
static u32           g_worker_stack[4096];   /* 16KB，CTRComposer 要求 ≥16KB */
static Handle        g_worker;

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

static void mark2(const char *tag, const char *k, unsigned v)
{
    char  b[80];
    char *p = b;

    while (*tag) *p++ = *tag++;
    *p++ = ' ';
    while (*k) *p++ = *k++;
    *p++ = '=';
    p = put_hex(p, v);
    *p = 0;
    mark(b);
}

/* ---------------- 新线程的 TLS ---------------- */

/* 裸 svcCreateThread 建出来的线程，TP 那块东西是空的/垃圾。
 * 这里按 CTRComposer 记的三件套补上：
 *   [0] = 魔数（__syscall_getreent 要校验它，不对就 svcBreak）
 *   [2] = 自己那块 reent（TP+8 的地址）
 *   [4] = fs_magic = 0 → 用全局的 fs 会话（我们 fsInit 过了）
 * 少任何一项，线程里第一次 snprintf/fopen 就崩，且崩点看起来毫无关联。 */
static void worker_tls(void)
{
    u32 *tv = NULL;

    __asm__ volatile ("mrc p15, 0, %0, c13, c0, 3" : "=r"(tv));
    if (!tv || (uintptr_t)tv < 0x00100000u) {
        mark("A3x tls seed FAILED (bad tp)");   /* 这一步没做，后面必崩 */
        return;
    }

    tv[0] = 0x21545624u;
    tv[1] = 0u;
    tv[2] = (u32)(uintptr_t)&g_reent_store;
    tv[3] = 0xFFFFFFF8u;
    tv[4] = 0u;                       /* fs_magic = 0：走全局 fs 会话 */
    ((unsigned char *)tv)[24] = 0;
}

static void worker_main(void *arg)
{
    char buf[32];

    (void)arg;

    worker_tls();
    mark("A3 worker tls ok");

    /* 真调一次 newlib —— 过了这步才算 TLS 真的可用 */
    snprintf(buf, sizeof(buf), "w%d", 1);
    mark("A4 worker newlib ok");

    /* 活着就行，不再写痕迹（免得把结论那一行盖掉） */
    for (;;) svcSleepThread(1000000000LL);
}

int main(void)
{
    Result rc;
    u32    hdr_magic;

    /* ① 服务框架（mark 靠它） */
    srvInit();
    fsInit();
    mark("A0 srv+fs ok");

    /* ② Luma 在 0x07000000 注入 PluginHeader。我们不依赖它（不知道魔数
     *    具体值），但把 magic 记下来，以后要用它拿 heapVA 就有据可依。 */
    hdr_magic = *(volatile u32 *)0x07000000u;
    mark2("A1 hdr", "magic", hdr_magic);

    /* ③ 起工作线程，然后**立刻返回** —— 这是本次要验的核心 */
    rc = svcCreateThread(&g_worker, (ThreadFunc)worker_main, 0,
                         g_worker_stack + sizeof(g_worker_stack) / 4,
                         0x30, -1);
    mark2("A2 thread", "rc", (unsigned)rc);

    /* ★ 这一行是给 22:24 那次崩溃加的探针。
     * 如果痕迹停在这里就崩了，说明 crash 发生在"返回加载器"这最后一步
     * （boot.s 的现场还原）；如果出现 A3/A4，说明返回没问题、问题在工作线程。 */
    mark("A2b returning");

    return 0;
}
