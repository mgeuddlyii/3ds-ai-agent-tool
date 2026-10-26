/*
 * widgets.c -- 见 widgets.h
 *
 * 所有函数都假设调用方已经 r_begin() 并且 r_target() 到了正确的屏幕。
 * 这里不校验 target —— 在一个绘制工具里做状态检查，出问题时更难查。
 */

#include "widgets.h"

#include <stdio.h>
#include <string.h>
#include <3ds.h>          /* osGetTime —— w_bot_footer_roll 的轮播时钟 */
#include "uistr.h"
#include "anim.h"
#include "app.h"          /* app_note —— 轮播探针写 status.txt */

/* --------------------------------------------------------------------------
 * 主题表
 *
 * 暗色不是把亮色取反 —— 那样青色会变成粉红，页眉会白得刺眼。
 * 手挑了一版：底色用偏蓝的深灰（纯黑在 3DS 的 TN 屏上对比过猛、边缘发灰），
 * 主色降低明度并提高纯度，警告底改成深棕以免整块界面被打亮。
 * ----------------------------------------------------------------------- */

static const RColor THEMES[][C_SLOT_COUNT] = {
    /* ---- W_THEME_LIGHT：白底 + 青 ---- */
    {
        R_RGB(0xFF, 0xFF, 0xFF),   /* BG      */
        R_RGB(0x1A, 0x1A, 0x1A),   /* FG      */
        R_RGB(0x75, 0x75, 0x75),   /* MUTED   */
        R_RGB(0xA8, 0xA8, 0xA8),   /* FAINT   */
        R_RGB(0x00, 0x96, 0x88),   /* ACCENT  */
        R_RGB(0x00, 0x69, 0x5C),   /* ACC_DK  */
        R_RGB(0xE2, 0xF3, 0xF1),   /* ACC_LT  */
        R_RGB(0xDD, 0xDD, 0xDD),   /* LINE    */
        R_RGB(0xF5, 0xF7, 0xF7),   /* PANEL   */
        R_RGB(0x2E, 0x7D, 0x32),   /* OK      */
        R_RGB(0xD3, 0x2F, 0x2F),   /* WARN    */
        R_RGB(0xFF, 0xF6, 0xD9),   /* WARN_BG */
        R_RGB(0xE8, 0xC4, 0x4A),   /* WARN_LN */
        R_RGB(0x8A, 0x64, 0x00),   /* WARN_DK */
        R_RGB(0xA8, 0xA8, 0xA8),   /* DIS_FG  */
        R_RGB(0xF4, 0xF4, 0xF4),   /* DIS_BG  */
        R_RGB(0xE2, 0xE2, 0xE2),   /* DIS_LN  */
        R_RGB(0x00, 0x69, 0x5C),   /* GEN     */
    },
    /* ---- W_THEME_DARK：深灰蓝底 + 亮青 ---- */
    {
        R_RGB(0x16, 0x1B, 0x1E),   /* BG      */
        R_RGB(0xE6, 0xEA, 0xEC),   /* FG      */
        R_RGB(0x97, 0xA2, 0xA8),   /* MUTED   */
        R_RGB(0x60, 0x6B, 0x72),   /* FAINT   */
        R_RGB(0x2A, 0xC7, 0xB6),   /* ACCENT  */
        R_RGB(0x18, 0x9C, 0x8F),   /* ACC_DK  */
        R_RGB(0x11, 0x2B, 0x2A),   /* ACC_LT  */
        R_RGB(0x2E, 0x38, 0x3D),   /* LINE    */
        R_RGB(0x1E, 0x24, 0x28),   /* PANEL   */
        R_RGB(0x5C, 0xC8, 0x6A),   /* OK      */
        R_RGB(0xFF, 0x6B, 0x6B),   /* WARN    */
        R_RGB(0x2A, 0x22, 0x14),   /* WARN_BG */
        R_RGB(0x7A, 0x63, 0x22),   /* WARN_LN */
        R_RGB(0xF0, 0xC4, 0x5A),   /* WARN_DK */
        R_RGB(0x55, 0x5F, 0x65),   /* DIS_FG  */
        R_RGB(0x1C, 0x21, 0x24),   /* DIS_BG  */
        R_RGB(0x2A, 0x32, 0x36),   /* DIS_LN  */
        R_RGB(0x4A, 0xE3, 0xD0),   /* GEN     */
    },
};

#define THEME_COUNT ((int)(sizeof THEMES / sizeof THEMES[0]))

static int g_theme = W_THEME_LIGHT;

/* ---- 主题色族 ----------------------------------------------------------
 * 只存一个"基准饱和色"，派生的三档（DK / LT / 暗色变体）在 w_c 里算。
 * 存 8×6 个手挑值也不是不行，但那样改一处颜色要动 6 个数，
 * 而且很难保证 8 个颜色的明度关系一致（会出现某个颜色特别刺眼）。
 * 用同一个混合公式派生，8 个颜色的"深浅关系"天然一致。
 */
typedef struct { const char *name; unsigned char r, g, b; } AccentDef;

static const AccentDef ACCENTS[] = {
    { "teal",   0x00, 0x96, 0x88 },
    { "blue",   0x19, 0x76, 0xD2 },
    { "indigo", 0x3F, 0x51, 0xB5 },
    { "purple", 0x7B, 0x1F, 0xA2 },
    { "pink",   0xC2, 0x18, 0x5B },
    { "orange", 0xEF, 0x6C, 0x00 },
    { "green",  0x38, 0x8E, 0x3C },
    { "red",    0xC6, 0x28, 0x28 },
};

#define ACCENT_N ((int)(sizeof ACCENTS / sizeof ACCENTS[0]))

static int g_accent;

static RColor mix(RColor base, RColor toward, float t)
{
    /* RColor 是**打包的 u32**（见 render.h 的 R_RGB），不是带 r/g/b 成员的结构体。
     * 所以我按同一个宏的顺序把三个通道拆出来再混，混完再用 R_RGB 装回去 ——
     * 绝不自己拼位移：拼错一个 shift 会得到"看着像颜色但通道串了"的结果，
     * 而且只在某些颜色上明显。 */
    unsigned int br = (base   >> 16) & 0xFF, bg = (base   >> 8) & 0xFF, bb = base   & 0xFF;
    unsigned int tr = (toward >> 16) & 0xFF, tg = (toward >> 8) & 0xFF, tb = toward & 0xFF;

    unsigned char r = (unsigned char)((float)br + ((float)tr - (float)br) * t);
    unsigned char g = (unsigned char)((float)bg + ((float)tg - (float)bg) * t);
    unsigned char b = (unsigned char)((float)bb + ((float)tb - (float)bb) * t);
    return R_RGB(r, g, b);
}

static RColor accent_base(void)
{
    const AccentDef *a = &ACCENTS[g_accent];
    return R_RGB(a->r, a->g, a->b);
}

int w_accent_count(void) { return ACCENT_N; }

int w_accent_current(void) { return g_accent; }

void w_accent_set(int idx)
{
    if (idx >= 0 && idx < ACCENT_N)
        g_accent = idx;
}

RColor w_accent_swatch(int idx)
{
    if (idx < 0 || idx >= ACCENT_N)
        idx = 0;
    return R_RGB(ACCENTS[idx].r, ACCENTS[idx].g, ACCENTS[idx].b);
}

int w_accent_find(const char *name)
{
    if (name) {
        for (int i = 0; i < ACCENT_N; ++i)
            if (strcmp(name, ACCENTS[i].name) == 0)
                return i;
    }
    return 0;
}

const char *w_accent_name(int idx)
{
    if (idx < 0 || idx >= ACCENT_N)
        idx = 0;
    return ACCENTS[idx].name;
}

RColor w_c(int slot)
{
    /* ★ 主题色族在最前面拦一道：这 4 个槽位由 g_accent 决定，
     *   不再读 THEMES —— 否则换了颜色只有色块变、界面不变。
     *   其余 14 个槽位（底/前/警告/禁用）仍来自验过的亮暗调色板。 */
    if (slot == C_SLOT_ACCENT || slot == C_SLOT_ACC_DK ||
        slot == C_SLOT_ACC_LT || slot == C_SLOT_GEN) {
        RColor base = accent_base();
        bool dark = (g_theme == W_THEME_DARK);

        if (slot == C_SLOT_ACCENT)
            return dark ? mix(base, R_RGB(0xFF, 0xFF, 0xFF), 0.30f) : base;
        if (slot == C_SLOT_ACC_DK)
            return mix(base, R_RGB(0x00, 0x00, 0x00), dark ? 0.30f : 0.35f);
        if (slot == C_SLOT_GEN)
            return dark ? mix(base, R_RGB(0xFF, 0xFF, 0xFF), 0.42f)
                        : mix(base, R_RGB(0x00, 0x00, 0x00), 0.30f);
        /* ACC_LT：亮色下接近白，暗色下接近黑 —— 都是"底" */
        return dark ? mix(base, R_RGB(0x08, 0x0C, 0x0E), 0.82f)
                    : mix(base, R_RGB(0xFF, 0xFF, 0xFF), 0.88f);
    }

    if (slot < 0 || slot >= C_SLOT_COUNT)
        return R_RGB(0x00, 0x00, 0x00);
    return THEMES[g_theme][slot];
}

int w_theme(int id)
{
    if (id >= 0 && id < THEME_COUNT)
        g_theme = id;
    return g_theme;
}

int w_theme_current(void) { return g_theme; }

const char *w_theme_name(int id)
{
    switch (id) {
        case W_THEME_DARK: return S(S0755);
        default:           return S(S0756);
    }
}

/* --------------------------------------------------------------------------
 * 版面溢出计数（见 widgets.h）。这些字都是编译期常量，量一次就定死；
 * 不计数的话，"某行被右边缘裁掉最后一个字"只能靠肉眼在截图里发现。
 * ----------------------------------------------------------------------- */

static int  g_overflow;
static char g_overflow_last[96];
static int  g_voverflow;
static char g_voverflow_last[96];

static void note_overflow(const char *s, float avail)
{
    if (!s || !*s)
        return;
    int w = text_width(s);
    if (w <= (int)avail)
        return;
    ++g_overflow;
    if (!g_overflow_last[0])
        /* 整数化：%.0f 会把 newlib 的 dtoa（2-3KB 栈）拖进每帧渲染路径，
         * 在 Azahar 的小主栈上直接砸穿栈底（曾导致 PC=0 崩溃）。 */
        snprintf(g_overflow_last, sizeof g_overflow_last,
                 "%.44s (%dpx > %dpx)", s, w, (int)(avail + 0.5f));
}

int w_overflow_count(void) { return g_overflow; }

void w_overflow_reset(void)
{
    g_overflow = 0;
    g_overflow_last[0] = '\0';
    g_voverflow = 0;
    g_voverflow_last[0] = '\0';
}

const char *w_overflow_last(void) { return g_overflow_last; }

/* --------------------------------------------------------------------------
 * 纵向溢出（写在屏**下面**了）
 * ----------------------------------------------------------------------- */

/*
 * 为什么要单独一套判据：w_overflow 只管**右边缘**。
 * 而「画在了页脚底下」这件事横向完全合规 —— 文字宽度对、坐标也合法，
 * 它只是**看不见**了。
 *
 * 这个问题在一个下午咬了两次：
 *   1) 内容更新屏五行 × 34px 画到 236，而内容区下界是 200
 *      → 「全部回滚」整行沉在页脚底下，**永远点不到**；
 *   2) 修成 30px 之后余下 8px，我又放了一行 18px 的说明
 *      → 落在 194..212，与页脚叠字。
 *
 * 两次都没被"横向溢出计数"抓到，也都不在"能点到的坐标"里 ——
 * 触摸测试按坐标点，只按**能看见的位置**点。
 *
 * → 判据：**一个元素底边越过它所在屏的内容区下界，就记一次**。
 * 这与横向那条是同一类问题的两个方向，合起来才完整。
 */
void w_vbounds(int scr_h, int limit)
{
    if (scr_h > limit) {
        ++g_voverflow;
        if (!g_voverflow_last[0]) {
            snprintf(g_voverflow_last, sizeof g_voverflow_last,
                     S(S0757), scr_h, limit);
        }
    }
}

int  w_voverflow_count(void) { return g_voverflow; }
const char *w_voverflow_last(void) { return g_voverflow_last; }

/* --------------------------------------------------------------------------
 * 框架
 * ----------------------------------------------------------------------- */

/* 页眉底色。白天用主色（青），黑夜下主色本身已经调亮，铺满一条 400px
 * 的横条会太扎眼 —— 改用 ACC_DK（暗青），保持"这是页眉"的识别度
 * 又不至于在暗环境里发光。 */
static RColor header_bg(void)
{
    return (w_theme_current() == W_THEME_DARK) ? w_c(C_SLOT_ACC_DK)
                                               : w_c(C_SLOT_ACCENT);
}

/* 页眉上的文字色：白天是白字（底是青），黑夜是近白字（底是暗青）。
 * 两者都用 BG 槽位取不到正确值，所以显式给。 */
static RColor header_fg(void)
{
    return (w_theme_current() == W_THEME_DARK) ? w_c(C_SLOT_FG)
                                               : w_c(C_SLOT_BG);
}

void w_top_header(const char *title, const char *right)
{
    r_rect(&(Rect){ 0, 0, R_TOP_W, W_TOP_HEADER_H, 0 }, header_bg());
    r_rect(&(Rect){ 0, W_TOP_HEADER_H - 2, R_TOP_W, 2, 0 }, w_c(C_SLOT_ACC_DK));

    note_overflow(title, 260);
    r_text(&(Rect){ W_TOP_MARGIN, 3, 260, 16, 0 }, title, header_fg());

    if (right && *right) {
        int w = text_width(right);
        /* 右侧文字是右对齐的，留给它的空间是标题(260)之外的部分 */
        note_overflow(right, (float)(R_TOP_W - 260 - W_TOP_MARGIN));
        r_text(&(Rect){ (float)(R_TOP_W - w - W_TOP_MARGIN), 3, (float)w, 16, 0 },
               right, header_fg());
    }
}

void w_top_footer(const char *status, const char *hint)
{
    r_rect(&(Rect){ 0, W_TOP_CONTENT_B, R_TOP_W, W_FOOTER_H, 0 },
           w_c(C_SLOT_PANEL));
    r_hline(0, W_TOP_CONTENT_B, R_TOP_W, w_c(C_SLOT_LINE));

    note_overflow(status, W_TOP_USABLE);
    note_overflow(hint, W_TOP_USABLE);
    r_text(&(Rect){ W_TOP_MARGIN, W_TOP_CONTENT_B + 2, W_TOP_USABLE, 16, 0 },
           status ? status : "", w_c(C_SLOT_MUTED));
    r_text(&(Rect){ W_TOP_MARGIN, W_TOP_CONTENT_B + 20, W_TOP_USABLE, 16, 0 },
           hint ? hint : "", w_c(C_SLOT_ACC_DK));
}
void w_bot_header(const char *title)
{
    note_overflow(title, W_BOT_USABLE);
    r_rect(&(Rect){ 0, 0, R_BOT_W, W_BOT_HEADER_H, 0 }, header_bg());
    r_text(&(Rect){ W_BOT_MARGIN, 4, W_BOT_USABLE, 16, 0 }, title, header_fg());
}

void w_bot_footer(const char *hint)
{
    r_rect(&(Rect){ 0, W_BOT_CONTENT_B, R_BOT_W, W_BOT_FOOTER_H, 0 },
           w_c(C_SLOT_PANEL));
    r_hline(0, W_BOT_CONTENT_B, R_BOT_W, w_c(C_SLOT_LINE));
    note_overflow(hint, W_BOT_USABLE);
    r_text(&(Rect){ W_BOT_MARGIN, W_BOT_CONTENT_B + 5, W_BOT_USABLE, 16, 0 },
           hint ? hint : "", w_c(C_SLOT_MUTED));
}

/* ★ 轮播式操作指引（2026-10-23）。
 *
 *   为什么是轮播而不是写死一行：底屏只有 300px 可用宽，汉字 16px 一个，
 *   也就是**一行最多 18 个全角字**。而"十字键切格 + 上下选行 + 摇杆滚上屏
 *   + A/SELECT/X 各自的功能"这套键位，用一行无论如何讲不完 ——
 *   实测最短的那版也有 325px，会被右边缘裁掉半个"退"字。
 *
 *   所以改成"一次说一条，自动换下一条"：每条都完整可读，几秒后自动切。
 *   代价是用户可能刚好错过某一条，但它会绕回来 —— 比一行永远看不全强。
 *
 *   为什么用 osGetTime 而不是帧计数：帧率在真机与模拟器上差很多
 *   （实测 60fps vs ≈7fps），按帧算的话，模拟器上一屏提示要停二十几秒，
 *   等于没轮播。毫秒是唯一在两边都成立的时间基准。
 *
 *   `ms` 传 0 = 不轮播，永远显示第一条（设置类静态提示用这条）。
 *   返回值不是必需的，所以是 void —— 调用方只管每帧调一次。
 */
void w_bot_footer_roll(const char *const *items, int n, int ms)
{
    static int s_idx;
    static u64 s_at;
    static const char *const *s_owner;

    /* 探针：轮播到底换了几次、当前第几条。写进 status.txt ——
     * "页脚没换"有两种可能（没走到轮播 / 时间没到），
     * 截图分不出来，这两个数字能分出来。
     *
     * ★ 只在**真的换条**时记一行（3.2 秒一行），不要每帧或每秒记：
     *   app_note 的缓冲只有 6KB，一路写下去会把后面的证据挤掉，
     *   而"证据被缓冲区吃掉"和"这段代码没跑"在 status.txt 里长得一模一样。
     *   2026-10-24 排查时曾临时加过一条"每秒记时钟读数"的探针，
     *   它的作用只是证明 osGetTime 在模拟器里确实在走 —— 结论已确认
     *   （t 从 3201566447 递增），那条就不再留着。 */
    static int s_step;
    if (!items || n <= 0) {
        w_bot_footer("");
        return;
    }
    if (n == 1 || ms <= 0) {
        w_bot_footer(items[0]);
        return;
    }

    /* 换了另一组提示 → 从头开始，别让上一组的序号带过来 */
    if (s_owner != items) {
        s_owner = items;
        s_idx   = 0;
        s_at    = osGetTime();
    }

    u64 now = osGetTime();
    if (now - s_at >= (u64)ms) {
        /* 用 while 而不是 if：帧率低时一步就可能错过好几轮，
         * 只减一次会让轮播越走越慢，最后停在某一条上。 */
        while (now - s_at >= (u64)ms) {
            s_at += (u64)ms;
            s_idx = (s_idx + 1) % n;
            ++s_step;
        }
        app_note("hintroll: idx=%d step=%d dt=%d", s_idx, s_step,
                 (int)(now - s_at));
    }
    w_bot_footer(items[s_idx]);
}
/* --------------------------------------------------------------------------
 * 内容
 * ----------------------------------------------------------------------- */

float w_line(float x, float y, float w, const char *s, RColor c)
{
    int lh = TEXT_LINE_H;
    note_overflow(s, w);
    r_text(&(Rect){ x, y, w, (float)lh, 0 }, s, c);
    return y + (float)lh;
}

float w_wrap(float x, float y, float w, const char *s, RColor c, int line_h)
{
    int lh = line_h > 0 ? line_h : TEXT_LINE_H;
    int lines = text_line_count(s, (int)w);

    if (lines < 1)
        lines = 1;

    text_draw(&(Rect){ x, y, w, (float)(lines * lh), 0 }, s, c, lh);
    return y + (float)(lines * lh);
}

float w_bullet(float x, float y, float w, const char *s, RColor c, int line_h)
{
    int lh = line_h > 0 ? line_h : TEXT_LINE_H;

    /* 方块对齐到第一行字的中部偏下，视觉上才和文字基线齐 */
    r_rect(&(Rect){ x + 1, y + 5, 5, 5, 0 }, w_c(C_SLOT_ACCENT));

    return w_wrap(x + 13, y, w - 13, s, c, lh);
}

void w_hr(float x, float y, float w, RColor c)
{
    r_hline(x, y, w, c);
}

void w_panel(const Rect *r, RColor fill, RColor border)
{
    r_rect(r, fill);
    r_frame_rect(r, border, 1);
}

float w_kv(float x, float y, float kw, const char *key, const char *val,
           RColor kc, RColor vc, int line_h)
{
    int lh = line_h > 0 ? line_h : TEXT_LINE_H;

    note_overflow(key, kw - 6);
    note_overflow(val, (float)(R_TOP_W - x - kw - W_TOP_MARGIN));
    r_text(&(Rect){ x, y, kw - 6, (float)lh, 0 }, key, kc);
    r_text(&(Rect){ x + kw, y, R_TOP_W - x - kw - W_TOP_MARGIN, (float)lh, 0 }, val, vc);

    return y + (float)lh;
}

/* --------------------------------------------------------------------------
 * 控件
 * ----------------------------------------------------------------------- */

bool w_hit(const Rect *r, int x, int y)
{
    return x >= (int)r->x && x < (int)(r->x + r->w) &&
           y >= (int)r->y && y < (int)(r->y + r->h);
}

/* 按下态的压暗：按亮度判断往黑还是往白混。
 *
 * 为什么不直接减 RGB：在暗色主题下 ACC_DK 之类本来就暗，再减就糊成黑块。
 * 为什么不统一"按下=变暗"：白色普通按钮在白天是亮底、黑夜是暗底，
 * 而主动作按钮在两主题下亮度也相反 —— 找不到一个统一的"变暗方向"。
 * 所以判据是**当前颜色自己的亮度**：亮底往黑混、暗底往白混，
 * 结果就是"任何底色按下后都和原来差得够远"，这才是按下该有的感觉。
 *
 * 38% 是量出来的：四组典型底色的亮度差分别是
 *   白天普通(白→浅灰) 97 · 白天主动作(青→浅青) 57
 *   黑夜普通(深灰→中灰) 87 · 黑夜主动作(亮青→暗青) 58
 * 再往上（45%）白色按钮会变成中灰，看着像被禁用了而不是按下了。
 */
static RColor w_press_shade(RColor c)
{
    int r = R_RED(c), g = R_GREEN(c), b = R_BLUE(c);
    int lum = (r * 299 + g * 587 + b * 114) / 1000;
    int t = 38;                       /* 混色比例 */

    if (lum < 128)
        return R_RGB(r + (255 - r) * t / 100,
                     g + (255 - g) * t / 100,
                     b + (255 - b) * t / 100);
    return R_RGB(r * (100 - t) / 100,
                 g * (100 - t) / 100,
                 b * (100 - t) / 100);
}

/* --------------------------------------------------------------------------
 * 按钮按下动画
 *
 * 为什么挂在 widgets 层而不是改 w_button 的签名：
 *   w_button 有 20+ 个调用点（各屏的绘制代码），签名一动就得全改，
 *   而这些调用点关心的是"这是不是一个按下的按钮"，不该关心动画状态。
 *   所以这里按**矩形几何**维护一张小动画表 —— 矩形在渲染期是稳定的
 *   （同一屏同一按钮每帧坐标相同），拿它当 key 足够可靠。
 *
 *   表满时退化为无动画（直接跳到目标态）：宁可少一个动效，
 *   也不能因为表溢出让按钮画错位置。
 * ----------------------------------------------------------------------- */

#define WANIM_MAX 24

typedef struct {
    int  x, y, w, h;   /* key：矩形几何（整数化） */
    Anim anim;
    bool used;
    int  age;          /* 未被命中多少帧；用于回收 */
} WBtnAnim;

static WBtnAnim s_wanim[WANIM_MAX];

static WBtnAnim *wanim_find(const Rect *r, bool create)
{
    /* 几何整数化：浮点每次渲染可能有极小的末位差，
     * 直接比浮点会不停 miss。四舍五入到整数即可稳定。 */
    int x = (int)(r->x + 0.5f), y = (int)(r->y + 0.5f);
    int w = (int)(r->w + 0.5f), h = (int)(r->h + 0.5f);

    WBtnAnim *slot = NULL;

    for (int i = 0; i < WANIM_MAX; ++i) {
        if (!s_wanim[i].used)
            continue;
        if (s_wanim[i].x == x && s_wanim[i].y == y &&
            s_wanim[i].w == w && s_wanim[i].h == h) {
            s_wanim[i].age = 0;
            return &s_wanim[i];
        }
        /* 顺手回收：很多帧没被画到的条目（换屏了）可以腾出来 */
        if (s_wanim[i].age > 2 && (!slot || s_wanim[i].age > slot->age))
            slot = &s_wanim[i];       /* 记住最老的一个当候选 */
    }

    if (!create)
        return NULL;

    if (!slot) {
        for (int i = 0; i < WANIM_MAX; ++i) {
            if (!s_wanim[i].used) { slot = &s_wanim[i]; break; }
        }
    }
    if (!slot)
        return NULL;                  /* 表满：调用方退化为无动画 */

    slot->x = x; slot->y = y; slot->w = w; slot->h = h;
    slot->used = true;
    slot->age  = 0;
    anim_reset(&slot->anim);
    return slot;
}

/* 每帧推进所有按钮动画（多推进几帧，反正不画的条目也会被回收）。
 * 放在 w_button 里惰性推进也可以，但那样"某一帧没画它就不动"，
 * 松手后按钮会僵在那里 —— 所以需要一个每帧都会跑到的入口。 */
void w_anim_frame(void)
{
    for (int i = 0; i < WANIM_MAX; ++i) {
        if (!s_wanim[i].used)
            continue;
        anim_tick(&s_wanim[i].anim);
        if (s_wanim[i].age < 1000)
            ++s_wanim[i].age;
    }
}

void w_anim_reset_all(void)
{
    memset(s_wanim, 0, sizeof s_wanim);
}

/* 颜色按比例向目标混。t 是 0..ANIM_ONE 的定点值。 */
static RColor mix_col(RColor a, RColor b, int t)
{
    /* RColor 的位布局见 render.h —— 这里按 R/G/B 分量拆开混。 */
    int ar = (a >> 16) & 0xFF, ag = (a >> 8) & 0xFF, ab = a & 0xFF;
    int br = (b >> 16) & 0xFF, bg = (b >> 8) & 0xFF, bb = b & 0xFF;
    int rr = ar + (br - ar) * t / ANIM_ONE;
    int rg = ag + (bg - ag) * t / ANIM_ONE;
    int rb = ab + (bb - ab) * t / ANIM_ONE;
    return R_RGB(rr, rg, rb);
}

void w_button(const Rect *r, const char *label, int state)
{
    RColor fill, border, fg;
    const bool pressed = (state & W_BTN_PRESSED) != 0;

    switch (state & ~W_BTN_PRESSED) {
        case W_BTN_SELECTED: fill = w_c(C_SLOT_ACCENT); border = w_c(C_SLOT_ACC_DK); fg = w_c(C_SLOT_BG); break;
        case W_BTN_PRIMARY:  fill = w_c(C_SLOT_ACCENT); border = w_c(C_SLOT_ACC_DK); fg = w_c(C_SLOT_BG); break;
        case W_BTN_DISABLED: fill = w_c(C_SLOT_DIS_BG); border = w_c(C_SLOT_DIS_LN); fg = w_c(C_SLOT_DIS_FG); break;
        /* 危险动作：红底白字。按下时同样走 w_press_shade（压暗一档）。 */
        case W_BTN_DANGER:   fill = w_c(C_SLOT_WARN);    border = w_c(C_SLOT_WARN_DK); fg = w_c(C_SLOT_BG);     break;
        default:             fill = w_c(C_SLOT_BG);     border = w_c(C_SLOT_ACCENT); fg = w_c(C_SLOT_ACC_DK); break;
    }

    /* ★ 按下动画（2026-10-18）：把原先"按下瞬间跳变"改成按帧平滑过渡。
     *   t = 0 完全松开，t = ANIM_ONE 完全按下。按下/松开都走同一条
     *   anim_tick 推进，所以中途手指滑走也不会突兀。 */
    WBtnAnim *wa = wanim_find(r, true);
    float t = 0.0f;
    if (wa) {
        anim_set(&wa->anim, pressed);
        t = anim_value(&wa->anim);
    } else {
        /* 表满：退化为原来的瞬时行为，按钮外观仍然正确 */
        t = pressed ? 1.0f : 0.0f;
    }

    /* 按下：整体下移 1px，让"手压上去了"有物理感。位移先算出来，
     * 后面所有绘制统一用它 —— 否则会出现"底移了字没移"的错位。 */
    Rect b = *r;
    if (t > 0.0f)
        b.y += t;                       /* 平滑下沉，最大 1px */

    /* 按下时把亮色压暗一档。不能直接减 RGB —— 在暗色主题下
     * ACC_DK 本来就暗，再减就糊了。改用固定混色到黑/白，
     * 两个主题下都保持可见的对比。
     * 现在用定点插值，按下深度随动画推进而加深。 */
    if (t > 0.0f) {
        int ti = (int)(t * (float)ANIM_ONE);
        if (ti > ANIM_ONE) ti = ANIM_ONE;
        RColor shaded = w_press_shade(fill);
        fill   = mix_col(fill, shaded, ti);
        border = mix_col(border, w_c(C_SLOT_ACC_DK), ti);
    }

    r_rect(&b, fill);
    r_frame_rect(&b, border, 1);

    /* 选中态再加一圈内描边，在 3DS 的小屏上比换色更容易看出焦点。
     * 按下时省掉它：两道边框叠着看会以为按钮变厚了。 */
    if (!pressed && (state & ~W_BTN_PRESSED) == W_BTN_SELECTED)
        r_frame_rect(&(Rect){ b.x + 2, b.y + 2, b.w - 4, b.h - 4, b.z }, w_c(C_SLOT_BG), 1);

    note_overflow(label, b.w - 6);
    int tw = text_width(label);
    r_text(&(Rect){ b.x + (b.w - (float)tw) / 2.0f,
                    b.y + (b.h - 16.0f) / 2.0f,
                    (float)tw, 16, b.z }, label, fg);
}

void w_center(float cx, float y, const char *s, RColor c)
{
    int w = text_width(s);
    r_text(&(Rect){ cx - (float)w / 2.0f, y, (float)w, 16, 0 }, s, c);
}

void w_dots(float cx, float y, int n, int cur, RColor on, RColor off)
{
    const float d = 8.0f;
    const float gap = 4.0f;
    float total = (float)n * d + (float)(n - 1) * gap;
    float x = cx - total / 2.0f;

    for (int i = 0; i < n; ++i) {
        r_rect(&(Rect){ x, y, d, d, 0 }, (i == cur) ? on : off);
        x += d + gap;
    }
}

/* ---------------------------------------------------------------- 进度条 */

/* 经典 Windows 的三个颜色。**不跟主题** —— 理由见 widgets.h 的说明。 */
#define XPBAR_TRACK  R_RGB(0xC8, 0xC8, 0xC0)   /* 底槽：Win 的按钮面灰 */
#define XPBAR_FILL   R_RGB(0x2A, 0x5D, 0xA8)   /* 已完成：经典进度蓝 */
#define XPBAR_FILL_D R_RGB(0x1C, 0x3F, 0x76)   /* 已完成的暗侧（做出一点立体感） */
#define XPBAR_EDGE   R_RGB(0x60, 0x60, 0x60)   /* 外框 */
#define XPBAR_GAP    R_RGB(0xA8, 0xA8, 0xA0)   /* 段与段之间的缝（露出底槽） */

void w_progress(float x, float y, float w, float h, int pct, const char *label)
{
    const float seg_w = 9.0f;      /* 每段 9px */
    const float gap   = 2.0f;      /* 段间 2px 缝 */
    const float inset = 2.0f;      /* 边框内缩 */
    int       n_seg, i, done_seg;
    float     inner_w, inner_h, cx;

    if (w <= 8.0f || h <= 6.0f)
        return;                     /* 太窄就画不出来，宁可不画 */

    if (pct < 0)   pct = 0;
    if (pct > 100) pct = 100;

    /* 底槽 + 外框：先画框再画段，段正好压在框里面。 */
    r_rect(&(Rect){ x, y, w, h, 0 }, XPBAR_TRACK);
    r_frame_rect(&(Rect){ x, y, w, h, 0 }, XPBAR_EDGE, 1.0f);

    inner_w = w - inset * 2.0f;
    inner_h = h - inset * 2.0f;
    if (inner_w <= 0.0f || inner_h <= 0.0f)
        return;

    /* 段数按**可用宽度**算，而不是固定 20 段。
     * 固定段数在窄屏上会让最后一段被裁掉一半 ——
     * 而那一段可能正是"已完成"的收尾，看起来像进度条画错了。 */
    n_seg = (int)((inner_w + gap) / (seg_w + gap));
    if (n_seg < 1)
        n_seg = 1;

    /* 已完成的段数用**整数分配**（pct 映射到 0..n_seg），
     * 不用 float 逐段比较 —— 后者会在边界上随机多亮或少亮一段，
     * 每帧抖一下。 */
    done_seg = (n_seg * pct) / 100;

    cx = x + inset;
    for (i = 0; i < n_seg; ++i, cx += seg_w + gap) {
        float sw = (i == n_seg - 1) ? (inner_w - (seg_w + gap) * (n_seg - 1))
                                     : seg_w;
        if (sw <= 0.0f)
            break;
        if (i < done_seg) {
            r_rect(&(Rect){ cx, y + inset, sw, inner_h, 0 }, XPBAR_FILL);
            /* 下缘压一道暗色，做出经典进度条那点"块"的厚度感。
             * 高度只取 1px：再厚就变成渐变了，不是那个年代的东西。 */
            r_rect(&(Rect){ cx, y + inset + inner_h - 1.0f, sw, 1.0f, 0 },
                   XPBAR_FILL_D);
        } else {
            /* 未完成段画一条很淡的底，让"还剩多少"看得见。
             * 留空也可以，但空槽在深色主题下会像进度条断了。 */
            r_rect(&(Rect){ cx, y + inset, sw, inner_h, 0 }, XPBAR_GAP);
        }
    }

    if (label) {
        /* 标签压在条上居中。底色跟轨道同色，否则深色段上的字会糊。 */
        float tw = (float)strlen(label) * 6.0f + 8.0f;
        r_rect(&(Rect){ x + (w - tw) / 2.0f, y + inset - 1.0f, tw, inner_h + 2.0f, 0 },
               XPBAR_TRACK);
        r_text(&(Rect){ x + (w - tw) / 2.0f + 4.0f, y + inset + 1.0f,
                        tw, inner_h, 0 },
               label, R_RGB(0x10, 0x10, 0x10));
    }
}

float w_banner(float x, float y, float w, const char *text)
{
    const float h = 30.0f;

    r_rect(&(Rect){ x, y, w, h, 0 }, w_c(C_SLOT_WARN_BG));
    r_frame_rect(&(Rect){ x, y, w, h, 0 }, w_c(C_SLOT_WARN_LN), 1);

    /* 左侧竖条 + "!"方块，让它在白底界面上足够跳 */
    r_rect(&(Rect){ x, y, 4, h, 0 }, w_c(C_SLOT_WARN_LN));
    r_rect(&(Rect){ x + 12, y + 8, 14, 14, 0 }, w_c(C_SLOT_WARN_LN));
    w_center(x + 19, y + 7, "!", w_c(C_SLOT_WARN_DK));

    note_overflow(text, w - 44);
    r_text(&(Rect){ x + 34, y + 7, w - 44, 16, 0 }, text, w_c(C_SLOT_WARN_DK));
    return h;
}
