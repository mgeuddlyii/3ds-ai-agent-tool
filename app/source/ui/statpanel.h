/*
 * statpanel.h -- 对话页滑轨上的「状态」面板
 *
 * 数据来自 shared/sysstat.c（每帧采样，主循环里零额外 syscall）。
 * 三块：主循环占用（折线）、帧耗时（折线，带 16.7ms 基准线）、内存（条）。
 *
 * 标签刻意用 ASCII（CPU / FRAME / MEM）：
 *   · 滑轨那几格只有 44px 宽，中文两个字刚好，英文单词放不下；
 *   · 这三个词在任何语言下都认得出，且不必再往 i18n 表里加三条短标签。
 */

#pragma once

/* 画在 dock 面板内容区（y 46..205，x 10..310）。 */
void statpanel_render(void);
