/*
 * sha256.c -- 见 sha256.h
 *
 * 标准 FIPS 180-4 SHA-256。常量表是固定的，没有可发挥空间；
 * 唯一要注意的是 3DS 是**小端** ARM，而 SHA-256 常量与状态是大端语义，
 * 所以每一轮把消息字按大端读出来 —— 这里用逐字节组装，不依赖主机序。
 */

#include "sha256.h"

#include <stdio.h>
#include <string.h>

static const uint32_t K[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u,
    0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
    0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u,
    0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
    0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu,
    0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
    0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u,
    0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
    0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u,
    0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
    0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u,
    0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
    0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u,
    0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
    0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
    0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u,
};

#define ROR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))
#define CH(x, y, z)  (((x) & (y)) ^ (~(x) & (z)))
#define MAJ(x, y, z) (((x) & (y)) ^ ((x) & (z)) ^ ((y) & (z)))
#define BSIG0(x) (ROR(x, 2) ^ ROR(x, 13) ^ ROR(x, 22))
#define BSIG1(x) (ROR(x, 6) ^ ROR(x, 11) ^ ROR(x, 25))
#define SSIG0(x) (ROR(x, 7) ^ ROR(x, 18) ^ ((x) >> 3))
#define SSIG1(x) (ROR(x, 17) ^ ROR(x, 19) ^ ((x) >> 10))

void sha256_init(Sha256Ctx *c)
{
    c->h[0] = 0x6a09e667u; c->h[1] = 0xbb67ae85u;
    c->h[2] = 0x3c6ef372u; c->h[3] = 0xa54ff53au;
    c->h[4] = 0x510e527fu; c->h[5] = 0x9b05688cu;
    c->h[6] = 0x1f83d9abu; c->h[7] = 0x5be0cd19u;
    c->total = 0;
    c->buflen = 0;
}

/* 64 字节一块。block 按大端解出 w[0..15]。 */
static void sha256_block(Sha256Ctx *c, const uint8_t block[64])
{
    uint32_t w[64];
    uint32_t a, b, cc, d, e, f, g, h;

    for (int i = 0; i < 16; ++i) {
        const uint8_t *p = block + i * 4;
        w[i] = ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
               ((uint32_t)p[2] << 8)  | ((uint32_t)p[3]);
    }
    for (int i = 16; i < 64; ++i)
        w[i] = SSIG1(w[i - 2]) + w[i - 7] + SSIG0(w[i - 15]) + w[i - 16];

    a = c->h[0]; b = c->h[1]; cc = c->h[2]; d = c->h[3];
    e = c->h[4]; f = c->h[5]; g = c->h[6]; h = c->h[7];

    for (int i = 0; i < 64; ++i) {
        uint32_t t1 = h + BSIG1(e) + CH(e, f, g) + K[i] + w[i];
        uint32_t t2 = BSIG0(a) + MAJ(a, b, cc);
        h = g; g = f; f = e;
        e = d + t1;
        d = cc; cc = b; b = a;
        a = t1 + t2;
    }

    c->h[0] += a; c->h[1] += b; c->h[2] += cc; c->h[3] += d;
    c->h[4] += e; c->h[5] += f; c->h[6] += g; c->h[7] += h;
}

void sha256_update(Sha256Ctx *c, const void *data, size_t len)
{
    const uint8_t *p = (const uint8_t *)data;

    c->total += len;

    /* 先凑满缓冲区里剩下的部分 */
    if (c->buflen > 0) {
        size_t need = 64 - c->buflen;
        if (len < need) {
            memcpy(c->buf + c->buflen, p, len);
            c->buflen += len;
            return;
        }
        memcpy(c->buf + c->buflen, p, need);
        sha256_block(c, c->buf);
        c->buflen = 0;
        p += need;
        len -= need;
    }

    while (len >= 64) {
        sha256_block(c, p);
        p += 64;
        len -= 64;
    }

    if (len > 0) {
        memcpy(c->buf, p, len);
        c->buflen = len;
    }
}

void sha256_final(Sha256Ctx *c, uint8_t out[32])
{
    uint64_t bits = c->total * 8;
    uint8_t tail[8];

    /* 填充：0x80，补 0 到 56 字节（模 64），再放 8 字节大端长度 */
    uint8_t pad = 0x80;
    sha256_update(c, &pad, 1);
    pad = 0x00;
    while (c->buflen != 56)
        sha256_update(c, &pad, 1);

    for (int i = 0; i < 8; ++i)
        tail[i] = (uint8_t)(bits >> (56 - i * 8));
    sha256_update(c, tail, 8);

    for (int i = 0; i < 8; ++i) {
        out[i * 4 + 0] = (uint8_t)(c->h[i] >> 24);
        out[i * 4 + 1] = (uint8_t)(c->h[i] >> 16);
        out[i * 4 + 2] = (uint8_t)(c->h[i] >> 8);
        out[i * 4 + 3] = (uint8_t)(c->h[i]);
    }
}

void sha256_hex(const uint8_t digest[32], char out[65])
{
    static const char *hexd = "0123456789abcdef";
    for (int i = 0; i < 32; ++i) {
        out[i * 2]     = hexd[digest[i] >> 4];
        out[i * 2 + 1] = hexd[digest[i] & 0xF];
    }
    out[64] = '\0';
}

bool sha256_file(const char *path, char out_hex[65])
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return false;

    /* 64KB 分块：3DS 的栈很浅，这个缓冲放在静态区而不是栈上 */
    static uint8_t chunk[65536];
    Sha256Ctx c;
    size_t got;

    sha256_init(&c);
    while ((got = fread(chunk, 1, sizeof chunk, f)) > 0)
        sha256_update(&c, chunk, got);
    fclose(f);

    uint8_t digest[32];
    sha256_final(&c, digest);
    sha256_hex(digest, out_hex);
    return true;
}
