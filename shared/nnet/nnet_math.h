/*
 * nnet_math.h/c — 定点超越函数（3DS 专属）
 *
 * ══ 为什么不用 libm的 expf / tanhf / sqrtf ══════════════════════════
 *
 * 反汇编实测（arm-none-eabi-objdump -d nnet.o）：
 *     nnet_push  → expf ×1, tanhf ×1, sqrtf ×1
 *     nnet_sample→ expf ×1
 *     layernorm  → sqrtf ×1
 * 全部是 `bl` 到 newlib 的软浮点实现。
 *
 * ARM11（ARMv6K）没有 f64 硬件这件事你已经知道了，但**还有第二层**：
 * 即便是 f32，ARM11 也没有硬件超越函数单元 —— exp/ln/sin/cos
 * 全是软件例程。libm 的 expf 大致是 "2^k * 多项式" 的实现，
 * 一次的拍数是**几百到上千**，且内部还有 spc_csr_f32 之类
 * 会打断流水线的模式切换。
 *
 * 而这里的需求精度低得离谱：
 *   - 输入是 int8 量化过的激活，有效位8 位
 *   - softmax 结果要归一化，最终进 int8
 *   - GELU 输出立刻乘回fp32 权重再量化成 int8
 * f32 的 24 位尾数在这些位置**用不掉 10 位以上**。
 * 所以 Q15 定点（1 位符号 + 15 位小数 ≈ 11 位有效精度）
 * 绰绰有余，而且能把依赖链压进 VFP 流水线，不触发模式切换。
 *
 * ══ 安全设计（插件场景，这是硬要求）═══════════════════════════════
 *
 * 这个模块会在「游戏正在运行时」被调用。任何一次崩溃都意味着
 * 游戏一起挂掉，用户对3DS 的信任也就没了。所以：
 *
 *   1. **所有输入都先过 NaN/Inf 判定**，绝不把垃圾值带进定点路径。
 *      定点运算遇到 NaN 不会像 IEEE 那样传播，而是变成一个
 *      巨大的正数一路算到底 —— 那比崩溃更难查。
 *   2. **所有输出都夹在有限区间内**，不存在"返回 Inf 让上层
 *      自己去判断"这种约定。约定是最容易在改动中被忘记的东西。
 *   3. 无动态内存、无全局可变状态，可重入。
 *
 * ══ 精度自检 ════════════════════════════════════════════════════
 * nnet_math_selftest() 在 Azahar 里跑一次，逐点对比 libm，
 * 打出最大绝对误差。判据写在函数末尾的注释里——
 * **判据必须落在确定值上，不能是"看起来差不多"**。
 */
#ifndef NNET_MATH_H
#define NNET_MATH_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * nnet_expf — e^x，饱和到 [-80, +80]。
 *
 * 范围取±80 而不是 ±88：softmax 里已经减掉了最大值，
 * 传进来的x 恒≤ 0，所以只有下界 [-80] 是真实会遇到的。
 * 上界保留是为了防止上层传进来正的大数时溢出。
 *
 * x < -80  → 返回 0（e^-80 = 1.8e-35，在 fp32 里就是噪声）
 * x > +80  → 返回 1.8e35 的钳位值
 */
float nnet_expf(float x);

/*
 * nnet_tanhf — tanh(x)，用 2/(1+e^-2x) - 1 的 Q15 形式。
 * 输入 |x| > 9 时直接返回 ±1（tanh(9) 与 ±1 的差已在 fp32 精度内）。
 */
float nnet_tanhf(float x);

/*
 * nnet_rsqrtf — 1/sqrt(x)，牛顿迭代 + Q15 种子。
 *
 * 为什么不直接 1.0f/sqrtf(x)：
 *   fp32 硬件除法在 ARM11 上是 14 拍，比定点乘法贵得多。
 *   而 LayerNorm 每层调2 次、每 token 4 层 = 8 次，
 *   加上注意力的 d_head 归一化，是真正的高频路径。
 *
 * x <= 0 返回 0 而不是 Inf：LayerNorm 的 var 在数值上恒 ≥ 0，
 * 传进来负数只可能是调用方的bug，此时返回 0 让下游得到
 * 全 0 而不是 NaN 扩散 —— 前者看起来像"这层废了"，
 * 后者会让整个 logits 变成 NaN 而采样器死循环。
 */
float nnet_rsqrtf(float x);

/* 1.0f / x，x==0 时返回 0（见 nnet_rsqrtf 的理由）。 */
float  nnet_rcp(float x);

/*
 * nnet_powf(x, e) — x^e，**只用于采样的 1/T 次幂**。
 *
 * 实现为 exp(e·ln x) 的定点版。为什么不用 powf：
 *   ARM11 没有硬件超越函数，newlib 的 powf 要调 logf + expf，
 *   两次软浮点；而这里 e 的取值范围被 nnet_pick 夹在 [0.25, 20]，
 *   x 的取值范围被夹在 (0, 1]（概率），**乘积 e·ln(x) ∈ [-55, 0]**，
 *   完全落在 nnet_expf 已验证的 [-80, 0] 工作区里。
 *   所以精度是现成的，多花几十字节 flash 换掉一次 logf 很划算。
 *
 * 边界：x <= 0 一律返回 0（0^正数 = 0；负底数在概率采样里
 * 不会出现，出现了说明调用方传错了，返回 0 至少不会算出 NaN）。
 */
float nnet_powf(float x, float e);

/*
 * 精度自检：逐点对比 libm，返回**最大绝对误差**。
 * 判据（Azahar 实测后确定）：
 *   expf  maxerr < 1e-3   （相对误差 < 0.2%，softmax 后立刻量化）
 *   tanhf maxerr < 1e-3
 *   rsqrtf maxerr < 1e-3
 * 超了就把实测值打进 app_note —— 光返回 0 看不出是哪一项坏了。
 */
float nnet_math_selftest(int32_t *out_exp, int32_t *out_tanh,
                         int32_t *out_rsqrt, int32_t *out_bad);

#ifdef __cplusplus
}
#endif
#endif /* NNET_MATH_H */
