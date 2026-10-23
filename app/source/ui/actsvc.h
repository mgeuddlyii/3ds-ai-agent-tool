/*
 * actsvc.h -- svc.set 动作的执行
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>

/* 切到指定服务商，并连带换 model、切 Key 仓库激活项、落盘。
 * 逻辑与「服务目录」屏的切换按钮完全一致（见 actsvc.c 顶部说明）。 */
bool act_svc_set(const char *name, char *msg, size_t cap);
