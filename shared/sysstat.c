/*
 * sysstat.c -- 见 sysstat.h
 *
 * 环形缓冲，索引 0 = 最旧。写满之前不回头，所以前 60 帧图上是从左往右长。
 */

#include "sysstat.h"

#include <3ds.h>
#include <string.h>

static float s_frame[SYSSTAT_N];
static float s_cpu[SYSSTAT_N];
static int   s_head;            /* 下一个写的位置 */
static int   s_count;           /* 已写入（<= N） */

static unsigned s_prev_begin;
static bool     s_have_prev;

static float    s_fps, s_cpu_now, s_frame_now;
static unsigned s_mem_used_kb, s_mem_total_kb;
static int      s_mem_tick;

static void push(float frame_ms, float cpu)
{
    s_frame[s_head] = frame_ms;
    s_cpu[s_head]   = cpu;
    s_head = (s_head + 1) % SYSSTAT_N;
    if (s_count < SYSSTAT_N)
        ++s_count;
}

void sysstat_tick(unsigned t_begin, unsigned t_work_end)
{
    float frame_ms, busy_ms, cpu = 0.0f;

    /* 第一帧没有"上一帧起点"，只能记 0 —— 编一个 16.7 会让图的第一根柱子说谎。 */
    if (!s_have_prev) {
        s_prev_begin = t_begin;
        s_have_prev  = true;
        return;
    }

    frame_ms = (float)(int)(t_begin - s_prev_begin);
    s_prev_begin = t_begin;

    /* 时间戳是毫秒整数，一帧往往就是 16 或 17 —— 直接除会把 15/16 判成抖动。
     * 用"上一帧的实际间隔"做分母，再把 busy 夹在 [0, frame] 内。 */
    busy_ms = (float)(int)(t_work_end - t_begin);
    if (busy_ms < 0.0f)
        busy_ms = 0.0f;
    if (frame_ms < 1.0f)
        frame_ms = 1.0f;              /* 防止除 0；真出现说明时间戳同毫秒 */
    if (busy_ms > frame_ms)
        busy_ms = frame_ms;

    cpu = busy_ms * 100.0f / frame_ms;

    s_frame_now = frame_ms;
    s_cpu_now   = cpu;
    s_fps       = 1000.0f / frame_ms;
    push(frame_ms, cpu);

    /* 内存每 30 帧查一次。这是 syscall，每帧查会把它自己算进开销里 ——
     * 那正是"观测改变观测对象"。 */
    if (++s_mem_tick >= 30) {
        s_mem_tick = 0;
        s_mem_used_kb  = osGetMemRegionUsed(MEMREGION_APPLICATION) / 1024;
        s_mem_total_kb = osGetMemRegionSize(MEMREGION_APPLICATION) / 1024;

    }
}

float sysstat_frame_ms(int i)
{
    if (i < 0 || i >= s_count)
        return 0.0f;
    /* s_count < N 时数据从 0 开始；写满后最旧的在 s_head。 */
    return s_frame[(s_head - s_count + i + SYSSTAT_N * 2) % SYSSTAT_N];
}

float sysstat_cpu_pct(int i)
{
    if (i < 0 || i >= s_count)
        return 0.0f;
    return s_cpu[(s_head - s_count + i + SYSSTAT_N * 2) % SYSSTAT_N];
}

int   sysstat_count(void)      { return s_count; }
float sysstat_fps(void)        { return s_fps; }
float sysstat_cpu_now(void)    { return s_cpu_now; }
float sysstat_frame_now(void)  { return s_frame_now; }
unsigned sysstat_mem_used_bytes(void) { return s_mem_used_kb * 1024u; }
unsigned sysstat_mem_size_bytes(void) { return s_mem_total_kb * 1024u; }
unsigned sysstat_mem_used_kb(void)  { return s_mem_used_kb; }
unsigned sysstat_mem_total_kb(void) { return s_mem_total_kb; }
