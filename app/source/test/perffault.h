/*
 * perffault.h -- 故障注入验收（见 perffault.c 的完整说明）
 *
 * 由 autorun 脚本的 `perffault <场景>` 指令触发，结果进 status.txt。
 *
 * ⚠️ 与 amprobe 的区别：amprobe 回答的是「这台机器的事实是什么」，
 *    本文件回答的是「**坏情况下会不会崩**」。它要证明的判据全是
 *    否定式的 —— 「不崩」「还能继续」「没有把句柄 free 掉」。
 */
#pragma once

/*
 * 跑一个具名场景。返回值恒为 0（**永不失败**）：
 * 这本身就是一条判据 —— 探针自己要是能把 app 弄死，脚本就看不到后续步骤。
 *
 * 场景名见 perffault.c 里的场景表。名字不认识时只记一条 note，不做别的。
 */
void perffault_run(const char *scenario);
