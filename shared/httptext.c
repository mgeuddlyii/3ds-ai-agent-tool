/*
 * httptext.c -- 见 httptext.h
 */

#include "httptext.h"

#include "httpx.h"

#include <string.h>

typedef struct {
    char  *buf;
    size_t cap;
    size_t len;
    bool   overflow;
} TxtCtx;

static size_t on_text(const char *data, size_t len, void *user)
{
    TxtCtx *c = (TxtCtx *)user;
    size_t  room;

    if (c->len + 1 >= c->cap) {
        c->overflow = true;
        return 0;               /* 告诉 curl 停下，别白收 */
    }
    room = c->cap - 1 - c->len;
    if (len > room)
        len = room;
    memcpy(c->buf + c->len, data, len);
    c->len += len;
    return len;
}

long http_get_text(const char *url, char *out, size_t cap, long *http_out)
{
    TxtCtx c;
    HxReq  req;
    char   err[128];
    long   code;

    if (http_out)
        *http_out = 0;
    if (!out || cap < 2)
        return -1;
    out[0] = '\0';

    c.buf = out;
    c.cap = cap;
    c.len = 0;
    c.overflow = false;

    memset(&req, 0, sizeof req);
    req.url         = url;
    req.timeout_s   = 10;       /* 清单在局域网里，10 秒还连不上就是没开 */
    req.follow      = true;
    req.add_cookies = false;

    code = hx_perform(&req, on_text, &c, err, sizeof err);
    if (http_out)
        *http_out = code;
    if (code < 0)
        return -1;                  /* 压根没连上 */
    if (code != 200)
        return 0;                   /* 连上了但被拒（403 未配对 / 404 不存在…） */
    out[c.len] = '\0';
    return (long)c.len;
}
