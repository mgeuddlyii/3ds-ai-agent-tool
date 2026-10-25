/*
 * sc_more.c -- 二级菜单（"更多功能"）
 *
 * ── 为什么要有这一屏 ──
 * 主页原来塞了 8 个入口（对话/攻略/服务商/设置/区域/内容更新/传文件/插件），
 * 代价是**说明文字被按钮压住了**：说明占 30..66、配置路径面板 74..110，
 * 而按钮从 114 起才安全 —— 8 个格子塞进 98px 只能靠压行高和挪位置，
 * 结果是面板与第一排按钮重叠 12px（截图能看出来，读代码看不出来）。
 *
 * 2026-10-17 用户拍板：**主页只放核心，其余进二级**。
 * 于是这一屏承接了那四个"偶尔用一次"的入口 —— 它们不是不重要，
 * 而是**不需要天天看见**：设过的区域一周看一眼就够，
 * 内容更新一个月一次，传文件装完游戏就不再碰，插件装好就忘。
 */

#include <3ds.h>
#include <stdio.h>

#include "app.h"
#include "screen.h"
#include "uistr.h"
#include "widgets.h"

#define MORE_N 7       /* 删掉「内容更新」、加进「软件更新」，总数仍是 7 */

typedef struct {
    /* ★ 存**编号的枚举值**，不是字符串 "S0888" ——
     * S() 是宏（展开成 uistr(编号)），参数必须是编译期常量。
     * 存字符串的话 S("S0371") 会把指针当整数传给 uistr，
     * 症状是编译期报 "makes integer from pointer without a cast"。 */
    short    label_id;
    ScreenId target;
    int      translate;    /* 1 = 进对话屏前打开翻译模式 */
} MoreItem;

extern void chat_set_translate(int on);

/* ★ 顺序 = 使用频率从高到低。第一项是「已设区域」——
 *   它是唯一"看完可能要撤销"的，放最上面最顺手。 */
/* ★ 5 项，但底屏只给 80..214 这 134px —— 所以是 24 高 + 2 间隙。
 *   为什么把「服务目录」放第一：它比"已设区域"常用得多，
 *   而且是换服务商/换 Key 的入口（主页原来有这一格，改版时挪进来了）。
 *   ⚠️ 改版时**漏过它一次**：主页删掉后没加进这个表，
 *   结果服务目录整整一轮无人可达 —— 直到跑 0 趟"服务目录守卫"才发现。 */
static const MoreItem kItems[MORE_N] = {
    { S0893, SCREEN_CHAT,      1 },   /* 翻译：对话屏的翻译模式，不是新屏 */
    { S0368, SCREEN_PROVIDERS, 0 },   /* 服务目录 */
    /* ★ 2026-10-22 新增：直接录一把新 API（手输网址/Key/模型，或从 key.txt
     *   导入）。用户要求「API 除了新手教程之外，在更多里也能加进来」。
     *   为什么与服务目录并存而不是合并：服务目录是"这家已经配好了没"
     *   的**清单**，本项是"我要新加一把"的**动作**。合并的话，用户得先
     *   在一堆服务商里找到那个入口 —— 而他此刻根本还不知道要选哪家。
     *   放在服务目录**之后**：先看见现状，再决定要不要加。 */
    { S0918, SCREEN_KEYEDIT,   0 },   /* 添加 API */
    { S0371, SCREEN_LOCALES,   0 },   /* 已设区域 */
    /* ★★ 2026-10-25 用户要求删掉「内容更新」这一项。
     *
     *   理由是他自己踩出来的：菜单里「软件更新」和「内容更新」只差两个字，
     *   而它们其实是**完全不同的两件事** ——
     *     · 软件更新（SCREEN_UPDATE）= 程序自身升级，联网查 manifest；
     *     · 内容更新（SCREEN_CONTENT）= 攻略/资料包的离线安装。
     *   他打开「内容更新」发现样式和「软件更新」不一样，以为改版没生效。
     *   两个入口同叫"更新"本身就是设计缺陷，删掉其中一个比改名更干脆。
     *
     *   ⚠️ **只删菜单项**，SCREEN_CONTENT 这个屏和它的实现全部保留 ——
     *      与 2026-10-23 删「工作台」是同一套做法（屏注册表里还有它，
     *      删屏会引起链接错误）。将来要找回，把这一行加回来即可。
     *
     * ★★ 紧接着把**「软件更新」放进空出来的这一格**。
     *   为什么必须补：删之前菜单里根本没有「软件更新」——它原来只挂在
     *   主页的 **R 键**上（sc_home.c: `if (down & KEY_R) app_push(SCREEN_UPDATE)`）。
     *   于是"删掉旧的那个、留下最新的"会变成"菜单里一个更新入口都没有，
     *   新的那个藏在一个按键后面" —— 与用户的本意正好相反。
     *   现在：菜单里唯一的更新项就是这个改好版面的屏，R 键仍当快捷键。 */
    { S0618, SCREEN_UPDATE,    0 },   /* 软件更新（程序升级，联网） */
    { S0372, SCREEN_XFER,      0 },   /* 从电脑取文件 */
    { S0881, SCREEN_PLUGINS,   0 },   /* 插件（S0881 是「插件」的真实编号，别硬猜） */
    /* ★★ 2026-10-23 用户要求删掉「工作台」这一项：「现在没有任何意义了」。
     *
     *   删它的理由站得住：滑轨（键盘/项目/文件/浏览）**已经常驻在对话屏下屏**，
     *   从「更多」再进一个只画滑轨、没有上文的屏，等于让人多走一步
     *   去看同一个东西。真正要用键盘/项目的人本来就在对话屏里。
     *
     *   注意：**只删菜单项**。SCREEN_DOCK 这个屏本身留着 ——
     *   sc_chat.c 的 dockui_render / dockui_touch 都靠它那套组件，
     *   屏注册表里删掉会引起链接错误。 */

};

static int g_sel;
static int g_top;          /* 首条可见的下标（滚动位置） */

#define MORE_ROW_Y0 76.0f
#define MORE_ROW_H  20.0f
#define MORE_ROW_GAP 1.0f

/* ★ 可见行数由**几何**算出来，不写死 —— 早先 7 项直接照 21px/项 画，
 *   最后一项落在 y=223，被页脚面板（从 214 起）盖掉一半，用户报
 *   "下面被挡住了看不到"。现在按可用高度算能放几行，多的走滚动。
 *   76 起、到 W_BOT_CONTENT_B(214) 止：(214-76+1)/(20+1) = 6 行。 */
#define MORE_VIS_ROWS  6
#define MORE_SCROLL    1     /* 1 = 需要滚动（条目数 > 可见行数时自动成立） */

static Rect row_rect(int i)
{
    Rect r;
    /* i 是**可见序号**（0..MORE_VIS_ROWS-1），不是绝对下标 */
    r.x = (float)W_BOT_MARGIN;
    r.y = MORE_ROW_Y0 + (float)i * (MORE_ROW_H + MORE_ROW_GAP);
    r.w = (float)W_BOT_USABLE;
    r.h = MORE_ROW_H;
    return r;
}

/* 把光标滚进可视窗口。任何改变 g_sel 的地方都必须调一次 ——
 * 否则会出现"选中了但看不见"，比"点不到"更难查。 */
static void clamp_scroll(void)
{
    if (g_sel < g_top)
        g_top = g_sel;
    if (g_sel >= g_top + MORE_VIS_ROWS)
        g_top = g_sel - MORE_VIS_ROWS + 1;
    if (g_top < 0)
        g_top = 0;
    if (g_top > MORE_N - MORE_VIS_ROWS)
        g_top = (MORE_N > MORE_VIS_ROWS) ? MORE_N - MORE_VIS_ROWS : 0;
}

static void more_enter(void)
{
    g_sel = 0;
    g_top = 0;
    app_note("more: enter (%d items, %d visible)", MORE_N, MORE_VIS_ROWS);
}

static void render_top(void)
{
    w_bot_header(S(S0889));
    w_line((float)W_TOP_MARGIN, (float)W_TOP_CONTENT_Y, (float)W_TOP_USABLE,
           S(S0890), w_c(C_SLOT_FG));
    w_top_footer(app_status(), "");
}

static void render_bottom(void)
{
    w_bot_header(S(S0889));

    for (int v = 0; v < MORE_VIS_ROWS; ++v) {
        int i = g_top + v;
        if (i >= MORE_N)
            break;
        Rect r = row_rect(v);
        w_button(&r, S(kItems[i].label_id),
                 (g_sel == i ? W_BTN_SELECTED : W_BTN_NORMAL)
                 | (app_touch_on(&r) ? W_BTN_PRESSED : 0));
    }

    /* 滚动提示：还有几条没显示，就在**页眉右侧**报一下。
     * 放底边会撞最后一行（181..201）和页脚面板（214），
     * 页眉右侧是唯一常年空着的位置。报 当前/总数 而不是只画个 ↓ ——
     * 用户想知道"还剩多少"，箭头说不出来。 */
    if (MORE_N > MORE_VIS_ROWS) {
        char ind[24];
        snprintf(ind, sizeof ind, "%d/%d", g_sel + 1, MORE_N);
        w_line((float)R_BOT_W - 64.0f, 6.0f, 56.0f, ind, w_c(C_SLOT_FAINT));
    }

    w_bot_footer(S(S0891));
}

static void more_handle(u32 down, u32 held)
{
    (void)held;

    if (down & KEY_B) {
        app_pop();
        return;
    }
    if (down & (KEY_UP | KEY_LEFT)) {
        g_sel = (g_sel > 0) ? g_sel - 1 : MORE_N - 1;
        clamp_scroll();
        return;
    }
    if (down & (KEY_DOWN | KEY_RIGHT)) {
        g_sel = (g_sel + 1 < MORE_N) ? g_sel + 1 : 0;
        clamp_scroll();
        return;
    }
    /* ★ L/R 一次翻一整页。列表长了以后逐条挪太慢；这是**额外的**通道，
     *   不占用 ↑↓ 的语义，所以两条路都留着。 */
    if (down & KEY_R) {
        g_sel += MORE_VIS_ROWS;
        if (g_sel >= MORE_N) g_sel = MORE_N - 1;
        clamp_scroll();
        return;
    }
    if (down & KEY_L) {
        g_sel -= MORE_VIS_ROWS;
        if (g_sel < 0) g_sel = 0;
        clamp_scroll();
        return;
    }

    /* 触摸：按下沿那一帧。命中判定与绘制**同源**（都用 row_rect 的
     * 可见序号），所以不会出现"看着能点、点不到"。
     * 点在**行外**（比如底部空白）当成拖动/翻页区：上半屏往上翻、
     * 下半屏往下翻 —— 电阻屏没有真正的拖动手势，这个折中是够用的。 */
    {
        int tx, ty;
        if (app_touch_down(&tx, &ty)) {
            for (int v = 0; v < MORE_VIS_ROWS; ++v) {
                int i = g_top + v;
                if (i >= MORE_N)
                    break;
                Rect r = row_rect(v);
                if (w_hit(&r, tx, ty)) {
                    app_note("more: touch -> item %d", i);
                    chat_set_translate(kItems[i].translate);
                    app_push(kItems[i].target);
                    return;
                }
            }
            /* 空白区：按 y 决定翻页方向 */
            if (MORE_N > MORE_VIS_ROWS) {
                if (ty < MORE_ROW_Y0) {
                    g_sel -= MORE_VIS_ROWS;
                    if (g_sel < 0) g_sel = 0;
                    clamp_scroll();
                } else if (ty > W_BOT_CONTENT_B - 8) {
                    g_sel += MORE_VIS_ROWS;
                    if (g_sel >= MORE_N) g_sel = MORE_N - 1;
                    clamp_scroll();
                }
                return;
            }
        }
    }

    if (down & KEY_A) {
        app_note("more: -> item %d", g_sel);
        /* 每次都要显式设一次：translate 是全局态，上次翻译过、
         * 这次进普通对话，不清就会一直翻译。 */
        chat_set_translate(kItems[g_sel].translate);
        app_push(kItems[g_sel].target);
        return;
    }
}

const Screen g_screen_more = {
    .id            = SCREEN_MORE,
    .name          = "SCREEN_MORE",
    .enter         = more_enter,
    .leave         = NULL,
    .render_top    = render_top,
    .render_bottom = render_bottom,
    .handle        = more_handle,
};