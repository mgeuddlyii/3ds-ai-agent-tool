/*
 * orch.c — 非攻略问题的统一路由（编排层 M1）
 *
 * 刻意保持薄：只做「按顺序问一遍，谁先接住算谁的」。真正的判据在
 * tools.c 和 chitchat.c 里。这里唯一值得测的就是**顺序**。
 */
#include <string.h>

#include "orch.h"
#include "tools.h"
#include "chitchat.h"

OrchKind orch_route(const char *q, char *out, int out_sz)
{
    ToolKind tk;
    ChatKind ck;

    if (!q || !out || out_sz <= 0) return ORCH_GUIDE;

    /* 1) 工具层优先：算式与日期都是无歧义的精确问题 */
    tk = tools_try(q, out, out_sz);
    if (tk != TOOL_NONE) return ORCH_TOOL;

    /* 2) 闲聊层：判不准会自己放行 */
    ck = chitchat_try(q, out, out_sz);
    if (ck != CHAT_NONE) return ORCH_CHITCHAT;

    /* 3) 都不是 -> 攻略检索 */
    return ORCH_GUIDE;
}
