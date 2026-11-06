/*
 * main.c —— AIConsole 翻译插件
 *
 * 界面：面板顶上三个按钮「翻译 / 设置 / 说明」，都能点（触摸）。
 *       短按触发键 = 翻译，长按 ≈0.75 秒 = 设置。
 *
 * ── 为什么全部自绘 ──
 *   libctru 的 PrintConsole 里 windowX/Y/Width/Height 标着 **not implemented**，
 *   只能整屏；而"面板能随便摆、按钮能点"正是这个插件要的东西。
 *
 * ── 三条硬约束（骨架版就记着，别改）──
 *   1. 自己定义 __service_ptr / __system_runflags / __apt_appid（crt0 被拿掉）
 *   2. 别调 hidInit()，New 3DS 上会拉入 irrst 把游戏冻住
 *   3. BSS 由 boot.s 自己清
 *
 * ⚠️ 未在真机验证：Azahar 不加载 .3gx，模拟器里什么都看不到。
 */

#include <3ds.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cfg.h"
#include "help.h"
#include "hud.h"
#include "net.h"

void *__service_ptr = NULL;
u32   __system_runflags = 0;
u32   __apt_appid = 0;

#define MODE_GAME     0
#define MODE_SETTINGS 1
#define MODE_HELP     2
#define MODE_LAYOUT   3

#define TRIGGER_LONG_FRAMES 45

static PluginCfg g_cfg;
static int       g_mode = MODE_GAME;
static int       g_sel;
static char      g_text[512] = "点「翻译」，或短按触发键";
static u32       g_hold;
static bool      g_hold_fired;

/* 一次触摸只消费一次：按住不放不该反复触发 */
static bool      g_touching, g_tap_used;

static void do_translate(void);

/* ---------------- 设置页 ---------------- */

#define SET_ROWS 7

static const char *row_label(int i)
{
    switch (i) {
    case 0: return "显示在哪块屏";
    case 1: return "翻译屏位置";
    case 2: return "宽度（字符）";
    case 3: return "高度（字符）";
    case 4: return "背板深浅";
    case 5: return "触发键";
    default: return "摆放位置";
    }
}

static void row_value(int i, char *out, size_t n)
{
    switch (i) {
    case 0: snprintf(out, n, "%s", g_cfg.screen == SCR_TOP ? "上屏" : "下屏"); break;
    case 1: {
        static const char *k[] = { "左上", "上中", "右上", "左中", "中央",
                                   "右中", "左下", "下中", "右下" };
        snprintf(out, n, "%s", k[(int)g_cfg.anchor]);
        break;
    }
    case 2: snprintf(out, n, "%d", g_cfg.cols); break;
    case 3: snprintf(out, n, "%d", g_cfg.rows); break;
    case 4: {
        static const char *k[] = { "最浅", "浅", "中", "最深" };
        snprintf(out, n, "%s", k[g_cfg.opacity & 3]);
        break;
    }
    case 5: snprintf(out, n, "%s", g_cfg.use_zl ? "ZL" : "SELECT"); break;
    default:
        snprintf(out, n, "%s", g_cfg.free_pos ? "已拖动" : "按 A 拖动");
        break;
    }
}

static void row_step(int i, int dir)
{
    switch (i) {
    case 0: g_cfg.screen = g_cfg.screen == SCR_TOP ? SCR_BOTTOM : SCR_TOP; break;
    case 1: {
        int a = (int)g_cfg.anchor + dir;

        if (a < 0) a = 8;
        if (a > 8) a = 0;
        g_cfg.anchor = (CfgAnchor)a;
        break;
    }
    case 2:
        g_cfg.cols += dir * 4;
        if (g_cfg.cols < 10) g_cfg.cols = 10;
        if (g_cfg.cols > 50) g_cfg.cols = 50;
        break;
    case 3:
        g_cfg.rows += dir;
        if (g_cfg.rows < 2) g_cfg.rows = 2;
        if (g_cfg.rows > 16) g_cfg.rows = 16;
        break;
    case 4: g_cfg.opacity = (g_cfg.opacity + dir + 4) & 3; break;
    case 5: g_cfg.use_zl = !g_cfg.use_zl; break;
    default: break;                 /* 摆放行不响应左右键 */

    }
}

/* 行矩形：绘制与触摸共用一份，两处各算一遍必定错开 */
static void row_rect(int i, int w, int *rx, int *ry, int *rw, int *rh)
{
    *rx = 6;
    *ry = 42 + i * 26 - 5;
    *rw = w - 12;
    *rh = 24;
}

static void draw_settings(void)
{
    int w = 0, h = 0;

    if (!hud_begin(GFX_BOTTOM, &w, &h))
        return;

    hud_rect(0, 0, w, h, 10, 12, 16);
    hud_text(10, 8, w - 10, "翻译插件 · 设置", 120, 220, 210);
    hud_rect(10, 30, w - 20, 1, 40, 70, 80);

    for (int i = 0; i < SET_ROWS; ++i) {
        int  rx, ry, rw, rh;
        char val[32];

        row_value(i, val, sizeof val);
        row_rect(i, w, &rx, &ry, &rw, &rh);

        if (i == g_sel)
            hud_rect(rx, ry, rw, rh, 26, 62, 72);

        hud_text(14, ry + 5, w - 130, row_label(i),
                 i == g_sel ? 240 : 190,
                 i == g_sel ? 245 : 200,
                 i == g_sel ? 250 : 210);
        {
            int vw = hud_text_w(val);

            hud_text(w - 14 - vw, ry + 5, w - 6, val, 150, 220, 190);
        }
    }

    hud_rect(10, h - 44, w - 20, 1, 40, 70, 80);
    hud_text(10, h - 40, w - 10, "点一行选中，再点一次改值", 120, 150, 160);
    hud_text(10, h - 20, w - 10, "A 保存    B 返回", 120, 150, 160);
    hud_end();
}

/* ---------------- 触摸 ---------------- */

static bool touch_pressed(touchPosition *tp)
{
    if (!(hidKeysDown() & KEY_TOUCH)) {
        g_touching = false;
        g_tap_used = false;
        return false;
    }
    if (g_touching && g_tap_used)
        return false;
    g_touching = true;
    hidTouchRead(tp);
    g_tap_used = true;
    return true;
}

static u32 trigger_mask(void)
{
    return g_cfg.use_zl ? KEY_ZL : KEY_SELECT;
}


/* ---------------- 拖动摆放页 ----------------
 *
 * 触摸只在下屏有效，而目标屏可能是上屏 —— 所以这里画的是**缩略画布**：
 * 下屏那块区域按比例代表目标屏，拖的是画布里的窗口。
 * 直接"在上屏拖"是做不到的（上屏没有触摸层），这条限制绕不过去。
 */

#define LAY_TITLE_H 30
#define LAY_BTN_H   40

static int   g_drag;                 /* 正在拖 */
static int   g_drag_dx, g_drag_dy;   /* 触摸点相对窗口左上角的偏移（屏像素） */
static int   g_lay_x, g_lay_y;       /* 布局页里的自由坐标（目标屏像素） */

static void lay_canvas(int *cx, int *cy, int *cw, int *ch, float *scale)
{
    int scr_w = g_cfg.screen == SCR_TOP ? 400 : 320;
    int scr_h = 240;
    int avail_w = 320;
    int avail_h = 240 - LAY_TITLE_H - LAY_BTN_H;

    *scale = (float)avail_w / scr_w;
    if ((float)avail_h / scr_h < *scale)
        *scale = (float)avail_h / scr_h;

    *cw = (int)(scr_w * *scale);
    *ch = (int)(scr_h * *scale);
    *cx = (320 - *cw) / 2;
    *cy = LAY_TITLE_H + (avail_h - *ch) / 2;
}

static void draw_layout(void)
{
    int   cx, cy, cw, ch;
    float sc;
    int   px, py, pw, ph;

    if (!hud_begin(GFX_BOTTOM, NULL, NULL))
        return;

    hud_rect(0, 0, 320, 240, 10, 12, 16);
    hud_text(10, 6, 310, g_cfg.screen == SCR_TOP ? "拖动摆放 · 上屏" : "拖动摆放 · 下屏",
             120, 220, 210);

    lay_canvas(&cx, &cy, &cw, &ch, &sc);

    /* 画布 = 目标屏的缩影 */
    hud_rect(cx, cy, cw, ch, 22, 26, 34);
    hud_frame(cx, cy, cw, ch, 60, 90, 100);

    {
        int pw0 = g_cfg.cols * 8, ph0 = g_cfg.rows * 16;

        if (pw0 > (g_cfg.screen == SCR_TOP ? 400 : 320)) pw0 = (g_cfg.screen == SCR_TOP ? 400 : 320);
        if (ph0 > 240) ph0 = 240;

        pw = (int)(pw0 * sc);
        ph = (int)(ph0 * sc);
        px = cx + (int)(g_lay_x * sc);
        py = cy + (int)(g_lay_y * sc);

        hud_rect(px, py, pw, ph, 40, 120, 130);
        hud_frame(px, py, pw, ph, 120, 220, 210);
        if (pw > 40 && ph > 20)
            hud_text(px + 4, py + 4, px + pw - 4, "拖我", 235, 245, 250);
    }

    /* 底部三个按钮：换屏 / 确认 / 取消 */
    {
        static const char *kLbl[3] = { "换屏", "确认", "取消" };
        int bw = (320 - 8 * 2 - 6 * 2) / 3;

        for (int i = 0; i < 3; ++i) {
            int bx = 8 + i * (bw + 6);
            int by = 240 - LAY_BTN_H + 8;

            hud_rect(bx, by, bw, LAY_BTN_H - 16, 34, 42, 52);
            hud_frame(bx, by, bw, LAY_BTN_H - 16, 70, 120, 130);
            {
                int tw = hud_text_w(kLbl[i]);

                hud_text(bx + (bw - tw) / 2, by + 4, bx + bw, kLbl[i], 220, 235, 240);
            }
        }
    }

    hud_end();
}

/* 布局页的触摸：返回 0=没动作 1=确认 2=取消 3=换屏 */
static int layout_touch(int pressed, int tx, int ty)
{
    int   cx, cy, cw, ch;
    float sc;
    int   pw = (int)(g_cfg.cols * 8 * 0.0f);   /* 占位，下面重算 */

    lay_canvas(&cx, &cy, &cw, &ch, &sc);

    /* 底部按钮 */
    {
        int bw = (320 - 8 * 2 - 6 * 2) / 3;
        int by = 240 - LAY_BTN_H + 8;

        if (ty >= by && ty < by + LAY_BTN_H - 16) {
            for (int i = 0; i < 3; ++i) {
                int bx = 8 + i * (bw + 6);

                if (tx >= bx && tx < bx + bw)
                    return i == 0 ? 3 : (i == 1 ? 1 : 2);
            }
        }
    }

    {
        int pw0 = g_cfg.cols * 8, ph0 = g_cfg.rows * 16;
        int maxw = g_cfg.screen == SCR_TOP ? 400 : 320;

        if (pw0 > maxw) pw0 = maxw;
        if (ph0 > 240) ph0 = 240;
        pw = (int)(pw0 * sc);

        {
            int ph  = (int)(ph0 * sc);
            int px  = cx + (int)(g_lay_x * sc);
            int py  = cy + (int)(g_lay_y * sc);

            if (pressed) {
                if (tx >= px && tx < px + pw && ty >= py && ty < py + ph) {
                    g_drag    = 1;
                    g_drag_dx = (int)((tx - px) / sc);
                    g_drag_dy = (int)((ty - py) / sc);
                }
            } else if (g_drag) {
                /* 拖动中：把画布坐标换算回目标屏像素 */
                int nx = (int)((tx - cx) / sc) - g_drag_dx;
                int ny = (int)((ty - cy) / sc) - g_drag_dy;
                int maxx = maxw - pw0;
                int maxy = 240 - ph0;

                if (nx < 0) nx = 0;
                if (ny < 0) ny = 0;
                if (nx > maxx) nx = maxx;
                if (ny > maxy) ny = maxy;
                g_lay_x = nx;
                g_lay_y = ny;
            }
        }
    }
    return 0;
}

/* ---------------- 主循环 ---------------- */

int main(void)
{
    gfxInitDefault();
    cfg_load(&g_cfg);

    for (;;) {
        u32 down = 0, held = 0;
        touchPosition tp = { 0, 0 };

        hidScanInput();
        down = hidKeysDown();
        held = hidKeysHeld();

        if (g_mode == MODE_HELP) {
            int w = 0, h = 0;

            if (down & KEY_UP)    help_scroll(-1);
            if (down & KEY_DOWN)  help_scroll(+1);
            if (down & KEY_LEFT)  help_page(-1);
            if (down & KEY_RIGHT) help_page(+1);
            if (down & (KEY_B | KEY_SELECT))
                g_mode = MODE_GAME;

            if (touch_pressed(&tp)) {
                /* 点最下面 24px = 返回；其余按左右半屏翻页 —— 翻页比滚动更常用 */
                if (tp.py > 216)
                    g_mode = MODE_GAME;
                else
                    help_page(tp.px < 160 ? -1 : +1);
            }

            if (g_mode == MODE_HELP) {
                if (hud_begin(GFX_BOTTOM, &w, &h))
                    help_draw(w, h);
                hud_end();
            }
        } else if (g_mode == MODE_SETTINGS) {
            int w = 0, h = 0;

            if (down & KEY_UP)   { if (--g_sel < 0) g_sel = SET_ROWS - 1; }
            if (down & KEY_DOWN) { if (++g_sel >= SET_ROWS) g_sel = 0; }
            if (down & KEY_LEFT)   row_step(g_sel, -1);
            if (down & KEY_RIGHT)  row_step(g_sel, +1);

            if (touch_pressed(&tp) && hud_begin(GFX_BOTTOM, &w, &h)) {
                for (int i = 0; i < SET_ROWS; ++i) {
                    int rx, ry, rw, rh;

                    row_rect(i, w, &rx, &ry, &rw, &rh);
                    if (tp.px >= rx && tp.px < rx + rw &&
                        tp.py >= ry && tp.py < ry + rh) {
                        /* 触摸屏没有左右键，所以"再点一次"就是它的加减 */
                        if (g_sel == i)
                            row_step(i, +1);
                        g_sel = i;
                        break;
                    }
                }
            }

            if (down & KEY_A) {
                if (g_sel == SET_ROWS - 1) {
                    /* 进拖动页：把当前位置换算成自由坐标作为起点 */
                    int sx, sy, sw2, sh2;

                    cfg_panel_rect(&g_cfg, g_cfg.screen == SCR_TOP ? 400 : 320, 240,
                                   &sx, &sy, &sw2, &sh2);
                    g_lay_x = sx;
                    g_lay_y = sy;
                    g_drag  = 0;
                    g_mode  = MODE_LAYOUT;
                } else {
                    cfg_save(&g_cfg);
                    g_mode = MODE_GAME;
                }
            }
            if (down & (KEY_B | KEY_SELECT))
                g_mode = MODE_GAME;

            if (g_mode == MODE_SETTINGS)
                draw_settings();
        } else if (g_mode == MODE_LAYOUT) {
            int pressed = (down & KEY_TOUCH) ? 1 : 0;
            int heldt   = (held & KEY_TOUCH) ? 1 : 0;

            if (down & KEY_LEFT)  { if ((g_lay_x -= 4) < 0) g_lay_x = 0; }
            if (down & KEY_RIGHT) { g_lay_x += 4; }
            if (down & KEY_UP)    { if ((g_lay_y -= 4) < 0) g_lay_y = 0; }
            if (down & KEY_DOWN)  { g_lay_y += 4; }

            if (pressed || heldt) {
                touchPosition t2;

                hidTouchRead(&t2);
                {
                    int act = layout_touch(pressed, t2.px, t2.py);

                    if (act == 1) {              /* 确认 */
                        g_cfg.free_pos = true;
                        g_cfg.pos_x    = g_lay_x;
                        g_cfg.pos_y    = g_lay_y;
                        cfg_save(&g_cfg);
                        g_drag = 0;
                        g_mode = MODE_GAME;
                    } else if (act == 2) {       /* 取消 */
                        g_drag = 0;
                        g_mode = MODE_SETTINGS;
                    } else if (act == 3) {       /* 换屏 */
                        g_cfg.screen = g_cfg.screen == SCR_TOP ? SCR_BOTTOM : SCR_TOP;
                        g_lay_x = g_lay_y = 8;
                        g_drag  = 0;
                    }
                }
            }
            if (down & KEY_A) {                  /* 键盘确认 */
                g_cfg.free_pos = true;
                g_cfg.pos_x    = g_lay_x;
                g_cfg.pos_y    = g_lay_y;
                cfg_save(&g_cfg);
                g_mode = MODE_GAME;
            }
            if (down & (KEY_B | KEY_SELECT))
                g_mode = MODE_SETTINGS;

            if (g_mode == MODE_LAYOUT)
                draw_layout();
        } else {
            /* ---- 游戏态：面板常驻，三个按钮 ---- */
            if (held & trigger_mask()) {
                if (++g_hold >= TRIGGER_LONG_FRAMES && !g_hold_fired) {
                    g_hold_fired = true;
                    g_sel = 0;
                    g_mode = MODE_SETTINGS;
                }
            } else {
                if (g_hold > 0 && g_hold < TRIGGER_LONG_FRAMES && !g_hold_fired)
                    do_translate();
                g_hold = 0;
                g_hold_fired = false;
            }

            if (touch_pressed(&tp)) {
                int b = hud_button_at(&g_cfg, tp.px, tp.py);

                if (b == BTN_TRANSLATE) {
                    do_translate();
                } else if (b == BTN_SETTINGS) {
                    g_sel = 0;
                    g_mode = MODE_SETTINGS;
                } else if (b == BTN_HELP) {
                    help_init(9);
                    g_mode = MODE_HELP;
                }
            }

            if (g_mode == MODE_GAME)
                hud_draw(&g_cfg, HUD_TEXT, g_text);
        }

        gfxFlushBuffers();
        gfxSwapBuffers();
        gspWaitForVBlank();
    }
    return 0;
}

/* ---------------- 翻译 ---------------- */

static void do_translate(void)
{
    gfxScreen_t scr = g_cfg.screen == SCR_TOP ? GFX_TOP : GFX_BOTTOM;
    u8 *px = NULL;
    int w = 0, h = 0;

    snprintf(g_text, sizeof g_text, "截图中…");
    hud_draw(&g_cfg, HUD_CAPTURING, NULL);

    if (!hud_grab(scr, &px, &w, &h)) {
        snprintf(g_text, sizeof g_text, "抓屏失败\n拿不到帧缓冲");
        hud_draw(&g_cfg, HUD_ERROR, g_text);
        return;
    }

    snprintf(g_text, sizeof g_text, "等 AI 回复…");
    hud_draw(&g_cfg, HUD_WAITING, NULL);

    if (net_translate(px, w, h, g_text, sizeof g_text))
        hud_draw(&g_cfg, HUD_TEXT, g_text);
    else
        hud_draw(&g_cfg, HUD_ERROR, g_text);

    free(px);
}
