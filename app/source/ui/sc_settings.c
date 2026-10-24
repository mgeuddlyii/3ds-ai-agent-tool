/*
 * sc_settings.c -- 设置屏
 *
 * 双屏分工与 Key 管理一致：上屏列出**事实**（每一项现在是什么），
 * 下屏给选中的那一项**改**（这是唯一能改配置的地方之一）。
 *
 * 为什么做成"一张可滚动的行列表"而不是"一堆开关按钮"：
 * 这些设置有 10 项，全摊在 320x240 上会挤成一团；而且以后只会更多。
 * 列表 + 光标是唯一能一直加下去的版面，也让触摸有明确的目标 ——
 * 点哪一行就选哪一行，选中的行在下屏展开，不需要另外找按钮。
 *
 * 三类设置刻意混在一张表里，但用不同颜色标出类别：
 *   外观（主题/字号/立体深度）  —— 纯视觉，随时可改
 *   提醒（NAND/更新）          —— 行为提醒，随时可改
 *   护栏（确认/上限）          —— 限制程序自己，改它要三思
 * 最后一行永远是"诊断"。把"能改外观"和"能放松安全闸门"放在同一个列表里，
 * 看着一样、后果差很远，所以类别标签必须一直可见。
 */

#include "screen.h"

#include "app.h"
#include "config.h"
#include "widgets.h"
#include "ota.h"   /* APP_VERSION_NAME —— 见下面版本号那段 */
#include "konami.h"
#include "netprobe.h"   /* 网络自检（R_ROW_NETPROBE，2026-10-23 从主页搬来） */

#include <3ds.h>
#include <stdio.h>
#include <string.h>
#include "uistr.h"

/* --------------------------------------------------------------------------
 * 行表
 * ----------------------------------------------------------------------- */

enum {
    R_ROW_THEME = 0,
    R_ROW_ACCENT,       /* 主题色：选中它才展开调色块 */
    R_ROW_FONTSIZE,
    R_ROW_DEPTH,
    R_ROW_SYSPROMPT,
    /* ★ 2026-10-18：「思考强度」这一行**搬到了对话页**（页眉那个
     *   「思考·xx」钮，见 sc_chat.c 的 CHAT_BTN_TLVL）。
     *   理由是它回答的是"**这一次**对话带不带思考"，而设置屏是常驻的
     *   全局面板 —— 用户不会为了"这轮想让它多想一会儿"去翻设置。
     *   档位本身还在 config 里（ai.thinking），录入凭据屏（sc_keyedit）
     *   也仍留着一个入口，值不锁、只是这里不再常驻。
     *
     * ⚠️ 行号纪律：删中间一行，它**后面所有行的编号都前移一位**。
     *   验收脚本（tools/azahar_test.py 的 SCRIPT_1 / SCRIPT_K）是按
     *   位置导航的，改行数必须同步改那两趟，否则会静默验到隔壁行。 */
    /* AI 记忆与三项行为开关紧跟人设 —— 它们都是"这个助手怎么干活"，
     * 放一起用户才不用在整个列表里找。 */
    R_ROW_MEMORY,
    /* ★ 2026-10-22：思考强度**加回设置屏**（用户要求"选项页面可以控制
     *   思考强度"）。它原本在 2026-10-18 被搬到对话页页眉（那里有
     *   「思考·xx」钮），理由是"这一次对话带不带思考"更该就近改。
     *   两处并存是**有意的**：对话页管"临时调一次"，设置屏管
     *   "我的默认习惯是什么"。值同源（都写 cfg->ai.thinking），
     *   不会各说各话。
     * ⚠️ 行号纪律：插在这一位，**后面所有行 +1**。
     *   验收脚本 SCRIPT_1 / SCRIPT_K 是按位置导航的，已同步改过
     *   （VERSION 0-based 20 -> 21，OTA 10 -> 11）。 */
    R_ROW_THINK,
    R_ROW_REPLYLEN,
    R_ROW_CTXTURNS,
    R_ROW_REPLYLANG,
    R_ROW_NAND,
    R_ROW_OTA,
    R_ROW_G_OVERWRITE,
    R_ROW_G_DELETE,
    R_ROW_G_BULK,
    R_ROW_G_WRITEKB,
    R_ROW_G_BATCH,
    R_ROW_SECLEVEL,      /* 安全等级：L0 对话 … L5 最高 */
    R_ROW_D_OVERFLOW,
    R_ROW_D_NOTES,
    /* ★ 2026-10-23：网络自检从**主页 SELECT** 搬到这里。
     *   两个理由（主页那边留了完整说明）：① 它阻塞，主页不该卡住；
     *   ② 它是排障工具不是功能，正常用户按了只会看到看不懂的状态码。
     *   放在诊断分类里语义正确：这一组"出问题时才来看"。
     * ⚠️ 行号纪律：插在这一位，V_STATUS / VERSION / LANG **各 +1**。
     *   验收脚本 SCRIPT_1 的 DOWN 21 -> 22、断言 cursor_row 22 -> 23；
     *   SCRIPT_K 的 OTA 在这一行**之前**，不受影响。 */
    R_ROW_NETPROBE,
    R_ROW_V_STATUS,
    R_ROW_VERSION,       /* 版本号：连点六下进隐藏设置 */
    /* ★ 语言放**最后一行**，不是紧挨主题。理由：设置列表的行号被
     * 验收脚本按位置导航（"15 次 DOWN 到版本号"），插在中间会让
     * 既有判据全部点偏 —— 而那种失败看起来像"功能坏了"，
     * 真因只是多了一行。放末尾则对所有既有行号零影响。
     * 代价是它不在最上面，但设置列表本来就要滚。 */
    R_ROW_LANG,
    R_ROW_COUNT,
};

/* 类别。决定行左边那条色带。</br>
 * 顺序即"从无害到危险"：外观 < 提醒 < 护栏 < 诊断（诊断只读显示，不危险）。 */
enum { RC_UI = 0, RC_AI, RC_REMIND, RC_GUARD, RC_DIAG };

typedef struct {
    /* ★ 存**编号**而不是字符串：静态初始化里必须是编译期常量，
     * 而 S() 是函数调用（编译器报 "initializer element is not constant"，
     * 却指着 uistr.h 的宏定义，真因在几百行外）。
     * 取值统一走 row_label()。 */
    short       label_id;
    int         category;
} RowDef;

static const RowDef ROWS[R_ROW_COUNT] = {
    [R_ROW_THEME]      = { S0216,     RC_UI     },
    [R_ROW_ACCENT]     = { S0913,     RC_UI     },
    [R_ROW_FONTSIZE]   = { S0532,         RC_UI     },
    [R_ROW_DEPTH]      = { S0218,     RC_UI     },
    [R_ROW_SYSPROMPT]  = { S0533,     RC_AI     },
    [R_ROW_MEMORY]     = { S0857,     RC_AI     },
    /* ★ 行标签用已有的 S0382「思考强度」（引导/凭据屏早就用它），
     *   不另起一条 —— 同一个概念在三个地方各有一条文案，翻译迟早会岔。 */
    [R_ROW_THINK]      = { S0382,     RC_AI     },
    [R_ROW_REPLYLEN]   = { S0858,   RC_AI     },
    [R_ROW_CTXTURNS]   = { S0859, RC_AI     },
    [R_ROW_REPLYLANG]  = { S0860,  RC_AI     },
    [R_ROW_NAND]       = { S0534, RC_REMIND },
    [R_ROW_OTA]        = { S0535, RC_REMIND },
    [R_ROW_G_OVERWRITE]= { S0536,   RC_GUARD  },
    [R_ROW_G_DELETE]   = { S0537,   RC_GUARD  },
    [R_ROW_G_BULK]     = { S0538,   RC_GUARD  },
    [R_ROW_G_WRITEKB]  = { S0539, RC_GUARD  },
    [R_ROW_G_BATCH]    = { S0540, RC_GUARD  },
    [R_ROW_SECLEVEL]   = { S0215,     RC_GUARD  },
    [R_ROW_D_OVERFLOW] = { S0541,     RC_DIAG   },
    [R_ROW_D_NOTES]    = { S0542,   RC_DIAG   },
    [R_ROW_NETPROBE]   = { S0919,   RC_DIAG   },   /* 网络自检（从主页 SELECT 搬来） */
    [R_ROW_V_STATUS]   = { S0543, RC_DIAG   },
    /* 版本号做成一行而不是挂在页眉右上角。
     * 2026-10-10 真机反馈：「版本号挂在那个顶上又没法按」。
     * 两个原因：
     *   1. 设置列表画在**上屏**，而 3DS 只有**下屏能触摸** ——
     *      上屏上的任何东西都点不到，只能靠十字键选中；
     *   2. 它当时实际还与页眉右侧的"7/15"进度**重叠**，
     *      旁边那句提示又压在标题「设置」上。
     * 做成一行之后：十字键选得到、A 键按得动，也不再和别人抢位置。
     * 连点六下的机关不变（konami 吃的是全局按键，选在这一行按 A 自然算）。 */
    [R_ROW_VERSION]    = { S0544,       RC_DIAG   },
    [R_ROW_LANG]       = { S0818,     RC_UI     },
};

/* 取第 i 行的标签。取字符串这件事收在这一个函数里，
 * 所以"忘了包一层 S()"这个漏法只可能出现在这里。 */
static const char *row_label(int i)
{
    if (i < 0 || i >= R_ROW_COUNT || ROWS[i].label_id == 0)
        return "";
    return S(ROWS[i].label_id);
}

static int g_cursor;

/* 每行的可见区域（前 7 行）与滚动窗口。
 * 上屏内容区能放 7 行 × 22px = 154px，从 y=30 起，到 184 止。 */
#define ROW_H       22
#define ROW_Y0      32
#define ROW_VISIBLE 7

static int g_top_row;   /* 滚动窗口的第一行 */

/* 降低安全等级的"待确认"目标档（0 = 没在等待确认）。
 * 第一次按左键只是上膛并提示，再按一次才真改。 */
static int g_seclevel_armed;

/* 网络自检的结果缓存（R_ROW_NETPROBE 的值列）。
 *
 * 为什么要缓存而不是每次进屏现探一遍：探测是**阻塞**的（httpc 同步），
 * 进屏就探会让"进设置"这个动作卡几秒 —— 那正是它被从主页搬过来的原因，
 * 不能在这里重新犯一遍。所以只在用户**主动按 A** 时探，
 * 结果留在这里给值列显示，跨进屏有效（同一次运行内）。
 * 空串 = 这次运行还没探过。 */
static char g_netprobe_text[64];

static void clamp_scroll(void)
{
    if (g_cursor < 0) g_cursor = 0;
    if (g_cursor >= R_ROW_COUNT) g_cursor = R_ROW_COUNT - 1;

    if (g_cursor < g_top_row)
        g_top_row = g_cursor;
    if (g_cursor >= g_top_row + ROW_VISIBLE)
        g_top_row = g_cursor - ROW_VISIBLE + 1;
    if (g_top_row > R_ROW_COUNT - ROW_VISIBLE)
        g_top_row = R_ROW_COUNT - ROW_VISIBLE;
    if (g_top_row < 0)
        g_top_row = 0;
}

static const char *cat_name(int c)
{
    switch (c) {
        case RC_UI:     return S(S0545);
        case RC_AI:     return S(S0546);
        case RC_REMIND: return S(S0547);
        case RC_GUARD:  return S(S0548);
        default:        return S(S0549);
    }
}

static RColor cat_color(int c)
{
    switch (c) {
        case RC_UI:     return w_c(C_SLOT_ACCENT);
        case RC_AI:     return w_c(C_SLOT_ACC_DK);
        case RC_REMIND: return w_c(C_SLOT_OK);
        case RC_GUARD:  return w_c(C_SLOT_WARN_DK);
        default:        return w_c(C_SLOT_FAINT);
    }
}

/* 每行的当前值，写成一句人话。val 由调用方给的缓冲写入。 */
static void row_value(int row, char *out, size_t n)
{
    AppConfig *c = app_cfg();

    switch (row) {
        case R_ROW_THEME:
            snprintf(out, n, "%s", w_theme_name(
                !strcmp(c->ui.theme, "dark") ? W_THEME_DARK : W_THEME_LIGHT));
            break;
        case R_ROW_ACCENT:
            /* 值显示颜色名（teal/blue/…）。不翻译：这是**颜色标识**，
             * 不是界面文案 —— 用户报"我用 blue"时双方说的是同一个词。 */
            snprintf(out, n, "%s", w_accent_name(w_accent_current()));
            break;
        case R_ROW_LANG:
            snprintf(out, n, "%s", uistr_lang_name(uistr_lang()));
            break;
        case R_ROW_FONTSIZE:
            snprintf(out, n, "%s", c->ui.font_size);
            break;
        case R_ROW_DEPTH:
            snprintf(out, n, "%d%%", (int)(c->ui.depth * 100.0f + 0.5f));
            break;
        case R_ROW_SYSPROMPT:
            /* 不把全文摊在这里：240 字节的人设会把这一行撑爆。
             * 报个字数和"设没设"就够 —— 想看全文按 A 就进去了。 */
            /* ★ 译文当格式串用有坑：译文里出现字面 %（"20%"）会被当转换符。
             * 所以凡是 S(...) 里有 %d/%s 之外的百分号、或只是想拼接，
             * 一律写成 snprintf(out, n, "%s" S(...), ...) 的形式不对 ——
             * 正确做法：把 S() 放最后当**纯文本**时用 "%s" 单独占位。 */
            if (c->ai.system_prompt[0])
                snprintf(out, n, S(S0550),
                         (int)strlen(c->ai.system_prompt));
            else
                snprintf(out, n, "%s", S(S0551));
            break;
        case R_ROW_MEMORY:
            /* 只报字数，不报全文：200 字会把这一行撑爆，而且它本来就
             * 是"有没有设过"的信息，全文按 A 进去看。 */
            if (c->ai.memory[0])
                snprintf(out, n, "%d", (int)strlen(c->ai.memory));
            else
                snprintf(out, n, "%s", S(S0551));
            break;
        case R_ROW_REPLYLEN:
            snprintf(out, n, "%s",
                     c->ai.reply_len == CONFIG_REPLY_SHORT ? S(S0861)
                   : c->ai.reply_len == CONFIG_REPLY_LONG  ? S(S0862) : S(S0826));
            break;
        /* ★ 显示"思考·中"这种形式（S0900），与对话页页眉那个钮同一个文案 ——
         *   用户在两处看到同一个字符串，就知道改的是同一件事。 */
        case R_ROW_THINK:
            snprintf(out, n, S(S0900), thinking_name(c->ai.thinking));
            break;
        case R_ROW_CTXTURNS:
            snprintf(out, n, S(S0864), c->ai.context_turns);
            break;
        case R_ROW_REPLYLANG:
            /* 语言名用**它自己的写法**（中文/English），不随界面翻译 ——
             * 界面上"Chinese"和"中文"是同一件事，让用户认得出来更重要。 */
            /* ★ 2026-10-23：这里原本是 S(S0001)，而 S0001 的真实内容是
             *   「这个键还没有对应实现（内部问题）」—— 选"中文"时值列显示的
             *   就是那行错误提示。已改为 S0947（中文）。 */
            if (c->ai.reply_lang == 1)      snprintf(out, n, "%s", S(S0946));
            else if (c->ai.reply_lang == 2) snprintf(out, n, "English");
            else                            snprintf(out, n, "%s", S(S0863));
            break;
        case R_ROW_NAND:
            if (c->nand_backup.confirmed)
                snprintf(out, n, "%s", S(S0552));
            else
                snprintf(out, n, c->nand_backup.remind_on_startup ? S(S0553)
                                                                 : S(S0554));
            break;
        case R_ROW_OTA:
            snprintf(out, n, S(S0555),
                     c->ota.enabled && c->ota.auto_check ? S(S0349) : S(S0350));
            break;
        case R_ROW_G_OVERWRITE:
            /* 这一项显示成"锁定"而不是"开"，让用户一眼看出它不是普通开关 */
            snprintf(out, n, "%s", S(S0556));
            break;
        case R_ROW_G_DELETE:
            snprintf(out, n, "%s", c->guard.confirm_delete ? S(S0349) : S(S0350));
            break;
        case R_ROW_G_BULK:
            snprintf(out, n, "%s", c->guard.confirm_bulk ? S(S0349) : S(S0350));
            break;
        case R_ROW_G_WRITEKB:
            if (c->guard.max_write_kb <= 0) snprintf(out, n, "%s", S(S0557));
            else snprintf(out, n, "%d KB", c->guard.max_write_kb);
            break;
        case R_ROW_G_BATCH:
            if (c->guard.max_batch_files <= 0) snprintf(out, n, "%s", S(S0557));
            else snprintf(out, n, S(S0558), c->guard.max_batch_files);
            break;
        case R_ROW_SECLEVEL:
            /* 安全等级原来**根本没有 UI 入口**（config 里有这一项，
             * 设置屏里却没有对应的行）—— 结果是永远停在 L0 对话，
             * 用户没法用上文件/系统那几级能力，又完全看不出来为什么。
             * 补上这一行之后，每个等级会做什么写在下面的说明里。 */
            snprintf(out, n, "%s", security_level_name(c->security.current_level));
            break;
        case R_ROW_D_OVERFLOW:
            snprintf(out, n, "%s", c->diag.show_layout_overflow ? S(S0349) : S(S0350));
            break;
        case R_ROW_D_NOTES:
            snprintf(out, n, "%s", c->diag.show_notes ? S(S0349) : S(S0350));
            break;
        case R_ROW_NETPROBE:
            /* 值列显示**上次探测的结果**，没探测过就写"按 A 执行"。
             * 不显示实时进度：探测是阻塞的，界面上能更新的机会只有
             * 探测前后各一次 —— 中间那段本来就没帧可画。
             * 用 g_netprobe_text 缓存，跨进屏保留（同一趟运行内有效）。 */
            snprintf(out, n, "%s", g_netprobe_text[0] ? g_netprobe_text
                                                      : S(S0920));
            break;
        case R_ROW_VERSION:
            /* ★ 必须是 **APP_VERSION_NAME**（"0.99"），不是 app_cfg()->version。
             *   后者是**配置文件结构的版本**（config.h 的 CONFIG_VERSION，
             *   当前是 1），与程序版本毫无关系 —— 早先画的是 "v1"，
             *   用户看到"版本 1"，与桌面上的 0.99 对不上，
             *   于是"这是不是最新版"这个判断从一开始就没有依据。
             *   **两个"版本"同名不同义，是这一处最容易骗过人的地方。** */
            /* 版本号是**构建常量**，不进文案表（translate 只会把它弄坏：
             * APP_VERSION_NAME 是宏，展开后与 S() 拼在一起编译器不接受；
             * 而且版本号翻成英文对用户没有意义）。
             * 后面那句提示才是文案，走 S()。 */
            snprintf(out, n, "v" APP_VERSION_NAME "  (tap A here six times)");
            break;
        case R_ROW_V_STATUS:
        default:
            snprintf(out, n, "%s", c->diag.verbose_status ? S(S0349) : S(S0350));
            break;
    }
}

/* --------------------------------------------------------------------------
 * 生成期动作
 * ----------------------------------------------------------------------- */

/* 每次进屏把主题与真配置同步一次 —— 用户可能直接改过 config.json，
 * 而主题是"界面层的当前值"，两者不一致时以文件为准（文件是持久的那个）。 */
static void apply_theme_from_cfg(void)
{
    const AppConfig *c = app_cfg();
    w_theme(!strcmp(c->ui.theme, "dark") ? W_THEME_DARK : W_THEME_LIGHT);
}

static void settings_enter(void)
{
    clamp_scroll();
    apply_theme_from_cfg();
    /* 重新进屏时清掉"待确认降级"：上一次留下的上膛状态不该跨屏生效 */
    g_seclevel_armed = 0;
    konami_reset();      /* 连点计数与密码进度不留到下一次进来 */
    app_status_set(S(S0560),
                   w_theme_name(w_theme_current()));
}

/* 切界面语言。照 cycle_theme() 的形状：**立刻落盘**，否则用户退出再进来
 * 又回到中文，会以为这一项没生效（与"思考强度"同一个理由）。
 * 落盘前先写回 cfg，app_shutdown 还会再存一次，两处都无害。 */
static void cycle_reply_len(void)
{
    AppConfig *c = app_cfg();
    c->ai.reply_len = (c->ai.reply_len + 1) % 3;
    app_cfg_dirty();
    app_cfg_save_now();
    app_status_set(S(S0561),
                   c->ai.reply_len == CONFIG_REPLY_SHORT ? S(S0861)
                 : c->ai.reply_len == CONFIG_REPLY_LONG  ? S(S0862) : S(S0826));
    app_note("settings: reply_len=%d", c->ai.reply_len);
}

/* 思考强度 0..3 循环（关/低/中/高）。
 * 与对话页 cycle_think_level() **同源**：都改 cfg->ai.thinking，都立刻落盘。
 * 刻意不共用函数 —— 对话页那份在 sc_chat.c 里，是"这一轮临时调"；
 * 这份在设置屏是"我的默认习惯"。两者改的是同一个字段，值必须一致，
 * 但不该互相 #include（界面屏之间不交叉依赖，这是本项目的分层规矩）。 */
static void cycle_think_setting(void)
{
    AppConfig *c = app_cfg();
    int n = CONFIG_THINK_HIGH + 1;      /* 档位总数，别写死 4 */

    c->ai.thinking = (c->ai.thinking + 1) % n;
    app_cfg_dirty();
    app_cfg_save_now();
    app_status_set(S(S0900), thinking_name(c->ai.thinking));
    app_note("settings: thinking=%d", c->ai.thinking);
}

/* 网络自检。从主页 SELECT 搬来（2026-10-23）。
 *
 * 探测的是"能不能出去"，不是"某家通不通" —— 后者是 config 里
 * 那家服务商的事，而这里要回答的是"网络这一层通不通"。
 *
 * ⚠️ 它会**阻塞**（httpc 是同步的）。放在用户主动按 A 的位置上，
 *   卡一下有心理预期；绝不能挂到进屏或每帧上。 */
static void do_netprobe(void)
{
    int  code = 0;
    char detail[96];

    bool ok = net_probe(NULL, &code, detail, sizeof detail);

    /* 结果同时进值列、状态行、status.txt 三处：
     *   值列  = 下次回来还能看见（缓存）
     *   状态行= 当场可见
     *   备注  = 验收/排障可回读 */
    snprintf(g_netprobe_text, sizeof g_netprobe_text, "%s",
             ok ? S(S0376) : S(S0377));
    app_status_set("%s · %s", ok ? S(S0376) : S(S0377), detail);
    app_note("netprobe=ok=%d code=%d %s", (int)ok, code, detail);
}

/* 滚一整屏。给 L/R 用 —— 23 行只靠方向键逐行挪太慢
 * （要到最后一行的「界面语言」得按 22 下）。
 * 光标走到哪，窗口就跟到哪，与 clamp_scroll 是同一条规矩。 */
static void page_step(int dir)
{
    int step = ROW_VISIBLE;

    g_cursor += (dir > 0 ? step : -step);
    if (g_cursor < 0) g_cursor = 0;
    if (g_cursor >= R_ROW_COUNT) g_cursor = R_ROW_COUNT - 1;
    clamp_scroll();
    g_seclevel_armed = 0;      /* 翻页也算"动了别的行"，取消降级上膛 */
    /* 打点给验收用：光标的**绝对**位置（1-based）与可见窗口首行。
     * 没有这条的话，"按 R 到底翻没翻"只能靠截图看高亮在哪一行 ——
     * 而截图需要人眼比对，脚本判不了。 */
    app_note("settings: page_step dir=%d cursor=%d top=%d",
             dir, g_cursor + 1, g_top_row + 1);
}

/* 轮数按 4/8/12 循环。不做 +1 递增：用户要的是"少带点/多带点"，
 * 4 和 5 轮的实际差别看不出，徒增按键次数。 */
static void cycle_ctx_turns(void)
{
    AppConfig *c = app_cfg();
    c->ai.context_turns = (c->ai.context_turns <= 4) ? 8
                        : (c->ai.context_turns <= 8) ? 12 : 4;
    app_cfg_dirty();
    app_cfg_save_now();
    app_note("settings: context_turns=%d", c->ai.context_turns);
}

static void cycle_reply_lang(void)
{
    AppConfig *c = app_cfg();
    c->ai.reply_lang = (c->ai.reply_lang + 1) % 3;
    app_cfg_dirty();
    app_cfg_save_now();
    app_note("settings: reply_lang=%d", c->ai.reply_lang);
}

static void cycle_lang(void)
{
    AppConfig *c = app_cfg();
    int next = (uistr_lang() == 1) ? 0 : 1;

    uistr_set_lang(next);
    snprintf(c->ui.lang, sizeof c->ui.lang, "%s", next ? "en" : "zh");
    app_cfg_dirty();
    app_cfg_save_now();
    /* 状态行本身此刻还是**旧语言**（这一帧的文案已经取完了），
     * 所以这里不塞 uistr 的句子，只写语言名 —— 两边都能看懂。 */
    app_status_set("%s", uistr_lang_name(next));
    app_note("settings: lang=%s", c->ui.lang);
}

/* AI 记忆：照 edit_prompt() 的形状写。**不复用同一个缓冲** ——
 * 记忆比人设短（200 vs 240 字节），共用一把尺子会让人敲出越界的长度。 */
static void edit_memory(void)
{
    AppConfig *c = app_cfg();
    SwkbdState swkbd;
    char buf[CONFIG_MAX_MEMORY + 1];
    SwkbdButton btn;

    /* swkbd 的长度参数是**字符数**，字段上限是**字节数**：
     * 60 个汉字 = 180 字节，稳在 200 以内；传 200 会让人敲出 600 字节。 */
    swkbdInit(&swkbd, SWKBD_TYPE_WESTERN, 2, 60);
    swkbdSetHintText(&swkbd, S(S0857));
    swkbdSetInitialText(&swkbd, c->ai.memory);

    btn = swkbdInputText(&swkbd, buf, sizeof buf);
    if (btn != SWKBD_BUTTON_CONFIRM) {
        app_status_set(S(S0134));
        return;
    }

    buf[CONFIG_MAX_MEMORY] = '\0';
    memcpy(c->ai.memory, buf, sizeof c->ai.memory);
    c->ai.memory[sizeof c->ai.memory - 1] = '\0';

    app_cfg_dirty();
    app_cfg_save_now();

    if (c->ai.memory[0])
        app_status_set(S(S0865), (int)strlen(c->ai.memory));
    else
        app_status_set(S(S0866));
    app_note("settings: memory=%d bytes", (int)strlen(c->ai.memory));
}

static void cycle_theme(void)
{
    AppConfig *c = app_cfg();
    int next = (w_theme_current() == W_THEME_DARK) ? W_THEME_LIGHT : W_THEME_DARK;

    w_theme(next);
    snprintf(c->ui.theme, sizeof c->ui.theme, "%s",
             next == W_THEME_DARK ? "dark" : "light");
    app_cfg_dirty();
    app_cfg_save_now();
    app_status_set(S(S0561), w_theme_name(next));
    app_note("settings: theme=%s", c->ui.theme);
}
/* 主题色循环。★ 只有**选中这一行**时才显示色块（见 render_bottom），
 * 但左右键在这个行上也要能改 —— 不然纯按键用户进不来。 */
static void cycle_accent(int delta)
{
    AppConfig *c = app_cfg();
    int n = w_accent_count();
    int i = w_accent_current() + (delta > 0 ? 1 : -1);

    if (i < 0)      i = n - 1;
    if (i >= n)     i = 0;

    w_accent_set(i);
    if (c) {
        snprintf(c->ui.accent, sizeof c->ui.accent, "%s", w_accent_name(i));
        if (!config_save(c))
            app_status_set("已换色，但写配置失败（重启会还原）");
        else
            app_status_set("主题色：%s", w_accent_name(i));
    }
}


static void cycle_fontsize(void)
{
    AppConfig *c = app_cfg();
    /* 字库目前只有一档字号（16x16 点阵是烘焙死的），所以这一项
     * 改了只是**记下偏好**，不重建字库。界面上要如实说明，
     * 否则用户会以为改了没生效。 */
    const char *order[] = { "small", "mid", "large" };
    int idx = 1;
    for (int i = 0; i < 3; ++i)
        if (!strcmp(c->ui.font_size, order[i])) { idx = i; break; }
    idx = (idx + 1) % 3;

    snprintf(c->ui.font_size, sizeof c->ui.font_size, "%s", order[idx]);
    app_cfg_dirty();
    app_cfg_save_now();
    app_status_set(S(S0562), order[idx]);
}

static void bump_depth(int delta)
{
    AppConfig *c = app_cfg();
    int pct = (int)(c->ui.depth * 100.0f + 0.5f) + delta;

    if (pct < 0)   pct = 0;
    if (pct > 100) pct = 100;
    c->ui.depth = (float)pct / 100.0f;
    app_cfg_dirty();
    app_cfg_save_now();
    app_status_set(S(S0563), pct);
}

static void toggle_nand(void)
{
    AppConfig *c = app_cfg();

    if (c->nand_backup.confirmed) {
        app_status_set(S(S0564));
        return;
    }
    c->nand_backup.remind_on_startup = !c->nand_backup.remind_on_startup;
    app_cfg_dirty();
    app_cfg_save_now();
    app_status_set(S(S0565), c->nand_backup.remind_on_startup ? S(S0349) : S(S0350));
}

static void toggle_ota(void)
{
    AppConfig *c = app_cfg();
    bool on = c->ota.enabled && c->ota.auto_check;
    c->ota.enabled    = !on;
    c->ota.auto_check = !on;
    app_cfg_dirty();
    app_cfg_save_now();
    app_status_set(S(S0566), (!on) ? S(S0349) : S(S0350));
}

static void toggle_guard_delete(void)
{
    AppConfig *c = app_cfg();
    c->guard.confirm_delete = !c->guard.confirm_delete;
    app_cfg_dirty();
    app_cfg_save_now();
    app_status_set(c->guard.confirm_delete
                   ? S(S0567)
                   : S(S0568));
}

static void toggle_guard_bulk(void)
{
    AppConfig *c = app_cfg();
    c->guard.confirm_bulk = !c->guard.confirm_bulk;
    app_cfg_dirty();
    app_cfg_save_now();
    app_status_set(S(S0569), c->guard.confirm_bulk ? S(S0349) : S(S0350));
}

static void bump_writekb(int delta)
{
    AppConfig *c = app_cfg();
    int v = c->guard.max_write_kb;

    if (v <= 0) v = (delta > 0) ? 512 : 512;    /* 从"不限"回到默认再调 */
    else        v += delta;

    if (v < 64)   v = 0;        /* 走到下限就变"不限" */
    if (v > 4096) v = 4096;
    c->guard.max_write_kb = v;
    app_cfg_dirty();
    app_cfg_save_now();
    if (v == 0) app_status_set(S(S0570));
    else        app_status_set(S(S0571), v);
}

static void bump_batch(int delta)
{
    AppConfig *c = app_cfg();
    int v = c->guard.max_batch_files;

    if (v <= 0) v = (delta > 0) ? 20 : 20;
    else        v += delta;

    if (v < 1)    v = 0;
    if (v > 200)  v = 200;
    c->guard.max_batch_files = v;
    app_cfg_dirty();
    app_cfg_save_now();
    if (v == 0) app_status_set(S(S0572));
    else        app_status_set(S(S0573), v);
}

static void toggle_diag(int row)
{
    AppConfig *c = app_cfg();

    switch (row) {
        case R_ROW_D_OVERFLOW:
            c->diag.show_layout_overflow = !c->diag.show_layout_overflow;
            app_status_set(S(S0574), c->diag.show_layout_overflow ? S(S0349) : S(S0350));
            break;
        case R_ROW_D_NOTES:
            c->diag.show_notes = !c->diag.show_notes;
            app_status_set(S(S0575), c->diag.show_notes ? S(S0349) : S(S0350));
            break;
        /* ★ 2026-10-23：把原来的 `default:` 改成**显式** case。
         *   原来这里写 default，意思是"剩下那个就是 V_STATUS" ——
         *   而这条假设在**加行**的那一刻就悄悄失效了：新加的行会掉进来，
         *   变成"按 A 结果切了别的开关"，界面上完全看不出来。
         *   现在只认 V_STATUS，加新诊断行时编译器不会报错但**行为是对的**
         *   （不响应，而不是错误响应）。 */
        case R_ROW_V_STATUS:
            c->diag.verbose_status = !c->diag.verbose_status;
            app_status_set(S(S0576), c->diag.verbose_status ? S(S0349) : S(S0350));
            break;
        default:
            /* 非开关类诊断行（如 NETPROBE）意外走到这里时**什么都不做** ——
             * 静默不响应比"悄悄改了别的设置"安全得多。 */
            return;
    }
    app_cfg_dirty();
    app_cfg_save_now();
}

/* 编辑助手人设。
 *
 * 文本项不能沿用"左右键切下一档"那一套 —— 那不是枚举，是自由输入，
 * 所以单独开一个入口（按 A 或点下屏那两个按钮）。 */
static void edit_prompt(void)
{
    AppConfig *c = app_cfg();
    SwkbdState swkbd;
    char buf[CONFIG_MAX_PROMPT + 1];
    SwkbdButton btn;

    /* 和快捷短语同理：swkbd 的长度参数是**字符数**，而字段上限是**字节数**。
     * 30 个汉字 = 90 字节，稳在 240 以内；传 CONFIG_MAX_PROMPT 会让人
     * 敲出 240 个汉字（720 字节）撑爆缓冲。 */
    swkbdInit(&swkbd, SWKBD_TYPE_WESTERN, 2, 30);
    swkbdSetHintText(&swkbd, S(S0577));
    swkbdSetInitialText(&swkbd, c->ai.system_prompt);

    btn = swkbdInputText(&swkbd, buf, sizeof buf);

    if (btn != SWKBD_BUTTON_CONFIRM) {
        app_status_set(S(S0134));
        return;
    }

    buf[CONFIG_MAX_PROMPT] = '\0';

    /* 用 memcpy 而不是 snprintf：buf 和字段一样长，snprintf 会因"源可能
     * 有 240 字符 + 结尾"报截断警告。这里要的正是"照搬、不越界"，
     * 拷完之后自己补终止符。 */
    memcpy(c->ai.system_prompt, buf, sizeof c->ai.system_prompt);
    c->ai.system_prompt[sizeof c->ai.system_prompt - 1] = '\0';

    app_cfg_dirty();
    app_cfg_save_now();

    /* 两个分支分开写：format 字符串若是变量，编译器的截断检查就失效了
     * （而且 %d 配到那句没有占位符的文案上会打印出垃圾）。 */
    if (c->ai.system_prompt[0])
        app_status_set(S(S0578),
                       (int)strlen(c->ai.system_prompt));
    else
        app_status_set(S(S0579));
    app_note("settings: system_prompt=%d bytes",
             (int)strlen(c->ai.system_prompt));
}

/* 左：减/上一个；右：加/下一个。触摸点按钮，键盘用左右方向键。
 * 人设那一行是例外 —— 见上面 edit_prompt。 */
static void row_step(int row, int delta)
{
    /* 动了别的行就取消"降级待确认"的上膛状态 ——
     * 否则用户按左键想调别的东西，却把上一行的降级确认给触发了。 */
    if (row != R_ROW_SECLEVEL)
        g_seclevel_armed = 0;

    switch (row) {
        case R_ROW_THEME:     if (delta) cycle_theme(); break;
        case R_ROW_ACCENT:    if (delta) cycle_accent(delta); break;
        case R_ROW_LANG:      if (delta) cycle_lang();  break;
        case R_ROW_FONTSIZE:  if (delta) cycle_fontsize(); break;
        case R_ROW_DEPTH:     bump_depth(delta * 10); break;
        case R_ROW_SYSPROMPT:
            /* 文本项，没有"下一档"可切。给个明确提示而不是静默 ——
             * 静默会让用户以为程序坏了（和护栏那项同理）。 */
            if (delta)
                app_status_set(S(S0580));
            break;
        case R_ROW_MEMORY:    if (delta) edit_memory();      break;
        case R_ROW_THINK:     if (delta) cycle_think_setting(); break;
        case R_ROW_REPLYLEN:  if (delta) cycle_reply_len();  break;
        case R_ROW_CTXTURNS:  if (delta) cycle_ctx_turns();  break;
        case R_ROW_REPLYLANG: if (delta) cycle_reply_lang(); break;
        case R_ROW_NAND:      if (delta) toggle_nand(); break;
        case R_ROW_OTA:       if (delta) toggle_ota(); break;
        case R_ROW_G_OVERWRITE:
            /* 刻意什么都不做：这一项就是不可改，按下要给明确反馈，
             * 而不是静默无响应（静默会让用户以为程序坏了）。 */
            app_status_set(S(S0582));
            break;
        case R_ROW_G_DELETE:  if (delta) toggle_guard_delete(); break;
        case R_ROW_G_BULK:    if (delta) toggle_guard_bulk(); break;
        case R_ROW_G_WRITEKB: bump_writekb(delta * 128); break;
        case R_ROW_G_BATCH:   bump_batch(delta * 5); break;
        case R_ROW_SECLEVEL: {
            /* 提升直接生效；**降低要二次确认** ——
             * 它等于主动放宽"AI 能动到哪一层"，误触左键就放行太危险。
             * 用内联二次确认（上膛 → 再按一次才真改），**不走 SCREEN_CONFIRM**：
             * 那一屏是给"AI 想做什么"用的，用户自己的设置留在原地
             * （理由见 sc_locales.c 顶部注释）。 */
            AppConfig *c = app_cfg();
            int want = c->security.current_level + delta;

            if (want < SEC_LEVEL_CHAT || want > SEC_LEVEL_MAX) {
                g_seclevel_armed = 0;
                app_status_set(S(S0583),
                               security_level_name(c->security.current_level));
                break;
            }

            /* ★ 往上调（L3 以上）必须先过密码门。
             *
             * 理由：sec.level 自己也有确认屏，看起来够安全。但那一屏上写着
             * "调高会放宽限制" —— 而用户**看不出 L3 和 L5 差在哪**。
             * 于是"AI 建议提升权限"就成了一句看起来无害的建议。
             * 把提升锁进密码后面，才真正切断这条链。
             *
             * 降级（delta < 0）**不**要密码：把权限交回出去是安全方向，
             * 让人为了"降回去"先去输密码只会逼着用户干脆别降。 */
            if (delta > 0 && want > SEC_LEVEL_LOWLEVEL && !konami_unlocked()) {
                app_status_set(S(S0584));
                app_note("settings: 拦下无密码的提级请求 %d->%d",
                         c->security.current_level, want);
                break;
            }

            if (delta < 0 && g_seclevel_armed != want) {
                g_seclevel_armed = want;
                app_status_set(S(S0585),
                               security_level_name(want));
                break;
            }

            g_seclevel_armed = 0;
            c->security.current_level = want;
            /* 记住选择：否则下次启动又掉回 L0，用户会以为没生效 */
            c->security.persist_level = true;
            app_cfg_save_now();
            app_note("settings: sec_level=%s", security_level_name(want));
            app_status_set(S(S0586), security_level_name(want));
            break;
        }
        /* ★ 网络自检是**动作**不是开关 —— 必须在 default 之前拦下。
         *   否则会掉进 `toggle_diag()` 的 default，被当成
         *   「详细状态文件」那一项去切 verbose_status：按了有反应，
         *   但反应的是隔壁功能，而界面上看不出任何异常。
         *   这正是"default 兜底吃掉未处理项"的典型形态。 */
        case R_ROW_NETPROBE:  if (delta) do_netprobe(); break;
        default:              if (delta) toggle_diag(row); break;
        case R_ROW_VERSION:
            /* 这一行没有"上一档/下一档"。给一句明确指引，
             * 而不是静默 —— 静默会让人以为程序坏了（同"助手人设"那条）。 */
            app_status_set(S(S0587));
            break;
    }
}

/* --------------------------------------------------------------------------
 * 绘制
 * ----------------------------------------------------------------------- */

/* 密码门弹窗的绘制。实现放在文件后半（和 settings_handle 挨着，
 * 因为它们是"键位被接管期间"的行为），这里先声明 ——
 * 顺序反了的话编译器只会给一句 implicit-declaration，看着像小毛病，
 * 实际是"两份类型不同的同名函数"。 */
static void draw_konami_top(void);
static void draw_konami_bottom(void);

static void render_top(void)
{
    const float x = (float)W_TOP_MARGIN;
    float y = (float)ROW_Y0;

    /* 页眉右侧显示"7/13"这样的进度，让人知道下面还有 */
    char pos[24];
    snprintf(pos, sizeof pos, "%d/%d", g_cursor + 1, R_ROW_COUNT);
    w_top_header(S(S0369), pos);

    for (int i = g_top_row; i < g_top_row + ROW_VISIBLE && i < R_ROW_COUNT; ++i) {
        char line[96], val[64];
        const bool cur = (i == g_cursor);
        RColor fg;

        row_value(i, val, sizeof val);
        snprintf(line, sizeof line, "%s", row_label(i));

        /* 选中行整行加底色，比只改字色显眼得多 —— 触摸下屏时
         * 上屏是唯一的"我现在选中了谁"的反馈 */
        if (cur)
            r_rect(&(Rect){ 0, y - 3, R_TOP_W, ROW_H - 2, 0 }, w_c(C_SLOT_ACC_LT));

        /* 左侧类别色带：3px。外观/提醒/护栏/诊断一眼分得开 */
        r_rect(&(Rect){ 2, y - 1, 3, ROW_H - 6, 0 }, cat_color(ROWS[i].category));

        fg = cur ? w_c(C_SLOT_ACC_DK) : w_c(C_SLOT_FG);
        r_text(&(Rect){ x + 4, y, 150, 16, 0 }, line, fg);

        /* 值右对齐，别和长标签挤在一起 */
        {
            int vw = text_width(val);
            r_text(&(Rect){ (float)(R_TOP_W - W_TOP_MARGIN - vw), y,
                            (float)vw, 16, 0 }, val,
                   cur ? w_c(C_SLOT_ACC_DK) : w_c(C_SLOT_MUTED));
        }

        y += (float)ROW_H;
    }

    /* 滚动提示：还有内容在窗口外时，在上/下边缘按着页眉页脚画。
     * 早先把下箭头放在 y=188，正好压在最后一行字上 —— 现在挪到
     * 内容区下沿（W_TOP_CONTENT_B - 14 = 186）并只在窗口还能滚时才画。 */
    if (g_top_row > 0)
        r_text(&(Rect){ (float)(R_TOP_W / 2 - 4), W_TOP_HEADER_H + 2, 12, 12, 0 },
               "↑", w_c(C_SLOT_ACCENT));
    if (g_top_row + ROW_VISIBLE < R_ROW_COUNT)
        r_text(&(Rect){ (float)(R_TOP_W / 2 - 4), (float)(W_TOP_CONTENT_B - 14),
                        12, 12, 0 }, "↓", w_c(C_SLOT_ACCENT));

    /* ── 版本号已经**不再画在页眉**上了 ──────────────────────────────────
     *
     * 2026-10-10 真机反馈：「版本号挂在那个顶上又没法按」，用户还觉得它"招笑"。
     * 查下来是两个真问题，而不只是审美：
     *   1. **设置列表在上屏，而 3DS 只有下屏能触摸** —— 画在上屏的任何东西
     *      都点不到，只能靠十字键选。所以“可点”就必须是“可选择的一行”。
     *   2. 它当时占的 (R_TOP_W-74, 4, 70x16) 恰好压在页眉右侧的
     *      "7/15"进度上（w_top_header 的 right 参数就是画在那里的），
     *      而旁边那句"点版本号有隐藏设置"又写在 (W_TOP_MARGIN, 6)，
     *      刚好盖住标题「设置」(W_TOP_MARGIN, 3)。
     *      —— 又是"两个 Rect 各自都合法、叠在一起看不出来"那类 bug，
     *      靠读代码想不到，**判据是截图**。
     *
     * 现在版本号是列表里的 R_ROW_VERSION 一行，见上面的 ROWS[]。
     * 连点六下的机关**没变**：konami 吃的是全局按键，选到那一行按 A 自然算。 */

    if (konami_capturing()) {
        /* 页脚先画（它在 y>=200，弹窗只到 196），再盖弹窗。
         * ⚠️ 判据是 konami_capturing() 而**不是** konami_prompt_active()：
         *    提示被第一个方向键关掉之后还在收密码，那时如果放行，
         *    界面就会显示成"正常设置屏"，而键位其实被接管了 ——
         *    用户看到的是一个"按键没反应"的设置屏。 */
        w_top_footer(S(S0588), S(S0589));
        draw_konami_top();
        return;
    }
    if (konami_unlocked())
        w_top_footer(app_status(), S(S0590));
    else
        w_top_footer(app_status(), S(S0422));
}

/* 下屏两个触摸按钮：上一个 / 下一个 */
#define SET_BTN_W   132.0f
#define SET_BTN_Y   148.0f
#define SET_BTN_H   40.0f
static const Rect SET_BTN_MINUS = { W_BOT_MARGIN, SET_BTN_Y, SET_BTN_W, SET_BTN_H, 0 };
static const Rect SET_BTN_PLUS  = { W_BOT_MARGIN + SET_BTN_W + 16.0f,
                                    SET_BTN_Y, SET_BTN_W, SET_BTN_H, 0 };

/* ---- 主题色色块 --------------------------------------------------------
 * 8 个色块，34 宽 + 4 间隙 = 300，正好铺满可用宽。
 * 位置在动作按钮（148..188）之下、页脚（>=214）之上，占 192..212。
 *
 * ★ 为什么用色块而不是"主题色"这一行做加减：
 *   颜色是**看**出来的，不是读出来的。给一行 "主题色: teal" 再让用户按左右键
 *   逐个试，等于让他盲选。8 个色块摆在那，一眼就知道有什么可选。
 * ★ 矩形函数同时供绘制和 w_hit 用 —— 同源才不会"看着能点、点不到"。
 */
#define ACCENT_SW_W   34.0f
#define ACCENT_SW_GAP  4.0f
#define ACCENT_SW_Y  192.0f
#define ACCENT_SW_H   20.0f

static Rect accent_rect(int i)
{
    Rect r;
    r.x = (float)W_BOT_MARGIN + (float)i * (ACCENT_SW_W + ACCENT_SW_GAP);
    r.y = ACCENT_SW_Y;
    r.w = ACCENT_SW_W;
    r.h = ACCENT_SW_H;
    return r;
}

static void draw_accent_swatches(void)
{
    int n = w_accent_count(), cur = w_accent_current();

    for (int i = 0; i < n; ++i) {
        Rect r = accent_rect(i);
        RColor c = w_accent_swatch(i);

        r_rect(&r, c);
        if (i == cur) {
            /* 选中的那个描一圈前景色边框：单色块在浅色主题下
             * 只靠"亮一点"是看不出来的。 */
            r_frame_rect(&r, w_c(C_SLOT_FG), 2.0f);
        }
    }
}

static void render_bottom(void)
{
    const float x = (float)W_BOT_MARGIN;
    const float w = (float)W_BOT_USABLE;
    float y = (float)W_BOT_CONTENT_Y;

    /* 弹窗期间下屏**只画弹窗**：底屏那两个按钮改的是"当前选中行的值"，
     * 密码期间它们既不该能按，也不该在屏幕上招人按。 */
    if (konami_capturing()) {
        w_bot_header(S(S0591));
        w_bot_footer(S(S0592));
        draw_konami_bottom();
        return;
    }

    char val[64];
    row_value(g_cursor, val, sizeof val);

    {
        char hdr[64];
        snprintf(hdr, sizeof hdr, "%s · %s", cat_name(ROWS[g_cursor].category),
                 row_label(g_cursor));
        w_bot_header(hdr);
    }

    /* 说明：护栏类给一句"改了会怎样"，别让用户盲改 */
    {
        const char *desc;
        switch (g_cursor) {
            case R_ROW_SECLEVEL:
                desc = S(S0817);
                break;
            case R_ROW_THEME:
                desc = S(S0802);
                break;
            case R_ROW_ACCENT:
                desc = S(S0914);
                break;
            case R_ROW_MEMORY:
                desc = S(S0867);
                break;
            case R_ROW_REPLYLEN:
                desc = S(S0868);
                break;
            case R_ROW_CTXTURNS:
                desc = S(S0869);
                break;
            case R_ROW_REPLYLANG:
                desc = S(S0870);
                break;
            case R_ROW_FONTSIZE:
                desc = S(S0803);
                break;
            case R_ROW_DEPTH:
                desc = S(S0804);
                break;
            case R_ROW_NAND:
                desc = S(S0805);
                break;
            case R_ROW_OTA:
                desc = S(S0806);
                break;
            case R_ROW_G_OVERWRITE:
                desc = S(S0807);
                break;
            case R_ROW_G_DELETE:
                desc = S(S0808);
                break;
            case R_ROW_G_BULK:
                desc = S(S0809);
                break;
            case R_ROW_G_WRITEKB:
                desc = S(S0810);
                break;
            case R_ROW_G_BATCH:
                desc = S(S0811);
                break;
            case R_ROW_D_OVERFLOW:
                desc = S(S0812);
                break;
            case R_ROW_D_NOTES:
                desc = S(S0813);
                break;
            case R_ROW_NETPROBE:
                desc = S(S0921);
                break;
            default:
                desc = S(S0814);
                break;
        }
        y = w_wrap(x, y, w, desc, w_c(C_SLOT_FG), TEXT_LINE_H);
    }

    /* 当前值用大一点的存在感显示 */
    y += 6;
    w_panel(&(Rect){ x, y, w, 30, 0 }, w_c(C_SLOT_PANEL), w_c(C_SLOT_LINE));
    {
        char cur[80];
        snprintf(cur, sizeof cur, S(S0606), val);
        r_text(&(Rect){ x + 10, y + 7, w - 20, 16, 0 }, cur, w_c(C_SLOT_FG));
    }

    /* 箭头用 ← → 而不是 ◀ ▶：后者在东亚宽度表里是全角（16px），
     * 挤在按钮标签里显得又大又重；前者是比例宽度的箭头，跟汉字排一起更协调。 */
    w_button(&SET_BTN_MINUS, S(S0607),
             W_BTN_NORMAL | (app_touch_on(&SET_BTN_MINUS) ? W_BTN_PRESSED : 0));
    w_button(&SET_BTN_PLUS, S(S0608),
             W_BTN_PRIMARY | (app_touch_on(&SET_BTN_PLUS) ? W_BTN_PRESSED : 0));

    if (g_cursor == R_ROW_ACCENT)
        draw_accent_swatches();
    w_bot_footer(S(S0609));
}

/* --------------------------------------------------------------------------
 * 输入
 * ----------------------------------------------------------------------- */

/* --------------------------------------------------------------------------
 * 密码门弹窗
 *
 * ★ 为什么从"页脚一行提示"改成"盖住全屏的弹窗"（2026-10-16 真机反馈）：
 *
 *   旧做法只在页脚写一句"正在等密码"，**键位仍然和设置列表共享** ——
 *   用户按 ↑↓ 既在输密码、又在挪选中的行；按 ←→ 顺手改掉了当前行的值；
 *   按 B 直接把设置屏弹掉。结果是密码**永远输不完**，
 *   而用户看到的只是"选中的项目会跟着移动"。
 *
 *   这跟"提示写得够不够清楚"无关，是**键位归属**没定下来。
 *   所以做成弹窗，并在 handle 里用 konami_capturing() 把这一帧的键
 *   全部吞掉 —— 弹窗期间，键盘属于密码，不属于列表。
 *
 * ★ 取消键用 **R**（不是 START）：main.c 在 app_handle **之前**就把
 *   START 当全局退出键吃了（`if (down & KEY_START) break;`），
 *   拿它当取消键等于"一按就退出程序"。
 *   B/A/方向键全是密码本身的键，动不得。
 * ----------------------------------------------------------------------- */

/* 下屏「取消」按钮。**render 与 handle 共用这一份** ——
 * 两处各写一遍坐标，改版面时必然漂移成"看着能点、点不到"。 */
static const Rect KONAMI_CANCEL_BTN = { 95, 132, 130, 34, 0 };

/* 进度点：**已对上的**画实心，剩下的留浅色。
 *
 * 刻意**不复用 w_dots**，虽然它看起来正是这个形状：
 * 它是"分页指示器"，语义是"你在第几页"，只点亮 i == cur 那一颗。
 * 拿它来画进度，progress=0 时反而第一颗是亮的 —— 看起来像"已经对上
 * 一个了"，而这正是密码门最不该给错的信息。 */
static void draw_konami_dots(float cx, float y)
{
    const float d    = 10.0f;
    const float gap  = 4.0f;
    const int   n    = konami_len();
    const int   done = konami_progress();
    float       x    = cx - ((float)n * d + (float)(n - 1) * gap) / 2.0f;

    for (int i = 0; i < n; ++i) {
        r_rect(&(Rect){ x, y, d, d, 0 },
               (i < done) ? w_c(C_SLOT_ACCENT) : w_c(C_SLOT_LINE));
        x += d + gap;
    }
}

static void draw_konami_top(void)
{
    /* 面板**整块盖住内容区**（不是留边的对话框）。
     *
     * 早先留了 24px 边距，结果被盖住的设置列表在左右两侧露出半列字
     * （"单 / 批 / 安 / 超…"），看着像界面画歪了 —— 3DS 这块屏太小，
     * 留边的对话感换不来干净。整块盖住之后，露在外面的只剩页眉和页脚，
     * 反而一眼就知道"现在是弹窗在管事"。 */
    const Rect box = { 0, 26, R_TOP_W, 172, 0 };
    char       prog[48];

    w_panel(&box, w_c(C_SLOT_BG), w_c(C_SLOT_ACCENT));

    w_center(200.0f, 44.0f, S(S0610), w_c(C_SLOT_ACC_DK));
    w_center(200.0f, 70.0f, S(S0611), w_c(C_SLOT_FG));

    snprintf(prog, sizeof prog, S(S0612),
             konami_progress(), konami_len());
    w_center(200.0f, 96.0f, prog, w_c(C_SLOT_ACCENT));

    draw_konami_dots(200.0f, 120.0f);

    w_center(200.0f, 146.0f, S(S0613),
             w_c(C_SLOT_MUTED));
    w_center(200.0f, 166.0f, S(S0614), w_c(C_SLOT_MUTED));
}

static void draw_konami_bottom(void)
{
    const Rect box = { 0, 26, R_BOT_W, 182, 0 };

    w_panel(&box, w_c(C_SLOT_BG), w_c(C_SLOT_ACCENT));

    w_center(160.0f, 44.0f, S(S0615), w_c(C_SLOT_ACC_DK));

    w_wrap(42.0f, 70.0f, 236.0f,
           S(S0775),
           w_c(C_SLOT_FG), TEXT_LINE_H);

    w_button(&KONAMI_CANCEL_BTN, S(S0288),
             W_BTN_NORMAL | (app_touch_on(&KONAMI_CANCEL_BTN) ? W_BTN_PRESSED : 0));

    w_center(160.0f, 180.0f, S(S0617), w_c(C_SLOT_FAINT));
}

/* --------------------------------------------------------------------------
 * 输入
 * ----------------------------------------------------------------------- */

/* 密码要吃的键。START 也算：konami 内部拿它当取消（真机上到不了，
 * 见 konami.c，但探针会直接喂它）。 */
#define KONAMI_KEYS (KEY_A | KEY_B | KEY_START | \
                     KEY_UP | KEY_DOWN | KEY_LEFT | KEY_RIGHT)

static void settings_handle(u32 down, u32 held)
{
    (void)held;

    /* 主题色色块：**只在光标停在「主题色」那一行时才画、才响应** ——
     * 用户明确要求"选到颜色那个选项才会出现选颜色的选项"，
     * 所以它是**条件展开**的，不是常驻调色板。
     * 判它放在最前面是因为它压在按钮下方的空白区，不属于任何一行，
     * 若落到行逻辑之后就会被"点击落空点 = 无动作"提前吃掉。 */
    if (g_cursor == R_ROW_ACCENT) {
        int tx, ty;
        if (app_touch_down(&tx, &ty)) {
            for (int i = 0; i < w_accent_count(); ++i) {
                Rect r = accent_rect(i);
                if (w_hit(&r, tx, ty)) {
                    if (i != w_accent_current()) {
                        AppConfig *c = app_cfg();
                        w_accent_set(i);
                        if (c) {
                            snprintf(c->ui.accent, sizeof c->ui.accent,
                                     "%s", w_accent_name(i));
                            if (!config_save(c))
                                app_status_set("已换色，但写配置失败（重启会还原）");
                            else
                                app_status_set("主题色：%s", w_accent_name(i));
                        }
                        app_note("settings: accent=%s", w_accent_name(i));
                    }
                    return;
                }
            }
        }
    }

    /* ---- 密码门：先喂键，再看它有没有接管 ----
     *
     * ★ A 只在**光标停在「版本号」那一行**时才算"连点"。
     *   否则用户在别的行连按 A 改值（比如连着把几个护栏项都打开），
     *   同样能把弹窗叫出来 —— 而弹窗一出来键位就被接管，
     *   用户只会觉得"设置突然不能用了"。
     *   这也是"连点版本号"这个说法本来就该表达的意思。 */
    {
        u32 feed = down & KONAMI_KEYS;

        if ((feed & KEY_A) && !konami_capturing() &&
            g_cursor != R_ROW_VERSION)
            feed &= ~KEY_A;

        if (feed)
            konami_feed(feed);

        /* ★★ 接管期间：这一帧到此为止。
         *    少了这个 return，↑↓ 会一边输密码一边挪光标、←→ 会顺手改
         *    当前行的值、B 会把整屏弹掉 —— 真机反馈的"一按密码，选中的
         *    项目就跟着移动"就是这里漏了闸。 */
        if (konami_capturing()) {
            int tx, ty;

            /* 只在**真的吞了这个键**的那一帧记一行。
             *
             * ⚠️ 不能按帧记：用户在弹窗上停留多久，就往 notes 里灌多少行，
             *    而 notes 缓冲只有 6144 字节 —— 灌满之后 app_note 会
             *    **静默丢行**（W 趟那条 notes_bytes 判据就是为这个加的）。
             *    第一版写成按帧记，跑一趟就写了 46 行，其中 35 行是空帧。
             *
             * 这一行是**给验收看的**：它把"键位被接管期间光标一动没动"
             * 变成能从 status.txt 读出来的事实。而"按密码的时候选中的
             * 项目跟着跑"正是 2026-10-16 真机反馈的那个 bug ——
             * 光比对截图看不出光标有没有动（弹窗把列表盖住了）。 */
            if (feed)
                app_note("secgate: swallow %08x cursor_row=%d",
                         (unsigned)feed, g_cursor + 1);

            if (down & KEY_R) {
                konami_cancel();
                return;
            }
            if (app_touch_down(&tx, &ty) &&
                w_hit(&KONAMI_CANCEL_BTN, tx, ty)) {
                konami_cancel();
                return;
            }
            return;
        }
    }

    /* 触摸：只有两个按钮，但它们改变的是**当前选中行**的值，
     * 所以点上/下按钮之前要先在列表里选好行 —— 这是刻意的：
     * 14 行摊在一张 320x240 的下屏上会小到点不中。 */
    int tx, ty;
    if (app_touch_down(&tx, &ty)) {
        const bool minus = w_hit(&SET_BTN_MINUS, tx, ty);
        const bool plus  = w_hit(&SET_BTN_PLUS,  tx, ty);

        if (minus || plus) {
            /* 人设是自由文本，没有"上一档/下一档"——两个按钮都引到编辑器。
             * 让它们什么都不做会让人以为按钮坏了。 */
            if (g_cursor == R_ROW_SYSPROMPT) { edit_prompt(); return; }
            if (g_cursor == R_ROW_MEMORY)    { edit_memory(); return; }
            row_step(g_cursor, plus ? +1 : -1);
            return;
        }
    }

    if (down & KEY_B) { app_pop(); return; }

    if (down & KEY_UP) {
        if (g_cursor > 0) --g_cursor;
        clamp_scroll();
        return;
    }
    if (down & KEY_DOWN) {
        if (g_cursor < R_ROW_COUNT - 1) ++g_cursor;
        clamp_scroll();
        return;
    }
    if (down & KEY_LEFT)  { row_step(g_cursor, -1); return; }
    if (down & KEY_RIGHT) { row_step(g_cursor, +1); return; }

    /* ★ 2026-10-23：L/R **整屏翻页**。
     *
     * 这一屏有 23 行、只显示 7 行，而列表画在**上屏** ——
     * 3DS 顶屏不能触摸，所以一行一行按方向键是**唯一**的移动方式。
     * 想改最后一行「界面语言」得按 22 下，这不是"稍微有点累"，
     * 是实质上不可用（用户宁可不改）。
     *
     * 与「更多」屏同一个键位、同一个手感（那边 8 项 6 可见也是 L/R 翻页），
     * 两个列表屏的行为终于一致了。
     *
     * ⚠️ 位置必须在 LEFT/RIGHT 之后：L/R 是独立键位（不是方向键别名），
     *   两者不会互相吃掉，但顺序上先判方向更符合直觉。 */
    if (down & KEY_L) { page_step(-1); return; }
    if (down & KEY_R) { page_step(+1); return; }

    if (down & KEY_A) {
        if (g_cursor == R_ROW_SYSPROMPT) { edit_prompt(); return; }
        if (g_cursor == R_ROW_MEMORY)    { edit_memory(); return; }
        row_step(g_cursor, +1);
        return;
    }
}

const Screen g_screen_settings = {
    .id            = SCREEN_SETTINGS,
    .name          = "SCREEN_SETTINGS",
    .enter         = settings_enter,
    .leave         = NULL,
    .render_top    = render_top,
    .render_bottom = render_bottom,
    .handle        = settings_handle,
};
