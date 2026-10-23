/*
 * keystore.h -- SD 卡上的多 Key 仓库（keys.json）
 *
 * 为什么要有它：key.txt 是"入口"——人手编辑、一次一把。但用户手里
 * 往往不止一把 Key（智谱免费 + DeepSeek 便宜 + 豆包……），要求他每次
 * 切换都去改文件是本末倒置。仓库存多把，屏上随时切。
 *
 * 分工（与 credfile.h 的约定一致）：
 *   key.txt   = 收件箱（人手写，导入后可以删）
 *   keys.json = 仓库（程序读写，屏上管理）
 *   creds.enc = M5 的加密归宿（迁移时从 keys.json 转，接口不用变）
 *
 * 刻意**不做**自动导入：启动时悄悄把 key.txt 收进仓库，会在验收趟次
 * 往仓库里塞测试凭据，也违背"导入是用户明确的动作"这条原则。
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "credfile.h"       /* CREDFILE_KEY_MAX */

#define KEYSTORE_MAX 8      /* 8 把够了；要更多说明该做分组的钱包了 */
#define KEYSTORE_NAME_MAX 24

typedef struct {
    char name[KEYSTORE_NAME_MAX];        /* 用户起的名，如 "zhipu-3ds" */
    char provider[24];
    char model[48];
    char key[CREDFILE_KEY_MAX + 1];
} KeyEntry;

/* 载入 sdmc:/3ds/aiconsole/keys.json。文件不存在或坏掉时仓库为空，
 * 返回 false —— UI 据此显示引导文案，不炸。 */
bool keystore_load(void);

int              keystore_count(void);
const KeyEntry  *keystore_at(int i);
const KeyEntry  *keystore_active(void);      /* 没有或指错名时返回 NULL */
const char      *keystore_active_name(void);

/* 以下都会落盘。 */
bool keystore_set_active(const char *name);
bool keystore_remove(const char *name);      /* 删的是当前激活的会自动换头一个 */

/* 取消激活，让 keystore_active() 回到 NULL。
 *
 * 为什么需要它（不是"多此一举"）：**豆包不用 Key**，所以切到豆包时
 * 仓库里根本没有它的条目 —— 没有条目可激活。而对话屏进屏时的 provider
 * 来源优先级是「keystore 激活条目 > key.txt > config」（见 sc_chat.c 的
 * chat_enter），只要仓库里还激活着别家的一把 Key，服务目录里选的那家
 * 就会被悄悄盖掉 —— 用户的切换看起来"没生效"。
 * keystore_set_active("") 做不到这件事（它对空名直接 return false，
 * 那是有意的：那是"传错了参数"，不是"请清空"）。 */
bool keystore_clear_active(void);

/* 从 key.txt 导入一把（credfile_scan 读收件箱）。
 * 名字自动生成：provider-序号，重名自动加后缀。
 * 仓库满返回 false。
 *
 * ★ 2026-10-22：out_base_url 是**可选**的出口参数。
 *   为什么要它：key.txt 现在可以写 `provider=custom` + `base_url=...`
 *   （见 credfile.h），但 KeyEntry 里**没有**网址字段 —— 自定义网址住在
 *   cfg->ai.base_url，属于 app 层。让 shared 层的 keystore 反过来调
 *   app_cfg() 会破坏分层（shared 不许依赖 app/），所以把读到的网址
 *   从这里带出去，由**调用方**决定往哪写。
 *   provider 不是 custom 时这里是空串（内置那几家的地址是编译期常量，
 *   带出来只会误导人）。传 NULL 表示"我不管网址"。
 *
 * 顺带两条行为（2026-10-22 修）：文件里不写 provider/model 时
 * **不再留空**，而是沿用当前配置那一家 —— 空 provider 的条目在界面上
 * 显示成空名字，请求也发不出去。 */
bool keystore_import_from_credfile(char *out_base_url, size_t out_cap);

/* ★ 直接落一把（不经过 key.txt）。给"在机器上把 Key 敲进去"那条路用
 *   （引导页 / Key 编辑屏），因为 3DS 上没有粘贴板，收件箱那条路要求
 *   用户把卡拔下来插电脑 —— 走不通的时候得有一条纯设备内的路。
 *
 * 名字规则与 keystore_import_from_credfile **完全共用一份实现**
 * （provider-序号 + 重名递增）：两处各写一份的话，迟早出现
 * "从文件导入的叫 zhipu-1、手敲的叫 zhipu1"这种分裂。
 *
 * out_name 可为 NULL；非 NULL 时写回最终采用的名字 —— 调用方要用它
 * 去 keystore_set_active()（**激活不在这里做**：只有调用方知道
 * "用户这次到底想用哪一把"）。
 *
 * 空 Key / 仓库满 返回 false。 */
bool keystore_add(const char *provider, const char *model, const char *key,
                  char *out_name, size_t out_cap);

/* 仓库是否为空（UI 用来决定显示引导还是列表）。 */
bool keystore_empty(void);

/* 仓库是不是存在加密文件里（creds.enc 在 = 加密模式）。
 * UI 用它显示「已加密」字样 —— 让用户看得见这道保护确实生效了。 */
bool keystore_is_encrypted(void);
