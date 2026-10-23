/*
 * xfer.c -- 见 xfer.h
 *
 * ── 三件事决定了这个模块的形状 ──
 *
 * 1. **传输会阻塞**。curl 的接收回调是在一个循环里跑的，
 *    800 MB 在 2 MB/s 下是六分多钟。这期间**不能碰界面**——
 *    3DS 的主循环是单线程的，一卡就是黑屏。
 *    所以：进度只更新状态缓冲，由调用方在传输结束后读；
 *    真要边传边显示，需要另开线程（见 xfer.h 的说明）。
 *    现在先做对：宁可六分钟不动，也不要"看起来在动其实在冻屏"。
 *
 * 2. **落盘必须原子**。3DS 没有 fsync 保证，FAT 也没有。
 *    中途断电/拔卡 = 半截文件。半截的 .cia 装上去会砖。
 *    所以：先写 <name>.tmp，校验通过才 rename。
 *    这与项目里内容更新（cupd.c）的做法是同一套纪律。
 *
 * 3. **新旧 3DS 的差距是硬件的**。New 3DS 的 Wi-Fi 约 2.1 MB/s、
 *    旧款约 0.6 MB/s（社区多方实测），内存也差一档。
 *    所以缓冲与预期速度都按型号给，界面上要**明说"兼容模式"** ——
 *    让用户以为卡死了，比慢更糟。
 */

#include "xfer.h"

#include "httpx.h"
#include "util.h"

#include <3ds.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

/* 收一个 800 MB 的 CIA 慢到什么程度都还在传。
 * 超时的意义是"卡死了要退出"，不是"传完就退出"。 */
#define XFER_TIMEOUT_S   1800

void xfer_profile(XferProfile *out)
{
    bool new3ds = false;

    if (!out)
        return;
    memset(out, 0, sizeof *out);

    /* APT_CheckNew3DS 是 apt 的公开接口，返回失败就按旧机处理 ——
     * 宁可慢，不能因为一个没读到的型号就分配大缓冲。 */
    if (R_FAILED(APT_CheckNew3DS(&new3ds)) || !new3ds) {
        out->model      = "3DS（兼容模式）";
        out->is_new     = false;
        out->buf_size   = 64 * 1024;      /* 64 KB */
        out->speed_hint = 600;            /* KB/s */
    } else {
        out->model      = "New 3DS";
        out->is_new     = true;
        out->buf_size   = 256 * 1024;     /* 256 KB */
        out->speed_hint = 2000;           /* KB/s */
    }
}

/* ---------------------------------------------------------------- 落盘 */

typedef struct {
    FILE  *f;
    long   done;
    long   total;
    int    last_pct;
    int    oom;                /* 写盘失败：磁盘满或拔卡了 */
    XferProgress cb;
    void  *user;
} XferCtx;

static size_t on_chunk(const char *data, size_t len, void *user)
{
    XferCtx *c = (XferCtx *)user;
    size_t   w;

    if (c->oom)
        return 0;               /* 告诉 curl 停下，别继续收 */
    if (!c->f)
        return 0;

    w = fwrite(data, 1, len, c->f);
    c->done += (long)w;

    if (w == 0 && len > 0) {
        c->oom = 1;
        return 0;
    }

    /* 进度按 5% 跳着报。总线数比百分比有用 ——
     * 用户看"还剩 412 MB"比看"还剩 63%"更能判断要不要等。 */
    if (c->cb && c->total > 0) {
        int pct = (int)((c->done * 100) / c->total);
        if (pct - c->last_pct >= 5 || pct >= 100) {
            c->last_pct = pct;
            c->cb(pct, c->done, c->total, c->user);
        }
    }
    return w;
}

bool xfer_pull(const char *url, const char *sd_dir, const char *name,
               long expect_bytes, const char *sha256,
               XferProgress on_progress, void *user,
               char *err, size_t errcap)
{
    XferProfile prof;
    XferCtx     c;
    HxReq       req;
    char        tmp[288], final[256];
    long        code;
    int         n;

    if (err && errcap)
        err[0] = '\0';
    if (!url || !sd_dir || !name || !err || !errcap) {
        if (err) snprintf(err, errcap, "参数不齐");
        return false;
    }

    /* 只放行 http/https。
     *
     * 为什么要挡：`file://` 在 newlib 下能读本机文件，
     * 而这个模块将来可能被 AI 触发（"帮我把那个文件拉过来"）——
     * 那样就成了"任意文件读取"。现在它只由界面触发，
     * 但白名单要在**这一层**写死，不能指望调用方永远守规矩。 */
    if (strncmp(url, "http://", 7) != 0 && strncmp(url, "https://", 8) != 0) {
        snprintf(err, errcap, "只支持 http / https 地址");
        return false;
    }

    xfer_profile(&prof);

    n = snprintf(final, sizeof final, "%s/%s", sd_dir, name);
    if (n < 0 || n >= (int)sizeof final) {
        snprintf(err, errcap, "目标路径太长");
        return false;
    }
    /* 目标名里不许带路径分隔符：sd_dir 是白名单给的，
     * 而 name 会原样拼进 rename 的目标，夹带斜杠就写到别处去了。 */
    if (strchr(name, '/') || strchr(name, '\\') || strstr(name, "..")) {
        snprintf(err, errcap, "文件名不合法");
        return false;
    }
    n = snprintf(tmp, sizeof tmp, "%s.tmp", final);
    if (n < 0 || n >= (int)sizeof tmp) {
        snprintf(err, errcap, "临时路径太长");
        return false;
    }

    util_mkdirs(sd_dir);

    memset(&c, 0, sizeof c);
    c.total   = expect_bytes;
    c.last_pct = -1;
    c.cb      = on_progress;
    c.user    = user;

    c.f = fopen(tmp, "wb");
    if (!c.f) {
        snprintf(err, errcap, "建不了临时文件（SD 卡不可写？）");
        return false;
    }

    memset(&req, 0, sizeof req);
    req.url       = url;
    req.timeout_s = XFER_TIMEOUT_S;
    req.follow    = true;
    req.add_cookies = false;
    req.buf_size  = prof.buf_size;

    code = hx_perform(&req, on_chunk, &c, err, errcap);

    fclose(c.f);
    c.f = NULL;

    if (c.oom) {
        remove(tmp);
        snprintf(err, errcap, "写不进 SD 卡（满了或被拔了）");
        return false;
    }
    if (code != 200) {
        /* 先删掉半截文件：留在那里的话，下次重试会撞上一个"看起来存在"
         * 的坏文件，而用户根本不知道它是上次没传完的。 */
        remove(tmp);
        if (code < 0)
            return false;                 /* err 里已有网络原因 */
        snprintf(err, errcap, "服务器回了 %ld", code);
        return false;
    }
    if (expect_bytes > 0 && c.done != expect_bytes) {
        remove(tmp);
        snprintf(err, errcap, "大小不对：收到 %ld，应为 %ld", c.done, expect_bytes);
        return false;
    }

    /* rename 是原子的：这一步之后，要么是完整的新文件，要么是完整的旧文件。
     * 这是整个模块存在的原因 —— 没有它，一个断电就可能留下一个能砖机的 .cia。 */
    remove(final);
    if (rename(tmp, final) != 0) {
        remove(tmp);
        snprintf(err, errcap, "写好了但改名失败（SD 卡不可写？）");
        return false;
    }

    if (on_progress)
        on_progress(100, c.done, c.done, user);
    snprintf(err, errcap, "%s 已存好（%ld 字节）", name, c.done);
    return true;
}

/* ---------------------------------------------------------------- 分块 */

bool xfer_pull_chunked(const char *url, const char *sd_dir, const char *name,
                       long expect_bytes, long chunk_bytes,
                       XferProgress on_progress, void *user,
                       XferYield on_yield, void *yield_user,
                       char *err, size_t errcap)
{
    XferProfile prof;
    XferCtx     c;
    char        tmp[288], final[256], range[64];
    long        off = 0;
    int         n;

    if (err && errcap)
        err[0] = '\0';
    if (!url || !sd_dir || !name || !err || !errcap) {
        if (err) snprintf(err, errcap, "参数不齐");
        return false;
    }
    /* 分块必须有总大小 —— 不知道总共多少就没法切。
     * 这里**拒绝**而不是"先按 0 处理往下走"，那样会静默传成一个空文件。 */
    if (expect_bytes <= 0) {
        snprintf(err, errcap, "不知道文件多大，没法分块传");
        return false;
    }
    if (strncmp(url, "http://", 7) != 0 && strncmp(url, "https://", 8) != 0) {
        snprintf(err, errcap, "只支持 http / https 地址");
        return false;
    }
    if (strchr(name, '/') || strchr(name, '\\') || strstr(name, "..")) {
        snprintf(err, errcap, "文件名不合法");
        return false;
    }

    xfer_profile(&prof);
    if (chunk_bytes <= 0)
        chunk_bytes = prof.is_new ? (4 * 1024 * 1024) : (2 * 1024 * 1024);

    n = snprintf(final, sizeof final, "%s/%s", sd_dir, name);
    if (n < 0 || n >= (int)sizeof final) {
        snprintf(err, errcap, "目标路径太长");
        return false;
    }
    n = snprintf(tmp, sizeof tmp, "%s.tmp", final);
    if (n < 0 || n >= (int)sizeof tmp) {
        snprintf(err, errcap, "临时路径太长");
        return false;
    }
    util_mkdirs(sd_dir);

    memset(&c, 0, sizeof c);
    c.total    = expect_bytes;
    c.last_pct = -1;
    c.cb       = on_progress;
    c.user     = user;

    /* 一开始就用追加模式，且先把上次的残留清掉 ——
     * 半截的 .tmp 若不清，会被当成"已经传了 300 MB"接着往后写，
     * 结果是一个拼接出来的坏文件。 */
    c.f = fopen(tmp, "ab");
    if (!c.f) {
        snprintf(err, errcap, "建不了临时文件（SD 卡不可写？）");
        return false;
    }
    fclose(c.f);
    remove(tmp);
    c.f = fopen(tmp, "wb");
    if (!c.f) {
        snprintf(err, errcap, "建不了临时文件（SD 卡不可写？）");
        return false;
    }

    while (off < expect_bytes) {
        long end = off + chunk_bytes - 1;
        bool first = (off == 0);

        if (end > expect_bytes - 1)
            end = expect_bytes - 1;
        snprintf(range, sizeof range, "Range: bytes=%ld-%ld", off, end);

        {
            const char *hdrs[2];
            HxReq req;
            long  code;

            hdrs[0] = range;
            hdrs[1] = NULL;

            memset(&req, 0, sizeof req);
            req.url         = url;
            req.timeout_s   = XFER_TIMEOUT_S;
            req.follow      = true;
            req.add_cookies = false;
            req.buf_size    = prof.buf_size;
            req.headers     = hdrs;

            code = hx_perform(&req, on_chunk, &c, err, errcap);

            if (c.oom) {
                fclose(c.f); c.f = NULL;
                remove(tmp);
                snprintf(err, errcap, "写不进 SD 卡（满了或被拔了）");
                return false;
            }
            /* ★ 必须确认服务器真的按 Range 给了 206。
             *   不支持 Range 的服务器会回 **200 + 整个文件**，
             *   而我们是**追加**写的 —— 不检查的话，第二块就把整个文件
             *   又追加了一遍，攒出一个几倍大的坏文件，而且**不会报错**。
             *   这是"看着传完了、装上去才发现是坏的"最可能的一步。 */
            if ((!first && code == 200) || (first && code != 200 && code != 206)) {
                fclose(c.f); c.f = NULL;
                remove(tmp);
                snprintf(err, errcap,
                         "服务器不支持分段传（回了 %ld），整传会更慢但更稳", code);
                return false;
            }
            if (code != 200 && code != 206) {
                fclose(c.f); c.f = NULL;
                remove(tmp);
                snprintf(err, errcap, "服务器回了 %ld", code);
                return false;
            }
        }

        off = end + 1;
        /* 主动报一次 —— 页面靠它刷新进度条（两次回调之间的间隔正好是一块的时间） */
        if (on_progress)
            on_progress((int)((c.done * 100) / expect_bytes), c.done, expect_bytes, user);
        if (on_yield)
            on_yield(yield_user);
    }

    fclose(c.f);
    c.f = NULL;

    if (c.done != expect_bytes) {
        remove(tmp);
        snprintf(err, errcap, "收不齐：收到 %ld，应为 %ld", c.done, expect_bytes);
        return false;
    }
    remove(final);
    if (rename(tmp, final) != 0) {
        remove(tmp);
        snprintf(err, errcap, "写好了但改名失败（SD 卡不可写？）");
        return false;
    }
    if (on_progress)
        on_progress(100, c.done, c.done, user);
    snprintf(err, errcap, "%s 已存好（%ld 字节）", name, c.done);
    return true;
}
