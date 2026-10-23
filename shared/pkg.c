/*
 * pkg.c -- 见 pkg.h（能力边界与四道闸的落点都写在那儿，改这里前先读一遍）
 */

#include "pkg.h"

#include "sha256.h"
#include "util.h"

#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#define PKG_DIR  "sdmc:/3ds/aiconsole/pkgs"
#define PKG_EXT  ".3dsx"

const char *pkg_src_dir(void)     { return PKG_DIR; }
const char *pkg_install_dir(void) { return PKG_INSTALL_DIR; }

const char *pkg_state_text(PkgState s)
{
    switch (s) {
        case PKG_OK:           return "装好了";
        case PKG_ERR_NO_FILE:  return "找不到这个包";
        case PKG_ERR_NAME:     return "包名不合法";
        case PKG_ERR_TOO_BIG:  return "包太大，超过 32MB";
        case PKG_ERR_EMPTY:    return "包是 0 字节，不像一个程序";
        case PKG_ERR_HASH:     return "摘要对不上 —— 拒绝安装";
        case PKG_ERR_IO:       return "读写失败";
        case PKG_ERR_ALREADY:  return "已经装过了";
        /* 这一条要说得足够具体：用户看到"没有权限"会去怀疑包，
         * 而实情多半是他运行的是开发用的那个形态。 */
        case PKG_ERR_NO_PERM:  return "没有安装权限（多半是当前为 3dsx 形态，"
                                      "装 .cia 要用 CIA 形态的本程序）";
        default:               return "?";
    }
}

/* --------------------------------------------------------------------------
 * 包名校验
 *
 * 与 actions.c 的 name_ok 是同一条判据，但**不能共用**：
 * 那边管的是"AI 写文件时的文件名"，这边管的是"装什么程序"。
 * 两处都各自留一份，是因为它们将来会朝不同方向长（比如这边以后要
 * 允许版本号后缀），而共用一份会让改一边时悄悄松掉另一边。
 * 真要防的只有一件事：**路径穿越**。
 * ----------------------------------------------------------------------- */
bool pkg_name_ok(const char *n)
{
    size_t len;

    if (!n || !*n)
        return false;

    len = strlen(n);
    if (len > PKG_NAME_MAX)
        return false;

    for (const char *p = n; *p; ++p) {
        unsigned char c = (unsigned char)*p;

        if (c < 0x20)                       /* 控制字符 */
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
 * 路径拼接
 *
 * 第 4 道闸的落点，与 actions.c 里那次拼路径是同一个形状：
 * 拼完必须核对返回值（snprintf 静默截断会拼出**另一个路径**），
 * 再核对前缀（万一校验哪天被改松，这里能兜住）。
 * 两条独立的防线，不靠同一处判断。
 *
 * 下面 src / dst 两个函数的尾巴几乎一样，**故意不抽公共函数**：
 * 它们校的是两个不同的前缀（PKG_DIR vs PKG_INSTALL_DIR），
 * 抽出去就会变成"校哪个前缀"由参数决定 —— 那正是最容易传错的地方。
 * 宁可多写 8 行，也不要一个能传错参数的通用版。
 * ----------------------------------------------------------------------- */
bool pkg_src_path(const char *name, char *dst, size_t cap)
{
    char buf[192];
    int  n;

    if (!dst || cap == 0)
        return false;
    dst[0] = '\0';

    if (!pkg_name_ok(name))
        return false;

    /* 源文件带 .3dsx 后缀；如果调用方已经带了就不重复加。
     * 这里允许"名字里含点"，因为 "my.tool" 是个合理的包名。 */
    if (strlen(name) >= strlen(PKG_EXT) &&
        !strcmp(name + strlen(name) - strlen(PKG_EXT), PKG_EXT))
        n = snprintf(buf, sizeof buf, "%s/%s", PKG_DIR, name);
    else
        n = snprintf(buf, sizeof buf, "%s/%s" PKG_EXT, PKG_DIR, name);

    if (n < 0 || n >= (int)sizeof buf)
        return false;
    if (strncmp(buf, PKG_DIR, strlen(PKG_DIR)) != 0 ||
        buf[strlen(PKG_DIR)] != '/')
        return false;

    snprintf(dst, cap, "%s", buf);
    return true;
}

bool pkg_dst_path(const char *name, char *dst, size_t cap)
{
    char buf[192];
    int  n;

    if (!dst || cap == 0)
        return false;
    dst[0] = '\0';

    if (!pkg_name_ok(name))
        return false;

    if (strlen(name) >= strlen(PKG_EXT) &&
        !strcmp(name + strlen(name) - strlen(PKG_EXT), PKG_EXT))
        n = snprintf(buf, sizeof buf, "%s/%s", PKG_INSTALL_DIR, name);
    else
        n = snprintf(buf, sizeof buf, "%s/%s" PKG_EXT, PKG_INSTALL_DIR, name);

    if (n < 0 || n >= (int)sizeof buf)
        return false;
    if (strncmp(buf, PKG_INSTALL_DIR, strlen(PKG_INSTALL_DIR)) != 0 ||
        buf[strlen(PKG_INSTALL_DIR)] != '/')
        return false;

    snprintf(dst, cap, "%s", buf);
    return true;
}

/* --------------------------------------------------------------------------
 * 查大小 / 核摘要
 * ----------------------------------------------------------------------- */

static bool file_size(const char *p, int *out)
{
    struct stat st;

    if (stat(p, &st) != 0)
        return false;
    if (out)
        *out = (int)st.st_size;
    return true;
}

bool pkg_size(const char *name, int *out_bytes)
{
    char p[192];

    if (!pkg_src_path(name, p, sizeof p))
        return false;
    return file_size(p, out_bytes);
}

bool pkg_verify(const char *name, const char *want_hex, char *got_hex,
                size_t got_cap)
{
    char src[192];
    char hex[65];

    if (got_hex && got_cap)
        got_hex[0] = '\0';

    if (!pkg_src_path(name, src, sizeof src))
        return false;

    /* 复用 sha256_file —— 与 OTA 校验是同一份实现。
     * "摘要对不上"这件事在两个场景里的含义完全一样，不该有两套算法。 */
    if (!sha256_file(src, hex))
        return false;

    if (got_hex && got_cap)
        snprintf(got_hex, got_cap, "%s", hex);

    if (want_hex && *want_hex)
        return strcmp(hex, want_hex) == 0;

    return true;
}

/* --------------------------------------------------------------------------
 * 安装
 *
 * 先写 .tmp 再 rename：断电时最坏是留一个 .tmp，而不会在 3ds/ 里
 * 留一个**写了一半的、能被 Launcher 扫到的**程序。
 * 这条与 ota.c 的 copy_then_rename 是同一个理由。
 * ----------------------------------------------------------------------- */

PkgState pkg_install(const char *name, const char *want_sha256,
                     char *msg, size_t cap)
{
    char   src[192], dst[192], tmp[200];
    char   hex[65];
    int    sz = 0, n;
    FILE  *a, *b;
    size_t got;
    static char chunk[4096];
    bool   ok = true;

    if (!msg || cap == 0)
        return PKG_ERR_IO;
    msg[0] = '\0';

    /* ---- 1. 名字 ---- */
    if (!pkg_name_ok(name)) {
        snprintf(msg, cap, "包名不合法，已拦下：%.40s", name ? name : "(空)");
        return PKG_ERR_NAME;
    }

    if (!pkg_src_path(name, src, sizeof src) ||
        !pkg_dst_path(name, dst, sizeof dst)) {
        snprintf(msg, cap, "路径太长或跑出沙箱了，已拦下");
        return PKG_ERR_NAME;
    }

    /* ---- 2. 源在不在、大小合不合理 ---- */
    if (!file_size(src, &sz)) {
        snprintf(msg, cap, "在 pkgs/ 里找不到这个包 —— 先把 .3dsx 放进去");
        return PKG_ERR_NO_FILE;
    }
    if (sz <= 0) {
        snprintf(msg, cap, "这个包是 0 字节，不像一个程序");
        return PKG_ERR_EMPTY;
    }
    if (sz > PKG_MAX_BYTES) {
        snprintf(msg, cap, "包有 %d 字节，超过 32MB 上限", sz);
        return PKG_ERR_TOO_BIG;
    }

    /* ---- 3. 摘要（最要紧的一条）----
     * 装进去的东西**下次会被执行**，所以这里不是"核一下更放心"，
     * 而是"核不过就不许装"。摘要不符意味着包在路上被改过 ——
     * 那正是安装这件事最需要防的。 */
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

    /* ---- 4. 落点目录 ---- */
    util_mkdirs(PKG_INSTALL_DIR);

    n = snprintf(tmp, sizeof tmp, "%s.tmp", dst);
    if (n < 0 || n >= (int)sizeof tmp) {
        snprintf(msg, cap, "目标路径太长，已拦下");
        return PKG_ERR_NAME;
    }

    /* ---- 5. 复制 ---- */
    a = fopen(src, "rb");
    if (!a) {
        snprintf(msg, cap, "打不开源文件 %.60s", src);
        return PKG_ERR_IO;
    }
    b = fopen(tmp, "wb");
    if (!b) {
        fclose(a);
        snprintf(msg, cap, "写不进 3ds/ 目录 —— 卡满了还是写保护？");
        return PKG_ERR_IO;
    }

    while ((got = fread(chunk, 1, sizeof chunk, a)) > 0) {
        if (fwrite(chunk, 1, got, b) != got) {
            ok = false;
            break;
        }
    }
    fclose(a);
    if (fclose(b) != 0)
        ok = false;

    if (!ok) {
        remove(tmp);
        snprintf(msg, cap, "复制过程中出错，已清理临时文件");
        return PKG_ERR_IO;
    }

    /* ---- 6. 就位 ---- */
    remove(dst);
    if (rename(tmp, dst) != 0) {
        /* 有些 fs devoptab 上 rename 不可用，退回复制（与 ota.c 同策） */
        FILE *s2 = fopen(tmp, "rb");
        FILE *d2 = fopen(dst, "wb");

        if (!s2 || !d2) {
            if (s2) fclose(s2);
            if (d2) fclose(d2);
            remove(tmp);
            snprintf(msg, cap, "装到 3ds/ 失败");
            return PKG_ERR_IO;
        }
        /* ★ 回退复制**必须查错**。早先是
         *       while ((got = fread(...)) > 0) fwrite(...);
         *       fclose(s2); fclose(d2);
         *   fread/fwrite/fclose 的返回值全没看，之后无条件往下走。
         *   卡满 / 坏卡时这会留下**半截文件**，而函数照样报成功 ——
         *   最坏的情况是目标正好是正在运行的 AIConsole.3dsx，
         *   下次启动才发现程序坏了，而那时已经找不到原因。
         *   现在：任一步失败就删掉半成品并返回错误号。 */
        bool cok = true;
        while ((got = fread(chunk, 1, sizeof chunk, s2)) > 0) {
            if (fwrite(chunk, 1, got, d2) != got) {
                cok = false;
                break;
            }
        }
        if (ferror(s2))
            cok = false;
        fclose(s2);
        if (fclose(d2) != 0)
            cok = false;

        if (!cok) {
            remove(dst);            /* 不留半截文件 */
            remove(tmp);
            snprintf(msg, cap, "复制到目标失败（存储可能已满），没有留下半截文件");
            return PKG_ERR_IO;
        }
        remove(tmp);
    }

    /* 如实说出两件事：装到哪、以及**下一步还得用户自己动手**。
     * 不许写成"已安装完成" —— 我们做到的是"放到该放的位置"，
     * 真正让它变成"能启动的程序"的是 Homebrew Launcher 扫到它，
     * 而那不是这里能替用户做的（见 pkg.h 的能力边界）。 */
    {
        const char *base = strrchr(dst, '/');
        base = base ? base + 1 : dst;
        snprintf(msg, cap, "已放到 3ds/%s（%d 字节）。"
                           "去 Homebrew Launcher 里就能看到它。",
                 base, sz);
    }
    return PKG_OK;
}

/* --------------------------------------------------------------------------
 * 列目录
 *
 * 为什么要这个：AI 要装东西，得先知道**卡上有什么可装**。
 * 没有这一条，"安装"这个动作就只能靠用户把名字一字不差地背给 AI。
 * ----------------------------------------------------------------------- */

int pkg_list(char names[][PKG_NAME_MAX + 1], int max, int *count_out)
{
    DIR           *d;
    struct dirent *e;
    int            n = 0;

    if (count_out)
        *count_out = 0;

    d = opendir(PKG_DIR);
    if (!d)
        return 0;

    while ((e = readdir(d)) != NULL && n < max) {
        size_t nl = strlen(e->d_name);

        if (nl <= strlen(PKG_EXT))
            continue;
        if (strcmp(e->d_name + nl - strlen(PKG_EXT), PKG_EXT) != 0)
            continue;

        /* 去掉 .3dsx 后缀再存 —— 对外的一致口径是"包名"，
         * 后缀由这里自己加，免得调用方到处判断带没带。
         *
         * ★ 这里曾是一个**越界写**：早先写成
         *       snprintf(names[n], PKG_NAME_MAX + 1, "%.*s",
         *                (int)PKG_NAME_MAX, e->d_name);
         *       names[n][nl - strlen(PKG_EXT)] = '\0';
         *   `nl` 是**原始**文件名长度，而 snprintf 已经把内容截到
         *   PKG_NAME_MAX(60) 字节。当文件名 ≥66 字符时 nl-5 ≥ 61，
         *   而缓冲只有 61 字节 → 直接写到缓冲外 5 个字节。
         *   SD 卡上 `pkgs/` 里放一个长名字的 .3dsx 就能触发；
         *   3DS 没有栈保护，这种越界是静默踩内存。
         * 现在按"截断后的长度"落终止符，并且用 memcpy 明确边界。 */
        {
            size_t keep = nl - strlen(PKG_EXT);
            if (keep > PKG_NAME_MAX)
                keep = PKG_NAME_MAX;
            memcpy(names[n], e->d_name, keep);
            names[n][keep] = '\0';
        }
        ++n;
    }

    closedir(d);

    if (count_out)
        *count_out = n;
    return n;
}
