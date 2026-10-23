/*
 * util.h -- 几个到处都要用的小东西
 *
 * 它们原本各自长在 chatlog.c 里（mkdirs / stamp_now）。等第二个模块
 * （actions.c）也要用的时候就该搬出来了 —— 各留一份的结局是：
 * 哪天修好了其中一个的边界情况，另一个还是旧的，而两边看起来"一样"。
 */

#pragma once

#include <stddef.h>

/* 逐级建目录，路径形如 "sdmc:/a/b/c"。已存在算成功。
 * 不做错误上报：调用方紧接着的 fopen 会报，那时候才知道到底行不行。 */
void util_mkdirs(const char *path);

/* 本地时间戳 "YYYYMMDD_HHMM"，给文件名用。
 * 3DS 的 RTC 给的是 UTC，这里按**东八区**换算（理由见 util.c）。 */
void util_stamp(char *out, size_t cap);

/* 带秒的版本 "YYYY-MM-DD HH:MM:SS"，给审计日志用 ——
 * 只精确到分钟的话，同一分钟内的"允许/拒绝"两条会分不清先后。 */
void util_stamp_full(char *out, size_t cap);
