/*
 * sha256.h -- 自实现的 SHA-256
 *
 * 为什么自己写：libctru 其实链了 mbedtls，但**不暴露它的头文件**，
 * 所以应用拿不到 mbedtls_sha256_*。装 3ds-mbedtls 包是另一条路，
 * 但为了一个函数引入一整个依赖不划算 —— 而且这是 OTA 的 G4 护栏
 * （"下载完必须算摘要再落盘"），实现必须是自己能把控的。
 *
 * 只提供一次性算完整缓冲的接口。OTA 的产物是几百 KB，
 * 一次读进来算掉就够了，不需要流式更新。
 */

#pragma once

#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint32_t h[8];
    uint64_t total;      /* 已处理的字节数 */
    size_t   buflen;
    uint8_t  buf[64];
} Sha256Ctx;

void sha256_init(Sha256Ctx *c);
void sha256_update(Sha256Ctx *c, const void *data, size_t len);
void sha256_final(Sha256Ctx *c, uint8_t out[32]);

/* 把 32 字节摘要写成 64 个十六进制字符（out 至少 65 字节，含结尾 0）。 */
void sha256_hex(const uint8_t digest[32], char out[65]);

/* 一步算完文件。失败（打不开/读不动）返回 false。
 * 内部按 64KB 分块，不会把整个文件一次性塞进内存。 */
bool sha256_file(const char *path, char out_hex[65]);
