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

#define SET_ROWS 6

static const char *row_label(int i)
{
    switch (i) {
    case 0: return "显示在哪块屏";
    case 1: return "翻译屏位置";
    case 2: return "宽度（字符）";
    case 3: return "高度（字符）";
    case 4: return "背板深浅";
    default: return "触发键";
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
    default: snprintf(out, n, "%s", g_cfg.use_zl ? "ZL" : "SELECT"); break;
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
    default: g_cfg.use_zl = !g_cfg.use_zl; break;
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
                cfg_save(&g_cfg);
                g_mode = MODE_GAME;
            }
            if (down & (KEY_B | KEY_SELECT))
                g_mode = MODE_GAME;

            if (g_mode == MODE_SETTINGS)
                draw_settings();
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
