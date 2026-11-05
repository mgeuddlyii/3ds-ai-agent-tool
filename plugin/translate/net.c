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

/* 读 API Key：优先 key.txt（和主程序共用一份），再退回 ini 里的 key= */
static void read_key(char *out, size_t cap)
{
    FILE *f;

    out[0] = '\0';
    f = fopen(KEY_PATH, "r");
    if (f) {
        if (fgets(out, (int)cap, f)) {
            size_t n = strlen(out);

            while (n && (out[n - 1] == '\n' || out[n - 1] == '\r'))
                out[--n] = '\0';
        }
        fclose(f);
    }
    if (!out[0])
        ini_get("key", out, cap, "");
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

    ini_get("api_base", base,  sizeof base,  "https://api.deepseek.com");
    ini_get("model",    model, sizeof model, "deepseek-chat");
    read_key(key, sizeof key);
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
        "\"把图里的文字翻译成简体中文。只输出译文，不要解释、不要引号。"
        "看不清就回“看不清”。\"},"
        "{\"type\":\"image_url\",\"image_url\":{\"url\":"
        "\"data:image/png;base64,%s\"}}]}]}", model, b64);
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
