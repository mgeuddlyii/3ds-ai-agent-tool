/*
 * chitchat.c — 闲聊层（编排层 M5）
 *
 * 判据纪律：这张表每一项都要能被「带实质内容的问句」测红。
 * 孪生判据 chitchat_twin.py 里对每一类都配了反例，
 * 比如「谢谢你，火焰纹章怎么打」必须**不**被当成闲聊。
 */
#include <string.h>
#include <stdio.h>

#include "chitchat.h"

#define MAX_CHAT_LEN 16   /* 整句长度上限，超了基本是带内容的问题 */

typedef struct {
    const char *key;   /* 关键词（UTF-8 子串） */
    ChatKind     kind;
    const char *rep;
} ChatEntry;

/* 刻意短、刻意朴素。不会写俏皮话 —— 小模型也写不出来，不如老实。 */
static const ChatEntry g_chat[] = {
    { "你好",     CHAT_GREET,  "你好。我能帮你查 3DS 游戏攻略。" },
    { "您好",     CHAT_GREET,  "你好。我能帮你查 3DS 游戏攻略。" },
    { "在吗",     CHAT_GREET,  "在的。问游戏攻略就行。" },
    { "早上好",   CHAT_GREET,  "早上好。今天想查哪款游戏？" },
    { "晚上好",   CHAT_GREET,  "晚上好。今天想查哪款游戏？" },
    { "谢谢",     CHAT_THANKS, "不客气。" },
    { "多谢",     CHAT_THANKS, "不客气。" },
    { "感谢",     CHAT_THANKS, "不客气。" },
    { "再见",     CHAT_BYE,    "再见。" },
    { "拜拜",     CHAT_BYE,    "拜拜。" },
    { "走了",     CHAT_BYE,    "再见。" },
    { "好的",     CHAT_ACK,    "好。" },
    { "知道了",   CHAT_ACK,    "好。" },
    { "明白",     CHAT_ACK,    "好。" },
    { "哈哈",     CHAT_LAUGH,  "哈哈。" },
    { "嘿嘿",     CHAT_LAUGH,  "嘿嘿。" },
    { "抱歉",     CHAT_SORRY,  "没事。" },
    { "不好意思", CHAT_SORRY,  "没事。" },
    { "你能做什么", CHAT_ASK_CAP, "我能查 3DS 游戏攻略：流程、boss、道具、职业、结局都能问。" },
    { "你会什么",   CHAT_ASK_CAP, "我能查 3DS 游戏攻略：流程、boss、道具、职业、结局都能问。" },
    { "能干什么",   CHAT_ASK_CAP, "我能查 3DS 游戏攻略：流程、boss、道具、职业、结局都能问。" },
    { "支持哪些游戏", CHAT_ASK_CAP, "支持的游戏在下面的选择条里，点一款就能问。" },
};
#define NCHAT ((int)(sizeof g_chat / sizeof g_chat[0]))

/*
 * 语气词/标点：去掉关键词之后**只剩这些**才算闲聊。
 * 这份表就是「抢答防护栏」——「谢谢你，火焰纹章怎么打」去掉「谢谢」
 * 剩下「你，火焰纹章怎么打」，不是纯语气词，于是放行给攻略检索。
 */
static const char *g_particle[] = {
    "啊","呀","哇","吧","呢","吗","哦","噢","嗯","欸","哈","了","啦","咯",
    "，","。","！","？","、","~","！","?","!",".",
    "请","请问","麻烦","你","我"," ","\t"
};
#define NPART ((int)(sizeof g_particle / sizeof g_particle[0]))

/* 把 out 里所有 particle 依次删掉；返回剩下的长度 */
static int strip_particles(char *out)
{
    int changed = 1;
    while (changed) {
        int i;
        changed = 0;
        for (i = 0; i < NPART; i++) {
            int L = (int)strlen(g_particle[i]);
            char *p;
            if (L == 0) continue;
            p = strstr(out, g_particle[i]);
            if (p) {
                /* 只在开头找：整句剥完应该只剩语气词，不该有中间内容 */
                if (p == out) {
                    memmove(out, out + L, strlen(out + L) + 1);
                    changed = 1;
                    break;
                }
            }
        }
    }
    return (int)strlen(out);
}

ChatKind chitchat_try(const char *q, char *out, int out_sz)
{
    char buf[128];
    int i, L, j, found = 0, idx = -1;
    int earliest = 0;      /* 命中的关键词位置，越靠前越可信 */

    if (!q || !out || out_sz <= 0) return CHAT_NONE;
    out[0] = '\0';

    L = (int)strlen(q);
    if (L == 0 || L >= (int)sizeof buf) return CHAT_NONE;

    /* 太长的一律不接管：带实质内容的问题都在这一档后面 */
    if (L > MAX_CHAT_LEN * 3) return CHAT_NONE;

    /* 拷贝一份并把整串两端夹到最短 */
    memcpy(buf, q, (size_t)L);
    buf[L] = '\0';

    /* 找关键词：取**最靠前**的那个命中 */
    for (i = 0; i < NCHAT; i++) {
        const char *p = strstr(buf, g_chat[i].key);
        if (!p) continue;
        if (!found || (int)(p - buf) < earliest) {
            earliest = (int)(p - buf);
            idx = i;
            found = 1;
        }
    }
    if (!found) return CHAT_NONE;

    /* 整句去掉关键词后，只允许剩语气词 */
    {
        int klen = (int)strlen(g_chat[idx].key);
        char rest[128];
        int rlen = 0;
        /* 关键词之前的部分 + 之后的部分 */
        for (j = 0; j < earliest; j++) rest[rlen++] = buf[j];
        for (j = earliest + klen; j < (int)strlen(buf); j++) rest[rlen++] = buf[j];
        rest[rlen] = '\0';
        if (strip_particles(rest) > 0) return CHAT_NONE;   /* 还有实质内容 -> 放行 */
    }

    snprintf(out, (size_t)out_sz, "%s", g_chat[idx].rep);
    return g_chat[idx].kind;
}
