/*
 * creds.h -- 加密凭据文件 creds.enc（M5）
 *
 * 做什么
 * ------
 * 把 keys.json 那份明文仓库整体加密成 creds.enc，存在 SD 卡上。
 * 目的不是"绝对安全"，而是把风险从「把卡插电脑上直接读到 API Key」
 * 降到「有决心的人拿到卡也要多费一道功夫」。
 *
 * 为什么这算数
 * ------------
 * 3DS 上 SD 卡是裸 FAT 分区，任何电脑都能读。用户把卡拔下来拷照片时，
 * keys.json 就是一份明文密钥文件躺在那儿 —— 这是**真实存在**的泄露路径。
 * 加密后，拷走卡的人拿到的是密文；而密钥由「设备因子 + 随机 salt」派生，
 * 设备因子换机器就变，所以密文拷到别的机器也解不开。
 *
 * 挑明做不到什么
 * --------------
 * 3DS 用户态**没有安全隔区**（TEE）。密钥必须在这台机器的内存里拼出来，
 * 所以挡不住能在这台机器上跑任意代码的攻击者（比如自制程序、调试器）。
 * 界面文案要照实说，不写"军事级加密"这种话。
 *
 * 文件格式 creds.enc
 * ------------------
 *   magic    8 B   "AICRED01"
 *   version  u16   = 1
 *   flags    u16   = 0
 *   salt     16 B  随机，首次保存时生成，之后每次保存复用
 *   nonce    12 B  随机，**每次保存都换新**（流密码复用 nonce 会致命）
 *   len      u32   明文长度
 *   mac      16 B  Poly1305 标签，覆盖明文（先 MAC 后加密，防篡改）
 *   body     len B ChaCha20 密文
 *
 * 注意 nonce 必须每次保存都换：ChaCha20 是流密码，同一 (key, nonce) 下
 * 加密两份不同的明文，异或就能泄出信息。所以每次 creds_save 都重新随机。
 * salt 反而不必每次换（它只进 KDF，换不换都不影响流密码安全性），
 * 而且换 salt 会让旧文件在只拿到新 salt 时无法解 —— 保持稳定更好。
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*: creds.enc 的固定路径。和 key.txt / keys.json 同一目录。 */
#define CREDS_PATH "sdmc:/3ds/aiconsole/creds.enc"

typedef enum {
    CREDS_OK = 0,
    CREDS_ERR_NONE,          /* 文件不存在（还没保存过） */
    CREDS_ERR_IO,            /* 读写失败 */
    CREDS_ERR_FORMAT,        /* 魔术/版本/长度不对 */
    CREDS_ERR_AUTH,          /* MAC 校验失败 —— 密文被改过，或换了机器 */
    CREDS_ERR_NOMEM,         /* 明文太长 */
} CredsStatus;

/*: 明文大小上限。keys.json 满仓库 8 把 key、每把 200 字节也就几 KB。 */
#define CREDS_MAX_PLAIN 16384

/*: 把状态翻译成给人看的中文（界面/日志共用，避免两处措辞不一致）。 */
const char *creds_status_str(CredsStatus st);

/*
 * 加密可用性闸门。creds_save 在这道闸门关闭时直接返回 CREDS_ERR_NONE，
 * 让调用方（keystore）自动退回明文路径。
 *
 * 为什么不干脆不看闸门、能写就写：加密内核自检挂了还照样生成一个
 * creds.enc，会造出一个"看着加密了、其实解不开或不可靠"的文件 ——
 * 那比明文的 keys.json 更坏，因为用户会以为自己是安全的。
 * main 在跑完 crypto_selftest 之后调用 creds_set_available()。
 */
void creds_set_available(bool ok);
bool creds_available(void);

/* 加密保存：buf/len 是明文（通常是 keys.json 的原文）。
 * 首次调用会生成并记住 salt；每次调用都换新 nonce。
 * 内部用"写临时文件再改名"，避免写一半掉电把原文件毁了。 */
CredsStatus creds_save(const void *buf, size_t len);

/* 解密读取。out/out_cap 是调用方给的缓冲区，实际长度写进 *out_len。
 * MAC 不过一律返回 CREDS_ERR_AUTH，绝不把可疑数据交给上层。 */
CredsStatus creds_load(void *out, size_t out_cap, size_t *out_len);

/*: 文件在不在（不看内容是否有效）。 */
bool creds_exists(void);

/*: 删掉 creds.enc（清空凭据用）。文件本来不在也算成功。 */
bool creds_remove(void);

/* 开发/诊断用：把加解密跑一遍（用内存里的假数据），返回是否自洽。
 * 不动 SD 卡上的真实文件。 */
bool creds_roundtrip_selftest(void);

/*: 最近一次操作的人话说明，写进 status.txt 用。 */
const char *creds_last_note(void);
