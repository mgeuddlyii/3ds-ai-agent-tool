/*
 * asrnetprobe.h -- 语音请求连接复用验收（要联网，不进常规回归）
 */

#pragma once

/* 对同一主机连发两次请求，比较 DNS/TCP/TLS 三段耗时。
 * 结论一行落在 status.txt：`asrnet: RESULT=PASS|FAIL|SKIP ...`。 */
void asrnetprobe_run(void);
