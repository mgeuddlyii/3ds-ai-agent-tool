/*
 * help.h -- 插件内的使用说明
 *
 * 为什么说明要放在插件里，而不是只写 README：
 *   插件是"游戏正跑着的时候"才出现的。这时候人手上只有 3DS，
 *   电脑可能根本没开着 —— README 在那种时刻等于不存在。
 *   所以教程必须是**能就地翻**的。
 *
 * 内容用一段长文本 + 自动折行，不手排版：
 *   手排会在换字号/改宽度时全部错位，而 16px 是固定的，
 *   折行逻辑只写一次，以后加条目只要往文本里塞句子。
 */

#pragma once

#include <stdbool.h>

/* 一屏能放几行（由调用方按屏幕高度算出来）。 */
void help_init(int lines_per_page);

/* 上下滚一行 / 左右翻一页。返回 true 表示确实动了（用来触发重绘）。 */
bool help_scroll(int lines);
bool help_page(int dir);

/* 当前页 / 总页数，给页脚用。 */
void help_pos(int *page, int *pages);

/* 画一整屏。宽高由调用方给，内部按 16px 字宽/字高折行。 */
void help_draw(int w, int h);
