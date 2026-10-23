/*
 * sysinfo.h -- sys.info 动作的采集
 */

#pragma once

#include <stddef.h>

/* 把这台机器与本软件的确定事实写进 out（UTF-8，可含换行）。
 * out 会被当作"动作执行结果"回灌给模型，所以里面**不能出现凭据**。 */
void sysinfo_collect(char *out, size_t cap);
