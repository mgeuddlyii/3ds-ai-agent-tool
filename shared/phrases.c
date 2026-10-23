/*
 * phrases.c -- 见 phrases.h
 */

#include "phrases.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#define PHRASES_PATH "sdmc:/3ds/aiconsole/phrases.txt"

/* 没配置过时用的四条。它们要能覆盖"第一次打开就想试试"的几种典型用途：
 * 自我介绍、翻译、解释、写作 —— 而不是四句同义的话。
 *
 * 最后一条是**动作演示**：它请助手写一个文件。装了这个应用的人第一次
 * 打开、按两下 SELECT、按 A，就能亲眼看到"AI 想创建一个文件"的确认屏，
 * 以及点"允许"之后文件真的出现在卡上。整个 agent 能力的第一步
 * （见 shared/actions.h）由此变成**用户点得到的东西**，而不是文档里的一句话。 */
static const char *const DEFAULTS[] = {
    "你好！请用两三句话介绍一下你自己。",
    "把下面这句话翻译成英文：",
    "用小学生能听懂的话解释一下：",
    "帮我写一条朋友圈，主题是：",
    "试着创建一个文件，告诉我 agent 能力是通的。",
};
#define DEFAULTS_N ((int)(sizeof DEFAULTS / sizeof DEFAULTS[0]))

static char g_list[PHRASE_MAX_COUNT][PHRASE_MAX_LEN + 1];
static int  g_n;

const char *phrases_path(void) { return PHRASES_PATH; }

static void load_defaults(void)
{
    g_n = 0;
    for (int i = 0; i < DEFAULTS_N && i < PHRASE_MAX_COUNT; ++i) {
        snprintf(g_list[i], sizeof g_list[i], "%s", DEFAULTS[i]);
        ++g_n;
    }
}

int phrases_load(void)
{
    FILE *f;

    g_n = 0;

    f = fopen(PHRASES_PATH, "rb");
    if (f) {
        char line[PHRASE_MAX_LEN + 16];

        while (g_n < PHRASE_MAX_COUNT && fgets(line, (int)sizeof line, f)) {
            size_t n = strlen(line);

            /* 去掉行尾的 CR/LF —— 用户可能在 Windows 上编辑这个文件 */
            while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r'))
                line[--n] = '\0';

            if (n == 0 || line[0] == '#')
                continue;               /* 空行与注释 */

            snprintf(g_list[g_n], sizeof g_list[g_n], "%s", line);
            ++g_n;
        }
        fclose(f);
    }

    /* 一条都没有 → 用默认。注意这不是"失败"：文件不在是最正常的情况，
     * 用户压根没配过。 */
    if (g_n == 0)
        load_defaults();

    return g_n;
}

int phrases_count(void) { return g_n; }

const char *phrases_at(int i)
{
    if (i < 0 || i >= g_n)
        return "";
    return g_list[i];
}

bool phrases_save(void)
{
    FILE *f;

    /* 目录一般已经有了（config.json 就住在同一个目录），
     * 但首启时可能真的还没有 —— 建一下不亏。 */
    mkdir("sdmc:/3ds", 0777);
    mkdir("sdmc:/3ds/aiconsole", 0777);

    f = fopen(PHRASES_PATH, "wb");
    if (!f)
        return false;

    fputs("# 3DS AI Console 快捷短语：一行一条，最多 8 条\n", f);
    fputs("# 在电脑上直接编辑这个文件，或者在对话屏按 R 改当前那条\n", f);

    for (int i = 0; i < g_n; ++i) {
        fputs(g_list[i], f);
        fputc('\n', f);
    }

    fclose(f);
    return true;
}

bool phrases_set(int i, const char *text)
{
    if (i < 0 || i >= PHRASE_MAX_COUNT)
        return false;

    if (!text || !*text) {
        /* 空 = 删掉这条，后面的往前顶。不重排的话会留下一个空条目，
         * 按 SELECT 轮换时会"卡"一下（填进去什么都没有）。 */
        if (i < g_n) {
            for (int k = i; k + 1 < g_n; ++k)
                memcpy(g_list[k], g_list[k + 1], sizeof g_list[k]);
            --g_n;
        }
        if (g_n == 0)
            load_defaults();            /* 删光了就回到默认，别留一个空的轮换列表 */
        return phrases_save();
    }

    if (i >= g_n)
        return false;

    snprintf(g_list[i], sizeof g_list[i], "%s", text);
    return phrases_save();
}
