/*
 * png.h -- 帧缓冲 → PNG → base64
 */

#pragma once

#include <stddef.h>
#include <stdint.h>

/* 输入 BGR8（3DS 帧缓冲顺序），输出 PNG 文件内容。调用方 free。 */
uint8_t *png_encode(const uint8_t *bgr, int w, int h, size_t *out_size);

/* 标准 base64（带 '=' 填充），调用方 free。 */
char *base64(const uint8_t *in, size_t n);
