/*
 * text.h -- UTF-8 解码、文本度量与折行排版
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "render.h"

/*: 默认行高。字形格是 16 高，留 2px 行距。
 *: 三、五行字不会挤在一起，也不至于太浪费 240px 的高度。 */
#define TEXT_LINE_H 18

/* --------------------------------------------------------------------------
 * UTF-8
 * ----------------------------------------------------------------------- */

/* 解一个码位。返回消耗的字节数；返回 0 表示非法序列（调用方跳过 1 字节）。 */
int utf8_decode(const char *s, uint32_t *cp);

/* 编码一个码位。返回写入的字节数。 */
int utf8_encode(uint32_t cp, char *out, size_t cap);

/* --------------------------------------------------------------------------
 * 度量
 * ----------------------------------------------------------------------- */

/* 不含换行符的单行宽度（px）。遇 '\n' 停止。 */
int text_width(const char *utf8);

/* 按 max_w 折行后的行数。 */
int text_line_count(const char *utf8, int max_w);

/* --------------------------------------------------------------------------
 * 绘制
 * ----------------------------------------------------------------------- */

/* 在 rect 内画多行文本，自动折行 + 裁剪。返回画出的行数。
 * line_height 传 0 用 TEXT_LINE_H。 */
int text_draw(const Rect *rect, const char *utf8, RColor c, int line_height);

/* render.h 里声明的单行版：在 rect 左上角起笔，不折行，超出 rect 宽度的裁掉。 */
void r_text(const Rect *r, const char *utf8, RColor c);
