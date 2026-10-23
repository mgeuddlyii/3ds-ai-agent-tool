/*
 * sc_keys.c -- Key 管理屏
 *
 * 多把 Key 存在 keys.json（见 keystore.h），这里负责四件事：
 * 切换（A）、删除（X）、从 key.txt 导入新的一把（Y）、
 * **在机器上直接敲一把（L / 「新建」按钮）**。
 *
 * 双屏分工（§4.7）：上屏 = 仓库里有什么、哪把在用（事实区）；
 * 下屏 = 选中那把的详情和操作（操作区）。
 *
 * Key 全程打星显示（credfile_mask），别让旁观者或截图带走它。
 *
 * ★ "导入"与"新建"是两条**平行**的路，不是主次：
 *   导入吃的是卡上的 key.txt（人在电脑/手机上编辑，省打字功夫）；
 *   新建吃的是系统键盘（手边没有读卡器时唯一能走的路）。
 *   两条都必须在**仓库为空**时可达 —— 否则新用户哪条都进不去。
 */

#include "screen.h"

#include "app.h"
#include "credfile.h"
#include "keystore.h"
#include "widgets.h"

#include <3ds.h>
#include <stdio.h>
#include <string.h>
#include "uistr.h"

static int g_cursor;

/* 下屏四个按钮。**render 与 handle 共用同一份坐标**。
 *
 * ★ 顺带修掉一处出屏：原来是 3 个 96px 宽、从 x=10 排到 322 ——
 *   底屏只有 320，最后一个按钮**有 2px 被切在屏幕外**。
 *   现在是 4 个 70px + 6px 间隔 = 298px，整排落在 300px 可用宽内。 */
static const Rect KEY_BTN_SWITCH = {  10, 150, 70, 36, 0 };
static const Rect KEY_BTN_DEL    = {  86, 150, 70, 36, 0 };
static const Rect KEY_BTN_IMP    = { 162, 150, 70, 36, 0 };
static const Rect KEY_BTN_NEW    = { 238, 150, 70, 36, 0 };

/* 四个动作各抽成一个函数：键盘与触摸两条入口调用同一份实现。
 * 分成两份的话，"用触摸删除"和"用 X 删除"迟早出现行为差异
 * （比如只有一条会修正光标越界）。 */
static void do_switch(void)
{
    const KeyEntry *e = keystore_at(g_cursor);

    if (e && keystore_set_active(e->name))
        app_status_set(S(S0427), e->name);
    else
        app_status_set(S(S0428));
}

static void do_del(void)
{
    const KeyEntry *e = keystore_at(g_cursor);

    if (e && keystore_remove(e->name)) {
        app_status_set(S(S0429), e->name);
        if (g_cursor >= keystore_count() && g_cursor > 0)
            --g_cursor;
    } else {
        app_status_set(S(S0430));
    }
}

static void do_import(void)
{
    /* ★ 2026-10-22：key.txt 现在可以写 provider=custom + base_url=...
     *   但网址不住在仓库里（KeyEntry 没有这个字段），得由这一层写进
     *   cfg->ai.base_url —— keystore 在 shared 层，拿不到 app_cfg()。 */
    char url[96];

    if (keystore_import_from_credfile(url, sizeof url)) {
        g_cursor = keystore_count() - 1;
        app_status_set(S(S0431), keystore_count());

        if (url[0]) {
            AppConfig *c = app_cfg();

            snprintf(c->ai.base_url, sizeof c->ai.base_url, "%s", url);
            app_cfg_dirty();
            app_cfg_save_now();
            app_note("keys: import set base_url len=%d (custom)", (int)strlen(url));
        }
    } else {
        app_status_set(S(S0432));
    }
}

static void do_new(void)
{
    app_push(SCREEN_KEYEDIT);
}

static void keys_enter(void)
{
    keystore_load();                    /* 每次进屏现读：别的入口可能改过 */
    g_cursor = 0;

    if (keystore_empty())
        app_status_set(S(S0433));
    else
        app_status_set(S(S0434),
                       keystore_count(), keystore_active_name(),
                       keystore_is_encrypted() ? S(S0435) : S(S0436));
}

static void render_top(void)
{
    const float x = (float)W_TOP_MARGIN;
    const float w = (float)W_TOP_USABLE;
    float y = (float)W_TOP_CONTENT_Y;
    int i;
    const int n = keystore_count();

    w_top_header(S(S0437), keystore_active_name());

    /* 存储方式要写在屏幕上，而不是只进日志 —— 用户看不见的保护
     * 等于没有保护。加密是否生效是这一屏最该让人知道的事。 */
    {
        const bool enc = keystore_is_encrypted();
        w_wrap(x, y, w,
               enc ? S(S0438)
                   : S(S0439),
               enc ? w_c(C_SLOT_OK) : w_c(C_SLOT_WARN), TEXT_LINE_H);
        y += TEXT_LINE_H + 4;
    }

    if (n == 0) {
        y = w_wrap(x, y, w, S(S0798),
                   w_c(C_SLOT_MUTED), TEXT_LINE_H);
        w_top_footer(app_status(), S(S0441));
        return;
    }

    for (i = 0; i < n && y <= (float)W_TOP_CONTENT_B - 24; ++i) {
        const KeyEntry *e = keystore_at(i);
        char line[96];
        bool cur = (i == g_cursor);

        /* ★ "[在用]" 后面必须**留一个空格**：不加的话
         *   "[在用]deepseek-main" 会挤在一起，标签看上去像名字的一部分
         *   （用户说的"字飘出格子"就有这一处）。 */
        snprintf(line, sizeof line, "%s%s%s",
                 cur ? "> " : "  ",
                 !strcmp(e->name, keystore_active_name()) ? S(S0442) : "",
                 e->name);
        y = w_wrap(x, y, w, line,
                   cur ? w_c(C_SLOT_ACC_DK) : w_c(C_SLOT_FG), TEXT_LINE_H);

        snprintf(line, sizeof line, "    %s / %s",
                 e->provider[0] ? e->provider : "?",
                 e->model[0] ? e->model : S(S0443));
        y = w_wrap(x, y, w, line, w_c(C_SLOT_FAINT), TEXT_LINE_H);
    }

    w_top_footer(app_status(), S(S0444));
}

static void render_bottom(void)
{
    const float x = (float)W_BOT_MARGIN;
    const float w = (float)W_BOT_USABLE;
    float y = (float)W_BOT_CONTENT_Y;
    const KeyEntry *e = keystore_at(g_cursor);

    w_bot_header(S(S0445));

    if (!e) {
        w_wrap(x, y, w, S(S0446), w_c(C_SLOT_MUTED), TEXT_LINE_H);
        w_bot_footer(S(S0447));
        return;
    }

    y = w_wrap(x, y, w, e->name, w_c(C_SLOT_FG), TEXT_LINE_H);
    {
        char line[96];
        snprintf(line, sizeof line, "%s · %s",
                 e->provider[0] ? e->provider : "?",
                 e->model[0] ? e->model : S(S0443));
        y = w_wrap(x, y, w, line, w_c(C_SLOT_MUTED), TEXT_LINE_H);
    }

    {
        char masked[64];
        credfile_mask(e->key, masked, sizeof masked);
        y = w_wrap(x, y, w, masked, w_c(C_SLOT_FAINT), TEXT_LINE_H);
    }

    y += 10;
    w_hr(x, y, w, w_c(C_SLOT_LINE));

    const bool is_active = !strcmp(e->name, keystore_active_name());
    w_button(&KEY_BTN_SWITCH, S(S0448),
             (is_active ? W_BTN_NORMAL : W_BTN_PRIMARY) |
             (app_touch_on(&KEY_BTN_SWITCH) ? W_BTN_PRESSED : 0));
    w_button(&KEY_BTN_DEL, S(S0449),
             W_BTN_NORMAL | (app_touch_on(&KEY_BTN_DEL) ? W_BTN_PRESSED : 0));
    w_button(&KEY_BTN_IMP, S(S0450),
             W_BTN_NORMAL | (app_touch_on(&KEY_BTN_IMP) ? W_BTN_PRESSED : 0));
    /* 「新建」= 在机器上直接敲一把（不用拔卡去改 key.txt）。
     * 它与「导入」是两条平行的路：导入吃卡上的文件，新建吃系统键盘。 */
    w_button(&KEY_BTN_NEW, S(S0451),
             W_BTN_NORMAL | (app_touch_on(&KEY_BTN_NEW) ? W_BTN_PRESSED : 0));

    w_bot_footer(S(S0452));
}

static void keys_handle(u32 down, u32 held)
{
    (void)held;

    /* 触摸先查（按下沿只有一帧），和主界面同一套次序 */
    {
        int tx, ty;

        if (app_touch_down(&tx, &ty)) {
            if (w_hit(&KEY_BTN_SWITCH, tx, ty)) { do_switch(); return; }
            if (w_hit(&KEY_BTN_DEL,    tx, ty)) { do_del();    return; }
            if (w_hit(&KEY_BTN_IMP,    tx, ty)) { do_import(); return; }
            if (w_hit(&KEY_BTN_NEW,    tx, ty)) { do_new();    return; }
        }
    }

    if (down & KEY_SELECT) {
        app_push(SCREEN_APIKEY);        /* 怎么申请 Key 的教程页 */
        return;
    }
    if (down & KEY_Y) {
        do_import();
        return;
    }
    /* L = 在机器上敲一把。**空仓库也要能走这条路** ——
     * 仓库为空时下面那半段整个 return 掉了，L 必须放在它前面，
     * 否则"一把都没有"的新用户根本进不去新建屏（而那正是最需要它的时候）。 */
    if (down & KEY_L) {
        do_new();
        return;
    }

    const int n = keystore_count();
    if (n == 0) {
        if (down & KEY_B)
            app_pop();
        return;
    }

    if (down & KEY_UP) {
        if (g_cursor > 0) --g_cursor;
        return;
    }
    if (down & KEY_DOWN) {
        if (g_cursor < n - 1) ++g_cursor;
        return;
    }
    if (down & KEY_A) {
        do_switch();
        return;
    }
    if (down & KEY_X) {
        do_del();
        return;
    }
    if (down & KEY_B)
        app_pop();
}

const Screen g_screen_keys = {
    .id            = SCREEN_KEYS,
    .name          = "SCREEN_KEYS",
    .enter         = keys_enter,
    .leave         = NULL,
    .render_top    = render_top,
    .render_bottom = render_bottom,
    .handle        = keys_handle,
};
