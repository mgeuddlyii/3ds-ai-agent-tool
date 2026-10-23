/*
 * sc_viewer.c -- 内置轻量查看器（HTML 子集）
 *
 * 它叫什么很关键：叫"浏览器"是抬举它。没有 JS、没有 CSS、没有 DOM，
 * 所以任何 SPA 在这里都是一张白纸 —— 豆包那种连页面框架都靠前端画的
 * 站点，加载出来就是空的。这一点在 §4.9 里写死了，界面上也照实说。
 *
 * 它真正要干的事只有一件：**把 OTA 的更新说明显示出来**。
 * 次要用途是给内置文档用。
 *
 * 双屏分工：上屏是文档内容（只读事实区），下屏是滚动与返回（操作区）。
 */

#include "screen.h"

#include "app.h"
#include "html.h"
#include "widgets.h"

#include <stdio.h>
#include <string.h>
#include "uistr.h"

static HtmlDoc g_doc;
static int     g_top;        /* 当前显示的第一行下标 */
static bool    g_loaded;
static char    g_path[160];

static void viewer_enter(void)
{
    g_top = 0;
    snprintf(g_path, sizeof g_path, "%s", app_viewer_path());

    g_loaded = html_parse_file(g_path, &g_doc);

    if (g_loaded && g_doc.count > 0)
        app_status_set(S(S0638), g_doc.count);
    else
        app_status_set(S(S0639));
}

/* --------------------------------------------------------------------------
 * 上屏：文档内容
 * ----------------------------------------------------------------------- */

static float draw_one(const HtmlLine *l, float x, float y, float w)
{
    float ix = x;
    char  buf[HTML_TEXT_MAX + 8];

    /* 层级靠缩进表达 —— 点阵字体只有一种字号，没法靠字号区分 */
    switch (l->kind) {
        case HL_H1:
            return w_wrap(x, y, w, l->text, w_c(C_SLOT_ACC_DK), TEXT_LINE_H);
        case HL_H2:
            return w_wrap(x + 4, y, w - 4, l->text, w_c(C_SLOT_ACC_DK), TEXT_LINE_H);
        case HL_H3:
            return w_wrap(x + 8, y, w - 8, l->text, w_c(C_SLOT_ACC_DK), TEXT_LINE_H);
        case HL_ITEM:
            ix = x + 8.0f + (float)l->indent * 12.0f;
            snprintf(buf, sizeof buf, "· %s", l->text);
            return w_wrap(ix, y, w - (ix - x), buf, w_c(C_SLOT_FG), TEXT_LINE_H);
        case HL_QUOTE:
            ix = x + 10.0f;
            return w_wrap(ix, y, w - 10.0f, l->text, w_c(C_SLOT_MUTED), TEXT_LINE_H);
        default:
            return w_wrap(x, y, w, l->text, w_c(C_SLOT_FG), TEXT_LINE_H);
    }
}

static void render_top(void)
{
    const float x = (float)W_TOP_MARGIN;
    const float w = (float)W_TOP_USABLE;
    float y = (float)W_TOP_CONTENT_Y;

    w_top_header(S(S0640), S(S0641));

    if (!g_loaded || g_doc.count == 0) {
        y = w_wrap(x, y, w, S(S0642),
                   w_c(C_SLOT_FG), TEXT_LINE_H);
        y += 10;
        w_panel(&(Rect){ x, y, w, 34, 0 }, w_c(C_SLOT_ACC_LT), w_c(C_SLOT_ACCENT));
        w_line(x + 10, y + 9, w - 20, g_path, w_c(C_SLOT_ACC_DK));
        w_top_footer(app_status(), S(S0643));
        return;
    }

    float bottom = (float)W_TOP_CONTENT_B - (float)TEXT_LINE_H;
    for (int i = g_top; i < g_doc.count && y <= bottom; ++i)
        y = draw_one(&g_doc.line[i], x, y, w);

    /* 还有没显示完的，给个可见的提示，别让人以为就这些 */
    if (g_top + 1 < g_doc.count)
        w_line(x, (float)W_TOP_CONTENT_B - 16, w, S(S0644), w_c(C_SLOT_FAINT));

    w_top_footer(app_status(), S(S0645));
}

/* --------------------------------------------------------------------------
 * 下屏：滚动与返回
 * ----------------------------------------------------------------------- */

static void render_bottom(void)
{
    const float x = (float)W_BOT_MARGIN;
    const float w = (float)W_BOT_USABLE;
    float y = (float)W_BOT_CONTENT_Y;

    /* 文件名只显示尾部 —— 路径前缀都一样长，留着没用 */
    const char *base = strrchr(g_path, '/');
    base = base ? base + 1 : g_path;

    w_bot_header(base);

    y = w_wrap(x, y, w,
               S(S0776),
               w_c(C_SLOT_FG), TEXT_LINE_H);

    y += 8;
    if (g_loaded && g_doc.count > 0) {
        char prog[64];
        snprintf(prog, sizeof prog, S(S0647), g_top + 1, g_doc.count);
        w_line(x, y, w, prog, w_c(C_SLOT_MUTED));
    } else {
        w_wrap(x, y, w, S(S0648), w_c(C_SLOT_WARN_DK),
               TEXT_LINE_H);
    }

    w_button(&(Rect){ x + (w - 180.0f) / 2.0f, 150, 180, 36, 0 },
             S(S0233), W_BTN_PRIMARY);

    w_center((float)R_BOT_W / 2.0f, 192, S(S0649), w_c(C_SLOT_FAINT));
    w_bot_footer(S(S0332));
}

/* 按钮矩形（render 与 handle 共用，避免"看到的位置点不到"）。 */
static const Rect VIEWER_BTN = { 70, 150, 180, 36, 0 };

static void viewer_handle(u32 down, u32 held)
{
    (void)held;

    /* 触摸：把"点了按钮"折算成一次 A 键按下 —— 后面那段既有分支原样复用。
     * 不另写一套动作：复制一份逻辑迟早会和按键走岔。 */
    {
        int tx, ty;
        if (app_touch_down(&tx, &ty) && w_hit(&VIEWER_BTN, tx, ty))
            down |= KEY_A;
    }

    if (down & (KEY_UP | KEY_L)) {
        if (g_top > 0) --g_top;
        return;
    }
    if (down & (KEY_DOWN | KEY_R)) {
        if (g_loaded && g_top + 1 < g_doc.count) ++g_top;
        return;
    }
    if (down & (KEY_A | KEY_B))
        app_pop();
}


const Screen g_screen_viewer = {
    .id            = SCREEN_VIEWER,
    .name          = "SCREEN_VIEWER",
    .enter         = viewer_enter,
    .leave         = NULL,
    .render_top    = render_top,
    .render_bottom = render_bottom,
    .handle        = viewer_handle,
};
