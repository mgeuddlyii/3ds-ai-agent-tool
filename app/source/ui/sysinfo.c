/*
 * sysinfo.c -- sys.info 动作的采集（app 层）
 *
 * 报的是**这台机器与本软件**的确定事实，不做任何猜测：
 * 凡是读不到的就明说"读不到"，不用默认值假装知道 ——
 * 一条编出来的"剩余空间 12GB"比"剩余空间读不到"糟得多，
 * 因为用户会照着它做决定。
 *
 * 刻意**不报**的东西：固件版本、SD 剩余容量、电量。
 * 3DS 上这三样要么没有公开接口，要么要读私有结构（易错且会随系统更新失效）。
 * 与其给一个可能错的数，不如不报。
 */

#include "sysinfo.h"

#include "app.h"
#include "config.h"
#include "doubao.h"
#include "provider.h"

#include <3ds.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include "uistr.h"

/* 文件在不在。用 stat 而不是 fopen：只需要"有没有"，
 * 而 fopen 成功与否还会顺手把文件建出来（O_CREAT 是默认行为）——
 * 一个只读动作去创建文件，那就不叫只读了。 */
static bool exists(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0;
}

void sysinfo_collect(char *out, size_t cap)
{
    const AppConfig     *c = app_cfg();
    const ProviderDef   *p;
    size_t               n = 0;

    out[0] = '\0';

    /* ---- 机型 ----
     * 用 APT_CheckNew3DS() 而不是读硬件 ID：3DS 上没有公开的
     * "我是哪一型"接口，而 apt 能判断的这条"是不是新架构"，
     * 对回答"这台机器跑不跑得动"来说更贴题 ——
     * 用户真正想知道的是它够不够快，不是它的商品型号。
     * 读不到就明说读不到，不拿"大概是 3DS"糊过去。 */
    {
        bool new3ds = false;
        if (R_SUCCEEDED(APT_CheckNew3DS(&new3ds))) {
            n += (size_t)snprintf(out + n, cap - n,
                                  S(S0739),
                                  new3ds ? S(S0740)
                                         : S(S0741));
        } else {
            n += (size_t)snprintf(out + n, cap - n, "%s", S(S0742));
        }
    }

    /* ---- 本软件状态 ---- */
    n += (size_t)snprintf(out + n, cap - n,
                          S(S0743),
                          c->launches, c->version);

    /* ★ provider_find() 找不到时返回**默认那家**（provider.h 有说明）。
     * 这里必须比名字：配置里写错一个名字时，早先的判空写法会报出
     * 默认那家的名 —— 那是**在说谎**，而且用户正靠这条判断自己配错了没有。 */
    p = provider_find(c->ai.provider);
    if (p && !strcmp(p->name, c->ai.provider))
        n += (size_t)snprintf(out + n, cap - n,
                              S(S0744), p->name, provider_note(p));
    else
        n += (size_t)snprintf(out + n, cap - n,
                              S(S0745),
                              c->ai.provider[0] ? c->ai.provider : S(S0044));

    /* ---- 卡上有没有登录态：只说在不在，不说内容 ----
     * 登录态是凭据，绝不能进 system 消息 —— 它下一轮就发给云端了。
     * 这里只报"有/无"。 */
    n += (size_t)snprintf(out + n, cap - n,
                          S(S0746),
                          exists(DB_COOKIE_FILE) ? S(S0747)
                                                 : S(S0748));

    /* ---- 几个关键文件在不在 ----
     * 逐个 stat 判存在；不存在就是"没有"，不猜。 */
    n += (size_t)snprintf(out + n, cap - n,
                          S(S0749),
                          exists("sdmc:/3ds/aiconsole/out") ? S(S0750) : S(S0751));
    n += (size_t)snprintf(out + n, cap - n,
                          S(S0752),
                          exists("sdmc:/3ds/aiconsole/guide_model.bin") ? S(S0627) : S(S0753));

    /* ---- 读不到的东西，明说读不到 ----
     * 这一行是刻意的：模型被问"还剩多少空间"时，
     * 有这一行它就会改口说"读不到"，而不是拿一个编出来的数去回答。 */
    n += (size_t)snprintf(out + n, cap - n, "%s", S(S0754));
}
