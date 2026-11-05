/*
 * png.c -- 把 BGR8 帧缓冲压成 PNG，再 base64
 *
 * 为什么自己写而不引库：
 *   插件里只为了"发一张截图"引进一个 PNG 编码器不划算；而 zlib 本来就在
 *   （devkitPro portlibs 里有 libz），PNG 真正需要的只有
 *   IHDR / IDAT / IEND 三个块 + zlib 压缩。一百多行就够。
 *
 * ⚠️ 输入是 3DS 的帧缓冲顺序 B,G,R；PNG 要 R,G,B。这两者写反了，
 *    发出去的图会是蓝脸 —— 而且看起来像"模型识别错了"，很难查。
 */

#include "png.h"

#include <stdlib.h>
#include <string.h>
#include <zlib.h>

static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

/* 一个 PNG 块：长度 + 类型 + 数据 + CRC(类型+数据) */
static size_t chunk(uint8_t *out, const char *type, const uint8_t *data, size_t n)
{
    put32(out, (uint32_t)n);
    memcpy(out + 4, type, 4);
    if (n)
        memcpy(out + 8, data, n);
    {
        uLong crc = crc32(0L, Z_NULL, 0);

        crc = crc32(crc, out + 4, (uInt)(n + 4));
        put32(out + 8 + n, (uint32_t)crc);
    }
    return n + 12;
}

uint8_t *png_encode(const uint8_t *bgr, int w, int h, size_t *out_size)
{
    size_t   raw_n = (size_t)w * h * 3 + (size_t)h;   /* 每行前面一个 filter 字节 */
    uint8_t *raw;
    uLong    comp_n;
    uint8_t *comp;
    uint8_t *png;
    size_t   pos = 0;
    uint8_t  ihdr[13];

    if (!bgr || w <= 0 || h <= 0 || !out_size)
        return NULL;

    raw = malloc(raw_n);
    if (!raw)
        return NULL;

    for (int y = 0; y < h; ++y) {
        uint8_t *row = raw + (size_t)y * (w * 3 + 1);

        row[0] = 0;                                   /* filter: None */
        for (int x = 0; x < w; ++x) {
            const uint8_t *s = bgr + ((size_t)y * w + x) * 3;

            row[1 + x * 3 + 0] = s[2];                /* R */
            row[1 + x * 3 + 1] = s[1];                /* G */
            row[1 + x * 3 + 2] = s[0];                /* B */
        }
    }

    comp_n = compressBound((uLong)raw_n);
    comp   = malloc(comp_n);
    if (!comp) {
        free(raw);
        return NULL;
    }
    if (compress2(comp, &comp_n, raw, (uLong)raw_n, 6) != Z_OK) {
        free(raw);
        free(comp);
        return NULL;
    }
    free(raw);

    png = malloc(8 + 25 + 12 + comp_n + 12);
    if (!png) {
        free(comp);
        return NULL;
    }

    /* 签名 */
    memcpy(png, "\x89PNG\r\n\x1a\n", 8);
    pos = 8;

    put32(ihdr + 0, (uint32_t)w);
    put32(ihdr + 4, (uint32_t)h);
    ihdr[8]  = 8;      /* bit depth */
    ihdr[9]  = 2;      /* color type: truecolor RGB */
    ihdr[10] = 0;      /* compression */
    ihdr[11] = 0;      /* filter */
    ihdr[12] = 0;      /* interlace */
    pos += chunk(png + pos, "IHDR", ihdr, sizeof ihdr);
    pos += chunk(png + pos, "IDAT", comp, comp_n);
    pos += chunk(png + pos, "IEND", NULL, 0);

    free(comp);
    *out_size = pos;
    return png;
}

char *base64(const uint8_t *in, size_t n)
{
    static const char *kTbl =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t   olen = ((n + 2) / 3) * 4;
    char    *out  = malloc(olen + 1);
    size_t   i, o = 0;

    if (!out)
        return NULL;

    for (i = 0; i + 2 < n; i += 3) {
        uint32_t v = (uint32_t)in[i] << 16 | (uint32_t)in[i + 1] << 8 | in[i + 2];

        out[o++] = kTbl[(v >> 18) & 63];
        out[o++] = kTbl[(v >> 12) & 63];
        out[o++] = kTbl[(v >> 6) & 63];
        out[o++] = kTbl[v & 63];
    }
    if (i < n) {
        uint32_t v = (uint32_t)in[i] << 16;

        if (i + 1 < n)
            v |= (uint32_t)in[i + 1] << 8;
        out[o++] = kTbl[(v >> 18) & 63];
        out[o++] = kTbl[(v >> 12) & 63];
        out[o++] = (i + 1 < n) ? kTbl[(v >> 6) & 63] : '=';
        out[o++] = '=';
    }
    out[o] = '\0';
    return out;
}
