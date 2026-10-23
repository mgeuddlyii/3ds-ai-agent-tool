/*
 * micprobe.h -- 语音输入链路探针（自动回归指令 `micprobe`）
 */

#pragma once

/* 同步执行完，结论一条条写进 status.txt。 */
void micprobe_run(void);
