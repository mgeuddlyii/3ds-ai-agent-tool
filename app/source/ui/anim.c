/*
 * anim.c -- 见 anim.h
 */

#include "anim.h"

void anim_reset(Anim *a)
{
    if (!a)
        return;
    a->v      = 0;
    a->target = 0;
}

void anim_set(Anim *a, bool on)
{
    if (!a)
        return;
    a->target = on ? ANIM_ONE : 0;
}

/* ease-out 二次式：先把差值朝目标推进一个固定步长，
 * 步长是总时长的倒数。整数实现，避免浮点。
 *
 * 步长用 ANIM_ONE / ANIM_FRAMES = 1024/12 = 85 —— 除法是编译期常量，
 * 不会在每帧里真的做除法。 */
static int step_of(int delta)
{
    /* 步长随剩余距离缩放：离得远就走得快（起步猛），
     * 离得近就慢下来（收尾柔）—— 这就是"缓动"的手感来源。 */
    int s = (delta * 3) / ANIM_FRAMES;   /* 3 = 比例系数，经验值 */
    if (s < 8)
        s = 8;                            /* 保底步长，保证一定能收敛 */
    return s;
}

bool anim_tick(Anim *a)
{
    if (!a)
        return false;

    int d = a->target - a->v;
    if (d == 0)
        return false;

    int s = step_of(d < 0 ? -d : d);
    if (d > 0) {
        a->v += s;
        if (a->v >= a->target)
            a->v = a->target;
    } else {
        a->v -= s;
        if (a->v <= a->target)
            a->v = a->target;
    }
    return true;
}

float anim_value(const Anim *a)
{
    if (!a)
        return 0.0f;
    return (float)a->v / (float)ANIM_ONE;
}

bool anim_done(const Anim *a)
{
    return a && a->v == a->target;
}

/* ---- 自检 ----
 * 三项都验"过渡真的存在"，而不是"最终值对不对"——
 * 最终值对而中间跳变，正是"没有动画"的表现。 */
int anim_selftest(void)
{
    int ok = 0;

    /* 1. 需要多帧：从 0 推到满量程，中途某帧必须处于 (0, ANIM_ONE) 开区间内。 */
    {
        Anim a;
        anim_reset(&a);
        anim_set(&a, true);
        bool saw_middle = false;
        int  steps = 0;
        while (anim_tick(&a)) {
            ++steps;
            if (a.v > 0 && a.v < ANIM_ONE)
                saw_middle = true;
            if (steps > 1000) break;      /* 死循环护栏，不该发生 */
        }
        if (saw_middle && steps >= 2 && a.v == ANIM_ONE)
            ++ok;
    }

    /* 2. 单调递增：推进过程中 v 不允许回退或跳过区间。 */
    {
        Anim a;
        anim_reset(&a);
        anim_set(&a, true);
        bool mono = true;
        int  prev = 0;
        int  steps = 0;
        while (anim_tick(&a)) {
            if (a.v <= prev) { mono = false; break; }
            prev = a.v;
            if (++steps > 1000) break;
        }
        if (mono && a.v == ANIM_ONE)
            ++ok;
    }

    /* 3. 中途反向：推到半途改目标为 0，值必须开始回落。 */
    {
        Anim a;
        anim_reset(&a);
        anim_set(&a, true);
        for (int i = 0; i < 3; ++i)
            anim_tick(&a);
        int mid = a.v;
        anim_set(&a, false);
        bool dropped = false;
        int steps = 0;
        while (anim_tick(&a)) {
            if (a.v < mid) { dropped = true; }
            if (++steps > 1000) break;
        }
        if (mid > 0 && mid < ANIM_ONE && dropped && a.v == 0)
            ++ok;
    }

    return ok;
}
