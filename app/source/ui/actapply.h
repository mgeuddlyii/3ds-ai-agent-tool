/*
 * actapply.h -- 设置类动作的落盘（app 层）
 *
 * 为什么从 sc_confirm.c 里抽出来：
 *   落盘要调 app_cfg() 与 w_theme()，这两个在 app 层；
 *   而 shared/actions.c 只做校验、不碰配置（它不该反向依赖 app）。
 *   抽成独立模块后，**确认屏与测试探针走的是同一份落盘代码** ——
 *   否则会出现"探针验的那份和用户实际跑的那份不是同一个"，
 *   那种绿是假的。
 *
 * 键的映射表不在这里另写：合法性由 shared/actions.c 的
 * actions_check_setting() 用白名单把关，这里只负责"这个键落到
 * config 的哪个字段"。两层职责不同，缺一层都不行。
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "actions.h"

/* 改一项设置并立刻落盘。value 必须是 actions_check_setting() 规范化过的值
 * （本函数不再校验 —— 校验在上一层已经做过，这里再校验一遍会出现
 * "两处规则不一致"，而那种不一致排查起来极难）。
 * 成功返回 true，msg 里是给用户看的一句话；
 * 失败返回 false，msg 说清是"没有对应实现"还是"写不进卡"。 */
bool act_apply_setting(const char *key, const char *value,
                       char *msg, size_t cap);
