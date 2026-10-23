/*
 * font.c -- 字库加载与查询
 *
 * AFNT v2 布局（小端）：
 *   Header 32B: magic "AFNT" | u16 ver | u16 flags | u8 cell_w | u8 cell_h
 *               | u8 baseline | u8 rsv | u32 count | u32 index_off
 *               | u32 bitmap_off | u32 bitmap_size | u32 fallback
 *   flags bit0..1 = 位深编码（0=1bpp / 1=4bpp / 2=8bit）
 *   Index : count * 8B,  { u32 cp; u8 advance; u8 flags; u16 rsv }  按 cp 升序
 *   Bitmap: count * cell_w*cell_h*depth/8 字节，第 i 个字形在 bitmap_off + i*cell_bytes
 *           depth=1 时与 v1 布局完全相同（每行 2 字节，MSB 在左）
 *
 * ★ v1（ver=1）**仍可读**，且默认按 1bpp 解释 —— 这样改动前后都能跑，
 *   出问题能二分定位是格式的锅还是渲染的锅。
 *
 * 数据来源是**内嵌**的（app/data/font.bin 经 bin2s 编成 font_bin.o），
 * 不走 romfs —— 原因见 Makefile 里的说明。
 */
#include <3ds.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "font.h"
#include "font_bin.h"     /* 由 bin2s 生成到 build/ ：font_bin[] / font_bin_size */

#define AFNT_MAGIC     "AFNT"
#define AFNT_HEADER_SZ 32u
#define AFNT_INDEX_SZ  8u

static const uint8_t *s_data;      /* 借用：指向内嵌数组或调用方给的内存 */
static uint8_t       *s_owned;     /* 自己 malloc 的那份，退出时才释放 */
static size_t         s_size;
static bool           s_ok;

static const uint8_t *s_index;
static const uint8_t *s_bitmap;
static uint32_t s_count;
static uint32_t s_fallback;
static uint8_t  s_cell_w, s_cell_h, s_baseline;
static uint32_t s_cell_bytes;
static uint8_t  s_depth;          /* 位深：1 / 4 / 8 bpp */

static inline uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0]
         | ((uint32_t)p[1] << 8)
         | ((uint32_t)p[2] << 16)
         | ((uint32_t)p[3] << 24);
}

static inline uint16_t rd16(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

/* 借用语义：不接管 data 的生命周期。 */
int font_load_from_memory(const uint8_t *data, size_t size)
{
    if (!data || size < AFNT_HEADER_SZ)
        return -1;
    if (memcmp(data, AFNT_MAGIC, 4) != 0)
        return -2;

    uint16_t ver     = rd16(data + 4);
    uint16_t hflags  = rd16(data + 6);
    if (ver != 1 && ver != 2)
        return -3;

    /* 位深：v1 没有这个字段（一律 1bpp）；v2 从 flags 低 2 位取。
     * ⚠️ 编码 3 是保留值，必须显式拒掉 —— 落到下面的 switch 默认分支，
     *    而"默认值恰好等于 1bpp"会把未知格式当成合法字库读出乱码字形。 */
    static const uint8_t depth_of_code[4] = { 1, 4, 8, 0 };
    uint8_t depth = (ver == 1) ? 1u : depth_of_code[hflags & 3u];
    if (depth == 0)
        return -9;

    uint8_t  cw      = data[8];
    uint8_t  ch      = data[9];
    uint8_t  bl      = data[10];
    uint32_t count   = rd32(data + 12);
    uint32_t ioff    = rd32(data + 16);
    uint32_t boff    = rd32(data + 20);
    uint32_t bsize   = rd32(data + 24);
    uint32_t fb      = rd32(data + 28);

    if (cw == 0 || ch == 0)
        return -4;
    /* 1bpp 要求 cell_w 是 8 的倍数（按字节打包，MSB 在左）；灰度不用 */
    if (depth == 1 && (cw % 8) != 0)
        return -4;
    if (count == 0 || fb >= count)
        return -5;
    if ((size_t)ioff + (size_t)count * AFNT_INDEX_SZ > size)
        return -6;

    /* 位图区：既查声明的 bsize，也要查"按位深算出来的"应有大小。
     * 只查 bsize 的话，一个 bsize 写小了的字库会让后面的字形全部错位，
     * 表现为"某些字变成别的字"—— 不报错、看起来像渲染 bug。
     * 这里取两者**较大值**再查，保证读到的每一格都在缓冲内。
     *
     * ★ 两处历史错误（2026-10-18 修）：
     *  1. 早先写成 `if (bsize < need) need = bsize;` —— 取的是**较小值**，
     *     和上面这段注释正好相反。后果：bsize 写小的字库反而更容易通过校验，
     *     然后按 index 取字形就越界读堆（3DS 无 MMU，读脏数据不崩）。
     *  2. `count * cw * ch * depth / 8` 是 u32 乘法且**无溢出保护**。
     *     count 来自不可信的文件头，设成极大值会让乘积回绕成小数，
     *     校验照样通过 —— 等于把上一行的检查变成摆设。
     * 现在用 u64 算、取 max、并先做一次范围合理性检查。 */
    if (cw == 0 || ch == 0 || depth == 0 || cw > 256 || ch > 256)
        return -8;
    {
        uint64_t need64 = (uint64_t)count * (uint64_t)cw * (uint64_t)ch
                        * (uint64_t)depth / 8u;
        uint64_t bs64   = (uint64_t)bsize;
        if (bs64 > need64)
            need64 = bs64;                  /* 取较大值 */
        if ((uint64_t)boff + need64 > (uint64_t)size)
            return -7;
    }

    s_data       = data;
    s_size       = size;
    s_index      = data + ioff;
    s_bitmap     = data + boff;
    s_count      = count;
    s_fallback   = fb;
    s_cell_w     = cw;
    s_cell_h     = ch;
    s_baseline   = bl;
    s_depth      = depth;
    s_cell_bytes = (uint32_t)cw * ch * depth / 8u;
    s_ok         = true;
    return 0;
}

int font_init(void)
{
    if (font_bin_size < AFNT_HEADER_SZ)
        return -8;
    return font_load_from_memory(font_bin, font_bin_size);
}

/* 从 SD 卡加载一份字库（换字体 / 调试用）。这份内存由我们负责释放。 */
int font_load_from_file(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return -2;

    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return -3; }
    long sz = ftell(f);
    if (sz <= 0) { fclose(f); return -4; }
    rewind(f);

    uint8_t *buf = (uint8_t *)malloc((size_t)sz);
    if (!buf) { fclose(f); return -5; }

    size_t got = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    if (got != (size_t)sz) { free(buf); return -6; }

    int r = font_load_from_memory(buf, (size_t)sz);
    if (r != 0) { free(buf); return r; }

    if (s_owned)
        free(s_owned);
    s_owned = buf;
    return 0;
}

void font_exit(void)
{
    if (s_owned) {
        free(s_owned);
        s_owned = NULL;
    }
    s_data = NULL;
    s_index = NULL;
    s_bitmap = NULL;
    s_count = 0;
    s_ok = false;
}

bool font_ready(void) { return s_ok; }

int font_cell_w(void)  { return s_cell_w; }
int font_cell_h(void)  { return s_cell_h; }
int font_baseline(void){ return s_baseline; }

int font_glyph_count(void) { return (int)s_count; }

uint32_t font_codepoint_at(uint32_t index)
{
    if (!s_ok || index >= s_count)
        return 0;
    return rd32(s_index + (size_t)index * AFNT_INDEX_SZ);
}

static const RGlyph *glyph_at(uint32_t index)
{
    static RGlyph g;
    const uint8_t *e = s_index + (size_t)index * AFNT_INDEX_SZ;
    g.bits    = s_bitmap + (size_t)index * s_cell_bytes;
    g.advance = e[4];
    g.flags   = e[5];
    return &g;
}

const RGlyph *font_lookup(uint32_t codepoint)
{
    if (!s_ok)
        return NULL;

    uint32_t lo = 0, hi = s_count;          /* [lo, hi) */
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2u;
        uint32_t cp  = rd32(s_index + (size_t)mid * AFNT_INDEX_SZ);
        if (cp == codepoint)
            return glyph_at(mid);
        if (cp < codepoint)
            lo = mid + 1u;
        else
            hi = mid;
    }
    return glyph_at(s_fallback);
}

int font_depth(void) { return s_ok ? (int)s_depth : 0; }

/* 把字形第 y 行展开成 cell_w 个覆盖率，写进 out；返回该行是否"有墨"。
 *
 * 为什么要按行而不是按像素对外：
 *   按像素（font_glyph_alpha）会在渲染循环里形成 cross-TU 函数调用，
 *   16x16 一个字就是 256 次调用，每次都要过边界检查 + switch 位深。
 *   而旧代码有"整行为空就 continue"的快速路径 —— 直接改按像素会**丢掉**
 *   它，文字渲染明显变慢。
 *   按行展开把调用数降到 cell_w 分之一（16 次/字），快速路径也回来了，
 *   同时位深知识仍然只留在 font.c 里（**不在渲染层散落位寻址**）。
 *
 * out 必须至少有 cell_w 字节。越界行返回 false 并把 out 清零。
 */
bool font_glyph_alpha_row(const RGlyph *g, int y, uint8_t *out)
{
    int cw = (int)s_cell_w;
    if (!g || !g->bits || y < 0 || y >= (int)s_cell_h) {
        memset(out, 0, (size_t)cw);
        return false;
    }

    bool any = false;
    const uint8_t *row = g->bits + (size_t)y * s_cell_bytes / s_cell_h;

    switch (s_depth) {
    case 1: {
        /* 一行 == 2 字节（cw=16），MSB 在左 */
        for (int x = 0; x < cw; ++x) {
            uint8_t v = (uint8_t)((row[x >> 3] & (0x80u >> (x & 7)))
                                  ? 255u : 0u);
            out[x] = v;
            any |= (v != 0);
        }
        break;
    }
    case 8:
        /* 8bit：一行就是连续的 cw 字节，直接拷，零转换 */
        memcpy(out, row, (size_t)cw);
        for (int x = 0; x < cw; ++x)
            if (out[x]) { any = true; break; }
        break;
    case 4:
        for (int x = 0; x < cw; ++x) {
            uint8_t b = row[x >> 1];
            uint8_t nib = (x & 1) ? (uint8_t)(b & 0x0Fu) : (uint8_t)(b >> 4);
            uint8_t v = (uint8_t)(nib * 17u);
            out[x] = v;
            any |= (v != 0);
        }
        break;
    default:
        memset(out, 0, (size_t)cw);
        return false;
    }
    return any;
}

/* 位深无关的覆盖率读取（单像素）。
 * ⚠️ 渲染循环里**优先用 font_glyph_alpha_row()**，别逐像素调这个
 *    （会丢掉"整行为空跳过"的快速路径）。这个函数留给
 *    "取某一点"的场合，例如 UI 里画放大预览。
 *
 * ⚠️ 别在别处自己写位寻址。sc_lab.c 的字形巡检器曾硬编码
 *    `bits[py*2 + (px>>3)] & (0x80>>(px&7))` —— 那只对 1bpp 成立，
 *    字库换成灰度后会静默读出垃圾（看着像"巡检器坏了"，
 *    其实是读法错了）。同一个错误还会在 mkfont.py 的 audit_strokes 里重演，
 *    所以那边的 glyph_alpha_at() 与本函数**逐行对应**（铁律 6b）。
 *
 * 越界返回 0（= 全透明），与"格外的空白"一致。
 */
uint8_t font_glyph_alpha(const RGlyph *g, int x, int y)
{
    if (!g || !g->bits)
        return 0;
    if (x < 0 || y < 0 || x >= (int)s_cell_w || y >= (int)s_cell_h)
        return 0;

    switch (s_depth) {
    case 1: {
        /* ★ 行步长必须**算**出来，不能写死 2（B-09）。
         *   写死 2 等于假设 cell_w == 16；一旦加载别的宽度（比如 12），
         *   这里的 y*2 会读到上一行/下一行 —— 表现为"字形右半边发虚/串行"，
         *   而截图上看不出是读法错了。
         *   与 font_glyph_alpha_row() 用的是同一个式子（s_cell_bytes/s_cell_h）。 */
        size_t stride = (size_t)s_cell_bytes / (size_t)s_cell_h;
        return (uint8_t)((g->bits[(size_t)y * stride + (size_t)(x >> 3)]
                          & (0x80u >> (x & 7))) ? 255u : 0u);
    }
    case 4: {
        /* 每字节两个像素，高 4 位在左（与 mkfont.py 的 pack_gray 一致） */
        uint32_t i = (uint32_t)y * s_cell_w + (uint32_t)x;
        uint8_t  b = g->bits[i >> 1];
        uint8_t  nib = (i & 1u) ? (uint8_t)(b & 0x0Fu) : (uint8_t)(b >> 4);
        return (uint8_t)(nib * 17u);          /* 0..15 -> 0..255 */
    }
    case 8:
        return g->bits[(size_t)y * s_cell_w + (size_t)x];
    default:
        return 0;
    }
}
