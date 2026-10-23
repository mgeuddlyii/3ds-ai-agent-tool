/*
 * sc_xfer.c -- 从电脑取文件（互传页）
 *
 * ── 这个页在等什么 ──
 * 它在等**电脑上有个 HTTP 服务**。所以第一屏是"去电脑上打开那个脚本"，
 * 而不是"请输入网址" —— 后者对一个不看说明的人是死路。
 *
 * ── 进度条为什么能动，而传输又没开线程 ──
 * xfer_pull_chunked() 把文件切成 2~4 MB 的块，一块一块拉，
 * 每拉完一块就把主循环放出来跑一帧（on_yield）。
 * New 3DS 一秒传约 2 MB，也就是**每秒跳一次** ——
 * 肉眼看着是连续动画，而整个过程**零线程**。
 * （项目里线程用出过问题：join 的 timeout 没传下去、
 *  线程还在跑就被 free。分块没有这类风险。）
 *
 * ── 界面上必须说的三件事 ──
 * 1. 型号档位（旧 3DS 明显慢，且这**不是故障**）
 * 2. 预计还要多久（否则六分钟的白屏等于卡死）
 * 3. 中断后留下的 .tmp 是可以被清掉的（不制造"垃圾文件"的错觉）
 */

#include "sc_xfer.h"

#include "app.h"
#include "config.h"
#include "httptext.h"
#include "screen.h"
#include "widgets.h"
#include "xfer.h"

#include <3ds.h>
#include <stdio.h>
#include <stdlib.h>   /* strtol —— 清单里的字节数 */
#include <string.h>
#include "uistr.h"

#define XFER_PORT     8080
#define XFER_MAX_ITEMS 12
#define XFER_URL_MAX  256
#define XFER_NAME_MAX 64

typedef struct {
    char name[XFER_NAME_MAX];
    long size;
} XferItem;

static XferItem g_items[XFER_MAX_ITEMS];
static int      g_count;
static int      g_sel;
static char     g_ip[24] = "";

static int      g_pct;
static long     g_done;
static long     g_total;
static bool     g_busy;        /* 正在传 */
static bool     g_done_flag;   /* 这一轮收工了（成功或失败） */
static bool     g_ok;
static char     g_msg[192];
static int      g_elapsed;     /* 秒 */
static char     g_pair[8];     /* 电脑上那一对要的四位码；空 = 还没取到 */
static bool     g_asked;       /* 已经问过一次码了（别每次翻页都重问） */
static bool     g_paired;      /* 清单读到了 = 电脑上已配好 */
static int      g_skipped;     /* 名字太长被跳过的条数（要告诉用户） */

/* 传输状态里跨回调共享的东西。xfer 的回调在同一个线程里跑
 * （我们没用线程），所以不需要任何锁 —— 这也是分块方案的好处之一。 */
typedef struct {
    int  pct;
    long done;
} XferUi;

static XferUi g_ui;

static void fmt_size(long b, char *out, size_t cap)
{
    if (b >= 1024L * 1024L)
        snprintf(out, cap, "%ld MB", b / (1024L * 1024L));
    else if (b >= 1024L)
        snprintf(out, cap, "%ld KB", b / 1024L);
    else
        snprintf(out, cap, S(S0697), b);
}

/* 3DS 侧拿不到自己的 Wi-Fi 地址（libctru 没给这个接口），
 * 所以由用户照着 PC 脚本打印的那一行填进来。
 * 预填 192.168.1.100 —— 家用路由器最常见的那一段，
 * 命中率高，而用户改一次也就按两下键盘。 */
static void default_ip(void)
{
    snprintf(g_ip, sizeof g_ip, "192.168.1.100");
}

/* 用系统软键盘改地址。照搬对话屏的做法：
 * 系统键盘自带输入法与符号，自己画的结果只是"接近"，还多一堆布局坑。 */
static void ask_ip(void)
{
    /* 类型与常量照抄对话屏的 open_keyboard()：libctru 2.x 把它改名成
     * SwkbdState / SwkbdButton 了（不是老版的 swkbd_t / swkbdState），
     * 而且**没有 swkbdExit** —— 别照着网上的老例子写。 */
    SwkbdState swkbd;
    SwkbdButton btn;
    /* ★ 缓冲按**字节**放大到 64，字符数下面按 3 字节/字收紧到 20。
     * 早先是 buf[24] + `(int)sizeof buf - 1`（=23 个字符）：
     * 敲中文时 23×3 = 69 字节 → **写穿 24 字节缓冲**（越界写栈）。
     * IPv4 最长 15 字符（255.255.255.255），20 已给足余量；
     * 20×3 = 60 ≤ 63，放得下。多出来的字节由下面的有界拷贝安全截断。 */
    char       buf[64];

    swkbdInit(&swkbd, SWKBD_TYPE_WESTERN, 2, 20);
    swkbdSetHintText(&swkbd, S(S0698));
    swkbdSetInitialText(&swkbd, g_ip);

    btn = swkbdInputText(&swkbd, buf, sizeof buf);

    if (btn == SWKBD_BUTTON_CONFIRM && buf[0]) {
        /* ★ 显式有界拷贝，不用 snprintf("%s")：
         * buf 现在 64 字节、g_ip 只有 24，编译器会报
         * -Wformat-truncation（"最多 63 字节写进 24"）。
         * 这里要的就是"照搬、超了就截"，写成 memcpy 才表达得准确；
         * 而且这个字段是 IPv4（最长 15 字节），实际永远不会截断。 */
        size_t n = strlen(buf);
        if (n >= sizeof g_ip) n = sizeof g_ip - 1;
        memcpy(g_ip, buf, n);
        g_ip[n] = '\0';
        app_status_set(S(S0699), g_ip);
    } else {
        app_status_set(S(S0700));
    }
}

/* ---------------------------------------------------------------- 状态 */

static void st_idle(void)
{
    g_busy = g_done_flag = false;
    g_pct = 0; g_done = g_total = 0; g_elapsed = 0;
    g_msg[0] = '\0';
    g_ui.pct = 0; g_ui.done = 0;
}

static void st_listing(void)
{
    g_busy = true; g_done_flag = false; g_elapsed = 0;
    snprintf(g_msg, sizeof g_msg, "%s", S(S0701));
}

static void st_running(long total)
{
    g_busy = true; g_done_flag = false; g_ok = false;
    g_pct = 0; g_done = 0; g_total = total; g_elapsed = 0;
    g_ui.pct = 0; g_ui.done = 0;
    snprintf(g_msg, sizeof g_msg, "%s", S(S0702));
}

static void st_finished(bool ok, const char *msg)
{
    g_busy = false; g_done_flag = true; g_ok = ok;
    snprintf(g_msg, sizeof g_msg, "%s", msg ? msg : "");
    if (ok) { g_pct = 100; g_done = g_total; }
}

/* ---------------------------------------------------------------- 回调 */

static void on_progress(int pct, long done, long total, void *user)
{
    (void)user;
    g_ui.pct  = pct;
    g_ui.done = done;
    (void)total;
}

static void on_yield(void *user)
{
    (void)user;
    /* 每一块跑完把状态取回来，然后交还主循环出一帧。
     * 进度条就是靠这一下动的 —— 所以这个回调**必须**放在块与块之间，
     * 放在别的位置它就只会被下一次状态取回覆盖掉。 */
    g_pct  = g_ui.pct;
    g_done = g_ui.done;
    g_elapsed++;
    app_status_set(S(S0703), g_pct);
}

/* ---------------------------------------------------------------- 取码 */

/* 问电脑上要那个四位码。
 *
 * 为什么要显示它：配对是**网页那边输入**的，而电脑上那个窗口没人盯着，
 * 所以码必须出现在 3DS 上、让拿着 3DS 的人念到电脑上去。
 * 反过来（码显示在网页、3DS 输入）也行不通 —— 3DS 没数字键盘的舒服布局，
 * 而输错一位的后果是"配对不上"，用户会以为是网络问题。
 *
 * ★ 用 /ask.txt（纯文本）而不是 /ask（JSON）：3DS 侧不该为四个数字
 *   引一个 JSON 解析器，minijson 那点体积与复杂度换四个数字不值。 */
static void fetch_pair_code(void)
{
    char url[128], txt[16];
    long  n, http = 0;
    int   i;

    if (g_asked)
        return;                     /* 问过一次就别每帧重问 */
    g_asked = true;

    snprintf(url, sizeof url, "http://%s:%d/ask.txt", g_ip, XFER_PORT);
    n = http_get_text(url, txt, sizeof txt, &http);
    g_pair[0] = '\0';
    if (n < 0) {
        app_status_set(S(S0704));
        return;
    }
    if (n == 0) {
        app_status_set(S(S0705), http);
        return;
    }
    /* 只取前四位数字。服务端的保证是"四位"，
     * 这里仍然逐个校验 —— 拿到别的东西当码用，等于配不上。 */
    for (i = 0; i < n && i < 4; ++i) {
        if (txt[i] < '0' || txt[i] > '9')
            break;
    }
    if (i < 4) {
        app_status_set(S(S0706));
        return;
    }
    snprintf(g_pair, sizeof g_pair, "%.4s", txt);
    app_status_set(S(S0707), g_pair);
    app_note("xfer: 配对码 %s", g_pair);
}

/* ---------------------------------------------------------------- 取清单 */

static void fetch_list(void)
{
    char url[128], text[1400];
    long  n, http = 0;

    st_listing();
    snprintf(url, sizeof url, "http://%s:%d/list.txt", g_ip, XFER_PORT);

    n = http_get_text(url, text, sizeof text, &http);
    /* ★ 三种结局必须分开说，它们的处置完全相反：
     *   -1  连不上          → 查 IP 与服务
     *  403  没配对          → 去电脑上敲那个四位码
     *    0  其它错          → 电脑那边有状况，看它的窗口 */
    if (n < 0) {
        st_finished(false, S(S0708));
        return;
    }
    if (n == 0) {
        if (http == 403) {
            st_finished(false, S(S0709));
        } else {
            /* st_finished 不收变参（它只是把一句现成的话存起来），
             * 所以要带数字的那句先在这里拼好。 */
            char why[96];
            snprintf(why, sizeof why, S(S0705), http);
            st_finished(false, why);
        }
        return;
    }
    g_paired = true;
    if (g_skipped)
        app_note("xfer: 清单里有 %d 个条目名字太长被跳过", g_skipped);

    /* 清单格式：每行 "名字|字节数"。**故意用纯文本**——
     * 3DS 上为这点东西引一个 JSON 解析器不值得，而纯文本在
     * 传输坏了的时候还能用记事本看出一半发生了什么。 */
    g_count = 0;
    {
        char *p = text;
        while (p && *p && g_count < XFER_MAX_ITEMS) {
            char *nl = strchr(p, '\n');
            char *bar;
            if (nl) *nl = '\0';
            bar = strchr(p, '|');
            if (bar && p[0]) {
                *bar = '\0';
                /* ★ 名字太长就**跳过这一项并记账**，绝不截断后存下来。
                 *   截断出来的名字会被当成真名去下载，于是卡上存成一个
                 *   缺了尾巴的文件 —— 而清单上看着是"有这个文件"。
                 *   宁可少显示一项，也不要给一个下不下来的名字。 */
                if (strlen(p) >= XFER_NAME_MAX) {
                    g_skipped++;
                } else {
                    snprintf(g_items[g_count].name, XFER_NAME_MAX, "%s", p);
                    g_items[g_count].size = strtol(bar + 1, NULL, 10);
                    g_count++;
                }
            }
            p = nl ? nl + 1 : NULL;
        }
    }
    if (g_count == 0) {
        st_finished(false, S(S0710));
        return;
    }
    g_sel = 0;
    st_finished(false, "");
    g_done_flag = false;             /* 清单是常态，不算"收工" */
    g_busy = false;
}

/* ---------------------------------------------------------------- 传输 */

static void start_transfer(void)
{
    /* 缓冲按"最坏情况"给足：名字 63 字节若全是中文，
     * 百分号编码后是 189，再加 "http://ip:port/get/" 一段。
     * 给小了不是报错，而是**静默截断 URL** -> 404。 */
    char url[XFER_URL_MAX + 96], err[192];

    if (g_count <= 0 || g_sel < 0 || g_sel >= g_count)
        return;

    /* ★ 走 /get/<URL 编码后的名字>。
     *   文件名里常有中文与空格，而 URL 里必须百分号编码 ——
     *   服务端按 UTF-8 解（它那边也是这个约定）。不做这一步的表现是
     *   "清单里明明有这个名字，一按取就说没了"。 */
    {
        const char *nm = g_items[g_sel].name;
        char enc[XFER_URL_MAX];
        int  o = 0;
        size_t k;
        static const char HEX[] = "0123456789ABCDEF";

        for (k = 0; nm[k] && o < (int)sizeof enc - 4; ++k) {
            unsigned char ch = (unsigned char)nm[k];
            int safe = (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') ||
                       (ch >= '0' && ch <= '9') || ch == '.' || ch == '-' ||
                       ch == '_' || ch == '~';
            if (safe) {
                enc[o++] = (char)ch;
            } else {
                enc[o++] = '%';
                enc[o++] = HEX[ch >> 4];
                enc[o++] = HEX[ch & 15];
            }
        }
        enc[o] = '\0';
        snprintf(url, sizeof url, "http://%s:%d/get/%s", g_ip, XFER_PORT, enc);
    }
    st_running(g_items[g_sel].size);

    if (xfer_pull_chunked(url, "sdmc:/3ds/aiconsole/pkgs",
                          g_items[g_sel].name, g_items[g_sel].size, 0,
                          on_progress, NULL, on_yield, NULL,
                          err, sizeof err))
        st_finished(true, err);
    else
        st_finished(false, err);
}

/* ---------------------------------------------------------------- 渲染 */

static void xfer_render_top(void)
{
    const float x = W_TOP_MARGIN;
    const float w = W_TOP_USABLE;
    float       y = (float)W_TOP_CONTENT_Y;
    XferProfile prof;
    char        line[20];   /* fmt_size 最多写 "8388608 KB" 一类，12 字符够 */

    w_top_header(S(S0372), g_ip[0] ? g_ip : S(S0711));

    xfer_profile(&prof);
    y = w_line(x, y, w, prof.model, w_c(C_SLOT_ACC_DK));
    /* 旧 3DS 慢是硬件决定的，不是出了故障 ——
     * 这句话必须出现在**传输开始之前**，不然用户等了两分钟
     * 才发现"哦原来是这样"，那两分钟就白焦虑了。 */
    if (!prof.is_new)
        y = w_wrap(x, y, w, S(S0712),
                   w_c(C_SLOT_MUTED), TEXT_LINE_H);

    /* ---- 四位码画成门牌 ----
     * 为什么要画得这么显眼：它是**对着电脑念**的东西。
     * 夹在一堆说明文字里的四位数字，人会看漏；
     * 而看漏的后果是"我不知道要填什么"，于是整条路卡住。
     * 空框 + "还没问到"是刻意的 —— 那一帧就是提示"先按 Y 去问"。 */
    y += 6.0f;
    {
        const float box_h = 40.0f;
        w_panel(&(Rect){ x, y, w, box_h, 0 },
                 g_pair[0] ? w_c(C_SLOT_ACC_LT) : w_c(C_SLOT_PANEL),
                 w_c(C_SLOT_ACCENT));
        r_text(&(Rect){ x + 12, y + 8, 74, 24, 0 },
               S(S0713), w_c(C_SLOT_ACC_DK));
        r_text(&(Rect){ x + 90, y + 6, w - 102, 28, 0 },
               g_pair[0] ? g_pair : S(S0714),
               g_pair[0] ? w_c(C_SLOT_ACC_DK) : w_c(C_SLOT_FAINT));
        y += box_h + 6.0f;
    }
    if (g_pair[0] && !g_paired)
        y = w_line(x, y, w, S(S0715),
                   w_c(C_SLOT_FG));

    y = w_line(x, y, w, S(S0716), w_c(C_SLOT_FG));

    if (g_busy && g_count == 0) {
        w_wrap(x, y, w, g_msg, w_c(C_SLOT_MUTED), TEXT_LINE_H);
    } else if (g_count == 0) {
        w_wrap(x, y, w, S(S0717), w_c(C_SLOT_MUTED),
               TEXT_LINE_H);
    } else {
        for (int i = 0; i < g_count && i < 9; ++i) {
            /* 一行画完：选中的加 "▸ " 前缀。
             * 早先是"先画前缀、再用 y - TEXT_LINE_H 回退一行画内容" ——
             * 那是靠像素级回退凑出来的两段式，读代码要来回对照才知道
             * 画的是同一行。合成一行后既少一次绘制，也不会错位。 */
            char row[XFER_NAME_MAX + 40];
            fmt_size(g_items[i].size, line, sizeof line);
            /* ★ 三个 %.Ns 是**精度限定符**，作用有两个：
             *   ① 把"最多几个字符"告诉编译器，-Wformat-truncation
             *      就能算出上界（不给它就在这里报一个 815 字节的假警告，
             *      而实际三个来源加起来最多 88）。
             *   ② 万一超了，截的是"大小"那一段，而名字与前缀完好 ——
             *      名字被截才会导致存错文件。
             *
             * ★ 选中标记用 ASCII 的 '>' 而不是 '▸'(U+25B8)：
             *   3DS 内置字体**没有几何图形区（U+25xx）的字形**
             *   （同 U+2581 块元素那道坑），画出来是个空框。 */
            snprintf(row, sizeof row, "%s%.63s  %.12s",
                     i == g_sel ? "> " : "  ",
                     g_items[i].name[0] ? g_items[i].name : S(S0718), line);
            y = w_line(x, y, w, row,
                       i == g_sel ? w_c(C_SLOT_ACC_DK) : w_c(C_SLOT_FG));
        }
        if (g_skipped) {
            /* 跳过的必须说出来：用户在自己的电脑上明明看见那个文件，
             * 而清单里没有 —— 不说就变成"文件丢了"。 */
            y = w_line(x, y, w, S(S0719),
                       w_c(C_SLOT_MUTED));
        }
        if (g_count > 9)
            y = w_line(x, y, w, S(S0720), w_c(C_SLOT_FAINT));
    }

    w_top_footer(app_status(),
                 g_paired    ? S(S0721)
                 : g_pair[0]  ? S(S0722)
                 : g_msg[0]   ? g_msg
                               : S(S0193));
}

/* 三个按钮的矩形（render 与 handle 共用，避免"看到的位置点不到"）。
 * 位置：左 = 回主界面、中 = 读清单/问配对码、右 = 开始取/重试。 */
static const Rect XF_BTN_BACK = {  10, 150, 132, 36, 0 };
static const Rect XF_BTN_MID  = { 154, 150, 132, 36, 0 };
static const Rect XF_BTN_GO   = { 178, 150, 132, 36, 0 };

static void xfer_render_bottom(void)
{
    const float x = W_BOT_MARGIN;
    const float w = W_BOT_USABLE;
    char        pct_buf[16];
    int         pct = g_pct;

    w_bot_header(S(S0723));

    if (g_done_flag) {
        /* 收工态：进度条满格/空着，但要说清成没成。 */
        w_progress(x, 60.0f, w, 22.0f, g_ok ? 100 : pct,
                   g_ok ? S(S0724) : S(S0725));
        w_center(R_BOT_W / 2.0f, 96.0f, g_msg, w_c(C_SLOT_FG));
        w_button(&XF_BTN_BACK, S(S0726), W_BTN_NORMAL);
        w_button(&XF_BTN_GO, g_ok ? S(S0727) : S(S0329), W_BTN_PRIMARY);
    } else if (g_busy) {
        if (g_total > 0) {
            snprintf(pct_buf, sizeof pct_buf, "%d%%", pct);
        } else {
            snprintf(pct_buf, sizeof pct_buf, "…");
        }
        w_progress(x, 60.0f, w, 22.0f, pct, pct_buf);

        /* 三个数：已传 / 总量 / 大约还要多久。
         * 只报百分比不够 —— 用户想知道"要不要去干别的"。 */
        {
            char a[48], b[48], c[80];
            fmt_size(g_done, a, sizeof a);
            fmt_size(g_total, b, sizeof b);
            if (g_done > 0 && g_elapsed > 0) {
                long rate = g_done / g_elapsed;      /* KB/s，粗粒度足够 */
                long left = rate > 0 ? (g_total - g_done) / rate : 0;
                snprintf(c, sizeof c, S(S0728),
                         rate, left / 60, left % 60);
            } else {
                snprintf(c, sizeof c, "%s", S(S0729));
            }
            w_line(x, 92.0f, w, a, w_c(C_SLOT_FG));
            w_line(x, 108.0f, w, b, w_c(C_SLOT_MUTED));
            w_line(x, 124.0f, w, c, w_c(C_SLOT_FG));
        }
        w_center(R_BOT_W / 2.0f, 152.0f, S(S0730),
                 w_c(C_SLOT_MUTED));
    } else {
        w_progress(x, 60.0f, w, 22.0f, 0, NULL);
        w_center(R_BOT_W / 2.0f, 100.0f, S(S0731), w_c(C_SLOT_MUTED));
        w_button(&XF_BTN_BACK, S(S0726), W_BTN_NORMAL);
        w_button(&XF_BTN_MID, g_pair[0] ? S(S0732) : S(S0733), W_BTN_NORMAL);
        w_button(&XF_BTN_GO, S(S0734), W_BTN_PRIMARY);
    }

    w_bot_footer(g_done_flag ? S(S0735)
                  : g_busy     ? S(S0736)
                               : S(S0816));
    w_vbounds((int)(170.0f + 26.0f), W_BOT_CONTENT_B);
}

static void xfer_enter(void)
{
    st_idle();
    default_ip();
    g_pair[0]   = '\0';
    g_asked     = false;   /* 每次进屏重新问 —— 电脑上可能换了一个码 */
    g_paired    = false;
    g_skipped   = 0;
    app_status_set("");
}

static void xfer_handle(u32 down, u32 held)
{
    (void)held;

    /* 传输中只接受 B：其余键会打断一次写盘，
     * 而一次写盘被挪到别的位置意味着文件顺序出错。 */
    if (g_busy) {
        if (down & KEY_B) {
            st_finished(false, S(S0738));
        }
        return;
    }

    /* 触摸：三个按钮各自映射到既有按键语义，不另写动作。
     * 左=返回（同 B）、中=读清单/问配对码（同 Y）、右=开始/重试（同 A）。
     * ★ 放在 g_busy 那道闸**之后** —— 传输中只认 B，
     *   触摸不该能把一次写盘挪走（那会让文件顺序出错）。 */
    {
        int tx, ty;
        if (app_touch_down(&tx, &ty)) {
            if (w_hit(&XF_BTN_BACK, tx, ty)) {
                app_pop();
                return;
            }
            /* ⚠️ 中间那个按钮**只在空闲态才画**，而它的矩形（154..286）
             * 与右键（178..310）**重叠**。完工态若不加这个前提，
             * 点右边「再取一个 / 重试」会先命中中间键的空矩形，
             * 变成"读清单"—— 这正是"看到的位置点不到"的典型。
             * 判据必须与 render 的分支条件一致（g_done_flag）。 */
            if (!g_done_flag && w_hit(&XF_BTN_MID, tx, ty))
                down |= KEY_Y;
            else if (w_hit(&XF_BTN_GO, tx, ty))
                down |= KEY_A;
        }
    }

    if (down & KEY_B) {
        app_pop();
        return;
    }
    if (down & KEY_X) {
        ask_ip();
        return;
    }
    if (down & KEY_Y) {
        /* 没码就先问码 —— 用户第一次进来该看到的是那个要填的数字，
         * 而不是"读不到清单"（而那只是因为还没配对）。 */
        if (!g_pair[0]) {
            fetch_pair_code();
        } else {
            fetch_list();
        }
        return;
    }
    if (down & (KEY_UP | KEY_DOWN)) {
        if (g_count > 0) {
            g_sel += (down & KEY_DOWN) ? 1 : -1;
            if (g_sel < 0) g_sel = g_count - 1;
            if (g_sel >= g_count) g_sel = 0;
        }
        return;
    }
    if (down & KEY_A) {
        if (g_done_flag && g_ok) {
            st_idle();
            return;
        }
        if (g_count > 0)
            start_transfer();
        else
            fetch_list();
    }
}

const Screen g_screen_xfer = {
    .id            = SCREEN_XFER,
    .name          = "SCREEN_XFER",
    .enter         = xfer_enter,
    .leave         = NULL,
    .render_top    = xfer_render_top,
    .render_bottom = xfer_render_bottom,
    .handle        = xfer_handle,
};
