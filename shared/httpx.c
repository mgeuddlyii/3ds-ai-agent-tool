/*
 * httpx.c -- 见 httpx.h（TLS 1.2 / CA / cookie 的理由都写在那儿）
 *
 * 这一层刻意"薄"：它只管把一次 HTTP 发出去、把正文喂给回调、
 * 顺手把 Set-Cookie 收进 jar。协议知识（豆包怎么拼 body、
 * OpenAI 怎么解析 SSE）一律不在这儿。
 */

#include "httpx.h"

#include <3ds.h>
#include <curl/curl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

/* 内嵌的 Mozilla CA bundle（build.py 的 bin2s 段生成）。
 *
 * ⚠️ **符号名规则容易搞错**：bin2s 取的是"源文件名里非字母数字→下划线"，
 * 所以 `cacert.pem` → `cacert_pem` / `cacert_pem_size`。
 * 它不是"去掉扩展名再加 _bin" —— `font.bin` → `font_bin` 只是**碰巧**
 * 因为扩展名正好叫 .bin。照着 font 那个例子去猜 pem 的名字，
 * 得到的就是 `cacert_bin`，然后编译报 undeclared。
 * 头文件本身叫 cacert_bin.h（build.py 用 stem 拼的），名字对不上是正常的。
 *
 * 生成不出来时给一个空数组让编译过 —— 运行时会因为"没有 CA"而连不上，
 * 那个现象比"编译直接失败"更好定位（编译失败会被当成代码写错）。 */
#if defined(__has_include)
#  if __has_include("cacert_bin.h")
#    include "cacert_bin.h"
#    define HX_HAVE_CA 1
#  endif
#endif
#ifndef HX_HAVE_CA
static const unsigned char  cacert_pem[1] = { 0 };
static const size_t         cacert_pem_size = 0;
#endif

/* 卡上的覆盖文件。存在就用它（本机卡巴斯基 MITM 时要加它的根证书），
 * 不存在就用内嵌的 —— 真机走这条路。 */
#define HX_CA_OVERRIDE "sdmc:/config/ssl/cacert.pem"

static bool g_inited;

/* --------------------------------------------------------------------------
 * cookie jar
 *
 * 就是一张 name/value 表。不实现 domain/path/expiry —— 我们只跟一个站点
 * 打交道，把完整的 cookie 语义搬进来只会盖住真正要看的东西（登录态在不在）。
 * ----------------------------------------------------------------------- */

#define CK_MAX 40
#define CK_NAME 48
#define CK_VAL 1024

static char g_ck_name[CK_MAX][CK_NAME];
static char g_ck_val[CK_MAX][CK_VAL];
static int  g_ck_n;

void hx_cookie_clear(void)
{
    g_ck_n = 0;
}

void hx_cookie_set(const char *name, const char *value)
{
    if (!name || !name[0])
        return;

    for (int i = 0; i < g_ck_n; ++i) {
        if (strcmp(g_ck_name[i], name) == 0) {
            snprintf(g_ck_val[i], CK_VAL, "%s", value ? value : "");
            return;
        }
    }
    if (g_ck_n >= CK_MAX)
        return;                     /* 满了就丢新的。豆包实测 17 个，40 够 */
    snprintf(g_ck_name[g_ck_n], CK_NAME, "%s", name);
    snprintf(g_ck_val[g_ck_n], CK_VAL, "%s", value ? value : "");
    ++g_ck_n;
}

const char *hx_cookie_get(const char *name)
{
    for (int i = 0; i < g_ck_n; ++i)
        if (strcmp(g_ck_name[i], name) == 0)
            return g_ck_val[i];
    return "";
}

int hx_cookie_count(void) { return g_ck_n; }

void hx_cookie_string(char *out, size_t cap)
{
    size_t o = 0;

    if (!out || !cap)
        return;
    out[0] = '\0';

    for (int i = 0; i < g_ck_n && o + 1 < cap; ++i) {
        int n = snprintf(out + o, cap - o, "%s%s=%s",
                         i ? "; " : "", g_ck_name[i], g_ck_val[i]);
        if (n < 0 || (size_t)n >= cap - o)
            break;
        o += (size_t)n;
    }
}

bool hx_cookie_save(const char *path)
{
    char line[8192];
    FILE *f;

    if (!path)
        return false;
    hx_cookie_string(line, sizeof line);
    if (!line[0])
        return false;

    f = fopen(path, "w");
    if (!f)
        return false;
    if (fputs(line, f) < 0 || fflush(f) != 0) {
        fclose(f);
        return false;
    }
    fclose(f);
    return true;
}

bool hx_cookie_load(const char *path)
{
    char line[8192];
    FILE *f;
    char *p;

    if (!path)
        return false;
    f = fopen(path, "r");
    if (!f)
        return false;
    if (!fgets(line, sizeof line, f)) {
        fclose(f);
        return false;
    }
    fclose(f);

    /* 解析 "name=value; name=value"。遇到不成对的部分就跳过这一段，
     * 不能因为它坏了就丢掉后面所有 cookie —— 那会让"读到了但没用"
     * 表现得像"根本没读"。 */
    p = line;
    while (*p) {
        char name[CK_NAME], val[CK_VAL];
        char *eq = strchr(p, '=');
        char *sc;

        if (!eq)
            break;
        sc = strchr(p, ';');
        if (!sc)
            sc = p + strlen(p);
        if (eq >= sc) {                     /* 这一段没有值，跳到下一段 */
            p = (*sc == ';') ? sc + 1 : sc;
            while (*p == ' ') ++p;
            continue;
        }

        {
            size_t nl = (size_t)(eq - p);
            size_t vl = (size_t)(sc - eq - 1);
            if (nl == 0 || nl >= CK_NAME || vl >= CK_VAL) {
                p = (*sc == ';') ? sc + 1 : sc;
                while (*p == ' ') ++p;
                continue;
            }
            memcpy(name, p, nl); name[nl] = '\0';
            memcpy(val, eq + 1, vl); val[vl] = '\0';
            while (nl && (name[nl-1] == ' ' || name[nl-1] == '\t'))
                name[--nl] = '\0';
            hx_cookie_set(name, val);
        }

        p = (*sc == ';') ? sc + 1 : sc;
        while (*p == ' ') ++p;
    }
    return g_ck_n > 0;
}

/* --------------------------------------------------------------------------
 * curl 回调
 * ----------------------------------------------------------------------- */

/* "ttwid=abc; Path=/; Max-Age=..." -> 存下 ttwid。
 * 只取第一个 '=' 之前当名字、第一个 ';' 之前当值 —— 值里可以有 '='。 */
static void ck_parse_set_cookie(const char *v)
{
    char name[CK_NAME], val[CK_VAL];
    const char *eq = strchr(v, '=');
    const char *sc;

    if (!eq)
        return;
    sc = strchr(v, ';');
    if (!sc)
        sc = v + strlen(v);
    if (eq >= sc)
        return;
    {
        size_t nl = (size_t)(eq - v), vl = (size_t)(sc - eq - 1);
        if (nl == 0 || nl >= CK_NAME || vl >= CK_VAL)
            return;
        memcpy(name, v, nl); name[nl] = '\0';
        memcpy(val, eq + 1, vl); val[vl] = '\0';
    }
    hx_cookie_set(name, val);
}

/* 一次传输的上下文。**body 回调与 header 回调共用它** ——
 * 跟重定向时要同时知道"下一跳去哪"（Location 从响应头来）和"正文给谁"。 */
typedef struct {
    HxSink fn;
    void  *user;
    char   location[1024];
} XferCtx;

static size_t on_header(char *ptr, size_t sz, size_t nm, void *u)
{
    XferCtx *xc = (XferCtx *)u;
    size_t got = sz * nm;
    char   line[1024];
    size_t n = got < sizeof line - 1 ? got : sizeof line - 1;

    memcpy(line, ptr, n);
    line[n] = '\0';
    while (n && (line[n-1] == '\r' || line[n-1] == '\n'))
        line[--n] = '\0';

    if (strncasecmp(line, "Set-Cookie:", 11) == 0) {
        const char *v = line + 11;
        while (*v == ' ' || *v == '\t') ++v;
        ck_parse_set_cookie(v);
    } else if (xc && strncasecmp(line, "Location:", 9) == 0) {
        const char *v = line + 9;
        while (*v == ' ' || *v == '\t') ++v;
        snprintf(xc->location, sizeof xc->location, "%s", v);
    }
    return got;
}

static size_t on_body(char *ptr, size_t sz, size_t nm, void *u)
{
    XferCtx *xc = (XferCtx *)u;
    size_t   got = sz * nm;

    if (!xc || !xc->fn)
        return got;                 /* 只要状态码的调用 */
    return xc->fn(ptr, got, xc->user);
}

/* --------------------------------------------------------------------------
 * init / CA
 * ----------------------------------------------------------------------- */

/* 定义在下面的"长连接池"一节；hx_exit 要先用它。 */
static void pool_free_all(void);

bool hx_init(void)
{
    if (g_inited)
        return true;
    if (curl_global_init(CURL_GLOBAL_DEFAULT) != 0)
        return false;
    g_inited = true;
    return true;
}

void hx_exit(void)
{
    if (!g_inited)
        return;
    /* 先放连接，再 global_cleanup：反过来的话 curl 会在已销毁的全局态里
     * 去关 socket，3DS 上表现为退出时卡一下甚至崩溃。 */
    pool_free_all();
    curl_global_cleanup();
    g_inited = false;
}

const char *hx_version(void)
{
    return curl_version();
}

static void apply_ca(CURL *e)
{
    FILE *f = fopen(HX_CA_OVERRIDE, "rb");

    if (f) {
        /* 卡上有就用卡上的。**顺序是刻意的**：本机开发时必须能覆盖，
         * 否则卡巴斯基中间人一来，连豆包都连不上，什么都验不了。 */
        fclose(f);
        curl_easy_setopt(e, CURLOPT_CAINFO, HX_CA_OVERRIDE);
        return;
    }

    if (cacert_pem_size > 0) {
        static struct curl_blob blob;   /* curl 只用不释放，静态存活即可 */
        blob.data  = (void *)cacert_pem;
        blob.len   = cacert_pem_size;
        blob.flags = CURL_BLOB_NOCOPY;
        curl_easy_setopt(e, CURLOPT_CAINFO_BLOB, &blob);
    }
    /* 两边都没有时什么都不设 —— curl 会因为没有 CA 而失败，
     * 报出来的错误码(-60/-77)本身就说明了问题，比我们瞎编一句更好。 */
}

/* --------------------------------------------------------------------------
 * 长连接池（2026-10-24）
 *
 * ── 为什么要有这个 ──
 * 原来每条请求都是 curl_easy_init → perform → curl_easy_cleanup。
 * `curl_easy_cleanup` 会把 handle 名下的连接缓存一起扔掉，于是**每按一次
 * ZL，3DS 都要重走一遍** DNS 解析 → TCP 三次握手 → TLS 1.2 握手。
 * 对桌面机这是一两百毫秒；对 ARM11 上的 mbedtls（还要校验整条证书链），
 * 这是**秒级**的纯等待，而且发生在"用户已经说完话、正等着出字"的那一刻。
 *
 * 复用同一个 easy handle 能一次省掉三样东西，因为 curl 的连接缓存、
 * DNS 缓存、TLS 会话缓存**都挂在 handle 上**：
 *   · DNS：同一主机名不再去问 resolver；
 *   · TCP：连接还在，直接接着用（HTTP keep-alive）；
 *   · TLS：走 session resumption，省掉非对称运算 —— 这条收益最大。
 *
 * ── 分寸 ──
 * 池子很小（4 个），按主机名匹配，**busy 标记独占**：同主机并发两条时会
 * 各自新开 handle，不会两条请求共用一个连接（那会把响应搅在一起）。
 * 连接坏了（perform 失败）就丢掉，绝不把坏连接留给下一条请求 ——
 * 否则用户会遇到"第一次好、第二次必失败"这种最难查的状态。
 * ----------------------------------------------------------------------- */
#define HX_POOL_N 4

static struct {
    char  host[80];
    CURL *e;
    bool  busy;
} g_pool[HX_POOL_N];

/* 服务端空闲多久会把连接掐掉，我们不知道；给 CURLE 一个保守的上限，
 * 免得拿一条**已经被对端关掉**的连接去发请求（那会白等一个 RTT 再重连）。 */
/* ★ 2026-10-25 从 115s 压到 25s。
 *   服务端的空闲连接超时普遍是 30~60 秒，我们留着超过这个时间的连接，
 *   等到的不是"复用"，而是**一条对端已经关掉的半死连接** ——
 *   往里写请求可能"成功"，然后一直等响应直到超时（真机表现：卡 9~12 秒后说超时）。
 *   25 秒覆盖"连着说两句话"这个真实场景，够了。 */
#define HX_POOL_MAXAGE 25L

static void pool_init_opts(CURL *e)
{
    curl_easy_setopt(e, CURLOPT_TCP_KEEPALIVE, 1L);
    curl_easy_setopt(e, CURLOPT_TCP_KEEPIDLE, 30L);
    curl_easy_setopt(e, CURLOPT_TCP_KEEPINTVL, 20L);
    curl_easy_setopt(e, CURLOPT_DNS_CACHE_TIMEOUT, 600L);
    curl_easy_setopt(e, CURLOPT_MAXAGE_CONN, HX_POOL_MAXAGE);
}

/* 从 URL 里抠主机名。只认 http(s)://host[:port]/…，失败返回 NULL。 */
static const char *url_host(const char *url, char *out, size_t cap)
{
    const char *p = strstr(url, "://");
    size_t n;

    if (!p)
        return NULL;
    p += 3;
    n = strcspn(p, "/:?#");
    if (n == 0 || n >= cap)
        return NULL;
    memcpy(out, p, n);
    out[n] = '\0';
    return out;
}

/* 取出可复用的 handle（同时置 busy）。没有就返回 NULL。 */
static CURL *pool_take(const char *host)
{
    for (int i = 0; i < HX_POOL_N; ++i) {
        if (g_pool[i].e && !g_pool[i].busy && strcmp(g_pool[i].host, host) == 0) {
            g_pool[i].busy = true;
            return g_pool[i].e;
        }
    }
    return NULL;
}

/* 把用成功的 handle 放进池（新 handle）或解锁（池里取出来的）。 */
static void pool_release(const char *host, CURL *e)
{
    for (int i = 0; i < HX_POOL_N; ++i) {
        if (g_pool[i].e == e) {
            g_pool[i].busy = false;
            return;
        }
    }
    for (int i = 0; i < HX_POOL_N; ++i) {
        if (!g_pool[i].e) {
            g_pool[i].e    = e;
            g_pool[i].busy = false;
            snprintf(g_pool[i].host, sizeof g_pool[i].host, "%s", host);
            return;
        }
    }
    /* 池满：这条连接不要了，否则句柄泄漏。 */
    curl_easy_cleanup(e);
}

/* 丢弃：坏连接绝不留在池里。 */
static void pool_drop(CURL *e)
{
    for (int i = 0; i < HX_POOL_N; ++i) {
        if (g_pool[i].e == e) {
            curl_easy_cleanup(g_pool[i].e);
            g_pool[i].e    = NULL;
            g_pool[i].busy = false;
            g_pool[i].host[0] = '\0';
            return;
        }
    }
    curl_easy_cleanup(e);
}

static void pool_free_all(void)
{
    for (int i = 0; i < HX_POOL_N; ++i) {
        if (g_pool[i].e)
            curl_easy_cleanup(g_pool[i].e);
        g_pool[i].e    = NULL;
        g_pool[i].busy = false;
        g_pool[i].host[0] = '\0';
    }
}

/* 最近一次请求的阶段耗时（秒）。**这是给验收用的尺子**：
 * 光说"更快了"没有意义，要看 dns/conn/tls 这三段各自还剩多少。 */
static double g_t_dns, g_t_conn, g_t_tls, g_t_total;

void hx_last_timing(double *dns, double *conn, double *tls, double *total)
{
    if (dns)   *dns   = g_t_dns;
    if (conn)  *conn  = g_t_conn;
    if (tls)   *tls   = g_t_tls;
    if (total) *total = g_t_total;
}

/* --------------------------------------------------------------------------
 * 发请求
 * ----------------------------------------------------------------------- */

long hx_perform(const HxReq *req, HxSink on_body_fn, void *user,
                char *err, size_t errcap)
{
    char      cur[1024];
    long      http_code = 0;
    XferCtx   xc;
    int       hops;
    int       max_hops;

    if (err && errcap)
        err[0] = '\0';
    if (!req || !req->url) {
        if (err) snprintf(err, errcap, "没有 URL");
        return -1;
    }
    if (!hx_init()) {
        if (err) snprintf(err, errcap, "curl 起不来");
        return -1;
    }

    snprintf(cur, sizeof cur, "%s", req->url);
    xc.fn   = on_body_fn;
    xc.user = user;
    xc.location[0] = '\0';

    max_hops = req->follow ? 6 : 1;

    for (hops = 0; hops < max_hops; ++hops) {
        CURLcode rc;
        struct curl_slist *hl = NULL;
        char  ck[8192];
        char  host[80];
        bool  poolable, fresh;
        CURL *e;

        /* 单跳 + 调用方明确要求复用，才进池。重定向链每跳主机都不同，
         * 共用 handle 只会互相踩；多跳的仍然一条一清。 */
        poolable = req->persist && max_hops == 1 &&
                   url_host(cur, host, sizeof host) != NULL;

        e = poolable ? pool_take(host) : NULL;
        fresh = (e == NULL);
        if (!e)
            e = curl_easy_init();

        if (!e) {
            if (err) snprintf(err, errcap, "curl_easy_init 失败");
            return -1;
        }
        if (poolable && fresh)
            pool_init_opts(e);

        http_code = 0;
        xc.location[0] = '\0';

        /* 第 2 跳起是重定向：让调用方清掉它自己的接收缓冲，
         * 否则 302 那页的正文会混进最终结果（见 httpx.h 的说明）。 */
        if (hops > 0 && req->on_new_response)
            req->on_new_response(xc.user);

        curl_easy_setopt(e, CURLOPT_URL, cur);
        curl_easy_setopt(e, CURLOPT_USERAGENT, "Mozilla/5.0 (Nintendo 3DS)");
        apply_ca(e);
        curl_easy_setopt(e, CURLOPT_SSL_VERIFYPEER, 1L);
        curl_easy_setopt(e, CURLOPT_SSL_VERIFYHOST, 2L);
        curl_easy_setopt(e, CURLOPT_FOLLOWLOCATION, 0L);   /* 自己跟，见下 */
        curl_easy_setopt(e, CURLOPT_TIMEOUT,
                         req->timeout_s > 0 ? req->timeout_s : 30L);
        /* 连接超时必须**小于**总超时，否则它是死的：
         * 原来写 20L 而 ASR 的总超时是 12s（见 asr.c），
         * 于是"域名压根连不通"这种情况必定是先耗尽总超时才收场，
         * CONNECTTIMEOUT 一次也触发不到 —— 报错永远是同一个 -28
         * "操作超时"，看不出到底是握手慢还是整个卡住。
         * 8s 够 3DS 走完一次 WiFi+DNS+TCP+TLS1.2 握手，
         * 超了就直接告诉用户"没连上"，别让他干等满 12s。 */
        curl_easy_setopt(e, CURLOPT_CONNECTTIMEOUT, 8L);
        curl_easy_setopt(e, CURLOPT_WRITEFUNCTION, on_body);
        curl_easy_setopt(e, CURLOPT_WRITEDATA, &xc);
        /* 接收缓冲：只给调用方指定了才设。分寸见 httpx.h 的说明 ——
         * 大缓冲摊薄系统调用（默认 16 KB 意味着每 16 KB 一次 write 系统调用），
         * 小缓冲省内存。所以这个值归调用方按型号定，不在这里写死一个"快"的。 */
        if (req->buf_size > 0)
            curl_easy_setopt(e, CURLOPT_BUFFERSIZE, req->buf_size);
        curl_easy_setopt(e, CURLOPT_HEADERFUNCTION, on_header);
        curl_easy_setopt(e, CURLOPT_HEADERDATA, &xc);
        curl_easy_setopt(e, CURLOPT_NOSIGNAL, 1L);

        if (req->add_cookies) {
            hx_cookie_string(ck, sizeof ck);
            if (ck[0])
                curl_easy_setopt(e, CURLOPT_COOKIE, ck);
        } else if (poolable) {
            /* 复用的 handle 上还记着上一条请求的 Cookie。不清的话，
             * 一条"不要带 cookie"的请求会**悄悄把上次的登录态带上**。 */
            curl_easy_setopt(e, CURLOPT_COOKIE, "");
        }

        if (req->content_type)
            hl = curl_slist_append(hl, req->content_type);

        if (req->headers) {
            for (int i = 0; req->headers[i]; ++i)
                hl = curl_slist_append(hl, req->headers[i]);
        }
        /* ★ 无论有没有头都要设：池子里的 handle 可能还指向上一条请求
         * 已经 free 掉的 slist，那是**野指针**。这条 if 不能省。 */
        if (hl || poolable)
            curl_easy_setopt(e, CURLOPT_HTTPHEADER, hl);

        /* POST 只在**第一跳**发。重定向一律变成 GET —— 这是 HTTP 语义，
         * 也是为了跟服务端行为一致（3DS 上的表现与浏览器相同）。 */
        if (req->post_raw && req->post_raw_len > 0 && hops == 0) {
            curl_easy_setopt(e, CURLOPT_POST, 1L);
            curl_easy_setopt(e, CURLOPT_POSTFIELDS, req->post_raw);
            curl_easy_setopt(e, CURLOPT_POSTFIELDSIZE, (long)req->post_raw_len);
        } else if (req->post && hops == 0) {
            curl_easy_setopt(e, CURLOPT_POST, 1L);
            curl_easy_setopt(e, CURLOPT_POSTFIELDS, req->post);
            curl_easy_setopt(e, CURLOPT_POSTFIELDSIZE,
                             (long)strlen(req->post));
        } else if (poolable) {
            /* 同上的反面：上一条是 POST，这一条是 GET，
             * 不显式复位就会**带着旧的 body 去发 GET**。 */
            curl_easy_setopt(e, CURLOPT_HTTPGET, 1L);
        }

        rc = curl_easy_perform(e);
        if (rc == CURLE_OK) {
            curl_easy_getinfo(e, CURLINFO_RESPONSE_CODE, &http_code);
            curl_easy_getinfo(e, CURLINFO_NAMELOOKUP_TIME, &g_t_dns);
            curl_easy_getinfo(e, CURLINFO_CONNECT_TIME,    &g_t_conn);
            curl_easy_getinfo(e, CURLINFO_APPCONNECT_TIME, &g_t_tls);
            curl_easy_getinfo(e, CURLINFO_TOTAL_TIME,      &g_t_total);
        }

        /* 顺序是刻意的：先把 handle 上的 HTTPHEADER 摘掉，再释放 slist。
         * 反过来的话，池里的 handle 会留着一个已释放的指针。 */
        if (poolable)
            curl_easy_setopt(e, CURLOPT_HTTPHEADER, (struct curl_slist *)NULL);
        if (hl)
            curl_slist_free_all(hl);

        if (poolable) {
            if (rc == CURLE_OK || rc == CURLE_HTTP_RETURNED_ERROR)
                pool_release(host, e);      /* 连接是好的，留着下次用 */
            else
                pool_drop(e);               /* 坏连接绝不回流 */
        } else {
            curl_easy_cleanup(e);
        }

        if (rc != CURLE_OK) {
            /* 负返回值 = curl 层失败。调用方据此把"没连上"和
             * "服务端说不行"分开 —— 这两种的应对方式完全不同。 */
            if (err)
                snprintf(err, errcap, "%s（%d）", curl_easy_strerror(rc), (int)rc);
            return -(long)rc;
        }

        /* 重定向是**自己跳**的（FOLLOWLOCATION 关掉了）：
         * 这样每一跳的响应头都走同一个 on_header，Set-Cookie 一个不漏。
         * 让 curl 自己跟的话，中间那些 302 的 Set-Cookie 有可能被吞掉 ——
         * 而豆包登录**恰恰是靠 302 一路设 cookie** 的，吞一个就登不上。 */
        if (req->follow && http_code >= 300 && http_code < 400 &&
            xc.location[0]) {
            snprintf(cur, sizeof cur, "%s", xc.location);
            continue;
        }
        return http_code;
    }

    /* 跳数用完仍在重定向：返回最后那个状态码，让调用方看见。
     * 不另编错误信息 —— 3xx 本身就说明"服务端还在让我们去别处"，
     * 这个事实比一句自造的"失败"有用。 */
    return http_code;
}
