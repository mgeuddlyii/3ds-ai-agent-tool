/*
 * provider.c -- 见 provider.h
 *
 * 顺序就是 UI 列表的顺序：把最该推荐的放在前面。
 * note 那几句是说给用户听的，写人话，别写参数。
 */

#include "provider.h"
#include "uistr.h"

#include <stdio.h>    /* snprintf —— 自定义项要把配置里的字符串拷进来 */
#include <string.h>

static const ProviderDef TABLE[] = {
    {
        .name          = "deepseek",
        .base_url      = "https://api.deepseek.com/v1",
        .default_model = "deepseek-chat",
        .auth          = "bearer",
        .note_id      = S0848,
        /* 官方「深度思考」文档：thinking.type + reasoning_effort（low/medium/high/max）。
         * 另注明 medium 会映射成 high —— 也就是说中档实际等于高档，但我们照发。 */
        .think         = PROVIDER_THINK_TYPE_EFFORT,
    },
    {
        .name          = "zhipu",
        .base_url      = "https://open.bigmodel.cn/api/paas/v4",
        .default_model = "glm-4-flash",
        .auth          = "bearer",
        .note_id      = S0849,
        /* glm-4-flash 这一档没有思考开关（带思考的是 4.5 以上的型号），
         * 明确标"不可调"，别让它看起来有个旋钮。 */
        .think         = PROVIDER_THINK_NONE,
    },
    {
        .name          = "ark",
        .base_url      = "https://ark.cn-beijing.volces.com/api/v3",
        .default_model = "doubao-seed-1.6",
        .auth          = "bearer",
        .note_id      = S0850,
        /* 方舟「深度思考」文档：thinking.type (enabled/disabled/auto)
         * 与 reasoning_effort 都支持，形状与 DeepSeek 相同。 */
        .think         = PROVIDER_THINK_TYPE_EFFORT,
    },
    {
        .name          = "moonshot",
        .base_url      = "https://api.moonshot.cn/v1",
        .default_model = "moonshot-v1-8k",
        .auth          = "bearer",
        .note_id      = S0851,
        /* moonshot-v1-8k 是纯对话模型，没有 reasoning 参数。 */
        .think         = PROVIDER_THINK_NONE,
    },
    {
        .name          = "dashscope",
        .base_url      = "https://dashscope.aliyuncs.com/compatible-mode/v1",
        .default_model = "qwen-plus",
        .auth          = "bearer",
        .note_id      = S0852,
        /* 百炼「OpenAI 兼容 - Chat」：enable_thinking + reasoning_effort。
         * ⚠️ 同一份文档写明 qwen3.8 系列**不能**同时设 reasoning_effort 与
         *    thinking_budget（会报错），所以这里只发前者。 */
        .think         = PROVIDER_THINK_ENABLE_EFFORT,
    },
    /* ---- 2026-10-23 用户要求「包括中外知名厂商」，下列为新补 ---- */
    {
        /* 硅基流动：国内最常用的 OpenAI 兼容转发，一把 Key 能打很多家模型。
         * 它同时也是本项目语音识别的默认通道（见 asr.c）。 */
        .name          = "siliconflow",
        .base_url      = "https://api.siliconflow.cn/v1",
        .default_model = "deepseek-ai/DeepSeek-V3",
        .auth          = "bearer",
        .note_id      = S0938,
        /* ⚠️ 硅基流动是**转发层**：能不能调思考取决于它背后那个模型，
         * 而用户可以在这家随便换模型名 —— 所以这里判不了，标 UNKNOWN。 */
        .think         = PROVIDER_THINK_UNKNOWN,
    },
    {
        .name          = "hunyuan",
        .base_url      = "https://api.hunyuan.cloud.tencent.com/v1",
        .default_model = "hunyuan-turbos-latest",
        .auth          = "bearer",
        .note_id      = S0939,
        /* 混元的思考参数没有权威文档写进 OpenAI 兼容端点，**未查证**。 */
        .think         = PROVIDER_THINK_UNKNOWN,
    },
    {
        .name          = "minimax",
        .base_url      = "https://api.minimax.chat/v1",
        .default_model = "abab6.5s-chat",
        .auth          = "bearer",
        .note_id      = S0940,
        /* abab6.5s-chat 没有 reasoning 参数。 */
        .think         = PROVIDER_THINK_NONE,
    },
    {
        .name          = "openai",
        .base_url      = "https://api.openai.com/v1",
        .default_model = "gpt-4o-mini",
        .auth          = "bearer",
        .note_id      = S0941,
        /* ★ 这里必须按**默认模型**判，不能按厂商判：
         *   gpt-4o-mini 不认 reasoning_effort（只有 o 系列 / gpt-5 才认）。
         *   按厂商一律标"能调"，用户换到 gpt-4o-mini 就会收到 400。 */
        .think         = PROVIDER_THINK_NONE,
    },
    {
        /* Gemini 的原生接口不是 OpenAI 形状，但它提供了 /v1beta/openai/ 兼容端点 ——
         * 必须用这个，因为本项目只发 OpenAI 形状的请求体。 */
        .name          = "gemini",
        .base_url      = "https://generativelanguage.googleapis.com/v1beta/openai",
        .default_model = "gemini-2.0-flash",
        .auth          = "bearer",
        .note_id      = S0942,
        /* gemini-2.0-flash 的思考是靠 generationConfig.thinkingBudget，
         * 而**本程序只发 OpenAI 形状的请求体**，这个兼容端点不认 reasoning_effort。 */
        .think         = PROVIDER_THINK_NONE,
    },
    {
        .name          = "grok",
        .base_url      = "https://api.x.ai/v1",
        .default_model = "grok-2-latest",
        .auth          = "bearer",
        .note_id      = S0943,
        /* grok-2 没有 reasoning 参数（grok-3-mini 才有），默认模型是 grok-2。 */
        .think         = PROVIDER_THINK_NONE,
    },
    {
        .name          = "mistral",
        .base_url      = "https://api.mistral.ai/v1",
        .default_model = "mistral-small-latest",
        .auth          = "bearer",
        .note_id      = S0944,
        /* mistral-small 没有 reasoning 参数（带推理的是 magistral 系列）。 */
        .think         = PROVIDER_THINK_NONE,
    },
    {
        /* OpenRouter：一把 Key 通上百个模型，含 Claude 系列。
         * Claude 原生接口是 x-api-key + 不同 body，本项目不支持；
         * 走 OpenRouter 是**唯一**能用上 Claude 的路。 */
        .name          = "openrouter",
        .base_url      = "https://openrouter.ai/api/v1",
        .default_model = "anthropic/claude-3.5-sonnet",
        .auth          = "bearer",
        .note_id      = S0945,
        /* 又是转发层（一把 Key 通上百个模型），后台是哪个模型才知道能不能调 ——
         * 判不了就 UNKNOWN，別假装能。 */
        .think         = PROVIDER_THINK_UNKNOWN,
    },
    {
        /* 豆包。**唯一一个不需要 API Key 的服务商** —— 它靠扫码登录，
         * 用的是账号本身（所以对话会同步进手机端豆包的历史记录，
         * 这正是它相对"调个模型 API"的价值）。
         *
         * 它也是唯一一个**非 OpenAI 格式**的，靠 fmt 字段分流
         * （见 provider.h 与 doubao.h）。 */
        .name          = "doubao",
        .base_url      = "https://www.doubao.com",
        .default_model = "doubao",
        .auth          = "cookie",
        .note_id      = S0853,
        .fmt           = PROVIDER_FMT_DOUBAO,
        /* 豆包走的是**网页登录态**（cookie），不是 API —— 网页版没有
         * "思考强度"这个可传参数，明确标不可调。 */
        .think         = PROVIDER_THINK_NONE,
    },
    {
        /* ★★ 自定义：网址与模型都由**用户**填（config.ai.base_url / model）。
         * 这里这一项只是"出现在列表里"用的占位；真正查它时走 provider_find()，
         * 那里会用配置里的值现组一个 ProviderDef（表本身是 const，改不了）。
         *
         * ⚠️ 它的 base_url 故意留空，而"空网址"以前是离线的判据 ——
         * 所以离线判据已经改成 offline 字段。别改回去。
         *
         * 放在 demo 之前：demo 必须在表尾，因为 provider_default() 取 TABLE[0]。 */
        .name          = "custom",
        .base_url      = "",
        .default_model = "",
        .auth          = "bearer",
        .note_id      = S0854,
        /* 自定义按"兼容 OpenAI"处理 —— 这也是 g_custom 里默认值的来源。
         * 用户自己填的服务商，他比我们更清楚能不能调；给 EFFORT 是让这个
         * 旋钮**可用**，而不是替他判死。真发错了服务端会回 400，界面上有原话。 */
        .think         = PROVIDER_THINK_EFFORT,
    },
    {
        /* 离线演示。**base_url 为空字符串**是这里的约定信号：
         * chatapi 看见空地址就不走网络，改用内置剧本（见 chatapi.c）。
         * 放表尾是为了不动 provider_default() —— 它取的是 TABLE[0]，
         * 把 demo 挪到前面会让"没配过"的机器默认进演示模式。 */
        .name          = "demo",
        .base_url      = "",
        .default_model = "demo",
        .auth          = "none",
        .note_id      = S0855,
        /* ★ 唯一的离线判据。**别再用"网址为空"判** —— 自定义服务商
         * 还没填网址时也是空串，那样判会把它静默当成演示。 */
        .offline       = true,
        /* 演示模式是内置剧本，根本没有"思考"这回事 → 明确不可调。
         * （原来这里没写，落到默认 NONE 恰好也对；但**默认值不该当答案用**。） */
        .think         = PROVIDER_THINK_NONE,
    },
};
/* 取说明文案。表里存编号，这里换成人话 ——
 * 这样"翻不翻"只取决于 uistr 的表，而表结构不用随语言变。 */
bool provider_think_capable(const ProviderDef *p)
{
    if (!p)
        return false;
    return p->think == PROVIDER_THINK_EFFORT
        || p->think == PROVIDER_THINK_TYPE_EFFORT
        || p->think == PROVIDER_THINK_ENABLE_EFFORT;
}

ProviderThink provider_think_kind(const ProviderDef *p)
{
    /* 空指针按"未查证"处理：调用方拿不到厂商时，界面该灰着，
     * 而不是亮着等人点。 */
    return p ? p->think : PROVIDER_THINK_UNKNOWN;
}

const char *provider_note(const ProviderDef *p)
{
    if (!p || !p->note_id)
        return "";
    return uistr(p->note_id);
}


#define TABLE_N ((int)(sizeof TABLE / sizeof TABLE[0]))

/* ---------------------------------------------------------------------------
 * 「自定义」服务商
 *
 * TABLE 是**编译期常量**，而"用户自己填网址"这一项的值必须来自配置 ——
 * 这两件事不能混进同一张表，否则下次有人加表项时就**看不出哪一条的值是可信的**。
 * 所以自定义项单独放在这里，由 provider_find / provider_at 在命中时改道过来。
 *
 * 谁来喂：**UI 层**。shared/ 看不到 app_cfg()（那是 app 层的东西），
 * 所以照抄 asr_set() 那套做法 —— UI 把配置解析好再塞进来。
 * 调用点就在 sc_chat.c 的 chat_provider()：那是"这次请求用哪家"的唯一定夺处。
 *
 * ⚠️ 漏喂的后果是**静默的**：base_url 为空 → 请求发不出去，
 *    而"空网址"以前又正好是离线演示的判据……所以离线判据已经改成
 *    offline 字段（见 provider.h）。两道都要有，不能只靠一道。
 * ------------------------------------------------------------------------- */

static char        g_custom_url[96];      /* 与 config 里 AiCfg.base_url 同宽 */
static char        g_custom_model[48];
static ProviderDef g_custom;
static bool        g_custom_built;

static bool is_custom_name(const char *name)
{
    return name && strcmp(name, "custom") == 0;
}

static const ProviderDef *custom_def(void)
{
    if (!g_custom_built) {
        /* 从没喂过也要给一个**形状正确**的结构：name 认得出、
         * offline=false（免得被当成演示）、base_url 指向空串。
         * 这样"忘了喂"的表现是"发不出去 + 界面显示没配网址"，
         * 而不是"莫名其妙演了一段假回复"。 */
        provider_set_custom("", "");
    }
    return &g_custom;
}

void provider_set_custom(const char *base_url, const char *model)
{
    snprintf(g_custom_url,   sizeof g_custom_url,
             "%s", base_url ? base_url : "");
    snprintf(g_custom_model, sizeof g_custom_model,
             "%s", model ? model : "");

    g_custom.name          = "custom";
    g_custom.base_url      = g_custom_url;
    g_custom.default_model = g_custom_model;
    g_custom.auth          = "bearer";
    g_custom.note_id       = S0854;
    g_custom.fmt           = PROVIDER_FMT_OPENAI;
    g_custom.offline       = false;
    /* 自定义按"兼容 OpenAI 接口"处理，所以只发标准的 reasoning_effort。
     * 不猜别家的私有参数 —— 用户填的网址背后是谁我们都不知道。 */
    g_custom.think         = PROVIDER_THINK_EFFORT;
    g_custom_built         = true;
}

int provider_count(void) { return TABLE_N; }

const ProviderDef *provider_at(int i)
{
    if (i < 0 || i >= TABLE_N)
        return NULL;
    /* 列表里的「自定义」那一行也要显示**用户实际填的**网址与模型，
     * 否则界面上写着"自定义"、地址栏却是空的，用户没法确认自己填进去没有。 */
    if (is_custom_name(TABLE[i].name))
        return custom_def();
    return &TABLE[i];
}

const ProviderDef *provider_default(void) { return &TABLE[0]; }

const ProviderDef *provider_find(const char *name)
{
    if (!name || !*name)
        return provider_default();

    for (int i = 0; i < TABLE_N; ++i) {
        if (!strcmp(TABLE[i].name, name))
            return is_custom_name(name) ? custom_def() : &TABLE[i];
    }
    return provider_default();
}
