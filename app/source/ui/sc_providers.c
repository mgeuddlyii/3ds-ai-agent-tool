/*
 * sc_providers.c -- 服务目录：在这里切换豆包 / 其他 API
 *
 * ── 这一屏补的是什么洞 ──
 * 在它出现之前，**UI 里没有任何地方能选服务商**（全项目搜 provider_count
 * 只有这一屏有用到的静态表）。服务商只能靠手改卡上 key.txt 里的
 * `provider=doubao` 再导入 —— 那不是给用户用的路。
 *
 * ── 为什么"选一家"和"配它的凭据"要合成一个动作 ──
 * 这两件事在用户脑子里本来就是一件事："我要用豆包"。拆成两步的话，
 * 用户会选完豆包就直接去对话，然后在对话屏被"缺凭据"挡住 ——
 * 而那时候他已经不在这一屏了，提示里那句"去配置"还得他自己找路回来。
 * 所以按 A 就是**一条路走到底**：切过去；如果这家还没凭据，顺手带你过去配。
 * 已经配好的就只切不跳，免得每次都多弹一屏。
 *
 * ── 为什么凭据状态要分三种而不是两种 ──
 * "有没有凭据"和"凭据能不能用"是两件事，而这个 app 里恰好第三种情况很常见：
 *   · 豆包根本不用 Key，用登录态（所以不能报"缺 Key"）
 *   · 别的服务商在 Key 仓库里按 provider 字段各自存档
 *   · 演示 provider 压根不需要凭据
 * 混成"有没有 Key"一种判断，用户会在豆包那一行看到"缺 Key"而不知道该干嘛。
 */

#include "screen.h"

#include "app.h"
#include "doubao.h"
#include "input.h"
#include "keystore.h"
#include "provider.h"
#include "widgets.h"

#include <stdio.h>
#include <string.h>
#include "uistr.h"

static int  g_sel;
static char g_msg[192];       /* 上一次操作的结果 */
static bool g_msg_warn;       /* true = 这条结果是"被拒了"，画成警示色。
                               * 用颜色区分"成了/没成"是必要的：两句文案
                               * 都走同一个位置，光看字得读完整句才知道结果。 */

/* 清结果。两件事必须**一起**做 —— 只清文字不清颜色的话，
 * 下一条成功文案会继承上一次的警示色（"改成了"画成红的）。 */
static void clear_msg(void)
{
    g_msg[0]   = '\0';
    g_msg_warn = false;
}

#define ROW_H    30.0f
/* ★ 可见行数 = 5，不是 6（2026-10-16 修）。
 *
 * 每行实际占 30px（名字 0..16、状态 14..30 两行叠着放），而内容区只有
 * 26..200 共 174px。放 6 行的话第 6 行从 176 起 —— 名字 176..192、
 * 状态 190..206，**状态那行整条压到页脚上**，用户看到的是半个字。
 * 6 × 30 = 180 > 174，本来就放不下，这一屏从加上第 7 家（custom）
 * 之后就一直是这样，只是没人截图看过第 6 行。
 *
 * 放 5 行：26..176，最后一行状态 160..176，离下界还有 24px。
 * 剩下的家靠滚动（下面的"共 N 家，显示第 a-b 家"就是为这个准备的）。 */
#define ROW_MAX  5

/* 「网址」这一行能不能改：只有 custom 能。
 *
 * 内置那几家的地址是**编译期常量**（provider.h 开头写了理由：地址可改
 * 就等于让配置文件决定把你的 Key 发到哪去）。所以这里不去放宽它，
 * 而是把话说明白 —— 按了没反应、或者弹了键盘再拒绝，都会被当成 bug。 */
static bool prov_url_editable(const ProviderDef *p)
{
    return p && strcmp(p->name, "custom") == 0;
}

/* 这一家现在是什么状态。返回显示用的颜色槽。
 *
 * 注意 **豆包那一支必须先判** —— 它的 auth 是 "cookie"，
 * 走"查 Key 仓库"那条路会永远得到"缺 Key"，而它压根不需要 Key。 */
static RColor prov_state(const ProviderDef *p, char *out, size_t cap)
{
    /* ★ 判据是 offline 字段，不是"网址为空" —— 自定义服务商没填网址时也是
     * 空串，以前会被这一屏显示成"离线演示 · 不用任何凭据"，
     * 用户就以为配好了。现在自定义项会往下走，正常显示"缺 Key/缺网址"。 */
    if (p->offline) {
        snprintf(out, cap, S(S0503));
        return w_c(C_SLOT_OK);
    }

    if (p->fmt == PROVIDER_FMT_DOUBAO) {
        if (db_load_cookie() && db_has_cookie()) {
            snprintf(out, cap, S(S0504),
                     db_cookie_count());
            return w_c(C_SLOT_OK);
        }
        snprintf(out, cap, "%s", S(S0505));
        return w_c(C_SLOT_WARN);
    }

    /* 别的服务商：Key 仓库里按 provider 字段找。
     * 不用 keystore_active() —— 那是"当前激活的那一把"，
     * 在这一屏会变成"只要有一把 Key，所有服务商都显示已配"。 */
    for (int i = 0; i < keystore_count(); ++i) {
        const KeyEntry *e = keystore_at(i);
        if (e && e->key[0] && strcmp(e->provider, p->name) == 0) {
            snprintf(out, cap, S(S0506), e->name);
            return w_c(C_SLOT_OK);
        }
    }
    snprintf(out, cap, "%s", S(S0507));
    return w_c(C_SLOT_WARN);
}

/* 这家现在能不能直接用（有凭据）。 */
static bool prov_ready(const ProviderDef *p)
{
    char tmp[96];

    if (p->offline)
        return true;                    /* 演示不用凭据 */
    if (p->fmt == PROVIDER_FMT_DOUBAO)
        return db_load_cookie() && db_has_cookie();
    for (int i = 0; i < keystore_count(); ++i) {
        const KeyEntry *e = keystore_at(i);
        if (e && e->key[0] && strcmp(e->provider, p->name) == 0)
            return true;
    }
    (void)tmp;
    return false;
}

/* -------------------------------------------------------------------------- */

static void providers_enter(void)
{
    const AppConfig *c = app_cfg();

    clear_msg();
    keystore_load();

    /* 进来时选中"正在用的那家"，而不是第一行 —— 用户多半是来看
     * "我现在用的是哪个"的，直接停在那儿最省事。 */
    g_sel = 0;
    for (int i = 0; i < provider_count(); ++i) {
        const ProviderDef *p = provider_at(i);
        if (p && strcmp(p->name, c->ai.provider) == 0) {
            g_sel = i;
            break;
        }
    }
    app_status_set(S(S0508), provider_count());
}

/* 切换服务商。缺凭据就顺手带过去配。 */
static void do_pick(void)
{
    const ProviderDef *p = provider_at(g_sel);
    AppConfig         *c = app_cfg();

    if (!p)
        return;

    snprintf(c->ai.provider, sizeof c->ai.provider, "%s", p->name);

    /* model 也必须跟着换。留着上一家的模型名，请求就会带着不相干的
     * model 发出去 —— 切到豆包却发 glm-4-flash 就是这么来的。
     * default_model 为空的 provider 用自己的名字兜底。 */
    snprintf(c->ai.model, sizeof c->ai.model, "%s",
             p->default_model[0] ? p->default_model : p->name);

    /* Key 仓库的"当前激活"也要跟着换。
     *
     * 原因：对话屏进屏时的来源优先级是「keystore 激活条目 > key.txt >
     * config」（sc_chat.c 的 chat_enter）。**只要仓库里还激活着别家的
     * 一把 Key，用户在这里选的那家就会被悄悄盖掉** —— 症状是"我在服务
     * 目录选了豆包，回对话屏还是 DeepSeek"，而 config 里明明写着 doubao。
     * 豆包不用 Key、仓库里没有它的条目，所以那种情况不是"切激活"而是
     * "清激活"（见 keystore.h 里 keystore_clear_active 的说明）。 */
    {
        bool matched = false;

        for (int i = 0; i < keystore_count(); ++i) {
            const KeyEntry *e = keystore_at(i);
            if (e && e->key[0] && strcmp(e->provider, p->name) == 0) {
                keystore_set_active(e->name);
                matched = true;
                break;
            }
        }
        if (!matched)
            keystore_clear_active();
    }

    app_cfg_save_now();
    app_note("providers: 切到 %s（model=%s）", p->name, c->ai.model);

    if (!prov_ready(p)) {
        /* 还没凭据：切过去的同时把人送到配置的地方。
         * 这一步是这一屏存在的意义 —— 不送过去的话，用户回到对话屏
         * 只会看到一句"缺凭据"，而那一屏并没有告诉他去哪。 */
        snprintf(g_msg, sizeof g_msg,
                 S(S0509), p->name);
        if (p->fmt == PROVIDER_FMT_DOUBAO) {
            app_push(SCREEN_DOUBAO);
        } else {
            app_push(SCREEN_KEYS);
        }
        return;
    }

    snprintf(g_msg, sizeof g_msg,
             S(S0510), p->name);
    app_status_set(S(S0511), p->name);
}

/* --------------------------------------------------------------------------
 * 改模型 / 改网址
 *
 * ★ 为什么只允许改**当前在用**的那一家：这一屏的行是"选谁"，而模型和网址
 *   属于"那一家怎么连"。光标停在一家、改的却是另一家，用户根本看不出来
 *   改的是谁（屏上没有任何地方会显示"你刚改的是 zhipu"）。
 *   要求先按 A 切过去，这个约束是**可见**的 —— 屏上会说明。
 * ----------------------------------------------------------------------- */

/* 判断能不能对光标这一家动手。不能就写好 g_msg 并返回 false。 */
static bool edit_guard(const ProviderDef *p, const char *what)
{
    AppConfig *c = app_cfg();

    if (!p)
        return false;

    if (strcmp(p->name, c->ai.provider) != 0) {
        snprintf(g_msg, sizeof g_msg, S(S0512), what);
        g_msg_warn = true;
        /* 这一行 note 是**给验收看的**：它让"按了 X/Y 走到了守卫"
         * 变成能从 status.txt 读出来的事实。没有它的话，这一趟只能靠
         * 对比截图判断，而截图差异的来源太多（状态行、闪烁帧）。 */
        app_note("providers: edit blocked (sel=%s cur=%s what=%s)",
                 p->name, c->ai.provider, what);
        return false;
    }
    return true;
}

static void do_edit_model(void)
{
    const ProviderDef *p = provider_at(g_sel);
    AppConfig         *c = app_cfg();
    char               buf[48];

    clear_msg();
    if (!edit_guard(p, S(S0380)))
        return;

    if (!ui_input_text(S(S0400), c->ai.model, 40,
                       buf, sizeof buf)) {
        app_status_set(S(S0403));
        return;
    }

    snprintf(c->ai.model, sizeof c->ai.model, "%s",
             buf[0] ? buf : (p->default_model[0] ? p->default_model : p->name));
    app_cfg_dirty();
    app_cfg_save_now();
    app_note("providers: model -> %s (provider=%s)", c->ai.model, p->name);
    snprintf(g_msg, sizeof g_msg, S(S0513), c->ai.model);
    app_status_set(S(S0401), c->ai.model);
}

static void do_edit_url(void)
{
    const ProviderDef *p = provider_at(g_sel);
    AppConfig         *c = app_cfg();
    char               buf[96];

    clear_msg();
    if (!edit_guard(p, S(S0381)))
        return;

    /* 非自定义：**不弹键盘**，直接把"为什么改不了 + 怎么才能改"说出来。
     *
     * ⚠️ 这里**不报出它现在的地址**：内置表格里最长的一条有 47 字符
     *    （dashscope 的 compatible-mode/v1），光它自己就折两行，
     *    加上说明一共四行 —— 底屏那块位置（约三行）放不下，
     *    结果就是文字压到按钮上。地址不可改，报它也没有可操作性。 */
    if (!prov_url_editable(p)) {
        snprintf(g_msg, sizeof g_msg, S(S0800));
        g_msg_warn = true;
        app_status_set(S(S0515));
        app_note("providers: url edit refused (builtin %s)", p->name);
        return;
    }

    if (!ui_input_text(S(S0406),
                       c->ai.base_url, 90, buf, sizeof buf)) {
        app_status_set(S(S0408));
        return;
    }

    if (!buf[0]) {
        /* 空网址的后果是**静默**的：请求发不出去（provider.h 里那段
         * "空网址陷阱"就是同一个东西换了条路进来），所以这里直接拦住。 */
        snprintf(g_msg, sizeof g_msg, "%s", S(S0516));
        g_msg_warn = true;
        app_status_set(S(S0517));
        return;
    }

    snprintf(c->ai.base_url, sizeof c->ai.base_url, "%s", buf);
    app_cfg_dirty();
    app_cfg_save_now();
    app_note("providers: base_url -> len=%d (custom)", (int)strlen(c->ai.base_url));
    snprintf(g_msg, sizeof g_msg, S(S0518), (int)strlen(buf));
    app_status_set(S(S0519));
}

/* --------------------------------------------------------------------------
 * 渲染
 * ----------------------------------------------------------------------- */

static void render_top(void)
{
    const float x = (float)W_TOP_MARGIN;
    const float w = (float)W_TOP_USABLE;
    float       y = (float)W_TOP_CONTENT_Y;
    const char *cur = app_cfg()->ai.provider;
    int         first = 0;

    w_top_header(S(S0368), S(S0520));

    if (provider_count() == 0) {
        w_wrap(x, y, w, S(S0521),
               w_c(C_SLOT_WARN), TEXT_LINE_H);
        w_top_footer(app_status(), "");
        return;
    }

    /* 滚动窗口。表现在只有 6 家，但"现在放得下"不是不写滚动的理由：
     * 不写的话，将来加到第 7 家就永远选不到，而界面看着一切正常。 */
    if (g_sel >= ROW_MAX)
        first = g_sel - ROW_MAX + 1;

    for (int i = first; i < provider_count() && i < first + ROW_MAX; ++i) {
        const ProviderDef *p = provider_at(i);
        bool  sel = (i == g_sel);
        bool  on  = (strcmp(p->name, cur) == 0);
        char  st[96];

        if (!p)
            continue;

        if (sel)
            /* ★ 面板高度必须**罩住两行文字**（名字 0..16、状态 14..30）。
             *   原来是 ROW_H-4 = 26，只到 y+23 —— 状态那行有 7px 露在
             *   高亮框外面，看着就是"字飘出格子"。现在给 ROW_H+2 = 32
             *   （y-2 .. y+30），上下各留 2px。 */
            w_panel(&(Rect){ x - 4, y - 2, w + 8, ROW_H + 2, 0 },
                    w_c(C_SLOT_ACC_LT), w_c(C_SLOT_ACCENT));

        /* 名字行：[● 在用] 名字 */
        {
            char nm[64];
            snprintf(nm, sizeof nm, "%s%s", on ? "● " : "  ", p->name);
            w_line(x, y, 120, nm,
                   sel ? w_c(C_SLOT_ACC_DK) : (on ? w_c(C_SLOT_ACCENT)
                                                  : w_c(C_SLOT_FG)));
        }

        /* 状态行缩进一级，颜色表达能不能直接用 */
        prov_state(p, st, sizeof st);
        w_line(x + 14, y + 14, w - 14, st,
               sel ? w_c(C_SLOT_ACC_DK) : w_c(C_SLOT_MUTED));

        y += ROW_H;
    }

    if (provider_count() > ROW_MAX) {
        char l[64];
        snprintf(l, sizeof l, S(S0522),
                 provider_count(), first + 1,
                 first + ROW_MAX < provider_count() ? first + ROW_MAX
                                                    : provider_count());
        w_line(x, y + 2, w, l, w_c(C_SLOT_FAINT));
    }

    /* ★ 纵向护栏：行高与行数都是算得出来的，而"最后一行被页脚吃掉半行"
     *   这件事在截图上要眯着眼才看得见 —— 上一版放 6 行就是这么漏了一个月
     *   （6×30=180 > 内容区 174）。以后谁再加行数，这一条会先红。 */
    w_vbounds((int)(y + TEXT_LINE_H + 2), W_TOP_CONTENT_B);

    w_top_footer(app_status(), S(S0523));
}

static void render_bottom(void)
{
    const float x = (float)W_BOT_MARGIN;
    const float w = (float)W_BOT_USABLE;
    float       y = (float)W_BOT_CONTENT_Y;
    const ProviderDef *p = provider_at(g_sel);
    char        st[96];

    w_bot_header(p ? p->name : S(S0368));

    if (!p) {
        w_wrap(x, y, w, S(S0524), w_c(C_SLOT_WARN), TEXT_LINE_H);
        w_bot_footer(S(S0332));
        return;
    }

    /* 说明：provider 表里那句 note 是给用户看的，原样引用，
     * 不在这里另写一份 —— 两处文案迟早会分叉。 */
    if (provider_note(p)[0])
        y = w_wrap(x, y, w, provider_note(p), w_c(C_SLOT_FG), TEXT_LINE_H);

    y += 6;
    prov_state(p, st, sizeof st);
    y = w_wrap(x, y, w, st, w_c(C_SLOT_MUTED), TEXT_LINE_H);

    const bool is_cur = (strcmp(p->name, app_cfg()->ai.provider) == 0);

    /* ★ 结果文案**紧跟状态行**，不再钉死在 y=126。
     *
     * 原先钉死的写法在短文案下看着没问题，但 X/Y 的说明一长就折成两行，
     * 126 + 36 = 162 直接压到按钮（150）上 —— **叠字**。
     * 这是 2026-10-16 实测截到的（新加的"内置地址改不了"那条）。
     *
     * 改成"有结果就不画动作提示"：两者都是解释性文字，同时出现只会
     * 互相挤，而按过键之后用户要看的本来就是结果。 */
    if (g_msg[0]) {
        y += 4;
        w_wrap(x, y, w, g_msg,
               g_msg_warn ? w_c(C_SLOT_WARN) : w_c(C_SLOT_OK), TEXT_LINE_H);
    } else {
        y += 6;
        w_wrap(x, y, w,
               is_cur ? S(S0525) : S(S0526),
               w_c(C_SLOT_ACCENT), TEXT_LINE_H);
    }

    w_button(&(Rect){ x, 150, w, 36, 0 },
             is_cur ? (p->fmt == PROVIDER_FMT_DOUBAO ? S(S0527)
                                                    : S(S0528))
                    : S(S0529),
             is_cur ? W_BTN_NORMAL : W_BTN_SELECTED);

    if (is_cur) {
        /* ⚠️ 页脚这里**故意不列 [B] 返回**：底屏只有 300px，
         *    「[A]配凭据 [Y]模型 [X]网址」已经占满，再挤一个 [B] 就溢出 ——
         *    而溢出是**会被** w_bot_footer 计进 layout_overflow 的。
         *    B=返回 是全项目统一的约定，不列也不会有人找不到。 */
        w_bot_footer(S(S0530));
    } else {
        w_bot_footer(S(S0531));
    }
}

/* --------------------------------------------------------------------------
 * 行为
 * ----------------------------------------------------------------------- */

/* 按钮矩形（render 与 handle 共用，避免"看到的位置点不到"）。 */
static const Rect PROV_BTN = { 10, 150, 300, 36, 0 };

static void providers_handle(u32 down, u32 held)
{
    (void)held;

    /* 触摸：唯一那个全宽按钮 = A 键语义，直接折算成一次 A，原样复用后面的分支。 */
    {
        int tx, ty;
        if (app_touch_down(&tx, &ty) && w_hit(&PROV_BTN, tx, ty))
            down |= KEY_A;
    }

    if (down & KEY_B) {
        app_pop();
        return;
    }

    if (provider_count() == 0)
        return;

    if (down & (KEY_UP | KEY_DOWN)) {
        clear_msg();
        if (down & KEY_UP)
            g_sel = (g_sel + provider_count() - 1) % provider_count();
        else
            g_sel = (g_sel + 1) % provider_count();
        return;
    }

    /* Y = 改这一家的模型，X = 改这一家的网址。
     * 两者都只对**当前在用**的那家生效（理由见 edit_guard）。 */
    if (down & KEY_Y) {
        do_edit_model();
        return;
    }
    if (down & KEY_X) {
        do_edit_url();
        return;
    }

    if (down & KEY_A) {
        const ProviderDef *p = provider_at(g_sel);

        clear_msg();
        if (!p)
            return;

        /* 已经就是它了 → A 的含义变成"去配凭据"，而不是重复切换
         * （重复切换不会有任何可见效果，用户会以为按键坏了）。 */
        if (strcmp(p->name, app_cfg()->ai.provider) == 0) {
            app_push(p->fmt == PROVIDER_FMT_DOUBAO ? SCREEN_DOUBAO
                                                   : SCREEN_KEYS);
            return;
        }
        do_pick();
    }
}


const Screen g_screen_providers = {
    .id            = SCREEN_PROVIDERS,
    .name          = "SCREEN_PROVIDERS",
    .enter         = providers_enter,
    .leave         = NULL,
    .render_top    = render_top,
    .render_bottom = render_bottom,
    .handle        = providers_handle,
};
