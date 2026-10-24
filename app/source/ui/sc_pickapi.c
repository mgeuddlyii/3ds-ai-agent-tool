/*
 * sc_pickapi.c -- 为刚建好的任务挑一套「已添加的 API」
 *
 * ── 为什么要有这一屏 ──
 * 2026-10-23 真机反馈原话：「点进去会跳转一个页面，让你选更直白」。
 *
 * 在此之前这件事是在**项目面板顶部画一条横幅**、靠 ←/→ 循环切换 ——
 * 横幅本身会被列表行挤掉（下屏只有 240px），而且循环切换意味着
 * **看不到全貌**：用户不知道一共几套、各自是什么、下一个是什么。
 *
 * 一个真正的列表页能一次说清三件事：有几套、分别是什么、我现在选的是哪套。
 *
 * ── 数据来源 ──
 * 只列 **keystore 里真的有 Key** 的条目（`e->key[0]` 非空）。
 * 没有 Key 的条目选了也发不出请求，列出来只会让人白点一次。
 *
 * ── 进入方式 ──
 * 对话屏 → 项目面板 → 新建任务（Y 或「+ 新建任务」）→ 起完名字自动进来。
 * 按 B 退出则保持 new_task 时绑的那套默认（不留半截状态）。
 */

#include "screen.h"

#include <3ds.h>

#include "app.h"
#include "dock_ui.h"
#include "uistr.h"
#include "keystore.h"
#include "widgets.h"

#include <stdio.h>
#include <string.h>

#define PICK_VIS   5            /* 一屏几行（320x240，行高 26） */

static int g_sel;

/* 有 Key 的条目数。与 sc_dock.c 的 api_ready_count() 同一条判据 ——
 * 两处不一致会出现"这里能选、那边说没配"的鬼故事。 */
static int ready_count(void)
{
    int n = 0;

    for (int i = 0; i < keystore_count(); ++i) {
        const KeyEntry *e = keystore_at(i);
        if (e && e->key[0])
            ++n;
    }
    return n;
}

static const KeyEntry *ready_at(int k)
{
    for (int i = 0; i < keystore_count(); ++i) {
        const KeyEntry *e = keystore_at(i);
        if (e && e->key[0]) {
            if (k == 0)
                return e;
            --k;
        }
    }
    return NULL;
}

/* ------------------------------------------------------------------ 绘制 */

static void pick_enter(void)
{
    g_sel = 0;
    app_status_set(S(S0950));       /* 选一套 API，A 确定 */
}

static void render_top(void)
{
    w_top_header(S(S0951), "");     /* 「选 API」 */

    if (ready_count() == 0) {
        w_wrap((float)W_TOP_MARGIN, 40.0f, (float)W_TOP_USABLE,
               S(S0952), w_c(C_SLOT_FG), TEXT_LINE_H);
        w_wrap((float)W_TOP_MARGIN, 66.0f, (float)W_TOP_USABLE,
               S(S0953), w_c(C_SLOT_MUTED), TEXT_LINE_H);
        return;
    }

    for (int i = 0; i < PICK_VIS; ++i) {
        const KeyEntry *e = ready_at(i);
        char line[80];

        if (!e)
            break;
        snprintf(line, sizeof line, "%s  ·  %s",
                 e->name[0] ? e->name : e->provider,
                 e->model[0] ? e->model : "?");

        w_button(&(Rect){ (float)W_TOP_MARGIN,
                          40.0f + (float)i * 26.0f,
                          (float)W_TOP_USABLE, 24.0f, 0 },
                 line,
                 (i == g_sel ? W_BTN_SELECTED : W_BTN_NORMAL));
    }
}

static void render_bottom(void)
{
    w_bot_header(S(S0951));
    w_wrap((float)W_BOT_MARGIN, (float)W_BOT_CONTENT_Y, (float)W_BOT_USABLE,
           S(S0950), w_c(C_SLOT_FG), TEXT_LINE_H);
    w_bot_footer(S(S0424));         /* [B] 返回 */
}

/* ------------------------------------------------------------------ 按键 */

static void pick_handle(u32 down, u32 held)
{
    int n = ready_count();

    (void)held;

    if (down & KEY_B) {
        /* 退出但**不留 g_pick**：new_task 时已经绑了默认那套，
         * 所以退出等于"就用默认的"，不会留下半截状态。 */
        dockui_pick_cancel();
        app_pop();
        return;
    }

    if (n == 0) {
        if (down & KEY_A)
            app_push(SCREEN_PROVIDERS);   /* 一个都没有 → 带他去配 */
        return;
    }

    if (down & KEY_UP)   { g_sel = (g_sel > 0) ? g_sel - 1 : n - 1; return; }
    if (down & KEY_DOWN) { g_sel = (g_sel + 1 < n) ? g_sel + 1 : 0; return; }

    if (down & KEY_A) {
        if (dockui_pick_apply(g_sel)) {
            app_status_set(S(S0954));
            app_pop();
        } else {
            app_status_set(S(S0955));
        }
        return;
    }
}

const Screen g_screen_pickapi = {
    .id            = SCREEN_PICKAPI,
    .name          = "SCREEN_PICKAPI",
    .enter         = pick_enter,
    .leave         = NULL,
    .render_top    = render_top,
    .render_bottom = render_bottom,
    .handle        = pick_handle,
};
