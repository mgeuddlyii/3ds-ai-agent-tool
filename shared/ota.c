/*
 * ota.c -- 见 ota.h
 *
 * manifest 解析复用 minijson。数组元素的路径形如 "artifacts[0].sha256"，
 * 所以这里按前缀 + 下标来分派，而不是逐个 strcmp 全名。
 */

#include "ota.h"

#include "minijson.h"
#include "sha256.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <3ds.h>

#define DIR_PATH     "sdmc:/3ds/aiconsole"
#define OTA_DIR      DIR_PATH "/update"
#define OTA_MANIFEST OTA_DIR "/manifest.json"

/* 3dsx 的目标位置：3ds/ 是 homebrew 的惯例目录。
 * 注意我们并不知道用户原来把 .3dsx 放在哪，所以装完之后
 * 必须把实际路径如实地告诉用户 —— 见 ota_install 的 err 文案。 */
#define TARGET_3DSX  "sdmc:/3ds/AIConsole.3dsx"
#define TARGET_CIA   "sdmc:/cias/AIConsole.cia"

/* 发布基址。改这里要同时改 tools/make_update.py 的说明。
 * 目前离线通道是主路径，它只作占位与将来的在线检查用。 */
/* ★ 默认值是**故意不可用**的占位（example.invalid 是保留域名，永远解析不了）：
 * 这样"忘了配更新源"的表现是"检查失败"，而不是"悄悄连到某个第三方"。
 *
 * 发布前必须二选一：
 *   a. 改这里的字面量；或
 *   b. 构建时注入：AIC_EXTRA_CFLAGS='-DOTA_BASE_URL="https://your.host/aiconsole"'
 *      —— 验收时指向本机测试服务就走这条路，不必动源码。
 *
 * ⚠️ 它刻意是**编译期常量**，不进 config.json：
 *    能让用户改 URL 等于给中间人留一个"把我指到我的服务器"的后门。
 *    这条原则写在 ota.h 的 G1 里。 */
#ifndef OTA_BASE_URL
/* ★ 已定稿为正式更新源（2026-10-17）：阿里云 OSS **中国香港** Bucket
 *   + 自定义域名 update.acrac.cn（香港地域无需 ICP 备案）。
 *
 *   ★★ 这个字符串一旦发版就写死在所有用户机器里，改不了。
 *      所以它用的是**子域名**而不是主域 —— 以后从 OSS 换到任何别的地方，
 *      只需要改 DNS 的 CNAME，客户端一行代码都不用动。
 *
 *   构建期仍可用 -DOTA_BASE_URL=... 覆盖（验收时指向本机测试服务）。 */
#define OTA_BASE_URL "https://update.acrac.cn/aiconsole"
#endif
const char kUpdateBase[] = OTA_BASE_URL;

const char *ota_dir(void) { return OTA_DIR; }

const char *ota_state_text(OtaState s)
{
    switch (s) {
        case OTA_NONE:        return "还没检查";
        case OTA_NO_PACKAGE:  return "卡上没有更新包";
        case OTA_UP_TO_DATE:  return "已经是最新";
        case OTA_AVAILABLE:   return "有可用更新";
        case OTA_VERIFY_OK:   return "校验通过，可安装";
        case OTA_DONE:        return "已就位";
        case OTA_ERR_PARSE:   return "manifest 读不出来";
        case OTA_ERR_TOO_OLD: return "这是个降级包，已拒绝";
        case OTA_ERR_MISMATCH: return "文件大小或摘要对不上";
        case OTA_ERR_IO:      return "读写失败";
        default:              return "?";
    }
}

/* --------------------------------------------------------------------------
 * 解析
 * ----------------------------------------------------------------------- */

static void copy_str(char *dst, size_t cap, const char *src)
{
    if (cap == 0)
        return;
    strncpy(dst, src ? src : "", cap - 1);
    dst[cap - 1] = '\0';
}

typedef struct {
    OtaManifest *m;
    bool         ok;
} ParseCtx;

static bool on_value(const MiniJsonValue *v, void *user)
{
    ParseCtx *pc = (ParseCtx *)user;
    OtaManifest *m = pc->m;
    const char *p = v->path;

    if (v->type == MJ_OBJECT_START || v->type == MJ_OBJECT_END ||
        v->type == MJ_ARRAY_START  || v->type == MJ_ARRAY_END)
        return true;

    /* ---- artifacts[].xxx ----
     * 注意 minijson 对数组元素**不拼 [i]**：路径就是 "artifacts.kind"，
     * 下标在 v->index 里（parse_array 只把 index 往下传）。
     * 早先按 "artifacts[0].kind" 去匹配，结果一个字段都没命中。 */
    if (strncmp(p, "artifacts.", 10) == 0) {
        int idx = v->index;
        const char *field = p + 10;

        if (idx < 0 || idx >= OTA_MAX_ARTIFACTS)
            return true;                 /* 超了就丢弃，不把解析判成失败 */

        if (idx >= m->count)
            m->count = idx + 1;

        if (v->type == MJ_STRING) {
            if (!strcmp(field, "kind"))    copy_str(m->art[idx].kind,    sizeof m->art[idx].kind,    v->str);
            else if (!strcmp(field, "file"))copy_str(m->art[idx].file,   sizeof m->art[idx].file,   v->str);
            else if (!strcmp(field, "sha256")) copy_str(m->art[idx].sha256, sizeof m->art[idx].sha256, v->str);
            else if (!strcmp(field, "install")) copy_str(m->art[idx].install, sizeof m->art[idx].install, v->str);
        } else if (v->type == MJ_NUMBER) {
            if (!strcmp(field, "size"))   m->art[idx].size = (int)v->num;
        }
        return true;
    }

    /* ---- 顶层 ---- */
    if (v->type == MJ_NUMBER) {
        if (!strcmp(p, "schema"))          m->schema = (int)v->num;
        else if (!strcmp(p, "version_code"))  m->version_code = (int)v->num;
        else if (!strcmp(p, "min_supported")) m->min_supported = (int)v->num;
    } else if (v->type == MJ_STRING) {
        if (!strcmp(p, "version_name")) copy_str(m->version_name, sizeof m->version_name, v->str);
        else if (!strcmp(p, "channel")) copy_str(m->channel, sizeof m->channel, v->str);
        else if (!strcmp(p, "notes"))   copy_str(m->notes, sizeof m->notes, v->str);
    }

    return true;
}

bool ota_parse(const char *json, size_t n, OtaManifest *m)
{
    ParseCtx ctx;

    if (!json || !m)
        return false;

    memset(m, 0, sizeof *m);
    ctx.m = m;
    ctx.ok = true;

    if (!minijson_walk(json, n, on_value, &ctx))
        return false;

    /* version_code 是 G3 的唯一依据，缺了就当这份 manifest 不可信 */
    if (m->version_code <= 0)
        return false;

    return true;
}

int ota_compare(const OtaManifest *m, int cur_code)
{
    if (m->version_code > cur_code) return 1;
    if (m->version_code < cur_code) return -1;
    return 0;
}

/* --------------------------------------------------------------------------
 * 校验与安装
 * ----------------------------------------------------------------------- */

static void ensure_dir(const char *path)
{
    char tmp[160];
    const char *scan;
    char *q;

    if (strlen(path) >= sizeof tmp)
        return;

    strcpy(tmp, path);
    scan = (tmp[0] && tmp[1] == ':') ? tmp + 3 : tmp;

    for (q = (char *)scan; *q; ++q) {
        if (*q == '/') {
            *q = '\0';
            mkdir(tmp, 0777);
            *q = '/';
        }
    }
    mkdir(tmp, 0777);
}

static bool file_size(const char *path, int *out)
{
    struct stat st;
    if (stat(path, &st) != 0)
        return false;
    *out = (int)st.st_size;
    return true;
}

/* 先写 .tmp 再改名（G5）。3DS 上拔卡断电是常态，
 * 半个文件比没有文件更糟 —— 尤其当它覆盖的是正在运行的程序时。 */
static bool copy_then_rename(const char *src, const char *dst)
{
    FILE *a = fopen(src, "rb");
    FILE *b;
    char tmp[200];
    static uint8_t chunk[8192];
    size_t got;
    bool ok = true;

    if (!a)
        return false;

    snprintf(tmp, sizeof tmp, "%s.tmp", dst);
    ensure_dir(dst);
    remove(tmp);

    b = fopen(tmp, "wb");
    if (!b) {
        fclose(a);
        return false;
    }

    while ((got = fread(chunk, 1, sizeof chunk, a)) > 0) {
        if (fwrite(chunk, 1, got, b) != got) {
            ok = false;
            break;
        }
    }

    fclose(a);
    if (fclose(b) != 0)
        ok = false;

    if (!ok) {
        remove(tmp);
        return false;
    }

    remove(dst);
    if (rename(tmp, dst) != 0) {
        /* rename 在 fs devoptab 上不总是可用，退回复制。
         *
         * ★ 回退复制**必须查错**（与 pkg.c 的同一条教训）：
         *   早先这里 fread/fwrite/fclose 的返回值全没看，然后
         *   直接 `return true`。卡满时目标会变成**半截文件**却被报成功，
         *   而这个函数更新的正是**正在运行的 3dsx**，
         *   真出问题时表现是"下次开机程序没了"，根本追不回来。
         *   现在任一步失败都删掉半成品并返回 false。 */
        bool cok = true;
        FILE *s = fopen(tmp, "rb");
        FILE *d = fopen(dst, "wb");
        if (!s || !d) {
            if (s) fclose(s);
            if (d) fclose(d);
            remove(tmp);
            remove(dst);
            return false;
        }
        while ((got = fread(chunk, 1, sizeof chunk, s)) > 0) {
            if (fwrite(chunk, 1, got, d) != got) {
                cok = false;
                break;
            }
        }
        if (ferror(s))
            cok = false;
        fclose(s);
        if (fclose(d) != 0)
            cok = false;
        if (!cok) {
            remove(dst);        /* 不留半截的可执行文件 */
            remove(tmp);
            return false;
        }
        remove(tmp);
    }

    return true;
}

/* 公共实现：verify=false 时跳过 sha256（只验存在性 + size）。
 * 见 ota.h 里 ota_scan_local_fast 的说明。 */
static bool scan_local_impl(OtaManifest *m, OtaState *state,
                            char *detail, size_t cap, bool verify)
{
    FILE *f;
    static char buf[8192];
    size_t n;
    int cmp;

    if (state) *state = OTA_NONE;
    if (detail && cap) detail[0] = '\0';

    f = fopen(OTA_MANIFEST, "rb");
    if (!f) {
        if (state) *state = OTA_NO_PACKAGE;
        if (detail && cap)
            snprintf(detail, cap, "没找到 %s", OTA_MANIFEST);
        return false;
    }

    n = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    buf[n] = '\0';

    if (!ota_parse(buf, n, m)) {
        if (state) *state = OTA_ERR_PARSE;
        if (detail && cap)
            snprintf(detail, cap, "manifest 解析失败或缺少 version_code");
        return false;
    }

    cmp = ota_compare(m, APP_VERSION_CODE);
    if (cmp < 0) {
        /* G3：降级包一律拒绝。这是安全信号，不是"出错" ——
         * 有人想推旧版本回来，绕掉后来修掉的东西。 */
        if (state) *state = OTA_ERR_TOO_OLD;
        if (detail && cap)
            snprintf(detail, cap, "包里是 v%d，当前已是 v%d —— 拒绝降级",
                     m->version_code, APP_VERSION_CODE);
        return false;
    }
    if (cmp == 0) {
        if (state) *state = OTA_UP_TO_DATE;
        if (detail && cap)
            snprintf(detail, cap, "包里是 v%d，和当前一样", m->version_code);
        return false;
    }

    /* 有更新 —— 逐个产物验 size（轻量版到此为止）和 sha256（完整版） */
    for (int i = 0; i < m->count; ++i) {
        char path[192];
        char hex[65];
        int  sz = 0;

        snprintf(path, sizeof path, "%s/%s", OTA_DIR, m->art[i].file);

        if (!file_size(path, &sz)) {
            if (state) *state = OTA_ERR_MISMATCH;
            if (detail && cap)
                snprintf(detail, cap, "缺文件 %s", m->art[i].file);
            return false;
        }
        if (sz != m->art[i].size) {
            if (state) *state = OTA_ERR_MISMATCH;
            if (detail && cap)
                snprintf(detail, cap, "%s 大小不符：包里写 %d，实际 %d",
                         m->art[i].file, m->art[i].size, sz);
            return false;
        }
        if (!verify)
            continue;               /* 轻量版：size 对上就够，摘要留给安装前 */

        if (!sha256_file(path, hex) || strcmp(hex, m->art[i].sha256) != 0) {
            if (state) *state = OTA_ERR_MISMATCH;
            if (detail && cap)
                snprintf(detail, cap, "%s 摘要不符", m->art[i].file);
            return false;
        }
    }

    if (state) *state = verify ? OTA_VERIFY_OK : OTA_AVAILABLE;
    if (detail && cap) {
        if (verify)
            snprintf(detail, cap, "v%d → v%s，%d 个产物全部校验通过",
                     APP_VERSION_CODE, m->version_name, m->count);
        else
            snprintf(detail, cap, "v%d → v%s，%d 个产物（摘要待安装前校验）",
                     APP_VERSION_CODE, m->version_name, m->count);
    }
    return true;
}

bool ota_scan_local(OtaManifest *m, OtaState *state, char *detail, size_t cap)
{
    return scan_local_impl(m, state, detail, cap, true);
}

bool ota_scan_local_fast(OtaManifest *m, OtaState *state,
                         char *detail, size_t cap)
{
    return scan_local_impl(m, state, detail, cap, false);
}

bool ota_install(const OtaManifest *m, int idx, char *err, size_t errsz)
{
    char src[192];
    const char *dst;

    if (!m || idx < 0 || idx >= m->count) {
        if (err && errsz) snprintf(err, errsz, "产物下标不对");
        return false;
    }

    /* 用标志而不是拿 dst 去比字符串常量地址 ——
     * 两个不同的字面量恰好同地址是未定义行为，编译器会警告。 */
    bool is_cia = !strcmp(m->art[idx].install, "fbi");

    snprintf(src, sizeof src, "%s/%s", OTA_DIR, m->art[idx].file);

    if (!strcmp(m->art[idx].install, "replace")) {
        dst = TARGET_3DSX;
    } else if (is_cia) {
        dst = TARGET_CIA;
    } else {
        if (err && errsz) snprintf(err, errsz, "不认识的 install 类型：%s",
                                   m->art[idx].install);
        return false;
    }

    if (!copy_then_rename(src, dst)) {
        if (err && errsz) snprintf(err, errsz, "复制失败：%s", src);
        return false;
    }

    /* 这两句话必须如实 —— 不能让人以为按一下就完事了 */
    if (is_cia) {
        if (err && errsz)
            snprintf(err, errsz, "已放到 %s。装 CIA 得你自己去 FBI 里点一下 —— "
                                 "那是 AM 服务的权限，自制程序拿不到。", dst);
    } else {
        if (err && errsz)
            snprintf(err, errsz, "已覆盖 %s。下次从这里启动就是新版本。", dst);
    }
    return true;
}


/* 上一次 fetch 的原始文本。**故意是文件级静态**：
 * 它只在"检查 → 下载"这一小段里有效，不跨请求、不跨屏，
 * 交给调用方管理生命周期反而容易忘。 */
static char   s_remote_raw[4096];
static size_t s_remote_raw_n;

const char *ota_remote_raw(size_t *n)
{
    if (n) *n = s_remote_raw_n;
    return s_remote_raw;
}

/* ==========================================================================
 * 在线通道（L2）：检查 / 下载
 *
 * ★ 三条纪律，写在这里免得以后有人"优化"掉：
 *
 * 1) **检查只读 manifest，不读包。** manifest 几百字节，包几 MB；
 *    启动时顺手检查是可行的，顺手下载是不可行的。
 *
 * 2) **下载只写 .partial，校验通过才 rename。**
 *    直接往正式文件名写，中途断网就留一个半个二进制；
 *    下一次 ota_scan_local 会把它当成"包在卡上"，然后报摘要不符 ——
 *    用户看到的是"更新坏了"，而不是"下载没下完"。
 *
 * 3) **channel 必须白名单。** 它来自 config.json（人手可改），
 *    直接拼进 URL 等于让人把请求指到任意路径。
 * ========================================================================== */

/* channel 只允许小写字母/数字/下划线/连字符，最长 11。
 * 这不是"防注入"的万全之策，但足以挡住 "../" 和各种路径花招 ——
 * 而代价只有一次逐字符判断。 */
static bool channel_ok(const char *ch)
{
    size_t n;

    if (!ch || !*ch)
        return false;
    n = strlen(ch);
    if (n > 11)
        return false;
    for (size_t i = 0; i < n; ++i) {
        char c = ch[i];
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')
              || c == '_' || c == '-'))
            return false;
        }
    return true;
}

/* 发一个 GET 并拿回状态码。返回 false 表示连请求都没发出去/没拿到响应 ——
 * 与"拿到了但不是 200"是两件事，调用方要能分开说。 */
static bool http_get_open(httpcContext *ctx, const char *url, u32 *status,
                          char *err, size_t errsz)
{
    Result r;

    *status = 0;
    r = httpcInit(0);
    if (R_FAILED(r)) {
        if (err && errsz) snprintf(err, errsz, "httpcInit 0x%08lx",
                                   (unsigned long)r);
        return false;
    }
    r = httpcOpenContext(ctx, HTTPC_METHOD_GET, url, 0);
    if (R_FAILED(r)) {
        if (err && errsz) snprintf(err, errsz, "open 0x%08lx",
                                   (unsigned long)r);
        httpcExit();
        return false;
    }
    /* 3DS 没有根证书库，只能关掉校验 —— 这条**必须**如实告诉用户：
     * 它意味着"能防传输损坏，防不了中间人"。真正的防线是
     * 产物 sha256 + CIA 的 Title ID 白名单 + 拒降级。 */
    httpcSetSSLOpt(ctx, SSLCOPT_DisableVerify);
    httpcSetKeepAlive(ctx, HTTPC_KEEPALIVE_DISABLED);
    httpcAddRequestHeaderField(ctx, "User-Agent", "AIConsole/ota");

    r = httpcBeginRequest(ctx);
    if (R_SUCCEEDED(r))
        r = httpcGetResponseStatusCode(ctx, status);
    if (R_FAILED(r)) {
        if (err && errsz) snprintf(err, errsz, "req 0x%08lx",
                                   (unsigned long)r);
        httpcCloseContext(ctx);
        httpcExit();
        return false;
    }
    return true;
}

/* 注意：这个宏故意写成小函数 —— 两个调用点都要 关 context + Exit，
 * 漏一个会让 httpc 服务留在"用过了但没释放"的状态，
 * 下一次检查直接失败（症状是"第一次能检查，之后永远连不上"）。 */
static void http_get_close(httpcContext *ctx)
{
    httpcCloseContext(ctx);
    httpcExit();
}

bool ota_fetch_remote(const char *channel, OtaManifest *m, OtaState *state,
                      char *detail, size_t cap)
{
    httpcContext ctx;
    u32 status = 0;
    u32 got = 0, total = 0;
    char url[160];
    static char body[4096];
    bool ok = false;

    if (state)  *state = OTA_ERR_NET;
    if (detail && cap) detail[0] = '\0';

    if (!m)
        return false;
    if (!channel_ok(channel)) {
        if (detail && cap)
            snprintf(detail, cap, "%s", "channel 名不合法");
        return false;
    }

    snprintf(url, sizeof url, "%s/%s/manifest.json", kUpdateBase, channel);

    if (!http_get_open(&ctx, url, &status, detail, cap))
        return false;

    if (status != 200) {
        if (detail && cap)
            snprintf(detail, cap, "HTTP %lu", (unsigned long)status);
        http_get_close(&ctx);
        return false;
    }

    /* content-length 先看一眼：manifest 超过 4KB 说明这个 URL 给的不是
     * manifest（常见于托管站返回一个 HTML 404 页），继续读下去只会
     * 让解析器报一句"manifest 读不出来"，而真因是地址填错了。 */
    httpcGetDownloadSizeState(&ctx, &got, &total);
    if (total == 0 || total >= sizeof body) {
        if (detail && cap)
            snprintf(detail, cap, "大小异常（%lu 字节）", (unsigned long)total);
        http_get_close(&ctx);
        return false;
    }

    for (u32 off = 0; off < total; ) {
        u32 want = total - off;
        if (want > 1024) want = 1024;
        if (R_FAILED(httpcReceiveData(&ctx, (u8 *)body + off, want)))
            break;
        off += want;
        got = off;
    }
    body[got] = '\0';
    http_get_close(&ctx);

    if (got != total) {
        if (detail && cap)
            snprintf(detail, cap, "只收到 %lu/%lu 字节",
                     (unsigned long)got, (unsigned long)total);
        return false;
    }

    /* 留一份原文：下载完要原样落盘（见 ota_remote_raw 的说明） */
    if (got < sizeof body) {
        memcpy(s_remote_raw, body, got);
        s_remote_raw[got] = '\0';
        s_remote_raw_n = got;
    }

    if (!ota_parse(body, got, m)) {
        if (state) *state = OTA_ERR_PARSE;
        if (detail && cap)
            snprintf(detail, cap, "%s", "manifest 解析失败");
        return false;
    }

    /* 版本比较复用离线通道那套语义：0 = 最新，-1 = 降级包（拒绝），1 = 有更新 */
    {
        int cmp = ota_compare(m, APP_VERSION_CODE);
        if (cmp < 0) {
            if (state) *state = OTA_ERR_TOO_OLD;
            if (detail && cap)
                snprintf(detail, cap, "线上 v%s 比本机旧，已拒绝",
                         m->version_name);
            return false;
        }
        if (cmp == 0 || m->version_code == APP_VERSION_CODE) {
            if (state) *state = OTA_UP_TO_DATE;
            if (detail && cap)
                snprintf(detail, cap, "线上 v%s，已经是最新", m->version_name);
            ok = true;
        } else {
            if (state) *state = OTA_AVAILABLE;
            if (detail && cap)
                snprintf(detail, cap, "线上 v%s 可用（%d 个产物）",
                         m->version_name, m->count);
            ok = true;
        }
    }
    return ok;
}

bool ota_download(const char *channel, const OtaManifest *m, int idx,
                  char *err, size_t errsz)
{
    httpcContext ctx;
    u32 status = 0;
    u32 got = 0, total = 0;
    char url[192];
    char tmp[192];
    char dst[192];
    char hex[65];
    FILE *f = NULL;
    Sha256Ctx sh;
    uint8_t dg[32];
    u32 written = 0;
    bool ok = false;

    if (err && errsz) err[0] = '\0';
    if (!m || idx < 0 || idx >= m->count)
        return false;
    if (!channel_ok(channel)) {
        if (err && errsz) snprintf(err, errsz, "%s", "channel 名不合法");
        return false;
    }

    /* 产物文件名同样不能直接拼进 URL/路径 —— 它来自远端 manifest，
     * 是**外部输入**。挡掉斜杠就够了（我们自己的产物名都是平的文件名）。 */
    if (strchr(m->art[idx].file, '/') || strchr(m->art[idx].file, '\\')) {
        if (err && errsz) snprintf(err, errsz, "%s", "产物名不合法");
        return false;
    }

    snprintf(url, sizeof url, "%s/%s/%s", kUpdateBase, channel,
             m->art[idx].file);
    snprintf(tmp, sizeof tmp, "%s/%s.partial", OTA_DIR, m->art[idx].file);
    snprintf(dst, sizeof dst, "%s/%s", OTA_DIR, m->art[idx].file);

    if (!http_get_open(&ctx, url, &status, err, errsz))
        return false;
    if (status != 200) {
        if (err && errsz) snprintf(err, errsz, "HTTP %lu", (unsigned long)status);
        http_get_close(&ctx);
        return false;
    }

    httpcGetDownloadSizeState(&ctx, &got, &total);
    if ((int)total != m->art[idx].size) {
        /* 提前拒绝，省掉几 MB 的流量 —— 而且这条错误比"下完才说摘要不符"
         * 有用得多：它直接指向"线上文件与 manifest 不一致"。 */
        if (err && errsz)
            snprintf(err, errsz, "大小不符：线上 %lu，声明 %d",
                     (unsigned long)total, m->art[idx].size);
        http_get_close(&ctx);
        return false;
    }

    f = fopen(tmp, "wb");
    if (!f) {
        if (err && errsz) snprintf(err, errsz, "%s", "建不了临时文件");
        http_get_close(&ctx);
        return false;
    }
    sha256_init(&sh);

    while (written < total) {
        u8 buf[1024];
        u32 want = total - written;
        if (want > sizeof buf) want = sizeof buf;
        if (R_FAILED(httpcReceiveData(&ctx, buf, want)))
            break;
        if (fwrite(buf, 1, want, f) != want)
            break;
        sha256_update(&sh, buf, want);
        written += want;
    }
    http_get_close(&ctx);
    fclose(f);

    if (written != total) {
        remove(tmp);
        if (err && errsz)
            snprintf(err, errsz, "下载中断（%lu/%lu）",
                     (unsigned long)written, (unsigned long)total);
        return false;
    }

    sha256_final(&sh, dg);
    sha256_hex(dg, hex);
    if (strcmp(hex, m->art[idx].sha256) != 0) {
        /* 这一条要单独说清楚：**不是网络问题**。可能是线上文件被换过、
         * 或者是中间人（TLS 校验关着，见 http_get_open 的说明）。 */
        remove(tmp);
        if (err && errsz)
            snprintf(err, errsz, "摘要不符，已删除（%s…）", "");
        return false;
    }

    /* 先删旧的再 rename：某些 FAT 实现下 rename 到已存在的文件会失败。 */
    remove(dst);
    if (rename(tmp, dst) != 0) {
        remove(tmp);
        if (err && errsz) snprintf(err, errsz, "%s", "改名失败");
        return false;
    }
    ok = true;
    return ok;
}
