/*
 * asr.c -- 见 asr.h
 *
 * ── 一次请求长什么样 ────────────────────────────────────────────
 *   POST <url>                       multipart/form-data
 *     part 1  file        audio/wav  ← 我们现场拼 44 字节 WAV 头 + 原始 PCM
 *     part 2  model                  ← 厂商的模型 id
 *     part 3  language               ← 中文场景
 *   Authorization: Bearer <key>
 *
 * 现场拼 WAV 头而不是直接喂裸 PCM，是因为各家 /audio/transcriptions 的
 * 探测顺序不一样：有些看 Content-Type、有些看文件名扩展名、有些干脆
 * 顺序试。给一个**完整合法的 16kHz 单声道 16bit WAV** 是唯一不靠猜的写法。
 *
 * ── 为什么必须支持二进制 body ───────────────────────────────────
 * 旧的 HxReq.post 是 const char*，curl 那边按 strlen 取长度；
 * 而音频里 NUL 字节多得是（静音段就是一串 0），一截就成了半段 WAV，
 * 服务端报的错却是"这个文件格式不对"—— 指向完全错误的方向。
 * 所以 httpx 加了 post_raw / post_raw_len 这一对，老字段原样保留。
 */

#include "asr.h"

#include "httpx.h"
#include "voice.h"

/* 本文件用到 u8/u16（写 WAV 头要按字节塞），而 <3ds.h> 之外没人给这个 typedef。
 * ⚠️ 只 include <3ds/types.h> 也行，但别把 <3ds.h> 删了再靠别处的传递包含 ——
 *    voice.c 里它顺手拿到了 micInit / svcGetSystemTick，两边要保持一致。 */
#include <3ds.h>

#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* ★★ 内置的语音识别 Key（2026-10-17 用户要求"别人也能直接用"）。
 *
 * 取舍必须说清楚：
 *   · 好处：装上就能按住 ZL 说话，不用先教用户去注册什么平台。
 *   · 代价：**这是明文写在二进制里的**。任何人解开 .cia/.3dsx 都能拿到它，
 *     也就是说这把 Key 会变成"公共的"。额度、速率限制、甚至账号风险，
 *     都由 Key 的主人承担 —— 这不是技术缺陷，是内置 Key 的必然属性。
 *
 * 因此优先级是：**asr.txt 里的 key 覆盖这个内置值**。
 * 想用自己的 Key（或怀疑内置的被刷爆了），写 asr.txt 即可，不用重编程序。
 *
 * ★ 2026-10-23：这把 Key 是**故意**随源码公开的（用户确认：该账号余额为 0，
 *   仅供识别用，被刷也不影响任何付费资源）。所以它留在源码里，不另做抽离。
 *   ⚠️ 别把它当成"我们忘了删的密钥"而清空 —— 清空会让"按住 ZL 说话"
 *   直接退回"没配 Key"，而普通用户根本不知道要配什么。
 *   真正的付费 Key（对话用的那几把）**一律不进仓库**，见 .gitignore 的说明。 */
#define ASR_BUILTIN_KEY "sk-bjeblakmrjwvqvhliqpwzpeqhrgjjzniwipuuutkpreionvq"

/* 编译期白名单。**只能往这儿加**，配置文件改不了里面的 url。 */
static const AsrDef kAsrTable[] = {
    {
        "siliconflow",
        "https://api.siliconflow.cn/v1/audio/transcriptions",
        /* ★ 用免费的那个（2026-10-17 用户确认 XingChenASR-V3.2-Ultra 免费）。
         *   实测 HTTP 200，返回 `{"duration":1.0,"text":"","usage":{...}}` ——
         *   结构与 SenseVoiceSmall 完全兼容，**解析代码一行都不用改**。
         *   选 Ultra 而不是 Diarize 版：后者带 speaking 分段（我们单人语音输入用不上），
         *   Ultra 是同系列里识别质量最高的一档，而两者都免费。 */
        "XingChenAGI/XingChenASR-V3.2-Ultra",
        "zh",
    },
    /* ⚠️⚠️ "openai" 这一项留着，但它**配了 Key 也用不成**：
     * api.openai.com 在当前网络下 TCP 直接超时（本机探针实测
     * connect timed out，而 api.siliconflow.cn 是 DNS+TCP 0.09s+TLS1.3 全通）。
     * 3DS 上跑出来的现象是 curl -28，屏上只说一句"没连上" ——
     * 用户会以为是网络坏了、去查代理，其实 provider 从一开始就不该选。
     * 所以：**别把 asr.txt 的 provider 配成 openai**，除非你所在的
     * 网络能直连 api.openai.com。
     * （保留在表里是为了白名单的可读性 —— 删掉又会让"表内可选"
     *   变成"只有一个能选"，将来换了网络还得再改一次代码。） */
    {
        "openai",
        "https://api.openai.com/v1/audio/transcriptions",
        "whisper-1",
        "zh",
    },
};
#define ASR_TABLE_N ((int)(sizeof kAsrTable / sizeof kAsrTable[0]))

#define ASR_DEFAULT_NAME "siliconflow"

/* boundary 里不能有非字母数字（RFC 2046 限得死），所以是纯字母 */
#define ASR_BOUNDARY "3dsAICONSOLEasr0"

static char g_provider[24];              /* 空 = 用默认那家 */
static char g_key[ASR_KEY_MAX];

/* ---- 请求体缓冲 ----
 * 容量必须按 **VOICE_CAP_BYTES（满 5 秒录音）** 算，不能拍脑袋给几 KB：
 * 以前写 4096+64+44+… 只有 5KB 出头，而 asr_transcribe 会把整段 PCM
 * memcpy 进来 —— 这是**静态数组的静态溢出**，arm-none-eabi-gcc 不会拦，
 * 真机上就是"录满 5 秒必崩、短一点没事"，最难归的一种。
 * 上界 = boundary 头 + 空行余量 + 44 字节 WAV 头 + 录音 + 尾部字段。 */
#define ASR_BODY_HEAD  256
#define ASR_BODY_TAIL  512
static u8 g_body[VOICE_CAP_BYTES + ASR_BODY_HEAD + ASR_BODY_TAIL];

/* 上一次发送的自检信息。真机出错时**必须能回答"我到底发了什么"** ——
 * 否则只能对着屏幕上一句服务端的"参数不合法"瞎猜（2026-10-22 就卡在这）。 */
static int  g_last_body_len;
static int  g_last_model_len;
static int  g_clamped;

int asr_last_body_len(void)  { return g_last_body_len; }
int asr_last_model_len(void) { return g_last_model_len; }
int asr_last_clamped(void)   { return g_clamped; }

static char g_resp[4096];
static size_t g_resp_len;

static const char *asr_boundary(void) { return ASR_BOUNDARY; }

void asr_reset(void)
{
    g_provider[0] = '\0';
    /* 先填内置 Key；asr.txt 若给了 key 会覆盖它（见 asr_load 的解析）。
     * 这样"没配文件"= 用内置（能用），"配了文件"= 用用户的（更安全）。 */
    snprintf(g_key, sizeof g_key, "%s", ASR_BUILTIN_KEY);
}

const AsrDef *asr_current(void)
{
    for (int i = 0; i < ASR_TABLE_N; ++i) {
        if (strcmp(g_provider, kAsrTable[i].name) == 0)
            return &kAsrTable[i];
    }
    for (int i = 0; i < ASR_TABLE_N; ++i) {
        if (strcmp(kAsrTable[i].name, ASR_DEFAULT_NAME) == 0)
            return &kAsrTable[i];
    }
    return &kAsrTable[0];
}

const char *asr_key(void) { return g_key; }

bool asr_set(const char *provider, const char *key)
{
    bool known = false;

    for (int i = 0; i < ASR_TABLE_N; ++i) {
        if (provider && strcmp(provider, kAsrTable[i].name) == 0)
            known = true;
    }
    if (!known)
        return false;                    /* 表里没有就**不 partial 生效** */

    /* 两处都写全，哪怕原串是空的 —— 留半截状态（provider 换了、key 没换）
     * 会变成"用 A 家的地址发 B 家的 Key"，那是最难归因的一种错。 */
    if (provider)
        snprintf(g_provider, sizeof g_provider, "%s", provider);
    else
        g_provider[0] = '\0';
    g_key[0] = '\0';
    if (key)
        snprintf(g_key, sizeof g_key, "%s", key);
    return true;
}

bool asr_load_file(const char *path)
{
    FILE *f;

    if (!path)
        return false;

    /* ★ asr_reset() 必须在 fopen **之前**。
     *
     * 2026-10-17 用户实测「语音模块显示 Key 还没进去」，根因就是这一步
     * 原来的位置：asr_reset 原先放在 fopen 成功之后，于是**卡上没有
     * asr.txt 时直接 return，内置 Key 永远填不进去** ——
     * asr_transcribe 看到 g_key 是空串，只会回一句
     * 「没配语音 Key：建 SD:/3ds/aiconsole/asr.txt」。
     * 而"别人装上就能用"这个需求恰恰建立在**没有 asr.txt** 的机器上
     * （普通用户不会去建这个文件），等于内置 Key 对真实用户全部失效。
     *
     * 放在这里之后：没文件 = 内置 Key + 默认服务商（开箱能用）；
     * 有文件 = 文件覆盖（想换自己的 Key 就写 asr.txt）。 */
    asr_reset();

    f = fopen(path, "r");
    if (!f)
        return false;                    /* 没有配置文件：用内置的，不是错误 */

    /* **先全读完再统一生效** —— 一路读一路 set 的话，
     * provider 写在 key 后面（或反过来）就会把先读到那一项冲掉，
     * 变成"用 A 家的地址发 B 家的 Key"，而屏上只有"识别失败"四个字。 */
    char line[ASR_KEY_MAX + 48];
    char lp[24];
    char lk[ASR_KEY_MAX];

    lp[0] = '\0';
    lk[0] = '\0';

    while (fgets(line, (int)sizeof line, f)) {
        char *p = line;
        char *eq;
        char *val;
        size_t n;

        while (*p == ' ' || *p == '\t')
            ++p;
        if (*p == '#' || *p == '\n' || *p == '\r' || !*p)
            continue;
        eq = strchr(p, '=');
        if (!eq)
            continue;

        *eq = '\0';
        val = eq + 1;
        n   = strlen(val);
        while (n && (val[n - 1] == '\n' || val[n - 1] == '\r'))
            val[--n] = '\0';

        if (strcmp(p, "provider") == 0)
            snprintf(lp, sizeof lp, "%s", val);
        else if (strcmp(p, "key") == 0)
            snprintf(lk, sizeof lk, "%s", val);
        /* 别的键一律忽略：这个文件是给人手敲的，
         * 写坏一行不该让整个语音功能跟着哑掉。 */
    }
    fclose(f);

    /* 文件里**没写 key 行**时保留内置 Key（asr_reset 刚填的那个）。
     *
     * 原来这里传的是 `lk[0] ? lk : NULL`，而 asr_set 见 NULL 就把 key
     * 清成空串 —— 于是"asr.txt 里只写了 provider"的机器反而把内置 Key
     * 擦了，屏上照样一句"没配语音 Key"。没文件和没 key 行必须同罪：
     * 都退回内置的。
     *
     * ⚠️ 不能直接传 asr_key()：那个指针就是 g_key 本身，而 asr_set 里
     * 先 `g_key[0] = '\0'` 再 snprintf 拷贝 —— 传自己等于先把源清空
     * 再拷贝，结果一定是空串。必须先拷进局部缓冲。
     * （显式写 `key=` 空值也按"没写"处理：空 Key 没有任何用处，
     *   只会让语音彻底哑掉，不如退回内置的。） */
    char builtin[ASR_KEY_MAX];
    snprintf(builtin, sizeof builtin, "%s", asr_key());

    asr_set(lp[0] ? lp : NULL, lk[0] ? lk : builtin);
    return true;
}

/* ------------------------------------------------------------------
 * multipart 拼装
 * ---------------------------------------------------------------- */

static size_t put_str(char *p, size_t off, const char *s)
{
    size_t n = strlen(s);
    memcpy(p + off, s, n);
    return off + n;
}

static void put_le16(u8 *p, unsigned v)
{
    p[0] = (u8)(v & 0xFF);
    p[1] = (u8)((v >> 8) & 0xFF);
}

static void put_le32(u8 *p, unsigned long v)
{
    for (int i = 0; i < 4; ++i)
        p[i] = (u8)((v >> (8 * i)) & 0xFF);
}

/* 44 字节的 PCM16 / 单声道 / rate Hz WAV 头。 */
static void put_wav_hdr(u8 *p, unsigned long rate, unsigned long bytes)
{
    memcpy(p, "RIFF", 4);
    put_le32(p + 4, 36 + bytes);
    memcpy(p + 8, "WAVE", 4);
    memcpy(p + 12, "fmt ", 4);
    put_le32(p + 16, 16);        /* fmt chunk 长度 */
    put_le16(p + 20, 1);         /* PCM */
    put_le16(p + 22, 1);         /* 声道数 */
    put_le32(p + 24, rate);
    put_le32(p + 28, rate * 2);  /* 字节率 = rate × 声道 × 位宽/8 */
    put_le16(p + 32, 2);         /* 块对齐 */
    put_le16(p + 34, 16);        /* 位深 */
    memcpy(p + 36, "data", 4);
    put_le32(p + 40, bytes);
}

/* 尾部固定长度（model / language 两个字段 + 结束 boundary）。
 * 单独算一条是为了不联网就能量 body —— 见 asr_payload_probe。 */
static size_t asr_tail_len(const AsrDef *d)
{
    size_t n = 0;

    n += 64 + strlen(d->model) + 8 + 64;      /* model 字段 */
    if (d->lang)
        n += 64 + strlen(d->lang) + 8 + 64;   /* language 字段 */
    return n;
}

static size_t build_multipart(const AsrDef *d, const void *pcm, size_t n, u8 *body)
{
    char *p = (char *)body;
    char  b[128];
    size_t off = 0;

    snprintf(b, sizeof b, "--%s\r\n", asr_boundary());
    off = put_str(p, off, b);
    off = put_str(p, off,
                  "Content-Disposition: form-data; name=\"file\"; "
                  "filename=\"aiconsole.wav\"\r\n"
                  "Content-Type: audio/wav\r\n\r\n");

    put_wav_hdr((u8 *)p + off, 16360UL, (unsigned long)n);
    off += 44;
    memcpy(p + off, pcm, n);
    off += n;

    off = put_str(p, off, "\r\n");

    snprintf(b, sizeof b, "--%s\r\nContent-Disposition: form-data; "
                          "name=\"model\"\r\n\r\n", asr_boundary());
    off = put_str(p, off, b);
    off = put_str(p, off, d->model);
    off = put_str(p, off, "\r\n");

    if (d->lang) {
        snprintf(b, sizeof b, "--%s\r\nContent-Disposition: form-data; "
                              "name=\"language\"\r\n\r\n", asr_boundary());
        off = put_str(p, off, b);
        off = put_str(p, off, d->lang);
        off = put_str(p, off, "\r\n");
    }

    snprintf(b, sizeof b, "--%s--\r\n", asr_boundary());
    off = put_str(p, off, b);

    return off;
}

/* 不联网的拼装自检：一段 PCM 变成 body 要多少字节、装不装得下。
 *
 * 为什么值得单独做一条：**这段拼装出错了不会崩、也不会报"文件损坏"**，
 * 只会在真机上变成"识别失败，原因栏写 HTTP 400"。
 * 于是把它从网络依赖里剥出来，在 Azahar（那里麦克风和 ASR 都不可用）
 * 也能验到本地这一半是对的。 */
bool asr_payload_probe(size_t pcm_bytes, size_t *body_len)
{
    const AsrDef *d = asr_current();
    size_t need;

    if (pcm_bytes + 64 + 44 > sizeof g_body)
        return false;                 /* 缓冲不够：这是常数写错了 */
    need = 64 + 44 + pcm_bytes + asr_tail_len(d);
    if (need > sizeof g_body)
        return false;

    if (body_len)
        *body_len = need;
    return true;
}

const char *asr_content_type(void)
{
    static char buf[96];
    snprintf(buf, sizeof buf, "multipart/form-data; boundary=%s", asr_boundary());
    return buf;
}

/* ------------------------------------------------------------------
 * 响应解析：只要 {"text":"…"} 里的那段
 * ---------------------------------------------------------------- */

static size_t sink_body(const char *data, size_t len, void *user)
{
    (void)user;
    if (g_resp_len + len >= sizeof g_resp)
        len = sizeof g_resp - 1 - g_resp_len;
    memcpy(g_resp + g_resp_len, data, len);
    g_resp_len += len;
    g_resp[g_resp_len] = '\0';
    return len;
}

/* 找到 "text" 后面的 JSON 字符串。返回长度（0 = 没找到 / 语法不像）。 */
static size_t pick_text(const char *s, char *out, size_t cap)
{
    const char *k = "\"text\"";
    const char *p;

    if (!s)
        return 0;
    p = strstr(s, k);
    if (!p)
        return 0;
    p += strlen(k);
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r' || *p == ':')
        ++p;
    if (*p != '"')
        return 0;

    size_t n = 0;
    ++p;
    while (*p && *p != '"') {
        if (*p == '\\' && p[1]) {
            ++p;
            if (*p == 'n') {
                out[n < cap - 1 ? n : cap - 1] = '\n';
                ++n;
            } else if (*p == 'u') {
                /* 3DS 上不做 UTF-8 转码也没法在屏上显示，
                 * 落一个 '?' 至少不会把后面整段吃掉 */
                if (n < cap - 1) out[n++] = '?';
            } else {
                if (n < cap - 1) out[n++] = *p;
            }
            ++p;
            continue;
        }
        if (n < cap - 1)
            out[n++] = *p;
        ++p;
    }
    out[n < cap ? n : cap - 1] = '\0';
    return n;
}

void asr_transcribe(const void *pcm, size_t bytes, AsrResult *out)
{
    const AsrDef  *d;
    static char    auth[ASR_KEY_MAX + 32];
    static const char *hdrs[3];
    static char    errb[ASR_ERR_MAX];
    HxReq          req;
    long           http;
    size_t         n;
    size_t         tlen;

    memset(out, 0, sizeof *out);
    out->http  = -1;
    out->bytes = (int)bytes;

    if (!pcm || bytes < 2) {
        snprintf(out->err, sizeof out->err, "没有音频");
        return;
    }
    /* ★ 防御：VOICE_CAP_BYTES 是 voice.c 的输出上限，但**不要靠它保证**。
     *   build_multipart 里的 put_str/memcpy 都没有边界检查，
     *   一旦这里拿到更大的 bytes，就会直接写穿 g_body ——
     *   而后果是发出一个长度与内容不符的 body，服务端只会说"参数不合法"，
     *   让人以为是接口用法错了。宁可截断并如实记一笔。 */
    if (bytes > (size_t)VOICE_CAP_BYTES) {
        g_clamped = 1;
        bytes = (size_t)VOICE_CAP_BYTES;
    }
    if (!g_key[0]) {
        /* ★ 必须把**路径和格式**说全。"没配 ASR Key（看 asr.txt）"只说了一半：
         *   用户不知道 asr.txt 该放哪、里面写什么，而本 App **没有配置语音
         *   Key 的界面**（对话的 Key 在 key.txt / 服务目录里，语音是另一份），
         *   于是这句提示等于"你自己去猜"。2026-10-10 真机反馈的"连不上"
         *   很可能就是这一支 —— 卡上压根没有这个文件。
         *
         * ⚠️ out->err 只有 ASR_ERR_MAX(96) 字节，**写完要数字节**：
         *   超了 snprintf 会截断，而半截提示比"没连上"更难懂。
         *   本句 UTF-8 实测 83 字节（中文 3 字节/字），留 13 字节余量。 */
        snprintf(out->err, sizeof out->err,
                 "没配语音 Key：建 SD:/3ds/aiconsole/asr.txt"
                 "（provider=siliconflow + key=…）");
        return;
    }

    memset(&req, 0, sizeof req);

    d = asr_current();
    n = build_multipart(d, pcm, bytes, g_body);
    g_last_body_len  = (int)n;
    g_last_model_len = d && d->model ? (int)strlen(d->model) : -1;

    snprintf(auth, sizeof auth, "Authorization: Bearer %s", g_key);
    hdrs[0] = auth;
    hdrs[1] = asr_content_type();
    hdrs[2] = NULL;

    req.url          = d->url;
    req.post_raw     = g_body;
    req.post_raw_len = n;
    req.headers      = hdrs;
    /* 12 秒，别再往长了给。
     * 这是**阻塞在主循环里**的请求，超时期间整个界面一帧都动不了：
     * 25 秒的时候，用户按完 ZL 之后对着一块冻住的屏干等半分钟，
     * 观感就是"按了没反应 / 坏了"。卡在模拟器里跑回归时也会因此
     * 把后面所有脚本步骤一起拖掉（一帧只 tick 一次，wait N 帧
     * 实际要 N/帧率 + 阻塞时长）。12 秒足够一次 ASR 往返，
     * 超时了就明说"没连上"，别让读者猜。 */
    req.timeout_s    = 12;
    req.follow       = false;
    req.add_cookies  = false;
    req.on_new_response = NULL;

    g_resp_len = 0;
    g_resp[0]  = '\0';
    errb[0]    = '\0';

    http = hx_perform(&req, sink_body, NULL, errb, sizeof errb);

    if (http < 0) {
        /* ★ 这里必须把**是哪一家**和**哪一步**说清楚。
         *
         * 早先只说"没连上：curl -28"。2026-10-07 真机上用户报的就是
         * 「无法连接」，而他卡上 asr.txt 配的是 provider=openai ——
         * 那家在当前网络下 **TCP 直接超时**，所以永远连不上。
         * 只报"没连上"的话，排查方向必然跑到"是不是网络坏了 / 要不要
         * 开代理"，而真因是**服务商选错了**。两个方向的处置完全相反。
         *
         * -28 是 curl 的超时；用它加上 provider 名就能一句话定位：
         *   「连不上 openai（网络超时 · 换 siliconflow 或换网络）」。 */
        const AsrDef *d = asr_current();
        int   timedout = (http == -28);
        int   is_openai = d && !strcmp(d->name, "openai");

        out->http = http;
        /* out->err 只有 ASR_ERR_MAX(96) 字节 —— 句子要短。
         * 长句会被截成半截，而半截提示比"没连上"更难懂。 */
        if (is_openai && timedout)
            snprintf(out->err, sizeof out->err,
                     "openai 连不上（超时）· 请改用 siliconflow");
        else if (timedout)
            snprintf(out->err, sizeof out->err,
                     "%s 超时 · 检查 Wi-Fi",
                     (d && d->name) ? d->name : "服务商");
        else
            snprintf(out->err, sizeof out->err, "%s：%s",
                     (d && d->name) ? d->name : "服务商",
                     errb[0] ? errb : "curl 失败");
        return;
    }

    out->http = http;
    if (http != 200 && http != 201) {
        /* 服务商回了错误码 —— 那一多半是 **Key 不对或没配**，
         * 与"连不上"是完全不同的处置（一个改配置，一个查网络）。
         * 把 curl 的原因一并带出去，用户才不用去猜。 */
        const AsrDef *d = asr_current();
        const char  *nm = (d && d->name) ? d->name : "服务商";

        /* ★ 错误文案必须区分"Key 被拒"(401/403) 和"别的错"(400/404/5xx)。
         *
         *   早先这里对**任何**非 200 都写"（多半 Key 不对）"。2026-10-22 实测证明这句
         *   会把人带偏：同一个内置 Key、同一个模型，在 PC 上打该接口是 **HTTP 200**
         *   （{"duration":1.2,"text":"",...}）—— Key 与模型都是好的。
         *   于是真机上一旦出错，用户被指向"换 Key"，而真正的原因（音频格式、
         *   multipart 拼装、TLS）根本没被看见。
         *
         *   现在：401/403 才说 Key；其它码把**服务端原话**的前 100 字节带出来。
         *   服务端几乎总会回一句人能读的 JSON（{"code":...,"message":"..."}），
         *   那比我们猜一句有用得多。 */
        if (http == 401 || http == 403) {
            snprintf(out->err, sizeof out->err,
                     "%s 拒绝了 Key（HTTP %ld）。可在 SD:/3ds/aiconsole/asr.txt "
                     "写一行 key=你的Key（硅基流动）覆盖内置 Key",
                     nm, http);
        } else {
            char detail[120];
            size_t n = g_resp_len < sizeof detail - 1 ? g_resp_len : sizeof detail - 1;

            memcpy(detail, g_resp, n);
            detail[n] = '\0';
            for (n = 0; detail[n]; ++n)          /* 折成一行的效果：换行换成空格 */
                if (detail[n] == '\n' || detail[n] == '\r') detail[n] = ' ';
            if (n > 100) { detail[100] = '\0'; }

            snprintf(out->err, sizeof out->err, "%s 收 HTTP %ld：%s",
                     nm, http, n ? detail : "(服务端没给内容)");
        }
        return;
    }

    tlen = pick_text(g_resp, out->text, sizeof out->text);
    out->text_len = (int)tlen;
    out->truncated = (tlen > 0 && strlen(out->text) == ASR_TEXT_MAX - 1);
    out->ok = (tlen > 0);
    if (!out->ok)
        snprintf(out->err, sizeof out->err, "响应里没有 text");
}
