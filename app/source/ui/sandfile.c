/*
 * sandfile.c -- 沙箱目录的只读访问（app 层）
 *
 * 给 file.list / file.read 两个动作用。它们**只读**：
 * 这里没有任何 unlink / rename / fopen(...,"w")，将来也不该有。
 *
 * ── 一个必须知道的前提：dirent 在 sdmc: 上能不能用，尚未在真机验证过 ──
 * newlib 的 opendir/readdir 走 POSIX 层，libctru 给 sdmc: 配了 FAT 驱动，
 * 理论上可用；但"理论上"三个字不值钱，而这里是**崩溃风险点**。
 * 所以这里的规矩是：
 *   · opendir 返回 NULL → 当成"这个目录列不了"，写一句人话就返回 false；
 *   · 读不到与"空目录"必须**分开说** —— 把读不了说成"是空的"，
 *     会让模型下结论"文件没保存成功"，那是在误导用户。
 * 沙箱目录本来就是程序自己写的，出错时那句话要说清是哪一种。
 */

#include "sandfile.h"

#include "actions.h"

#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include "uistr.h"

/* 工作目录：aiconsole 本身。
 *
 * ★ 为什么要单独一个常量：`actions_dir()` 是 `.../aiconsole/out`，
 * 而 pkgs / doubao / update 是它的**兄弟**，不是它的子目录。
 * 早先这里写成 `actions_dir() + "/" + sub`，于是 sub=out 拼出
 * `.../out/out` —— 一个不存在的目录。实测报的是
 * 「sdmc:/3ds/aiconsole/out/out 还不存在」，一眼就能看出是拼错了，
 * 但它同时意味着 file.list 对**每一个** sub 都失败。
 *
 * 三者的真实关系：
 *     sdmc:/3ds/aiconsole/            ← 根（不写 sub 时列这里）
 *     sdmc:/3ds/aiconsole/out         ← actions_dir()，AI 写文件的地方
 *     sdmc:/3ds/aiconsole/pkgs        ← 兄弟
 *     sdmc:/3ds/aiconsole/doubao      ← 兄弟
 *     sdmc:/3ds/aiconsole/update      ← 兄弟
 */
#define ACT_ROOT "sdmc:/3ds/aiconsole"

/* 把白名单里的子目录名翻成完整路径。
 * 白名单表在 shared/actions.c（actions_sandbox_subdir），
 * 这里**只做翻译**，不自己再写一张 —— 两张表迟早会有一处漏改，
 * 而漏改的后果是"校验放行的目录"和"实际打开的目录"不是同一个。 */
static bool subdir_path(const char *sub, char *out, size_t cap)
{
    if (!sub || !*sub || !strcmp(sub, "...")) {
        snprintf(out, cap, "%s", ACT_ROOT);
        return true;
    }
    /* out 就是 actions_dir() 本身，不能再拼一次 */
    if (!strcmp(sub, "out")) {
        snprintf(out, cap, "%s", actions_dir());
        return true;
    }
    /* 回收站在 out 里面（actfile.c 的 TRASH_DIR），不在根下 ——
     * 按兄弟目录拼会得到 .../aiconsole/.trash，一个不存在的地方。
     * 顺带说明：能**列**回收站，但 file.delete 依然不许删它
     * （那个校验拦的是以点开头的文件名，与这里不冲突）。 */
    if (!strcmp(sub, ".trash")) {
        snprintf(out, cap, "%s/.trash", actions_dir());
        return true;
    }
    for (int i = 0; actions_sandbox_subdir(i); ++i) {
        if (!strcmp(actions_sandbox_subdir(i), sub)) {
            snprintf(out, cap, "%s/%s", ACT_ROOT, sub);
            return true;
        }
    }
    return false;
}

bool sandfile_list(const char *sub, char *out, size_t cap)
{
    char            path[192];
    DIR            *dp;
    struct dirent  *de;
    size_t          n;
    int             cnt = 0;

    if (!out || cap == 0)
        return false;
    out[0] = '\0';

    if (!subdir_path(sub, path, sizeof path)) {
        snprintf(out, cap, S(S0043), sub ? sub : S(S0044));
        return false;
    }

    dp = opendir(path);
    if (!dp) {
        struct stat st;
        if (stat(path, &st) != 0)
            snprintf(out, cap, S(S0045), path);
        else
            snprintf(out, cap, S(S0046), path);
        return false;
    }

    n = (size_t)snprintf(out, cap, S(S0047), path);
    while ((de = readdir(dp)) != NULL && n + 2 < cap) {
        /* "." 与 ".." 不是文件，报给模型只会白占它的注意力 */
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, ".."))
            continue;
        n += (size_t)snprintf(out + n, cap - n, "\n  %s", de->d_name);
        ++cnt;
    }
    closedir(dp);

    if (cnt == 0) {
        snprintf(out, cap, S(S0048), path);
        return true;
    }
    snprintf(out + n, cap - n, S(S0015), cnt);
    return true;
}

/* 把末尾的半个 UTF-8 字符整个丢掉。
 *
 * 为什么宁可少给一个字符也不能腰斩：模型收到半截汉字，
 * 会把它当成一个怪字符去解释，然后写进给用户的回答里 ——
 * 那比少一个字糟糕得多。 */
static void drop_partial_utf8(char *s, size_t *len)
{
    size_t e = *len;
    size_t start;
    unsigned char c0;
    int need;

    if (e == 0)
        return;
    while (e > 0 && ((unsigned char)s[e - 1] & 0xC0) == 0x80)
        --e;                          /* e 现在落在最后一个字符的首字节 */
    if (e == 0)
        return;
    start = e - 1;
    c0 = (unsigned char)s[start];
    need = (c0 >= 0xF0) ? 4 : (c0 >= 0xE0) ? 3 : (c0 >= 0xC0) ? 2 : 1;
    if ((int)(*len - start) < need)
        *len = start;                 /* 半个字符，整个丢掉 */
}

bool sandfile_read(const char *name, long from, char *out, size_t cap)
{
    char   path[192];
    FILE  *f;
    size_t got;
    int    n;

    if (!out || cap == 0)
        return false;
    out[0] = '\0';

    /* name 已在 actions_check_readonly() 里过 name_ok（无 / \ : ..），
     * 这里再核一次前缀：两道独立的防线，不靠同一处判断。 */
    n = snprintf(path, sizeof path, "%s/%s", actions_dir(), name ? name : "");
    if (n < 0 || n >= (int)sizeof path) {
        snprintf(out, cap, "%s", S(S0004));
        return false;
    }
    if (strncmp(path, actions_dir(), strlen(actions_dir())) != 0) {
        snprintf(out, cap, S(S0049));
        return false;
    }

    f = fopen(path, "rb");
    if (!f) {
        snprintf(out, cap, S(S0050), name ? name : "");
        return false;
    }

    if (from > 0 && fseek(f, from, SEEK_SET) != 0) {
        /* 偏移越界不是错误，是"读不到那一段" —— 换个说法，
         * 模型才知道该从头读，而不是反复试同一个偏移。 */
        fclose(f);
        snprintf(out, cap, S(S0051),
                 name ? name : "", from);
        return false;
    }

    got = fread(out, 1, cap - 1, f);
    fclose(f);
    out[got] = '\0';
    drop_partial_utf8(out, &got);
    out[got] = '\0';

    if (got == 0) {
        snprintf(out, cap, S(S0048), name ? name : "");
        return true;
    }
    return true;
}
