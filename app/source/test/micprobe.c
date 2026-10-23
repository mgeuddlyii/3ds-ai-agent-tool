/*
 * micprobe.c -- 见 micprobe.h
 *
 * ── 这个探针要回答的三件事 ──────────────────────────────────────
 * 1. **麦克风能不能开**（micInit / StartSampling），并且**能不能反复开**。
 *    开一半关掉、再开起不来的话，用户会撞见"第一次能录、第二次就说这台
 *    机器没有可用的麦克风"，而屏上不会说是这个原因 —— 这个探针就是
 *    把它摊开在 status.txt 里的。
 *    采 0.4 秒，报采样数与 RMS（RMS 非零 = 真的采到了声音，不是空跑一圈）。
 * 2. **ASR 配置读进来没有**（provider / model / 有没有 Key）。
 *    这一条离线可验，而且是最容易静默失效的：配置文件里写个
 *    表里没有的 provider 名，屏上只会有"识别失败"四个字。
 * 3. **本地拼装够不够**（WAV 头 + multipart 的缓冲常数）。
 *    asr_payload_probe 不联网也能量，量的是"5 秒的录音装得下吗"。
 *
 * ── 为什么它不联网 ──────────────────────────────────────────────
 * 因为它要进常规回归（像 featprobe / hwtest 那样）。
 * 真发一次识别请求的话，没有 Key / 没有网的环境里必然红，
 * 那这条断言就退化成"网络通不通" —— 与语音功能无关，
 * 而"永远红的断言"和"永远绿的断言"一样没有价值。
 * 真要验端到端识别，走 dbprobe 那条路：人工配好 Key 再跑。
 */

#include "micprobe.h"

#include "app.h"
#include "asr.h"
#include "voice.h"

#include <3ds.h>
#include <stdio.h>
#include <string.h>

#define ASR_CFG_PATH "sdmc:/3ds/aiconsole/asr.txt"

#define PROBE_TICK_MS 10
#define PROBE_TICKS   40      /* 0.4 秒 */

/* 采一轮：init -> start -> pump -> stop -> 报数。
 * 返回采到的字节数（0 = 这一轮麦克风是空的）。 */
static int one_round(int idx)
{
    int n;

    if (!voice_init()) {
        app_note("micprobe: 第%d轮 mic 开不了 rc=%d", idx, voice_mic_rc());
        return -1;
    }
    if (!voice_available()) {
        app_note("micprobe: 第%d轮 voice_available=0 rc=%d",
                 idx, voice_mic_rc());
        return -1;
    }
    if (!voice_start()) {
        app_note("micprobe: 第%d轮 采样起不来 rc=%d", idx, voice_mic_rc());
        return -1;
    }

    for (int i = 0; i < PROBE_TICKS; ++i) {
        voice_pump();
        svcSleepThread((u64)PROBE_TICK_MS * 1000 * 1000);   /* 单位纳秒 */
    }
    voice_stop();

    n = voice_bytes();
    app_note("micprobe: 第%d轮 采到 %d 字节 = %d 毫秒，RMS=%d",
             idx, n, voice_ms(), voice_rms());
    return n;
}

void micprobe_run(void)
{
    const AsrDef *d;
    size_t        body_len = 0;
    int           pass     = 0;
    int           fail     = 0;
    bool          ok;
    int           n1;

    app_note("micprobe: ===== 语音输入链路 =====");

    /* ---- 1. 配置（离线、最容易静默失效的一段）---- */
    asr_load_file(ASR_CFG_PATH);
    d = asr_current();
    app_note("micprobe: ASR provider=%s model=%s lang=%s key=%d",
             d->name, d->model, d->lang ? d->lang : "-",
             asr_key()[0] ? 1 : 0);
    if (d && d->url && d->url[0])
        ++pass;
    else
        ++fail;

    /* ---- 1b. **没有 asr.txt** 时的回落（2026-10-18 补的判据）----
     * 用户实测「语音模块显示 Key 还没进去」的根因：asr_reset 原先放在
     * fopen 成功之后，于是卡上没有 asr.txt 时直接 return，内置 Key
     * 永远填不进去 —— 而"别人装上就能用"恰恰建立在没有 asr.txt 的
     * 机器上。这里故意读一个**铁定不存在**的路径，把那条路跑出来：
     *   · 修好了 -> provider=默认（siliconflow）key=1（内置 Key）
     *   · 退化了   -> key=0（回到"没配 Key"的老 bug）
     * 这条路在真机/模拟器上都不需要动卡上的文件，纯进程内复现。 */
    asr_load_file("sdmc:/3ds/aiconsole/__nofile_probe__.txt");
    d = asr_current();
    app_note("micprobe: ASR nofile provider=%s key=%d",
             d->name, asr_key()[0] ? 1 : 0);
    if (asr_key()[0])
        ++pass;
    else
        ++fail;
    /* 恢复成卡上那份的语义，别让 1b 的副作用影响后面的判据 */
    asr_load_file(ASR_CFG_PATH);

    /* body 拼装：满 5 秒的录音也得装得下 */
    ok = asr_payload_probe(VOICE_CAP_BYTES, &body_len);
    app_note("micprobe: payload body(5s)=%u 装得下=%d",
             (unsigned)body_len, (int)ok);
    if (ok)
        ++pass;
    else
        ++fail;

    /* ---- 2. 麦克风：连着采两轮 ---- */
    /* 分两轮不是为了好看，是为了把"关掉再开"这条路径也跑一遍。
     * 第一轮换来 micExit，第二轮换来 micInit —— 真机上第二次起不来
     * 的话，屏上只会说"这台机器没有可用的麦克风"，Nobody 知道
     * 是硬件没开还是被上一次录音关掉了。 */
    n1 = one_round(1);
    voice_fini();                       /* 关掉，交给第二轮去重开 */

    int n2 = one_round(2);
    voice_fini();

    if (n1 > 0)
        ++pass;
    else
        ++fail;   /* 采不到 = 麦克风通道是空的（硬件/权限问题） */

    if (n2 < 0) {
        /* 第二轮起不来 = mic 关不掉重开 —— 这条比"采不到"更具体 */
        app_note("micprobe: RESULT=FAIL mic 关掉后开不了（录第二次会没声音）");
        ++fail;
    } else if (n2 > 0) {
        ++pass;
    } else {
        app_note("micprobe: RESULT=FAIL 第二轮采不到（第一轮 %d 字节）", n1);
        ++fail;
    }

    app_note("micprobe: RESULT=%s pass=%d fail=%d",
             fail == 0 ? "PASS" : "FAIL", pass, fail);
    app_note("micprobe: ===== 结束 =====");
}
