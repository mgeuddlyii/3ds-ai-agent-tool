/*
 * content_panel.h -- 内容包面板（原「内容更新」屏）
 *
 * 2026-10-25 用户要求：内容更新不再单独保留，它的功能**整合进「软件更新」**。
 *
 * 做法上不复制那 600 多行：把原 sc_content.c 的四个入口改成可复用函数，
 * 由 sc_update.c 在「内容包」页**整屏转发**过去。
 *   · 只有一份实现，将来改内容包的交互不会出现两套行为；
 *   · 内容包自带页眉/页脚与坐标（5 行状态机 + 装/退两种模式 + 确认），
 *     整屏转发最稳，不需要把它的坐标重新排一遍（重排必然踩重叠）。
 *
 * ⚠️ 面板自己处理 UP/DOWN/LEFT/RIGHT/A/B/Y；**L/R 留给宿主**做页签切换，
 *    所以宿主要先截走 L/R，再把剩下的键交给 cu_panel_handle()。
 */

#pragma once

#include <3ds.h>

/* 进面板：重扫一遍本地清单（这也是唯一刷新本机版本/备份状态的时机）。 */
void cu_panel_enter(void);

void cu_panel_render_top(void);
void cu_panel_render_bottom(void);

/* 转发按键。**调用前宿主必须已经截走 L/R**（见上）。 */
void cu_panel_handle(u32 down, u32 held);
