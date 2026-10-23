/*
 * tls12 probe — can a 3DS do TLS 1.2 at all?
 * ------------------------------------------------------------------
 * Why: libctru's httpc/sslc tops out at TLS 1.1 (sslc.h only exposes
 * SSLCOPT_Default == TLS1.1 and SSLCOPT_TLSv10). Modern endpoints such as
 * ark.cn-beijing.volces.com refuse anything below 1.2, which is why the
 * "3DS can never reach Doubao" conclusion was reached.
 *
 * This probe re-tests that with the *portlibs* stack instead: libcurl linked
 * against 3ds-mbedtls (which devkitPro patches to take entropy from
 * sslcGenerateRandomData). Unlike Azahar's http_c.cpp — which unconditionally
 * disables cert verification — the TLS handshake here is performed by mbedtls
 * compiled into this binary, so whatever it reports is real.
 *
 * Result goes to sdmc:/3ds/aiconsole/tls12.txt (fallback: sdmc:/tls12.txt).
 */

#include <3ds.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <stdlib.h>
#include <malloc.h>

#include <curl/curl.h>
#include <mbedtls/ssl.h>
#include <mbedtls/version.h>

static FILE *g_out = NULL;
static const char *g_out_path = "/3ds/aiconsole/tls12.txt";
static volatile int g_started = 0;
static volatile int g_done = 0;

static void say(const char *fmt, ...)
{
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    puts(buf);
    fflush(stdout);
    if (g_out) {
        fputs(buf, g_out);
        fputc('\n', g_out);
        fflush(g_out);
    }
}

/* curl wants somewhere to put the body; we throw it away. */
static size_t sink(void *ptr, size_t size, size_t nmemb, void *userdata)
{
    (void)ptr; (void)userdata;
    return size * nmemb;
}

typedef struct {
    const char *label;
    const char *url;
} Endpoint;

static const Endpoint ENDPOINTS[] = {
    { "ark(doubao-official)", "https://ark.cn-beijing.volces.com/api/v3/models" },
    { "doubao-www",           "https://www.doubao.com/" },
    { "deepseek",             "https://api.deepseek.com/models" },
    { "zhipu",                "https://open.bigmodel.cn/api/paas/v4/models" },
};
static const int N_EP = sizeof(ENDPOINTS) / sizeof(ENDPOINTS[0]);

/*
 * One request.  verify=0 -> protocol reachability only (is TLS 1.2 even
 * possible?).  verify=1 -> also ask mbedtls to validate the chain, which
 * tells us whether we additionally need to embed a CA bundle.
 */
static void probe(const Endpoint *ep, int verify, const char *tag)
{
    CURL *e = curl_easy_init();
    if (!e) { say("  %-14s curl_easy_init FAILED", tag); return; }

    long http_code = 0, verifyresult = -1;
    char errbuf[CURL_ERROR_SIZE];
    errbuf[0] = 0;

    curl_easy_setopt(e, CURLOPT_URL, ep->url);
    curl_easy_setopt(e, CURLOPT_WRITEFUNCTION, sink);
    curl_easy_setopt(e, CURLOPT_ERRORBUFFER, errbuf);
    curl_easy_setopt(e, CURLOPT_TIMEOUT, 20L);
    curl_easy_setopt(e, CURLOPT_CONNECTTIMEOUT, 12L);
    curl_easy_setopt(e, CURLOPT_USERAGENT, "3ds-tls12-probe/1.0");
    curl_easy_setopt(e, CURLOPT_NOSIGNAL, 1L);

    /* Pin the floor AND the ceiling at TLS 1.2: if this succeeds, 1.2 works. */
    curl_easy_setopt(e, CURLOPT_SSLVERSION,
                     (long)(CURL_SSLVERSION_TLSv1_2 | CURL_SSLVERSION_MAX_TLSv1_2));
    curl_easy_setopt(e, CURLOPT_SSL_VERIFYPEER, verify ? 1L : 0L);
    curl_easy_setopt(e, CURLOPT_SSL_VERIFYHOST, verify ? 2L : 0L);

    CURLcode rc = curl_easy_perform(e);

    /* Ask mbedtls itself what version was negotiated, and which backend
     * curl actually picked. backend==MBEDTLS is the proof that the handshake
     * was done by the code in this binary, not by some emulator shortcut. */
    const char *tlsver = "?";
    int backend = -1;
    struct curl_tlssessioninfo *si = NULL;
    if (curl_easy_getinfo(e, CURLINFO_TLS_SESSION, &si) == CURLE_OK && si) {
        backend = (int)si->backend;
        if (si->internals)
            tlsver = mbedtls_ssl_get_version((const mbedtls_ssl_context *)si->internals);
    }

    curl_easy_getinfo(e, CURLINFO_RESPONSE_CODE, &http_code);
    long vr = 0;
    if (curl_easy_getinfo(e, CURLINFO_SSL_VERIFYRESULT, &vr) == CURLE_OK)
        verifyresult = vr;

    say("  %-14s rc=%d http=%ld tls=%s backend=%d verifyresult=%ld%s%s",
        tag, (int)rc, http_code, tlsver, backend, verifyresult,
        errbuf[0] ? "  err=" : "", errbuf);
    if (rc != CURLE_OK && errbuf[0])
        say("      -> %s", errbuf);

    curl_easy_cleanup(e);
}

static void worker(void *arg)
{
    (void)arg;

    say("=== 3DS TLS 1.2 probe ===");
    say("curl     : %s", curl_version());
    say("mbedtls  : compiled " MBEDTLS_VERSION_STRING);

    g_out = fopen(g_out_path, "w");
    if (!g_out) {
        g_out_path = "/tls12.txt";
        g_out = fopen(g_out_path, "w");
    }
    g_started = 1;

    say("curl     : %s", curl_version());
    say("mbedtls  : compiled " MBEDTLS_VERSION_STRING);
    say("");

    for (int i = 0; i < N_EP; i++) {
        say("[%s] %s", ENDPOINTS[i].label, ENDPOINTS[i].url);
        probe(&ENDPOINTS[i], 0, "no-verify");   /* protocol layer */
        probe(&ENDPOINTS[i], 1, "verify");      /* + certificate chain */
        say("");
    }

    say("=== done ===");
    if (g_out) { fclose(g_out); g_out = NULL; }
    g_done = 1;
}

int main(void)
{
    /* The 3dsx loader hands the main thread a hardcoded 16 KB stack and
     * ignores __stacksize__; curl + mbedtls blow straight through that.
     * Everything real runs on a worker thread (see the 2026-10-01 crash note). */
    if (R_FAILED(svcSetThreadPriority(CUR_THREAD_HANDLE, 0x30))) { /* best effort */ }

    u32 *soc_buf = (u32 *)memalign(0x1000, 0x100000);
    if (!soc_buf) return 1;
    if (R_FAILED(socInit(soc_buf, 0x100000))) {
        puts("socInit failed");
        free(soc_buf);
        return 1;
    }

    if (curl_global_init(CURL_GLOBAL_DEFAULT) != 0) {
        puts("curl_global_init failed");
        socExit();
        free(soc_buf);
        return 1;
    }

    /* No gfx: this probe writes a file and exits. hbmenu/Azahar is fine with it. */
    threadCreate(worker, NULL, 0x80000, 0x3F, -2, true);

    /* Park the main thread until the worker is finished (it closes the file). */
    while (!g_started || !g_done)
        svcSleepThread(100 * 1000 * 1000);
    svcSleepThread(300 * 1000 * 1000);

    curl_global_cleanup();
    socExit();
    free(soc_buf);
    return 0;
}
