/*
 * asr.h -- 语音识别（ASR）：把采到的 PCM 换成文字
 *
 * ── 为什么 ASR 要单独一张表，而不是复用聊天那张 provider 表 ──────
 * chatapi 那张表是**对话模型**的（DeepSeek / 智谱 / Kimi / 豆包…），
 * 而语音识别走的是**另一套接口**：POST 一个音频文件上去，拿回 {"text":…}。
 * 除了 OpenAI 系那几家，大多数厂商的 ASR 参数（模型 id、语言字段）都不同，
 * 硬塞进同一张表里只能靠 name 字符串给它们分叉 ——
 * 而本项目已经明确禁止"靠名字判断格式"（见 provider.h 的 PROVIDER_FMT）。
 *
 * ── 表和地址都是**编译期常量** ──────────────────────────────────
 * 和聊天 provider 同样的道理：配置文件只能决定"用哪个"和"Key 是什么"，
 * 改不了"往哪儿发"。否则一份配置文件就能把录音和对话内容送到别处去。
 * 用户能改的只有两件事（见 asr_load_file）：provider 名、Key。
 *
 * ──  presently 支持的都是 /audio/transcriptions 这一种形态 ──────
 * 好处是 3DS 上不需要 multipart 之外的任何东西（没有 WebSocket、
 * 也没有厂商自有的长连接识别），一次 POST 就完事。
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>

/* 识别结果里正文的上限。中文一句话通常几十字，256 字节（约 85 个汉字）
 * 对"一句话的语音输入"绰绰有余；超长的会在 status.txt 里报截断字节数。 */
#define ASR_TEXT_MAX   256
#define ASR_ERR_MAX    96
#define ASR_KEY_MAX    128

typedef struct {
    const char *name;       /* 屏上显示 + 配置文件里写的那个值 */
    const char *url;        /* 编译期常量：用户改不了 */
    const char *model;
    const char *lang;       /* NULL = 这个厂商不吃 language 字段 */
} AsrDef;

typedef struct {
    bool   ok;
    long   http;            /* <0 = curl 层就没连上（值是 -CURLcode） */
    int    bytes;           /* 上传的音频字节数（0 = 没东西可发） */
    int    text_len;        /* 识别出的正文长度（可能被截） */
    bool   truncated;
    char   text[ASR_TEXT_MAX];
    char   err[ASR_ERR_MAX];
} AsrResult;

/* 当前生效的配置：provider / model / lang。key 不在这里 ——
 * key 只进当次请求的头，不落进任何长期结构（避免被别处顺手打进日志）。 */
void        asr_reset(void);
const AsrDef *asr_current(void);
const char *asr_key(void);

/* 上一次发送的自检（诊断用）。真机报"参数不合法"时，
 * 这三项能区分"我发错了"和"服务端不认"：
 *   body_len  —— 实际发出的 multipart 长度（0 = 根本没发）
 *   model_len —— 模型名的字符数（可以暴露 asr.txt 覆盖成了别的模型）
 *   clamped   —— 录音超上限被截断过（1 = 说明 length 与内容对不上过） */
int asr_last_body_len(void);
int asr_last_model_len(void);
/* 发请求前"内置 Key 被补回来"的次数。>0 说明有别的路径在抹 g_key。 */
int asr_key_refilled(void);
int asr_last_clamped(void);

/* 读配置文件。格式（一行一项，`#` 开头是注释）：
 *     provider=moark       ← 可选。不写就按下面哪把 Key 有值自动挑
 *     key_moark=xxxxxxxx   ← 模力方舟令牌（国内默认，每日免费 100 次）
 *     key_groq=gsk_xxxx    ← Groq 的 Key（备选，海外免费档）
 *     key=sk-xxxx          ← 硅基流动的 Key（兜底；不写就用内置那把）
 * 只认这四项；认不出来就忽略，不留半截状态。
 * 优先级：显式 provider > key_moark > key_groq > 内置 siliconflow。
 * 返回 false = 文件不存在/打不开（那时用编译期默认，不是错误）。 */
bool asr_load_file(const char *path);

/* 直接指定 provider 名与 Key（不读文件）。 */
bool asr_set(const char *provider, const char *key);

/* 不联网量一次 body 的长度，顺便校验缓冲常数够不够。
 * 用它可以在"麦克风和识别服务都不可用"的环境里，仍然验到本地
 * WAV 头 + multipart 这一半是对的 —— 那一段拼错的话，
 * 真机上只会表现为"识别失败 HTTP 400"，看不出是本地拼歪的。 */
bool asr_payload_probe(size_t pcm_bytes, size_t *body_len);

/* 把一段 PCM16 单声道采样的录音送上去。
 * pcm/bytes 来自 voice_pcm()（内部缓冲，本函数用完即弃）。
 * ⚠️ 本调用**会阻塞**在网络上（几秒）。要先把"正在识别…"画出来、
 * 下一帧再发，见 sc_chat.c 里的两帧拆法。 */
void asr_transcribe(const void *pcm, size_t bytes, AsrResult *out);
