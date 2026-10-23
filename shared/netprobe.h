/*
 * netprobe.h -- 网络连通性探测
 *
 * 存在的理由很朴素：**在做 M4（真实 API 调用）之前，必须先知道模拟器
 * 到底能不能联网。** 3DS 的 httpc 是个系统服务，Azahar/Citra 对它的支持
 * 程度并不确定 —— 如果 httpcInit 在这里直接失败，那在线相关的一切
 * 都只能等实机再验，而现在就把这件事定性，比写完整个对话模块再发现强。
 *
 * 探测结果同时给两边看：返回值给代码，detail 写一句人话给 UI 和日志。
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>

/* 成功（拿到 200）返回 true。http_code 和 detail 都可以传 NULL。
 *
 * 注意：这个函数**会阻塞**，所以别放在启动路径上 ——
 * 万一模拟器卡在网络等待，整个 app 就起不来了。手动触发（按键）才安全。 */
bool net_probe(const char *url, int *http_code, char *detail, size_t cap);

/* 探测用的默认目标。选它是因为它只返回一个很小的纯文本，
 * 而且不依赖任何厂商 —— 探测的是"能不能出去"，不是"某家通不通"。 */
const char *net_probe_default_url(void);
