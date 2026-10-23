/*
 * font.h -- 点阵字库运行时
 *
 * 字库文件由 tools/mkfont.py 离线生成（AFNT v1），格式见该脚本头部注释。
 * 运行时把整个 font.bin 读进内存，之后按码位二分查找，不再碰文件系统。
 *
 * 为什么整份读进内存：一个字形的随机读取如果每次都走 FS 会很慢
 * （每帧几百次 FSFile 调用）。整份 ~300 KB，对 3DS 的堆来说是小数目。
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    const uint8_t *bits;    /* 字形位图，长度 = cell_w*cell_h*depth/8 */
    uint8_t        advance; /* 笔位前进量（全角 16，半角按字体比例） */
    uint8_t        flags;
} RGlyph;

#define RGLYPH_WIDE 0x01u

/* 使用内嵌字库（app/data/font.bin，bin2s 编入）。成功返回 0。 */
int  font_init(void);
void font_exit(void);

/* 从指定路径读一份字库（换字体 / 调试用）。这块内存由本模块负责释放。 */
int  font_load_from_file(const char *path);

/* 直接借用一块已映射的内存（**不**接管它的生命周期）。 */
int  font_load_from_memory(const uint8_t *data, size_t size);

/* 查字形。永远不返回 NULL（无兜底时才返回 NULL）。 */
const RGlyph *font_lookup(uint32_t codepoint);

int      font_cell_w(void);
int      font_cell_h(void);
int      font_baseline(void);      /* 格内基线行号 */
int      font_glyph_count(void);
uint32_t font_codepoint_at(uint32_t index);   /* 巡检器用 */
bool     font_ready(void);

/* 当前字库位深（1 / 4 / 8）。未就绪返回 0。
 * v1 字库恒为 1；v2 由 flags 字段决定。 */
int      font_depth(void);

/* ★ 位深无关的覆盖率读取（0..255，格内越界返回 0）。
 * 凡"这个像素算不算有墨"的判据都走这里 —— **别自己写位寻址**，
 * 那样只对 1bpp 成立，换成灰度字库后会静默读出垃圾。
 * 越界判定由本函数负责，调用方不必再判。
 * ⚠️ 渲染循环里优先用 font_glyph_alpha_row()。 */
uint8_t  font_glyph_alpha(const RGlyph *g, int x, int y);

/* ★ 把第 y 行展开成 cell_w 个覆盖率写进 out，返回"该行是否有墨"。
 * 渲染循环用这个：调用数从 每字 cell_w*cell_h 降到 cell_w，
 * 且保留了"整行为空就跳过"的快速路径。
 * out 至少 cell_w 字节；越界行返回 false 并清零。 */
bool     font_glyph_alpha_row(const RGlyph *g, int y, uint8_t *out);
