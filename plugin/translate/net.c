/*
 * net.c -- 见 net.h
 *
 * 流程：soc 起服务 → curl 组请求 → POST → 取 choices[0].message.content
 *
 * 三处刻意的选择：
 *  1. **不引 minijson**：这里只需要从固定形状的响应里抠一个字符串字段，
 *     引一个解析器反而多一处会崩的地方。代价是"只认这一种响应形状"，
 *     所以遇到别的形状要明确报错，而不是返回空字符串。
 *  2. **超时 60 秒**：模型看图本来就慢，30 秒会经常性地截断成"连不上"，
 *     那种失败最难查（看起来像网络问题，其实是等太短）。
 *  3. **每个失败都写人话**：插件崩了现场全黑，面板是唯一的日志。
 */

#include "net.h"
#include "png.h"

#include <3ds.h>
#include <curl/curl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cfg.h"

/* 译文目标语言。由 net_set_target() 在 cfg_load 之后灌进来。
 * 声明放在这里而不是 net_translate 附近：拼请求那段在文件前部，必须先声明。 */
static char g_target[24] = "简体中文";

#define SOC_BUF_SIZE 0x100000        /* 1MiB：插件内存区总共 5MiB，别贪 */
#define KEY_PATH     "sdmc:/3ds/aiconsole/key.txt"
#define CFG_PATH     "sdmc:/3ds/aiconsole/plugin.ini"

static bool g_inited;

typedef struct {
    char  *p;
    size_t n, cap;
} Buf;

static size_t sink(char *data, size_t sz, size_t cnt, void *user)
{
    Buf   *b = user;
    size_t n = sz * cnt;

    if (b->n + n + 1 > b->cap) {
        size_t cap = b->cap ? b->cap * 2 : 4096;

        while (cap < b->n + n + 1)
            cap *= 2;
        {
            char *np = realloc(b->p, cap);

            if (!np)
                return 0;
            b->p = np;
            b->cap = cap;
        }
    }
    memcpy(b->p + b->n, data, n);
    b->n += n;
    b->p[b->n] = '\0';
    return n;
}

bool net_init(void)
{
    static u32 *soc_buf;

    if (g_inited)
        return true;

    if (!soc_buf) {
        /* memalign 不在 newlib 的默认声明里，这里自己对齐分配：
         * soc 要求缓冲 0x1000 对齐，用 malloc 拿到的不保证。 */
        void *raw = malloc(SOC_BUF_SIZE + 0x1000);

        if (!raw)
            return false;
        soc_buf = (u32 *)(((uintptr_t)raw + 0xFFF) & ~(uintptr_t)0xFFF);
    }
    /* ⚠️ socInit 在插件里能否成功取决于游戏有没有先占了 SOC 服务。
     *    失败时不要慌，先怀疑这条 —— 真机上表现为"一直连不上"。 */
    if (R_FAILED(socInit(soc_buf, SOC_BUF_SIZE))) {
        /* 有些游戏已经初始化过，这里再 init 会失败但仍可用；
         * 真判断要看 curl 能不能出网，所以不在这里直接返回 false。 */
    }
    if (curl_global_init(CURL_GLOBAL_DEFAULT) != 0)
        return false;

    g_inited = true;
    return true;
}

/* 从 ini 里读一个键（插件这边没有配置结构体，只有几个字段，直接扫） */
static void ini_get(const char *key, char *out, size_t cap, const char *def)
{
    FILE *f = fopen(CFG_PATH, "r");

    snprintf(out, cap, "%s", def);
    if (!f)
        return;
    {
        char line[192];

        while (fgets(line, sizeof line, f)) {
            char *eq = strchr(line, '=');
            size_t klen = strlen(key);

            if (!eq || (size_t)(eq - line) != klen || strncmp(line, key, klen) != 0)
                continue;
            {
                char *v = eq + 1;

                while (*v && (v[strlen(v) - 1] == '\n' || v[strlen(v) - 1] == '\r'))
                    v[strlen(v) - 1] = '\0';
                snprintf(out, cap, "%s", v);
            }
            break;
        }
        fclose(f);
    }
}

/* 读 key.txt。
 *
 * ★ 这里必须按**主程序同一套规则**解析：那个文件是带注释的 k=v 模板
 *   （# 开头是注释，然后 key= / base_url= / model=），
 *   **不是"第一行就是 Key"**。之前按第一行读，拿到的是 "# ====" 这行注释，
 *   发出去就是一张 401 —— 而且报错长得像"Key 无效"，很容易误判成 Key 过期。
 */
static void read_creds(char *key, size_t kcap, char *base, size_t bcap,
                       char *model, size_t mcap)
{
    FILE *f;

    key[0] = base[0] = model[0] = '\0';
    f = fopen(KEY_PATH, "r");
    if (f) {
        char line[256];

        while (fgets(line, sizeof line, f)) {
            char  *s   = line;
            char  *eq;
            char  *eol;

            while (*s == ' ' || *s == '\t')
                ++s;
            eol = s + strlen(s);
            while (eol > s && (eol[-1] == '\n' || eol[-1] == '\r' ||
                               eol[-1] == ' '  || eol[-1] == '\t'))
                *--eol = '\0';

            if (*s == '\0' || *s == '#')
                continue;                 /* 注释与空行 */

            eq = strchr(s, '=');
            if (!eq) {
                /* 没有等号：允许"只贴了一串 Key"的写法（老用户习惯） */
                if (strlen(s) > 20 && !key[0])
                    snprintf(key, kcap, "%s", s);
                continue;
            }
            *eq = '\0';
            {
                char *k = s;
                char *v = eq + 1;

                while (*v == ' ' || *v == '\t')
                    ++v;
                if (strcmp(k, "key") == 0)
                    snprintf(key, kcap, "%s", v);
                else if (strcmp(k, "base_url") == 0 || strcmp(k, "url") == 0)
                    snprintf(base, bcap, "%s", v);
                else if (strcmp(k, "model") == 0)
                    snprintf(model, mcap, "%s", v);
            }
        }
        fclose(f);
    }

    /* 文件里没写的，用 plugin.ini 兜底（插件自己的覆盖优先级更高） */
    {
        char tmp[160];

        ini_get("api_base", tmp, sizeof tmp, "");
        if (tmp[0]) snprintf(base, bcap, "%s", tmp);
        ini_get("model", tmp, sizeof tmp, "");
        if (tmp[0]) snprintf(model, mcap, "%s", tmp);
        ini_get("key", tmp, sizeof tmp, "");
        if (!key[0] && tmp[0]) snprintf(key, kcap, "%s", tmp);
    }

    if (!base[0])  snprintf(base,  bcap,  "%s", "https://api.deepseek.com");
    if (!model[0]) snprintf(model, mcap, "%s", "deepseek-chat");
}

/* \uXXXX → UTF-8 */
static size_t put_utf8(char *o, uint32_t cp)
{
    if (cp < 0x80) { o[0] = (char)cp; return 1; }
    if (cp < 0x800) {
        o[0] = (char)(0xC0 | (cp >> 6));
        o[1] = (char)(0x80 | (cp & 0x3F));
        return 2;
    }
    o[0] = (char)(0xE0 | (cp >> 12));
    o[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
    o[2] = (char)(0x80 | (cp & 0x3F));
    return 3;
}

/* 从 {"...content":"...."} 里取出正文，处理常见转义 */
static bool extract_content(const char *json, char *out, size_t cap)
{
    const char *p = strstr(json, "\"content\"");
    size_t      o = 0;

    if (!p)
        return false;
    p = strchr(p, ':');
    if (!p)
        return false;
    p = strchr(p, '"');
    if (!p)
        return false;
    ++p;

    while (*p && o + 4 < cap) {
        if (*p == '"')
            break;
        if (*p == '\\' && p[1]) {
            ++p;
            switch (*p) {
            case 'n': out[o++] = '\n'; ++p; break;
            case 'r': out[o++] = '\r'; ++p; break;
            case 't': out[o++] = ' ';  ++p; break;
            case '"': out[o++] = '"';  ++p; break;
            case '\\': out[o++] = '\\'; ++p; break;
            case '/': out[o++] = '/';  ++p; break;
            case 'u': {
                unsigned cp = 0;

                if (sscanf(p + 1, "%4x", &cp) == 1) {
                    o += put_utf8(out + o, cp);
                    p += 5;
                } else {
                    ++p;
                }
                break;
            }
            default: out[o++] = *p; ++p; break;
            }
            continue;
        }
        out[o++] = *p++;
    }
    out[o] = '\0';
    return o > 0;
}

bool net_translate(const uint8_t *bgr, int w, int h, char *out, size_t cap)
{
    char   base[160], model[64], key[200];
    size_t png_n = 0;
    uint8_t *png;
    char   *b64;
    char   *body;
    char   *url;
    Buf     resp = { 0 };
    CURL   *c;
    CURLcode rc;
    bool    ok = false;
    size_t  body_cap;

    if (!out || cap < 8)
        return false;
    out[0] = '\0';

    read_creds(key, sizeof key, base, sizeof base, model, sizeof model);
    if (!key[0]) {
        snprintf(out, cap, "没有 API Key\n放到 sdmc:/3ds/aiconsole/key.txt");
        return false;
    }
    if (!net_init()) {
        snprintf(out, cap, "网络初始化失败");
        return false;
    }

    png = png_encode(bgr, w, h, &png_n);
    if (!png) {
        snprintf(out, cap, "图片编码失败（内存不够？）");
        return false;
    }
    b64 = base64(png, png_n);
    free(png);
    if (!b64) {
        snprintf(out, cap, "base64 失败（内存不够）");
        return false;
    }

    /* 请求体：多模态格式，和主程序用的那家一致 */
    body_cap = strlen(b64) + 1024;
    body = malloc(body_cap);
    if (!body) {
        free(b64);
        snprintf(out, cap, "内存不够拼请求");
        return false;
    }
    snprintf(body, body_cap,
        "{\"model\":\"%s\",\"max_tokens\":400,\"messages\":[{\"role\":\"user\","
        "\"content\":[{\"type\":\"text\",\"text\":"
        "\"把图里的文字翻译成%s。只输出译文，不要解释、不要引号。"
        "看不清就回“看不清”。\"},"
        "{\"type\":\"image_url\",\"image_url\":{\"url\":"
        "\"data:image/png;base64,%s\"}}]}]}", model, g_target, b64);
    free(b64);

    url = malloc(strlen(base) + 64);
    if (!url) {
        free(body);
        snprintf(out, cap, "内存不够拼地址");
        return false;
    }
    snprintf(url, strlen(base) + 64, "%s/chat/completions", base);

    c = curl_easy_init();
    if (!c) {
        free(body);
        free(url);
        snprintf(out, cap, "curl 初始化失败");
        return false;
    }

    {
        struct curl_slist *hdr = NULL;
        char auth[256];

        snprintf(auth, sizeof auth, "Authorization: Bearer %s", key);
        hdr = curl_slist_append(hdr, "Content-Type: application/json");
        hdr = curl_slist_append(hdr, auth);

        curl_easy_setopt(c, CURLOPT_URL, url);
        curl_easy_setopt(c, CURLOPT_POST, 1L);
        curl_easy_setopt(c, CURLOPT_POSTFIELDS, body);
        curl_easy_setopt(c, CURLOPT_HTTPHEADER, hdr);
        curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, sink);
        curl_easy_setopt(c, CURLOPT_WRITEDATA, &resp);
        curl_easy_setopt(c, CURLOPT_TIMEOUT, 60L);        /* 看图慢，别用 30s */
        curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
        curl_easy_setopt(c, CURLOPT_USERAGENT, "AIConsole-plugin/0.3");

        rc = curl_easy_perform(c);

        if (rc != CURLE_OK) {
            snprintf(out, cap, "连不上：%s", curl_easy_strerror(rc));
        } else {
            long code = 0;

            curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &code);
            if (code != 200) {
                snprintf(out, cap, "HTTP %ld\n%.80s", code, resp.p ? resp.p : "");
            } else if (!resp.p || !extract_content(resp.p, out, cap)) {
                snprintf(out, cap, "回包里没找到译文\n%.80s", resp.p ? resp.p : "(空)");
            } else {
                ok = true;
            }
        }
        curl_slist_free_all(hdr);
    }

    curl_easy_cleanup(c);
    free(resp.p);
    free(body);
    free(url);
    return ok;
}

void net_set_target(const char *name)
{
    if (!name || !*name)
        return;
    snprintf(g_target, sizeof g_target, "%s", name);
}



bool net_ping(char *out, size_t cap)
{
    char   base[160], model[64], key[200];
    char  *url;
    char   body[256];
    Buf    resp = { 0 };
    CURL  *c;
    CURLcode rc;
    u64    t0, t1;
    bool   ok = false;

    if (!out || cap < 8)
        return false;
    out[0] = '\0';

    read_creds(key, sizeof key, base, sizeof base, model, sizeof model);
    if (!key[0]) {
        snprintf(out, cap, "没有 Key\n放 sdmc:/3ds/aiconsole/key.txt");
        return false;
    }
    if (!net_init()) {
        snprintf(out, cap, "网络初始化失败");
        return false;
    }

    url = malloc(strlen(base) + 64);
    if (!url) {
        snprintf(out, cap, "内存不够");
        return false;
    }
    snprintf(url, strlen(base) + 64, "%s/chat/completions", base);

    snprintf(body, sizeof body,
             "{\"model\":\"%s\",\"max_tokens\":8,\"messages\":"
             "[{\"role\":\"user\",\"content\":\"只回复 OK 两个字母\"}]}", model);

    c = curl_easy_init();
    if (!c) {
        free(url);
        snprintf(out, cap, "curl 初始化失败");
        return false;
    }

    {
        struct curl_slist *hdr = NULL;
        char auth[256];

        snprintf(auth, sizeof auth, "Authorization: Bearer %s", key);
        hdr = curl_slist_append(hdr, "Content-Type: application/json");
        hdr = curl_slist_append(hdr, auth);

        curl_easy_setopt(c, CURLOPT_URL, url);
        curl_easy_setopt(c, CURLOPT_POST, 1L);
        curl_easy_setopt(c, CURLOPT_POSTFIELDS, body);
        curl_easy_setopt(c, CURLOPT_HTTPHEADER, hdr);
        curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, sink);
        curl_easy_setopt(c, CURLOPT_WRITEDATA, &resp);
        curl_easy_setopt(c, CURLOPT_TIMEOUT, 20L);   /* 纯文本，20s 足够 */
        curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
        curl_easy_setopt(c, CURLOPT_USERAGENT, "AIConsole-plugin/0.5");

        t0 = osGetTime();
        rc = curl_easy_perform(c);
        t1 = osGetTime();

        if (rc != CURLE_OK) {
            /* 这几条是插件里最常见的三种死法，分开报 */
            const char *why =
                rc == CURLE_COULDNT_RESOLVE_HOST ? "DNS 解析失败（Wi-Fi 连了吗？）" :
                rc == CURLE_COULDNT_CONNECT      ? "连不上服务器（端口被挡？）" :
                rc == CURLE_SSL_CONNECT_ERROR    ? "TLS 握手失败" :
                rc == CURLE_OPERATION_TIMEDOUT   ? "超时（网络太慢或服务不可达）" :
                                                   curl_easy_strerror(rc);

            snprintf(out, cap, "网络不通：%s", why);
        } else {
            long code = 0;

            curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &code);
            if (code == 200) {
                char txt[96] = { 0 };

                if (resp.p)
                    (void)extract_content(resp.p, txt, sizeof txt);
                snprintf(out, cap, "连接正常 %llums\n模型：%s\n回：%s",
                         (unsigned long long)(t1 - t0), model,
                         txt[0] ? txt : "(空)");
                ok = true;
            } else if (code == 401) {
                snprintf(out, cap, "HTTP 401：Key 不对\n检查 key.txt 的 key= 那行");
            } else if (code == 402 || code == 429) {
                snprintf(out, cap, "HTTP %ld：余额/限流\n换一把 Key 或充值", code);
            } else {
                snprintf(out, cap, "HTTP %ld\n%.60s", code, resp.p ? resp.p : "");
            }
        }
        curl_slist_free_all(hdr);
    }

    curl_easy_cleanup(c);
    free(resp.p);
    free(url);
    return ok;
}
