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

/* 快捷键组合的**个数**（真正的组合表在 main.c 的 k_trig[] 里）。 */
#define CFG_TRIGGER_MAX 8

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
    /* 唤出面板的快捷键：存的是**下标**，具体组合表在 main.c 的 k_trig[]。
     * 为什么存下标不存字符串：组合会增删、名字会改，存下标最稳。
     * ⚠️ 新增组合只能**追加到末尾**，不能插在中间 —— 插了老配置会错位。 */
    int       trigger;

    /* ★ 自由摆放（拖动页确认过的位置）。
     *   free_pos=false 时按 anchor 的九宫格算，true 时直接用 pos_x/pos_y。
     *   为什么两套并存：九宫格是"一键就能摆好"的快路径，
     *   自由位置是"我想压在血条旁边"的精细路径。只有自由位置的话，
     *   每次换游戏都要重新拖一遍。 */
    bool      free_pos;
    int       pos_x, pos_y;   /* 目标屏上的像素坐标（左上角） */
} PluginCfg;

void cfg_defaults(PluginCfg *c);

/* 读不到就用默认值，**不报错**：第一次装插件时文件本来就不存在。 */
void cfg_load(PluginCfg *c);

/* 写失败也只是下次回到默认值，不该打断游戏 —— 所以返回 void。 */
void cfg_save(const PluginCfg *c);

/* 给人看的名字（设置页直接用）。 */
const char *cfg_screen_name(CfgScreen s);
const char *cfg_anchor_name(CfgAnchor a);

/* 面板落在屏幕上的像素矩形。free_pos 时用 pos_x/pos_y（并夹在屏内），
 * 否则按 anchor 的九宫格算。 */
void cfg_panel_rect(const PluginCfg *c, int scr_w, int scr_h,
                    int *x, int *y, int *w, int *h);
