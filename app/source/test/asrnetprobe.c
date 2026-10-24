/*
 * asrnetprobe.c -- 语音请求的"连接复用"验收探针
 *
 * ── 它要回答的问题 ──
 * 语音慢，到底是慢在**握手**还是慢在**传音频**？
 * 我们把语音请求改成了复用长连接（HxReq.persist），可是"改完了更快"
 * 这句话本身不可测 —— 除非能分别看到 DNS / TCP / TLS 三段各花了多久。
 *
 * 做法很直白：对**同一个主机**连发两次请求，把两次的三段耗时都打出来。
 * 判据是"第二次的 conn 和 tls 必须是 0"：
 *   · conn = 0  → 没有重新做 TCP 三次握手（连接真的留着）；
 *   · tls  = 0  → 没有重新握手（TLS 会话也复用了）。
 * 两个都是 0 才叫复用成功。只看"总时长变短"是不够的 ——
 * 服务端缓存、网络抖动都能让总时长变短，那证明不了我们的池子生效。
 *
 * ── 为什么不是每次回归都跑 ──
 * 它**要联网**。离线环境（或 Wi-Fi 没连）下必然失败，而那是环境问题
 * 不是回归问题。所以它跟 perfprobe 一样是"手动调用的尺子"，
 * 不进常规门禁；进常规门禁的是"离线就能验"的那部分（见 micprobe）。
 *
 * ── 只读性 ──
 * 只发 GET /v1/models（不带 Key，服务端回 401 也算通过 —— 我们要的是
 * 连接过程，不是业务结果）。不写卡、不花钱、不改配置。
 */

#include "asrnetprobe.h"

#include <stdio.h>
#include <string.h>

#include "app.h"
#include "httpx.h"

/* 探测目标：语音服务商的主机。换默认服务商时这里要跟着换，
 * 否则量出来的是"另一台机器的握手"，与本 App 的语音链路无关。 */
#define PROBE_URL "https://api.siliconflow.cn/v1/models"

typedef struct {
    long   http;
    double dns, conn, tls, total;
} NetOne;

static void once(const char *tag, NetOne *o)
{
    HxReq req;
    char  err[96];

    memset(o, 0, sizeof *o);
    memset(&req, 0, sizeof req);
    err[0] = '\0';

    req.url       = PROBE_URL;
    req.timeout_s = 15;
    req.follow    = false;
    req.add_cookies = false;
    /* ★ 这一行就是被测对象：置 true 才走连接池。 */
    req.persist   = true;

    o->http = hx_perform(&req, NULL, NULL, err, sizeof err);
    hx_last_timing(&o->dns, &o->conn, &o->tls, &o->total);

    app_note("asrnet: %s http=%ld dns=%.3f conn=%.3f tls=%.3f total=%.3f%s%s",
             tag, o->http, o->dns, o->conn, o->tls, o->total,
             (o->http < 0) ? " err=" : "",
             (o->http < 0) ? err : "");
}

void asrnetprobe_run(void)
{
    NetOne cold, warm;
    int    pass;

    app_note("asrnet: begin url=%s", PROBE_URL);

    once("cold", &cold);
    once("warm", &warm);

    if (cold.http < 0) {
        /* 连第一次都没连上：这是环境问题（Wi-Fi / DNS），
         * 不能说"复用失败" —— 那样会把网络故障记成代码缺陷。 */
        app_note("asrnet: RESULT=SKIP 第一次就没连上（查 Wi-Fi），无判据可言");
        return;
    }

    /* 判据：第二次**不得**重新建连、也不得重新握手。
     * 用 0.005s 而不是严格的 0.0 —— curl 报的是 double，某些平台会给
     * 一个极小的非零残值；用严格相等会把"其实复用了"判成失败。 */
    pass = (warm.http >= 0) && (warm.conn <= 0.005) && (warm.tls <= 0.005);

    app_note("asrnet: RESULT=%s cold(conn=%.3f tls=%.3f total=%.3f) "
             "warm(conn=%.3f tls=%.3f total=%.3f)",
             pass ? "PASS" : "FAIL",
             cold.conn, cold.tls, cold.total,
             warm.conn, warm.tls, warm.total);

    if (pass) {
        app_note("asrnet: 判据 = 第二次没有重新建连/握手（conn=tls=0），"
                 "省下 %.2fs", cold.total - warm.total);
    } else {
        app_note("asrnet: 判据未过 —— 第二次仍在建连(conn=%.3f)或握手(tls=%.3f)，"
                 "连接池没生效", warm.conn, warm.tls);
    }
}
