/*
 * hud.c -- 见 hud.h
 */

#include "hud.h"
#include "font16.h"

#include <stdlib.h>
#include <string.h>

/* 3DS 帧缓冲是 B,G,R。这里统一按 r,g,b 传，省得每个调用点记顺序。 */
typedef struct { u8 r, g, b; } Rgb;

static void px(u8 *fb, int stride, int x, int y, Rgb c)
{
    u8 *p = fb + (size_t)y * stride + (size_t)x * 3;

    p[0] = c.b;
    p[1] = c.g;
    p[2] = c.r;
}

static void fill(u8 *fb, int stride, int x0, int y0, int w, int h, Rgb c)
{
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
            px(fb, stride, x0 + x, y0 + y, c);
}

static void box(u8 *fb, int stride, int x0, int y0, int w, int h, Rgb c)
{
    for (int x = 0; x < w; ++x) {
        px(fb, stride, x0 + x, y0, c);
        px(fb, stride, x0 + x, y0 + h - 1, c);
    }
    for (int y = 0; y < h; ++y) {
        px(fb, stride, x0, y0 + y, c);
        px(fb, stride, x0 + w - 1, y0 + y, c);
    }
}

const unsigned char *font16_find(uint32_t cp)
{
    int lo = 0, hi = g_font16_count - 1;

    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (g_font16_index[mid].cp == cp)
            return g_font16_bits + (size_t)g_font16_index[mid].idx * 32;
        if (g_font16_index[mid].cp < cp)
            lo = mid + 1;
        else
            hi = mid - 1;
    }
    return NULL;
}

/* 极简 UTF-8 解码：够了，不引 iconv。非法序列当成 U+FFFD 并前进一字节，
 * 否则一个坏字节会让后面整段文字错位。 */
static uint32_t utf8_next(const char **s)
{
    const unsigned char *p = (const unsigned char *)*s;
    uint32_t cp;
    int      n;

    if (p[0] < 0x80) { cp = p[0]; n = 1; }
    else if ((p[0] & 0xE0) == 0xC0) { cp = p[0] & 0x1F; n = 2; }
    else if ((p[0] & 0xF0) == 0xE0) { cp = p[0] & 0x0F; n = 3; }
    else if ((p[0] & 0xF8) == 0xF0) { cp = p[0] & 0x07; n = 4; }
    else { *s += 1; return 0xFFFD; }

    for (int i = 1; i < n; ++i) {
        if ((p[i] & 0xC0) != 0x80) { *s += 1; return 0xFFFD; }
        cp = (cp << 6) | (p[i] & 0x3F);
    }
    *s += n;
    return cp;
}

/* 画一行字，返回画完之后的 x。超出右边界就停（宁可截断也不画出面板）。 */
static int draw_text(u8 *fb, int stride, int x, int y, int max_x,
                     const char *s, Rgb fg)
{
    if (!s)
        return x;

    while (*s && x + 16 <= max_x) {
        uint32_t cp = utf8_next(&s);
        const unsigned char *g = font16_find(cp);

        if (!g) {
            /* 缺字画空心方框：让"这里少个字"看得见 */
            box(fb, stride, x + 3, y + 3, 10, 10, fg);
        } else {
            for (int ry = 0; ry < 16; ++ry) {
                int row = (g[ry * 2] << 8) | g[ry * 2 + 1];
                for (int rx = 0; rx < 16; ++rx)
                    if (row & (1 << (15 - rx)))
                        px(fb, stride, x + rx, y + ry, fg);
            }
        }
        x += 16;
    }
    return x;
}

static bool screen_of(const PluginCfg *cfg, gfxScreen_t *scr)
{
    *scr = cfg->screen == SCR_TOP ? GFX_TOP : GFX_BOTTOM;
    return true;
}

/* 按 opacity 档位给背板颜色：0=最透明感（贴近纯黑），3=最实。 */
static Rgb back_color(int opacity)
{
    switch (opacity) {
    case 0:  return (Rgb){ 0, 0, 0 };
    case 1:  return (Rgb){ 12, 14, 18 };
    case 3:  return (Rgb){ 40, 46, 56 };
    default: return (Rgb){ 24, 28, 34 };
    }
}

/* 按钮条的尺寸：三个按钮平分面板宽，高 20px。
 * 20px 是 16px 字高 + 上下各 2px —— 再矮按钮里的字会顶到边框。 */
#define TB_H     20
#define TB_PAD   2
#define TB_GAP   4

static void btn_rect(const PluginCfg *cfg, int idx, int *bx, int *by, int *bw, int *bh)
{
    int x, y, w, h;
    int inner;

    cfg_panel_rect(cfg, cfg->screen == SCR_TOP ? 400 : 320, 240, &x, &y, &w, &h);
    inner = w - TB_PAD * 2 - TB_GAP * (BTN_COUNT - 1);
    *bw = inner / BTN_COUNT;
    *bx = x + TB_PAD + idx * (*bw + TB_GAP);
    *by = y + TB_PAD;
    *bh = TB_H;
}

int hud_button_at(const PluginCfg *cfg, int tx, int ty)
{
    for (int i = 0; i < BTN_COUNT; ++i) {
        int bx, by, bw, bh;

        btn_rect(cfg, i, &bx, &by, &bw, &bh);
        if (tx >= bx && tx < bx + bw && ty >= by && ty < by + bh)
            return i;
    }
    return -1;
}

static const char *btn_label(int i)
{
    switch (i) {
    case BTN_TRANSLATE: return "翻译";
    case BTN_SETTINGS:  return "设置";
    default:            return "说明";
    }
}

bool hud_draw(const PluginCfg *cfg, HudState st, const char *text)
{
    gfxScreen_t scr;
    u8  *fb;
    u16  sw, sh;
    int  x, y, w, h, stride;

    if (st == HUD_HIDDEN)
        return true;
    scr = cfg->screen == SCR_TOP ? GFX_TOP : GFX_BOTTOM;

    fb = gfxGetFramebuffer(scr, GFX_LEFT, &sw, &sh);
    if (!fb)
        return false;
    stride = sw * 3;

    cfg_panel_rect(cfg, sw, sh, &x, &y, &w, &h);
    fill(fb, stride, x, y, w, h, back_color(cfg->opacity));
    box(fb, stride, x, y, w, h, (Rgb){ 90, 200, 190 });

    /* ---- 按钮条 ---- */
    for (int i = 0; i < BTN_COUNT; ++i) {
        int bx, by, bw, bh;

        btn_rect(cfg, i, &bx, &by, &bw, &bh);
        fill(fb, stride, bx, by, bw, bh, (Rgb){ 34, 42, 52 });
        box(fb, stride, bx, by, bw, bh, (Rgb){ 70, 120, 130 });

        {
            /* 按钮文字居中：16px 一个字，按字数算宽度 */
            int  tw = 0;
            const char *lb = btn_label(i);

            for (const char *q = lb; *q; ) {
                const char *p2 = q;
                (void)utf8_next(&p2);
                q = p2;
                tw += 16;
            }
            draw_text(fb, stride, bx + (bw - tw) / 2, by + 2, bx + bw, lb,
                      (Rgb){ 220, 235, 240 });
        }
    }

    /* 按钮条与正文之间一条分隔线 */
    for (int i = 0; i < w - 4; ++i)
        px(fb, stride, x + 2 + i, y + TB_PAD + TB_H + 1, (Rgb){ 60, 100, 110 });

    {
        Rgb fg = (Rgb){ 235, 240, 245 };
        int tx = x + 6, ty = y + TB_PAD + TB_H + 6, maxx = x + w - 6;

        switch (st) {
        case HUD_CAPTURING:
            draw_text(fb, stride, tx, ty, maxx, "截图中…", fg);
            break;
        case HUD_WAITING:
            draw_text(fb, stride, tx, ty, maxx, "等 AI 回复…", fg);
            break;
        case HUD_ERROR:
            draw_text(fb, stride, tx, ty, maxx, text ? text : "出错了",
                      (Rgb){ 255, 150, 140 });
            break;
        case HUD_TEXT:
            /* 手动换行：16px 一行，超出就停（不做滚动，这块屏是"瞟一眼"用的） */
            {
                const char *p = text;

                while (p && *p && ty + 16 <= y + h - 4) {
                    const char *nl = strchr(p, '\n');
                    char   buf[256];
                    size_t n = nl ? (size_t)(nl - p) : strlen(p);

                    if (n >= sizeof buf) n = sizeof buf - 1;
                    memcpy(buf, p, n);
                    buf[n] = '\0';
                    draw_text(fb, stride, tx, ty, maxx, buf, fg);
                    ty += 18;
                    p = nl ? nl + 1 : NULL;
                }
            }
            break;
        default: break;
        }
    }

    gfxFlushBuffers();
    return true;
}

bool hud_grab(gfxScreen_t scr, u8 **out, int *w, int *h)
{
    u8  *fb;
    u16  sw, sh;
    size_t n;

    if (!out || !w || !h)
        return false;
    fb = gfxGetFramebuffer(scr, GFX_LEFT, &sw, &sh);
    if (!fb)
        return false;

    n = (size_t)sw * sh * 3;
    *out = malloc(n);
    if (!*out)
        return false;
    memcpy(*out, fb, n);
    *w = sw;
    *h = sh;

    /* ⚠️ 这里拿到的是**我们自己的**帧缓冲。游戏是否在同一块内存上画，
     *    未在真机确认 —— 抓屏之后要肉眼比对一次"抓到的画面是不是游戏画面"。
     *    真机上如果抓到黑屏，说明得改用 GSP 的 display capture。 */
    return true;
}

/* ---- 设置页原语 -------------------------------------------------------- */

static u8  *g_fb;
static int  g_stride;
static int  g_w, g_h;

bool hud_begin(gfxScreen_t scr, int *w, int *h)
{
    u16 sw, sh;

    g_fb = gfxGetFramebuffer(scr, GFX_LEFT, &sw, &sh);
    if (!g_fb)
        return false;
    g_stride = sw * 3;
    g_w = sw;
    g_h = sh;
    if (w) *w = sw;
    if (h) *h = sh;
    return true;
}

void hud_end(void)
{
    gfxFlushBuffers();
}

void hud_rect(int x, int y, int w, int h, int r, int g, int b)
{
    if (!g_fb) return;
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > g_w) w = g_w - x;
    if (y + h > g_h) h = g_h - y;
    if (w <= 0 || h <= 0) return;
    fill(g_fb, g_stride, x, y, w, h, (Rgb){ (u8)r, (u8)g, (u8)b });
}

void hud_frame(int x, int y, int w, int h, int r, int g, int b)
{
    if (!g_fb) return;
    box(g_fb, g_stride, x, y, w, h, (Rgb){ (u8)r, (u8)g, (u8)b });
}

void hud_text(int x, int y, int max_x, const char *s, int r, int g, int b)
{
    if (!g_fb) return;
    draw_text(g_fb, g_stride, x, y, max_x, s, (Rgb){ (u8)r, (u8)g, (u8)b });
}

int hud_text_w(const char *s)
{
    int n = 0;

    if (!s) return 0;
    while (*s) {
        const char *p = s;
        (void)utf8_next(&p);
        s = p;
        ++n;
    }
    return n * 16;
}
