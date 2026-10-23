/*
 * chatapi.h -- 调服务商的 /chat/completions
 *
 * 两条路径，同一份请求构造与错误翻译：
 *   chat_completion()         一次请求、一次拿完整回复（M4 的地基，永远保留）
 *   chat_completion_stream()  边收边回调，逐 token 交给界面（M6）
 *
 * 为什么两个都留：流式是好体验，但**不是所有服务商/所有中转都支持 SSE**。
 * 流式失败必须能干净地退回非流式，而不是让用户看着一个卡住的「正在问…」。
 * 退回的逻辑在调用方（sc_chat.c）：流式拿不到任何内容且没到 200 就重发一次。
 *
 * 失败时不返回笼统的"错误"，而是把**服务端说了什么**带给界面：
 * 401 是 Key 不对，429 是限流，连不上是网络问题 ——
 * 这三种的应对方式完全不同，混成一句"失败了"等于没说。
 *
 * ── 关于流式怎么做到的（容易踩坑，写在这儿免得下个人重走一遍）──
 * libctru 的 httpcReceiveData 在真机与 Azahar 上的返回条件都是
 *   ready = cancelled || transfer_finished || (缓冲水位 > 请求的 size)
 * 注意是**严格大于**。所以：
 *   · 请求 size=N（N 大）→ 要等缓冲堆到 N+1 才回来 → 流式下会一直等到传输结束
 *   · 请求 size=1       → 缓冲有 ≥2 字节就回来 → 这才是"有多少拿多少"的正确用法
 * 我们取「想一次拿多少，就填 N-1」：水位到 N 就返回，误差 1 字节。
 * 另外 httpcGetDownloadSizeState 的第一个输出量是**应用自己已拷走的字节数**，
 * 不是内存里已缓冲的水位 —— 拿它当"还有没有新数据"有时会看走眼（同一次
 * 返回里它可能还没更新），所以流式循环不依赖它驱动，只用它做收尾确认。
 */

#pragma once

#include <stdbool.h>

/* 回复上限。2KB 对 3DS 的上屏来说已经是好几屏内容了 */
#define CHAT_REPLY_MAX 2048

/* 思考内容（reasoning）的上限。
 *
 * 为什么单独定、而且比正文小：思考是**过程**，不是用户要的答案。
 * 真机上看完一段 2KB 的思考对用户毫无意义，而 RAM 与 SSE 缓冲都得为它留位置。
 * 1024 大约能装一段中等长度的推理（中文 ~340 字 / 英文 ~150 词），
 * 超了就在尾部收尾并留一个"已截断"的记号 —— 见 chatapi.c 的 append_buf()。
 * **不留记号最坏**：用户会以为模型没想完，或者以为后面那句就是它的结论。
 *
 * ★ 这个值也是 sc_chat.c 里 `Msg.think[]` 的大小：每条消息都带一份，
 *   24 条静态占 24 KB。看着不小，但对这个程序可以忽略（字库就 5 MB），
 *   换来的是"随便点开哪一条的思考都能看"。 */
#define CHAT_THINK_MAX 1024

typedef struct {
    int  http_code;
    char error[192];      /* 失败原因（人话）。成功时为空串 */
    char reply[CHAT_REPLY_MAX];
    /* 模型的思考内容（**开了思考档位、且这家会返回它**时才非空）。
     *
     * 各家的字段名不一样，这里统一收口（见 chatapi.c 的 pick_reason）：
     *   · DeepSeek 系：`message.reasoning_content`（流式在 delta 上）
     *   · 部分别家/中转：`message.reasoning`
     * 都读不到就是空串 —— **空串是正常情况**（关掉思考档位、或那家不给），
     * 界面层不要把它当成错误，更不要因此弹提示。 */
    char reasoning[CHAT_THINK_MAX];
    /* 服务端给的会话 id（**目前只有豆包会填**，别家留空串）。
     *
     * 为什么要有：豆包的 samantha 接口里，conversation_id 传 "0" 就是
     * 「新建会话」，传上次那个 id 才是「接着聊」。不把它带出去的话，
     * 每一句都在服务端开一个新会话 —— 豆包每次都失忆，
     * 症状是它回答「我看不到你上一个任务」。
     * 调用方把这里的值回填进下一次请求的 ctx.conversation_id 即可。 */
    char conversation_id[32];
} ChatResult;

/* ---- 上下文（多轮对话 + 人设）----
 * 一组"之前说过的话"。调用方只管按顺序填，JSON 怎么拼是这里的事 ——
 * 让界面层去拼 messages 数组，等于把协议知识漏到 UI 里。
 *
 * text 是**借用**的指针（调用方自己的存储），本函数不复制、不释放；
 * 请求是同步做完的，调用期间保持有效即可。
 *
 * 顺序必须**从旧到新**，最后一条是"上一句"。 */
typedef struct {
    bool        mine;     /* true = 用户说的；false = AI 说的 */
    const char *text;
} ChatTurn;

/* 一次请求要带的全部"背景"。
 *
 * 收成一个结构体，而不是继续往参数表上加：人设与历史本质是同一类东西
 * （都是"这次额外告诉模型的事"），而且以后还可能有 temperature 之类 ——
 * 参数表再长，调用点就容易传错位置，而**两个同类型的指针参数调换顺序
 * 编译器不会吭声**，只会安静地产生错误请求。 */
typedef struct {
    const char     *system_prompt;  /* NULL 或空串 = 不发 system 消息 */
    const ChatTurn *turns;          /* 从旧到新，最后一条是"上一句" */
    int             n_turns;
    /* 续聊用：上一个会话的服务端 id。NULL/空串 = 让服务端新建一个。
     * 只有豆包认这个字段（别家走 OpenAI 格式，压根没有会话概念），
     * 所以别家传了也只是被忽略 —— 不用为它写分支。 */
    const char     *conversation_id;
    /* 回复长度上限（max_tokens）。0 = 不发这个字段，由服务商决定。
     * 由界面按"回复长度"档位映射，协议细节留在 chatapi.c。 */
    int max_tokens;
} ChatCtx;

/* provider_name 传 NULL 就用当前默认那家。
 * ctx 可以是 NULL（等于"没有上下文"，即老行为）。
 * user_text 是**本次**要说的话，不要把它也塞进 ctx->turns —— 会重复。
 *
 * ★ thinking 是**思考强度档位**：0 关 / 1 低 / 2 中 / 3 高（见 CONFIG_THINK_*）。
 *   传进来的只是个档位，**怎么翻译成请求参数是这里的事** ——
 *   因为各家写法不一样（见 provider.h 的 ProviderThink）。
 *   这家不支持（PROVIDER_THINK_NONE）或档位为 0 时，请求体里一个多余字段都不加。
 *
 * 成功返回 true，reply 里是回复正文。 */
bool chat_completion(const char *provider_name, const char *model,
                     int thinking,
                     const char *apikey, const ChatCtx *ctx,
                     const char *user_text,
                     ChatResult *out);

/* ---- 流式 ----
 * on_delta 每拿到一段增量文本就被调一次（绝大多数情况是一两个 token）。
 *   · delta 是**以 '\0' 结尾的 UTF-8 片段**，可能以半个多字节字符结尾 ——
 *     SSE 的切点不认字符边界，调用方自己按字节追加即可，拼起来才是完整文本。
 *   · ★ reasoning = 这一段属于**思考内容**（不是要用户读的正文）。
 *     开了思考档位的模型会**先吐一大段思考、再吐正文**，两股流在同一个
 *     回调里出来，靠这个标志区分。调用方通常要分开累积/分开展示 ——
 *     混在一起会让用户以为 AI 在回答里自言自语。
 *   · 返回 false 表示"我不想再要了"（比如用户按了 B），请求会被中止；
 *     此时函数仍返回 true，reply 里是**已经收到的部分**。
 *   · 回调在 HTTP 读取循环里同步调用 —— 不要在里面做耗时操作。
 *
 * 返回 true = 拿到了内容（可能被调用方中途叫停），out->reply 是全文。
 * 返回 false = 没拿到内容，out->error / out->http_code 说明原因，
 *              调用方应当据此决定要不要退回 chat_completion()。
 *
 * 实现细节：读到的正文只保留尾部若干字节用于 SSE 行解析，同时把完整
 * 回复写进 out->reply（上限 CHAT_REPLY_MAX，超出就停止累积但继续读干净
 * body —— 关 context 前必须读干净，否则 httpcCloseContext 会卡住）。 */
typedef bool (*ChatStreamFn)(const char *delta, bool reasoning, void *user);

bool chat_completion_stream(const char *provider_name, const char *model,
                            int thinking,
                            const char *apikey, const ChatCtx *ctx,
                            const char *user_text,
                            ChatStreamFn on_delta, void *user,
                            ChatResult *out);

/* ---- 关于请求体装不下这件事 ----
 * req_body 只有 4KB，而上下文是可以无限长的。所以 build_request
 * 会**从最新往回**挑能装下的那几条，装不下的丢最早的 ——
 * 近的比远的要紧：用户说"你刚才说的再展开讲讲"，靠的就是最近两轮，
 * 而十轮之前的内容对当下这轮几乎没用，却会把预算吃光。
 *
 * 保留优先级：user_text > system_prompt > 最近的历史 > 更早的历史。
 * 人设排在历史前面，是因为它定义了"这个助手是谁"——
 * 把它挤掉换来多一轮旧对话，得不偿失。
 * 上下界见 chatapi.c 的 REQ_BUDGET / CTX_MAX_TURNS。
 *
 * ⚠️ **已知的真实缺陷（2026-10-18 定位，未修）**：
 *   sc_chat.c 会把整份 AGENT.md 运行手册（约 21.7 KB）拼进 system_prompt，
 *   而 req_body 只有 4 KB。于是：
 *     · 前缀一开始就超出 REQ_BUDGET → 历史**永远 0 条**；
 *     · 请求体在 ~4095 B 处被 escape_into/snprintf 静默截断，
 *       收尾的 "}],"stream":…} 根本没写进去 → 服务端拿到的是**半个 JSON**。
 *   症状是"人设和历史全丢了 + 服务商报解析错误"。
 *   修法有三个方向（都在文档里记着，等定夺）：① 手册改走卡上文件按需摘要；
 *   ② req_body 加大并改成分段发送；③ 手册只发与本次相关的片段。
 *   在这之前，对话页那个环会把超标**如实显示成满环 + 弹窗报警** ——
 *   不粉饰，也不静默。 */

/* ---- 上下文用量（对话页那个环 + 点它的弹窗）----
 * 环要回答"这次会带多少上下文、离上限还有多远"，而且必须在**发送前**
 * 就画得出来（用户打字时就想看），所以只能是预估。
 *
 * 预估能和实际对上的原因：前缀走 build_prefix、历史挑选走 ctx_pick_start，
 * 两个都是 build_request 正在用的**同一个函数**，不是照着抄一遍公式。
 * 抄一遍的后果是"预估绿、实际红"而用户看不出来。 */
int chat_ctx_budget(void);

typedef struct {
    int sys_bytes;   /* 请求体前缀字节（含 system 消息，即人设+记忆+语言+手册+动作结果） */
    int n_turns;     /* 真正会带上的历史条数（0 = 一条都装不下） */
    int turn_bytes;  /* 这些历史的估算字节（含 JSON 壳，strlen*2+40/条） */
    int user_bytes;  /* 本次这句（strlen*2，与 build_request 同口径） */
    int total;       /* 合计（含收尾余量 48） */
} ChatCtxEst;

/* model 传 NULL = 用当前默认服务商的默认模型（与 build_request 同规则）。
 * system_prompt 传调用方**真正要发的那份**（sc_chat.c 里拼好的那个 sp）。
 * 全部参数都可为 NULL/0，函数不崩、只是算出个空账。 */
void chat_ctx_estimate(const char *model, int max_tokens,
                       const char *system_prompt,
                       const ChatTurn *turns, int n_turns,
                       const char *user_text, ChatCtxEst *out);

/* 上一条**真实**请求的字节数 / 是否触到 req_body 上限被截断。
 * 环是预估，这两个是事实；弹窗里两个一起给，让用户自己看出来对不对得上。
 * 返回 0 = 还没发过 —— 演示与豆包不走 build_request，永远是这个值。 */
int  chat_last_request_len(void);
bool chat_last_request_capped(void);
