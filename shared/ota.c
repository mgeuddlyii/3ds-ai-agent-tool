/*
 * ota.c -- 见 ota.h
 *
 * manifest 解析复用 minijson。数组元素的路径形如 "artifacts[0].sha256"，
 * 所以这里按前缀 + 下标来分派，而不是逐个 strcmp 全名。
 */

#include "ota.h"

#include "minijson.h"
#include "sha256.h"
#include "httpx.h"

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

/* ★★ 2026-10-24：bucket 的**规范域名**，第二个更新源（也是今天唯一能过
 *   证书校验的那个）。update.acrac.cn 是它的自定义域名，但 bucket 没绑 HTTPS
 *   证书，服务器返回的是 oss-cn-hongkong.aliyuncs.com 的通用证书 —— 主机名
 *   对不上，校验必失败（curl -60）。改 DNS 救不了，证书名不对就是不对。
 *   等 bucket 给自定义域名绑好证书，第一个源就会自然生效，这里不用动。 */
#ifndef OTA_OSS_URL
#define OTA_OSS_URL "https://acrachongkong.oss-cn-hongkong.aliyuncs.com/aiconsole"
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

/* --------------------------------------------------------------------------
 * 更新源：**一串**候选地址，按顺序试
 *
 * ★★ 2026-10-24 修：这里原来用 libctru 的 httpc，而 httpc/sslc 只支持到 TLS 1.1，
 *    阿里云 OSS 现在只收 TLS 1.2 —— 表现就是"网络远程更新永远连不上"。
 *    项目里聊天（chatapi）和语音（asr）早就改用 curl + mbedtls 了，OTA 这块是漏网的。
 *
 *    换过来还顺带补上一个安全缺陷：httpc 时代 3DS 没有根证书库，只能
 *    httpcSetSSLOpt(ctx, SSLCOPT_DisableVerify) 把校验关掉（那时注释里写着
 *    "防不了中间人，只能靠产物 sha256 兜底"）。curl 这条路我们有内嵌 CA，
 *    **可以真的验主机名和证书链**了 —— 这一点比以前强，不是弱。
 *
 * 为什么是多个源而不是一个：
 *    见 OTA_OSS_URL 的注释：自定义域名的证书名对不上，规范域名才对得上。
 *    第一个源是产品域名（也是 -DOTA_BASE_URL 的覆盖点，验收指向本机测试服务
 *    时靠它），第二个是 bucket 规范域名。真实网络下第一个会先失败一次
 *    （约 0.3~1s），然后落到第二个；s_base_hint 记住上次成功的那个，
 *    同一进程内再检查就不再白试。
 * ----------------------------------------------------------------------- */
static const char *const kUpdateBases[] = {
    OTA_BASE_URL,                 /* 产品域名（-D 可覆盖） */
    OTA_OSS_URL,                  /* bucket 规范域名：证书名对得上 */
};
#define N_UPDATE_BASES ((int)(sizeof kUpdateBases / sizeof kUpdateBases[0]))

static int s_base_hint;

/* 给验收探针用的只读访问器（见 app/source/test/otaprobe.c）。
 * 为什么要把源列表暴露出来：探针要能**逐个**报出每个源的握手结果，
 * 否则"换源生效"这件事只能靠猜 —— 而这次要修的恰恰是"某个源连不上"。 */
int ota_base_count(void) { return N_UPDATE_BASES; }

const char *ota_base_url(int i)
{
    if (i < 0 || i >= N_UPDATE_BASES)
        return NULL;
    return kUpdateBases[i];
}

/* 上一次成功的源（还没成功过就是第一个）。 */
const char *ota_last_base(void)
{
    if (s_base_hint < 0 || s_base_hint >= N_UPDATE_BASES)
        return kUpdateBases[0];
    return kUpdateBases[s_base_hint];
}

/* 取一段资源。返回 HTTP 状态码；负值 = 连网络层都没过（DNS/TCP/TLS）。
 *
 * 只在**网络层失败**时才换下一个源：服务器回了 404 说明"地址对、文件没有"，
 * 换源重试只会把"产物压根没上传"误报成"网络不好"，那是两个完全不同的处置。 */
static long http_get_any(const char *path, HxSink sink, void *user,
                         char *err, size_t errsz)
{
    long code = -1;

    for (int n = 0; n < N_UPDATE_BASES; ++n) {
        int   i = (s_base_hint + n) % N_UPDATE_BASES;
        char  url[192];
        char  e[96];
        HxReq req;

        snprintf(url, sizeof url, "%s/%s", kUpdateBases[i], path);
        memset(&req, 0, sizeof req);
        req.url         = url;
        /* 30s：香港 OSS 这条链路实测会慢到 15s 以上；
         * 检查更新是人手点的动作，宁可多等几秒也不要误报"连不上"。 */
        req.timeout_s   = 30;
        req.follow      = true;     /* OSS 可能 302 到加速/内网域名 */
        req.add_cookies = false;
        req.persist     = true;     /* 复用连接：检查更新常有连着点两次的情况 */
        e[0] = '\0';

        code = hx_perform(&req, sink, user, e, sizeof e);
        if (code >= 0) {            /* 服务器答话了，就用这个源 */
            s_base_hint = i;
            if (err && errsz) err[0] = '\0';
            return code;
        }
        if (err && errsz)
            snprintf(err, errsz, "%s", e);
    }
    return code;                    /* 所有源都在网络层失败 */
}

/* manifest 收进固定缓冲。超了就判失败 —— manifest 正常只有几百字节，
 * 超过 4KB 说明这个 URL 返的不是 manifest（常见于托管站把 404 渲染成 HTML 页）。 */
typedef struct {
    char  *buf;
    size_t cap;
    size_t n;
    bool   over;
} MemSink;

static size_t mem_sink(const char *data, size_t len, void *user)
{
    MemSink *s = user;

    if (s->n + len >= s->cap) {
        s->over = true;
        return 0;                   /* 返回 0 让 curl 中止：再收也没地方放 */
    }
    memcpy(s->buf + s->n, data, len);
    s->n += len;
    s->buf[s->n] = '\0';
    return len;
}

/* 产物落盘 + 边下边算 sha256。
 * ⚠️ 写失败必须返回 0 让 curl 中止：否则会把"磁盘满了"当成"下载成功"，
 *    然后摘要不符，报出来的原因反而是"线上文件被换过"。 */
typedef struct {
    FILE      *f;
    Sha256Ctx *sh;
    size_t     n;
    bool       io_err;
} FileSink;

static size_t file_sink(const char *data, size_t len, void *user)
{
    FileSink *s = user;

    if (s->io_err || !s->f)
        return 0;
    if (fwrite(data, 1, len, s->f) != len) {
        s->io_err = true;
        return 0;
    }
    sha256_update(s->sh, (const u8 *)data, len);
    s->n += len;
    return len;
}

bool ota_fetch_remote(const char *channel, OtaManifest *m, OtaState *state,
                      char *detail, size_t cap)
{
    static char body[4096];    char   path[96];
    MemSink sk;
    long   code;
    bool   ok = false;

    if (state)  *state = OTA_ERR_NET;
    if (detail && cap) detail[0] = '\0';

    if (!m)
        return false;
    if (!channel_ok(channel)) {
        if (detail && cap)
            snprintf(detail, cap, "%s", "channel 名不合法");
        return false;
    }

    snprintf(path, sizeof path, "%s/manifest.json", channel);

    sk.buf  = body;
    sk.cap  = sizeof body;
    sk.n    = 0;
    sk.over = false;

    code = http_get_any(path, mem_sink, &sk, detail, cap);
    if (code < 0)
        return false;               /* detail 里已经是 curl 的原话 */

    if (code != 200) {
        if (detail && cap)
            snprintf(detail, cap, "HTTP %ld", code);
        return false;
    }
    if (sk.over) {
        if (detail && cap)
            snprintf(detail, cap, "manifest 过大（>%d 字节）", (int)sizeof body);
        return false;
    }

    /* 留一份原文：下载完要原样落盘（见 ota_remote_raw 的说明） */
    memcpy(s_remote_raw, body, sk.n);
    s_remote_raw[sk.n] = '\0';
    s_remote_raw_n = sk.n;

    if (!ota_parse(body, sk.n, m)) {
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
    char   url[192];
    char   tmp[192];
    char   dst[192];
    char   hex[65];
    FILE  *f = NULL;
    Sha256Ctx sh;
    uint8_t dg[32];
    size_t written = 0;
    bool   ok = false;
    bool   got200 = false;

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

    snprintf(tmp, sizeof tmp, "%s/%s.partial", OTA_DIR, m->art[idx].file);
    snprintf(dst, sizeof dst, "%s/%s", OTA_DIR, m->art[idx].file);

    for (int n = 0; n < N_UPDATE_BASES; ++n) {
        int      i = (s_base_hint + n) % N_UPDATE_BASES;
        HxReq    req;
        FileSink fs;
        char     e[96];
        long     code;

        snprintf(url, sizeof url, "%s/%s/%s", kUpdateBases[i], channel,
                 m->art[idx].file);

        /* 每次重开 = 截断：上一个源可能已经写了半截。 */
        f = fopen(tmp, "wb");
        if (!f) {
            if (err && errsz) snprintf(err, errsz, "%s", "建不了临时文件");
            return false;
        }
        sha256_init(&sh);
        fs.f = f;
        fs.sh = &sh;
        fs.n = 0;
        fs.io_err = false;

        memset(&req, 0, sizeof req);
        req.url         = url;
        req.timeout_s   = 180;      /* 几 MB 的包，别用默认 30s */
        req.follow      = true;
        req.add_cookies = false;
        req.persist     = false;    /* 大文件一次性，不占连接池 */
        e[0] = '\0';

        code = hx_perform(&req, file_sink, &fs, e, sizeof e);
        fclose(f);
        f = NULL;
        written = fs.n;

        if (code == 200 && !fs.io_err) {
            got200 = true;
            s_base_hint = i;
            break;
        }
        if (code >= 0) {
            /* 服务器答话了但不是 200：这是"文件/权限"问题，换源无意义。 */
            if (err && errsz) snprintf(err, errsz, "HTTP %ld", code);
            remove(tmp);
            return false;
        }
        if (n == N_UPDATE_BASES - 1) {
            if (err && errsz) snprintf(err, errsz, "%s", e);
            remove(tmp);
            return false;
        }
    }
    fclose(f);

    if (!got200 || written != (size_t)m->art[idx].size) {
        /* 早先这里比对的是 content-length，现在 curl 不给我们单独的长度，
         * 就用 manifest 声明的 size —— 比"下完再说摘要不符"有用得多，
         * 它直接指向"线上文件与 manifest 不一致"。 */
        remove(tmp);
        if (err && errsz)
            snprintf(err, errsz, "大小不符（实收 %lu，声明 %d）",
                     (unsigned long)written, m->art[idx].size);
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
