/*
 * sysstat.h -- 帧率 / 主循环占用 / 内存 的实时采样
 *
 * ── 为什么叫"主循环占用"而不是"CPU 使用率" ──
 *   3DS 没有给用户态任何 CPU 利用率计数器（没有 /proc，svc 也不暴露 idle 时间）。
 *   硬编一个"CPU 62%"就是编数字，而这个项目里编数字比不显示更糟 ——
 *   用户会照着它做判断。
 *
 *   能给的是**实测**：这一帧我们从输入采集到画完用了多少毫秒，除以帧间隔。
 *   它等于"主循环吃掉了多少帧预算"，不含垂直同步那段被动等待 ——
 *   想知道"还有多少余量"，看的就是这个数。
 *
 * ── GPU 呢 ──
 *   本 App 的界面是**纯 CPU 光栅**（r_rect / r_text 直接写帧缓冲），
 *   PICA200 只负责把两块缓冲显示出去。所以"GPU 占用"在这里没有意义，
 *   面板上给的是**绘制耗时**（render 那一段），那才是真正在花时间的地方。
 *
 * ── 采样成本 ──
 *   每帧只用调用方已经取到的时间戳（主循环里本来就有 t_start / t2），
 *   **不额外调 osGetTime**；内存每 30 帧查一次 syscall，摊薄到可忽略。
 */

#pragma once

#define SYSSTAT_N 60        /* 保留最近 60 帧 ≈ 1 秒 */

/* 每帧调用一次。t_begin = 本帧起点，t_work_end = app_render 返回的时刻（毫秒）。 */
void sysstat_tick(unsigned t_begin, unsigned t_work_end);

/* 第 i 个样本（0 = 最旧）。越界返回 0。 */
float sysstat_frame_ms(int i);
float sysstat_cpu_pct(int i);
int   sysstat_count(void);

/* 最近一次的值 / 均值 */
float sysstat_fps(void);
float sysstat_cpu_now(void);
float sysstat_frame_now(void);

/* 内存（应用区，单位 KB）。读不到时 total = 0，调用方要据此显示"读不到"。 */
/* 原始字节（给诊断用：这两个数必须自洽 used <= size，
 * 不自洽说明当前环境的 svc 与内核配置对不上，那就别拿它画百分比）。 */
unsigned sysstat_mem_used_bytes(void);
unsigned sysstat_mem_size_bytes(void);

unsigned sysstat_mem_used_kb(void);
unsigned sysstat_mem_total_kb(void);
