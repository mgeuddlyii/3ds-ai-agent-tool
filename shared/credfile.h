/*
 * credfile.h -- SD 卡上的凭据文件（用户手动写入，程序自动读取）
 *
 * 为什么要有这个东西：3DS 上敲一长串 API Key 是酷刑。虚拟键盘没有粘贴、
 * 打错一个字符就得从头再来。而绝大多数人有电脑或手机 —— 让他们在那边
 * 用记事本改一行，回来这边自动读取，体验差别是数量级的。
 *
 * 文件位置固定为 sdmc:/3ds/aiconsole/key.txt。称它为"约定"而不是"配置项"：
 * 路径写在代码里，用户不能改 —— 改了就得猜，猜就会错。
 *
 * 这个通道与 M5 的 creds.enc 并不冲突：
 *   key.txt   = 入口（明文，用户自己写）
 *   creds.enc = 归宿（加密，程序导入后自己存）
 * 导入成功后建议提示用户可以删掉 key.txt。
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>

/* API Key 长度上限。各家目前都在 100 字符左右，200 留足余量；
 * 超过就不认 —— 那多半是有人把整页网页粘进来了。 */
#define CREDFILE_KEY_MAX 200

typedef enum {
    CREDFILE_NONE = 0,   /* 文件不存在（会顺带说明模板已建好） */
    CREDFILE_EMPTY,      /* 文件在，但没有有效内容 */
    CREDFILE_OK,         /* 读到 Key 了 */
    CREDFILE_TOO_LONG,   /* Key 超过上限，多半是粘错了 */
} CredFileStatus;

typedef struct {
    CredFileStatus status;
    char provider[24];   /* 可选：provider=xxx */
    char model[48];      /* 可选：model=xxx */
    /* ★ 可选：base_url=xxx —— 只在 provider=custom 时有意义。
     * 缺了它，"自定义服务商"这条路就只能靠系统键盘敲网址，
     * 而网址恰好是最长、最容易敲错的一串。 */
    char base_url[96];
    char key[CREDFILE_KEY_MAX + 1];
    int  key_len;
    int  lines;          /* 读到的有效行数，诊断用 */
} CredFile;

const char *credfile_path(void);          /* sdmc:/3ds/aiconsole/key.txt */
const char *credfile_path_display(void);  /* SD:/3ds/aiconsole/key.txt（给人看） */

/* 扫描一次。永远返回 true/false 表示"能不能算完成"，结构体里带原因。 */
bool credfile_scan(CredFile *out);

/* 状态 -> 一句人话。UI 直接显示，别再自己 switch 一遍。 */
const char *credfile_status_text(CredFileStatus s);

/* 文件不存在时写一份带说明的模板。用户插上卡就能看见该文件，
 * 不至于要自己猜文件名、猜放哪 —— 这一步省掉的困惑比想象的多。 */
bool credfile_write_template(void);

/* 遮蔽预览：前 4 后 4，中间打星。key 为空时输出 "(空)"。 */
void credfile_mask(const char *key, char *out, size_t cap);
