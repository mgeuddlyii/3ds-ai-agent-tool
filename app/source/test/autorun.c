/*
 * autorun.c -- 见 autorun.h
 *
 * 脚本在 init 阶段一次性解析成指令数组，回放阶段只做数组遍历，
 * 不再碰文件系统 —— SD 卡读写有几十毫秒的抖动，放在帧循环里会让
 * 截图时机不可预测。
 */

#include "autorun.h"
#include "app.h"     /* app_note */

#include "actprobe.h"
#include "amprobe.h"
#include "dbprobe.h"
#include "perffault.h"
#include "cupdtest.h"
#include "featprobe.h"
#include "hwtest.h"
#include "perfprobe.h"
#include "exttest.h"
#include "micprobe.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define AR_MAX_STEPS 96
#define AR_MAX_LINES 128
#define AR_LINE_MAX  96
#define AR_ARG_MAX   32

typedef enum {
    AR_WAIT = 0,
    AR_KEY,
    AR_TOUCH,
    AR_SHOT,
    AR_QUIT,
    AR_AMPROBE,      /* 探 CIA 安装权限（只读 + 立即撤回，不装东西） */
    AR_TITLES,       /* 列出 SD 卡上已装的 title */
    AR_PERFFAULT,    /* 故障注入：故意触发 perf_lvl 的失败路径 */
    AR_DBPROBE,      /* 探豆包通道：校验登录态 + 真发一句话（**要联网**） */
    AR_CUPDTEST,     /* 内容更新通道自测：解析 + 安装/回滚闭环（离线可跑） */
    AR_FEATPROBE,    /* 区域切换 + 字库安全闸验收（会真写一个 locale.txt 再撤销） */
    AR_HWTEST,       /* 硬件知识检索验收（**只读**，不碰任何内容文件） */
    AR_PERFPROBE,    /* 推理耗时测量（只读，但要卡上有模型文件） */
    AR_MICPROBE,     /* 语音输入链路：麦克风 + ASR 配置 + 拼装自检（离线可跑） */
    AR_ACTPROBE,     /* 设置类动作：解析 + 白名单 + 落盘读回（会改 config） */
    AR_XFER,         /* 互传页：清单 + 经典 Win 进度条（要有 PC 上的 HTTP 服务） */
    AR_HOLD,         /* 按住某键 N 帧（PTT 类交互只能这么驱动，见 parse_line） */
    AR_EXTTEST,      /* 抽取式回答验收（只读） */
} ArOp;

typedef struct {
    ArOp op;
    int  n;                  /* AR_WAIT */
    char arg[AR_ARG_MAX];    /* AR_KEY / AR_SHOT */
} ArStep;

static ArStep g_steps[AR_MAX_STEPS];
static int    g_count;
static bool   g_active;

static int  g_pc;            /* 程序计数器 */
static int  g_delay;         /* wait 剩余帧数 */
static int  g_hold;          /* AR_HOLD 剩余帧数（>0 时每帧都把同一批键报给主循环） */
static u32  g_hold_keys;
static char g_err[96];

bool autorun_active(void) { return g_active; }
int  autorun_steps(void)  { return g_count; }
int  autorun_pc(void)     { return g_pc; }
const char *autorun_error(void) { return g_err; }

/* --------------------------------------------------------------------------
 * 解析
 * ----------------------------------------------------------------------- */

static const struct {
    const char *name;
    u32         key;
} KEY_TABLE[] = {
    { "A",      KEY_A      },
    { "B",      KEY_B      },
    { "X",      KEY_X      },
    { "Y",      KEY_Y      },
    { "L",      KEY_L      },
    { "R",      KEY_R      },
    { "START",  KEY_START  },
    { "SELECT", KEY_SELECT },
    { "UP",     KEY_UP     },
    { "DOWN",   KEY_DOWN   },
    { "LEFT",   KEY_LEFT   },
    { "RIGHT",  KEY_RIGHT  },
    { "ZL",     KEY_ZL     },
    { "ZR",     KEY_ZR     },
};
#define KEY_TABLE_N ((int)(sizeof(KEY_TABLE) / sizeof(*KEY_TABLE)))

static u32 lookup_key(const char *name)
{
    for (int i = 0; i < KEY_TABLE_N; ++i)
        if (!strcmp(KEY_TABLE[i].name, name))
            return KEY_TABLE[i].key;
    return 0;
}

static char *trim(char *s)
{
    while (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n')
        ++s;

    size_t n = strlen(s);
    while (n > 0) {
        char c = s[n - 1];
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n')
            s[--n] = '\0';
        else
            break;
    }
    return s;
}

static void push(ArOp op, int n, const char *arg)
{
    if (g_count >= AR_MAX_STEPS) {
        snprintf(g_err, sizeof g_err, "指令超过 %d 条，后面的被忽略", AR_MAX_STEPS);
        return;
    }
    g_steps[g_count].op = op;
    g_steps[g_count].n  = n;
    g_steps[g_count].arg[0] = '\0';
    if (arg)
        snprintf(g_steps[g_count].arg, AR_ARG_MAX, "%s", arg);
    ++g_count;
}

static void parse_line(char *line, int lineno)
{
    char *op, *arg;

    line = trim(line);
    if (*line == '\0' || *line == '#')
        return;

    op = line;
    arg = strpbrk(line, " \t");
    if (arg) {
        *arg++ = '\0';
        arg = trim(arg);
    } else {
        arg = (char *)"";
    }

    if (!strcmp(op, "wait")) {
        int n = atoi(arg);
        if (n < 0) n = 0;
        if (n > 600) n = 600;
        push(AR_WAIT, n, NULL);
        return;
    }

    if (!strcmp(op, "key")) {
        if (!lookup_key(arg)) {
            snprintf(g_err, sizeof g_err, "第 %d 行：不认识的按键 '%s'", lineno, arg);
            return;
        }
        push(AR_KEY, 0, arg);
        return;
    }

    /* hold <KEY> <帧数> —— 按住某键若干帧后再松手。
     *
     * 为什么 `key` 不够用：AR_KEY 只向主循环报**一帧**，而 PTT 类交互
     * （按住 ZL 说话、长按返回）判的是"按住期间"与"松手那一帧的边沿"。
     * 单帧按键在 sc_chat 里只会被读成"按下又立刻松开" —— 200ms 都没到，
     * 直接判成误触。于是 UI 编排那条路（voice_begin -> 采 -> voice_finish
     * -> do_asr）用 `key` **一辈子跑不到**，而"从没被执行过的代码"是最
     * 会埋雷的地方。加这条指令是为了让它能被回放驱动。 */
    if (!strcmp(op, "hold")) {
        char kn[AR_ARG_MAX];
        int  n;

        if (sscanf(arg, "%15s %d", kn, &n) != 2 || !lookup_key(kn)) {
            snprintf(g_err, sizeof g_err,
                     "第 %d 行：%s 不是 hold <按键> <帧数> 或按键不认识",
                     lineno, arg);
            return;
        }
        if (n < 1) n = 1;
        if (n > 600) n = 600;
        push(AR_HOLD, n, kn);
        return;
    }

    if (!strcmp(op, "touch")) {
        int x = 0, y = 0;
        if (sscanf(arg, "%d %d", &x, &y) != 2 ||
            x < 0 || x > 319 || y < 0 || y > 239) {
            snprintf(g_err, sizeof g_err,
                     "第 %d 行：touch 需要 'x y'（0..319 0..239）", lineno);
            return;
        }
        push(AR_TOUCH, 0, arg);
        return;
    }

    if (!strcmp(op, "shot")) {
        if (!*arg) {
            snprintf(g_err, sizeof g_err, "第 %d 行：shot 缺名字", lineno);
            return;
        }
        push(AR_SHOT, 0, arg);
        return;
    }

    if (!strcmp(op, "quit")) {
        push(AR_QUIT, 0, NULL);
        return;
    }

    /* 这两条是**探针**指令，不模拟按键，直接调函数然后把结果写进 status。
     * 放在 autorun 里而不是单独写个程序，是因为"能跑起来 + 能留痕 +
     * 能断言"这一整套机器已经在这儿了，探针只需要借一下。 */
    if (!strcmp(op, "amprobe")) {
        push(AR_AMPROBE, 0, NULL);
        return;
    }

    if (!strcmp(op, "titles")) {
        push(AR_TITLES, 0, NULL);
        return;
    }

    /* 豆包通道探针。**这条要联网**，且依赖卡上有登录态 ——
     * 所以它不进常规回归（离线环境里跑必然红），
     * 只做"人工验豆包"时的入口。 */
    if (!strcmp(op, "dbprobe")) {
        push(AR_DBPROBE, 0, NULL);
        return;
    }

    /* 内容更新通道自测。纯离线、不碰真实内容文件
     * （cupd.c 里用 cu_set_root 换到了 _cupdtest/ 子目录），
     * 所以它**进常规回归** —— cupd 的 bug 只会表现为
     * 「内容更新静默失灵」，模拟器里就能验，不必等真机。 */
    if (!strcmp(op, "xfer")) {
        push(AR_XFER, 0, NULL);
        return;
    }
    if (!strcmp(op, "actprobe")) {
        /* 设置类动作自测。**会改 config.json** —— 所以它不进常规回归，
         * 由调用方（验收脚本）在备份之后触发、跑完还原。
         * 离线可跑：整条链路不碰网络。 */
        push(AR_ACTPROBE, 0, NULL);
        return;
    }
    if (!strcmp(op, "cupdtest")) {
        push(AR_CUPDTEST, 0, NULL);
        return;
    }

    /* 区域切换 + 字库安全闸的验收。**纯离线**（只写 SD 卡上的
     * luma/titles/，写完自己撤销），所以它**进常规回归** ——
     * 这两个功能离线就该跑通，任何一条红了都是真缺陷。 */
    if (!strcmp(op, "featprobe")) {
        push(AR_FEATPROBE, 0, NULL);
        return;
    }

    /* 硬件知识检索验收。**纯只读**（hwk.c 里没有落盘路径），
     * 所以进常规回归 —— 之前 hw_knowledge.txt 装上不生效就是因为
     * 根本没有读者，而"没有读者"这件事在这里才验得出来。 */
    if (!strcmp(op, "hwtest")) {
        push(AR_HWTEST, 0, NULL);
        return;
    }

    /* 推理耗时测量。**不进常规回归** —— 它的输出是时间，
     * 而时间没有固定值可断言（只有"比上次快"这种相对判据，
     * 那要靠人工比对两次数值）。它是"改推理内核之后先跑一次"
     * 的工具，不是回归门禁。 */
    if (!strcmp(op, "perfprobe")) {
        push(AR_PERFPROBE, 0, NULL);
        return;
    }

    if (!strcmp(op, "exttest")) {
        push(AR_EXTTEST, 0, NULL);
        return;
    }

    /* perffault 走 ar[32] 传场景名 —— 它需要一个参数，
     * 而 AR_ARG_MAX 只有 32 字节，场景名最长 8 字节，够用。 */
    if (!strcmp(op, "perffault")) {
        if (!*arg) {
            snprintf(g_err, sizeof g_err, "第 %d 行：perffault 需要场景名", lineno);
            return;
        }
        push(AR_PERFFAULT, 0, arg);
        return;
    }

    /* 语音输入链路验收。**离线可跑**（不联网、不发识别请求），
     * 所以在 Azahar 里验到的是"麦克风不可用时的降级路径"加上
     * "ASR 配置 + 请求体拼装"这两件本地事实 ——
     * 真机上有麦克风时同一条指令会连采样字节数和 RMS 一起报出来。 */
    if (!strcmp(op, "micprobe")) {
        push(AR_MICPROBE, 0, NULL);
        return;
    }

    snprintf(g_err, sizeof g_err, "第 %d 行：不认识的指令 '%s'", lineno, op);
}

bool autorun_init(void)
{
    FILE *f;
    static char buf[AR_MAX_LINES * AR_LINE_MAX];
    size_t n;
    int lineno = 0;
    char *line, *next;

    g_active = false;
    g_count  = 0;
    g_pc     = 0;
    g_delay  = 0;
    /* 上一段脚本若按住过键、又没走完就被换掉，g_hold/hold_keys 会残留：
     * 新脚本第一步就"莫名其妙按着一个键不松"，而屏上看不出来。
     * 换脚本时必须一并清掉。 */
    g_hold      = 0;
    g_hold_keys = 0;
    g_err[0]    = '\0';

    f = fopen(AUTORUN_PATH, "rb");
    if (!f)
        return false;

    n = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    buf[n] = '\0';

    if (n == 0)
        return false;

    for (line = buf; line && *line; line = next) {
        next = strchr(line, '\n');
        if (next)
            *next++ = '\0';
        ++lineno;
        parse_line(line, lineno);
    }

    g_active = (g_count > 0);
    return g_active;
}

/* --------------------------------------------------------------------------
 * 回放
 * ----------------------------------------------------------------------- */

bool autorun_tick(u32 *keys, int *tx, int *ty, const char **shot, bool *quit)
{
    if (!g_active)
        return false;

    *shot = NULL;
    *quit = false;
    *tx   = -1;
    *ty   = -1;

    /* hold 未走完：这一帧**照旧按着**（这一帧的返回值就是"还按着"）。
     * 走完之后 g_hold 变 0，下面自然落到"松手"那一帧 —— 也就是 down=0，
     * 于是 PTT 那条"松手边沿"在回放里真的会发生。 */
    if (g_hold > 0) {
        --g_hold;
        *keys = g_hold_keys;
        return true;
    }

    /* wait 未走完：这一帧什么都不按 */
    if (g_delay > 0) {
        --g_delay;
        return false;
    }

    for (;;) {
        if (g_pc >= g_count) {
            /* 脚本跑完 —— 无人值守场景下必须自己退，否则会挂到超时 */
            *quit = true;
            return false;
        }

        const ArStep *s = &g_steps[g_pc++];

        switch (s->op) {
            case AR_WAIT:
                /* g_delay = n - 1 而不是 n：本次调用就算第 1 帧，
                 * 接口语义是"wait N 一共消耗 N 帧"。 */
                if (s->n > 0) {
                    g_delay = s->n - 1;
                    return false;
                }
                continue;

            case AR_KEY:
                *keys = lookup_key(s->arg);
                return true;

            case AR_HOLD: {
                /* 第一条 hold 帧：按下，同时把剩余帧数立起来。
                 * s 是 const，帧数先抄一份（夹逼放在这里而不是 parse_line，
                 * 因为 parse_line 那侧 push 之前就 clamp 掉更不容易被漏）。 */
                int hold_n = s->n;

                if (hold_n < 1)   hold_n = 1;
                if (hold_n > 600) hold_n = 600;
                g_hold      = hold_n - 1;
                g_hold_keys = lookup_key(s->arg);
                *keys       = g_hold_keys;
                return true;
            }

            case AR_TOUCH: {
                int x = 0, y = 0;
                (void)sscanf(s->arg, "%d %d", &x, &y);
                *keys = KEY_TOUCH;
                *tx   = x;
                *ty   = y;
                return true;
            }

            case AR_SHOT:
                *shot = s->arg;
                return false;

            case AR_QUIT:
                *quit = true;
                return false;

            /* ---- 探针：不模拟按键，只执行一次然后继续往下走 ----
             * 这两个 op 存在的意义是"回答一个有争议的事实问题"，
             * 而不是驱动 UI，所以它们执行完立刻 continue（同帧内），
             * 不消耗帧、也不改变按键状态。 */
            case AR_AMPROBE:
                amprobe_run();
                continue;

            case AR_TITLES:
                amprobe_list_titles();
                continue;

            case AR_PERFFAULT:
                /* 场景名在 arg 里。perffault_run 内部保证自己不会失败 ——
                 * 崩了的话脚本就看不到后面的步骤，这本身就是判据。 */
                perffault_run(s->arg);
                continue;

            case AR_DBPROBE:
                /* 会阻塞在网络请求上（几十秒），但这是脚本里明写的意图，
                 * 不是卡死 —— 断言看的是它写下的结果，不是它多久返回。 */
                dbprobe_run();
                continue;

            case AR_CUPDTEST:
                /* 同 perffault：崩了本身就是判据失败。
                 * 跑完会留下一行 "cupdtest: RESULT PASS/FAIL"。 */
                cupdtest_run();
                continue;

            case AR_FEATPROBE:
                /* 纯本地（写 SD 卡 + 读返回值），不会卡。
                 * 结论一行一行落在 status.txt 里，断言直接读那些行。 */
                featprobe_run();
                continue;

            case AR_HWTEST:
                /* 纯只读：hwk_load 只读文件，hwk_search 只算。
                 * 耗时 25 条 × 40 来个 bigram，微秒级，不会卡。 */
                hwtest_run();
                continue;

            case AR_MICPROBE:
                /* 最多采 0.4 秒再睡过去，不会卡住帧推进（60ms 量级）。 */
                micprobe_run();
                continue;

            case AR_XFER:
                /* 只进屏，不自动开传 —— 传不传由人决定。
                 * 自动化替人按下"开始取"是没有意义的。 */
                app_note("autorun: 已进互传页（不自动开传）");
                continue;

            case AR_ACTPROBE:
                /* 会改 config.json —— 验收脚本负责还原。
                 * 它的价值恰恰在于"真的落盘再读回"：只验返回值的话，
                 * 验到的是"函数说它成功了"，不是"设置真的变了"。 */
                actprobe_run();
                continue;

            case AR_EXTTEST:
                exttest_run();
                continue;

            case AR_PERFPROBE:
                /* 会跑 12 次生成，单次最坏 1.5 秒 → 最坏 18 秒。
                 * 所以这里**不设长超时**：与 perffault 一样，靠
                 * autorun 的帧推进跑完，而不是靠等待。 */
                perfprobe_run();
                continue;

            default:
                continue;
        }
    }
}
