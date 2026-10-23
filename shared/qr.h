/*
 * qr.h -- 把豆包返回的二维码 PNG 解成模块矩阵
 *
 * 为什么需要它：豆包扫码登录返回的是一张 **512×512 的 PNG**（base64 塞在
 * `data:image/png;base64,...` 里），而 3DS 要显示的是"每个模块一个方块"。
 * 直接把 PNG 缩放贴上去是不行的：屏幕和图的尺寸对不上，缩放会糊掉，
 * 扫码识别的成功率取决于**黑白边界是否锐利**。
 *
 * ⚠️ 一个反直觉的点：**必须自己解码，不能"按已知格子采样"**。
 * 实测（2026-10-04，真实图）：黑区 508×508，而模块尺寸是 **17.5172 px**
 * ——**不是整数**。也就是说模块边界在像素上是不对齐的，
 * 拍脑袋按固定步长采样会一路累积偏移，到图的右下角就错格了。
 * 正确做法是先用**左上定位符**自校准（它的顶边恰好是 7 个黑模块），
 * 再按 `minx + (i+0.5)*bw/N` 取每个模块的中心。
 *
 * ── 内存约束（决定了实现方式）──
 * 512×512 的 RGB 图解压后有 **786,944 字节**，比本项目任何现有缓冲都大一个量级。
 * 所以不能一次性解出来放着，而是：
 *   · 只保留 PNG 原始字节（本例 2.6 KB）；
 *   · 用 zlib 的**流式** inflate，按行喂、按行处理，缓冲只有两行（约 3 KB）；
 *   · 需要两遍（先量几何、再采样）就**重放一次 inflate** ——
 *     反正输入只有几 KB，重置状态比留 787 KB 划算得多。
 * 峰值内存：inflate 状态 + 2 行缓冲 + 矩阵（64×64 位）≈ 30 KB。
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>

/* 模块数上限。QR version 40 = 177 模块，但豆包实测是 version 3 = 29。
 * 取 64 是"够用且矩阵只占 4 KB"的折中；真的超了会明确报错而不是截断。 */
#define QR_MAX_MODULES 64

typedef struct {
    int           n;                                  /* 每边模块数 */
    unsigned char mod[QR_MAX_MODULES][QR_MAX_MODULES];/* 1 = 黑 */
} QrCode;

typedef enum {
    QR_OK = 0,
    QR_ERR_B64,        /* base64 坏了 */
    QR_ERR_PNG,        /* 不是 PNG，或 chunk 结构不认识 */
    QR_ERR_FORMAT,     /* 位深/颜色类型/隔行方式不支持（只做 8bit 真彩非隔行）*/
    QR_ERR_INFLATE,    /* zlib 解不开 */
    QR_ERR_BIG,        /* 模块数超过 QR_MAX_MODULES */
    QR_ERR_NO_PATTERN, /* 解出来了但找不到定位符 —— 图不是二维码，或几何算法失效 */
} QrState;

const char *qr_state_text(QrState s);

/* 从 `data:image/png;base64,...` 整串解码成模块矩阵。
 * 也接受不含前缀的裸 base64。
 * msg 里填一句人话（成功时说明模块数）。 */
QrState qr_decode_data_uri(const char *uri, QrCode *out,
                           char *msg, size_t cap);

/* ---- 给自检/断言用的判据 ----
 * 三个定位符（左上/右上/左下）各自是 7×7 的固定花纹。
 * 返回"不符的格数"（0 = 完全正确）。这是**"这张图真能被扫出来"的硬判据**，
 * 比"画了一堆方块"强得多 —— 错格/错位的图看着也像二维码。 */
int qr_finder_mismatch(const QrCode *q);
