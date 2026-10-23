/*
 * util.c -- 见 util.h
 */

#include "util.h"

#include <3ds.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

/* 3DS 的 RTC 给的是 UTC（2000-01-01 起算的毫秒）。
 *
 * 显示给用户的时间按**东八区**换算 —— 目标用户在中国，而 libctru 上
 * 没有可靠可用的 localtime（要正确的时区设置加 RTC 驱动配合，不保证成立）。
 * 与其为了一个"看起来对"的时间引一套时区机制，不如把这一个偏移量写死、
 * 并且在这里说清楚它是什么。换时区就改这一行。 */
#define TZ_OFFSET_HOURS 8

void util_mkdirs(const char *path)
{
    char   tmp[192];
    size_t n = strlen(path);
    char  *colon;
    char  *start;

    if (n == 0 || n >= sizeof tmp)
        return;

    memcpy(tmp, path, n + 1);

    /* 有 "sdmc:" 这类盘符前缀就跳过它，从它后面第一个 '/' 开始切；
     * 没有前缀（纯绝对路径 "/a/b/c"）则从**第二个字符**开始 ——
     * 从 tmp+1 起是为了不把开头的那个 '/' 当成目录末尾（那会变成
     * mkdir("")，什么都没建，且第一个目录段整个被漏掉）。 */
    colon = strchr(tmp, ':');
    start = colon ? colon + 1 : tmp;
    if (*start != '/')
        start = tmp + 1;      /* 相对路径：跳过首字符，别把 "" 当目录 */
    else
        ++start;              /* 绝对路径：跳过开头的 '/' */

    for (char *p = start; *p; ++p) {
        if (*p == '/') {
            *p = '\0';
            mkdir(tmp, 0777);
            *p = '/';
        }
    }
    mkdir(tmp, 0777);
}

/* 把 UTC 毫秒拆成日历。手算而不是走 newlib 的 gmtime/localtime ——
 * 省一条对时区数据库的依赖，也省一个新lib 的静态初始化。 */
static void split_now(int *y, int *mo, int *d, int *hh, int *mm, int *ss)
{
    static const int mdays[12] = { 31, 28, 31, 30, 31, 30,
                                   31, 31, 30, 31, 30, 31 };
    u64 sec  = osGetTime() / 1000ULL + (u64)TZ_OFFSET_HOURS * 3600ULL;
    int days = (int)(sec / 86400ULL);
    int rem  = (int)(sec % 86400ULL);
    int m    = 0;

    *hh = rem / 3600;
    *mm = (rem % 3600) / 60;
    *ss = rem % 60;

    for (*y = 2000; ; ++*y) {
        int leap = (*y % 4 == 0 && *y % 100 != 0) || (*y % 400 == 0);
        int dy   = 365 + leap;
        if (days < dy)
            break;
        days -= dy;
    }

    while (m < 11) {
        int leap = (*y % 4 == 0 && *y % 100 != 0) || (*y % 400 == 0);
        int dm   = mdays[m] + ((m == 1 && leap) ? 1 : 0);
        if (days < dm)
            break;
        days -= dm;
        ++m;
    }
    *mo = m + 1;
    *d  = days + 1;
}

void util_stamp(char *out, size_t cap)
{
    int y, mo, d, hh, mm, ss;

    split_now(&y, &mo, &d, &hh, &mm, &ss);
    snprintf(out, cap, "%04d%02d%02d_%02d%02d", y, mo, d, hh, mm);
}

void util_stamp_full(char *out, size_t cap)
{
    int y, mo, d, hh, mm, ss;

    split_now(&y, &mo, &d, &hh, &mm, &ss);
    snprintf(out, cap, "%04d-%02d-%02d %02d:%02d:%02d",
             y, mo, d, hh, mm, ss);
}
