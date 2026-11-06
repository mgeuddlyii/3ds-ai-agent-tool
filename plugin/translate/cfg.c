/*
 * cfg.c -- 见 cfg.h
 */

#include "cfg.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CFG_PATH "sdmc:/3ds/aiconsole/plugin.ini"

/* 档位边界写在这里，设置页与绘制都用同一份常量 —— 两处各写一遍
 * 就会出"设置里能选到 60 列，画的时候按 50 截断"这种毛病。 */
#define COLS_MIN 10
#define COLS_MAX 50
#define ROWS_MIN 2
#define ROWS_MAX 16

void cfg_defaults(PluginCfg *c)
{
    if (!c)
        return;
    c->screen  = SCR_BOTTOM;
    c->anchor  = ANCH_BL;      /* 左下角：最不挡游戏画面的位置之一 */
    c->cols    = 34;
    c->rows    = 7;   /* 按钮条吃掉 24px，6 行只剩 3 行正文 */
    c->opacity = 2;
    c->use_zl  = false;
    c->free_pos = false;
    c->pos_x    = 8;
    c->pos_y    = 136;
}

const char *cfg_screen_name(CfgScreen s)
{
    return s == SCR_TOP ? "top" : "bottom";
}

const char *cfg_anchor_name(CfgAnchor a)
{
    switch (a) {
    case ANCH_TL: return "top-left";
    case ANCH_TC: return "top-center";
    case ANCH_TR: return "top-right";
    case ANCH_ML: return "mid-left";
    case ANCH_MC: return "center";
    case ANCH_MR: return "mid-right";
    case ANCH_BL: return "bottom-left";
    case ANCH_BC: return "bottom-center";
    case ANCH_BR: return "bottom-right";
    }
    return "bottom-left";
}

static int anchor_from_name(const char *s)
{
    for (int i = 0; i <= (int)ANCH_BR; ++i)
        if (strcmp(s, cfg_anchor_name((CfgAnchor)i)) == 0)
            return i;
    return ANCH_BL;
}

static int clamp(int v, int lo, int hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

void cfg_load(PluginCfg *c)
{
    FILE *f = NULL;

    if (!c)
        return;
    cfg_defaults(c);

    f = fopen(CFG_PATH, "r");
    if (!f)
        return;

    {
        char line[128];
        while (fgets(line, sizeof line, f)) {
            char *eq, *k, *v;

            if (line[0] == '#' || line[0] == '\n')
                continue;
            eq = strchr(line, '=');
            if (!eq)
                continue;
            *eq = '\0';
            k = line;
            v = eq + 1;
            /* 去掉行尾空白（Windows 记事本存出来会是 \r\n） */
            while (*v && (v[strlen(v) - 1] == '\n' || v[strlen(v) - 1] == '\r'))
                v[strlen(v) - 1] = '\0';

            if (strcmp(k, "screen") == 0)
                c->screen = strcmp(v, "top") == 0 ? SCR_TOP : SCR_BOTTOM;
            else if (strcmp(k, "anchor") == 0)
                c->anchor = (CfgAnchor)anchor_from_name(v);
            else if (strcmp(k, "cols") == 0)
                c->cols = clamp(atoi(v), COLS_MIN, COLS_MAX);
            else if (strcmp(k, "rows") == 0)
                c->rows = clamp(atoi(v), ROWS_MIN, ROWS_MAX);
            else if (strcmp(k, "opacity") == 0)
                c->opacity = clamp(atoi(v), 0, 3);
            else if (strcmp(k, "key") == 0)
                c->use_zl = strcmp(v, "zl") == 0;
            else if (strcmp(k, "free") == 0)
                c->free_pos = atoi(v) != 0;
            else if (strcmp(k, "pos_x") == 0)
                c->pos_x = atoi(v);
            else if (strcmp(k, "pos_y") == 0)
                c->pos_y = atoi(v);
        }
        fclose(f);
    }
}

void cfg_save(const PluginCfg *c)
{
    FILE *f;

    if (!c)
        return;
    f = fopen(CFG_PATH, "w");
    if (!f)
        return;
    fprintf(f, "# AIConsole 翻译插件设置（可以手改，游戏里下次启动生效）\n");
    fprintf(f, "screen=%s\n",  cfg_screen_name(c->screen));
    fprintf(f, "anchor=%s\n",  cfg_anchor_name(c->anchor));
    fprintf(f, "cols=%d\n",    c->cols);
    fprintf(f, "rows=%d\n",    c->rows);
    fprintf(f, "opacity=%d\n", c->opacity);
    fprintf(f, "key=%s\n",     c->use_zl ? "zl" : "select");
    fprintf(f, "free=%d\n",    c->free_pos ? 1 : 0);
    fprintf(f, "pos_x=%d\n",   c->pos_x);
    fprintf(f, "pos_y=%d\n",   c->pos_y);
    fclose(f);
}

void cfg_panel_rect(const PluginCfg *c, int scr_w, int scr_h,
                    int *x, int *y, int *w, int *h)
{
    int pw, ph, px, py, col, row;

    if (!c)
        return;

    pw = c->cols * 8;
    ph = c->rows * 8;
    if (pw > scr_w) pw = scr_w;
    if (ph > scr_h) ph = scr_h;

    if (c->free_pos) {
        px = c->pos_x;
        py = c->pos_y;
        if (px > scr_w - pw) px = scr_w - pw;
        if (py > scr_h - ph) py = scr_h - ph;
        if (px < 0) px = 0;
        if (py < 0) py = 0;
        if (x) *x = px;
        if (y) *y = py;
        if (w) *w = pw;
        if (h) *h = ph;
        return;
    }

    col = (int)c->anchor % 3;
    row = (int)c->anchor / 3;

    /* 留 8px 边：贴边会让文字压到屏幕圆角与扫描边缘 */
    px = col == 0 ? 8 : (col == 1 ? (scr_w - pw) / 2 : scr_w - pw - 8);
    py = row == 0 ? 8 : (row == 1 ? (scr_h - ph) / 2 : scr_h - ph - 8);

    if (px < 0) px = 0;
    if (py < 0) py = 0;

    if (x) *x = px;
    if (y) *y = py;
    if (w) *w = pw;
    if (h) *h = ph;
}
