/*
 * splash.c -- 见 splash.h
 */

#include "splash.h"

#include "render.h"
#include "text.h"
#include "widgets.h"

#include <stdio.h>
#include <string.h>
#include "uistr.h"

/* 启动期字体已就绪（font_init 在 splash 之前），所以这里可以正常画字。
 * 配色刻意不走主题：启动阶段主题还没从配置里读出来，
 * 用固定的浅色更稳（也避免"先画暗色再切亮色"的闪变）。 */
#define SP_BG     R_RGB(0xF7, 0xFB, 0xFA)
#define SP_ACC    R_RGB(0x00, 0x96, 0x88)
#define SP_ACC_DK R_RGB(0x00, 0x69, 0x5C)
#define SP_FG     R_RGB(0x1A, 0x1A, 0x1A)
#define SP_MUTED  R_RGB(0x75, 0x75, 0x75)
#define SP_TRACK  R_RGB(0xE0, 0xE4, 0xE3)

static void draw_top(int step, const char *label)
{
    r_target(R_SCREEN_TOP);
    r_begin(SP_BG);

    /* 标题：居中的产品名。用固定字符串，不依赖 uistr（文案系统可能还没就绪）。 */
    const char *title = "AI Console";
    int tw = text_width(title);
    r_text(&(Rect){ (R_TOP_W - (float)tw) / 2.0f, 78, (float)tw, 20, 0 },
           title, SP_ACC_DK);

    /* 副标题 */
    const char *sub = S(S0003);
    int sw = text_width(sub);
    r_text(&(Rect){ (R_TOP_W - (float)sw) / 2.0f, 106, (float)sw, 16, 0 },
           sub, SP_MUTED);

    /* 进度条：居中、宽 240、高 12。底槽 + 已完成段。
     * 不用 w_progress（那是经典 Windows 分段风格，留给文件传输），
     * 这里要的是简洁的连续条。 */
    const float bar_w = 240.0f, bar_h = 12.0f;
    float bx = (R_TOP_W - bar_w) / 2.0f;
    float by = 146.0f;

    r_rect(&(Rect){ bx, by, bar_w, bar_h, 0 }, SP_TRACK);

    if (step > SPLASH_STEPS) step = SPLASH_STEPS;
    if (step < 0) step = 0;
    float fill_w = bar_w * (float)step / (float)SPLASH_STEPS;
    if (fill_w > 0.0f)
        r_rect(&(Rect){ bx, by, fill_w, bar_h, 0 }, SP_ACC);

    /* 描边让进度条边界清楚 */
    r_frame_rect(&(Rect){ bx, by, bar_w, bar_h, 0 }, SP_ACC_DK, 1);

    /* 百分比数字 */
    char pct[16];
    snprintf(pct, sizeof pct, "%d%%", step * 100 / SPLASH_STEPS);
    int pw = text_width(pct);
    r_text(&(Rect){ (R_TOP_W - (float)pw) / 2.0f, by + bar_h + 10.0f,
                    (float)pw, 16, 0 }, pct, SP_MUTED);

    /* 当前动作说明 */
    if (label && label[0]) {
        int lw = text_width(label);
        r_text(&(Rect){ (R_TOP_W - (float)lw) / 2.0f, 196, (float)lw, 16, 0 },
               label, SP_FG);
    }

    /* ⚠️ 这里**不**调 r_end() —— 交换是全局的（翻转两块屏的缓冲索引），
     *    每次 r_end 都会把"本轮画过"的记录清掉。必须等两块屏都画完
     *    再由调用方调**一次** r_end（与 app_render 的纪律一致）。
     *    早先在 draw_top/draw_bottom 里各调一次，直接制造了
     *    "换了缓冲却没画" → 屏闪。这是被 swap_undrawn 守卫抓出来的。 */
}

static void draw_bottom(void)
{
    r_target(R_SCREEN_BOTTOM);
    r_begin(SP_BG);

    /* 底屏留一条静态提示，避免"一块空屏" */
    const char *hint = S(S0004);
    int hw = text_width(hint);
    r_text(&(Rect){ (R_BOT_W - (float)hw) / 2.0f, 112, (float)hw, 16, 0 },
           hint, SP_MUTED);

    /* 同上：不调 r_end */
}

void splash_frame(int step, const char *label)
{
    draw_top(step, label);
    draw_bottom();
    r_end();              /* 两块屏都画完，只交换一次 */
}

void splash_done(void)
{
    draw_top(SPLASH_STEPS, S(S0002));
    draw_bottom();
    r_end();
}
