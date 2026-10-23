/*
 * agentdoc.h -- AI 运行手册（AGENT.md）的装载
 *
 * ── 这个模块是干什么的 ──
 * 把随程序分发的《AIConsole 运行手册（模型专用）》读进来，
 * 让这一轮 API 请求的 system 消息里带上"这台机器到底能让我干什么、动作怎么写"。
 *
 * ── 为什么单独拆一个模块 ──
 * 手册是**内容**，不是配置。它全篇约 8 KB，而 config.json 里的
 * system_prompt 只有 CONFIG_MAX_PROMPT(240) 字节 —— 塞不进去，
 * 也不该塞：手册要能单独换、要能审计、不该混进"人写的那句人设"里。
 *
 * ── 取值优先级（顺序有意义）──
 *   1. **内嵌副本**（agentdoc_txt[]，由 build.py 从 AGENT.md 编进程序）——
 *      装了程序就有，不依赖用户往卡上拷文件。
 *   2. 卡上 sdmc:/3ds/aiconsole/AGENT.md —— 想不重编译就换一版手册时用。
 *      ⚠️ 内嵌是"确定的那一份"，卡上那份可能已经过期或被别的会话改过，
 *      所以它**不覆盖**内嵌副本，只在内嵌缺失（size==0）时才顶上。
 *   3. 都没有 → 返回 false —— **不做任何兜底文本**。
 *      宁可这一轮没手册（AI 会少一份约束），也不要拿一份过期的、
 *      或者程序自己编的假手册骗模型 —— 那等于给模型一份错误地图。
 *
 * ── 读的是文件，不是资源 ──
 * 3DS 上 sdmc 可写可读，直接 fopen 最省事（memory 也小）。
 * 若将来要打进 ROM 当内嵌资源，改 agentdoc_load 的实现即可，
 * 调用方不用动。
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>

/* 手册在卡上的位置。与 sc_chat 拼接时用的也是这个串。 */
#define AGENTDOC_PATH  "sdmc:/3ds/aiconsole/AGENT.md"
/* 英文手册的卡上兜底路径。界面切成 English 时才读这一份。
 * 两份**分开存放**，不共用同一个文件名 —— 共用会出现
 * "切成英文却读到中文卡上文件"这种没人能一眼看出的错。 */
#define AGENTDOC_PATH_EN "sdmc:/3ds/aiconsole/AGENT.en.md"

/* 上限：AGENT.md 会随动作增加而变长（每加一个动作就多一节），
 * 所以 16 KB 已经不够用了 —— 2026-10-05 加安装类动作时手册到 16977 字节，
 * 超出后**装载时被静默削尾**，模型收到的半份手册里没有那两个新动作，
 * 而"手册与代码对账"之外的一切检查都显示通过。
 *
 * 因此定 24 KB：新动作还有约 6 KB 余量，够再扩十几个。
 * 代价是 24 KB 静态缓冲（g_agdoc + g_sysprompt 各一份），
 * 3DS 的内存充裕，这笔交换很划算。
 *
 * ★ 上限同时也是**熔断**，而熔断的语义是"**宁可不给，不给半份**"：
 * 超限时 agentdoc_load 会返回 false，界面上一句"没有手册"，
 * 而不是给模型一份被削了尾的规则集 —— 半份手册会让模型以为
 * "规则就这么多，没提到就是不许"，那比没有手册更糟。 */
#define AGENTDOC_MAX   (24 * 1024)

#ifdef __cplusplus
extern "C" {
#endif

/* 把手册读进 buf。
 *
 * 返回 true 且 *len 为实际字节数（不含结尾'\0'）时，buf 里是可直接发出去的文本；
 * 返回 false 表示这一轮**没有手册**，调用方应当跳过拼接，不要伪造。
 *
 * src 可为空；非空时回填这一份手册是**从哪来的**（"embedded" / "sdmc"）。
 * 这个出参不是装饰：探针要报"实际读出的是哪一份"，
 * 报"读到了 N 字节"看不出读的是内嵌的还是卡上的旧版。
 *
 * buf 必须由调用方提供且 cap >= AGENTDOC_MAX —— 手册是常量，
 * 让调用方自己开缓冲（而不是这里 malloc）是为了不把分配失败变成
 * "带着半份手册继续跑"这种静默降级。 */
bool agentdoc_load(char *buf, size_t cap, size_t *len, const char **src);

#ifdef __cplusplus
}
#endif
