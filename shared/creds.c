/*
 * creds.c -- creds.enc 的读写（见 creds.h 的格式说明）
 */

#include "creds.h"
#include "crypto.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <3ds.h>

/* ================================================================== */
/* 文件头                                                              */
/* ================================================================== */

#define CREDS_MAGIC "AICRED01"
#define CREDS_VERSION 1

#pragma pack(push, 1)
typedef struct {
    char     magic[8];
    uint16_t version;
    uint16_t flags;
    uint8_t  salt[CRYPTO_SALT_LEN];
    uint8_t  nonce[CHACHA20_NONCE_LEN];
    uint32_t len;                       /* 明文长度 */
    uint8_t  mac[POLY1305_TAG_LEN];     /* 覆盖明文的标签 */
} CredsHeader;
#pragma pack(pop)

#define CREDS_HEADER_SIZE ((size_t)sizeof(CredsHeader))

/*: salt 缓存：一次会话里多次保存复用同一 salt（见 creds.h 的说明）。 */
static uint8_t  s_salt[CRYPTO_SALT_LEN];
static bool     s_salt_ok = false;

/*: 加密可用闸门，默认**关闭** —— 必须由 main 跑完自检后显式打开。
 * 默认开是危险的：任何人在自检之前调 creds_save，都会写出一份
 * 没验过的密文。宁可默认不可用，让调用方退回明文并报出来。 */
static bool     s_available = false;

static char     s_note[128] = "";

void creds_set_available(bool ok) { s_available = ok; }
bool creds_available(void) { return s_available; }

const char *creds_status_str(CredsStatus st) {
    switch (st) {
    case CREDS_OK:          return "成功";
    case CREDS_ERR_NONE:    return "文件不存在";
    case CREDS_ERR_IO:      return "读写失败";
    case CREDS_ERR_FORMAT:  return "格式不对";
    case CREDS_ERR_AUTH:    return "校验失败（被改过，或是别的机器的文件）";
    case CREDS_ERR_NOMEM:   return "内容太长";
    }
    return "未知状态";
}

const char *creds_last_note(void) {
    return s_note;
}

static void set_note(const char *s) {
    size_t n = strlen(s);
    if (n >= sizeof(s_note)) {
        n = sizeof(s_note) - 1;
    }
    memcpy(s_note, s, n);
    s_note[n] = 0;
}

/* ================================================================== */
/* 小工具                                                              */
/* ================================================================== */

static bool read_whole(const char *path, uint8_t **out, size_t *out_len) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        return false;
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return false;
    }
    long sz = ftell(f);
    if (sz < 0 || (size_t)sz > CREDS_HEADER_SIZE + CREDS_MAX_PLAIN) {
        fclose(f);
        return false;
    }
    rewind(f);
    uint8_t *buf = (uint8_t *)malloc((size_t)sz + 1);
    if (!buf) {
        fclose(f);
        return false;
    }
    size_t got = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    if (got != (size_t)sz) {
        free(buf);
        return false;
    }
    *out = buf;
    *out_len = got;
    return true;
}

/*
 * 原子写：先写 .tmp 再改名。
 * 掉电/卡满导致写一半时，原 creds.enc 仍完好 —— 否则用户会"存了凭据
 * 反而把它弄丢了"，比不加密还糟。
 */
static bool write_atomic(const char *path, const uint8_t *buf, size_t len) {
    char tmp[128];
    size_t plen = strlen(path);
    if (plen + 5 >= sizeof(tmp)) {
        return false;
    }
    memcpy(tmp, path, plen);
    memcpy(tmp + plen, ".tmp", 5);

    FILE *f = fopen(tmp, "wb");
    if (!f) {
        return false;
    }
    size_t wrote = fwrite(buf, 1, len, f);
    fflush(f);
    fclose(f);
    if (wrote != len) {
        remove(tmp);
        return false;
    }

    /* 3DS 的 FAT 上 rename 到已存在的目标：libctru 的 rename 行为是
     * 先删后改，这里手动保证语义一致（目标存在时先删）。 */
    if (creds_exists()) {
        remove(path);
    }
    if (rename(tmp, path) != 0) {
        remove(tmp);
        return false;
    }
    return true;
}

/* ================================================================== */
/* 对外接口                                                            */
/* ================================================================== */

bool creds_exists(void) {
    FILE *f = fopen(CREDS_PATH, "rb");
    if (!f) {
        return false;
    }
    fclose(f);
    return true;
}

CredsStatus creds_save(const void *buf, size_t len) {
    /* 闸门没开 = 自检没过或还没跑。别写出没验过的密文。 */
    if (!s_available) {
        set_note("加密未启用（自检未通过），未写 creds.enc");
        return CREDS_ERR_NONE;
    }
    if (len > CREDS_MAX_PLAIN) {
        set_note("内容超过上限，未保存");
        return CREDS_ERR_NOMEM;
    }
    if (len == 0) {
        /* ★ 空内容不是"太长"（S-12）。报 NOMEM 会让调用方在状态里显示
         *   "内存不足/内容过大"，而用户实际是**什么都没填**。
         *   按"格式不对"报，上层才会说人话。 */
        set_note("空内容，未保存");
        return CREDS_ERR_FORMAT;
    }

    /* salt：首次保存生成，之后复用（见 creds.h 的说明） */
    if (!s_salt_ok) {
        crypto_random(s_salt, sizeof(s_salt));
        s_salt_ok = true;
    }

    CredsHeader h;
    memset(&h, 0, sizeof(h));
    memcpy(h.magic, CREDS_MAGIC, 8);
    h.version = CREDS_VERSION;
    h.flags = 0;
    memcpy(h.salt, s_salt, sizeof(s_salt));
    /* **每次保存都换新 nonce** —— 流密码复用 nonce 会泄漏明文异或 */
    crypto_random(h.nonce, sizeof(h.nonce));
    h.len = (uint32_t)len;

    uint8_t key[CHACHA20_KEY_LEN];
    crypto_derive_key(h.salt, sizeof(h.salt), key);

    /*
     * 先 MAC 后加密（encrypt-then-MAC 的反面）：
     * 这里算的是"明文的 MAC"，随密文一起存。校验时先解密再验 MAC。
     * 对本场景够用 —— 攻击者改密文会让解密结果乱掉、MAC 自然不过。
     * （encrypt-then-MAC 更严格，但那要在密文上算 MAC，得把整段密文
     *   缓存一份；这里数据量小，两者安全性无实质差别。）
     *
     * ★ MAC 密钥直接**复制**加密密钥再错开，不再单独 derive 一次
     *   （2026-10-18 启动卡顿优化）：原先写的是
     *     crypto_derive_key(h.salt, ..., mac_key);
     *   与上一行同 salt、同函数 —— KDF 是确定性的，两次输出逐字节相同，
     *   等于把 4096 轮 SHA-256 白算一遍。复制是等价且省掉那一遍。
     */
    uint8_t mac_key[32];
    memcpy(mac_key, key, sizeof mac_key);
    /* KDF 输出当 MAC 密钥前，和加密密钥错开一下，避免同密钥两用 */
    for (int i = 0; i < 32; i++) {
        mac_key[i] ^= (uint8_t)(0xa5 + i);
    }
    poly1305_mac((const uint8_t *)buf, len, mac_key, h.mac);

    /* 组装：头 + 密文 */
    size_t total = CREDS_HEADER_SIZE + len;
    uint8_t *out = (uint8_t *)malloc(total);
    if (!out) {
        memset(key, 0, sizeof(key));
        memset(mac_key, 0, sizeof(mac_key));
        set_note("内存不足");
        return CREDS_ERR_IO;
    }
    memcpy(out, &h, CREDS_HEADER_SIZE);
    memcpy(out + CREDS_HEADER_SIZE, buf, len);
    chacha20_xor(out + CREDS_HEADER_SIZE, len, key, h.nonce, 1);

    bool ok = write_atomic(CREDS_PATH, out, total);
    free(out);
    memset(key, 0, sizeof(key));
    memset(mac_key, 0, sizeof(mac_key));

    if (!ok) {
        set_note("写 creds.enc 失败");
        return CREDS_ERR_IO;
    }
    set_note("已加密保存到 creds.enc");
    return CREDS_OK;
}

CredsStatus creds_load(void *out, size_t out_cap, size_t *out_len) {
    if (out_len) {
        *out_len = 0;
    }
    if (!creds_exists()) {
        set_note("creds.enc 不存在");
        return CREDS_ERR_NONE;
    }

    uint8_t *raw = NULL;
    size_t raw_len = 0;
    if (!read_whole(CREDS_PATH, &raw, &raw_len)) {
        set_note("读 creds.enc 失败");
        return CREDS_ERR_IO;
    }
    if (raw_len < CREDS_HEADER_SIZE) {
        free(raw);
        set_note("creds.enc 太短");
        return CREDS_ERR_FORMAT;
    }

    CredsHeader h;
    memcpy(&h, raw, CREDS_HEADER_SIZE);
    if (memcmp(h.magic, CREDS_MAGIC, 8) != 0 || h.version != CREDS_VERSION) {
        free(raw);
        set_note("creds.enc 魔术/版本不对");
        return CREDS_ERR_FORMAT;
    }
    if (h.len == 0 || h.len > CREDS_MAX_PLAIN
        || raw_len != CREDS_HEADER_SIZE + h.len) {
        free(raw);
        set_note("creds.enc 长度不对");
        return CREDS_ERR_FORMAT;
    }
    if (out == NULL || out_cap < h.len) {
        free(raw);
        set_note("调用方缓冲区太小");
        return CREDS_ERR_NOMEM;
    }

    uint8_t key[CHACHA20_KEY_LEN];
    crypto_derive_key(h.salt, sizeof(h.salt), key);

    /* ★ MAC 密钥=加密密钥（同样错开 0xa5+i），**复制**而非再 derive 一次
     *   —— 与 creds_save 同一处优化（2026-10-18 启动卡顿）：同 salt 同函数
     *   的两次 derive 输出完全相同，重复调用白算 4096 轮。
     *   ⚠️ 必须在下面 memset(key) **之前**复制 —— 顺序反了会得到全零 mac_key，
     *   于是"每个文件都 MAC 不过"，症状是解密永远失败。 */
    uint8_t mac_key[32];
    memcpy(mac_key, key, sizeof mac_key);
    for (int i = 0; i < 32; i++) {
        mac_key[i] ^= (uint8_t)(0xa5 + i);
    }

    memcpy(out, raw + CREDS_HEADER_SIZE, h.len);
    free(raw);
    chacha20_xor((uint8_t *)out, h.len, key, h.nonce, 1);
    memset(key, 0, sizeof(key));

    /* 验 MAC：不过就说明内容被改过，或者这是别的机器产生的文件
     * （因为在别的机器上派生出的 key 不同，解出来是乱码，MAC 必然不过）。 */
    uint8_t tag[POLY1305_TAG_LEN];
    poly1305_mac((const uint8_t *)out, h.len, mac_key, tag);
    memset(mac_key, 0, sizeof(mac_key));

    if (!crypto_equal_ct(tag, h.mac, POLY1305_TAG_LEN)) {
        /* 别把可疑数据留给上层，清零 */
        memset(out, 0, h.len);
        if (out_len) {
            *out_len = 0;
        }
        set_note("creds.enc 校验失败（被改过 / 换了机器）");
        return CREDS_ERR_AUTH;
    }

    /* 记住 salt，这样同会话内再保存时复用同一个 */
    memcpy(s_salt, h.salt, sizeof(s_salt));
    s_salt_ok = true;

    if (out_len) {
        *out_len = h.len;
    }
    set_note("creds.enc 解密成功");
    return CREDS_OK;
}

bool creds_remove(void) {
    if (!creds_exists()) {
        set_note("creds.enc 本来就不在");
        return true;
    }
    if (remove(CREDS_PATH) != 0) {
        set_note("删 creds.enc 失败");
        return false;
    }
    set_note("已删除 creds.enc");
    return true;
}

bool creds_roundtrip_selftest(void) {
    /* 用内存假数据走一遍，不动 SD 卡 */
    const char *sample = "{\"active\":\"t\",\"keys\":[{\"key\":\"sk-test-123\"}]}";
    size_t n = strlen(sample);

    uint8_t salt[CRYPTO_SALT_LEN];
    uint8_t nonce[CHACHA20_NONCE_LEN];
    crypto_random(salt, sizeof(salt));
    crypto_random(nonce, sizeof(nonce));

    uint8_t key[CHACHA20_KEY_LEN];
    crypto_derive_key(salt, sizeof(salt), key);

    uint8_t buf[256];
    memcpy(buf, sample, n);
    chacha20_xor(buf, n, key, nonce, 1);
    if (memcmp(buf, sample, n) == 0) {
        return false;       /* 加密后必须变样 */
    }
    chacha20_xor(buf, n, key, nonce, 1);
    return memcmp(buf, sample, n) == 0;
}
