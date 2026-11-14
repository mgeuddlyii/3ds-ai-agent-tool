/*
 * hud.c -- 见 hud.h
 */

#include "hud.h"
#include "font16.h"
#include "lcd.h"

#include <stdlib.h>
#include <string.h>

/* ★★ 2026-11-14 后台大改：不再往 gfx 帧缓冲画
 *
 * 病根：插件里 `gspInit()` 会永久阻塞（gsp::Gpu 单会话独占，游戏握着），
 * 所以 `gfxInitDefault()` / `gfxGetFramebuffer()` / `gfxFlushBuffers()` 全废。
 *
 * 现在的路子（照 CTRComposer）：
 *   所有绘制先落到 RAM 里一块 row-major RGB888 的"合成缓冲"（就是原来的画法，
 *   一行代码都不用改），画完再**一次性**贴到 LCD 的真帧缓冲。
 *   为什么不逐像素直写 LCD：那块走物理镜像，很慢（全屏约 18ms）；
 *   合成缓冲在普通内存里，最后一次 blit 只付一次代价。
 */
static u8 g_compose[(size_t)LCD_W_TOP * LCD_H_TOP * 3];   /* 288KB，bss */

/* 当前绘制目标（= g_compose）与目标屏的 LCD 帧缓冲信息。
 * 声明放这里是因为 hud_draw 在 hud_begin 之前就要用它们。 */
static u8    *g_fb;
static int    g_stride;
static int    g_w, g_h;
static LcdFb  g_lfb;

/* 当前高亮的按钮（键盘 ←/→ 选择时用；-1 = 不高亮） */
static int    g_btn_sel = 0;

void hud_set_sel(int i)
{
    g_btn_sel = i;
}

/* 3DS 帧缓冲是 B,G,R。这里统一按 r,g,b 传，省得每个调用点记顺序。 */
typedef struct { u8 r, g, b; } Rgb;

/* 脏矩形（含边界）。只把"这一帧真的画过的区域"贴回 LCD ——
 * 整屏贴会把合成缓冲里的上一帧残留糊到游戏画面上（合成缓冲不备份游戏内容）。 */
static int g_dx0 = 0, g_dy0 = 0, g_dx1 = -1, g_dy1 = -1;

static void px(u8 *fb, int stride, int x, int y, Rgb c)
{
    u8 *p = fb + (size_t)y * stride + (size_t)x * 3;

    p[0] = c.b;
    p[1] = c.g;
    p[2] = c.r;

    if (x < g_dx0) g_dx0 = x;
    if (y < g_dy0) g_dy0 = y;
    if (x > g_dx1) g_dx1 = x;
    if (y > g_dy1) g_dy1 = y;
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
    case BTN_HELP:      return "说明";
    default:            return "测试";
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

    /* ★ 改走合成缓冲：先 hud_begin 拿 LCD 帧缓冲信息并把 g_fb 指到 RAM 缓冲，
     *   画完后由 hud_end() 一次性贴上去。 */
    {
        int cw = 0, chh = 0;

        if (!hud_begin(scr, &cw, &chh))
            return false;
        fb     = g_fb;
        sw     = (u16)cw;
        sh     = (u16)chh;
        stride = g_stride;
    }

    cfg_panel_rect(cfg, sw, sh, &x, &y, &w, &h);
    fill(fb, stride, x, y, w, h, back_color(cfg->opacity));
    box(fb, stride, x, y, w, h, (Rgb){ 90, 200, 190 });

    /* ★★ 2026-11-14：**不再画按钮条**。
     *
     * 用户明确要求：插件只负责"显示文字"，所有设置（显示屏/位置/宽高/深浅/
     * 触发键/说明/安装教程/启用开关）都搬到 AIConsole 的「更多 → 插件」页里。
     *
     * 原来这里有四个按钮（翻译/设置/说明/测试）。删掉的直接后果是：
     * 面板里再也进不去设置页与说明页 —— 这是**要的效果**，不是功能缺失。
     * 触发方式也随之一并简化：按快捷键就截图翻译并弹出面板，
     * 再按一次或按 B 收起。
     *
     * ⚠️ BTN_* 那套枚举和 btn_rect() 先留着没删：sc_plugins 那边还要用
     *    btn_rect 算行框，删了会牵连 cfg.c。等界面稳定下来再一起清理。 */

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

    hud_end();          /* ★ 合成缓冲 → LCD 帧缓冲，一次性贴 */
    return true;
}

bool hud_grab(gfxScreen_t scr, u8 **out, int *w, int *h)
{
    LcdFb f;
    size_t n;

    if (!out || !w || !h)
        return false;

    /* ★ 从 LCD 的**可见帧缓冲**直接读（那正是游戏正在显示的画面），
     *   不走 GSP，也不需要 gfxInit。抓出来的像素顺序是 R,G,B 逐字节。 */
    if (lcd_visible(scr == GFX_TOP ? 0 : 1, &f) != 0)
        return false;

    n = (size_t)f.w * f.h * 3;
    *out = malloc(n);
    if (!*out)
        return false;

    if (lcd_grab(&f, 0, 0, f.w, f.h, *out) != 0) {
        free(*out);
        *out = NULL;
        return false;
    }

    *w = f.w;
    *h = f.h;
    return true;
}

/* ---- 设置页原语 -------------------------------------------------------- */

bool hud_begin(gfxScreen_t scr, int *w, int *h)
{
    /* 拿 LCD 的可见帧缓冲。失败就直接返回 —— 宁可这一帧不画，
     * 也不要往一个无效地址写像素（那会 data abort 把游戏打崩）。 */
    if (lcd_visible(scr == GFX_TOP ? 0 : 1, &g_lfb) != 0) {
        g_fb = NULL;
        return false;
    }

    g_w      = g_lfb.w;
    g_h      = g_lfb.h;
    g_stride = g_w * 3;
    g_fb     = g_compose;      /* ★ 画进 RAM，不直写 LCD */

    /* 每帧重置脏矩形：hud_end 只贴"这一帧画过的"那块 */
    g_dx0 = g_w; g_dy0 = g_h; g_dx1 = -1; g_dy1 = -1;

    if (w) *w = g_w;
    if (h) *h = g_h;
    return true;
}

void hud_end(void)
{
    int x0, y0, x1, y1;

    if (!g_fb)
        return;
    if (g_dx1 < g_dx0 || g_dy1 < g_dy0)
        return;                     /* 这一帧什么都没画 */

    x0 = g_dx0 < 0 ? 0 : g_dx0;
    y0 = g_dy0 < 0 ? 0 : g_dy0;
    x1 = g_dx1 >= g_w ? g_w - 1 : g_dx1;
    y1 = g_dy1 >= g_h ? g_h - 1 : g_dy1;
    if (x1 < x0 || y1 < y0)
        return;

    /* 一次性把"画过的那块"贴到 LCD 可见帧缓冲。
     * lcd_blit 内部已处理列主序 / Y 翻转 / BGR / 物理镜像四件事。 */
    lcd_blit(&g_lfb, x0, y0, x1 - x0 + 1, y1 - y0 + 1,
             g_compose + (size_t)y0 * g_stride + (size_t)x0 * 3, g_stride);
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
