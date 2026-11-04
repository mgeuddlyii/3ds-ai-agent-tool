/*
 * font16.h -- 内嵌 16×16 点阵字库（由 tools/mkfont16.py 生成）
 *
 * 为什么插件要自带字库：
 *   libctru 的控制台字体只有 ASCII，系统字库（citro2d + 共享字体）要接管 GPU，
 *   在游戏进程里那是高风险动作。点阵只往帧缓冲写字节，不碰 GPU 状态。
 *
 * 覆盖：ASCII 可见字符 + GB2312 全表（7540 字形，约 235KB）。
 * 没有的字（生僻字/emoji）会画成空心方框 —— 明确"缺字"，而不是静默画空白。
 */

#pragma once

#include <stdint.h>

typedef struct {
    uint16_t cp;    /* Unicode 码点 */
    uint16_t idx;   /* 位图序号 */
} Font16Entry;

extern const unsigned char  g_font16_bits[];
extern const Font16Entry    g_font16_index[];
extern const int            g_font16_count;

/* 找不到返回 NULL。 */
const unsigned char *font16_find(uint32_t cp);
