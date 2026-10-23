/*
 * sc_dock.c -- 下屏滑轨（Dock）与四个面板
 *
 * ── 这是什么 ──
 * 下屏底部一条四格滑轨：键盘 / 项目 / 文件 / 浏览。
 * 点一格，上方的面板就换成那一格的内容。滑轨**常驻**，切面板不换屏。
 *
 * ── 为什么做成"一屏 + 四个面板"而不是四个屏 ──
 * 四件事的使用节奏是"来回切"的：打一句话 → 看看文件 → 切个模型 → 再打一句。
 * 做成四个独立屏的话，每次切换都要 push/pop 一次，返回栈越叠越深，
 * 而且用户会丢失"我刚才在文件列表选到第几行"这种上下文。
 * 一个常驻滑轨 + 面板切换，切的是内容不是屏幕 —— 每个面板的状态各自保留。
 *
 * ── 排版为什么这么算 ──
 * 下屏 320×240，可用宽 W_BOT_USABLE=300（左右各 10 边距）。
 *   内容区   y 24 .. 166
 *   滑轨     y 172 .. 212（高 40）
 *   提示行   y 218 起
 * 四格：每格 72 宽、间隔 4 → 4*72 + 3*4 = 300，正好铺满。
 * ★ 滑轨矩形和面板矩形都由**同一个函数**算（rail_rect / row_rect），
 *   绘制与命中判定同源，不会出现"看着能点、点不到"。
 *
 * ── 键盘这一格为什么先接系统软键盘 ──
 * 3DS 的系统软键盘**没有拼音输入法**（日版/美版固件都没有）。
 * 所以"中文输入"不是一个开关，而是要自己写一个拼音输入法（词库 + 候选 +
 * 自绘键盘）。那是一个独立模块（shared/ime.*），不在这一屏里硬塞 ——
 * 塞进去的结果是两边都做不完整。这一格现在做的是：
 *   · 调系统键盘输入（英文/数字/符号，可用）
 *   · 把结果送到对话输入框
 * 中文输入走同屏的语音入口，或等 IME 模块接上。
 */

#include <3ds.h>
#include <stdio.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>

#include "app.h"
#include "screen.h"
#include "uistr.h"
#include "widgets.h"
#include "html.h"
#include "input.h"
#include "ime.h"
#include "dock_ui.h"

#include "projects.h"
#include "provider.h"
#include "actions.h"
#include "keystore.h"

/* ------------------------------------------------------------------ 布局 */

#define RAIL_Y      DOCKUI_RAIL_Y
#define RAIL_H      DOCKUI_RAIL_H
#define RAIL_GAP    DOCKUI_RAIL_GAP
#define RAIL_W      DOCKUI_RAIL_W

#define PANEL_Y0     46.0f
#define PANEL_BOT   205.0f

#define ROW_H        24.0f
#define ROW_GAP       2.0f
#define ROW_Y0       54.0f

enum { PANEL_KEY = 0, PANEL_PROJ, PANEL_FILE, PANEL_BROWSE, PANEL_N };

static int  g_panel = PANEL_KEY;
static int  g_sel   = 0;          /* 各面板共用一个"当前行"，切面板时重置 */

/* ------------------------------------------------------------------ 键盘面板 */
#define DOCK_TEXT_MAX 256
static char g_text[DOCK_TEXT_MAX];
/* 拼音键盘是否展开。展开时整个下屏让给键盘，候选与字母键都在这里。 */
static bool g_ime;
/* 候选翻页。一屏 6 个，但一个音节最多 16 个候选，
 * 不翻页的话后面的候选**永远够不着**（电脑 就排在第 7 位）。 */
static int  g_cpage;

/* ------------------------------------------------------------------ 文件面板 */
#define DOCK_FILE_MAX 24
typedef struct {
    char     name[64];
    unsigned size;
    int      is_dir;
} DockFile;

static DockFile g_files[DOCK_FILE_MAX];
static int      g_fn;

/* ------------------------------------------------------------------ 浏览面板 */
static HtmlDoc g_doc;
static int     g_top;
static bool    g_loaded;
static char    g_docname[64];

/* ------------------------------------------------------------------ 矩形 */

static Rect rail_rect(int i)
{
    Rect r;
    r.x = (float)W_BOT_MARGIN + (float)i * (RAIL_W + RAIL_GAP);
    r.y = RAIL_Y;
    r.w = RAIL_W;
    r.h = RAIL_H;
    return r;
}

static Rect row_rect(int i)
{
    Rect r;
    r.x = (float)W_BOT_MARGIN;
    r.y = ROW_Y0 + (float)i * (ROW_H + ROW_GAP);
    r.w = (float)W_BOT_USABLE;
    r.h = ROW_H;
    return r;
}

/* 内容区能放几行 */
#define ROW_CAP ((int)((PANEL_BOT - ROW_Y0) / (ROW_H + ROW_GAP)))

/* ------------------------------------------------------------------ 文件列表 */

static void scan_files(void)
{
    DIR *d;
    struct dirent *e;
    const char *base = actions_dir();

    g_fn = 0;
    d = opendir(base);
    if (!d)
        return;

    while ((e = readdir(d)) != NULL && g_fn < DOCK_FILE_MAX) {
        struct stat st;
        char full[192];

        if (e->d_name[0] == '.')
            continue;                    /* .trash / . / .. 都不列 */

        snprintf(full, sizeof full, "%s/%s", base, e->d_name);
        if (stat(full, &st) != 0)
            continue;

        snprintf(g_files[g_fn].name, sizeof g_files[g_fn].name,
                 "%s", e->d_name);
        g_files[g_fn].size   = (unsigned)st.st_size;
        g_files[g_fn].is_dir = S_ISDIR(st.st_mode);
        ++g_fn;
    }
    closedir(d);
}

static void open_doc(const char *name)
{
    char path[192];

    snprintf(path, sizeof path, "%s/%s", actions_dir(), name);
    g_loaded = html_parse_file(path, &g_doc);
    g_top = 0;
    snprintf(g_docname, sizeof g_docname, "%s", name);

    if (g_loaded && g_doc.count > 0) {
        g_panel = PANEL_BROWSE;
        g_sel = 0;
        app_status_set("%s · %d 行", g_docname, g_doc.count);
    } else {
        app_status_set("打不开 %s（不是文本 / 空文件）", name);
    }
}

/* 删到 .trash 而不是真删 —— 与 file.delete 动作同一个语义：
 * 用户点错了还能从电脑上捞回来。 */
static void trash_file(const char *name)
{
    char src[192], dst[192];

    snprintf(src, sizeof src, "%s/%s", actions_dir(), name);
    snprintf(dst, sizeof dst, "%s/.trash/%s", actions_dir(), name);

    if (rename(src, dst) == 0) {
        app_status_set("已移到回收站：%s", name);
        app_note("dock: trash %s", name);
    } else {
        /* 目录可能不存在 —— 建一次再试。 */
        char tdir[192];
        snprintf(tdir, sizeof tdir, "%s/.trash", actions_dir());
        mkdir(tdir, 0777);
        if (rename(src, dst) == 0) {
            app_status_set("已移到回收站：%s", name);
            app_note("dock: trash %s (created .trash)", name);
        } else {
            app_status_set("移不动 %s（可能被占用）", name);
        }
    }
    scan_files();
}

/* ------------------------------------------------------------------ 项目面板 */

/* 切换项目 = 把它的 provider/model 写进 cfg 并落盘。
 * 这一层必须留在 app 侧：shared/projects.c 不依赖 AppConfig（见 projects.h）。 */
static void apply_project(int i)
{
    const Project *p = projects_at(i);
    AppConfig *c = app_cfg();

    if (!p || !c)
        return;

    if (!projects_select(i)) {
        app_status_set("项目已切换，但写卡失败（下次开机可能回退）");
        app_note("dock: project %d select save FAILED", i);
    }

    snprintf(c->ai.provider, sizeof c->ai.provider, "%s", p->provider);
    snprintf(c->ai.model, sizeof c->ai.model, "%s", p->model);
    config_save(c);

    app_status_set("当前项目：%s（%s）", p->name, p->provider);
    app_note("dock: project %d -> %s / %s", i, p->provider, p->model);
}

/* 把当前项目的服务商切到下一条（provider 表是编译期常量，见 provider.h）
 *
 * ★ 现在任务面板的 X 走的是 cycle_task_api（只在**已添加的 API** 里转），
 *   这个函数保留作备用：它的语义是"遍历全部服务商（不管有没有 Key）"，
 *   有时用户就是想先切过去、再被带去配 Key（服务目录那条路就是这心思）。 */
__attribute__((unused))
static void cycle_project_provider(int i)
{
    const Project *p = projects_at(i);
    const ProviderDef *cur;
    int n = 0, idx = 0;

    if (!p)
        return;

    while (provider_at(n))
        ++n;
    if (n == 0) {
        app_status_set("没有可选服务商");
        return;
    }

    cur = provider_find(p->provider);
    if (cur) {
        for (int k = 0; k < n; ++k) {
            if (provider_at(k) == cur) { idx = k; break; }
        }
    }
    idx = (idx + 1) % n;

    {
        const ProviderDef *nx = provider_at(idx);
        projects_set_ai(i, nx->name, nx->default_model ? nx->default_model : "");
        if (i == projects_active())
            apply_project(i);            /* 改的是当前项 → 立刻生效 */
        app_status_set("%s → %s", p->name, nx->name);
    }
}

/* --------------------------------------------------------------------------
 * 任务 ⇄ 已添加的 API
 *
 * ★ 用户的原话是"每个任务用不同的模型，而且模型是**已添加的 API**"。
 *   所以模型不是让用户在一个长列表里翻 provider.default_model，
 *   而是**只从已经存了 Key 的那几条里挑** —— 没 Key 的选了也发不出去。
 *
 * 数据流：
 *   keystore 条目 = { name, provider, model, key }   （keys.json，用户已添加的）
 *   任务(project) = { name, provider, model }        （projects.txt，绑定到某条 API）
 *
 * ★ 为什么任务里存 provider+model 而不是存 keystore 的条目名：
 *   keystore 的条目可以被删/改名，任务存名字就会在下次开机变成一个
 *   指不到人的引用（而且没有提示）。存 provider+model 是**自包含**的，
 *   Key 由 keystore 的"激活条目"统一提供 —— 只要那家的 Key 还在，任务就能用。
 * ----------------------------------------------------------------------- */

/* 已添加的 API 条目数（只数"真的有 Key"的 —— 空 Key 条目是废条目）。 */
static int api_ready_count(void)
{
    int n = 0;
    for (int i = 0; i < keystore_count(); ++i) {
        const KeyEntry *e = keystore_at(i);
        if (e && e->key[0])
            ++n;
    }
    return n;
}

/* 第 k 个"可用"条目（k 是 0..api_ready_count()-1）。没有返回 NULL。 */
static const KeyEntry *api_ready_at(int k)
{
    for (int i = 0; i < keystore_count(); ++i) {
        const KeyEntry *e = keystore_at(i);
        if (e && e->key[0]) {
            if (k == 0)
                return e;
            --k;
        }
    }
    return NULL;
}

/* ★ 把一个任务切到"下一个已添加的 API"。
 *   这一步同时改 provider、model，并把 keystore 的激活条目指过去 ——
 *   三件事必须一起做：只改 provider 不改激活条目的话，对话屏进屏时
 *   「keystore 激活 > config」的优先级会把这次切换静默盖掉。
 *   （sc_chat.c 的 chat_enter 里那段注释就是讲这个坑的。） */
static void task_set_api(int proj_idx, const KeyEntry *e)
{
    const Project *p = projects_at(proj_idx);

    if (!p || !e)
        return;

    projects_set_ai(proj_idx, e->provider, e->model[0] ? e->model : "");
    keystore_set_active(e->name);

    if (proj_idx == projects_active())
        apply_project(proj_idx);

    app_status_set("任务「%s」→ %s / %s", p->name, e->provider,
                   e->model[0] ? e->model : "(默认)");
    app_note("dock: task %d api -> %s (%s/%s)", proj_idx, e->name,
             e->provider, e->model);
}

/* 切到下一个可用 API 条目。没有可用条目时明确报出来，
 * 并且**引导去加一个** —— 只说"没有"用户不知道下一步该干什么。 */
static void cycle_task_api(int proj_idx, int delta)
{
    const Project *p = projects_at(proj_idx);
    int n = api_ready_count();
    int cur = -1;

    if (!p)
        return;

    if (n <= 0) {
        app_status_set("还没有添加过 API —— 去「更多 → API 管理」加一个");
        app_note("dock: cycle api, none added (keystore=%d)", keystore_count());
        return;
    }

    /* 找当前条目：provider 和 model 都对得上才算（同 provider 可以有
     * 多条不同模型的条目，比如 deepseek-chat 和 deepseek-reasoner）。 */
    for (int k = 0; k < n; ++k) {
        const KeyEntry *e = api_ready_at(k);
        if (e && strcmp(e->provider, p->provider) == 0 &&
            strcmp(e->model, p->model) == 0) {
            cur = k;
            break;
        }
    }

    /* 找不到就从头开始（delta 为负时从末尾开始，保持"往回切"的直觉） */
    if (cur < 0)
        cur = (delta > 0) ? -1 : 0;

    cur = (cur + delta + n * 2) % n;
    task_set_api(proj_idx, api_ready_at(cur));
}

/* 改任务名。名字只是标签，不影响请求 —— 但它是**找回自己那件事**的唯一线索，
 * 所以必须能改（用户原话："任务可以更改任务"）。 */
static void rename_task(int proj_idx)
{
    const Project *p = projects_at(proj_idx);
    char buf[PROJ_NAME_MAX + 1];

    if (!p)
        return;

    snprintf(buf, sizeof buf, "%s", p->name);
    if (!ui_input_text("任务名字", buf, PROJ_NAME_MAX - 1, buf, sizeof buf)) {
        app_status_set("没改名");
        return;
    }
    if (!buf[0]) {
        app_status_set("名字不能为空");
        return;
    }

    /* projects_set_ai 只改 provider/model，名字要单独写。
     * projects.c 没有改名接口 —— 用"删+加"会打乱 active 下标，
     * 所以这里直接补一个改名的接口太绕；项目表本身就是内存里的数组，
     * 通过 remove/add 会丢顺序。因此用 set_ai 的姊妹函数（见 projects.h）。 */
    if (projects_rename(proj_idx, buf)) {
        if (proj_idx == projects_active())
            apply_project(proj_idx);
        app_status_set("已改名为「%s」", buf);
        app_note("dock: task %d rename -> %s", proj_idx, buf);
    } else {
        app_status_set("改名写卡失败");
    }
}

/* 新建任务：名字 + 默认绑定"当前正在用的那条 API"。
 * ★ 为什么默认绑当前 API：用户新建任务时多半想"还是用这套 API，只是换个用途"。
 *   一上来就让他再选一次模型是多余的动作，之后按 X 就能换。 */
static void new_task(void)
{
    AppConfig *c = app_cfg();
    char name[PROJ_NAME_MAX + 1];
    int  i;

    if (projects_count() >= PROJ_MAX) {
        app_status_set("最多 %d 个任务，先删一个", PROJ_MAX);
        return;
    }

    snprintf(name, sizeof name, "任务%d", projects_count() + 1);
    if (!ui_input_text("新任务名字", name, PROJ_NAME_MAX - 1, name, sizeof name)) {
        app_status_set("已取消");
        return;
    }
    if (!name[0]) {
        app_status_set("名字不能为空");
        return;
    }

    i = projects_add(name,
                     c ? c->ai.provider : "deepseek",
                     c ? c->ai.model : "");
    if (i < 0) {
        app_status_set("新建失败（写卡）");
        return;
    }

    /* 新建的立刻切过去 —— 用户刚起完名字，期待的就是"现在在这个任务里"。 */
    g_sel = i;
    apply_project(i);
    app_status_set("已新建任务「%s」（X 换模型）", name);
    app_note("dock: task add %d -> %s", i, name);
}

/* ------------------------------------------------------------------ 渲染 */

static const char *panel_name(int p)
{
    switch (p) {
        case PANEL_KEY:    return "键盘";
        case PANEL_PROJ:   return "项目";
        case PANEL_FILE:   return "文件";
        case PANEL_BROWSE: return "浏览";
        default:           return "?";
    }
}

static void render_top(void)
{
    float y = (float)W_TOP_CONTENT_Y;

    w_bot_header(panel_name(g_panel));

    switch (g_panel) {
    case PANEL_KEY:
        y = w_wrap((float)W_TOP_MARGIN, y, (float)W_TOP_USABLE,
                   "输入的文字会留在这里，按「送到对话」才发出去。",
                   w_c(C_SLOT_FG), TEXT_LINE_H);
        y += 4;
        if (g_text[0])
            w_wrap((float)W_TOP_MARGIN, y, (float)W_TOP_USABLE, g_text,
                   w_c(C_SLOT_ACCENT), TEXT_LINE_H);
        else
            w_wrap((float)W_TOP_MARGIN, y, (float)W_TOP_USABLE, "（还没输入）",
                   w_c(C_SLOT_MUTED), TEXT_LINE_H);
        break;

    case PANEL_PROJ: {
        int n = projects_count(), act = projects_active();
        char line[96];
        y = w_wrap((float)W_TOP_MARGIN, y, (float)W_TOP_USABLE,
                   "Y 新建 · START 改名 · X 换模型 · A 切换 · SELECT 删除",
                   w_c(C_SLOT_MUTED), TEXT_LINE_H);
        y += 2;
        for (int i = 0; i < n; ++i) {
            const Project *p = projects_at(i);
            snprintf(line, sizeof line, "%s %s  ·  %s / %s",
                     i == act ? "▶" : "  ", p->name, p->provider,
                     p->model[0] ? p->model : "(默认)");
            y = w_wrap((float)W_TOP_MARGIN, y, (float)W_TOP_USABLE, line,
                       i == act ? w_c(C_SLOT_ACCENT) : w_c(C_SLOT_FG),
                       TEXT_LINE_H);
        }
        if (n == 0)
            w_wrap((float)W_TOP_MARGIN, y, (float)W_TOP_USABLE,
                   "还没有任务", w_c(C_SLOT_MUTED), TEXT_LINE_H);
        break;
    }

    case PANEL_FILE: {
        char line[64];
        snprintf(line, sizeof line, "%s  ·  %d 个文件", actions_dir(), g_fn);
        y = w_wrap((float)W_TOP_MARGIN, y, (float)W_TOP_USABLE, line,
                   w_c(C_SLOT_MUTED), TEXT_LINE_H);
        y += 4;
        w_wrap((float)W_TOP_MARGIN, y, (float)W_TOP_USABLE,
               "A 打开 / X 移到回收站 / Y 刷新。删除是可恢复的。",
               w_c(C_SLOT_FG), TEXT_LINE_H);
        break;
    }

    default:
        w_wrap((float)W_TOP_MARGIN, y, (float)W_TOP_USABLE,
               g_loaded ? g_docname : "（没有打开的文档）",
               w_c(C_SLOT_ACCENT), TEXT_LINE_H);
        break;
    }

    w_top_footer(app_status(), "");
}


/* ------------------------------------------------------------------ 拼音键盘
 *
 * ── 布局 ──
 *   候选行   y 18..44   （6 个，可触摸）
 *   输入回显 y 46..64
 *   字母三行 y 68 / 100 / 132（高 28）
 *   动作行   y 170..204  [退格] [上屏] [完成]
 *
 * ★ 键宽 28、间隙 2 —— 10 个键正好 298 宽度，铺满 300 可用宽。
 *   再宽一点就会溢出右边界，而溢出的部分**看得见点不到**。
 * ★ 三行字母按 QWERTY 排，不是按字母序：会打字的人凭肌肉记忆找键，
 *   换成字母序反而每个字都要重新找。
 */

/* ---- 拼音键盘几何（下屏 240 高，全程别越 240）----------------------------
 *   候选行   y  10 .. 36   （6 个，48 宽 + 2 隙 = 298，铺满）
 *   页码     y  38
 *   输入回显 y  58 .. 76
 *   字母 3 行 y  80 / 112 / 144  （每行 28 高 + 4 隙）
 *   动作行   y 178 .. 212
 *   提示     y 214
 * ★ 早先 ROW_Y0=68 配 DY=32、动作行 170，是**面板内容上移之前**的坐标；
 *   面板整体下移 22 之后就没跟着改，动作行压到 204 之外，
 *   提示行 210 直接出屏 —— 用户看到的"输入法有问题"就是这个：底部按钮点不到。
 */
static const char *const IME_ROWS[3] = {
    "qwertyuiop",
    "asdfghjkl",
    "zxcvbnm",
};

#define IME_KEY_W   28.0f
#define IME_KEY_GAP  2.0f
#define IME_KEY_H   28.0f
#define IME_ROW_Y0  80.0f
#define IME_ROW_DY  32.0f
#define IME_CAND_Y  10.0f
#define IME_CAND_H  26.0f
#define IME_CAND_W  48.0f
#define IME_CAND_N   6          /* 一屏显示几个候选 */

static Rect ime_key_rect(int row, int col)
{
    int n = (int)strlen(IME_ROWS[row]);
    float total = (float)n * IME_KEY_W + (float)(n - 1) * IME_KEY_GAP;
    float x0 = (float)W_BOT_MARGIN + ((float)W_BOT_USABLE - total) / 2.0f;
    Rect r;
    r.x = x0 + (float)col * (IME_KEY_W + IME_KEY_GAP);
    r.y = IME_ROW_Y0 + (float)row * IME_ROW_DY;
    r.w = IME_KEY_W;
    r.h = IME_KEY_H;
    return r;
}

static Rect ime_cand_rect(int i)
{
    Rect r;
    r.x = (float)W_BOT_MARGIN + (float)i * (IME_CAND_W + IME_KEY_GAP);
    r.y = IME_CAND_Y;
    r.w = IME_CAND_W;
    r.h = IME_CAND_H;
    return r;
}

static Rect ime_act_rect(int i)
{
    static const float xs[3] = { 10.0f, 105.0f, 210.0f };
    static const float ws[3] = {  90.0f, 100.0f, 100.0f };
    Rect r;
    r.x = xs[i];
    r.y = 178.0f;
    r.w = ws[i];
    r.h = 34.0f;
    return r;
}

/* ★ 上屏目标。NULL = 写工作台面板的 g_text（老行为）；
 *   宿主屏可以 bind 到自己的输入框（对话页的 g_input）。
 *   cap 是那个缓冲的**字节**容量，用来做"写不下就整字拒绝"。 */
static char  *g_ime_dst;
static size_t g_ime_cap;

void dockui_ime_bind(char *buf, size_t cap)
{
    g_ime_dst = buf;
    g_ime_cap = buf ? cap : 0;
}

/* 把第 i 个候选追加到文本末尾。写不下就整字拒绝（不截半个汉字）。 */
static void ime_commit_idx(int i)
{
    const char *c = ime_cand(i);
    char  *dst;
    size_t cap, used, need;

    if (!c || !c[0])
        return;

    if (g_ime_dst) {
        dst = g_ime_dst;
        cap = g_ime_cap;
    } else {
        dst = g_text;
        cap = sizeof g_text;
    }

    used = strlen(dst);
    need = strlen(c);
    if (used + need + 1 > cap) {
        app_status_set("文本已满（%d 字节），先送到对话清空", (int)used);
        return;
    }
    memcpy(dst + used, c, need);
    dst[used + need] = '\0';
    ime_commit(i, (char[16]){0}, 16);      /* 只为清掉输入串 */
    g_cpage = 0;
}

void dockui_render_ime(void)
{
    int n = ime_cand_count();
    char buf[64];

    /* 候选（按页） */
    {
        int base = g_cpage * IME_CAND_N;
        int pages = (n + IME_CAND_N - 1) / IME_CAND_N;

        if (pages < 1) pages = 1;
        if (g_cpage >= pages) { g_cpage = pages - 1; base = g_cpage * IME_CAND_N; }

        for (int i = 0; i < IME_CAND_N; ++i) {
            Rect r = ime_cand_rect(i);
            int k = base + i;
            if (k < n)
                w_button(&r, ime_cand(k),
                         (k == 0 ? W_BTN_PRIMARY : W_BTN_NORMAL)
                         | (app_touch_on(&r) ? W_BTN_PRESSED : 0));
            else
                w_button(&r, " ", W_BTN_DISABLED);
        }

        if (pages > 1) {
            char pg[16];
            snprintf(pg, sizeof pg, "%d/%d", g_cpage + 1, pages);
            w_line((float)W_BOT_MARGIN, 38.0f, 60.0f, pg, w_c(C_SLOT_MUTED));
        }
    }

    /* 输入回显 + 状态 */
    snprintf(buf, sizeof buf, "%s%s", ime_input(),
             ime_input()[0] ? "" : "（打字母，如 ni / nihao）");
    w_line((float)W_BOT_MARGIN, 58.0f, (float)W_BOT_USABLE, buf,
           ime_input()[0] ? w_c(C_SLOT_ACCENT) : w_c(C_SLOT_MUTED));

    /* 字母键 */
    for (int row = 0; row < 3; ++row) {
        int len = (int)strlen(IME_ROWS[row]);
        for (int col = 0; col < len; ++col) {
            Rect r = ime_key_rect(row, col);
            char k[2] = { IME_ROWS[row][col], 0 };
            w_button(&r, k,
                     (app_touch_on(&r) ? W_BTN_PRESSED : 0));
        }
    }

    /* 动作行（先算矩形再取地址：ime_act_rect 是函数，返回值不能 &） */
    {
        Rect a0 = ime_act_rect(0), a1 = ime_act_rect(1), a2 = ime_act_rect(2);
        w_button(&a0, "退格", W_BTN_NORMAL);
        w_button(&a1, "上屏", W_BTN_PRIMARY);
        w_button(&a2, "完成", W_BTN_NORMAL);
    }

    w_line((float)W_BOT_MARGIN, 216.0f, (float)W_BOT_USABLE,
           "A=上屏  X=退格  B=收起键盘", w_c(C_SLOT_FAINT));
}

bool dockui_ime_touch(int tx, int ty)
{
    int n = ime_cand_count();

    /* 候选（按页） */
    {
        int base = g_cpage * IME_CAND_N;
        for (int i = 0; i < IME_CAND_N; ++i) {
            Rect r = ime_cand_rect(i);
            int k = base + i;
            if (k < n && w_hit(&r, tx, ty)) {
                ime_commit_idx(k);
                return true;
            }
        }
    }

    /* 字母 */
    for (int row = 0; row < 3; ++row) {
        int len = (int)strlen(IME_ROWS[row]);
        for (int col = 0; col < len; ++col) {
            Rect r = ime_key_rect(row, col);
            if (w_hit(&r, tx, ty)) {
                g_cpage = 0;
                if (!ime_push(IME_ROWS[row][col]))
                    app_status_set("输入串满了（%d 字母）", IME_INPUT_MAX);
                return true;
            }
        }
    }

    /* 动作 */
    {
        Rect a0 = ime_act_rect(0), a1 = ime_act_rect(1), a2 = ime_act_rect(2);
        if (w_hit(&a0, tx, ty)) { g_cpage = 0; ime_backspace(); return true; }
        if (w_hit(&a1, tx, ty)) { ime_commit_idx(0); return true; }
        if (w_hit(&a2, tx, ty)) { g_ime = false; return true; }
    }

    return true;      /* 键盘展开时，点空白也不该穿透到别的面板 */
}

void dockui_ime_keys(u32 down)
{
    /* 翻页：L/R 两个键（左右方向键留给"移动光标"的未来用途，
     * 这里先用肩键，不会和任何东西冲突）。 */
    if (down & KEY_L) { if (g_cpage > 0) --g_cpage; return; }
    if (down & KEY_R) { ++g_cpage; return; }

    if (down & KEY_B) {
        if (ime_input()[0])
            ime_reset();            /* 先清输入串，再按一次才收起 */
        else
            g_ime = false;
        return;
    }
    if (down & KEY_A) { ime_commit_idx(0); return; }
    if (down & KEY_X) { g_cpage = 0; ime_backspace(); return; }
}

/* 画一格面板的内容（不含滑轨条）。宿主屏也调这个。 */
void dockui_render(int panel)
{
    float x = (float)W_BOT_MARGIN;
    float w = (float)W_BOT_USABLE;

    switch (panel) {
    case PANEL_KEY: {
        Rect box = { x, 62.0f, w, 56.0f, 0 };
        w_panel(&box, w_c(C_SLOT_ACC_LT), w_c(C_SLOT_ACCENT));
        w_wrap(x + 8, 68.0f, w - 16, g_text[0] ? g_text : "（点下面的按钮输入）",
               g_text[0] ? w_c(C_SLOT_FG) : w_c(C_SLOT_MUTED), TEXT_LINE_H);

        /* 两行两列：上排是"输中文 / 输英文"，下排是"清掉 / 送出去"。
         * 一排放四个会把按钮压到 70px 宽，中文标签就放不下了。 */
        w_button(&(Rect){ x,        126.0f, 146.0f, 36.0f, 0 }, "拼音",
                 W_BTN_PRIMARY);
        w_button(&(Rect){ x + 154.0f, 126.0f, 146.0f, 36.0f, 0 }, "英文键盘",
                 W_BTN_NORMAL);
        w_button(&(Rect){ x,        168.0f, 146.0f, 36.0f, 0 }, "清空",
                 W_BTN_NORMAL);
        w_button(&(Rect){ x + 154.0f, 168.0f, 146.0f, 36.0f, 0 }, "送到对话",
                 W_BTN_NORMAL);
        break;
    }

    case PANEL_PROJ: {
        int n = projects_count();
        for (int i = 0; i < n && i < ROW_CAP; ++i) {
            const Project *p = projects_at(i);
            char label[72];
            /* 两行信息压进一行：任务名 + 模型。模型才是"每个任务用不同模型"
             * 这件事的可见证据 —— 只显示名字的话，用户看不出换没换成功。 */
            snprintf(label, sizeof label, "%s · %s",
                     p->name, p->model[0] ? p->model : "(默认模型)");
            Rect r = row_rect(i);
            w_button(&r, label,
                     (i == g_sel ? W_BTN_SELECTED : W_BTN_NORMAL)
                     | (i == projects_active() ? W_BTN_PRIMARY : 0)
                     | (app_touch_on(&r) ? W_BTN_PRESSED : 0));
        }
        if (n == 0)
            w_wrap(x, ROW_Y0, w, "还没有任务，按 Y 新建一条",
                   w_c(C_SLOT_MUTED), TEXT_LINE_H);
        break;
    }

    case PANEL_FILE: {
        for (int i = 0; i < g_fn && i < ROW_CAP; ++i) {
            char label[72];
            Rect r = row_rect(i);
            snprintf(label, sizeof label, "%s  %uK", g_files[i].name,
                     (g_files[i].size + 1023) / 1024);
            w_button(&r, label,
                     (i == g_sel ? W_BTN_SELECTED : W_BTN_NORMAL)
                     | (app_touch_on(&r) ? W_BTN_PRESSED : 0));
        }
        if (g_fn == 0)
            w_wrap(x, ROW_Y0, w, "这个目录是空的（AI 写出来的文件会出现在这）",
                   w_c(C_SLOT_MUTED), TEXT_LINE_H);
        break;
    }

    default: {
        /* 浏览：画文档行。这里与 sc_viewer.c 是同一种做法，
         * 但不复用它的静态状态 —— 两个屏各有各的滚动位置。 */
        int y = 0;
        for (int i = g_top; i < g_doc.count && y < ROW_CAP; ++i, ++y) {
            const HtmlLine *l = &g_doc.line[i];
            float yy = PANEL_Y0 + (float)y * (float)TEXT_LINE_H;
            w_line(x, yy, w, l->text,
                   l->kind == HL_H1 || l->kind == HL_H2 || l->kind == HL_H3
                       ? w_c(C_SLOT_ACC_DK) : w_c(C_SLOT_FG));
        }
        if (!g_loaded)
            w_wrap(x, PANEL_Y0, w, "在「文件」里选一个 .html 或 .txt 打开",
                   w_c(C_SLOT_MUTED), TEXT_LINE_H);
        break;
    }
    }

}

/* 画滑轨条本身。宿主屏自己也能画（它有 5 格而不是 4 格）。 */
static void render_rail(void)
{
    for (int i = 0; i < PANEL_N; ++i) {
        Rect r = rail_rect(i);
        w_button(&r, panel_name(i),
                 (i == g_panel ? W_BTN_SELECTED : W_BTN_NORMAL)
                 | (app_touch_on(&r) ? W_BTN_PRESSED : 0));
    }

    w_center((float)R_BOT_W / 2.0f, 202.0f,
             "← → 切格   A 执行   B 返回", w_c(C_SLOT_FAINT));
}

static void render_bottom(void)
{
    /* 拼音键盘展开时整屏让给它 —— 候选、字母、动作都在里面。 */
    if (g_ime) {
        dockui_render_ime();
        return;
    }
    dockui_render(g_panel);
    render_rail();
}

/* ------------------------------------------------------------------ 行为 */

static void dock_enter(void)
{
    AppConfig *c = app_cfg();

    projects_load(c ? c->ai.provider : NULL, c ? c->ai.model : NULL);
    scan_files();
    g_panel = PANEL_KEY;
    g_sel = 0;
    g_text[0] = '\0';
    app_note("dock: enter (%d projects, %d files)",
             projects_count(), g_fn);
}

/* 键盘面板的三个按钮 */
static void key_edit(void)
{
    char buf[DOCK_TEXT_MAX];
    snprintf(buf, sizeof buf, "%s", g_text);
    /* 字符数上限按 3 字节/字保守换算，避免中文把缓冲写穿
     * （同 sc_chat.c 的 open_keyboard 那条规矩）。 */
    if (ui_input_text("输入文字", buf, (DOCK_TEXT_MAX - 1) / 3,
                      g_text, sizeof g_text))
        app_status_set("已输入 %d 字节", (int)strlen(g_text));
}

/* 送到对话：直接把文字塞进对话屏的输入框不合适（那边是静态的），
 * 这里走"复制到剪贴板式"的变通 —— 打开对话屏前先记住。
 * 现阶段先如实提示：文字留在这里，去对话屏可以再编辑发送。 */
static void key_send(void)
{
    if (!g_text[0]) {
        app_status_set("还没有输入内容");
        return;
    }
    app_status_set("文字已备好（%d 字节），按 A 进对话继续", (int)strlen(g_text));
    app_note("dock: key_send len=%d", (int)strlen(g_text));
}

bool dockui_touch(int panel, int tx, int ty)
{
    switch (panel) {
    case PANEL_KEY:
        if (w_hit(&(Rect){ 10.0f,  126.0f, 146.0f, 36.0f, 0 }, tx, ty)) {
            ime_reset();
            g_cpage = 0;
            g_ime = true;
            app_status_set("拼音：打字母，选候选");
            return true;
        }
        if (w_hit(&(Rect){ 164.0f, 126.0f, 146.0f, 36.0f, 0 }, tx, ty)) {
            key_edit();
            return true;
        }
        if (w_hit(&(Rect){ 10.0f,  168.0f, 146.0f, 36.0f, 0 }, tx, ty)) {
            g_text[0] = '\0';
            app_status_set("已清空");
            return true;
        }
        if (w_hit(&(Rect){ 164.0f, 168.0f, 146.0f, 36.0f, 0 }, tx, ty)) {
            key_send();
            return true;
        }
        break;

    case PANEL_PROJ:
        for (int i = 0; i < projects_count() && i < ROW_CAP; ++i) {
            Rect r = row_rect(i);
            if (w_hit(&r, tx, ty)) {
                g_sel = i;
                apply_project(i);
                return true;
            }
        }
        break;

    case PANEL_FILE:
        for (int i = 0; i < g_fn && i < ROW_CAP; ++i) {
            Rect r = row_rect(i);
            if (w_hit(&r, tx, ty)) {
                g_sel = i;
                open_doc(g_files[i].name);
                return true;
            }
        }
        break;

    default:
        break;
    }
    return false;   /* 没命中：宿主可以继续处理（比如点到了别的键） */
}

static void dock_handle(u32 down, u32 held)
{
    (void)held;

    if (g_ime) {
        int tx, ty;
        if (app_touch_down(&tx, &ty))
            dockui_ime_touch(tx, ty);
        else
            dockui_ime_keys(down);
        return;
    }

    /* ---- 滑轨：触摸优先，然后是左右键 ---- */
    {
        int tx, ty;
        if (app_touch_down(&tx, &ty)) {
            for (int i = 0; i < PANEL_N; ++i) {
                Rect r = rail_rect(i);
                if (w_hit(&r, tx, ty)) {
                    g_panel = i;
                    g_sel = 0;
                    if (i == PANEL_FILE)
                        scan_files();
                    app_note("dock: rail -> %d", i);
                    return;
                }
            }
            dockui_touch(g_panel, tx, ty);
            return;
        }
    }

    if (down & (KEY_LEFT | KEY_L)) {
        g_panel = (g_panel + PANEL_N - 1) % PANEL_N;
        g_sel = 0;
        if (g_panel == PANEL_FILE) scan_files();
        return;
    }
    if (down & (KEY_RIGHT | KEY_R)) {
        g_panel = (g_panel + 1) % PANEL_N;
        g_sel = 0;
        if (g_panel == PANEL_FILE) scan_files();
        return;
    }

    if (down & KEY_B) {
        if (g_panel == PANEL_BROWSE) {
            g_panel = PANEL_FILE;        /* 浏览里 B 只退一层 */
            g_sel = 0;
            return;
        }
        app_pop();
        return;
    }

    dockui_keys(g_panel, down);
}

void dockui_keys(int panel, u32 down)
{
    (void)panel;
    switch (panel) {
    case PANEL_KEY:
        if (down & KEY_A) key_edit();
        else if (down & KEY_X) { g_text[0] = '\0'; app_status_set("已清空"); }
        else if (down & KEY_Y) key_send();
        break;

    case PANEL_PROJ: {
        int n = projects_count();
        if (down & KEY_UP)   { g_sel = (g_sel > 0) ? g_sel - 1 : n - 1; return; }
        if (down & KEY_DOWN) { g_sel = (g_sel + 1 < n) ? g_sel + 1 : 0; return; }

        if (down & KEY_A) { apply_project(g_sel); return; }

        /* ★ X = 换这个任务的模型，取值来源是**已添加的 API**（有 Key 的条目）。
         *   左肩键回退、X 前进 —— 两条方向都留着，免得只能单向转圈。 */
        if (down & KEY_X)     { cycle_task_api(g_sel, +1); return; }
        if (down & KEY_L)     { cycle_task_api(g_sel, -1); return; }

        /* ★ Y = 新建任务（先起名，再按 X 换模型） */
        if (down & KEY_Y) { new_task(); return; }

        /* ★ START = 改当前任务的名字 */
        if (down & KEY_START) { rename_task(g_sel); return; }

        if (down & KEY_SELECT) {
            if (projects_remove(g_sel)) {
                if (g_sel >= projects_count())
                    g_sel = projects_count() - 1;
                app_status_set("已删除（当前 %d 个）", projects_count());
            } else {
                app_status_set("至少要留一个任务");
            }
            return;
        }
        break;
    }

    case PANEL_FILE:
        if (down & KEY_Y) { scan_files(); app_status_set("已刷新：%d 个文件", g_fn); return; }
        if (g_fn == 0) return;
        if (down & KEY_UP)   { g_sel = (g_sel > 0) ? g_sel - 1 : g_fn - 1; return; }
        if (down & KEY_DOWN) { g_sel = (g_sel + 1 < g_fn) ? g_sel + 1 : 0; return; }
        if (down & KEY_A) { open_doc(g_files[g_sel].name); return; }
        if (down & KEY_X) { trash_file(g_files[g_sel].name); return; }
        break;

    case PANEL_BROWSE:
        if (down & KEY_UP)   { if (g_top > 0) --g_top; return; }
        if (down & KEY_DOWN) { if (g_top + 1 < g_doc.count) ++g_top; return; }
        if (down & KEY_A)    { g_panel = PANEL_BROWSE; return; }
        break;

    default:
        break;
    }
}

/* ---------------- 组件 API（宿主屏用） ---------------- */

void dockui_init(void)
{
    AppConfig *c = app_cfg();
    projects_load(c ? c->ai.provider : NULL, c ? c->ai.model : NULL);
    scan_files();
}

const char *dockui_name(int panel)
{
    return panel_name(panel);
}

bool dockui_ime_active(void)
{
    return g_ime;
}

/* ★ 给宿主屏用的开/关。对话页的「键盘」按钮走这里，不再开系统 swkbd
 *   —— 3DS 的系统键盘**没有中文拼音页**（SWKBD_TYPE_WESTERN 还会在日版
 *   机器上关掉假名输入），所以中文只能靠我们自己的 ime 引擎。
 *   宿主屏想收自己的缓冲时，先 ime_bind() 再 open。 */
void dockui_ime_open(void)
{
    ime_reset();
    g_cpage = 0;
    g_ime = true;
}

void dockui_ime_close(void)
{
    g_ime = false;
}

void dockui_rail_rect(int i, float *x, float *y, float *w, float *h)
{
    Rect r = rail_rect(i);
    if (x) *x = r.x;
    if (y) *y = r.y;
    if (w) *w = r.w;
    if (h) *h = r.h;
}

const Screen g_screen_dock = {
    .id            = SCREEN_DOCK,
    .name          = "SCREEN_DOCK",
    .enter         = dock_enter,
    .leave         = NULL,
    .render_top    = render_top,
    .render_bottom = render_bottom,
    .handle        = dock_handle,
};
