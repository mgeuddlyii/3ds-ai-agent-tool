/*
 * chatapi.c -- 见 chatapi.h
 *
 * 用 libctru 的 httpc 服务。几处刻意的取舍：
 *
 *  1. **不禁用证书校验**（netprobe.c 里禁了，那是探网络）。
 *     这里是真要把 Key 和对话内容发出去，关掉校验等于把 HTTPS 降级成 HTTP。
 *     代价是 3DS 那套老根证书可能不认某些站点 —— 真遇到就明确报错，
 *     让用户知道是证书问题，而不是偷偷关掉校验装作没事。
 *
 *  2. 用户输入的文本**必须转义**再塞进 JSON。用户会打引号、换行、
 *     反斜杠，不转义就会拼出一个服务端看不懂的请求，而错误看起来
 *     像"服务器坏了"，排查方向完全跑偏。
 *
 *  3. 收响应循环里带"没进展就退出"的判断。httpcReceiveData 不保证一次读完，
 *     但也不能无脑 while(true) —— 网络卡住会把整个 app 冻死。
 *
 * M6 之后请求构造（build_request）与状态码翻译（http_word）两路共用，
 * 差别只在开不开 stream 与读循环的步长 —— 这是唯一没有分叉的地方。
 */

#include "chatapi.h"

#include "doubao.h"
#include "minijson.h"
#include "provider.h"

#include <3ds.h>
#include <stdio.h>
#include <string.h>

/* ★★ 请求体容量 —— 这里踩过一个**致命**坑（2026-10-22 真机报"格式问题"）。
 *
 * 原来是 4096。而系统手册 AGENT.md 就有 21689 字节（英文版 24041）。
 * 于是 build_prefix 写 system 消息时 snprintf 直接截断，末尾的
 * `"}]...}` 永远写不进去 —— 发出去的是**半截 JSON**，
 * 服务端只能回 400「格式错误」，而界面上看不出是本地截断，
 * 看起来像"网络/服务商不认我的请求"。
 *
 * 现在按最坏情况给：手册 24KB + 8 条历史（每条最多 1KB，转义最坏 6× 但要留余量）
 * + 用户输入 + 结构开销 → 40KB 够。3DS 有 64MB 内存，一个 40KB 静态缓冲区
 * 不算负担；而**发不出消息**是致命的。 */
#define REQ_BUF  (40 * 1024)

/* 非流式响应缓冲。8KB 对语音/长回复够用了 */
#define RESP_BUF 8192

/* 流式读取步长 —— 这个数字是流式的**命门**，不是随便拍的。
 *
 * httpcReceiveData 的返回条件是「缓冲里积够 size+1 字节」或者「传输结束」，
 * 而它每次拷给我们的量**也正好是 size**（ReceiveBody 里 to_copy = min(size, available)）。
 * 也就是说 size 同时决定了"等多少才回来"和"一次拿多少"，两者绑死。
 *
 * 于是 size 太大就不是流式了：
 *   · size=511 → 要等 512 字节才返回。一个 SSE token 才几个字节，
 *     短回复（<512B，约 350 个汉字）会**整段一次性**上屏，流式白做。
 *   · size=63  → 等到 64 字节就返回。SSE 每个事件通常 100~200 字节
 *     （含 JSON 壳），所以基本是"对面写一块、我们上一块"，
 *     人眼看起来就是逐句冒出来；token 密集时甚至一个 token 一块。
 * 再小（比如 1）也流式，但每次只拿 1 字节，IPC 次数暴涨，反而更慢。
 * 64 是"够流式"与"别把 CPU 烧在 IPC 上"之间的折中。 */
#define STREAM_CHUNK 64

/* SSE 行解析器。绝大多数字段只有几十字节，但单个 token 的 JSON 偶尔会带
 * 长内容（代码块/一次性吐出整段），给 1KB 兜底；超了就丢掉这一行 ——
 * 丢一行是显示瑕疵，缓冲区溢出是崩溃。 */
#define SSE_LINE_MAX 1024

/* 请求体放静态区：4KB 放在栈上，在 Azahar 给 homebrew 的
 * 小主栈里是很大的占用 —— 曾经和 dtoa 一起把栈底打穿（见 minijson.c）。 */
static char req_body[REQ_BUF];

/* build_request 装不下系统提示时置真（它是 void，只能走标志）。
 * 调用方在发请求**之前**检查，别把半截 JSON 发出去。 */
static bool g_build_overflow;
static int  g_build_overflow_sys;

/* 上下文的两道上界，缺一不可：
 *   CTX_MAX_TURNS —— 条数上限。多轮是有成本的：每多带一条，请求体就大一截，
 *                    服务端也按 token 收钱，而 3DS 上用户往往只是想接着上一句问。
 *                    8 条够覆盖最近三四轮（长回复会被切成多块，故一条 ≠ 一轮）。
 *   预算          —— 字节上限。4KB 装不下无限长的历史，装不下就丢最早的。 */
#define CTX_MAX_TURNS 8
#define REQ_BUDGET    (REQ_BUF - 96)   /* 余量留给收尾的 ],"stream":true} */

/* 上一条真实请求的记账。给对话页的环当"实际值" —— 环是预估，
 * 这两个是事实。演示/豆包不走 build_request，永远停在 0 / false。 */
static int  g_last_req_len = 0;
static bool g_last_capped  = false;

/* ---- JSON 字符串转义（增量版）----
 * 把 src 转义后**追加**到 dst+o，返回新的写入位置。
 *
 * 做成"追加"而不是"写进一块独立缓冲"，是因为多轮上下文要一条接一条地拼。
 * 老写法每条都得先转义进一个固定缓冲再拷贝过去，等于每条多走一趟、
 * 还要额外养一块 2KB 的静态区。现在直接落进 req_body，一步到位。
 *
 * 只处理会让 JSON 不合法的字符。非 ASCII 一律原样输出 —— UTF-8 的字节
 * 都 ≥ 0x80，不会被误伤，而转成 \uXXXX 反而会把人话变成乱码。 */
static size_t escape_into(char *dst, size_t cap, size_t o, const char *src)
{
    for (const char *s = src; *s && o + 7 < cap; ++s) {
        unsigned char ch = (unsigned char)*s;
        switch (ch) {
            case '"':  dst[o++] = '\\'; dst[o++] = '"';  break;
            case '\\': dst[o++] = '\\'; dst[o++] = '\\'; break;
            case '\n': dst[o++] = '\\'; dst[o++] = 'n';  break;
            case '\r': dst[o++] = '\\'; dst[o++] = 'r';  break;
            case '\t': dst[o++] = '\\'; dst[o++] = 't';  break;
            default:
                if (ch < 0x20)
                    o += (size_t)snprintf(dst + o, cap - o, "\\u%04x", ch);
                else
                    dst[o++] = (char)ch;
        }
    }
    dst[o < cap ? o : cap - 1] = '\0';
    return o;
}

/* ---- 请求体 ----
 * 两路（流式/非流式）唯一的差别就是这个 "stream":true/false。
 * 曾经这里硬编码 false、而主界面显示「流式开」—— 屏幕上写的和实际发的
 * 不是一件事，最坑人。现在由调用方把 stream 传进来，两边必然一致。
 *
 * 消息顺序：system（人设）→ 历史（旧→新）→ 本次这句。
 * 顺序反了模型会把因果搞反（把你的新问题当成它自己以前说过的）。
 *
 * 保留优先级：user_text > system > 最近的历史 > 更早的历史。
 * 人设排在历史前面，因为它定义了"这个助手是谁"，
 * 把它挤掉换来多一轮旧对话得不偿失。 */
/* 思考强度 → 请求体片段。
 *
 * 返回一段**可直接接在 "stream":true 后面**的字符串（带前导逗号），
 * 或空串表示"什么都不加"。
 *
 * 四档映射（每一档都对着官方文档写，出处见 provider.h 的 ProviderThink）：
 *   档位   reasoning_effort    thinking.type    enable_thinking
 *   关     见下               disabled         false
 *   低     low                 enabled          true
 *   中     medium              enabled          true
 *   高     high                enabled          true
 *
 * ⚠️ **"关"档不能一律省略**。DeepSeek 官方写明「思考模式默认打开，effort 默认
 *    high」—— 不显式关掉的话，用户选了"关"却还在烧 token，而且界面上看不出
 *    来。所以按各家写法显式关闭。
 * ⚠️ 反过来，PROVIDER_THINK_EFFORT（通用 OpenAI 兼容）关档时**故意什么都不发**：
 *    那类接口背后是谁我们不知道，"none" 这种较新的取值不一定认，
 *    发过去可能直接 400。省略是所有人都接受的写法。
 */
static const char *thinking_json(const ProviderDef *p, int level)
{
    static const char *const EFF[4] = { NULL, "low", "medium", "high" };
    static char buf[128];
    const char *eff;

    /* UNKNOWN（未查证）也**一个参数都不发**：猜错就是 400，而用户看不出
     * 是自己多点了那个旋钮造成的。表里既然没查到，就别赌。 */
    if (!p || p->think == PROVIDER_THINK_NONE || p->think == PROVIDER_THINK_UNKNOWN)
        return "";

    if (level <= 0) {
        switch (p->think) {
        case PROVIDER_THINK_TYPE_EFFORT:
            return ",\"thinking\":{\"type\":\"disabled\"}";
        case PROVIDER_THINK_ENABLE_EFFORT:
            return ",\"enable_thinking\":false";
        default:                       /* PROVIDER_THINK_EFFORT：省略最安全 */
            return "";
        }
    }

    eff = EFF[level > 3 ? 3 : level];
    switch (p->think) {
    case PROVIDER_THINK_EFFORT:
        snprintf(buf, sizeof buf, ",\"reasoning_effort\":\"%s\"", eff);
        return buf;
    case PROVIDER_THINK_TYPE_EFFORT:
        snprintf(buf, sizeof buf,
                 ",\"thinking\":{\"type\":\"enabled\"}"
                 ",\"reasoning_effort\":\"%s\"", eff);
        return buf;
    case PROVIDER_THINK_ENABLE_EFFORT:
        snprintf(buf, sizeof buf,
                 ",\"enable_thinking\":true,\"reasoning_effort\":\"%s\"", eff);
        return buf;
    default:
        return "";
    }
}

/* ---- 请求体前缀：{"model":".."[,"max_tokens":N],"messages":[ + system 消息 ----
 *
 * ★ build_request 与 chat_ctx_estimate（对话页那个环）**共用**这一个函数。
 *   为什么必须共用：环要回答"这次会带多少上下文、离上限还有多远"，
 *   判据必须和真正发出去的请求同一个口径。复制一份公式的后果是
 *   预估绿、实际红（或反过来），而这种偏差用户看不出来 ——
 *   正是"永远绿的守卫比没有更坏"。
 *
 * 返回写入字节数（不含结尾 '\0'）。cap 不够时按 cap-1 截断，
 * 与调用方各自的安全写法一致（谁都不许写越界）。 */
static size_t build_prefix(char *dst, size_t cap, const ProviderDef *p,
                           const char *model, int max_tokens, const char *sys)
{
    const char *mdl = (model && *model) ? model : p->default_model;
    size_t o = (size_t)snprintf(dst, cap, "{\"model\":\"%s\"", mdl);
    if (o >= cap)
        o = cap - 1;

    /* 回复长度上限。0 = 不发这个字段，由服务商决定（有的家默认值很大）。 */
    if (max_tokens > 0) {
        o += (size_t)snprintf(dst + o, cap - o, ",\"max_tokens\":%d", max_tokens);
        if (o >= cap)
            o = cap - 1;
    }

    o += (size_t)snprintf(dst + o, cap - o, ",\"messages\":[");
    if (o >= cap)
        o = cap - 1;

    if (sys && *sys) {
        o += (size_t)snprintf(dst + o, cap - o,
                              "{\"role\":\"system\",\"content\":\"");
        if (o >= cap)
            return 0;
        o = escape_into(dst, cap, o, sys);
        if (o + 4 >= cap)
            return 0;               /* 连收尾的 "} 都放不下 → 装不下，别发半截 */
        o += (size_t)snprintf(dst + o, cap - o, "\"}");
        if (o >= cap)
            return 0;
    }

    /* ★ 返回 0 = **装不下**，由调用方如实报告。
     *   绝不返回一个"被截断但看起来正常"的长度 —— 那正是这个 bug 的成因：
     *   截断后的 JSON 服务端不认，错误信息却完全指向别处。 */
    if (o + 2 >= cap)
        return 0;
    return o;
}

/* ---- 历史挑选：从最新往回，装不下就丢最早的 ----
 * 返回第一条的下标；一条都不要就返回 n_all。
 *
 * ★ 与 build_request 共用（见文件头「关于请求体装不下这件事」）。
 *   base 是**前缀已占用的字节数**（含 system 消息），由 build_prefix
 *   算出后原样传进来 —— 估算器那边也走同一条路，两边对"起跑线"的
 *   认知必然一致。
 *
 * 先估后拼（转义最多让一个字符变两个，按 2 倍算是安全上界），
 * 免得拼到一半才发现放不下、还得回退。 */
static int ctx_pick_start(const ChatTurn *turns, int n_all, size_t base,
                          const char *user_text)
{
    int    lo    = n_all - CTX_MAX_TURNS;      /* 条数上限 */
    int    start = n_all;
    size_t used  = base + strlen(user_text) * 2 + 48;   /* 本次那条 + 收尾 */

    if (lo < 0)
        lo = 0;

    for (int i = n_all - 1; i >= lo; --i) {
        size_t c = strlen(turns[i].text) * 2 + 40;
        if (used + c > REQ_BUDGET)
            break;
        used += c;
        start = i;
    }
    return start;
}

static void build_request(const ProviderDef *p, const char *model,
                          const ChatCtx *ctx, const char *user_text, bool stream,
                          int thinking)
{
    const char     *sys   = (ctx && ctx->system_prompt) ? ctx->system_prompt : "";
    const ChatTurn *turns = (ctx && ctx->turns) ? ctx->turns : NULL;
    const int       n_all = (turns && ctx->n_turns > 0) ? ctx->n_turns : 0;
    const int       mtok  = ctx ? ctx->max_tokens : 0;
    bool   first = true;          /* 下一条消息前面要不要逗号 */
    size_t o;

    /* ★ 先只写 model，再按需要补 max_tokens，最后才开 messages 数组。
     * 早先是一整条 snprintf 拼到底，加字段就得整句重排 —— 拆开之后
     * 每个可选字段各自独立，漏了也只是少发一个字段，不会写坏 JSON。
     * 现在前缀整段收进 build_prefix，环的估算也走它。 */
    o = build_prefix(req_body, sizeof req_body, p, model, mtok, sys);
    if (o == 0) {
        /* 装不下就**记下来**，由调用方如实报错（本函数是 void）。
         * 绝不继续拼一个半截 JSON —— 那正是"服务端说格式错误"的成因。 */
        g_build_overflow = true;
        g_build_overflow_sys = (int)strlen(sys);
        snprintf(req_body, sizeof req_body, "{}");   /* 留个合法空壳，反正不会发 */
        return;
    }
    if (*sys)
        first = false;

    /* ---- 2) 历史 ---- */
    {
        int start = ctx_pick_start(turns, n_all, o, user_text);

        for (int i = start; i < n_all; ++i) {
            o += (size_t)snprintf(req_body + o, sizeof req_body - o,
                                  "%s{\"role\":\"%s\",\"content\":\"",
                                  first ? "" : ",",
                                  turns[i].mine ? "user" : "assistant");
            if (o >= sizeof req_body)
                o = sizeof req_body - 1;
            o = escape_into(req_body, sizeof req_body, o, turns[i].text);
            o += (size_t)snprintf(req_body + o, sizeof req_body - o, "\"}");
            first = false;
        }
    }

    /* ---- 3) 本次这句 ---- */
    o += (size_t)snprintf(req_body + o, sizeof req_body - o,
                          "%s{\"role\":\"user\",\"content\":\"", first ? "" : ",");
    if (o >= sizeof req_body)
        o = sizeof req_body - 1;
    o = escape_into(req_body, sizeof req_body, o, user_text);

    snprintf(req_body + (o < sizeof req_body ? o : sizeof req_body - 1),
             sizeof req_body - (o < sizeof req_body ? o : sizeof req_body - 1),
             "\"}],\"stream\":%s%s}", stream ? "true" : "false",
             thinking_json(p, thinking));

    /* ---- 记账：上一条真实请求有多大、有没有被截断 ----
     * 给对话页那个环当"实际值"用。环本身是**预估**（发送前就得画出来），
     * 而这两个数是**事实** —— 两边对不上时，弹窗里要能让用户看出来。
     * 演示与豆包不走这条路，永远停在 0 / false。 */
    g_last_req_len = (int)strlen(req_body);
    g_last_capped  = (strlen(req_body) >= sizeof req_body - 1);
}

/* ---- 从响应里取 choices[0].message.content ---- */
typedef struct {
    char *out;
    size_t cap;
    bool  got;
} PickCtx;

static bool pick_reply(const MiniJsonValue *v, void *user)
{
    PickCtx *pc = (PickCtx *)user;

    if (v->type != MJ_STRING)
        return true;

    /* minijson 对数组元素不拼 [i]，路径就是 "choices.message.content"，
     * 下标在 v->index。只要第一个（绝大多数接口只返回一个 choice）。 */
    if (v->index == 0 && !strcmp(v->path, "choices.message.content")) {
        /* ★ cap==0 时 pc->cap - 1 是 size_t 下溢（SIZE_MAX），
     *   strncpy 会往一个 0 字节的缓冲区里灌 —— 野写。
     *   现在恒有上限 2048，但这类"靠调用方保证"的写法不该留着。 */
    if (pc->cap > 0) strncpy(pc->out, v->str, pc->cap - 1);
        pc->out[pc->cap - 1] = '\0';
        pc->got = true;
        return false;                 /* 拿到就收工 */
    }
    return true;
}

/* ---- 从响应里取思考内容（choices[0].message.reasoning_*）----
 *
 * ★ 为什么单独一个函数走第二遍，而不是在 pick_reply 里多判一个路径：
 *   pick_reply 一拿到正文就 `return false` 收工了，而**思考内容总是排在
 *   正文之前**（字段顺序由服务端定，不保证），夹在里头就得处理
 *   "谁先谁后、能不能继续"这些状态 —— 为一个辅助字段把一个判据搞复杂，
 *   不划算。正文一次 walk、思考一次 walk，两次都是小 body，成本可忽略。
 *
 * ★ 认两个名字，且**按优先级**：
 *   · `reasoning_content` —— DeepSeek 官方字段（它也是唯一一家
 *     文档里明确写了"流式与非流式都返回"的）。
 *   · `reasoning`       —— 部分别家/中转用的名字。
 *   都读不到就当没有（空串）—— 那是**正常情况**，不是错误。
 *   ⚠️ 两条路径都命中时以先拿到的为准：混着改名只会让界面一会儿有
 *   一会儿没有，那比"一直没有"更让人困惑。 */
static bool pick_reason(const MiniJsonValue *v, void *user)
{
    PickCtx *pc = (PickCtx *)user;

    if (v->type != MJ_STRING || v->index != 0)
        return true;

    if (!strcmp(v->path, "choices.message.reasoning_content") ||
        !strcmp(v->path, "choices.message.reasoning")) {
        if (pc->got)
            return true;                  /* 已经收过一条了，不再覆盖 */
        /* ★ cap==0 时 pc->cap - 1 是 size_t 下溢（SIZE_MAX），
     *   strncpy 会往一个 0 字节的缓冲区里灌 —— 野写。
     *   现在恒有上限 2048，但这类"靠调用方保证"的写法不该留着。 */
    if (pc->cap > 0) strncpy(pc->out, v->str, pc->cap - 1);
        pc->out[pc->cap - 1] = '\0';
        pc->got = true;
        return false;
    }
    return true;
}

/* ---- 流式增量：choices[0].delta.content ----
 * 只认字典型（type==MJ_STRING）的那一个。SSE 的每个 chunk 都是完整的
 * 一小段 JSON，所以可以直接丢给 minijson_walk，不需要维护跨 chunk 的状态。 */
typedef struct {
    ChatStreamFn on_delta;
    void        *user;
    char        *full;        /* 累积正文（可为 NULL = 只回调不累积） */
    size_t       full_cap;
    size_t       full_len;
    char        *think;       /* 累积思考内容（可为 NULL） */
    size_t       think_cap;
    size_t       think_len;
    bool         stop;        /* 调用方叫停 */
} DeltaCtx;

/* 往累积缓冲里追加一段。**装不下时留一个"已截断"的记号**，不是悄悄少一截。
 *
 * 记号为什么必须留：界面上就是一行"思考过程（N 字）"，用户点开看到结尾
 * 突然断掉却没有任何说明，只会以为模型没想完、或者程序坏了。
 * 留 11 字节给"…（已截断）"是中文 5 字 + 省略号，正好在末尾摆得下。 */
#define TRUNC_MARK  "…（已截断）"

static void append_buf(char *dst, size_t *len, size_t cap,
                       const char *piece)
{
    size_t n = strlen(piece);

    if (!dst)
        return;

    if (*len + n < cap - sizeof(TRUNC_MARK)) {
        memcpy(dst + *len, piece, n);
        *len += n;
        dst[*len] = '\0';
        return;
    }

    /* 装不下：能塞多少塞多少，然后把尾巴换成记号。
     * ⚠️ 塞完之后**不再接收**（调用侧仍会继续读干净 body，只是不累积），
     *    否则每来一段都重写一次尾部，最后剩的还是一截断句。 */
    {
        size_t room = cap - sizeof(TRUNC_MARK) - *len;

        if (room > 0) {
            memcpy(dst + *len, piece, room);
            *len += room;
        }
        memcpy(dst + *len, TRUNC_MARK, sizeof(TRUNC_MARK) - 1);
        *len    += sizeof(TRUNC_MARK) - 1;
        dst[*len] = '\0';
    }
}

static bool pick_delta(const MiniJsonValue *v, void *user)
{
    DeltaCtx *dc = (DeltaCtx *)user;
    bool      is_think = false;

    if (v->type != MJ_STRING)
        return true;
    if (v->index != 0)
        return true;

    /* ★ 两条路：正文 与 思考。**同一段 chunk 里两者可能同时出现**
     *   （有的实现把思考塞在 delta 的一个固定字段里），所以是 if/else 判路径，
     *   不是"取到就 return" —— 那样会把另一股流丢掉。 */
    if (!strcmp(v->path, "choices.delta.content")) {
        is_think = false;
    } else if (!strcmp(v->path, "choices.delta.reasoning_content") ||
               !strcmp(v->path, "choices.delta.reasoning")) {
        is_think = true;
    } else {
        return true;
    }

    if (is_think) {
        if (dc->think && dc->think_len + sizeof(TRUNC_MARK) < dc->think_cap)
            append_buf(dc->think, &dc->think_len, dc->think_cap, v->str);
    } else if (dc->full) {
        append_buf(dc->full, &dc->full_len, dc->full_cap, v->str);
    }

    if (dc->on_delta && !dc->on_delta(v->str, is_think, dc->user)) {
        dc->stop = true;
        return false;             /* 中止本 chunk 的解析 */
    }
    return true;
}

static const char *http_word(int code, const char *detail)
{
    /* 把状态码翻成一句能指导行动的话，而不是让界面显示一个三位数 */
    if (code == 401 || code == 403)
        return "API Key 不对或没权限，去控制台检查一下";
    if (code == 429)
        return "触发限流了，等一会儿再试";
    if (code == 0)
        return detail[0] ? detail : "连不上（网络或证书问题）";
    return detail;
}

/* --------------------------------------------------------------------------
 * 离线演示（provider = "demo"，判据是 base_url 为空串）
 *
 * 为什么要留这条路，三个理由缺一个都不该加这段代码：
 *
 *   1. 还没配 Key、或者网不通的时候，用户第一次打开也能看到"它长什么样"。
 *      一个只会报 401 的界面，是没法让人判断"值不值得去配 Key"的。
 *   2. **验收脚本不再需要真 Key 就能验"回复过程中文本在增长"** —— 这是自动化
 *      里最贵的一环：要一把能用的 Key、要额度、要网络，三者缺一测试就废。
 *      演示走的是和真实请求**完全相同**的 on_delta 回调链路（同一套节流、
 *      滚动、上屏），所以它能替真请求证明 UI 这半边的正确性。
 *      它证明不了的是 httpc 那一半 —— 那部分只能靠真 Key，别混淆。
 *   3. 反过来也是个结构断言：如果把 httpc 摘掉界面就崩，说明 UI 和网络
 *      还黏在一起；能干净地换掉数据源，耦合就是真的分开了。
 *
 * 切块必须**落在 UTF-8 字符边界**上，不能按字节切 —— 中文一个字 3 字节，
 * 从中间劈开会往屏上送半个字符，渲染出来就是个乱码方块。
 * ----------------------------------------------------------------------- */

static const char DEMO_REPLY[] =
    "你好，我是一个跑在 3DS 上的小助手。\n"
    "\n"
    "你现在看到的是离线演示：这段文字是内置的，"
    "没有联网，也没有用你的 Key。留这么一条路，"
    "是为了在还没配好 Key 的时候，也能先看清界面怎么运作、"
    "回复是怎么一点一点长出来的。\n"
    "\n"
    "真接上服务商之后，流程和现在完全一样，只是那些字"
    "会从网络那头一个字一个字流过来。到那时候把服务商换掉就行。";

/* --------------------------------------------------------------------------
 * 演示：动作分支
 *
 * 用户在话里提到"创建文件"时，演示也照真模型的样子给出一段
 * [[ACT:file.write ...]]。为什么非要让离线演示也会这一手：
 *
 *   · 动作能力是全项目最需要"能重复验"的一块。它跨了四道闸、三个源文件
 *     和一个新屏幕，而真模型愿不愿意吐这段标记、吐成什么格式，都是**它
 *     自己说了算**的 —— 拿它当验收的输入，等于把测试的稳定性外包给模型。
 *     演示把输入钉死，于是"解析 → 确认屏 → 落盘 → 审计"这整条链
 *     可以在没有任何 Key 的情况下反复跑。
 *
 *   · 顺带它也是**用户看得见的引导**：第一次装应用的人不用去猜
 *     "它能不能动手"，按一下就有。
 *
 * 参数里的名字**故意**起得平常：它走的是和真模型一模一样的一条路
 * （同一个解析器、同一个黑名单、同一个沙箱目录），没有任何"演示专用通道"，
 * 否则验的就不是真路径了。
 * ----------------------------------------------------------------------- */

/* 在 user_text 里找触发词。找不到返回 NULL。
 *
 * 只认这一小把词，是因为演示的定位是"把路径演一遍"，不是"理解你说什么"。
 * 判据越窄越好：宽了会出现"我随口说一句它就开始写文件"，
 * 那反倒让人以为这应用很危险。 */
static const char *demo_action_trigger(const char *user_text)
{
    static const char *const WORDS[] = {
        "创建文件", "建文件", "写文件", "写一个文件", "创建一个文件", "建一个文件",
    };

    if (!user_text || !*user_text)
        return NULL;

    for (size_t i = 0; i < sizeof WORDS / sizeof *WORDS; ++i)
        if (strstr(user_text, WORDS[i]))
            return WORDS[i];
    return NULL;
}

/* 演示的动作正文。内容本身无所谓有用，要紧的是它**可见、可核对** ——
 * 用户会在确认屏上看见这几行，然后才决定批不批准。 */
static const char DEMO_ACTION_BODY[] =
    "# 3DS AI Console\n"
    "\n"
    "这个文件是 AI 助手在 3DS 上创建的。\n"
    "它经过了一整套确认流程：\n"
    "\n"
    "  1. 动作在编译期的白名单里（file.write）\n"
    "  2. 路径由程序拼，AI 只给了一个文件名\n"
    "  3. 你亲手点了\"允许\"\n"
    "  4. 这次决定记进了 action.log\n";

/* 演示的"提问"触发词。与动作词表**不重叠**：提问是问用户，
 * 动作是动系统，混在一起会让演示看起来像"问一句就把文件写了"。 */
static bool demo_ask_trigger(const char *user_text)
{
    return user_text && (strstr(user_text, "问我") || strstr(user_text, "选项")
                         || strstr(user_text, "确认一下"));
}

/* 把 ask 动作拼到 dst 末尾（问题 + 用 | 分隔的选项）。 */
static bool demo_ask_append(char *dst, size_t cap)
{
    size_t used = strlen(dst);
    int    n    = snprintf(dst + used, cap - used,
                           "\n[[ACT:ask question=%s options=%s]][[/ACT]]",
                           "要我用大字还是标准字号？", "标准|大");

    if (n < 0 || (size_t)n >= cap - used) {
        dst[used] = '\0';
        return false;
    }
    return true;
}

/* 把动作标记拼到 dst 末尾。返回 false 表示放不下（此时 dst 保持原样）。 */
static bool demo_action_append(char *dst, size_t cap, const char *name)
{
    size_t used = strlen(dst);
    int    n    = snprintf(dst + used, cap - used,
                           "\n[[ACT:file.write name=%s]]%s[[/ACT]]",
                           name, DEMO_ACTION_BODY);

    if (n < 0 || (size_t)n >= cap - used) {
        dst[used] = '\0';           /* snprintf 截断时可能已经写了一半 */
        return false;
    }
    return true;
}

/* --------------------------------------------------------------------------
 * 演示：安装一个程序
 *
 * 与 file.write 是**两条独立的触发词表**，不共用一个函数：
 * "装个游戏"和"写个文件"是两种意图，混在一起判会让某句话意外地
 * 触发另一种动作 —— 而那正是四道闸里第一道要防的（模型只给数据，
 * 但"给什么数据"必须是由它自己想清楚的，不能靠我们猜）。
 * ----------------------------------------------------------------------- */

static const char *demo_install_trigger(const char *user_text)
{
    static const char *const WORDS[] = {
        "装游戏", "安装游戏", "装个游戏", "安装一个游戏", "装程序", "安装程序",
        "装一个程序", "安装一个程序", "装个程序", "装软件", "安装软件",
    };

    if (!user_text || !*user_text)
        return NULL;

    for (size_t i = 0; i < sizeof WORDS / sizeof *WORDS; ++i)
        if (strstr(user_text, WORDS[i]))
            return WORDS[i];
    return NULL;
}

/* 装哪个包？演示固定挑一个名字。
 *
 * 为什么不从 pkg_list() 里随机挑：**演示要可复现**。
 * 同一句输入跑一百遍应该产出同一条动作，这样"这趟验过了没有"
 * 只需要看一个确定的包名。真实场景下这个选择会由模型做 ——
 * 它会先看到卡上有哪些包（pkg_list 进上下文），然后自己决定装哪个。
 * 但那一步的前提是"卡上真有那个包"，而演示环境里包是我们自己摆的。 */
#define DEMO_INSTALL_PKG "demo_game"

/* 演示的包名与摘要。摘要这一条是刻意的：pkg.install 最要紧的护栏
 * 就是"装前必须核对"，而演示如果不带 sha256，那条护栏在这条路径上
 * 就永远没被跑过 —— 等真接上模型才发现它坏了。
 *
 * 这个值不是真的（演示包也不是真的），但它的**形状**是真的：64 位
 * 小写十六进制。解析器按形状收，install 按值比对 —— 两边都走到了。 */
#define DEMO_INSTALL_SHA \
    "0000000000000000000000000000000000000000000000000000000000000000"

static bool demo_install_append(char *dst, size_t cap)
{
    size_t used = strlen(dst);
    int    n    = snprintf(dst + used, cap - used,
                           "\n[[ACT:pkg.install name=%s sha256=%s]][[/ACT]]",
                           DEMO_INSTALL_PKG, DEMO_INSTALL_SHA);

    if (n < 0 || (size_t)n >= cap - used) {
        dst[used] = '\0';
        return false;
    }
    return true;
}

/* 不带摘要的版本。存在的理由：pkg.install 的 sha256 是**可选**的，
 * 而"可选"这条路径必须有人走 —— 否则它坏了也没人知道，直到某个
 * 真实模型偶尔不给摘要时才炸。
 *
 * 触发词与带摘要的那条不同（"不用核对"），于是两条路径都能在验收里
 * 被单独跑到，而不是靠改夹具猜行为。 */
static const char *demo_install_nohash_trigger(const char *user_text)
{
    static const char *const WORDS[] = {
        "不用核对", "不要核对", "跳过校验", "不校验",
    };

    if (!user_text || !*user_text)
        return NULL;

    for (size_t i = 0; i < sizeof WORDS / sizeof *WORDS; ++i)
        if (strstr(user_text, WORDS[i]))
            return WORDS[i];
    return NULL;
}

static bool demo_install_append_nohash(char *dst, size_t cap)
{
    size_t used = strlen(dst);
    int    n    = snprintf(dst + used, cap - used,
                           "\n[[ACT:pkg.install name=%s]][[/ACT]]",
                           DEMO_INSTALL_PKG);

    if (n < 0 || (size_t)n >= cap - used) {
        dst[used] = '\0';
        return false;
    }
    return true;
}

/* 从 s 的 pos 处取不超过 want 字节的一块，且**不切开 UTF-8 字符**。
 * 返回取到的字节数；pos 已到末尾返回 0。
 *
 * 0x80..0xBF 是 UTF-8 的"后续字节"（高位 10xxxxxx）。切点若落在这种字节上，
 * 说明正劈在一个字中间 —— 往后挪到下一个字符的首字节处。
 * pos 本身一定落在字符首字节（调用方每次前进的都是本函数返回的长度）。 */
static size_t utf8_slice(const char *s, size_t len, size_t pos,
                         size_t want, char *dst, size_t cap)
{
    size_t end, n;

    if (pos >= len)
        return 0;

    end = pos + want;
    if (end > len)
        end = len;
    while (end < len && ((unsigned char)s[end] & 0xC0) == 0x80)
        ++end;
    if (end <= pos)                     /* want 小到挪不动 → 至少吃一个字 */
        end = pos + 1;

    n = end - pos;
    if (n > cap - 1)
        n = cap - 1;
    memcpy(dst, s + pos, n);
    dst[n] = '\0';
    return n;
}

/* 拼出这次要"回答"的全文：上下文条数/有无 + 用户原文的开头一截 + 内置正片。
 *
 * 把**上下文条数和有没有人设**写在最前面，是有意的：演示模式没法真的
 * "记住"什么，但它能如实汇报"这次带了什么背景"—— 于是"多轮和人设到底
 * 有没有生效"由屏幕直接回答，不用去猜、也不用翻日志。
 * 真机上接上服务商后这段会消失。
 *
 * 带上用户那句话，是为了让人看出"我说的话真的送出去了"——
 * 纯固定剧本看起来像一段动画，用户会怀疑输入根本没生效。 */
static bool demo_answer(const char *user_text, const ChatCtx *ctx,
                        char *dst, size_t cap)
{
    const int   n_hist = (ctx && ctx->turns) ? ctx->n_turns : 0;
    const char *sys    = (ctx && ctx->system_prompt) ? ctx->system_prompt : "";
    size_t used = 0;
    char   head[160];

    dst[0] = '\0';

    snprintf(head, sizeof head, "（本轮带上了 %d 条上下文·%s）\n",
             n_hist, *sys ? "有人设" : "无人设");
    used = strlen(head);
    if (used + 1 >= cap)
        return false;
    memcpy(dst, head, used);
    dst[used] = '\0';

    if (user_text && *user_text) {
        char frag[40];

        utf8_slice(user_text, strlen(user_text), 0, 27, frag, sizeof frag);
        snprintf(head, sizeof head, "\n你刚才说的是「%s」。\n", frag);
        if (used + strlen(head) + 1 >= cap)
            return used > 0;
        memcpy(dst + used, head, strlen(head));
        used += strlen(head);
        dst[used] = '\0';
    }

    {
        size_t n = strlen(DEMO_REPLY);
        if (used + n + 1 > cap)
            return used > 0;
        memcpy(dst + used, DEMO_REPLY, n);
        dst[used + n] = '\0';
    }

    /* 动作分支：用户提到"创建文件"就附一段动作标记。
     * 放在正片的**后面** —— 剥掉标记之后，用户看到的是"说明 + 一段确认请求"，
     * 先解释再动手，读起来才顺。 */
    /* ★ 2026-10-29：提问分支。放在动作分支**之前**并与之互斥 ——
     * 同一段回复只带一个动作，这是上层约定，演示也遵守。 */
    if (demo_ask_trigger(user_text)) {
        if (!demo_ask_append(dst, cap)) {
            /* 放不下就不放，绝不送半截标记。 */
        }
    } else if (demo_action_trigger(user_text)) {
        char fname[64];

        /* 文件名固定、不含时间戳：同一台机器上跑一百遍也该产出同一个文件，
         * 这样"验证 O 趟跑过没有"只需要看一个名字就够了。覆盖同名文件
         * 本来也是这条路要验的行为之一（确认屏上写着"同名会被覆盖"）。 */
        snprintf(fname, sizeof fname, "agent_demo.txt");
        if (!demo_action_append(dst, cap, fname)) {
            /* 放不下就不放：宁可这次没有动作，也不能送一个**被腰斩的**
             * 标记出去 —— 残缺的标记在解析器眼里等于"模型说了句奇怪的话"，
             * 会让动作路径安静地失效，而且看起来像解析器的锅。 */
        }
    }

    /* 安装分支：与上面互不干扰（两条词表没有交集）。
     * 同一段回复里最多带一个动作 —— 这是**上层的约定**而不是这儿的限制，
     * 但演示遵守它，免得造出一段现实中不会出现的输入。
     *
     * 三分支的优先级：带摘要 → 不带摘要 → 都不匹配就什么都不给。
     * "不用核对"那条排在后面，是因为它必须是**显式**的：默认永远带摘要，
     * 用户得特意说一句才跳过 —— 这与 pkg.h 里"没摘要必须被显示出来"
     * 是同一条原则的两个落点。 */
    if (!demo_action_trigger(user_text)) {
        if (demo_install_nohash_trigger(user_text)) {
            if (!demo_install_append_nohash(dst, cap)) {
                /* 放不下就一个动作都不给，不送残缺标记。 */
            }
        } else if (demo_install_trigger(user_text)) {
            if (!demo_install_append(dst, cap)) {
                /* 同上。 */
            }
        }
    }

    return true;
}

/* 演示的非流式：一次给全文。两条路都留着，是为了让"流式 vs 非流式"
 * 这个开关在演示下也能看出区别 —— 不然没法拿它做对照。 */
static bool demo_reply(const char *user_text, const ChatCtx *ctx, ChatResult *out)
{
    static char full[CHAT_REPLY_MAX];

    if (!demo_answer(user_text, ctx, full, sizeof full)) {
        snprintf(out->error, sizeof out->error, "演示剧本拼装失败");
        return false;
    }
    snprintf(out->reply, CHAT_REPLY_MAX, "%s", full);
    out->http_code = 200;
    return true;
}

/* 演示的流式：按块回调，节奏模仿真实吐字。 */
static bool demo_stream(const char *user_text, const ChatCtx *ctx,
                        int thinking,
                        ChatStreamFn on_delta,
                        void *user, ChatResult *out)
{
    static char full[CHAT_REPLY_MAX];   /* 2KB，别放栈 */
    char piece[24];
    size_t len, pos = 0, total = 0;

    if (!demo_answer(user_text, ctx, full, sizeof full)) {
        snprintf(out->error, sizeof out->error, "演示剧本拼装失败");
        return false;
    }
    len = strlen(full);

    /* 首 token 前先停一下。真实模型有一段"思考"时间，演示里不装这一下，
     * 用户上了真机会觉得"怎么反而变慢了"，把正常现象当成故障。 */
    svcSleepThread(350000000LL);

    /* ---- 先吐一段"思考"（**只在开了思考档位时**）----
     *
     * ★ 为什么挂在档位上，而不是无条件吐：
     *   真机上就是"开了思考才有思考内容"，演示不该比真机更慷慨 ——
     *   无条件吐的话，用户关着档位也看到一段思考，会以为这个开关没用。
     *
     * ★ 为什么必须有这一支：思考块（收/放、与正文分开累积、截断记号）
     *   是 2026-10-16 新写的一整条链路。没有它，这半边**只在真机配了
     *   真模型、且开了档位时才可能被走到** —— 那正是最容易埋雷的位置。
     *   它是唯一一条不用 Key、不出网就能把这条链路走完的路。
     *
     * ⚠️ 别动 70ms 那块节奏：M 趟的四张进度快照是**配着这个节奏**排的
     *    （详见下面那段注释）。思考块默认不走（档位 0），所以 M 趟不受影响。 */
    if (thinking > 0) {
        static const char think[] =
            "先看用户要什么：他要一条能看出来的流式回复。"
            "那就先确认演示剧本在不在，再决定说几句、说到哪儿停。";
        size_t tlen = strlen(think), tp = 0, tn = 0;

        out->reasoning[0] = '\0';
        while (tp < tlen) {
            size_t n = utf8_slice(think, tlen, tp, 6, piece, sizeof piece);

            if (n == 0)
                break;
            tp += n;

            if (tn + n + 1 < CHAT_THINK_MAX) {
                memcpy(out->reasoning + tn, piece, n);
                tn += n;
                out->reasoning[tn] = '\0';
            }

            if (on_delta && !on_delta(piece, true, user)) {
                out->http_code = 200;   /* 叫停不是失败 */
                return tn > 0;
            }
            svcSleepThread(70000000LL);
        }
    }

    while (pos < len) {
        size_t n = utf8_slice(full, len, pos, 6, piece, sizeof piece);
        if (n == 0)
            break;
        pos += n;

        if (total + n + 1 < CHAT_REPLY_MAX) {
            memcpy(out->reply + total, piece, n);
            total += n;
            out->reply[total] = '\0';
        }

        /* 正文：reasoning=false。演示的思考段已经在上面那一支吐完了。 */
        if (on_delta && !on_delta(piece, false, user)) {
            out->http_code = 200;       /* 叫停不是失败：已收到的部分算数 */
            return total > 0;
        }

        /* 70ms 一块。这个数是**配着验收脚本的连拍节奏**定的，不是随手拍的：
         * 本剧本约 500 字节，按 6 字节切 ≈ 83 块 → 全程约 6 秒。
         * M 趟在发送后 2.0s / 3.6s / 5.2s / 6.8s 各拍一张（脚本里 wait 是帧，
         * 模拟器约 5fps，1 帧≈0.2s），于是四张分别落在吐字的
         * 约 28% / 56% / 84% / 100% 处 —— 张张递增，一眼能看出是流式。
         * 切太快（比如 20ms）四张就全成了成品，验收断言会退化成
         * "最后一帧比第一帧多"，看着像过了，其实什么也没证明。 */
        svcSleepThread(70000000LL);
    }

    out->http_code = 200;
    return total > 0;
}

/* --------------------------------------------------------------------------
 * 共用的连接建立
 *
 * 把 open → 头 → body → begin → status 这段走完。成功（R_SUCCEEDED 且
 * status==200）返回 true，此时 *ctx 是活的、body 还没读；失败时已经把
 * ctx/httpc 收干净并填好 out，调用方直接返回即可。
 * ----------------------------------------------------------------------- */
/* 参数名用 chat_ctx 而不是 ctx：下面还有个 httpcContext *ctx，
 * 两者同名会遮蔽，读起来也容易搞混（一个装上下文、一个装连接）。 */
static bool open_and_send(const ProviderDef *p, const char *model,
                          const char *apikey, const ChatCtx *chat_ctx,
                          const char *user_text, int thinking,
                          bool stream, httpcContext *ctx, u32 *status,
                          ChatResult *out)
{
    char url[256];
    Result r;

    snprintf(url, sizeof url, "%s/chat/completions", p->base_url);

    g_build_overflow = false;
    build_request(p, model, chat_ctx, user_text, stream, thinking);
    if (g_build_overflow) {
        out->http_code = 0;
        snprintf(out->error, sizeof out->error,
                 "请求体装不下系统提示（手册 %d 字节 / 缓冲 %d 字节）。"
                 "请缩短 AGENT.md 或联系开发者调大 REQ_BUF",
                 g_build_overflow_sys, (int)sizeof req_body);
        return false;
    }

    /* ★★ httpcInit 的参数**不是**"随便给个缓冲"，而是
     *   「POST 数据要上传的那块共享内存」。libctru 的注释写得很清楚：
     *     "The sharedmem contains data which will be later uploaded for HTTP
     *      POST. sharedmem_size should be aligned to 0x1000-bytes."
     *
     *   原来传 0 → 用默认大小（4KB 量级）。于是：
     *     · 旧请求体 4095 字节**刚好塞得下** → 请求能发出去（只是被我们自己截断成半截 JSON）；
     *     · 扩到 40KB 之后**塞不进去** → httpcAddPostDataRaw 直接返回 0xD8A0A003，
     *       屏上就是"请求体写不进去 0xd8a0a003"。
     *   一分钱没花就同时解释了前后两个症状。
     *
     *   现在给 64KB（0x10000，按 0x1000 对齐），比 REQ_BUF(40KB) 有余量。 */
    r = httpcInit(64 * 1024);
    if (R_FAILED(r)) {
        snprintf(out->error, sizeof out->error, "网络服务起不来 0x%08lx",
                 (unsigned long)r);
        return false;
    }

    r = httpcOpenContext(ctx, HTTPC_METHOD_POST, url, 0);
    if (R_FAILED(r)) {
        snprintf(out->error, sizeof out->error, "打不开连接 0x%08lx",
                 (unsigned long)r);
        httpcExit();
        return false;
    }

    httpcAddRequestHeaderField(ctx, "Content-Type", "application/json");
    {
        static char auth[512];
        snprintf(auth, sizeof auth, "Bearer %s", apikey);
        httpcAddRequestHeaderField(ctx, "Authorization", auth);
    }
    /* SSE 必须告诉服务端"别缓冲、别压缩"。Accept 一下更保险 ——
     * 有的中转不认 stream:true，但认这个头。 */
    if (stream) {
        httpcAddRequestHeaderField(ctx, "Accept", "text/event-stream");
        httpcAddRequestHeaderField(ctx, "Cache-Control", "no-cache");
    }
    httpcSetKeepAlive(ctx, HTTPC_KEEPALIVE_DISABLED);

    r = httpcAddPostDataRaw(ctx, (const u32 *)req_body, (u32)strlen(req_body));
    if (R_FAILED(r)) {
        snprintf(out->error, sizeof out->error, "请求体写不进去 0x%08lx",
                 (unsigned long)r);
        httpcCloseContext(ctx);
        httpcExit();
        return false;
    }

    r = httpcBeginRequest(ctx);
    if (R_SUCCEEDED(r))
        r = httpcGetResponseStatusCode(ctx, status);

    out->http_code = (int)*status;

    if (R_FAILED(r)) {
        snprintf(out->error, sizeof out->error, "请求失败 0x%08lx",
                 (unsigned long)r);
        httpcCloseContext(ctx);
        httpcExit();
        return false;
    }

    if (*status != 200) {
        /* 把服务端返回的 body 也读一点出来 —— 很多 API 会在里面说具体原因 */
        static char peek[256];
        u32 size = 0;
        peek[0] = '\0';
        httpcGetDownloadSizeState(ctx, NULL, &size);
        if (size > 0) {
            u32 want = size < sizeof peek - 1 ? size : sizeof peek - 1;
            /* want-1：ready 是严格大于，要 N 字节就填 N-1 */
            if (R_SUCCEEDED(httpcReceiveData(ctx, (u8 *)peek,
                                             want > 1 ? want - 1 : 1)))
                peek[want] = '\0';
        }
        snprintf(out->error, sizeof out->error, "HTTP %lu · %s",
                 (unsigned long)*status, http_word((int)*status, peek));
        httpcCloseContext(ctx);
        httpcExit();
        return false;
    }

    return true;
}

/* ---- 非流式 ---- */

bool chat_completion(const char *provider_name, const char *model,
                     int thinking,
                     const char *apikey, const ChatCtx *chat_ctx,
                     const char *user_text,
                     ChatResult *out)
{
    const ProviderDef *p = provider_find(provider_name);
    httpcContext ctx;
    u32 status = 0;
    /* 响应缓冲放静态区：8KB 放栈上对 3DS 有点冒险 */
    static char resp[RESP_BUF];

    if (!out)
        return false;
    out->http_code = 0;
    out->error[0] = '\0';
    out->reply[0] = '\0';
    /* ★ 必须每轮清：`res` 在界面层是 **static**（sc_chat.c:809），
     * 不清的话上一轮的思考内容会挂在下一轮上 —— 用户关了档位也在看上一轮的思考。 */
    out->reasoning[0] = '\0';

    /* 演示 provider → 不发请求。判据是 **offline 字段**，不是"网址为空"。
     * ⚠️ 2026-10-16 改：以前判"base_url 为空"，但**自定义服务商**在用户
     *   还没填网址时也是空串 —— 于是"选了自定义却没填"会静默走进这里，
     *   演一段假回复，用户以为配通了。判据必须能区分这两件事。 */
    if (p->offline)
        return demo_reply(user_text, chat_ctx, out);

    /* ---- 豆包：一条完全不同的路 ----
     * 它走自己的 samantha 接口，**不靠 API Key 而靠登录 cookie**，
     * 所以这一支必须站在下面那句"还没有 API Key"**之前** ——
     * 否则用户会看到"还没有 API Key"，而他压根不需要配 Key，
     * 只会去找一个不存在的设置项。
     * 请求构造与三层 JSON 的 SSE 解析都在 doubao.c。 */
    if (p->fmt == PROVIDER_FMT_DOUBAO) {
        char    msg[192];
        DbState ds = db_chat(user_text, chat_ctx ? chat_ctx->conversation_id : NULL,
                             out->reply, sizeof out->reply,
                             msg, sizeof msg);

        if (ds == DB_OK) {
            out->http_code = 200;
            snprintf(out->conversation_id, sizeof out->conversation_id,
                     "%s", db_last_conversation_id());
            return true;
        }
        out->http_code = 0;
        snprintf(out->error, sizeof out->error, "%s", msg);
        return false;
    }

    if (!apikey || !*apikey) {
        snprintf(out->error, sizeof out->error, "还没有 API Key");
        return false;
    }
    if (!user_text || !*user_text) {
        snprintf(out->error, sizeof out->error, "没输入内容");
        return false;
    }

    if (!open_and_send(p, model, apikey, chat_ctx, user_text, thinking,
                       false, &ctx, &status, out))
        return false;

    /* ---- 读完整响应 ----
     * 坑一：状态码到手 ≠ body 到手（远程 HTTPS 上 body 后到）。
     * 坑二：**服务商走 chunked 编码，响应头里没有 Content-Length**，
     *       GetDownloadSizeState 的 content-length 返回 0 —— 按"目标长度"
     *       读取的写法一个字节都拿不到（实测 size=0 got=0）。
     * 所以改用第一个参数（已拷贝字节）当进度：边读边看它涨不涨，
     * 连续多次不涨就认为读完了。 */
    u32 got = 0;
    int stall = 0;
    while (got < RESP_BUF - 1 && stall < 30) {
        u32 want = RESP_BUF - 1 - got;
        (void)httpcReceiveData(&ctx, (u8 *)(resp + got), want > 1 ? want - 1 : 1);
        u32 now = 0;
        httpcGetDownloadSizeState(&ctx, &now, NULL);
        if (now > got) {
            got = now > RESP_BUF - 1 ? RESP_BUF - 1 : now;
            stall = 0;
        } else {
            ++stall;
            svcSleepThread(20000000LL);      /* 20ms，最多空转 0.6 秒 */
        }
    }
    resp[got] = '\0';

    httpcCloseContext(&ctx);
    httpcExit();

    PickCtx pc = { out->reply, CHAT_REPLY_MAX, false };
    minijson_walk(resp, got, pick_reply, &pc);

    /* 思考内容：**能拿到就拿到，拿不到就空着**，绝不影响这一轮算不算成功。
     * 顺序放在正文之后，是因为正文才是"这一轮有没有用"的判据 ——
     * 下面紧接着就判 pc.got。 */
    {
        PickCtx rc = { out->reasoning, CHAT_THINK_MAX, false };
        out->reasoning[0] = '\0';
        minijson_walk(resp, got, pick_reason, &rc);
    }

    if (!pc.got) {
        snprintf(out->error, sizeof out->error,
                 "收到了回复，但没找到内容字段（服务商格式变了？）");
        return false;
    }
    return true;
}

/* --------------------------------------------------------------------------
 * 流式
 *
 * 主循环的写法（这是整段代码里唯一需要"想清楚"的地方）：
 *
 *   · httpcReceiveData(ctx, buf, STREAM_CHUNK-1)
 *       —— 缓冲水位到 STREAM_CHUNK 就返回 STREAM_CHUNK-1 字节；
 *          传输结束时也返回（可能是 0 字节，也可能是剩下的尾巴）。
 *   · 返回 ErrorBufferSmall 完全正常 —— 它只表示"这次没读完整个 body"，
 *     数据该拿到多少拿到了多少。
 *   · 只有 httpcGetDownloadSizeState 的**已拷贝量涨了**才算真拿到东西；
 *     一次没涨就马上用一个小超时调用再试一次 —— **不要**在这里长睡，
 *     否则"等新 token"就变成了"界面冻住"。
 *   · 连续若干次（含超时）都没进展就收工。5 × 300ms = 1.5s 静默，
 *     对 SSE 来说足够判定"对面不发了"。
 *
 * 收工后**必须把 body 读干净**再 httpcCloseContext：还剩数据没读时，
 * 关闭会一直等在那儿（真机和 Azahar 都是）。所以有个 drain 循环，
 * 上限兜底，宁可放弃后几字节也不能卡死。
 * ----------------------------------------------------------------------- */

/* SSE 一行是怎么吐给回调的：只看 data: 前缀，忽略 event:/id:/注释 */
static void feed_sse_line(char *line, size_t len, DeltaCtx *dc)
{
    if (len < 5 || memcmp(line, "data:", 5) != 0)
        return;

    char *p = line + 5;
    /* SSE 允许 "data: " 也允许 "data:"，跳掉一个可选空格 */
    if (*p == ' ')
        ++p;

    if (!strcmp(p, "[DONE]"))
        return;                      /* 结束标记不是内容 */

    /* 有些实现会往每个 chunk 里塞空 content（首帧的 role 帧就是这样），
     * minijson 找不到 choices.delta.content 就什么也不做 —— 不用特判。 */
    minijson_walk(p, strlen(p), pick_delta, dc);
}

bool chat_completion_stream(const char *provider_name, const char *model,
                            int thinking,
                            const char *apikey, const ChatCtx *chat_ctx,
                            const char *user_text,
                            ChatStreamFn on_delta, void *user,
                            ChatResult *out)
{
    const ProviderDef *p = provider_find(provider_name);
    httpcContext ctx;
    u32 status = 0;
    u32 copied = 0;
    long started_snapshot;

    /* 都放静态区：单线程、一次一个请求，静态化是安全的，
     * 而 2.2KB(ChatResult) + 1.5KB(缓冲) 放栈上会把发送路径顶到危险深度 */
    static char chunk[STREAM_CHUNK];
    static char line[SSE_LINE_MAX];
    DeltaCtx dc;
    size_t line_len = 0;
    bool   done = false;
    int    idle = 0;

    if (!out)
        return false;
    out->http_code = 0;
    out->error[0] = '\0';
    out->reply[0] = '\0';
    out->reasoning[0] = '\0';      /* 同上：res 是 static，必须每轮清 */

    /* 演示 provider → 不发请求，走内置剧本。判据是 offline 字段（同上）。
     * 回调链路与真实流式完全一致，所以 UI 那半边照样被完整地跑一遍。 */
    if (p->offline)
        return demo_stream(user_text, chat_ctx, thinking, on_delta, user, out);

    /* 豆包：一次拿全文，然后**当成一段 delta 发一次**。
     *
     * 为什么不在这里解它的 SSE：豆包是三层 JSON 套嵌（见 doubao.c），
     * 逐帧解出来交给回调当然更顺眼，但那是锦上添花 ——
     * "能用"才是这条线的第一目标。一次性发一段，UI 那半边的整条回调链
     * 照样被完整走通（与 demo_stream 同一个形状）。
     * 将来要做真流式，只需把 db_chat 换成逐步回调的版本，这一支不用动。 */
    if (p->fmt == PROVIDER_FMT_DOUBAO) {
        char    msg[192];
        DbState ds = db_chat(user_text, chat_ctx ? chat_ctx->conversation_id : NULL,
                             out->reply, sizeof out->reply,
                             msg, sizeof msg);

        if (ds != DB_OK) {
            out->http_code = 0;
            snprintf(out->error, sizeof out->error, "%s", msg);
            return false;
        }
        out->http_code = 200;
        snprintf(out->conversation_id, sizeof out->conversation_id,
                 "%s", db_last_conversation_id());
        /* 豆包一次发全文：全是正文，没有思考那一股。
         * （它不返回 reasoning 字段，这里也不能假装它有。） */
        if (on_delta && out->reply[0])
            on_delta(out->reply, false, user);
        return true;
    }

    if (!apikey || !*apikey) {
        snprintf(out->error, sizeof out->error, "还没有 API Key");
        return false;
    }
    if (!user_text || !*user_text) {
        snprintf(out->error, sizeof out->error, "没输入内容");
        return false;
    }

    if (!open_and_send(p, model, apikey, chat_ctx, user_text, thinking,
                       true, &ctx, &status, out))
        return false;

    dc.on_delta  = on_delta;
    dc.user      = user;
    dc.full      = out->reply;
    dc.full_cap  = CHAT_REPLY_MAX;
    dc.full_len  = 0;
    /* 思考内容走**另一份**缓冲：正文与思考是两股流，共用一份的话
     * 界面上就是"AI 一边回答一边自言自语"，而且正文上限会被思考吃光。 */
    dc.think     = out->reasoning;
    dc.think_cap = CHAT_THINK_MAX;
    dc.think_len = 0;
    dc.stop      = false;
    out->reply[0]     = '\0';
    out->reasoning[0] = '\0';

    /* 起始水位：open_and_send 里 peek 过（那是非 200 分支，这里到不了），
     * 所以正常情况就是 0。取一次是为了让下面的"涨没涨"有基准。 */
    httpcGetDownloadSizeState(&ctx, &copied, NULL);
    started_snapshot = (long)copied;

    while (!done) {
        Result r = httpcReceiveDataTimeout(&ctx, (u8 *)chunk,
                                           STREAM_CHUNK - 1,
                                           idle > 0 ? 400000000ULL : 3000000000ULL);
        u32 now = 0;

        if (r == (Result)HTTPC_RESULTCODE_TIMEDOUT) {
            /* 超时不是错误：模型首 token 延迟经常好几秒，对面只是还没写。
             * 第一次给 3s（首 token 想久一点是正常的），之后就缩到 400ms
             * —— 已经在吐字了还停 400ms，基本就是吐完了。 */
            httpcGetDownloadSizeState(&ctx, &now, NULL);
            if ((long)now > started_snapshot) {
                started_snapshot = (long)now;
                idle = 0;
            } else if (++idle >= 5) {
                done = true;             /* 连 5 次静默（约 2s）→ 收工 */
            }
            continue;
        }

        httpcGetDownloadSizeState(&ctx, &now, NULL);

        if ((long)now <= started_snapshot) {
            /* 有返回但没有任何新数据 → 传输结束（或对面明确不发了） */
            done = true;
            continue;
        }

        /* 真拿到了。数据长度 = 两次已拷贝量之差 —— 不依赖 libctru 的返回值，
         * 因为 ErrorBufferSmall 和 Success 都会把数据拷进来。 */
        size_t n = (size_t)((long)now - started_snapshot);
        started_snapshot = (long)now;
        idle = 0;
        if (n > STREAM_CHUNK - 1)
            n = STREAM_CHUNK - 1;

        /* 逐字节喂给行解析器。chunk 是**字节流**不是行流：
         * 一个 SSE 事件可能横跨两次 ReceiveData，所以留着跨调用的 line 缓冲。 */
        for (size_t i = 0; i < n && !done; ++i) {
            char c = chunk[i];
            if (c == '\r')
                continue;                /* CRLF：CR 直接吃掉 */
            if (c == '\n') {
                line[line_len] = '\0';
                if (line_len > 0)
                    feed_sse_line(line, line_len, &dc);
                line_len = 0;
                if (dc.stop)
                    done = true;
                continue;
            }
            if (line_len + 1 < sizeof line)
                line[line_len++] = c;
            else
                line_len = 0;            /* 超长行：丢掉，别溢出 */
        }
    }

    /* ---- 把 body 读干净再关 ----
     * httpcCloseContext 在还有未读 body 时会一直等（连接不是 keep-alive，
     * 对面可能还在发）。给一个上限：最多再读 32KB / 40 次，
     * 到了就走 —— 宁可丢几字节也不能把界面卡在关闭上。 */
    for (int guard = 0; guard < 40; ++guard) {
        u32 now = 0;
        httpcGetDownloadSizeState(&ctx, &now, NULL);
        if ((long)now <= started_snapshot)
            break;
        started_snapshot = (long)now;
        (void)httpcReceiveDataTimeout(&ctx, (u8 *)chunk, STREAM_CHUNK - 1, 100000000ULL);
    }

    httpcCloseContext(&ctx);
    httpcExit();

    (void)done;

    /* 一个字符都没收到 → 这条路没走通，让调用方退回非流式。
     * 注意：真的回一个空回复（比如模型拒答）也会命中这里，
     * 退回非流式再问一次是合理的兜底（多花一次请求，换"不装作成功"）。 */
    if (out->reply[0] == '\0') {
        snprintf(out->error, sizeof out->error,
                 on_delta ? "对方没有返回流式内容（可能是中转不支持 SSE）"
                          : "对方没有返回任何内容");
        return false;
    }
    return true;
}

/* --------------------------------------------------------------------------
 * 上下文用量估算（对话页那个环 + 点它的弹窗）
 *
 * 为什么需要"预估"而不是只报实际值：环要在**发送之前**就画出来
 * （用户打字的时候就想看见离上限还有多远），而那会儿还没有请求。
 *
 * 为什么预估能和实际对上：前缀走 build_prefix、历史挑选走
 * ctx_pick_start，两个都是 build_request 正在用的同一个函数 ——
 * 不是照着抄一遍公式。抄一遍的后果见那两个函数头上的注释。
 *
 * 已知的一处偏差：这里用**当前默认服务商**的 default_model 算前缀，
 * 而实际请求可能发往别家（模型名长短差几个字节）。影响 <20 B，
 * 弹窗里同时给出"上一条实际发出多少字节"来兜底这个偏差。
 * ----------------------------------------------------------------------- */
int chat_ctx_budget(void)
{
    return REQ_BUDGET;
}

void chat_ctx_estimate(const char *model, int max_tokens,
                       const char *system_prompt,
                       const ChatTurn *turns, int n_turns,
                       const char *user_text, ChatCtxEst *out)
{
    /* 4KB 静态区：与 req_body 同规格。放栈上不合适 —— 这条路径从
     * 界面层调进来，栈深已经不好说了（req_body 本身就是这么挪静态的）。 */
    static char pfx[REQ_BUF];
    const char *sys = system_prompt ? system_prompt : "";
    const char *ut  = user_text ? user_text : "";
    size_t p;

    if (!out)
        return;
    memset(out, 0, sizeof *out);

    if (n_turns < 0)
        n_turns = 0;

    p = build_prefix(pfx, sizeof pfx, provider_find(NULL), model, max_tokens, sys);

    out->sys_bytes  = (int)p;                 /* 前缀 = 壳 + system 消息 */
    out->user_bytes = (int)(strlen(ut) * 2);  /* 与 build_request 同口径 */

    {
        int start = ctx_pick_start(turns, (turns && n_turns > 0) ? n_turns : 0,
                                   p, ut);
        int n_kept = ((turns && n_turns > 0) ? n_turns : 0) - start;
        if (n_kept > 0) {
            for (int i = start; i < n_turns; ++i)
                out->turn_bytes += (int)(strlen(turns[i].text) * 2 + 40);
        }
        out->n_turns = n_kept;
    }

    out->total = out->sys_bytes + out->turn_bytes + out->user_bytes + 48;
}

int chat_last_request_len(void)
{
    return g_last_req_len;
}

bool chat_last_request_capped(void)
{
    return g_last_capped;
}
