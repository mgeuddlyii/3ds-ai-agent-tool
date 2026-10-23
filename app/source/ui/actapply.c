/*
 * actapply.c -- 见 actapply.h
 */

#include "actapply.h"

#include "app.h"
#include "config.h"
#include "widgets.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "uistr.h"

bool act_apply_setting(const char *key, const char *value,
                       char *msg, size_t cap)
{
    AppConfig *c = app_cfg();
    bool  b;
    long  n;

    if (!key || !value || !msg || cap == 0)
        return false;
    msg[0] = '\0';

    b = (strcmp(value, "on") == 0);
    n = strtol(value, NULL, 10);

    if (!strcmp(key, "theme")) {
        snprintf(c->ui.theme, sizeof c->ui.theme, "%s", value);
        w_theme(strcmp(value, "dark") ? W_THEME_LIGHT : W_THEME_DARK);
    } else if (!strcmp(key, "fontsize")) {
        snprintf(c->ui.font_size, sizeof c->ui.font_size, "%s", value);
    } else if (!strcmp(key, "depth")) {
        c->ui.depth = (float)n / 100.0f;
    } else if (!strcmp(key, "level")) {
        /* ⚠️ 这里必须先剥掉 'L'。
         *
         * value 已经被 shared/actions.c 规范化成 "L0".."L3"，
         * 而 strtol 遇到第一个非数字就停 —— strtol("L1") 返回的是 **0**，
         * 不是 1。于是"把安全等级提到 L1"会静默地把它写成 L0：
         * 落盘返回 true、确认屏显示"已执行"、而配置一点没变。
         *
         * 这个 bug 是 actprobe 抓出来的：它读回了 current_level，
         * 才看见"写进去 L1、读回来 L0"。只看返回值的话一路绿。
         */
        const char *p = value;
        if (*p == 'L' || *p == 'l')
            ++p;
        c->security.current_level = (int)strtol(p, NULL, 10);
        /* 记住选择：否则下次启动又掉回 L0，用户会以为没生效。
         * 这一位是"AI 自己改了自己的权限范围"——所以它**必须**被记住，
         * 否则用户批准了一个不会生效的改动，那比不批准更糟。 */
        c->security.persist_level  = true;
    } else if (!strcmp(key, "guard.confirm_delete")) {
        c->guard.confirm_delete = b;
    } else if (!strcmp(key, "guard.confirm_bulk")) {
        c->guard.confirm_bulk = b;
    } else if (!strcmp(key, "guard.max_write_kb")) {
        c->guard.max_write_kb = (int)n;
    } else if (!strcmp(key, "guard.max_batch_files")) {
        c->guard.max_batch_files = (int)n;
    } else if (!strcmp(key, "nand_backup.remind_on_startup")) {
        c->nand_backup.remind_on_startup = b;
    } else if (!strcmp(key, "ota.enabled")) {
        c->ota.enabled = b;
    } else if (!strcmp(key, "ota.auto_check")) {
        c->ota.auto_check = b;
    } else if (!strcmp(key, "diag.show_layout_overflow")) {
        c->diag.show_layout_overflow = b;
    } else if (!strcmp(key, "diag.show_notes")) {
        c->diag.show_notes = b;
    } else if (!strcmp(key, "diag.verbose_status")) {
        c->diag.verbose_status = b;
    } else {
        /* 走到这里说明 shared 的白名单与这里的映射不一致 —— 那是代码漏改，
         * 不是用户的输入有问题。措辞要分开，否则用户会去改自己的说法。 */
        snprintf(msg, cap, "%s", S(S0001));
        return false;
    }

    if (!app_cfg_save_now()) {
        snprintf(msg, cap, S(S0002));
        return false;
    }

    snprintf(msg, cap, S(S0003), key, value);
    return true;
}
