/*
 * statpanel.c -- 见 statpanel.h
 *
 * 版面（320×240 的下屏，面板内容区 y 46..205）：
 *
 *   x=10                                                       x=310
 *   ┌──────────────────────────────────────────────────────────┐
 *   │ CPU                                          37%        │  y 46
 *   │ ▁▂▃▅▆▇█▇▆▅▃▂▁▂▃▅▆▇  （最近 60 帧，柱状）               │  y 64..92
 *   │ FRAME                                16.7 ms  59 fps   │  y 98
 *   │ ▁▁▁▁▁▂▁▁▁ - - - 16.7ms 基准线                          │  y 116..144
 *   │ MEM                                        142 / 176 MB │  y 152
 *   │ ████████████░░░░░░░░  （条）                           │  y 172
 *   └──────────────────────────────────────────────────────────┘
 *
 * 为什么用**柱子**而不是折线：raster 层只有矩形和水平线，没有任意斜率线段。
 * 60 根柱子在这块尺寸上反而比折线更好读（每根 4px，缝 2px）。
 */

#include "statpanel.h"

#include <stdio.h>
#include <string.h>

#include "sysstat.h"
#include "widgets.h"
#include "app.h"

#define PX       10.0f                  /* 左边界（与 W_BOT_MARGIN 一致） */
#define PW       300.0f                 /* 面板可用宽 */
#define CHART_H  26.0f                  /* 图高 */
#define BAR_GAP   1.4f                  /* 柱间缝 */
#define BAR_W    ((PW - (SYSSTAT_N - 1) * BAR_GAP) / (float)SYSSTAT_N)

/* 一根柱子。y_base = 图的底边（柱子从这里往上长）。 */
static void bar(float x, float y_base, float h, RColor c)
{
    Rect r;

    if (h < 1.0f)
        h = 1.0f;                       /* 0 高也要留一条头发丝，否则"没有数据"和
                                         * "这一帧几乎不花时间"在屏上看不出区别 */
    r.x = x;
    r.y = y_base - h;
    r.w = BAR_W;
    r.h = h;
    r_rect(&r, c);
}

static void chart_cpu(float y)
{
    const int   n = sysstat_count();
    const float base = y + CHART_H;
    int         i;

    /* 轨道：先铺一条浅底，让"数据不足 60 帧"时右边不是空白，
     * 而是看得出"这里迟早会有数据"。 */
    r_rect(&(Rect){ PX, y, PW, CHART_H, 0 }, w_c(C_SLOT_PANEL));
    r_hline(PX, base, PW, w_c(C_SLOT_LINE));

    for (i = 0; i < n; ++i) {
        float pct = sysstat_cpu_pct(i);
        /* 80% 以上转成警告色：那不是"快"，是"快要掉帧了"。 */
        bar(PX + (float)i * (BAR_W + BAR_GAP), base,
            pct / 100.0f * CHART_H,
            pct >= 80.0f ? w_c(C_SLOT_WARN) : w_c(C_SLOT_ACCENT));
    }

    /* 50% 参考线（画在柱子之上会反过来压住数据，所以用一条极浅的线叠上去） */
    r_hline(PX, y + CHART_H * 0.5f, PW, w_c(C_SLOT_LINE));
}

static void chart_frame(float y)
{
    const int   n = sysstat_count();
    const float base = y + CHART_H;
    float       top = 33.34f;           /* 至少 2 帧的窗口，否则 16.7ms 会被顶到天花板 */
    int         i;

    for (i = 0; i < n; ++i) {
        float ms = sysstat_frame_ms(i);
        if (ms > top)
            top = ms;
    }

    r_rect(&(Rect){ PX, y, PW, CHART_H, 0 }, w_c(C_SLOT_PANEL));

    /* 16.7ms 基准线 = 60fps 的帧预算。柱子超过它就是掉帧。 */
    {
        float by = base - (16.7f / top) * CHART_H;
        r_hline(PX, by, PW, w_c(C_SLOT_LINE));
    }

    for (i = 0; i < n; ++i) {
        float ms = sysstat_frame_ms(i);
        bar(PX + (float)i * (BAR_W + BAR_GAP), base,
            ms / top * CHART_H,
            ms > 20.0f ? w_c(C_SLOT_WARN) : w_c(C_SLOT_OK));
    }
}

void statpanel_render(void)
{
    char  buf[64];
    float y = 46.0f;
    unsigned used = sysstat_mem_used_kb(), total = sysstat_mem_total_kb();

    /* ---- CPU（主循环占用） ---- */
    r_text(&(Rect){ PX, y, 120.0f, 16.0f, 0 }, "CPU", w_c(C_SLOT_MUTED));
    snprintf(buf, sizeof buf, "%.0f%%", sysstat_cpu_now());
    r_text(&(Rect){ PX + 140.0f, y, PW - 140.0f, 16.0f, 0 }, buf,
           sysstat_cpu_now() >= 80.0f ? w_c(C_SLOT_WARN) : w_c(C_SLOT_FG));

    y += 18.0f;
    chart_cpu(y);

    /* ---- 帧耗时 ---- */
    y += CHART_H + 6.0f;
    r_text(&(Rect){ PX, y, 120.0f, 16.0f, 0 }, "FRAME", w_c(C_SLOT_MUTED));
    snprintf(buf, sizeof buf, "%.1f ms   %.0f fps",
             sysstat_frame_now(), sysstat_fps());
    r_text(&(Rect){ PX + 140.0f, y, PW - 140.0f, 16.0f, 0 }, buf,
           w_c(C_SLOT_FG));

    y += 18.0f;
    chart_frame(y);

    /* 一次性把内存原始字节记进 status.txt：这一对数字**必须自洽**
     * （已用 <= 总量）。不自洽时屏上不能画百分比 —— 画出来的条是假的。 */
    {
        static bool logged;
        if (!logged && total) {
            logged = true;
            app_note("stat: mem used=%u size=%u free=%u bytes",
                     sysstat_mem_used_bytes(), sysstat_mem_size_bytes(),
                     sysstat_mem_size_bytes() - sysstat_mem_used_bytes());
        }
    }

    /* ---- 内存 ---- */
    y += CHART_H + 6.0f;
    r_text(&(Rect){ PX, y, 120.0f, 16.0f, 0 }, "MEM", w_c(C_SLOT_MUTED));
    if (total) {
        snprintf(buf, sizeof buf, "%u / %u MB", used / 1024, total / 1024);
        r_text(&(Rect){ PX + 140.0f, y, PW - 140.0f, 16.0f, 0 }, buf,
               used <= total ? w_c(C_SLOT_FG) : w_c(C_SLOT_WARN));

        y += 18.0f;
        {
            /* ★ 先检查这两个数**自不自洽**（已用不能超过总量）。
             *   实测 azahar 上 svc 报 used=124MB、内核配置 size=96MB ——
             *   于是 free 还出现了无符号下溢（4.2GB）。
             *   这种时候画百分比就是在编数据：条长度取决于用哪个分母，
             *   而两个分母里必有一个是错的。所以只画一个空槽 + 明说"不一致"。
             *   真机上两者应当自洽，正常走下面的百分比分支。 */
            const bool sane = (total > 0) && (used <= total);
            Rect tr = { PX, y, PW, 10.0f, 0 };

            r_rect(&tr, w_c(C_SLOT_PANEL));
            if (sane) {
                float pct = (float)used * 100.0f / (float)total;
                Rect  fl = { PX, y, PW * pct / 100.0f, 10.0f, 0 };
                r_rect(&fl, pct >= 85.0f ? w_c(C_SLOT_WARN) : w_c(C_SLOT_ACCENT));
            }
            r_frame_rect(&tr, sane ? w_c(C_SLOT_LINE) : w_c(C_SLOT_WARN), 1.0f);

            if (!sane)
                r_text(&(Rect){ PX + 4.0f, y - 2.0f, PW - 8.0f, 14.0f, 0 },
                       "inconsistent", w_c(C_SLOT_WARN));
        }
    } else {
        /* 读不到就明说读不到 —— 这个项目里，编一个数比不显示糟得多。 */
        r_text(&(Rect){ PX + 140.0f, y, PW - 140.0f, 16.0f, 0 }, "n/a",
               w_c(C_SLOT_FAINT));
    }
}
