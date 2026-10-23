/*
 * doubao.h -- 豆包通道（扫码登录 + 聊天 + 历史）
 *
 * 全部细节见仓库根的 `豆包通道技术交接.md`。那份文档是自包含的、每条都实测过，
 * **改本模块前先读它**。这里只留代码里必须知道的那些「为什么」。
 *
 * ── 为什么值得单独一个模块，而不是塞进 chatapi.c ──
 * 豆包的接口**不是 OpenAI 格式**，差别是全方位的：
 *   · URL 必须带一整套设备参数 query，缺了就回 710022002 block
 *     （它伪装成"访问频繁"，极易误判成限流 —— 这条坑过一次）
 *   · body 是 bot_id / completion_option / conversation_id / ext 那一套
 *   · 响应是 SSE，且 event_data 里是**二次 JSON 编码**的字符串，
 *     要先解外层再解内层，答案又在 message.content（还是 JSON）
 *   · 要带登录 cookie，而 cookie 来自扫码
 * 把这一坨塞进 chatapi 会把"标准 /chat/completions"那条路也搅浑。
 *
 * ── 本模块只做"数据"，不做"策略" ──
 * 它不弹确认、不写审计、不决定要不要联网 —— 那些是 actions.c / UI 的事。
 * 这里只负责"把话说出去、把回话拿回来"。
 *
 * ── 关于模式开关（本项目只要联网，不要 agent）──
 *   ext.use_search = "1"                    联网搜索 → 开
 *   completion_option.use_auto_cot = false  自主多步/写代码自己跑 → 关
 *   completion_option.use_deep_think = false 深度思考 → 关
 * **开关位置是交错的**（use_search 在 ext 里，另两个在 completion_option 里），
 * 放错位置不会报错，只是悄无声息地不生效 —— 这是最容易"以为开了其实没开"的一处。
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>

/* 登录态落盘位置。格式与 PC 端工具链一致（"name=value; name=value" 一行），
 * 所以 run_doubao.py 种下的会话能被本模块直接读到。 */
#define DB_DIR         "sdmc:/3ds/aiconsole/doubao"
#define DB_COOKIE_FILE "sdmc:/3ds/aiconsole/doubao/cookie.txt"

typedef enum {
    DB_OK = 0,
    DB_ERR_NO_LOGIN,     /* 没有登录态（或服务端不认）—— 与"网络不通"是两回事 */
    DB_ERR_NET,          /* 连不上 / TLS 失败 */
    DB_ERR_HTTP,         /* 连上了，服务端给的状态码不是 200 */
    DB_ERR_BLOCK,        /* 710022002 —— 设备参数缺失或真限流。**要等，别重试** */
    DB_ERR_PARSE,        /* 收到了但解不出来（接口改了？）*/
    DB_ERR_EMPTY,        /* 拿到了但回复是空的 */
} DbState;

const char *db_state_text(DbState s);

/* --------------------------------------------------------------------------
 * 登录态
 * ----------------------------------------------------------------------- */

bool db_has_cookie(void);          /* 内存里有没有（不代表服务端认） */
int  db_cookie_count(void);
bool db_load_cookie(void);         /* 从 DB_COOKIE_FILE 读进内存 */
bool db_save_cookie(void);         /* 存到 DB_COOKIE_FILE */
void db_forget_cookie(void);       /* 清内存。盘上的文件不动 —— 那是用户的 */

/* 问服务端"这个登录态还算数吗"。
 *
 * **这一步不能省**：cookie 文件存在 ≠ 登录态有效（服务端会过期）。
 * 判据取 history 接口的 `"code":0` —— 那是需要登录才能拿到的真实数据。
 * detail 里填一句给用户看的话（含账号线索，如果拿得到）。 */
bool db_verify_login(char *detail, size_t cap);

/* --------------------------------------------------------------------------
 * 扫码登录（三步，由界面驱动）
 *
 * 为什么拆成三步而不是一个 db_login()：
 *   二维码 **TTL 只有 60 秒**，界面必须每 45 秒换一张码并持续轮询。
 *   做成一个阻塞函数的话，界面在整段登录期间什么都画不出来。
 * ----------------------------------------------------------------------- */

/* 第 1 步：GET / 拿 ttwid + web_id，再取 csrf_token。
 * 必须成功才能继续 —— 少了 web_id，后面 chat 的 query 就拼不全。 */
bool db_login_begin(char *msg, size_t cap);

/* 第 2 步：取二维码。
 * ⚠️ 是 **512×512 的大图**，不是 29×29 —— 早先按像素采样试图还原矩阵，
 *    得到的是一个 512×512 的错误矩阵。界面要自己缩放出方块（见 shared/qr.h）。
 *
 * 二维码的原始 `data:image/png;base64,...` 会**存在模块内部**，
 * 用 db_login_qrcode_uri() 取 —— 不通过参数传，是因为那一串有 3.5 KB，
 * 让每个调用点都自备一个这么大的缓冲不合适。 */
bool db_login_qrcode(char *msg, size_t msgcap);

/* 取最近一次 db_login_qrcode 拿到的原始 data URI。
 * 返回 false 表示还没取过码（或缓冲太小）。 */
bool db_login_qrcode_uri(char *out, size_t cap);

/* 第 3 步：轮询一次扫码状态。status 填 new / scanned / confirmed。
 * 返回 true 表示"这一轮问到了"（不代表扫完了）；
 * confirmed 时本函数会**自动把 cookie 收进 jar 并落盘**。 */
bool db_login_poll(char *status, size_t cap, char *msg, size_t msgcap);

/* --------------------------------------------------------------------------
 * 聊天
 *
 * conversation_id 传 "0" = 新建会话；续聊传真 id。
 * reply 里是完整回复文本（非流式；SSE 一路收完再解）。
 * msg 填一句人话，失败时必须说得出理由。
 * ----------------------------------------------------------------------- */
DbState db_chat(const char *user_text, const char *conversation_id,
                char *reply, size_t reply_cap,
                char *msg, size_t msg_cap);

/* 上一次 db_chat 之后，服务端给的会话 id。
 *
 * 为什么要暴露它：`conversation_id` 填 "0" 是**新建会话**，续聊必须填真 id。
 * 而这个 id 只有服务端在响应里告诉你 —— 不捞出来存住，多轮对话就无从谈起
 * （每一句都会开一个新会话，在手机端历史里看着像一堆碎对话）。
 * 没聊过、或响应里没带，返回空串。 */
const char *db_last_conversation_id(void);
