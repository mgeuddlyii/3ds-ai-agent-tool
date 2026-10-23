/*
 * sc_lab.c -- M1 验收页（回归屏）
 *
 * M1 的三页验收程序（中英混排样例 / 帧缓冲诊断 / 字库巡检）没有删，
 * 而是搬进了这个屏幕。理由：
 *   · 中文渲染和帧缓冲朝向是**每个里程碑都要复测**的东西，
 *     回归清单里就写着"中文无缺字/错位"；
 *   · 每换一次渲染后端（M11 立体）都要重新看一遍这三页。
 * 藏在主界面按 A/X 进去，不影响正常使用。
 *
 * 页内操作保持 M1 的手感：L/R 切页、方向键移光标、X/Y 翻字库页。
 * B 返回主界面。
 */

#include "app.h"
#include "font.h"
#include "widgets.h"

#include <stdio.h>
#include "uistr.h"

typedef enum {
    LAB_SAMPLE = 0,
    LAB_DIAG,
    LAB_GRID,
    LAB_PAGE_COUNT
} LabPage;

/* ★ 存编号不存字符串：静态初始化里 S() 放不下（理由见同文件 SAMPLE[]）。 */
static const short LAB_TITLE[LAB_PAGE_COUNT] = { S0453, S0454, S0455 };

#define GRID_COLS     25
#define GRID_ROWS     7
#define GRID_PER_PAGE (GRID_COLS * GRID_ROWS)

static LabPage g_page     = LAB_SAMPLE;
static int     g_grid_pg  = 0;
static int     g_grid_cur = 0;

static int grid_pages(void)
{
    int n = font_glyph_count();
    return (n + GRID_PER_PAGE - 1) / GRID_PER_PAGE;
}

/* --------------------------------------------------------------------------
 * 样例页
 * ----------------------------------------------------------------------- */

/* ★ 存**编号**而不是字符串：静态初始化里必须是编译期常量，
 * 而 S() 是函数调用（编译器报 "initializer element is not constant"，
 * 却指着 uistr.h 的宏定义）。取值时记得包一层 S()。 */
static const short SAMPLE[] = {
    S0358, S0456, S0457, S0458, S0459,
    S0460, S0461, S0462, S0463,
};
#define SAMPLE_N ((int)(sizeof(SAMPLE) / sizeof(SAMPLE[0])))

static void render_sample(void)
{
    const float x = W_TOP_MARGIN;
    const float w = W_TOP_USABLE;
    float y = (float)W_TOP_CONTENT_Y;

    for (int i = 0; i < SAMPLE_N; ++i) {
        w_line(x, y, w, S(SAMPLE[i]), (i == 0) ? w_c(C_SLOT_ACC_DK) : w_c(C_SLOT_FG));
        y += (float)TEXT_LINE_H;
    }

    y += 2;
    w_hr(x, y, w, w_c(C_SLOT_LINE));
    y += 6;

    char stat[80];
    snprintf(stat, sizeof stat, S(S0464),
             font_glyph_count(), font_cell_w(), font_cell_h(), font_baseline());
    w_line(x, y, w, stat, w_c(C_SLOT_OK));
}

/* --------------------------------------------------------------------------
 * 诊断页：四角彩块。TL 红 / TR 绿 / BL 蓝 / BR 黄 —— 朝向或通道错了立刻看出来。
 * ----------------------------------------------------------------------- */

#define CORNER_W 38
#define CORNER_H 28

static void corner(float x, float y, RColor c, const char *label)
{
    r_rect(&(Rect){ x, y, CORNER_W, CORNER_H, 0 }, c);
    r_text(&(Rect){ x + 3, y + 6, 32, 16, 0 }, label, R_RGB(0xFF, 0xFF, 0xFF));
}

static void render_diag(void)
{
    const float y0 = (float)W_TOP_CONTENT_Y + 2;
    const float y1 = (float)W_TOP_CONTENT_B - (float)CORNER_H - 2;

    corner(W_TOP_MARGIN, y0, R_RGB(0xE5, 0x39, 0x35), S(S0465));
    corner((float)R_TOP_W - CORNER_W - W_TOP_MARGIN, y0, R_RGB(0x43, 0xA0, 0x47), S(S0466));
    corner(W_TOP_MARGIN, y1, R_RGB(0x1E, 0x88, 0xE5), S(S0467));
    corner((float)R_TOP_W - CORNER_W - W_TOP_MARGIN, y1, R_RGB(0xFB, 0xC0, 0x2D), S(S0468));

    const float cy = (y0 + y1 + (float)CORNER_H) / 2.0f;
    r_hline(W_TOP_MARGIN, cy, (float)W_TOP_USABLE, w_c(C_SLOT_LINE));
    r_vline(200, (float)W_TOP_CONTENT_Y, (float)(W_TOP_CONTENT_B - W_TOP_CONTENT_Y), w_c(C_SLOT_LINE));
    r_rect(&(Rect){ 197, cy - 3, 6, 6, 0 }, w_c(C_SLOT_ACCENT));
}

/* --------------------------------------------------------------------------
 * 字库巡检页
 * ----------------------------------------------------------------------- */

static void render_grid(void)
{
    const int total = font_glyph_count();
    const int pages = grid_pages();
    const int base  = g_grid_pg * GRID_PER_PAGE;

    const float gy = (float)W_TOP_CONTENT_Y;

    for (int c = 0; c <= GRID_COLS; ++c)
        r_vline((float)(c * 16), gy, (float)(GRID_ROWS * TEXT_LINE_H),
                R_RGB(0xEC, 0xEC, 0xEC));
    for (int rw = 0; rw <= GRID_ROWS; ++rw)
        r_hline(0, gy + (float)(rw * TEXT_LINE_H), (float)(GRID_COLS * 16),
                R_RGB(0xEC, 0xEC, 0xEC));

    for (int i = 0; i < GRID_PER_PAGE; ++i) {
        int idx = base + i;
        if (idx >= total)
            break;

        float px = (float)((i % GRID_COLS) * 16);
        float py = gy + (float)((i / GRID_COLS) * TEXT_LINE_H) + 1.0f;

        if (i == g_grid_cur)
            r_rect(&(Rect){ px, py - 1, 16, 16, 0 }, w_c(C_SLOT_ACCENT));

        r_glyph(px, py, font_codepoint_at((uint32_t)idx),
                (i == g_grid_cur) ? w_c(C_SLOT_BG) : w_c(C_SLOT_FG));
    }

    const float dy = gy + (float)(GRID_ROWS * TEXT_LINE_H) + 6.0f;
    r_hline(0, dy - 4, (float)R_TOP_W, w_c(C_SLOT_LINE));

    uint32_t scp = font_codepoint_at((uint32_t)(base + g_grid_cur));
    const RGlyph *sg = font_lookup(scp);

    r_rect(&(Rect){ 4, dy, 32, 32, 0 }, R_RGB(0xF7, 0xF7, 0xF7));
    r_frame_rect(&(Rect){ 4, dy, 32, 32, 0 }, w_c(C_SLOT_LINE), 1);
    /* 2x 放大画字形。★ 覆盖率必须走 font_glyph_alpha()：
     * 原先这里硬编码 1bpp 位寻址（bits[py*2+(px>>3)] & 0x80>>(px&7)），
     * 换成灰度字库后会静默读出垃圾 —— 表现为"巡检器坏了"，
     * 其实只是读法不对。位深差异一个字库不该影响这个界面。 */
    {
        int cw = font_cell_w(), chh = font_cell_h();
        int side = (cw > chh ? cw : chh);
        int zoom = 32 / side;                        /* 32px 框内放大到整数倍 */
        int pad  = (side * zoom - cw * zoom) / 2;
        RColor fg = w_c(C_SLOT_FG);
        for (int py = 0; py < chh; ++py)
            for (int px = 0; px < cw; ++px) {
                uint8_t a = font_glyph_alpha(sg, px, py);
                if (a == 0)
                    continue;
                r_rect(&(Rect){ 4.0f + (float)(px * zoom) + (float)pad,
                                dy + 1.0f + (float)(py * zoom),
                                (float)zoom, (float)zoom, 0 },
                        R_AlphaShade(R_RGB(0xF7, 0xF7, 0xF7), fg, a));
            }
    }

    char l1[96], l2[96];
    snprintf(l1, sizeof l1, S(S0469),
             (unsigned)scp, (unsigned)sg->advance,
             (sg->flags & RGLYPH_WIDE) ? S(S0470) : S(S0471));
    snprintf(l2, sizeof l2, S(S0472),
             g_grid_pg + 1, pages, GRID_PER_PAGE, total, g_grid_cur);

    r_text(&(Rect){ 42, dy, (float)R_TOP_W - 48, TEXT_LINE_H, 0 }, l1, w_c(C_SLOT_FG));
    r_text(&(Rect){ 42, dy + 16, (float)R_TOP_W - 48, TEXT_LINE_H, 0 }, l2, w_c(C_SLOT_MUTED));
}

/* --------------------------------------------------------------------------
 * 调度
 * ----------------------------------------------------------------------- */

static void lab_enter(void)
{
    g_page = LAB_SAMPLE;
    app_status_set(S(S0473));
}

static void lab_render_top(void)
{
    char right[32];
    snprintf(right, sizeof right, "< %d/%d >", (int)g_page + 1, (int)LAB_PAGE_COUNT);
    w_top_header(S(LAB_TITLE[g_page]), right);

    switch (g_page) {
        case LAB_SAMPLE: render_sample(); break;
        case LAB_DIAG:   render_diag();   break;
        default:         render_grid();   break;
    }

    w_top_footer(app_status(), S(S0474));
}

static void lab_render_bottom(void)
{
    w_bot_header(S(S0475));

    /* 同 SAMPLE[]：存编号，0 表示空行（编号从 1 起） */
    static const short HELP[] = {
        S0476, S0477, S0478, S0479,
        0,
        S0480, S0481, S0482, S0483,
        /* 底屏可用宽度 300px。⚠️ 换英文后这些行更长，
         * "量过的"不再成立 —— 英文版真机必须重新核这一屏。 */
        S0484,
    };
    const int n = (int)(sizeof(HELP) / sizeof(HELP[0]));
    for (int i = 0; i < n; ++i)
        w_line(W_BOT_MARGIN, (float)W_BOT_CONTENT_Y + (float)(i * TEXT_LINE_H),
               (float)W_BOT_USABLE, HELP[i] ? S(HELP[i]) : "",
               (i < 4) ? w_c(C_SLOT_FG) : w_c(C_SLOT_MUTED));

    w_bot_footer(S(S0485));
}

static void lab_handle(u32 down, u32 held)
{
    (void)held;

    if (down & KEY_B) {
        app_pop();
        return;
    }

    if (down & KEY_L) g_page = (LabPage)(((int)g_page + LAB_PAGE_COUNT - 1) % LAB_PAGE_COUNT);
    if (down & KEY_R) g_page = (LabPage)(((int)g_page + 1) % LAB_PAGE_COUNT);

    if (g_page != LAB_GRID)
        return;

    const int pages = grid_pages();
    const int total = font_glyph_count();

    if (down & KEY_LEFT)  g_grid_cur--;
    if (down & KEY_RIGHT) g_grid_cur++;
    if (down & KEY_UP)    g_grid_cur -= GRID_COLS;
    if (down & KEY_DOWN)  g_grid_cur += GRID_COLS;

    if (g_grid_cur < 0) {
        if (g_grid_pg > 0) { g_grid_pg--; g_grid_cur += GRID_PER_PAGE; }
        else g_grid_cur = 0;
    }
    if (g_grid_cur >= GRID_PER_PAGE) {
        if (g_grid_pg + 1 < pages) { g_grid_pg++; g_grid_cur -= GRID_PER_PAGE; }
        else g_grid_cur = GRID_PER_PAGE - 1;
    }

    int remain = total - g_grid_pg * GRID_PER_PAGE;
    if (remain < GRID_PER_PAGE && g_grid_cur >= remain)
        g_grid_cur = remain - 1;
    if (g_grid_cur < 0)
        g_grid_cur = 0;

    if (down & KEY_Y) { g_grid_pg = (g_grid_pg + pages - 1) % pages; g_grid_cur = 0; }
    if (down & KEY_X) { g_grid_pg = (g_grid_pg + 1) % pages;         g_grid_cur = 0; }
}

const Screen g_screen_lab = {
    .id            = SCREEN_LAB,
    .name          = "SCREEN_LAB",
    .enter         = lab_enter,
    .leave         = NULL,
    .render_top    = lab_render_top,
    .render_bottom = lab_render_bottom,
    .handle        = lab_handle,
};
