/*
 * doubao_img.c -- 见 doubao_img.h
 *
 * 四步（全部实测通过，2026-11-14）：
 *   1 POST /alice/resource/prepare_upload   → service_id + AWS 临时凭证
 *   2 GET  imagex ApplyImageUpload (AWS4)   → StoreUri / Auth / SessionKey
 *   3 POST tos-d-x-hl.snssdk.com/upload/v1/<StoreUri> → "Success"
 *   4 POST imagex CommitImageUpload (AWS4)  → ImageUri
 *
 * AWS4 只用 SHA256 + HMAC-SHA256（shared/sha256.c 已有 SHA256），
 * 所以这段能原样跑在 3DS 上，不需要 mbedtls 的 tls 层。
 */

#include "doubao_img.h"

#include <3ds.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "doubao.h"     /* DB_COOKIE_FILE */
#include "httpx.h"
#include "sha256.h"

#define IMAGEX_HOST "imagex.bytedanceapi.com"
#define TOS_HOST    "tos-d-x-hl.snssdk.com"
#define DB_ORIGIN   "https://www.doubao.com"
#define DB_REFERER  "https://www.doubao.com/chat/"
#define DB_UA       "Mozilla/5.0 (Windows NT 10.0; Win64; x64) " \
                    "AppleWebKit/537.36 (KHTML, like Gecko) " \
                    "Chrome/120.0 Safari/537.36"

/* ------------------------------------------------------------------ */
/* 收包                                                                */
/* ------------------------------------------------------------------ */

typedef struct {
    char  *p;
    size_t n, cap;
} Buf;

static size_t sink(const char *d, size_t len, void *u)
{
    Buf *b = (Buf *)u;

    if (b->n + len + 1 > b->cap) {
        size_t nc = b->cap ? b->cap : 4096;

        while (nc < b->n + len + 1)
            nc *= 2;
        {
            char *np = (char *)realloc(b->p, nc);

            if (!np)
                return 0;
            b->p = np;
            b->cap = nc;
        }
    }
    memcpy(b->p + b->n, d, len);
    b->n += len;
    b->p[b->n] = '\0';
    return len;
}

static void sink_reset(void *u)
{
    Buf *b = (Buf *)u;

    if (b->p && b->cap)
        b->p[0] = '\0';
    b->n = 0;
}

/* 从 JSON 里抠一个字符串字段（不引解析器，理由同 net.c）。 */
static bool jstr(const char *json, const char *key, char *out, size_t cap)
{
    char pat[64];
    const char *p;
    size_t i = 0;

    if (!json || !key || !out || cap == 0)
        return false;
    snprintf(pat, sizeof pat, "\"%s\":\"", key);
    p = strstr(json, pat);
    if (!p)
        return false;
    p += strlen(pat);
    while (*p && *p != '"' && i + 1 < cap) {
        if (*p == '\\' && p[1]) {
            ++p;
            switch (*p) {
            case 'n': out[i++] = '\n'; break;
            case 't': out[i++] = '\t'; break;
            case '/': out[i++] = '/';  break;
            default:  out[i++] = *p;   break;
            }
            ++p;
            continue;
        }
        out[i++] = *p++;
    }
    out[i] = '\0';
    return i > 0;
}

/* ------------------------------------------------------------------ */
/* MD5（attachment 里要 md5；mbedtls 的头文件没装，自己写一个）          */
/* ------------------------------------------------------------------ */

typedef struct {
    u32 h[4];
    u32 lo;         /* 已处理字节数（本用途不会超过 4GB） */
    u8  buf[64];
    int n;
} Md5;

static u32 rol(u32 x, int c) { return (x << c) | (x >> (32 - c)); }

static void md5_block(Md5 *m, const u8 *p)
{
    static const u32 K[64] = {
        0xd76aa478,0xe8c7b756,0x242070db,0xc1bdceee,0xf57c0faf,0x4787c62a,
        0xa8304613,0xfd469501,0x698098d8,0x8b44f7af,0xffff5bb1,0x895cd7be,
        0x6b901122,0xfd987193,0xa679438e,0x49b40821,0xf61e2562,0xc040b340,
        0x265e5a51,0xe9b6c7aa,0xd62f105d,0x02441453,0xd8a1e681,0xe7d3fbc8,
        0x21e1cde6,0xc33707d6,0xf4d50d87,0x455a14ed,0xa9e3e905,0xfcefa3f8,
        0x676f02d9,0x8d2a4c8a,0xfffa3942,0x8771f681,0x6d9d6122,0xfde5380c,
        0xa4beea44,0x4bdecfa9,0xf6bb4b60,0xbebfbc70,0x289b7ec6,0xeaa127fa,
        0xd4ef3085,0x04881d05,0xd9d4d039,0xe6db99e5,0x1fa27cf8,0xc4ac5665,
        0xf4292244,0x432aff97,0xab9423a7,0xfc93a039,0x655b59c3,0x8f0ccc92,
        0xffeff47d,0x85845dd1,0x6fa87e4f,0xfe2ce6e0,0xa3014314,0x4e0811a1,
        0xf7537e82,0xbd3af235,0x2ad7d2bb,0xeb86d391
    };
    static const int R[64] = {
        7,12,17,22, 7,12,17,22, 7,12,17,22, 7,12,17,22,
        5, 9,14,20, 5, 9,14,20, 5, 9,14,20, 5, 9,14,20,
        4,11,16,23, 4,11,16,23, 4,11,16,23, 4,11,16,23,
        6,10,15,21, 6,10,15,21, 6,10,15,21, 6,10,15,21
    };
    u32 M[16], a = m->h[0], b = m->h[1], c = m->h[2], d = m->h[3];

    for (int i = 0; i < 16; ++i)
        M[i] = (u32)p[i * 4] | ((u32)p[i * 4 + 1] << 8) |
               ((u32)p[i * 4 + 2] << 16) | ((u32)p[i * 4 + 3] << 24);

    for (int i = 0; i < 64; ++i) {
        u32 f;
        int g;

        if (i < 16)      { f = (b & c) | (~b & d);      g = i; }
        else if (i < 32) { f = (d & b) | (~d & c);      g = (5 * i + 1) & 15; }
        else if (i < 48) { f = b ^ c ^ d;               g = (3 * i + 5) & 15; }
        else             { f = c ^ (b | ~d);            g = (7 * i) & 15; }
        {
            u32 t = d;

            d = c;
            c = b;
            b = b + rol(a + f + K[i] + M[g], R[i]);
            a = t;
        }
    }
    m->h[0] += a; m->h[1] += b; m->h[2] += c; m->h[3] += d;
}

static void md5_init(Md5 *m)
{
    m->h[0] = 0x67452301; m->h[1] = 0xefcdab89;
    m->h[2] = 0x98badcfe; m->h[3] = 0x10325476;
    m->lo = 0; m->n = 0;
}

static void md5_update(Md5 *m, const void *data, size_t len)
{
    const u8 *p = (const u8 *)data;

    m->lo += (u32)len;
    while (len--) {
        m->buf[m->n++] = *p++;
        if (m->n == 64) { md5_block(m, m->buf); m->n = 0; }
    }
}

static void md5_final(Md5 *m, char out_hex[33])
{
    u32 bits = m->lo << 3;
    u8  pad[72];
    int pn = 0;

    pad[pn++] = 0x80;
    while ((m->n + pn) % 64 != 56)
        pad[pn++] = 0;
    pad[pn++] = (u8)bits;         pad[pn++] = (u8)(bits >> 8);
    pad[pn++] = (u8)(bits >> 16); pad[pn++] = (u8)(bits >> 24);
    md5_update(m, pad, (size_t)pn);

    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j)
            sprintf(out_hex + i * 8 + j * 2, "%02x", (unsigned)((m->h[i] >> (8 * j)) & 0xff));
    out_hex[32] = '\0';
}

/* ------------------------------------------------------------------ */
/* HMAC-SHA256 + AWS4                                                  */
/* ------------------------------------------------------------------ */

static void hmac_sha256(const u8 *key, size_t klen, const char *msg, size_t mlen,
                        u8 out[32])
{
    u8  k[64], ip[64], op[64], inner[32];
    Sha256Ctx c;

    memset(k, 0, sizeof k);
    if (klen > 64) {
        sha256_init(&c);
        sha256_update(&c, key, klen);
        sha256_final(&c, k);
    } else {
        memcpy(k, key, klen);
    }
    for (int i = 0; i < 64; ++i) { ip[i] = k[i] ^ 0x36; op[i] = k[i] ^ 0x5c; }

    sha256_init(&c);
    sha256_update(&c, ip, 64);
    sha256_update(&c, msg, mlen);
    sha256_final(&c, inner);

    sha256_init(&c);
    sha256_update(&c, op, 64);
    sha256_update(&c, inner, 32);
    sha256_final(&c, out);
}

static void hex_of(const u8 *d, size_t n, char *out)
{
    static const char *H = "0123456789abcdef";

    for (size_t i = 0; i < n; ++i) {
        out[i * 2]     = H[d[i] >> 4];
        out[i * 2 + 1] = H[d[i] & 15];
    }
    out[n * 2] = '\0';
}

static void sha256_hex_buf(const void *data, size_t n, char out[65])
{
    Sha256Ctx c;
    u8        d[32];

    sha256_init(&c);
    sha256_update(&c, data, n);
    sha256_final(&c, d);
    hex_of(d, 32, out);
}

/* 签一次请求。query 必须是**已按 key 排序**的规范串（我们的参数固定，直接写死顺序）。
 * with_ct=true 时把 content-type 也纳入签名（CommitImageUpload 需要）。 */
static void aws4(const char *method, const char *host, const char *query,
                 int with_ct, const u8 *payload, size_t plen,
                 const char *ak, const char *sk, const char *token,
                 char *auth, size_t acap,
                 char *amz_out, size_t amz_cap)
{
    char  amzdate[20], datestamp[12], scope[96];
    char  hdrs_canon[640];
    char  canon[1600], signed_headers[128], sts[256], sig_hex[65];
    char  payload_hash[65];
    u8    key[32];
    time_t t = time(NULL);
    struct tm *g = gmtime(&t);

    snprintf(amzdate, sizeof amzdate, "%04d%02d%02dT%02d%02d%02dZ",
             g->tm_year + 1900, g->tm_mon + 1, g->tm_mday,
             g->tm_hour, g->tm_min, g->tm_sec);
    snprintf(datestamp, sizeof datestamp, "%.8s", amzdate);
    if (amz_out && amz_cap)
        snprintf(amz_out, amz_cap, "%s", amzdate);

    sha256_hex_buf(payload, plen, payload_hash);

    /* SignedHeaders 必须**字母序**：
     * content-type < host < origin < referer < user-agent < x-amz-date
     * < x-amz-security-token */
    if (with_ct)
        snprintf(signed_headers, sizeof signed_headers,
                 "content-type;host;origin;referer;user-agent;"
                 "x-amz-date;x-amz-security-token");
    else
        snprintf(signed_headers, sizeof signed_headers,
                 "host;origin;referer;user-agent;x-amz-date;x-amz-security-token");

    /* ★ 规范化头块必须**和真正发出去的请求头逐字一致**：
     *   参与签名却不发送 → 服务端算出来的签名必然不同 → 403。
     *   这就要求调用方把 origin/referer/user-agent/x-amz-date/
     *   x-amz-security-token 全都显式带上（curl 默认的 UA 不是这个）。 */
    snprintf(hdrs_canon, sizeof hdrs_canon,
             "%s"
             "host:%s\n"
             "origin:" DB_ORIGIN "\n"
             "referer:" DB_REFERER "\n"
             "user-agent:" DB_UA "\n"
             "x-amz-date:%s\n"
             "x-amz-security-token:%s\n",
             with_ct ? "content-type:application/json\n" : "",
             host, amzdate, token);

    /* 规范请求：METHOD\nURI\nQUERY\n(规范头块已自带尾部换行)SignedHeaders\n哈希 */
    snprintf(canon, sizeof canon, "%s\n/\n%s\n%s%s\n%s",
             method, query, hdrs_canon, signed_headers, payload_hash);

    snprintf(scope, sizeof scope, "%s/cn-north-1/imagex/aws4_request", datestamp);
    {
        char canon_hash[65];

        sha256_hex_buf(canon, strlen(canon), canon_hash);
        snprintf(sts, sizeof sts, "AWS4-HMAC-SHA256\n%s\n%s\n%s",
                 amzdate, scope, canon_hash);
    }

    /* kSigning = HMAC(HMAC(HMAC(HMAC("AWS4"+sk, date), region), service), "aws4_request") */
    {
        char  k1[128];
        u8    a[32], b[32], c2[32];

        snprintf(k1, sizeof k1, "AWS4%s", sk);
        hmac_sha256((const u8 *)k1, strlen(k1), datestamp, strlen(datestamp), a);
        hmac_sha256(a, 32, "cn-north-1", 10, b);
        hmac_sha256(b, 32, "imagex", 7, c2);
        hmac_sha256(c2, 32, "aws4_request", 12, key);
    }
    {
        u8 sg[32];

        hmac_sha256(key, 32, sts, strlen(sts), sg);
        hex_of(sg, 32, sig_hex);
    }

    snprintf(auth, acap,
             "AWS4-HMAC-SHA256 Credential=%s/%s, SignedHeaders=%s, Signature=%s",
             ak, scope, signed_headers, sig_hex);
}

/* ------------------------------------------------------------------ */
/* 主流程                                                              */
/* ------------------------------------------------------------------ */

/* 第 1 步用的设备参数：这一步**不需要**真 web_id（实测 device_id=0 即可）。 */
#define DEV_PARAMS \
    "aid=497858&device_id=0&device_platform=web&language=zh" \
    "&pc_version=2.20.0&pkg_type=release_version&real_aid=497858" \
    "&region=CN&samantha_web=1&sys_region=CN&tea_uuid=0" \
    "&use-olympus-account=1&version_code=20800"

static long do_req(const char *url, const void *raw, size_t rawlen, const char *ct,
                   const char *const *hdrs, Buf *b, char *err, size_t errcap)
{
    HxReq r;

    sink_reset(b);
    memset(&r, 0, sizeof r);
    r.url           = url;
    r.post_raw      = raw;
    r.post_raw_len  = rawlen;
    r.content_type  = ct;
    r.headers       = hdrs;
    r.timeout_s     = 40;
    r.follow        = false;
    r.add_cookies   = true;      /* 豆包这边全靠 cookie */
    r.on_new_response = sink_reset;
    return hx_perform(&r, sink, b, err, errcap);
}

bool dbimg_upload(const unsigned char *png, size_t n, int w, int h,
                  char *att_json, size_t att_cap,
                  char *msg, size_t msg_cap)
{
    Buf   b = { 0 };
    char  url[512], body[256], err[160];
    char  service_id[64] = "", ak[96] = "", sk[128] = "", token[1024] = "";
    char  store_uri[256] = "", store_auth[1024] = "", session_key[1024] = "";
    char  image_uri[256] = "", md5hex[33];
    char  auth[512], qs[384], amz[24];
    char  h_auth[1200], h_crc[64], h_date[40], h_tok[1100];
    const char *hdrs1[2];
    long  st;

    if (!png || n == 0 || !att_json || att_cap < 64) {
        snprintf(msg, msg_cap, "参数不对");
        return false;
    }

    /* ---- 1. prepare_upload：拿 AWS 临时凭证 ---- */
    snprintf(url, sizeof url, "%s/alice/resource/prepare_upload?%s",
             DB_ORIGIN, DEV_PARAMS);
    hdrs1[0] = "Accept: application/json";
    hdrs1[1] = NULL;
    st = do_req(url, "{\"resource_type\":2,\"scene_id\":\"5\",\"tenant_id\":\"5\"}",
                49, "application/json", hdrs1, &b, err, sizeof err);
    if (st != 200) {
        snprintf(msg, msg_cap, "1/4 取凭证失败 http=%ld %s", st, b.p ? b.p : err);
        free(b.p);
        return false;
    }
    if (!jstr(b.p, "service_id", service_id, sizeof service_id) ||
        !jstr(b.p, "access_key", ak, sizeof ak) ||
        !jstr(b.p, "secret_key", sk, sizeof sk)) {
        snprintf(msg, msg_cap, "1/4 回包里没有凭证：%.80s", b.p ? b.p : "");
        free(b.p);
        return false;
    }
    jstr(b.p, "session_token", token, sizeof token);

    /* ---- 2. ApplyImageUpload（AWS4 签名）---- */
    snprintf(qs, sizeof qs,
             "Action=ApplyImageUpload&FileExtension=.png&FileSize=%u"
             "&NeedFallback=true&ServiceId=%s&Version=2018-08-01",
             (unsigned)n, service_id);
    aws4("GET", IMAGEX_HOST, qs, 0, (const u8 *)"", 0, ak, sk, token,
         auth, sizeof auth, amz, sizeof amz);
    snprintf(h_auth, sizeof h_auth, "Authorization: %s", auth);
    snprintf(h_date, sizeof h_date, "x-amz-date: %s", amz);
    snprintf(h_tok,  sizeof h_tok,  "x-amz-security-token: %s", token);
    snprintf(url, sizeof url, "https://%s/?%s", IMAGEX_HOST, qs);
    {
        const char *hd[9];

        hd[0] = h_auth;
        hd[1] = h_date;
        hd[2] = h_tok;
        hd[3] = "origin: " DB_ORIGIN;
        hd[4] = "referer: " DB_REFERER;
        hd[5] = "user-agent: " DB_UA;
        hd[6] = "accept: application/json";
        hd[7] = NULL;
        st = do_req(url, NULL, 0, NULL, hd, &b, err, sizeof err);
    }
    if (st != 200) {
        snprintf(msg, msg_cap, "2/4 ApplyImageUpload http=%ld %.80s", st,
                 b.p ? b.p : err);
        free(b.p);
        return false;
    }
    if (!jstr(b.p, "StoreUri", store_uri, sizeof store_uri) ||
        !jstr(b.p, "Auth", store_auth, sizeof store_auth)) {
        snprintf(msg, msg_cap, "2/4 回包里没有 StoreUri：%.80s", b.p ? b.p : "");
        free(b.p);
        return false;
    }
    jstr(b.p, "SessionKey", session_key, sizeof session_key);

    /* ---- 3. 传二进制到 TOS ---- */
    {
        Md5 m;
        u32 crc = 0;

        md5_init(&m);
        md5_update(&m, png, n);
        md5_final(&m, md5hex);

        /* content-crc32：zlib 的 crc32（多项式 0xEDB88320） */
        crc = 0xFFFFFFFFu;
        for (size_t i = 0; i < n; ++i) {
            crc ^= png[i];
            for (int k = 0; k < 8; ++k)
                crc = (crc >> 1) ^ (0xEDB88320u & (u32)(-(int)(crc & 1)));
        }
        crc ^= 0xFFFFFFFFu;

        snprintf(h_auth, sizeof h_auth, "authorization: %s", store_auth);
        snprintf(h_crc, sizeof h_crc, "content-crc32: %08x", (unsigned)crc);
        snprintf(url, sizeof url, "https://%s/upload/v1/%s", TOS_HOST, store_uri);
        {
            const char *hd[6];

            hd[0] = h_auth;
            hd[1] = h_crc;
            hd[2] = "content-disposition: attachment; filename=\"undefined\"";
            hd[3] = NULL;
            st = do_req(url, png, n, "application/octet-stream", hd, &b,
                        err, sizeof err);
        }
        if (st != 200 || !b.p || !strstr(b.p, "Success")) {
            snprintf(msg, msg_cap, "3/4 上传失败 http=%ld %.80s", st,
                     b.p ? b.p : err);
            free(b.p);
            return false;
        }
    }

    /* ---- 4. CommitImageUpload（AWS4 签名）---- */
    snprintf(qs, sizeof qs,
             "Action=CommitImageUpload&ServiceId=%s&Version=2018-08-01",
             service_id);
    snprintf(body, sizeof body, "{\"SessionKey\":\"%s\"}", session_key);
    aws4("POST", IMAGEX_HOST, qs, 1, (const u8 *)body, strlen(body), ak, sk,
         token, auth, sizeof auth, amz, sizeof amz);
    snprintf(h_auth, sizeof h_auth, "Authorization: %s", auth);
    snprintf(h_date, sizeof h_date, "x-amz-date: %s", amz);
    snprintf(h_tok,  sizeof h_tok,  "x-amz-security-token: %s", token);
    snprintf(url, sizeof url, "https://%s/?%s", IMAGEX_HOST, qs);
    {
        const char *hd[9];

        hd[0] = h_auth;
        hd[1] = h_date;
        hd[2] = h_tok;
        hd[3] = "origin: " DB_ORIGIN;
        hd[4] = "referer: " DB_REFERER;
        hd[5] = "user-agent: " DB_UA;
        hd[6] = "accept: application/json";
        hd[7] = NULL;
        st = do_req(url, body, strlen(body), "application/json", hd, &b,
                    err, sizeof err);
    }
    if (st != 200 || !jstr(b.p, "ImageUri", image_uri, sizeof image_uri)) {
        snprintf(msg, msg_cap, "4/4 Commit 失败 http=%ld %.80s", st,
                 b.p ? b.p : err);
        free(b.p);
        return false;
    }
    free(b.p);

    /* ---- 组装 attachment ---- */
    {
        char uuid[40];
        u32  r1 = (u32)osGetTime();

        snprintf(uuid, sizeof uuid, "%08x-%04x-%04x-%04x-%08x%04x",
                 (unsigned)r1, (unsigned)(rand() & 0xffff), (unsigned)(rand() & 0xffff),
                 (unsigned)(rand() & 0xffff), (unsigned)rand(),
                 (unsigned)(rand() & 0xffff));

        snprintf(att_json, att_cap,
                 "{\"key\":\"%s\",\"name\":\"shot.png\",\"type\":\"image\","
                 "\"file_review_state\":3,\"file_parse_state\":3,"
                 "\"identifier\":\"%s\","
                 "\"option\":{\"width\":%d,\"height\":%d},"
                 "\"md5\":\"%s\",\"size\":%u}",
                 image_uri, uuid, w, h, md5hex, (unsigned)n);
    }
    snprintf(msg, msg_cap, "上传成功");
    return true;
}
