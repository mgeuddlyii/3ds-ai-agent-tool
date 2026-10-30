/*
 * netdl.c -- 见 netdl.h
 */

#include "netdl.h"

#include <stdio.h>
#include <string.h>

#include "httpx.h"
#include "sha256.h"

/* 下载时的落盘上下文：文件句柄 + 摘要 + 已收字节 + 上限。 */
typedef struct {
    FILE      *f;
    Sha256Ctx *sh;
    size_t     n;
    size_t     cap;
    bool       io_err;
    bool       too_big;
} Sink;

/* ⚠️ 返回 0 = 让 curl 中止。
 *   写失败、超过上限都必须走到这里 —— 返回 len 假装没事，
 *   后面要么得到一个半截文件，要么把卡写满。 */
static size_t sink_cb(const char *data, size_t len, void *user)
{
    Sink *s = user;

    if (s->io_err || !s->f)
        return 0;
    if (s->n + len > s->cap) {
        s->too_big = true;
        return 0;
    }
    if (fwrite(data, 1, len, s->f) != len) {
        s->io_err = true;
        return 0;
    }
    sha256_update(s->sh, (const unsigned char *)data, len);
    s->n += len;
    return len;
}

bool netdl_fetch(const char *url, const char *dir, const char *name,
                 size_t max_bytes, const char *sha_expect, NetDlResult *out)
{
    char       tmp[192], dst[192];
    FILE      *f;
    Sink       s;
    Sha256Ctx  sh;
    HxReq      req;
    char       err[96];
    long       code;
    size_t     written;

    if (!out)
        return false;
    memset(out, 0, sizeof *out);

    if (!url || !*url || !dir || !*dir || !name || !*name) {
        snprintf(out->msg, sizeof out->msg, "参数不全（url / 文件名）");
        return false;
    }
    snprintf(tmp, sizeof tmp, "%s/%s.partial", dir, name);
    snprintf(dst, sizeof dst, "%s/%s", dir, name);

    f = fopen(tmp, "wb");
    if (!f) {
        snprintf(out->msg, sizeof out->msg, "建不了临时文件（卡满了？）");
        return false;
    }

    sha256_init(&sh);
    s.f       = f;
    s.sh      = &sh;
    s.n       = 0;
    s.cap     = max_bytes;
    s.io_err  = false;
    s.too_big = false;

    memset(&req, 0, sizeof req);
    req.url         = url;
    req.timeout_s   = 180;      /* 几 MB 的文件别用默认 30s */
    req.follow      = true;
    req.add_cookies = false;
    req.persist     = false;    /* 一次性大文件，不占连接池 */
    err[0] = '\0';

    code     = hx_perform(&req, sink_cb, &s, err, sizeof err);
    written  = s.n;
    fclose(f);

    if (code != 200 || s.io_err || s.too_big) {
        /* 失败一律删半截文件：留着它，下一次 rename 会把它当成品。 */
        remove(tmp);
        if (s.too_big)
            snprintf(out->msg, sizeof out->msg,
                     "文件超过 %u KB 上限，已中止", (unsigned)(max_bytes / 1024));
        else if (s.io_err)
            snprintf(out->msg, sizeof out->msg, "写盘失败（卡满了？）已中止");
        else if (code > 0)
            snprintf(out->msg, sizeof out->msg, "服务器返回 HTTP %ld", code);
        else
            snprintf(out->msg, sizeof out->msg, "%s", err[0] ? err : "连不上");
        out->bytes = written;
        return false;
    }

    sha256_final(&sh, out->sha256);

    if (sha_expect && *sha_expect && strcmp(sha_expect, out->sha256) != 0) {
        remove(tmp);            /* 内容不对的文件不能留在卡上 */
        snprintf(out->msg, sizeof out->msg, "摘要不符（期望 %.8s…，实得 %.8s…）",
                 sha_expect, out->sha256);
        out->bytes = written;
        return false;
    }

    remove(dst);                /* Windows 的 rename 不覆盖 */
    if (rename(tmp, dst) != 0) {
        remove(tmp);
        snprintf(out->msg, sizeof out->msg, "改名失败（同名文件被占用？）");
        out->bytes = written;
        return false;
    }

    out->ok    = true;
    out->bytes = written;
    snprintf(out->msg, sizeof out->msg, "已下载 %u 字节", (unsigned)written);
    return true;
}
