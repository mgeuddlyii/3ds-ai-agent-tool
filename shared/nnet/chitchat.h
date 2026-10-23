/*
 * chitchat.h — 闲聊层（编排层 M5）
 *
 * 只做「整句就是寒暄」这一类。判不准就返回 CHAT_NONE。
 *
 * ⚠️ 红线：闲聊绝不能抢攻略问题。
 * 「我不会玩怎么办」里有「怎么办」，但那是攻略问题；
 * 「谢谢你」才是闲聊。区别不在关键词，在**去掉关键词之后还剩什么**——
 * 剩下实质内容就放行。match_entry() 就是按这个判的。
 */
#ifndef CHITCHAT_H
#define CHITCHAT_H

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    CHAT_NONE = 0,   /* 不是闲聊，交给别处 */
    CHAT_GREET,      /* 你好 */
    CHAT_THANKS,     /* 谢谢 */
    CHAT_BYE,        /* 再见 */
    CHAT_ACK,        /* 好的/嗯/哦 */
    CHAT_LAUGH,      /* 哈哈 */
    CHAT_SORRY,      /* 抱歉 */
    CHAT_ASK_CAP     /* 「你能做什么」这类元问题 */
} ChatKind;

/*
 * 试着回应一句闲聊，结果写进 out。返回 CHAT_NONE 表示不归我管。
 * 短句优先：状态行只有 384px，长解释走对话流。
 */
ChatKind chitchat_try(const char *q, char *out, int out_sz);

#ifdef __cplusplus
}
#endif
#endif /* CHITCHAT_H */
