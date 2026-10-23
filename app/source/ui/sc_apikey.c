/*
 * sc_apikey.c -- 新手教程：怎么弄到 API Key，放到哪里，程序怎么读
 *
 * 这一屏的存在理由：3DS 上没有粘贴板，虚拟键盘敲一长串字符是酷刑。
 * 所以教程的重点不是"教你用 3DS 打字"，而是**教你避开打字** ——
 * 在电脑或手机上改一个文件，回来这边自动读。
 *
 * 双屏分工按 §4.7：
 *   上屏 = 事实区：这是啥、去哪弄、文件在哪、现在读到了什么
 *   下屏 = 操作区：具体怎么做一二三四 + 按钮
 */

#include "screen.h"

#include "app.h"
#include "config.h"
#include "credfile.h"
#include "keystore.h"
#include "widgets.h"

#include <stdio.h>
#include <string.h>
#include "uistr.h"

#define APIKEY_PAGES 4

static int   g_page;
static CredFile g_cred;
static bool  g_template_created;
static bool  g_saved;

/* -------------------------------------------------------------------------- */

/* ★ 存**编号**而不是字符串。静态初始化里必须是编译期常量，
 * 而 S() 是函数调用 —— 症状是编译期一句
 * "initializer element is not constant"，却指着 uistr.h 的宏定义。
 * 第 0 条不是文案（是域名），所以用 0 表示"直接给字面量"。 */
static const short PROVIDER_IDS[] = { 0, S0052, S0053 };
#define PROVIDER_N ((int)(sizeof(PROVIDER_IDS) / sizeof(PROVIDER_IDS[0])))

/* ★ 取步骤文案。必须放**文件级**：C 不允许在函数里定义函数
 * （报 "invalid storage class for function"）。
 * 同 PROVIDER_LINES 为什么不用数组的理由见 uistr.h。 */
static const char *step_line(int i)
{
    switch (i) {
    case 0:  return S(S0070);
    case 1:  return S(S0071);
    case 2:  return S(S0072);
    case 3:  return S(S0073);
    case 4:  return S(S0074);
    default: return "";
    }
}
#define STEP_N 5

/* --------------------------------------------------------------------------
 * 上屏：事实区
 * ----------------------------------------------------------------------- */

static void page0_top(void)
{
    const float x = (float)W_TOP_MARGIN;
    const float w = (float)W_TOP_USABLE;
    float y = (float)W_TOP_CONTENT_Y;

    y = w_wrap(x, y, w,
               S(S0054),
               w_c(C_SLOT_FG), TEXT_LINE_H);
    y += 6;
    y = w_wrap(x, y, w,
               S(S0055),
               w_c(C_SLOT_FG), TEXT_LINE_H);

    y += 12;
    w_panel(&(Rect){ x, y, w, 46, 0 }, w_c(C_SLOT_ACC_LT), w_c(C_SLOT_ACCENT));
    w_wrap(x + 10, y + 8, w - 20,
           S(S0056),
           w_c(C_SLOT_ACC_DK), TEXT_LINE_H);
}

static void page1_top(void)
{
    const float x = (float)W_TOP_MARGIN;
    const float w = (float)W_TOP_USABLE;
    float y = (float)W_TOP_CONTENT_Y;

    y = w_wrap(x, y, w,
               S(S0057),
               w_c(C_SLOT_FG), TEXT_LINE_H);

    y += 10;
    const float ph = (float)(PROVIDER_N * TEXT_LINE_H) + 8;
    w_panel(&(Rect){ x, y, w, ph, 0 }, w_c(C_SLOT_PANEL), w_c(C_SLOT_LINE));

    float ky = y + 4;
    for (int i = 0; i < PROVIDER_N; ++i)
        ky = w_line(x + 10, ky, w - 20,
                   PROVIDER_IDS[i] ? S(PROVIDER_IDS[i])
                                     : "DeepSeek   platform.deepseek.com",
                   w_c(C_SLOT_FG));

    y = ky + 10;
    w_wrap(x, y, w,
           S(S0058),
           w_c(C_SLOT_MUTED), TEXT_LINE_H);
}

static void page2_top(void)
{
    const float x = (float)W_TOP_MARGIN;
    const float w = (float)W_TOP_USABLE;
    float y = (float)W_TOP_CONTENT_Y;

    y = w_wrap(x, y, w, S(S0059),
               w_c(C_SLOT_FG), TEXT_LINE_H);

    y += 10;
    /* 路径单独一个面板 + 更深的颜色，这是用户唯一必须记住的东西 */
    w_panel(&(Rect){ x, y, w, 34, 0 }, w_c(C_SLOT_ACC_LT), w_c(C_SLOT_ACCENT));
    w_line(x + 10, y + 9, w - 20, credfile_path_display(), w_c(C_SLOT_ACC_DK));

    y += 46;
    if (g_template_created) {
        w_wrap(x, y, w,
               S(S0758),
               w_c(C_SLOT_FG), TEXT_LINE_H);
    } else {
        w_wrap(x, y, w,
               S(S0061),
               w_c(C_SLOT_FG), TEXT_LINE_H);
    }
}

static void page3_top(void)
{
    const float x = (float)W_TOP_MARGIN;
    const float w = (float)W_TOP_USABLE;
    float y = (float)W_TOP_CONTENT_Y;

    y = w_wrap(x, y, w, S(S0062),
               w_c(C_SLOT_FG), TEXT_LINE_H);

    y += 12;
    const float ph = 3 * TEXT_LINE_H + 10;
    w_panel(&(Rect){ x, y, w, ph, 0 }, w_c(C_SLOT_PANEL), w_c(C_SLOT_LINE));

    char masked[64];
    credfile_mask(g_cred.key, masked, sizeof masked);

    float ky = y + 5;
    if (g_cred.status == CREDFILE_OK) {
        ky = w_kv(x + 10, ky, 76, S(S0063), S(S0064), w_c(C_SLOT_MUTED), w_c(C_SLOT_OK), TEXT_LINE_H);
        ky = w_kv(x + 10, ky, 76, "Key", masked, w_c(C_SLOT_MUTED), w_c(C_SLOT_FG), TEXT_LINE_H);
        w_kv(x + 10, ky, 76, S(S0065),
             g_cred.provider[0] ? g_cred.provider : S(S0066),
             w_c(C_SLOT_MUTED), w_c(C_SLOT_FG), TEXT_LINE_H);
    } else {
        ky = w_kv(x + 10, ky, 76, S(S0063), credfile_status_text(g_cred.status),
                  w_c(C_SLOT_MUTED), w_c(C_SLOT_WARN), TEXT_LINE_H);
        w_kv(x + 10, ky, 76, S(S0067), credfile_path_display(), w_c(C_SLOT_MUTED), w_c(C_SLOT_FG),
             TEXT_LINE_H);
    }

    y += ph + 12;
    /* 这句必须说清楚，否则用户会以为已经配好了。
     * M5 之前这里写的是"存起来要等 M5"；现在链已经通了（按 A 就走
     * creds.enc 加密落盘），文案跟着改 —— 但要说清加密**挡得住什么**，
     * 别写成"军事级加密"。3DS 用户态没有安全隔区，如实说。 */
    w_wrap(x, y, w,
           g_cred.status == CREDFILE_OK ? S(S0068) : S(S0069),
           w_c(C_SLOT_MUTED), TEXT_LINE_H);
}

static void apikey_render_top(void)
{
    switch (g_page) {
        case 0:  page0_top(); break;
        case 1:  page1_top(); break;
        case 2:  page2_top(); break;
        default: page3_top(); break;
    }
    w_dots((float)R_TOP_W - 46.0f, 205.0f, APIKEY_PAGES, g_page, w_c(C_SLOT_ACCENT), w_c(C_SLOT_LINE));
}

/* --------------------------------------------------------------------------
 * 下屏：操作区
 * ----------------------------------------------------------------------- */

/* 唯一那个按钮的矩形（render 与 handle 共用）。 */
static const Rect APIKEY_BTN = { 70, 156, 180, 36, 0 };

static void apikey_render_bottom(void)
{
    const float x = (float)W_BOT_MARGIN;
    const float w = (float)W_BOT_USABLE;
    float y = (float)W_BOT_CONTENT_Y;

    /* STEPS 的取法见文件级 step_line() */

    switch (g_page) {
        case 0:
            w_bot_header(S(S0075));
            y = w_wrap(x, y, w,
                       S(S0076),
                       w_c(C_SLOT_FG), TEXT_LINE_H);
            y += 10;
            w_wrap(x, y, w, S(S0077), w_c(C_SLOT_MUTED), TEXT_LINE_H);
            break;

        case 1:
            w_bot_header(S(S0078));
            y = w_wrap(x, y, w,
                       S(S0759),
                       w_c(C_SLOT_FG), TEXT_LINE_H);
            y += 8;
            w_wrap(x, y, w, S(S0080), w_c(C_SLOT_MUTED),
                   TEXT_LINE_H);
            break;

        case 2:
            w_bot_header(S(S0081));
            for (int i = 0; i < 5; ++i)
                y = w_wrap(x, y, w, step_line(i), w_c(C_SLOT_FG), TEXT_LINE_H);
            break;

        default:
            w_bot_header(S(S0082));
            if (g_cred.status == CREDFILE_OK) {
                y = w_wrap(x, y, w, S(S0083),
                           w_c(C_SLOT_FG), TEXT_LINE_H);
            } else {
                y = w_wrap(x, y, w,
                           S(S0084),
                           w_c(C_SLOT_FG), TEXT_LINE_H);
                y += 6;
                w_wrap(x, y, w, S(S0085),
                       w_c(C_SLOT_MUTED), TEXT_LINE_H);
            }
            break;
    }

    const bool ok = (g_cred.status == CREDFILE_OK);
    const char *btn = (g_page == APIKEY_PAGES - 1)
                        ? (ok ? S(S0086) : S(S0087))
                        : S(S0088);

    w_button(&APIKEY_BTN, btn, W_BTN_PRIMARY);

    if (g_page == APIKEY_PAGES - 1)
        w_center((float)R_BOT_W / 2.0f, 198, S(S0089), w_c(C_SLOT_FAINT));
    else
        w_center((float)R_BOT_W / 2.0f, 198, S(S0090), w_c(C_SLOT_FAINT));

    w_bot_footer(g_page == APIKEY_PAGES - 1 ? S(S0091) : S(S0092));
}

/* --------------------------------------------------------------------------
 * 行为
 * ----------------------------------------------------------------------- */

static void do_rescan(void)
{
    credfile_scan(&g_cred);
    if (g_cred.status == CREDFILE_OK)
        app_status_set(S(S0093), g_cred.key_len);
    else
        app_status_set("%s", credfile_status_text(g_cred.status));
}

/* 离开这一屏的统一出口：把"教程看过了"记下来再走。
 * 跳过的也算看过 —— 不记的话，用户只是想进去瞄一眼，下次却还得重走整条引导。
 * 想重复看？那是主界面上一个「重看教程」入口该管的事，不该由这个字段兼管。 */
static void finish_to_home(void)
{
    AppConfig *c = app_cfg();
    c->tutorial_completed = true;
    app_cfg_dirty();
    app_cfg_save_now();
    app_switch(SCREEN_HOME);
}

/* 把 Key 收进仓库。仓库的落盘在 M5 之后走 creds.enc 加密路径
 * （见 keystore.c 的 save），所以这里不再像 M4 那样"只记服务商、
 * 把 Key 丢掉"。 */
static void do_save_provider(void)
{
    AppConfig *c = app_cfg();
    /* ★ 2026-10-22：key.txt 可写 provider=custom + base_url=...
     *   网址不属于仓库（KeyEntry 没有该字段），得在这一层写进 cfg。 */
    char url[96];

    if (g_cred.provider[0])
        snprintf(c->ai.provider, sizeof c->ai.provider, "%s", g_cred.provider);
    if (g_cred.model[0])
        snprintf(c->ai.model, sizeof c->ai.model, "%s", g_cred.model);

    /* 收进加密仓库。导入本身会把新的一把设为激活（仓库原本为空时）。 */
    bool stored = keystore_import_from_credfile(url, sizeof url);

    if (stored && url[0]) {
        snprintf(c->ai.base_url, sizeof c->ai.base_url, "%s", url);
        app_cfg_dirty();
        app_cfg_save_now();
    }

    g_saved = true;
    if (stored)
        app_status_set(S(S0094), keystore_count());
    else
        app_status_set(S(S0095));

    finish_to_home();
}

/* A 键 / 点按钮 —— 同一个动作：翻到下一页；最后一页则是「去配」或「返回」。 */
static void do_primary(void)
{
    if (g_page < APIKEY_PAGES - 1) {
        ++g_page;
        return;
    }
    if (g_cred.status == CREDFILE_OK)
        do_save_provider();
    else
        finish_to_home();
}

static void apikey_handle(u32 down, u32 held)
{
    (void)held;

    if (down & (KEY_RIGHT | KEY_R)) {
        if (g_page < APIKEY_PAGES - 1) ++g_page;
        return;
    }
    if (down & (KEY_LEFT | KEY_L)) {
        if (g_page > 0) --g_page;
        return;
    }

    if (down & KEY_Y) {
        do_rescan();
        return;
    }

    /* 跳过。之所以走得这么干脆：主界面上随时有回来的入口（按 X），
     * 而且那里会一直显示"还没配 AI"。没给人留后路的"跳过"是假跳过 ——
     * 用户一点就再也找不回来，最后只能去设置里翻。 */
    if (down & KEY_X) {
        app_status_set(S(S0096));
        app_note("apikey: skipped at page %d", g_page);
        finish_to_home();
        return;
    }

    if (down & KEY_B) {
        if (g_page > 0)
            --g_page;
        else
            finish_to_home();
        return;
    }

    /* 触摸：唯一那个按钮 = 同 A（翻页 / 去配 / 返回） */
    {
        int tx, ty;
        if (app_touch_down(&tx, &ty) && w_hit(&APIKEY_BTN, tx, ty)) {
            do_primary();
            return;
        }
    }

    if (down & KEY_A) {
        do_primary();
    }
}

static void apikey_enter(void)
{
    g_page  = 0;
    g_saved = false;

    /* 第一次进来就顺手把模板建好 —— 用户取卡时不用猜文件名。
     * 已存在的话这个函数什么都不做，不会覆盖用户已有的 Key。 */
    g_template_created = credfile_write_template();

    credfile_scan(&g_cred);
    do_rescan();
}

static void apikey_leave(void)
{
    /* 一点都不留：Key 只存在于扫描结果的内存里，离屏即丢。
     * 既然它暂时还不能加密落盘，那就根本不要落盘。 */
    memset(&g_cred, 0, sizeof g_cred);
}

const Screen g_screen_apikey = {
    .id            = SCREEN_APIKEY,
    .name          = "SCREEN_APIKEY",
    .enter         = apikey_enter,
    .leave         = apikey_leave,
    .render_top    = apikey_render_top,
    .render_bottom = apikey_render_bottom,
    .handle        = apikey_handle,
};
