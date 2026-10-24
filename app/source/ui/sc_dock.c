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
#include "statpanel.h"

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

enum { PANEL_KEY = 0, PANEL_PROJ, PANEL_FILE, PANEL_BROWSE, PANEL_STAT, PANEL_N };

static int  g_panel = PANEL_KEY;
static int  g_sel   = 0;          /* 各面板共用一个"当前行"，切面板时重置 */

/* ★ 2026-10-23 真机反馈：「建任务二直接就豆包了，不行 —— 建新任务的时候
 *   会让你选择是哪一个模型」。
 *
 *   原先 new_task() 直接绑 `app_cfg()->ai`（当前正在用的那套），
 *   于是用户上一轮用豆包，新建出来的任务就默认是豆包，而他根本没机会选。
 *
 *   >=0 表示「正在为第 g_pick 个任务挑 API」，此时 ←/→ 换、A 确定、B 先这样。
 *   为什么做成"先建出来再挑"而不是"选完才建"：起名已经用掉一个输入框，
 *   再插一个选择流程会让"我只是想新建一个"变成三步；先落地再挑，
 *   用户按 B 也能拿到一个可用的默认任务（只是没换模型）。 */
static int  g_pick  = -1;

/* ------------------------------------------------------------------ 键盘面板 */
#define DOCK_TEXT_MAX 256
static char g_text[DOCK_TEXT_MAX];

/* ★★ 文本落点（2026-10-24）。
 *
 *   这个面板最早是独立"工作台"屏上的一个格子，文字存在自己的 g_text 里。
 *   后来它被搬进对话屏当"键盘格"，问题就来了：**这里有一个文本缓冲，
 *   对话屏还有一个 g_input**，两边谁也看不见谁。于是"送到对话"只能
 *   如实写一句"文字已备好，按 A 进对话继续" —— 它根本没东西可送。
 *
 *   现在由宿主 bind：对话屏进屏时把这里绑到它自己的 g_input。于是
 *   "键盘格"编辑的就是对话那条输入框，不是一个看不见的副本；
 *   「送到对话」也就能真的把消息发出去（send 请求登记在 g_send_req）。
 *
 *   NULL 表示没绑（独立工作台用法），仍用本模块的 g_text。
 *   为什么不用回调：回调会让本模块反过来依赖对话屏；而"按了没发"
 *   这种 bug 一旦出现，靠回调链排查比靠一个布尔量难得多。 */
static char  *g_txt_dst;
static size_t g_txt_cap;

void dockui_bind_text(char *buf, size_t cap)
{
    g_txt_dst = buf;
    g_txt_cap = buf ? cap : 0;
}

/* 当前该读写哪个缓冲。所有 g_text 的用途都必须走这两个函数 ——
 * 漏一处，那一处就会继续读写那个看不见的副本。 */
static char  *txt(void)    { return g_txt_dst ? g_txt_dst : g_text; }
static size_t txt_cap(void){ return g_txt_dst ? g_txt_cap : sizeof g_text; }

/* 「发送」请求：键盘格只负责登记，真正发出去由宿主做（见 dockui_take_send）。
 * 一次登记、一次取走、取走即清 —— 不做队列，因为同一帧按两下 Y
 * 也没有"发两条"的语义。 */
static bool g_send_req;

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

/* ---- 任务行右端的「×」删除键（2026-10-24 用户要求）----
 *
 * 用户原话：「再给每个项目加一个可以删除的按钮，就是一个叉。」
 *
 * 为什么**做在行内右侧**而不是另起一列：底屏只有 300px 可用宽，
 * 再开一列会把任务名压到 12 个全角字以内，而任务名本来就允许 7 个字 +
 * 模型名 —— 结果是"名字显示不全"换"删除按钮更好看"，不值。
 *
 * 为什么整行按钮要**收窄**：叉画在行右端，行按钮若不收窄，文字会居中到
 * 叉的下面 —— 看似只是重叠，实际是"最后一个字被叉切掉半边"，
 * 用户会以为任务名就长那样。
 *
 * ⚠️ 触摸判定的顺序必须是**先叉后行**：叉的矩形整个落在行矩形里面，
 *   先判行的话叉永远点不到（点哪儿都是"切项目"）。 */
#define PROJ_DEL_W    24.0f
#define PROJ_DEL_GAP  2.0f

static Rect proj_del_rect(int i)
{
    Rect r = row_rect(i);
    Rect d;

    d.x = r.x + r.w - PROJ_DEL_W;
    d.y = r.y;
    d.w = PROJ_DEL_W;
    d.h = r.h;
    return d;
}

/* 「+ 新建任务」落在第几行（0-based）。
 *
 * 规则：任务按顺序占 0..n-1，按钮紧接着它们；但**最多占到 ROW_CAP-1**，
 * 也就是最后一行永远留给按钮 —— 否则任务一多，按钮就被挤出可见区，
 * 用户又回到"找不到新建"的老问题上。
 *
 * ★ 渲染和触摸**必须**共用这一个函数。分开各算一次是这类布局最常见的
 *   错法：改了一处没改另一处，表现是"按钮画在这儿、点了没反应"。
 */
static int proj_new_row(void)
{
    int n    = projects_count();
    int last = ROW_CAP - 1;
    return (n < last) ? n : last;
}

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
/* 删掉第 i 个任务。触摸「×」和 SELECT 键共用这一条路。
 *
 * ★ 为什么必须抽出来：删任务不只是"从表里去掉一行"，还有两件连带的事 ——
 *   ① 每个任务有一份自己的对话历史，文件名按**序号**编（chat_p0/p1/…）。
 *      删掉中间那个，后面所有任务的序号都会前移，文件不跟着挪的话
 *      "任务丙"就会读到"任务乙"的历史 —— 屏幕上完全看不出错。
 *   ② 当前上屏显示的可能正是被删那个项目的内容，删完必须重载。
 *   这两件事只要漏一处，表现都是"数据错了但不报错"，比崩溃难查得多。
 *   所以只留这一个入口：谁想删任务都走这里，不许直接调 projects_remove。 */
static void proj_remove(int i)
{
    int total = projects_count();

    if (i < 0 || i >= total)
        return;
    if (total <= 1) {
        /* 面板会变成空壳，所以不许删到 0。UI 上这一格是禁用态，
         * 这里再挡一次是因为按键路径也能进来（SELECT）。 */
        app_status_set("至少要留一个任务");
        return;
    }
    if (!projects_remove(i)) {
        app_status_set("删除失败（写卡）");
        return;
    }

    /* 历史文件跟着挪（total 是**删之前**的个数，函数内部要拿它算挪几个） */
    extern void chat_drop_project(int idx, int total);
    chat_drop_project(i, total);

    /* 光标跟着"同一个任务"走：删的是它上面那行就上移一格。
     * 不减的话"删掉第一行"会让高亮跳到下一个任务上 ——
     * 看起来像"删完之后程序自己选了另一个"，而用户只想删掉不要的那个。 */
    if (g_sel > i)
        --g_sel;
    if (g_sel >= projects_count())
        g_sel = projects_count() - 1;
    app_status_set("已删除（当前 %d 个）", projects_count());
}

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

    /* ★★ 切完任务，**上屏立刻换成这个任务的历史**（2026-10-24）。
     *
     *   用户原话：「选择新项目后，上屏的页面没有变化，没有进入新项目的空间；
     *   打开新项目的时候，也要看到那个项目之前做的内容。」
     *
     *   在那之前，这里只改了 provider/model —— 对话历史是同一条
     *   chat.json，于是"换项目"在屏幕上等于什么都没发生。
     *   现在每个任务一份记录（见 shared/chatlog.c 的 chatlog_set_slot），
     *   这里切完顺手让它重载，用户不用退出去再进来才看到变化。
     *
     *   为什么不做成"下次进屏再生效"：用户的操作是"点这个项目"，
     *   界面对这一下的回应必须**在这一下**发生。让人再退再进一次，
     *   他只会得出"点了没用"的结论。 */
    extern void chat_reload_project(void);
    chat_reload_project();

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

/* 播报"正在为哪个任务挑哪套 API"。 */
static void announce_pick(void)
{
    const Project *p = projects_at(g_pick);

    if (!p)
        return;
    app_status_set(S(S0935), p->name, p->provider,
                   p->model[0] ? p->model : S(S0923));
}

/* ---- 与「选 API」页（SCREEN_PICKAPI）的桥 ---- */

int dockui_pick_target(void)
{
    return g_pick;
}

/* 把第 k 套"可用 API"绑给正在挑的那个任务。
 * 返回 false = 没在挑 / 下标越界，调用方据此决定要不要提示。 */
bool dockui_pick_apply(int k)
{
    const KeyEntry *e = api_ready_at(k);
    Project *p;

    if (g_pick < 0 || !e)
        return false;

    task_set_api(g_pick, e);

    p = projects_at(g_pick);
    if (p)
        app_status_set(S(S0954), p->name, p->provider,
                       p->model[0] ? p->model : S(S0923));

    g_pick = -1;
    return true;
}

void dockui_pick_cancel(void)
{
    /* 取消 = 就用 new_task 时绑的那套默认。**不清任务**：
     * 用户已经起好名字了，半路按 B 就把他刚建的条目删掉太粗暴。 */
    g_pick = -1;
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

    /* ★★ 2026-10-23 第二轮反馈：「横幅不行，会被遮挡。就是点进去会跳转一个
     *   页面，让你选更直白」。
     *
     *   横幅只能挤在项目列表上方那一行，列表一多就把字压掉；
     *   而且 ←/→ 循环看不到"一共有几套、各自是什么"。
     *   改成**跳转到一个真正的列表页**（SCREEN_PICKAPI），一次看全。
     *
     *   一个都没有时不跳（跳过去也是空页），只提示去配。 */
    if (api_ready_count() > 0) {
        g_pick = i;
        app_push(SCREEN_PICKAPI);
    } else {
        app_status_set(S(S0934));
    }
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
        case PANEL_STAT:   return "状态";
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
        if (txt()[0])
            w_wrap((float)W_TOP_MARGIN, y, (float)W_TOP_USABLE, txt(),
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
        dst = txt();
        cap = txt_cap();
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

    /* ★ B 不在这里处理了：宿主屏（sc_chat / sc_keyedit 等）已经在
     *   自己的 handle 最前面统一接管 —— 有输入就先清、没有就退屏。
     *   留在这里会造成"两处各吃一次"，正是真机上"按 B 没反应"的来源。 */
    if (down & KEY_A) { ime_commit_idx(0); return; }
    if (down & KEY_X) { g_cpage = 0; ime_backspace(); return; }
}

/* 画一格面板的内容（不含滑轨条）。宿主屏也调这个。 */
void dockui_render(int panel)
{
    float x = (float)W_BOT_MARGIN;
    float w = (float)W_BOT_USABLE;

    switch (panel) {
    case PANEL_STAT:
        /* 状态页：帧率 / 主循环占用 / 内存。数据是主循环每帧采的，
         * 这里只负责画（见 statpanel.c 顶部对"为什么没有 GPU%"的说明）。 */
        statpanel_render();
        break;

    case PANEL_KEY: {
        Rect box = { x, 62.0f, w, 56.0f, 0 };
        w_panel(&box, w_c(C_SLOT_ACC_LT), w_c(C_SLOT_ACCENT));
        w_wrap(x + 8, 68.0f, w - 16, txt()[0] ? txt() : "（点下面的按钮输入）",
               txt()[0] ? w_c(C_SLOT_FG) : w_c(C_SLOT_MUTED), TEXT_LINE_H);

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
        int nrow = proj_new_row();      /* 「+ 新建」固定落在这一行 */

        /* 横幅已删（2026-10-23 第二轮）：新建任务改为**跳转选 API 页**，
         * 这里不再需要"正在挑"的提示 —— 挑选过程整个在另一个屏里。 */
        for (int i = 0; i < n && i < nrow; ++i) {
            const Project *p = projects_at(i);
            char label[72];
            /* 两行信息压进一行：任务名 + 模型。模型才是"每个任务用不同模型"
             * 这件事的可见证据 —— 只显示名字的话，用户看不出换没换成功。 */
            snprintf(label, sizeof label, "%s · %s",
                     p->name, p->model[0] ? p->model : S(S0923));

            /* 行本体收窄，把右端让给「×」（见 proj_del_rect 上面的注释） */
            Rect rb = row_rect(i);
            rb.w -= PROJ_DEL_W + PROJ_DEL_GAP;
            w_button(&rb, label,
                     (i == g_sel ? W_BTN_SELECTED : W_BTN_NORMAL)
                     | (i == projects_active() ? W_BTN_PRIMARY : 0)
                     | (app_touch_on(&rb) ? W_BTN_PRESSED : 0));

            /* 「×」删除键。只剩一个任务时画成**禁用态** ——
             * projects_remove() 本来就会拒绝，但"点了没反应"和
             * "看得出不能点"是两回事，后者才不让人反复去试。 */
            Rect rx = proj_del_rect(i);
            /* 「×」= U+00D7，字库里是**全角 16px**（实测 app/data/font.bin），
             * 24px 宽的按钮扣掉 6px 内边距还剩 18px，放得下。
             * 用 ✕(U+2715) 就不行：它只有 6px，画出来又细又小，
             * 在一排 24px 按钮里像是少画了一笔。 */
            w_button(&rx, "×",
                     (projects_count() <= 1 ? W_BTN_DISABLED : W_BTN_NORMAL)
                     | (app_touch_on(&rx) ? W_BTN_PRESSED : 0));
        }

        /* ★ 2026-10-23 真机反馈：「项目根本无法新建」。
         *
         *   查下来功能是有的（Y 键 = 新建），但界面上**一个字都没提** ——
         *   底屏这一屏只有任务列表，空的时候才有一句"按 Y 新建"。
         *   一旦有过任务，那句提示就消失了，新用户完全看不到入口。
         *
         *   所以给它一个**常驻的可点按钮**，而不是靠藏起来的快捷键。
         *   位置放在列表末尾（不是标题栏）：底屏只有 320px 宽，
         *   标题栏右侧已经有页码，再塞按钮会把任务名挤掉。
         *
         *   ⚠️ 任务超过 4 个时列表要滚动，而滚动**还没实现** ——
         *   那时第 5 个及以后会看不见，这个按钮也会落到可见区之外。
         *   这是既有缺陷，不是本次改出来的，已记进 BACKLOG。 */
        {
            Rect r = row_rect(nrow);
            w_button(&r, S(S0922),
                     W_BTN_NORMAL | (app_touch_on(&r) ? W_BTN_PRESSED : 0));
        }
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

/* ==========================================================================
 * 对话屏第 0 格「选项」——即时可调的四项
 *
 * 用户原话：「把聊天那一页改成选项，加入一些可以及时控制的选项，
 * 包括当时做好的上下文等也放在这里」。
 *
 * 为什么是这四项：它们**直接影响下一次请求**，用户调完立刻想问一句看看效果
 * （比如"答太短了"就调回复长度、"它不记得我刚才说的"就加轮数）。
 * 而主题、语言、OTA 那些调一次就不动的，留在设置屏就好。
 *
 * 为什么不放"助手人设 / 记忆"：那是**长文本**，在 320px 的底屏上
 * 用左右键调不了，得开文本框 —— 那是录入屏/设置屏的活。
 * ========================================================================== */

#define OPT_ROWS  4
#define OPT_Y0    46.0f
#define OPT_H     24.0f

static int g_opt_sel;

/* 值的中文/英文显示都走 S()，与设置屏同一套编号 —— 两处显示必须一致，
 * 否则用户会以为设置屏改了没生效。 */
static void opt_value(int i, char *out, size_t n)
{
    const AppConfig *c = app_cfg();

    if (!c) { snprintf(out, n, "-"); return; }

    switch (i) {
    case 0:
        snprintf(out, n, "%s", thinking_name(c->ai.thinking));
        break;
    case 1: {
        /* 0 的含义是"用默认"，而默认就是 8 轮 —— 直接显示实际生效的轮数，
         * 比显示"默认"两个字有用（用户问的是"它记得几轮"）。 */
        int t = c->ai.context_turns > 0 ? c->ai.context_turns : 8;
        snprintf(out, n, S(S0864), t);
        break;
    }
        break;
    case 2:
        snprintf(out, n, "%s",
                 c->ai.reply_len == CONFIG_REPLY_SHORT ? S(S0861)
               : c->ai.reply_len == CONFIG_REPLY_LONG  ? S(S0862) : S(S0826));
        break;
    default:
        /* ★ 这里原先写的是 S0001 / S0002 —— 而那两个编号真实内容是
         *   「这个键还没有对应实现」和「设置已改，但写不进配置文件」。
         *   也就是说设置屏选"中文"时，值列显示的是"这个键还没有对应实现"。
         *   已改用 S0947（中文）/ 硬编码 English / S0863（跟随界面），
         *   与设置屏保持一致（那边也一起修了）。 */
        if (c->ai.reply_lang == 1)      snprintf(out, n, "%s", S(S0946));
        else if (c->ai.reply_lang == 2) snprintf(out, n, "English");
        else                            snprintf(out, n, "%s", S(S0863));
        break;
    }
}

static void opt_label(int i, char *out, size_t n)
{
    snprintf(out, n, "%s",
             i == 0 ? S(S0382) : i == 1 ? S(S0859)
           : i == 2 ? S(S0858) : S(S0860));
}

/* 切一格。**改完立刻 config_save()** —— 这一屏的价值就是"即时生效"，
 * 攒着不存的话，用户退出去发现没变会以为是坏的。 */
static void opt_step(int i, int delta)
{
    AppConfig *c = app_cfg();

    if (!c)
        return;

    switch (i) {
    case 0:
        c->ai.thinking = (c->ai.thinking + delta + 4) % 4;
        app_status_set(S(S0900), thinking_name(c->ai.thinking));
        break;
    case 1: {
        /* 0(默认) -> 4 -> 8 -> 12 -> 0，和设置屏同一串档位 */
        static const int kTurn[] = { 0, 4, 8, 12 };
        int k = 0;
        for (int j = 0; j < 4; ++j)
            if (kTurn[j] == c->ai.context_turns) { k = j; break; }
        k = (k + (delta > 0 ? 1 : 3)) % 4;
        c->ai.context_turns = kTurn[k];
        app_status_set(S(S0859));
        break;
    }
    case 2:
        c->ai.reply_len = (c->ai.reply_len + delta + 3) % 3;
        app_status_set(S(S0858));
        break;
    default:
        c->ai.reply_lang = (c->ai.reply_lang + delta + 3) % 3;
        app_status_set(S(S0860));
        break;
    }
    /* 用 app 层的封装：先标脏再立刻落盘。
     * config_save(cfg) 只在"我手里有一份完整副本"时才该直接调，
     * 这里改的是全局 app_cfg()，走 dirty + save_now 才不会漏掉
     * 别的模块挂在同一份配置上的改动。 */
    app_cfg_dirty();
    app_cfg_save_now();
}

void dockui_render_opt(void)
{
    const float x = (float)W_BOT_MARGIN;
    const float w = (float)W_BOT_USABLE;

    for (int i = 0; i < OPT_ROWS; ++i) {
        char lab[40], val[40], line[96];
        Rect r;
        r.x = x;
        r.y = OPT_Y0 + (float)i * (OPT_H + 2.0f);
        r.w = w;
        r.h = OPT_H;

        opt_label(i, lab, sizeof lab);
        opt_value(i, val, sizeof val);
        /* 一行放不下"标签 ……… 值"：值和标签分别画，值右对齐 */
        snprintf(line, sizeof line, "%s", lab);
        r_rect(&(Rect){ 0, r.y - 2, R_BOT_W, OPT_H - 2, 0 },
               i == g_opt_sel ? w_c(C_SLOT_ACC_LT) : w_c(C_SLOT_BG));
        r_text(&(Rect){ x + 8, r.y + 3, w - 100, 16, 0 }, line,
               i == g_opt_sel ? w_c(C_SLOT_ACC_DK) : w_c(C_SLOT_FG));

        {
            int vw = text_width(val);
            r_text(&(Rect){ (float)(W_BOT_MARGIN + W_BOT_USABLE - vw), r.y + 3,
                            (float)(vw + 2), 16, 0 }, val,
                   w_c(C_SLOT_ACC_DK));
        }
    }

    /* ★ 这里原先另有一行「点一行即切换，立刻生效」（S0947），
     *   2026-10-23 删掉：页脚现在给了操作指引，两者说的都是"这一格怎么用"，
     *   两行叠在 320×240 的下屏上就是同一件事讲两遍，而内容区只剩
     *   168..205 这三十来像素 —— 留着它，第四行选项会被压到页脚边上。 */

    /* ★ 最下行操作指引（2026-10-23 用户要求）：十字键上下选行、左右切值。
     *   这一格的页脚原先没有内容，指引就写在这里。
     *   与对话框不同，这里**不轮播** —— 这一格只有四个键位要讲，
     *   一行放得下（实测 271px < 300），轮播只会让它闪。 */
    w_bot_footer(S(S0963));
}

bool dockui_touch_opt(int tx, int ty)
{
    for (int i = 0; i < OPT_ROWS; ++i) {
        Rect r;
        r.x = (float)W_BOT_MARGIN;
        r.y = OPT_Y0 + (float)i * (OPT_H + 2.0f);
        r.w = (float)W_BOT_USABLE;
        r.h = OPT_H;
        if (w_hit(&r, tx, ty)) {
            g_opt_sel = i;
            opt_step(i, +1);        /* 点一下 = 换下一档，和设置屏一致 */
            return true;
        }
    }
    return false;
}

void dockui_keys_opt(u32 down)
{
    if (down & KEY_UP)   { g_opt_sel = (g_opt_sel + OPT_ROWS - 1) % OPT_ROWS; return; }
    if (down & KEY_DOWN) { g_opt_sel = (g_opt_sel + 1) % OPT_ROWS; return; }
    if (down & (KEY_RIGHT | KEY_A)) { opt_step(g_opt_sel, +1); return; }
    if (down & KEY_LEFT)            { opt_step(g_opt_sel, -1); return; }
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
    /* 只在**没被宿主绑走**时清自己的缓冲：绑走之后 g_text 是对话的输入框，
     * 在这一屏 enter 时清它，就等于用户切一下格子就把刚打的字擦了。 */
    if (!g_txt_dst)
        g_text[0] = '\0';
    app_note("dock: enter (%d projects, %d files)",
             projects_count(), g_fn);
}

/* 键盘面板的三个按钮 */
static void key_edit(void)
{
    char buf[DOCK_TEXT_MAX];
    snprintf(buf, sizeof buf, "%s", txt());
    /* 字符数上限按 3 字节/字保守换算，避免中文把缓冲写穿
     * （同 sc_chat.c 的 open_keyboard 那条规矩）。 */
    if (ui_input_text("输入文字", buf, (int)(txt_cap() - 1) / 3,
                      txt(), txt_cap()))
        app_status_set("已输入 %d 字节", (int)strlen(txt()));
}

/* 送到对话。
 *
 * ★ 2026-10-24：这里原来是**占位实现** —— 只写一句"文字已备好，
 *   按 A 进对话继续"，因为那时本模块的 g_text 与对话屏的 g_input
 *   是两个互不相识的缓冲，"送"不过去。
 *
 *   现在宿主把文本绑了进来（见 dockui_bind_text），这里就只登记
 *   "有一次发送请求"，由对话屏在同一帧取走并真的发出去。
 *   为什么不在这里直接发：发送要用对话屏的 g_pending / 占位消息 /
 *   错误分支那一整套，把网线拉进本模块会让面板也依赖 chatapi。 */
static void key_send(void)
{
    if (!txt()[0]) {
        app_status_set("还没有输入内容");
        return;
    }
    g_send_req = true;
    app_note("dock: key_send len=%d", (int)strlen(txt()));
}

bool dockui_take_send(void)
{
    if (!g_send_req)
        return false;
    g_send_req = false;
    return true;
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
            txt()[0] = '\0';
            app_status_set("已清空");
            return true;
        }
        if (w_hit(&(Rect){ 164.0f, 168.0f, 146.0f, 36.0f, 0 }, tx, ty)) {
            key_send();
            return true;
        }
        break;

    case PANEL_PROJ:
        for (int i = 0; i < projects_count() && i < proj_new_row(); ++i) {
            /* ★ 先判「×」，后判整行（2026-10-24）。
             *   叉的矩形整个落在行矩形里面 —— 顺序反了的话，
             *   点叉会被当成"切到这一行"，删除按钮形同不存在，
             *   而且用户会以为"我明明点了叉，怎么只是切过去了"。 */
            Rect rx = proj_del_rect(i);
            if (w_hit(&rx, tx, ty)) {
                proj_remove(i);
                return true;
            }
            Rect r = row_rect(i);
            if (w_hit(&r, tx, ty)) {
                g_sel = i;
                apply_project(i);
                return true;
            }
        }
        /* 「+ 新建任务」按钮（行号与渲染共用 proj_new_row，别各算各的）。
         * ⚠️ 必须先落成局部变量再取地址 —— row_rect() 返回的是**值**，
         *   `&row_rect(...)` 编译不过（error: lvalue required as unary '&'）。 */
        {
            Rect rn = row_rect(proj_new_row());
            if (w_hit(&rn, tx, ty)) {
                new_task();
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

bool dockui_wants_lr(void)
{
    /* g_pick >= 0 = "刚建完任务，正在给挑模型"这个模态子状态。
     * 只有它在用 ←/→；其余面板一律把左右让给宿主的切格。 */
    return g_panel == PANEL_PROJ && g_pick >= 0;
}

bool dockui_wants_select(void)
{
    /* 只有项目格：SELECT = 删掉当前任务（见 dockui_keys 的 PANEL_PROJ）。 */
    return g_panel == PANEL_PROJ;
}

void dockui_keys(int panel, u32 down)
{
    (void)panel;
    switch (panel) {
    case PANEL_KEY:
        if (down & KEY_A) key_edit();
        else if (down & KEY_X) { txt()[0] = '\0'; app_status_set("已清空"); }
        else if (down & KEY_Y) key_send();
        break;

    case PANEL_PROJ: {
        int n = projects_count();

        /* ★ "刚建完，正在挑模型"状态：这一小段**接管全部按键**，
         *   免得用户在挑的时候误按 A 又把任务重开一遍。
         *   ←/→ 换一家的下一把、A 敲定、B 保持默认、其余键忽略。 */
        if (g_pick >= 0) {
            if (down & KEY_LEFT)  { cycle_task_api(g_pick, -1); announce_pick(); }
            else if (down & KEY_RIGHT) { cycle_task_api(g_pick, +1); announce_pick(); }
            else if (down & KEY_A)     { g_pick = -1; app_status_set(S(S0936)); }
            else if (down & KEY_B)     { g_pick = -1; app_status_set(S(S0937)); }
            return;
        }

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

        if (down & KEY_SELECT) { proj_remove(g_sel); return; }
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
