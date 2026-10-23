/*
 * crypto.c -- ChaCha20 + Poly1305 + SHA256 + KDF（见 crypto.h 的说明）
 *
 * 实现来源：RFC 8439。三个算法的测试向量都在 tools/crypto_selftest.py 里
 * 对照过（那份 Python 实现先跑通测试向量，再逐行翻译成这里的 C）。
 *
 * 只有 osGetLocalFriendCodeSeed 依赖 libctru，其余是纯 C —— 这样
 * 单元测试可以在 PC 上编译运行，不非得进模拟器。
 */

#include "crypto.h"

#include <string.h>

#include <3ds.h>
/* 设备因子走 cfgu（比 ps 轻），随机数走 ps。
 * 注意 cfgu.h / ps.h 不在 3ds.h 的默认伞里，要单独 include。 */
#include <3ds/services/cfgu.h>
#include <3ds/services/ps.h>

/* ================================================================== */
/* 工具：32 位循环移位与 little-endian 读写                            */
/* ================================================================== */

static inline uint32_t rotl32(uint32_t x, int n) {
    return (x << n) | (x >> (32 - n));
}

static inline uint32_t le32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8)
         | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static inline void put_le32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static inline uint32_t be32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16)
         | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static inline void put_be32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

/* ================================================================== */
/* ChaCha20 (RFC 8439 §2.3)                                          */
/* ================================================================== */

/*: 常量 "expand 32-byte k" 的四个 little-endian 字。 */
#define CHACHA_CONST0 0x61707865u
#define CHACHA_CONST1 0x3320646eu
#define CHACHA_CONST2 0x79622d32u
#define CHACHA_CONST3 0x6b206574u

/* 一次 quarter round，就地改 4 个状态字 */
#define QR(s, a, b, c, d)                    \
    do {                                     \
        s[a] += s[b]; s[d] ^= s[a]; s[d] = rotl32(s[d], 16); \
        s[c] += s[d]; s[b] ^= s[c]; s[b] = rotl32(s[b], 12); \
        s[a] += s[b]; s[d] ^= s[a]; s[d] = rotl32(s[d], 8);  \
        s[c] += s[d]; s[b] ^= s[c]; s[b] = rotl32(s[b], 7);  \
    } while (0)

/*
 * 生成一个 64 字节密钥流块。
 * counter 是块序号（RFC 8439 的加密用法从 1 开始）。
 */
static void chacha20_block(uint32_t out[16],
                           const uint8_t key[CHACHA20_KEY_LEN],
                           const uint8_t nonce[CHACHA20_NONCE_LEN],
                           uint32_t counter) {
    uint32_t st[16];
    uint32_t w[16];

    st[0] = CHACHA_CONST0;
    st[1] = CHACHA_CONST1;
    st[2] = CHACHA_CONST2;
    st[3] = CHACHA_CONST3;
    for (int i = 0; i < 8; i++) {
        st[4 + i] = le32(key + i * 4);
    }
    st[12] = counter;
    st[13] = le32(nonce + 0);
    st[14] = le32(nonce + 4);
    st[15] = le32(nonce + 8);

    memcpy(w, st, sizeof(w));

    /* 20 轮 = 10 次 double round */
    for (int i = 0; i < 10; i++) {
        QR(w, 0, 4,  8, 12);
        QR(w, 1, 5,  9, 13);
        QR(w, 2, 6, 10, 14);
        QR(w, 3, 7, 11, 15);
        QR(w, 0, 5, 10, 15);
        QR(w, 1, 6, 11, 12);
        QR(w, 2, 7,  8, 13);
        QR(w, 3, 4,  9, 14);
    }

    for (int i = 0; i < 16; i++) {
        out[i] = w[i] + st[i];
    }
}

void chacha20_xor(uint8_t *buf, size_t len,
                  const uint8_t key[CHACHA20_KEY_LEN],
                  const uint8_t nonce[CHACHA20_NONCE_LEN],
                  uint32_t counter) {
    uint32_t blk[16];
    uint8_t ks[64];
    size_t off = 0;

    while (off < len) {
        chacha20_block(blk, key, nonce, counter);
        for (int i = 0; i < 16; i++) {
            put_le32(ks + i * 4, blk[i]);
        }
        size_t n = len - off;
        if (n > 64) {
            n = 64;
        }
        for (size_t i = 0; i < n; i++) {
            buf[off + i] ^= ks[i];
        }
        off += n;
        counter++;
    }

    /* 不留下密钥流残迹 */
    memset(ks, 0, sizeof(ks));
    memset(blk, 0, sizeof(blk));
}

/* ================================================================== */
/* Poly1305 (RFC 8439 §2.5)                                          */
/* ================================================================== */

/*
 * Poly1305 在 GF(2^130 - 5) 上做大数乘法。这里用 5 个 26-bit limb 的
 * 经典实现（来自 RFC 里的参考代码），因为 32 位平台上不能直接用 64 位乘法
 * 处理 130 位模数。
 *
 * r 是密钥前 16 字节经过 clamp 的部分，s 是后 16 字节。
 */
void poly1305_mac(const uint8_t *msg, size_t len,
                  const uint8_t key[32], uint8_t out[POLY1305_TAG_LEN]) {
    uint32_t r0, r1, r2, r3, r4;
    uint32_t s1, s2, s3, s4;
    uint32_t h0 = 0, h1 = 0, h2 = 0, h3 = 0, h4 = 0;

    /* r = le_bytes_to_num(key[0..15]) 然后 clamp */
    r0 = (le32(key + 0))       & 0x3ffffffu;
    r1 = (le32(key + 3) >> 2)  & 0x3ffff03u;
    r2 = (le32(key + 6) >> 4)  & 0x3ffc0ffu;
    r3 = (le32(key + 9) >> 6)  & 0x3f03fffu;
    r4 = (le32(key + 12) >> 8) & 0x00fffffu;

    s1 = r1 * 5;
    s2 = r2 * 5;
    s3 = r3 * 5;
    s4 = r4 * 5;

    while (len > 0) {
        /* 取一块（最多 16 字节），高位补 0x01（这是 Poly1305 的定义） */
        unsigned char blk[16];
        size_t want = len < 16 ? len : 16;
        memset(blk, 0, sizeof(blk));
        memcpy(blk, msg, want);
        if (want < 16) {
            blk[want] = 1;
        }
        h0 += (le32(blk + 0))       & 0x3ffffffu;
        h1 += (le32(blk + 3) >> 2)  & 0x3ffffffu;
        h2 += (le32(blk + 6) >> 4)  & 0x3ffffffu;
        h3 += (le32(blk + 9) >> 6)  & 0x3ffffffu;
        h4 += (le32(blk + 12) >> 8);
        /* 满块要额外加 2^128（即 h4 的 bit24）；不满块时高位置 1 已经
         * 由 blk[want] = 1 写进上面的 le32 里了 */
        if (want == 16) {
            h4 += (1u << 24);
        }

        /* h *= r (mod 2^130 - 5) */
        uint64_t d0 = (uint64_t)h0 * r0 + (uint64_t)h1 * s4
                    + (uint64_t)h2 * s3 + (uint64_t)h3 * s2 + (uint64_t)h4 * s1;
        uint64_t d1 = (uint64_t)h0 * r1 + (uint64_t)h1 * r0
                    + (uint64_t)h2 * s4 + (uint64_t)h3 * s3 + (uint64_t)h4 * s2;
        uint64_t d2 = (uint64_t)h0 * r2 + (uint64_t)h1 * r1
                    + (uint64_t)h2 * r0 + (uint64_t)h3 * s4 + (uint64_t)h4 * s3;
        uint64_t d3 = (uint64_t)h0 * r3 + (uint64_t)h1 * r2
                    + (uint64_t)h2 * r1 + (uint64_t)h3 * r0 + (uint64_t)h4 * s4;
        uint64_t d4 = (uint64_t)h0 * r4 + (uint64_t)h1 * r3
                    + (uint64_t)h2 * r2 + (uint64_t)h3 * r1 + (uint64_t)h4 * r0;

        uint32_t c;
        c = (uint32_t)(d0 >> 26); h0 = (uint32_t)d0 & 0x3ffffffu;
        d1 += c;  c = (uint32_t)(d1 >> 26); h1 = (uint32_t)d1 & 0x3ffffffu;
        d2 += c;  c = (uint32_t)(d2 >> 26); h2 = (uint32_t)d2 & 0x3ffffffu;
        d3 += c;  c = (uint32_t)(d3 >> 26); h3 = (uint32_t)d3 & 0x3ffffffu;
        d4 += c;  c = (uint32_t)(d4 >> 26); h4 = (uint32_t)d4 & 0x3ffffffu;
        h0 += c * 5;
        c = h0 >> 26; h0 &= 0x3ffffffu; h1 += c;

        msg += want;
        len -= want;
    }

    /* 完全运算：先做一次模约减，再把 h 转到「可加 s」的形式 */
    uint32_t c = h1 >> 26; h1 &= 0x3ffffffu;
    h2 += c;  c = h2 >> 26; h2 &= 0x3ffffffu;
    h3 += c;  c = h3 >> 26; h3 &= 0x3ffffffu;
    h4 += c;  c = h4 >> 26; h4 &= 0x3ffffffu;
    h0 += c * 5; c = h0 >> 26; h0 &= 0x3ffffffu; h1 += c;

    /* 算 g = h + 5，看是否溢出 2^130 */
    uint32_t g0 = h0 + 5; c = g0 >> 26; g0 &= 0x3ffffffu;
    uint32_t g1 = h1 + c; c = g1 >> 26; g1 &= 0x3ffffffu;
    uint32_t g2 = h2 + c; c = g2 >> 26; g2 &= 0x3ffffffu;
    uint32_t g3 = h3 + c; c = g3 >> 26; g3 &= 0x3ffffffu;
    uint32_t g4 = h4 + c - (1u << 26);

    /* 用掩码选 h 还是 g（避免分支，也避免时序泄漏） */
    uint32_t mask = (g4 >> 31) - 1u;   /* g4 有符号下的符号位 */
    h0 = (h0 & ~mask) | (g0 & mask);
    h1 = (h1 & ~mask) | (g1 & mask);
    h2 = (h2 & ~mask) | (g2 & mask);
    h3 = (h3 & ~mask) | (g3 & mask);
    h4 = (h4 & ~mask) | (g4 & mask);

    /*
     * 把 5 个 26-bit limb 重新拼成 128 位，然后整体加上 s。
     *
     * 分两步：先纯重组出 h 的四个 32 位字（不掺 s），再逐字加 s 并传播进位。
     * 曾经的写法把"重组"和"加 s"揉在一起（第一字就 +le32(key+16)，
     * 后面各字只带上一字的 >>32 进位），结果是 h1 的 bit26..31 既进了
     * 字1 的 (h1>>6)，又被 (h1<<26) 溢出的部分算了一次，导致第二字起
     * 全部错位 —— 症状是 tag 前 4 字节对、后 12 字节全错。
     * 2026-10-01 用 Python 逐步复现定位（见 tools/ 下的对照脚本）。
     */
    uint32_t hh0, hh1, hh2, hh3;
    hh0 = h0 | (h1 << 26);
    hh1 = (h1 >> 6) | (h2 << 20);
    hh2 = (h2 >> 12) | (h3 << 14);
    hh3 = (h3 >> 18) | (h4 << 8);

    uint64_t f;
    f = (uint64_t)hh0 + (uint64_t)le32(key + 16);        put_le32(out + 0, (uint32_t)f);
    f = (uint64_t)hh1 + (uint64_t)le32(key + 20) + (f >> 32); put_le32(out + 4, (uint32_t)f);
    f = (uint64_t)hh2 + (uint64_t)le32(key + 24) + (f >> 32); put_le32(out + 8, (uint32_t)f);
    f = (uint64_t)hh3 + (uint64_t)le32(key + 28) + (f >> 32); put_le32(out + 12, (uint32_t)f);
}

/* ================================================================== */
/* SHA256 (FIPS 180-4)                                                */
/* ================================================================== */

static const uint32_t SHA256_K[64] = {
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

typedef struct {
    uint32_t h[8];
    uint64_t total;       /* 已处理的字节数 */
    uint8_t buf[64];
    size_t buflen;
} Sha256Ctx;

static void sha256_init(Sha256Ctx *c) {
    c->h[0] = 0x6a09e667u; c->h[1] = 0xbb67ae85u;
    c->h[2] = 0x3c6ef372u; c->h[3] = 0xa54ff53au;
    c->h[4] = 0x510e527fu; c->h[5] = 0x9b05688cu;
    c->h[6] = 0x1f83d9abu; c->h[7] = 0x5be0cd19u;
    c->total = 0;
    c->buflen = 0;
}

static void sha256_compress(Sha256Ctx *c, const uint8_t *p) {
    uint32_t w[64];
    for (int i = 0; i < 16; i++) {
        w[i] = be32(p + i * 4);
    }
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = rotl32(w[i - 15], 25) ^ rotl32(w[i - 15], 14)
                    ^ (w[i - 15] >> 3);
        uint32_t s1 = rotl32(w[i - 2], 15) ^ rotl32(w[i - 2], 13)
                    ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }

    uint32_t a = c->h[0], b = c->h[1], d = c->h[3], e = c->h[4];
    uint32_t f = c->h[5], g = c->h[6], hh = c->h[7];
    uint32_t cc = c->h[2];
    for (int i = 0; i < 64; i++) {
        uint32_t S1 = rotl32(e, 26) ^ rotl32(e, 21) ^ rotl32(e, 7);
        uint32_t ch = (e & f) ^ ((~e) & g);
        uint32_t t1 = hh + S1 + ch + SHA256_K[i] + w[i];
        uint32_t S0 = rotl32(a, 30) ^ rotl32(a, 19) ^ rotl32(a, 10);
        uint32_t mj = (a & b) ^ (a & cc) ^ (b & cc);
        uint32_t t2 = S0 + mj;
        hh = g; g = f; f = e; e = d + t1;
        d = cc; cc = b; b = a; a = t1 + t2;
    }
    c->h[0] += a; c->h[1] += b; c->h[2] += cc; c->h[3] += d;
    c->h[4] += e; c->h[5] += f; c->h[6] += g; c->h[7] += hh;
}

static void sha256_update(Sha256Ctx *c, const uint8_t *p, size_t len) {
    c->total += len;
    while (len > 0) {
        size_t n = 64 - c->buflen;
        if (n > len) {
            n = len;
        }
        memcpy(c->buf + c->buflen, p, n);
        c->buflen += n;
        p += n;
        len -= n;
        if (c->buflen == 64) {
            sha256_compress(c, c->buf);
            c->buflen = 0;
        }
    }
}

static void sha256_final(Sha256Ctx *c, uint8_t out[32]) {
    uint64_t bits = c->total * 8;
    uint8_t pad = 0x80;
    sha256_update(c, &pad, 1);
    uint8_t z = 0;
    while (c->buflen != 56) {
        sha256_update(c, &z, 1);
    }
    uint8_t lenb[8];
    for (int i = 0; i < 8; i++) {
        lenb[i] = (uint8_t)(bits >> (56 - i * 8));
    }
    sha256_update(c, lenb, 8);

    for (int i = 0; i < 8; i++) {
        put_be32(out + i * 4, c->h[i]);
    }
}

/*: 便捷封装：一次算完。 */
static void sha256(const uint8_t *msg, size_t len, uint8_t out[32]) {
    Sha256Ctx c;
    sha256_init(&c);
    sha256_update(&c, msg, len);
    sha256_final(&c, out);
}

/* ================================================================== */
/* 密钥派生                                                            */
/* ================================================================== */

/*
 * 派生：key = SHA256^N( device_seed || salt )
 *
 * N = CRYPTO_KDF_ROUNDS 是刻意拉高的迭代数 —— 3DS 的 ARM11 只有 268MHz，
 * 4096 次 SHA256 也就几十毫秒，但对手要暴力枚举设备因子时，成本被放大 4096 倍。
 *
 * 注意这里用「迭代哈希」不是 PBKDF2：没有密码要保护，设备因子是高熵的
 * 32 字节，不需要 PBKDF2 那种抗字典攻击的设计。
 */
void crypto_derive_key(const uint8_t *salt, size_t salt_len,
                       uint8_t out_key[CHACHA20_KEY_LEN]) {
    /*
     * 设备因子：唯一且稳定，换机器就变 —— 这正是要的性质。
     *
     * 用 CFGI_GetLocalFriendCodeSeed 而不是 PS_GetLocalFriendCodeSeed：
     * 两者读的是同一个值，但 cfgu 是配置服务（读 SecureInfo），
     * 比 ps（进程服务）轻，且不需要额外权限。3DS 上这个 seed 与
     * 主机绑定，无法从一台机器搬到另一台。
     */
    uint8_t seed[8] = {0};
    uint64_t fc = 0;
    if (R_SUCCEEDED(cfguInit())) {
        CFGI_GetLocalFriendCodeSeed(&fc);
        cfguExit();
    }
    /* cfgu 拿不到时退化为全零 seed：此时密钥只由 salt 决定，
     * 安全性下降但仍能工作（不让"读不到设备信息"变成"功能不可用"）。
     * creds.c 会把这件事记进 status.txt，界面上也照实说。 */
    for (int i = 0; i < 8; i++) {
        seed[i] = (uint8_t)(fc >> (i * 8));
    }

    uint8_t buf[8 + CRYPTO_SALT_LEN];
    memcpy(buf, seed, 8);
    size_t n = salt_len < CRYPTO_SALT_LEN ? salt_len : CRYPTO_SALT_LEN;
    memcpy(buf + 8, salt, n);

    sha256(buf, 8 + n, out_key);

    for (int i = 0; i < CRYPTO_KDF_ROUNDS - 1; i++) {
        /*
         * 每轮把上一轮的输出连同 salt 再哈希一次。
         *
         * 缓冲必须按「32 字节密钥 + 最大 salt」开，不能只写 40 ——
         * salt 最长 16 字节时 32+16=48 会越界 8 字节，每轮都踩一次栈，
         * 4096 轮下来把调用者的栈帧打烂（症状：crypto_selftest 返回
         * 一个像 0x5C5C5C5D 的垃圾值，因为 0x5c 正是测试 salt 的填充）。
         * 2026-10-01 实锤。
         */
        uint8_t tmp[32 + CRYPTO_SALT_LEN];
        memcpy(tmp, out_key, 32);
        memcpy(tmp + 32, salt, n);
        sha256(tmp, 32 + n, out_key);
    }

    memset(seed, 0, sizeof(seed));
    memset(buf, 0, sizeof(buf));
}

void crypto_random(uint8_t *buf, size_t len) {
    /*
     * 优先用 ps 的硬件熵源（PS_GenerateRandomBytes 走内核 CSPRNG）。
     * 拿不到就退化为「osGetTime 起种 + xorshift」——
     * 弱很多，但只影响 salt/nonce 的不可预测性，不影响算法正确性；
     * 而 nonce 只要不重复就够（同一密钥下）。
     */
    if (R_SUCCEEDED(psInit())) {
        if (R_SUCCEEDED(PS_GenerateRandomBytes(buf, len))) {
            psExit();
            return;
        }
        psExit();
    }

    uint64_t t = osGetTime();
    uint32_t s = (uint32_t)(t ^ (t >> 32)) ^ 0x9e3779b9u;
    for (size_t i = 0; i < len; i++) {
        s ^= s << 13;
        s ^= s >> 17;
        s ^= s << 5;
        buf[i] = (uint8_t)(s >> 24);
    }
}

bool crypto_equal_ct(const uint8_t *a, const uint8_t *b, size_t len) {
    uint8_t d = 0;
    for (size_t i = 0; i < len; i++) {
        d |= (uint8_t)(a[i] ^ b[i]);
    }
    return d == 0;
}

/* ================================================================== */
/* 自检：RFC 8439 官方测试向量                                          */
/* ================================================================== */

/*: 16 进制串转字节，长度不符返回 -1。 */
static int unhex(const char *s, uint8_t *out, size_t out_max) {
    size_t n = 0;
    while (s[0] && s[1]) {
        if (n >= out_max) {
            return -1;
        }
        int hi, lo;
        char a = s[0], b = s[1];
        hi = (a >= '0' && a <= '9') ? a - '0'
           : (a >= 'a' && a <= 'f') ? a - 'a' + 10 : -1;
        lo = (b >= '0' && b <= '9') ? b - '0'
           : (b >= 'a' && b <= 'f') ? b - 'a' + 10 : -1;
        if (hi < 0 || lo < 0) {
            return -1;
        }
        out[n++] = (uint8_t)((hi << 4) | lo);
        s += 2;
    }
    return (int)n;
}

static bool selftest_chacha_keystream(void) {
    uint8_t key[32];
    for (int i = 0; i < 32; i++) {
        key[i] = (uint8_t)i;
    }
    uint8_t nonce[12] = {0, 0, 0, 0x09, 0, 0, 0, 0x4a, 0, 0, 0, 0};
    uint8_t buf[64];
    memset(buf, 0, sizeof(buf));   /* 全零 XOR 密钥流 = 密钥流本身 */
    chacha20_xor(buf, 64, key, nonce, 1);

    uint8_t want[64];
    const char *h =
        "10f1e7e4d13b5915500fdd1fa32071c4c7d1f4c733c068030422aa9ac3d46c4e"
        "d2826446079faa0914c2d705d98b02a2b5129cd1de164eb9cbd083e8a2503c4e";
    if (unhex(h, want, sizeof(want)) != 64) {
        return false;
    }
    return crypto_equal_ct(buf, want, 64);
}

static bool selftest_poly1305(void) {
    uint8_t key[32];
    const char *kh =
        "85d6be7857556d337f4452fe42d506a8"
        "0103808afb0db2fd4abff6af4149f51b";
    if (unhex(kh, key, sizeof(key)) != 32) {
        return false;
    }
    const char *msg = "Cryptographic Forum Research Group";
    uint8_t tag[16];
    poly1305_mac((const uint8_t *)msg, strlen(msg), key, tag);

    uint8_t want[16];
    if (unhex("a8061dc1305136c6c22b8baf0c0127a9", want, sizeof(want)) != 16) {
        return false;
    }
    return crypto_equal_ct(tag, want, 16);
}

static bool selftest_sha256(void) {
    /* SHA256("abc") = ba7816bf... —— FIPS 180-4 标准样例。
     * sha256 是 static，这里直接在文件内调。 */
    uint8_t out[32];
    sha256((const uint8_t *)"abc", 3, out);
    uint8_t want[32];
    if (unhex("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
              want, sizeof(want)) != 32) {
        return false;
    }
    return crypto_equal_ct(out, want, 32);
}

static bool selftest_roundtrip(void) {
    uint8_t key[32], nonce[12], a[100], b[100];
    /* 用固定值而不是随机数，保证自检可复现 */
    for (int i = 0; i < 32; i++) {
        key[i] = (uint8_t)(i * 7 + 1);
    }
    for (int i = 0; i < 12; i++) {
        nonce[i] = (uint8_t)(i * 3 + 5);
    }
    for (int i = 0; i < 100; i++) {
        a[i] = (uint8_t)(i ^ 0x5a);
    }
    memcpy(b, a, 100);
    chacha20_xor(b, 100, key, nonce, 1);
    if (memcmp(a, b, 100) == 0) {
        return false;   /* 加密后必须变样 */
    }
    chacha20_xor(b, 100, key, nonce, 1);
    return memcmp(a, b, 100) == 0;
}

static bool selftest_derive(void) {
    uint8_t salt[16], k1[32], k2[32];
    memset(salt, 0x5c, sizeof(salt));
    crypto_derive_key(salt, sizeof(salt), k1);
    crypto_derive_key(salt, sizeof(salt), k2);
    if (memcmp(k1, k2, 32) != 0) {
        return false;   /* 同输入必须同输出 */
    }
    /* 换个 salt 应该得到不同密钥 */
    salt[0] ^= 0xff;
    crypto_derive_key(salt, sizeof(salt), k2);
    return memcmp(k1, k2, 32) != 0;
}

/*: 位掩码形式的自检结果：bit0=chacha bit1=poly1305 bit2=sha256
 *  bit3=roundtrip bit4=derive。1 = 通过。
 *  上面那个"通过项数"只知道总分，看不出**哪一项**挂了；排障时
 *  光看"4/5"完全没法定位。这个位掩码一次就能说清是哪个算法在出错。 */
static int s_selftest_mask = -1;   /* -1 = 还没跑过 */

/* ★ 跑一遍全部 5 个自检，把结果缓存在 s_selftest_mask。
 *
 * 为什么要合并（2026-10-18 启动卡顿优化）：
 *   crypto_selftest()（数通过项）与 crypto_selftest_mask()（位掩码）
 *   原先**各自把 5 个测试跑一遍** —— 而 selftest_derive() 单次就调
 *   3 次 crypto_derive_key()，每次 4096 轮 SHA-256。于是启动路径上
 *   光 derive 一项就跑了 2(两函数) × 3 × 4096 = 24576 轮，
 *   再加上 creds_roundtrip_selftest() 的 4096 轮，接近 3 万轮 ——
 *   ARM11 268MHz 上这就是"开软件卡"的主因。
 *   自检是纯函数（无状态、同输入同输出），跑一遍缓存结果与跑两遍
 *   在语义上完全等价，判据（项数 / mask）一个字节都不变。
 *
 * 【副作用】结果跨调用缓存。自检只在启动时跑一次，进程生命周期内
 *   算法不会变，所以缓存安全；crypto_test_reset() 可在需要时清掉。 */
static void selftest_run_once(void)
{
    if (s_selftest_mask >= 0)
        return;
    int m = 0;
    if (selftest_chacha_keystream()) m |= 1 << 0;
    if (selftest_poly1305())         m |= 1 << 1;
    if (selftest_sha256())           m |= 1 << 2;
    if (selftest_roundtrip())        m |= 1 << 3;
    if (selftest_derive())           m |= 1 << 4;
    s_selftest_mask = m;
}

void crypto_test_reset(void) { s_selftest_mask = -1; }

int crypto_selftest_mask(void) {
    selftest_run_once();
    return s_selftest_mask;
}

int crypto_selftest(void) {
    /* 从缓存掩码里数 bit —— 不再重跑 5 个测试。
     * 数出来与逐项累加完全同值（每项只贡献 1 bit）。 */
    int m = crypto_selftest_mask();
    int ok = 0;
    for (int i = 0; i < 5; ++i)
        if (m & (1 << i)) ++ok;
    return ok;
}
