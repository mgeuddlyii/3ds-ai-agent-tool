/*
 * sc_plugins.c -- 插件页（3gx / Luma 插件）
 *
 * ── 为什么要有这一页 ──
 * 插件是**唯一能在"别的程序正在运行"时还活着**的形态（见 docs 的能力矩阵）。
 * 但它一直只能靠电脑往 luma/plugins/ 里拷贝，用户在 3DS 上既看不到装了
 * 什么、也关不掉任何一个 —— 装坏了只能拔卡去电脑上删。
 *
 * 这一页只做三件事：**看得到、能开关、能刷新**。刻意不做"删除"：
 * 禁用（改名 .off）已经能解决"它出问题"这个诉求，而删除是不可撤销的，
 * 在一个"插件崩了会连带游戏崩"的场景里不值得给一键删除。
 *
 * ── Luma 的插件路径（写死，不要"更聪明"地去猜）──
 *   sdmc:/luma/plugins/<TITLE_ID>/<name>.3gx   只对该游戏生效
 *   sdmc:/luma/plugins/<name>.3gx              对所有游戏生效（全局）
 *   <name>.3gx.off                             存在但被禁用
 *
 * ⚠️ 插件是否真的被加载，最终由 Rosalina 决定（L + 下 + SELECT）。
 *    这一页只能改卡上的文件状态，改不了 Rosalina 的运行时开关 ——
 *    所以页脚必须把这句话说出来，否则用户会以为"这里启用了就该生效"。
 */

#include <3ds.h>
#include <dirent.h>
#include <stdio.h>
#include <string.h>

#include "app.h"
#include "screen.h"
#include "uistr.h"
#include "widgets.h"

/* 一次最多显示这么多条。插件实际就几个，不做滚动 ——
 * 做滚动要引入"焦点不动、视图动"的另一套状态，为了十几条不值得。 */
#define PLUG_MAX 9
#define PLUG_DIR "sdmc:/luma/plugins"
#define PLUG_NAME 48

typedef struct {
    char rel[PLUG_NAME];    /* 相对 PLUG_DIR 的路径，如 "default.3gx" */
    char label[PLUG_NAME];  /* 显示用：文件名（含所属 Title ID 前缀） */
    bool on;                /* false = 名字以 .off 结尾 */
    bool global;            /* 直接在 plugins/ 下（全局） */
} PlugItem;

static PlugItem g_it[PLUG_MAX];
static int      g_n;
static int      g_sel;
static char     g_msg[96];

static bool ends_with(const char *s, const char *suf)
{
    size_t a = strlen(s), b = strlen(suf);
    return a >= b && strcmp(s + a - b, suf) == 0;
}

/* 把一个候选文件放进表里。rel 是相对路径，label 是显示名。
 *
 * .off 的判定只看**后缀**：Luma 认的就是这个约定，
 * 我们不去猜"文件里写了什么"，因为插件格式是二进制、没有开关字段。 */
static void add_one(const char *rel, const char *label, bool global)
{
    size_t n;

    if (g_n >= PLUG_MAX)
        return;
    n = strlen(rel);
    if (n == 0 || n >= PLUG_NAME)
        return;
    snprintf(g_it[g_n].rel, sizeof g_it[g_n].rel, "%s", rel);
    snprintf(g_it[g_n].label, sizeof g_it[g_n].label, "%s", label);
    g_it[g_n].on     = !ends_with(rel, ".off");
    g_it[g_n].global = global;
    g_n++;
}

static bool is_plug(const char *name)
{
    return ends_with(name, ".3gx") || ends_with(name, ".3gx.off");
}

/* 扫两级：plugins/ 下的文件（全局）+ plugins/<TID>/ 下的文件（按游戏）。 */
static void scan_plugins(void)
{
    DIR *d;
    struct dirent *e;

    g_n = 0;
    d = opendir(PLUG_DIR);
    if (!d)
        return;

    while ((e = readdir(d)) != NULL && g_n < PLUG_MAX) {
        char rel[PLUG_NAME];

        if (e->d_name[0] == '.')          /* . 和 .. 以及隐藏项 */
            continue;

        if (is_plug(e->d_name)) {
            add_one(e->d_name, e->d_name, true);
            continue;
        }

        /* 子目录 = Title ID。再扫一层。 */
        {
            DIR *sub;
            char path[96];
            struct dirent *se;

            snprintf(path, sizeof path, "%s/%s", PLUG_DIR, e->d_name);
            sub = opendir(path);
            if (!sub)
                continue;
            while ((se = readdir(sub)) != NULL && g_n < PLUG_MAX) {
                if (se->d_name[0] == '.' || !is_plug(se->d_name))
                    continue;
                snprintf(rel, sizeof rel, "%s/%s", e->d_name, se->d_name);
                /* 显示成 "ABC12345/x.3gx"：用户要能一眼看出这插件
                 * 挂在哪款游戏上，而 Title ID 是他唯一能在别处对上的东西。 */
                snprintf(g_it[g_n].label, sizeof g_it[g_n].label, "%s", rel);
                snprintf(g_it[g_n].rel, sizeof g_it[g_n].rel, "%s", rel);
                g_it[g_n].on     = !ends_with(se->d_name, ".off");
                g_it[g_n].global = false;
                g_n++;
            }
            closedir(sub);
        }
    }
    closedir(d);

    if (g_sel >= g_n)
        g_sel = (g_n > 0) ? g_n - 1 : 0;
}

static void plugins_enter(void)
{
    g_sel = 0;
    g_msg[0] = '\0';
    scan_plugins();
    app_note("plugins: scanned %d item(s)", g_n);
}

/* 启用/禁用：改名。这是**唯一**改文件的地方，而且是可逆的。
 *
 * 失败也要说清楚是哪个文件、为什么（文件被占用 / 目录不可写），
 * 否则用户只会看到"按了没反应"。 */
static void toggle(void)
{
    char from[128], to[128];

    if (g_n <= 0)
        return;

    snprintf(from, sizeof from, "%s/%s", PLUG_DIR, g_it[g_sel].rel);
    if (g_it[g_sel].on) {
        /* 启用 → 禁用：加 .off */
        snprintf(to, sizeof to, "%s.off", from);
    } else {
        /* 禁用 → 启用：去掉 .off */
        size_t n = strlen(from);
        snprintf(to, sizeof to, "%.*s", (int)(n - 4), from);
    }

    if (rename(from, to) != 0) {
        snprintf(g_msg, sizeof g_msg, "%s", S(S0886));
        app_status_set("%s", g_msg);
        app_note("plugins: rename failed %s", from);
        return;
    }

    app_note("plugins: %s -> %s", from, to);
    scan_plugins();
    app_status_set("%s", S(S0887));
}

static void render_top(void)
{
    w_bot_header(S(S0881));

    /* 顶屏放"为什么需要 Rosalina"这一句 —— 它是这一页唯一会让用户
     * 困惑的地方：文件明明启用了，进游戏却没反应。 */
    /* 用 w_wrap：这句英文 99 字符 ≈ 792px，单行会被右边缘裁掉；
     * 中文只有 40 字放得下 —— 这类"只有英文溢出"的地方，
     * 中文模式下永远发现不了。 */
    w_wrap((float)W_TOP_MARGIN, (float)W_TOP_CONTENT_Y,
           (float)W_TOP_USABLE, S(S0884), w_c(C_SLOT_FG), TEXT_LINE_H);

    w_top_footer(app_status(), S(S0885));
}

static void render_bottom(void)
{
    float y = (float)W_BOT_CONTENT_Y;
    const float x = (float)W_BOT_MARGIN;
    const float w = (float)W_BOT_USABLE;

    w_bot_header(S(S0881));

    if (g_n == 0) {
        y = w_line(x, y, w, S(S0882), w_c(C_SLOT_MUTED));   /* 空状态：没有插件 */
        w_bot_footer(S(S0885));
        return;
    }

    for (int i = 0; i < g_n; ++i) {
        char line[96];
        const char *mark = g_it[i].on ? "on " : "off";

        /* 状态放在最前面并用同样的宽度 —— 一列对齐比"读到句尾才知道状态"好扫。 */
        /* 不做"全局"标记：default.3gx 在根目录、按游戏的带 TitleID 前缀，
         * 从路径本身就看得出来，再缀一个词反而让 144px 的行更容易溢出。 */
        (void)mark;
        snprintf(line, sizeof line, "[%s] %s", g_it[i].on ? "ON " : "OFF",
                 g_it[i].label);
        y = w_line(x + (i == g_sel ? 0.0f : 0.0f), y, w, line,
                   i == g_sel ? w_c(C_SLOT_OK) : w_c(C_SLOT_FG));
        if (y > 200.0f)
            break;
    }

    if (g_msg[0])
        w_line(x, 196.0f, w, g_msg, w_c(C_SLOT_WARN_DK));

    w_bot_footer(S(S0883));
}

static void plugins_handle(u32 down, u32 held)
{
    (void)held;

    if (down & KEY_B) {
        app_pop();
        return;
    }
    if (down & KEY_Y) {
        g_msg[0] = '\0';
        scan_plugins();
        app_status_set("%s", S(S0887));
        return;
    }
    if (down & KEY_UP) {
        if (g_sel > 0) --g_sel;
        return;
    }
    if (down & KEY_DOWN) {
        if (g_sel + 1 < g_n) ++g_sel;
        return;
    }
    if (down & KEY_A) {
        toggle();
        return;
    }
}

const Screen g_screen_plugins = {
    .id            = SCREEN_PLUGINS,
    .name          = "SCREEN_PLUGINS",
    .enter         = plugins_enter,
    .leave         = NULL,
    .render_top    = render_top,
    .render_bottom = render_bottom,
    .handle        = plugins_handle,
};