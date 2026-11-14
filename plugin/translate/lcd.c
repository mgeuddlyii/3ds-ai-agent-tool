/*
 * lcd.c —— 见 lcd.h
 *
 * 全部算术都照抄 CTRComposer 的 render.inc.c（已发布可用的实现），
 * 不敢自己改顺序 —— 列主序/Y 翻转/物理镜像这三件事错一处就是花屏或崩机。
 */

#include "lcd.h"

/* 物理镜像：用户态要访问帧缓冲必须把 bit31 置 1 */
#define PA(p)   ((volatile u8 *)((p) | (1u << 31)))

static u32 reg_base(int screen)
{
    return screen ? LCD_BOT_BASE : LCD_TOP_BASE;
}

static void fill_info(int screen, LcdFb *out, u32 fb)
{
    u32 base = reg_base(screen);

    out->fb     = fb;
    out->fmt    = LCD_REG(base + LCD_FORMAT) & 7u;
    out->stride = LCD_REG(base + LCD_STRIDE);
    out->bpp    = (out->fmt == 0) ? 4u : (out->fmt == 1) ? 3u : 2u;
    out->w      = screen ? LCD_W_BOT : LCD_W_TOP;
    out->h      = screen ? LCD_H_BOT : LCD_H_TOP;
}

int lcd_visible(int screen, LcdFb *out)
{
    u32 base = reg_base(screen);
    u32 sel  = LCD_REG(base + LCD_SELECT) & 1u;

    fill_info(screen, out, LCD_REG(base + (sel ? LCD_FBA1 : LCD_FBA2)));
    return out->fb ? 0 : -1;
}

int lcd_hidden(int screen, LcdFb *out)
{
    u32 base = reg_base(screen);
    u32 sel  = LCD_REG(base + LCD_SELECT) & 1u;

    fill_info(screen, out, LCD_REG(base + (sel ? LCD_FBA2 : LCD_FBA1)));
    return out->fb ? 0 : -1;
}

static inline volatile u8 *px_addr(const LcdFb *f, int x, int y)
{
    u32 off = (u32)x * f->stride + (u32)(f->h - 1 - y) * f->bpp;

    return PA(f->fb + off);
}

static inline void put(volatile u8 *px, u32 fmt, u32 bpp, u8 r, u8 g, u8 b)
{
    if (bpp >= 3) {
        px[0] = b;                          /* BGR，不是 RGB */
        px[1] = g;
        px[2] = r;
        if (bpp == 4) px[3] = 0xFF;
        return;
    }

    {
        u16 v;

        if (fmt == 3)       /* RGB565 */
            v = (u16)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
        else if (fmt == 4)  /* RGBA4444 */
            v = (u16)(((r >> 4) << 12) | ((g >> 4) << 8) | ((b >> 4) << 4) | 0xF);
        else                /* 兜底按 RGB5551 */
            v = (u16)(((r >> 3) << 11) | ((g >> 3) << 6) | ((b >> 3) << 1) | 1);
        *(volatile u16 *)px = v;
    }
}

void lcd_px(const LcdFb *f, int x, int y, u32 rgb)
{
    if (!f->fb || x < 0 || y < 0 || x >= f->w || y >= f->h) return;
    put(px_addr(f, x, y), f->fmt, f->bpp,
        (u8)(rgb >> 16), (u8)(rgb >> 8), (u8)rgb);
}

void lcd_fill(const LcdFb *f, int x, int y, int w, int h, u32 rgb)
{
    int x1 = x + w, y1 = y + h;

    if (!f->fb) return;
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x1 > f->w) x1 = f->w;
    if (y1 > f->h) y1 = f->h;

    for (int cx = x; cx < x1; ++cx) {
        volatile u8 *p = px_addr(f, cx, y);

        /* 沿 y 走时偏移是 +bpp（因为 (h-1-y) 随 y 增大而减小 → 地址递减）；
         * 为省事直接用 px_addr 重算，编译器会消掉公共部分。 */
        for (int cy = y; cy < y1; ++cy)
            put(px_addr(f, cx, cy), f->fmt, f->bpp,
                (u8)(rgb >> 16), (u8)(rgb >> 8), (u8)rgb);
        (void)p;
    }
}

int lcd_grab(const LcdFb *f, int x, int y, int w, int h, u8 *dst)
{
    if (!f->fb) return -1;

    /* ★★ 2026-11-14 性能修正：**按列遍历**，不要按行。
     *
     * 帧缓冲是列主序：同一列的像素在内存里是连续的（间隔 bpp 字节），
     * 同一行的像素却隔着整整一行宽（下屏 720 字节）。
     * 原来写成"外层 cy、内层 cx"，每个像素都跳到另一条缓存线 ——
     * 320×240 就是 7.7 万次跨行访问，真机上表现为**卡死**（实测反馈）。
     *
     * 改成外层 cx、内层 cy 后，一趟只在同一列（720 字节）里走，
     * 几乎是顺序读，快一个数量级。 */
    for (int cx = 0; cx < w; ++cx) {
        int sx = x + cx;

        if (sx < 0 || sx >= f->w) {
            for (int cy = 0; cy < h; ++cy) {
                u8 *o = dst + ((size_t)cy * w + cx) * 3;
                o[0] = o[1] = o[2] = 0;
            }
            continue;
        }

        for (int cy = 0; cy < h; ++cy) {
            int sy = y + cy;
            u8 *o = dst + ((size_t)cy * w + cx) * 3;

            if (sy < 0 || sy >= f->h) {
                o[0] = o[1] = o[2] = 0;
                continue;
            }

            {
                volatile u8 *px = px_addr(f, sx, sy);

                if (f->bpp >= 3) {
                    o[2] = px[0];   /* B */
                    o[1] = px[1];   /* G */
                    o[0] = px[2];   /* R */
                } else {
                    u16 v = *(volatile u16 *)px;

                    /* 16 位色有三种排法，必须按 fmt 分开 —— 混着解会把
                     * 绿色通道解偏（这是最容易被误判成"花屏"的那种错）。 */
                    if (f->fmt == 4) {                 /* RGBA4444 */
                        o[0] = (u8)(((v >> 12) & 15) * 17);
                        o[1] = (u8)(((v >>  8) & 15) * 17);
                        o[2] = (u8)(((v >>  4) & 15) * 17);
                    } else if (f->fmt == 3) {          /* RGB565 */
                        o[0] = (u8)(((v >> 11) & 31) << 3);
                        o[1] = (u8)(((v >>  5) & 63) << 2);
                        o[2] = (u8)(( v        & 31) << 3);
                    } else {                           /* 兜底 RGB5551 */
                        o[0] = (u8)(((v >> 11) & 31) << 3);
                        o[1] = (u8)(((v >>  6) & 31) << 3);
                        o[2] = (u8)(((v >>  1) & 31) << 3);
                    }
                }
            }
        }
    }
    return 0;
}

void lcd_blit(const LcdFb *f, int x, int y, int w, int h,
              const u8 *src, int src_stride)
{
    if (!f->fb) return;

    for (int cy = 0; cy < h; ++cy) {
        int sy = y + cy;

        if (sy < 0 || sy >= f->h) continue;
        for (int cx = 0; cx < w; ++cx) {
            int sx = x + cx;
            const u8 *s = src + (size_t)cy * (size_t)src_stride + (size_t)cx * 3;

            if (sx < 0 || sx >= f->w) continue;
            put(px_addr(f, sx, sy), f->fmt, f->bpp, s[0], s[1], s[2]);
        }
    }
}
