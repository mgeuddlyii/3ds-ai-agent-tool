/*
 * pkg_cia.c -- 把 .cia 装到主菜单（见 pkg.h 的能力边界）
 *
 * ── 这个文件和 pkg.c 为什么必须分开 ──
 * 表面上两者都是"装东西"，实际是两件不同的事：
 *
 *   pkg.c     搬文件。判据是 SD 卡写不写得进去，出错最多是"没装上"。
 *   pkg_cia.c 跟系统的 am 服务打交道。它会写系统的 import.db / title.db，
 *             出错可能留下**装到一半的 title**（占着主菜单位置、点开报错）。
 *
 * 后者的失败模式严重得多，所以它需要自己的一套收拾逻辑（Cancel），
 * 而不是复用"删掉 .tmp 就干净了"。混在一起写，只会让两者的护栏互相污染。
 *
 * ── 为什么必须分块写，不能一次性读进内存 ──
 * .cia 可以很大（本机 PKG_CIA_MAX_BYTES 设 512MB，实际游戏常见 1-4GB）。
 * 3DS 的 Application 模式只有 64MB 线性堆，一次读进来必然失败。
 * 所以是 fopen → 4KB 块 → svcWriteMemory 到 am 给的句柄。
 *
 * ── 写入句柄是什么 ──
 * AM_StartCiaInstall 返回的 Handle 是一个"虚拟文件句柄"，写到它的字节
 * 会由 am 服务直接落到 SD 上的 content 区。它**不是**普通文件描述符，
 * 所以不能用 fwrite；要用 svcWriteMemory(句柄, ...) 那一套。
 *
 * ⚠️ Azahar 上这个返回值是**不可信**的（它不检查调用者 exheader，
 *    对谁都返成功，而且返回的 handle 是模拟编号不是内核 handle）。
 *    凡"能不能"的结论都必须在真机上取。
 */

#include "pkg.h"

#include "sha256.h"
#include "util.h"

#include <3ds.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#define CIA_DIR  "sdmc:/3ds/aiconsole/pkgs"
#define CIA_EXT  ".cia"

/* .cia 可以比 .3dsx 大得多（一个 3DS 游戏常见 1-4GB）。
 * 但这个上限仍然要有一道 —— 它是"卡是不是要满了"的第一道预警。
 * 与 PKG_MAX_BYTES(32MB，给 .3dsx 用) 是**两个不同的数**，别合并：
 * 合并成 32MB 会让正常游戏装不进来，合并成 512MB 则让坏包把卡写穿。 */
#define PKG_CIA_MAX_BYTES (512 * 1024 * 1024)

/* 每次写给 am 的块大小。4KB 是一个权衡：
 * 太小 → 系统调用次数爆炸（1GB 要 26 万次）；
 * 太大 → 要一块大缓冲，而我们的堆在 Application 模式下很紧。 */
#define CIA_CHUNK 4096

const char *pkg_cia_src_dir(void) { return CIA_DIR; }

/* --------------------------------------------------------------------------
 * 包名校验 —— 与 pkg.c 的 pkg_name_ok 是**同一条判据**，故意各留一份。
 * 理由同 pkg.c 里写的：两处将来会朝不同方向长（.cia 这边可能要允许
 * 版本号后缀、"v1.2.3"这种），共用一份会让改一边时悄悄松掉另一边。
 * 要防的只有一件事：**路径穿越**。
 * ----------------------------------------------------------------------- */
bool pkg_cia_name_ok(const char *n)
{
    size_t len;

    if (!n || !*n)
        return false;

    len = strlen(n);
    if (len > PKG_NAME_MAX)
        return false;

    for (const char *p = n; *p; ++p) {
        unsigned char c = (unsigned char)*p;

        if (c < 0x20)
            return false;
        switch (c) {
            case '/': case '\\': case ':':
            case '*': case '?': case '"':
            case '<': case '>': case '|':
                return false;
            default:
                break;
        }
    }

    if (!strcmp(n, ".") || !strcmp(n, ".."))
        return false;

    return true;
}

/* --------------------------------------------------------------------------
 * 源路径拼接 —— 第 4 道闸的落点。
 * 与 pkg.c 的 pkg_src_path 同一个形状：拼完核对返回值（snprintf 静默截断
 * 会拼出**另一个**路径），再核对前缀（万一校验被改松，这里能兜住）。
 * 两条独立防线，不靠同一处判断。
 * ----------------------------------------------------------------------- */
bool pkg_cia_src_path(const char *name, char *dst, size_t cap)
{
    char buf[192];
    int  n;

    if (!dst || cap == 0)
        return false;
    dst[0] = '\0';

    if (!pkg_cia_name_ok(name))
        return false;

    if (strlen(name) >= strlen(CIA_EXT) &&
        !strcmp(name + strlen(name) - strlen(CIA_EXT), CIA_EXT))
        n = snprintf(buf, sizeof buf, "%s/%s", CIA_DIR, name);
    else
        n = snprintf(buf, sizeof buf, "%s/%s" CIA_EXT, CIA_DIR, name);

    if (n < 0 || n >= (int)sizeof buf)
        return false;
    if (strncmp(buf, CIA_DIR, strlen(CIA_DIR)) != 0 ||
        buf[strlen(CIA_DIR)] != '/')
        return false;

    snprintf(dst, cap, "%s", buf);
    return true;
}

/* --------------------------------------------------------------------------
 * 形态自检
 *
 * 为什么要有这个函数：3dsx 形态下 Luma 会把服务表覆盖掉，am:u 拿不到。
 * 如果直接去调 AM_StartCiaInstall，用户看到的是"装失败"，而真正的原因
 * 是"你运行的是开发用的那个形态"。这两句话对用户的意义完全不同 ——
 * 前者听起来像包坏了，后者才是实情。
 *
 * 所以这里先探一次 amInit，探不到就明确把原因说出来。
 * 注意：**只做 amInit，不做任何写操作** —— 它是给 UI 判断"要不要显示
 * 这个入口"用的，必须没有副作用。
 * ----------------------------------------------------------------------- */
bool pkg_cia_available(void)
{
    Result r = amInit();

    if (R_FAILED(r))
        return false;
    amExit();
    return true;
}

/* 查大小 —— 与 pkg.c 的 pkg_size 同样**故意各留一份**（查的目录/后缀不同）。
 * 确认屏要用它把"这个包多大"画在用户眼前，所以它必须能在**没权限**
 * 的情况下也能工作（只读 stat，不碰 am 服务）。 */
bool pkg_cia_size(const char *name, int *out_bytes)
{
    char        p[192];
    struct stat st;

    if (!pkg_cia_src_path(name, p, sizeof p))
        return false;
    if (stat(p, &st) != 0)
        return false;
    if (out_bytes)
        *out_bytes = (int)st.st_size;
    return true;
}

/* --------------------------------------------------------------------------
 * 安装本体
 * ----------------------------------------------------------------------- */

/* 固定 4KB 缓冲。不放在栈上：3DS 的线程栈不宽裕，而这个是"每次调用都在"，
 * 放静态区比放栈上安全（放栈上要么撑栈，要么得小心调用深度）。 */
static unsigned char g_cia_buf[CIA_CHUNK];

PkgState pkg_install_cia(const char *name, const char *want_sha256,
                         char *msg, size_t cap)
{
    char   src[192];
    char   hex[65];
    FILE  *f = NULL;
    Handle h = 0;
    Result r;
    int    sz = 0;
    long   sent = 0;
    PkgState st = PKG_OK;
    struct stat stt;

    if (!msg || cap == 0)
        return PKG_ERR_IO;
    msg[0] = '\0';

    /* ---- 1. 名字 ---- */
    if (!pkg_cia_name_ok(name)) {
        snprintf(msg, cap, "包名不合法，已拦下：%.40s", name ? name : "(空)");
        return PKG_ERR_NAME;
    }
    if (!pkg_cia_src_path(name, src, sizeof src)) {
        snprintf(msg, cap, "路径太长或跑出沙箱了，已拦下");
        return PKG_ERR_NAME;
    }

    /* ---- 2. 源在不在、大小合不合理 ---- */
    if (stat(src, &stt) != 0) {
        snprintf(msg, cap, "在 pkgs/ 里找不到这个 .cia —— 先把它放进去");
        return PKG_ERR_NO_FILE;
    }
    sz = (int)stt.st_size;
    if (sz <= 0) {
        snprintf(msg, cap, "这个包是 0 字节，不像一个程序");
        return PKG_ERR_EMPTY;
    }
    if (sz > PKG_CIA_MAX_BYTES) {
        snprintf(msg, cap, "包有 %d 字节，超过 512MB 上限", sz);
        return PKG_ERR_TOO_BIG;
    }

    /* ---- 3. 摘要（最要紧的一条）----
     * 装进主菜单的东西**下次会被系统加载执行**，比装 .3dsx 还要紧
     * （.3dsx 至少还隔着 Launcher 一层）。摘要不符 = 包在路上被改过，
     * 必须拒绝，不是"警告一下继续"。判据与 ota.c 用同一份 sha256_file。 */
    if (want_sha256 && *want_sha256) {
        if (!sha256_file(src, hex)) {
            snprintf(msg, cap, "读不出这个包，没法核摘要");
            return PKG_ERR_IO;
        }
        if (strcmp(hex, want_sha256) != 0) {
            snprintf(msg, cap,
                     "摘要对不上，已拒绝安装。期望 %.16s…，实际 %.16s…",
                     want_sha256, hex);
            return PKG_ERR_HASH;
        }
    }

    /* ---- 4. 权限（形态）----
     * 这一步失败时给出**可行动**的说明，而不是一个错误码了事。
     * 见 pkg_cia_available 的注释：这多半不是包的问题。 */
    r = amInit();
    if (R_FAILED(r)) {
        snprintf(msg, cap,
                 "拿不到 am 服务（rc=0x%08lX）—— 说明当前运行的是 3dsx 形态。"
                 "装 .cia 需要用 CIA 形态的本程序（app/AIConsole.cia）。",
                 (unsigned long)r);
        return PKG_ERR_NO_PERM;
    }

    f = fopen(src, "rb");
    if (!f) {
        amExit();
        snprintf(msg, cap, "打不开源文件 %.60s", src);
        return PKG_ERR_IO;
    }

    /* ---- 5. 开一个安装上下文 ----
     * MEDIATYPE_SD：装到 SD 卡（主菜单位置）。**
     * 不用 MEDIATYPE_NAND** —— 往 NAND 写是这台机器上最不该做的事，
     * 写坏了机器起不来。这条和 SAFETY_DESIGN.md 的"全程不写 NAND"一致。 */
    r = AM_StartCiaInstall(MEDIATYPE_SD, &h);
    if (R_FAILED(r) || !h) {
        fclose(f);
        amExit();
        snprintf(msg, cap,
                 "系统拒绝了安装请求（rc=0x%08lX）。"
                 "若是权限被拒，说明本程序这个形态没有 am:u。",
                 (unsigned long)r);
        return PKG_ERR_NO_PERM;
    }

    /* ---- 6. 分块写 ----
     * AM_StartCiaInstall 返回的是一个**文件句柄**（由 am 打开，指向
     * 系统为该 title 准备的 content 区），所以用 FSFILE_Write 写。
     *
     * 这里是本文件最容易写错的一处，三个细节都别省：
     *   · **偏移必须自己累加**。这不是"追加写"的流式接口，
     *     签名是 FSFILE_Write(handle, &written, offset, buf, size, flags) ——
     *     传 0 就会每块都覆盖开头，装出来的包必然损坏。
     *   · **written 可能小于 size**，所以按 written 推进偏移，
     *     不能想当然地 += got（那样会在部分写时跳字节）。
     *   · 加 FS_WRITE_FLUSH：CIA 动辄几百 MB，攒着不刷会让失败点
     *     离报错点很远，而这里最需要的就是"失败时能报出写到哪了"。
     *
     * 依据：libctru fs.h 的 FSFILE_Write 签名 + am.h 对 AM_StartCiaInstall
     * "a handle to write CIA data to" 的说明。*/
    while (!feof(f)) {
        size_t got = fread(g_cia_buf, 1, CIA_CHUNK, f);
        u32    written = 0;

        if (got == 0)
            break;

        r = FSFILE_Write(h, &written, (u64)sent, g_cia_buf,
                         (u32)got, FS_WRITE_FLUSH);
        if (R_FAILED(r)) {
            st = PKG_ERR_IO;
            /* 报出"写到第几字节"而不是只报错误码：
             * 大包失败时，卡满 / 写保护 / 内容损坏 的概率完全不同，
             * 进度是区分它们最直接的线索。 */
            snprintf(msg, cap, "写到第 %ld 字节时失败（rc=0x%08lX）",
                     sent, (unsigned long)r);
            break;
        }
        if (written == 0) {
            /* 没报错但一个字节没写 —— 继续循环会变成死循环。
             * 宁可在这里停住并说清楚，也不要卡住整台机器。 */
            st = PKG_ERR_IO;
            snprintf(msg, cap, "写到第 %ld 字节时系统不再接受数据（写入 0 字节）",
                     sent);
            break;
        }
        sent += (long)written;
    }

    fclose(f);

    /* ---- 7. 收拾 ----
     * 走到这里只有两种可能：写完了，或者中途失败。
     * 失败的路径**必须走 Cancel** —— 否则那个 install context 会以
     * "装到一半的 title"的形式留在 import.db 里，占着主菜单位置、
     * 点开报错，而用户根本不知道它从哪来的。 */
    if (st != PKG_OK) {
        Result cr = AM_CancelCIAInstall(h);
        /* 长度跟着 cap 走，不写死160：调用方传多大都不截断。
         * 写死一个小常数会让长消息（包名 + 摘要那种）在拼接前就被砍掉
         * 一半，而砍掉哪一半看不出来 —— 报错信息半截最更难查。
         * cap 已在本函数开头校验过 != 0，所以 VLA 合法。 */
        char   why[cap];

        amExit();

        /* 先把已有的失败原因**拷出来**再拼新消息。
         * 直接 snprintf(msg, cap, "%s…", msg) 是往同一个缓冲区里
         * 边读边写 —— C 标准说这是未定义行为，gcc 会报 -Wrestrict。
         * 这里之前就踩了，留着它等于把"报错信息可能乱码"变成常态。 */
        snprintf(why, sizeof why, "%s", msg);

        /* 连 Cancel 都失败——如实说，别假装收拾干净了 */
        if (R_FAILED(cr))
            snprintf(msg, cap,
                     "%s。撤回也失败了（rc=0x%08lX），"
                     "如有残留请用 FBI 删掉那个 title。",
                     why, (unsigned long)cr);
        else
            snprintf(msg, cap, "%s。已撤回，没留下残留。", why);
        return st;
    }

    /* ---- 8. 定稿 ----
     * 这一步才让 title 真正进 title.db、出现在主菜单。 */
    r = AM_FinishCiaInstall(h);
    if (R_FAILED(r)) {
        Result cr = AM_CancelCIAInstall(h);
        amExit();
        if (R_FAILED(cr))
            snprintf(msg, cap,
                     "定稿失败（rc=0x%08lX），撤回也失败（rc=0x%08lX），"
                     "请用 FBI 检查有没有残留。",
                     (unsigned long)r, (unsigned long)cr);
        else
            snprintf(msg, cap,
                     "定稿失败（rc=0x%08lX），已撤回，没留下残留。",
                     (unsigned long)r);
        return PKG_ERR_IO;
    }

    amExit();

    /* 如实说三件事：装了多少字节、出现在哪、以及**下一步要用户自己做什么**。
     * 不许写"已启动" —— 本程序没有 chainload 权限，说得像会启动就是骗人。 */
    snprintf(msg, cap,
             "已装好 %d 字节的包。回到主菜单就能看到它了"
             "（本程序不能替你启动它，得你自己点）。", sz);
    return PKG_OK;
}
