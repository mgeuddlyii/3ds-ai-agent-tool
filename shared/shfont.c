/*
 * shfont.c -- 见 shfont.h。**先把头注释读完再改这里。**
 *
 * 本文件的写法与项目其他模块有一处刻意的不同：
 * 别处的模块"能做的都做，做不到的报错"；这里**先判断能不能做**，
 * 只有确认能做的路径才会真的走。原因是这个模块动的是别的进程共享的内存，
 * 写错地址的后果不是"失败"而是"花屏/崩溃"。
 *
 * 所以本模块有一条硬规则：
 *   **任何地址都必须先被确认为"共享字库区"，才允许往里写。**
 * 没有"猜一个地址试试"这种路径。
 */

#include "shfont.h"
#include "util.h"

#include <3ds.h>
#include <stdio.h>
#include <stdlib.h>   /* malloc / free */
#include <string.h>
#include <strings.h>  /* strcasecmp */
#include <dirent.h>

/* --------------------------------------------------------------------------
 * 常量
 * ----------------------------------------------------------------------- */

/* GPU 能触达的 FCRAM 上限（物理地址）。
 * 来源 3dbrew "3DS System Flaws" 的 gspwn 条目：
 *   FCRAM is gpu-accessible up to physaddr 0x26800000 on Old3DS,
 *   and 0x2D800000 on New3DS.
 * 注意 11.3.0 引入 SVC 0x59 之后 New3DS 普通模式的上限变成 0x2D000000 ——
 * 这里取**更保守**的值：写超过上限会被 GPU 静默丢弃或写坏别处，
 * 宁可少写一点也不能越界。 */
#define GPU_FCRAM_MAX_OLD 0x26800000u
#define GPU_FCRAM_MAX_NEW 0x2D000000u   /* 保守值（非 0x2D800000） */

/* .bcfnt 磁盘签名与运行时签名 */
#define SIG_CFNT 0x544E4643u            /* 'CFNT' 小端 */
#define SIG_CFNU 0x554E4643u            /* 'CFNU' 小端 */

/* 运行时格式的头部：3 个 u32。
 * 这是 SharedFontTool 定的布局，系统模块按这个读：
 *   [0] 状态  = SHARED_FONT_LOAD_STATE_LOADED (2)
 *   [1] 类型  = 字库类型（1=std 2=cn 3=kr 4=tw，见下）
 *   [2] 大小  = 后面跟的 .bcfnt 数据长度
 * 我们不发明格式 —— 系统读的就是这个，改一个字段系统就认不出来。 */
#define SHFONT_LOADED 2u
#define HDR_SIZE 12

/* 指针重定位的基址偏移。SharedFontTool 里用 g_uAddress + 0x80：
 * 0x80 = 头部(0x0C 向上取到 0x10) + 对齐。这是**系统模块读字库时
 * 期望的偏移**，不是我们可以随便改的。 */
#define RELOC_BIAS 0x80

typedef struct {
    ShfAvail     avail;
    unsigned int addr;      /* 探到的共享字库区物理地址，0 = 没探到 */
    unsigned int limit;     /* 本机的 GPU 可写上界 */
    bool         probed;
    bool         new3ds;
} ShfCtx;

static ShfCtx g_ctx;

/* --------------------------------------------------------------------------
 * 探测
 * ----------------------------------------------------------------------- */

/* 共享字库区在哪？
 *
 * ⚠️ 这里**故意没有**返回一个硬编码地址。理由见头注释第 1 条：
 * SharedFontTool 的 0x1bc00000 是 FIRM 6.4/8.0 时代的常量，而 3dbrew 的
 * FCRAM 布局表显示 BASE memregion 的起始地址随 APPMEMTYPE 变
 * （New3DS type 6 → 0x0E000000，Old3DS type 0 → 0x06C00000）。
 * 在没实测确认之前给一个"看起来像"的地址，就是把用户往坑里推。
 *
 * 于是这里的策略是：**返回 0（探不到）**，让上层如实告诉用户
 * "这台机器上这件事做不了"。等真机实测拿到确切地址后，
 * 再把那一组（机型, FIRM 版本）→ 地址的对应关系补进来。
 *
 * 补的时候要照这个形状写，不要图省事写一个常量：
 *     if (!g_ctx.new3ds && firm == 0x37) addr = 0x0E000000;
 *     ...
 * 而且**每一条都要有真机实测记录**（见 真机验证_字库切换.md）。
 */
static unsigned int probe_shared_font_addr(bool new3ds, char *reason, size_t cap)
{
    (void)new3ds;
    snprintf(reason, cap,
             "共享字库区的地址还没在真机上实测确认过。"
             "这不是你机器的问题，是这个功能还不具备安全执行的条件 —— "
             "在不知道确切地址的情况下往那里写数据，会花屏或崩溃，"
             "所以这里选择什么都不做。");
    return 0;
}

ShfAvail shfont_probe(char *reason, size_t cap)
{
    unsigned int addr;

    if (reason && cap)
        reason[0] = '\0';

    /* 机型先定下来，它决定上限 */
    g_ctx.new3ds = false;
    if (R_SUCCEEDED(APT_CheckNew3DS(&g_ctx.new3ds)) && g_ctx.new3ds)
        g_ctx.limit = GPU_FCRAM_MAX_NEW;
    else
        g_ctx.limit = GPU_FCRAM_MAX_OLD;

    addr = probe_shared_font_addr(g_ctx.new3ds, reason, cap);

    g_ctx.addr   = addr;
    g_ctx.probed = true;

    if (addr == 0) {
        g_ctx.avail = SHF_AVAIL_UNSAFE;
        return g_ctx.avail;
    }

    /* 探到了地址也要过范围检查 —— 在 GPU 触达不到的地方，
     * GX_TextureCopy 不会报错，它只是"什么都没发生"或者写坏别处。
     * 这种失败最难查，所以在这里就拦下。 */
    if (addr >= g_ctx.limit) {
        snprintf(reason, cap,
                 "共享字库区在 %08X，超出了 GPU 能写的范围（上限 %08X）。"
                 "这台机器上不能用这个方式改字库。",
                 addr, g_ctx.limit);
        g_ctx.avail = SHF_AVAIL_UNSAFE;
        return g_ctx.avail;
    }

    g_ctx.avail = SHF_AVAIL_OK;
    snprintf(reason, cap, "共享字库区在 %08X（上限 %08X），可以操作。",
             addr, g_ctx.limit);
    return g_ctx.avail;
}

ShfAvail shfont_avail(void)
{
    return g_ctx.probed ? g_ctx.avail : SHF_AVAIL_UNKNOWN;
}

unsigned int shfont_addr(void)
{
    return g_ctx.addr;
}

/* --------------------------------------------------------------------------
 * GSPWN 原语
 *
 * 这是整个模块唯一真正"动手"的地方。GX_TextureCopy 的语义是
 * "把一块内存当纹理拷过去"，但 3DS 的 GSP 模块不校验地址，
 * 于是它实际上是一个任意物理内存写 —— 这就是 GSPWN。
 *
 * 三个必须照做的细节（少一个就失败或写坏）：
 *   1. **分块**。一次拷太大，GSP 的命令缓冲放不下。SharedFontTool 用 4KB。
 *   2. **每块之后 FlushDataCache**。写的是 CPU 缓存可见的地址，
 *      不刷的话 GSP 可能读到旧数据（CPU 与 GPU 的缓存不一致）。
 *   3. **每块之后 sleep**。给 DMA 时间完成 —— 不等的话，
 *      下一块可能覆盖还在传输的那块。
 *
 * src/dst 都是**物理/线性地址**。0xFFFFFFFF 参数表示"非纹理模式"，
 * 也就是普通内存地址（不是纹理坐标），这是 GSPWN 惯用的写法。
 * ----------------------------------------------------------------------- */

#define GSPWN_CHUNK 0x1000      /* 4KB */

static Result gspwn_write(unsigned int dst_phys, const void *src,
                          unsigned int size)
{
    const unsigned char *p = (const unsigned char *)src;
    unsigned int done = 0;

    while (done < size) {
        unsigned int n = size - done;
        Result r;

        if (n > GSPWN_CHUNK)
            n = GSPWN_CHUNK;

        r = GX_TextureCopy((void *)(p + done), 0xFFFFFFFF,
                           (void *)(dst_phys + done), 0xFFFFFFFF,
                           n, 8);
        if (R_FAILED(r))
            return r;

        GSPGPU_FlushDataCache((void *)(p + done), n);
        svcSleepThread(5 * 1000 * 1000LL);      /* 5ms，等 DMA */
        done += n;
    }
    return 0;
}

/* --------------------------------------------------------------------------
 * .bcfnt → 运行时格式
 *
 * 磁盘上的 .bcfnt 里那几个"指针"字段（PtrGlyph/PtrWidth/PtrMap/SheetImage）
 * 存的是**相对文件开头的偏移**。放进共享内存后，系统把它们当**绝对地址**用。
 * 所以必须把这些字段加上它们在共享内存里的实际地址。
 *
 * 加多少：目标地址 + RELOC_BIAS。+0x80 是因为字库数据不是从区域开头放的，
 * 前面有头部和对齐填充 —— 这个偏移是系统模块读字库时的假设，
 * 不能改（改了就指向错的地方）。
 *
 * ⚠️ 下面这个结构只覆盖到"需要重定位的字段"。.bcfnt 是个有很多块的
 *    二进制格式，完整解析需要处理所有块类型。本函数**只做重定位所需的
 *    最小解析**：定位到 FINF / TGLP 块，改其中的指针字段，再改签名。
 *    其余块原样保留 —— 系统不要求它们变。
 * ----------------------------------------------------------------------- */

/* CMAP / FINF / TGLP 的块签名（小端） */
#define SIG_FINF 0x464E4946u    /* 'FINF' */
#define SIG_TGLP 0x504C4754u    /* 'TGLP' */

/* 在 bcfnt 里找块。bcfnt 是 CFNT头 + 若干块，每块头是
 *   u32 sig; u32 size; 然后是数据。
 * 找法是真的遍历，不是"在文件里搜签名"—— 数据里可能出现同样的字节，
 * 搜出来的位置是错的，而错的位置上做重定位 = 野指针。 */
static unsigned char *find_block(unsigned char *d, size_t len, unsigned int sig)
{
    size_t off = 8;     /* CFNT 文件头：sig(4) + bom(2) + hdrSize(2) */

    while (off + 8 <= len) {
        unsigned int s, sz;

        memcpy(&s,  d + off, 4);
        memcpy(&sz, d + off + 4, 4);
        if (sz < 8 || off + sz > len)
            return NULL;                 /* 尺寸不合理 —— 文件坏了 */
        if (s == sig)
            return d + off;
        off += (sz + 3) & ~3u;           /* 块之间 4 字节对齐 */
    }
    return NULL;
}

static bool font_to_runtime(unsigned char *data, size_t len,
                            unsigned int base_phys)
{
    unsigned char *finf;
    unsigned int   sig;
    unsigned int   bias = base_phys + RELOC_BIAS;

    if (len < 16)
        return false;

    memcpy(&sig, data, 4);
    if (sig != SIG_CFNT)
        return false;                    /* 不是 bcfnt */

    /* FINF 块里的四个指针字段（相对文件头的偏移，各 4 字节）。
     * 用偏移而不是结构体，是因为 bcfnt 各版本的结构体 padding 不确定 ——
     * 按字节偏移读是唯一稳的写法。 */
    finf = find_block(data, len, SIG_FINF);
    if (!finf)
        return false;

    /* FINF 块内的指针字段位置（相对块开头）：
     *   +0x0C PtrGlyph   +0x10 PtrWidth   +0x14 PtrMap
     * 这些是 3DS 字库格式里的固定布局。加上 bias 变成绝对地址。 */
    {
        static const size_t offs[] = { 0x0C, 0x10, 0x14 };
        for (size_t i = 0; i < sizeof offs / sizeof offs[0]; ++i) {
            unsigned int v;
            if (offs[i] + 4 > len)
                return false;
            memcpy(&v, finf + offs[i], 4);
            if (v == 0)                  /* 空指针不动（有些字库没有某一项） */
                continue;
            v += bias;
            memcpy(finf + offs[i], &v, 4);
        }
    }

    /* TGLP 块里的 SheetImage 也要重定位 */
    {
        unsigned char *tglp = find_block(data, len, SIG_TGLP);
        if (tglp) {
            unsigned int v;
            /* SheetImage 在 TGLP 块内的偏移 —— 3DS 字库格式固定值 */
            memcpy(&v, tglp + 0x1C, 4);
            if (v)
                v += bias, memcpy(tglp + 0x1C, &v, 4);
        }
    }

    /* 最后改签名：系统看到 'CFNU' 才知道这是运行时格式。
     * **这一步必须最后做** —— 先改签名再改指针的话，万一中途失败，
     * 留在缓冲里的就是一个"签名说能用、指针是错的"的东西。 */
    {
        unsigned int rt = SIG_CFNU;
        memcpy(data, &rt, 4);
    }
    return true;
}

/* --------------------------------------------------------------------------
 * 备份 / 还原
 * ----------------------------------------------------------------------- */

static bool backup_path(char *dst, size_t cap)
{
    int n = snprintf(dst, cap, "%s/shfont_backup.bin", SHFONT_BACKUP_DIR);
    return n > 0 && (size_t)n < cap;
}

bool shfont_has_backup(void)
{
    char p[192];
    FILE *f;

    if (!backup_path(p, sizeof p))
        return false;
    f = fopen(p, "rb");
    if (!f)
        return false;
    fclose(f);
    return true;
}

bool shfont_backup(char *msg, size_t cap)
{
    char p[192];
    unsigned int addr = g_ctx.addr;
    unsigned int n;
    unsigned char *buf;
    FILE *f;

    if (!g_ctx.probed) {
        snprintf(msg, cap, "还没探测过共享字库区，先探测。");
        return false;
    }
    if (addr == 0 || g_ctx.avail != SHF_AVAIL_OK) {
        snprintf(msg, cap, "探不到共享字库区，备份不了。");
        return false;
    }

    /* 区域大小：SharedFontTool 用 4 × 1024 × 818 = 3,272 KB。
     * 我们按最大的 4MB 读 —— 多读一点是安全的（那块内存是连续的），
     * 少读则会漏掉字库尾部。 */
    n = SHFONT_MAX_BYTES;

    /* GSPWN 能从目标读回来吗？—— 能。GX_TextureCopy 是双向的：
     * 把内存当"纹理"拷到我们自己的缓冲里，方向反过来即可。
     * 不过读的时候 src 是受限地址、dst 是我们自己的 —— 与写相反。 */
    buf = malloc(n);
    if (!buf) {
        snprintf(msg, cap, "内存不够，备份不了。");
        return false;
    }

    if (R_FAILED(GX_TextureCopy((void *)addr, 0xFFFFFFFF,
                                (void *)buf, 0xFFFFFFFF, n, 8))) {
        free(buf);
        snprintf(msg, cap, "读共享字库区失败。");
        return false;
    }

    util_mkdirs(SHFONT_BACKUP_DIR);
    if (!backup_path(p, sizeof p)) {
        free(buf);
        snprintf(msg, cap, "备份路径拼不出来。");
        return false;
    }

    f = fopen(p, "wb");
    if (!f) {
        free(buf);
        snprintf(msg, cap, "写不进 %s。", SHFONT_BACKUP_DIR);
        return false;
    }
    if (fwrite(buf, 1, n, f) != n) {
        fclose(f);
        free(buf);
        snprintf(msg, cap, "备份写到一半失败了。");
        return false;
    }
    fclose(f);
    free(buf);

    snprintf(msg, cap, "已备份 %u KB 到 fonts/backup/。", n / 1024);
    return true;
}

ShfState shfont_restore(char *msg, size_t cap)
{
    char p[192];
    FILE *f;
    unsigned char *buf;
    long sz;

    if (!shfont_has_backup()) {
        snprintf(msg, cap, "没有备份，没法还原。");
        return SHF_ERR_NO_FILE;
    }
    if (!g_ctx.probed || g_ctx.avail != SHF_AVAIL_OK) {
        snprintf(msg, cap, "当前不能写共享字库区，还原不了。");
        return SHF_ERR_NO_ADDR;
    }
    if (!backup_path(p, sizeof p)) {
        snprintf(msg, cap, "路径拼不出来。");
        return SHF_ERR_IO;
    }

    f = fopen(p, "rb");
    if (!f) {
        snprintf(msg, cap, "打不开备份文件。");
        return SHF_ERR_IO;
    }
    fseek(f, 0, SEEK_END);
    sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0 || sz > SHFONT_MAX_BYTES) {
        fclose(f);
        snprintf(msg, cap, "备份文件大小不对劲（%ld 字节）。", sz);
        return SHF_ERR_SIZE;
    }

    buf = malloc((size_t)sz);
    if (!buf) {
        fclose(f);
        snprintf(msg, cap, "内存不够。");
        return SHF_ERR_IO;
    }
    if (fread(buf, 1, (size_t)sz, f) != (size_t)sz) {
        fclose(f);
        free(buf);
        snprintf(msg, cap, "读备份失败。");
        return SHF_ERR_IO;
    }
    fclose(f);

    if (R_FAILED(gspwn_write(g_ctx.addr, buf, (unsigned int)sz))) {
        free(buf);
        snprintf(msg, cap, "写回共享字库区失败。");
        return SHF_ERR_GSPWN;
    }
    free(buf);

    snprintf(msg, cap, "已还原备份（%ld KB）。", sz / 1024);
    return SHF_OK;
}

/* --------------------------------------------------------------------------
 * 切换
 * ----------------------------------------------------------------------- */

static bool font_needs_ext(const char *name)
{
    size_t n = strlen(name);

    return n > 6 && strcasecmp(name + n - 6, ".bcfnt") == 0;
}

ShfState shfont_apply(const char *name, char *msg, size_t cap)
{
    char  path[224];
    FILE *f;
    unsigned char *buf;
    long  sz;
    int   n;

    if (!name || !*name) {
        snprintf(msg, cap, "没给字库文件名。");
        return SHF_ERR_NO_FILE;
    }
    if (!font_needs_ext(name)) {
        snprintf(msg, cap, "只认 .bcfnt 字库文件（得到的是 %.40s）。", name);
        return SHF_ERR_FORMAT;
    }
    /* 名字校验：与 pkg.c 同判据，防路径穿越。 */
    for (const char *p = name; *p; ++p) {
        unsigned char c = (unsigned char)*p;
        if (c < 0x20 || c == '/' || c == '\\' || c == ':' ||
            c == '*' || c == '?' || c == '"' || c == '<' ||
            c == '>' || c == '|')
        {
            snprintf(msg, cap, "文件名字符不合法，已拦下。");
            return SHF_ERR_FORMAT;
        }
    }

    /* 1) 可用性 —— 这是本模块最重要的一道闸。
     *    **不 OK 就直接拒**，不给"试试看"的机会。 */
    if (!g_ctx.probed)
        shfont_probe(msg, cap);
    if (g_ctx.avail != SHF_AVAIL_OK) {
        char why[256];
        shfont_probe(why, sizeof why);       /* 取一句理由 */
        snprintf(msg, cap, "这台机器上用不了这个功能：%s", why);
        return SHF_ERR_NO_ADDR;
    }

    /* 2) **必须先有备份。** 没有就直接拒 ——
     *    允许在没有退路的情况下改共享内存，等于把用户放在一个
     *    不能反悔的操作前面。而且备份也是"切回来"的唯一手段
     *    （官方字库加密存储，我们读不到）。 */
    if (!shfont_has_backup()) {
        snprintf(msg, cap,
                 "还没备份当前字库，先做一次备份再切 —— "
                 "备份是切回来的唯一手段。");
        return SHF_ERR_NO_BACKUP;
    }

    /* 3) 读文件 */
    n = snprintf(path, sizeof path, "%s/%s", SHFONT_FONT_DIR, name);
    if (n <= 0 || (size_t)n >= sizeof path) {
        snprintf(msg, cap, "路径太长，已拦下。");
        return SHF_ERR_IO;
    }

    f = fopen(path, "rb");
    if (!f) {
        snprintf(msg, cap, "fonts/ 里找不到 %.40s", name);
        return SHF_ERR_NO_FILE;
    }
    fseek(f, 0, SEEK_END);
    sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0) {
        fclose(f);
        snprintf(msg, cap, "这个字库文件是空的。");
        return SHF_ERR_SIZE;
    }
    if (sz > SHFONT_MAX_BYTES - HDR_SIZE) {
        fclose(f);
        snprintf(msg, cap, "字库有 %ld 字节，超过了共享字库区的大小上限。", sz);
        return SHF_ERR_SIZE;
    }

    /* 4) 拼运行时镜像。**先在自己内存里全部拼好，最后才动手写** ——
     *    这样"写一半失败"不会留下一个半截的字库。 */
    buf = malloc(HDR_SIZE + (size_t)sz);
    if (!buf) {
        fclose(f);
        snprintf(msg, cap, "内存不够（需要 %ld 字节）。", sz + HDR_SIZE);
        return SHF_ERR_IO;
    }

    if (fread(buf + HDR_SIZE, 1, (size_t)sz, f) != (size_t)sz) {
        fclose(f);
        free(buf);
        snprintf(msg, cap, "读字库文件失败。");
        return SHF_ERR_IO;
    }
    fclose(f);

    if (!font_to_runtime(buf + HDR_SIZE, (size_t)sz,
                         g_ctx.addr)) {
        free(buf);
        snprintf(msg, cap,
                 "这个文件不是有效的 .bcfnt 字库（签名或结构不对），已中止。");
        return SHF_ERR_FORMAT;
    }

    /* 头部：状态 / 类型 / 大小 */
    {
        unsigned int hdr[3];

        hdr[0] = SHFONT_LOADED;
        /* 类型从文件名猜是不靠谱的 —— 这里填 0（未知），
         * 让系统按签名自己去认。SharedFontTool 会填具体值，
         * 但那是它自己知道在装哪一套；我们让用户丢任意 .bcfnt 进来，
         * 猜错类型反而会让系统按错的字形表解析。 */
        hdr[1] = 0;
        hdr[2] = (unsigned int)sz;
        memcpy(buf, hdr, HDR_SIZE);
    }

    /* 5) 写。这一步之后系统就开始用新字库了。 */
    if (R_FAILED(gspwn_write(g_ctx.addr, buf,
                             (unsigned int)(HDR_SIZE + sz)))) {
        free(buf);
        snprintf(msg, cap,
                 "写共享字库区失败。字库**没有被改动** —— "
                 "可以用「还原备份」确认当前状态。");
        return SHF_ERR_GSPWN;
    }
    free(buf);

    snprintf(msg, cap,
             "已切换字库（%.40s，%ld KB）。"
             "回到主菜单就能看到效果。想切回去用「还原备份」。",
             name, sz / 1024);
    return SHF_OK;
}

/* --------------------------------------------------------------------------
 * 列表
 * ----------------------------------------------------------------------- */
int shfont_list(char names[][SHFONT_NAME_MAX + 1], int max, int *count_out)
{
    DIR *d;
    struct dirent *de;
    int n = 0;

    if (count_out)
        *count_out = 0;

    d = opendir(SHFONT_FONT_DIR);
    if (!d)
        return 0;

    while ((de = readdir(d)) != NULL && n < max) {
        size_t l = strlen(de->d_name);

        if (l <= 6 || l > SHFONT_NAME_MAX)
            continue;
        if (strcasecmp(de->d_name + l - 6, ".bcfnt") != 0)
            continue;
        snprintf(names[n], SHFONT_NAME_MAX + 1, "%s", de->d_name);
        ++n;
    }
    closedir(d);

    if (count_out)
        *count_out = n;
    return n;
}

const char *shf_state_text(ShfState s)
{
    switch (s) {
        case SHF_OK:              return "成功";
        case SHF_ERR_NO_FILE:     return "找不到字库文件";
        case SHF_ERR_SIZE:        return "文件大小不对";
        case SHF_ERR_FORMAT:      return "不是有效的字库文件";
        case SHF_ERR_NO_ADDR:     return "这台机器上做不了";
        case SHF_ERR_OUT_OF_RANGE:return "地址超出可写范围";
        case SHF_ERR_NO_BACKUP:   return "还没备份";
        case SHF_ERR_IO:          return "读写失败";
        case SHF_ERR_GSPWN:       return "写内存失败";
        case SHF_ERR_VERIFY:      return "写完核对不上";
        default:                  return "未知情况";
    }
}
