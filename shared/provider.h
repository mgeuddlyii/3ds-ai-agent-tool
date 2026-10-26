/*
 * provider.h -- 服务商内置表
 *
 * 设计规划.md §5.4 的那张表落到代码里。所有厂商走同一个
 * POST /chat/completions，只切 base_url + model + 鉴权头。
 *
 * 表是**编译期常量**：用户能改的是"用哪家"（写进 config.ai.provider）
 * 和"模型叫什么"，但改不了地址本身 —— 那等于让配置文件决定
 * 把你的 Key 和对话内容发到哪里去。
 *
 * ★ 例外一：表尾的 demo 是"这次不发请求"，由 chatapi.c 换成内置剧本。
 *   它不是一家服务商，而是"还没有服务商"时的一条退路。
 *
 * ★★ 例外二（2026-10-16 加）：**custom**。「自己填网址」这一项是用户
 *   明确要求的，所以地址确实允许来自配置 —— 但边界守住了：
 *     · 只有**用户**能填（服务目录 / 引导页 / 电脑端写进来的）。
 *     · **AI 碰不到** —— actions.c 里没有任何一条改 ai.* 的路径。
 *   也就是说"模型只给数据、不能自己选主机"这条闸门依然成立。
 *
 * ⚠️⚠️ 因此**不要再拿"base_url 是空串"当离线判据**。原先 demo 就是这么判的
 *   （见上一版的注释），但自定义服务商在用户还没填网址时**也是空串** ——
 *   于是"选了自定义却没填"会被**静默当成离线演示**，用户看着一段假回复
 *   以为配通了。实测就是这个坑逼出了下面那个 explicit 的 offline 字段。
 *   判据统一改成 `p->offline`，两个调用点都在 chatapi.c。
 */

#pragma once

#include <stdbool.h>

/* 请求/响应**格式**。这张表里绝大多数走 OpenAI 那套
 * （POST {base_url}/chat/completions + SSE delta），但豆包不是 ——
 * 它有自己的 samantha 接口和三层 JSON 的 SSE（见 doubao.h）。
 *
 * 为什么要有这个字段，而不是靠名字判断：
 * provider.h 上面刚说过"别在别处硬编码 name=='demo'" —— 同一条道理。
 * 判名字的话，将来加第二个非 OpenAI 的源，就得在调用点再加一个条件，
 * 而漏掉的那个地方不会报错，只会用一个格式去解另一种响应。
 *
 * 默认值 0 = OPENAI，所以已有的表项一个字都不用改。 */
typedef enum {
    PROVIDER_FMT_OPENAI = 0,
    PROVIDER_FMT_DOUBAO,
} ProviderFmt;

/* 这家的"思考强度"能怎么表达。**四档（关/低/中/高）是 UI 的档位，
 * 各家接口的表达方式不一样**，所以翻译规则挂在这里。
 *
 * ★ 下面每一种都对着**官方文档查证过**（2026-10-16），不是猜的：
 *   · DEEPSEEK / ARK：「深度思考」文档
 *       {"thinking":{"type":"enabled"|"disabled"}} + {"reasoning_effort":"low"|"high"|"max"}
 *     DeepSeek 官方还写明：low/medium 都会映射成 high，xhigh 映射成 max。
 *   · QWEN：百炼「OpenAI 兼容 - Chat」
 *       extra_body={"enable_thinking":true,"reasoning_effort":"low"|"medium"|"xhigh"}
 *     ⚠️ qwen3.8 系列**不能**同时设 reasoning_effort 与 thinking_budget（会报错），
 *        所以这里只发 reasoning_effort，不发 budget。
 *   · EFFORT：OpenAI 标准参数 {"reasoning_effort":"low"|"medium"|"high"}。
 *       自定义服务商按兼容 OpenAI 处理，走这一档。
 *
 * ⚠️ 没查到权威说法的家（zhipu / moonshot）**一律标 NONE**，
 *    界面上直说"这家没有可调的思考档位"。**不许凭印象编一个参数** ——
 *    编错的后果是请求 400，而用户完全不知道为什么。 */
typedef enum {
    PROVIDER_THINK_NONE = 0,          /* **明确不可调**：这个模型根本没有该参数 */
    PROVIDER_THINK_EFFORT,            /* 只发 reasoning_effort（OpenAI 标准） */
    PROVIDER_THINK_TYPE_EFFORT,       /* thinking.type + reasoning_effort（DeepSeek/方舟） */
    PROVIDER_THINK_ENABLE_EFFORT,     /* enable_thinking + reasoning_effort（Qwen） */
    /* ★★ 2026-10-25 加的第三态：**未查证**。
     *
     *   用户要求：「每个厂商都要明确能否控制；确定不了的，选项页那行字变灰、
     *   点不动」。原来 NONE 把"明确没有这个参数"和"我没查到"混成一种，
     *   于是界面上没法告诉用户是哪一种 —— 而这两种对用户的意义完全不同：
     *   · NONE    = 换模型也一样，别找了；
     *   · UNKNOWN = 这一家我不确定，你可以告诉我或者自己查。
     *   两者在 UI 上**都不可点**（宁可少给一个旋钮，也不要让它看起来能用），
     *   区别只在提示文字。 */
    PROVIDER_THINK_UNKNOWN,
} ProviderThink;

typedef struct ProviderDef {
    const char *name;
    const char *base_url;
    const char *default_model;
    const char *auth;        /* "bearer" = Authorization: Bearer <key>；
                              * "cookie" = 不用 Key，靠登录态（豆包） */
    /* ★ 存**编号**而不是字符串：这张表是静态初始化，而 S() 是函数调用
     * （编译器报 "initializer element is not constant"，却指着 uistr.h）。
     * 取值统一走 provider_note()。 */
    short       note_id;     /* 给 UI 显示的一句说明（文案编号，0 = 没有） */
    ProviderFmt fmt;         /* 见上。不写就是 PROVIDER_FMT_OPENAI */
    /* 离线条目（只发内置剧本，不联网）。**这是唯一的离线判据** ——
     * 别再用"base_url 是空串"判，自定义服务商没填网址时也是空串。
     * 不写就是 false，所以已有表项一个字都不用改。 */
    bool        offline;
    /* 思考强度怎么翻译（见 ProviderThink）。不写 = PROVIDER_THINK_NONE，
     * 也就是"这家不可调"——保守的默认值，宁可界面上少给一个旋钮，
     * 也不要对着不支持的接口发一个没人认的参数。 */
    ProviderThink think;
} ProviderDef;

/* 这家的思考强度**能不能调**（能做 UI 判据的唯一入口）。
 *   true  仅当表里明确写了一个具体翻译方式（EFFORT / TYPE_EFFORT / ENABLE_EFFORT）；
 *   false 覆盖 NONE（明确不可调）与 UNKNOWN（未查证）—— 两种都**不许点**。 */
bool provider_think_capable(const ProviderDef *p);

/* 不用点、但要能说清原因时用它：返回 NONE / UNKNOWN / 具体方式。 */
ProviderThink provider_think_kind(const ProviderDef *p);

/* 取服务商的一句说明（按当前界面语言）。表里存编号，见 note_id。 */
const char *provider_note(const ProviderDef *p);

int  provider_count(void);
const ProviderDef *provider_at(int i);

/* 找不到就返回默认那家（DeepSeek），不返回 NULL ——
 * 调用方不必到处判空，unknown 名字的后果是"用默认"，不是崩溃。 */
/* ★ 按名字找服务商。**找不到时返回默认那一家，而不是 NULL** ——
 * 这是刻意的（调用方多数只想"给我一家能用的"，不想处理失败），
 * 但它有一个**必须知道的陷阱**：
 *
 *     if (!provider_find(name)) ...      // ← 永远不成立，等于没校验
 *
 * 想知道"这个名字认不认识"，要**比名字**而不是判空：
 *     const ProviderDef *p = provider_find(name);
 *     if (!p || strcmp(p->name, name) != 0) { ... }   // 不认识就是这个名字
 *
 * 2026-10-06 实测踩过：svc.set 的白名单校验写成判空，于是
 * `name=nope`（根本不存在的一家）畅通无阻。 */
const ProviderDef *provider_find(const char *name);
const ProviderDef *provider_default(void);

/* 把「自定义」服务商的网址与模型喂进来（provider == "custom" 时才用得上）。
 *
 * ★ 为什么是 setter 而不是让 provider.c 自己去读配置：shared/ 看不到 app_cfg()
 *   （它在 app 层）。这正是 asr_set() 的同一套做法。
 * ★ 谁来调：sc_chat.c 的 chat_provider() —— "这次请求用哪家"的唯一定夺处。
 *   漏调的后果是自定义服务商发不出请求（base_url 为空），
 *   而界面会显示"没配网址"，不会静默变成离线演示（离线判据是 offline 字段）。
 */
void provider_set_custom(const char *base_url, const char *model);
