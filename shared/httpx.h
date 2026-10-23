/*
 * httpx.h -- HTTPS 客户端（curl + mbedtls）
 *
 * ── 为什么不用 libctru 自带的 httpc ──
 * httpc/sslc **只到 TLS 1.1**。豆包要求 TLS 1.2，用 httpc 会直接死在握手。
 * 而 libcurl + 3ds-mbedtls 能跑 1.2（devkitPro 已给 mbedtls 打好了熵源补丁），
 * 且 mbedtls 是**静态链进二进制**的 —— 握手由它自己做。
 *
 * ⚠️ 有一条必须记住的差别，它决定"测得准不准"：
 *    Azahar 的 http_c.cpp **无条件**关掉了证书校验
 *    （enable_server_certificate_verification(false)），
 *    所以基于 httpc 的 HTTPS 测试在模拟器里**必然成功** —— 那是假阳性。
 *    而 curl 这条路的握手在 mbedtls 里做，模拟器管不着 → **结果可信**。
 *    这就是为什么本项目的 HTTPS 全部走这里，httpc 只剩回退价值。
 *
 * ── CA 从哪来 ──
 * 内嵌（cacert_bin，见 build.py 的 bin2s 段）—— 出厂即可用，不要求用户
 * 往卡上放任何东西。若 `sdmc:/config/ssl/cacert.pem` **存在则优先用它**：
 * 本机开发时卡巴斯基在中间人解密 HTTPS，必须把它的根证书加进去才连得上
 * 豆包；真机不需要这个文件。这个优先级是"开发可覆盖、出厂能工作"的折中，
 * 而不是"随便哪个都行"。
 *
 * ── cookie 为什么放在这一层 ──
 * 豆包的登录态就是一串 cookie（17 个），而"发出去 + 收回来存住"这件事
 * 每种协议都要做。放这儿，上层（doubao.c）只管问"我登录了吗"和
 * "把登录态存盘/读盘"，不用关心 Set-Cookie 长什么样。
 * 格式与 PC 端工具链保持 `name=value; name=value` 一行 ——
 * 这样 run_doubao.py 种下去的文件、和 3DS 自己存的，是同一种东西。
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>

/* 初始化 / 清理。整个进程各调一次。
 * init 里做的事：curl_global_init + 记住 CA 来源。
 * ⚠️ **调用方必须先 socInit()** —— curl 走的是 3DS 的 socket 层，
 *    soc 没起来时 curl_easy_perform 会失败且报错很难懂。
 *    这一层不替调用方 socInit：那是进程级生命周期，不该由客户端库悄悄决定。 */
bool hx_init(void);
void hx_exit(void);

/* 链进来的 curl 版本串（例如 "libcurl/8.4.0 mbedTLS/2.28.8"）。
 * 排 TLS 问题时第一个要看的东西 —— "握手失败"到底是哪套实现失败，
 * 不看这行就只能猜。 */
const char *hx_version(void);

/* 收正文的回调。返回实际处理的字节数；返回 != len 会让 curl 中止传输
 * （用"想提前收工"的场合，比如用户按了 B）。
 * data 不是以 '\0' 结尾的，len 才是长度 —— SSE 的切点不认字符边界。 */
typedef size_t (*HxSink)(const char *data, size_t len, void *user);

typedef struct {
    const char *url;
    const char *post;            /* NULL = GET。NUL 结尾的 body（strlen 定长） */
    /* 二进制 / 含 NUL 的 body（POSTFIELDSIZE 定长）。
     *
     * ⚠️ **有音频上传的场景必须走 post_raw**，不能把 PCM 塞进 post：
     * curl 对 CURLOPT_POSTFIELDS 是按 strlen 取长度的，而录音里的静音段
     * 就是一整串 0x00 —— 一截就成了半段 WAV，服务端报的却是
     * "文件损坏/格式不对"，把人往完全相反的方向带。
     * post_raw 非空时优先，且两个字段同时给以 post_raw 为准。 */
    const void *post_raw;
    size_t      post_raw_len;
    /* 显式 Content-Type。multipart / 音频这类必须自己带，
     * 不能指望 curl 或服务端去猜。与 headers 里的 Content-Type 冲突时
     * 以这个为准（curl 只会留最后一个同名头）。 */
    const char *content_type;
    const char *const *headers;  /* NULL 结尾的额外请求头，可为 NULL */
    long        timeout_s;       /* 整次请求超时；<=0 取默认 30。
                                  * ★ 拉大文件时**必须显式放大**：默认 30 秒，
                                  * 而 800 MB 在 New 3DS 上按 2 MB/s 要传 6 分多钟，
                                  * 用默认值必然中途超时，且报错是笼统的 -28，
                                  * 看不出"其实是超时了"。 */
    bool        follow;          /* 是否跟随 3xx（登录取码要，POST 聊天不要）*/
    bool        add_cookies;     /* 是否带上 cookie jar（登录取码阶段要关掉）*/

    /* **每一跳开始之前**调一次，让调用方清掉自己的接收缓冲。
     *
     * 为什么必须有这个钩子：跟重定向时，前几跳（302）的**响应正文也会
     * 走同一个 on_body**。不清的话，最终拿到的正文里会混进一段
     * 「跳转页的 HTML」+「真正的 JSON」，解析必然出错 ——
     * 而且错得很像"服务端返回了怪东西"，方向完全被带偏。
     * 可以为 NULL，那时调用方得自己容忍正文里混了跳转页。 */
    void      (*on_new_response)(void *user);
    /* curl 接收缓冲字节数；<=0 用 curl 默认（16 KB）。
     *
     * 为什么需要它：默认 16 KB 意味着 sink 每收 16 KB 被调一次，
     * 而 socket -> 用户态 -> 文件是一次拷贝 + 一次系统调用。
     * 缓冲越大，系统调用摊得越薄。**但它吃内存** ——
     * 旧 3DS 给 app 的内存远少于 New 3DS，所以该由调用方按型号给，
     * 而不是在这里写死一个"快"的值。 */
    long        buf_size;
} HxReq;

/* 发一次请求。
 * 返回 HTTP 状态码（100..599）；**<0 表示 curl 层就失败了**，
 * 值是 -CURLcode（例如 -60 = CURLE_PEER_FAILED_VERIFICATION，证书不过）。
 * 这两种失败要分得开：状态码是"服务端说不行"，负值是"根本没连上"。
 *
 * 响应正文逐段交给 on_body。on_body 可以为 NULL（只要状态码）。
 * err 非空时填一句人话（成功时清空）。
 *
 * 期间的 Set-Cookie 会被自动收进 cookie jar（见下）。 */
long hx_perform(const HxReq *req, HxSink on_body, void *user,
                char *err, size_t errcap);

/* --------------------------------------------------------------------------
 * cookie jar
 *
 * 刻意做成"很笨的 name/value 表"而不是完整的 cookie 语义：
 * 不需要 domain/path/expiry —— 我们只跟一个站点打交道，
 * 而完整语义带来的复杂度会掩盖真正要盯的东西（登录态在不在）。
 * ----------------------------------------------------------------------- */

void        hx_cookie_clear(void);
void        hx_cookie_set(const char *name, const char *value);
const char *hx_cookie_get(const char *name);      /* 没有则返回 "" */
int         hx_cookie_count(void);

/* 拼成 "name=value; name=value"（给 Cookie: 头发）。 */
void hx_cookie_string(char *out, size_t cap);

/* 存/读盘。行格式就是上面那个字符串，与 PC 端工具链一致。 */
bool hx_cookie_save(const char *path);
bool hx_cookie_load(const char *path);
