/*
 * sc_welcome.c -- 首启引导（3 页，不可跳过）
 *
 * 第 1 页 · 能力边界：先讲"不会做什么"。这不是免责声明，是真实边界告知 ——
 *              3DS 上的 AI 应用如果连边界都说不清，用户没法放心用。
 * 第 2 页 · NAND 备份提醒（核心）：不强制，但持续可见。
 * 第 3 页 · AI 服务：本应用不自建服务端（设计规划.md D1），
 *              所以没有"设备码登录"这一步，改成说明 + 稍后填 Key。
 *
 * 版面：内容区只有 26..200 共 174px，一页放不下四段话。
 * 所以把"不会做什么"和按钮一起放到**底屏** —— 两块屏一起用，这是 3DS 的本分。
 *
 * 全部文案是编译期常量。引导流程里不放任何来自 AI 的文本。
 */

#include "app.h"
#include "widgets.h"

#include <stdio.h>
#include <string.h>
#include "uistr.h"

#define WELCOME_PAGES 4

/* 底屏按钮行 / 提示行的 y。提示必须落在内容区下边界（W_BOT_CONTENT_B = 214）
 * 之上 —— 早先按 198 放，那一行直接压到页脚条上了。 */
#define BOT_BTN_Y   150
#define BOT_HINT_Y  190

/* 每页要点一句话。顶屏页脚放它，底屏页脚放按键提示 ——
 * 两块屏各说各的，不重复同一句。 */
/* ★ 存编号不存字符串：静态初始化里 S() 放不下
 * （编译报 "initializer element is not constant"，却指着 uistr.h）。 */
static const short PAGE_POINT[WELCOME_PAGES] = { S0873, S0650, S0651, S0652 };

static int g_page;
static int g_choice;      /* 第 2 页：0 = 我已备份，1 = 稍后提醒 */
/* 第 3 页选的是"哪个按钮"：0 = 录入凭据，1 = 完成设置。
 *
 * ★ 默认值必须是 **1（完成设置）**，这不是审美问题：
 *   tools/azahar_test.py 的 A / L 趟在这一页只按一次 A 就往下走，
 *   默认选中"录入凭据"的话，那一趟会一头撞进软键盘 ——
 *   而模拟器里 swkbd 弹的是 Qt 窗口，脚本够不着，整趟直接挂死。
 *   改动这里的默认值 = 同时改那两趟的脚本，别只改一边。 */
static int g_p2_choice;
static bool g_started;    /* 见 welcome_enter 里那段"为什么不能每次复位" */

/* --------------------------------------------------------------------------
 * 第 1 页
 * ----------------------------------------------------------------------- */

/* ★ 存编号不存字符串：静态初始化里 S() 放不下（理由见 sc_lab.c） */
static const short CAN_DO[] = { S0653, S0654, S0655, S0656 };
#define CAN_DO_N ((int)(sizeof(CAN_DO) / sizeof(CAN_DO[0])))

static const short CANT_DO[] = { S0657, S0658, S0659 };
#define CANT_DO_N ((int)(sizeof(CANT_DO) / sizeof(CANT_DO[0])))

/* ---- 第 1 步：选界面语言 ----
 *
 * 为什么单独占一屏，而不是塞进后面某一屏的角落：
 * 它是**唯一一个"选错了整屏都看不懂"的选项** —— 后面每一步的操作提示
 * 都靠文字表达，语言不对就等于没有提示。所以放在最前面，
 * 且选完立刻重画（uistr_set_lang 之后下一帧全部文案就变了，
 * 用户当场看到效果，不用等进主界面）。
 */
static int g_lang_sel;      /* 0 = 中文，1 = English */

static void render_plang_top(void)
{
    const float x = (float)W_TOP_MARGIN;
    const float w = (float)W_TOP_USABLE;

    w_wrap(x, (float)W_TOP_CONTENT_Y, w, S(S0872),
           w_c(C_SLOT_FG), TEXT_LINE_H);
}

static void render_plang_bottom(void)
{
    const float x = (float)W_BOT_MARGIN;
    const float w = (float)W_BOT_USABLE;
    const float bw = (w - 16.0f) / 2.0f;
    float y = (float)W_BOT_CONTENT_Y;

    w_bot_header(S(S0856));

    /* 两个选项各一个按钮，**选中的那个用主色** ——
     * 只靠文字加不加括号区分，在 3DS 的小屏上不够明显。 */
    /* ★ 2026-10-26 修：这里原来写的是 **S0001**，而 S0001 的真实内容是
     *   「这个键还没有对应实现（内部问题）」—— 也就是说新手引导第一屏的
     *   中文选项按钮，一直显示的是那句占位提示（英文界面下更长，直接把
     *   136px 的按钮撑到 314px，被排版护栏记了一条溢出）。
     *   这是"占位文案忘了换"留下的，改用「中文」(S0946)。 */
    w_button(&(Rect){ x, BOT_BTN_Y, bw, 36, 0 },
             S(S0946), g_lang_sel == 0 ? W_BTN_PRIMARY : W_BTN_NORMAL);
    w_button(&(Rect){ x + bw + 16.0f, BOT_BTN_Y, bw, 36, 0 },
             "English", g_lang_sel == 1 ? W_BTN_PRIMARY : W_BTN_NORMAL);

    w_center((float)R_BOT_W / 2.0f, BOT_HINT_Y, S(S0871), w_c(C_SLOT_FAINT));

    y += 0;   /* 版面：标题下留给按钮区，正文说明放在顶屏 */
    (void)y;

    w_bot_footer(S(S0667));
}

static void save_lang_choice(void)
{
    AppConfig *c = app_cfg();
    const char *code = g_lang_sel ? "en" : "zh";

    uistr_set_lang(g_lang_sel);
    snprintf(c->ui.lang, sizeof c->ui.lang, "%s", code);
    app_cfg_dirty();
    app_cfg_save_now();
    app_status_set(S(S0874), uistr_lang_name(g_lang_sel));
    app_note("welcome: lang=%s", code);
}

static void render_p0_top(void)
{
    float y = (float)W_TOP_CONTENT_Y;
    const float x = W_TOP_MARGIN;
    const float w = W_TOP_USABLE;

    y = w_line(x, y, w, S(S0358), w_c(C_SLOT_ACC_DK));
    y = w_line(x, y + 2, w, S(S0660), w_c(C_SLOT_MUTED));

    y += 8;
    w_hr(x, y, w, w_c(C_SLOT_LINE));
    y += 8;

    y = w_line(x, y, w, S(S0661), w_c(C_SLOT_FG));
    y += 4;
    for (int i = 0; i < CAN_DO_N; ++i)
        y = w_bullet(x, y, w, S(CAN_DO[i]), w_c(C_SLOT_FG), TEXT_LINE_H);

    y += 4;
    w_line(x, y, w, S(S0662), w_c(C_SLOT_MUTED));
}

static void render_p0_bottom(void)
{
    float y = (float)W_BOT_CONTENT_Y;
    const float x = W_BOT_MARGIN;
    const float w = W_BOT_USABLE;

    w_bot_header(S(S0663));

    y = w_line(x, y, w, S(S0664), w_c(C_SLOT_FG));
    y += 2;
    for (int i = 0; i < CANT_DO_N; ++i)
        y = w_bullet(x, y, w, S(CANT_DO[i]), w_c(C_SLOT_FG), TEXT_LINE_H);

    y += 8;
    w_line(x, y, w, S(S0665), w_c(C_SLOT_MUTED));

    w_button(&(Rect){ x + (w - 150.0f) / 2.0f, BOT_BTN_Y, 150, 36, 0 },
             S(S0088), W_BTN_PRIMARY);
    w_center((float)R_BOT_W / 2.0f, BOT_HINT_Y, S(S0666), w_c(C_SLOT_FAINT));

    w_bot_footer(S(S0667));
}

/* --------------------------------------------------------------------------
 * 第 2 页
 * ----------------------------------------------------------------------- */

static void render_p1_top(void)
{
    float y = (float)W_TOP_CONTENT_Y;
    const float x = W_TOP_MARGIN;
    const float w = W_TOP_USABLE;

    y = w_line(x, y, w, S(S0668), w_c(C_SLOT_WARN));
    y += 4;
    w_hr(x, y, w, w_c(C_SLOT_LINE));
    y += 8;

    y = w_wrap(x, y, w,
               S(S0777),
               w_c(C_SLOT_FG), TEXT_LINE_H);

    y += 2;
    w_wrap(x, y, w, S(S0670), w_c(C_SLOT_FG), TEXT_LINE_H);
    y += TEXT_LINE_H;

    y += 6;
    w_hr(x, y, w, w_c(C_SLOT_LINE));
    y += 8;

    /* 步骤用 17px 行高：这一页是三步流程里最长的一屏，
     * 内容区只有 174px，18px 行高会正好压到页脚上。 */
    y = w_line(x, y, w, S(S0671), w_c(C_SLOT_FG));
    y = w_line(x, y, w, S(S0672), w_c(C_SLOT_MUTED));
    y = w_line(x, y, w, S(S0673), w_c(C_SLOT_MUTED));
    w_line(x, y, w, S(S0674), w_c(C_SLOT_MUTED));
}

static void render_p1_bottom(void)
{
    float y = (float)W_BOT_CONTENT_Y;
    const float x = W_BOT_MARGIN;
    const float w = W_BOT_USABLE;

    w_bot_header(S(S0675));

    y = w_wrap(x, y, w,
               S(S0815),
               w_c(C_SLOT_FG), TEXT_LINE_H);

    const float bw = (w - 16.0f) / 2.0f;
    const float by = BOT_BTN_Y;

    w_button(&(Rect){ x, by, bw, 36, 0 }, S(S0677),
             g_choice == 0 ? W_BTN_SELECTED : W_BTN_NORMAL);
    w_button(&(Rect){ x + bw + 16.0f, by, bw, 36, 0 }, S(S0678),
             g_choice == 1 ? W_BTN_SELECTED : W_BTN_NORMAL);

    w_center((float)R_BOT_W / 2.0f, BOT_HINT_Y, S(S0679), w_c(C_SLOT_FAINT));

    w_bot_footer(S(S0680));
}

/* --------------------------------------------------------------------------
 * 第 3 页
 * ----------------------------------------------------------------------- */

static void render_p2_top(void)
{
    float y = (float)W_TOP_CONTENT_Y;
    const float x = W_TOP_MARGIN;
    const float w = W_TOP_USABLE;

    y = w_line(x, y, w, S(S0681), w_c(C_SLOT_ACC_DK));
    y += 4;
    w_hr(x, y, w, w_c(C_SLOT_LINE));
    y += 8;

    y = w_wrap(x, y, w,
               S(S0779),
               w_c(C_SLOT_FG), TEXT_LINE_H);

    /* 服务商摘要面板：把默认配置摊开给用户看一眼 */
    const float px = x;
    const float py = y + 4.0f;
    const float pw = w;
    const int   row_h = 17;                 /* 同第 2 页：这一页也偏长 */
    const float ph = 4 + 3 * row_h;

    w_panel(&(Rect){ px, py, pw, ph, 0 }, w_c(C_SLOT_PANEL), w_c(C_SLOT_LINE));

    const AppConfig *c = app_cfg();
    char l2[64], l3[64];
    snprintf(l2, sizeof l2, "%s", c->ai.model);
    snprintf(l3, sizeof l3, "%s", c->ai.stream ? S(S0349) : S(S0350));

    float ky = py + 4;
    ky = w_kv(px + 10, ky, 80, S(S0065), c->ai.provider, w_c(C_SLOT_MUTED), w_c(C_SLOT_FG), row_h);
    ky = w_kv(px + 10, ky, 80, S(S0443), l2, w_c(C_SLOT_MUTED), w_c(C_SLOT_FG), row_h);
    ky = w_kv(px + 10, ky, 80, S(S0683), l3, w_c(C_SLOT_MUTED), w_c(C_SLOT_FG), row_h);

    y = py + ph + 6.0f;

    y = w_line(x, y, w, S(S0684),
               w_c(C_SLOT_FG));
    w_line(x, y, w, S(S0685), w_c(C_SLOT_MUTED));
}

static void render_p2_bottom(void)
{
    const float x = W_BOT_MARGIN;
    const float w = W_BOT_USABLE;

    w_bot_header(S(S0686));

    float y = (float)W_BOT_CONTENT_Y;
    y = w_wrap(x, y, w,
               S(S0780),
               w_c(C_SLOT_FG), TEXT_LINE_H);

    y += 6;
    y = w_wrap(x, y, w,
               S(S0688),
               w_c(C_SLOT_MUTED), TEXT_LINE_H);

    const float bw = (w - 16.0f) / 2.0f;
    const float by = BOT_BTN_Y;

    /* 这两个按钮**都是真的能按的**。上一版左边那个是灰按钮占位
     * （"界面还没做"），那条路现在通了，就该让它看起来能按 ——
     * 留着灰样子只会让人以为还是没做。
     *
     * ⚠️ 选中态必须用 W_BTN_SELECTED + **W_BTN_NORMAL** 这一对：
     *    W_BTN_PRIMARY 与 SELECTED 的**外观完全一样**（都是主色底白字），
     *    差别只在语义（有没有焦点）。上一版把没选中的那个画成 PRIMARY，
     *    结果两个按钮一模一样，看不出选了谁 —— 这是看截图才发现的，
     *    读代码只会觉得"PRIMARY 更强调主操作，挺合理"。 */
    w_button(&(Rect){ x, by, bw, 36, 0 }, S(S0689),
             g_p2_choice == 0 ? W_BTN_SELECTED : W_BTN_NORMAL);
    w_button(&(Rect){ x + bw + 16.0f, by, bw, 36, 0 }, S(S0690),
             g_p2_choice == 1 ? W_BTN_SELECTED : W_BTN_NORMAL);

    w_center((float)R_BOT_W / 2.0f, BOT_HINT_Y, S(S0679), w_c(C_SLOT_FAINT));

    w_bot_footer(S(S0680));
}

/* --------------------------------------------------------------------------
 * 调度
 * ----------------------------------------------------------------------- */

static void welcome_enter(void)
{
    /* ★ 只在**第一次**进来时复位页号。
     *
     * 原因：这一屏现在会把「录入凭据」推上去（app_push），而
     * **app_pop 会对回来的那一屏重新调 enter()**（见 app.c）。
     * 不加这个守卫的话，用户录完 Key 回来会被踢回第 1 步，
     * 得重新走两页才能到第 3 步 —— 而第 2 页那个"选过没有"的答案
     * 也已经被 apply_nand_choice() 处理过一次了，重走一遍语义上就是错的。
     *
     * 进程内静态量，每个验收趟次都是新进程，所以不会跨趟残留。 */
    if (!g_started) {
        g_page      = 0;
        g_lang_sel  = uistr_lang();   /* 从当前语言起选，不硬编中文 */
        g_choice    = 0;
        g_p2_choice = 1;
        g_started   = true;
        app_status_set(S(S0035));
    } else {
        /* 从子屏（录入凭据）退回来的情况。这一行 note 是**给验收用的**：
         * 它把"页号有没有被复位"变成可从 status.txt 读出来的事实，
         * 否则只能靠比对截图 —— 而截图上"第 3 步 / 共 3 步"的字样
         * 是画出来的像素，脚本读不到。 */
        app_note("welcome: resume at page %d", g_page + 1);
    }
}

static void welcome_render_top(void)
{
    char right[40];
    snprintf(right, sizeof right, S(S0691), g_page + 1, WELCOME_PAGES);
    w_top_header(S(S0692), right);

    switch (g_page) {
        case 0: render_plang_top(); break;
        case 1: render_p0_top(); break;
        case 2: render_p1_top(); break;
        default: render_p2_top(); break;
    }

    w_top_footer(app_status(), S(PAGE_POINT[g_page]));
}

static void welcome_render_bottom(void)
{
    switch (g_page) {
        case 0:  render_plang_bottom(); break;
        case 1:  render_p0_bottom(); break;
        case 2:  render_p1_bottom(); break;
        default: render_p2_bottom(); break;
    }

    /* 步骤圆点放底屏页眉右侧。早先放在顶屏 y=205 —— 那已经是页脚条的地盘，
     * 加回页脚之后两者就会叠在一起。 */
    w_dots((float)R_BOT_W - 30.0f, 8.0f, WELCOME_PAGES, g_page,
           w_c(C_SLOT_BG), R_RGB(0x66, 0xC8, 0xBC));
}

static void apply_nand_choice(void)
{
    AppConfig *c = app_cfg();

    if (g_choice == 0) {
        c->nand_backup.confirmed         = true;
        c->nand_backup.remind_on_startup = false;
        app_status_set(S(S0693));
    } else {
        c->nand_backup.confirmed         = false;
        c->nand_backup.remind_on_startup = true;
        app_status_set(S(S0694));
    }
    app_cfg_dirty();
}

/* 引导的收尾。包一层只是为了**把 g_started 放掉** ——
 * 它是"这一轮引导要不要复位页号"的标记（见 welcome_enter）；
 * 不清掉的话，同一个进程里万一再进引导，会停在上次那一页。 */
static void complete_onboarding(void)
{
    g_started = false;
    app_finish_onboarding();
}

static void welcome_handle(u32 down, u32 held)
{
    (void)held;

    /* 触摸只有按下沿那一帧，先查它，再看按键（照抄 sc_home.c 的次序）。
     * 只有第 3 步有两个可点的按钮，前两页不参与。 */
    if (g_page == 3) {
        int tx, ty;

        if (app_touch_down(&tx, &ty)) {
            const float bw = ((float)W_BOT_USABLE - 16.0f) / 2.0f;
            /* ⚠️ 这两个矩形必须与 render_p2_bottom 里画的那一对**同源**：
             *    各写一份的话，改了版面就会出现"看着能点、点不到"，
             *    而截图完全看不出来（w_hit 的说明里写过这类 bug）。 */
            const Rect  lb = { (float)W_BOT_MARGIN, BOT_BTN_Y, bw, 36, 0 };
            const Rect  rb = { (float)W_BOT_MARGIN + bw + 16.0f, BOT_BTN_Y,
                               bw, 36, 0 };

            if (w_hit(&lb, tx, ty)) {
                g_p2_choice = 0;
                app_note("welcome: touch -> SCREEN_KEYEDIT");
                app_push(SCREEN_KEYEDIT);
                return;
            }
            if (w_hit(&rb, tx, ty)) {
                g_p2_choice = 1;
                complete_onboarding();
                return;
            }
        }
    }

    if (down & KEY_B) {
        if (g_page > 0) {
            --g_page;
            app_status_set(S(S0695), g_page + 1);
        }
        return;
    }

    /* 第 1 步：语言。**立刻生效** —— 切完这一帧的文案就变了，
     * 用户马上看到"这个选择真的起作用"，不用等到进主界面才验证。 */
    if (g_page == 0) {
        if (down & KEY_LEFT)  { g_lang_sel = 0; uistr_set_lang(0); }
        if (down & KEY_RIGHT) { g_lang_sel = 1; uistr_set_lang(1); }
        if (down & (KEY_UP | KEY_DOWN)) {
            g_lang_sel = !g_lang_sel;
            uistr_set_lang(g_lang_sel);
        }
    }

    if (g_page == 2) {
        if (down & KEY_LEFT)  g_choice = 0;
        if (down & KEY_RIGHT) g_choice = 1;
        if (down & (KEY_UP | KEY_DOWN))
            g_choice = !g_choice;
    }

    if (g_page == 3) {
        if (down & KEY_LEFT)  g_p2_choice = 0;
        if (down & KEY_RIGHT) g_p2_choice = 1;
        if (down & (KEY_UP | KEY_DOWN))
            g_p2_choice = !g_p2_choice;
    }

    if (!(down & KEY_A))
        return;

    switch (g_page) {
        case 0:
            /* 语言页：落盘 + 进下一步 */
            save_lang_choice();
            g_page = 1;
            break;

        case 1:
            g_page = 2;
            app_status_set(S(S0696));
            break;

        case 2:
            apply_nand_choice();
            g_page = 3;
            break;

        default:
            /* 第 3 步：左边「录入凭据」推一屏上去，右边「完成设置」收尾。
             * 默认选中右边（见 g_p2_choice 的说明）。 */
            if (g_p2_choice == 0) {
                app_note("welcome: -> SCREEN_KEYEDIT (record credentials)");
                app_push(SCREEN_KEYEDIT);
                return;
            }
            complete_onboarding();
            break;
    }
}

const Screen g_screen_welcome = {
    .id            = SCREEN_WELCOME,
    .name          = "SCREEN_WELCOME",
    .enter         = welcome_enter,
    .leave         = NULL,
    .render_top    = welcome_render_top,
    .render_bottom = welcome_render_bottom,
    .handle        = welcome_handle,
};
