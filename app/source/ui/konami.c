/*
 * konami.c -- 见 konami.h
 *
 * ══════════════════════════════════════════════════════════════
 *  这个模块保护的是**安全等级**，也就是"AI 能动到哪一层"。
 *  它不是防窥屏、不是防误触，是防"AI 自己在对话里把权限提上去"。
 *
 *  为什么必须有：
 *   sec.level 动作本身要过确认屏，看起来够安全。但那一屏上写着
 *   "调高会放宽限制"，用户是**看不懂 L3 和 L5 差在哪**的 ——
 *   于是"AI 建议提升到 L5"就变成了一句看起来无害的建议。
 *   把提升本身也锁进密码后面，才真正切断这条链。
 *
 *  为什么密码是 KONAMI 码：
 *   它足够长（10 键）、没有记忆负担、而且是公开的 ——
 *   所以它的价值**不在保密**，而在"必须是有意为之的那一串输入"。
 *   随手连按方向键不会碰巧对上，而想进来的人一定知道自己在输什么。
 *   这一点必须写清楚：把 KONAMI 码说成"加密"是自欺欺人。
 *
 *  ★ 密码**不落盘**。落盘等于把明文密码留在 SD 卡上，
 *    读一份 config.json 就知道了。通过状态只存在内存里，
 *    关机就没了 —— 这正是我们要的。
 * ══════════════════════════════════════════════════════════════
 */

#include "konami.h"

#include "app.h"

#include <3ds.h>
#include "uistr.h"

/* KONAMI 经典码：↑ ↑ ↓ ↓ ← → ← → B A
 *
 * ★ 表里存的是**各键的低 8 位**，不是按键常量本身。
 *
 * 3DS 的方向键是"通用方向"别名：KEY_UP = KEY_DUP | KEY_CPAD_UP
 * = BIT(6) | BIT(30) = 0x40000040；而 KEY_DOWN 还带 BIT(31)
 * = 0x80000080 —— **在 int 下是负数**。
 * 早先把按键常量直接存进 u8 表，编译器报一串 -Woverflow
 * （"changes value from 1073741888 to 64"），而它"碰巧对"是因为
 * 比对时也取低 8 位。
 *
 * **"碰巧对"就是隐患**：哪天比对那侧改成用全值，这里就静默失配。
 * 所以表里直接存最终要比的东西，让两边一眼就是同一套口径。
 *
 * 顺带这也让 A/B 与方向键的处理天然一致（它们本来就 < 256）。 */
#define K_LOW(k) ((unsigned char)((k) & 0xFFu))

static const u8 KONAMI[] = {
    K_LOW(KEY_UP),   K_LOW(KEY_UP),   K_LOW(KEY_DOWN), K_LOW(KEY_DOWN),
    K_LOW(KEY_LEFT), K_LOW(KEY_RIGHT), K_LOW(KEY_LEFT), K_LOW(KEY_RIGHT),
    K_LOW(KEY_B),    K_LOW(KEY_A)
};
#define KONAMI_N ((int)(sizeof KONAMI / sizeof KONAMI[0]))

/* 连点六下的时间窗。
 *
 * ★ 2026-10-16 放宽（原 1200ms 总窗 / 1500ms 间隔）：真机上**连点六下
 *   根本凑不满** —— 六次按键要挤在 1.2 秒里，等于每 200ms 一下，
 *   正常人手速做不到，于是"点了没反应"。现在改成：
 *   相邻两下 ≤900ms、整串 ≤4000ms —— 这才是"连着点"的速度。
 *
 *   放宽会不会误触发？配合 sc_settings 那条"只有光标停在版本号那一行
 *   时 A 才算连点"，误触面的来源就只剩"在版本号上连按 A"这一种，
 *   而那本来就是要触发它的动作。 */
#define TAP_WINDOW_MS 4000    /* 从第一下算起的总时长 */
#define TAP_COUNT     6
#define TAP_WINDOW     900    /* 两次点击之间的最大间隔 */

static int  s_taps;           /* 连点计数 */
static u64 s_last_tick;       /* 上一次点击的时刻 */
static u64 s_open_tick;       /* 计数从哪一刻开始算 */

static int  s_pos;            /* 密码进度 */
static bool s_prompt;         /* 已连点六下，正在显示提示 */
static bool s_accepting;      /* 正在接收密码（连点之后为真，直到解锁或取消） */
static bool s_unlocked;       /* 本次开机已通过 */

static u64 now_ms(void)
{
    /* osGetTime 是毫秒。
     * 刻意**不用** svcGetSystemTick：那是 268 MHz 而��是 60 Hz，
     * 早先有代码按 60 算过，差 440 万倍。 */
    return osGetTime();
}

/* 密码门是否正在接管键盘。见 konami.h —— 屏幕层的总闸读的就是它。 */
static bool s_capturing(void) { return s_prompt || s_accepting; }

bool konami_capturing(void) { return s_capturing(); }

void konami_cancel(void)
{
    if (!s_capturing())
        return;                  /* 本来就没在输，别刷一行没意义的日志 */

    s_prompt    = false;
    s_taps      = 0;
    s_pos       = 0;
    s_accepting = false;
    app_status_set(S(S0041));
    app_note("secgate: 用户取消（R / 取消按钮）");
}

void konami_feed(u32 key)
{
    u64 now;

    /* 判据必须是 `== 0` 而不是 `<= 0`：方向键里 KEY_DOWN 的 CPAD 位是
     * BIT(31)，在 int 下是负数 —— 用 `<= 0` 会把两个下键静默丢掉。 */
    if (key == 0)
        return;

    /* START = 取消。
     *
     * ⚠️ 这条路在**真机上走不到**：main.c 在 app_handle 之前就把
     *    START 当全局退出键吃了（`if (down & KEY_START) break;`）。
     *    保留它是因为 actprobe 的用例 3 直接喂 konami_feed(KEY_START)
     *    验"输一半能取消"，而且万一将来主循环改了语义，这里仍然自洽。
     *    真机上用户能按到的取消键是 **R**（见 sc_settings）。 */
    if (key == KEY_START) {
        konami_cancel();
        return;
    }

    /* 提示态：**关掉提示，但不把按键吞掉** —— 它就是密码的第一个键。
     *
     * ★ 这里原先写的是"任意键都关掉它，然后 return"，于是
     *   密码的**第一个 ↑** 被当成取消给吞了，后面的键全部错位 ——
     *   输对了也过不了。2026-10-06 00:0x 实测抓到：
     *   70 个用例里 69 过，挂的正是这一条（"KONAMI 码 FAIL 输对了却没过"）。
     *
     * 顺带一说，这不是"探针喂键的姿势不对"：真机上是同一个毛病 ——
     * 用户连点六下看到提示，按下第一个方向键，提示消失、密码从头错位，
     * 于是"明明输对了却不通过"。判据在这里的价值又一次体现：
     * 探针按真实顺序喂键，就把真实流程里的洞照出来了。 */
    if (s_prompt) {
        s_prompt = false;
        s_taps = 0;
        s_pos = 0;
        s_accepting = true;       /* 从这一刻起才收密码 */
        app_status_set("");
        /* ★ 故意**不 return**：继续往下走密码比对 */
    }

    /* 已经通过的人不该再被提示打断 —— 密码门是用来拦"第一次进入"的，
     * 不是用来每次调设置都问一遍。 */
    if (s_unlocked)
        return;

    now = now_ms();

    /* ---- 连点计数 ----
     * 条件：同一个键、在时间窗内、已满 6 次。
     * "同一个键"是刻意的：连点不同键凑满 6 次很容易被正常操作误触发
     * （比如方向键 + A 交替按），那会让提示莫名出现。
     *
     * ★ `s_pos == 0` 这个条件是必需的，不能省：
     *   KONAMI 码的**最后两个键就是 B 和 A**。而 A 同时是连点用的键，
     *   如果不分情况，A 永远被连点逻辑吃掉，密码就永远输不完 ——
     *   写这一版时我第一遍就踩了这个坑。 */
    if (key == KEY_A && s_pos == 0) {
        if (s_taps == 0) {
            s_taps = 1;
            s_open_tick = now;
        } else if (s_taps > 0 && (now - s_last_tick) <= TAP_WINDOW &&
                   (now - s_open_tick) <= TAP_WINDOW_MS) {
            s_taps++;
        } else {
            s_taps = 1;
            s_open_tick = now;
        }
        s_last_tick = now;

        if (s_taps >= TAP_COUNT) {
            s_taps = 0;
            s_prompt = true;
            s_pos = 0;
            app_status_set(S(S0042));
            app_note("secgate: 提示已出现（连点 %d 下）", TAP_COUNT);
        }
        return;                     /* 无密码进度时的 A = 连点；有进度时下面才当密码 */
    }

    /* ★ 关键闸门：**不在"正在接收密码"状态时，按键一律不参与比对。**
     *
     * 少了这一条，任何时候把 10 个键按对都能解锁 —— 连点六下就只是个
     * 装饰。更糟的是设置屏的**方向键要用来选行、B 要用来返回**，
     * 它们也被拿去比密码，于是用户正常操作就可能撞上密码序列。
     *
     * 判据只认 s_accepting，不认"当前 s_pos > 0" ——
     * 输错之后 s_pos 会归零，而用户应该能**继续输**，
     * 所以需要一个跨帧的"正在接收"状态，而不是从进度推断。
     *
     * ⚠️ 这段必须在上面那个 `if (key == KEY_A …) { … }` **块外**。
     *   它一度被插在块内、`return;` 之后 —— 那是**死代码**，
     *   而括号仍然配平、编译仍然通过、闸门等于没写。
     *   "插错位置但编译绿灯"是最贵的一类错：它看起来做完了。
     */
    if (!s_accepting)
        return;

    /* ---- 密码比对 ----
     * 允许中途输错后自动重置：全错时从头再来，
     * 而不是把进度卡在某个永远对不上的位置。 */
    {
        unsigned k = key & 0xFFu;
        if (k == (unsigned)KONAMI[s_pos]) {
            s_pos++;
            app_note("secgate: key %02x ok, progress=%d/%d",
                     k, s_pos, KONAMI_N);
            if (s_pos >= KONAMI_N) {
                s_unlocked = true;
                s_accepting = false;
                s_pos = 0;
                s_prompt = false;
                app_note("secgate: 密码通过，本次开机内有效");
            }
        } else {
            /* 打印**期望值与实际值**：错位到第几个键、期望什么、来了什么，
             * 三个数在一起就足以判断是常量错了还是掩码错了。 */
            app_note("secgate: mismatch at %d, want %02x got %02x",
                     s_pos, (unsigned)KONAMI[s_pos], (unsigned)k);
            s_pos = 0;      /* 保持 s_accepting —— 用户可以接着输 */
        }
    }
}

bool konami_prompt_active(void) { return s_prompt; }

int konami_progress(void) { return s_pos; }
int konami_len(void)      { return KONAMI_N; }
bool konami_unlocked(void)     { return s_unlocked; }

void konami_dismiss_prompt(void)
{
    s_prompt = false;
    s_taps = 0;
    s_pos = 0;
}

void konami_reset(void)
{
    /* 注意：**不清 s_unlocked**。
     * 它表示"本次开机已通过"，离开设置屏再回来不该重新输 ——
     * 而"关机之后要重输"由它不落盘自然保证。 */
    s_prompt = false;
    s_taps = 0;
    s_pos = 0;
    s_accepting = false;   /* 离开屏就不该还在收密码 */
}

void konami_test_reset_all(void)
{
    konami_reset();
    s_unlocked = false;      /* 只有测试会走到这里 */
}
