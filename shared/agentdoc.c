/*
 * agentdoc.c -- AI 运行手册（AGENT.md）的装载
 *
 * 只做一件事：把《AIConsole 运行手册（模型专用）》拿到手，交给调用方拼进
 * 这一轮 API 请求的 system 消息。判断"手册在不在"的责任在调用方（sc_chat.c）。
 *
 * ── 两条来源，优先级有意义 ──
 *   1. **内嵌副本** agentdoc_txt[] —— 由 tools/build.py 直接从 AGENT.md
 *      生成 .s 编进程序本体。随程序走：装了就能用，不用往卡上拷，
 *      也不存在"拷了一份旧的"这种事。
 *   2. **卡上文件** sdmc:/3ds/aiconsole/AGENT.md —— 想临时改手册而不重编译时用。
 *
 * 内嵌在前不是因为"内嵌更权威"，是因为它是**确定的那一份**：
 * 卡上那份可能被别的会话、上一个测试趟次、用户手改过，谁也不知道现在读到的是几版的。
 * 只有"我明确想试一版新手册"时才该让卡上的遮住内嵌的。
 *
 * ── 为什么不用 fseek/ftell 先取大小 ──
 * 与其赌 seek 一定可用（newlib 在 sdmc 上一般可以，但没有真机趟次
 * 专门验过这个组合），不如就按上限一路读、读满即停。
 * 上限 16 KB 本来就是熔断开关，读循环不依赖文件长度，行为更可预测：
 * seek 失败时返回的值可能是垃圾尺寸，那样反而会漏读或误判"太大"。
 */

#include <stdio.h>
#include <string.h>

#include "agentdoc.h"
#include "uistr.h"          /* uistr_lang()：按界面语言挑中/英手册 */

#ifdef AGENTDOC_EMBEDDED
#include "agentdoc_md_bin.h"     /* build.py 生成：agentdoc_txt[] / agentdoc_txt_size */
#include "agentdoc_en_md_bin.h"  /* build.py 生成：agentdoc_en_txt[] / agentdoc_en_txt_size */
#endif

#define AGDOC_SRC_EMBEDDED "embedded"
#define AGDOC_SRC_SDMC     "sdmc"

/* ★ 为什么手册要跟界面语言走，而不是固定中文：
 *   模型说什么语言，很大程度由手册用什么语言写决定（手册是 system 消息的
 *   主体）。界面切成 English 却塞一份中文手册，模型会继续用中文回话 ——
 *   用户看到的是"界面英文、AI 说中文"，这不是 bug 报告里能一眼看出的现象。
 *
 * ★ 英文手册缺失时**回退到中文**，而不是"没有手册"：
 *   没有手册模型会乱编动作名（比说中文严重得多）。回退等于"语言不理想
 *   但行为仍然正确"，这是这里刻意选的方向。
 *
 * ★ 选择依据是**界面语言**（uistr_lang()），不是 config 里的回答语言。
 *   回答语言是"让模型用哪种语言说话"的软指令，界面语言才是"整个产品
 *   现在是哪一版"的判据。两者可以不同（比如英文界面 + 中文回答），
 *   那时手册仍该用英文版 —— 手册讲的是**规则**，不是**措辞**。 */
static const uint8_t *pick_doc(size_t *out_size)
{
#ifdef AGENTDOC_EMBEDDED
    if (uistr_lang() != 0 && agentdoc_en_txt_size > 0) {
        *out_size = agentdoc_en_txt_size;
        return agentdoc_en_txt;
    }
    *out_size = agentdoc_txt_size;
    return agentdoc_txt;
#else
    *out_size = 0;
    return NULL;
#endif
}

bool agentdoc_load(char *buf, size_t cap, size_t *len, const char **src)
{
    FILE  *f;
    size_t got;

    if (!buf || !len || cap < AGENTDOC_MAX)
        return false;

    *len = 0;
    if (src)
        *src = NULL;

#ifdef AGENTDOC_EMBEDDED
    /* 内嵌副本优先。size 是编译期常量，不读文件系统，失败不了 ——
     * 但它可能为 0（AGENT.md 不在仓库里时 build.py 生成的是空数组），
     * 那种情况下要**落到卡上那份**，不能拿 0 字节当"有手册"交上去。 */
    {
        size_t n;
        const uint8_t *doc = pick_doc(&n);

        if (doc && n > 0) {

        /* ★ 超限 = **拒绝**，不是削尾。
         *
         * 早先这里是"只削尾部，规则都在前半段"—— 那句话听起来很有道理，
         * 实际后果是：手册长到装不下时，**末尾那几节动作说明被静默丢掉**，
         * 而模型收到的是一份看起来完整的规则集。它于是以为"没提到的动作
         * 是不许的"，而不是"这一版手册没写到" —— 那比不给手册更糟。
         *
         * 界面那边（sc_chat.c）对"没有手册"的处理是照实说没有，
         * 不做兜底文本；这里是同一件事的另一半：**宁可这一轮没手册，
         * 也不给一份半的**。
         *
         * 真正的防呆在上游：tools/check_agentdoc.py 会比对
         * AGENT.md 与 AGENTDOC_MAX 并报红 —— 手册超限要在构建期就看见，
         * 而不是等到某一轮对话里模型突然变笨。 */
        if (n > cap - 1)
            return false;                /* 装不下 = 没有，不是半份 */

        memcpy(buf, doc, n);
        buf[n] = '\0';
        *len   = n;
        if (src)
            *src = AGDOC_SRC_EMBEDDED;
        return true;
        }
    }
#endif

    /* 卡上兜底：英文界面读 AGENT.en.md，中文界面读 AGENT.md。
     * 两者分开，避免"切成英文却读到中文卡上文件"。 */
    f = fopen(uistr_lang() != 0 ? AGENTDOC_PATH_EN : AGENTDOC_PATH, "rb");
    if (!f)
        return false;                 /* 没有手册就是没有，不猜 */

    got = 0;
    for (;;) {
        size_t chunk;

        if (got + 1 >= cap)
            break;                    /* 留一个字节给结尾'\0' */

        chunk = fread(buf + got, 1, cap - 1 - got, f);
        if (chunk == 0)
            break;                    /* 到底了，或读错了 */

        got += chunk;
    }

    fclose(f);

    if (got == 0) {
        buf[0] = '\0';
        return false;                 /* 空文件不当"有手册" */
    }

    buf[got] = '\0';
    *len     = got;
    if (src)
        *src = AGDOC_SRC_SDMC;
    return true;
}
