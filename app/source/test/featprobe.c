/*
 * featprobe.c -- 见 featprobe.h
 *
 * 写作口径与 amprobe/dbprobe 一致：**每条结论都要能被独立的证据支持**，
 * 不说"成功了"这种没法反驳也没法验证的话。
 */

#include "featprobe.h"

#include "app.h"
#include "locale.h"
#include "qr.h"
#include "shfont.h"

#include <3ds.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* 测试用的游戏 ID。挑一个**明显是编的**：真实 title id 不会是 0004 开头
 * 然后中间一堆 FFEE。这样万一探针中途崩了没撤销，用户在主菜单/列表里
 * 一眼就知道这条不是自己的。 */
#define TEST_TID "0004000000FFEE01"

/* ---- 小工具 ---- */

static int count_titles_dir(void)
{
    DIR *d = opendir(LOC_DIR);
    int  n = 0;

    if (!d)
        return -1;                  /* 目录不存在时给 -1，与"空目录"区分开 */
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        if (de->d_name[0] == '.')
            continue;
        ++n;
    }
    closedir(d);
    return n;
}

/* 按原始字节读回 locale.txt，用来核对**写进去的确切内容**。
 * 不用 loc_read：那个会把内容解析成 region/lang，解析成功不代表
 * 落盘的字节是对的（比如多了个换行、或大小写没统一）。 */
static bool read_raw(const char *tid, char *out, size_t cap)
{
    char  path[192];
    FILE *f;

    if (out && cap)
        out[0] = '\0';
    if (!loc_path(tid, path, sizeof path))
        return false;
    f = fopen(path, "r");
    if (!f)
        return false;
    if (!fgets(out, (int)cap, f)) {
        fclose(f);
        return false;
    }
    fclose(f);
    return true;
}

static bool file_exists(const char *tid)
{
    char path[192];

    if (!loc_path(tid, path, sizeof path))
        return false;
    FILE *f = fopen(path, "r");
    if (!f)
        return false;
    fclose(f);
    return true;
}

/* ==========================================================================
 * 第一部分：区域切换
 * ======================================================================== */

static void probe_locale(void)
{
    char     msg[192];
    char     path[192];
    LocState ls;

    app_note("featprobe: ===== 区域切换（写 luma/titles/<TID>/locale.txt）=====");

    /* ---- 1. 路径拼得对不对 ----
     * 这是本模块最危险的一环：TID 会被拼进路径，拼错就是写进别的目录。
     * 先单独把 path 打出来，让"写到哪"这件事有据可查。 */
    if (loc_path(TEST_TID, path, sizeof path))
        app_note("featprobe: 目标路径 = %s", path);
    else
        app_note("featprobe: !! loc_path 对合法 TID 返回了失败（不该发生）");

    /* ---- 2. 正常设置 ---- */
    ls = loc_set(TEST_TID, "usa", "en", msg, sizeof msg);   /* 故意小写 */
    app_note("featprobe: loc_set(usa/en 小写) = %s · %s", loc_state_text(ls), msg);
    if (ls != LOC_OK) {
        app_note("featprobe: 结论 = 设置这一步就没过，后面的都免谈");
        return;
    }

    /* 小写输入应当被**规范成大写**落盘 —— 否则同一个游戏按小写设、
     * 按大写查，会读不到，用户看到的是"设了没用"。 */
    {
        char raw[64] = {0};
        if (read_raw(TEST_TID, raw, sizeof raw))
            app_note("featprobe: 落盘内容 = %s", raw);
        else
            app_note("featprobe: !! 文件写完后读不回原始内容");
    }

    /* ---- 3. 读回来 ---- */
    {
        char r[16], l[16];
        if (loc_read(TEST_TID, r, sizeof r, l, sizeof l))
            app_note("featprobe: loc_read = %s %s（期望 USA EN）", r, l);
        else
            app_note("featprobe: !! loc_read 读不回来");
    }

    /* ---- 4. 列表里能不能看到 ---- */
    {
        static char tids[LOC_LIST_MAX][LOC_TID_MAX];
        static char vals[LOC_LIST_MAX][24];
        int n = 0, hit = 0;

        loc_list(tids, vals, LOC_LIST_MAX, &n);
        for (int i = 0; i < n; ++i)
            if (strcmp(tids[i], TEST_TID) == 0)
                hit = 1;
        app_note("featprobe: loc_list 列出 %d 条，其中%s测试那条",
                 n, hit ? "包含" : "**不含**");
    }

    /* ---- 5. 非法输入：必须**全部**被拒，且一个文件都不能落地 ----
     * 这一组是本探针的重点。TID 直接参与拼路径，
     * 所以这里塞的全是路径穿越的常见写法。 */
    {
        static const char *const bad[] = {
            "../evil",
            "..\\evil",
            "0004000000055D0",        /* 15 位：少一位 */
            "0004000000055D000",      /* 17 位：多一位 */
            "0004000000055D0Z",       /* 含非十六进制字符 */
            "0004000000/5D0E",        /* 夹斜杠 */
            "0004000000055D00/..",    /* 尾部穿越 */
            "",
        };
        int before = count_titles_dir();
        int rejected = 0, wrote = 0;

        for (size_t i = 0; i < sizeof bad / sizeof bad[0]; ++i) {
            char   m[192];
            LocState s = loc_set(bad[i], "USA", "EN", m, sizeof m);

            if (s == LOC_ERR_TID)
                ++rejected;
            else
                app_note("featprobe: !! 非法 TID 没被拒：\"%s\" -> %s",
                         bad[i], loc_state_text(s));
            if (file_exists(bad[i]))
                ++wrote;
        }
        app_note("featprobe: 非法 TID 共 %d 个，被拒 %d 个，落地 %d 个",
                 (int)(sizeof bad / sizeof bad[0]), rejected, wrote);

        /* 目录条目数不变 = **没有任何路径穿越真的写出了东西**。
         * 这比逐个 file_exists 更硬：那个只查它能拼出来的路径，
         * 而穿越的目的地本来就在它能拼的范围之外。 */
        {
            int after = count_titles_dir();
            app_note("featprobe: luma/titles 目录条目 %d -> %d（应相等）",
                     before, after);
        }
    }

    /* ---- 6. 非法区域/语言 ---- */
    {
        char m[192];
        LocState s1 = loc_set(TEST_TID, "XXX", "EN", m, sizeof m);
        char m2[192];
        LocState s2 = loc_set(TEST_TID, "USA", "XX", m2, sizeof m2);

        app_note("featprobe: 非法区域 XXX -> %s · %s", loc_state_text(s1), m);
        app_note("featprobe: 非法语言 XX  -> %s · %s", loc_state_text(s2), m2);
        /* 被拒之后，原来那条设置不能被破坏 */
        {
            char r[16], l[16];
            if (loc_read(TEST_TID, r, sizeof r, l, sizeof l))
                app_note("featprobe: 被拒之后原设置仍在 = %s %s", r, l);
            else
                app_note("featprobe: !! 非法输入把原设置冲掉了");
        }
    }

    /* ---- 7. 撤销 + 幂等 ---- */
    {
        char m[192];
        LocState s1 = loc_clear(TEST_TID, m, sizeof m);
        app_note("featprobe: loc_clear = %s · %s", loc_state_text(s1), m);
        app_note("featprobe: 撤销后文件还在吗 = %s",
                 file_exists(TEST_TID) ? "**还在（不该）**" : "不在了");

        LocState s2 = loc_clear(TEST_TID, m, sizeof m);
        app_note("featprobe: 再撤销一次 = %s（期望「本来就没设过」）",
                 loc_state_text(s2));
    }

    /* ---- 8. Luma 那个总开关读不读得到 ---- */
    app_note("featprobe: Luma game patching 读取 = %d"
             "（0=读不到 1=开 2=关；读不到不影响操作，只是少一句提醒）",
             (int)loc_game_patching());
}

/* ==========================================================================
 * 第二部分：字库模块的**安全闸**
 *
 * 这个模块的"功能"就是"在不确定时拒绝" —— 所以可验的正是那些拒绝分支。
 * 真正往共享内存写的那条路**故意无法在本环境触发**：
 * 地址没有真机实测过，模块选择拒绝（见 shfont.c 的 probe_shared_font_addr）。
 * 探针要证明的正是"它真的拒绝了，而不是偷偷试了一把"。
 * ======================================================================== */

static void probe_shfont(void)
{
    char reason[256] = {0};
    ShfAvail av;

    app_note("featprobe: ===== 字库安全闸（本模块的功能是「拒绝」）=====");

    av = shfont_probe(reason, sizeof reason);
    app_note("featprobe: shfont_probe = %d（0=未知 1=可用 2=不安全）", (int)av);
    app_note("featprobe: 理由 = %s", reason);
    app_note("featprobe: 探到的地址 = 0x%08X（0 = 没探到）", shfont_addr());

    /* 本环境（地址未实测）必须是不安全。若报"可用"，说明有人把
     * probe_shared_font_addr 改成返回硬编码地址了 —— 那正是最危险的一种改法，
     * 因为硬编码地址在现代 FIRM 上会写到别处，而现象是花屏不是报错。 */
    if (av == SHF_AVAIL_OK)
        app_note("featprobe: !! 报成了「可用」。地址是实测过的吗？"
                 "没实测就给硬编码地址 = 往错的地方写内存");
    else
        app_note("featprobe: 结论 = 正确拒绝（没有实测地址就不动手）");

    /* 真去调一次 apply，确认它**拒绝**而且**什么都不写**。
     * 用不存在的字库名：期待它先撞上"不可用"这道闸，
     * 而不是走到读文件那一步 —— 顺序反了说明闸门没在正确的位置。 */
    {
        char    msg[256];
        ShfState s = shfont_apply("nonexistent.bcfnt", msg, sizeof msg);
        app_note("featprobe: shfont_apply(不存在的包) = %s · %s",
                 shf_state_text(s), msg);

        /* 还要确认备份路径的判据是**独立**的：没备份时也要拒绝，
         * 不能因为"地址不可用"这一个理由就顺带把备份检查也遮住了。 */
        app_note("featprobe: shfont_has_backup = %s",
                 shfont_has_backup() ? "有" : "没有");
        if (!shfont_has_backup()) {
            char m2[256];
            ShfState s2 = shfont_apply("whatever.bcfnt", m2, sizeof m2);
            app_note("featprobe: 没备份时 apply = %s（必须是拒绝，不是去写）",
                     shf_state_text(s2));
        }
    }

    /* fonts/ 目录扫描不该崩，哪怕目录不存在 */
    {
        static char names[16][SHFONT_NAME_MAX + 1];
        int n = 0;
        shfont_list(names, 16, &n);
        app_note("featprobe: shfont_list = %d 个 .bcfnt", n);
    }
}

/* ========================================================================== */

/* ==========================================================================
 * 第三部分：二维码解码（扫码登录的地基）
 *
 * 为什么值得单独验：**"画了一堆方块"和"画对了二维码"是两件事**。
 * 模块数算错、原点偏了、采样点跨到隔壁格 —— 这些在屏幕上看着都像二维码，
 * 但扫不出来。所以判据不能是"有没有画"，得用二维码**自己的结构**：
 * 三个定位符是 7×7 的固定花纹，必须逐格吻合。
 * 这也是这一层唯一能自证的硬判据（没法在模拟器里真拿手机扫）。
 * ======================================================================== */

#define QR_TEST_URI "sdmc:/3ds/aiconsole/qr_test.txt"

static void probe_qr(void)
{
    static char uri[8192];
    char   msg[200];
    FILE  *f;
    size_t n;

    app_note("featprobe: ===== 二维码解码 =====");

    f = fopen(QR_TEST_URI, "r");
    if (!f) {
        app_note("featprobe: 卡上没有 %s —— 这条跳过"
                 "（它要用一张真实二维码来验，造不出来）", QR_TEST_URI);
        return;
    }
    n = fread(uri, 1, sizeof uri - 1, f);
    fclose(f);
    uri[n] = '\0';
    for (size_t i = 0; i < n; ++i)          /* 去掉可能的换行 */
        if (uri[i] == '\n' || uri[i] == '\r') { uri[i] = '\0'; break; }

    app_note("featprobe: 读到 data URI %u 字节", (unsigned)strlen(uri));

    {
        QrCode  q;
        QrState s = qr_decode_data_uri(uri, &q, msg, sizeof msg);

        app_note("featprobe: qr_decode = %s · %s", qr_state_text(s), msg);
        if (s != QR_OK) {
            app_note("featprobe: 结论 = 解码失败，扫码屏没法工作");
            return;
        }

        {
            int total = q.n * q.n;
            int black = 0;
            int bad;

            for (int j = 0; j < q.n; ++j)
                for (int i = 0; i < q.n; ++i)
                    black += q.mod[j][i] ? 1 : 0;

            bad = qr_finder_mismatch(&q);

            app_note("featprobe: 模块数 = %d（期望 29 = QR version 3）", q.n);
            /* 整数百分比：%.1f 会拉进 dtoa，栈受不了 */
            app_note("featprobe: 黑模块 %d/%d = %d.%d%%"
                     "（真实二维码通常 40~55%%）",
                     black, total, black * 100 / total,
                     (black * 1000 / total) % 10);
            app_note("featprobe: 三个定位符不符格数 = %d（期望 0）", bad);
            app_note("featprobe: 结论 = %s",
                     (q.n == 29 && bad == 0)
                         ? "解码正确（定位符逐格吻合，可以拿去画）"
                         : "**解码不对**：几何或采样有偏差");
        }
    }
}

void featprobe_run(void)
{
    app_note("featprobe: 开始（会真写一个测试用 locale.txt 然后撤销）");
    probe_locale();
    probe_shfont();
    probe_qr();
    app_note("featprobe: 结束");
}
