/*
 * otaprobe.c -- 远程更新的网络链路验收
 *
 * ── 为什么要有它 ──
 * "网络远程更新"在真机上一直是坏的，但坏在哪一层没人说得清：DNS？TCP？
 * TLS？还是 HTTP？2026-10-24 查下来是**我们选错了客户端** —— 原来走
 * libctru 的 httpc，而 httpc/sslc 只到 TLS 1.1，阿里云 OSS 只收 TLS 1.2，
 * 于是握手必然失败。换成 curl + mbedtls 之后必须能证明"真的通了"，
 * 而不是"我改完了，应该通了吧"。
 *
 * 所以它做三件事，结论逐行写进 status.txt：
 *   1. 逐个源报一次握手结果（哪个源连得上、哪个连不上，一眼看出）
 *   2. 真正调一次 ota_fetch_remote()，报它落在哪个状态
 *   3. 给一句 RESULT=PASS/FAIL/SKIP
 *
 * ── 判据为什么是"状态"而不是"HTTP 200" ──
 * ota_fetch_remote 返回 false 有很多种：网上版本比本机旧（这是**正常**的！
 * 说明链路完全通了）、解析失败、连不上。三者必须分开，否则"线上比本机旧"
 * 会被误报成"网络坏了" —— 那正是这次要避免的那种误导。
 *
 * ── 只读性 ──
 * 只取几百字节的 manifest，不下载产物、不写卡、不改配置。
 */

#include "otaprobe.h"

#include <stdio.h>
#include <string.h>

#include <3ds.h>            /* osGetTime：量下载耗时 */

#include "app.h"
#include "httpx.h"
#include "ota.h"

#define PROBE_CHANNEL "stable"

static size_t sink_drop(const char *d, size_t len, void *u)
{
    (void)d; (void)u;
    return len;                 /* 只关心状态码，正文丢掉 */
}

/* 对某一个源单独发一次 GET，返回状态码（负 = 网络层失败）。 */
static long probe_base(const char *base)
{
    char  url[192];
    char  err[96];
    HxReq req;

    snprintf(url, sizeof url, "%s/%s/manifest.json", base, PROBE_CHANNEL);
    memset(&req, 0, sizeof req);
    req.url       = url;
    /* 25s：这台机器到香港 OSS 实测能慢到 15s 以上（PC 上 0.5s，
     * 模拟器/真机的 TLS 更贵）。给 15s 会把它误判成"不通"。 */
    req.timeout_s = 25;
    req.follow    = true;
    req.persist   = true;
    err[0] = '\0';

    {
        long code = hx_perform(&req, sink_drop, NULL, err, sizeof err);
        if (code < 0)
            app_note("otanet: base %s -> curl %ld %s", base, code, err);
        else
            app_note("otanet: base %s -> HTTP %ld", base, code);
        return code;
    }
}

void otaprobe_run(void)
{
    OtaManifest m;
    OtaState    st = OTA_ERR_NET;
    char        detail[128];
    double      d = 0, c = 0, t = 0, tt = 0;
    long        best = 0;       /* 逐源探测里最好的那个状态码 */
    int         reachable = 0;  /* 有没有任何一个源答话了 */
    int         pass;

    app_note("otanet: begin bases=%d", ota_base_count());

    memset(&m, 0, sizeof m);
    detail[0] = '\0';

    for (int i = 0; i < ota_base_count(); ++i) {
        const char *b = ota_base_url(i);
        long code = b ? probe_base(b) : -1;

        if (code >= 0) {
            reachable = 1;
            if (!best || code < best)
                best = code;
        }
    }

    /* 真正走一次产品路径（含换源逻辑）。 */
    ota_fetch_remote(PROBE_CHANNEL, &m, &st, detail, sizeof detail);
    hx_last_timing(&d, &c, &t, &tt);

    app_note("otanet: fetch state=%d version=%d name=%s base=%s detail=%s",
             (int)st, m.version_code, m.version_name, ota_last_base(),
             detail[0] ? detail : "-");
    app_note("otanet: net dns=%.2f conn=%.2f tls=%.2f total=%.2f", d, c, t, tt);

    /* ★ 判定以**真正那次 fetch 的状态**为准，不以"逐源探测"为准。
     *   第一版写成"探测里有人答话才算通过"，结果是：探测那条 15s 超时了，
     *   而紧接着的 fetch 其实拿到了 manifest（state=7）—— 于是把成功判成
     *   SKIP，而 SKIP 在断言里算通过。**那是假绿**，比判红更坏。
     *
     *   OTA_ERR_TOO_OLD 也算 PASS：它说明"线上比本机旧"，链路本身是通的；
     *   报成失败就是把正常情况说成故障。 */
    pass = (st == OTA_UP_TO_DATE || st == OTA_AVAILABLE || st == OTA_ERR_TOO_OLD);

    if (pass) {
        app_note("otanet: RESULT=PASS stat=%d best_http=%ld", (int)st, best);
        return;
    }

    /* 没拿到 manifest。这时才用逐源探测区分"环境不通"和"我们代码有问题"：
     * 一个源都没答话 → SKIP（网络问题）；有源答过话却仍失败 → FAIL（缺陷）。 */
    if (!reachable)
        app_note("otanet: RESULT=SKIP 一个源都没答话（查 Wi-Fi），验不到东西");
    else
        app_note("otanet: RESULT=FAIL state=%d —— 有源答话但 manifest 没拿到",
                 (int)st);
}

/* --------------------------------------------------------------------------
 * 下载产物：验证「流式写盘 + 边下边算 sha256 + rename」这条新路径。
 *
 * 为什么要单独探一次：manifest 通了不等于包能下下来 —— 那段是这次一起换的
 * （file_sink + 大小比对），错一点的表现是"下载完成但摘要不符"，
 * 而那句话会把人引向"线上文件被换过"，方向完全错。
 * ----------------------------------------------------------------------- */
void otadlprobe_run(void)
{
    OtaManifest m;
    OtaState    st = OTA_ERR_NET;
    char        detail[128];
    char        err[128];
    long        t0, dt;
    int         idx = -1;
    bool        ok;

    memset(&m, 0, sizeof m);
    detail[0] = '\0';
    err[0] = '\0';

    app_note("otadl: begin");

    /* ⚠️ 这里**不能看 ota_fetch_remote 的返回值**。
     *   "线上版本比本机旧"会被它判成 false（UI 该拒绝降级，这是对的），
     *   但 m 已经**解析好了** —— 拿返回值当"没拿到 manifest"会白跑一趟。
     *   判据只能是状态：这三档说明 manifest 是真的拿到了。 */
    ota_fetch_remote(PROBE_CHANNEL, &m, &st, detail, sizeof detail);
    if (!(st == OTA_AVAILABLE || st == OTA_UP_TO_DATE || st == OTA_ERR_TOO_OLD)) {
        app_note("otadl: RESULT=SKIP 取不到 manifest（state=%d %s）",
                 (int)st, detail[0] ? detail : "-");
        return;
    }
    if (m.count <= 0) {
        app_note("otadl: RESULT=SKIP manifest 里没有产物");
        return;
    }

    /* 挑最小的那个：这是验收，不是给用户装包，别在模拟器里耗两分钟。 */
    idx = 0;
    for (int i = 1; i < m.count; ++i)
        if (m.art[i].size < m.art[idx].size)
            idx = i;

    app_note("otadl: file=%s size=%d", m.art[idx].file, m.art[idx].size);

    t0 = osGetTime();
    ok = ota_download(PROBE_CHANNEL, &m, idx, err, sizeof err);
    dt = osGetTime() - t0;

    app_note("otadl: done ok=%d %ldms err=%s", (int)ok, (long)dt,
             err[0] ? err : "-");
    app_note("otadl: RESULT=%s", ok ? "PASS" : "FAIL");
}
