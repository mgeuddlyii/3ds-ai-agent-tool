/* -------------------------------------------------------------------------
 * doubao probe - end-to-end Doubao access from a 3DS, with no relay server.
 *
 * Why this exists
 *   The previous assumption was that Doubao needs an a_bogus signature that
 *   only a browser can produce, which would force a server into the design.
 *   Measurement showed otherwise:
 *     * x-flow-trace is a locally generated pair of random uuids, not a
 *       signature (confirmed against doubao-free-api's B3 path).
 *     * device_id / tea_uuid / web_id are not validated pre-login: four
 *       different param sets all produced byte-identical answers.
 *     * /samantha/conversation/list answers "登录已过期" pre-login, i.e. the
 *       gateway accepted the request and only the session is missing.
 *   So the only thing a 3DS cannot mint by itself is the login session, and
 *   QR login is plain HTTP.
 *
 * What it does
 *   1. GET /                                  -> ttwid cookie + web_id
 *   2. GET /passport/safe/csrf_token/         -> passport csrf token
 *   3. GET /passport/web/get_qrcode/          -> token + base64 PNG
 *      The PNG is written to disk as base64; the PC side renders it.
 *      (Decoding PNG on the 3DS would be pure waste.)
 *   4. poll /passport/web/check_qrconnect/    -> scan with the Douyin app
 *   5. once confirmed, save the cookie and hit:
 *        /samantha/conversation/list   (history)
 *        /samantha/chat/completion     (chat)
 *
 * Everything lands in /3ds/aiconsole/doubao/ as small text files, which is
 * how the PC-side driver script follows along.
 * ------------------------------------------------------------------------- */

#include <3ds.h>
#include <curl/curl.h>

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <malloc.h>
#include <sys/stat.h>

#define BASE "https://www.doubao.com"
#define AID  "497858"
#define UA   "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 " \
             "(KHTML, like Gecko) Chrome/135.0.0.0 Safari/537.36"
#define DIR  "/3ds/aiconsole/doubao/"
#define CACERT "/config/ssl/cacert.pem"

/* next= parameter, pre-encoded. The reference implementation passes the bare
 * origin here; anything else makes check_qrconnect answer "缺少参数". */
#define NEXT_ENC "https%3A%2F%2Fwww.doubao.com"

/* The QR itself carries an expire_time roughly 15 minutes out; poll for as
 * long so a human has time to actually scan it. */
#define POLL_SECONDS 900

/* ------------------------------------------------------------------ utils */

typedef struct {
    char  *p;
    size_t n, cap;
} Buf;

static void buf_init(Buf *b)
{
    b->cap = 8192;
    b->n = 0;
    b->p = (char *)malloc(b->cap);
    b->p[0] = 0;
}

static void buf_free(Buf *b) { free(b->p); b->p = NULL; b->n = b->cap = 0; }

static void buf_add(Buf *b, const char *s, size_t len)
{
    if (b->n + len + 1 > b->cap) {
        while (b->n + len + 1 > b->cap) b->cap *= 2;
        b->p = (char *)realloc(b->p, b->cap);
        if (!b->p) return;
    }
    memcpy(b->p + b->n, s, len);
    b->n += len;
    b->p[b->n] = 0;
}

static size_t sink_body(char *ptr, size_t sz, size_t nm, void *u)
{
    size_t got = sz * nm;
    buf_add((Buf *)u, ptr, got);
    return got;
}

/* ------------------------------------------------------------- cookie jar */

#define CK_MAX 32

static char ck_name[CK_MAX][48];
static char ck_val[CK_MAX][512];
static int  ck_n;

static void ck_set(const char *name, const char *val)
{
    int i;
    if (!name[0]) return;
    for (i = 0; i < ck_n; i++) {
        if (!strcmp(ck_name[i], name)) {
            snprintf(ck_val[i], sizeof(ck_val[i]), "%s", val);
            return;
        }
    }
    if (ck_n >= CK_MAX) return;
    snprintf(ck_name[ck_n], sizeof(ck_name[ck_n]), "%s", name);
    snprintf(ck_val[ck_n], sizeof(ck_val[ck_n]), "%s", val);
    ck_n++;
}

/* "ttwid=abc; Path=/; Max-Age=..." -> store ttwid */
static void ck_parse_set(const char *line)
{
    const char *p = line;
    char name[48], val[512];
    const char *eq, *sc;

    while (*p == ' ' || *p == '\t') p++;
    eq = strchr(p, '=');
    if (!eq) return;
    sc = strchr(p, ';');
    if (!sc) sc = p + strlen(p);
    if (eq >= sc) return;

    size_t nl = eq - p;
    if (nl >= sizeof(name)) return;
    memcpy(name, p, nl); name[nl] = 0;

    eq++;
    size_t vl = (size_t)(sc - eq);
    if (vl >= sizeof(val)) return;
    memcpy(val, eq, vl); val[vl] = 0;

    ck_set(name, val);
}

static char g_location[1024];

static size_t sink_header(char *ptr, size_t sz, size_t nm, void *u)
{
    size_t got = sz * nm;
    char line[1024];
    size_t n = got < sizeof(line) - 1 ? got : sizeof(line) - 1;
    memcpy(line, ptr, n);
    line[n] = 0;
    while (n > 0 && (line[n - 1] == '\r' || line[n - 1] == '\n')) line[--n] = 0;

    if (!strncasecmp(line, "Set-Cookie:", 11))
        ck_parse_set(line + 11);
    else if (!strncasecmp(line, "Location:", 9)) {
        const char *v = line + 9;
        while (*v == ' ' || *v == '\t') v++;
        snprintf(g_location, sizeof(g_location), "%s", v);
    }
    (void)u;
    return got;
}

static void ck_string(char *out, size_t outsz)
{
    int i;
    out[0] = 0;
    for (i = 0; i < ck_n; i++) {
        if (i) strncat(out, "; ", outsz - strlen(out) - 1);
        strncat(out, ck_name[i], outsz - strlen(out) - 1);
        strncat(out, "=", outsz - strlen(out) - 1);
        strncat(out, ck_val[i], outsz - strlen(out) - 1);
    }
}

static const char *ck_get(const char *name)
{
    int i;
    for (i = 0; i < ck_n; i++)
        if (!strcmp(ck_name[i], name)) return ck_val[i];
    return "";
}

/* Load a "name=value; name=value" line that the PC side dropped on the SD
 * card. Lets the 3DS skip a scan when a session already exists. */
static int ck_load_file(const char *path)
{
    FILE *f = fopen(path, "r");
    char line[8192];
    char *p, *eq, *sc;
    int got = 0;

    if (!f) return 0;
    if (!fgets(line, sizeof(line), f)) { fclose(f); return 0; }
    fclose(f);

    p = line;
    while (*p) {
        while (*p == ' ' || *p == ';' || *p == '\t') p++;
        if (!*p) break;
        eq = strchr(p, '=');
        sc = strchr(p, ';');
        if (!eq || (sc && eq > sc)) break;      /* malformed, stop */
        *eq = 0;
        if (sc) *sc = 0;
        ck_set(p, eq + 1);
        got++;
        p = sc ? sc + 1 : p + strlen(p);
    }
    return got;
}

/* --------------------------------------------------------------- tiny json */

/* Copy the string value of "key":"..." into out. Returns 1 on success.
 * Good enough for flat JSON; no nesting support needed here. */
static int json_str(const char *json, const char *key, char *out, size_t outsz)
{
    char needle[64];
    const char *p, *q;
    size_t i, o;

    snprintf(needle, sizeof(needle), "\"%s\":\"", key);
    p = strstr(json, needle);
    if (!p) return 0;
    p += strlen(needle);

    o = 0;
    for (i = 0; p[i] && o + 1 < outsz; i++) {
        char c = p[i];
        if (c == '\\') {
            i++;
            if (!p[i]) break;
            c = p[i];
            if (c == 'n') c = '\n';
            else if (c == 't') c = '\t';
            else if (c == 'r') c = '\r';
            else if (c == 'u') { out[o++] = '?'; i += 4; continue; }
        } else if (c == '"') {
            break;
        }
        out[o++] = c;
    }
    out[o] = 0;
    (void)q;
    return o > 0;
}

/* ------------------------------------------------------------------ output */

static FILE *g_log;

static void say(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    if (g_log) {
        va_start(ap, fmt);
        vfprintf(g_log, fmt, ap);
        va_end(ap);
        fflush(g_log);
    }
}

static void put_file(const char *name, const char *content)
{
    char path[256];
    FILE *f;
    snprintf(path, sizeof(path), "%s%s", DIR, name);
    f = fopen(path, "w");
    if (!f) { say("!! cannot write %s\n", path); return; }
    fputs(content, f);
    fflush(f);
    fclose(f);
}

static void set_status(const char *s) { put_file("status.txt", s); }

/* ------------------------------------------------------------------- http */

/* GET / answers 302 before serving the real page, so redirects are followed
 * by hand - CURLOPT_FOLLOWLOCATION is left off to keep cookie handling
 * explicit and to stay close to what the 3DS app will eventually do. */
static long http_req(const char *url, const char *post,
                     struct curl_slist *headers, Buf *body, long timeout_s)
{
    long http_code;
    int hops;
    char cur[1024], cookie[4096];

    snprintf(cur, sizeof(cur), "%s", url);

    for (hops = 0; hops < 6; hops++) {
        CURL *e = curl_easy_init();
        CURLcode rc;

        if (!e) return -1;
        http_code = 0;
        g_location[0] = 0;
        ck_string(cookie, sizeof(cookie));

        if (hops > 0) {
            /* Body of a redirect is noise; keep only the final payload. */
            body->n = 0;
            body->p[0] = 0;
        }

        curl_easy_setopt(e, CURLOPT_URL, cur);
        curl_easy_setopt(e, CURLOPT_USERAGENT, UA);
        curl_easy_setopt(e, CURLOPT_CAINFO, CACERT);
        curl_easy_setopt(e, CURLOPT_SSL_VERIFYPEER, 1L);
        curl_easy_setopt(e, CURLOPT_SSL_VERIFYHOST, 2L);
        curl_easy_setopt(e, CURLOPT_FOLLOWLOCATION, 0L);
        curl_easy_setopt(e, CURLOPT_TIMEOUT, timeout_s);
        curl_easy_setopt(e, CURLOPT_CONNECTTIMEOUT, 20L);
        curl_easy_setopt(e, CURLOPT_WRITEFUNCTION, sink_body);
        curl_easy_setopt(e, CURLOPT_WRITEDATA, body);
        curl_easy_setopt(e, CURLOPT_HEADERFUNCTION, sink_header);
        if (cookie[0]) curl_easy_setopt(e, CURLOPT_COOKIE, cookie);
        if (headers) curl_easy_setopt(e, CURLOPT_HTTPHEADER, headers);
        if (post && hops == 0) {
            curl_easy_setopt(e, CURLOPT_POST, 1L);
            curl_easy_setopt(e, CURLOPT_POSTFIELDS, post);
        }

        rc = curl_easy_perform(e);
        if (rc == CURLE_OK)
            curl_easy_getinfo(e, CURLINFO_RESPONSE_CODE, &http_code);
        else
            http_code = -rc;
        curl_easy_cleanup(e);

        if (http_code >= 300 && http_code < 400 && g_location[0]) {
            snprintf(cur, sizeof(cur), "%s", g_location);
            post = NULL;               /* a redirect is always a GET */
            continue;
        }
        return http_code;
    }
    return http_code;
}

/* ------------------------------------------------------------------ steps */

static char g_web_id[64];
static char g_qr_token[256];
static char g_csrf[256];

static int step_bootstrap(void)
{
    Buf b;
    long st;
    buf_init(&b);
    set_status("bootstrap");

    st = http_req(BASE "/", NULL, NULL, &b, 30);
    say("GET /            -> %ld  %u bytes\n", st, (unsigned)b.n);
    if (st != 200 && st != 302) { buf_free(&b); return 0; }

    /* web_id lives in the SSR payload: "web_id":"7691740..." */
    {
        const char *p = strstr(b.p, "\"web_id\":\"");
        if (p) {
            p += 10;
            size_t i = 0;
            while (p[i] && p[i] != '"' && i + 1 < sizeof(g_web_id)) {
                g_web_id[i] = p[i];
                i++;
            }
            g_web_id[i] = 0;
        }
    }
    say("  ttwid  = %.24s...\n", ck_get("ttwid"));
    say("  web_id = %s\n", g_web_id);
    buf_free(&b);
    return g_web_id[0] != 0;
}

static int step_csrf(void)
{
    Buf b;
    long st;
    buf_init(&b);
    st = http_req(BASE "/passport/safe/csrf_token/?aid=" AID, NULL, NULL, &b, 30);
    say("GET csrf_token   -> %ld\n", st);
    if (st == 200) {
        /* The field is "passport_csrf_token" - note that searching for
         * "token" cannot match it, because the quote sits before
         * "passport_csrf_". */
        if (!json_str(b.p, "passport_csrf_token", g_csrf, sizeof(g_csrf)))
            json_str(b.p, "token", g_csrf, sizeof(g_csrf));
        if (!g_csrf[0])
            say("  raw: %.300s\n", b.p);
    }
    say("  csrf = %.24s...\n", g_csrf);
    buf_free(&b);
    return g_csrf[0] != 0;
}

static int step_qrcode(void)
{
    Buf b;
    long st;
    char url[512], hdr_csrf[512];
    char img[65536];
    struct curl_slist *hl = NULL;

    /* NEXT_ENC contains '%' so it must never sit in a format string. */
    snprintf(url, sizeof(url),
             "%s/passport/web/get_qrcode/?next=%s&aid=%s", BASE, NEXT_ENC, AID);
    snprintf(hdr_csrf, sizeof(hdr_csrf), "x-tt-passport-csrf-token: %s", g_csrf);
    hl = curl_slist_append(hl, hdr_csrf);
    hl = curl_slist_append(hl, "Referer: " BASE "/chat/login");
    hl = curl_slist_append(hl, "Origin: " BASE);

    buf_init(&b);
    st = http_req(url, NULL, hl, &b, 30);
    say("GET get_qrcode   -> %ld  %u bytes\n", st, (unsigned)b.n);

    if (st == 200) {
        if (json_str(b.p, "token", g_qr_token, sizeof(g_qr_token))) {
            say("  qr token = %.20s...\n", g_qr_token);
            put_file("qr_token.txt", g_qr_token);
        }
        /* The field is "qrcode" (a data: URI holding a base64 PNG), and it
         * sits under "data", not at the top level. */
        if (json_str(b.p, "qrcode", img, sizeof(img))) {
            const char *comma = strchr(img, ',');
            if (comma && !strncmp(img, "data:", 5)) {
                char tmp[65536];
                snprintf(tmp, sizeof(tmp), "%s", comma + 1);
                put_file("qr_b64.txt", tmp);
            } else {
                put_file("qr_b64.txt", img);
            }
            say("  qr image base64 written (%u chars)\n",
                (unsigned)strlen(img));
        } else {
            say("  !! no image field: %.200s\n", b.p);
        }
    } else {
        say("  !! %.200s\n", b.p);
    }
    curl_slist_free_all(hl);
    buf_free(&b);
    return g_qr_token[0] != 0;
}

/* Returns 1 once the scan is confirmed. */
static int step_wait_scan(void)
{
    int waited = 0;
    char url[512], hdr_csrf[512];
    struct curl_slist *hl = NULL;
    set_status("waiting_scan");

    /* next= is required; without it the endpoint answers error_code 3
     * ("缺少参数") and never reports a status. */
    snprintf(url, sizeof(url),
             "%s/passport/web/check_qrconnect/?next=%s&token=%s&aid=%s",
             BASE, NEXT_ENC, g_qr_token, AID);
    snprintf(hdr_csrf, sizeof(hdr_csrf), "x-tt-passport-csrf-token: %s", g_csrf);
    hl = curl_slist_append(hl, hdr_csrf);
    hl = curl_slist_append(hl, "Referer: " BASE "/chat/login");
    hl = curl_slist_append(hl, "Origin: " BASE);

    while (waited < POLL_SECONDS) {
        Buf b;
        long st;
        char status[64];
        status[0] = 0;

        buf_init(&b);
        st = http_req(url, NULL, hl, &b, 20);
        if (st == 200) {
            if (!json_str(b.p, "status", status, sizeof(status))) {
                if (strstr(b.p, "confirmed")) snprintf(status, sizeof(status),
                                                       "confirmed");
            }
        }
        say("  poll %3ds  http=%ld  status=%s\n", waited, st,
            status[0] ? status : "?");
        if (status[0]) {
            char line[128];
            snprintf(line, sizeof(line), "waiting_scan:%s", status);
            set_status(line);
        }

        if (!strcmp(status, "confirmed") || !strcmp(status, "success")) {
            char redirect[1024];
            /* Extract before freeing - the buffer owns the string. */
            redirect[0] = 0;
            json_str(b.p, "redirect_url", redirect, sizeof(redirect));
            buf_free(&b);
            if (redirect[0]) {
                Buf r;
                say("  following redirect to plant the session cookie\n");
                buf_init(&r);
                http_req(redirect, NULL, NULL, &r, 30);
                buf_free(&r);
            }
            curl_slist_free_all(hl);
            return 1;
        }
        if (!strcmp(status, "expired") || !strcmp(status, "canceled") ||
            !strcmp(status, "failed")) {
            buf_free(&b);
            set_status(status);
            curl_slist_free_all(hl);
            return 0;
        }
        buf_free(&b);
        svcSleepThread(2 * 1000 * 1000 * 1000LL);
        waited += 2;
    }
    curl_slist_free_all(hl);
    set_status("scan_timeout");
    return 0;
}

/* Returns 1 when the server answered code=0, i.e. the session is real. */
static int step_history(void)
{
    Buf b;
    long st;
    struct curl_slist *hl = NULL;
    char url[512], post[256], xft[256];
    char cookie[4096];

    ck_string(cookie, sizeof(cookie));
    snprintf(xft, sizeof(xft), "x-flow-trace: {\"trace_id\":\"%08x%08x\","
             "\"span_id\":\"%08x%08x\"}",
             (unsigned)rand(), (unsigned)rand(),
             (unsigned)rand(), (unsigned)rand());

    hl = curl_slist_append(hl, "content-type: application/json");
    hl = curl_slist_append(hl, "accept: text/event-stream");
    hl = curl_slist_append(hl, "agw-js-conv: str");
    hl = curl_slist_append(hl, "Origin: " BASE);
    hl = curl_slist_append(hl, "Referer: " BASE "/chat/");
    hl = curl_slist_append(hl, xft);

    snprintf(url, sizeof(url),
             BASE "/samantha/conversation/list?aid=" AID
             "&device_id=7691730000000000000&device_platform=web&language=zh"
             "&pc_version=3.17.3&pkg_type=release_version&real_aid=" AID
             "&region=CN&samantha_web=1&sys_region=CN"
             "&tea_uuid=7691730000000000000&use-olympus-account=1"
             "&version_code=20800&web_id=%s", g_web_id);
    snprintf(post, sizeof(post), "{\"cursor\":0,\"count\":20}");

    buf_init(&b);
    st = http_req(url, post, hl, &b, 40);
    say("POST conversation/list -> %ld  %u bytes\n", st, (unsigned)b.n);
    say("  %.300s\n", b.p ? b.p : "(empty)");
    put_file("history.txt", b.p ? b.p : "");

    /* Pull a few conversation names out so the report shows it is real. */
    if (b.p) {
        const char *p = b.p;
        int shown = 0;
        while (shown < 5 && (p = strstr(p, "\"name\":\"")) != NULL) {
            char nm[128];
            p += 8;
            if (json_str(p - 8, "name", nm, sizeof(nm))) {
                say("  conv[%d]: %s\n", shown, nm);
                shown++;
            }
            p += 8;
        }
    }

    curl_slist_free_all(hl);
    {
        int ok = (st == 200 && b.p && strstr(b.p, "\"code\":0") != NULL);
        buf_free(&b);
        return ok;
    }
}

static void step_chat(void)
{
    Buf b;
    long st;
    struct curl_slist *hl = NULL;
    char url[512], post[2048], xft[256], lcid[64];

    snprintf(xft, sizeof(xft), "x-flow-trace: {\"trace_id\":\"%08x%08x\","
             "\"span_id\":\"%08x%08x\"}",
             (unsigned)rand(), (unsigned)rand(),
             (unsigned)rand(), (unsigned)rand());
    snprintf(lcid, sizeof(lcid), "local_%u", (unsigned)rand());

    hl = curl_slist_append(hl, "content-type: application/json");
    hl = curl_slist_append(hl, "accept: text/event-stream");
    hl = curl_slist_append(hl, "agw-js-conv: str");
    hl = curl_slist_append(hl, "Origin: " BASE);
    hl = curl_slist_append(hl, "Referer: " BASE "/chat/");
    hl = curl_slist_append(hl, xft);

    snprintf(url, sizeof(url),
             BASE "/samantha/chat/completion?aid=" AID
             "&device_id=7691730000000000000&device_platform=web&language=zh"
             "&pc_version=3.17.3&pkg_type=release_version&real_aid=" AID
             "&region=CN&samantha_web=1&sys_region=CN"
             "&tea_uuid=7691730000000000000&use-olympus-account=1"
             "&version_code=20800&web_id=%s", g_web_id);

    /* bot_id / completion_option / conversation_id are mandatory: without
     * them the server answers 710020702 "系统错误". That is body validation,
     * NOT a signature block - easy to misread as "a_bogus required". */
    snprintf(post, sizeof(post),
             "{\"bot_id\":\"7338286299411103781\","
             "\"completion_option\":{\"is_regen\":false,\"with_suggest\":false,"
             "\"need_create_conversation\":true,\"launch_stage\":1,"
             "\"use_auto_cot\":false,\"use_deep_think\":false},"
             "\"conversation_id\":\"0\","
             "\"local_conversation_id\":\"%s\","
             "\"local_message_id\":\"%08x%08x\","
             "\"messages\":[{\"content\":\"{\\\"text\\\":\\\"用一句话回答：1+1等于几？\\\"}\","
             "\"content_type\":2001,\"attachments\":[],\"references\":[]}],"
             "\"ext\":{}}",
             lcid, (unsigned)rand(), (unsigned)rand());

    buf_init(&b);
    st = http_req(url, post, hl, &b, 60);
    say("POST chat/completion -> %ld  %u bytes\n", st, (unsigned)b.n);
    say("  %.400s\n", b.p);
    put_file("chat.txt", b.p);
    curl_slist_free_all(hl);
    buf_free(&b);
}

/* ------------------------------------------------------------------- main */

static void worker(void *arg)
{
    char cookie[4096];
    (void)arg;

    say("=== doubao probe ===\n");
    say("curl: %s\n", curl_version());

    /* A session the PC already negotiated lets us exercise chat + history
     * without making a human scan again. */
    if (ck_load_file(DIR "cookie.txt") && ck_get("sessionid")[0]) {
        say("loaded session from cookie.txt (%d pairs)\n", ck_n);
        say("  sessionid = %.16s...\n", ck_get("sessionid"));

        /* Only trust it if the server agrees. */
        if (step_history()) {
            say("server accepted the session -> skipping QR login\n");
            set_status("logged_in_cached");
            step_chat();
            set_status("done");
            goto done;
        }
        say("cached session rejected -> falling back to QR login\n");
        ck_n = 0;
    }

    if (!step_bootstrap()) { say("!! bootstrap failed\n"); goto done; }
    if (!step_csrf())      { say("!! csrf failed\n");      goto done; }
    if (!step_qrcode())    { say("!! qrcode failed\n");    goto done; }

    set_status("qr_ready");
    say("\n>>> QR ready. Scan it with the Douyin app. <<<\n");

    if (!step_wait_scan()) { say("!! scan not completed\n"); goto done; }

    ck_string(cookie, sizeof(cookie));
    say("\n=== logged in ===\n");
    say("cookie: %.200s\n", cookie);
    put_file("cookie.txt", cookie);
    set_status("logged_in");

    step_history();
    step_chat();
    set_status("done");

done:
    say("=== end ===\n");
    if (g_log) { fclose(g_log); g_log = NULL; }
}

int main(void)
{
    Result rc;
    Thread th;

    gfxInitDefault();
    consoleInit(GFX_TOP, NULL);

    mkdir("/3ds", 0777);
    mkdir("/3ds/aiconsole", 0777);
    mkdir(DIR, 0777);

    g_log = fopen(DIR "report.txt", "w");
    if (!g_log) say("!! cannot open report.txt\n");

    rc = socInit((u32 *)memalign(0x1000, 0x10000), 0x10000);
    if (R_FAILED(rc)) {
        say("!! socInit failed: 0x%08lx\n", (unsigned long)rc);
        set_status("soc_fail");
        if (g_log) fclose(g_log);
        svcSleepThread(3 * 1000 * 1000 * 1000LL);
        gfxExit();
        return 1;
    }

    curl_global_init(CURL_GLOBAL_DEFAULT);

    /* The 3dsx loader only gives the main thread a 16 KB stack regardless of
     * __stacksize__, so the real work runs on a thread with a big stack. */
    th = threadCreate(worker, NULL, 0x40000, 0x3f, -2, false);
    if (th) threadJoin(th, U64_MAX);
    else worker(NULL);

    curl_global_cleanup();
    socExit();

    say("press START to exit\n");
    while (aptMainLoop()) {
        hidScanInput();
        if (hidKeysDown() & KEY_START) break;
        gfxFlushBuffers();
        gfxSwapBuffers();
        gspWaitForVBlank();
    }

    gfxExit();
    return 0;
}
