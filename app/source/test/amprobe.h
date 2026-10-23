/*
 * amprobe.h -- CIA 安装能力探针（见 amprobe.c 的详细说明）
 *
 * 这两个函数都**不会真的安装任何东西**：
 *   amprobe_run()          探权限。只在 StartCiaInstall 意外成功时
 *                          立刻 Cancel 掉，不留半个 title。
 *   amprobe_list_titles()  只读。列出 SD 卡上已装的 title。
 *
 * 由 autorun 脚本的 `amprobe` 指令触发（见 autorun.c），
 * 结果进 status.txt，宿主脚本断言。
 */

#pragma once

void amprobe_run(void);
void amprobe_list_titles(void);
