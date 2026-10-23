/*
 * actmodel.h -- model.install 动作的执行
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>

/* 按类别名（model / guide / hw）与序号装一份离线内容。
 * 逻辑与「内容更新」屏的安装按钮完全一致（见 actmodel.c 顶部说明）。 */
bool model_install_action(const char *kind, const char *idx_str,
                          char *msg, size_t cap);
