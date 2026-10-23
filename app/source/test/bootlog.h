/*
 * bootlog.h -- 启动/运行探针（黑匣子）
 *
 * boot.txt：每次覆写 —— 只保留"最后现场"，死时最后一行就是活到的位置。
 * boot.log：追加 —— 完整轨迹，卡死/闪退后能看到发生了什么、顺序如何。
 *           超过上限自动清空重写，防止 SD 卡被写爆。
 */
#pragma once

void boot_mark(const char *name, const char *fmt, ...);

/* scr 探针专用：每帧调用，只写 boot.txt 不进 log（否则几秒就滚掉轨迹）。
 * 死点定位靠它，轨迹回放靠 boot_mark。 */
void boot_mark_scr(const char *screen, int frames);
