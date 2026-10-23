/*
 * xfer.h -- 从电脑拉文件到 SD 卡（给"与电脑互传"用）
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>

/* 传输档位。**按型号自动选**，用户不需要设置 ——
 * 旧 3DS 的 Wi-Fi 与内存都差一档，用大缓冲只会白占内存。 */
typedef struct {
    const char *model;      /* "New 3DS" / "3DS" —— 给界面显示用 */
    bool        is_new;     /* 是不是新架构 */
    long        buf_size;   /* curl 接收缓冲字节 */
    long        speed_hint; /* 实测/估计的 MB/s，只用于给用户一个预期 */
} XferProfile;

/* 探测本机档位。**不缓存** —— 一次传输调一次就够。 */
void xfer_profile(XferProfile *out);

/* 拉一个 URL 到 SD 卡。
 *
 * 落盘走「先写 <name>.tmp → 校验大小 → rename」——
 * 3DS 上没有 fsync 保证，中途断电会留下半截文件，
 * 而一个半截的 .cia 装上去会砖。rename 是原子的，
 * 所以要么是完整的旧文件，要么是完整的新文件。
 *
 * expect_bytes > 0 时会校验最终大小；sha256 非空时校验摘要。
 * 返回 true 表示已落盘并 rename 成功。
 *
 * progress 用户可回调置空。on_progress 收到的是"已完成百分比"（0..100），
 * 调用发生在 curl 的接收回调里 —— **不要在里面做耗时操作**。
 */
typedef void (*XferProgress)(int pct, long done, long total, void *user);

bool xfer_pull(const char *url, const char *sd_dir, const char *name,
               long expect_bytes, const char *sha256,
               XferProgress on_progress, void *user,
               char *err, size_t errcap);

/* 每拉完一块调一次（页面靠它出帧、让进度条动）。
 *
 * ★ 为什么**分块而不是开线程**：
 *   3DS 主循环是单线程的，curl 接收回调一进去就是阻塞循环。
 *   一次传 800 MB = 黑屏六分钟，那不行。
 *   开线程能解决，但项目里线程用出过问题（join 的 timeout 没传下去、
 *   线程句柄还在跑就被 free），而分块**零风险**：
 *   New 3DS 一秒传 2 MB，块取 2~4 MB，也就是**每秒跳一次** ——
 *   肉眼看着就是连续动画。所以选分块。
 *   额外好处：某块失败只重传那几 MB（断点续传的自然形态）。
 */
typedef void (*XferYield)(void *user);

/* 分块拉取。chunk_bytes <= 0 时按型号自动取。
 * expect_bytes > 0 是**必须的**（分块要靠它算总块数），
 * 传 0 会直接拒绝 —— 不知道总大小就没法分块。 */
bool xfer_pull_chunked(const char *url, const char *sd_dir, const char *name,
                       long expect_bytes, long chunk_bytes,
                       XferProgress on_progress, void *user,
                       XferYield on_yield, void *yield_user,
                       char *err, size_t errcap);
