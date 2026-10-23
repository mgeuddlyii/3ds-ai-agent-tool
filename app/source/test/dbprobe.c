/*
 * dbprobe.c -- 见 dbprobe.h
 *
 * 写法与 amprobe.c 一致：**只下能被证据支持的结论**。
 * 每一行 note 都要能独立回答"哪一环通了/断在哪"，
 * 而不是笼统地说一句"豆包不通"。
 */

#include "dbprobe.h"

#include "app.h"
#include "doubao.h"
#include "httpx.h"

#include <stdio.h>
#include <string.h>

/* 探针问的那句话。固定文本，不取自任何配置 —— 探针要可复现，
 * 而且这句话本身不该由模型或用户影响。 */
#define DBPROBE_Q "用一句话回答：1+1等于几？"

void dbprobe_run(void)
{
    char msg[192];

    app_note("dbprobe: 开始（要联网；只读，不改任何状态）");

    /* ---- 第 0 环：传输库在不在 ----
     * 这一行能排除一大类问题：如果连 curl 都没链进来（或链了错的版本），
     * 后面所有失败都会指向 TLS/网络，而真正的原因是构建。
     * 版本串里带 mbedTLS 的版本 —— TLS 1.2 就是靠它，不是 httpc。 */
    app_note("dbprobe: 传输层 = %s", hx_version());

    /* ---- 第 1 环：卡上有没有登录态 ----
     * 只读文件，不联网。失败与"登录过期"是两件事，要分开报。 */
    if (!db_load_cookie()) {
        app_note("dbprobe: 卡上没有登录态（%s 不存在或内容是空的）—— "
                 "先扫码登录，或从 PC 种一份进去", DB_COOKIE_FILE);
        app_note("dbprobe: 结论 = 未登录（不是网络问题）");
        app_note("dbprobe: 结束");
        return;
    }
    app_note("dbprobe: 读到登录态，%d 个 cookie（sessionid=%s）",
             db_cookie_count(),
             db_has_cookie() ? "有" : "**没有**");

    /* ---- 第 2 环：服务端认不认这个登录态 ----
     * 这一步走通了，就等于**同时**证明了：
     *   soc 起来了 → TLS 1.2 握手成功 → CA 校验通过 → HTTP 往返正常
     *   → cookie 被服务端接受。
     * 也就是说，后面聊天再出问题，一定是请求体/SSE 解析那一层，
     * 而不是传输层 —— 这个区分能省掉大量排查。 */
    {
        char detail[192];
        bool ok = db_verify_login(detail, sizeof detail);

        app_note("dbprobe: 登录态校验 = %s · %s", ok ? "通过" : "不通过", detail);
        if (!ok) {
            app_note("dbprobe: 结论 = 传输层或登录态有问题（还没走到聊天那一步）");
            app_note("dbprobe: 结束");
            return;
        }
    }

    /* ---- 第 3 环：真发一句话 ----
     * 到这儿传输层已经确认没问题了，所以这一步若失败，
     * 只可能是请求构造或 SSE 解析 —— 范围一下子小了很多。 */
    {
        static char reply[2048];
        DbState ds = db_chat(DBPROBE_Q, NULL, reply, sizeof reply,
                             msg, sizeof msg);

        if (ds == DB_OK) {
            app_note("dbprobe: 聊天成功，回复 = %.180s", reply);
            app_note("dbprobe: 会话 id = %.40s",
                     db_last_conversation_id()[0]
                         ? db_last_conversation_id() : "(响应里没给)");
            app_note("dbprobe: 结论 = 豆包通道全通");
        } else {
            app_note("dbprobe: 聊天失败（%s）· %s", db_state_text(ds), msg);
            app_note("dbprobe: 结论 = 传输层和登录态是好的，"
                     "断在请求构造或响应解析这一层");
        }
    }

    app_note("dbprobe: 结束");
}
