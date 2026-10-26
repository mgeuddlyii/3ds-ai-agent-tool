/*
 * sc_update.c -- 软件更新（OTA）
 *
 * 版面按"手机系统更新"的样子重排（2026-10-25 用户要求）：
 *
 *   上屏 = 事实。左上标题、右上**常驻当前版本**；下面一行状态；
 *          有进度时给一条分段进度条（w_progress）；再下面是把更新包摊开：
 *          当前版本 / 更新到 / 更新包文件名 / 体积 / 摘要 / 更新说明。
 *   下屏 = 动作。永远有**一个明确的「检查更新」按钮**（以前只有 SELECT，
 *          提示还被标题盖掉了，用户根本不知道能查）。有更新时变成
 *          「稍后 / 下载并安装」，忙时两个按钮变灰。
 *
 * 三条不变的安全语义（跟改版前完全一致，别在排版里改掉）：
 *   1. 焦点默认落在**安全的那一侧**：确认安装时默认是「稍后」，
 *      不是「下载并安装」——没细看就按 A，结果应当是"什么都没发生"。
 *   2. 安装永远要人按允许，auto_check 只影响"要不要提示"。
 *   3. 装之前会重新算一遍 sha256（进屏只做轻量扫描，见 ota_scan_local_fast）。
 *
 * 进度为什么能画出来：下载是阻塞调用，中间不会回主循环。所以进度由
 * 真正收数据的那一层（ota.c 的 file_sink）回调上来（ota_set_progress_cb），
 * 回调里直接重绘 —— 同一线程，没有并发问题。
 */

#include "screen.h"

#include "app.h"
#include "ota.h"
#include "widgets.h"
#include "content_panel.h"

#include <stdio.h>
#include <string.h>
#include <3ds.h>            /* osGetTime / svcSleepThread */
#include "uistr.h"

/* 下屏焦点：0 = 左，1 = 右。含义随形态变，但**永远左边是安全的那个**。 */
static int      g_focus;

static OtaManifest g_man;      /* 卡上那份 */
/* 这次看到的是**线上** manifest 还是卡上的。两者要分开存：
 * 线上下载完还要回到本地那份去校验/安装，覆盖了就没法核对。 */
static bool        g_online;
static OtaManifest g_remote;

/* ★ 2026-10-25：本屏现在有**两页**。
 *   0 = 程序（联网升级，原来的内容）
 *   1 = 内容包（原「内容更新」屏，整屏转发给 content_panel）
 *
 *   为什么不并列画在同一屏：两块都有自己的状态机（程序页有下载/校验进度，
 *   内容包页有 5 行 × 装/退两种模式 × 确认），320×240 的下屏放不下两套按钮，
 *   硬塞必然变成"按钮叠着、点到错的"。分页之后每页都有完整的一屏可用。 */
static int      g_tab;

static OtaState g_state;
static char     g_detail[128];
static char     g_result[256];  /* 比 ota_install 的 err 大：还要拼「安装失败：」 */
static bool     g_installed;

/* ---- 忙闲与进度 ---- */
enum { BUSY_NONE = 0, BUSY_CHECK, BUSY_DOWN, BUSY_VERIFY, BUSY_INSTALL };
static int      g_busy;
static int      g_pct;
static unsigned g_last_ms;      /* 限流：进度回调不是每 KB 都值得重绘 */

static bool can_install(void)
{
    return g_state == OTA_VERIFY_OK || g_state == OTA_AVAILABLE;
}

/* 当前界面上"这一份清单"该用哪份：线上有新版就用线上的，
 * 否则用卡上的（下载完之后 g_online 会归位，界面自然回到本地那份）。 */
static const OtaManifest *shown_man(void)
{
    return (g_online && g_remote.count > 0) ? &g_remote : &g_man;
}

/* 有线上新版等着下（还没落盘）。 */
static bool has_remote_update(void)
{
    return g_online && g_state == OTA_AVAILABLE && g_remote.count > 0;
}

/* 体积：小于 1MB 报 KB，不然报 MB —— 5122936 写成 "5002 KB" 没人算得清。 */
static void fmt_size(char *out, size_t cap, int bytes)
{
    if (bytes < 1024 * 1024)
        snprintf(out, cap, "%d KB", bytes / 1024);
    else
        snprintf(out, cap, "%.1f MB", (double)bytes / (1024.0 * 1024.0));
}

/* 进度条的文案（左上角那条状态行也用同一句，别两处写法不一致）。 */
static const char *busy_label(char *buf, size_t cap)
{
    switch (g_busy) {
        case BUSY_CHECK:   return S(S0875);          /* 正在检查更新… */
        case BUSY_DOWN:    snprintf(buf, cap, S(S0876), g_pct); return buf;
        case BUSY_VERIFY:  return S(S0967);          /* 正在校验… */
        case BUSY_INSTALL: return S(S0968);          /* 正在安装… */
        default:           return "";
    }
}

/* ★ 进屏用**轻量扫描**（跳过 sha256），安装前才做完整校验。
 *   见 ota.h 的 ota_scan_local_fast：完整扫描要全文件读 ~9MB，
 *   ARM11 上好几秒，而 enter 是同步调用，期间主循环冻结 = 用户说的"选更新很卡"。 */
static void update_enter(void)
{
    g_focus     = 0;
    g_installed = false;
    g_result[0] = '\0';
    g_busy      = BUSY_NONE;
    g_pct       = 0;
    g_tab       = 0;          /* 每次进来都从「程序」页开始 */

    ota_scan_local_fast(&g_man, &g_state, g_detail, sizeof g_detail);
    app_status_set("%s", ota_state_text(g_state));
}

/* --------------------------------------------------------------------------
 * 上屏：事实区
 * ----------------------------------------------------------------------- */

static void render_top(void)
{
    const float x = (float)W_TOP_MARGIN;

    /* 内容包页：整屏交给面板（它自带页眉/页脚与全部坐标）。 */
    if (g_tab == 1) {
        cu_panel_render_top();
        return;
    }
    const float w = (float)W_TOP_USABLE;
    float y = (float)W_TOP_CONTENT_Y;
    char  buf[96];
    char  num[32];

    /* 右上角**常驻**当前版本：手机的系统更新页也是这样，
     * 版本号必须在第一眼能看到的位置，不用滚不用翻。 */
    snprintf(buf, sizeof buf, "v%s", APP_VERSION_NAME);
    w_top_header(S(S0618), buf);

    /* ---- 状态行（或"正在干什么"）---- */
    if (g_busy) {
        y = w_line(x, y, w, busy_label(buf, sizeof buf), w_c(C_SLOT_ACCENT));
    } else {
        RColor sc = can_install() ? w_c(C_SLOT_OK)
                  : (g_state == OTA_ERR_TOO_OLD ? w_c(C_SLOT_WARN)
                                                : w_c(C_SLOT_WARN_DK));
        y = w_line(x, y, w, ota_state_text(g_state), sc);
    }

    /* ---- 进度条：只在本屏真的在忙时出现 ---- */
    if (g_busy) {
        y += 2;
        w_progress(x, y, w, 14, g_pct, NULL);
        y += 18;
    }

    y += 4;
    w_hr(x, y, w, w_c(C_SLOT_LINE));
    y += 8;

    /* ---- 版本与包信息 ----
     * 手机 OTA 的信息顺序：现在是什么 → 要变成什么 → 多大的包 → 说明。 */
    {
        const OtaManifest *m = shown_man();
        const bool show_new = can_install() || has_remote_update();

        snprintf(num, sizeof num, "v%s (%d)", APP_VERSION_NAME, APP_VERSION_CODE);
        /* ★ 2026-10-26：标签列 76 → 104。
         *   76 时键列可用宽只有 70px，而英文 "Current version" 要 94px ——
         *   中文界面看不出来（「当前版本」正好塞得下），一换英文就被排版护栏
         *   记成溢出。上屏有 400px 宽，多让 28px 给标签完全放得开。 */
        y = w_kv(x, y, 104, S(S0620), num,
                 w_c(C_SLOT_MUTED), w_c(C_SLOT_FG), TEXT_LINE_H);

        if (show_new && m->count > 0) {
            snprintf(num, sizeof num, "v%s (%d)", m->version_name, m->version_code);
            y = w_kv(x, y, 104, S(S0621), num,
                     w_c(C_SLOT_MUTED), w_c(C_SLOT_OK), TEXT_LINE_H);

            y = w_kv(x, y, 104, S(S0969), m->art[0].file,
                     w_c(C_SLOT_MUTED), w_c(C_SLOT_FG), TEXT_LINE_H);

            fmt_size(num, sizeof num, m->art[0].size);
            y = w_kv(x, y, 104, S(S0623), num,
                     w_c(C_SLOT_MUTED), w_c(C_SLOT_FG), TEXT_LINE_H);

            snprintf(num, sizeof num, "%.8s…", m->art[0].sha256);
            y = w_kv(x, y, 104, S(S0624), num,
                     w_c(C_SLOT_MUTED), w_c(C_SLOT_FAINT), TEXT_LINE_H);

            y += 4;
            w_hr(x, y, w, w_c(C_SLOT_LINE));
            y += 6;

            /* 更新说明：有就显示，没有就明说"这个包没写说明" */
            if (m->notes[0])
                w_wrap(x, y, w, m->notes, w_c(C_SLOT_FG), TEXT_LINE_H);
            else
                w_wrap(x, y, w, S(S0625), w_c(C_SLOT_MUTED), TEXT_LINE_H);
        } else if (g_busy) {
            /* 忙的时候不铺细节，只留一句"在做什么"，屏幕越简单越好读 */
            y = w_wrap(x, y, w, g_detail, w_c(C_SLOT_MUTED), TEXT_LINE_H);
        } else {
            /* 没更新可装：把当前状态和"怎么办"讲清楚 */
            y = w_wrap(x, y, w, g_detail, w_c(C_SLOT_MUTED), TEXT_LINE_H);
            y += 6;
            w_wrap(x, y, w, S(S0630), w_c(C_SLOT_FAINT), TEXT_LINE_H);
        }
    }

    /* 底部提示：**只在有话可说时给**。
     * 忙的时候状态行已经写着"正在……"，再重复一遍是噪音；
     * 没更新可装时更不该说"看清楚上面再决定"——上面根本没东西要决定。 */
    w_top_footer(app_status(),
                 has_remote_update() ? S(S0879)
               : can_install()       ? S(S0193)
               : S(S0975));      /* 「L/R 内容包」——页签提示，短句不挤按钮 */
}

/* --------------------------------------------------------------------------
 * 下屏：按钮
 * ----------------------------------------------------------------------- */

/* 矩形由 render 与 handle **共用**：各写一遍坐标，改版面时必然漂移成
 * "看到的位置点不到"，而那种 bug 在截图里完全看不出来。 */
static const Rect BTN_L = {   8, 148, 148, 38, 0 };
static const Rect BTN_R = { 164, 148, 148, 38, 0 };

static void render_bottom(void)
{
    const float x = (float)W_BOT_MARGIN;

    if (g_tab == 1) {
        cu_panel_render_bottom();
        return;
    }
    const float w = (float)W_BOT_USABLE;
    float y = (float)W_BOT_CONTENT_Y;
    char  buf[96];

    w_bot_header(S(S0629));

    /* ---- 装完了：只报结果 + 一个返回 ---- */
    if (g_installed) {
        y = w_wrap(x, y, w, g_result, w_c(C_SLOT_FG), TEXT_LINE_H);
        (void)y;
        w_button(&(Rect){ 60, 148, 200, 38, 0 }, S(S0233), W_BTN_PRIMARY);
        w_bot_footer(S(S0234));
        return;
    }

    /* ---- 忙：进度在**上屏**，下屏按钮变灰（点了也没用）----
     * 仍然画出来而不是消失：按钮消失会让人觉得"跑错屏了"。 */
    if (g_busy) {
        snprintf(buf, sizeof buf, S(S0876), g_pct);
        y = w_wrap(x, y, w, buf, w_c(C_SLOT_FG), TEXT_LINE_H);
        (void)y;

        w_button(&BTN_L, S(S0964), W_BTN_DISABLED);
        w_button(&BTN_R, S(S0233), W_BTN_DISABLED);
        w_bot_footer(S(S0629));
        return;
    }

    /* ---- 有线上新版：稍后 / 下载并安装 ---- */
    if (has_remote_update()) {
        y = w_wrap(x, y, w, S(S0633), w_c(C_SLOT_MUTED), TEXT_LINE_H);

        w_button(&BTN_L, S(S0965),
                 g_focus == 0 ? W_BTN_SELECTED : W_BTN_NORMAL);
        w_button(&BTN_R, S(S0966),
                 g_focus == 1 ? W_BTN_SELECTED : W_BTN_NORMAL);
        w_bot_footer(S(S0972));
        return;
    }

    /* ---- 卡上有包待安装：允许 / 拒绝（默认拒绝）----
     * ⚠️ 条件必须是 can_install()，**不能写成 == OTA_VERIFY_OK**。
     *   进屏只做轻量扫描，state 只能到 AVAILABLE（摘要还没算），
     *   写成 VERIFY_OK 会让这一支永远进不去 —— 表现是"按右+A 直接返回了"，
     *   而屏上看着一切正常。摘要是在 do_install() 里补算的。 */
    if (can_install()) {
        y = w_wrap(x, y, w, S(S0632), w_c(C_SLOT_FG), TEXT_LINE_H);

        w_button(&BTN_L, S(S0245),
                 g_focus == 0 ? W_BTN_SELECTED : W_BTN_NORMAL);
        w_button(&BTN_R, S(S0246),
                 g_focus == 1 ? W_BTN_SELECTED : W_BTN_NORMAL);
        w_bot_footer(S(S0635));
        return;
    }

    /* ---- 常态：检查更新 / 返回（这就是用户要的那个专门按钮）---- */
    /* ⚠️ w_wrap 返回**折行后的新 y**，必须接住。不接的话第二行会从
     *    同一个 y 开始画 —— 屏上表现为两行字叠在一起（像乱码）。 */
    y = w_wrap(x, y, w, ota_state_text(g_state), w_c(C_SLOT_FG), TEXT_LINE_H);
    y += 4;
    y = w_wrap(x, y, w, g_detail, w_c(C_SLOT_MUTED), TEXT_LINE_H);

    w_button(&BTN_L, S(S0964),
             g_focus == 0 ? W_BTN_SELECTED : W_BTN_PRIMARY);
    w_button(&BTN_R, S(S0233),
             g_focus == 1 ? W_BTN_SELECTED : W_BTN_NORMAL);
    /* ⚠️ 底栏**不折行**，只能放短提示。早先这里塞了 S0630 那句长话，
     *    实测 381px > 300px 直接被右边缘裁掉（护栏当场判红）。 */
    w_bot_footer(S(S0971));
}

/* --------------------------------------------------------------------------
 * 行为
 * ----------------------------------------------------------------------- */

/* 画几帧，让"正在……"真的出现在屏幕上。
 * 请求是阻塞的，进去以后主循环不再动 —— 没有这一段，
 * 用户看到的是"按了没反应"（这条以前踩过，见 do_check_online 老注释）。 */
static void pump(const char *label, int from, int to)
{
    (void)label;
    for (int p = from; p <= to; p += 6) {
        g_pct = p;
        app_render();
        svcSleepThread(30ull * 1000 * 1000);   /* 30ms，够刷一帧又不拖慢请求 */
    }
}

/* ---- 在线检查：只读 manifest（几百字节），不下包 ---- */
static void do_check_online(void)
{
    char d[160];
    OtaState st = OTA_ERR_NET;
    const char *ch = app_cfg()->ota.channel;

    g_busy = BUSY_CHECK;
    g_pct  = 6;
    app_status_set(S(S0875));

    memset(&g_remote, 0, sizeof g_remote);
    pump(S(S0875), 6, 36);      /* 6% → 36%：先让用户看见"它在动" */

    if (ota_fetch_remote(ch, &g_remote, &st, d, sizeof d)) {
        g_online = true;
        g_state  = st;
    } else {
        g_online = false;
        g_state  = st;
    }

    g_pct  = 100;
    g_busy = BUSY_NONE;
    snprintf(g_detail, sizeof g_detail, "%s", d);
    app_status_set("%s", ota_state_text(g_state));
    app_note("ota: online check state=%d ch=%s detail=%s",
             (int)g_state, ch, d);

    g_focus = 0;                /* 回到安全侧 */
}

/* 下载中由 ota.c 的 file_sink 回调上来。限流到 ~12fps：
 * 4MB 的包会回调上百次，每次都整帧重绘会把下载拖慢。 */
static void on_dl_progress(int pct, const char *file)
{
    unsigned now = osGetTime();

    (void)file;
    g_pct = pct;
    if (pct < 100 && now - g_last_ms < 80)
        return;
    g_last_ms = now;
    app_render();
}

/* ---- 下载：把**所有**产物拉到卡上 ----
 *
 * ★ 必须下全。manifest 可能同时带 3dsx 和 cia，而 ota_scan_local 会
 *   逐个校验 art[]；只下第一个的话，校验会报"缺文件 XXX"，
 *   用户看到的是"包不完整"，实际是我们自己没下完。
 *
 * ★ 下载完**不直接装**：回头走 ota_scan_local，让状态机自己走到
 *   VERIFY_OK。这样"装"这一步永远建立在"校验通过"上。 */
static void do_download(void)
{
    char err[192];
    const char *ch = app_cfg()->ota.channel;
    int done = 0;

    g_busy = BUSY_DOWN;
    g_pct  = 0;
    g_last_ms = 0;
    app_status_set(S(S0876), 0);
    app_render();

    ota_set_progress_cb(on_dl_progress);

    for (int i = 0; i < g_remote.count; ++i) {
        if (!ota_download(ch, &g_remote, i, err, sizeof err))
            break;
        done++;
    }
    ota_set_progress_cb(NULL);

    if (done == g_remote.count && done > 0) {
        /* ★ 先把**线上 manifest 原样落盘**，再回头校验。
         *   漏这一步的话，ota_scan_local 用的还是卡上旧的 manifest。 */
        {
            size_t rn = 0;
            const char *raw = ota_remote_raw(&rn);
            if (raw && rn) {
                FILE *mf;
                /* ★ 2026-10-25：用 ota_manifest_path()，别自己拼。
                 *   统一下载目录之后，清单可能在 /download，也可能回落在 /update，
                 *   硬拼就会把清单写到 A 目录、产物却在 B 目录 → 校验必失败。 */
                const char *mpath = ota_manifest_path();
                mf = fopen(mpath, "wb");
                if (mf) {
                    fwrite(raw, 1, rn, mf);
                    fclose(mf);
                    app_note("ota: manifest synced %d bytes", (int)rn);
                } else {
                    app_note("ota: manifest write FAILED");
                }
            }
        }

        g_busy = BUSY_VERIFY;
        g_pct  = 50;
        app_status_set(S(S0967));
        app_render();

        ota_scan_local(&g_man, &g_state, g_detail, sizeof g_detail);
        if (g_state == OTA_VERIFY_OK)
            app_status_set(S(S0877));
        else
            app_status_set("%s", ota_state_text(g_state));
        app_note("ota: downloaded %d/%d -> state=%d",
                 done, g_remote.count, (int)g_state);
    } else {
        g_state = OTA_ERR_IO;
        snprintf(g_detail, sizeof g_detail, "%s", err);
        app_status_set("%s", ota_state_text(g_state));
        app_note("ota: download failed at %d/%d: %s",
                 done, g_remote.count, err);
    }

    g_online = false;
    g_busy   = BUSY_NONE;
    g_pct    = 100;
    g_focus  = 0;
}

static void do_install(void)
{
    char err[160];

    /* ★ 装之前**补一次完整校验**（sha256）—— 进屏只做了轻量扫描。
     *   没通过校验的包永远装不上，区别只是这份代价挪到了"真要装的那一次"。 */
    if (g_state != OTA_VERIFY_OK) {
        OtaState st = OTA_NONE;
        char det[128];

        g_busy = BUSY_VERIFY;
        g_pct  = 40;
        app_status_set(S(S0967));
        app_render();

        if (!ota_scan_local(&g_man, &st, det, sizeof det) || st != OTA_VERIFY_OK) {
            g_busy = BUSY_NONE;
            g_state = st;
            snprintf(g_detail, sizeof g_detail, "%s", det);
            g_installed = true;
            snprintf(g_result, sizeof g_result, "%s", det);
            app_status_set("%s", ota_state_text(g_state));
            app_note("ota: pre-install verify failed: %s", det);
            return;
        }
        g_state = OTA_VERIFY_OK;
    }

    g_busy = BUSY_INSTALL;
    g_pct  = 60;
    app_status_set(S(S0968));
    app_render();

    bool ok = ota_install(&g_man, 0, err, sizeof err);

    g_busy = BUSY_NONE;
    g_pct  = 100;
    g_installed = true;
    if (ok) {
        g_state = OTA_DONE;
        snprintf(g_result, sizeof g_result, "%s", err);
        app_status_set(S(S0636));
        app_note("ota: installed %s", g_man.art[0].file);
    } else {
        snprintf(g_result, sizeof g_result, S(S0296), err);
        app_status_set(S(S0637));
        app_note("ota: install failed: %s", err);
    }
}

/* ---- 主按钮：当前形态下"右边那个"到底做什么 ----
 *  有线上新版 → 下载；卡上有包待装 → 装；否则 → 返回。 */
static void do_primary(void)
{
    if (has_remote_update()) {
        do_download();
        return;
    }
    if (can_install()) {          /* 同上：AVAILABLE 也算，摘要由 do_install 补 */
        if (g_focus == 1)
            do_install();
        else
            app_pop();          /* 拒绝：直接走人，不写任何东西 */
        return;
    }
    if (g_focus == 1)
        app_pop();
    else
        do_check_online();
}

static void update_handle(u32 down, u32 held)
{
    /* ---- 页签切换：L / R ---- */
    if (g_tab == 1) {
        if (down & (KEY_L | KEY_R)) {
            g_tab = 0;
            app_note("ota: tab -> program");
            return;
        }
        /* 其余键全部转发给内容包面板。**L/R 已在这里截走**，
         * 所以面板那边不会把肩键当成它自己的操作（它只认十字键）。 */
        cu_panel_handle(down, held);
        return;
    }
    if (down & (KEY_L | KEY_R)) {
        /* 正在下载/校验时不许切页：切走了进度就没人看了，
         * 而下载还在后台阻塞主循环 —— 用户会以为"卡住"。 */
        if (g_busy) {
            app_note("ota: tab switch ignored (busy)");
            return;
        }
        g_tab = 1;
        cu_panel_enter();
        app_note("ota: tab -> content");
        return;
    }

    (void)held;

    /* 忙的时候只认"别乱按"：任何键都不启动第二个动作。
     * 这里不 pop —— 中途退出会让用户以为"取消了"，而下载其实还在跑。 */
    if (g_busy) {
        if (down & (KEY_A | KEY_B))
            app_note("ota: busy, key ignored");
        return;
    }

    /* 看完整更新说明（上屏地方有限，说明长了要单独一屏） */
    if (down & KEY_X) {
        char path[192];
        snprintf(path, sizeof path, "%s/notes.html", ota_dir());
        app_open_viewer(path);
        return;
    }

    /* 重新扫一遍卡：用户可能刚把更新包放进去 */
    if (down & KEY_Y) {
        g_installed = false;
        g_result[0] = '\0';
        ota_scan_local_fast(&g_man, &g_state, g_detail, sizeof g_detail);
        app_status_set("%s", ota_state_text(g_state));
        return;
    }

    /* SELECT 保留成快捷键（从任何形态都能直接查在线） */
    if (down & KEY_SELECT) {
        do_check_online();
        return;
    }

    /* 触摸：**判定顺序与画出来的顺序一致**，且与 A 键同一套语义。 */
    {
        int tx, ty;
        if (app_touch_down(&tx, &ty)) {
            if (g_installed) {
                if (w_hit(&(Rect){ 60, 148, 200, 38, 0 }, tx, ty))
                    app_pop();
                return;
            }
            if (w_hit(&BTN_L, tx, ty)) {
                g_focus = 0;
                do_primary();
                return;
            }
            if (w_hit(&BTN_R, tx, ty)) {
                g_focus = 1;
                do_primary();
                return;
            }
        }
    }

    /* 只剩十字键切焦点 —— L/R 已经改成页签了，别再抢。 */
    if (down & (KEY_LEFT | KEY_RIGHT)) {
        g_focus = g_focus ? 0 : 1;
        return;
    }

    if (down & KEY_B) {
        app_pop();
        return;
    }

    if (down & KEY_A)
        do_primary();
}

const Screen g_screen_update = {
    .id            = SCREEN_UPDATE,
    .name          = "SCREEN_UPDATE",
    .enter         = update_enter,
    .leave         = NULL,
    .render_top    = render_top,
    .render_bottom = render_bottom,
    .handle        = update_handle,
};
