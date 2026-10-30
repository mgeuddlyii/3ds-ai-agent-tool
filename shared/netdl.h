/*
 * netdl.h -- 把 URL 上的一个文件下到磁盘（动作层用）
 *
 * 为什么单独一个模块，而不是让 actions.c 直接调 curl：
 *   下载这件事有三条**必须一起满足**的约束，散落在调用点就一定会漏一条：
 *     1. 先写 .partial，成功后才 rename —— 中途断了不能留下"看起来完整"的文件
 *     2. 边下边算 sha256，并在写盘失败时**中止**（否则磁盘满会被当成下载成功）
 *     3. 有字节上限 —— 否则一个手滑的 URL 能把卡写满
 *   这三条与 ota.c 里那份下载器是同一套道理，但那边的目录/sink 是 OTA 专用的
 *   （进度回调绑在"更新"这个概念上）。这里只保留能通用的部分，不复制回调。
 *
 * ⚠️ 不做域名白名单：用户让 AI 下什么由用户负责（这是通用工具的边界）。
 *    能控的只有"落到哪个目录"和"多大规模"，这两条由本模块 + 调用方守。
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>

typedef struct {
    bool   ok;
    size_t bytes;
    char   sha256[65];      /* 实际下载内容的摘要（十六进制小写） */
    char   msg[160];        /* 给人看的一句话（失败原因也在这里） */
} NetDlResult;

/* 下载 url 到 dir/name。
 *   max_bytes  超过就中止（部分文件会被删掉）
 *   sha_expect 非空则逐字节比对摘要，不符视为失败
 * 成功返回 true；无论成败 out 都被填好。 */
bool netdl_fetch(const char *url, const char *dir, const char *name,
                 size_t max_bytes, const char *sha_expect, NetDlResult *out);
