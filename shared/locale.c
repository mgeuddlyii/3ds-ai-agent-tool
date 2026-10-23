/*
 * locale.c -- 见 locale.h（能力边界与安全约束写在那儿，改这里前先读一遍）
 */

#include "locale.h"
#include "util.h"

#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <dirent.h>

#define LOC_FILE "locale.txt"
#define LOC_CONFIG "sdmc:/luma/config.ini"

/* --------------------------------------------------------------------------
 * 白名单
 *
 * 这两张表就是 Luma3DS 认的值，一个都不能多、一个都不能少 —— 多一个
 * 它会忽略整行（然后用户看到"设了没用"），少一个用户就选不到他要的。
 * 来源：Luma3DS wiki 的 Locale Emulation 一节。
 *
 *   区域：JPN USA EUR AUS CHN KOR TWN
 *   语言：JP EN FR DE IT ES ZH KO NL PT RU TW
 *
 * 注意 CHN/TWN 是区域，ZH/TW 是语言 —— 两套码不通用，别混。
 * 例如"简体中文"要写 CHN ZH，"繁体中文"是 TWN TW。
 * ----------------------------------------------------------------------- */
static const char *const kRegions[] = {
    "JPN", "USA", "EUR", "AUS", "CHN", "KOR", "TWN",
};
static const char *const kLangs[] = {
    "JP", "EN", "FR", "DE", "IT", "ES", "ZH", "KO", "NL", "PT", "RU", "TW",
};

static bool in_list(const char *const *list, size_t n, const char *v)
{
    if (!v)
        return false;
    for (size_t i = 0; i < n; ++i)
        if (strcmp(list[i], v) == 0)
            return true;
    return false;
}

/* 转大写并校验。用自己写的循环而不是 strupr（那东西不是标准 C，
 * 而且它会就地改字符串 —— 我们这里不改调用方的缓冲区）。 */
static bool upper_norm(const char *in, char *out, size_t cap)
{
    size_t i;

    if (!in || !out || cap == 0)
        return false;
    for (i = 0; in[i]; ++i) {
        if (i + 1 >= cap)
            return false;               /* 放不下就拒，不截断 */
        out[i] = (char)toupper((unsigned char)in[i]);
    }
    out[i] = '\0';
    return i > 0;
}

bool loc_region_ok(const char *region)
{
    return in_list(kRegions, sizeof kRegions / sizeof kRegions[0], region);
}

bool loc_lang_ok(const char *lang)
{
    return in_list(kLangs, sizeof kLangs / sizeof kLangs[0], lang);
}

bool loc_region_norm(const char *in, char *out, size_t cap)
{
    if (!upper_norm(in, out, cap))
        return false;
    return loc_region_ok(out);
}

bool loc_lang_norm(const char *in, char *out, size_t cap)
{
    if (!upper_norm(in, out, cap))
        return false;
    return loc_lang_ok(out);
}

/* --------------------------------------------------------------------------
 * 游戏 ID 校验
 *
 * **这条是本模块的安全核心**：TID 会被拼进路径。如果放进来一个
 * "../../../.."，就写穿了整个目录树。所以用**穷举式**的判据：
 * 恰好 16 个字符，每个都必须是 [0-9A-Fa-f]。没有别的字符是合法的 ——
 * 连一个 '.' 或 '/' 都进不来，路径穿越从根上不可能。
 *
 * 为什么不用黑名单（像 actions.c 里的 name_ok 那样）：那里要放行中文
 * 文件名，字符集不可穷举，只能用黑名单；这里输入形态是**定长定字符集**的，
 * 白名单更严且更简单。两处判据不同是刻意的，各有各的理由。
 * ----------------------------------------------------------------------- */
bool loc_tid_ok(const char *tid)
{
    size_t i;

    if (!tid)
        return false;
    for (i = 0; tid[i]; ++i) {
        unsigned char c = (unsigned char)tid[i];
        bool hexd = (c >= '0' && c <= '9') ||
                    (c >= 'a' && c <= 'f') ||
                    (c >= 'A' && c <= 'F');
        if (!hexd)
            return false;
        if (i >= LOC_TID_HEX)
            return false;               /* 超长 */
    }
    return i == LOC_TID_HEX;            /* 必须**恰好** 16 位 */
}

bool loc_path(const char *tid, char *dst, size_t cap)
{
    char up[LOC_TID_MAX];
    int  n;

    if (dst && cap)
        dst[0] = '\0';
    if (!loc_tid_ok(tid) || !dst || cap == 0)
        return false;

    /* 统一成大写再拼路径。这样 "0004...55d00" 和 "0004...55D00"
     * 落到**同一个**目录 —— 否则同一个游戏会被当成两个，
     * 用户按小写设了、程序按大写查，读不到还以为没设上。 */
    for (size_t i = 0; i < LOC_TID_HEX; ++i)
        up[i] = (char)toupper((unsigned char)tid[i]);
    up[LOC_TID_HEX] = '\0';

    n = snprintf(dst, cap, "%s/%s/%s", LOC_DIR, up, LOC_FILE);
    if (n < 0 || (size_t)n >= cap) {
        dst[0] = '\0';                  /* snprintf 会静默截断，必须自己查 */
        return false;
    }
    return true;
}

/* --------------------------------------------------------------------------
 * 读 / 写
 * ----------------------------------------------------------------------- */

/* 从一个完整文件路径里取出目录部分（去掉最后一段和它前面的斜杠）。
 * loc_set 和 loc_clear 都要用 —— 两处各写一遍的话，哪天改了一处，
 * 就会出现"设置建在 A、撤销删的是 B"这种没人会想到去查的错。 */
static bool dir_of(const char *path, char *dir, size_t cap)
{
    const char *slash = strrchr(path, '/');
    size_t len;

    if (!slash || slash == path)
        return false;                    /* 没有斜杠，或斜杠就在开头 */
    len = (size_t)(slash - path);
    if (len + 1 > cap)
        return false;
    memcpy(dir, path, len);
    dir[len] = '\0';
    return true;
}

/* 读一行 "REGION LANG"，两个词都取出来。
 * 容忍多余空白与结尾换行 —— 用户可能手改过这个文件。 */
static bool parse_line(const char *line, char *region, size_t rcap,
                       char *lang, size_t lcap)
{
    char a[16] = {0}, b[16] = {0};

    if (sscanf(line, "%15s %15s", a, b) != 2)
        return false;
    if (!loc_region_norm(a, region, rcap))
        return false;
    if (!loc_lang_norm(b, lang, lcap))
        return false;
    return true;
}

bool loc_read(const char *tid, char *region, size_t rcap,
              char *lang, size_t lcap)
{
    char path[160], line[64];
    FILE *f;

    if (region && rcap)
        region[0] = '\0';
    if (lang && lcap)
        lang[0] = '\0';
    if (!loc_path(tid, path, sizeof path))
        return false;

    f = fopen(path, "r");
    if (!f)
        return false;
    if (!fgets(line, sizeof line, f)) {
        fclose(f);
        return false;
    }
    fclose(f);

    return parse_line(line, region, rcap, lang, lcap);
}

LocState loc_set(const char *tid, const char *region, const char *lang,
                 char *msg, size_t cap)
{
    char up_r[16], up_l[16], path[160], dir[160], line[40], back_r[16], back_l[16];
    int  n;
    FILE *f;

    /* 1) 三个输入都要过校验，一个不过就什么都不做 */
    if (!loc_tid_ok(tid)) {
        snprintf(msg, cap, "游戏 ID 不合法：必须是 16 位十六进制"
                           "（例如 0004000000055D00）。得到的是 %.40s",
                 tid ? tid : "(空)");
        return LOC_ERR_TID;
    }
    if (!loc_region_norm(region, up_r, sizeof up_r)) {
        snprintf(msg, cap, "区域 %s 不认识。能填的只有 "
                           "JPN / USA / EUR / AUS / CHN / KOR / TWN",
                 region ? region : "(空)");
        return LOC_ERR_REGION;
    }
    if (!loc_lang_norm(lang, up_l, sizeof up_l)) {
        snprintf(msg, cap, "语言 %s 不认识。能填的只有 "
                           "JP / EN / FR / DE / IT / ES / ZH / KO / NL / PT / RU / TW",
                 lang ? lang : "(空)");
        return LOC_ERR_LANG;
    }

    /* 2) 拼路径（这之后再没有模型给的字符串参与拼装了） */
    if (!loc_path(tid, path, sizeof path)) {
        snprintf(msg, cap, "路径拼不出来，已拦下");
        return LOC_ERR_IO;
    }

    /* 目录部分 = path 去掉最后的 "/locale.txt"。
     * 用同一个缓冲算出来，而不是另拼一次 —— 两条路径算法一旦分家，
     * 就会出现"目录建在 A、文件写到 B"这种最难查的错。 */
    if (!dir_of(path, dir, sizeof dir)) {
        snprintf(msg, cap, "路径异常，已拦下");
        return LOC_ERR_IO;
    }
    util_mkdirs(dir);

    /* 3) 写。结尾给一个换行 —— 有些读取实现不认没有换行的最后一行。 */
    n = snprintf(line, sizeof line, "%s %s\n", up_r, up_l);
    if (n < 0 || (size_t)n >= sizeof line) {
        snprintf(msg, cap, "内容拼装异常，已拦下");
        return LOC_ERR_IO;
    }

    f = fopen(path, "w");
    if (!f) {
        snprintf(msg, cap, "写不进 luma/titles/ 目录 —— "
                           "卡满了、写保护、或者这张卡上没有 luma 目录？");
        return LOC_ERR_IO;
    }
    if (fwrite(line, 1, (size_t)n, f) != (size_t)n || fflush(f) != 0) {
        fclose(f);
        snprintf(msg, cap, "写 locale.txt 时出错");
        return LOC_ERR_IO;
    }
    fclose(f);

    /* 4) **读回来核对**。这一步不是多余的：devoptab 上的写可能因为
     *    各种原因"看起来成功但没落盘"，而用户之后只会看到"设了没用"。
     *    核对失败要报错，不能当成功 —— 那是最难查的一种状态。 */
    if (!loc_read(tid, back_r, sizeof back_r, back_l, sizeof back_l)) {
        snprintf(msg, cap, "刚写完的文件读不回来 —— 卡可能有问题，"
                           "请重启后再试一次");
        return LOC_ERR_VERIFY;
    }
    if (strcmp(back_r, up_r) != 0 || strcmp(back_l, up_l) != 0) {
        snprintf(msg, cap, "写完读回来变成 %s %s（想写的是 %s %s）—— 已中止",
                 back_r, back_l, up_r, up_l);
        return LOC_ERR_VERIFY;
    }

    snprintf(msg, cap, "已把 %.16s 设成 %s %s。"
                       "下次启动这个游戏时生效（不用重启机器）。",
             tid, up_r, up_l);
    return LOC_OK;
}

LocState loc_clear(const char *tid, char *msg, size_t cap)
{
    char path[160], dir[160], r[16], l[16];

    if (!loc_tid_ok(tid)) {
        snprintf(msg, cap, "游戏 ID 不合法：必须是 16 位十六进制");
        return LOC_ERR_TID;
    }
    if (!loc_read(tid, r, sizeof r, l, sizeof l)) {
        snprintf(msg, cap, "%.16s 本来就没设过区域，不用撤销", tid);
        return LOC_ERR_NONE;
    }
    if (!loc_path(tid, path, sizeof path)) {
        snprintf(msg, cap, "路径拼不出来，已拦下");
        return LOC_ERR_IO;
    }

    if (remove(path) != 0) {
        snprintf(msg, cap, "删不掉 locale.txt —— 卡写保护了？");
        return LOC_ERR_IO;
    }

    /* 顺手把空掉的目录也删了。留一堆空目录不影响功能，
     * 但"已设区域"列表是按目录扫的，留着会让撤销过的游戏继续出现。 */
    if (dir_of(path, dir, sizeof dir))
        remove(dir);                     /* 非空会失败，正好，不用管返回值 */

    snprintf(msg, cap, "已撤销 %.16s 的区域设置（原来的 %s %s）", tid, r, l);
    return LOC_OK;
}

/* --------------------------------------------------------------------------
 * 列表
 * ----------------------------------------------------------------------- */
int loc_list(char tids[][LOC_TID_MAX], char vals[][24], int max,
             int *count_out)
{
    DIR *d;
    struct dirent *de;
    int n = 0;

    if (count_out)
        *count_out = 0;

    d = opendir(LOC_DIR);
    if (!d) {
        /* 目录不存在 = 一个都没设过。这不是错误。 */
        return 0;
    }

    while ((de = readdir(d)) != NULL && n < max) {
        char r[16], l[16], val[24];
        int  m;

        if (!loc_tid_ok(de->d_name))
            continue;                    /* 不是游戏 ID 的目录一律跳过 */
        if (!loc_read(de->d_name, r, sizeof r, l, sizeof l))
            continue;                    /* 目录在但没有像样的 locale.txt */

        for (int i = 0; i < LOC_TID_HEX; ++i)
            tids[n][i] = (char)toupper((unsigned char)de->d_name[i]);
        tids[n][LOC_TID_HEX] = '\0';

        m = snprintf(val, sizeof val, "%s %s", r, l);
        if (m < 0)
            val[0] = '\0';
        snprintf(vals[n], 24, "%s", val);
        ++n;
    }
    closedir(d);

    if (count_out)
        *count_out = n;
    return n;
}

/* --------------------------------------------------------------------------
 * Luma 的 "Enable game patching" 状态
 *
 * config.ini 里长这样（键名随版本略有出入，所以做的是"找关键字"）：
 *     [Luma3DS]
 *     enable game patching = 1
 * 或者老版本可能是别的写法。这里只做三态判断，读不到就是 UNKNOWN，
 * 绝不因为不确定而拦下用户的正常操作。
 * ----------------------------------------------------------------------- */
LocPatchState loc_game_patching(void)
{
    FILE *f = fopen(LOC_CONFIG, "r");
    char  line[256];
    bool  saw_key = false, on = false;

    if (!f)
        return LOC_PATCH_UNKNOWN;

    while (fgets(line, sizeof line, f)) {
        char low[256];
        size_t i;

        for (i = 0; line[i] && i + 1 < sizeof low; ++i)
            low[i] = (char)tolower((unsigned char)line[i]);
        low[i] = '\0';

        /* 找 "game patching" 这一项，看它后面跟的是 0 还是非 0 */
        if (strstr(low, "game patching") || strstr(low, "game_patching") ||
            strstr(low, "gamepatching")) {
            const char *eq = strchr(low, '=');
            saw_key = true;
            if (eq) {
                const char *v = eq + 1;
                while (*v == ' ' || *v == '\t')
                    ++v;
                on = (*v != '0');
            }
        }
    }
    fclose(f);

    if (!saw_key)
        return LOC_PATCH_UNKNOWN;
    return on ? LOC_PATCH_ON : LOC_PATCH_OFF;
}

const char *loc_state_text(LocState s)
{
    switch (s) {
        case LOC_OK:           return "已设置";
        case LOC_ERR_TID:      return "游戏 ID 不合法";
        case LOC_ERR_REGION:   return "区域不认识";
        case LOC_ERR_LANG:     return "语言不认识";
        case LOC_ERR_IO:       return "写文件失败";
        case LOC_ERR_VERIFY:   return "写完核对不上";
        case LOC_ERR_NONE:     return "本来就没设过";
        default:               return "未知情况";
    }
}
