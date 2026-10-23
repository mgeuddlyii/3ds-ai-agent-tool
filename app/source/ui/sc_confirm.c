/*
 * sc_confirm.c -- 动作确认（三道闸里的第三道）
 *
 * 上屏 = 全部事实：要做什么、动的是哪个文件、写进去的是什么内容。
 * 下屏 = 只有「拒绝」和「允许」两个按钮。
 *
 * 这一屏是整个项目里**唯一**能把模型的意图变成真实副作用的地方，
 * 所以它有几个刻意的设计，改之前先读一遍：
 *
 *   · **默认焦点是"拒绝"。** 没细看就顺手按 A，结果应当是"什么都没发生"，
 *     而不是"文件已经写下去了"。
 *
 *   · **解释动作的文字全部是编译期常量**（actions_verb / actions_detail）。
 *     只有参数区那几行是模型给的 —— 因为它们本来就是"待核对的内容"。
 *     一旦让模型决定标题怎么写，它就能把确认框伪装成别的东西，
 *     而用户以为自己批准的是另一件事。
 *
 *   · **参数原样显示，不做任何美化。** 文件名里的怪字符、内容里的奇怪换行，
 *     都要照原样摆出来 —— 兜底地"整理一下"等于替模型打掩护。
 *
 *   · **不认识的动作用不了"允许"。** 白名单外的动作只能拒。
 */

#include "screen.h"

#include "actions.h"
#include "app.h"
#include "locale.h"
#include "pkg.h"
#include "widgets.h"
#include "actapply.h"
#include "sysinfo.h"
#include "sandfile.h"
#include "actmodel.h"
#include "actfile.h"
#include "actsvc.h"
#include "cupd.h"
#include "shfont.h"

#include <stdio.h>
#include <stdlib.h>     /* strtol —— 把安全等级 L1 解析成 1 */
#include <string.h>
#include <strings.h>   /* strcasecmp —— 判 .cia 后缀用（与 actions.c 同判据，但
                        * 这里**必须再判一次**：本屏要用它决定显示哪个落点，
                        * 依赖别处判过就是在赌两处判断一致） */
#include "uistr.h"

/* 0 = 拒绝（默认），1 = 允许 */
static int       g_focus;
static ActionReq g_req;
static bool      g_have;
static bool      g_done;

/* 二次确认的"上膛"状态。0 = 未上膛，1 = 已上膛（再来一次 A 才真执行）。
 * 只给 actions_needs_double_confirm() 登记过的动作用。 */
static int       g_armed;
static char      g_result[192];

/* 这一轮**有没有**已经给模型留过结果。
 *
 * 用来保证"每条路都留一句话"：成功且有明细的那几支会存详细内容，
 * 其余的（参数不合法、执行失败、用户拒绝）由兜底补一句。
 * 少了这个标记，兜底那句会把详细结果**盖掉**（app_act_result_store 是整体覆盖的）。 */
static bool      g_result_stored;

/* 存一句给模型的结果，顺便记下"已经有了"。所有 store 都必须走这里。 */
static void store_result(const char *s)
{
    app_act_result_store(s);
    g_result_stored = true;
}

/* 只有白名单里的动作才谈得上"允许"。
 *
 * 每加一个动作，这里必须显式加一条 —— 不能写成 `id != ACT_UNKNOWN`。
 * 那种写法会在 next 动作加进来的那一刻**默认放行**它，
 * 而"新动作默认能不能执行"这件事是必须由人拍板的，不该由写法决定。 */
static bool can_allow(void)
{
    if (!g_have)
        return false;
    /* 设置类一并在这里放行 —— 它们改的只是本机配置，
     * 风险面比 file.write 小，但同样要过这一屏（用户要看见
     * "它要把安全等级从 L0 提到 L1"这句话，才谈得上决定）。 */
    return g_req.id == ACT_FILE_WRITE
        || g_req.id == ACT_PKG_INSTALL
        || g_req.id == ACT_LOCALE_SET
        || g_req.id == ACT_LOCALE_CLEAR
        || actions_is_setting(g_req.id)
        || actions_is_readonly(g_req.id)
        || actions_is_install(g_req.id)
        || actions_is_fileop(g_req.id);
}

/* 把 s 截到不超过 max 字节，且**不切开 UTF-8 字符**。
 *
 * 两个容易写错的地方：
 *   · 0x80..0xBF 是"后续字节"（高位 10xxxxxx），截点落在这种字节上
 *     说明正劈在一个字中间 —— 往前退到字符首字节。
 *   · 必须先看长度再动刀。s[max] 只有 max < len 时才是合法读取；
 *     若 len <= max 就原样返回，越界读是无从察觉的。
 *
 * 注意 sc_chat.c 里也有个同名的局部函数（utf8_floor），那个是
 * "给一个起点、往回找字符边界"，用途不同 —— 改这个的时候别顺手改那个。 */
static void utf8_cut(char *s, size_t max)
{
    size_t n = strlen(s);

    if (n <= max)
        return;
    while (max > 0 && ((unsigned char)s[max] & 0xC0) == 0x80)
        --max;
    s[max] = '\0';
}

static void confirm_enter(void)
{
    const ActionReq *req = actions_pending();

    g_focus     = 0;              /* 安全默认：拒绝 */
    g_done      = false;
    g_result[0] = '\0';
    g_have      = (req != NULL);
    g_result_stored = false;      /* 每进一次确认屏都从"还没给模型留话"起算 */

    if (req)
        g_req = *req;
    else
        memset(&g_req, 0, sizeof g_req);

    app_status_set(S(S0175),
                   g_have ? actions_verb(g_req.id) : S(S0176));
}

/* --------------------------------------------------------------------------
 * 上屏：事实
 * ----------------------------------------------------------------------- */

static void render_top(void)
{
    const float x = (float)W_TOP_MARGIN;
    const float w = (float)W_TOP_USABLE;
    float y = (float)W_TOP_CONTENT_Y;
    char  line[128];

    w_top_header(S(S0177), can_allow() ? S(S0178) : S(S0179));

    if (!g_have) {
        w_wrap(x, y, w, S(S0180),
               w_c(C_SLOT_FG), TEXT_LINE_H);
        w_top_footer(app_status(), "");
        return;
    }

    /* 动作名是编译期常量，不是模型给的。 */
    y = w_line(x, y, w, actions_verb(g_req.id),
               can_allow() ? w_c(C_SLOT_WARN_DK) : w_c(C_SLOT_MUTED));
    y += 6;

    if (!can_allow()) {
        w_wrap(x, y, w, actions_detail(ACT_UNKNOWN), w_c(C_SLOT_FG), TEXT_LINE_H);
        w_top_footer(app_status(), S(S0181));
        return;
    }

    /* ---- 安装程序：这一屏要说清的是"它是从哪来的、要装去哪" ----
     * 与 file.write 的区别在于：这里装进去的东西**下次会被执行**。
     * 所以摘要要单独占一行，而且**没有摘要时必须明说没有** ——
     * 一个空白看起来像"核过了没问题"，那是完全相反的意思。
     *
     * ⚠️ 装 .cia 与装 .3dsx 的**落点完全不同**，这一屏必须分开画：
     *   · .3dsx → 3ds/，Homebrew Launcher 里出现，只要 SD 写权限；
     *   · .cia  → 主菜单里出现，要 am:u，而且**只在 CIA 形态下才拿得到**。
     * 早先这里写死了 .3dsx 的后缀和落点，装 .cia 时会显示成
     * "会装到 sdmc:/3ds/xxx.3dsx" —— 那是**错的，且方向危险**：
     * 用户会以为它不动主菜单。所以后缀判定在这里必须再做一次。 */
    if (g_req.id == ACT_PKG_INSTALL) {
        int  sz = 0;
        bool is_cia;
        size_t nl = strlen(g_req.name);

        is_cia = (nl > 4) && (strcasecmp(g_req.name + nl - 4, ".cia") == 0);

        y = w_kv(x, y, 76, S(S0182), g_req.name,
                 w_c(C_SLOT_MUTED), w_c(C_SLOT_ACC_DK), TEXT_LINE_H);

        if (is_cia) {
            if (pkg_cia_size(g_req.name, &sz))
                snprintf(line, sizeof line, S(S0183), sz, sz / 1048576.0);
            else
                snprintf(line, sizeof line, "%s", S(S0184));
        } else {
            if (pkg_size(g_req.name, &sz))
                snprintf(line, sizeof line, S(S0183), sz, sz / 1048576.0);
            else
                snprintf(line, sizeof line, "%s", S(S0184));
        }
        y = w_kv(x, y, 76, S(S0185), line,
                 w_c(C_SLOT_MUTED),
                 sz > 0 ? w_c(C_SLOT_FG) : w_c(C_SLOT_WARN),
                 TEXT_LINE_H);

        if (is_cia)
            snprintf(line, sizeof line, "%s", S(S0186));
        else
            snprintf(line, sizeof line, "%s/%s.3dsx", pkg_install_dir(), g_req.name);
        y = w_kv(x, y, 76, S(S0187), line,
                 w_c(C_SLOT_MUTED),
                 is_cia ? w_c(C_SLOT_WARN_DK) : w_c(C_SLOT_MUTED), TEXT_LINE_H);

        if (g_req.sha256[0])
            snprintf(line, sizeof line, "%.32s…", g_req.sha256);
        else
            snprintf(line, sizeof line, "%s", S(S0188));
        y = w_kv(x, y, 76, S(S0189), line,
                 w_c(C_SLOT_MUTED),
                 g_req.sha256[0] ? w_c(C_SLOT_FG) : w_c(C_SLOT_WARN),
                 TEXT_LINE_H);

        y += 4;
        w_hr(x, y, w, w_c(C_SLOT_LINE));
        y += 6;

        /* 关键的差别必须画在屏幕上，而不是只写在 detail 里：
         * .3dsx 进的是 Homebrew Launcher，.cia 进的是主菜单。 */
        if (is_cia) {
            /* 如果当前形态拿不到 am:u，就**在这儿说出来**。
             * 让用户按了允许之后才在对话里看到失败，是最差的交互 ——
             * 他会以为是包坏了，而那是完全不同的一件事。 */
            if (pkg_cia_available())
                w_wrap(x, y, w,
                       S(S0761),
                       w_c(C_SLOT_ACCENT), TEXT_LINE_H);
            else
                w_wrap(x, y, w,
                       S(S0762),
                       w_c(C_SLOT_WARN_DK), TEXT_LINE_H);
        } else {
            w_wrap(x, y, w,
                   S(S0192),
                   w_c(C_SLOT_ACCENT), TEXT_LINE_H);
        }

        w_top_footer(app_status(), S(S0193));
        return;
    }

    /* ---- 设 / 撤销区域语言 ----
     * 这一屏要说清三件事：改的是**哪个游戏**、改成**什么**、以及
     * **它会写到哪个文件**。
     *
     * 第三件尤其不能省：用户对"region=USA lang=EN"这两个值是没有直觉的，
     * 但看到 "luma/titles/00040000.../locale.txt" 就知道这是 Luma 的文件、
     * 出问题能自己去删。确认屏的意义就是让人能自己判断，
     * 而判断的前提是看得见落点。
     *
     * 这一屏还要**明确说出它不碰系统文件** —— 用户很容易把"改区域"
     * 和"改字库"当成同一档风险（后者确实危险），不说清就不敢按，
     * 或者反过来以为很危险的东西也不危险。 */
    if (g_req.id == ACT_LOCALE_SET || g_req.id == ACT_LOCALE_CLEAR) {
        char path[192];

        y = w_kv(x, y, 76, S(S0194), g_req.tid[0] ? g_req.tid : S(S0044),
                 w_c(C_SLOT_MUTED), w_c(C_SLOT_ACC_DK), TEXT_LINE_H);

        if (g_req.id == ACT_LOCALE_SET) {
            snprintf(line, sizeof line, "%s %s",
                     g_req.region[0] ? g_req.region : S(S0044),
                     g_req.lang[0] ? g_req.lang : S(S0044));
            y = w_kv(x, y, 76, S(S0195), line,
                     w_c(C_SLOT_MUTED), w_c(C_SLOT_ACC_DK), TEXT_LINE_H);
        } else {
            char r[16], l[16];
            if (loc_read(g_req.tid, r, sizeof r, l, sizeof l))
                snprintf(line, sizeof line, S(S0196), r, l);
            else
                snprintf(line, sizeof line, "%s", S(S0197));
            y = w_kv(x, y, 76, S(S0198), line,
                     w_c(C_SLOT_MUTED), w_c(C_SLOT_MUTED), TEXT_LINE_H);
        }

        if (loc_path(g_req.tid, path, sizeof path))
            snprintf(line, sizeof line, "SD:/luma/titles/%s/locale.txt",
                     g_req.tid);
        else
            snprintf(line, sizeof line, "%s", S(S0199));
        y = w_kv(x, y, 76,
                 g_req.id == ACT_LOCALE_SET ? S(S0200) : S(S0201), line,
                 w_c(C_SLOT_MUTED), w_c(C_SLOT_MUTED), TEXT_LINE_H);

        y += 4;
        w_hr(x, y, w, w_c(C_SLOT_LINE));
        y += 6;

        /* Luma 的 "Enable game patching" 关着的话，locale.txt 会被完全忽略 ——
         * 文件安安静静躺着、游戏毫无变化，用户只会觉得"这功能没用"。
         * 我们能读到 config.ini，所以读到关着时**必须在这儿说**。
         * 读不到（LOC_PATCH_UNKNOWN）不拦，只提醒 —— 不同版本键名有出入，
         * 把我们的不确定变成用户的障碍是另一种错。 */
        if (g_req.id == ACT_LOCALE_SET) {
            LocPatchState ps = loc_game_patching();

            if (ps == LOC_PATCH_OFF)
                w_wrap(x, y, w,
                       S(S0763),
                       w_c(C_SLOT_WARN_DK), TEXT_LINE_H);
            else if (ps == LOC_PATCH_UNKNOWN)
                w_wrap(x, y, w, S(S0764),
                       w_c(C_SLOT_ACCENT), TEXT_LINE_H);
            else
                w_wrap(x, y, w,
                       S(S0765),
                       w_c(C_SLOT_ACCENT), TEXT_LINE_H);
        } else {
            w_wrap(x, y, w,
                   S(S0766),
                   w_c(C_SLOT_ACCENT), TEXT_LINE_H);
        }

        w_top_footer(app_status(), S(S0193));
        return;
    }

    /* ---- 清空回收站：把要没的东西**列出来** ----
     *
     * 只报"有 N 个文件"不够：用户点完之后照样不知道自己在删什么。
     * 清单要上屏（那儿有整屏的宽度与高度），下屏留给"批准之后会发生什么"。 */
    if (g_req.id == ACT_TRASH_PURGE) {
        static char tl[224];
        y = w_kv(x, y, 76, S(S0206), ".trash/",
                 w_c(C_SLOT_MUTED), w_c(C_SLOT_FG), TEXT_LINE_H);
        if (actfile_trash_list(tl, sizeof tl)) {
            y = w_wrap(x, y, w, tl, w_c(C_SLOT_FG), TEXT_LINE_H);
        } else {
            y = w_wrap(x, y, w, S(S0207),
                       w_c(C_SLOT_MUTED), TEXT_LINE_H);
        }
        w_wrap(x, y, w, S(S0208),
               w_c(C_SLOT_ACC_DK), TEXT_LINE_H);
        w_top_footer(app_status(), S(S0193));
        return;
    }

    /* ---- 切服务商：把"切到哪家、会连带改什么"摊开 ---- */
    if (g_req.id == ACT_SVC_SET) {
        y = w_kv(x, y, 76, S(S0209),
                 g_req.name[0] ? g_req.name : (g_req.key[0] ? g_req.key : S(S0210)),
                 w_c(C_SLOT_MUTED), w_c(C_SLOT_ACC_DK), TEXT_LINE_H);
        y = w_kv(x, y, 76, S(S0211), S(S0212),
                 w_c(C_SLOT_MUTED), w_c(C_SLOT_FG), TEXT_LINE_H);
        y = w_kv(x, y, 76, S(S0213), "SD:/3ds/aiconsole/config.json",
                 w_c(C_SLOT_MUTED), w_c(C_SLOT_FG), TEXT_LINE_H);
        w_top_footer(app_status(), S(S0193));
        return;
    }

    /* ---- 设置类：把"哪个键、改成什么"摊开给用户看 ----
     *
     * 少了这两行，用户只在标题看到"修改一项设置"，
     * 而 details 里那段通用说明并没有告诉他**具体是哪一项** ——
     * 那就不是可以据以决定的信息。 */
    if (actions_is_setting(g_req.id)) {
        const char *label = S(S0214);
        char       cur[64];

        if (g_req.id == ACT_SEC_LEVEL)
            label = S(S0215);
        else if (g_req.id == ACT_UI_THEME)
            label = S(S0216);
        else if (g_req.id == ACT_UI_FONTSIZE)
            label = S(S0217);
        else if (g_req.id == ACT_UI_DEPTH)
            label = S(S0218);

        y = w_kv(x, y, 76, label,
                 g_req.value[0] ? g_req.value : S(S0219),
                 w_c(C_SLOT_MUTED), w_c(C_SLOT_ACC_DK), TEXT_LINE_H);

        if (g_req.id == ACT_SEC_LEVEL) {
            /* 安全等级要显示**改完之后 AI 能做什么** ——
             * "L0" 这种代号对用户没有意义，
             * "只聊天 / 可写文件 / 可动系统设置 / 底层" 才有。 */
            /* ★ 存编号不存字符串：静态初始化里 S() 放不下。
             * 取值时记得包一层 S()。 */
            static const short LVL[] = {
                S0220, S0221, S0222, S0223, S0224, S0225
            };
            int lv = 0;
            const char *p = g_req.value[0] ? g_req.value : "L0";
            if (*p == 'L' || *p == 'l') ++p;
            lv = (int)strtol(p, NULL, 10);
            if (lv < 0) lv = 0;
            if (lv > 5) lv = 5;
            snprintf(line, sizeof line, "%s", S(LVL[lv]));
            y = w_kv(x, y, 76, S(S0226), line,
                     w_c(C_SLOT_MUTED), w_c(C_SLOT_FG), TEXT_LINE_H);
        }

        y = w_kv(x, y, 76, S(S0213), "SD:/3ds/aiconsole/config.json",
                 w_c(C_SLOT_MUTED), w_c(C_SLOT_FG), TEXT_LINE_H);

        w_top_footer(app_status(), S(S0193));
        return;
    }

    /* ---- 写文件（原有形态）---- */
    y = w_kv(x, y, 76, S(S0227), g_req.name,
             w_c(C_SLOT_MUTED), w_c(C_SLOT_ACC_DK), TEXT_LINE_H);

    snprintf(line, sizeof line, S(S0228), (int)strlen(g_req.text));
    y = w_kv(x, y, 76, S(S0229), line,
             w_c(C_SLOT_MUTED), w_c(C_SLOT_FG), TEXT_LINE_H);

    snprintf(line, sizeof line, "SD:/3ds/aiconsole/out/%s", g_req.name);
    y = w_kv(x, y, 76, S(S0200), line,
             w_c(C_SLOT_MUTED), w_c(C_SLOT_MUTED), TEXT_LINE_H);

    y += 4;
    w_hr(x, y, w, w_c(C_SLOT_LINE));
    y += 6;

    r_text(&(Rect){ x, y, w, 16, 0 }, S(S0230), w_c(C_SLOT_ACCENT));
    y += 18;

    /* 预览框的**下沿钉死在内容区底部**，高度由剩下的空间反推 ——
     * 不能给它一个固定高度：上面三行 kv 的行数会随文件名长度变（会折行），
     * 固定高度在长文件名下会把框推出内容区、被页脚盖住（实测过）。 */
    {
        const float bottom = (float)W_TOP_CONTENT_B - 4.0f;
        const float box_h  = bottom - y;

        if (box_h >= 34.0f) {
            Rect  box = { x, y, w, box_h, 0 };
            char  preview[ACT_TEXT_MAX + 1];

            snprintf(preview, sizeof preview, "%s", g_req.text);
            /* 最多展示框里放得下的行数：按实际行高算，不拍脑袋给字节数 ——
             * 给了字节数还是会折行溢出，等于没限制。 */
            utf8_cut(preview, (size_t)(box_h / 16.0f) * 27 * 3);

            w_panel(&box, w_c(C_SLOT_BG), w_c(C_SLOT_LINE));
            w_wrap(x + 8, y + 6, w - 16, preview, w_c(C_SLOT_FG), 16);
        }
    }

    w_top_footer(app_status(), S(S0193));
}

/* --------------------------------------------------------------------------
 * 下屏：两个按钮
 * ----------------------------------------------------------------------- */

/* ★ 三个按钮矩形由 render 与 handle 共用。各写一遍坐标必然漂移成
 * "看到的位置点不到"，而那种 bug 在截图里看不出来。
 * 左=拒绝(g_focus 0)、右=允许(g_focus 1)，与 A 键语义一致。 */
static const Rect CFM_BTN_SINGLE = {  70, 150, 180, 36, 0 };
static const Rect CFM_BTN_NO     = {  10, 150, 144, 36, 0 };
static const Rect CFM_BTN_YES    = { 166, 150, 144, 36, 0 };

static void render_bottom(void)
{
    const float x = (float)W_BOT_MARGIN;
    const float w = (float)W_BOT_USABLE;
    float y = (float)W_BOT_CONTENT_Y;

    w_bot_header(can_allow() ? S(S0231) : S(S0232));

    if (g_done) {
        w_wrap(x, y, w, g_result, w_c(C_SLOT_FG), TEXT_LINE_H);
        w_button(&CFM_BTN_SINGLE,
                 S(S0233), W_BTN_PRIMARY);
        w_center((float)R_BOT_W / 2.0f, 192, S(S0234), w_c(C_SLOT_FAINT));
        w_bot_footer(S(S0234));
        return;
    }

    if (!can_allow()) {
        w_wrap(x, y, w,
               g_have ? S(S0235)
                      : S(S0236),
               w_c(C_SLOT_FG), TEXT_LINE_H);
        y += 8;
        w_wrap(x, y, w, S(S0237),
               w_c(C_SLOT_MUTED), TEXT_LINE_H);

        w_button(&CFM_BTN_SINGLE,
                 S(S0233), W_BTN_PRIMARY);
        w_center((float)R_BOT_W / 2.0f, 192, S(S0234), w_c(C_SLOT_FAINT));
        w_bot_footer(S(S0234));
        return;
    }

    /* ---- 真正需要确认的形态：两个按钮，没有第三种可能 ---- */
    /* 文案必须**限高**。w_wrap 会老老实实按内容折行，而底屏高度是死的 ——
     * 不设上限的话，一段长说明会一路画到按钮上面去，两段文字叠在一起
     * （实测过：说明被画到了 y=150 的按钮行上）。
     * 这里只给"批准之后会发生什么"这一行，细节交给顶屏的 actions_detail。 */
    if (g_req.id == ACT_PKG_INSTALL)
        w_wrap(x, y, w, S(S0238),
               w_c(C_SLOT_FG), TEXT_LINE_H);
    else if (g_req.id == ACT_LOCALE_SET)
        w_wrap(x, y, w, S(S0239),
               w_c(C_SLOT_FG), TEXT_LINE_H);
    else if (g_req.id == ACT_LOCALE_CLEAR)
        w_wrap(x, y, w, S(S0240),
               w_c(C_SLOT_FG), TEXT_LINE_H);
    else if (g_req.id == ACT_TRASH_PURGE)
        w_wrap(x, y, w, S(S0241),
               w_c(C_SLOT_FG), TEXT_LINE_H);
    else if (actions_is_setting(g_req.id))
        /* 设置类的"批准之后"必须区分**放宽权限**与**改外观**：
         * 安全等级是唯一会扩大 AI 动手范围的那一条，
         * 跟"改个配色"共用一句话会让人以为是同一量级。 */
        w_wrap(x, y, w,
               g_req.id == ACT_SEC_LEVEL
                   ? S(S0242)
                   : S(S0243),
               w_c(C_SLOT_FG), TEXT_LINE_H);
    else
        w_wrap(x, y, w, S(S0244),
               w_c(C_SLOT_FG), TEXT_LINE_H);
    y += TEXT_LINE_H + 4;

    /* 说明区的下界：往上收一点，绝不碰按钮 */
    if (y < 146.0f - TEXT_LINE_H)
        w_wrap(x, y, w, actions_detail(g_req.id), w_c(C_SLOT_MUTED),
               TEXT_LINE_H);

    w_button(&CFM_BTN_NO, S(S0245),
             g_focus == 0 ? W_BTN_SELECTED : W_BTN_NORMAL);
    w_button(&CFM_BTN_YES, S(S0246),
             g_focus == 1 ? W_BTN_SELECTED : W_BTN_NORMAL);

    w_center((float)R_BOT_W / 2.0f, 192,
             S(S0247), w_c(C_SLOT_FAINT));
    w_bot_footer(S(S0248));
}

/* --------------------------------------------------------------------------
 * 行为
 * ----------------------------------------------------------------------- */

/* -------------------------------------------------------------------------- 
 * 设置类动作的落盘
 *
 * shared 层只做校验（actions_check_setting），写配置必须在这一层 ——
 * app_cfg() 与 w_theme() 都在 app 侧，shared 不该反过来依赖它。
 *
 * 键的映射表**只用 actions_cfg_key_at() 那一张**，不另写一份：
 * 两处各写一张表，迟早会有一处漏改，而漏改的后果是
 * "模型能改一个界面上根本不存在的开关"。
 * ----------------------------------------------------------------------- */
static void do_run(void)
{
    char msg[192];
    bool ok;

    if (actions_is_setting(g_req.id)) {
        char value[32];
        ok = actions_check_setting(&g_req, value, sizeof value, msg, sizeof msg);
        if (ok)
            ok = act_apply_setting(g_req.key[0] ? g_req.key : "?", value,
                                  msg, sizeof msg);
    } else if (actions_is_readonly(g_req.id)) {
        /* 只读动作：先校参数 → 采集 → 把**采集结果**存进回灌缓冲。
         * 存结果而不是那句"已执行"：模型要的是内容，
         * 存了"已执行"它下一轮还是答不上来，等于整个机制空转。
         *
         * 读不到时**也要回灌**：那句话本身就是信息
         * （"目录打不开"和"目录是空的"对模型的结论完全不同），
         * 只显示在确认屏上等于没说。 */
        static char info[ACT_RESULT_MAX];
        char vmsg[192];

        if (!actions_check_readonly(&g_req, vmsg, sizeof vmsg)) {
            /* 参数不合法：连采集都不该做（读了也是白读）。
             * 用 if 包住而不是 break —— do_run 里没有循环，
             * 早先写成 break 编译不过。 */
            ok = false;
            snprintf(msg, sizeof msg, "%s", vmsg);
        } else {
            info[0] = '\0';
            if (g_req.id == ACT_FILE_LIST) {
                sandfile_list(g_req.key[0] ? g_req.key : "...",
                              info, sizeof info);
            } else if (g_req.id == ACT_FILE_READ) {
                long from = 0;
                if (g_req.value[0])
                    from = strtol(g_req.value, NULL, 10);
                sandfile_read(g_req.name, from, info, sizeof info);
            } else {
                sysinfo_collect(info, sizeof info);
            }

            ok = (info[0] != '\0');
            snprintf(msg, sizeof msg, "%s",
                     ok ? S(S0249)
                        : S(S0250));
            if (ok)
                store_result(info);
        }
    } else if (actions_is_install(g_req.id)) {
        /* 安装类：先校参数（不合法就别去动文件），再真装。
         * 这一类**要落盘**，所以结果不是回灌给模型的"我装好了"，
         * 而是它随后会看到的界面变化 —— 但仍要存一句，
         * 因为模型下一轮要能对上"它说装上了"与屏幕上的实际结果。 */
        char vmsg[192];
        if (!actions_check_install(&g_req, vmsg, sizeof vmsg)) {
            ok = false;
            snprintf(msg, sizeof msg, "%s", vmsg);
        } else if (g_req.id == ACT_FONT_INSTALL) {
            ok = shfont_apply(g_req.name, msg, sizeof msg);
            if (ok)
                store_result(S(S0251));
        } else {
            ok = model_install_action(g_req.key, g_req.value, msg, sizeof msg);
            if (ok)
                store_result(msg);
        }
    } else if (g_req.id == ACT_SVC_SET) {
        /* 服务商单独一支：它的校验器是 actions_check_svc（白名单不同），
         * 而且**成功不等于能用** —— 没有凭据时切过去照样发不出消息，
         * 所以要把这一点写进给模型的那句话里。 */
        char vmsg[192];
        const char *nm = g_req.name[0] ? g_req.name : g_req.key;
        if (!actions_check_svc(&g_req, vmsg, sizeof vmsg)) {
            ok = false;
            snprintf(msg, sizeof msg, "%s", vmsg);
        } else {
            ok = act_svc_set(nm, msg, sizeof msg);
            if (ok)
                store_result(msg);
        }
    } else if (actions_is_fileop(g_req.id)) {
        /* 文件与凭据类：先校参数（不合法就别去动任何东西），再执行。
         * 这一类里 file.delete 走的是**移进回收站**而不是真删，
         * 所以即使执行失败，原文件通常还在 —— 但那不作为可以放松校验的理由：
         * 校验挡住的是"目标根本不对"，那种情况连回收站都进不去。 */
        char vmsg[192];
        if (!actions_check_fileop(&g_req, vmsg, sizeof vmsg)) {
            ok = false;
            snprintf(msg, sizeof msg, "%s", vmsg);
        } else {
            switch (g_req.id) {
                case ACT_TRASH_PURGE:
                    ok = actfile_trash_purge(msg, sizeof msg);
                    break;
                case ACT_FILE_DELETE:
                    ok = actfile_delete(g_req.name, msg, sizeof msg);
                    break;
                case ACT_FILE_RENAME:
                    ok = actfile_rename(g_req.name, g_req.to, msg, sizeof msg);
                    break;
                case ACT_KEY_REMOVE:
                    ok = actkey_remove(g_req.name, msg, sizeof msg);
                    break;
                default:
                    ok = actkey_switch(g_req.name, msg, sizeof msg);
                    break;
            }
            if (ok)
                store_result(msg);
        }
    } else {
        ok = actions_run(&g_req, msg, sizeof msg);
    }

    g_done = true;
    snprintf(g_result, sizeof g_result, "%s", msg);

    /* ★ 兜底：**这一条路要走完，就必须给模型留一句话**。
     *
     * 早先只有"成功且有明细"的那几支会 store，于是模型看不到两种最需要它知道的结果：
     *   · **参数不合法 / 执行失败** → 它下一轮什么都收不到，于是**照原样再发一遍**，
     *     而它本来应该改成合法的写法或换一个动作；
     *   · **用户点了拒绝** → 同理（见 do_deny），它会一遍遍请求同一个动作。
     * 对"多步干活"来说这两条是致命的：模型学不到任何东西，只会原地打转。
     * 这一段与 AGENT.md §0/§3 里那两句承诺是**一对**，改了这里要同步改手册。 */
    if (!g_result_stored) {
        char line[192];
        snprintf(line, sizeof line, ok ? S(S0252) : S(S0253), msg);
        store_result(line);
    }

    /* 允许与失败都记。失败也要记 —— 否则"它试了但没成"这件事就消失了。 */
    actions_audit(ok ? "ALLOW" : "FAIL", &g_req, msg);
    app_status_set("%s", ok ? S(S0254) : S(S0255));
    app_note("action: %s ok=%d %s", actions_verb(g_req.id), (int)ok, msg);

    /* ★ 给对话列表留一句人话（"动作后来怎么样了"）。
     * 与给模型那句**分开写**：两句话的读者不同（见 app.c 的注释）——
     * 把"不要原样再发一遍"这种命令摆在用户眼前，就是在自言自语。 */
    {
        /* 静态区：msg 最多 ACT_RESULT_MAX，加上前后缀仍比栈上这块安全
         * （do_run 这条路径的栈深和 sc_chat 那条一样有名）。
         * 早先按 200 给，编译器直接报 -Wformat-truncation —— 那条警告
         * 在"结果文案"上特别容易被忽略，而真被截断时用户看到的
         * 是一句半截的"没成：…"。 */
        static char ev[ACT_RESULT_MAX + 64];
        snprintf(ev, sizeof ev, "%s「%s」：%s",
                 ok ? S(S0254) : S(S0256), actions_verb(g_req.id), msg);
        app_act_event_set(ev);
    }
}

static void do_deny(void)
{
    /* 拒绝也要留痕：只记成功的话，"模型反复试探、每次都被人拦下"
     * 这件事就永远看不见 —— 而那正是最该被看见的模式。 */
    actions_audit("DENY", &g_req, "用户拒绝了");

    /* ★ 拒绝**也要回灌给模型**，否则它会原样再发一遍（在它看来上一轮
     * 什么都没发生）。手册 §3 决策树里那条"被拒绝就换一条路"就是靠这句话
     * 才成立的 —— 这里不写，那句话就是空话。 */
    {
        char line[160];
        snprintf(line, sizeof line,
                 S(S0257),
                 actions_verb(g_req.id));
        store_result(line);
    }

    /* 拒绝也留一条给对话列表：只记成功的话，"它反复试探、每次都被拦下"
     * 这件事在屏幕上就消失了 —— 而那正是最该被看见的模式。 */
    {
        char ev[200];
        snprintf(ev, sizeof ev, S(S0258),
                 actions_verb(g_req.id));
        app_act_event_set(ev);
    }

    app_note("action: %s denied", actions_verb(g_req.id));
    app_pop();
}

/* A 键 / 点「允许」—— **同一个动作**。
 * 抽出来是为了让触摸不可能绕过"上膛"那道校验：
 * 若触摸另写一份，很容易漏掉 actions_needs_double_confirm 的分支，
 * 于是一个不可逆动作点一下就直接执行了。 */
static void do_confirm_key(void)
{
    if (g_focus != 1) {
        do_deny();
        return;
    }
    /* 不可逆动作：第一次只是**上膛**，并把焦点打回"拒绝"。
     * 打回焦点是关键 —— 上膛之后焦点还留在"允许"上的话，
     * 用户顺手再按一下就等于确认生效了。 */
    if (actions_needs_double_confirm(g_req.id) && g_armed < 2) {
        g_armed++;
        if (g_armed == 1) {
            g_focus = 0;
            app_status_set(S(S0259));
            app_note("confirm: armed id=%d（第 1 次，未执行）", g_req.id);
            return;
        }
    }
    do_run();
}

static void confirm_handle(u32 down, u32 held)
{
    (void)held;

    if (g_done || !can_allow()) {
        if (down & (KEY_A | KEY_B)) {
            app_pop();
            return;
        }
        /* 触摸也要能走 —— 这个屏最该能点。闸关着时点按钮只是返回。 */
        {
            int tx, ty;
            if (app_touch_down(&tx, &ty)
                && (w_hit(&CFM_BTN_SINGLE, tx, ty)
                    || w_hit(&CFM_BTN_NO, tx, ty)
                    || w_hit(&CFM_BTN_YES,  tx, ty))) {
                app_pop();
            }
        }
        return;
    }

    if (down & (KEY_LEFT | KEY_RIGHT | KEY_L | KEY_R)) {
        g_focus = g_focus ? 0 : 1;
        /* 焦点被移回"允许"时撤销上膛：中间插一次改主意，
         * 不该让之前的"再按一次"还算数。 */
        if (g_focus == 1)
            g_armed = 0;
        return;
    }

    /* 触摸：左=拒绝（同 B）、右=允许（同 A，含上膛校验）。
     * 语义一律复用 do_deny / do_confirm_key，不另写一套。 */
    {
        int tx, ty;
        if (app_touch_down(&tx, &ty)) {
            if (w_hit(&CFM_BTN_NO, tx, ty)) {
                do_deny();
                return;
            }
            if (w_hit(&CFM_BTN_YES, tx, ty)) {
                g_focus = 1;
                do_confirm_key();
                return;
            }
        }
    }

    if (down & KEY_B) {
        do_deny();
        return;
    }

    if (down & KEY_A) {
        do_confirm_key();
    }
}

const Screen g_screen_confirm = {
    .id            = SCREEN_CONFIRM,
    .name          = "SCREEN_CONFIRM",
    .enter         = confirm_enter,
    .leave         = NULL,
    .render_top    = render_top,
    .render_bottom = render_bottom,
    .handle        = confirm_handle,
};
