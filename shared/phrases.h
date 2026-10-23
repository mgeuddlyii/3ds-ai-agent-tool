/*
 * phrases.h -- 快捷短语
 *
 * 存储是一行一条的纯文本（# 开头当注释），放在卡上：
 *   sdmc:/3ds/aiconsole/phrases.txt
 * 用户可以插到电脑上直接编辑 —— 这比在 3DS 上拿软键盘敲八条句子现实得多。
 *
 * 为什么这个功能不是"可有可无的小玩意"：它在对话屏里是**不用键盘也能
 * 把消息发出去**的唯一通道（没接外设、或者在床上够不着屏幕的时候就得靠它）。
 * 把它编译期写死成四句话，等于把这唯一的通道钉死在我拍脑袋想的四句话上。
 *
 * 上限 8 条 × 100 字节：够用，且能整份塞进一个静态数组里。
 */

#pragma once

#include <stdbool.h>

#define PHRASE_MAX_LEN   100
#define PHRASE_MAX_COUNT 8

const char *phrases_path(void);

/* 读盘。文件不在 / 读不出内容 → 用内置的默认几条（含一条动作演示），
 * 不算失败。返回当前可用条数（至少 1）。 */
int phrases_load(void);

int         phrases_count(void);
const char *phrases_at(int i);          /* 越界返回 "" */

/* 改一条并立刻落盘。text 为空表示"删掉这条"（后面的往前顶）。 */
bool phrases_set(int i, const char *text);

/* 整份写回。给"用户直接编辑过文件"那种情况兜底用的，
 * phrases_set 内部已经调它了。 */
bool phrases_save(void);
