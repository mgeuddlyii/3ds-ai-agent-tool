/*
 * cfg.h -- 插件的设置项（存在 SD 卡上，跨游戏、跨重启都记得）
 *
 * 为什么要落盘：这些设置是"这个人的屏幕观感"，每开一次游戏重设一遍
 * 比没有设置还烦。文件很小，一次读写就够，不需要任何缓存层。
 *
 * 文件：sdmc:/3ds/aiconsole/plugin.ini
 * 格式：key=value，一行一个，# 开头是注释。**故意用最笨的格式** ——
 *       插件里没有 JSON 解析器，也不想为六个字段引一个。
 */

#pragma once

#include <stdbool.h>

typedef enum {
    SCR_BOTTOM = 0,      /* 下屏：320×240，触控可达 */
    SCR_TOP              /* 上屏：400×240，不挡手指 */
} CfgScreen;

/* 九宫格锚点。翻译屏的边角贴着哪一格。 */
typedef enum {
    ANCH_TL = 0, ANCH_TC, ANCH_TR,
    ANCH_ML,     ANCH_MC, ANCH_MR,
    ANCH_BL,     ANCH_BC, ANCH_BR
} CfgAnchor;

typedef struct {
    CfgScreen screen;
    CfgAnchor anchor;
    int       cols;       /* 面板宽度（字符列，1 列 = 8px） */
    int       rows;       /* 面板高度（字符行，1 行 = 8px） */
    int       opacity;    /* 0..3 —— 背板深浅四档，不是 alpha（没有 alpha） */
    bool      use_zl;     /* true=用 ZL 触发，false=用 SELECT 单击 */
} PluginCfg;

void cfg_defaults(PluginCfg *c);

/* 读不到就用默认值，**不报错**：第一次装插件时文件本来就不存在。 */
void cfg_load(PluginCfg *c);

/* 写失败也只是下次回到默认值，不该打断游戏 —— 所以返回 void。 */
void cfg_save(const PluginCfg *c);

/* 给人看的名字（设置页直接用）。 */
const char *cfg_screen_name(CfgScreen s);
const char *cfg_anchor_name(CfgAnchor a);

/* 面板落在屏幕上的像素矩形。位置由 anchor + 屏幕尺寸算出来。 */
void cfg_panel_rect(const PluginCfg *c, int scr_w, int scr_h,
                    int *x, int *y, int *w, int *h);
