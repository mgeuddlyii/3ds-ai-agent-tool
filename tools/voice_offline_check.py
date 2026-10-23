#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
voice_offline_check.py —— 语音输入的离线判据（**不开模拟器、不联网、零积分**）

背景：Azahar 里麦克风永远采不到真声音（LLE 的 MIC 要真 3DS 麦克风硬件），
而且用户明确不许开模拟器。于是"改完语音这块有没有坏 / 优化有没有生效"
只能靠下面四路离线证据。四路合起来是为了堵一条铁律：

    ★ 复刻品只能证明"复刻品对"，证明不了"C 代码对"。
      → 所以凡是脚本里写死的数字/条件，都**从源码里读回来**再做断言，
        而不是脚本里另抄一份（抄的那份会在改动后悄悄失同步，
        然后一路全绿 —— 也就是"永远绿的守卫比没有更坏"）。

  A. 同源常量：STRIDE / GATE / VAD_MS 与 voice_pcm 的三道守卫，
     直接从 shared/voice.c 里正则抠出来，脚本与 C 必须一致。
  B. 抽样误差：复刻 measure_rms（STRIDE + u16 整字读）对全扫的相对误差，
     以及"档位没变"（档位门限从 sc_chat.c 的 voice_bar 里抠）。
  C. 裁剪逻辑：复刻 voice_pump 的 loud 跟踪 + voice_pcm 的返回，
     跑边界用例（全程静音 / 首字节就有声 / 末段才有声 / 头尾都静音），
     并断言一条强不变量：**送上去的每一个字节所在的那一段 rms 都过门限**。
  D. 状态行宽度：直接解析 app/data/font.bin（AFNT v1），复刻
     app/source/gfx/text.c 的 text_width()，量"正听…[======] 5.0s 松 ZL"——
     这是模拟器里 layout_overflow 判据（384px）的等价离线替身，
     量的是**真字库**，不是估算。

用法：python tools/voice_offline_check.py
退出码 0 = 全绿。
"""

import os
import re
import struct
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
VOICE_C = os.path.join(ROOT, "shared", "voice.c")
VOICE_H = os.path.join(ROOT, "shared", "voice.h")
CHAT_C = os.path.join(ROOT, "app", "source", "ui", "sc_chat.c")
FONT = os.path.join(ROOT, "app", "data", "font.bin")

W_TOP_USABLE = 384          # app/source/ui/widgets.h
VOICE_BPS = 16360 * 2       # 16360Hz × 16bit

PROBLEMS = []


def fail(msg):
    PROBLEMS.append(msg)
    print("  [FAIL] %s" % msg)


def ok(msg):
    print("  [OK]   %s" % msg)


def head(t):
    print("\n=== %s ===" % t)


def read(p):
    with open(p, "r", encoding="utf-8") as f:
        return f.read()


# ---------------------------------------------------------------- A. 同源常量
def grab_const(path, name):
    """从 C 源码里抠 #define 的数值。抠不到就是**失败**，不是给个默认值
    —— 给默认值会让脚本在源码被改坏之后仍然跑出绿。"""
    src = read(path)
    # 尾注释（`#define X 800    /* ... */`）很常见，所以只要求"数字后面不是标识符字符"
    m = re.search(r"#define\s+%(n)s\s+(\d+)\b" % {"n": re.escape(name)}, src)
    if not m:
        fail("%s 里找不到 #define %s（正则没抠到）" % (os.path.basename(path), name))
        return None
    try:
        return int(m.group(1))
    except ValueError:
        fail("%s 的 %s 不是数字：%r" % (os.path.basename(path), name, m.group(1)))
        return None


def check_same_source():
    head("A. 常量与守卫必须和 C 源码同源")

    # 常量分散在 voice.c（实现内的）和 voice.h（对外契约），两处都抠
    stride = grab_const(VOICE_C, "VOICE_RMS_STRIDE")
    gate = grab_const(VOICE_H, "VOICE_RMS_GATE")
    vad = grab_const(VOICE_H, "VOICE_VAD_MS")
    rate = grab_const(VOICE_H, "VOICE_RATE")
    cap = grab_const(VOICE_H, "VOICE_MAX_MS")
    if None in (stride, gate, vad, rate, cap):
        return None
    if None in (rate, cap):
        return None
    cap_bytes = rate * (cap // 1000) * 2
    ok("VOICE_RATE=%d VOICE_MAX_MS=%dms → 整段上限 %d 字节"
       % (rate, cap, cap_bytes))
    # ★ 这条专门盯"毫秒当字节用"这类单位错：voice.h 里那个算式漏了 /1000
    # 的时候（VOICE_CAP_BYTES 曾算成 1.6 亿）就是这么悄悄爽过去的。
    if "#define VOICE_CAP_BYTES  (VOICE_RATE * (VOICE_MAX_MS / 1000) * 2)" \
            not in read(VOICE_H):
        fail("voice.h 的 VOICE_CAP_BYTES 算式不是 VOICE_RATE * (VOICE_MAX_MS / 1000) * 2"
             " —— 脚本按这个换算，算式变了要一起改")

    src = read(VOICE_C)
    for pat in (r"g_loud_begin\s*>=\s*0\s*&&",
                r"g_loud_end\s*>\s*g_loud_begin\s*&&",
                r"g_loud_end\s*<=\s*g_total"):
        if not re.search(pat, src):
            fail("voice.c 缺 voice_pcm 的守卫 %s —— 脚本按它复刻，丢了就测了个寂寞" % pat)
    else:
        ok("voice_pcm 的三道守卫都在（脚本复刻的前提成立）")

    # 采样步长：C 里是 `i += 2 * VOICE_RMS_STRIDE`，脚本里也必须是同一个式
    if "i += 2 * VOICE_RMS_STRIDE" not in src:
        fail("measure_rms 的步长不再是 `i += 2 * VOICE_RMS_STRIDE`，脚本的复刻要跟着改")
    else:
        ok("measure_rms 步长 = 2*%d 字节（脚本同式）" % stride)

    # 首尾留白：常量在 voice.h，算式与三处 clamp 在 voice.c —— 都得对上号
    pad = grab_const(VOICE_H, "VOICE_EDGE_PAD_MS")
    if pad is not None:
        if "#define VOICE_EDGE_PAD_BYTES ((VOICE_RATE * VOICE_EDGE_PAD_MS / 1000) * 2)" \
                not in read(VOICE_C):
            fail("voice.h 的 VOICE_EDGE_PAD_BYTES 算式变了，脚本按原式换算"
                 "（VOICE_RATE * VOICE_EDGE_PAD_MS / 1000 * 2）")
        else:
            pad_bytes = (rate * pad // 1000) * 2
            ok("首尾留白 VOICE_EDGE_PAD_MS=%d → %d 字节（脚本同式）"
               % (pad, pad_bytes))
        for pat in ("int b = g_loud_begin - VOICE_EDGE_PAD_BYTES;",
                    "int e = g_loud_end   + VOICE_EDGE_PAD_BYTES;",
                    "if (b < 0)", "if (e > g_total)"):
            if pat not in read(VOICE_C):
                fail("voice.c 的 voice_pcm 里少了 `%s` —— 脚本的复刻要跟着改"
                     "（而且少了 clamp 会发出越界长度！）" % pat)
        else:
            ok("voice_pcm 的扩张 + 两处 clamp 都在（脚本同式）")

    if "0x10000u - s" not in src:
        fail("measure_rms 的 u16 整字读写法变了（不是 `s < 0x8000u ? s : 0x10000u - s`）")

    # voice_bar 的档位门限：从 sc_chat.c 抠数字，别在脚本里另抄一份
    chat = read(CHAT_C)
    m = re.search(r"static const char \*voice_bar\(int rms\).*?\n\}", chat, re.S)
    tiers = []
    if m:
        tiers = [int(x) for x in re.findall(r"rms\s*<\s*(\d+)", m.group(0))]
    if not tiers:
        fail("sc_chat.c 的 voice_bar 里没抠到档位门限 —— 6 格条是不是被改回去了？")
    else:
        ok("voice_bar 档位门限 %s（%d 档 → %d 格）从 sc_chat.c 原样取出"
           % (tiers, len(tiers), len(tiers)))

    return {"stride": stride, "gate": gate, "vad": vad,
            "rate": rate, "cap": cap, "tiers": tiers,
            "pad_ms": pad if pad is not None else 0}


def voice_bar_tiers():
    """从 sc_chat.c 的 voice_bar() 里抠出档位门限（6 格条 = 7 个门限）。
    抠不到就**不算过**，不提供兜底默认值 ——
    ★「永远绿的守卫比没有更坏」：给个写死的默认值，源码改坏了脚本照样绿。"""
    m = re.search(r"static const char \*voice_bar\(int rms\).*?\n\}",
                  read(CHAT_C), re.S)
    if not m:
        fail("sc_chat.c 找不到 voice_bar() —— 音量条函数被改了？")
        return []
    t = [int(x) for x in re.findall(r"rms\s*<\s*(\d+)", m.group(0))]
    if not t:
        fail("voice_bar() 里没抠到档位门限")
        return []
    return t


# ---------------------------------------------------------------- B. 抽样误差
def rms_full(pcm, n):
    """修改前的写法：每个采样两次 ldrb。"""
    acc = 0
    taken = 0
    for i in range(0, n - 1, 2):
        s = pcm[i] | (pcm[i + 1] << 8)
        acc += s if s < 0x8000 else 0x10000 - s
        taken += 1
    return acc // taken if taken else 0


def rms_stride(pcm, n, stride):
    """现状：STRIDE 抽样 + 一次 ldrh（步长 2*STRIDE 字节，i 恒为偶数）。"""
    acc = 0
    taken = 0
    i = 0
    while i + 1 < n:
        s = pcm[i] | (pcm[i + 1] << 8)
        acc += s if s < 0x8000 else 0x10000 - s
        taken += 1
        i += 2 * stride
    return acc // taken if taken else 0


def synth(amp, n, wave="sine"):
    """确定性信号（不用随机库：随机在 PC 上每次跑出的数不一样，判据就没法复现）。

    ⚠️ 光有"周期正弦"是不够的：周期和 STRIDE 一整除，抽样每次都抽到同一个
    相位，误差恰好是 0%，看着漂亮其实什么都没测（★ 永远绿的守卫比没有更坏）。
    所以这里额外给一条**伪噪声**（定种子 LCG）—— 它的子相位是乱的，
    抽到的样本和解到的样本真的不是同一批，误差才是抽样本身的误差。"""
    import math

    import math

    out = bytearray(n & ~1)
    noise = wave in ("lcg", "lcgsay")

    for k in range(len(out) // 2):
        if wave == "dc":
            v = float(amp)
        elif wave == "square":
            v = float(amp if (k % 4 < 2) else 0)
        elif noise:
            # 定种子 LCG（不用 random 模块：那玩意儿换个 Python 版本就换数，
            # 判据就不可复现了）。取高 16 位当样本，子相位是乱的。
            seed = (k * 1103515245 + 12345) & 0x7FFFFFFF
            v = float((seed >> 16) & 0xFFFF)
            if wave == "lcgsay":                # 伪噪声 + 音节包络，更像说话
                v *= 0.5 + 0.5 * math.sin(2 * math.pi * 2.3 * k / 4.0)
                v *= amp / 3000.0
        else:
            v = amp * (0.5 + 0.5 * math.sin(2 * math.pi * 3 * k / 4.0))

        v = max(0, min(0xFFFF, int(v)))
        out[2 * k] = v & 0xFF
        out[2 * k + 1] = (v >> 8) & 0xFF
    return bytes(out)


def check_sampling(cfg):
    head("B. 抽样（STRIDE=%d）相对全扫的误差"
         % (cfg["stride"] if cfg else 0))

    stride = cfg["stride"]
    tiers = voice_bar_tiers() or cfg["tiers"]
    cases = [
        ("安静房间（底噪 3）", synth(3, 4096, "dc")),
        ("轻语（RMS≈300）", synth(900, 4096)),
        ("正常说话（RMS≈1500）", synth(3000, 4096)),
        ("大声（RMS≈6000）", synth(12000, 4096)),
        ("削顶方波", synth(40000, 4096, "square")),
        ("伪噪声（子相位乱）", synth(3000, 4096, "lcg")),
        ("伪噪声+音节包络", synth(3000, 4096, "lcgsay")),
    ]
    worst = 0.0

    for name, buf in cases:
        n = len(buf)
        a = rms_full(buf, n)
        b = rms_stride(buf, n, stride)
        if a > 0:
            e = abs(b - a) / float(a)
            worst = max(worst, e)
        ta = len([i for i in range(0, n - 1, 2)])
        tb = len([i for i in range(0, n - 1, 2 * stride)])
        print("  %-22s full=%-6d stride=%-6d 相对误差 %5.1f%%  样本数 %d→%d（%.0f%%）"
              % (name, a, b, 100.0 * (abs(b - a) / a if a else 0.0), ta, tb,
                 100.0 * tb / ta))
        # 档位必须不变：档位变了，屏上那个条就会跳格，用户会以为坏了
        if tiers:

            def tier(v):
                t = 0
                for i, x in enumerate(tiers):
                    if v >= x:
                        t = i + 1
                return min(t, len(tiers))

            if tier(a) != tier(b):
                fail("%s：全扫档位 %d、抽样档位 %d —— 抽样改变了显示，不能这么抽"
                     % (name, tier(a), tier(b)))

    # ---- 步长里"质数"这一条：不是玄学，是抽样最常见的翻车方式 ----
    # 抽样的步长是 2*STRIDE 字节 = STRIDE 个采样。若某个音的周期 P 整除 STRIDE，
    # 抽样每次都落在同一相位上 —— 等于只读了信号的一个值，读数可以差到 100%
    # （脚本里那条"削顶方波"就是 P=4、STRIDE=4，full=12768 而 stride=25536）。
    # 8kHz / 4kHz 纯音都在 16360Hz 采样的带内（Nyquist 8.18kHz），
    # 所以这不是假想敌。取质数可以让"会翻车的周期"只有 P=STRIDE 一个。
    head("B2. 步长选型的混叠扫描（周期 2..16 个采样）")
    print("  STRIDE  因子  最坏误差  会翻车的周期 P")
    chosen = cfg["stride"]
    for s in range(2, 9):
        worst_p = 0.0
        bad = []
        for p in range(2, 17):
            buf = bytearray()
            for k in range(4096):
                v = 3000 if (k % p < p // 2) else 0       # 该周期上的方波
                v = max(0, min(0xFFFF, v))
                buf += bytes((v & 0xFF, (v >> 8) & 0xFF))
            a = rms_full(bytes(buf), len(buf))
            b = rms_stride(bytes(buf), len(buf), s)
            e = abs(b - a) / float(a) if a else 0.0
            if e > worst_p:
                worst_p = e
            if e > 0.15:
                bad.append(p)
        fac = [x for x in range(2, s) if s % x == 0]
        print("    %d     %-5s  %6.1f%%   %s"
              % (s, fac or "-", 100.0 * worst_p, ",".join(map(str, bad)) or "无"))

    def is_prime(n):
        return n >= 2 and all(n % d for d in range(2, int(n ** 0.5) + 1))

    if not is_prime(chosen):
        fail("VOICE_RMS_STRIDE=%d 不是质数：周期整除它的纯音（%s）会让表头读数翻车，"
             "换成质数（当前 3）只留 P=%d 一个隐患"
             % (chosen, [p for p in range(2, chosen) if chosen % p == 0], chosen))
    else:
        ok("VOICE_RMS_STRIDE=%d 是质数 → 会翻车的周期只剩 P=%d（其它周期都覆盖到）"
           % (chosen, chosen))

    n_case = len([1 for _ in cases])
    if worst < 0.15:
        ok("最坏相对误差 %.1f%%，且 %d 种信号档位全一致" % (100 * worst, n_case))
    else:
        fail("最坏相对误差 %.1f%% 偏大（>15%%），抽样会把 rms 数带跑" % (100 * worst))
    ok("样本数降到 1/%d（这就是这一版省下的循环次数）" % stride)


# ---------------------------------------------------------------- C. 裁剪逻辑
class Voice(object):
    """复刻 voice_pump 的 loud 跟踪 + voice_pcm 的返回。
    封顶（VOICE_CAP_BYTES）也要复刻：voice_pump 满了会 `break`，
    不复刻的话脚本会跑出"录了 7 秒"这种卡上根本不会出现的状态。"""

    def __init__(self, gate, cap, pad_bytes=0):
        self.gate = gate
        self.cap = cap
        self.pad_bytes = pad_bytes
        self.reset()

    def reset(self):
        self.total = 0
        self.loud_begin = -1
        self.loud_end = -1
        self.stopped = False

    def pump(self, n, rms):
        """搬 n 字节进来，这段的 RMS 是 rms。返回本段的起点。"""
        if self.stopped:
            return self.total
        room = self.cap - self.total
        if room <= 0:
            self.stopped = True
            return self.total
        if n > room:
            n = room

        seg = self.total
        if rms >= self.gate:
            if self.loud_begin < 0:
                self.loud_begin = seg
            self.loud_end = seg + n
        self.total += n
        return seg

    def pcm(self):
        """完全照抄 voice.c 的三道守卫 + 两头回吐 + 两处 clamp。"""
        if self.loud_begin >= 0 and self.loud_end > self.loud_begin and \
                self.loud_end <= self.total:
            b = self.loud_begin - self.pad_bytes
            e = self.loud_end + self.pad_bytes
            if b < 0:
                b = 0
            if e > self.total:
                e = self.total
            if e > b:
                return b, e - b
        return 0, self.total


def check_trim(cfg):
    head("C. 只上传有声音那一段（边界用例 + 强不变量）")

    gate = cfg["gate"]
    # VOICE_MAX_MS 是**毫秒**，字节封顶要按 voice.h 那个算式自己算，别直接用
    cap = cfg["rate"] * (cfg["cap"] // 1000) * 2
    pad_bytes = (cfg["rate"] * cfg["pad_ms"] // 1000) * 2 if cfg.get("pad_ms") else 0
    seg_b = 20 * VOICE_BPS // 1000            # 一帧 20ms ≈ 654 字节

    def build(plan):
        """plan = [(每帧 rms, 帧数), ...]"""
        v = Voice(gate, cap, pad_bytes)
        segs = []
        for rms, cnt in plan:
            for _ in range(cnt):
                s = v.pump(seg_b, rms)
                segs.append((s, v.total - s, rms))
        return v, segs

    cases = [
        ("头尾都静音，中间说话", [(0, 25), (2000, 75), (0, 75)]),
        ("全程静音（没到过门限）", [(0, 100)]),
        ("首帧就有声", [(2000, 50), (0, 50)]),
        ("末帧才收尾有声", [(0, 90), (2000, 10)]),
        ("两段话（中间 1s 静音）", [(0, 25), (2000, 25), (0, 50), (2000, 25)]),
        ("低到刚好不过门限", [(gate - 1, 50)]),
    ]

    for name, plan in cases:
        v, segs = build(plan)
        begin, n = v.pcm()
        total = v.total
        print("  %-22s captured=%d  sent=%d  (%.0f%%)"
              % (name, total, n, 100.0 * n / total if total else 0.0))

        if total == 0:
            fail("%s：录了 0 字节 —— 不可能" % name)
            continue
        if total > cap:
            fail("%s：total=%d 超过封顶 %d（脚本没复刻封顶）" % (name, total, cap))

        # 不变量 1：长度在 (0, total]
        if not (0 < n <= total):
            fail("%s：sent=%d 不在 (0, %d] 里" % (name, n, total))
        # 不变量 2：区间不越界
        if not (0 <= begin and begin + n <= total):
            fail("%s：区间 [%d,%d) 越出 [0,%d)" % (name, begin, begin + n, total))

        loud = [x for x in segs if x[2] >= gate]

        if loud:
            # 不变量 3：送的恰好是"第一处有声的起点 → 最后一处有声的终点"。
            # 中间那段静音（两段话之间的停顿）**故意保留** —— 它在一次上行里，
            # 切掉它不会省任何带宽（区间长度不变），却要多拷一份。
            f = loud[0][0]
            e = loud[-1][0] + loud[-1][1]
            # 两头各回吐 pad（再夹回 [0, total]）
            eb = max(0, f - pad_bytes)
            ee = min(total, e + pad_bytes)
            if begin != eb or begin + n != ee:
                fail("%s：送出区间 [%d,%d)，应该是 [%d,%d)"
                     "（第一处有声 %d 回吐 %d，末处 %d 回吐 %d，再夹到 [0,%d]）"
                     % (name, begin, begin + n, eb, ee, f, pad_bytes, e, pad_bytes, total))
            # 留白只能往外扩，不许把有声段咬掉
            if n < (e - f):
                fail("%s：留白之后送的反而比有声段还短（%d < %d）" % (name, n, e - f))
            # 不变量 4：任何一处有声都必须被送出去（不能悄悄裁掉一句）
            for s, ln, r in loud:
                if s < begin or s + ln > begin + n:
                    fail("%s：有声段 [start=%d len=%d rms=%d] 没被完整送出"
                         "（送出区是 [%d,%d)）" % (name, s, ln, r, begin, begin + n))
                if r < gate:
                    fail("%s：被当成有声的段 rms=%d < 门限 %d" % (name, r, gate))
        else:
            # 一次都没过门限 → 必须整段退回，宁可多传也不能送 0 字节
            if begin != 0 or n != total:
                fail("%s：没到过门限却没退回整段（begin=%d n=%d total=%d）"
                     % (name, begin, n, total))

    # 一节省流算账：录满 5 秒、说话约 1.5 秒
    v, _ = build([(0, 25), (2000, 75), (0, 150)])
    total = v.total
    begin, n = v.pcm()
    if total > 0 and n < total:
        print("  → 录满 %.2fs 只说了 %.2fs：上行 %d → %d 字节（省 %.0f%%）"
              % (total / float(VOICE_BPS), n / float(VOICE_BPS), total, n,
                 100.0 * (total - n) / total))
        ok("封顶 %d 字节内，只送 %d 字节" % (cap, n))
    else:
        fail("5s 场景没裁下来（sent == total），省流优化没生效")

    # 边界：全程没到门限必须退回整段，否则 do_asr 会判"没采到声音"
    v, _ = build([(0, 100)])
    begin, n = v.pcm()
    if begin == 0 and n == v.total:
        ok("全程静音 → 退回整段（do_asr 不会误判没采到声音）")
    else:
        fail("全程静音时 voice_pcm 退回整段的守卫没起作用")


# ---------------------------------------------------------------- D. 状态行宽度
def load_font(path):
    with open(path, "rb") as f:
        d = f.read()
    if d[0:4] != b"AFNT":
        raise ValueError("不是 AFNT")
    ver, _flags = struct.unpack_from("<HH", d, 4)
    cell_w, cell_h, base = d[8], d[9], d[10]
    count, ioff, boff, bsize, fb = struct.unpack_from("<IIIII", d, 12)
    # 2026-10-16 字体升级成 AFNT v2（8bit 灰度，见 tools/mkfont.py）。
    # v2 与 v1 的**头部字段布局完全一致**（ioff=32、IIIII 从 0x0C 起、
    # 索引条目仍是 8 字节 <IBBH），只是 ver 从 1 升到 2 —— cell_w/cell_h/base/
    # 索引表格式都没变。这里原本写死 ver==1，导致 v2 字库被误判成"版本不对"，
    # 连带整个语音 suite 误报 FAIL。放宽为接受 v1/v2（以后若再有 v3 且布局不变，
    # 这里只需加一个数字；若布局真变了，这条判据会先红，别静默吞）。
    if ver not in (1, 2) or cell_w == 0:
        raise ValueError("AFNT 版本/格宽不对（ver=%d）" % ver)
    adv = {}
    for i in range(count):
        cp, a, fl = struct.unpack_from("<IBB", d, ioff + i * 8)
        adv[cp] = a
    return cell_w, adv, base, fb, count


def text_width(s, cell_w, adv):
    """复刻 app/source/gfx/text.c 的 text_width()。"""
    w = 0
    i = 0
    b = s.encode("utf-8") if isinstance(s, str) else s
    while i < len(b):
        # 简易 UTF-8 解码（够用：只处理 1~3 字节）
        c = b[i]
        if c < 0x80:
            cp, n = c, 1
        elif c >= 0xE0:
            cp = ((c & 0x0F) << 12) | ((b[i + 1] & 0x3F) << 6) | (b[i + 2] & 0x3F)
            n = 3
        else:
            cp = ((c & 0x1F) << 6) | (b[i + 1] & 0x3F)
            n = 2
        w += adv.get(cp, cell_w)
        i += n
    return w


def check_status_width(cap=5000):
    head("D. 状态行宽度（解析真字库，替代模拟器的 384px 判据）")

    if not os.path.exists(FONT):
        fail("app/data/font.bin 不存在 —— 没法离线量宽度，先跑一次 python tools/build.py")
        return
    cell_w, adv, base, fb, count = load_font(FONT)
    print("  字库：%d 字形，cell_w=%d，fallback=#%d" % (count, cell_w, fb))

    tiers = voice_bar_tiers()

    def bar(filled):
        return "[" + "=" * filled + " " * (len(tiers) - filled) + "]"

    # 里头那个数字只能是 voice_ms() = total*1000/32720 印出来的十分之一秒，
    # 所以取值空间就是 t = 0..(VOICE_MAX_MS/100)，这里按 cap 推，别写死
    lines = []
    for filled in range(0, len(tiers) + 1):
        for t in range(0, cap // 100 + 1):
            lines.append("正听…%s %d.%ds 松 ZL" % (bar(filled), t // 10, t % 10))
    # 等宽（比如 0.0s 和 4.9s 一样宽）时取**最后一个**，别取第一个 ——
    # 取第一个会报出"0.0s 最宽"，看的人还以为 0.0s 比 4.9s 长。
    worst = ("", -1)
    for s in lines:
        w = text_width(s, cell_w, adv)
        if w >= worst[1]:
            worst = (s, w)
        if w > W_TOP_USABLE:
            fail("状态行 %r 宽 %dpx > %dpx（会撑爆布局）" % (s, w, W_TOP_USABLE))

    print("  最宽：%r = %dpx（上限 %d，余量 %d）"
          % (worst[0], worst[1], W_TOP_USABLE, W_TOP_USABLE - worst[1]))
    if worst[1] <= W_TOP_USABLE:
        ok("全部 %d 条候选状态行都在 %dpx 内" % (len(lines), W_TOP_USABLE))

    # 原来 4 格条 vs 现在 6 格条，看 6 格到底多占多少
    w4 = text_width("正听…[%s] 5.0s 松 ZL" % ("==" + "  " * 2), cell_w, adv)
    w6 = text_width("正听…[%s] 5.0s 松 ZL" % ("=" * 6), cell_w, adv)
    print("  4 格条 %dpx → 6 格条 %dpx（多 %dpx）" % (w4, w6, w6 - w4))
    if w6 <= W_TOP_USABLE:
        ok("6 格条（%dpx）没有把状态行撑宽" % w6)


def main():
    cfg = check_same_source()
    if cfg:
        check_sampling(cfg)
        check_trim(cfg)
    check_status_width(cfg["cap"] if cfg else 5000)

    print("\n-----------------------------------------------")
    if PROBLEMS:
        print("FAIL：%d 条" % len(PROBLEMS))
        for p in PROBLEMS:
            print("  - %s" % p)
        return 1
    print("ALL PASS（离线，未开模拟器、未联网）")
    return 0


if __name__ == "__main__":
    sys.exit(main())
