/*
 * sc_plugins.c -- 插件页（3gx / Luma 插件）
 *
 * ── 为什么要有这一页 ──
 * 插件是**唯一能在"别的程序正在运行"时还活着**的形态（见 docs 的能力矩阵）。
 * 但它一直只能靠电脑往 luma/plugins/ 里拷贝，用户在 3DS 上既看不到装了
 * 什么、也关不掉任何一个 —— 装坏了只能拔卡去电脑上删。
 *
 * ── 2026-11-14 重大调整：插件的**一切**都收到这一页 ──
 * 原来插件的面板里有「翻译/设置/说明/测试」四个按钮，设置页、说明页、
 * 拖动摆放页都做在插件自己里面。用户明确指出这不对：
 *
 *     插件只负责**显示文字**；能不能用、长什么样、在哪显示，
 *     全部在 AIConsole 的「更多 → 插件」页里改。
 *
 * 所以现在这一页是插件的**唯一配置入口**：
 *   A 进入某个插件的子页 → 启用/设置/说明/安装教程
 *
 * ── 数据落在哪 ──
 *   插件文件：sdmc:/luma/plugins/<TID>/<name>.3gx
 *   插件设置：sdmc:/3ds/aiconsole/plugin.ini   ← 这一页写，插件读
 * 设置**不**写进 config.json：那份是主程序的配置，插件进程读它要多背
 * 一整套 JSON 解析；而 plugin.ini 是极简 key=value，插件那边已经在用了。
 *
 * ⚠️ 插件是否真的被加载，最终由 Rosalina 决定（L + 下 + SELECT）。
 *    这一页只能改卡上的文件状态，改不了 Rosalina 的运行时开关 ——
 *    所以页脚必须把这句话说出来，否则用户会以为"这里启用了就该生效"。
 */

#include <3ds.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
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
#define PLUG_INI "sdmc:/3ds/aiconsole/plugin.ini"

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

/* ---- 子页状态 -------------------------------------------------------- */
#define PM_LIST    0
#define PM_DETAIL  1
#define PM_CFG     2
#define PM_HELP    3
#define PM_INSTALL 4

#define DETAIL_ROWS 4
#define CFG_ROWS    7

static int g_mode = PM_LIST;
static int g_row;

/* plugin.ini 的镜像。字段与 plugin/translate/cfg.c 一一对应，
 * 名字/取值必须**完全一致**，否则插件读不出来（那边按字面值比较）。 */
static struct {
    int screen;     /* 0=下屏 1=上屏 */
    int anchor;     /* 0..8 九宫格 */
    int cols;       /* 面板宽（字符列） */
    int rows;       /* 面板高（字符行） */
    int opacity;    /* 0..3 背板深浅 */
    int trigger;    /* 触发键组合下标 0..7 */
    int beta_pause; /* Beta：显示时暂停游戏 */
} g_pc;

static const char *const k_anchor[9] = {
    "top-left",  "top-center",  "top-right",
    "mid-left",  "center",      "mid-right",
    "bottom-left", "bottom-center", "bottom-right",
};

static const char *const k_anchor_cn[9] = {
    "左上", "上中", "右上",
    "左中", "居中", "右中",
    "左下", "下中", "右下",
};

static const char *const k_trigger[8] = {
    "L+R+SELECT", "L+R+START", "L+R+X", "L+R+Y",
    "L+R+A", "L+R+B", "L+R+上", "SELECT",
};

static const char *const k_screen_cn[2] = { "下屏", "上屏" };

/* 说明 / 安装教程的正文。
 * ⚠️ 这几行是**新加的界面文字**，还没走 i18n 三道闸（见 NOTES）。
 *    英文模式下它们仍显示中文 —— 已知，待补英文后一并生成表。 */
static const char *const k_help[] = {
    "按快捷键唤出翻译面板，再按一次收起。",
    "面板只显示文字，不能在这里改任何东西。",
    "",
    "要改显示屏、位置、宽高、深浅、触发键，",
    "请到这里：更多 → 插件 → 选中插件 → 设置。",
    "",
    "翻译要用能读图的模型（名字带 VL / Vision）。",
    "在 Key 管理里加一把视觉模型的 Key 再选它。",
    NULL,
};

static const char *const k_install[] = {
    "1. 把 .3gx 放到卡上：",
    "   luma/plugins/<TitleID>/名字.3gx",
    "   （对所有游戏生效就放 luma/plugins/）",
    "",
    "2. 开机进 Rosalina：L + 下 + SELECT",
    "   打开 Plugin Loader 开关",
    "",
    "3. 回到这一页，确认对应插件是 ON",
    "",
    "4. 进游戏按快捷键，面板出现即成功",
    "",
    "加载成功时开机 logo 会闪一下颜色。",
    NULL,
};

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

/* ---- plugin.ini 读写 -------------------------------------------------
 *
 * 为什么不让插件自己写、这里只读：用户要求"设置必须在软件里改"，
 * 所以写入口在这一页。插件那边删掉了设置页，只保留读取。
 *
 * 格式是极简 key=value（一行一个，# 开头是注释）—— 插件里没有 JSON
 * 解析器，也不该为了六个字段引一个。 */

static void pc_clamp(void)
{
    if (g_pc.screen < 0 || g_pc.screen > 1)        g_pc.screen = 0;
    if (g_pc.anchor < 0 || g_pc.anchor > 8)        g_pc.anchor = 6;
    if (g_pc.cols < 10) g_pc.cols = 10;
    if (g_pc.cols > 50) g_pc.cols = 50;
    if (g_pc.rows < 2)  g_pc.rows = 2;
    if (g_pc.rows > 16) g_pc.rows = 16;
    if (g_pc.opacity < 0 || g_pc.opacity > 3)      g_pc.opacity = 2;
    if (g_pc.trigger < 0 || g_pc.trigger > 7)      g_pc.trigger = 0;
    g_pc.beta_pause = g_pc.beta_pause ? 1 : 0;
}

static void pc_defaults(void)
{
    g_pc.screen = 0;
    g_pc.anchor = 6;          /* 左下 */
    g_pc.cols   = 34;
    g_pc.rows   = 7;
    g_pc.opacity = 2;
    g_pc.trigger = 0;         /* L+R+SELECT */
    g_pc.beta_pause = 0;
}

static void pc_load(void)
{
    FILE *f;
    char  line[128];

    pc_defaults();
    f = fopen(PLUG_INI, "r");
    if (!f) {
        pc_clamp();
        return;
    }

    while (fgets(line, sizeof line, f)) {
        char *eq, *v;
        int   n;

        if (line[0] == '#' || line[0] == '\n')
            continue;
        eq = strchr(line, '=');
        if (!eq)
            continue;
        *eq = '\0';
        v = eq + 1;
        n = (int)strlen(v);
        while (n > 0 && (v[n - 1] == '\n' || v[n - 1] == '\r' || v[n - 1] == ' '))
            v[--n] = '\0';

        if (!strcmp(line, "screen"))            g_pc.screen = !strcmp(v, "top");
        else if (!strcmp(line, "anchor")) {
            for (int i = 0; i < 9; ++i)
                if (!strcmp(v, k_anchor[i]))
                    g_pc.anchor = i;
        }
        else if (!strcmp(line, "cols"))         g_pc.cols = atoi(v);
        else if (!strcmp(line, "rows"))         g_pc.rows = atoi(v);
        else if (!strcmp(line, "opacity"))      g_pc.opacity = atoi(v);
        else if (!strcmp(line, "trigger"))      g_pc.trigger = atoi(v);
        else if (!strcmp(line, "beta_pause"))   g_pc.beta_pause = atoi(v);
        /* 认不出的键一律忽略：老版本文件里有 key=/model=/api_base=，
         * 那些已经不用了，读到只当没看见，**不要**因此报错。 */
    }
    fclose(f);
    pc_clamp();
}

static void pc_save(void)
{
    FILE *f = fopen(PLUG_INI, "w");

    if (!f) {
        snprintf(g_msg, sizeof g_msg, "设置写不进去（卡只读？）");
        app_status_set("%s", g_msg);
        return;
    }
    fprintf(f, "# AIConsole 翻译插件设置（在 更多→插件→设置 里改）\n");
    fprintf(f, "screen=%s\n",     g_pc.screen ? "top" : "bottom");
    fprintf(f, "anchor=%s\n",     k_anchor[g_pc.anchor]);
    fprintf(f, "cols=%d\n",       g_pc.cols);
    fprintf(f, "rows=%d\n",       g_pc.rows);
    fprintf(f, "opacity=%d\n",    g_pc.opacity);
    fprintf(f, "trigger=%d\n",    g_pc.trigger);
    fprintf(f, "beta_pause=%d\n", g_pc.beta_pause);
    fclose(f);
    snprintf(g_msg, sizeof g_msg, "已保存，进游戏生效");
    app_status_set("%s", g_msg);
    app_note("plugins: saved %s", PLUG_INI);
}

static void plugins_enter(void)
{
    g_sel  = 0;
    g_row  = 0;
    g_mode = PM_LIST;
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

/* ---- 绘制 ------------------------------------------------------------ */

/* 一行"卡片"：面板底 + 边框 + 里面一行字。
 *
 * 为什么不用 w_line 直接排：用户明确要求"要做成卡片状"。w_line 只画字，
 * 行与行之间没有边界，一屏七行看起来是一坨文字；有了卡片的边框和底色，
 * 每一行是一个可点的对象，选中态也有地方体现（边框换成强调色）。
 *
 * 返回下一行的 y。 */
static float card(float x, float y, float w, const char *text, bool sel, RColor fg)
{
    Rect r;
    const float h = 20.0f;

    r.x = x;
    r.y = y;
    r.w = w;
    r.h = h;
    r.z = 0.0f;
    w_panel(&r, w_c(sel ? C_SLOT_PANEL : C_SLOT_BG),
                w_c(sel ? C_SLOT_ACCENT : C_SLOT_LINE));
    w_line(x + 8.0f, y + 1.0f, w - 16.0f, text, fg);
    return y + h + 3.0f;
}

static void render_top(void)
{
    /* 说明 / 安装教程**显示在上屏**。
     *
     * 为什么：Rosalina 菜单（L + 下 + SELECT）是弹在**下屏**的，而安装教程
     * 讲的正是"进 Rosalina 打开 Plugin Loader"。如果教程自己也占着下屏，
     * 用户就得先记住步骤再切过去 —— 放在上屏才能一边看一边操作。 */
    if (g_mode == PM_HELP || g_mode == PM_INSTALL) {
        const char *const *lines = (g_mode == PM_HELP) ? k_help : k_install;
        float y = (float)W_TOP_CONTENT_Y;

        for (int i = 0; lines[i] && y < 190.0f; ++i)
            y = w_line((float)W_TOP_MARGIN, y, (float)W_TOP_USABLE,
                       lines[i], w_c(C_SLOT_FG));

        w_top_footer(app_status(), "B 返回");
        return;
    }

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

static void render_list(float x, float w, float y)
{
    if (g_n == 0) {
        w_line(x, y, w, S(S0882), w_c(C_SLOT_MUTED));
        return;
    }

    for (int i = 0; i < g_n; ++i) {
        char line[96];

        /* 状态放在最前面并用同样的宽度 —— 一列对齐比"读到句尾才知道状态"好扫。 */
        snprintf(line, sizeof line, "[%s] %s", g_it[i].on ? "ON " : "OFF",
                 g_it[i].label);
        y = card(x, y, w, line, i == g_sel,
                 i == g_sel ? w_c(C_SLOT_OK) : w_c(C_SLOT_FG));
        if (y > 190.0f)
            break;
    }
}

static void render_detail(float x, float w, float y)
{
    const char *rows[DETAIL_ROWS];
    char        buf[64];

    snprintf(buf, sizeof buf, "启用状态    %s", g_it[g_sel].on ? "ON" : "OFF");
    rows[0] = buf;
    rows[1] = "设置        >";
    rows[2] = "说明        >";
    rows[3] = "安装教程    >";

    /* 标题：告诉用户当前在改哪个插件 */
    y = w_line(x, y, w, g_it[g_sel].label, w_c(C_SLOT_MUTED));
    y += 6.0f;

    for (int i = 0; i < DETAIL_ROWS; ++i)
        y = card(x, y, w, rows[i], i == g_row,
                 i == g_row ? w_c(C_SLOT_OK) : w_c(C_SLOT_FG));
}

static void render_cfg(float x, float w, float y)
{
    char        buf[CFG_ROWS][64];
    int         i;

    snprintf(buf[0], 64, "显示屏      %s", k_screen_cn[g_pc.screen]);
    snprintf(buf[1], 64, "位置        %s", k_anchor_cn[g_pc.anchor]);
    snprintf(buf[2], 64, "宽度        %d 列", g_pc.cols);
    snprintf(buf[3], 64, "高度        %d 行", g_pc.rows);
    snprintf(buf[4], 64, "背板深浅    %d", g_pc.opacity);
    snprintf(buf[5], 64, "触发键      %s", k_trigger[g_pc.trigger]);
    snprintf(buf[6], 64, "暂停游戏    %s", g_pc.beta_pause ? "开(Beta)" : "关");

    y = w_line(x, y, w, "←/→ 改，A 保存，B 放弃", w_c(C_SLOT_MUTED));
    y += 6.0f;

    for (i = 0; i < CFG_ROWS; ++i) {
        if (y > 190.0f)
            break;
        y = card(x, y, w, buf[i], i == g_row,
                 i == g_row ? w_c(C_SLOT_OK) : w_c(C_SLOT_FG));
    }
}

static void render_bottom(void)
{
    float y = (float)W_BOT_CONTENT_Y;
    const float x = (float)W_BOT_MARGIN;
    const float w = (float)W_BOT_USABLE;

    /* 说明 / 安装教程的正文在**上屏**，下屏只留一句"看上面"，
     * 免得用户在下屏找内容。 */
    if (g_mode == PM_HELP || g_mode == PM_INSTALL) {
        w_bot_header(S(S0881));
        w_line(x, y, w, "内容在上屏 ↑", w_c(C_SLOT_MUTED));
        w_bot_footer("B 返回");
        return;
    }

    w_bot_header(S(S0881));

    switch (g_mode) {
    case PM_DETAIL:
        if (g_n > 0) render_detail(x, w, y);
        break;
    case PM_CFG:
        render_cfg(x, w, y);
        break;
    default:
        render_list(x, w, y);
        break;
    }

    if (g_msg[0])
        w_line(x, 196.0f, w, g_msg, w_c(C_SLOT_WARN_DK));

    w_bot_footer(S(S0883));
}

/* ---- 输入 ------------------------------------------------------------ */

static void handle_list(u32 down)
{
    if (down & KEY_UP) {
        if (g_sel > 0) --g_sel;
        return;
    }
    if (down & KEY_DOWN) {
        if (g_sel + 1 < g_n) ++g_sel;
        return;
    }
    if (down & KEY_A) {
        if (g_n > 0) {
            g_mode = PM_DETAIL;      /* 不再直接切换，先进子页 */
            g_row  = 0;
            g_msg[0] = '\0';
        }
        return;
    }
}

static void handle_detail(u32 down)
{
    if (down & KEY_UP)   { if (g_row > 0) --g_row; return; }
    if (down & KEY_DOWN) { if (g_row + 1 < DETAIL_ROWS) ++g_row; return; }
    if (!(down & KEY_A))  return;

    switch (g_row) {
    case 0:                       /* 启用/禁用 */
        toggle();
        break;
    case 1:                       /* 设置 */
        pc_load();
        g_mode = PM_CFG;
        g_row  = 0;
        break;
    case 2:
        g_mode = PM_HELP;
        break;
    case 3:
        g_mode = PM_INSTALL;
        break;
    default:
        break;
    }
}

static void handle_cfg(u32 down)
{
    int d = 0;

    if (down & KEY_LEFT)  d = -1;
    if (down & KEY_RIGHT) d = +1;

    if (down & KEY_DOWN) { if (g_row + 1 < CFG_ROWS) ++g_row; return; }
    if (down & KEY_UP)   { if (g_row > 0) --g_row; return; }

    if (d != 0) {
        switch (g_row) {
        case 0: g_pc.screen ^= 1; break;
        case 1: g_pc.anchor = (g_pc.anchor + (d > 0 ? 1 : 8)) % 9; break;
        case 2: g_pc.cols += d * 2; break;
        case 3: g_pc.rows += d; break;
        case 4: g_pc.opacity += d; break;
        case 5: g_pc.trigger = (g_pc.trigger + (d > 0 ? 1 : 7)) % 8; break;
        case 6: g_pc.beta_pause = !g_pc.beta_pause; break;
        default: break;
        }
        pc_clamp();
        return;
    }

    if (down & KEY_A) {
        pc_save();
        g_mode = PM_DETAIL;
        g_row  = 1;
    }
}

static void plugins_handle(u32 down, u32 held)
{
    (void)held;

    if (down & KEY_B) {
        switch (g_mode) {
        case PM_LIST:
            app_pop();
            return;
        case PM_CFG:
            /* 从设置返回不算保存：和"改了才保存"的直觉一致，
             * 用户按 B 就是放弃。
             */
            g_mode = PM_DETAIL;
            g_row  = 1;
            return;
        default:
            g_mode = PM_LIST;
            return;
        }
    }

    if (down & KEY_Y) {
        if (g_mode == PM_LIST) {
            g_msg[0] = '\0';
            scan_plugins();
            app_status_set("%s", S(S0887));
        }
        return;
    }

    switch (g_mode) {
    case PM_DETAIL:  handle_detail(down); break;
    case PM_CFG:     handle_cfg(down);    break;
    case PM_HELP:
    case PM_INSTALL:                      /* 文字页只响应 B（上面已处理） */
        break;
    default:         handle_list(down);   break;
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
