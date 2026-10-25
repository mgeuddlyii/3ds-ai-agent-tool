/*
 * sc_content.c -- 内容更新（模型 / 攻略库 / 3DS 硬件知识库）
 *
 * ══ 为什么它和 sc_update.c 是两屏，不是一屏 ═══════════════════════
 *
 * sc_update 装的是**程序本体**：装坏了软件打不开，所以它的交互是
 * 「上屏全部事实 + 下屏只有允许/拒绝」，焦点默认落在**拒绝**上。
 *
 * 这一屏装的是**数据**：装坏了检索还在、界面还在，只是「没有生成」
 * 那一层消失（guide_nn 的失败策略是回退到检索直出）。
 * 既然失败后果轻，交互就可以按内容的特点重排 ——
 * 而内容的特点是**三类各自独立**：
 *
 *   模型   —— 月级更新，影响"生成"这一层
 *   攻略库 —— 周级更新，影响"事实"准确性
 *   硬件库 —— 年级更新，纯加分项
 *
 * 三条更新**不要求同时装**。共用一个"全部安装"按钮会诱使用户
 * 在只想升级攻略库的时候把模型也换掉，而模型换掉意味着生成质量
 * 变了 —— 那是他没要求的副作用。所以这里逐条独立。
 *
 * ══ 交互 ══════════════════════════════════════════════════════════
 *
 *   ↑/↓   选一条（三类内容 + 全部安装/全部回滚两个动作项）
 *   A      选中项的动作。安装/回滚都要**再确认一次**
 *   Y      重新扫描（用户可能刚把包拷进卡里）
 *   B      返回；确认态下 B = 取消，一次只退一层
 *
 * 安装的确认是**内联**的而不是走 SCREEN_CONFIRM —— 后者留给
 * "AI 想做什么"，把用户自己的手动操作塞进去会让两道流程混在一起
 * （理由与 sc_locales.c 的撤销确认相同）。
 */

#include "screen.h"
#include "content_panel.h"

#include "app.h"
#include "cupd.h"
#include "guide_kb.h"
#include "hwk.h"
#include "widgets.h"

#include <stdio.h>
#include <string.h>
#include "uistr.h"

/* 三类内容 + 两个批量动作项。 */
#define ROW_N      (CU_KIND_COUNT + 2)
#define ROW_ALL    CU_KIND_COUNT      /* "全部安装" */
#define ROW_UNDO   (CU_KIND_COUNT + 1)/* "全部回滚" */

#define ROW_H      30.0f

/*
 * 上屏的纵向预算，**量出来的**（不是拍脑袋）：
 *   内容区 26..200 = 174px；五行 × 30 + 中间那条分隔线 8 = 158px；
 *   剩 16px 放一行说明。
 *
 * ⚠️ 行高原来是 34，画完第五行落在 y=202..236 —— **整行在页脚底下**，
 * 于是「全部回滚」这个条目**永远看不见**。
 * 而它没被发现的原因和验收里另一个坑同源：行都画了，
 * 只是画在了画不出来的位置；每一张截图单独看都"正常"。
 * → 判据是"最后一个条目的底边 < 内容区下界"，不是"画了五行"。
 */

/* 一次操作的结果文案。
 *
 * 尺寸必须 >= cu_install / cu_rollback 的 msg 缓冲（224），
 * 而且这里还要前面拼一句「安装失败：」—— 所以给 288。
 * 小了会被 -Wformat-truncation 抓出来，而那警告在这里是真话：
 * 截断之后用户看不到失败原因的后半句（比如「（下次会再装一次）」），
 * 而那半句恰恰是告诉他"要不要重试"的唯一依据。 */
static char g_msg[288];

/* 每条内容的当前状态。装完之后要刷新，所以每次动作都重扫一遍。 */
static CuManifest g_man;
static CuState    g_state;
static char       g_detail[128];
static int        g_avail;        /* cu_scan_local 的返回值：有几条可更新 */
static int        g_sel;
static bool       g_ask;          /* 正在等确认 */
static int        g_done;         /* 最近一次动作做完了（结果态） */

/*
 * 每一类内容有两个可能的动作：装新版、回退到上一份。
 * 用左右键在两者之间切，而不是把 A 定义成「能装就装、不能装就回退」——
 * 后者在"刚装完一个坏版本、同时又有新版本等着"时会出现**无法回退**
 * 的局面（因为"能装"优先），而那正是最需要回退的时候。
 *
 * 只对内容项有意义；两个批量项各自只有一个动作，不受这个开关影响。
 */
static int        g_mode;         /* 0 = 安装，1 = 回滚 */

/* --------------------------------------------------------------------------
 * 每类的「本机版本 / 有没有备份」缓存
 *
 * ── 为什么必须缓存（这是本屏曾经卡到 1fps 的原因）──
 * `cu_local_version()` 内部是 `fopen` + `fscanf` + `fclose`，
 * `cu_has_backup()` 内部是 `file_size()` —— **都是真读 SD 卡**。
 * 而它们被渲染路径每帧调用：row_status 每行一次版本，还**循环遍历所有类型**
 * 查备份；render_top 调 row_status 若干次，render_bottom 又经 action_label
 * 再来一遍。合计**每帧十几次 SD 访问**。
 *
 * SD 单次访问是几十到上百毫秒 → 实测这一屏只有 **1.2 fps**
 * （主界面约 33fps）。用户看到的是"进这屏就没反应/像卡死"。
 * 2026-10-05 定位：boot.log 里 81 秒只走了 101 帧。
 *
 * ⚠️ 这与 sc_home 的 cred_any() 是**同一类错误**：把"读文件"放进了渲染期。
 * 规矩：**渲染函数里不许出现 fopen / file_size 这类调用**，
 * 要用的数据一律在进屏或动作之后算好放静态区。
 *
 * 值只会在 install / rollback / 重扫之后变，所以刷新点很少（见 refresh_cache）。
 * ----------------------------------------------------------------------- */
static int  g_ver[CU_KIND_COUNT];   /* 每类的本机版本 */
static bool g_bak[CU_KIND_COUNT];   /* 每类有没有可回滚的备份 */

static void refresh_cache(void)
{
    for (int i = 0; i < CU_KIND_COUNT; ++i) {
        g_ver[i] = cu_local_version((CuKind)i);
        g_bak[i] = cu_has_backup((CuKind)i);
    }
}

/* 把「本机版本 / 包里版本 / 能不能装」拼成一行给人看。
 * 放函数里而不是内联，是因为 render_top 与 render_bottom 都要用，
 * 而两处拼法不一致的话用户会看到"上屏说能装、下屏按钮是灰的"。 */
static void row_status(int row, char *buf, int cap, RColor *color)
{
    if (row < CU_KIND_COUNT) {
        int cur = g_ver[row];   /* 缓存值：渲染期不许读盘，见 refresh_cache */
        /* ★ 按种类查下标（B-08）：manifest 是子集或乱序时，
         *   row 直接当数组下标会读到别的产物。 */
        int ai = cu_index_of_kind(&g_man, (CuKind)row);
        bool has = (g_avail > 0 && ai >= 0 &&
                    g_man.art[ai].version > cur);

        if (g_state == CU_VERIFY_OK && has) {
            snprintf(buf, cap, S(S0260), cur,
                     g_man.art[ai].version);
            *color = w_c(C_SLOT_OK);
        } else if (ai >= 0 && g_man.art[ai].version > cur) {
            /* 包在但校验没过 —— 显示版本差，但**不给绿色**。
             * 绿色的含义是"按了就能装"，这里按了会被拒。 */
            snprintf(buf, cap, S(S0261), cur,
                     g_man.art[ai].version);
            *color = w_c(C_SLOT_WARN);
        } else {
            snprintf(buf, cap, S(S0262), cur);
            *color = w_c(C_SLOT_MUTED);
        }
        return;
    }

    if (row == ROW_ALL) {
        if (g_avail > 0 && g_state == CU_VERIFY_OK) {
            snprintf(buf, cap, S(S0263), g_avail);
            *color = w_c(C_SLOT_OK);
        } else if (g_avail > 0) {
            snprintf(buf, cap, S(S0264), g_avail);
            *color = w_c(C_SLOT_WARN);
        } else {
            snprintf(buf, cap, "%s", S(S0265));
            *color = w_c(C_SLOT_FAINT);
        }
        return;
    }

    {
        int n = 0, i;
        for (i = 0; i < CU_KIND_COUNT; i++)
            if (g_bak[i]) n++;
        if (n > 0) {
            snprintf(buf, cap, S(S0266), n);
            *color = w_c(C_SLOT_ACCENT);
        } else {
            snprintf(buf, cap, "%s", S(S0267));
            *color = w_c(C_SLOT_FAINT);
        }
    }
}

/* 选中的那一项现在能不能按 A。没理由就返回 NULL。 */
static const char *action_label(int row, bool *enabled)
{
    if (row < CU_KIND_COUNT) {
        int cur = g_ver[row];   /* 缓存值：渲染期不许读盘，见 refresh_cache */

        if (g_mode == 0) {
            {
                int ai = cu_index_of_kind(&g_man, (CuKind)row);
                *enabled = (g_avail > 0 && g_state == CU_VERIFY_OK &&
                            ai >= 0 && g_man.art[ai].version > cur);
            }
            return *enabled ? S(S0268) : S(S0269);
        }
        *enabled = g_bak[row];
        return *enabled ? S(S0270) : S(S0271);
    }
    if (row == ROW_ALL) {
        *enabled = (g_avail > 0 && g_state == CU_VERIFY_OK);
        return *enabled ? S(S0272) : S(S0273);
    }
    *enabled = false;
    for (int i = 0; i < CU_KIND_COUNT; i++)
        if (g_bak[i]) { *enabled = true; break; }
    return *enabled ? S(S0274) : S(S0271);
}

/* 当前项在安装/回滚之间切换后，状态列该显示什么。
 *
 * 与 row_status 的"版本差"是**两套东西**：那一列回答"卡上现在是什么"，
 * 这一列回答"按 A 会发生什么"。合成一列的话，用户会看到
 * 「v2 → v3 待校验」却不知道按 A 是升级还是回退。 */
static void mode_hint(char *buf, int cap)
{
    if (g_sel < CU_KIND_COUNT) {
        snprintf(buf, cap, "A = %s",
                 g_mode == 0 ? S(S0268) : S(S0270));
    } else if (g_sel == ROW_ALL) {
        snprintf(buf, cap, "%s", S(S0275));
    } else {
        snprintf(buf, cap, "%s", S(S0276));
    }
}

static void rescan(void)
{
    g_avail = cu_scan_local(&g_man, &g_state, g_detail, sizeof g_detail);
    /* 顺手把"本机版本/有无备份"也刷一遍。**这里是唯一的刷新点** ——
     * 渲染期只读缓存（见 g_ver/g_bak 的说明）。
     * 装完/回退完之后各自会再调一次 rescan，所以那条路也覆盖到了。 */
    refresh_cache();
    app_note("content: scan avail=%d state=%s", g_avail,
             cu_state_text(g_state));
}

void cu_panel_enter(void)
{
    g_sel  = 0;
    g_ask  = false;
    g_done = false;
    g_mode = 0;          /* 默认先看安装；回退要多按一次左右，够防误触 */
    g_msg[0] = '\0';
    rescan();
    app_status_set("%s", cu_state_text(g_state));
}

/* --------------------------------------------------------------------------
 * 上屏：三类内容逐条列出，带版本与可执行状态
 * ----------------------------------------------------------------------- */

void cu_panel_render_top(void)
{
    const float x = (float)W_TOP_MARGIN;
    const float w = (float)W_TOP_USABLE;
    float y = (float)W_TOP_CONTENT_Y;
    char  line[96];
    RColor col;

    /* 标题用「内容包」而不是原来的「内容更新」：它现在是软件更新里的一个页，
     * 「内容更新」这个名字会和「软件更新」在概念上打架（用户就为这个点错过）。 */
    w_top_header(S(S0973), g_state == CU_VERIFY_OK ? S(S0278) : S(S0279));

    /* 整体状态**不单独占一行**：它与下面三行的状态列讲的是同一件事，
     * 单独写一遍就成了「卡上没有内容更新包」出现两次 ——
     * 第一次在页眉下，第二次在五行之后。重复的文案用户会读成
     * "出了两件事"，而其实只是一件。整体状态放在**页脚**（app_status）里，
     * 那里本来就在显示它。
     *
     * 腾出来的 22px 正是「全部回滚」那一行从页脚底下露出来所需要的。 */
    y += 2;
    w_hr(x, y, w, w_c(C_SLOT_LINE));
    y += 6;

    for (int row = 0; row < ROW_N; ++row) {
        bool  cur = (row == g_sel);

        if (cur)
            w_panel(&(Rect){ x - 2, y - 3, w + 4, ROW_H, 0 },
                    w_c(C_SLOT_ACC_LT), w_c(C_SLOT_ACCENT));

        /* 名称。内容项用 cu_kind_name()，动作项自己写 ——
         * 混用两套来源的话，改了其中一个另一个会静默停在旧文案。 */
        if (row < CU_KIND_COUNT)
            snprintf(line, sizeof line, "%s", cu_kind_name((CuKind)row));
        else
            snprintf(line, sizeof line, "%s",
                     row == ROW_ALL ? S(S0272) : S(S0280));

        w_line(x + 2, y, w - 150, line,
               cur ? w_c(C_SLOT_ACC_DK) : w_c(C_SLOT_FG));

        row_status(row, line, sizeof line, &col);
        w_line(x + w - 146, y, 144, line, col);

        y += ROW_H;
        if (row == CU_KIND_COUNT - 1) {
            /* 三类内容与两个动作项之间画一条线：它们不是一类东西，
             * 混在一列里用户会以为"全部安装"也是一条内容。 */
            y += 2;
            w_hr(x, y, w, w_c(C_SLOT_LINE));
            y += 6;
        }
    }

    /*
     * 装不了时的原因**画在页脚那一行里**，不单独占一行。
     *
     * 量过的：五行画完落在 y=192，而内容区下界是 200 ——
     * 剩 8px，而一行文字要 18px。所以再放任何一行都会**沉到页脚底下**
     * （第一版就是这样：g_detail 画在 194，页脚正好从 200 开始，
     * 于是它和页脚的按键提示糊在一起，两行字叠着显示）。
     *
     * 为什么当初没算：只按"5 行 × 30 + 分隔 8 = 158，加起来 192 < 200"
     * 确认放得下，**没意识到页脚本身也占 200..240**。
     * → 判据是"最后一行文字的**底边** + 行高 <= 200"，
     * 不是"最后一个元素的 y <= 200"。
     */
    w_top_footer(app_status(),
                 g_ask ? S(S0231)
                       : (g_state == CU_VERIFY_OK
                          ? S(S0281)
                          : g_detail));

    /* 纵向溢出自检。**每次渲染都调**（不进条件）——
     * 只在"觉得可能有问题"时才调，等于把它变成一个可选的检查，
     * 而这一类 bug 恰恰是在人们觉得没问题的时候发生的。 */
    w_vbounds((int)(y + 2.0f), W_TOP_CONTENT_B);
}

/* --------------------------------------------------------------------------
 * 下屏：确认 / 说明
 * ----------------------------------------------------------------------- */

/* 按钮矩形（render 与 handle 共用，避免"看到的位置点不到"）。 */
static const Rect CON_BTN_NO   = {  10, 150, 144, 36, 0 };
static const Rect CON_BTN_YES  = { 166, 150, 144, 36, 0 };
static const Rect CON_BTN_MAIN = {  10, 150, 200, 36, 0 };

void cu_panel_render_bottom(void)
{
    const float x = (float)W_BOT_MARGIN;
    const float w = (float)W_BOT_USABLE;
    float y = (float)W_BOT_CONTENT_Y;
    bool  enabled = false;
    const char *label = action_label(g_sel, &enabled);

    if (g_ask) {
        char l[96];

        if (g_sel < CU_KIND_COUNT) {
            if (g_mode == 0)
                snprintf(l, sizeof l, S(S0282),
                         cu_kind_name((CuKind)g_sel));
            else
                snprintf(l, sizeof l, S(S0283),
                         cu_kind_name((CuKind)g_sel));
        } else if (g_sel == ROW_ALL) {
            snprintf(l, sizeof l, S(S0284), g_avail);
        } else {
            snprintf(l, sizeof l, "%s", S(S0285));
        }

        y = w_wrap(x, y, w, l, w_c(C_SLOT_FG), TEXT_LINE_H);
        y += 4;

        if (g_sel == ROW_UNDO || (g_sel < CU_KIND_COUNT && g_mode == 1)) {
            w_wrap(x, y, w,
                   S(S0286),
                   w_c(C_SLOT_MUTED), TEXT_LINE_H);
        } else {
            w_wrap(x, y, w,
                   S(S0287),
                   w_c(C_SLOT_MUTED), TEXT_LINE_H);
        }

        w_button(&CON_BTN_NO,   S(S0288), W_BTN_NORMAL);
        w_button(&CON_BTN_YES,  S(S0289), W_BTN_SELECTED);
        w_center((float)R_BOT_W / 2.0f, 192, S(S0290),
                 w_c(C_SLOT_FAINT));
        w_bot_footer(S(S0290));
        return;
    }

    if (g_msg[0]) {
        y = w_wrap(x, y, w, g_msg,
                   g_done ? w_c(C_SLOT_OK) : w_c(C_SLOT_WARN), TEXT_LINE_H);
    } else {
        /* 当前模式**写在说明第一行**，因为它决定 A 键会做什么。
         * 只靠按钮上的标签（"安装这一类"/"回滚这一类"）不够 ——
         * 那个标签在按钮上，而用户的眼睛往往先落在说明上。 */
        char hint[48];
        mode_hint(hint, sizeof hint);
        y = w_wrap(x, y, w, hint, w_c(C_SLOT_ACC_DK), TEXT_LINE_H);
    }

    /*
     * 「更新包放这里」的面板。**故意放这里而不是上屏**：
     * 上屏五行画到 192，内容区下界 200，**一点余量都没有**。
     *
     * ⚠️ 面板的高度是量出来的，不是估的：这里能给它的空间是
     * 说明文字结束处到按钮顶（150）之间。第一版说明写了三行（到 124），
     * 面板却放在 108 —— 于是它**压在说明文字上面**，
     * 截图里能直接看到「任何一类装坏了都不影响别的」被面板盖掉半行。
     * 判据：**面板 y >= 说明结束 y**，不是"看起来差不多"。
     *
     * 所以说明文字压到**一行**（mode_hint 那行），
     * 面板放在 96..132，与按钮（150 起）留 18px 间隙。
     */
    if (g_state == CU_NO_PACKAGE || g_state == CU_UP_TO_DATE) {
        w_panel(&(Rect){ x, 96.0f, w, 36, 0 }, w_c(C_SLOT_ACC_LT),
                w_c(C_SLOT_ACCENT));
        r_text(&(Rect){ x + 10, 99, w - 20, 16, 0 },
               S(S0291), w_c(C_SLOT_ACC_DK));
        r_text(&(Rect){ x + 10, 115, w - 20, 16, 0 },
               cu_dir(), w_c(C_SLOT_FG));
    }

    y = 150.0f;
    /* 不可用时画成灰的。**不是**"按了没反应"：一个按不动的按钮
     * 会让用户以为自己按错了，然后反复按。灰掉本身就是答案。 */
    w_button(&CON_BTN_MAIN, label,
             enabled ? W_BTN_PRIMARY : W_BTN_DISABLED);
    w_vbounds(186, W_BOT_CONTENT_B);

    /* ★ 2026-10-25：这里原来还有一行居中的键位提示（S0292/S0293），
     *   和下面页脚现在带的那行**说的是同一批键**，屏上就是两行重复的提示。
     *   页脚必须带「L/R 程序」（不然用户不知道怎么回程序页），
     *   所以让这条居中的让位 —— 一个位置只说一遍。 */
    /* 两个分支都要带「L/R 程序」：面板现在是软件更新的第二页，
     * 没有这一句用户就只知道怎么操作内容包，不知道怎么回程序页。
     * 原来禁用态用的是 S0295（只有 ↑↓/Y/B），进门就看到空状态的人
     * 恰好最需要"怎么回去"这句提示。 */
    w_bot_footer(enabled ? S(S0976) : S(S0977));
}

/* --------------------------------------------------------------------------
 * 行为
 * ----------------------------------------------------------------------- */

static void do_one(int kind)
{
    char msg[224];
    int idx = cu_index_of_kind(&g_man, (CuKind)kind);

    /* 包里没有这一类 → 直接说，不要让 cu_install 去报"下标不对"
     * （那句话对用户毫无意义，而且看起来像程序坏了）。 */
    if (idx < 0) {
        g_done = false;
        snprintf(g_msg, sizeof g_msg, "卡上的更新包里没有「%s」这一类",
                 cu_kind_name((CuKind)kind));
        return;
    }

    if (cu_install(&g_man, idx, msg, sizeof msg)) {
        g_done = true;
        snprintf(g_msg, sizeof g_msg, "%s", msg);
        app_note("content: installed kind=%s at %d",
                 cu_kind_name((CuKind)kind), idx);
    } else {
        g_done = false;
        snprintf(g_msg, sizeof g_msg, S(S0296), msg);
        app_note("content: install failed kind=%s msg=%s",
                 cu_kind_name((CuKind)kind), msg);
    }
    rescan();
}

static void do_rollback(int kind)
{
    char msg[224];

    if (cu_rollback(&g_man, kind, msg, sizeof msg)) {
        g_done = true;
        snprintf(g_msg, sizeof g_msg, "%s", msg);
        app_note("content: rolled back kind=%s", cu_kind_name((CuKind)kind));
    } else {
        g_done = false;
        snprintf(g_msg, sizeof g_msg, S(S0297), msg);
        app_note("content: rollback failed kind=%s msg=%s",
                 cu_kind_name((CuKind)kind), msg);
    }
    /* 回滚把版本号归零了，所以必须重扫 —— 不重扫的话屏上还是
     * 回滚前那个"可安装"，而实际已经变了。 */
    rescan();
}

/*
 * 真正执行。分成"单条"和"全部"两种形态，但**没有共用一条循环**：
 * 全部安装走的是「每条各自装、失败不中断」的语义
 * —— 三条内容彼此独立，第一条坏了不该让后两条也装不上。
 * 而单条安装失败就是失败，没有"继续"这个概念。
 */
static void do_action(void)
{
    if (g_sel < CU_KIND_COUNT) {
        /* 单条：按当前模式走安装或回滚。两条各有各的函数，
         * **没有合并成一条带 if 的路径** —— 合并之后
         * "安装完要不要顺便备份"这类差异就会被抹掉，
         * 而那正是两条路径最容易出错的地方。 */
        if (g_mode == 0)
            do_one(g_sel);
        else
            do_rollback(g_sel);
    } else if (g_sel == ROW_ALL) {
        int ok = 0, bad = 0, i;
        char last[224];

        last[0] = '\0';
        /* ⚠️ 装几条就只报最后一条的结果，屏上只有一行放得下。
         * 所以这里明确报"成功几条/失败几条"，不假装只有一条。 */
        for (i = 0; i < CU_KIND_COUNT && i < g_man.count; i++) {
            char msg[224];
            if (cu_install(&g_man, i, msg, sizeof msg)) {
                ok++;
            } else {
                bad++;
                snprintf(last, sizeof last, "%s", msg);
            }
        }
        rescan();
        if (bad == 0) {
            g_done = true;
            snprintf(g_msg, sizeof g_msg, S(S0298), ok);
        } else {
            g_done = false;
            snprintf(g_msg, sizeof g_msg,
                     S(S0299), ok, bad, last);
        }
        app_note("content: install all ok=%d bad=%d", ok, bad);
    } else {
        int ok = 0, bad = 0, i;
        char last[224];

        last[0] = '\0';
        for (i = 0; i < CU_KIND_COUNT; i++) {
            char msg[224];
            if (!cu_has_backup((CuKind)i))
                continue;
            if (cu_rollback(&g_man, i, msg, sizeof msg)) {
                ok++;
            } else {
                bad++;
                snprintf(last, sizeof last, "%s", msg);
            }
        }
        rescan();
        if (ok == 0 && bad == 0) {
            g_done = false;
            snprintf(g_msg, sizeof g_msg, "%s", S(S0267));
        } else if (bad == 0) {
            g_done = true;
            snprintf(g_msg, sizeof g_msg, S(S0300), ok);
        } else {
            g_done = false;
            snprintf(g_msg, sizeof g_msg,
                     S(S0301), ok, bad, last);
        }
        app_note("content: rollback all ok=%d bad=%d", ok, bad);
    }
    /* ⚠️ 批量路径**直接调 cu_install / cu_rollback**，不走 do_one / do_rollback
     *（那两个函数末尾各有一次 rescan），所以这里必须自己刷缓存。
     * 不刷的话版本确实变了、屏上却还显示旧的 —— 表现为"装完了没反应"，
     * 而用户会去重装一遍。 */
    refresh_cache();
    app_status_set("%s", cu_state_text(g_state));
}

void cu_panel_handle(u32 down, u32 held)
{
    (void)held;

    if (down & KEY_B) {
        if (g_ask) {
            g_ask = false;      /* 一次 B 只退一层 */
            return;
        }
        app_pop();
        return;
    }

    if (g_ask) {
        /* 触摸：左=取消（同 B）、右=确认（同 A）。复用既有动作。 */
        {
            int tx, ty;
            if (app_touch_down(&tx, &ty)) {
                if (w_hit(&CON_BTN_NO, tx, ty)) {
                    g_ask = false;
                    return;
                }
                if (w_hit(&CON_BTN_YES, tx, ty))
                    down |= KEY_A;
            }
        }
        if (down & KEY_A) {
            g_ask = false;
            do_action();
        }
        return;
    }

    /* 触摸主按钮 = A。灰掉时也走这条 —— A 的分支会给出"为什么不能装"，
     * 这比"点了没反应"好。 */
    {
        int tx, ty;
        if (app_touch_down(&tx, &ty) && w_hit(&CON_BTN_MAIN, tx, ty))
            down |= KEY_A;
    }

    if (down & KEY_Y) {
        g_msg[0] = '\0';
        g_done = false;
        rescan();
        app_status_set("%s", cu_state_text(g_state));
        return;
    }

    if (down & KEY_UP) {
        g_sel = (g_sel + ROW_N - 1) % ROW_N;
        g_msg[0] = '\0';
        return;
    }
    if (down & KEY_DOWN) {
        g_sel = (g_sel + 1) % ROW_N;
        g_msg[0] = '\0';
        return;
    }

    /* 左右：只在内容项上切装/退。批量项只有一个动作，
     * 在它们上面按左右会让用户以为"全部安装"也有两个模式 ——
     * 而它没有，���了只是没反应。 */
    if (down & (KEY_LEFT | KEY_RIGHT)) {
        if (g_sel < CU_KIND_COUNT) {
            g_mode = g_mode ? 0 : 1;
            g_msg[0] = '\0';
            g_done = false;
        }
        return;
    }

    if (down & KEY_A) {
        bool enabled = false;
        action_label(g_sel, &enabled);
        if (!enabled) {
            /* 不弹确认，直接说原因。按了没反应是最差的交互。 */
            g_done = false;
            if (g_sel == ROW_UNDO)
                snprintf(g_msg, sizeof g_msg, "%s", S(S0302));
            else if (g_sel < CU_KIND_COUNT && g_mode == 1)
                snprintf(g_msg, sizeof g_msg,
                         S(S0303),
                         cu_kind_name((CuKind)g_sel));
            else if (g_sel < CU_KIND_COUNT)
                snprintf(g_msg, sizeof g_msg,
                         S(S0304),
                         cu_kind_name((CuKind)g_sel));
            else if (g_state != CU_VERIFY_OK)
                snprintf(g_msg, sizeof g_msg, S(S0305), g_detail);
            else
                snprintf(g_msg, sizeof g_msg, "%s", S(S0306));
            return;
        }
        g_ask = true;
        g_msg[0] = '\0';
    }
}

/* ★ 2026-10-25：Screen 注册删掉了 —— 这屏不再是独立屏，
 *   而是「软件更新」里的一个页（见 content_panel.h）。
 *   删掉注册后 screen.h 的 SCREEN_CONTENT 枚举与 app.c 的表项也一起删，
 *   否则表里会留一个指向不存在对象的空位。 */
