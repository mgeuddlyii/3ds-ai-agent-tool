/*
 * shot.c -- 24 位 BMP 写出
 *
 * BMP 而不是 PNG：BMP 是无压缩、格式固定、几十行就能写对的东西，
 * 不需要在 3DS 上塞一个压缩器。文件大一点（顶屏 400x240x3 = 288 KB）
 * 完全无所谓，反正只是开发期核对用。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "render.h"
#include "shot.h"

static void wr16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
}

static void wr32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
    p[2] = (uint8_t)((v >> 16) & 0xFFu);
    p[3] = (uint8_t)((v >> 24) & 0xFFu);
}

int shot_mkdirs(const char *path)
{
    char tmp[256];
    size_t n = strlen(path);
    if (n == 0 || n >= sizeof(tmp))
        return -1;
    memcpy(tmp, path, n + 1);

    /* 跳过 "sdmc:/" 这类前缀里的第一个 '/' */
    size_t start = 0;
    char *colon = strchr(tmp, ':');
    if (colon) start = (size_t)(colon - tmp) + 1;

    for (size_t i = start + 1; i < n; ++i) {
        if (tmp[i] == '/') {
            tmp[i] = '\0';
            mkdir(tmp, 0777);          /* 已存在也无所谓 */
            tmp[i] = '/';
        }
    }
    mkdir(tmp, 0777);
    return 0;
}

int shot_save_bmp(const char *path)
{
    int w = 0, h = 0;
    r_screen_size(&w, &h);
    if (w <= 0 || h <= 0)
        return -1;

    const int row = (w * 3 + 3) & ~3;
    const uint32_t data_size = (uint32_t)row * (uint32_t)h;
    const uint32_t file_size = 54u + data_size;

    FILE *f = fopen(path, "wb");
    if (!f)
        return -2;

    uint8_t hdr[54];
    memset(hdr, 0, sizeof hdr);
    hdr[0] = 'B';
    hdr[1] = 'M';
    wr32(hdr + 2,  file_size);
    wr32(hdr + 10, 54u);            /* 像素数据偏移 */
    wr32(hdr + 14, 40u);            /* BITMAPINFOHEADER */
    wr32(hdr + 18, (uint32_t)w);
    wr32(hdr + 22, (uint32_t)h);
    wr16(hdr + 26, 1);              /* planes */
    wr16(hdr + 28, 24);             /* bpp */
    wr32(hdr + 34, data_size);
    wr32(hdr + 38, 2835);           /* 72 dpi */
    wr32(hdr + 42, 2835);

    if (fwrite(hdr, 1, sizeof hdr, f) != sizeof hdr) { fclose(f); return -3; }

    uint8_t *line = (uint8_t *)malloc((size_t)row);
    if (!line) { fclose(f); return -4; }

    /* BMP 是从下往上存的 */
    int rc = 0;
    for (int y = h - 1; y >= 0; --y) {
        for (int x = 0; x < w; ++x) {
            RColor c = r_get_pixel(x, y);
            line[x * 3 + 0] = (uint8_t)R_BLUE(c);
            line[x * 3 + 1] = (uint8_t)R_GREEN(c);
            line[x * 3 + 2] = (uint8_t)R_RED(c);
        }
        if (row > w * 3)
            memset(line + w * 3, 0, (size_t)(row - w * 3));
        if (fwrite(line, 1, (size_t)row, f) != (size_t)row) { rc = -5; break; }
    }

    free(line);
    fclose(f);
    return rc;
}

void shot_sanitize(char *s)
{
    for (; *s; ++s) {
        char c = *s;
        int ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                 (c >= '0' && c <= '9') || c == '_' || c == '-';
        if (!ok)
            *s = '_';
    }
}

int shot_save_both(const char *dir, const char *prefix)
{
    char path[256];
    RScreenId keep = r_current_target();
    int rc = 0;

    r_target(R_SCREEN_TOP);
    snprintf(path, sizeof path, "%s/%s_top.bmp", dir, prefix);
    if (shot_save_bmp(path) != 0) rc = -1;

    r_target(R_SCREEN_BOTTOM);
    snprintf(path, sizeof path, "%s/%s_bottom.bmp", dir, prefix);
    if (shot_save_bmp(path) != 0) rc = -2;

    r_target(keep);
    return rc;
}
