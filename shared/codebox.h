/*
 * codebox.h -- 把模型回复里的代码块**落成文件**
 *
 * 为什么需要它：
 *   模型经常整段贴代码（```c ... ```）。在 3DS 上这段代码只能滚动着看，
 *   既不能存下来跑、也不能拷出去 —— 用户原话是「现在的版本还不能直接让我点开测试」。
 *   所以回复一落地，就把每个代码块写成沙箱里的独立文件：
 *       sdmc:/3ds/aiconsole/out/code001.c
 *   之后在对话页滑轨的「文件」格里就能直接列出、打开看。
 *
 * 三条边界：
 *   · 只写 actions_dir()（沙箱白名单里的 out 目录），**不碰别处**；
 *   · 单个文件超过 CODEBOX_MAX_BODY 就截断，并在末尾留一行说明 ——
 *     静默截断会让人以为代码本来就短；
 *   · 最多取 CODEBOX_MAX_FILES 个块，多的不写（一次回复贴十个文件也没意义）。
 */

#pragma once

#include <stddef.h>

/* 扫描 text 里的 ``` 围栏代码块，逐个写文件。
 * 返回写了几个；summary 里是给人看的一句话（写进状态行/日志）。 */
int codebox_extract(const char *text, char *summary, size_t cap);
