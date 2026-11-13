/*
 * lcd.h —— 直接操作 LCD 寄存器画像素（**不使用 GSP**）
 *
 * ── 为什么不能用 libctru 的 gfx ──
 * 2026-11-13 真机实测：插件里调 `gspInit()` **不返回**（痕迹停在 R9 之前）。
 * 原因是 `gsp::Gpu` 单会话独占，游戏进程已经握着，同进程再申请 → srv 无限等待。
 * 所以 `gfxInitDefault()` / `gfxGetFramebuffer()` 这一整套在插件里作废。
 *
 * ── 用的是什么办法 ──
 * 直接读 LCD 控制器寄存器拿帧缓冲的**物理地址**，然后按物理镜像写像素。
 * 寄存器布局（上屏基址 0x10400400，下屏 0x10400500）：
 *   +0x68  FBA1      帧缓冲 A1 的物理地址
 *   +0x6C  FBA2      帧缓冲 A2 的物理地址
 *   +0x70  FORMAT    0=RGBA8(4B) 1=RGB8(3B) 3=RGB565(2B) 4=RGBA4444(2B)
 *   +0x78  SELECT    bit0 = 当前扫描输出哪个缓冲
 *   +0x90  STRIDE    每"列"字节数
 *
 * ── 三个必须记住的坑 ──
 * 1. 帧缓冲是**列主序**（转置）且 **Y 轴翻转**：偏移 = x*stride + (h-1-y)*bpp。
 * 2. 物理地址要 `| (1u<<31)` 才能从用户态访问（物理镜像）。
 * 3. 字节序是 **BGR**，不是 RGB。
 *
 * 出处：CTRComposer 的 Sources/engine/render.inc.c（已发布可用），
 *       本文件只保留我们需要的最小集合。
 */

#ifndef LCD_H
#define LCD_H

#include <3ds.h>

#define LCD_TOP_BASE  0x10400400u
#define LCD_BOT_BASE  0x10400500u

#define LCD_FBA1      0x68u
#define LCD_FBA2      0x6Cu
#define LCD_FORMAT    0x70u
#define LCD_SELECT    0x78u
#define LCD_STRIDE    0x90u

#define LCD_W_TOP     400
#define LCD_H_TOP     240
#define LCD_W_BOT     320
#define LCD_H_BOT     240

/* ★★ 2026-11-14 真机教训：**读寄存器也必须走物理镜像**。
 *   直接 `*(volatile u32*)0x10400578` 会 data abort —— 真机转储
 *   `FAR=10400578 / Access type: Read / PC=070005BC`，就是裸读 SELECT 那一下。
 *   0x10400000 段是内核态 MMIO，普通进程没有映射；但 ARM11 上把 bit31 置 1
 *   就是"按物理地址直取"，这条路是通的。
 *   出处：CTRComposer 的 Includes/common.h
 *       #define PA_PTR(addr) (void *)((u32)(addr) | 1 << 31)
 *       #define REG32(addr)  (*(vu32 *)(PA_PTR(addr)))
 *   注意：这个宏只能用于**寄存器/物理地址**，不能用于普通变量的地址。 */
#define LCD_PA(a)     ((u32)(a) | (1u << 31))
#define LCD_REG(a)    (*(volatile u32 *)(LCD_PA(a)))

/* 按键：同样绕开 hidInit()（插件里绝不能调，N3DS 上会拉 irrst 冻游戏）。
 * 0x10146000 是 HID 的按键寄存器，低 12 位**取反**才是"按下"。
 * 位：A=0 B=1 SELECT=2 START=3 →=4 ←=5 ↑=6 ↓=7 R=8 L=9 X=10 Y=11 */
#define HID_REG_PAD   0x10146000u
#define HID_PAD       (LCD_REG(HID_REG_PAD) ^ 0xFFFu)

typedef struct {
    u32 fb;       /* 帧缓冲物理地址（0 = 取不到，别画） */
    u32 stride;   /* 每列字节数 */
    u32 bpp;      /* 每像素字节数：2/3/4 */
    u32 fmt;
    int w, h;
} LcdFb;

/* 取得某块屏**当前正在显示**的帧缓冲。screen: 0=上屏, 1=下屏。返回 0 成功。 */
int  lcd_visible(int screen, LcdFb *out);

/* 取得某块屏**隐藏**的那个帧缓冲（双缓冲时另一边）。返回 0 成功。 */
int  lcd_hidden(int screen, LcdFb *out);

/* 写一个像素。走物理镜像，速度不快（全屏约 18ms），所以只适合小面积重刷。 */
void lcd_px(const LcdFb *f, int x, int y, u32 rgb);

/* 填充矩形（x,y 为屏幕坐标，原点左上，宽 w 高 h 已按屏裁剪）。 */
void lcd_fill(const LcdFb *f, int x, int y, int w, int h, u32 rgb);

/* 从可见帧缓冲把一块区域拷进调用者的缓冲（row-major RGB888，3 字节/像素）。
 * 用于"游戏每帧会擦掉我们的浮层"时先备份原画面。返回 0 成功。 */
int  lcd_grab(const LcdFb *f, int x, int y, int w, int h, u8 *dst);

/* 把调用者的 RGB888 缓冲贴回可见帧缓冲。
 * src_stride = 源缓冲每行的字节数（通常是 src_w*3；只贴子矩形时必须传整行宽）。 */
void lcd_blit(const LcdFb *f, int x, int y, int w, int h,
              const u8 *src, int src_stride);

#endif
