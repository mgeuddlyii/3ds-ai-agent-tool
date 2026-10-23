/*
 * shot.h -- 屏幕截图（写 24 位 BMP 到 SD 卡）
 *
 * 存在的意义不只是"功能"：3DS 没有别的自动化验收通道，
 * 让程序自己把帧缓冲落盘，就能在模拟器（甚至真机）上做到零人工核对。
 * Azahar 的虚拟 SD 卡是一个普通目录，取文件直接看图即可。
 */
#pragma once

/* 递归创建目录，已存在算成功。返回 0 表示可用。 */
int shot_mkdirs(const char *path);

/* 把当前 target 的那块屏存成 BMP。返回 0 成功。 */
int shot_save_bmp(const char *path);

/* 顶屏 + 底屏各存一张：<dir>/<prefix>_top.bmp 和 _bottom.bmp。 */
int shot_save_both(const char *dir, const char *prefix);

/* 把脚本给的截图名就地净化成 [A-Za-z0-9_-]，防止名字里藏路径穿越。
 * 放这里而不是各调用点自己写一份：主循环（main.c）和流式期间的连拍
 * （sc_chat.c）都要用，两处各写一份迟早会漂移成不一样的规则。 */
void shot_sanitize(char *s);

/* 默认截图目录 sdmc:/3ds/aiconsole/shots */
#define SHOT_DIR "sdmc:/3ds/aiconsole/shots"
