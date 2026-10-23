/*
 * autorun.h -- 脚本化按键回放（无人值守验收用）
 *
 * 动机：Azahar 没有可编程的输入注入接口，而模拟器 GUI 里手点一遍
 * 既不能自动化、也不能复现。于是让**程序自己**回放按键 ——
 * 关键在于注入的是"合成 keysDown"，走的是和真实按键完全相同的
 * app_handle() 路径，所以验证的是真逻辑，不是一条平行的假分支。
 *
 * 脚本文件：sdmc:/3ds/aiconsole/autorun
 *   存在即启用。宿主脚本（tools/azahar_test.py）每趟启动前重写它。
 *
 * 语法（一行一条，# 开头是注释）：
 *   wait 30            等 30 帧（60fps 下约 0.5s）
 *   key A              在这一帧按下 A（下一帧自动松开）
 *   touch 120 168      在这一帧合成一次底屏触摸（按下沿），320x240 显示序
 *   shot 1_welcome     把当前帧存成 shots/1_welcome_top.bmp / _bottom.bmp
 *   quit               结束回放并退出程序
 *
 * 语义：每条 key / shot 各消耗一帧；wait N 消耗 N 帧。
 * 脚本跑完 = 自动退出（无人值守的场合不该挂在最后一屏等人）。
 */

#pragma once

#include <3ds.h>
#include <stdbool.h>

#define AUTORUN_PATH "sdmc:/3ds/aiconsole/autorun"

/* 读脚本。返回 true 表示脚本存在且解析出至少一条指令。 */
bool autorun_init(void);

bool autorun_active(void);

/* 每帧调用一次，**在 app_handle 之前**。
 * 返回 true 表示本帧应该使用合成输入：*keys 是按键掩码（触摸时含
 * KEY_TOUCH，触点坐标写入 tx / ty 两个出参；非触摸时两者为 -1）。
 * *shot 非空表示本帧渲染完要截图；*quit 为 true 表示请求退出。 */
bool autorun_tick(u32 *keys, int *tx, int *ty, const char **shot, bool *quit);

int         autorun_steps(void);     /* 脚本总条数 */
int         autorun_pc(void);        /* 已执行到第几条 */
const char *autorun_error(void);     /* 解析出错时的一句话；没错返回 "" */
