/*
 * sc_locales.c -- 已设区域：列出设过 locale 的游戏，可逐个撤销
 *
 * ── 为什么需要一个独立的一屏 ──
 * locale.txt 是散在 sdmc:/luma/titles/<16位ID>/ 下的文件。用户在 3DS 上
 * **没法浏览那个目录**（没有文件管理器），于是会出现这种情况：
 * 玩某个汉化游戏时设过一次区域，三个月后换回原版，忘了自己设过 ——
 * 于是"这个游戏怎么老是日文"就变成了一个查不出来的怪问题。
 *
 * 这一屏就是给这种情况兜底的：**把设过的东西全列出来**，一眼看完，
 * 不想要的就地撤掉。它同时也是"这个功能是可逆的"这件事的证据 ——
 * 用户看得见自己在哪动过手，才敢放心用。
 *
 * ── 交互 ──
 * 上下选、A 询问是否撤销、B 返回。撤销是一次**破坏性**操作，但破坏性
 * 极小（删一个文本文件），所以这里用一个"再确认一次"的内联提示就够了，
 * 不走 SCREEN_CONFIRM —— 那一屏是给"AI 想做什么"用的，把用户自己
 * 的手动操作也塞进去会让两道流程混在一起。
 */

#include "screen.h"

#include "app.h"
#include "locale.h"
#include "widgets.h"

#include <stdio.h>
#include <string.h>
#include "uistr.h"

static char       g_tids[LOC_LIST_MAX][LOC_TID_MAX];
static char       g_vals[LOC_LIST_MAX][24];
static int        g_n;
static int        g_sel;
static bool       g_ask;        /* 正在问"确定撤销这个吗" */
/* 上一次操作的结果，显示在页脚上方。
 * 尺寸必须 >= loc_clear() 的 msg 缓冲（192）—— 小了会被 -Wformat-truncation
 * 抓出来，而那个警告在这里是真话：截断之后就看不到失败原因的后半句了。 */
static char       g_msg[192];

#define ROW_H       20.0f
#define ROW_MAX     10          /* 一屏放得下的行数，超了靠滚动 */

static void reload(void)
{
    int n = 0;

    loc_list(g_tids, g_vals, LOC_LIST_MAX, &n);
    g_n = n;
    if (g_sel >= g_n)
        g_sel = g_n > 0 ? g_n - 1 : 0;
    if (g_sel < 0)
        g_sel = 0;
}

static void locales_enter(void)
{
    g_sel     = 0;
    g_ask     = false;
    g_msg[0]  = '\0';
    reload();

    app_status_set(S(S0486), g_n);
}

/* --------------------------------------------------------------------------
 * 上屏：列表
 * ----------------------------------------------------------------------- */

static void render_top(void)
{
    const float x = (float)W_TOP_MARGIN;
    const float w = (float)W_TOP_USABLE;
    float y = (float)W_TOP_CONTENT_Y;
    char  line[96];

    w_top_header(S(S0371), g_n > 0 ? S(S0487) : S(S0488));

    if (g_n == 0) {
        w_wrap(x, y, w,
               S(S0799),
               w_c(C_SLOT_FG), TEXT_LINE_H);
        w_top_footer(app_status(), "");
        return;
    }

    /* 滚动窗口：让选中的那行始终可见。列表通常很短（几十个游戏撑死），
     * 但"通常很短"不是不写滚动的理由 —— 不写的话，第 11 个往后就永远
     * 选不到，而且界面看上去一切正常。 */
    {
        int first = 0;

        if (g_sel >= ROW_MAX)
            first = g_sel - ROW_MAX + 1;

        for (int i = first; i < g_n && i < first + ROW_MAX; ++i) {
            bool  cur = (i == g_sel);
            /* 选中行用"实心底 + 反色字"来表达，而不是只加个箭头 ——
             * 箭头在小屏上太不明显，扫一眼看不出选了哪个。 */
            if (cur)
                w_panel(&(Rect){ x - 2, y - 2, w + 4, ROW_H, 0 },
                        w_c(C_SLOT_ACC_LT), w_c(C_SLOT_ACCENT));

            snprintf(line, sizeof line, "%s", g_tids[i]);
            w_line(x + 2, y + 1, w - 90, line,
                   cur ? w_c(C_SLOT_ACC_DK) : w_c(C_SLOT_FG));

            snprintf(line, sizeof line, "%s", g_vals[i]);
            /* 区域语言用强调色：它是这一行**唯一**需要用户核对的字段
             * （游戏 ID 是一串十六进制，没人会去读它）。 */
            w_line(x + w - 78, y + 1, 76, line,
                   cur ? w_c(C_SLOT_ACC_DK) : w_c(C_SLOT_ACCENT));

            y += ROW_H;
        }

        if (g_n > ROW_MAX) {
            y += 4;
            snprintf(line, sizeof line, S(S0490),
                     g_n, first + 1,
                     first + ROW_MAX < g_n ? first + ROW_MAX : g_n);
            w_line(x, y, w, line, w_c(C_SLOT_FAINT));
        }
    }

    w_top_footer(app_status(),
                 g_ask ? S(S0491) : S(S0492));
}

/* --------------------------------------------------------------------------
 * 下屏：说明 + 撤销入口
 * ----------------------------------------------------------------------- */

/* 问询态那两个按钮的矩形（render 与 handle 共用）。 */
static const Rect LOC_BTN_NO  = {  10, 150, 144, 36, 0 };
static const Rect LOC_BTN_YES = { 166, 150, 144, 36, 0 };

static void render_bottom(void)
{
    const float x = (float)W_BOT_MARGIN;
    const float w = (float)W_BOT_USABLE;
    float y = (float)W_BOT_CONTENT_Y;

    w_bot_header(g_ask ? S(S0493) : S(S0494));

    if (g_n == 0) {
        w_wrap(x, y, w,
               S(S0773),
               w_c(C_SLOT_FG), TEXT_LINE_H);
        w_bot_footer(S(S0332));
        return;
    }

    if (g_ask) {
        char l[64];

        snprintf(l, sizeof l, S(S0496), g_tids[g_sel], g_vals[g_sel]);
        w_wrap(x, y, w, l, w_c(C_SLOT_FG), TEXT_LINE_H);
        y += TEXT_LINE_H + 4;
        w_wrap(x, y, w,
               S(S0497),
               w_c(C_SLOT_MUTED), TEXT_LINE_H);

        w_button(&LOC_BTN_NO,  S(S0288), W_BTN_NORMAL);
        w_button(&LOC_BTN_YES, S(S0498), W_BTN_SELECTED);
        w_center((float)R_BOT_W / 2.0f, 192,
                 S(S0499), w_c(C_SLOT_FAINT));
        w_bot_footer(S(S0499));
        return;
    }

    if (g_msg[0])
        y = w_wrap(x, y, w, g_msg, w_c(C_SLOT_OK), TEXT_LINE_H);
    else
        y = w_wrap(x, y, w,
                   S(S0774),
                   w_c(C_SLOT_MUTED), TEXT_LINE_H);

    y += 8;
    w_wrap(x, y, w,
           S(S0501),
           w_c(C_SLOT_ACCENT), TEXT_LINE_H);

    w_bot_footer(S(S0502));
}

/* --------------------------------------------------------------------------
 * 行为
 * ----------------------------------------------------------------------- */

static void do_clear(void)
{
    char msg[192];

    if (g_sel < 0 || g_sel >= g_n)
        return;

    loc_clear(g_tids[g_sel], msg, sizeof msg);
    snprintf(g_msg, sizeof g_msg, "%s", msg);

    /* 撤销也留痕。理由与 actions.c 那边一致：这一屏虽然是用户手动操作，
     * 但它改的是同一批文件。事后翻 action.log 时应看到"某时某人撤了哪个"，
     * 否则"我明明没设过啊"这类问题查不下去。 */
    app_note("locale: 手动撤销 %s（%s）", g_tids[g_sel], msg);

    reload();
    g_ask = false;
    app_status_set(S(S0486), g_n);
}

static void locales_handle(u32 down, u32 held)
{
    (void)held;

    if (down & KEY_B) {
        if (g_ask) {
            g_ask = false;      /* 先退问询，再退屏 —— 一次 B 只退一层 */
            return;
        }
        app_pop();
        return;
    }

    if (g_n == 0)
        return;

    if (g_ask) {
        /* 触摸：左=取消（退问询，与 B 同）、右=确认撤销（与 A 同）。
         * 一律复用既有动作，不另写分支。 */
        {
            int tx, ty;
            if (app_touch_down(&tx, &ty)) {
                if (w_hit(&LOC_BTN_NO, tx, ty)) {
                    g_ask = false;
                    return;
                }
                if (w_hit(&LOC_BTN_YES, tx, ty))
                    down |= KEY_A;
            }
        }
        if (down & KEY_A)
            do_clear();
        return;
    }

    if (down & (KEY_UP | KEY_DOWN)) {
        g_msg[0] = '\0';
        if (down & KEY_UP)
            g_sel = (g_sel + g_n - 1) % g_n;
        else
            g_sel = (g_sel + 1) % g_n;
        return;
    }

    if (down & KEY_A) {
        g_ask = true;
        g_msg[0] = '\0';
    }
}

const Screen g_screen_locales = {
    .id            = SCREEN_LOCALES,
    .name          = "SCREEN_LOCALES",
    .enter         = locales_enter,
    .leave         = NULL,
    .render_top    = render_top,
    .render_bottom = render_bottom,
    .handle        = locales_handle,
};
