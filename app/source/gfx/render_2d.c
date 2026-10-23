/*
 * render_2d.c -- 2D 渲染后端（CPU 软件光栅化，直接写帧缓冲）
 *
 * 为什么不用 citro2d
 * ------------------
 * 我们的字库是 1bpp 点阵。走 GPU 就必须先把字形拼成纹理，
 * 7000+ 字形拼图集很别扭；而 CPU 位运算天然适合 1bpp —— 裁剪精确到像素、
 * 不需要纹理上传，M11 的双眼渲染也只是"同一条绘制流程跑两遍"。
 *
 * 帧缓冲的内存布局（这块最容易踩坑，已用 devkitPro 官方示例核实）
 * ------------------------------------------------------------------
 *   底屏：gfxGetFramebuffer 返回 320x240，**就是显示序**，
 *         官方 graphics/bitmap/24bit-color 示例直接把 320x240 的图 memcpy 进去。
 *         offset = y * 320 + x
 *   顶屏：返回 240x400 —— 行宽 240、共 400 行，是一张**横躺**的图，
 *         显示时转 90° 才正。所以屏幕 x 选的是"行"，屏幕 y 选的是"列"。
 *         offset = x * 240 + (239 - y)
 *   判据：sw < sh 就是横躺的（240 < 400）。这个规则对两块屏都成立，
 *   所以下面用同一个函数处理，不写死屏幕号。
 */
#include <3ds.h>
#include <string.h>

#include "render.h"
#include "font.h"

/* --------------------------------------------------------------------------
 * 画布
 * ----------------------------------------------------------------------- */

typedef struct {
    uint8_t *fb;      /* 帧缓冲基址（BGR8） */
    int      sw, sh;  /* 存储尺寸（像素） */
    int      lw, lh;  /* 逻辑尺寸（屏幕看到的） */
    int      stride;  /* 存储每行的像素数 = sw */
    bool     rotated; /* true = 横躺存储 */
} RCanvas;

static RCanvas   g_cv[R_SCREEN_COUNT];

/* 整体平移（转场动画用）。只有这三个落笔点认它：
 * r_pixel / r_rect / r_glyph —— 其余图元都是它们的组合，
 * 所以这里加一次，全渲染器的图元就都跟着平移了。 */
static float     g_off_x, g_off_y;
static RScreenId g_cur = R_SCREEN_TOP;

/* 裁剪：半开区间 [x0,x1) x [y0,y1) */
typedef struct { int x0, y0, x1, y1; } ClipBox;

#define CLIP_MAX 8
static ClipBox g_clip[CLIP_MAX];
static int     g_clip_n;

static inline int ifloorf(float v) { return (int)v; }
static inline int iceilf(float v)  { int i = (int)v; return (v > (float)i) ? i + 1 : i; }

static void canvas_init(RCanvas *c, uint8_t *fb, int sw, int sh)
{
    c->fb      = fb;
    c->sw      = sw;
    c->sh      = sh;
    c->stride  = sw;
    c->rotated = (sw < sh);
    if (c->rotated) {          /* 顶屏：横躺 */
        c->lw = sh;
        c->lh = sw;
    } else {                   /* 底屏：显示序 */
        c->lw = sw;
        c->lh = sh;
    }
}

static inline uint32_t canvas_index(const RCanvas *c, int x, int y)
{
    if (c->rotated)
        return (uint32_t)x * (uint32_t)c->stride
             + (uint32_t)(c->stride - 1 - y);
    return (uint32_t)y * (uint32_t)c->stride + (uint32_t)x;
}

static inline void canvas_put(const RCanvas *c, int x, int y, RColor col)
{
    uint8_t *p = c->fb + (size_t)canvas_index(c, x, y) * 3u;
    /* GSP_BGR8_OES：低地址是蓝 */
    p[0] = (uint8_t)R_BLUE(col);
    p[1] = (uint8_t)R_GREEN(col);
    p[2] = (uint8_t)R_RED(col);
}

/* 按 alpha(0..255) 把 col 混到底色上。**抗锯齿字形就靠它出灰阶。**
 *
 * ★ 公式与 tools/mkfont.py 的 draw_cell() **逐位对应**
 *   （铁律 6b：复刻实现的公式必须与被测代码同源，改一处必须改两处；
 *     两边不一致的话，预览图就跟真机对不上，"看着对"其实没验到东西）
 *
 *   统一成 (dst*(255-a) + src*a + 127) / 255：
 *     - a=255 退化成覆盖写，与旧的 canvas_put 完全一致（1bpp 路径不受影响）
 *     - +127 是四舍五入，不加会系统性偏暗半格
 *   用整数移位而非除法：ARM 上 /255 会退化成 __aeabi_idiv 调库，很慢。
 */
static inline void canvas_blend(const RCanvas *c, int x, int y,
                                RColor col, uint8_t a)
{
    if (a == 0)  return;
    if (a == 255) { canvas_put(c, x, y, col); return; }

    uint8_t *p = c->fb + (size_t)canvas_index(c, x, y) * 3u;
    const uint8_t src[3] = { (uint8_t)R_BLUE(col),
                             (uint8_t)R_GREEN(col),
                             (uint8_t)R_RED(col) };
    const uint8_t inv = (uint8_t)(255u - a);
    p[0] = (uint8_t)((p[0] * inv + src[0] * a + 127) / 255);
    p[1] = (uint8_t)((p[1] * inv + src[1] * a + 127) / 255);
    p[2] = (uint8_t)((p[2] * inv + src[2] * a + 127) / 255);
}

static inline bool clip_contains(int x, int y)
{
    /* ★ g_clip_n 为 0 时下面那行是 g_clip[-1]（B-11）。
     *   "先 r_begin 再画"只是调用约定，不是保证 —— r_begin 之前任何一次绘制
     *   都会读到栈外的一个 ClipBox，裁剪框变成不可控的值：
     *   要么放行越界坐标（写坏帧缓冲），要么整片画不出来。
     *   没有裁剪栈时按"全放行"处理：那是 r_begin 之前的语义。 */
    if (g_clip_n <= 0)
        return true;
    const ClipBox *b = &g_clip[g_clip_n - 1];
    return x >= b->x0 && x < b->x1 && y >= b->y0 && y < b->y1;
}

/* --------------------------------------------------------------------------
 * 生命周期
 * ----------------------------------------------------------------------- */

static void acquire(RScreenId s)
{
    u16 w = 0, h = 0;
    uint8_t *fb = gfxGetFramebuffer(
        (s == R_SCREEN_TOP) ? GFX_TOP : GFX_BOTTOM, GFX_LEFT, &w, &h);
    canvas_init(&g_cv[s], fb, (int)w, (int)h);
}

/* --------------------------------------------------------------------------
 * ★★ 「换了缓冲却没画」的守卫
 *
 * 病根：r_end() 里的 gfxSwapBuffers() 是**全局**的 —— 它翻转**所有**屏幕的
 * 缓冲索引，不是"当前 target"那一块。所以只要"只画一块屏就 r_end()"，
 * 另一块屏的索引照样翻、却没有新内容写进去，那块屏就在
 * **最近两次画过的内容**之间来回翻 —— 用户看到的是"屏一直在闪"。
 *
 * 这类缺陷**截图抓不到**（它是时序的，单帧看完全正常），
 * 读代码也容易漏（两个 r_begin 各自都合法）。所以只能让运行时自己记：
 * 每次 r_end() 检查"自上次 r_end 以来，这块屏画过没有"，
 * 没画过就记一笔。判据是 **swap_undrawn == 0**，报进 status.txt。
 *
 * ⚠️ 为什么要 s_ever：有些屏（屏对象）本来就没有底屏回调
 *   （app_render 里是 `if (s->render_bottom)`），那块屏是**故意**不画的。
 *   不做这个区分的话，守卫会把"设计如此"报成缺陷 → 常驻红 → 脱敏。
 *   所以只统计"**曾经**被画过"的屏：它一旦被用过，此后每次交换都该有新内容。
 * ----------------------------------------------------------------------- */

static bool s_ever[R_SCREEN_COUNT];    /* 这块屏自 r_init 起被画过 */
static bool s_frame[R_SCREEN_COUNT];   /* 本轮（两次 r_end 之间）画过 */
static int  s_swap_undrawn;            /* 交换时"用过却没画"的次数 */
static int  s_swap_undrawn_top;        /* 其中上屏/底屏各占多少，便于定位 */
static int  s_swap_undrawn_bot;

static void swap_guard_note(int screen)
{
    ++s_swap_undrawn;
    if (screen == R_SCREEN_TOP) ++s_swap_undrawn_top;
    else                        ++s_swap_undrawn_bot;
}

int  r_swap_undrawn_count(void) { return s_swap_undrawn; }
int  r_swap_undrawn_top_count(void) { return s_swap_undrawn_top; }
int  r_swap_undrawn_bot_count(void) { return s_swap_undrawn_bot; }
void r_swap_undrawn_reset(void)
{
    s_swap_undrawn = s_swap_undrawn_top = s_swap_undrawn_bot = 0;
}

int r_init(void)
{
    gfxInitDefault();

    /* 双缓冲 + 垂直同步（r_end 里等）。
     * 曾经用单缓冲：写入的是正在被扫描的那块缓冲，模拟器采到
     * 「清完屏还没画完」的空帧就是用户看到的闪屏。
     * 注释里的「静态 UI 只在变化时重画」并不成立 —— 主循环每帧都在 app_render，
     * 双缓冲零额外成本，换来的是每个呈现出去的帧都是画完的。 */
    gfxSetScreenFormat(GFX_TOP, GSP_BGR8_OES);
    gfxSetScreenFormat(GFX_BOTTOM, GSP_BGR8_OES);

    /* 这里只是为了拿到存储尺寸；每帧 r_begin 会重新取一次 */
    acquire(R_SCREEN_TOP);
    acquire(R_SCREEN_BOTTOM);

    if (g_cv[R_SCREEN_TOP].lw != R_TOP_W || g_cv[R_SCREEN_TOP].lh != R_TOP_H)
        return -1;   /* 布局假设被推翻，早点炸掉比画歪强 */
    if (g_cv[R_SCREEN_BOTTOM].lw != R_BOT_W || g_cv[R_SCREEN_BOTTOM].lh != R_BOT_H)
        return -2;

    g_cur = R_SCREEN_TOP;
    return 0;
}

void r_exit(void)
{
    gfxExit();
}

void r_target(RScreenId screen)
{
    if (screen < R_SCREEN_COUNT)
        g_cur = screen;
}

RScreenId r_current_target(void)
{
    return g_cur;
}

void r_begin(RColor clear)
{
    acquire(g_cur);

    /* 守卫用：这块屏本轮画过了（见上面 s_ever 那段说明） */
    s_frame[g_cur] = true;
    s_ever[g_cur]  = true;

    /* 清屏前**必须**把平移归零，否则清屏会跟着偏，边角残影。
     * 见 render.h 里 r_set_offset 的说明。 */
    g_off_x = 0.0f;
    g_off_y = 0.0f;

    g_clip_n = 1;
    g_clip[0].x0 = 0;
    g_clip[0].y0 = 0;
    g_clip[0].x1 = g_cv[g_cur].lw;
    g_clip[0].y1 = g_cv[g_cur].lh;
    r_rect(&(Rect){ 0, 0, (float)g_cv[g_cur].lw, (float)g_cv[g_cur].lh, 0 }, clear);
}

void r_set_offset(float dx, float dy)
{
    g_off_x = dx;
    g_off_y = dy;
}

void r_get_offset(float *dx, float *dy)
{
    if (dx) *dx = g_off_x;
    if (dy) *dy = g_off_y;
}

void r_end(void)
{
    /* ★ 守卫：交换之前先看"这块屏本轮画过没有"。见文件上方那段说明。 */
    for (int i = 0; i < R_SCREEN_COUNT; ++i) {
        if (s_ever[i] && !s_frame[i])
            swap_guard_note(i);
        s_frame[i] = false;
    }

    gfxFlushBuffers();
    gfxSwapBuffers();
    /* 等垂直同步：把循环锁在 60fps，也保证交换发生在消隐期。
     * 没有这一行主循环全速狂奔，是闪屏的另一半原因。 */
    gspWaitForVBlank();
}

void r_screen_size(int *w, int *h)
{
    if (w) *w = g_cv[g_cur].lw;
    if (h) *h = g_cv[g_cur].lh;
}

/* --------------------------------------------------------------------------
 * 裁剪
 * ----------------------------------------------------------------------- */

void r_clip_reset(void)
{
    g_clip_n = 1;
    g_clip[0].x0 = 0;
    g_clip[0].y0 = 0;
    g_clip[0].x1 = g_cv[g_cur].lw;
    g_clip[0].y1 = g_cv[g_cur].lh;
}

void r_clip_push(const Rect *r)
{
    if (g_clip_n >= CLIP_MAX)
        return;
    ClipBox nb;
    nb.x0 = ifloorf(r->x);
    nb.y0 = ifloorf(r->y);
    nb.x1 = iceilf(r->x + r->w);
    nb.y1 = iceilf(r->y + r->h);

    const ClipBox *p = &g_clip[g_clip_n - 1];
    if (nb.x0 < p->x0) nb.x0 = p->x0;
    if (nb.y0 < p->y0) nb.y0 = p->y0;
    if (nb.x1 > p->x1) nb.x1 = p->x1;
    if (nb.y1 > p->y1) nb.y1 = p->y1;
    if (nb.x1 < nb.x0) nb.x1 = nb.x0;
    if (nb.y1 < nb.y0) nb.y1 = nb.y0;

    g_clip[g_clip_n++] = nb;
}

void r_clip_pop(void)
{
    if (g_clip_n > 1)
        g_clip_n--;
}

/* --------------------------------------------------------------------------
 * 图元
 * ----------------------------------------------------------------------- */

void r_pixel(float x, float y, RColor c)
{
    int ix = ifloorf(x + g_off_x), iy = ifloorf(y + g_off_y);
    if (clip_contains(ix, iy))
        canvas_put(&g_cv[g_cur], ix, iy, c);
}

void r_rect(const Rect *r, RColor c)
{
    const ClipBox *b = &g_clip[g_clip_n - 1];
    /* 平移在**取整之前**加上：先取整再加会让半像素的偏移被吃掉，
     * 动画期间表现为"前几帧纹丝不动、然后突然跳一下"。 */
    int x0 = ifloorf(r->x + g_off_x), y0 = ifloorf(r->y + g_off_y);
    int x1 = iceilf(r->x + r->w + g_off_x), y1 = iceilf(r->y + r->h + g_off_y);

    if (x0 < b->x0) x0 = b->x0;
    if (y0 < b->y0) y0 = b->y0;
    if (x1 > b->x1) x1 = b->x1;
    if (y1 > b->y1) y1 = b->y1;
    if (x0 >= x1 || y0 >= y1)
        return;

    /* 逐像素那版是 canvas_put(cv, x, y, c)，每个像素都要算一次
     * canvas_index —— 里面有一次分支（rotated?）和一次乘法。
     * 一次全屏清屏就是 400×240 = 96,000 次；而清屏是**每帧最大的一笔固定开销**。
     *
     * 这里把索引算术提到内层循环之外，改成指针递进。**逐像素等价**：
     * 两个方向都只是把"每步重算索引"换成"每步 +3 字节"。
     *
     * 两个方向必须分开写，因为连续的那根轴不一样（这是本项目最容易搞反的地方）：
     *   · 顶屏是旋转画布：index = x*stride + (stride-1-y)
     *     → 固定 x、y 递增时 index **递减**；所以沿**一列**走（从 y1-1 往上）
     *       才是地址递增的，指针可以一直 +3。
     *   · 底屏是非旋转：index = y*stride + x → 沿**一行**走就是连续递增。
     * 搞反了不会崩，只是写错位置（画面斜着花），所以这里两边都写清楚。 */
    {
        const RCanvas *cv = &g_cv[g_cur];
        uint8_t b = (uint8_t)R_BLUE(c);
        uint8_t g = (uint8_t)R_GREEN(c);
        uint8_t r = (uint8_t)R_RED(c);

        if (cv->rotated) {
            uint32_t base = (uint32_t)cv->stride - (uint32_t)y1;
            for (int x = x0; x < x1; ++x) {
                uint8_t *p = cv->fb +
                             ((size_t)x * (size_t)cv->stride + base) * 3u;
                for (int y = y1 - 1; y >= y0; --y) {
                    p[0] = b; p[1] = g; p[2] = r;
                    p += 3;
                }
            }
        } else {
            for (int y = y0; y < y1; ++y) {
                uint8_t *p = cv->fb +
                             ((size_t)y * (size_t)cv->stride + (size_t)x0) * 3u;
                for (int x = x0; x < x1; ++x) {
                    p[0] = b; p[1] = g; p[2] = r;
                    p += 3;
                }
            }
        }
    }
}

void r_hline(float x, float y, float w, RColor c)
{
    r_rect(&(Rect){ x, y, w, 1.0f, 0 }, c);
}

void r_vline(float x, float y, float h, RColor c)
{
    r_rect(&(Rect){ x, y, 1.0f, h, 0 }, c);
}

void r_frame_rect(const Rect *r, RColor c, float t)
{
    if (t <= 0.0f) return;
    r_rect(&(Rect){ r->x, r->y, r->w, t, 0 }, c);                     /* 上 */
    r_rect(&(Rect){ r->x, r->y + r->h - t, r->w, t, 0 }, c);          /* 下 */
    r_rect(&(Rect){ r->x, r->y + t, t, r->h - 2 * t, 0 }, c);         /* 左 */
    r_rect(&(Rect){ r->x + r->w - t, r->y + t, t, r->h - 2 * t, 0 }, c); /* 右 */
}

/* --------------------------------------------------------------------------
 * 字形
 * ----------------------------------------------------------------------- */

/* 字形按行展开用的栈缓冲。cell_w 目前恒为 16；
 * 这里留到 64 是为了将来加宽字号时不必回头改这个常量。
 * 超过 64 就退回逐像素路径（慢但正确），不会越界写栈。 */
#define R_GLYPH_ROW_MAX 64

void r_glyph(float x, float y, uint32_t codepoint, RColor c)
{
    const RGlyph *g = font_lookup(codepoint);
    if (!g || !g->bits)
        return;

    const RCanvas *cv = &g_cv[g_cur];
    const ClipBox *b = &g_clip[g_clip_n - 1];

    int ox = ifloorf(x + g_off_x), oy = ifloorf(y + g_off_y);
    int cw = font_cell_w(), ch = font_cell_h();

    if (cw <= R_GLYPH_ROW_MAX) {
        uint8_t row[R_GLYPH_ROW_MAX];
        for (int gy = 0; gy < ch; ++gy) {
            int sy = oy + gy;
            if (sy < b->y0 || sy >= b->y1)
                continue;                  /* 整行在裁剪区外，跳过 */

            /* 整行为空直接跳过 —— 与旧 1bpp 实现的快速路径等价，
             * 空白行（上下留白、标点周围）不必逐像素判。 */
            if (!font_glyph_alpha_row(g, gy, row))
                continue;

            for (int gx = 0; gx < cw; ++gx) {
                uint8_t a = row[gx];
                if (a == 0)
                    continue;
                int sx = ox + gx;
                if (sx >= b->x0 && sx < b->x1)
                    canvas_blend(cv, sx, sy, c, a);
            }
        }
        return;
    }

    /* 兜底：超宽字库（本工程不会走到）逐像素，慢但正确。 */
    for (int gy = 0; gy < ch; ++gy) {
        int sy = oy + gy;
        if (sy < b->y0 || sy >= b->y1)
            continue;
        for (int gx = 0; gx < cw; ++gx) {
            uint8_t a = font_glyph_alpha(g, gx, gy);
            if (a == 0)
                continue;
            int sx = ox + gx;
            if (sx >= b->x0 && sx < b->x1)
                canvas_blend(cv, sx, sy, c, a);
        }
    }
}

/* --------------------------------------------------------------------------
 * 取像素（截图用）
 * ----------------------------------------------------------------------- */

RColor r_get_pixel(int x, int y)
{
    const RCanvas *cv = &g_cv[g_cur];
    if (x < 0 || y < 0 || x >= cv->lw || y >= cv->lh)
        return 0;
    const uint8_t *p = cv->fb + (size_t)canvas_index(cv, x, y) * 3u;
    return R_RGB(p[2], p[1], p[0]);
}
