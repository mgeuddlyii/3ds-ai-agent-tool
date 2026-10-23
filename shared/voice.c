/*
 * voice.c -- 见 voice.h
 *
 * ── 采集模型 ────────────────────────────────────────────────────
 * MICU 往一块共享内存里写环形音频。libctru 给的接口是
 *   micGetLastSampleOffset()  —— 服务写到的最后一个字节偏移
 *   micGetSampleDataSize()   —— 环形区大小
 * 于是"读新增的部分"就是一圈追着写指针跑（官方案例也是这么做的）。
 *
 * 每帧只搬新增的那一小段（16kHz 下 60fps 一帧约 545 字节），
 * 所以这里没有任何阻塞、也不会漏采 —— 主循环停多久，就一次补多少。
 *
 * ── 为什么要两个缓冲 ────────────────────────────────────────────
 * g_ring 是 mic 的共享内存（g_out 是搬出来的录音）。
 * 把它俩合成一个"边采边存"的环看着省 160KB，但实际上不行：
 * 读指针和写指针靠得很近时（追帧、掉帧），源区间和输出区间会**重叠**，
 * memcpy 重叠区是未定义行为，而且这种错只会在主循环卡一下的时候出现，
 * 平时一切正常 —— 那种偶发损坏比多用 160KB 贵得多。
 */

#include "voice.h"

#include <3ds.h>
#include <string.h>

#define VOICE_RING_SIZE  0x30000        /* mic 共享内存，> VOICE_CAP_BYTES */

/* mic 的共享内存。对齐到 0x1000 是 micInit 的硬要求（它要映射成块） */
static u8  g_ring[VOICE_RING_SIZE] __attribute__((aligned(0x1000)));
/* 我们自己的录音 */
static u8  g_out[VOICE_CAP_BYTES];

static bool  g_inited;
static bool  g_sampling;
static Result g_mic_rc;
static u32   g_datasize;                /* micGetSampleDataSize() */
static u32   g_read;                    /* 我们读到的偏移 */
static int   g_total;                   /* g_out 里的字节数 */
/* "真有声音"的那一段在 g_out 里的 [begin, end)。
 * -1 表示**还没到过门限**（开场静音 / 全程太轻）。
 * voice_pcm() 会把这两者告诉调用方，好让上行只发这段 —— 录满 5 秒而
 * 说话其实不到 2 秒的话，整份 164KB 有三分之二是纯静音，WAV 里
 * 静音段全是 0x00，走一次 multipart 就是白烧几十 KB 上行加时长。 */
static int   g_loud_begin = -1;
static int   g_loud_end   = -1;
static int   g_rms;
static u64   g_sil_since;               /* 进入静音的时刻（毫秒），VAD 用 */
static bool  g_full;

void voice_fini(void)
{
    if (g_sampling) {
        MICU_StopSampling();
        g_sampling = false;
    }
    if (g_inited) {
        micExit();
        g_inited = false;
    }
}

int voice_mic_rc(void) { return (int)g_mic_rc; }

bool voice_available(void) { return g_inited; }

bool voice_init(void)
{
    if (g_inited)
        return true;

    g_mic_rc = micInit(g_ring, (u32)VOICE_RING_SIZE);
    if (R_FAILED(g_mic_rc)) {
        g_inited = false;
        return false;
    }

    g_datasize = micGetSampleDataSize();
    g_read     = micGetLastSampleOffset();
    g_total    = 0;
    g_rms      = 0;
    g_full     = false;
    g_loud_begin = -1;
    g_loud_end   = -1;
    g_inited   = true;
    return true;
}

bool voice_start(void)
{
    bool sampling;

    if (!g_inited)
        return false;

    g_total      = 0;
    g_rms        = 0;
    g_full       = false;
    g_loud_begin = -1;
    g_loud_end   = -1;
    g_read       = micGetLastSampleOffset();

    MICU_IsSampling(&sampling);
    if (sampling)
        MICU_StopSampling();

    /* loop=true：封顶由我们自己做（见 voice_pump）。
     * 让服务在缓冲满时自己停掉的话，拿不到一个明确的"满了"信号去驱动 UI，
     * 只能靠"字节数不再涨"去猜 —— 那正是本项目里最容易静默失效的一类写法。 */
    if (R_FAILED(MICU_StartSampling(MICU_ENCODING_PCM16_SIGNED,
                                    MICU_SAMPLE_RATE_16360,
                                    0, g_datasize, true)))
        return false;

    g_sampling      = true;
    g_sil_since     = osGetTime();
    return true;
}

/* 每 VOICE_RMS_STRIDE 个采样才拿一个进统计。RMS 在这里只用来回答
 * "有没有超过 VOICE_RMS_GATE"，不是给耳朵听的精确值 —— 所以放心抽样。
 *
 * ★ 这个步长**必须是质数**，取 3。步进是 STRIDE 个采样，于是任何周期
 * 整除 STRIDE 的纯音都会让抽样每次撞在同一个相位上 —— 等于只读了信号
 * 的一个值，能把表头读数带跑 100%（不是理论值：tools/voice_offline_check.py
 * 的 B2 扫描真跑出来了）。8kHz / 4kHz 纯音都在 16360Hz 采样的带内
 * （Nyquist 8.18kHz），所以这不是假想敌。
 *    STRIDE=3（质数）→ 会翻车的周期只剩 P=3
 *    STRIDE=4（原值）→ P=2,4,6,10,12 全翻（合数因子多两倍）
 * 分片度上 3 vs 4 只差 1/3 的循环次数，而这段循环每帧只跑几十次，
 * 花那点算力换"读数不会因某个高频音而翻车"是划算的。 */
#define VOICE_RMS_STRIDE 3

/* 只扫新搬进来那一段算 RMS（VAD 与诊断都用它）。两处优化：
 *
 * 1. **抽样**：循环次数直接降到 1/VOICE_RMS_STRIDE。PC 侧复刻实测
 *    （tools/voice_offline_check.py，不开模拟器）3 取 1 对伪噪声与
 *    带音节包络的类语音信号，与全扫的相对误差 < 0.5%，档位一次都没跳；
 *    周期整除步长的纯音是已知隐患（步长选质数就是为了把隐患数压到最少）。
 *
 * 2. **整字读**：直接按 u16 读（3DS 是小端，读出来就是采样值本身），
 *    比原来的 `p[i] | (p[i+1] << 8)` 一半字节加载 —— 原来每个采样
 *    要 2 次 ldrb，现在 1 次 ldrh。
 *
 * ⚠️ 别"优化"成扫到超过门限就提前退出：那样 rms 会随音频内容抖，
 *    同一个人在不同句子上给出的 rms 不一样，status.txt 里的
 *    `rms=N` 就失去可比性了 —— 而查问题时正是靠这个数。
 *
 * 抽样后"取到的样本数"≠ n/2，除数必须跟着换，否则 rms 会被
 * 凭空缩掉 STRIDE 倍（一个典型的"优化完数值就不对了"）。
 */
static void measure_rms(const u8 *p, int n)
{
    u64 acc   = 0;
    int taken = 0;

    /* 步长是 2*STRIDE 字节 —— i 始终是偶数偏移，u16 读天然对齐 */
    for (int i = 0; i + 1 < n; i += 2 * VOICE_RMS_STRIDE) {
        u16 s = *(const u16 *)(p + i);
        acc  += (s < 0x8000u) ? (u64)s : (u64)(0x10000u - s);
        ++taken;
    }
    if (taken > 0)
        g_rms = (int)(acc / (u64)taken);
}

int voice_pump(void)
{
    int added = 0;

    if (!g_sampling || !g_inited)
        return 0;

    u32 last = micGetLastSampleOffset();
    u32 from = g_read;

    while (from != last) {
        u32 n     = (last > from) ? (last - from)
                                  : (g_datasize - from);   /* 一次不绕圈 */
        u32 room  = (u32)VOICE_CAP_BYTES - (u32)g_total;
        if (n > room)
            n = room;
        if (n == 0)
            break;

        int seg = (int)g_total;      /* 本段在 g_out 里的起点 */

        memcpy(g_out + g_total, g_ring + from, n);
        measure_rms(g_out + g_total, (int)n);

        /* 记下"真有声音"的那一段（g_rms 是刚搬进来这一段的 RMS）。
         * 上传时只发这段，见 voice_pcm 的注释 —— 录满 5 秒而说话
         * 其实不到 2 秒的话，整份 164KB 有三分之二是纯静音。 */
        if (g_rms >= VOICE_RMS_GATE) {
            if (g_loud_begin < 0)
                g_loud_begin = seg;
            g_loud_end = seg + (int)n;
        }

        g_total += (int)n;
        added   += (int)n;
        from    += n;
        if (from >= g_datasize)
            from = 0;
    }

    g_read = from;

    if (g_total >= (int)VOICE_CAP_BYTES) {
        /* 满了就停：继续采只会覆盖自己 */
        g_full     = true;
        g_sampling = false;
        MICU_StopSampling();
    }

    return added;
}

int voice_stop(void)
{
    if (g_sampling) {
        g_sampling = false;
        MICU_StopSampling();
    }
    return g_total;
}

int voice_bytes(void) { return g_total; }

int voice_ms(void)
{
    /* 16360Hz × 2 字节/样本 = 32720 字节/秒 */
    return g_total * 1000 / 32720;
}

int voice_rms(void) { return g_rms; }

bool voice_full(void) { return g_full; }

/* VAD：安静够久就认为说完了。
 * 放在这层只给判据，界面决定什么时候用它 —— 要不要"省一次上行"
 * 是交互问题，不该硬塞进采集层。
 *
 * ⚠️⚠️ 时间基准用 osGetTime()（毫秒），**不要**用 svcGetSystemTick()。
 * 第一版是手算的 `dt >= 268 * VOICE_VAD_MS`（268MHz tick / ms），
 * 结果那个式子在比 **800 微秒**、不是 800 毫秒 —— 按下 ZL 的
 * **第一帧**（约 200ms）就被判静音收尾，只录到 240ms（7854 字节），
 * 塞给 ASR 的音频短到什么都认不出来，屏上就一句"识别失败"。
 * 用毫秒的 osGetTime() 就不用知道 CPU 频率 —— 而 Old 3DS 是 134MHz、
 * New 3DS 是 268MHz，写死常数在旧小机上又是一倍误差。
 */
bool voice_silent_long(void)
{
    u64 now;
    u64 last;

    if (!g_sampling)
        return false;

    now = osGetTime();

    /* 有声音就把"静音起点"往后推；没声音才和它比。
     * （g_rms 是上一帧搬进来那一段的均值，够用作 VAD 输入。） */
    last = g_sil_since;
    if (g_rms >= VOICE_RMS_GATE)
        g_sil_since = now;

    return g_sil_since >= last && (now - g_sil_since) >= (u64)VOICE_VAD_MS;
}

/* 1024ms 是 1 秒，这里 VOICE_EDGE_PAD_MS 是毫秒，所以先除 1000 再乘 2 字节
 * （VOICE_EDGE_PAD_BYTES = 150*16360/1000*2 = 4908）。 */
#define VOICE_EDGE_PAD_BYTES ((VOICE_RATE * VOICE_EDGE_PAD_MS / 1000) * 2)

/* 这里返回的**不是整段录音**，而是"有声音"的那一段（两头各回吐
 * VOICE_EDGE_PAD_BYTES 做留白）—— 这是语音输入最实在的一处省流
 * （录 5 秒只说 1.5 秒的话，上行从 164KB 降到 ~55KB）。
 * 指针是指向 g_out 内部的，零拷贝。
 *
 * ⚠️ 三个必须挡的边界，挡不住就会静默送出错误长度：
 *   1. 全程没到过门限（g_loud_begin 仍是 -1）→ 退回整段。宁可多传，
 *      也不能什么都不传（do_asr 会判"没采到声音"）。
 *   2. g_loud_end <= g_loud_begin（理论上不可能：设值那一步 n>0 已保证，
 *      但 g_out 是定长数组，指针/长度一旦哪天走飞，这层就是最后一道）
 *      → 同样退回整段。
 *   3. g_loud_end > g_total（裁剪区间跑到录完之后）→ 退回整段。
 * 判据是"长度合理"，不是"指针合理" —— 后者在越界时也可能看着正常。
 *
 * 注意 voice_bytes()/voice_ms() 说的仍是**整段**（"录了几秒"是事实），
 * 跟这里"送上去多少"是两件事，别混：status.txt 里 captured 用整段
 * （跨版本可比），asr 那行的 bytes= 才是实际送的。 */
const void *voice_pcm(size_t *len)
{
    const void *p = g_out;
    size_t      n = (size_t)g_total;

    if (g_loud_begin >= 0 && g_loud_end > g_loud_begin &&
        g_loud_end <= g_total) {
        /* 两头回吐做留白（保住词首清辅音），再夹回 [0, g_total]。
         * 夹取必须在**扩张之后**做，顺序反了会发出越界长度。 */
        int b = g_loud_begin - VOICE_EDGE_PAD_BYTES;
        int e = g_loud_end   + VOICE_EDGE_PAD_BYTES;

        if (b < 0)
            b = 0;
        if (e > g_total)
            e = g_total;
        if (e > b) {
            p = g_out + b;
            n = (size_t)(e - b);
        }
    }

    if (len)
        *len = n;
    return p;
}
