/*
 * ime.h -- 拼音输入法引擎（不含界面）
 *
 * ── 为什么 3DS 需要自己写这个 ──
 * 系统软键盘（swkbd）**没有拼音输入法**，日版/美版固件都没有。
 * 所以"在 3DS 上打中文"不是调一个开关，而是要有引擎 + 键盘 + 候选。
 * 这一层只管**把字母变成候选**，界面在 sc_dock.c 里画。
 *
 * ── 数据从哪来 ──
 * `app/data/pinyin_tbl.bin`（tools/gen_pinyin_table.py 生成，内嵌进程序）：
 * GB2312 一级 3755 个常用字，按项目自己的攻略语料算字频排序。
 * 所以候选第一位通常就是用户想打的字（ni → 你，hao → 好）。
 *
 * ── 分词策略（刻意简单）──
 * 输入串按**最长音节贪婪切分**（"nihao" → ni + hao）。
 * 候选 = 前面各音节的首选字 + 最后一个音节的全部候选字：
 *     nihao → 你好 / 你耗 / 你号 / 你豪 …
 * 这样单音节和连打都能用，且**不需要词组表**。
 * 代价是没有真正的词组联想（不会从 nihao 直接给出"你好"当成一个词）——
 * 但那需要一个 GB 级词库，而 3DS 的卡上空间和内存都不该花在这上面。
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>

#define IME_INPUT_MAX 12      /* 字母缓冲：够打 4~6 个音节 */
#define IME_SEG_MAX    4      /* 一次最多切几个音节 */

/* 表在不在。不在时所有操作都安全地返回空。 */
bool ime_ready(void);

void ime_reset(void);

/* 追加一个字母（a-z，大小写都接受，统一转小写）。
 * 返回 false = 缓冲已满或不是字母。 */
bool ime_push(char c);

void ime_backspace(void);

/* 当前输入的字母串（永远是 NUL 结尾的 ASCII）。 */
const char *ime_input(void);

/* 候选个数。输入为空或无匹配音节时返回 0。 */
int ime_cand_count(void);

/* 第 i 个候选的 UTF-8 文本。指针指向内部静态缓冲，
 * **下次调用即失效** —— 要用就立刻拷走。 */
const char *ime_cand(int i);

/* 取第 i 个候选并把它写进 out，然后清空输入。
 * 返回 false = 下标越界。 */
bool ime_commit(int i, char *out, size_t cap);
