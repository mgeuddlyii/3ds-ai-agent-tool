/*
 * sc_home.c -- 主界面（M2 占位，但已经承载一个真实职责）
 *
 * 除了把读回的 config 摊开证明落盘成功，它现在还负责一件事：
 * **成为"我的 AI 到底配好没有"的唯一真相来源，以及回去配的入口。**
 *
 * 为什么这必须放在主界面：新手教程是可以跳过的。一旦可以跳过，
 * 就得保证跳过去的人看得见"我还差一步"，并且随手就能回去 ——
 * 否则那个"以后再搞"永远没有以后。
 *
 * 于是进屏时扫一次 SD 卡上的凭据文件，实时判定，不额外存布尔：
 * 文件里有就是有，用户随时可能在电脑上改，存个标志反而会过期。
 *
 * M3 会把这里换成 对话 / 设置 / 文件 的真菜单，本屏内容下移到「设置 → 关于」。
 *
 * 顺带承载两件已定义的行为：
 *   · 未备份 NAND 的常驻提醒条（SAFETY_DESIGN.md §1，按 Y 关掉）
 *   · M1 验收页的入口（回归用，见 sc_lab.c）
 */

#include "app.h"
#include "credfile.h"
#include "doubao.h"
#include "netprobe.h"
#include "widgets.h"
#include "font.h"

#include <stdio.h>
#include <string.h>
#include "uistr.h"

#define HOME_FONT_LINE_SLOT 1

/* 底屏三个按钮的矩形：render 与 handle（触摸命中）共用同一份，
 * 防止两处手写坐标漂移。y=150、高 36 —— 量过的，见 render_bottom。
 *
 * 「离线攻略」放在第三格而不是插在前面：前两格的坐标被
 * tools/azahar_test.py 的 J_BTN_CHAT / J_BTN_KEY 直接引用
 * （(82,168) 与 (238,168)），动它们会让既有验收脚本的点按落到
 * 别的按钮上。第三格是新增的右侧空位，不动前两格就不用重跑改脚本。 */
/* 第一行三个格子：开始对话 / 服务目录 / 设置。
 *
 * ⚠️ 动这几格之前先算一遍验收脚本的触摸点：
 *   · **(82,168) 必须仍然落在「开始对话」里** —— 它是 J 趟唯一真的点过的
 *     主界面按钮（脚本里的 J_BTN_CHAT）。所以 CHAT 保持在最左格、宽 92。
 *   · (238,168) 是脚本里 J_BTN_KEY 的坐标，但**它定义了却没被任何
 *     touch 指令用过**（全脚本只有 3 条 touch）。所以把「配 API Key」
 *     换成「设置」不会让既有趟次失败。
 *     将来若有人真去点 (238,168)，会点到设置屏 —— 那时请同步改脚本常量。
 *
 * 「配 API Key」不再单独占一格：**按服务商配凭据是服务目录的职责** ——
 * "这把 Key 是哪一家的"只有那一屏说得清，主界面上一个孤零零的按钮
 * 反而要求用户先知道这个区分。 */
/* ★ 主页只放**核心**（2026-10-17 用户拍板）：
 *   开始对话（通栏）/ 离线攻略 / 更多，外加右上角一个常驻「设置」。
 *
 *   为什么必须砍：说明文字占 30..66、配置路径面板 74..110，
 *   而按钮最早只能从 114 起 —— 8 个格子塞进 98px 一定会压到面板。
 *   实测 2 列 × 4 行（从 98 起）时面板与第一排**重叠 12px**，
 *   截图一眼能看出来，却因为 w_vbounds 只判 y+36 而漏掉了前 12px。
 *   现在按钮从 114 起、只占两排，面板完整可见。 */
#define HOME_BTN_W 300.0f        /* 主按钮通栏 */
#define HOME_BTN_H 32.0f
#define HOME_HALF_W 144.0f
/* ★ 四排的 y 必须**全部在这里**声明。
 *   踩过一次：PROV 那行用了 HOME_BTN3_Y，而它定义在 20 行之后 ——
 *   编译器报 "'HOME_BTN3_Y' undeclared here"，指的却是 PROV 那一行，
 *   看起来像拼写错误，实际是顺序问题。 */
#define HOME_BTN_Y  114.0f      /* 第一排（说明+面板之后） */
#define HOME_BTN3_Y 150.0f      /* 第二排 */
#define HOME_BTN4_Y 190.0f      /* 未用（保留给将来的第三排） */
#define HOME_BTN5_Y 222.0f      /* 未用 */
static const Rect HOME_BTN_CHAT = { W_BOT_MARGIN, HOME_BTN_Y,
                                   HOME_BTN_W, HOME_BTN_H, 0 };
static const Rect HOME_BTN_GUIDE = { W_BOT_MARGIN, HOME_BTN3_Y,
                                    HOME_HALF_W, HOME_BTN_H, 0 };
static const Rect HOME_BTN_MORE  = { W_BOT_MARGIN + HOME_HALF_W + 12.0f,
                                    HOME_BTN3_Y, HOME_HALF_W, HOME_BTN_H, 0 };

/* 右上角的「设置」：**不进网格**，所以方向键选不到它 ——
 * 靠触摸，或按 X（页脚有提示）。原因是这一格在标题行里（y=6），
 * 网格导航按 row/col 算，把它塞进去会让"上下移动保持列"的直觉失效。
 * 56×22 是量过的：「设置」两个字 32px + 内边距 6 + 余量。 */
/* ★ 2026-10-26：56 宽 → 66 宽。
 *   按钮内可用宽 = r->w - 6 = 50px，而英文 "Settings" 实测 51px ——
 *   差 1px 就被排版护栏记成溢出（中文「设置」32px，所以一直没暴露）。
 *   宁可把按钮加宽 10px，也不要为了省这点地方去把英文改成别扭的缩写。 */
static const Rect HOME_BTN_SETTING = { (float)R_BOT_W - 66.0f - 8.0f, 6.0f,
                                       66.0f, 22.0f, 0 };

/* 进屏时扫一次。不缓存到 config —— 用户随时可能改卡上的文件，
 * 存下来的布尔会过期，不如每次进来都老实读一遍。 */
static CredFile g_cred;
static bool     g_has_key;
/* 进屏时算一次的结果（见下面 cred_any 的说明）。 */
static bool     g_cred_ok;

/* "这台机器有没有任何可用的凭据"。
 *
 * **不能只看 g_has_key**：豆包走的是登录态、不走 API Key，只看 Key 的话
 * "已经扫码登录好豆包"的用户在主界面仍然会看到"还没配好"的强调色，
 * 于是他会以为白扫了，再去找 Key —— 而豆包压根没有 Key 可配。
 * 这一条和 sc_providers.c 里那套"凭据分三种"的判断是同一个道理。
 *
 * ⚠️ **只读 g_cred_ok，不许在这里现算**。这里原本写的是
 * `g_has_key || (db_load_cookie() && db_has_cookie())`，而它被
 * render_bottom 每帧调用 —— 于是**每一帧都读一次卡上的 cookie 文件**，
 * 而 `hx_cookie_load()` 内部还有 8KB 的栈缓冲（`char line[8192]`）。
 * 主界面是停留最久的一屏，这个开销是白烧的。
 * 改为进屏算一次（home_enter），渲染期只读一个布尔。 */
static bool cred_any(void)
{
    return g_cred_ok;
}

static bool home_show_banner(void)
{
    const AppConfig *c = app_cfg();
    return !c->nand_backup.confirmed && c->nand_backup.remind_on_startup;
}

/* ---- 方向键导航的焦点 ----
 *
 * ★ 为什么以前"移动选项没反应"：home_handle 里**一个方向键分支都没有**
 *   （A/B/X/R/L/SELECT/Y/ZL 全是直接跳屏），所以选中态这个东西压根不存在。
 *   不是"焦点画错了"，是"根本没有焦点"。
 *
 * 焦点存在哪：静态变量，不进 config —— 它是**这一瞬间的临时状态**，
 *   退出屏就该清零，存进配置会在下次开机时莫名高亮一个格子。
 * 起点选「开始对话」：它是这个软件存在的理由，也是用户最常去的那格。 */
static int g_focus;               /* 0..6，与 g_slots 顺序一致 */

/* 格子的唯一清单：绘制、触摸、方向键、A 键跳转**都走这一张表**。
 * 早先是四条独立的 if（触摸）、三条 w_button、六个 Rect 各写一遍 x ——
 * 加一格要改五个地方，漏一处就"看得到点不到"或者"能按但不显示高亮"。
 * 那个 bug 已经发生过一次（通栏的「已设区域」把两个主按钮盖住，
 * 见上面 HOME_BTN3_Y 那段注释），所以这次合成一张表。 */
/* 槽位序号。★★ **顺序 = 屏幕上从上到下的顺序**，不是"当初谁先写的"。
 *
 * 这里踩过一个坑（2026-10-10 真机反馈「十字键只能在下排切换，上排按不到」）：
 * 原先表是 CHAT/PROV/SET 打头，可它们画在 y=150（屏幕**中间**那排），
 * 而 GUIDE/LOCALE/CONTENT 画在 y=114（屏幕**最上面**那排）。
 * 于是 slot_row() 算出来的"第 0 行"其实是屏幕中间 ——
 * 焦点在中间时按↑得到 row -1，slot_at() 判越界返回 -1，焦点原地不动。
 * **表里的行号必须与眼睛看到的上下一致**，方向键才符合直觉。
 *
 * 用具名常量而不是 0..6 的裸数字：这样"顺序"只有这一处可改，
 * 下面所有 `g_focus == SLOT_XXX` 会跟着一起对。 */
enum {
    /* ★ 必须与 g_slots[] / g_slot_screen[] **逐项对应**（按下标索引）。
     * 只有 3 个：主页只放核心，其余进「更多」。 */
    SLOT_CHAT = 0,
    /* ★ 2026-10-23 真机反馈：把「离线攻略」换成「配置 API」。
     *   理由（用户原话）：配 API 才是第一次打开必做的事，而攻略按 ZL 就能进 ——
     *   占着主页一格却把"最该做的事"藏在「更多」里，顺序反了。
     *   攻略**没删**：ZL 仍直接进 SCREEN_GUIDE，页脚也写着 [ZL] 攻略。 */
    SLOT_APIKEY,
    SLOT_MORE,
};
static const Rect *g_slots[] = {
    &HOME_BTN_CHAT, &HOME_BTN_GUIDE, &HOME_BTN_MORE
};
#define HOME_SLOTS_N ((int)(sizeof g_slots / sizeof g_slots[0]))
/* 每格跳到哪一屏。顺序必须与 g_slots 严格一致 —— 判据里会比这一列。 */
extern void chat_set_translate(int on);

static const ScreenId g_slot_screen[] = {
    /* ★ 2026-10-23：第二格去 **服务目录（厂商列表）** 而不是直接进录入屏。
     *   用户原话「改回以前那样列出一些知名厂商的 API 填入」——
     *   在列表里选一家，比在录入屏里用左右键循环 16 家要容易得多
     *   （循环既看不到全貌，也不知道后面还有哪些）。 */
    SCREEN_CHAT, SCREEN_PROVIDERS, SCREEN_MORE
};

/* 每行的格子数：方向键左右怎么走取决于这个（3 列的网格，
 * 左移要跳到上一行末尾，而不是退一格 —— 退一格会在行首"原地不动"，
 * 那是三维游戏的手感，而这里是二维网格）。 */
#define HOME_COLS 3
static int slot_row(int i) { return i / HOME_COLS; }
static int slot_col(int i) { return i % HOME_COLS; }
static int slot_at(int row, int col)
{
    int i = row * HOME_COLS + col;
    if (row < 0 || i >= HOME_SLOTS_N)
        return -1;
    return i;
}

static void home_enter(void)
{
    g_has_key = credfile_scan(&g_cred);

    /* 凭据状态在**进屏时**算一次就够：这一屏停留时间最长，
     * 每帧去读卡上的文件是白烧（见 cred_any 的说明）。
     * 用户真要改凭据，也是去别的屏改完再回来 —— 回来就会重进这一屏。 */
    g_cred_ok = g_has_key || (db_load_cookie() && db_has_cookie());

    /* 每次进屏都回到「开始对话」。焦点是**这一瞬间的临时状态** ——
     * 不重置的话，用户上次停在「内容更新」、这次从别的路进来，
     * 打开主界面就看到一个陌生格子被选中，而 A 键会跳到他没预期的屏。 */
    g_focus = SLOT_CHAT;

    if (g_has_key)
        app_status_set(S(S0340));
    else
        app_status_set(S(S0341));
}

static void render_kv_rows(float x, float y, float w)
{
    const AppConfig *c = app_cfg();
    char v[96];

    y = y;  /* 版面常数集中在下方，避免每行重算 */

    /* 首次启动 */
    snprintf(v, sizeof v, S(S0342), c->launches);
    y = w_kv(x, y, 78, S(S0343), v, w_c(C_SLOT_MUTED), w_c(C_SLOT_OK), TEXT_LINE_H);

    /* NAND 备份 */
    if (c->nand_backup.confirmed)
        snprintf(v, sizeof v, "%s", S(S0344));
    else if (c->nand_backup.remind_on_startup)
        snprintf(v, sizeof v, "%s", S(S0345));
    else
        snprintf(v, sizeof v, "%s", S(S0346));
    y = w_kv(x, y, 78, S(S0347), v, w_c(C_SLOT_MUTED),
             c->nand_backup.confirmed ? w_c(C_SLOT_OK) : w_c(C_SLOT_WARN_DK), TEXT_LINE_H);

    /* AI 服务 */
    snprintf(v, sizeof v, S(S0348), c->ai.provider, c->ai.model,
             c->ai.stream ? S(S0349) : S(S0350));
    y = w_kv(x, y, 78, S(S0351), v, w_c(C_SLOT_MUTED), w_c(C_SLOT_FG), TEXT_LINE_H);

    /* API Key —— 这一行直接来自卡上的文件，不是配置里的布尔 */
    if (g_has_key) {
        char masked[40];
        credfile_mask(g_cred.key, masked, sizeof masked);
        snprintf(v, sizeof v, S(S0352), masked);
        y = w_kv(x, y, 78, "API Key", v, w_c(C_SLOT_MUTED), w_c(C_SLOT_OK), TEXT_LINE_H);
    } else {
        snprintf(v, sizeof v, "%s", S(S0353));
        y = w_kv(x, y, 78, "API Key", v, w_c(C_SLOT_MUTED), w_c(C_SLOT_WARN_DK), TEXT_LINE_H);
    }

    /* 安全等级：强调"本会话" */
    snprintf(v, sizeof v, S(S0354), security_level_name(c->security.current_level));
    y = w_kv(x, y, 78, S(S0215), v, w_c(C_SLOT_MUTED), w_c(C_SLOT_FG), TEXT_LINE_H);

    /* 界面。主题显示成中文名而不是 config 里的原始串（"dark"）——
     * 用户看到的是"黑夜"，跟设置屏里的说法对上。 */
    snprintf(v, sizeof v, S(S0355),
             w_theme_name(!strcmp(c->ui.theme, "dark") ? W_THEME_DARK : W_THEME_LIGHT),
             c->ui.font_size, (int)(c->ui.depth * 100.0f + 0.5f));
    y = w_kv(x, y, 78, S(S0356), v, w_c(C_SLOT_MUTED), w_c(C_SLOT_FG), TEXT_LINE_H);

    (void)w;
}

static void home_render_top(void)
{
    const float x = W_TOP_MARGIN;
    const float w = W_TOP_USABLE;
    float y = (float)W_TOP_CONTENT_Y;

    const AppConfig *c = app_cfg();

    char right[32];
    snprintf(right, sizeof right, S(S0357), c->launches);
    w_top_header(S(S0358), right);

    const bool banner = home_show_banner();

    if (banner) {
        y += w_banner(x, y, w, S(S0359)) + 4.0f;
    }

    w_hr(x, y, w, w_c(C_SLOT_LINE));
    y += 8;

    render_kv_rows(x, y, w);
    y += 6 * (float)TEXT_LINE_H;

    y += 4;
    w_hr(x, y, w, w_c(C_SLOT_LINE));
    y += 8;

    /* 有横幅时不再画这一行：提醒的优先级高于开发信息，
     * 而且两行一起放会越过内容区下边界（200px）。 */
    if (!banner) {
        char fl[80];
        snprintf(fl, sizeof fl, S(S0360),
                 font_glyph_count(), font_cell_w(), font_cell_h(), font_baseline());
        w_line(x, y, w, fl, w_c(C_SLOT_MUTED));
    }

    w_top_footer(app_status(), S(S0361));
}

static void home_render_bottom(void)
{
    const float x = W_BOT_MARGIN;
    const float w = W_BOT_USABLE;
    float y = (float)W_BOT_CONTENT_Y;

    w_bot_header(S(S0362));

    /* ⚠️ 这段说明**必须压在 2 行以内**（含下面那句强调色的话）。
     *
     * 为什么：下面「配置路径」面板高 36，再往下就是两排按钮 ——
     * 第二排的 y 是 114（HOME_BTN3_Y，被验收脚本的触摸点依赖，不能动）。
     * 内容区从 30 开始，到 114 只有 84px：面板占 44（36+8），
     * 只剩 40px 给文字，也就是**两行**。
     *
     * 2026-10-04 实测踩过：文字写成了 3 行（48px），面板落到 86..122，
     * 与第二排按钮（114 起）**重叠 8px** —— 面板里的路径被按钮压掉一半。
     * 两个 Rect 各自都合法，读代码看不出来，是截图才照出来的。
     * 所以下面加了 w_vbounds：以后谁再加一行字，回归里的
     * `layout_voverflow` 会直接报出来，而不是等哪次翻截图才发现。 */
    if (cred_any()) {
        y = w_wrap(x, y, w,
                   S(S0363),
                   w_c(C_SLOT_FG), TEXT_LINE_H);
    } else {
        y = w_wrap(x, y, w,
                   S(S0364),
                   w_c(C_SLOT_FG), TEXT_LINE_H);
    }
    y = w_wrap(x, y, w,
               S(S0365),
               w_c(C_SLOT_ACCENT), TEXT_LINE_H);

    /* ★★ 2026-10-23 真机反馈：「最上面那个配置路径被挡住了，直接删掉」。
     *
     *   它被挡住**不是偶发**：这段注释原本就写着 2026-10-04 面板与第二排
     *   按钮重叠过 8px。删掉的理由有三条：
     *     ① 它显示的是 `sdmc:/3ds/aiconsole/` —— 用户在 3DS 上**没法用**这个
     *        路径做任何事（没有文件管理器），知道了也只能记在纸上；
     *     ② 需要它的时候（往卡里拷 key.txt）用户是坐在电脑前的，
     *        那时看的是 README，不是这块 400×240 的屏；
     *     ③ 它占掉了唯一一块能放"该怎么开始"的位置，还把下面的按钮挤得
     *        叠上来 —— 净收益是负的。
     *
     *   路径仍然在「设置 → 关于」里看得到，那里空间够、不会被压。
     *   上面的说明文字（S0363/S0364/S0365）保留 —— 那是引导，不是装饰。
     */


    /* 第一行三个格子：开始对话 / 服务目录 / 设置。
     * 按下态由 app_touch_on() 实时判定 —— 手压在哪个按钮上，哪个就下陷。 */
    w_button(&HOME_BTN_CHAT, S(S0367),
             (g_focus == SLOT_CHAT ? W_BTN_SELECTED : W_BTN_NORMAL)
             | (app_touch_on(&HOME_BTN_CHAT) ? W_BTN_PRESSED : 0));
    w_button(&HOME_BTN_MORE, S(S0888),
             (g_focus == SLOT_MORE ? W_BTN_SELECTED : W_BTN_NORMAL)
             | (app_touch_on(&HOME_BTN_MORE) ? W_BTN_PRESSED : 0));

    /* 右上角常驻「设置」。**最后画** —— 它压在标题行右侧，
     * 后画才不会被 header 的底色盖掉。 */
    w_button(&HOME_BTN_SETTING, S(S0369),
             app_touch_on(&HOME_BTN_SETTING) ? W_BTN_PRESSED : 0);


    /* 「服务目录」：切换豆包/其他 API 的总入口。
     *
     * ★ 2026-10-10 改：**不再**在"还没配凭据"时用 W_BTN_PRIMARY 强调。
     *   用户原话「主界面不要搞有些明亮有些暗，全部都改成同样的色调」。
     *   原先这一格和「离线攻略」是实心主色（PRIMARY），另外五格是白底，
     *   屏上确实是一明一暗两块。而"谁更重要"已经由位置与文案表达了 ——
     *   再叠一层颜色，代价是整屏变得花，换来的是用户要多想一次
     *   "这两个为什么不一样"。
     *   **焦点态（SELECTED）仍然要区分**，那是"我现在选中了谁"的唯一反馈，
     *   所以只保留它。 */


    /* 「配置 API」：替换掉原来的「离线攻略」格。
     * 为什么它值得占主页一格：**第一次打开时这里是唯一必做的事** ——
     * 不配 Key 只能待在离线演示里。而攻略是熟客才用的，按 ZL 一下就到。
     * 矩形常量名字仍叫 HOME_BTN_GUIDE（改名字会牵动 slot 表、触摸命中、
     * 判据里的坐标），只改标签与去向 —— 位置对用户没意义，语义才重要。 */
    w_button(&HOME_BTN_GUIDE, S(S0370),
             (g_focus == SLOT_APIKEY ? W_BTN_SELECTED : W_BTN_NORMAL)
             | (app_touch_on(&HOME_BTN_GUIDE) ? W_BTN_PRESSED : 0));

    /* 「已设区域」：查看/撤销给游戏设过的区域语言。
     * 它替代不了对话式的"给某游戏设区域"（那个在对话里由 AI 触发），
     * 这里只是**回头看和反悔**的入口 —— 没有它，用户在 3DS 上
     * 根本看不到自己动过哪些文件。 */

    /* 「内容更新」：模型 / 攻略库 / 硬件知识库。与上面两个并排放在
     * 同一行 —— 三者都是"进去看看或改改"的地方，扫一眼就能找到，
     * 不需要用户在脑子里做分类。 */

    /* 第三排：从电脑取文件。★ 必须常显在主界面上 —— 它是把游戏
     * 从电脑搬到卡上的**唯一**入口，藏进多层页面等于没有。
     * 旁边那一格留空（不放"未做"的东西），空着比填一个灰按钮诚实。 */

    /* 「插件」：管理 luma/plugins 下的 3gx。同样式（用户要"不要一明一暗"），
     * 只有焦点态区分。 */

    /* 键位提示放在**上排与下排之间的空隙**（144..150 只有 6px，放不下）——
     * 所以它必须与第三排按钮**同行左右分置**：按钮在左 104px（见 HOME_BTN_XFER_W），
     * 提示从 x = margin+116 开始，中间留 12px 缝，同一基线不重叠。
     *
     * ★ 为什么不能另起一行：下屏 3 排按钮已经吃掉 114..212，
     *   页脚上沿是 214 —— 剩下 2px，放不下任何一行文字。
     *   早先这里把提示放在 y=196，结果它**压在第三排按钮上面**，
     *   而这个"按钮互相盖住"的 bug 这屏已经犯过一次
     *   （通栏的「已设区域」盖住两个主按钮，见 HOME_BTN3_Y 那段注释）。
     *   同一个屏、同一种病，不该犯第二次。
     * ★★ 两个由 2026-10-10 修掉的老问题（都是**静默**的，没人看得见）：
     *   1. 原来用 `r_text` 直接画 —— 而 r_text **不进 note_overflow**，
     *      所以文本超宽只会在右边缘**静静被裁掉**，验收趟次永远绿。
     *      原串 276px 而可用只有 196px，「配 AI [L] 设置」这一段
     *      用户从来就没看见过。改用 `w_line`（内部调 note_overflow）后，
     *      超宽会记进 layout_overflow，趟次能抓到。
     *   2. 原矩形高只有 **12px**，而字形格高是 **16px** —— 每个字的底部
     *      4 行被裁掉了。y=195 + 18 = 213 < 页脚上沿 214，放得下整行，
     *      所以直接用 w_line 的 TEXT_LINE_H(18)，不再自减高度。
     *   ⚠️ 现串 168px <= 可用 184px，留 16px 余量。改字前先量宽度
     *      （PC 侧读 font.bin 复刻 text_width 即可，不必开模拟器）。 */
    w_line((float)W_BOT_MARGIN + 116.0f, HOME_BTN4_Y + 5.0f,
           (float)R_BOT_W - W_BOT_MARGIN * 2.0f - 116.0f,
           S(S0373), w_c(C_SLOT_FAINT));

    w_bot_footer(cred_any() ? S(S0374) : S(S0375));
}

static void home_handle(u32 down, u32 held)
{
    (void)held;

    /* 触摸：模拟器里鼠标点底屏 = 触摸，真机手指同理。
     * 触摸只有按下沿那一帧，先查它，再看按键。 */
    int tx, ty;
    if (app_touch_down(&tx, &ty)) {
        if (w_hit(&HOME_BTN_SETTING, tx, ty)) {   /* 齿轮不在网格里，单独判 */
            app_note("home: touch -> settings");
            app_push(SCREEN_SETTINGS);
            return;
        }
        for (int i = 0; i < HOME_SLOTS_N; ++i) {
            if (w_hit(g_slots[i], tx, ty)) {
                g_focus = i;
                chat_set_translate(0);
                app_push(g_slot_screen[i]);
                return;
            }
        }
    }

    /* ---- 方向键：移动焦点 ----
     *
     * ★ KEY_* 是"通用方向"别名：十字键与 C-Stick 共享同一批位，
     *   所以这里**不需要**再判 KEY_DUP/KEY_CPAD_UP 之类 ——
     *   判断 KEY_UP 就同时覆盖了上下两种输入。
     *   （这条与 konami.c 里踩过的坑同源：那边是 `key <= 0` 把 KEY_DOWN
     *   这个"在 int 下为负数"的别名丢掉了。）
     *
     * 上下移动时**保持列**而不是保持序号：网格里上下对齐才是直觉。
     * 目标格不存在（最后一行不满）时停在原处而不是跳到别的行 ——
     * "按了没反应"比"跳到 unexpectedly 的格子"好，后者会让用户
     * 在按 A 之后莫名其妙进了另一屏。 */
    {
        int r = slot_row(g_focus), c = slot_col(g_focus), t;

        /* 3 个槽排成一行，所以**上下与左右同义**
         * （只认左右的话，用户按上下会以为焦点坏了）。 */
        if (down & (KEY_LEFT | KEY_UP))
            t = slot_at(r, c > 0 ? c - 1 : HOME_COLS - 1);
        else if (down & (KEY_RIGHT | KEY_DOWN))
            t = slot_at(r, c + 1 < HOME_COLS ? c + 1 : 0);
        else
            t = -1;

        if (t >= 0) {
            g_focus = t;
            app_note("home: focus=%d (%d,%d)", t, slot_row(t), slot_col(t));
            /* 移动焦点时把按键"吃掉"，不再往下走 A/B/X 那些分支 ——
             * 不 return 的话，一次按键可能既移动又跳屏。 */
            return;
        }
    }

    /* A：打开**当前焦点**那一格。
     * 与早先"恒定进对话"不同：用户移动过焦点就是要打开那一格，
     * 恒定跳对话会让方向键看起来"没反应"（焦点在动，A 却总是同一个结果）。 */
    if (down & KEY_A) {
        chat_set_translate(0);   /* 主页进的是普通对话，别残留翻译模式 */
        app_push(g_slot_screen[g_focus]);
        return;
    }

    /* ★★ 2026-10-23：**主页的 B 不再进 M1 验收页**（原 `app_push(SCREEN_LAB)`）。
     *
     * 那条入口是 M1 阶段的临时脚手架（注释原话"它是回归入口不是常用功能，
     * M3 做真菜单之后这个键位会被菜单取代"），M3 早就做完了，它却留了下来。
     * 后果很具体：主页是最常用的屏，而 **B 是用户"返回"的本能键** ——
     * 按一下就掉进字库巡检页，页面上全是字形和帧缓冲数据，页脚也没提示。
     *
     * 验收脚本从不走这条路（grep SCREEN_LAB 在 azahar_test.py 里 0 命中），
     * 所以删掉它对回归零影响。M1 验收页本身**没有删**，它仍可从
     * SCREEN_LAB 的注册处按 id 进入（需要时改一行 app_push 即可）。
     *
     * 主页现在按 B 什么都不做 —— 它是栈底屏，本来也无处可退。 */

    /* 不管配没配好，都进得去 —— 教程本身就有"重新读取"和"跳过"，
     * 配好的人进去只是想再看一眼放哪。 */
    if (down & KEY_X)
        app_push(SCREEN_KEYS);       /* 多 Key 仓库；申请教程从那里 SELECT 进 */

    /* 软件更新。永远只提示、不自动装 —— config 里的 auto_check
     * 决定的是"要不要提醒"，不是"要不要装"，两者不能合并。 */
    if (down & KEY_R)
        app_push(SCREEN_UPDATE);

    /* 设置：主题、字号、护栏、诊断开关。放 L 键是因为它不常用，
     * 但又必须从主界面一步可达（藏进别处等于没有）。 */
    if (down & KEY_L) {
        app_push(SCREEN_SETTINGS);
        return;
    }

    /* ⚠️「已设区域」**故意不设快捷键**，入口只有底屏那个按钮。
     *
     * 它曾经挂在 ZR 上（与 ZL=攻略 配对），但那是个 bug：
     * main.c 里 **ZR 是全局截图热键**，而全局键在 app_handle 之前处理 ——
     * 于是一按 ZR 会同时截图并跳屏。main.c 对此有明文规矩：
     * "全局热键不能抢屏幕键位，谁后做谁挪"。
     * 这条屏是后加的，该挪的是它，不是截图。
     *
     * 权衡：底屏按钮是**常驻可见**的入口（不像快捷键要靠人记住），
     * 少一个隐藏快捷键换来不跟全局键打架，这个交换划得来。
     * KEY_Y 之类的也都被别的屏占了（见 sc_chat/sc_guide/sc_keys…），
     * 不值得为了对称再挤一个键出来。
     *
     * 所以：**看到这段注释想给它加回快捷键的，先去看 main.c:193。** */

    /* ★★ 2026-10-23：**主页的 SELECT 不再做网络自检**。
     *
     * 两个理由，缺一都不够：
     *   ① 它**阻塞**（httpc 是同步的）。主页按一下，没网就卡几秒，
     *      期间界面完全不动 —— 用户看到的是"程序死了"，而不是"在探测"。
     *      而主页恰恰是最不该卡住的那一屏。
     *   ② 它不是"功能"，是排障工具。正常用户按 SELECT 只会得到一句
     *      他看不懂的 HTTP 状态码。
     *
     * 功能没删，而是搬到**设置屏 → 诊断**分类里（R_ROW_NETPROBE）——
     * 那里本来就是"出问题时才来看"的地方，且用户是主动去找的，
     * 卡一下有心理预期。 */

    if (down & KEY_Y) {
        AppConfig *c = app_cfg();
        if (!c->nand_backup.confirmed && c->nand_backup.remind_on_startup) {
            c->nand_backup.remind_on_startup = false;
            app_cfg_dirty();
            app_cfg_save_now();
            app_status_set(S(S0378));
            app_note("home: user dismissed nand reminder with Y");
        } else {
            app_status_set(S(S0379));
        }
    }

    /* 离线攻略。全离线功能，**从主界面一步可达**是它的全部意义 ——
     * 藏进设置深处就等于没有：一个连 API Key 都没配的人，
     * 打开这个 app 就应该还能拿到点有用的东西。
     *
     * 键位用 ZL：3DS 没有 G 键（面键只有 A/B/X/Y），而 A/B/X/R/L/SELECT/Y
     * 六个键位全被既有功能占了。ZL 是唯一剩下的、且 app/source/test/autorun.c
     * 的 KEY_TABLE 里已有名（能注入）的键 —— 验收脚本因此可以走这条路，
     * 不用新增 autorun 键名。
     *
     * 这个键位是**加在末尾**的：tools/azahar_test.py 的既有趟次只按
     * A/B/X/R/L/SELECT/Y 和两个固定坐标点按，没有"按下标选主菜单项"
     * 这种操作，所以新加键位不会改变既有趟次的任何一步。见交付说明。
     *
     * 真机上 ZL 是 New 3DS 才有的键，Old 3DS 用户走底屏那个按钮 ——
     * 两条路都留着，不押单一条。 */
    if (down & KEY_ZL) {
        app_push(SCREEN_GUIDE);
        return;
    }
}

const Screen g_screen_home = {
    .id            = SCREEN_HOME,
    .name          = "SCREEN_HOME",
    .enter         = home_enter,
    .leave         = NULL,
    .render_top    = home_render_top,
    .render_bottom = home_render_bottom,
    .handle        = home_handle,
};
