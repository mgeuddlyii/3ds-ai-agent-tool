/*
 * hud.h -- 在游戏画面上叠一块"翻译屏"
 *
 * 为什么自己往帧缓冲写像素，而不是用 libctru 的 console：
 *   PrintConsole 的 windowX/Y/Width/Height 在 libctru 里标着 **not implemented**，
 *   也就是说它只能整屏。而用户要的正是"翻译屏可以在任意位置、任意大小"。
 *
 * ⚠️ 帧缓冲格式：3DS 是 RGB8，但字节顺序是 B,G,R。
 *    所有颜色都走 rgba() 打包，别在调用点手写三元组 —— 试过就会懂。
 */

#pragma once

#include <3ds.h>
#include "cfg.h"

typedef enum {
    HUD_HIDDEN = 0,
    HUD_CAPTURING,      /* 正在抓屏 */
    HUD_WAITING,        /* 等模型回话 */
    HUD_TEXT,           /* 有译文了 */
    HUD_ERROR
} HudState;

/* 面板上的三个按钮：翻译 / 设置 / 说明 */
typedef enum { BTN_TRANSLATE = 0, BTN_SETTINGS, BTN_HELP, BTN_TEST, BTN_COUNT } HudBtn;

/* 高亮哪个按钮（键盘用 ←/→ 选择时，必须让用户看见选中的是哪个）。
 * 触摸可用时也用它做按下反馈。传 -1 取消高亮。 */
void hud_set_sel(int i);

/* 取当前屏的帧缓冲并画一帧（含顶部按钮条）。text 为 NULL 时按 state 画占位。
 * 返回 false = 拿不到帧缓冲（游戏没初始化 gfx 时可能发生）。 */
bool hud_draw(const PluginCfg *cfg, HudState st, const char *text);

/* 屏幕坐标 → 命中的按钮；没命中返回 -1。
 * 触摸判定必须和绘制用**同一个** cfg_panel_rect，否则会出现
 * "看着点在按钮上、实际点空" —— 这种偏差在真机上极难察觉。 */
int hud_button_at(const PluginCfg *cfg, int tx, int ty);

/* ---- 给设置页用的底层原语 ----------------------------------------------
 * 设置页和翻译屏都走同一套绘制（不引 console，也不引 citro2d）：
 * 一条代码路径，位置/字体表现一致，出问题只有一处要看。 */
bool hud_begin(gfxScreen_t scr, int *w, int *h);
void hud_end(void);
void hud_rect(int x, int y, int w, int h, int r, int g, int b);
void hud_frame(int x, int y, int w, int h, int r, int g, int b);
void hud_text(int x, int y, int max_x, const char *s, int r, int g, int b);
int  hud_text_w(const char *s);

/* 抓当前屏的一块像素（目前只做整屏拷贝，给上传用）。
 * 成功返回 true，并填 w/h/stride；调用方负责 free。 */
bool hud_grab(gfxScreen_t scr, u8 **out, int *w, int *h);
