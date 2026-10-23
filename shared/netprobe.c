/*
 * netprobe.c -- 见 netprobe.h
 *
 * 用 libctru 的 httpc 服务。几个刻意的取舍：
 *
 *  1. **禁用证书校验**（SSLCOPT_DisableVerify）。不是为了偷懒，而是探测
 *     的对象是"网络通不通"，不是"证书链能不能过"。3DS 的根证书列表停在
 *     好几年以前，现代站点多半过不了 —— 那是 M4 要单独处理的问题，
 *     在这里禁掉才能把两件事分开判断。
 *
 *  2. 只取状态码，不读 body。探测不看内容，读 body 只会多一个失败点。
 *
 *  3. 每次都 httpcInit / httpcExit 成对调用。这个服务在 3DS 上属于
 *     重量级资源，长期占着会影响别的程序（以及我们自己的插件）。
 */

#include "netprobe.h"

#include <3ds.h>
#include <stdio.h>

const char *net_probe_default_url(void)
{
    return "http://example.com";
}

bool net_probe(const char *url, int *http_code, char *detail, size_t cap)
{
    Result r;
    httpcContext ctx;
    u32 status = 0;
    bool ok = false;

    if (http_code)
        *http_code = 0;
    if (detail && cap)
        detail[0] = '\0';

    if (!url || !*url)
        url = net_probe_default_url();

    r = httpcInit(0);
    if (R_FAILED(r)) {
        if (detail && cap)
            snprintf(detail, cap, "httpcInit 失败 0x%08lx", (unsigned long)r);
        return false;
    }

    r = httpcOpenContext(&ctx, HTTPC_METHOD_GET, url, 0);
    if (R_FAILED(r)) {
        if (detail && cap)
            snprintf(detail, cap, "httpcOpenContext 失败 0x%08lx",
                     (unsigned long)r);
        httpcExit();
        return false;
    }

    httpcSetSSLOpt(&ctx, SSLCOPT_DisableVerify);
    httpcSetKeepAlive(&ctx, HTTPC_KEEPALIVE_DISABLED);
    httpcAddRequestHeaderField(&ctx, "User-Agent", "AIConsole/0.2");

    r = httpcBeginRequest(&ctx);
    if (R_SUCCEEDED(r))
        r = httpcGetResponseStatusCode(&ctx, &status);

    if (R_FAILED(r)) {
        if (detail && cap)
            snprintf(detail, cap, "请求失败 0x%08lx", (unsigned long)r);
        httpcCloseContext(&ctx);
        httpcExit();
        return false;
    }

    if (http_code)
        *http_code = (int)status;
    if (detail && cap)
        snprintf(detail, cap, "HTTP %lu", (unsigned long)status);

    ok = (status == 200);

    httpcCloseContext(&ctx);
    httpcExit();
    return ok;
}
