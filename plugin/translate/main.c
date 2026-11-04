/*
 * main.c —— AIConsole 翻译插件
 *
 * 用户要的三件事：
 *   ① 插件有自己的设置，能点进去
 *   ② 设置里能调"翻译屏"（位置 / 大小 / 深浅）
 *   ③ 游戏中按一个键 → 抓当前画面 → 发 AI → 译文显示在翻译屏
 *
 * 现在做到哪：①②完成；③的**抓屏**已实现并能报出真实分辨率，
 * 上传与 AI 调用还没接（要 soc + TLS，插件里这是独立的一块风险）。
 *
 * ── 为什么全部自绘，不用 console ──
 *   libctru 的 PrintConsole 里 windowX/Y/Width/Height 四个字段标着
 *   **not implemented**，也就是说它只能整屏。而"翻译屏能在任意位置"
 *   正是用户明确要的。所以干脆连设置页也自绘：一套绘制代码，
 *   字体、位置、边界行为完全一致 —— 出问题只有一处要看。
 *
 * ── 三条来自社区的硬约束（骨架版就记着，别改）──
 *   1. 必须自己定义 __service_ptr / __system_runflags / __apt_appid
 *      （crt0 被拿掉之后 libctru 找不到它们）
 *   2. 别调 hidInit()，New 3DS 上会拉入 irrst 把游戏冻住
 *   3. BSS 由 boot.s 自己清（加载器不清）
 *
 * ⚠️ 全部未在真机验证：Azahar 不加载 .3gx，模拟器里什么都看不到。
 *    第一次上真机时，按"面板有没有出现 → 按键有没有反应 → 抓屏是不是游戏画面"
 *    这个顺序排错（见 README）。
 */

#include <3ds.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cfg.h"
#include "hud.h"

/* libctru 需要应用侧提供（见文件头第 1 条）。 */
void *__service_ptr = NULL;
u32   __system_runflags = 0;
u32   __apt_appid = 0;

#define MODE_GAME     0
#define MODE_SETTINGS 1

#define TRIGGER_LONG_FRAMES 45      /* 按住 ~0.75 秒算长按（60fps 折算） */

static PluginCfg g_cfg;
static int       g_mode = MODE_GAME;
static int       g_sel;
static HudState  g_state = HUD_HIDDEN;
static char      g_text[512];
static u32       g_hold;
static bool      g_hold_fired;

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
    case 0:
        snprintf(out, n, "%s", g_cfg.screen == SCR_TOP ? "上屏" : "下屏");
        break;
    case 1: {
        static const char *kName[] = {
            "左上", "上中", "右上", "左中", "中央", "右中", "左下", "下中", "右下"
        };
        snprintf(out, n, "%s", kName[(int)g_cfg.anchor]);
        break;
    }
    case 2: snprintf(out, n, "%d", g_cfg.cols); break;
    case 3: snprintf(out, n, "%d", g_cfg.rows); break;
    case 4: {
        static const char *kDeep[] = { "最浅", "浅", "中", "最深" };
        snprintf(out, n, "%s", kDeep[g_cfg.opacity & 3]);
        break;
    }
    default:
        snprintf(out, n, "%s", g_cfg.use_zl ? "ZL" : "SELECT");
        break;
    }
}

static void row_step(int i, int dir)
{
    switch (i) {
    case 0:
        g_cfg.screen = g_cfg.screen == SCR_TOP ? SCR_BOTTOM : SCR_TOP;
        break;
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
    case 4:
        g_cfg.opacity = (g_cfg.opacity + dir + 4) & 3;
        break;
    default:
        g_cfg.use_zl = !g_cfg.use_zl;
        break;
    }
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
        int  y = 42 + i * 26;
        char val[32];

        row_value(i, val, sizeof val);

        if (i == g_sel)
            hud_rect(6, y - 5, w - 12, 24, 26, 62, 72);

        hud_text(14, y, w - 130, row_label(i),
                 i == g_sel ? 240 : 190,
                 i == g_sel ? 245 : 200,
                 i == g_sel ? 250 : 210);

        {
            int vw = hud_text_w(val);
            hud_text(w - 14 - vw, y, w - 6, val, 150, 220, 190);
        }
    }

    /* 提示拆两行：一行放不下 320px（中文 16px/字，实测右侧被裁掉"B 退出"）。 */
    hud_rect(10, h - 44, w - 20, 1, 40, 70, 80);
    hud_text(10, h - 40, w - 10, "↑↓ 选择    ←→ 修改", 120, 150, 160);
    hud_text(10, h - 20, w - 10, "A 保存      B 退出", 120, 150, 160);

    hud_end();
}

/* ---------------- 翻译流程 ---------------- */

static u32 trigger_mask(void)
{
    return g_cfg.use_zl ? KEY_ZL : KEY_SELECT;
}

/* 抓当前屏。这一步是"能截图"的实证：把真实分辨率写进面板，
 * 真机上一眼就能判断抓到的到底是不是游戏画面（还是我们自己的黑帧缓冲）。 */
static void start_translate(void)
{
    gfxScreen_t scr = g_cfg.screen == SCR_TOP ? GFX_TOP : GFX_BOTTOM;
    u8 *px = NULL;
    int w = 0, h = 0;

    g_state = HUD_CAPTURING;
    hud_draw(&g_cfg, g_state, NULL);

    if (hud_grab(scr, &px, &w, &h)) {
        int kb = (w * h * 3) / 1024;

        free(px);
        snprintf(g_text, sizeof g_text,
                 "抓屏成功：%d x %d（%d KB）\nAI 上传还没接上", w, h, kb);
        g_state = HUD_TEXT;
    } else {
        snprintf(g_text, sizeof g_text, "抓屏失败：拿不到帧缓冲");
        g_state = HUD_ERROR;
    }
    hud_draw(&g_cfg, g_state, g_text);
}

/* ---------------- 主循环 ---------------- */

int main(void)
{
    gfxInitDefault();
    cfg_load(&g_cfg);

    for (;;) {
        u32 down, held;

        hidScanInput();
        down = hidKeysDown();
        held = hidKeysHeld();

        if (g_mode == MODE_SETTINGS) {
            if (down & KEY_UP)   { if (--g_sel < 0) g_sel = SET_ROWS - 1; }
            if (down & KEY_DOWN) { if (++g_sel >= SET_ROWS) g_sel = 0; }
            if (down & KEY_LEFT)   row_step(g_sel, -1);
            if (down & KEY_RIGHT)  row_step(g_sel, +1);
            if (down & KEY_A) {
                cfg_save(&g_cfg);
                g_mode = MODE_GAME;
                g_state = HUD_HIDDEN;
            }
            if (down & (KEY_B | KEY_SELECT)) {
                cfg_load(&g_cfg);        /* 取消：把没保存的改动扔掉 */
                g_mode = MODE_GAME;
                g_state = HUD_HIDDEN;
            }
            draw_settings();
        } else {
            /* 长按触发键 → 进设置；短按 → 翻译。
             * 长按优先：先看按住多久，松开才判定短按，避免"想进设置却先截了图"。 */
            if (held & trigger_mask()) {
                if (++g_hold >= TRIGGER_LONG_FRAMES && !g_hold_fired) {
                    g_hold_fired = true;
                    g_sel = 0;
                    g_mode = MODE_SETTINGS;
                    g_state = HUD_HIDDEN;
                }
            } else {
                if (g_hold > 0 && g_hold < TRIGGER_LONG_FRAMES && !g_hold_fired)
                    start_translate();
                g_hold = 0;
                g_hold_fired = false;
            }

            if (g_mode == MODE_GAME)
                hud_draw(&g_cfg, g_state, g_text);
        }

        gfxFlushBuffers();
        gfxSwapBuffers();
        gspWaitForVBlank();
    }
    return 0;
}
