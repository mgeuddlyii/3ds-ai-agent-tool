/*
 * actfile.c -- file.delete / file.rename / key.remove / key.switch 的执行
 *
 * ── 三条设计前提 ──
 *
 * 1. **删除 = 移进回收站**，不是 unlink。
 *    真删在这个平台上救不回来，而"AI 判断该删哪个文件出错的概率"
 *    远高于"用户误点允许的概率"。移进 .trash/ 之后，用户在电脑上
 *    把它挪回去就行 —— 而那正是"误删之后怎么办"这个问题唯一的答案。
 *
 * 2. **模型不能写凭据。** 所以这里只有 key.remove 与 key.switch，
 *    没有 key.set：模型无法验证一个 Key 有没有效，而它很可能编一个
 *    看起来像的写进去，用户看到"已配置"却始终连不上，排查方向会被带偏。
 *
 * 3. **不许删掉最后一把 Key。** 那会让用户彻底没法对话 ——
 *    他当初要 AI 删，多半只是想去掉一把多余的。
 *
 * ⚠️ 未经模拟器/真机实测（积分约束）。
 */

#include "actfile.h"

#include "actions.h"
#include "app.h"
#include "keystore.h"
#include "util.h"

#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include "uistr.h"

#define TRASH_DIR "sdmc:/3ds/aiconsole/out/.trash"

/* 拼 out/<name>。name 已过 name_ok（无 / \ : ..），
 * 这里再核一次前缀：两道独立的防线，不靠同一处判断。 */
static bool out_path(const char *name, char *out, size_t cap)
{
    int n = snprintf(out, cap, "%s/%s", actions_dir(), name ? name : "");
    return n > 0 && n < (int)cap;
}

bool actfile_delete(const char *name, char *msg, size_t cap)
{
    char src[192], dst[224];
    FILE *f;
    int   n;

    if (!msg || cap == 0)
        return false;

    if (!out_path(name, src, sizeof src)) {
        snprintf(msg, cap, S(S0004));
        return false;
    }

    /* 先确认它真的在：对着一个不存在的文件说"已移入回收站"，
     * 是最容易让用户以为"文件还在"的谎。 */
    f = fopen(src, "rb");
    if (!f) {
        snprintf(msg, cap, S(S0005), name);
        return false;
    }
    fclose(f);

    util_mkdirs(TRASH_DIR);

    /* 目标名带时间戳：同名连删两次会互相覆盖，
     * 而用户正是"删了两次、第一次还想找回来"时最需要回收站。 */
    {
        char stamp[32];
        util_stamp(stamp, sizeof stamp);
        /* stamp 里可能有空格与冒号（文件名里都不合法），
         * 逐个换成下划线而不是硬删 —— 删字符可能让两枚时间戳撞成同一个。 */
        for (char *q = stamp; *q; ++q)
            if (*q == ':' || *q == ' ' || *q == '-')
                *q = '_';
        n = snprintf(dst, sizeof dst, "%s/%s_%s", TRASH_DIR, name, stamp);
    }
    if (n < 0 || n >= (int)sizeof dst) {
        snprintf(msg, cap, "%s", S(S0006));
        return false;
    }

    if (rename(src, dst) != 0) {
        snprintf(msg, cap, S(S0007));
        return false;
    }

    snprintf(msg, cap, S(S0008), dst);
    app_note("actfile: deleted name=%s -> %s", name, dst);
    return true;
}

bool actfile_rename(const char *from, const char *to, char *msg, size_t cap)
{
    char a[192], b[192];
    FILE *f;

    if (!msg || cap == 0)
        return false;

    if (!out_path(from, a, sizeof a) || !out_path(to, b, sizeof b)) {
        snprintf(msg, cap, S(S0004));
        return false;
    }

    f = fopen(a, "rb");
    if (!f) {
        snprintf(msg, cap, S(S0005), from);
        return false;
    }
    fclose(f);

    /* 目标已存在时 rename 在 FAT 上会直接覆盖，用户原来那份就没了。
     * 那是"不可逆"里最容易被忽略的一种，所以要先挡。 */
    f = fopen(b, "rb");
    if (f) {
        fclose(f);
        snprintf(msg, cap, S(S0009), to);
        return false;
    }

    if (rename(a, b) != 0) {
        snprintf(msg, cap, S(S0010));
        return false;
    }

    snprintf(msg, cap, S(S0011), from, to);
    app_note("actfile: renamed %s -> %s", from, to);
    return true;
}

/* ---- 清空回收站（真删）---- */

/* 先报清单：永久删除之前必须让用户知道**要没的是哪些东西**。
 * 只报"有 N 个文件"不够 —— 用户点完之后照样不知道自己在删什么。 */
bool actfile_trash_list(char *out, size_t cap)
{
    DIR           *dp;
    struct dirent  *de;
    size_t         n = 0;
    int            cnt = 0;

    if (!out || cap == 0)
        return false;

    dp = opendir(TRASH_DIR);
    if (!dp) {
        snprintf(out, cap, "%s", S(S0012));
        return true;
    }
    n = (size_t)snprintf(out, cap, "%s", S(S0013));
    while ((de = readdir(dp)) != NULL && n + 2 < cap) {
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, ".."))
            continue;
        n += (size_t)snprintf(out + n, cap - n, "\n  %s", de->d_name);
        ++cnt;
    }
    closedir(dp);

    if (cnt == 0)
        snprintf(out, cap, "%s", S(S0014));
    else
        snprintf(out + n, cap - n, S(S0015), cnt);
    return true;
}

bool actfile_trash_purge(char *msg, size_t cap)
{
    DIR       *dp;
    struct dirent *de;
    char       victim[256];
    int        cnt = 0;

    if (!msg || cap == 0)
        return false;

    dp = opendir(TRASH_DIR);
    if (!dp) {
        snprintf(msg, cap, "%s", S(S0016));
        return true;
    }

    /* ★ 边读边删是**错的**，而这段代码的注释早就写着"必须先记名再删"，
     *   实现却一直在循环里 remove（S-13，自相矛盾）。
     *   readdir 的位置在 unlink 之后失效，会漏掉一部分条目 ——
     *   "以为删干净了，其实还剩几个"。对回收站这种"用户以为清空了"的动作，
     *   漏删比删不掉更糟。
     *
     *   改成**分批**：一次读满一批名字 → 关目录 → 再逐批删 → 重新打开，
     *   直到读不到条目为止。分批而不是"一次性读全部"，是因为回收站
     *   可能有几百个文件，固定栈数组装不下。
     */
    closedir(dp);

    for (;;) {
        char names[32][64];
        int  n = 0, i;
        DIR *d2 = opendir(TRASH_DIR);

        if (!d2)
            break;
        while (n < 32 && (de = readdir(d2)) != NULL) {
            if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, ".."))
                continue;
            snprintf(names[n], sizeof names[n], "%s", de->d_name);
            ++n;
        }
        closedir(d2);

        if (n == 0)
            break;                      /* 空了 */

        /* 目录已关闭，这时候删是安全的 */
        for (i = 0; i < n; ++i) {
            snprintf(victim, sizeof victim, "%s/%s", TRASH_DIR, names[i]);
            if (remove(victim) == 0)
                ++cnt;
            else
                app_note("actfile: purge failed %s", victim);
        }
    }

    if (cnt == 0) {
        snprintf(msg, cap, "%s", S(S0017));
        return true;
    }
    snprintf(msg, cap, S(S0018), cnt);
    app_note("actfile: purged %d files", cnt);
    return true;
}

bool actkey_remove(const char *name, char *msg, size_t cap)
{
    int n;

    if (!msg || cap == 0)
        return false;
    if (!name || !*name) {
        snprintf(msg, cap, "%s", S(S0019));
        return false;
    }

    keystore_load();
    n = keystore_count();

    /* 存在性先问一遍：keystore_remove 对不存在的名字会安静地返回 false，
     * 而"删掉了"与"没找到"必须说清是哪一种。 */
    {
        bool found = false;
        for (int i = 0; i < n; ++i) {
            const KeyEntry *e = keystore_at(i);
            if (e && !strcmp(e->name, name)) {
                found = true;
                break;
            }
        }
        if (!found) {
            snprintf(msg, cap, S(S0020), name, n);
            return false;
        }
    }

    /* ★ 最后一把不许删。理由写在 actions.h 里：删掉它用户就没法对话了，
     * 而他要 AI 删多半只是想清掉一把多余的。 */
    if (n <= 1) {
        /* 整句一条：拼接里放不下 S()（它是函数调用，不是字面量） */
        snprintf(msg, cap, S(S0021));
        return false;
    }

    if (!keystore_remove(name)) {
        snprintf(msg, cap, S(S0022), name);
        return false;
    }

    snprintf(msg, cap, S(S0023), name, keystore_count());
    app_note("actkey: removed name=%s left=%d", name, keystore_count());
    return true;
}

bool actkey_switch(const char *name, char *msg, size_t cap)
{
    const KeyEntry *cur;

    if (!msg || cap == 0)
        return false;
    if (!name || !*name) {
        snprintf(msg, cap, "%s", S(S0024));
        return false;
    }

    keystore_load();
    cur = keystore_active();
    if (cur && !strcmp(cur->name, name)) {
        /* 切到已经在用的那一把不是错误，但要说清"没变化"，
         * 否则用户会以为切换没生效。 */
        snprintf(msg, cap, S(S0025), name);
        return true;
    }

    if (!keystore_set_active(name)) {
        int n = keystore_count();
        snprintf(msg, cap, S(S0020), name, n);
        return false;
    }

    snprintf(msg, cap, S(S0026), name);
    app_note("actkey: switched to %s", name);
    return true;
}
