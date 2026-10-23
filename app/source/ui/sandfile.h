/*
 * sandfile.h -- 沙箱目录的只读访问
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>

/* 列一个沙箱子目录。sub 传 NULL 或 "..." 表示根目录。
 * 返回 false 时 out 里是**一句人话**（为什么读不到），不是空串。 */
bool sandfile_list(const char *sub, char *out, size_t cap);

/* 读沙箱根目录下 name 指向的文本文件，从 from 字节开始。
 * 只会切在 UTF-8 字符边界上（宁可少给一个字符，也不给半截汉字）。
 * 返回 false 时 out 里是一句人话。 */
bool sandfile_read(const char *name, long from, char *out, size_t cap);
