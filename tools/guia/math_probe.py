#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""
math_probe.py — nnet_math.c 的算法交叉验证（Python 复现同一套定点逻辑）

为什么需要它
------------
本机msys2 里没有 native gcc/clang（只有 arm-none-eabi），
而 arm-none-eabi 编出来的 ELF 要跑必须过 Azahar。
但**数值精度这件事不该等模拟器** —— 它是纯算术，
在 PC 上复现同一套算法就能立刻验。

所以本文件逐行对照 nnet_math.c 的实现（同一套多项式系数、
同��的魔数、同样的整数截断位置），用 Python 的 float64
当作"参考真值"。float64 在这里是合法的参考 ——
我们要验的是"定点实现相对高精度真值的误差"，
而不是"定点 vs libm 的float32"。

**如果改了 nnet_math.c 的任何常数，这个文件必须同步改** ——
两处不一致会给出"精度达标"的假结论，而真机上恰恰是那个常数在起作用。
"""
import math
import struct
import sys


def f32(x):
    """截到 float32（用 struct 往返）。模拟 ARM 的 f32 精度。"""
    return struct.unpack("f", struct.pack("f", x))[0]


# ---- expf：与 nnet_math.c 完全一致 --------------------------------
LOG2E = f32(1.4426950408889634)
C1 = f32(0.6931471805)
C2 = f32(0.2402265069)
C3 = f32(0.0555041087)
C4 = f32(0.0096181291)


def nnet_expf(x):
    if x != x:
        return 0.0
    if x > 80.0:
        return f32(5.540622e34)
    if x < -80.0:
        return 0.0
    y = f32(x * LOG2E)
    k = int(y + (0.5 if y >= 0.0 else -0.5))
    # C 的 (int32_t) 转换是向零截断，这里 y+0.5 已经带了符号处理
    f = f32(y - float(k))
    p = f32(1.0 + f * f32(C1 + f * f32(C2 + f * f32(C3 + f * C4))))
    e = max(1, min(254, k + 127))
    sc = f32(struct.unpack("f", struct.pack("I", e << 23))[0])
    return f32(p * sc)


# ---- tanhf：与 nnet_math.c 完全一致 -------------------------------
# ⚠️ exp 的参数是 2·|x|，**不带任何 ln2**。
# e^(2x) 就是 exp(2x)；ln2 只在 nnet_expf 内部做 2^k 分解时用，
# 那已经封装掉了，调用方再乘一次就是把它当因子 —— 试过 a*LOG2E（1.4427）
# 和 a*TWO_LN2（1.3863），两次都错，tanh(0.5) 会算成 0.3333（=1/3）。
# 真值是 0.4621。两处必须同步。
def nnet_tanhf(x):
    if x != x:
        return 0.0
    if x > 9.0:
        return 1.0
    if x < -9.0:
        return -1.0
    a = -x if x < 0.0 else x
    sgn = -1 if x < 0.0 else 1
    e2 = nnet_expf(f32(a + a))          # 2·|x|，注意不是 a*TWO_LN2
    u = f32(e2 - 1.0)
    th = f32(u / f32(u + 2.0))
    # clamp 到 [-32767, 32767]/32768（C 里是 (int32_t)(th*32768 + 0.5)）
    q = int(math.floor(f32(th * 32768.0) + 0.5)) if th >= 0 else \
        int(math.ceil(f32(th * 32768.0) - 0.5))
    q = max(-32767, min(32767, q))
    th = f32(q / 32768.0)
    return -th if sgn < 0 else th


# ---- rsqrtf：与 nnet_math.c 完全一致 ------------------------------
def nnet_rsqrtf(x):
    if x != x or x <= 0.0 or x > 3.4e38:
        return 0.0
    bits = struct.unpack("I", struct.pack("f", f32(x)))[0]
    bits = (0x5F375A85 - (bits >> 1)) & 0xFFFFFFFF
    r = struct.unpack("f", struct.pack("I", bits))[0]
    r = f32(r * f32(1.5 - f32(f32(0.5 * x) * f32(r * r))))
    r = f32(r * f32(1.5 - f32(f32(0.5 * x) * f32(r * r))))
    if r > 1e30:
        return 1e30
    if r < 1e-30:
        return 0.0
    return r


# ---- nnet_lnf / nnet_powf：与 nnet_math.c 完全一致 ----------------
import struct as _s


def nnet_lnf(x):
    if x != x or x <= 0.0:
        return -88.0
    bits = _s.unpack("I", _s.pack("f", f32(x)))[0]
    e = ((bits >> 23) & 0xFF) - 127
    bits = (bits & 0x007FFFFF) | 0x3F800000
    m = _s.unpack("f", _s.pack("I", bits))[0]
    if m > 1.4142135:
        m = f32(m * 0.5)
        e += 1
    z = f32(f32(m - 1.0) / f32(m + 1.0))
    z2 = f32(z * z)
    # ln(m) = 2z(1 + z²·[1/3 + z²/5 + z⁴/7 + z⁶/9 + …])
    # ⚠️ sh 里**没有常数 1**，1 在 sh 外面、z² 也要在。
    #   少了 z²*sh这一层 -> ln(0.9) 得 -0.0351（真 -0.1054）
    #   少了 sh 里的 1 -> ln(0.9) 得 -0.1404（真 -0.1054）
    # 两处必须同步。
    sh = 0.1111111111
    sh = f32(0.1428571429 + f32(z2 * sh))
    sh = f32(0.2 + f32(z2 * sh))
    sh = f32(0.3333333333 + f32(z2 * sh))
    lnm = f32(f32(2.0 * z) * f32(1.0 + f32(z2 * sh)))
    return f32(lnm + f32(float(e) * 0.6931471805599453))


def nnet_powf(x, e):
    if x != x or e != e:
        return 0.0
    if x <= 0.0:
        return 0.0
    if x == 1.0:
        return 1.0
    return nnet_expf(f32(e * nnet_lnf(x)))


def main():
    # expf 工作区：softmax 减过 max 后恒 <=0；GELU 里 ±3.5
    xs = [-80, -70, -60, -50, -40, -30, -20, -15, -12, -10, -8, -6, -5,
          -4, -3, -2.5, -2, -1.75, -1.5, -1.25, -1.0, -0.875, -0.75,
          -0.625, -0.5, -0.375, -0.25, -0.125, -0.0625, -0.03125,
          -0.015625, 0.0]
    xs = [f32(v) for v in xs]

    print("=== expf 相对误差（softmax 工作区 x<=0）===")
    worst = 0.0
    worst_x = 0.0
    for x in xs:
        ref = math.exp(x)          # float64 参考
        got = nnet_expf(x)
        if ref < 1e-35:
            continue
        rel = abs(got - ref) / ref
        if rel > worst:
            worst, worst_x = rel, x
        if abs(x) in (80.0, 20.0, 1.0, 0.5, 0.125, 0.015625) or x == -40:
            print("  x=%12.6f  ref=%18.10e  got=%18.10e  rel=%.3e"
                  % (x, ref, got, rel))
    print("  -> 最大相对误差 %.4e  @ x=%.4f" % (worst, worst_x))

    print()
    print("=== expf 正半轴（饱和行为）===")
    for x in (1.0, 5.0, 20.0, 40.0, 80.0, 1e30):
        g = nnet_expf(f32(x))
        print("  expf(%-10g) = %-16.6g%s"
              % (x, g, "（钳位）" if x > 80 else ""))

    print()
    print("=== tanhf 绝对误差 ===")
    worst = 0.0
    worst_x = 0.0
    for x in xs + [f32(v) for v in (0.25, 0.75, 1.5, 3.0, 5.0, 9.0)]:
        ref = math.tanh(x)
        got = nnet_tanhf(x)
        d = abs(got - ref)
        if d > worst:
            worst, worst_x = d, x
        if abs(x) in (9.0, 3.0, 1.0, 0.5, 0.125):
            print("  x=%10.5f  ref=%14.9f  got=%14.9f  abs=%.3e"
                  % (x, ref, got, d))
    print("  -> 最大绝对误差 %.4e  @ x=%.4f" % (worst, worst_x))

    print()
    print("=== rsqrtf 相对误差 ===")
    rs = [1e-6, 1e-4, 1e-3, 1e-2, 0.1, 0.25, 0.5, 0.75, 1.0, 1.5, 2.0, 3.0,
          4.0, 6.0, 8.0, 16.0, 32.0, 64.0, 128.0, 256.0, 1024.0, 4096.0,
          16384.0, 65536.0, 1e6]
    worst = 0.0
    worst_x = 0.0
    for x in rs:
        xf = f32(x)
        ref = 1.0 / math.sqrt(x)
        got = nnet_rsqrtf(xf)
        rel = abs(got - ref) / ref
        if rel > worst:
            worst, worst_x = rel, xf
        if x in (1e-6, 1.0, 128.0, 1e6, 0.25):
            print("  x=%12.6g  ref=%16.10g  got=%16.10g  rel=%.3e"
                  % (xf, ref, got, rel))
    print("  -> 最大相对误差 %.4e  @ x=%.4g" % (worst, worst_x))

    print()
    print("=== 边界与垃圾输入 ===")
    nan = float("nan")
    inf = float("inf")
    checks = [
        ("expf(NaN) 须 0", nnet_expf(nan) == 0.0, nnet_expf(nan)),
        ("tanhf(NaN) 须 0", nnet_tanhf(nan) == 0.0, nnet_tanhf(nan)),
        ("rsqrtf(NaN) 须 0", nnet_rsqrtf(nan) == 0.0, nnet_rsqrtf(nan)),
        ("rsqrtf(-3.5) 须 0", nnet_rsqrtf(-3.5) == 0.0, nnet_rsqrtf(-3.5)),
        ("rsqrtf(+Inf) 须 0", nnet_rsqrtf(inf) == 0.0, nnet_rsqrtf(inf)),
        ("expf(-Inf) 须 0", nnet_expf(-inf) == 0.0, nnet_expf(-inf)),
        ("expf(+Inf) 须>0", nnet_expf(inf) > 0.0, nnet_expf(inf)),
        ("tanhf(9) 须≈1", abs(nnet_tanhf(9.0) - 1.0) < 1e-3, nnet_tanhf(9.0)),
        ("tanhf(-9) 须≈-1", abs(nnet_tanhf(-9.0) + 1.0) < 1e-3, nnet_tanhf(-9.0)),
        ("tanhf(20) 须 1", nnet_tanhf(20.0) == 1.0, nnet_tanhf(20.0)),
    ]
    allok = True
    for name, ok, got in checks:
        print("  %-22s %s   实得 %.6g" % (name, "PASS" if ok else "FAIL", got))
        allok = allok and ok

    print()
    print("=== nnet_lnf（powf 的底座）===")
    wl = 0.0
    wl_x = 0.0
    for x in [1e-6, 1e-4, 1e-3, 0.01, 0.05, 0.1, 0.2, 0.3, 0.4, 0.5,
              0.6, 0.7, 0.8, 0.9, 0.95, 0.99, 0.999, 1.0]:
        ref = math.log(x)
        got = nnet_lnf(f32(x))
        d = abs(got - ref)
        if d > wl:
            wl, wl_x = d, x
        if x in (1e-6, 0.1, 0.5, 0.9, 0.999):
            print("  ln(%-8.5g)真=%14.8f  实=%14.8f  abs=%.3e"
                  % (x, ref, got, d))
    print("  -> 最大绝对误差 %.4e@ x=%g" % (wl, wl_x))

    print()
    print("=== nnet_powf（采样的1/T 次幂）===")
    # nnet_pick 把 T 夹在 [0.05,4] -> e=1/T ∈ [0.25,20]
    # p 是 top-8 概率，实测落在 (0,1] 且>= ~1e-4
    wp = 0.0
    wp_d = ("", 0.0, 0.0)
    for p in (1.0, 0.9, 0.7, 0.5, 0.3, 0.1, 0.03, 0.01, 0.003, 0.001,
              1e-4, 1e-5):
        for T in (0.05, 0.25, 0.5, 1.0, 2.0, 4.0):
            e = 1.0 / T
            ref = p ** e
            got = nnet_powf(f32(p), f32(e))
            if ref > 1e-30:
                rel = abs(got - ref) / ref
                if rel > wp:
                    wp, wp_d = rel, (p, T, e)
            if p in (0.5, 0.1) and T in (0.25, 1.0, 4.0):
                print("  p=%-7.4g T=%-5.2g 真=%14.8f 实=%14.8f rel=%.3e"
                      % (p, T, ref, got,
                         abs(got - ref) / ref if ref > 1e-30 else 0))
    print("  -> 最大相对误差 %.4e  @ p=%g T=%g (e=%.3f)"
          % (wp, wp_d[0], wp_d[1], wp_d[2]))

    print()
    print("=== powf 边界 ===")
    for label, cond in [
        ("powf(0, 2) = 0", nnet_powf(0.0, 2.0) == 0.0),
        ("powf(-1, 2) = 0", nnet_powf(-1.0, 2.0) == 0.0),
        ("powf(1, 任意) = 1", nnet_powf(1.0, 7.0) == 1.0),
        ("powf(NaN, 2) = 0", nnet_powf(float('nan'), 2.0) == 0.0),
        ("powf(0.5, NaN) = 0", nnet_powf(0.5, float('nan')) == 0.0),
        ("powf(1e-30, 20) 不溢出", nnet_powf(1e-30, 20.0) < 1e30),
    ]:
        print("  %-24s %s" % (label, "PASS" if cond else "FAIL"))
        allok = allok and cond

    print()
    print("=== 判据（必须落在确定值上）===")
    verdicts = [
        ("expf  相对误差 < 1e-3", worst if False else 0, None),
    ]
    #重新取三个 worst
    we = 0.0
    for x in xs:
        ref = math.exp(x)
        if ref < 1e-35:
            continue
        we = max(we, abs(nnet_expf(x) - ref) / ref)
    wt = 0.0
    for x in xs + [0.25, 0.75, 1.5, 3.0, 5.0, 9.0]:
        wt = max(wt, abs(nnet_tanhf(x) - math.tanh(x)))
    wr = 0.0
    for x in rs:
        ref = 1.0 / math.sqrt(x)
        wr = max(wr, abs(nnet_rsqrtf(f32(x)) - ref) / ref)

    print("  expf  相对误差 < 1e-3   : %.4e  %s"
          % (we, "PASS" if we < 1e-3 else "FAIL"))
    print("  tanhf 绝对误差 < 1e-3   : %.4e  %s"
          % (wt, "PASS" if wt < 1e-3 else "FAIL"))
    print("  rsqrtf 相对误差 < 1e-3  : %.4e  %s"
          % (wr, "PASS" if wr < 1e-3 else "FAIL"))
    #ln 判据：powf 的底座。1e-4 绝对误差 -> pow 相对误差 1e-4*e <= 2e-3，
    # 略超1e-3，所以实际要求更紧。经验上能到 1e-6，这里定1e-5。
    print("  lnf   绝对误差 < 1e-5   : %.4e  %s"
          % (wl, "PASS" if wl < 1e-5 else "FAIL"))
    # powf 判据：相对误差要够小，否则采样分布会被悄悄改掉。
    # T=4(e=0.25) 时分布被压平、T=0.05(e=20) 时被拉尖，
    # 1e-3 的相对误差已经能看出分布形状变了，所以定 1e-3。
    print("  powf  相对误差 < 1e-3   : %.4e  %s"
          % (wp, "PASS" if wp < 1e-3 else "FAIL"))
    ok = we < 1e-3 and wt < 1e-3 and wr < 1e-3 and wl < 1e-5 \
        and wp < 1e-3 and allok
    print()
    print("======= 全部通过 =======" if ok else "======= 有失败项 =======")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
