/*
 * sc_doubao.c -- 豆包扫码登录
 *
 * ── 这一屏补的是什么洞 ──
 * 在它出现之前，用户在 3DS 上**没有任何办法**把豆包连起来：
 * 服务商只能靠手改 key.txt 里的 `provider=doubao` 导入，
 * 登录态只能从 PC 上拷一份 cookie.txt 到卡里。也就是说，
 * 传输层再通，普通用户也用不上 —— 缺的正是这一屏。
 *
 * ── 为什么必须自动换码（不是"锦上添花"）──
 * 服务端给的二维码 **TTL 只有 60 秒**（响应里的 expire_time）。
 * 画一张静态码放在那儿，用户只要慢一点去掏手机，扫到的就是过期码，
 * 而且**界面上不会有任何异常提示** —— 手机那边只会说"二维码已失效"。
 * 所以到点必换，且换码这件事要在屏幕上看得见（有倒计时）。
 *
 * ── 一次只允许一个阻塞点 ──
 * 取码和轮询都是阻塞的 HTTP（本项目的既定形态，见 sc_chat 的 live_flush）。
 * 阻塞之前先把"正在取码…"画上去并 flush，否则用户看到的是一屏死画面。
 */

#include "screen.h"

#include "app.h"
#include "doubao.h"
#include "qr.h"
#include "widgets.h"

#include <3ds.h>
#include <stdio.h>
#include <string.h>
#include "uistr.h"

/* 系统 tick = 268 MHz（**不是 60Hz**，见 perf_lvl 那段的教训）。
 * 用它算秒而不是 svcSleepThread 累计：sleep 只是"至少等这么久"，
 * 连续 await 会越漂越远，而这里要靠它判断"二维码还剩几秒"。 */
/* ★ 括号必须加：`x / TICKS_PER_SEC` 若展开成 `x / 268000 * 1000`，
 *   由于 / 和 * 同级左结合，结果会被放大 1000 倍 ——
 *   于是"过了 45 秒"在第一帧就成立，二维码会被逐帧换掉。
 *   这类宏缺陷平时不显形（因为没人拿它做比较），一改状态机就炸。 */
#define TICKS_PER_SEC (268000ULL * 1000ULL)

#define POLL_EVERY_SEC  2       /* 轮询间隔 */
#define QR_REFRESH_SEC  45      /* 换码周期。TTL 60s，留 15s 余量 */
#define QR_TTL_SEC      60

typedef enum {
    PH_INTRO = 0,     /* 未登录，等用户按 A */
    PH_FETCH,         /* 正在取码（阻塞） */
    PH_SCAN,          /* 显示二维码 + 轮询 */
    PH_DONE,          /* 已登录 */
    PH_FAIL,          /* 出错：显示原因 + 重试 */
} Phase;

static Phase  g_ph;
static QrCode g_qr;
static char   g_msg[200];      /* 最近一次操作的结果（人话） */
static char   g_scan[32];      /* 服务端回的扫码状态：new/scanned/confirmed */
static u64    g_qr_at;         /* 当前这张码是何时取的 */
static u64    g_poll_at;       /* 上次轮询时刻 */
static int    g_qr_seq;        /* 换了几张码（调试/取证用） */

/* --------------------------------------------------------------------------
 * 立即把两屏画一遍
 *
 * 只在**阻塞调用之前**用：那时主循环还停在 handle 里，不主动 flush 的话
 * 用户在这 1~3 秒里看到的是上一帧（或一片空白）。
 * ----------------------------------------------------------------------- */

static void flush_now(void)
{
    const Screen *s = screen_get(app_current());

    if (!s)
        return;

    /* ★★ 两屏都画完，**最后只换一次**。
     *
     * 早先的写法是"画上屏 → r_end() → 画底屏 → r_end()"。
     * 但 r_end() 里的 gfxSwapBuffers() 是**全局**的 —— 它翻的是所有屏幕的
     * 缓冲索引。于是：
     *   第 1 次换：底屏索引翻了却没有新内容（底屏还是旧帧）；
     *   第 2 次换：上屏索引又翻回去 —— 上屏反倒退回**两帧前**那张。
     * 也就是说这个函数本意是"立刻显示新状态"，实际结果是把两屏都推回旧内容。
     *
     * 正确写法与主循环 app_render() 一致（main.c 里一个循环只 r_end() 一次）。 */
    if (s->render_top) {
        r_target(R_SCREEN_TOP);
        r_begin(w_c(C_SLOT_BG));
        s->render_top();
    }
    if (s->render_bottom) {
        r_target(R_SCREEN_BOTTOM);
        r_begin(w_c(C_SLOT_BG));
        s->render_bottom();
    }
    r_end();
}

/* --------------------------------------------------------------------------
 * 二维码
 * ----------------------------------------------------------------------- */

/* 把模块矩阵画成方块。
 *
 * ⚠️ 两点不能省：
 *  1. **必须白底黑块**。深色主题下如果直接把黑块画在深色背景上，
 *     手机扫不到 —— 二维码要求"浅底深码"，这是标准，不是审美。
 *  2. **模块边长要取整**。用小数边长会让相邻模块的边界落在半像素上，
 *     黑白交界就糊了；扫描识别的鲁棒性直接取决于边界锐利程度。 */
static void draw_qr(void)
{
    const int n = g_qr.n;
    int       m;
    float     qw, ox, oy;

    if (n < 21)
        return;

    {
        /* 给标题和倒计时留位置，剩下的空间里取最大的整数边长 */
        int avail_w = R_TOP_W - 2 * 14;
        int avail_h = R_TOP_H - 2 * 44;
        int mw = avail_w / n;
        int mh = avail_h / n;

        m = mw < mh ? mw : mh;
        if (m < 2)
            m = 2;
    }

    qw = (float)(m * n);
    ox = ((float)R_TOP_W - qw) / 2.0f;
    oy = ((float)R_TOP_H - qw) / 2.0f + 8.0f;

    /* 静默区（quiet zone）：往外再扩 4 个模块的白边。
     * 这不是装饰 —— 标准要求码周围留白，贴着别的图案会扫不出来。 */
    {
        float pad = (float)m * 4.0f;
        r_rect(&(Rect){ ox - pad, oy - pad, qw + pad * 2, qw + pad * 2, 0 },
               R_RGB(255, 255, 255));
    }

    for (int j = 0; j < n; ++j)
        for (int i = 0; i < n; ++i)
            if (g_qr.mod[j][i])
                r_rect(&(Rect){ ox + (float)(i * m), oy + (float)(j * m),
                                (float)m, (float)m, 0 },
                       R_RGB(0, 0, 0));
}

/* --------------------------------------------------------------------------
 * 取码 / 轮询
 * ----------------------------------------------------------------------- */

/* 取一张新码。会**阻塞**（两次 HTTP），调用前请先 flush_now()。 */
static bool fetch_qr(void)
{
    char msg[200];

    if (!db_login_begin(msg, sizeof msg)) {
        snprintf(g_msg, sizeof g_msg, "%s", msg);
        g_ph = PH_FAIL;
        app_note("doubao: 登录第 1 步失败：%s", msg);
        return false;
    }

    /* 上一次的码已经作废，先清掉 —— 否则取码失败时屏幕上还留着旧码，
     * 用户会去扫一张早就过期的图，而界面看着一切正常。 */
    memset(&g_qr, 0, sizeof g_qr);

    if (!db_login_qrcode(msg, sizeof msg)) {
        snprintf(g_msg, sizeof g_msg, "%s", msg);
        g_ph = PH_FAIL;
        app_note("doubao: 取二维码失败：%s", msg);
        return false;
    }
    (void)msg;

    /* 二维码 PNG 解码。失败不是致命的：可以退化成"让用户去 PC 上扫"，
     * 但更可能是几何算法失效（服务端换了图），所以要留证据。 */
    {
        static char uri[8192];   /* 3.5 KB，放静态区（app 线程栈要紧） */
        char qmsg[200];

        if (db_login_qrcode_uri(uri, sizeof uri)) {
            QrState qs = qr_decode_data_uri(uri, &g_qr, qmsg, sizeof qmsg);
            app_note("doubao: 二维码解码 = %s · %s",
                     qr_state_text(qs), qmsg);
            if (qs != QR_OK) {
                snprintf(g_msg, sizeof g_msg, S(S0788),
                         qr_state_text(qs));
            }
        } else {
            app_note("doubao: 拿不到二维码原始数据（无法本地渲染）");
        }
    }

    g_qr_at  = svcGetSystemTick();
    g_poll_at = g_qr_at;
    g_scan[0] = '\0';
    ++g_qr_seq;

    /* ★★ 这一行曾经**漏了**（2026-10-17 用户报"扫码一直没反应"才发现）。
     *   不置 PH_SCAN 的话，handle 的 switch 会落到 `PH_FETCH: default: return`
     *   —— 那一支是"阻塞中，按键不处理"。
     *   后果：二维码画得出来（render_top 只看 g_qr，不看 g_ph），
     *   但**不轮询扫码状态、不换码、按 A 也没反应**，
     *   用户扫完码之后 3DS 端永远不知道，60 秒后码过期而界面还显示着它。
     *   症状是"要不到码"，其实是"码拿到了但没人在等结果"。 */
    g_ph = PH_SCAN;

    snprintf(g_msg, sizeof g_msg,
             S(S0308),
             g_qr_seq);
    app_note("doubao: 已出示第 %d 张二维码（%d×%d 模块）",
             g_qr_seq, g_qr.n, g_qr.n);
    return true;
}

/* 轮询一次。会阻塞（一次 HTTP）。 */
static void poll_once(void)
{
    char st[32];
    char msg[200];

    if (!db_login_poll(st, sizeof st, msg, sizeof msg)) {
        /* 单次轮询失败通常只是网络抖一下，**不直接判失败** ——
         * 判失败会让用户白等一场重来。留着码继续轮询，把原因记下来。 */
        snprintf(g_msg, sizeof g_msg, "%s", msg);
        app_note("doubao: 轮询失败（继续等）：%s", msg);
        return;
    }

    if (strcmp(st, g_scan) != 0) {
        snprintf(g_scan, sizeof g_scan, "%s", st);
        app_note("doubao: 扫码状态 -> %s", st);
    }

    if (strcmp(st, "confirmed") == 0 || strcmp(st, "success") == 0) {
        g_ph = PH_DONE;
        snprintf(g_msg, sizeof g_msg, "%s", msg);
        app_note("doubao: 登录成功（%d 个 cookie）", db_cookie_count());
        return;
    }

    if (strcmp(st, "expired") == 0 || strcmp(st, "canceled") == 0 ||
        strcmp(st, "failed") == 0) {
        /* 码废了：立刻换一张，别让用户对着废码继续扫 */
        app_note("doubao: 码作废（%s），立即换一张", st);
        g_ph = PH_FETCH;
    }
}

/* --------------------------------------------------------------------------
 * 渲染
 * ----------------------------------------------------------------------- */

static void render_top(void)
{
    char line[96];

    switch (g_ph) {
    case PH_SCAN:
        /* 二维码屏：不画标题栏，把整块地方留给码。
         * 码周围的白边就是"这一屏在等扫码"的视觉语言。 */
        draw_qr();
        {
            unsigned left = 0;
            u64      now  = svcGetSystemTick();
            unsigned used = (unsigned)((now - g_qr_at) / TICKS_PER_SEC);

            if (used < QR_TTL_SEC)
                left = QR_TTL_SEC - used;
            snprintf(line, sizeof line, S(S0309), left,
                     g_scan[0] ? g_scan : S(S0310));
            w_center((float)R_TOP_W / 2.0f, 6,
                     S(S0311), w_c(C_SLOT_FG));
            w_center((float)R_TOP_W / 2.0f, (float)R_TOP_H - 14, line,
                     w_c(C_SLOT_MUTED));
        }
        return;

    case PH_DONE:
        w_top_header(S(S0312), S(S0151));
        w_wrap((float)W_TOP_MARGIN, (float)W_TOP_CONTENT_Y,
               (float)W_TOP_USABLE,
               S(S0767),
               w_c(C_SLOT_FG), TEXT_LINE_H);
        w_top_footer(app_status(), "");
        return;

    case PH_FETCH:
        w_top_header(S(S0312), S(S0314));
        w_wrap((float)W_TOP_MARGIN, (float)W_TOP_CONTENT_Y,
               (float)W_TOP_USABLE,
               S(S0315),
               w_c(C_SLOT_FG), TEXT_LINE_H);
        w_top_footer(app_status(), "");
        return;

    case PH_FAIL:
        w_top_header(S(S0312), S(S0316));
        w_wrap((float)W_TOP_MARGIN, (float)W_TOP_CONTENT_Y,
               (float)W_TOP_USABLE, g_msg, w_c(C_SLOT_WARN), TEXT_LINE_H);
        w_top_footer(app_status(), "");
        return;

    case PH_INTRO:
    default:
        w_top_header(S(S0312), S(S0317));
        w_wrap((float)W_TOP_MARGIN, (float)W_TOP_CONTENT_Y,
               (float)W_TOP_USABLE, S(S0788),
               w_c(C_SLOT_FG), TEXT_LINE_H);
        w_top_footer(app_status(), "");
        return;
    }
}

static void render_bottom(void)
{
    const float x = (float)W_BOT_MARGIN;
    const float w = (float)W_BOT_USABLE;
    float       y = (float)W_BOT_CONTENT_Y;

    w_bot_header(g_ph == PH_SCAN ? S(S0319) :
                 g_ph == PH_DONE ? S(S0151) : S(S0320));

    switch (g_ph) {
    case PH_SCAN:
        w_wrap(x, y, w,
               S(S0769),
               w_c(C_SLOT_FG), TEXT_LINE_H);
        y += TEXT_LINE_H * 4;
        if (g_scan[0])
            y = w_wrap(x, y, w, g_scan, w_c(C_SLOT_ACCENT), TEXT_LINE_H);
        w_button(&(Rect){ x, 168, (float)w, 34, 0 }, S(S0322),
                 W_BTN_NORMAL);
        w_bot_footer(S(S0323));
        return;

    case PH_DONE:
        w_wrap(x, y, w,
               S(S0324),
               w_c(C_SLOT_FG), TEXT_LINE_H);
        y += TEXT_LINE_H * 3;
        w_wrap(x, y, w,
               S(S0325),
               w_c(C_SLOT_MUTED), TEXT_LINE_H);
        w_button(&(Rect){ x, 160, (float)w, 34, 0 }, S(S0326),
                 W_BTN_NORMAL);
        w_bot_footer(S(S0327));
        return;

    case PH_FAIL:
        w_wrap(x, y, w, S(S0328), w_c(C_SLOT_FG), TEXT_LINE_H);
        w_button(&(Rect){ x, 152, (float)w, 36, 0 }, S(S0329),
                 W_BTN_SELECTED);
        w_bot_footer(S(S0330));
        return;

    case PH_FETCH:
        w_wrap(x, y, w, S(S0331), w_c(C_SLOT_MUTED), TEXT_LINE_H);
        w_bot_footer(S(S0332));
        return;

    case PH_INTRO:
    default:
        w_wrap(x, y, w,
               S(S0770),
               w_c(C_SLOT_FG), TEXT_LINE_H);
        w_button(&(Rect){ x, 150, (float)w, 40, 0 }, S(S0334),
                 W_BTN_SELECTED);
        w_bot_footer(S(S0335));
        return;
    }
}

/* --------------------------------------------------------------------------
 * 行为
 * ----------------------------------------------------------------------- */

static void db_enter(void)
{
    g_ph     = PH_INTRO;
    g_scan[0] = '\0';
    g_qr_seq = 0;
    g_msg[0] = '\0';
    memset(&g_qr, 0, sizeof g_qr);

    /* 进屏先看一眼卡上有没有现成的登录态。有就**先信它**，
     * 不必立刻联网校验 —— 那会让进屏卡两秒。真正的校验发生在聊天时。 */
    if (db_load_cookie() && db_has_cookie()) {
        g_ph = PH_DONE;
        snprintf(g_msg, sizeof g_msg, S(S0336),
                 db_cookie_count());
        app_note("doubao: 进屏时发现卡上已有登录态（%d 个 cookie）",
                 db_cookie_count());
    } else {
        app_note("doubao: 进屏，卡上没有登录态");
    }
    app_status_set(S(S0320));
}

static void db_leave(void)
{
    /* 什么都不做：登录态已经落盘了，退出这一屏不该把它清掉。 */
}

static void start_login(void)
{
    g_ph = PH_FETCH;
    snprintf(g_msg, sizeof g_msg, "%s", S(S0337));
    flush_now();                 /* 先把"取码中"画出来，再进阻塞 */
    fetch_qr();
}

static void do_logout(void)
{
    db_forget_cookie();
    g_ph = PH_INTRO;
    memset(&g_qr, 0, sizeof g_qr);
    snprintf(g_msg, sizeof g_msg, "%s", S(S0338));
    app_note("doubao: 用户主动退出登录（只清内存）");
}

/* 按钮矩形（render 与 handle 共用）。四个状态各画一个**全宽**按钮，
 * y 略有差别（150/152/160/168），这里取并集 150..202。
 * 因为每个状态最多只有一个按钮、且都占满宽度，并集不会和别的东西重叠。 */
static const Rect DB_BTN = { 10, 150, 300, 52, 0 };

static void db_handle(u32 down, u32 held)
{
    (void)held;

    if (down & KEY_B) {
        app_pop();
        return;
    }

    /* 触摸：四个有按钮的状态都走 A（各状态的 A 语义见下面的 switch）。
     * ⚠️ 必须排除 PH_FETCH —— 那是纯等待态，屏幕上**没有按钮**，
     *    不排除的话在"正在取二维码"时点下屏空白也会触发登录。 */
    if (g_ph != PH_FETCH) {
        int tx, ty;
        if (app_touch_down(&tx, &ty) && w_hit(&DB_BTN, tx, ty))
            down |= KEY_A;
    }

    switch (g_ph) {
    case PH_SCAN: {
        u64 now = svcGetSystemTick();

        /* 换码：到点就换。60 秒的 TTL，45 秒换 —— 留 15 秒给"码刚画出来
         * 用户正在扫"的窗口。卡在最后一秒换，扫码的人会正好扫到废码。 */
        if ((now - g_qr_at) / TICKS_PER_SEC >= QR_REFRESH_SEC) {
            app_note("doubao: 到 %d 秒，自动换码", QR_REFRESH_SEC);
            g_ph = PH_FETCH;
            snprintf(g_msg, sizeof g_msg, "%s", S(S0339));
            flush_now();
            fetch_qr();
            return;
        }

        /* 轮询 */
        if ((now - g_poll_at) / TICKS_PER_SEC >= POLL_EVERY_SEC) {
            g_poll_at = now;
            poll_once();
        }

        if (down & KEY_A)
            start_login();
        return;
    }

    case PH_DONE:
        if (down & KEY_A)
            do_logout();
        return;

    case PH_INTRO:
    case PH_FAIL:
        if (down & KEY_A)
            start_login();
        return;

    case PH_FETCH:
    default:
        return;                     /* 阻塞中，按键不处理 */
    }
}

const Screen g_screen_doubao = {
    .id            = SCREEN_DOUBAO,
    .name          = "SCREEN_DOUBAO",
    .enter         = db_enter,
    .leave         = db_leave,
    .render_top    = render_top,
    .render_bottom = render_bottom,
    .handle        = db_handle,
};
