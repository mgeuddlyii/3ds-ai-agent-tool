/*
 * net.h -- 把抓到的画面发给 AI，拿回译文
 *
 * 为什么这一步必然"只能真机验"：
 *   插件要自己开 soc 服务、自己走 TLS。这两件事在模拟器里都不成立
 *   （Azahar 根本不加载 .3gx）。所以这里的代码只能保证"逻辑自洽 + 编译过"，
 *   真正的握手 / 证书 / 内存，都要上机才知道。
 *
 * 失败时**一定写一句人话进 out**：面板上要显示得出"哪一步挂了"，
 * 而不是一个空白框 —— 插件崩溃时现场全黑，没有日志可看。
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* 把 BGR8 帧缓冲编码成 PNG 并 POST 给模型，译文写进 out（可含换行）。
 * 成功返回 true。 */
bool net_translate(const uint8_t *bgr, int w, int h, char *out, size_t cap);

/* 一次性初始化（soc + curl）。重复调用无副作用。 */
bool net_init(void);
