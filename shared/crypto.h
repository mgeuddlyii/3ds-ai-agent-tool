/*
 * crypto.h -- ChaCha20 流密码 + 密钥派生（M5 凭据加密用）
 *
 * 为什么手写而不是装 3ds-mbedtls 包
 * ---------------------------------
 * libctru 把 mbedtls 静态链进去了，但**不导出头文件**，所以没有现成 API。
 * 装 3ds-mbedtls 包当然更正统，但它要多走一遍包下载/解压/展开软链接的流程，
 * 还会多链一个库进程序。而这里要的就是一个流密码加一次哈希 ——
 * ChaCha20 是 RFC 8439 里定义得很死的东西，实现短、测试向量明确，
 * 200 行就能写完且可验证。选它。
 *
 * 强度如实说明（界面上也要这么说，不过度承诺）
 * -------------------------------------------
 * 这防的是「SD 卡被拷走、顺手读明文 Key」，**不是**有决心的攻击者 ——
 * 3DS 用户态没有安全隔区（TEE），密钥最终要在内存里拼出来。
 * 它挡住的是"把卡插到电脑上直接看文件"这一档风险，这也是真实存在的场景。
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* ------------------------------------------------------------------ */
/* ChaCha20                                                           */
/* ------------------------------------------------------------------ */

/*: ChaCha20 的 nonce 是 96 bit（RFC 8439 的 IETF 变体）。 */
#define CHACHA20_NONCE_LEN 12
/*: 密钥 256 bit。 */
#define CHACHA20_KEY_LEN   32

/*
 * 对 buf 就地加/解密（ChaCha20 是对称流密码，加密解密同一个操作）。
 *
 * counter 是块计数器，从 1 开始（RFC 8439 的加密用法）。
 * 同一 (key, nonce, counter) 只能用来加密一份数据，否则异或流会复用 ——
 * 所以每次保存都必须换一个新 nonce（见 creds.c 的做法）。
 */
void chacha20_xor(uint8_t *buf, size_t len,
                  const uint8_t key[CHACHA20_KEY_LEN],
                  const uint8_t nonce[CHACHA20_NONCE_LEN],
                  uint32_t counter);

/*
 * Poly1305 一次性认证标签（MAC），32 字节密钥、16 字节输出。
 *
 * 用它来防"密文被改了一个字节但程序照样解密出一堆垃圾" ——
 * 没有 MAC 的话，篡改在解密前是发现不了的。
 */
#define POLY1305_TAG_LEN 16
void poly1305_mac(const uint8_t *msg, size_t len,
                  const uint8_t key[32], uint8_t out[POLY1305_TAG_LEN]);

/* ------------------------------------------------------------------ */
/* 密钥派生（KDF）                                                     */
/* ------------------------------------------------------------------ */

/*: salt 长度。每次… 准确说是**每台设备首次生成一次**，存 SD 卡。 */
#define CRYPTO_SALT_LEN 16

/*
 * 从「设备唯一信息 + 随机 salt」派生 32 字节密钥。
 *
 * 设备因子用 osGetLocalFriendCodeSeed()：它是设备唯一的、且 3DS 上
 * 用户态能稳定读到的值。换一台机器就读不到同样的值 → creds.enc 解不开，
 * 这正是 M5 验收要的结果（"拷到别的机器解不开"）。
 *
 * salt 是随机生成的，单独存在文件头里。有了它，两台设备就算设备因子
 * 意外相同，密钥也不同。
 *
 * 迭代次数故意拉高：3DS 的 ARM11 主频 268MHz，这里多算几次是毫秒级，
 * 但对"暴力猜设备因子"是实打实的成本。见 CRYPTO_KDF_ROUNDS。
 */
#define CRYPTO_KDF_ROUNDS 4096

void crypto_derive_key(const uint8_t *salt, size_t salt_len,
                       uint8_t out_key[CHACHA20_KEY_LEN]);

/*: 用系统熵源填随机字节（封装 libctru 的随机数接口）。 */
void crypto_random(uint8_t *buf, size_t len);

/*: 常量时间比较（防时序侧信道）。相等返回 true。 */
bool crypto_equal_ct(const uint8_t *a, const uint8_t *b, size_t len);

/* ------------------------------------------------------------------ */
/* 自检                                                                */
/* ------------------------------------------------------------------ */

/*
 * 跑一遍 RFC 8439 的官方测试向量，返回通过的项数（见 crypto.c 末尾）。
 *
 * 为什么要有这个：本机没有 PC 架构的 gcc（MSYS2 只装了 make），
 * 加密内核没法在 PC 上编译验证。那就让它**在设备上自证** ——
 * 启动时跑一遍标准向量，结果写进 status.txt。
 * 这样验的是真正跑在设备上的那份代码，比 PC 上另编一份更可信。
 *
/*: 全部通过返回 CRYPTO_SELFTEST_TOTAL。
 *
 * ★ 自检结果**跨调用缓存**（2026-10-18 启动卡顿优化）：crypto_selftest()
 *   与 crypto_selftest_mask() 现在共用同一份缓存，不再各跑一遍 5 个测试。
 *   自检是纯函数，缓存与重跑语义等价；判据（项数 / mask）不变。
 *   KDF 4096 轮的 derive 自检原本被跑两遍，是"开软件卡"的主因。
 */
#define CRYPTO_SELFTEST_TOTAL 5
int crypto_selftest(void);

/*
 * 上者的位掩码版本：bit0=chacha bit1=poly1305 bit2=sha256
 * bit3=roundtrip bit4=derive，置位 = 通过。
 * 排障用 —— "4/5" 看不出是哪一项挂的，掩码一眼能定位。
 */
int crypto_selftest_mask(void);

/*: 清掉自检缓存，让下一次调用重跑。仅测试/热重载用；正常启动不必调。 */
void crypto_test_reset(void);
#define CRYPTO_SELFTEST_ALL ((1 << CRYPTO_SELFTEST_TOTAL) - 1)

