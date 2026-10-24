/*
 * otaprobe.h -- 远程更新的网络链路验收（要联网，不进常规回归）
 */

#pragma once

/* 逐源握手 + 走一次 ota_fetch_remote，结论写 status.txt：
 * `otanet: RESULT=PASS|FAIL|SKIP ...`。只读，不下产物。 */
void otaprobe_run(void);

/* 真下一个产物（挑最小的那个），验证流式写盘 + sha256 + rename。
 * 会占卡上 update/ 目录，跑完可以不管（下次更新会覆盖/清理）。 */
void otadlprobe_run(void);
