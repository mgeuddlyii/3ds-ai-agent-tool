/*
 * doubao.c -- 见 doubao.h（协议细节与"为什么单独一个模块"写在那儿）
 *
 * 本文件里的所有常量与拼法都来自实测（`豆包通道技术交接.md` §2），
 * **不要凭直觉改**。特别是：URL 的设备参数 query、开关的位置
 * （use_search 在 ext 里、另两个在 completion_option 里）——
 * 这两处写错都不会报错，只会静默失效。
 */

#include "doubao.h"
#include "httpx.h"
#include "util.h"

#include <3ds.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DB_BASE   "https://www.doubao.com"
#define DB_AID    "497858"
/* next 参数编码的是**站点根**（不是 /chat/）。它含 '%'，
 * 所以**绝不能出现在格式串里** —— 会被 printf 当成转换符吃掉。 */
#define DB_NEXT   "https%3A%2F%2Fwww.doubao.com"
#define DB_UA     "Mozilla/5.0 (Windows NT 10.0; Win64; x64) " \
                  "AppleWebKit/537.36 (KHTML, like Gecko) " \
                  "Chrome/120.0.0.0 Safari/537.36"
/* bot_id 是豆包 Web 端的固定值，实测可用 */
#define DB_BOT_ID "7338286299411103781"

/* --------------------------------------------------------------------------
 * 小工具
 * ----------------------------------------------------------------------- */

/* 取 "key":"value" 里的 value，并**解码反斜杠转义**。
 *
 * 这一条是整个豆包解析的支点：协议是三层嵌套的 JSON 字符串
 * （data → event_data 字符串 → message.content 字符串），
 * 每层都要"把一个被转义的 JSON 串还原成真 JSON 再往里找"。
 * 这个函数正好做这件事 —— 它对 `\"` 解码，所以外层取出来的
 * 就是内层的原始文本，可以直接再喂给自己。
 *
 * 只支持扁平 JSON（本场景够用），不做嵌套对象解析。
 * `\uXXXX` 直接写成 '?'（回复里的中文是原样 UTF-8，不走 \u 转义；
 * 真出现了也只影响一个字符，不会让解析整体失败）。 */
static bool json_str(const char *json, const char *key, char *out, size_t cap)
{
    char        needle[64];
    const char *p;
    size_t      o = 0;

    if (!out || cap == 0)
        return false;
    out[0] = '\0';
    if (!json)
        return false;

    snprintf(needle, sizeof needle, "\"%s\":\"", key);
    p = strstr(json, needle);
    if (!p)
        return false;
    p += strlen(needle);

    for (size_t i = 0; p[i] && o + 1 < cap; ++i) {
        char c = p[i];

        if (c == '\\') {
            ++i;
            if (!p[i])
                break;
            c = p[i];
            if (c == 'n')      c = '\n';
            else if (c == 't') c = '\t';
            else if (c == 'r') c = '\r';
            else if (c == 'u') { out[o++] = '?'; i += 4; continue; }
        } else if (c == '"') {
            break;
        }
        out[o++] = c;
    }
    out[o] = '\0';
    return o > 0;
}

/* 收正文的缓冲。增长式，因为聊天回复 + SSE 原始帧很容易超过几个 KB。 */
typedef struct {
    char  *p;
    size_t n, cap;
} Buf;

static bool buf_init(Buf *b, size_t cap)
{
    b->p = (char *)malloc(cap);
    if (!b->p) { b->n = b->cap = 0; return false; }
    b->cap = cap;
    b->n = 0;
    b->p[0] = '\0';
    return true;
}

static void buf_free(Buf *b)
{
    free(b->p);
    b->p = NULL;
    b->n = b->cap = 0;
}

static void buf_add(Buf *b, const char *s, size_t len)
{
    if (!b->p || b->n + len + 1 > b->cap) {
        size_t want = b->cap ? b->cap : 4096;
        char  *np;
        while (b->n + len + 1 > want)
            want *= 2;
        np = (char *)realloc(b->p, want);
        if (!np)
            return;                 /* 满了就丢新数据，绝不让它撑爆内存 */
        b->p = np;
        b->cap = want;
    }
    memcpy(b->p + b->n, s, len);
    b->n += len;
    b->p[b->n] = '\0';
}

static size_t sink_buf(const char *d, size_t len, void *u)
{
    buf_add((Buf *)u, d, len);
    return len;
}

/* 每跳清一次（跟重定向时用）—— 见 httpx.h 里 on_new_response 的理由。 */
static void sink_reset(void *u)
{
    Buf *b = (Buf *)u;
    if (b && b->p) { b->n = 0; b->p[0] = '\0'; }
}

/* --------------------------------------------------------------------------
 * 设备参数
 *
 * 交接文档 §1.3：**服务端不校验**（全空 / 只 web_id / 全给，四组返回逐字节
 * 相同）。所以这里自己造，不抓包、不硬编码别人的 id。
 * 造一次存住即可 —— 同一次运行里必须稳定，否则服务端可能把两次请求
 * 当成两个设备（虽然实测不校验，但没必要去试它）。
 * ----------------------------------------------------------------------- */
static char g_dev_id[24];
static char g_tea_uuid[24];

static void dev_init(void)
{
    if (g_dev_id[0])
        return;
    /* 19 位数字，形状与真实值一致。首位非 0。 */
    snprintf(g_dev_id, sizeof g_dev_id, "769%016u", (unsigned)rand());
    snprintf(g_tea_uuid, sizeof g_tea_uuid, "769%016u", (unsigned)rand());
}

/* 拼设备参数 query（聊天/历史都要，**缺了会回 710022002**）。 */
static void dev_query(char *out, size_t cap)
{
    dev_init();
    snprintf(out, cap,
             "aid=" DB_AID
             "&device_id=%s&device_platform=web&language=zh"
             "&pc_version=3.17.3&pkg_type=release_version&real_aid=" DB_AID
             "&region=CN&samantha_web=1&sys_region=CN"
             "&tea_uuid=%s&use-olympus-account=1"
             "&version_code=20800",
             g_dev_id, g_tea_uuid);
}

/* x-flow-trace 不是签名，就是两个随机 hex（交接文档 §1.2）。 */
static void make_xft(char *out, size_t cap)
{
    snprintf(out, cap,
             "x-flow-trace: {\"trace_id\":\"%08x%08x\",\"span_id\":\"%08x%08x\"}",
             (unsigned)rand(), (unsigned)rand(),
             (unsigned)rand(), (unsigned)rand());
}

/* --------------------------------------------------------------------------
 * 登录态
 * ----------------------------------------------------------------------- */

bool db_has_cookie(void)   { return hx_cookie_get("sessionid")[0] != '\0'; }
int  db_cookie_count(void) { return hx_cookie_count(); }

bool db_load_cookie(void)
{
    return hx_cookie_load(DB_COOKIE_FILE);
}

bool db_save_cookie(void)
{
    util_mkdirs(DB_DIR);
    return hx_cookie_save(DB_COOKIE_FILE);
}

void db_forget_cookie(void)
{
    /* 只清内存。**盘上的文件不动** —— 那是用户的登录态，
     * 我们无权因为"这次没认出来"就把它删掉（可能只是网络问题）。 */
    hx_cookie_clear();
}

/* --------------------------------------------------------------------------
 * 登录第 1 步：GET / （拿 ttwid + web_id）+ csrf
 * ----------------------------------------------------------------------- */

/* 三个跨步骤的中间量。**必须在 fetch_web_id 之前声明** ——
 * 它要用 g_web_id。C 的静态全局没有"提升"，顺序错了就是编译错误。 */
static char g_web_id[64];
static char g_csrf[256];
static char g_qr_token[512];

/* web_id 藏在首页 SSR 里："web_id":"7691..."。**没有它就拼不出 query**，
 * 所以这一步失败必须直接返回 false，不能带着空 web_id 往下走 ——
 * 那样后面每个请求都会回 block，而现象看着像"账号被限流"。 */
static bool fetch_web_id(Buf *b)
{
    const char *p = strstr(b->p, "\"web_id\":\"");
    if (!p)
        return false;
    p += 10;
    for (size_t i = 0; i < sizeof g_web_id - 1 && p[i] && p[i] != '"'; ++i)
        g_web_id[i] = p[i], g_web_id[i + 1] = '\0';
    return g_web_id[0] != '\0';
}


bool db_login_begin(char *msg, size_t cap)
{
    static const char *const hdr_ua[] = { "Accept: text/html", NULL };
    Buf  b;
    long st;
    HxReq r;
    char err[160];

    if (msg && cap) msg[0] = '\0';

    if (!buf_init(&b, 65536)) {
        snprintf(msg, cap, "内存不够");
        return false;
    }

    /* GET / 是 302，必须跟随。cookie（ttwid）在这一跳种下。 */
    memset(&r, 0, sizeof r);
    r.url         = DB_BASE "/";
    r.headers     = hdr_ua;
    r.timeout_s   = 30;
    r.follow      = true;
    r.add_cookies = false;          /* 还没 cookie，别带空头 */
    r.on_new_response = sink_reset;

    st = hx_perform(&r, sink_buf, &b, err, sizeof err);
    if (st != 200 && st != 302) {
        snprintf(msg, cap, "连不上豆包（%s，http=%ld）", err, st);
        buf_free(&b);
        return false;
    }
    if (!fetch_web_id(&b)) {
        snprintf(msg, cap, "首页里没找到 web_id —— 页面结构可能变了");
        buf_free(&b);
        return false;
    }
    buf_free(&b);

    /* csrf。字段名是 **passport_csrf_token** —— 搜 "token":" 匹配不到它，
     * 因为引号前面是 "passport_csrf_"（这条踩过）。 */
    if (!buf_init(&b, 8192)) {
        snprintf(msg, cap, "内存不够");
        return false;
    }
    memset(&r, 0, sizeof r);
    r.url         = DB_BASE "/passport/safe/csrf_token/?aid=" DB_AID;
    r.timeout_s   = 30;
    r.follow      = true;
    r.add_cookies = true;
    r.on_new_response = sink_reset;

    st = hx_perform(&r, sink_buf, &b, err, sizeof err);
    if (st == 200) {
        if (!json_str(b.p, "passport_csrf_token", g_csrf, sizeof g_csrf))
            json_str(b.p, "token", g_csrf, sizeof g_csrf);
    }
    buf_free(&b);

    if (!g_csrf[0]) {
        snprintf(msg, cap, "没拿到 csrf token（http=%ld）", st);
        return false;
    }
    snprintf(msg, cap, "准备好了，可以取二维码");
    return true;
}

/* --------------------------------------------------------------------------
 * 登录第 2 步：取二维码
 * ----------------------------------------------------------------------- */

/* 二维码原始 data URI。放静态区：3.5 KB，而这一层本来就只有界面一个调用方，
 * 让每个调用点都自备这么大的缓冲没有意义（这个 app 的栈很紧）。 */
static char g_qr_uri[8192];

bool db_login_qrcode_uri(char *out, size_t cap)
{
    size_t n;

    if (!out || cap == 0 || !g_qr_uri[0])
        return false;
    n = strlen(g_qr_uri);
    if (n + 1 > cap)
        return false;
    memcpy(out, g_qr_uri, n + 1);
    return true;
}

bool db_login_qrcode(char *msg, size_t msgcap)
{
    char  url[512], h1[512], h2[256], h3[256];
    const char *hl[4];
    Buf   b;
    long  st;
    HxReq r;
    char  err[160];

    g_qr_uri[0] = '\0';
    if (msg && msgcap) msg[0] = '\0';

    if (!g_csrf[0]) {
        snprintf(msg, msgcap, "还没拿到 csrf，先跑登录第 1 步");
        return false;
    }

    /* DB_NEXT 含 '%'，只能当参数拼，不能进格式串 */
    snprintf(url, sizeof url, DB_BASE "/passport/web/get_qrcode/?next=%s&aid=%s",
             DB_NEXT, DB_AID);
    snprintf(h1, sizeof h1, "x-tt-passport-csrf-token: %s", g_csrf);
    h2[0] = '\0'; snprintf(h2, sizeof h2, "Referer: " DB_BASE "/chat/login");
    h3[0] = '\0'; snprintf(h3, sizeof h3, "Origin: " DB_BASE);
    hl[0] = h1; hl[1] = h2; hl[2] = h3; hl[3] = NULL;

    if (!buf_init(&b, 131072)) {     /* 512×512 PNG 的 base64 很大 */
        snprintf(msg, msgcap, "内存不够");
        return false;
    }

    memset(&r, 0, sizeof r);
    r.url         = url;
    r.headers     = hl;
    r.timeout_s   = 30;
    r.follow      = true;
    r.add_cookies = true;
    r.on_new_response = sink_reset;

    st = hx_perform(&r, sink_buf, &b, err, sizeof err);
    if (st != 200) {
        snprintf(msg, msgcap, "取二维码失败（%s，http=%ld）", err, st);
        buf_free(&b);
        return false;
    }

    /* 字段是 **qrcode**（不是 image），值是 data:image/png;base64,...
     * 而且它在 "data" 下面，不在顶层。 */
    json_str(b.p, "token", g_qr_token, sizeof g_qr_token);
    if (!json_str(b.p, "qrcode", g_qr_uri, sizeof g_qr_uri)) {
        snprintf(msg, msgcap, "响应里没有二维码字段 —— 接口可能变了");
        buf_free(&b);
        return false;
    }
    buf_free(&b);

    if (!g_qr_token[0]) {
        snprintf(msg, msgcap, "拿到了码但没有 token，没法轮询");
        g_qr_uri[0] = '\0';
        return false;
    }
    snprintf(msg, msgcap, "二维码已就绪，60 秒内扫");
    return true;
}

/* --------------------------------------------------------------------------
 * 登录第 3 步：轮询一次
 * ----------------------------------------------------------------------- */

bool db_login_poll(char *status, size_t cap, char *msg, size_t msgcap)
{
    char  url[640], h1[512], h2[256], h3[256];
    const char *hl[4];
    Buf   b;
    long  st;
    HxReq r;
    char  err[160];

    if (status && cap) status[0] = '\0';
    if (msg && msgcap) msg[0] = '\0';

    if (!g_qr_token[0]) {
        snprintf(msg, msgcap, "还没取过码");
        return false;
    }

    /* **next= 是必须的**：缺了它这个端点永远回 error_code:3（缺少参数），
     * 而且不会报错，只是永远不给你状态 —— 看着像"扫码没反应"。 */
    snprintf(url, sizeof url,
             DB_BASE "/passport/web/check_qrconnect/?next=%s&token=%s&aid=%s",
             DB_NEXT, g_qr_token, DB_AID);
    snprintf(h1, sizeof h1, "x-tt-passport-csrf-token: %s", g_csrf);
    snprintf(h2, sizeof h2, "Referer: " DB_BASE "/chat/login");
    snprintf(h3, sizeof h3, "Origin: " DB_BASE);
    hl[0] = h1; hl[1] = h2; hl[2] = h3; hl[3] = NULL;

    if (!buf_init(&b, 16384)) {
        snprintf(msg, msgcap, "内存不够");
        return false;
    }

    memset(&r, 0, sizeof r);
    r.url         = url;
    r.headers     = hl;
    r.timeout_s   = 20;
    r.follow      = true;
    r.add_cookies = true;
    r.on_new_response = sink_reset;

    st = hx_perform(&r, sink_buf, &b, err, sizeof err);
    if (st != 200) {
        snprintf(msg, msgcap, "轮询失败（%s，http=%ld）", err, st);
        buf_free(&b);
        return false;
    }

    if (!json_str(b.p, "status", status, cap)) {
        /* 有些分支不带 status 字段，只有 confirmed 字样 */
        if (strstr(b.p, "confirmed"))
            snprintf(status, cap, "confirmed");
    }

    if (strcmp(status, "confirmed") == 0 || strcmp(status, "success") == 0) {
        /* 跟 redirect_url 那一跳 —— **登录态就是这样种下的**
         * （sessionid 在那次响应的 Set-Cookie 里）。少了这一跳，
         * 前面的 cookie 全是匿名态，history 会回 code!=0。 */
        char redirect[1024];
        redirect[0] = '\0';
        if (json_str(b.p, "redirect_url", redirect, sizeof redirect) &&
            redirect[0]) {
            HxReq r2;
            Buf   r3;
            if (buf_init(&r3, 4096)) {
                memset(&r2, 0, sizeof r2);
                r2.url         = redirect;
                r2.timeout_s   = 30;
                r2.follow      = true;
                r2.add_cookies = true;
                hx_perform(&r2, sink_buf, &r3, NULL, 0);
                buf_free(&r3);
            }
        }
        db_save_cookie();
        snprintf(msg, msgcap, "登录成功（%d 个 cookie），已存到卡上",
                 hx_cookie_count());
        buf_free(&b);
        return true;
    }
    buf_free(&b);

    if (status[0]) {
        snprintf(msg, msgcap, "状态：%s", status);
        return true;
    }
    snprintf(msg, msgcap, "还没扫（响应里没有状态字段）");
    return false;
}

/* --------------------------------------------------------------------------
 * 登录态校验
 *
 * 判据取 conversation/list 的 `"code":0` —— 那是**需要登录才拿得到**的真实
 * 数据。不能只看"cookie 文件在不在"：服务端会过期，而文件不会自己消失，
 * 于是"文件在但登录已失效"会表现成"聊天总是失败"，方向完全被带偏。
 *
 * ⚠️ 这里**不要求 g_web_id 非空**。探针实测：带着空的 web_id 请求
 * conversation/list 一样回 code=0（与交接文档 §1.3 "设备参数不校验" 一致）。
 * 冷启动读缓存登录态时还没走过 GET /，本来就没有 web_id ——
 * 要是这里卡住，缓存登录态就永远用不上了。
 * ----------------------------------------------------------------------- */

bool db_verify_login(char *detail, size_t cap)
{
    char  url[768], q[512], h1[256], h2[256], h3[256], xft[256];
    const char *hl[6];
    Buf   b;
    long  st;
    HxReq r;
    char  err[160];
    bool  ok;

    if (detail && cap) detail[0] = '\0';

    if (!buf_init(&b, 65536)) {
        snprintf(detail, cap, "内存不够");
        return false;
    }

    dev_query(q, sizeof q);
    snprintf(url, sizeof url,
             DB_BASE "/samantha/conversation/list?%s&web_id=%s", q, g_web_id);

    /* 这几个头是探针实测必须带的（accept: text/event-stream 连 history
     * 都要带 —— 少一个就可能被网关判成"非预期客户端"）。 */
    snprintf(h1, sizeof h1, "%s", "content-type: application/json");
    snprintf(h2, sizeof h2, "%s", "agw-js-conv: str");
    snprintf(h3, sizeof h3, "%s", "Referer: " DB_BASE "/chat/");
    make_xft(xft, sizeof xft);
    {
        char origin[128];
        snprintf(origin, sizeof origin, "Origin: " DB_BASE);
        hl[0] = h1; hl[1] = h2; hl[2] = h3; hl[3] = origin;
        hl[4] = xft; hl[5] = NULL;

        memset(&r, 0, sizeof r);
        r.url         = url;
        r.post        = "{\"cursor\":0,\"count\":20}";
        r.headers     = hl;
        r.timeout_s   = 40;
        r.follow      = false;
        r.add_cookies = true;
        r.on_new_response = sink_reset;

        st = hx_perform(&r, sink_buf, &b, err, sizeof err);
    }

    ok = (st == 200 && b.p && strstr(b.p, "\"code\":0") != NULL);

    if (ok) {
        char name[128];
        if (json_str(b.p, "name", name, sizeof name))
            snprintf(detail, cap, "登录态有效（最近会话：%.40s）", name);
        else
            snprintf(detail, cap, "登录态有效");
    } else if (st == 200) {
        snprintf(detail, cap, "服务端不认这个登录态了（多半过期，要重新扫码）");
    } else {
        snprintf(detail, cap, "校验失败（%s，http=%ld）", err, st);
    }

    buf_free(&b);
    return ok;
}

/* --------------------------------------------------------------------------
 * 聊天
 * ----------------------------------------------------------------------- */

const char *db_state_text(DbState s)
{
    switch (s) {
        case DB_OK:           return "成功";
        case DB_ERR_NO_LOGIN: return "没有登录态";
        case DB_ERR_NET:      return "连不上";
        case DB_ERR_HTTP:     return "服务端拒绝了";
        case DB_ERR_BLOCK:    return "被拦下（设备参数缺失或访问频繁）";
        case DB_ERR_PARSE:    return "响应解不出来";
        case DB_ERR_EMPTY:    return "回复是空的";
        default:              return "未知情况";
    }
}

/* JSON 字符串转义（不含外层引号）。 */
static bool esc_json(char *dst, size_t cap, const char *src)
{
    size_t o = 0;

    if (!dst || cap == 0)
        return false;
    for (size_t i = 0; src && src[i]; ++i) {
        unsigned char c = (unsigned char)src[i];
        const char   *rep = NULL;
        char          tmp[8];

        switch (c) {
            case '"':  rep = "\\\""; break;
            case '\\': rep = "\\\\"; break;
            case '\n': rep = "\\n";  break;
            case '\r': rep = "\\r";  break;
            case '\t': rep = "\\t";  break;
            default:
                if (c < 0x20) {
                    snprintf(tmp, sizeof tmp, "\\u%04x", c);
                    rep = tmp;
                }
                break;
        }
        if (rep) {
            size_t l = strlen(rep);
            if (o + l + 1 > cap) return false;
            memcpy(dst + o, rep, l);
            o += l;
        } else {
            if (o + 2 > cap) return false;
            dst[o++] = (char)c;
        }
    }
    dst[o] = '\0';
    return true;
}

/* 解析临时区。**放静态区而不是栈上**：三层 JSON 一层一个 16KB 缓冲，
 * 四个叠起来 50 多 KB，而 app 线程栈总共 256KB —— 一次调用就吃掉五分之一。
 * 本模块只在 app 线程用（单线程），静态复用没有并发问题。 */
static char s_payload[16384];
static char s_inner[16384];
static char s_content[16384];
static char s_conv[64];

const char *db_last_conversation_id(void) { return s_conv; }

/* 粘一段文本到回复尾部。 */
static void reply_append(char *reply, size_t cap, const char *text)
{
    size_t n = strlen(reply);
    size_t l = strlen(text);

    if (n + l + 1 > cap)
        l = (n + 1 < cap) ? cap - n - 1 : 0;
    memcpy(reply + n, text, l);
    reply[n + l] = '\0';
}

DbState db_chat(const char *user_text, const char *conversation_id,
                char *reply, size_t reply_cap,
                char *msg, size_t msg_cap)
{
    char  url[768], q[512];
    char  h1[128], h2[128], h3[128], h4[128], xft[256], origin[128];
    const char *hl[7];
    char  tesc[768], inner_msg[1024], cesc[1400];
    static char body[4096];
    static Buf  b;
    HxReq r;
    long  st;
    char  err[160];
    int   n;

    if (reply && reply_cap) reply[0] = '\0';
    if (msg && msg_cap)     msg[0] = '\0';
    s_conv[0] = '\0';

    if (!user_text || !*user_text) {
        snprintf(msg, msg_cap, "没内容可发");
        return DB_ERR_EMPTY;
    }
    if (!db_has_cookie()) {
        /* 与"网络不通"分开报 —— 这两种的处理方式完全不一样 */
        snprintf(msg, msg_cap, "还没登录豆包，先扫码登录");
        return DB_ERR_NO_LOGIN;
    }

    /* ---- 双层 JSON 编码 ----
     * body 里的 content 是**一个 JSON 字符串**，其内容又是 JSON：
     *     "content":"{\"text\":\"用户说的话\"}"
     * 所以用户文本要被转义两次：先转进内层 {"text":"..."}，
     * 再把整个内层转义一次当外层的字符串值。
     * 少转一层的话，文本里只要有一个引号就会把请求体弄坏 ——
     * 而报错会是服务端的 710020702"系统错误"，指向完全错误的方向。 */
    if (!esc_json(tesc, sizeof tesc, user_text)) {
        snprintf(msg, msg_cap, "这句话太长或含无法编码的字符");
        return DB_ERR_EMPTY;
    }
    n = snprintf(inner_msg, sizeof inner_msg, "{\"text\":\"%s\"}", tesc);
    if (n < 0 || (size_t)n >= sizeof inner_msg) {
        snprintf(msg, msg_cap, "这句话太长");
        return DB_ERR_EMPTY;
    }
    if (!esc_json(cesc, sizeof cesc, inner_msg)) {
        snprintf(msg, msg_cap, "这句话太长");
        return DB_ERR_EMPTY;
    }

    /* conversation_id 传空/ NULL 当 "0" = 新建会话 */
    {
        const char *conv = (conversation_id && *conversation_id)
                           ? conversation_id : "0";
        /* need_create_conversation 必须**跟着 conversation_id 走**。
         *
         * 它原先是硬编码的 true —— 于是即使把上一轮的 id 原样传回来，
         * 服务端仍然开一个新会话，症状是**每一轮返回的 conversation_id
         * 都不一样**，豆包每次都失忆（它会答"我看不到你上一个任务"）。
         * 判据句：连发两轮，第二轮报的 id 应当与第一轮**相同**。
         */
        const char *mk = (strcmp(conv, "0") == 0) ? "true" : "false";
        n = snprintf(body, sizeof body,
            "{\"bot_id\":\"" DB_BOT_ID "\","
            "\"completion_option\":{\"is_regen\":false,\"with_suggest\":false,"
            "\"need_create_conversation\":%s,\"launch_stage\":1,"
            "\"use_auto_cot\":false,\"use_deep_think\":false},"
            "\"conversation_id\":\"%s\","
            "\"local_conversation_id\":\"local_%u\","
            "\"local_message_id\":\"%08x%08x\","
            "\"messages\":[{\"content\":\"%s\",\"content_type\":2001,"
            "\"attachments\":[],\"references\":[]}],"
            /* use_search 在 ext 里，值是**字符串 "1"** —— 不是布尔，
             * 也不是数字。放错地方/写错类型都不会报错，只是联网搜索不生效。 */
            "\"ext\":{\"use_search\":\"1\"}}",
            mk, conv, (unsigned)rand(), (unsigned)rand(), (unsigned)rand(),
            cesc);
        if (n < 0 || (size_t)n >= sizeof body) {
            snprintf(msg, msg_cap, "请求体太长，请把问题说短一点");
            return DB_ERR_EMPTY;
        }
    }

    /* ---- URL：**必须带完整设备参数 query** ----
     * 少了它服务端回 710022002 block，而那个码长得像"访问频繁"，
     * 极易误判成限流去等 60 秒 —— 等多久都没用，因为根本不是限流。 */
    dev_query(q, sizeof q);
    snprintf(url, sizeof url,
             DB_BASE "/samantha/chat/completion?%s&web_id=%s", q, g_web_id);

    snprintf(h1, sizeof h1, "%s", "content-type: application/json");
    snprintf(h2, sizeof h2, "%s", "accept: text/event-stream");
    snprintf(h3, sizeof h3, "%s", "agw-js-conv: str");
    snprintf(h4, sizeof h4, "%s", "Referer: " DB_BASE "/chat/");
    snprintf(origin, sizeof origin, "Origin: " DB_BASE);
    make_xft(xft, sizeof xft);
    hl[0] = h1; hl[1] = h2; hl[2] = h3; hl[3] = h4;
    hl[4] = origin; hl[5] = xft; hl[6] = NULL;

    if (!buf_init(&b, 131072)) {
        snprintf(msg, msg_cap, "内存不够");
        return DB_ERR_NET;
    }

    memset(&r, 0, sizeof r);
    r.url         = url;
    r.post        = body;
    r.headers     = hl;
    r.timeout_s   = 60;
    r.follow      = false;      /* POST 不跟重定向 */
    r.add_cookies = true;

    st = hx_perform(&r, sink_buf, &b, err, sizeof err);

    if (st < 0) {
        snprintf(msg, msg_cap, "连不上（%s）", err);
        buf_free(&b);
        return DB_ERR_NET;
    }
    if (st != 200) {
        /* 710022002 单独认出来 —— 它要的是"等一会儿"，不是"重试"。 */
        if (b.p && strstr(b.p, "710022002")) {
            snprintf(msg, msg_cap,
                     "服务端拦下了（710022002）。这多半是设备参数不全或"
                     "短时间内问得太频繁，等 60 秒再试。");
            buf_free(&b);
            return DB_ERR_BLOCK;
        }
        snprintf(msg, msg_cap, "服务端回了 HTTP %ld", st);
        buf_free(&b);
        return DB_ERR_HTTP;
    }

    /* ---- 解析 SSE ----
     * 结构是**三层 JSON**（交接文档 §2.2 与参考实现 sse.py 一致）：
     *   data: {"event_type":2001,"event_data":"<转义过的 JSON 字符串>"}
     *         内层：{"message":{"content_type":2001,"content":"<又是 JSON>"}}
     *           最内层：{"text":"真正的文字"}
     * json_str 每次都会解一层反斜杠转义，所以逐层喂进去正好。
     * 文本是**增量**（参考实现是 full_text += extracted），所以逐帧追加。 */
    {
        const char *ln = b.p;
        int frames = 0;

        while (ln && *ln) {
            const char *nl  = strchr(ln, '\n');
            size_t      len = nl ? (size_t)(nl - ln) : strlen(ln);

            while (len && (ln[len-1] == '\r' || ln[len-1] == ' '))
                --len;

            if (len > 5 && strncmp(ln, "data:", 5) == 0) {
                size_t pl = len - 5;
                const char *ps;

                if (pl >= sizeof s_payload)
                    pl = sizeof s_payload - 1;
                memcpy(s_payload, ln + 5, pl);
                s_payload[pl] = '\0';

                ps = s_payload;
                while (*ps == ' ' || *ps == '\t')
                    ++ps;

                if (strcmp(ps, "[DONE]") == 0)
                    break;

                /* 会话 id **不在 2001 帧里** —— 实测它在第一帧
                 * （event_type=2002）的 event_data 里。所以这里单独解一次
                 * 去找它，不能塞在下面那个 2001 分支里（那样永远找不到，
                 * 而症状是"多轮对话每次都开新会话"，看着像服务端的问题）。
                 * 用 !s_conv[0] 兜住：拿到之后就不再解了。 */
                if (!s_conv[0] &&
                    json_str(ps, "event_data", s_inner, sizeof s_inner))
                    json_str(s_inner, "conversation_id", s_conv,
                             sizeof s_conv);

                /* 只要文字事件。别的 event_type 是心跳/元数据 */
                if (strstr(ps, "\"event_type\":2001")) {
                    ++frames;
                    if (json_str(ps, "event_data", s_inner, sizeof s_inner) &&
                        json_str(s_inner, "content", s_content,
                                 sizeof s_content)) {
                        /* 带 thinking 字段的是思考链，不是回答。
                         * 我们没开 deep_think，正常不会出现；真出现了也不能
                         * 把它当答案粘进去。 */
                        if (!strstr(s_content, "\"thinking\"") &&
                            !strstr(s_content, "\"reasoning_content\"")) {
                            char text[8192];
                            const char *cs = s_content;

                            while (*cs == ' ' || *cs == '\t')
                                ++cs;

                            if (*cs == '{') {
                                /* 是 JSON 对象 → **只在真有 text 时才取**。
                                 * 实测：一次回答的 6 帧里有 2 帧是 event_type
                                 * 2001，其中**收尾那一帧的 content 是空对象 {}**。
                                 * 如果在这里无条件退回"把 content 原文当内容"，
                                 * 答案就会变成 `2{}` —— 而它看着还挺像对的，
                                 * 属于最难被发现的那类错。
                                 * 这一类只有拿真实响应才照得出来，文档里看不出来。 */
                                if (json_str(s_content, "text", text,
                                             sizeof text))
                                    reply_append(reply, reply_cap, text);
                            } else {
                                /* 不是 JSON 对象：原文本身就是内容（参考实现的兜底）*/
                                reply_append(reply, reply_cap, cs);
                            }
                        }
                    }
                }
            }

            if (!nl)
                break;
            ln = nl + 1;
        }

        if (!reply[0]) {
            snprintf(msg, msg_cap,
                     frames ? "收到了 %d 帧但没解出文字（协议可能改版了）"
                            : "没收到任何数据帧（可能被风控拦了）",
                     frames);
            buf_free(&b);
            return DB_ERR_PARSE;
        }
    }

    buf_free(&b);
    snprintf(msg, msg_cap, "收到 %u 字节回复", (unsigned)strlen(reply));
    return DB_OK;
}

