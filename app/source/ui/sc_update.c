/*
 * sc_update.c -- 软件更新（OTA）
 *
 * 这一屏是 §4.7 R3 的样板间：
 *   上屏 = 全部事实。版本差、体积、摘要、来源、更新说明，一个字不省。
 *   下屏 = 只有「允许」和「拒绝」两个大按钮。
 *
 * 焦点默认落在**拒绝**上。这不是排版习惯，是安全默认值：
 * 用户没细看就顺手按 A，结果应当是"什么都没发生"，而不是"装了"。
 *
 * 安装永远不自动开始。config 里的 auto_check 只影响"要不要提示"，
 * 不影响"要不要装"。这两件事一旦合并，通道就变成了远程执行。
 */

#include "screen.h"

#include "app.h"
#include "ota.h"
#include "widgets.h"

#include <stdio.h>
#include <string.h>
#include "uistr.h"

/* 0 = 拒绝（默认），1 = 允许 */
static int      g_focus;
static OtaManifest g_man;
/* 这次看到的是**线上** manifest 还是卡上的。两者要分开存：
 * 线上下载完还要回到本地那份去校验/安装，覆盖了就没法核对。 */
static bool        g_online;
static OtaManifest g_remote;

static OtaState g_state;
static char     g_detail[128];
/* 要比 ota_install 的 err 缓冲大：装失败时前面还要拼「安装失败：」几个字 */
static char     g_result[256];
static bool     g_installed;

/*: 能走到"要不要装"那个确认形态吗。
 *  两种状态都算：VERIFY_OK（已完整校验过，会话内重复进入时）与
 *  AVAILABLE（轻量扫描，摘要还没验）—— 后者在 do_install() 之前
 *  会补一次完整校验，不过就拒绝并说明原因，不会带着未校验的包去装。 */
static bool can_install(void)

{
    return g_state == OTA_VERIFY_OK || g_state == OTA_AVAILABLE;
}

/* ★ 进屏用**轻量扫描**（跳过 sha256），安装前才做完整校验。
 *   见 ota.h 的 ota_scan_local_fast：完整扫描要全文件读 ~9MB，
 *   ARM11 上好几秒，而 enter 是同步调用，期间主循环冻结 = 用户说的"选更新很卡"。
 *   轻量扫描只做 manifest 解析 + size 检查（全是元数据操作），立刻出屏。 */
static void update_enter(void)
{
    g_focus     = 0;          /* 安全默认：拒绝 */
    g_installed = false;
    g_result[0] = '\0';

    ota_scan_local_fast(&g_man, &g_state, g_detail, sizeof g_detail);
    app_status_set("%s", ota_state_text(g_state));
}

/* --------------------------------------------------------------------------
 * 上屏：事实区
 * ----------------------------------------------------------------------- */

static void render_top(void)
{
    const float x = (float)W_TOP_MARGIN;
    const float w = (float)W_TOP_USABLE;
    float y = (float)W_TOP_CONTENT_Y;

    w_top_header(S(S0618), S(S0619));

    /* 状态行：能装是绿色，装不了是黄/红，别让人猜 */
    RColor sc = can_install() ? w_c(C_SLOT_OK)
              : (g_state == OTA_ERR_TOO_OLD ? w_c(C_SLOT_WARN) : w_c(C_SLOT_WARN_DK));
    y = w_line(x, y, w, ota_state_text(g_state), sc);

    y += 4;
    w_hr(x, y, w, w_c(C_SLOT_LINE));
    y += 10;

    if (can_install()) {
        char line[96];

        snprintf(line, sizeof line, "%s（v%d）", APP_VERSION_NAME,
                 APP_VERSION_CODE);
        y = w_kv(x, y, 84, S(S0620), line, w_c(C_SLOT_MUTED), w_c(C_SLOT_FG), TEXT_LINE_H);

        snprintf(line, sizeof line, "%s（v%d）", g_man.version_name,
                 g_man.version_code);
        y = w_kv(x, y, 84, S(S0621), line, w_c(C_SLOT_MUTED), w_c(C_SLOT_OK), TEXT_LINE_H);

        if (g_man.count > 0) {
            snprintf(line, sizeof line, S(S0622), g_man.count,
                     g_man.art[0].size / 1024);
            y = w_kv(x, y, 84, S(S0623), line, w_c(C_SLOT_MUTED), w_c(C_SLOT_FG), TEXT_LINE_H);

            snprintf(line, sizeof line, "%.16s...", g_man.art[0].sha256);
            y = w_kv(x, y, 84, S(S0624), line, w_c(C_SLOT_MUTED), w_c(C_SLOT_FG), TEXT_LINE_H);
        }

        y += 6;
        w_hr(x, y, w, w_c(C_SLOT_LINE));
        y += 8;

        if (g_man.notes[0])
            w_wrap(x, y, w, g_man.notes, w_c(C_SLOT_FG), TEXT_LINE_H);
        else
            w_wrap(x, y, w, S(S0625), w_c(C_SLOT_MUTED), TEXT_LINE_H);
    } else {
        /* 装不了时把原因讲清楚，而不是只给一个状态码 */
        y = w_wrap(x, y, w, g_detail, w_c(C_SLOT_FG), TEXT_LINE_H);
        y += 10;

        w_panel(&(Rect){ x, y, w, 36, 0 }, w_c(C_SLOT_ACC_LT), w_c(C_SLOT_ACCENT));
        r_text(&(Rect){ x + 10, y + 3,  w - 20, 16, 0 }, S(S0291), w_c(C_SLOT_ACC_DK));
        r_text(&(Rect){ x + 10, y + 19, w - 20, 16, 0 }, ota_dir(), w_c(C_SLOT_FG));

        y += 44;
        w_wrap(x, y, w,
               S(S0626),
               w_c(C_SLOT_MUTED), TEXT_LINE_H);
    }

    w_top_footer(app_status(),
                 (g_online && g_state == OTA_AVAILABLE) ? S(S0879)
               : g_installed ? S(S0627) : S(S0193));
}

/* --------------------------------------------------------------------------
 * 下屏：只有两个按钮
 * ----------------------------------------------------------------------- */

/* ★ 三个按钮矩形由 render 和 handle **共用**（早先是 render 里现算的
 * 临时 Rect，触屏没法复用）。各写一遍坐标的话，改版面时必然漂移成
 * "看到的位置点不到"，而那种 bug 在截图里完全看不出来。
 * 左边=拒绝(g_focus 0)、右边=允许(g_focus 1)，与 A 键语义一致。 */
static const Rect UPD_BTN_SINGLE = {  70, 150, 180, 36, 0 };  /* 单按钮形态 */
static const Rect UPD_BTN_NO     = {  10, 150, 144, 36, 0 };  /* 拒绝 */
static const Rect UPD_BTN_YES    = { 166, 150, 144, 36, 0 };  /* 允许 */

static void render_bottom(void)
{
    const float x = (float)W_BOT_MARGIN;
    const float w = (float)W_BOT_USABLE;
    float y = (float)W_BOT_CONTENT_Y;

    w_bot_header(can_install() ? S(S0628) : S(S0629));

    if (g_installed) {
        y = w_wrap(x, y, w, g_result, w_c(C_SLOT_FG), TEXT_LINE_H);
        w_button(&UPD_BTN_SINGLE,
                 S(S0233), W_BTN_PRIMARY);
        w_center((float)R_BOT_W / 2.0f, 192, S(S0897), w_c(C_SLOT_FAINT));
        w_bot_footer(S(S0234));
    /* ★ 这里原来画了一条"SELECT 检查在线"的常驻提示，位置是
     *   W_BOT_CONTENT_Y - 10（= y20）—— 那是**标题行**，被 w_bot_header
     *   盖得严严实实，屏幕上什么都看不到（2026-10-17 用户实测：
     *   "底下也没有写 select 的检查更新"）。提示现在并入 y=192 那行。 */
        return;
    }

    if (!can_install()) {
        y = w_wrap(x, y, w,
                   S(S0630),
                   w_c(C_SLOT_FG), TEXT_LINE_H);
        y += 8;
        w_wrap(x, y, w, g_detail, w_c(C_SLOT_MUTED), TEXT_LINE_H);

        w_button(&UPD_BTN_SINGLE,
                 S(S0233), W_BTN_PRIMARY);
        w_center((float)R_BOT_W / 2.0f, 192, S(S0898), w_c(C_SLOT_FAINT));
        w_bot_footer(S(S0234));
        return;
    }

    /* ---- 真正需要确认的形态：两个按钮，没有第三种可能 ---- */
    y = w_wrap(x, y, w,
               S(S0632),
               w_c(C_SLOT_FG), TEXT_LINE_H);
    y += 8;
    w_wrap(x, y, w, S(S0633), w_c(C_SLOT_MUTED),
           TEXT_LINE_H);

    w_button(&UPD_BTN_NO, S(S0245),
             g_focus == 0 ? W_BTN_SELECTED : W_BTN_NORMAL);
    w_button(&UPD_BTN_YES, S(S0246),
             g_focus == 1 ? W_BTN_SELECTED : W_BTN_NORMAL);

    w_center((float)R_BOT_W / 2.0f, 192, S(S0899), w_c(C_SLOT_FAINT));
    w_bot_footer(S(S0635));
}

/* --------------------------------------------------------------------------
 * 行为
 * ----------------------------------------------------------------------- */

/* ---- 在线检查：只读 manifest（几百字节），不下包 ---- */
static void do_check_online(void)
{
    char d[160];
    OtaState st = OTA_ERR_NET;
    const char *ch = app_cfg()->ota.channel;

    app_status_set(S(S0875));
    /* ★ 刷一帧再发请求：ota_fetch_remote 同步阻塞，期间主循环不重绘。
     *   只设 status 不 render = 状态变了但屏幕没变，用户看到"按了没反应"。 */
    app_render();
    memset(&g_remote, 0, sizeof g_remote);

    if (ota_fetch_remote(ch, &g_remote, &st, d, sizeof d)) {
        g_online = true;
        g_state  = st;
    } else {
        g_online = false;
        g_state  = st;
    }
    snprintf(g_detail, sizeof g_detail, "%s", d);
    app_status_set("%s", ota_state_text(g_state));
    app_note("ota: online check state=%d ch=%s detail=%s",
             (int)g_state, ch, d);
}

/* ---- 下载：把**所有**产物拉到卡上 ----
 *
 * ★ 必须下全。manifest 可能同时带 3dsx 和 cia，而 ota_scan_local 会
 *   逐个校验 art[]；只下第一个的话，校验会报"缺文件 XXX"，
 *   用户看到的是"包不完整"，实际是我们自己没下完。
 *
 * ★ 下载完**不直接装**：回头走 ota_scan_local，让状态机自己走到
 *   VERIFY_OK。这样"装"这一步永远建立在"校验通过"上，
 *   而不是建立在"我刚才下载成功了"这个更弱的假设上。 */
static void do_download(void)
{
    char err[192];
    const char *ch = app_cfg()->ota.channel;
    int done = 0;

    for (int i = 0; i < g_remote.count; ++i) {
        app_status_set(S(S0876), (i * 100) / (g_remote.count ? g_remote.count : 1));
        app_render();   /* 让"正在下载（x%）"刷到屏上 */
        if (!ota_download(ch, &g_remote, i, err, sizeof err))
            break;
        done++;
    }

    if (done == g_remote.count && done > 0) {
        /* ★ 先把**线上 manifest 原样落盘**，再回头校验。
         *   漏这一步的话，ota_scan_local 用的还是卡上旧的 manifest，
         *   拿它的 size/sha 去核对刚下载的产物 —— 必然不符，
         *   而错误信息会写成"大小不符"，把"我们没同步 manifest"说成"包坏了"。 */
        {
            size_t rn = 0;
            const char *raw = ota_remote_raw(&rn);
            if (raw && rn) {
                char mpath[192];
                FILE *mf;
                snprintf(mpath, sizeof mpath, "%s/manifest.json", ota_dir());
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
        /* 回到本地那份：它现在既有 manifest 又有产物，能全量校验 */
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
}

static void do_install(void)
{
    char err[160];

    /* ★ 装之前**补一次完整校验**（sha256）。
     *   进屏用的是轻量扫描（只验了 size），摘要闸门挪到了这里 ——
     *   用户点了「允许」才付全文件哈希这份代价。
     *   语义与"进屏就完整校验"完全一致：没通过校验的包永远装不上，
     *   区别只是这份代价从"每次进屏"挪到了"真正要装的那一次"。
     *   校验失败时把状态与原因如实更新到屏上，不静默。 */
    if (g_state != OTA_VERIFY_OK) {
        OtaState st = OTA_NONE;
        char det[128];
        app_status_set(S(S0877));       /* 校验中… */
        app_render();                   /* 先把"校验中"刷出去，别让用户以为卡了 */
        if (!ota_scan_local(&g_man, &st, det, sizeof det) || st != OTA_VERIFY_OK) {
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

    bool ok = ota_install(&g_man, 0, err, sizeof err);

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

/* A 键 / 点「允许」/ 点唯一的那个按钮 —— 三者是**同一个动作**。
 * 抽出来是为了让按键与触摸不可能走岔（早先触摸没接，
 * 于是"更新确认"这种最该能点的对话框反而只能按键）。 */
static void do_primary(void)
{
    /* 线上有新版、还没下到卡上 → 就是"下载"。
     * 语义连续：主按钮在这个屏一直是"继续下一步"，
     * 只不过下一步从"装"变成了"先下"。 */
    if (g_online && g_state == OTA_AVAILABLE) {
        do_download();
        return;
    }
    if (g_focus == 1)
        do_install();
    else
        app_pop();              /* 拒绝：直接走人，不写任何东西 */
}

static void update_handle(u32 down, u32 held)
{
    (void)held;

    /* 看完整更新说明。上屏地方有限，说明长了要单独一屏 */
    if (down & KEY_X) {
        char path[192];
        snprintf(path, sizeof path, "%s/notes.html", ota_dir());
        app_open_viewer(path);
        return;
    }

    if (down & KEY_Y) {
        /* 重新扫一遍：用户可能刚把更新包放进卡里。
         * 用轻量版（同 enter）—— 重扫是交互动作，不该让界面冻结几秒；
         * 摘要留到 do_install() 前补。 */
        g_installed = false;
        g_result[0] = '\0';
        ota_scan_local_fast(&g_man, &g_state, g_detail, sizeof g_detail);
        app_status_set("%s", ota_state_text(g_state));
        return;
    }

    /* ★ SELECT 必须在下面那道"闸"**之前**。
     *   闸的语义是"当前能不能装"，而 SELECT 是"去看看有没有可装的"——
     *   把它放闸后面，等于"必须先有可装的包，才能检查有没有包"，
     *   自相矛盾。2026-10-17 用户实测"按 SELECT 一点反应都没有"就是这个：
     *   没包 → can_install() false → 闸里 return，SELECT 永远执行不到。 */
    if (down & KEY_SELECT) {
        do_check_online();
        return;
    }

    /* 触摸：下屏三个按钮。**语义与 A 键完全同一套**（闸 / 下载 / 安装 / 返回），
     * 不另造一套 —— 否则会出现"点得到但行为和按键不一样"。
     * ⚠️ 必须放在下面那道闸**之前**：闸对 A/B 直接 return，
     *    触摸不是 A，放后面就永远执行不到（和 SELECT 当年同一个坑）。 */
    {
        int tx, ty;
        if (app_touch_down(&tx, &ty)) {
            const bool hit_no  = w_hit(&UPD_BTN_NO,  tx, ty);
            const bool hit_yes = w_hit(&UPD_BTN_YES, tx, ty);
            const bool hit_one = w_hit(&UPD_BTN_SINGLE, tx, ty);

            if (hit_no) {           /* 拒绝 */
                app_pop();
                return;
            }
            if (hit_yes || hit_one) {
                /* 闸关着（装完了 / 当前不可装且线上也没有）→ 点它只是返回 */
                if (g_installed
                    || (!can_install()
                        && !(g_online && g_state == OTA_AVAILABLE))) {
                    app_pop();
                    return;
                }
                if (hit_yes)
                    g_focus = 1;
                do_primary();
                return;
            }
        }
    }

    /* 闸：装完了、或者当前不可装 —— 但**线上有新版待下载**是个例外，
     * 那时 g_state 只能是 AVAILABLE（产物还没落到卡上，没法 VERIFY_OK）。
     * 不放行的话，A 在这条路上会直接 pop，用户永远下不了。 */
    if (g_installed || (!can_install()
                        && !(g_online && g_state == OTA_AVAILABLE))) {
        if (down & (KEY_A | KEY_B))
            app_pop();
        return;
    }

    if (down & (KEY_LEFT | KEY_RIGHT | KEY_L | KEY_R)) {
        g_focus = g_focus ? 0 : 1;
        return;
    }

    if (down & KEY_B) {
        app_pop();
        return;
    }

    if (down & KEY_A) {
        do_primary();
    }
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
