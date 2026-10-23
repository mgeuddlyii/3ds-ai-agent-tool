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
        CURL *e = curl_easy_init();
        CURLcode rc;
        struct curl_slist *hl = NULL;
        char  ck[8192];

        if (!e) {
            if (err) snprintf(err, errcap, "curl_easy_init 失败");
            return -1;
        }

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
        }

        if (req->content_type)
            hl = curl_slist_append(hl, req->content_type);

        if (req->headers) {
            for (int i = 0; req->headers[i]; ++i)
                hl = curl_slist_append(hl, req->headers[i]);
            curl_easy_setopt(e, CURLOPT_HTTPHEADER, hl);
        }

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
        }

        rc = curl_easy_perform(e);
        if (rc == CURLE_OK)
            curl_easy_getinfo(e, CURLINFO_RESPONSE_CODE, &http_code);

        if (hl)
            curl_slist_free_all(hl);
        curl_easy_cleanup(e);

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
