/*
 * render.h -- 渲染抽象层接口
 *
 * 设计约束（见 设计规划.md §4.5）：
 *   1. 每一次绘制都收 Rect，Rect 里带 z。2D 后端**忽略** z；
 *      M11 的立体后端用 z 换算左右眼水平偏移。UI 代码永远不感知立体。
 *   2. 颜色统一是 0xRRGGBB，与帧缓冲的字节序无关。
 *      3DS 的帧缓冲是 BGR8，这个转换只在 render_2d.c 内部发生一次。
 *
 * 后端说明：render_2d.c 是**CPU 软件光栅化**，不是 citro2d。
 * 理由：我们是 1bpp 点阵字库，走 GPU 得先拼纹理会非常别扭；
 * 而软件光栅化对 1bpp 字形有天然优势（直接位运算 + 裁剪容易做到像素级精确），
 * 且顺带把 M11 的双眼渲染变成"多画一遍"。
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

/* --------------------------------------------------------------------------
 * 颜色
 * ----------------------------------------------------------------------- */

typedef uint32_t RColor;

#define R_RGB(r, g, b) \
    ((RColor)((((r) & 0xFFu) << 16) | (((g) & 0xFFu) << 8) | ((b) & 0xFFu)))
#define R_RED(c)   (((c) >> 16) & 0xFFu)
#define R_GREEN(c) (((c) >> 8) & 0xFFu)
#define R_BLUE(c)  ((c) & 0xFFu)

/* 把 fg 按 alpha(0..255) 混到白底(base) 上，得到一个可直接 r_rect 的实色。
 *
 * 给"要按灰度表现字形覆盖率、但手上只有一个实心矩形图元"的地方用
 * （例如 sc_lab 的字形放大图：它画的是放大后的方块，没有 alpha 通道）。
 *
 * ★ 混合公式与 render_2d.c 的 canvas_blend()、tools/mkfont.py 的 draw_cell()
 *   三处**必须一致**（铁律 6b）。不一致的话预览图与真机会有可见色差，
 *   于是"预览看着好"根本没验到东西。
 *   base 与 fg 都是 0xRRGGBB，alpha 越大越接近 fg。
 */
static inline RColor R_AlphaShade(RColor base, RColor fg, uint8_t a)
{
    if (a >= 255) return fg;
    if (a == 0)   return base;
    uint32_t inv = 255u - a;
    return R_RGB((R_RED(base)   * inv + R_RED(fg)   * a + 127u) / 255u,
                 (R_GREEN(base) * inv + R_GREEN(fg) * a + 127u) / 255u,
                 (R_BLUE(base)  * inv + R_BLUE(fg)  * a + 127u) / 255u);
}

/* --------------------------------------------------------------------------
 * 几何
 * ----------------------------------------------------------------------- */

/* 屏幕逻辑坐标下的一个矩形。z 为深度，2D 时恒 0。 */
typedef struct {
    float x, y, w, h;
    float z;
} Rect;

typedef enum {
    R_SCREEN_TOP = 0,
    R_SCREEN_BOTTOM,
    R_SCREEN_COUNT
} RScreenId;

/* 逻辑屏幕尺寸（注意不是帧缓冲的存储尺寸 —— 顶屏帧缓冲是横躺的） */
#define R_TOP_W 400
#define R_TOP_H 240
#define R_BOT_W 320
#define R_BOT_H 240

static inline Rect r_rect_make(float x, float y, float w, float h)
{
    Rect r;
    r.x = x; r.y = y; r.w = w; r.h = h; r.z = 0.0f;
    return r;
}

/* --------------------------------------------------------------------------
 * 生命周期
 * ----------------------------------------------------------------------- */

/* 初始化图形。成功返回 0。 */
int  r_init(void);
void r_exit(void);

/* 选择后续绘制落到哪块屏。 */
void r_target(RScreenId screen);
RScreenId r_current_target(void);

/* 开一帧：用 clear 清屏，并把裁剪栈重置为整屏。 */
void r_begin(RColor clear);

/* 收一帧：提交并交换缓冲。 */
void r_end(void);

/* ★★ 「换了缓冲却没画」守卫的读数。
 *
 * r_end() 里的 gfxSwapBuffers() 是**全局**的（翻转所有屏幕的缓冲索引），
 * 所以"只画一块屏就 r_end()"会让另一块屏在两次旧内容之间来回翻
 * —— 表现就是用户看到的"屏一直在闪"。这种缺陷**截图抓不到**（时序问题），
 * 只能让运行时自己记：交换时某块屏"曾经用过、本轮却没画"就记一笔。
 *
 * 健康值恒为 0。报进 status.txt，与 layout_overflow 并列。
 * 上/下屏分开计数是为了让人一眼知道是**哪块**屏在闪。 */
int  r_swap_undrawn_count(void);
int  r_swap_undrawn_top_count(void);
int  r_swap_undrawn_bot_count(void);
void r_swap_undrawn_reset(void);

/* --------------------------------------------------------------------------
 * 裁剪
 * ----------------------------------------------------------------------- */

void r_clip_push(const Rect *r);
void r_clip_pop(void);
void r_clip_reset(void);

/* 整体平移。给转场动画用：把整屏画在偏移位置上（超出屏幕的部分由裁剪吃掉）。
 *
 * ⚠️ **r_begin() 会把它清回 (0,0)** —— 这是刻意的保护：
 * r_begin 要清一整个屏幕，如果那时偏移还留着，清屏就会跟着偏，
 * 边角上会留下上一帧的残影，而那种"没清干净"在深色主题下很难看出来。
 * 所以用法固定是：r_begin() 先清干净 → 再 r_set_offset() → 画 → 归零。
 */
void r_set_offset(float dx, float dy);
void r_get_offset(float *dx, float *dy);

/* --------------------------------------------------------------------------
 * 图元
 * ----------------------------------------------------------------------- */

void r_pixel(float x, float y, RColor c);
void r_rect(const Rect *r, RColor c);
void r_frame_rect(const Rect *r, RColor c, float thickness);
void r_hline(float x, float y, float w, RColor c);
void r_vline(float x, float y, float h, RColor c);

/* --------------------------------------------------------------------------
 * 文本
 * ----------------------------------------------------------------------- */

/* 在 r 的左上角作为笔位起点，画一行不折行的 UTF-8。超出 r.w 的部分裁掉。 */
void r_text(const Rect *r, const char *utf8, RColor c);

/* 画单个字形（字库巡检器用）。x/y 是 16x16 格的左上角。 */
void r_glyph(float x, float y, uint32_t codepoint, RColor c);

/* --------------------------------------------------------------------------
 * 截图支撑
 * ----------------------------------------------------------------------- */

/* 按**逻辑坐标**取像素，返回 0xRRGGBB。供截图/调试用。 */
RColor r_get_pixel(int x, int y);

/* 当前 target 的逻辑尺寸 */
void r_screen_size(int *w, int *h);
