/*
 * qr.c -- 见 qr.h（内存约束与"为什么必须自校准"都写在那儿）
 *
 * 实现路子：base64 -> PNG chunk -> 流式 inflate -> 反滤波 -> 两遍扫描。
 */

#include "qr.h"

#include <zlib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define QR_PNG_MAX (1 << 18)     /* 原始 PNG 上限 256 KB（实测 2.6 KB） */

/* --------------------------------------------------------------------------
 * base64
 * ----------------------------------------------------------------------- */

static int b64_val(int c)
{
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+')             return 62;
    if (c == '/')             return 63;
    return -1;
}

/* 解码到 out。遇到 '=' 就停（PNG 的 base64 不会有内嵌 '='）。
 * 非字母表字符（换行、空白）跳过 —— 服务端偶尔会折行。 */
static size_t b64_decode(const char *s, unsigned char *out, size_t cap)
{
    unsigned acc  = 0;
    int      nbits = 0;
    size_t   o = 0;

    for (; *s; ++s) {
        int v;
        if (*s == '=')
            break;
        v = b64_val((unsigned char)*s);
        if (v < 0)
            continue;
        acc = (acc << 6) | (unsigned)v;
        nbits += 6;
        if (nbits >= 8) {
            nbits -= 8;
            if (o >= cap)
                return 0;
            out[o++] = (unsigned char)((acc >> nbits) & 0xFFu);
        }
    }
    return o;
}

/* --------------------------------------------------------------------------
 * PNG
 * ----------------------------------------------------------------------- */

typedef struct {
    unsigned char *idat;
    size_t         idat_len, idat_cap;
    unsigned       w, h;
    int            stride;      /* 每行像素字节数 */
    int            bpp;         /* 每像素字节数 */
    bool           bad;
} Png;

static unsigned be32(const unsigned char *p)
{
    return ((unsigned)p[0] << 24) | ((unsigned)p[1] << 16) |
           ((unsigned)p[2] << 8)  | (unsigned)p[3];
}

/* 只认 8bit 真彩（颜色类型 2）非隔行 —— 豆包实测就是这种。
 * 别的组合直接报"不支持"，而不是硬着头皮按错的方式解：
 * 位深/颜色类型认错会导致**整张图内容错位**，而现象只是"扫不出来"，
 * 极难定位。宁可明确拒绝。 */
static bool png_parse(const unsigned char *raw, size_t len, Png *p, QrState *why)
{
    static const unsigned char sig[8] = {0x89,'P','N','G','\r','\n',0x1A,'\n'};
    size_t pos = 8;
    bool   have_ihdr = false;

    memset(p, 0, sizeof *p);
    if (len < 8 || memcmp(raw, sig, 8) != 0) {
        *why = QR_ERR_PNG;
        return false;
    }

    while (pos + 12 <= len) {
        unsigned    clen = be32(raw + pos);
        const unsigned char *typ = raw + pos + 4;
        const unsigned char *data = raw + pos + 8;

        if (clen > len - pos - 12) {
            *why = QR_ERR_PNG;          /* 长度越界：文件被截断或结构错 */
            return false;
        }

        if (!memcmp(typ, "IHDR", 4)) {
            if (clen < 13) { *why = QR_ERR_PNG; return false; }
            p->w      = be32(data);
            p->h      = be32(data + 4);
            {
                int bd = data[8], ct = data[9], inter = data[12];
                if (bd != 8 || ct != 2 || inter != 0) {
                    *why = QR_ERR_FORMAT;
                    return false;
                }
                p->bpp    = 3;
                p->stride = (int)p->w * 3;
            }
            have_ihdr = true;
            if (p->w == 0 || p->h == 0 || p->stride > (1 << 14)) {
                *why = QR_ERR_FORMAT;
                return false;
            }
        } else if (!memcmp(typ, "IDAT", 4)) {
            if (!have_ihdr) { *why = QR_ERR_PNG; return false; }
            if (p->idat_len + clen > p->idat_cap) {
                size_t want = p->idat_cap ? p->idat_cap : 4096;
                unsigned char *np;
                while (want < p->idat_len + clen)
                    want *= 2;
                np = (unsigned char *)realloc(p->idat, want);
                if (!np) { *why = QR_ERR_PNG; return false; }
                p->idat = np;
                p->idat_cap = want;
            }
            memcpy(p->idat + p->idat_len, data, clen);
            p->idat_len += clen;
        } else if (!memcmp(typ, "IEND", 4)) {
            break;
        }

        pos += 12 + clen;               /* 长度4 + 类型4 + 数据 + CRC4 */
    }

    if (!have_ihdr || !p->idat_len) {
        *why = QR_ERR_PNG;
        return false;
    }
    return true;
}

/* --------------------------------------------------------------------------
 * 按行走完整个解压结果
 *
 * inflate 的输出边界**不按行对齐**，所以要自己按 (1 个滤波字节 + stride) 切。
 * 每切满一行就反滤波，然后回调 —— 调用方因此在**同一遍里**能看到像素，
 * 而不必把 787 KB 全留在内存里。
 * ----------------------------------------------------------------------- */

typedef void (*RowFn)(const unsigned char *row, unsigned y, void *user);

static bool walk_rows(const Png *p, RowFn fn, void *user)
{
    z_stream       zs;
    unsigned char *line = NULL;         /* 一行：滤波字节 + 像素 */
    unsigned char *prev = NULL;         /* 上一行反滤波后的像素 */
    unsigned char *inbuf  = NULL;       /* inflate 的输入（分片喂 IDAT） */
    unsigned char *outbuf = NULL;       /* inflate 的输出（**不能与输入同一块**）*/
    const size_t   need = (size_t)p->stride + 1;
    size_t         got = 0;
    size_t         fed = 0;
    unsigned       y = 0;
    bool           ok = false;
    int            rc;
    int            produced;

    memset(&zs, 0, sizeof zs);
    if (inflateInit(&zs) != Z_OK)
        return false;

    line   = (unsigned char *)malloc(need);
    prev   = (unsigned char *)calloc(1, (size_t)p->stride);
    inbuf  = (unsigned char *)malloc(4096);
    outbuf = (unsigned char *)malloc(4096);
    if (!line || !prev || !inbuf || !outbuf)
        goto done;

    for (;;) {
        size_t i;

        if (zs.avail_in == 0 && fed < p->idat_len) {
            size_t take = p->idat_len - fed;
            if (take > 4096)
                take = 4096;
            memcpy(inbuf, p->idat + fed, take);
            fed += take;
            zs.next_in  = inbuf;
            zs.avail_in = (uInt)take;
        }

        /* ⚠️ 输入输出**必须是两块不同的内存**。用同一块的话，
         * inflate 一边读还没消费的输入、一边往同一个地址写输出，
         * 结果是解出来一堆噪声 —— 而报错只会是"扫不出来"，方向全错。 */
        zs.next_out  = outbuf;
        zs.avail_out = 4096u;

        rc = inflate(&zs, Z_NO_FLUSH);
        produced = (int)(4096u - zs.avail_out);

        for (i = 0; i < (size_t)produced; ) {
            size_t take = need - got;
            if (take > (size_t)produced - i)
                take = (size_t)produced - i;
            memcpy(line + got, outbuf + i, take);
            got += take;
            i   += take;

            if (got == need) {
                unsigned char ft = line[0];
                unsigned char *cur = line + 1;
                unsigned x;

                /* ---- 反滤波 ----
                 * 五种都要支持，不是只有 PNG 常用的那几种：
                 * 实测这张图用了 1(Sub)/2(Up)/4(Paeth)，但**换一张图就可能是别的**，
                 * 只实现一部分会让"某些二维码能解、某些解出来是噪声"——
                 * 那种 bug 只在特定图上出现，最容易被当成偶发故障放过。 */
                for (x = 0; x < (unsigned)p->stride; ++x) {
                    int a = (x >= (unsigned)p->bpp) ? cur[x - p->bpp] : 0;
                    int b = prev[x];
                    int c = (x >= (unsigned)p->bpp) ? prev[x - p->bpp] : 0;

                    switch (ft) {
                        case 0: break;
                        case 1: cur[x] = (unsigned char)(cur[x] + a); break;
                        case 2: cur[x] = (unsigned char)(cur[x] + b); break;
                        case 3: cur[x] = (unsigned char)(cur[x] + ((a + b) >> 1)); break;
                        case 4: {
                            int pp = a + b - c;
                            int pa = pp > a ? pp - a : a - pp;
                            int pb = pp > b ? pp - b : b - pp;
                            int pc = pp > c ? pp - c : c - pp;
                            int pr = (pa <= pb && pa <= pc) ? a : (pb <= pc ? b : c);
                            cur[x] = (unsigned char)(cur[x] + pr);
                            break;
                        }
                        default:
                            goto done;  /* 非法滤波类型：数据坏了 */
                    }
                }

                fn(cur, y, user);
                memmove(prev, cur, (size_t)p->stride);
                got = 0;
                ++y;
            }
        }

        if (rc == Z_STREAM_END) {
            ok = true;
            break;
        }
        if (rc != Z_OK && rc != Z_BUF_ERROR)
            goto done;
        if (rc == Z_BUF_ERROR && zs.avail_in == 0 && fed >= p->idat_len)
            goto done;                  /* 没输入了又没进展 = 数据不完整 */
    }

done:
    inflateEnd(&zs);
    free(line);
    free(prev);
    free(inbuf);
    free(outbuf);
    return ok;
}

/* --------------------------------------------------------------------------
 * 第一遍：量几何
 *
 * 要三个量：黑区包围盒（定原点与边长）、以及**左上定位符顶边的长度**
 * （它恰好是 7 个黑模块，用来除出模块尺寸）。
 * ----------------------------------------------------------------------- */

typedef struct {
    long     w, bpp;
    long     minx, maxx, miny, maxy;
    long     first_run;         /* 第一段黑游程像素数；-1 = 还没见过黑 */
} Geom;

static void geom_row(const unsigned char *row, unsigned y, void *user)
{
    Geom *g = (Geom *)user;
    long  x;

    for (x = 0; x < g->w; ++x) {
        /* 只看 R 通道：图是黑白的，三个通道相同 */
        if (row[x * g->bpp] < 128) {
            if (x < g->minx)      g->minx = x;
            if (x > g->maxx)      g->maxx = x;
            if ((long)y < g->miny) g->miny = (long)y;
            if ((long)y > g->maxy) g->maxy = (long)y;
        }
    }

    /* 第一段黑游程只在**第一次**碰到含黑的行时取 —— 那正是定位符的顶边。
     * 后面再遇到的黑游程长短不一（数据区就是随机的），不能用。 */
    if (g->first_run < 0) {
        for (x = 0; x < g->w && row[x * g->bpp] >= 128; ++x)
            ;
        if (x < g->w) {
            long run = 0;
            while (x < g->w && row[x * g->bpp] < 128) {
                ++run;
                ++x;
            }
            g->first_run = run;
        }
    }
}

/* --------------------------------------------------------------------------
 * 第二遍：按模块中心采样
 * ----------------------------------------------------------------------- */

typedef struct {
    const Geom *g;
    QrCode     *q;
    long        bw;             /* 黑区边长（像素）*/
} Sample;

static void sample_row(const unsigned char *row, unsigned y, void *user)
{
    Sample *s = (Sample *)user;
    const Geom *g = s->g;
    const int   n = s->q->n;
    int         j;

    /* 找出"这一行的 y 恰是哪个模块行的中心"。最多命中一个。
     * **全程整数运算**：`miny + ((2j+1)*bw)/(2n)`。
     * 用浮点会引入平台相关的舍入差异，而采样点偏半个像素就可能跨到隔壁模块 ——
     * 那种错在图上只会表现为"个别格子反了"，扫不出来但看不出哪里错。 */
    for (j = 0; j < n; ++j) {
        long cy = g->miny + ((2L * j + 1) * s->bw) / (2L * n);
        if (cy != (long)y)
            continue;
        for (int i = 0; i < n; ++i) {
            long cx = g->minx + ((2L * i + 1) * s->bw) / (2L * n);
            if (cx < 0 || cx >= g->w)
                continue;
            s->q->mod[j][i] = (row[cx * g->bpp] < 128) ? 1 : 0;
        }
    }
}

/* --------------------------------------------------------------------------
 * 公开接口
 * ----------------------------------------------------------------------- */

const char *qr_state_text(QrState s)
{
    switch (s) {
        case QR_OK:             return "解码成功";
        case QR_ERR_B64:        return "二维码数据不是合法的 base64";
        case QR_ERR_PNG:        return "不是 PNG，或文件结构不认识";
        case QR_ERR_FORMAT:     return "PNG 格式不支持（只做 8 位真彩非隔行）";
        case QR_ERR_INFLATE:    return "PNG 解压失败（数据不完整？）";
        case QR_ERR_BIG:        return "二维码模块数超过上限";
        case QR_ERR_NO_PATTERN: return "找不到二维码定位符（几何算法失效）";
        default:                return "未知情况";
    }
}

QrState qr_decode_data_uri(const char *uri, QrCode *out, char *msg, size_t cap)
{
    unsigned char *png = NULL;
    const char    *b64;
    size_t         pnglen;
    Png            p;
    Geom           g;
    QrState        why = QR_ERR_PNG;
    QrState        ret;
    long           bw;
    int            n;

    if (msg && cap) msg[0] = '\0';
    if (!uri || !out) {
        snprintf(msg, cap, "没有内容");
        return QR_ERR_B64;
    }
    memset(out, 0, sizeof *out);

    /* 跳过 `data:image/png;base64,`；也容忍裸 base64 */
    b64 = strstr(uri, "base64,");
    b64 = b64 ? b64 + 7 : uri;

    png = (unsigned char *)malloc(QR_PNG_MAX);
    if (!png) {
        snprintf(msg, cap, "内存不够");
        return QR_ERR_PNG;
    }
    pnglen = b64_decode(b64, png, QR_PNG_MAX);
    if (pnglen < 16) {
        snprintf(msg, cap, "base64 解出来只有 %u 字节，太短",
                 (unsigned)pnglen);
        free(png);
        return QR_ERR_B64;
    }

    if (!png_parse(png, pnglen, &p, &why)) {
        snprintf(msg, cap, "%s（PNG %u 字节）", qr_state_text(why),
                 (unsigned)pnglen);
        free(p.idat);               /* 解析中途失败时可能已经收了一部分 IDAT */
        free(png);
        return why;
    }

    memset(&g, 0, sizeof g);
    g.w = (long)p.w;
    g.bpp = p.bpp;
    g.minx = 1L << 30; g.maxx = -1;
    g.miny = 1L << 30; g.maxy = -1;
    g.first_run = -1;

    if (!walk_rows(&p, geom_row, &g)) {
        snprintf(msg, cap, "%s", qr_state_text(QR_ERR_INFLATE));
        free(p.idat);
        free(png);
        return QR_ERR_INFLATE;
    }

    bw = g.maxx - g.minx + 1;
    if (g.maxx < 0 || bw <= 0) {
        snprintf(msg, cap, "整张图没有黑像素 —— 这是空白图，不是二维码");
        free(p.idat);
        free(png);
        return QR_ERR_NO_PATTERN;
    }

    /* 模块尺寸 = 定位符顶边 ÷ 7；模块数 = 黑区边长 ÷ 模块尺寸。
     * 用"乘回来再除"避免先算出浮点的模块尺寸：
     *     N = round(bw * 7 / first_run) */
    if (g.first_run < 7) {
        snprintf(msg, cap,
                 "左上定位符的顶边只有 %ld 像素（不足 7 个模块）—— "
                 "找不到二维码结构", g.first_run);
        free(p.idat);
        free(png);
        return QR_ERR_NO_PATTERN;
    }
    n = (int)((bw * 7 + g.first_run / 2) / g.first_run);

    /* QR 的模块数只能是 21 + 4k（version 1..40）。不符就是几何算错了，
     * 与其按错的值采样出一张"看着像二维码"的错图，不如当场拒绝。 */
    if (n < 21 || n > QR_MAX_MODULES || ((n - 21) % 4) != 0) {
        if (n > QR_MAX_MODULES) {
            snprintf(msg, cap, "模块数算出来 %d，超过上限 %d",
                     n, QR_MAX_MODULES);
            free(p.idat);
            free(png);
            return QR_ERR_BIG;
        }
        snprintf(msg, cap,
                 "模块数算出来 %d，不是合法值（应为 21+4k）—— "
                 "几何算法对这张图失效了", n);
        free(p.idat);
        free(png);
        return QR_ERR_NO_PATTERN;
    }

    out->n = n;
    {
        Sample s;
        s.g = &g;
        s.q = out;
        s.bw = bw;
        if (!walk_rows(&p, sample_row, &s)) {
            snprintf(msg, cap, "%s", qr_state_text(QR_ERR_INFLATE));
            free(p.idat);
            free(png);
            return QR_ERR_INFLATE;
        }
    }

    free(p.idat);
    free(png);

    ret = QR_OK;
    /* 不用 %f：那会把 newlib 的 dtoa 拉进来，单帧 2~3 KB 栈，
     * 而这个 app 的栈预算很紧（见 main.c 关于 16KB 主线程栈的说明）。
     * 模块尺寸用整数拼出两位小数就够了。 */
    snprintf(msg, cap, "%d×%d 模块（黑区 %ld px，每模块 %ld.%02ld px）",
             n, n, bw, bw / n, (bw % n) * 100 / n);
    return ret;
}

/* 三个定位符的期望花纹（7×7）：黑边 + 白圈 + 中间 3×3 黑。 */
static const unsigned char FINDER[7][7] = {
    {1,1,1,1,1,1,1},
    {1,0,0,0,0,0,1},
    {1,0,1,1,1,0,1},
    {1,0,1,1,1,0,1},
    {1,0,1,1,1,0,1},
    {1,0,0,0,0,0,1},
    {1,1,1,1,1,1,1},
};

int qr_finder_mismatch(const QrCode *q)
{
    int bad = 0;
    int n;

    if (!q || q->n < 7)
        return 7 * 7 * 3;
    n = q->n;

    /* 左上 (0,0)、右上 (n-7,0)、左下 (0,n-7) */
    for (int j = 0; j < 7; ++j)
        for (int i = 0; i < 7; ++i) {
            if (q->mod[j][i]           != FINDER[j][i]) ++bad;
            if (q->mod[j][n - 7 + i]   != FINDER[j][i]) ++bad;
            if (q->mod[n - 7 + j][i]   != FINDER[j][i]) ++bad;
        }
    return bad;
}
