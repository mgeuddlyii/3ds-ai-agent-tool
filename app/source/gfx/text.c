/*
 * text.c -- UTF-8 + 折行排版 + 文本绘制
 *
 * 折行规则（M1 够用版，M3 上软键盘时再细化）
 * ------------------------------------------------
 *   1. '\n' 硬换行
 *   2. 汉字/日文假名/全角标点：**任意处可断**
 *      —— 这是中文排版的正常行为，不需要按词切
 *   3. 半角字母数字：优先在**空格**处断，保住整词
 *   4. 禁则处理：收尾标点（，。、；：？！）」』》〉】…·—％）不允许出现在行首，
 *      实在放不下就让它挤在本行末尾（最多溢出半个字宽），比把标点甩到下一行好看。
 */
#include <stdbool.h>
#include <string.h>

#include "font.h"
#include "render.h"
#include "text.h"

/* --------------------------------------------------------------------------
 * UTF-8
 * ----------------------------------------------------------------------- */

int utf8_decode(const char *s, uint32_t *cp)
{
    const uint8_t *p = (const uint8_t *)s;
    uint8_t b0 = p[0];

    if (b0 < 0x80u) {
        *cp = b0;
        return 1;
    }
    if ((b0 & 0xE0u) == 0xC0u) {
        if ((p[1] & 0xC0u) != 0x80u) return 0;
        *cp = ((uint32_t)(b0 & 0x1Fu) << 6) | (uint32_t)(p[1] & 0x3Fu);
        return (*cp < 0x80u) ? 0 : 2;
    }
    if ((b0 & 0xF0u) == 0xE0u) {
        if ((p[1] & 0xC0u) != 0x80u || (p[2] & 0xC0u) != 0x80u) return 0;
        *cp = ((uint32_t)(b0 & 0x0Fu) << 12)
            | ((uint32_t)(p[1] & 0x3Fu) << 6)
            | (uint32_t)(p[2] & 0x3Fu);
        return (*cp < 0x800u) ? 0 : 3;
    }
    if ((b0 & 0xF8u) == 0xF0u) {
        if ((p[1] & 0xC0u) != 0x80u || (p[2] & 0xC0u) != 0x80u
            || (p[3] & 0xC0u) != 0x80u)
            return 0;
        *cp = ((uint32_t)(b0 & 0x07u) << 18)
            | ((uint32_t)(p[1] & 0x3Fu) << 12)
            | ((uint32_t)(p[2] & 0x3Fu) << 6)
            | (uint32_t)(p[3] & 0x3Fu);
        return (*cp < 0x10000u || *cp > 0x10FFFFu) ? 0 : 4;
    }
    return 0;
}

int utf8_encode(uint32_t cp, char *out, size_t cap)
{
    if (cp < 0x80u) {
        if (cap < 1) return 0;
        out[0] = (char)cp;
        return 1;
    }
    if (cp < 0x800u) {
        if (cap < 2) return 0;
        out[0] = (char)(0xC0u | (cp >> 6));
        out[1] = (char)(0x80u | (cp & 0x3Fu));
        return 2;
    }
    if (cp < 0x10000u) {
        if (cap < 3) return 0;
        out[0] = (char)(0xE0u | (cp >> 12));
        out[1] = (char)(0x80u | ((cp >> 6) & 0x3Fu));
        out[2] = (char)(0x80u | (cp & 0x3Fu));
        return 3;
    }
    if (cap < 4) return 0;
    out[0] = (char)(0xF0u | (cp >> 18));
    out[1] = (char)(0x80u | ((cp >> 12) & 0x3Fu));
    out[2] = (char)(0x80u | ((cp >> 6) & 0x3Fu));
    out[3] = (char)(0x80u | (cp & 0x3Fu));
    return 4;
}

/* --------------------------------------------------------------------------
 * 小工具
 * ----------------------------------------------------------------------- */

/*: 宽字符（汉字/假名/CJK 标点/全角）。到了这个区段就可以任意断行。 */
static inline bool is_wide_cp(uint32_t cp)
{
    /* 0x1F300+ 是 emoji 区：字库里这些字形 advance=16（全宽），
     * 断行偏好必须与真实步进一致，否则会出现"空格后紧跟 emoji 却被
     * 优先在空格处折行"的怪折行。只影响断行偏好，不影响排版宽度
     * （宽度走 font_lookup 的 advance）。*/
    return (cp >= 0x2E80u && cp <= 0xFF60u) || cp >= 0x1F300u;
}

int text_line_h(void)
{
    int s = r_text_scale_get();
    if (s <= 100)
        return TEXT_LINE_H;
    return (TEXT_LINE_H * s + 99) / 100;
}

static inline int glyph_advance(uint32_t cp)
{
    const RGlyph *g = font_lookup(cp);
    int           base = g ? (int)g->advance : font_cell_w();
    int           s    = r_text_scale_get();

    /* 缩放时 advance 必须同步，否则"字放大了但折行按原宽算" ——
     * 结果是每行右侧被裁掉一块，而排版护栏会以为一切正常。 */
    if (s == 100)
        return base;
    return (base * s + 99) / 100;
}

/*: 不允许出现在行首的收尾标点 */
static bool is_no_line_start(uint32_t cp)
{
    switch (cp) {
        case 0x3001: case 0x3002:                     /* 、 。 */
        case 0xFF0C: case 0xFF0E: case 0xFF1A:        /* ， ． ： */
        case 0xFF1B: case 0xFF01: case 0xFF1F:        /* ； ！ ？ */
        case 0x300D: case 0x300F: case 0x3009:        /* 」 』 〉 */
        case 0x300B: case 0x3011:                     /* 》 】 */
        case 0x2026: case 0x00B7: case 0x2014:        /* … · — */
        case 0x0029: case 0x005D: case 0x007D:        /* ) ] } */
        case 0xFF09: case 0xFF3D: case 0xFF5D:        /* ） ］ ｝ */
            return true;
        default:
            return false;
    }
}

/* --------------------------------------------------------------------------
 * 度量
 * ----------------------------------------------------------------------- */

int text_width(const char *utf8)
{
    int w = 0;
    size_t i = 0;
    while (utf8[i]) {
        uint32_t cp;
        int n = utf8_decode(utf8 + i, &cp);
        if (n <= 0) { n = 1; cp = 0xFFFD; }
        if (cp == '\n')
            break;
        w += glyph_advance(cp);
        i += (size_t)n;
    }
    return w;
}

/*: 返回第一行**吃掉**的字节数（含行尾的 '\n'）。 */
static size_t first_line_bytes(const char *s, int max_w)
{
    int w = 0;
    size_t i = 0;
    size_t last_space_after = 0;     /* 最近一个空格之后的字节位置 */

    while (s[i]) {
        uint32_t cp;
        int n = utf8_decode(s + i, &cp);
        if (n <= 0) { n = 1; cp = 0xFFFD; }

        if (cp == '\n')
            return i + (size_t)n;

        int adv = glyph_advance(cp);

        if (i > 0 && w + adv > max_w) {
            /* 拉丁词优先在空格处断 */
            if (!is_wide_cp(cp) && last_space_after > 0 && last_space_after <= i)
                return last_space_after;
            /* 禁则：收尾标点硬留在本行 */
            if (!is_no_line_start(cp))
                return i;
        }

        w += adv;
        i += (size_t)n;
        if (cp == ' ')
            last_space_after = i;
    }
    return i;
}

int text_line_count(const char *utf8, int max_w)
{
    if (max_w <= 0)
        return 0;
    int lines = 0;
    size_t i = 0;
    while (utf8[i]) {
        size_t n = first_line_bytes(utf8 + i, max_w);
        if (n == 0)
            break;
        i += n;
        ++lines;
    }
    return lines;
}

/* --------------------------------------------------------------------------
 * 绘制
 * ----------------------------------------------------------------------- */

int text_draw(const Rect *rect, const char *utf8, RColor c, int line_height)
{
    if (line_height <= 0)
        line_height = TEXT_LINE_H;

    const int max_w = (int)rect->w;
    const int cell_h = font_cell_h();
    const float bottom = rect->y + rect->h;

    int lines = 0;
    size_t i = 0;

    r_clip_push(rect);

    while (utf8[i]) {
        float y = rect->y + (float)(lines * line_height);
        if (y + (float)cell_h > bottom)
            break;                       /* 矩形装不下了 */

        size_t n = first_line_bytes(utf8 + i, max_w);
        if (n == 0)
            break;

        float x = rect->x;
        for (size_t k = 0; k < n; ) {
            uint32_t cp;
            int m = utf8_decode(utf8 + i + k, &cp);
            if (m <= 0) { m = 1; cp = 0xFFFD; }
            if (cp != '\n') {
                r_glyph(x, y, cp, c);
                x += (float)glyph_advance(cp);
            }
            k += (size_t)m;
        }

        i += n;
        ++lines;
    }

    r_clip_pop();
    return lines;
}

void r_text(const Rect *r, const char *utf8, RColor c)
{
    r_clip_push(r);
    float x = r->x;
    for (size_t i = 0; utf8[i]; ) {
        uint32_t cp;
        int n = utf8_decode(utf8 + i, &cp);
        if (n <= 0) { n = 1; cp = 0xFFFD; }
        if (cp == '\n')
            break;
        r_glyph(x, r->y, cp, c);
        x += (float)glyph_advance(cp);
        i += (size_t)n;
    }
    r_clip_pop();
}
