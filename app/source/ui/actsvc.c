/*
 * actsvc.c -- svc.set 动作的执行（app 层）
 *
 * 这一节原本是「服务目录」屏里的 do_pick()。现在同一段逻辑也由
 * [[ACT:svc.set]] 触发，所以这里是**照搬**，不是重写。
 *
 * ★ 为什么必须照搬、而且必须保持那三步的**顺序**：
 *
 *   1. 改 config.ai.provider
 *   2. **model 也必须跟着换** —— 留着上一家的模型名，请求就带着不相干的
 *      model 发出去。"切到豆包却发 glm-4-flash"就是这么来的。
 *   3. **Key 仓库的激活项也要跟着换** —— 对话屏进屏时的来源优先级是
 *      「keystore 激活条目 > key.txt > config」。只要仓库里还激活着别家的
 *      一把 Key，用户在这里选的那家就会被悄悄盖掉。
 *      豆包不用 Key、仓库里没有它的条目，所以那种情况是"清激活"而不是"切激活"。
 *   4. 落盘
 *
 * 把 2 或 3 简化掉（只改 provider）会复现两个已修过的 bug。
 * 少写一步的代价是"界面上选对了、实际发出去的却不是那家"，极难查。
 */

#include "actsvc.h"

#include "actions.h"
#include "app.h"
#include "config.h"
#include "keystore.h"
#include "provider.h"

#include <stdio.h>
#include <string.h>
#include "uistr.h"

bool act_svc_set(const char *name, char *msg, size_t cap)
{
    const ProviderDef *p;
    AppConfig         *c;
    bool               matched = false;

    if (!msg || cap == 0)
        return false;
    msg[0] = '\0';

    if (!name || !*name) {
        snprintf(msg, cap, "%s", S(S0030));
        return false;
    }

    /* 比名字而不是判空 —— provider_find() 找不到时返回默认那家（provider.h
     * 有说明）。判空的话 `name=nope` 会被当成"切到默认那家"，
     * 界面上显示成功、实际切到别家 —— 正是这个动作最不该出的错。 */
    p = provider_find(name);
    if (!p || strcmp(p->name, name) != 0) {
        snprintf(msg, cap, S(S0031), name);
        return false;
    }

    c = app_cfg();
    snprintf(c->ai.provider, sizeof c->ai.provider, "%s", p->name);
    /* 第 2 步：model 跟着换。default_model 为空的用服务商名兜底。 */
    snprintf(c->ai.model, sizeof c->ai.model, "%s",
             p->default_model[0] ? p->default_model : p->name);

    /* 第 3 步：Key 仓库的激活项跟着换。 */
    for (int i = 0; i < keystore_count(); ++i) {
        const KeyEntry *e = keystore_at(i);
        if (e && e->key[0] && !strcmp(e->provider, p->name)) {
            keystore_set_active(e->name);
            matched = true;
            break;
        }
    }
    if (!matched)
        keystore_clear_active();

    if (!app_cfg_save_now()) {
        snprintf(msg, cap, S(S0032));
        return false;
    }

    app_note("actsvc: 切到 %s（model=%s，key=%s）",
             c->ai.provider, c->ai.model, matched ? "已切换" : "无凭据");

    /* 缺凭据**不是失败** —— 豆包本来就不用 Key，用户接着去「Key 管理」配就行。
     * 但必须说出来，否则"切了却发不出消息"会变成一件怪事。 */
    if (!matched)
        snprintf(msg, cap, S(S0033),
                 p->name, c->ai.model);
    else
        snprintf(msg, cap, S(S0034), p->name, c->ai.model);
    return true;
}
