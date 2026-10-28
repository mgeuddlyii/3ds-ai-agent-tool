/*
 * app.c -- 见 app.h
 *
 * 屏幕栈很小（4 层），因为 M2/M3 的层级就这么深：
 *   主界面 →(push)→ 实验室 / 对话 / 设置 →(push)→ 确认弹窗
 */

#include "app.h"
#include "uistr.h"

#include "anim.h"
#include "creds.h"
#include "crypto.h"
#include "keystore.h"
#include "shot.h"
#include "widgets.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

/* 各屏的定义体（各自 .c 里的单例） */
extern const Screen g_screen_welcome;
extern const Screen g_screen_home;
extern const Screen g_screen_chat;
extern const Screen g_screen_apikey;
extern const Screen g_screen_keys;
extern const Screen g_screen_update;
extern const Screen g_screen_confirm;
extern const Screen g_screen_viewer;
extern const Screen g_screen_settings;
extern const Screen g_screen_lab;
extern const Screen g_screen_xfer;
extern const Screen g_screen_plugins;
extern const Screen g_screen_more;
extern const Screen g_screen_guide;
extern const Screen g_screen_locales;
extern const Screen g_screen_content;
extern const Screen g_screen_doubao;
extern const Screen g_screen_providers;
extern const Screen g_screen_keyedit;
extern const Screen g_screen_pickapi;
extern const Screen g_screen_dock;

static const Screen *const g_screens[SCREEN_COUNT] = {
    [SCREEN_NONE]    = NULL,
    [SCREEN_WELCOME] = &g_screen_welcome,
    [SCREEN_HOME]    = &g_screen_home,
    [SCREEN_CHAT]    = &g_screen_chat,
    [SCREEN_APIKEY]  = &g_screen_apikey,
    [SCREEN_KEYS]    = &g_screen_keys,
    [SCREEN_UPDATE]  = &g_screen_update,
    [SCREEN_CONFIRM] = &g_screen_confirm,
    [SCREEN_VIEWER]  = &g_screen_viewer,
    [SCREEN_SETTINGS]= &g_screen_settings,
    [SCREEN_LAB]     = &g_screen_lab,
    /* 「枚举只能加末尾」那条规矩针对的是 config.h 里的槽位数组；
     * 屏幕表是按 id 索引的，加在末尾即可，中间插会漏掉后面的映射。 */
    [SCREEN_XFER]    = &g_screen_xfer,
    [SCREEN_PLUGINS] = &g_screen_plugins,
    [SCREEN_MORE]    = &g_screen_more,
    [SCREEN_PICKAPI] = &g_screen_pickapi,
    [SCREEN_DOCK]    = &g_screen_dock,
    [SCREEN_GUIDE]   = &g_screen_guide,
    [SCREEN_LOCALES] = &g_screen_locales,
 [SCREEN_DOUBAO]  = &g_screen_doubao,
 [SCREEN_PROVIDERS] = &g_screen_providers,
 [SCREEN_KEYEDIT]   = &g_screen_keyedit,
};

const Screen *screen_get(ScreenId id)
{
    if (id <= SCREEN_NONE || id >= SCREEN_COUNT)
        return NULL;
    return g_screens[id];
}

const char *screen_name(ScreenId id)
{
    const Screen *s = screen_get(id);
    return s ? s->name : "SCREEN_NONE";
}

/* --------------------------------------------------------------------------
 * 全局状态
 * ----------------------------------------------------------------------- */

static ScreenId      g_stack[APP_STACK_MAX];
static int           g_depth;
static AppConfig     g_cfg;
static ConfigLoadInfo g_cfg_info;

static bool g_saved;
static int  g_notes_len;
/* notes 是静态区（.bss），加这点不占栈。
 *
 * 大小是**算出来的**，不是试出来的：
 *   · T 趟（故障注入）单个场景就有 10~14 行，每行 40~130 字节，
 *     八个场景累计约 1.7KB —— 而旧值 2048 只够前 3 个场景。
 *   · 溢出的后果特别隐蔽：app_note 在 room<=8 时**直接 return**，
 *     不报错、不截断标记，于是后面几个场景的记录凭空消失，
 *     宿主脚本读不到"场景 X DONE"，判它是**崩溃**——
 *     而真相只是缓冲区满了。上一次 T 趟就有 7 条断言这样被误判。
 *   → 所以给 6KB，并且把"曾经溢出过"这件事本身写进 status.txt
 *     （见 g_notes_overflow），让溢出变成可观测事实而不是沉默。
 */
static char g_notes[6144];
static int  g_notes_overflow;   /* 被丢弃的 app_note 次数 */
static char g_status[96];

static ScreenId g_start_screen = SCREEN_NONE;
static ScreenId g_end_screen   = SCREEN_NONE;

/* M5 加密自检结果。放在文件级（不是 app_init 局部）是为了让
 * app_write_status 能读到 —— status.txt 是无人值守验收的硬证据，
 * "自检跑了没、过了几项"必须能从文件里看出来，不能只靠屏幕。 */
static int  g_crypto_ok;        /* crypto_selftest() 通过项数 */
static int  g_crypto_mask;      /* 位掩码：哪几项过了（排障用） */
static bool g_creds_roundtrip;  /* 加解密往返自洽 */
static bool g_creds_present;    /* creds.enc 在不在 */

/* --------------------------------------------------------------------------
 * 配置
 * ----------------------------------------------------------------------- */

AppConfig *app_cfg(void) { return &g_cfg; }

const ConfigLoadInfo *app_cfg_info(void) { return &g_cfg_info; }

bool app_cfg_was_saved(void) { return g_saved; }

bool app_ai_configured(void) { return config_ai_configured(); }

/* ---- M5 加密 ---------------------------------------------------------- */

/* 自检是否全过。界面用它决定"能不能承诺加密存储"这句话说不说得出口。 */
bool app_crypto_ok(void)
{
    return g_crypto_ok == CRYPTO_SELFTEST_TOTAL && g_creds_roundtrip;
}

int app_crypto_selftest_passed(void) { return g_crypto_ok; }

/* creds.enc 是不是正在被用（存在且能解开 → 视为加密存储已启用）。 */
bool app_creds_used(void)
{
    return g_creds_present && app_crypto_ok();
}

void app_cfg_dirty(void)
{
    g_saved = false;    /* 有改动 = 尚未落盘；shutdown 会写 */
}

bool app_cfg_save_now(void)
{
    bool ok = config_save(&g_cfg);
    if (ok)
        g_saved = true;
    return ok;
}

/* --------------------------------------------------------------------------
 * 状态行 / 备注
 * ----------------------------------------------------------------------- */

void app_status_set(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(g_status, sizeof g_status, fmt, ap);
    va_end(ap);
}

const char *app_status(void) { return g_status; }

/* ---- M6 流式探针（见 app.h）---- */

static int g_stream_used  = -1;    /* -1 = 本次启动还没发过请求 */
static int g_stream_calls = 0;

void app_stream_report(int used)
{
    g_stream_used = used;
    ++g_stream_calls;
}

int ai_stream_used(void)  { return g_stream_used; }
int ai_stream_calls(void) { return g_stream_calls; }

/* ---- 语音识别探针（见 app.h）----
 *
 * 和 ai_stream 同款：配置是"用户想要什么"，探针是"本次实际走了哪条"。
 * 语音能不能验，靠的就是这两个数的组合 ——
 * 麦克风在 Azahar 里必然不可用，而"识别按钮按下去之后整个程序还活着、
 * 而且如实报了'没有麦克风'"这件事，只有从 status.txt 里看得到。 */
static int g_voice_used  = -1;    /* -1 = 这次启动压根没走语音 */
static int g_voice_calls = 0;

void app_voice_report(int used)
{
    g_voice_used = used;
    ++g_voice_calls;
}

int ai_voice_used(void)  { return g_voice_used; }
int ai_voice_calls(void) { return g_voice_calls; }

void app_note(const char *fmt, ...)
{
    va_list ap;
    int room = (int)sizeof g_notes - g_notes_len - 1;

    if (room <= 8) {
        /* 记一笔，但**不要**在这里写进 g_notes（已经没地方了）。
         * app_write_status 会把计数单独写一行，验收脚本据此能区分
         * 「场景崩了」和「证据被缓冲区吃掉了」—— 这两件事的现象
         * 在 status.txt 里长得一模一样（后面的行凭空消失）。 */
        ++g_notes_overflow;
        return;
    }

    va_start(ap, fmt);
    int n = vsnprintf(g_notes + g_notes_len, (size_t)room, fmt, ap);
    va_end(ap);

    if (n > 0) {
        if (n >= room)
            ++g_notes_overflow;      /* 这一行自己也被截了 */

        g_notes_len += (n < room) ? n : room - 1;

        /* 备注是逐行写的：自己补换行，调用方不用管 */
        if (g_notes_len + 1 < (int)sizeof g_notes) {
            g_notes[g_notes_len++] = '\n';
            g_notes[g_notes_len] = '\0';
        }
    }
}

/* --------------------------------------------------------------------------
 * 栈
 * ----------------------------------------------------------------------- */

static void call_enter(ScreenId id)
{
    const Screen *s = screen_get(id);
    if (s && s->enter)
        s->enter();
}

static void call_leave(ScreenId id)
{
    const Screen *s = screen_get(id);
    if (s && s->leave)
        s->leave();
}

int app_depth(void) { return g_depth; }

ScreenId app_current(void)
{
    return g_depth > 0 ? g_stack[g_depth - 1] : SCREEN_NONE;
}

ScreenId app_previous(void)
{
    return g_depth > 1 ? g_stack[g_depth - 2] : SCREEN_NONE;
}

/* --------------------------------------------------------------------------
 * 转场：切换屏幕时让新屏从一侧滑入
 *
 * ── 为什么是"滑入"而不是"旧屏滑出 + 新屏滑入" ──
 * 后者要**重画旧屏**，而 app_push 是**先调旧屏 leave() 再切栈**的。
 * 至少有一屏的 leave() 是真释放资源的（sc_guide 会 gkb_free 掉检索索引），
 * 切完之后再调它的 render 就是 use-after-free —— 崩在切屏那一瞬间，
 * 而且只在"离开那一屏"时复现，最难查。
 * 所以这里不碰旧屏：清底 + 新屏滑入。效果一样流畅，且没有释放/重入风险。
 *
 * ── 为什么按帧计数而不是按 tick 计时 ──
 * 帧率受渲染耗时影响很大（真机接近 60fps，Azahar 里只有 ~10fps）。
 * 按 tick 算的话，同一段动画在慢环境里会变成"一格一格地跳"，
 * 而按帧算在任何环境下的**观感步数一致**。
 * ----------------------------------------------------------------------- */

#define TR_FRAMES 10        /* 10 帧 ≈ 167ms@60fps：短到不烦人，长到看得清 */

static bool g_tr_on;
static int  g_tr_left;
static int  g_tr_dir;       /* +1 = 新屏从右边进；-1 = 从左边进（返回时）*/

/* 缓出曲线：起步快、收尾慢。线性位移看着像机械滑轨，
 * 缓出看着像"被推到位"。成本是一次乘法。 */
static float tr_ease(float p)
{
    float inv = 1.0f - p;
    return 1.0f - inv * inv;
}

static void tr_begin(int dir)
{
    g_tr_dir  = dir;
    g_tr_left = TR_FRAMES;
    g_tr_on   = true;
}

/* 推进一帧，返回本帧的进度 0..1（1 = 就位）。 */
static float tr_advance(void)
{
    if (!g_tr_on)
        return 1.0f;
    if (g_tr_left > 0)
        --g_tr_left;
    if (g_tr_left <= 0) {
        g_tr_on = false;
        return 1.0f;
    }
    return 1.0f - (float)g_tr_left / (float)TR_FRAMES;
}

/* 新屏前进边上的那道引导条。
 * 加它是因为"内容凭空出现在一侧"看着像没画完；
 * 有一道边跟着走，就变成"这一屏正在被推进来"。
 * ⚠️ 调用时**必须**已经把偏移归零 —— 它画的是屏幕坐标。 */
static void tr_edge(float off, float w, float h)
{
    if (off > 0.5f)                     /* 从右边进：左边界在 off */
        r_rect(&(Rect){ off - 3.0f, 0, 3.0f, h, 0 }, w_c(C_SLOT_ACCENT));
    else if (off < -0.5f)               /* 从左边进：右边界在 off+w */
        r_rect(&(Rect){ off + w, 0, 3.0f, h, 0 }, w_c(C_SLOT_ACCENT));
}

void app_push(ScreenId id)
{
    if (screen_get(id) == NULL)
        return;

    /* 同一屏连推两次是没有意义的（比如一帧里按了两次 A） */
    if (g_depth > 0 && g_stack[g_depth - 1] == id)
        return;

    if (g_depth >= APP_STACK_MAX) {
        app_note("stack overflow: push %s ignored", screen_name(id));
        return;
    }

    if (g_depth > 0)
        call_leave(g_stack[g_depth - 1]);

    g_stack[g_depth++] = id;
    call_enter(id);
    tr_begin(+1);            /* 进屏：新屏从右边进来 */
}

void app_pop(void)
{
    if (g_depth <= 1) {
        app_note("pop ignored: already at root");
        return;
    }

    call_leave(g_stack[g_depth - 1]);
    --g_depth;
    call_enter(g_stack[g_depth - 1]);
    tr_begin(-1);            /* 返回：往回退，新屏从左边进来 */
}

void app_switch(ScreenId id)
{
    if (screen_get(id) == NULL)
        return;

    if (g_depth == 0) {
        app_push(id);
        return;
    }

    call_leave(g_stack[g_depth - 1]);
    g_stack[g_depth - 1] = id;
    call_enter(id);
    tr_begin(+1);            /* 同一层替换：当"进屏"处理 */
}

/* --------------------------------------------------------------------------
 * 生命周期
 * ----------------------------------------------------------------------- */

bool app_init(void)
{
    bool file_present;

    memset(&g_cfg, 0, sizeof g_cfg);
    memset(&g_notes, 0, sizeof g_notes);
    g_notes_len = 0;
    g_depth     = 0;
    g_saved     = false;
    g_status[0] = '\0';

    bool loaded = config_load(&g_cfg);
    g_cfg_info  = *config_last_load();
    file_present = g_cfg_info.file_present;

    /* 启动计数：它同时是"二次启动确实读到了文件"的证据 */
    g_cfg.launches += 1;
    config_clamp(&g_cfg);
    app_cfg_dirty();

    /* ★ 语言必须在**任何界面绘制之前**生效。
     * 位置在 clamp 之后：clamp 才把 ui.lang 夹到合法短码，
     * 在它之前调会把"文件里写坏的值"当成有效语言。 */
    uistr_apply(g_cfg.ui.lang);

    if (!file_present) {
        app_push(SCREEN_WELCOME);
        app_status_set(S(S0035));
        app_note("start: no config file -> SCREEN_WELCOME");
    } else if (!g_cfg_info.parse_ok) {
        /* 文件在但读不懂：不能装作没事，也不能直接崩。
         * 走引导让用户重新确认一遍设置，同时保留（未解析的）原文件不覆盖。 */
        app_push(SCREEN_WELCOME);
        app_status_set(S(S0036));
        app_note("start: config parse failed -> SCREEN_WELCOME (defaults)");
    } else if (!g_cfg.first_run_completed) {
        app_push(SCREEN_WELCOME);
        app_status_set(S(S0037));
        app_note("start: first_run_completed=false -> SCREEN_WELCOME");
    } else {
        app_push(SCREEN_HOME);
        app_status_set(S(S0038), g_cfg.launches);
        app_note("start: first_run_completed=true -> SCREEN_HOME");
    }

    g_start_screen = app_current();

    /* 主题从配置里恢复。放在 push 之后、第一次 render 之前 ——
     * 早于它没有屏幕可画，晚于它第一帧会闪一下亮色。 */
    w_theme(!strcmp(g_cfg.ui.theme, "dark") ? W_THEME_DARK : W_THEME_LIGHT);
    /* 主题色是正交的一层：亮暗先定，再叠强调色。
     * 认不出的名字由 w_accent_find 兜底成 teal，不会因为配置写坏就变成不可见。 */
    w_accent_set(w_accent_find(g_cfg.ui.accent));
    app_note("theme=%s", w_theme_name(w_theme_current()));

    app_note("cfg_file=%d parse_ok=%d version=%d unknown=%d bad=%d",
             (int)g_cfg_info.file_present, (int)g_cfg_info.parse_ok,
             g_cfg_info.version_seen, g_cfg_info.unknown_keys,
             g_cfg_info.bad_values);

    /* ---- M5 加密自检 ----------------------------------------------------
     * 本机没有 PC 架构的 gcc，加密内核没法在电脑上编一份来验；那就让它
     * 在设备上自证：启动时对 RFC 8439 的官方向量跑一遍，结果写进 status.txt。
     * 这比在 PC 上另编一份更可信 —— 验的是真正在跑的那份代码。
     *
     * 刻意**不**因为自检失败就拒绝启动：自检挂了说明加密不可用，
     * 那就退回"明文存取"这条老路（keystore 依旧可用），而不是让用户
     * 连界面都进不去。但要把失败**响亮地记下来**（status.txt + 备注），
     * 绝不能静默降级 —— 静默降级的加密是比没有加密更坏的东西。 */
    g_crypto_ok       = crypto_selftest();
    g_crypto_mask     = crypto_selftest_mask();
    g_creds_roundtrip = creds_roundtrip_selftest();
    g_creds_present   = creds_exists();

    /* 自检全过才开加密闸门：creds_save 在闸门关闭时直接失败，
     * keystore 随之退回明文路径。默认关闭，这里显式打开。 */
    creds_set_available(g_crypto_ok == CRYPTO_SELFTEST_TOTAL
                        && g_creds_roundtrip);

    app_note("crypto_selftest=%d/%d mask=0x%02x roundtrip=%d",
             g_crypto_ok, CRYPTO_SELFTEST_TOTAL, g_crypto_mask,
             (int)g_creds_roundtrip);
    app_note("creds.enc exists=%d last=\"%s\"",
             (int)g_creds_present, creds_last_note());

    if (g_crypto_ok != CRYPTO_SELFTEST_TOTAL || !g_creds_roundtrip)
        app_note("WARNING: crypto self-test FAILED -> creds.enc 不可用，"
                 "回退明文存储");

    (void)loaded;   /* 解析失败已有专门分支处理，这里不需要额外动作 */
    return true;
}

void app_shutdown(void)
{
    /* 先记下退出时停在哪一屏 —— 之后栈就被清空了，status.txt 会读不到。 */
    g_end_screen = app_current();

    /* 从栈顶往下 leave，保证每个进过的屏都有配对的收尾 */
    for (int i = g_depth - 1; i >= 0; --i)
        call_leave(g_stack[i]);
    g_depth = 0;

    /* 每次都写：一来保证 launches 递增落盘，二来每次启动都是一次
     * 真实的"读 → 改 → 写"往返，坏了会立刻暴露。 */
    g_saved = config_save(&g_cfg);
    app_note("saved=%d path=%s", (int)g_saved, config_path());
}

void app_finish_onboarding(void)
{
    g_cfg.first_run_completed = true;
    app_cfg_dirty();

    /* 立刻落盘。三屏是"不可跳过"的流程，万一后续崩了让用户重走一遍
     * 是最糟的体验 —— 宁可多写一次盘。 */
    bool ok = app_cfg_save_now();
    app_note("onboarding finished, save_now=%d", (int)ok);

    app_status_set(S(S0039));

    /* 引导三屏之后并不是终点 —— 还要教会用户怎么拿到凭据。
     * 这一段单独用 tutorial_completed 记账，而不是塞进 first_run_completed：
     * 前者是"这轮流程走完了"，后者是"用户知道怎么配 Key 了"，
     * 混在一个布尔里，将来想"只重看教程"就没法表达。 */
    if (!g_cfg.tutorial_completed) {
        app_note("-> SCREEN_APIKEY (tutorial not completed)");
        app_switch(SCREEN_APIKEY);
        return;
    }

    app_switch(SCREEN_HOME);
}

/* 查看器的"带参跳转"：路径先存在这里，查看器 enter 时取走。
 * 屏幕栈保持是唯一的导航真相，不额外开直通车的口子。 */
static char g_viewer_path[160];

void app_open_viewer(const char *path)
{
    snprintf(g_viewer_path, sizeof g_viewer_path, "%s", path ? path : "");
    app_push(SCREEN_VIEWER);
}

const char *app_viewer_path(void) { return g_viewer_path; }

static void draw_diag_badge(void);

void app_render(void)
{
    const Screen *s = screen_get(app_current());
    float         p;
    float         off_top = 0.0f, off_bot = 0.0f;

    if (!s)
        return;

    /* 转场：k = 新屏离就位还差多少（1 = 整屏在屏外，0 = 就位）。
     * ⚠️ k 必须**按各自屏宽算**，不能用同一个像素数 ——
     * 顶屏 400、底屏 320，共用偏移量的话两块屏会不同步到位。 */
    p = tr_advance();
    if (p < 1.0f) {
        float k = 1.0f - tr_ease(p);
        off_top = k * (float)R_TOP_W * (float)g_tr_dir;
        off_bot = k * (float)R_BOT_W * (float)g_tr_dir;
    }

    if (s->render_top) {
        r_target(R_SCREEN_TOP);
        r_begin(w_c(C_SLOT_BG));    /* 清底；顺带把平移归零 */
        w_overflow_reset();         /* 每帧从零计：不然数字会一直涨 */
        if (off_top != 0.0f) {
            r_set_offset(off_top, 0.0f);
            s->render_top();
            r_set_offset(0.0f, 0.0f);
            tr_edge(off_top, (float)R_TOP_W, (float)R_TOP_H);
        } else {
            s->render_top();
        }
        draw_diag_badge();
    }

    /* ★ 2026-10-28：字号缩放是**全局**状态，只有阅读区会临时打开。
     * 在这里统一复位 —— 让"忘了关"不可能发生。
     * 放在两块屏都画完之后：底屏也可能要放大（文档浏览就在底屏）。 */
    r_text_scale_set(100);

    if (s->render_bottom) {
        r_target(R_SCREEN_BOTTOM);
        r_begin(w_c(C_SLOT_BG));
        if (off_bot != 0.0f) {
            r_set_offset(off_bot, 0.0f);
            s->render_bottom();
            r_set_offset(0.0f, 0.0f);
            tr_edge(off_bot, (float)R_BOT_W, (float)R_BOT_H);
        } else {
            s->render_bottom();
        }
    }
}

/* 诊断角标：开着 diag.show_layout_overflow 时，右上角显示本帧的溢出计数。
 * 验收脚本读的是 status.txt，这个角标是给人**在模拟器窗口里直接看**的 ——
 * 改文案之后不用等跑完整套验收，扫一眼就知道排版有没有被撑破。
 * 计数为 0 时什么都不画：一个常驻的"0"反而是噪音。 */
static void draw_diag_badge(void)
{
    if (!g_cfg.diag.show_layout_overflow)
        return;

    int n = w_overflow_count();
    if (n <= 0)
        return;

    char t[24];
    snprintf(t, sizeof t, "OVER %d", n);
    int tw = text_width(t);

    r_rect(&(Rect){ (float)(R_TOP_W - tw - 10), 22, (float)(tw + 6), 16, 0 },
           w_c(C_SLOT_WARN_LN));
    r_text(&(Rect){ (float)(R_TOP_W - tw - 7), 23, (float)tw, 16, 0 },
           t, w_c(C_SLOT_WARN_DK));
}

void app_handle(u32 down, u32 held)
{
    const Screen *s = screen_get(app_current());
    if (s && s->handle)
        s->handle(down, held);
}

/* ---- 触摸 --------------------------------------------------------------- */

static int  g_touch_x = -1;
static int  g_touch_y = -1;
static bool g_touch_held;      /* 手指当前按着（持续） */
static bool g_touch_fresh;     /* 本帧是按下沿（只在 main 喂的那一帧为真） */

/* 按下态的**粘滞**。
 *
 * 为什么必须有：一次快速点击（鼠标 click、或手指轻点）在物理上按下到
 * 抬起可能只有几十毫秒，而本机 Azahar 里一帧约 143ms（≈7fps）——
 * 按下态会在两次采样之间就结束掉，用户反馈"点了没反应"。
 * 真机 60fps 时这个窗口反而不是问题，但按帧数算就不成立，
 * 所以这里按**帧**而不是按时间保留。
 *
 * 8 帧 ≈ 1.1 秒 @7fps、≈0.13 秒 @60fps：模拟器上足以看清，
 * 真机上短到几乎察觉不到，两边都成立。 */
#define PRESS_STICKY_FRAMES 8

static int  g_press_x = -1;   /* 按下那一刻的位置 */
static int  g_press_y = -1;
static int  g_press_frames;   /* 按下态还要保持几帧 */

void app_touch_frame(int x, int y, bool pressed)
{
    /* down 与 held 的区别就在这里：main 只在 KEY_TOUCH 的**按下沿**
     * 那一帧传 pressed=true，之后的保持帧传 false。 */
    g_touch_fresh = pressed && (x >= 0);
    g_touch_held  = (x >= 0);
    g_touch_x     = x;
    g_touch_y     = y;

    if (g_touch_fresh) {
        g_press_x      = x;
        g_press_y      = y;
        g_press_frames = PRESS_STICKY_FRAMES;
    } else if (g_press_frames > 0) {
        --g_press_frames;
    }
}

bool app_touch_down(int *x, int *y)
{
    if (!g_touch_fresh)
        return false;
    if (x) *x = g_touch_x;
    if (y) *y = g_touch_y;
    return true;
}

bool app_touch_held(int *x, int *y)
{
    if (!g_touch_held)
        return false;
    if (x) *x = g_touch_x;
    if (y) *y = g_touch_y;
    return true;
}

/* ---- 摇杆（圆摇）-------------------------------------------------------
 *
 * ★ 2026-10-23 用户定的分工：
 *     「十字键控制下屏内容，摇杆上下控制上屏文字上下滑动」
 *
 *   在此之前**摇杆一路都没被读过**（全仓 grep circlePosition 为空），
 *   所以这是一条新通路，而不是改既有行为。
 *
 * 为什么经 app 层转一手，而不是让屏幕自己调 hidCircleRead：
 *   ① main 才是唯一该碰 hid 的地方 —— 屏幕里再读一次，等于同一帧
 *      两个采样点，"看到的"和"处理掉的"可能不是同一帧的数据；
 *   ② 回放脚本（autorun）替的是**按键**，直接在屏幕里读摇杆会绕过
 *      回放层，验收趟永远推不动它 —— 而验收正是唯一能自动化的事。
 *   所以 main 喂一次 app_circle_frame()，屏幕层用 app_circle_x/y 取。
 *
 * 值域：摇杆原始值是 -156..156，这里**照原样**传，不做缩放 ——
 * 死区阈值属于"用它的那一屏"的判断（不同屏幕要的灵敏度不一样），
 * 在采集层先掐死就再也调不回来了。
 */
static int g_circle_x;
static int g_circle_y;

void app_circle_frame(int x, int y)
{
    /* 超范围钳制：hidCircleRead 有抖动，偶尔会吐出比 156 大的值。
     * 不钳的话，按"值 / 156"算比例的地方会算出 >1 的系数。 */
    if (x < -156) x = -156;
    if (x >  156) x =  156;
    if (y < -156) y = -156;
    if (y >  156) y =  156;
    g_circle_x = x;
    g_circle_y = y;
}

int app_circle_x(void) { return g_circle_x; }
int app_circle_y(void) { return g_circle_y; }

bool app_touch_on(const Rect *r)
{
    if (g_touch_held && w_hit(r, g_touch_x, g_touch_y))
        return true;

    /* 粘滞：手指（或鼠标）已经抬起来了，但按下态还要多留几帧。
     * 不加这一条，一次快速点击在低帧率下等于"从来没按过" ——
     * 而"点了没反应"是最容易被当成程序坏了的现象。 */
    if (g_press_frames > 0 && g_press_x >= 0 && w_hit(r, g_press_x, g_press_y))
        return true;

    return false;
}

/* --------------------------------------------------------------------------
 * status.txt -- 无人值守验收的硬证据
 *
 * 截图看的是"长什么样"，这个文件看的是"程序自己认为发生了什么"。
 * 两者都要有：截图能骗过眼睛（比如引导页画对了但没落盘），
 * 日志能骗过断言（比如写了自己跳过了引导但其实没跳）。
 * ----------------------------------------------------------------------- */

void app_write_status(int frames)
{
    shot_mkdirs(SHOT_DIR);

    /* ⚠️⚠️ **先写临时文件，再 rename** —— 这不是洁癖，是修一个真实竞态。
     *
     * 原来直接 fopen(".../status.txt", "wb")，而 "w" 一调用就把文件
     * **截断、mtime 也跟着变新**。于是宿主脚本那个
     * 「mtime 比启动时刻新就 break」的等待逻辑，会在**内容还没落盘**
     * 的那一瞬间读到一个 0 字节文件。
     *
     * 现象与"程序崩了"**完全一样**：status.txt 里所有字段都没了。
     * 我为此专门加过一个 read_status_stable（连读两次一致才算稳），
     * 但那只治了读取端，**产生 0 字节窗口的这一侧没治** ——
     * 2026-10-03 B/K 趟间歇失败就是它（实测抓到的时序：
     *   t=12.0s  status=0B      <- 截断了
     *   t=13.5s  status=1241B   <- 才写完，只差 1.5 秒）
     *
     * rename 在同一个目录内是原子的，读者要么看到**完整的旧文件**，
     * 要么看到**完整的新文件**，看不到中间的 0 字节。
     * 这样"证据是完整的"变成了写盘方式保证的性质，而不是靠读方去猜。
     *
     * 失败处理：rename 在 FAT 上有极小概率失败（比如目标被占用）。
     * 那就退回收到的结果直接写正式路径 —— 宁可回到旧的竞态，
     * 也不能不写（不写的话整轮验收直接失去证据）。
     */
    static const char *TMP = "sdmc:/3ds/aiconsole/status.tmp";

    FILE *f = fopen(TMP, "wb");
    int used_tmp = (f != NULL);
    if (!f)
        f = fopen("sdmc:/3ds/aiconsole/status.txt", "wb");   /* 兜底 */
    if (!f)
        return;

    fprintf(f, "app=3DS-AI-Console\n");
    fprintf(f, "milestone=M2\n");
    fprintf(f, "frames=%d\n", frames);

    fprintf(f, "cfg_file=%d\n",        (int)g_cfg_info.file_present);
    fprintf(f, "cfg_parse_ok=%d\n",    (int)g_cfg_info.parse_ok);
    fprintf(f, "cfg_version=%d\n",     g_cfg_info.version_seen);
    fprintf(f, "cfg_unknown_keys=%d\n",g_cfg_info.unknown_keys);
    fprintf(f, "cfg_bad_values=%d\n",  g_cfg_info.bad_values);

    fprintf(f, "first_run_completed=%d\n", (int)g_cfg.first_run_completed);
    fprintf(f, "launches=%d\n",            g_cfg.launches);

    fprintf(f, "nand_confirmed=%d\n",      (int)g_cfg.nand_backup.confirmed);
    fprintf(f, "nand_remind_on_startup=%d\n", (int)g_cfg.nand_backup.remind_on_startup);

    fprintf(f, "security_level=%d\n",      g_cfg.security.current_level);
    fprintf(f, "security_persist=%d\n",    (int)g_cfg.security.persist_level);
    fprintf(f, "dev_mode_unlocked=%d\n",   (int)g_cfg.security.dev_mode_unlocked);

    fprintf(f, "ai_provider=%s\n",         g_cfg.ai.provider);
    fprintf(f, "ai_model=%s\n",            g_cfg.ai.model);
    /* ai_stream 是**配置**（用户想要什么）；ai_stream_used 是**本次实际走的**
     * 路径（0=非流式 / 1=流式 / 2=流式失败后退回）。两个都要 ——
     * "配置里写着开、实际却没走流式"正是 M6 之前那个撒谎的坑。 */
    fprintf(f, "ai_stream=%d\n",           (int)g_cfg.ai.stream);
    fprintf(f, "ai_stream_used=%d\n",      ai_stream_used());
    fprintf(f, "ai_stream_calls=%d\n",     ai_stream_calls());
    fprintf(f, "ai_configured=%d\n",       (int)config_ai_configured());

    /* 语音：used = 1 识别成 / 2 识别失败；calls = 试了几次。
     * calls > 0 而 used == -1 就是"按了但一次都没走到识别" ——
     * 这正是"按钮按下去没反应"要抓的形态。 */
    fprintf(f, "ai_voice_used=%d\n",       ai_voice_used());
    fprintf(f, "ai_voice_calls=%d\n",      ai_voice_calls());

    fprintf(f, "ui_theme=%s\n",            g_cfg.ui.theme);
    fprintf(f, "ui_theme_active=%s\n",     w_theme_name(w_theme_current()));
    /* 整数化：避免 %f 拉进 newlib dtoa（栈炸元凶，见 widgets.c 同类修复） */
    fprintf(f, "ui_depth=%d\n",           (int)(g_cfg.ui.depth * 100.0f + 0.5f));
    fprintf(f, "ui_font_size=%s\n",        g_cfg.ui.font_size);
    /* ★ 界面语言必须**单独报出来**，不能靠 ui_theme_active 那种"英文名"去反推：
     *   主题名在两种语言下不同只是巧合，将来主题改成中性名就断了。
     *   而这件事有真实后果 —— **内嵌手册是按它挑中/英两份的**
     *   （见 shared/agentdoc.c 的 pick_doc），验收里想判断"这一轮用的是哪份
     *   手册"就必须知道语言。 */
    fprintf(f, "ui_lang=%s\n",             uistr_lang() ? "en" : "zh");

    fprintf(f, "guard_overwrite=%d\n",     (int)g_cfg.guard.confirm_overwrite);
    fprintf(f, "guard_delete=%d\n",        (int)g_cfg.guard.confirm_delete);
    fprintf(f, "guard_bulk=%d\n",          (int)g_cfg.guard.confirm_bulk);
    fprintf(f, "guard_write_kb=%d\n",      g_cfg.guard.max_write_kb);
    fprintf(f, "guard_batch_files=%d\n",   g_cfg.guard.max_batch_files);

    fprintf(f, "diag_overflow=%d\n",       (int)g_cfg.diag.show_layout_overflow);
    fprintf(f, "diag_notes=%d\n",          (int)g_cfg.diag.show_notes);
    fprintf(f, "diag_verbose=%d\n",        (int)g_cfg.diag.verbose_status);

    fprintf(f, "stats_saved=%d\n",         (int)g_saved);

    /* ---- M5 加密 ---------------------------------------------------- */
    fprintf(f, "crypto_selftest=%d\n",     g_crypto_ok);
    fprintf(f, "crypto_selftest_total=%d\n", CRYPTO_SELFTEST_TOTAL);
    fprintf(f, "crypto_selftest_mask=%d\n", g_crypto_mask);
    fprintf(f, "crypto_ok=%d\n",
            (int)(g_crypto_ok == CRYPTO_SELFTEST_TOTAL && g_creds_roundtrip));
    fprintf(f, "creds_roundtrip=%d\n",     (int)g_creds_roundtrip);
    fprintf(f, "creds_exists=%d\n",        (int)(g_creds_present = creds_exists()));
    fprintf(f, "creds_last=%s\n",          creds_last_note());
    fprintf(f, "creds_used=%d\n",          (int)app_creds_used());

    /* 仓库状态。放在 status.txt 里是刻意的：M5 之后仓库读写在**加密文件**里，
     * 宿主脚本没法直接解析（KDF 带设备因子，PC 上重现不出来）。
     * 所以由程序自己把"现在仓库里有几把、哪把在用"报出来 ——
     * 验收端据此判断切换是否真的落了盘，不需要去解密文件。 */
    {
        int n = keystore_count();
        fprintf(f, "ks_count=%d\n", n);
        fprintf(f, "ks_encrypted=%d\n", (int)keystore_is_encrypted());
        fprintf(f, "ks_active=%s\n", n > 0 ? keystore_active_name() : "");
    }

    fprintf(f, "start_screen=%s\n",        screen_name(g_start_screen));
    fprintf(f, "end_screen=%s\n",          screen_name(g_end_screen));

    fprintf(f, "layout_overflow=%d\n",     w_overflow_count());
    fprintf(f, "layout_overflow_last=%s\n", w_overflow_last());
    /* 纵向那条。与横向分开报，因为**症状完全不同**：
     * 横向是"字被裁掉半个"，纵向是"整个元素看不见也点不到"。 */
    fprintf(f, "layout_voverflow=%d\n",    w_voverflow_count());
    fprintf(f, "layout_voverflow_last=%s\n", w_voverflow_last());

    /* 「换了缓冲却没画」次数。健康值恒为 **0**。
     * 非 0 = 某次 r_end() 交换时有一块屏本轮没被画过，
     * 那块屏就会在两次旧内容之间来回翻 → 用户看到"屏一直在闪"。
     * 这类缺陷截图抓不到（时序问题），所以必须有这个读数。
     * 上/下屏分开报，是为了直接指出是**哪块**屏在闪。 */
    fprintf(f, "swap_undrawn=%d\n",        r_swap_undrawn_count());
    fprintf(f, "swap_undrawn_top=%d\n",    r_swap_undrawn_top_count());
    fprintf(f, "swap_undrawn_bot=%d\n",    r_swap_undrawn_bot_count());

    /* 按钮/界面过渡动画的自检项数（2026-10-18）。
     * 动画是**时序**行为，单帧截图证明不了"中间有过渡"——两个时刻的
     * 样子各自都正常，错的是"直接跳变"。所以让动画模块自己跑三项
     * 逻辑自检（多帧推进 / 单调 / 中途反向），健康值 = ANIM_SELFTEST_TOTAL。 */
    fprintf(f, "anim_selftest=%d\n",       anim_selftest());
    fprintf(f, "anim_selftest_total=%d\n", ANIM_SELFTEST_TOTAL);

    /* 备注区被写满的次数。**必须写出来**，理由同上：
     * 溢出在 status.txt 里的现象是「后面的行凭空消失」，
     * 和「程序崩了」完全一样。不给这个计数，验收脚本只能靠猜，
     * 而它已经猜错过一次（把缓冲区满判成 use-after-free）。
     * 正常情况恒为 0。 */
    fprintf(f, "notes_overflow=%d\n",      g_notes_overflow);
    fprintf(f, "notes_bytes=%d\n",         g_notes_len);
    if (g_notes_overflow > 0)
        fprintf(f, "notes_warning=有 %d 条备注被缓冲区丢弃，"
                   "下面的 notes 不完整（**不是崩溃**）\n",
                g_notes_overflow);

    fprintf(f, "\n-- notes --\n%s", g_notes);

    /* 刷到介质再改名。fflush 只把数据交给 stdio，fclose 才交给文件系统 ——
     * 少这一步的话，rename 可能把一个还没落盘的文件换上去。 */
    if (fflush(f) != 0) {
        fclose(f);
        if (used_tmp)
            remove(TMP);
        return;
    }
    fclose(f);

    if (!used_tmp)
        return;                     /* 已经是正式路径，无需改名 */

    /* rename 失败（FAT 上有极小概率）就删掉临时的。
     * 宁可这一轮没有 status.txt，也不能留一个半截文件 ——
     * 半截文件比没有文件更坏：读方会把它当成"跑到了这里就停了"。 */
    if (rename(TMP, "sdmc:/3ds/aiconsole/status.txt") != 0)
        remove(TMP);
}
/* ---- 动作结果的回灌缓冲 ----
 *
 * 只有对话屏会读它，所以缓冲放在这里而不是散进各模块：
 * 写入方（确认屏）与读取方（对话屏）分处两个文件，中间只靠这一个入口，
 * 那一"清空"的动作才不会漏掉 —— 漏了的话，同一份结果会被反复发出去。
 */
static char g_act_result[ACT_RESULT_MAX];
static char g_act_event[ACT_RESULT_MAX];   /* 给界面看的那句（见 app_act_event_set） */

void app_act_result_store(const char *text)
{
    if (!text)
        return;
    snprintf(g_act_result, sizeof g_act_result, "%s", text);
}

const char *app_act_result_get(void)
{
    return g_act_result[0] ? g_act_result : NULL;
}

void app_act_result_clear(void)
{
    g_act_result[0] = '\0';
}

/* ---- 给**界面**看的那一句：动作后来怎么样了 ----
 *
 * ★ 为什么不能拿 g_act_result 顶替：那份是**写给模型看的**
 *   （"用户拒绝了……不要原样再发一遍"），把它直接摆在对话列表里，
 *   用户会看到一句对他说话的命令 —— 那是在自言自语。
 *   两句话的读者不同，就必须各写一份，凑合共用迟早露馅。
 *
 * 语义是"取走即清"：回到对话屏时显示一次就够了，
 * 反复出现会变成刷屏。 */
void app_act_event_set(const char *text)
{
    if (!text)
        return;
    snprintf(g_act_event, sizeof g_act_event, "%s", text);
}

const char *app_act_event_take(void)
{
    if (!g_act_event[0])
        return NULL;
    return g_act_event;
}

void app_act_event_clear(void)
{
    g_act_event[0] = '\0';
}


/* 字号档位 -> 渲染缩放百分比（2026-10-28）。
 * 放在 app 层而不是 config：config.c 里没有"当前配置"这个全局，
 * 它所有函数都是把 AppConfig* 当参数传的。 */
int app_text_scale(void)
{
    const AppConfig *c = app_cfg();
    const char      *v = c ? c->ui.font_size : "mid";

    /* ★ 只做**整数倍**（100 / 200）。
     *   150% 是最近邻的"非整数"放大：一个源像素要摊到 1~2 个目标像素上，
     *   笔画粗细忽粗忽细，整行字看起来像糊了 —— 实拍确认过，不能用。
     *   2 倍是 1 个源像素恰好 4 个目标像素，边缘干净。 */
    if (!strcmp(v, "large"))  return 200;
    if (!strcmp(v, "mid"))    return 100;   /* 中 = 原始大小（默认） */
    return 100;                              /* small 也是 100：点阵没法再小 */
}
