/*
 * sc_chat.c -- 对话屏
 *
 * 双屏分工（§4.7）：
 *   上屏 = AI 说的话 + 你说的话，整段往下读（事实区）
 *   下屏 = 你要说的话：当前输入、发送、调键盘（操作区）
 *
 * 输入**用系统自带的软键盘（swkbd）**，不自己画：系统键盘自带中文输入法、
 * 预测和符号，自己做的最好结果也只是"接近"，还多出一堆布局和输入法的坑。
 * 想加的功能（快捷短语、语音）放在键盘外面那层 UI 上一样能用。
 *
 * 关于卡界面：网络请求是阻塞的，直接在按 A 的那一帧发请求，
 * 用户会看到屏幕冻住好几秒而不知道发生了什么。所以拆成两帧 ——
 * 第一帧先把「正在问…」画出来，第二帧才真的发请求。
 *
 * ── M6 流式上屏：为什么要在回调里自己出帧 ──────────────────────────
 * main.c 的帧循环是 handle() → render()，中间**没有插入点**；
 * 而流式读取是一个跑在 handle() 里的阻塞循环，几秒钟不返回。
 * 如果什么都不做，用户看到的就是"按了发送 → 冻住几秒 → 整段回复啪地出现"，
 * 流式白做了。
 *
 * 所以让流式回调**直接把当前消息刷到屏幕上**（r_target / r_begin /
 * render_top / r_end）。这不会和主循环打架：请求期间整个 app 就卡在这个
 * 回调里，主循环的 render 还没轮到；回调出完帧再回到读取循环，
 * 主循环最后再画一次同样的内容 —— 幂等，不影响任何状态。
 *
 * 代价是每次出帧都有 SD 图层的开销（我们是 CPU 光栅化 + 整屏清屏），
 * 所以用**节流**：每 60ms 或累计 24 个 token 才刷一次。
 * 3DS 上"每秒刷十几次"人眼就已经是连续的了，刷太勤只会掉帧。
 */

#include "screen.h"

#include "actions.h"
#include "agentdoc.h"
#include "app.h"
#include "asr.h"
#include "autorun.h"
#include "bootlog.h"
#include "chatapi.h"
#include "chatlog.h"
#include "codebox.h"
#include "dock_ui.h"
#include "credfile.h"
#include "keystore.h"
#include "phrases.h"
#include "projects.h"        /* projects_active/count —— 按任务分历史（proj_slot） */
#include "provider.h"
#include "doubao.h"
#include "render.h"
#include "shot.h"
#include "voice.h"
#include "widgets.h"
#include "ime.h"   /* B 键统一处理要读 ime_input() / ime_reset() */

#include <3ds.h>
#include <math.h>            /* ctx_ring_draw 每环一次 cosf/sinf */
#include <stdio.h>
#include <string.h>
#include "uistr.h"

/* 内存里保留多少条。★ 从 24 提到 48：
 * 24 条大概只够 12 轮，用户一开新话题就把前面的挤掉了，而**磁盘上其实还在** ——
 * 屏幕上却看不到，等于白存。每条最大约 2KB（正文 1KB + 思考 1KB），
 * 48 条约 96KB，对 64MB 的机器不算负担。 */
#define CHAT_MAX   48
#define CHAT_TEXT  150      /* 用户一条输入的上限（软键盘里也就打这么多） */
#define CHAT_AITXT 1024     /* AI 回复的显示上限。2KB 全收会一条占满整个聊天记录 */

/* 每次请求最多带几条上下文。这是**界面侧**的意愿上限（"我最多愿意给这么多"），
 * chatapi 那边还有一道独立的 4KB 预算兜底（"装得下才算数"）。
 * 两道上界各自成立：这里管的是"给多了没意义且烧钱"，那边管的是"给多了放不下"。
 * 8 条大约覆盖最近三四轮 —— 长回复会被切成多块，所以一条 ≠ 一轮。 */
#define CHAT_CTX_TURNS 8

/* ── 每条消息是什么"类型" ──────────────────────────────────────────
 *
 * 为什么要显式分类型（而不是靠 `mine` 一个 bool 撑着）：
 * 屏幕上一排消息里，用户要能一眼分出"这句是我说的 / AI 答的 /
 * 它想了个动作 / 动作后来怎么样了"。只靠 mine 的话，动作结果和
 * 系统提示都只能画成"我"或"AI" —— 那是在**说谎**，而且用户很快就会发现。
 *
 * ★ 徽章必须与"这条进不进上下文"解耦：MSG_ACTION 不进上下文
 *   （skip=true），但它在屏幕上要有自己的样子。 */
typedef enum {
    MSG_ME = 0,     /* 用户说的 */
    MSG_AI,         /* AI 的回复（正文） */
    MSG_ACTION,     /* 动作的执行/拒绝结果（界面自己记的一条，不进上下文） */
    MSG_SYS,        /* 开场引导语、发送失败提示这类（同样不进上下文） */
} MsgKind;

typedef struct {
    MsgKind kind;
    /* 这条算不算"真的说过的话"。占位文案（正在问…）、失败提示、
     * 以及开场引导语都要置 true —— 它们是我方界面写上去的，
     * 不是对话内容。混进上下文会让模型以为自己说过那些话，
     * 或者把「抱歉，发送失败」当成人设的一部分照抄给下一轮。 */
    bool skip;
    char text[CHAT_AITXT];
    /* 思考内容。**空串 = 这条没有思考**（关掉档位、或那家不返回）——
     * 那是正常情况，不是错误，界面上就是"没有那一行"。
     *
     * 展示时是**贴在正文上面的一块**，可以收起/展开，不是正文的一部分：
     * 混进正文会让用户以为 AI 在回答里自言自语。 */
    char think[CHAT_THINK_MAX];
} Msg;

static Msg  g_msgs[CHAT_MAX];
static int  g_count;

/* 用户那条：kind == MSG_ME。用函数而不是再存一个 bool ——
 * 两个字段表示同一件事，迟早会出现"kind 改了、mine 忘了改"的那一天。 */
static bool msg_mine(int i) { return g_msgs[i].kind == MSG_ME; }

/* 每条消息的折行数缓存（0 = 还没算过）。
 *
 * 为什么必须有：msg_lines() 算一次要 snprintf + text_line_count()（要按
 * UTF-8 逐字算显示宽度），而 reanchor() 会对**当前所有**消息各算一次。
 * 流式输出时每个增量都调 reanchor（见 live_delta），
 * 16 条消息 = 每帧 16 轮重算 —— 实测把对话屏压到 **6.97 fps**，
 * 而同机其它屏约 10.7（其它屏没有这份开销）。
 *
 * 规矩：**谁改 g_msgs[i].text 谁清 g_lines[i]**，改完下次访问自动重算。
 * 这条规矩漏了会表现为"滚动位置不对/底部对不齐"，而且只在长消息上偶发。 */
static short g_lines[CHAT_MAX];

/* 文本变了 → 缓存作废。只有这一个入口，不许在别处直接写 g_lines。 */
static void text_changed(int i)
{
    if (i >= 0 && i < CHAT_MAX)
        g_lines[i] = 0;
}
static int  g_scroll;

/* ★ 摇杆滚动（2026-10-23）：连续量 + 余数。
 *
 *   摇杆是**模拟量**，十字键是**开关量**，两者不能共用同一份计数：
 *   开关量每次按下走一格，模拟量则在按住期间每帧都要按"推了多少"累积。
 *   所以这里加两个状态：
 *     g_stick_acc  推杆累积量，累到 STICK_STEP 才走一格（推得深累得快，
 *                  轻推慢、推到底快 —— 手感靠这个，不是靠加速度）
 *     g_stick_was  上一帧有没有推到死区外。**松手要清累积**：不清的话，
 *                  轻轻推一下攒了半格，松手后再推一下就会"凭空"多跳一格，
 *                  用户会觉得滚动在乱跑。
 *   方向：上推（dy 为负）= 往回看更早的消息（g_scroll 减）。
 */
#define STICK_DEAD   40      /* 死区：回中抖动约 ±5，40 大约"推到 1/4 才算动" */
#define STICK_ACC    10      /* 每帧最小累积量（死区边缘时的速度） */
#define STICK_STEP   48      /* 累积到这个数走一格（死区边缘约 5 帧一格） */

static int  g_stick_acc;
static bool g_stick_was;

/* 下屏滑轨当前格：0 = 聊天，1..4 = 键盘/项目/文件/浏览。
 * 声明放这么靠前，是因为 chat_enter() 要用它，而它在文件前半部分。 */
static int g_tab;
static char g_input[CHAT_TEXT];

static char g_key[CREDFILE_KEY_MAX + 1];

/* 底屏按钮矩形：render 与 handle（触摸命中）共用同一份，防止两处漂移 */
#define CHAT_BTN_W 144.0f
#define CHAT_BTN_Y 124.0f
#define CHAT_BTN_H 36.0f
static const Rect CHAT_BTN_SEND = { W_BOT_MARGIN, CHAT_BTN_Y, CHAT_BTN_W, CHAT_BTN_H, 0 };
static const Rect CHAT_BTN_KBD  = { W_BOT_MARGIN + CHAT_BTN_W + 12.0f,
                                    CHAT_BTN_Y, CHAT_BTN_W, CHAT_BTN_H, 0 };

/* ── 语音按钮 ────────────────────────────────────────────────────
 * 刻意**不挤进主按钮行**：那一行 144×2 + 12 = 300，正好是 W_BOT_USABLE，
 * 再塞第三个必然超宽 —— 版面护栏（w_overflow）会记一笔，
 * 而真正的问题是"发送/键盘"这两个主操作被一起挤小。
 *
 * 所以放在底屏页眉右侧那块本来就空着的 24px 里。
 * 页眉只有左边一个标题，右边从来没画过东西 —— 这是唯一不碰
 * 主操作布局的位置，也顺手避开了"三个按钮看起来同级"的问题。
 *
 * ★ 宽 70 不是拍的，是按**最长标签**量的（2026-10-18 用 font.bin 实测，
 *   见 tools/_probe_adv.py 的口径）：
 *     无麦 32 / No mic 44 / 识别中 48 / Listening 57 / 语音 ZL 50 /
 *     Voice ZL 52 / 录 12s 33 / Rec 12s 52
 *   最长 57，加护栏余量 6 → 63；给到 70 是连"以后再长一点的译文"一起留。
 *   ⚠️ 旧宽 58（可用 52）本来就是**超宽**的：英文 Listening=57、录音时
 *   "录 12.3s"=69 都会越过按钮框 —— 只是没有一趟验收在录音态渲染对话屏，
 *   所以这个溢出一直没被护栏抓到。真机上手指按着 ZL 说话时，那个标签
 *   会画出按钮、压到页眉上去。 */
#define CHAT_BTN_MIC_W 70.0f
#define CHAT_BTN_MIC_H 19.0f
static const Rect CHAT_BTN_MIC = { R_BOT_W - W_BOT_MARGIN - CHAT_BTN_MIC_W,
                                   3.0f,
                                   CHAT_BTN_MIC_W, CHAT_BTN_MIC_H, 0 };

/* ── 「思考」按钮 ──────────────────────────────────────────────────
 * 与语音按钮同一行（页眉右侧那块 24px）：主按钮行已经 144×2=300 占满，
 * 再塞必然超宽。位置放在语音按钮**左边**，中间留出标题的地盘。
 *
 * ★ 只在真的有思考内容时才画 —— 没有内容时它就是一个"点了没反应"的键，
 *   那正是这个项目一直在躲的东西。
 *
 * ★ 箭头 ▼/▶ 而不是 ▾/▸：后两个码位（U+25BE/U+25B8）**不在字库里**，
 *   画出来是豆腐块 —— 旧版这里一直是个空心方框，还占了全宽。
 *   ▼/▶ 在库（advance=16）。实测 "思考▼"=48、"Think▼"=52，
 *   按钮可用 62-6=56，两种语言都过得去（英文余量 4px，见下方排布注释）。 */
#define CHAT_BTN_THINK_W 62.0f
#define CHAT_BTN_THINK_H 19.0f
static const Rect CHAT_BTN_THINK = { 170.0f, 3.0f,
                                     CHAT_BTN_THINK_W, CHAT_BTN_THINK_H, 0 };

/* ── 「思考强度」按钮（从设置屏搬过来的那个旋钮）───────────────────
 * 放在页眉标题右边：标题改成短名（S0156「对话」）之后，x10..42 是空的，
 * 而 THINK/MIC 两个钮仍停在原处（170..232 / 240..310）不动。
 *
 * ★ 宽 116 不是拍的：英文 "Think·Medium" 实测 102px（ proportional 字体：
 *   i=4 / m=12 / 空格只有 3，不是早年估的 104），w_button 的溢出判据是
 *   b.w-6 = 110。102 ≤ 110 留 8px 余量 —— 译文再加长一两个字母也稳。
 *
 * ★ 服务商不支持时**整个按钮不画**（判据是 provider_think_capable() ——
 *   它同时覆盖 NONE（明确不可调）与 **UNKNOWN（未查证）**。
 *   2026-10-25 之前这里判的是 `p->think == PROVIDER_THINK_NONE`，
 *   加了 UNKNOWN 那一档之后，这种写法会让"未查证"的家**莫名其妙多出一个按钮**，
 *   点下去却什么都不发 —— 所以两处判据统一改成能力函数。
 *   离线演示与豆包都没设这个字段，天然落进这一支）。
 *   这与设置屏那条"不锁、只在状态行说明"是**两个不同的取舍**，理由是
 *   场景不同：设置行是常驻的，锁死档位会让用户切走服务商后改不回来；
 *   而对话页这个钮贴在"这一次对话"上，那家没有思考旋钮时画一个
 *   "点了只改配置、对当下毫无效果"的键，比不画更糟。
 *   档位本身仍然存在 config 里，切到支持的服务商就会出现 —— 不锁值。 */
#define CHAT_BTN_TLVL_W 116.0f
#define CHAT_BTN_TLVL_H 19.0f
static const Rect CHAT_BTN_TLVL = { 48.0f, 3.0f,
                                    CHAT_BTN_TLVL_W, CHAT_BTN_TLVL_H, 0 };

/* ── 上下文用量环 ─────────────────────────────────────────────────
 * 画在底屏内容区右上角：输入框右端（x278）之外、页眉（y24）之下。
 * 外 13 内 9 —— 环带 4px，在 320×240 上是"看得见又不抢戏"的尺寸；
 * 再细（2~3px）真机上会被行里的字吃掉。
 *
 * 点击弹窗。命中区比视觉大 4px（30×30）—— 手指点 26px 的目标太苛，
 * 而旁边就是输入框右端，误触的代价是"弹了个窗"，可接受。
 *
 * ★ 只在**真的会走 chatapi 请求体**的时候画：离线演示和豆包都不经过
 *   build_request（一个本地剧本、一个自己的 samantha 接口），
 *   chat_ctx_estimate / chat_last_request_len 对它们没有意义。
 *   画一个环告诉演示用户"你用了 0 字节上下文"是撒谎。 */
#define CTX_RING_CX   296.0f
#define CTX_RING_CY    66.0f
#define CTX_RING_R_OUT 13.0f
#define CTX_RING_R_IN   9.0f
static const Rect CTX_RING_HIT = { CTX_RING_CX - 15.0f, CTX_RING_CY - 15.0f,
                                   30.0f, 30.0f, 0 };

/* 输入区右端。环占着 x281..311，所以折行宽从 300 收到 268
 * （x10..278）。少 32px ≈ 2 个全角字，换来环不压字。 */
#define CHAT_INPUT_W 268.0f

/* ── 上下文弹窗（模态）───────────────────────────────────────────
 * 开着的时候键位全归弹窗（chat_handle 里吞），关闭只有两条路：
 * 点「返回」钮或按 B。形状照 sc_settings.c 的密码门弹窗 ——
 * 320×240 上"留边的对话框"会露出被盖住内容的半列字，
 * 整块盖住内容区才干净（2026-10-16 定下的规矩）。 */
static bool g_ctx_open;
static const Rect CTX_POP_BTN = { 95.0f, 150.0f, 130.0f, 34.0f, 0 };

static char g_provider[24];   /* key.txt 里指定的服务商；空 = 用配置/默认 */
static char g_model[48];      /* 同理，指定模型；空 = 用那家的默认 */
static bool g_have_key;

/* 动作结果缓冲在 app.c（app_act_result_store / _get / _clear）——
 * 写入方是确认屏、读取方是对话屏，分处两个文件，中间只走那一个入口，
 * "读一次就清掉"这个约定才不会漏。
 * 它解决的是：动作机制原本单向（模型 → 动作 → 显示给用户），
 * 模型拿不到自己动作的结果，于是只读类动作没有意义。 */

/* 豆包服务端给的会话 id；空串 = 下一次让服务端新建。
 *
 * 为什么必须持久化到文件：它和对话正文是**一对**，但存在两个地方
 * （正文在 chat.json，这里在 chat_conv.txt）。如果 id 丢了而正文还在，
 * 重开机后看起来"接着聊"，豆包那边其实已经开新会话了 —— 这正是
 * 修复前那个症状的来源。所以 save_history 保存正文时必须一起存它。 */
static char g_conv_id[32];

#define CONV_PATH "sdmc:/3ds/aiconsole/chat_conv.txt"

/* ---- 按任务分开存（2026-10-24 用户要求）----
 *
 * 用户原话：「我现在选择新项目后，上屏的页面没有变化，没有进入新项目的空间。
 * 同理，当我打开新项目的时候，也要看到那个项目之前做的内容。」
 *
 * 根因：切换任务只改 provider/model，而**对话历史一直是一份全局 chat.json**，
 * 会话 id 也只有一个 chat_conv.txt。于是换项目之后上屏毫无变化，
 * 两个项目的对话混在同一条时间线上。
 *
 * 判据：只按 `projects_active()` 下标分文件（见 chatlog.c 里为什么用序号
 * 不用名字）。
 *
 * ★ 为什么**不**做"单任务时用老路径 chat.json"的兼容（2026-10-24 改口）：
 *   我第一版就是这么写的，想着老用户只有一个任务，直接沿用老文件最省事。
 *   但它有个必然踩到的坑 —— 用户从两个任务删回到一个时，槽突然从 p1
 *   变回"老路径"，那个任务的历史**当场消失**（chat_p0.json 还在，只是没人读了）。
 *   "删掉一个项目，剩下的项目对话全没了"这种 bug 比升级兼容难解释得多。
 *
 *   改成**一律按任务号**取槽，老数据由 chatlog.c 的 migrate_default_to_p0()
 *   在第一次用到 p0 时搬过去 —— 兼容只在一个地方做，不散在判据里。
 */
static void proj_slot(char *out, size_t n)
{
    snprintf(out, n, "p%d", projects_active());
}

/* 会话 id 的文件路径也跟着任务走 —— 正文分开了 id 却共用一份，
 * 就会出现"这个项目的对话接着上一个项目的服务端会话"，
 * 而豆包那边直接答非所问（它记得的是另一个项目的事）。 */
static void conv_path(char *out, size_t n)
{
    char slot[16];

    proj_slot(slot, sizeof slot);
    if (!slot[0])
        snprintf(out, n, "%s", CONV_PATH);   /* 兜底：不该发生 */
    else
        snprintf(out, n, "sdmc:/3ds/aiconsole/chat_conv_%s.txt", slot);
}

/* 切任务后让上屏立刻换成新任务的历史。
 *
 * 由 sc_dock.c 的 apply_project() 调用（它切完 provider/model 之后）。
 * 不在这里判"现在是不是对话屏"：调用方在切任务时一定在对话屏的项目格里，
 * 而"不在对话屏"时做这件事也无害（只是把消息表重建了一遍）。
 *
 * ⚠️ 必须连 g_conv_id 一起重读：只换正文不换 id，续聊会接到上一个任务的
 *   服务端会话上去，症状是"新任务里 AI 莫名其妙记得旧任务的事"。 */
static void save_conv_id(void)
{
    FILE *f;

    if (!g_conv_id[0])
        return;                 /* 还没会话 id，别把空文件留在卡上 */
    char path[96];
    conv_path(path, sizeof path);
    f = fopen(path, "wb");
    if (!f)
        return;
    fprintf(f, "%s\n", g_conv_id);
    fclose(f);
}

static void load_conv_id(void)
{
    char path[96];
    conv_path(path, sizeof path);
    FILE *f = fopen(path, "rb");

    g_conv_id[0] = '\0';
    if (!f)
        return;
    if (!fgets(g_conv_id, sizeof g_conv_id, f))
        g_conv_id[0] = '\0';
    fclose(f);
    /* fgets 会连换行一起读进来；下面统一截断到首个空白 */
    for (int i = 0; g_conv_id[i]; ++i) {
        if (g_conv_id[i] == '\n' || g_conv_id[i] == '\r' ||
            g_conv_id[i] == ' ' || g_conv_id[i] == '\t') {
            g_conv_id[i] = '\0';
            break;
        }
    }
}

static bool g_pending;              /* 下一帧要真的发请求 */
static int  g_pending_slot;         /* 占位消息的下标，回来后原地替换 */
static char g_pending_text[CHAT_TEXT];

/* ★ 翻译模式：**不新开屏**，只给对话屏加一个模式位。
 * 翻译要的输入法、语音、多轮、滚动、错误提示对话屏全有，
 * 复制一份等于把同样的问题写两遍。这里只在**发出去之前**包一层指令，
 * 界面显示的仍是用户原文。 */
static bool g_translate;

void chat_set_translate(int on)
{
    g_translate = (on != 0);
    app_note("chat: translate mode = %d", (int)g_translate);
}

/* --------------------------------------------------------------------------
 * 语音输入状态
 *
 * 三态机，刻意做成"录完 → 画一帧 → 再发请求"的**两帧拆法**：
 * 和 do_send 同款理由 —— 识别是阻塞在网络上的（几秒），
 * 如果在按下的那一帧直接发，用户看到的是"点了一下，屏冻住几秒"，
 * 根本不知道发生了什么。
 *
 *   g_rec      正在录。每帧 pump 一次把采样搬进来（非阻塞）
 *   g_zl_prev  ZL 上一帧的状态，用来找按下/松开的边沿（PTT）
 *   g_touch_arm 语音按钮这一轮是不是"正录着"（触摸是点两下，不是按住）
 *   g_asr_armed 已经录完，下一帧真的发识别请求
 *
 * ⚠️ g_asr_armed 与 g_pending（发对话）**不会同时为真**：
 * 语音结束的这一帧用户还可能按 A，但 g_pending 要等下一帧才成立，
 * 而 do_asr 也在下一帧 —— 两条路各自一帧，不会缠在一起。
 * 真撞上了就是"这条语音晚一轮发"，不是错乱。 */
static bool g_rec;
static bool g_zl_prev;
static bool g_touch_arm;
static bool g_asr_armed;

/* --------------------------------------------------------------------------
 * M6 流式上屏状态
 *
 * 这一组只有 do_real_send 的流式路径会碰，做成文件级是为了让回调能看见：
 *   g_live_slot   当前正在被"边收边改"的那条消息（占位槽）
 *   g_live_len    这条消息已有多少字节（增量接在后面，不做 O(n²) 的拼接）
 *   g_live_count  本帧区间内已收的 token 数（节流用）
 *   g_live_tick   上次刷屏时的系统 tick（节流用）
 * ----------------------------------------------------------------------- */
static int  g_live_slot = -1;
static size_t g_live_len;
static u64  g_live_tick;
static int  g_live_count;
static int  g_live_trace;       /* 已记了几条长度轨迹 */
static int  g_live_snap;        /* 已按进度存了几张图 */
static int  g_send_seq;         /* 本次开机第几次发送（多轮时给截图分轮次） */
static bool g_action_ready;     /* 这条回复里带了动作，画完就去推确认屏 */

/* 长度轨迹：每涨 LIVE_TRACE_STEP 字节往 status.txt 记一条。
 * 这是"回复是一点一点长出来的"最直接的证据 —— 比截图稳，见 on_delta。 */
#define LIVE_TRACE_N    8
#define LIVE_TRACE_STEP 64

/* 进度取证：每涨 LIVE_SNAP_STEP 字节存一张截图，最多 LIVE_SNAP_N 张。 */
#define LIVE_SNAP_N     4
#define LIVE_SNAP_STEP  128

/* 节流阈值：任一满足就刷一次。
 * 24 个 token ≈ 一行左右；60ms 在 3DS 上是 1000/60≈16.7ms 一帧，
 * 也就是"每三四帧刷新一次"。再密就是白烧 CPU（整屏重画不便宜）。 */
#define CHAT_LIVE_MS     60
#define CHAT_LIVE_TOKENS 24

/* 快捷短语：按 SELECT 轮换着填进输入框。
 * 别看功能小 —— 它是「不用键盘也能把消息发出去」的唯一通道，
 * 自动化验收（H 趟）就靠它，没配外设的真机用户也靠它。
 *
 * 内容从 shared/phrases.c 读（卡上的 phrases.txt），不再编译期写死：
 * 把唯一通道钉死在我拍脑袋想的四句话上，是不合理的。
 * 按 R 改当前这条（见 edit_phrase）。 */
static int g_qp;            /* 下一次 SELECT 填第几条 */
static int g_qp_last = -1;  /* 上一次真的填了第几条（R 改的就是它） */

static void push_msg(MsgKind kind, const char *text, bool in_ctx);
static void edit_phrase(void);
static void export_history(void);
static const ProviderDef *chat_provider(void);
static bool chat_offline(void);
static bool chat_ready(void);

/* ---- 底部锚定 ----
 * 滚动按「条」算的旧办法对长回复失效：一条 10 行的消息会把
 * 预算撑爆，最新的尾巴被屏幕底边裁掉（实测第一条真回复就这样）。
 * 改成从最新往回数行，数到放不下为止 —— 最新一条永远整条可见。 */

#define CHAT_LINES_BUDGET \
    (((W_TOP_CONTENT_B - TEXT_LINE_H) - W_TOP_CONTENT_Y) / TEXT_LINE_H)

/* ---- 思考块的展开状态 ----
 * -1 = 全都收着。默认**收着**：展开的话一段思考会把整屏吃掉，
 * 用户反而看不到它最后给的答案是啥。要看得自己点开。 */
static int  g_think_open = -1;
/* 流式期间"正在收思考" —— 这会儿自动展开，因为这是唯一能看到
 * 模型正在想什么的时刻（真机上思考可能持续好几秒，屏幕不能是一片空白）。 */
static bool g_live_thinking;

/* 思考块展开时最多显示这么多字节（超出就截，尾部加省略号）。
 *
 * 为什么必须截：一块思考可以到 1KB，全画出来就是十几行 ——
 * 上屏整个内容区只有 9 行，于是它把"这条消息要说什么"全挤出去了。
 * 240 字节 ≈ 4~5 行，剩下的字数在标题那行交代（"思考过程（N 字）"），
 * 用户知道"还有更多、但屏幕上放不下"，而不是看到一段没头没尾的话。 */
#define THINK_SHOW_BYTES 240

static const char *kind_badge(MsgKind k)
{
    switch (k) {
        case MSG_ME:     return S(S0097);
        case MSG_AI:     return "AI";
        case MSG_ACTION: return S(S0098);
        default:         return S(S0099);
    }
}

static RColor kind_color(MsgKind k)
{
    switch (k) {
        case MSG_ME:     return w_c(C_SLOT_ACC_DK);
        case MSG_AI:     return w_c(C_SLOT_FG);
        case MSG_ACTION: return w_c(C_SLOT_ACCENT);
        default:         return w_c(C_SLOT_MUTED);
    }
}

/* 左侧类别色带（与设置屏那条色带同一套语言：颜色 = 这是哪一类东西） */
static RColor kind_bar(MsgKind k)
{
    switch (k) {
        case MSG_ME:     return w_c(C_SLOT_ACCENT);
        case MSG_AI:     return w_c(C_SLOT_LINE);
        case MSG_ACTION: return w_c(C_SLOT_ACCENT);
        default:         return w_c(C_SLOT_FAINT);
    }
}

static bool think_is_open(int i)
{
    if (!g_msgs[i].think[0])
        return false;
    if (i == g_live_slot && g_live_thinking)
        return true;                 /* 流式收思考时自动展开 */
    return (i == g_think_open);
}

/* 把思考内容截成能显示的一段。**按 UTF-8 字符边界回退**再补省略号 ——
 * 按字节硬切会把一个汉字劈成半个（中文 UTF-8 在 C 里踩过四次，见 memory）。 */
static void think_display(const char *in, char *out, size_t cap)
{
    size_t n = strlen(in);

    if (n <= THINK_SHOW_BYTES) {
        snprintf(out, cap, "%s", in);
        return;
    }
    {
        size_t cut = THINK_SHOW_BYTES;

        while (cut > 0 && ((unsigned char)in[cut] & 0xC0) == 0x80)
            --cut;
        snprintf(out, cap, S(S0100),
                 (int)cut, in, (int)n);
    }
}

static int wrap_count(float *y, float x, float w, const char *s,
                      RColor c, bool draw)
{
    int n = text_line_count(s, (int)w);

    if (n < 1)
        n = 1;
    if (draw)
        *y = w_wrap(x, *y, w, s, c, TEXT_LINE_H);
    return n;
}

/* ★★ 一条消息怎么画、占几行 —— **只有一个地方说了算**。
 *
 * 早先是 msg_lines() 自己拼一遍 "%s %s"、render_top() 再拼一遍。
 * 那两处在加了思考块之后必然漂移：一处算了块的高度、一处没算，
 * 表现就是"最后一条被切掉一半"或者"下面空出一大块" ——
 * 而这两种在截图上都像是"正常的排版"。
 *
 * 现在绘制与数行走同一个函数（draw=false 只数不画），
 * 于是"屏幕上看到的"和"滚动预算算的"**不可能**对不上。 */
static int msg_layout(int i, float x, float y_in, float w, bool draw,
                      float *y_out)
{
    char  line[CHAT_AITXT + 32];
    float y   = y_in;
    int   n   = 0;
    bool  open;

    /* 先数一遍，色带要用整块的高度 */
    {
        float probe = y_in;
        int   sub   = 0;

        snprintf(line, sizeof line, "%s %s",
                 kind_badge(g_msgs[i].kind), g_msgs[i].text);
        sub += wrap_count(&probe, x, w, line, kind_color(g_msgs[i].kind), false);
        if (g_msgs[i].think[0]) {
            char hdr[72];
            open = think_is_open(i);
            snprintf(hdr, sizeof hdr, S(S0101),
                     (int)strlen(g_msgs[i].think),
                     open ? "▾" : S(S0102));
            sub += wrap_count(&probe, x, w, hdr, w_c(C_SLOT_MUTED), false);
            if (open) {
                char shown[THINK_SHOW_BYTES + 48];
                think_display(g_msgs[i].think, shown, sizeof shown);
                sub += wrap_count(&probe, x + 10.0f, w - 10.0f, shown,
                                  w_c(C_SLOT_MUTED), false);
            }
        }
        n = sub;
    }

    if (!draw) {
        if (y_out)
            *y_out = y_in;
        return n;
    }

    /* ---- 绘制 ---- */
    r_rect(&(Rect){ x - 6, y - 2, 3, n * TEXT_LINE_H, 0 }, kind_bar(g_msgs[i].kind));

    snprintf(line, sizeof line, "%s %s",
             kind_badge(g_msgs[i].kind), g_msgs[i].text);
    y = w_wrap(x, y, w, line, kind_color(g_msgs[i].kind), TEXT_LINE_H);

    if (g_msgs[i].think[0]) {
        char hdr[72];
        open = think_is_open(i);
        snprintf(hdr, sizeof hdr, S(S0101),
                 (int)strlen(g_msgs[i].think),
                 open ? "▾" : S(S0102));
        y = w_wrap(x, y, w, hdr, w_c(C_SLOT_MUTED), TEXT_LINE_H);
        if (open) {
            char shown[THINK_SHOW_BYTES + 48];
            think_display(g_msgs[i].think, shown, sizeof shown);
            y = w_wrap(x + 10.0f, y, w - 10.0f, shown,
                       w_c(C_SLOT_MUTED), TEXT_LINE_H);
        }
    }

    if (y_out)
        *y_out = y;
    return n;
}

static int msg_lines(int i)
{
    if (g_lines[i] > 0)
        return g_lines[i];

    int n = msg_layout(i, 0.0f, 0.0f, (float)W_TOP_USABLE, false, NULL);

    if (n < 1)
        n = 1;
    g_lines[i] = (short)n;
    return n;
}

static void reanchor(void)
{
    int used = 0;
    int start = g_count;

    while (start > 0) {
        int ln = msg_lines(start - 1);
        if (used + ln > CHAT_LINES_BUDGET && start < g_count)
            break;
        used += ln;
        --start;
    }
    g_scroll = start;
}

static void push_msg(MsgKind kind, const char *text, bool in_ctx)
{
    if (g_count >= CHAT_MAX) {
        /* 满了就挤掉最旧的：屏幕本来就放不下更早的内容 */
        memmove(g_msgs, g_msgs + 1, sizeof(Msg) * (CHAT_MAX - 1));
        g_count = CHAT_MAX - 1;

        /* ★ 挤掉一条之后**所有下标都往左挪了一格**，打开的思考块也要跟着挪，
         * 否则它指向的是下一条 —— 用户点开的是 A 条的思考，屏幕上展开的
         * 却是 B 条。这种"差一格"的 bug 在截图上看着完全正常。 */
        if (g_think_open > 0)
            --g_think_open;
        else if (g_think_open == 0)
            g_think_open = -1;
    }
    snprintf(g_msgs[g_count].text, CHAT_AITXT, "%s", text);
    g_msgs[g_count].kind = kind;
    g_msgs[g_count].think[0] = '\0';
    g_msgs[g_count].skip = !in_ctx;
    ++g_count;
    text_changed(g_count - 1);

    /* 跟着滚到底部：新消息出现却要手动往下翻，是最招人烦的 */
    reanchor();
}

/* --------------------------------------------------------------------------
 * 语音输入
 *
 * 麦 → 文字这一段分两层：voice.c 只管波形，asr.c 只管"波形变字"。
 * 这里只做**编排**：什么时候开始、什么时候收尾、结果往哪儿放。
 *
 * ⚠️ 操作逻辑：**按住 ZL 说话、松手即停**（PTT），5 秒硬上限只作兜底。
 * 停录的权威触发只有 ZL 释放边沿 —— 不再用「静音检测自动收尾」
 * （voice_silent_long）：3DS 底噪下，说话中途一停顿就会被误切、要重录，
 * 那才是体感"反应慢/不准"的真正来源。去掉后松手立刻开始识别，反应最快。
 * 上行只发「有声音那段 + 两端留白」（见 voice.c），不浪费带宽也不咬字。
 * ----------------------------------------------------------------------- */

/* ASR 的配置来源：卡上的 asr.txt。只有 provider 名和 Key，
 * 地址仍在编译期表里（见 asr.h）。
 * 每次进屏重读 —— 用户随时可能在电脑上改这个文件，和 phrases 一个道理。 */
#define ASR_CFG_PATH "sdmc:/3ds/aiconsole/asr.txt"

/* 音量条（纯 ASCII —— ▁▃ 这类"块元素"字符 3DS 内置字体里没有字形，
 * 画出来是一堆豆腐块）。
 *
 * 做这个是为了让"有没有真的采到声音"**当场看得见**：
 * 模拟器里的麦克风是静音的（RMS≈0），不把它露出来的话，
 * 用户按完 ZL 只看到一句"识别失败"，分不清是自己没说话、
 * 还是这台机器根本采不到声 —— 上个版本就是这样，
 * 我把"麦克风能用"和"真能听到声音"当成一件事，白查了半天。
 * 门限对得上是采集层的 VOICE_RMS_GATE 的 1/4/2/4 档。 */
static const char *voice_bar(int rms)
{
    static char b[10];
    /* 6 格 7 档。门限按"量级"分而不是等分 —— 说话时 RMS 通常在
     * 几百到几千，静音底噪是个位数，直接线性等分会把 0~300 全挤在第 1 格。 */
    int n = (rms < 50) ? 0 : (rms < 200) ? 1 : (rms < 400) ? 2
          : (rms < 800) ? 3 : (rms < 1600) ? 4 : (rms < 3200) ? 5 : 6;
    int i;

    b[0] = '[';
    for (i = 0; i < 6; ++i)
        b[1 + i] = (i < n) ? '=' : ' ';
    b[7] = ']';
    b[8] = '\0';
    return b;
}

static void voice_begin(void)
{
    if (!voice_available()) {
        /* 说清"是什么没有"，不要一句"语音不可用"打发 ——
         * 模拟器里这就是常态，用户得知道是"这台机器/这个环境"而不是坏了。 */
        app_status_set(S(S0103));
        app_note("voice: unavailable mic_rc=%d", voice_mic_rc());
        return;
    }
    if (!voice_start()) {
        app_status_set(S(S0104));
        app_note("voice: start failed rc=%d", voice_mic_rc());
        return;
    }

    g_rec        = true;
    g_asr_armed  = false;
    app_status_set(S(S0105));
    app_note("voice: begin");
}

static void voice_finish(void)
{
    int n;
    int ms;
    int rms;
    size_t sent;

    if (!g_rec)
        return;                 /* 幂等：松手那一帧可能同时被 VAD 触发 */

    n    = voice_stop();
    ms   = voice_ms();
    rms  = voice_rms();
    /* 实际送上去的字节数（voice_pcm 只给"有声音"那段，静音不发）。
     * captured= 是录了多少，sent= 是发多少 —— 两个数分开写，真机上
     * 才能一眼看出裁剪有没有生效（sent 明显小于 captured 才对）。 */
    sent = 0;
    voice_pcm(&sent);
    g_rec = false;

    if (ms < VOICE_MIN_MS) {
        app_status_set(S(S0106));
        app_note("voice: too short ms=%d bytes=%d rms=%d", ms, n, rms);
        return;
    }

    app_note("voice: captured ms=%d bytes=%d rms=%d sent=%d",
             ms, n, rms, (int)sent);
    app_status_set(S(S0107));
    g_asr_armed = true;         /* 下一帧才真的发（见文件头两帧拆法的说明） */
}

static void do_asr(void)
{
    size_t      n = 0;
    AsrResult   r;
    const AsrDef *d = asr_current();
    const void  *pcm = voice_pcm(&n);

    if (n < 2) {
        app_status_set(S(S0108));
        app_note("voice: empty pcm");
        return;
    }

    boot_mark("asr", "begin provider=%s model=%s bytes=%d",
              d->name, d->model, (int)n);
    app_note("voice: asr provider=%s model=%s bytes=%d key=%d refilled=%d",
             d->name, d->model, (int)n, asr_key()[0] ? 1 : 0,
             asr_key_refilled());

    asr_transcribe(pcm, n, &r);

    boot_mark("asr", "done ok=%d http=%ld", (int)r.ok, r.http);
    /* 网络三段耗时（DNS/TCP/TLS）—— 用来复核"连接复用"到底有没有生效。
     * 第一次是冷的，第二次起 dns/conn/tls 应当明显变小。 */
    {
        double td = 0.0, tc = 0.0, tl = 0.0, tt = 0.0;
        asr_last_timing(&td, &tc, &tl, &tt);
        app_note("voice: net dns=%.2f conn=%.2f tls=%.2f total=%.2f retried=%d",
                 td, tc, tl, tt, asr_retried());
    }

    if (r.ok && r.text[0]) {
        /* **追加**到输入框尾部，而不是覆盖：语音接着键盘打是很自然的需求
         * （先输了"帮我看"，再接着说"这个文件"）。 */
        size_t have = strlen(g_input);
        size_t add  = strlen(r.text);

        if (have + add + 2 < CHAT_TEXT) {
            snprintf(g_input + have, CHAT_TEXT - have, " %s", r.text);
        } else {
            snprintf(g_input, CHAT_TEXT, "%s", r.text);
            r.truncated = true;
        }

        app_voice_report(1);
        app_status_set(S(S0109), r.text_len,
                       r.truncated ? S(S0110) : "");
        app_note("voice: ok len=%d %s", r.text_len, r.text);
    } else {
        /* 状态行只有 384px —— 与 do_send 失败分支同一条规矩：
         * 理由全文走对话流（w_wrap 会折行），这儿只留一句短提示。
         * 早先写成 "识别失败：%s（原因看 status.txt）"，把 curl 的
         * 原文（"没连上：Timeout was reach"）塞进去直接撑爆版面，
         * 护栏报的是"467px > 384px"，看的人只会以为是文案超长，
         * 不会想到是"为什么要把整句话塞进状态行"。 */
        app_voice_report(2);
        /* 短提示必须**能区分原因**。早先一律报"识别失败，原因看下面那条"，
         * 于是"provider 选了个连不通的域名"（curl -28，屏上显示没连上）
         * 和"Key 填错了/没额度了"（HTTP 401）在屏上长得一模一样 ——
         * 用户照着"没连上"去查网线、查代理，永远查不到真正的错。
         * 仍守 384px 的规矩：状态行只给一句短话（约 15 个半角），
         * 具体理由走对话流。
         *
         * http < 0  = curl 层面没连上（域名不可达 / DNS / TLS）
         * http 401/403 = 连上了但被拒（Key 不对、没额度、provider 名下没这模型）
         * http 其它 = 服务器回了业务错误码 */
        if (r.ok)
            app_status_set(S(S0111));
        else if (r.http < 0)
            app_status_set(S(S0112));
        else if (r.http == 401 || r.http == 403)
            app_status_set(S(S0113), r.http);
        else
            app_status_set(S(S0114));

        /* ★★ 必须把原因**写进对话流**，否则上面那句"原因见上屏"是空头支票。
         *
         * 2026-10-10 真机反馈原话：「我都没看到上屏有文字」——
         * 这一支早先只调了 app_status_set()（下屏状态行）和 app_note()，
         * 而 **app_note 写的是 status.txt，要拔卡拷出来才看得到**，
         * 对话流一个字都没写。于是上屏干干净净，
         * 用户只知道"连接失败"，连"是没配 Key 还是超时"都分不出来。
         *
         * 这与 do_send 的失败分支是同一条规矩（那里早就这么做了）：
         * 状态行只有 384px，**理由全文一律走对话流**（w_wrap 会折行）。 */
        {
            char why[CHAT_AITXT];
            if (r.ok)
                snprintf(why, sizeof why, S(S0760),
                         (unsigned)n);
            else
                snprintf(why, sizeof why, S(S0116),
                         r.err[0] ? r.err : S(S0117));
            push_msg(MSG_SYS, why, false);
        }
        app_note("voice: fail http=%ld body=%d model_len=%d clamped=%d %s",
                 r.http, asr_last_body_len(), asr_last_model_len(),
                 asr_last_clamped(), r.err);
    }
}

static void do_send(void)
{
    /* 黑匣子：发送键是"卡死/没反应"投诉的高发路径，每次按都留痕 */
    boot_mark("do_send", "in_len=%d have_key=%d offline=%d",
              (int)strlen(g_input), (int)g_have_key, (int)chat_offline());

    if (g_input[0] == '\0') {
        app_status_set(S(S0118));
        return;
    }
    if (!chat_ready()) {
        app_status_set(S(S0119));
        return;
    }

    if (g_translate) {
        const char *pfx = uistr_lang() ? S(S0895) : S(S0894);
        size_t n = strlen(pfx);
        if (n + strlen(g_input) + 1 < CHAT_TEXT) {
            memcpy(g_pending_text, pfx, n);
            snprintf(g_pending_text + n, CHAT_TEXT - n, "%s", g_input);
        } else {
            /* 拼不下就退回普通发送并说清楚 —— 静默截断会让模型只拿到
             * 半句原文，译出来的东西完全跑偏，而用户看不出为什么。 */
            snprintf(g_pending_text, CHAT_TEXT, "%s", g_input);
            app_status_set("%s", S(S0896));
        }
    } else {
        snprintf(g_pending_text, CHAT_TEXT, "%s", g_input);
    }
    push_msg(MSG_ME, g_input, true);

    /* ★ 用户自己说的话**立刻**落盘，不等 AI 回复。
     * 早先只有"成功回复后"才整份保存，于是：网络断、Key 失效、用户中途退出
     * —— 他刚打的那段话一个字都不会留在卡上。而"我说过的话"恰恰是他最
     * 不愿意丢的东西。 */
    if (!chatlog_append(true, g_input))
        app_note("chat: append(me) FAILED len=%d", (int)strlen(g_input));

    /* 占位消息：先占个位，请求回来后原地替换成真内容。
     * 它**不进上下文**（in_ctx=false）—— 万一请求失败、这条留在了
     * 列表里，下一轮把它当"AI 说过的话"发出去就荒谬了。 */
    g_pending_slot = g_count;
    push_msg(MSG_AI, S(S0120), false);

    g_input[0] = '\0';
    g_pending = true;

    /* 状态行说清楚"这次到底在干什么"。离线演示时还写"正在请求"就是在撒谎 ——
     * 用户会以为网通了。 */
    if (chat_offline())
        app_status_set(S(S0121));
    else
        app_status_set(app_cfg()->ai.stream ? S(S0122) : S(S0123));
}

/* 这次对话实际在用的服务商：仓库激活条目优先（chat_enter 已把它的
 * provider 存进 g_provider），其次 key.txt，最后默认。
 * 显示和请求必须用同一个来源 —— 屏上写着 zhipu、请求却发去 deepseek
 * 是最坑人的不一致。 */
static const ProviderDef *chat_provider(void)
{
    /* ★ 每次解析之前，把配置里的自定义网址/模型喂给 provider 层。
     *
     * 放在这里而不是"配置一改就同步"，是因为这里**就是**"这次请求用哪家"
     * 的唯一定夺处 —— 只要请求从这儿出去，喂进去的一定是最新的。
     * 散着同步（改配置处一次、进屏处一次）迟早会漏一处，
     * 而漏掉的那处是**静默**的：自定义地址为空 → 请求发不出去。 */
    provider_set_custom(app_cfg()->ai.base_url, app_cfg()->ai.model);

    if (g_provider[0])
        return provider_find(g_provider);
    return provider_find(app_cfg()->ai.provider);
}

/* 当前服务商是不是"离线演示"。
 * ★ 判据是 **p->offline**（见 provider.h），既不是判名字、也不是判"网址为空"。
 *   判名字：以后加第二个离线源要改两处。
 *   判空网址：**自定义服务商没填网址时也是空串**，会被静默当成演示 ——
 *   2026-10-16 就是这条逼出了 offline 字段。 */
static bool chat_offline(void)
{
    return chat_provider()->offline;
}

/* 现在能不能把消息发出去。
 * 有 Key 当然能；没有 Key 但服务商是离线演示也能 —— 不需要 Key 正是它的卖点。
 * 界面上一切"能不能发"的判断都该走这里，别再散着判 g_have_key。 */
static bool chat_ready(void)
{
    /* ⚠️ 豆包**必须先判**：它走登录态、不走 API Key，
     * 所以"有没有 Key"这个判据对它恒为假。不单独分这一支的后果是
     * 「已经扫码登录好豆包」的用户在对话屏被拦住，而提示让他去配一个
     * **豆包根本没有的 Key** —— 方向完全错了，他会去找一个不存在的东西。 */
    if (chat_provider()->fmt == PROVIDER_FMT_DOUBAO)
        return db_load_cookie() && db_has_cookie();
    return g_have_key || chat_offline();
}

/* ---- 长回复切块 ----
 * 一条回复再长也不能裁尾巴：按显示预算切成几条连发。
 * 8 行 × ~29 个全角字 ≈ 690 字节，取 512 留足余量
 * （前缀 "AI " 占宽、块尾切在行中间都会多出一行）。
 * 行级滚动（部分行渲染）等真需要时再做，现在不做没必要的机制。 */
#define REPLY_CHUNK 512

/* 把切点对齐到 UTF-8 码点起点 —— 不许把多字节字符腰斩 */
static size_t utf8_floor(const char *s, size_t n)
{
    size_t i = n;
    while (i > 0 && ((unsigned char)s[i] & 0xC0) == 0x80)
        --i;
    return i;
}

static void push_reply_chunks(const char *reply, int first_slot)
{
    static char chunk[REPLY_CHUNK + 1];
    size_t len = strlen(reply);
    size_t off = 0;
    bool first = true;

    do {
        size_t n = len - off;
        if (n > REPLY_CHUNK)
            n = utf8_floor(reply + off, REPLY_CHUNK);
        memcpy(chunk, reply + off, n);
        chunk[n] = '\0';

        if (first && first_slot >= 0 && first_slot < g_count) {
            /* 第一块进占位槽 —— 消息已经占着位置，原地替换。
             * 顺手把它转成"有效对话"：占位时它是 skip 的，现在里面
             * 是真的 AI 回复了，必须让后续轮次能把它当上下文。 */
            snprintf(g_msgs[first_slot].text, CHAT_AITXT, "%s", chunk);
            g_msgs[first_slot].skip = false;
            text_changed(first_slot);
        } else {
            push_msg(MSG_AI, chunk, true);
        }
        first = false;
        off += n;
    } while (off < len);
}

/* --------------------------------------------------------------------------
 * 流式：回调 + 帧内刷新
 * ----------------------------------------------------------------------- */

/* --------------------------------------------------------------------------
 * 流式期间把验收脚本推进一格
 *
 * 主循环是 handle() → render()，而整个流式都跑在 handle() 里面 ——
 * 也就是说这几秒钟 main.c 的帧循环**一步都没走**，autorun 的 wait/shot
 * 跟着一起停摆。后果是 M 趟那句"回复过程中连拍"全部落在流式结束之后：
 * 四张截图一模一样（2026-10-01 实测，ink 全是 0.797，等于什么都没证明）。
 *
 * live_flush 是流式期间唯一周期性执行的地方，就让它顺手推一下脚本。
 * 附带效果是脚本在流式期间的节奏按 live_flush 的 60ms 走，比主循环的
 * ~300ms 快 —— 这正是我们要的：连拍才会落在吐字过程中。
 * ----------------------------------------------------------------------- */
static void pump_autorun(void)
{
    static u64 last;
    u32 keys = 0;
    int tx = -1, ty = -1;
    int stick_y = 0;
    const char *shot = NULL;
    bool quit = false;
    u64 now;

    if (!autorun_active())
        return;

    /* 自己压一个 200ms 的闸门：脚本的 wait 是按帧算的，而主循环在模拟器里
     * 约 0.2s 一帧。若跟着 live_flush 的 60ms 推，流式期间脚本会跑快 3 倍，
     * 最后那张"回复完成"的截图会在吐字还没结束时就按下去。 */
    now = svcGetSystemTick();
    if (last && now - last < (u64)CPU_TICKS_PER_MSEC * 200)
        return;
    last = now;

    autorun_tick(&keys, &tx, &ty, &stick_y, &shot, &quit);

    /* keys 丢掉：流式期间不响应按键（"取消"是 M7 的事）。
     * quit 也丢掉：脚本喊退出也不能在这儿退，会把连接留在半路。
     * stick_y 同样丢掉：流式期间上屏在自动跟随最新内容（reanchor），
     * 这时让摇杆插一脚只会和"边收边滚"打架。两者都只能等回到主循环再处理。 */

    if (shot) {
        char name[48];
        shot_mkdirs(SHOT_DIR);
        snprintf(name, sizeof name, "%s", shot);
        shot_sanitize(name);
        shot_save_both(SHOT_DIR, name);
    }
}

/* --------------------------------------------------------------------------
 * 流式进度取证：按**内容进度**自动截图
 *
 * 为什么不靠脚本连拍：脚本的 wait 是按帧算的，而流式整个阻塞在 handle()
 * 里，主循环停摆、帧号不走 —— 四张会全拍在流式结束之后（2026-10-01 实测）。
 * pump_autorun 让它能在流式期间继续走，但那是**时间驱动**的，快慢取决于
 * 渲染耗时，于是同一份代码有时拍到过程、有时全拍到结尾（两次实测交替出现）。
 *
 * 改按内容驱动：长度每涨一档就存一张。长度是单调的，因而必然拍到
 * 递增的几个阶段 —— 这件事不依赖任何时序假设，也就没有竞态可言。
 * ----------------------------------------------------------------------- */
static void live_snap(void)
{
    char name[32];

    /* g_live_slot >= 0 表示"正有一条消息在被边收边改"，即流式进行中 */
    if (g_live_slot < 0 || g_live_snap >= LIVE_SNAP_N)
        return;
    if (g_live_len < (size_t)(g_live_snap + 1) * LIVE_SNAP_STEP)
        return;

    /* 用 if 而不是 while：一次增量跨过好几档时只存一张（那张对应的是
     * 已经长到的地方）。while 会在同一个内容状态下连拍几张一模一样的。 */
    ++g_live_snap;
    /* 名字里带**轮次**：多轮对话时每轮都会产生 stream_1..4，
     * 不带轮次前缀的话后一轮会把前一轮的图覆盖掉。 */
    snprintf(name, sizeof name, "stream_%d_%d", g_send_seq, g_live_snap);
    shot_mkdirs(SHOT_DIR);
    shot_save_both(SHOT_DIR, name);
}

/* 把当前屏重画一遍然后提交。只在流式回调里调。
 *
 * ★★ 必须**两屏都画完再提交一次** —— 与主循环 app_render() 完全同一条规矩。
 *
 * r_end() 里的 gfxSwapBuffers() 是**全局**的：它翻转所有屏幕的缓冲索引，
 * 而不是"当前 target"那一块。早先这里"只画上屏就 r_end()"，
 * 于是底屏的索引照样翻、却没有新内容写进去 ——
 * 底屏在**最近两次画过的内容**之间来回翻。
 * 那两块内容只要有一点不同，用户看到的就是"底屏一直在闪"。
 * 2026-10-10 真机反馈的正是这个（离线演示生成回复时最明显：
 * 流式刷新约 60ms 一次，翻得慢，眼睛看得清）。
 *
 * 早先这里的注释写着"底屏内容与输入状态无关，少画一块屏 = 每次刷新省一半时间"——
 * 省时间的**前提是不动它的缓冲**，而 r_end() 做不到。
 * 省下来的那点时间，不值一块闪屏的底屏。
 * （顺带修好：live_snap() 拍的是两屏，底屏不再会拍到一个中间态。） */
static void live_flush(void)
{
    const Screen *s = screen_get(app_current());

    if (!s || !s->render_top)
        return;

    r_target(R_SCREEN_TOP);
    r_begin(w_c(C_SLOT_BG));
    w_overflow_reset();          /* 每帧从零计，与 app_render 一致 */
    s->render_top();

    /* 底屏一并重画。**不要**在这里再 w_overflow_reset()：
     * 主循环也是"上屏之前重置一次、两屏共用这个计数"，
     * 多重置一次会把上屏刚记下的溢出数抹掉。 */
    if (s->render_bottom) {
        r_target(R_SCREEN_BOTTOM);
        r_begin(w_c(C_SLOT_BG));
        s->render_bottom();
    }

    r_end();                     /* 两屏都画完了才换 —— 这是修复的关键 */

    /* 帧已提交，此刻 canvas 就是用户正看着的样子 —— 在这里取证，
     * 拍到的就是"吐字吐到一半"的画面。 */
    live_snap();
    pump_autorun();

    g_live_tick  = svcGetSystemTick();
    g_live_count = 0;
}

/* 回调：每收到一段增量就被 chatapi 叫一次。
 * 这里只做三件事 —— 追加、节流、必要时刷屏；不做任何耗时操作。
 *
 * 增量**直接追加到槽位自己的缓冲**里，不额外做一份累积副本：
 * 多一份副本就要每次拷贝整个前缀（O(n²)），而槽位的 text[] 本来就是
 * 我们要显示的东西，长度上限也是它自己的上限。 */
static bool on_delta(const char *delta, bool reasoning, void *user)
{
    (void)user;

    if (g_live_slot < 0 || g_live_slot >= g_count)
        return true;

    size_t n = strlen(delta);

    /* ★ 思考与正文**分开累积**。
     *
     * 混在一起的话用户看到的是"AI 一边回答一边自言自语"，而且
     * 思考动辄几百上千字，会把正文的 1KB 上限直接吃光 —— 那才是真事故：
     * 答案是空的，界面上却有一大段字。 */
    if (reasoning) {
        char  *dst = g_msgs[g_live_slot].think;
        size_t tn  = strlen(dst);

        if (tn + n < CHAT_THINK_MAX) {
            memcpy(dst + tn, delta, n);
            dst[tn + n] = '\0';
            text_changed(g_live_slot);
        }
        /* 流式收思考期间那块自动展开（think_is_open 会认这个标志）——
         * 真机上思考可能持续好几秒，这段时间屏幕不能是一片空白。 */
        g_live_thinking = true;
        reanchor();
    } else {
        char *dst = g_msgs[g_live_slot].text;

        if (g_live_len + n < CHAT_AITXT) {
            memcpy(dst + g_live_len, delta, n);
            g_live_len += n;
            dst[g_live_len] = '\0';
            text_changed(g_live_slot);

            /* 滚到底：新内容出现却要手动翻，是最招人烦的 */
            reanchor();
        }
        /* 正文开始 = 思考阶段结束 */
        g_live_thinking = false;

        /* 长度轨迹只记**正文**：把思考的长度混进来的话，
         * "文本在增长"这条证据会变得含糊（思考一停数字就不动了）。 */
        while (g_live_trace < LIVE_TRACE_N &&
               g_live_len >= (size_t)(g_live_trace + 1) * LIVE_TRACE_STEP) {
            app_note("live: len=%d", (int)g_live_len);
            ++g_live_trace;
        }
    }

    /* 长度轨迹：给"文本在增长"留一条**不依赖截图时机**的证据。
     * 截图那条路受帧率和阻塞影响（见 pump_autorun 的注释），日志不受 ——
     * 它由回调自己写，长度就在手边。跨过每一档记一条，于是 status.txt
     * 里留下一串严格递增的 len：比对图更难糊弄，也不用人眼看。
     * while 而不是 if：一次增量可能跨过好几档（长 token 或补记）。 */
    while (g_live_trace < LIVE_TRACE_N &&
           g_live_len >= (size_t)(g_live_trace + 1) * LIVE_TRACE_STEP) {
        app_note("live: len=%d", (int)g_live_len);
        ++g_live_trace;
    }

    ++g_live_count;

    /* 节流判据：满 CHAT_LIVE_TOKENS 个 token，或者距上次刷屏够 CHAT_LIVE_MS。
     * tick 换算是整数乘除 —— CPU_TICKS_PER_MSEC 是 double，参与运算会把
     * 除法/转换拉进软浮点，这里先算成整数常量。 */
    bool by_time = (svcGetSystemTick() - g_live_tick)
                   >= (u64)CPU_TICKS_PER_MSEC * CHAT_LIVE_MS;
    bool by_tok  = g_live_count >= CHAT_LIVE_TOKENS;

    if (by_time || by_tok)
        live_flush();

    return true;      /* 继续要；用户叫停的场景留给 M7 的取消键 */
}

/* 回复长度档位 → max_tokens。抽出来给**两处**用：do_real_send（真发）与
 * 上下文环（预估）。
 * ★ 必须是同一个函数，不能两边各写一个映射表：max_tokens 是请求体前缀的
 *   一部分（chatapi 的 build_prefix 会把它写进 JSON），两边不一致的话
 *   "预估 3000 字节、实际 3200"这种对不上没人背得动 —— 而环的意义
 *   就在于"发送前就知道会带多少"。
 * 300 大约是一屏对话的量；1200 够写完整攻略步骤。数值保守是因为 3DS 的
 * 屏就那么大，再长也只是滚屏成本，没有信息增量。 */
static int reply_max_tokens(void)
{
    switch (app_cfg()->ai.reply_len) {
    case CONFIG_REPLY_SHORT: return 300;
    case CONFIG_REPLY_LONG:  return 1200;
    default:                 return 600;
    }
}

/* 从下标 end（**不含**）往前数 CHAT_CTX_TURNS 条有效对话，填进 turns，
 * 返回条数。text 借用 g_msgs 的指针，不复制 —— 调用期间 g_msgs 不动，
 * 而复制一份要多养 8×1024 字节的静态区，不值。
 *
 * ★ do_real_send 与上下文环**共用这一个函数**（铁律：预估与实测同源）。
 *    各写一遍的后果是"环说带 5 条、实际发 2 条"，而用户无从判断谁在撒谎。
 *    抽的时候特意连"从 end-1 往回数"这个边界一起搬，没让它变成
 *    "传个数进来、怎么挑我自己算"—— 那种抽法迟早在一侧悄悄改窄。 */
static int collect_turns(int end, ChatTurn *turns)
{
    int n_turns = 0;
    int start;
    int seen = 0;

    if (end > g_count)
        end = g_count;          /* 防御：调用方算错下标时不许读到界外 */
    start = end - 1;

    while (start >= 0 && seen < CHAT_CTX_TURNS) {
        if (!g_msgs[start].skip && g_msgs[start].text[0])
            ++seen;
        --start;
    }
    ++start;

    for (int i = start; i < end; ++i) {
        if (g_msgs[i].skip || !g_msgs[i].text[0])
            continue;
        turns[n_turns].mine = msg_mine(i);
        turns[n_turns].text = g_msgs[i].text;
        ++n_turns;
    }
    return n_turns;
}

/* 拼 system 消息 = 人设 + 记忆 + 回答语言 + 运行手册 + 上一轮动作结果。
 *
 * 为什么必须拼：AI 只有知道"这台机子上我能触发哪几个动作、动作怎么写"，
 * 才可能说出能被本地执行的东西；而手册是 KB 量级的内容，config 里那个
 * CONFIG_MAX_PROMPT 的字段塞不下，也不该混进去 —— 手册要能单独换、
 * 要能审计，它是内容不是人设。
 *
 * ★ 抽出来给两处用：do_real_send（真发）与上下文环（预估）。
 *   环要报"这次会带多少字节"，而其中最大的一块正是这份手册 ——
 *   不让两边走同一个函数，环就只能猜，而猜出来的数字不如不显示。
 *
 * ⚠️ consume = false 时**不消费**动作结果（只读 peek）。
 *    环每帧都在算，真消费的话上一轮的动作结果会被它擦掉，
 *    等真发送时模型反而收不到 —— 这种 bug 在屏上完全看不出来。
 *
 * ⚠️ adbuf（手册暂存）是函数内的静态 Scratch，两个调用方共用一份：
 *    请求路径在 open_and_send 里就把 body 序列化完了，之后不再读它，
 *    所以流式渲染期间环重算不会碰到在飞的请求。 */
static size_t build_sysprompt(char *sp, size_t cap, const char **adoc,
                              bool consume)
{
    static char adbuf[AGENTDOC_MAX];
    size_t spn = 0;
    size_t adl = 0;
    const char *asrc = NULL;   /* 手册是从内嵌副本来的，还是卡上那份 */

    if (!sp || cap == 0)
        return 0;
    sp[0] = '\0';
    if (adoc)
        *adoc = "";

    if (app_cfg()->ai.system_prompt[0]) {
        spn = strlen(app_cfg()->ai.system_prompt);
        if (spn > cap - 3)
            spn = cap - 3;
        memcpy(sp, app_cfg()->ai.system_prompt, spn);
        sp[spn++] = '\n';
        sp[spn++] = '\n';
        sp[spn]   = '\0';
    }

    /* ---- AI 记忆：关于用户的长期事实 ----
     * 放在人设之后、手册之前：人设讲"你是谁"，记忆讲"你面对的是谁"，
     * 手册讲"你能做什么"。这个顺序模型最容易分层对待。
     * 标记用英文 —— 它是给模型的指令，不是给用户看的界面文案。 */
    if (app_cfg()->ai.memory[0]) {
        int n = snprintf(sp + spn, cap - spn,
                         "[About the user]\n%s\n\n", app_cfg()->ai.memory);
        if (n > 0 && spn + (size_t)n < cap)
            spn += (size_t)n;
    }

    /* ---- 回答语言 ----
     * 明确发一条指令，而不是"让模型自己跟着上下文走"：
     * 界面英文但历史里有中文时，模型会摇摆，同一个会话里中英混着来。
     * 0 = 跟随界面（换算成具体语言），1 = 中文，2 = 英文。 */
    {
        int rl   = app_cfg()->ai.reply_lang;
        int lang = (rl == 0) ? uistr_lang() : (rl - 1);
        int n = snprintf(sp + spn, cap - spn, "%s\n\n",
                         lang ? "Reply in English." : "Reply in Chinese.");
        if (n > 0 && spn + (size_t)n < cap)
            spn += (size_t)n;
    }

    /* 手册不在就**没有手册**，不做兜底文本。
     * 宁可这一轮少份约束，也不能拿程序自己编的、或过期的假手册骗模型 ——
     * 那等于给模型一张错地图，比没有地图更糟。 */
    if (agentdoc_load(adbuf, sizeof adbuf, &adl, &asrc)) {
        if (adoc)
            *adoc = asrc ? asrc : "?";
        if (adl > cap - spn - 1)
            adl = cap - spn - 1;    /* 装不下就到此为止，绝不写出界 */
        memcpy(sp + spn, adbuf, adl);
        spn += adl;
        sp[spn] = '\0';
    }

    /* ---- 上一轮动作的结果，回灌给模型 ----
     * 放进 system 而不是塞进对话历史：塞进历史会被当成"用户说过的话"，
     * 下一轮它可能复述给用户听。它是**系统告诉它的事实**。
     * 消费一次即清：同一份结果重复发会让模型以为出了两次不同的结果。 */
    if (consume) {
        const char *res = app_act_result_get();
        if (res) {
            size_t rl = strlen(res);
            if (spn + rl + 40 < cap)
                spn += (size_t)snprintf(sp + spn, cap - spn,
                                        S(S0124), res);
            app_act_result_clear();
        }
    }
    return spn;
}

static void do_real_send(void)
{
    /* 静态区：2.2KB 的结构体放栈上会把发送路径的栈深顶起来
     * （单线程、一次只有一个请求，静态化是安全的） */
    static ChatResult res;
    bool ok;
    bool stream_ok = false;         /* 流式路径是否真的成功了（探针用） */
    const ProviderDef *p = chat_provider();
    const bool want_stream = app_cfg()->ai.stream;

    /* 第几轮。多轮对话里每轮都会产生 stream_N_*.bmp，得能区分开。 */
    ++g_send_seq;

    /* 黑匣子：HTTP 是阻塞的 —— 如果模拟器里请求挂死，
     * boot.log 停在 http begin、永远没有 done，就是铁证 */
    boot_mark("http", "begin provider=%s model=%s len=%d stream=%d",
              p->name, g_model[0] ? g_model : "-",
              (int)strlen(g_pending_text), (int)want_stream);

    /* ---- 上下文：把之前几轮带上，让模型知道我们在聊什么 ----
     *
     * 本次这句**不**放进来 —— 它单独作为 user_text 发。放两遍模型会
     * 以为你说了两次，轻则答两遍，重则开始复读。
     *
     * 只取最近 CHAT_CTX_TURNS 条**有效**对话（见 Msg.skip）。更早的对
     * 当下这轮几乎没用，却会先把请求预算（4KB）吃掉，结果是"该带的
     * 没带上、不该带的占着地方"。
     *
     * 挑选逻辑在 collect_turns() 里，和上下文环共用 —— 见那里的注释。 */
    static ChatTurn turns[CHAT_CTX_TURNS];
    const int end = g_pending_slot - 1;   /* = 本次用户消息的下标，不含它 */
    int n_turns = collect_turns(end, turns);

    /* 打包成一次请求要带的全部背景。拼装本身在 build_sysprompt() 里，
     * 与上下文环共用（见那里的注释：环要报字节数，就不能另猜一套）。
     *
     * 缓冲放静态区：两段加起来 20 KB 上下，叠在这个函数的栈上不合适，
     * 这条路径（send）的栈深已经很有名了。 */
    static char g_sysprompt[CONFIG_MAX_PROMPT + CONFIG_MAX_MEMORY
                            + AGENTDOC_MAX + ACT_RESULT_MAX + 160];
    const char *adoc = "";     /* 探针用：这一轮到底带没带手册、哪来的 */
    size_t spn = build_sysprompt(g_sysprompt, sizeof g_sysprompt,
                                 &adoc, true);

    /* 上下文轮数：按配置裁掉最早的。
     * 裁剪放在这里而不是保存历史时 —— 用户可能只想"这次少带点"，
     * 而不是把旧记录删掉（那是另一件事，chatlog 有自己的上限）。 */
    int keep = app_cfg()->ai.context_turns;
    int from = (n_turns > keep) ? (n_turns - keep) : 0;

    /* 回复长度 → max_tokens。映射表在 reply_max_tokens()，与环共用。 */
    int mt = reply_max_tokens();

    ChatCtx cx;
    cx.system_prompt = g_sysprompt;
    cx.turns         = turns + from;
    cx.n_turns       = n_turns - from;
    cx.max_tokens    = mt;
    cx.conversation_id = g_conv_id;      /* 续聊；空 = 服务端新建 */

    /* 探针：多轮和人设到底有没有生效，必须能从 status.txt 看出来 ——
     * 只看界面是看不出的（模型记不记得住上句，从一次回答判断不了）。
     *
     * 报的是**实际发出去的那份**的长度，不是 config 里那份：
     * 配置说带了手册而实际没读到，正是要抓的情况。
     * 顺带报手册来源：只报字节数的话，内嵌的那份和卡上过期的那份都是"N 字节"，
     * 从 status.txt 上分不出来。 */
    app_note("ctx: history=%d/%d sys=%d mt=%d doc=%s", n_turns - from, keep, (int)spn, mt, adoc);

    if (want_stream) {
        /* 流式：占位槽从空开始，回调往里长。
         * 清掉「正在问…」—— 留着的话第一段增量会拼在它后面。 */
        g_live_slot  = g_pending_slot;
        g_live_len   = 0;
        g_live_count = 0;
        g_live_trace = 0;
        g_live_snap  = 0;
        g_live_tick  = svcGetSystemTick();
        if (g_live_slot >= 0 && g_live_slot < g_count) {
            /* 占位转正：内容马上要长进来，它从现在起是 AI 真的说过的话 */
            g_msgs[g_live_slot].text[0] = '\0';
            g_msgs[g_live_slot].skip    = false;
            text_changed(g_live_slot);
        }

        ok = chat_completion_stream(p->name,
                                    g_model[0] ? g_model : NULL,
                                    app_cfg()->ai.thinking,
                                    g_key, &cx, g_pending_text,
                                    on_delta, NULL, &res);
        g_live_slot = -1;
        stream_ok = ok;

        if (!ok) {
            /* 退回非流式。理由要记下来 —— "为什么这条没流式"是个会被问的问题。
             * 前缀用 "chat: stream" 而不是 "chat:"，免得和"最终结果"那条
             * 混在一起：验收脚本会按前缀抓记录，两条同前缀会互相污染。 */
            app_note("stream: failed http=%d %s -> fallback",
                     res.http_code, res.error[0] ? res.error : "?");
            boot_mark("http", "stream fallback http=%d err=%s",
                      res.http_code, res.error);
            /* 占位槽在我们手里被清空了，恢复成占位文案，免得空一条看起来像 bug */
            if (g_pending_slot >= 0 && g_pending_slot < g_count) {
                snprintf(g_msgs[g_pending_slot].text, CHAT_AITXT, "%s", S(S0120));
                text_changed(g_pending_slot);
            }
            ok = chat_completion(p->name,
                                 g_model[0] ? g_model : NULL,
                                 app_cfg()->ai.thinking,
                                 g_key, &cx, g_pending_text, &res);
        }
    } else {
        ok = chat_completion(p->name,
                             g_model[0] ? g_model : NULL,
                             app_cfg()->ai.thinking,
                             g_key, &cx, g_pending_text, &res);
    }

    boot_mark("http", "done ok=%d http=%d", (int)ok, res.http_code);

    /* 报告"实际走了哪条路"。探针值进 status.txt，验收脚本据此断言 ——
     * 只看配置字段是不够的（配置说开、实际退回，正是要抓的情况）。 */
    app_stream_report(want_stream ? (stream_ok ? 1 : 2) : 0);

    if (g_pending_slot >= 0 && g_pending_slot < g_count) {
        if (ok) {
            /* ---- 从回复里抠动作 ----
             * 模型要"做"一件事时，会在正文里夹一段 [[ACT:...]]。
             * 这里把它解析出来、并从正文里剥掉：用户看到的还是人话，
             * 而动作走确认屏（三道闸的第三道，实现在 sc_confirm.c）。
             *
             * 缓冲放静态区：CHAT_REPLY_MAX 是 2KB，叠加在 send 路径的
             * 栈上不合适（这条路径的栈深已经很有名了）。 */
            static char clean[CHAT_REPLY_MAX];
            ActionReq   req;
            ActionId    act = actions_parse(res.reply, clean, sizeof clean, &req);

            /* ★ 非流式那条路没有回调可分股，思考内容得在这里补进槽位。
             * 流式那半边由 on_delta 直接写进同一个字段。
             *
             * 判空放在这里：`res` 是 static，chat_completion 每轮会清，
             * 所以"这一轮没思考"就是空串 —— 空串不该把上一轮的思考留下来。 */
            if (res.reasoning[0]) {
                snprintf(g_msgs[g_pending_slot].think, CHAT_THINK_MAX,
                         "%s", res.reasoning);
                text_changed(g_pending_slot);
            }

            push_reply_chunks(clean, g_pending_slot);

            if (act != ACT_NONE) {
                actions_set_pending(&req);
                g_action_ready = true;
                app_note("action: parsed %s name=%s",
                         actions_verb(act), req.name[0] ? req.name : "-");
            }
        } else {
            /* 显式截断拷贝，不用 snprintf：错误文案比槽位长是正常情况 */
            size_t n = strlen(res.error);
            if (n > CHAT_AITXT - 1)
                n = CHAT_AITXT - 1;
            memcpy(g_msgs[g_pending_slot].text, res.error, n);
            g_msgs[g_pending_slot].text[n] = '\0';
            text_changed(g_pending_slot);
            /* 失败提示不是"AI 说过的话"。不排除掉的话，下一轮它会带着
             * 上一轮的错误信息一起发出去 —— 模型可能顺着那句编下去。 */
            g_msgs[g_pending_slot].skip = true;
        }
    }
    g_pending_slot = -1;

    if (ok) {
        /* 记住服务端给的会话 id，下一句就接着它聊。
         * 少了这一步，豆包每句都在服务端开一个新会话 ——
         * 症状是它回答「我看不到你上一个任务」，而本地明明连着。 */
        if (res.conversation_id[0]) {
            snprintf(g_conv_id, sizeof g_conv_id, "%s", res.conversation_id);
            app_note("chat: conv=%s", g_conv_id);
        }

        /* 状态行报的是**实际走的路**，不是配置里想要的 ——
         * 显示"流式"却其实退回非流式，就是 M6 之前那个撒谎的翻版。 */
        if (chat_offline())
            app_status_set(S(S0125), (int)strlen(res.reply),
                           stream_ok ? S(S0126) : "");
        else
            app_status_set(S(S0127), res.http_code,
                           (int)strlen(res.reply),
                           stream_ok ? S(S0126)
                                     : (want_stream ? S(S0128) : ""));
        app_note("chat: ok provider=%s http=%d len=%d stream=%d",
                 p->name, res.http_code, (int)strlen(res.reply),
                 want_stream ? (stream_ok ? 1 : 2) : 0);
    } else {
        /* 状态行只有 384px，理由的全文放进对话流里（w_wrap 会折行）。
         * 这里只报短句，别让版面护栏替我们收拾溢出。 */
        app_status_set(S(S0129), res.http_code);
        app_note("chat: fail provider=%s http=%d %s", p->name,
                 res.http_code, res.error);
    }

    /* 成功回复 → **追加**一条。
     * ★ 这里原来调 save_history()，而它是 chatlog_open(false) 整份重写：
     *   "wb" 打开即清空，且只写内存里那 24 条 —— 长对话每次保存都在
     *   把更早的内容截掉。现在改成 append，磁盘上的历史只增不减。
     * 失败不追加：失败提示不是对话内容。 */
    if (ok) {
        if (!chatlog_append(false, res.reply))
            app_note("chat: append(ai) FAILED len=%d", (int)strlen(res.reply));
        save_conv_id();

        /* ★★ 2026-10-25 用户要求：「AI 发给我一段代码文件，现在的版本还不能
         *   直接让我点开测试。要能接收到，也能打开。」
         *
         *   做法：回复里每个 ``` 围栏块都写成一个独立文件，落在
         *   actions_dir()（sdmc:/3ds/aiconsole/out）—— 那是沙箱白名单里
         *   唯一允许写的目录，也正是「文件」格列的那个目录。
         *   所以写完不用额外刷新：切到「文件」格就看到，按 A 就能读内容。
         *
         *   ⚠️ 只写文件、**不执行**。模型给的代码是不是能跑、跑了会怎样，
         *      那是用户自己判断的事；本程序不替人按下执行键。 */
        {
            char names[160];
            int  nc = codebox_extract(res.reply, names, sizeof names);

            if (nc > 0) {
                app_status_set(S(S0978), nc, names);
                app_note("chat: codebox saved=%d [%s]", nc, names);
            }
        }
    }
}

/* ★ 现在对话页的输入走**我们自己的拼音 IME**（见键盘按钮 / Y 键），
 *   不再用系统 swkbd —— 3DS 的系统键盘没有中文拼音页，
 *   而 SWKBD_TYPE_WESTERN 在日版机器上还会关掉假名输入，中文打不出来。
 *   这个函数保留作**备用**：将来若要"长文本编辑/粘贴"这种整块替换的场景，
 *   swkbd 的分页/符号表比自绘键盘更省事。保留但暂时无调用点，
 *   所以标 unused 避免 -Werror=unused-function。 */
__attribute__((unused))
static void open_keyboard(void)
{
    SwkbdState swkbd;
    char buf[CHAT_TEXT];
    SwkbdButton btn;

    /* ★ swkbd 的第 4 参是**字符数**，而 buf 是按**字节**算的 CHAT_TEXT。
     * 一个汉字 3 字节 —— 传 CHAT_TEXT-1（=149）等于允许敲 149 个汉字
     * = 447 字节，直接写穿 150 字节的缓冲：
     *   · 若 libctru 按 bufsize 硬切 → 中文被劈成半个，下游 UTF-8 错位；
     *   · 若它不切 → 越界写栈。
     * 两种都是缺陷，所以按 3 字节/字符收紧：
     *   (150-1)/3 = 49 字 × 3 = 147 字节，稳在 150 以内（还留得下结尾符）。
     * 这与同文件 edit_phrase()（30 字 / 100 字节缓冲）是同一条规矩。 */
    swkbdInit(&swkbd, SWKBD_TYPE_WESTERN, 2, (CHAT_TEXT - 1) / 3);
    swkbdSetHintText(&swkbd, S(S0040));
    swkbdSetInitialText(&swkbd, g_input);

    btn = swkbdInputText(&swkbd, buf, sizeof buf);

    if (btn == SWKBD_BUTTON_CONFIRM) {
        snprintf(g_input, CHAT_TEXT, "%s", buf);
        app_status_set(S(S0130), (int)strlen(g_input));
    } else {
        app_status_set(S(S0131));
    }
}

/* --------------------------------------------------------------------------
 * 快捷短语的编辑
 *
 * 改的是**刚才被填进输入框的那一条**（g_qp_last），不是"列表里的第 N 条"——
 * 用户的心智是"我刚填进来那句不合适"，而不是"我要改第几项"。
 * 没填过就退回第 0 条。
 * ----------------------------------------------------------------------- */
static void edit_phrase(void)
{
    SwkbdState swkbd;
    char buf[PHRASE_MAX_LEN + 1];
    char cur[PHRASE_MAX_LEN + 1];
    SwkbdButton btn;
    int  n   = phrases_count();
    int  idx;

    if (n <= 0) {
        app_status_set(S(S0132));
        return;
    }

    idx = (g_qp_last >= 0) ? g_qp_last : 0;
    snprintf(cur, sizeof cur, "%s", phrases_at(idx));

    /* swkbd 的长度参数是**字符数**，而我们的上限是**字节数**。
     * 一个汉字 3 字节，所以 30 字符最多 90 字节，稳在 100 以内。
     * 直接传 PHRASE_MAX_LEN 会让用户敲 100 个汉字（300 字节）撑爆缓冲。 */
    swkbdInit(&swkbd, SWKBD_TYPE_WESTERN, 2, 30);
    swkbdSetHintText(&swkbd, S(S0133));
    swkbdSetInitialText(&swkbd, cur);

    btn = swkbdInputText(&swkbd, buf, sizeof buf);

    if (btn != SWKBD_BUTTON_CONFIRM) {
        app_status_set(S(S0134));
        return;
    }

    buf[PHRASE_MAX_LEN] = '\0';

    if (phrases_set(idx, buf)) {
        app_status_set(buf[0] ? S(S0135) : S(S0136), idx + 1);
        /* 列表变了，旧的下标不再可信 */
        g_qp_last = -1;
        g_qp      = 0;
    } else {
        app_status_set(S(S0137));
    }
}

/* 数一下有多少条"算数的"消息（跳过占位文案与错误提示） */
static int valid_msg_count(void)
{
    int n = 0;

    for (int i = 0; i < g_count; ++i)
        if (!g_msgs[i].skip && g_msgs[i].text[0])
            ++n;
    return n;
}

/* 把对话导到卡上，插电脑就能看。
 * 导的是"真的说过的话"—— 占位文案和错误提示不算对话内容。 */
static void export_history(void)
{
    char path[160];
    int  total = valid_msg_count();
    int  wrote = 0;

    if (total == 0) {
        app_status_set(S(S0138));
        return;
    }

    if (!chatlog_open(true, path, sizeof path)) {
        app_status_set(S(S0139));
        return;
    }

    for (int i = 0; i < g_count; ++i) {
        if (g_msgs[i].skip || !g_msgs[i].text[0])
            continue;
        chatlog_item(msg_mine(i), g_msgs[i].text);
        ++wrote;
    }
    chatlog_close();

    /* 完整路径太长（SD:/3ds/aiconsole/chat_20261001_2245.txt），
     * 状态行只有 384px，说清"去哪儿找"就够。 */
    app_status_set(S(S0140), wrote);
    app_note("export: %s turns=%d", path, wrote);
}

/* ---- 思考块的开/合 ----
 *
 * 交互定成"左/右 翻到哪一条、点按钮 开最新那条"，是因为：
 *   · 上/下 已经被滚动占用（而且滚动是这里最要紧的动作）；
 *   · 上屏**不能触摸**（3DS 只有下屏能点），所以"点开"这件事
 *     只能落在下屏 —— 这就是那个「思考」按钮的来历。
 * 走到头再按一下 = 收起，比"另外找一个收起键"好找得多。 */
static bool any_think(void)
{
    for (int i = 0; i < g_count; ++i)
        if (g_msgs[i].think[0])
            return true;
    return false;
}

static bool think_find(int from, int dir, int *out)
{
    for (int i = from; i >= 0 && i < g_count; i += dir) {
        if (g_msgs[i].think[0]) {
            *out = i;
            return true;
        }
    }
    return false;
}

static void think_step(int dir)
{
    int i;

    if (!any_think()) {
        app_status_set(S(S0141));
        return;
    }

    if (g_think_open >= 0 && think_find(g_think_open + dir, dir, &i)) {
        g_think_open = i;
    } else if (g_think_open >= 0) {
        g_think_open = -1;          /* 走到头 = 收起 */
        app_status_set(S(S0142));
        return;
    } else if (think_find((dir > 0) ? 0 : g_count - 1, dir, &i)) {
        g_think_open = i;           /* 收着的时候：按方向从一头开始 */
    } else {
        return;
    }

    /* 展开会改变这条消息的行数，缓存必须失效，否则滚动预算算的是旧高度 */
    text_changed(g_think_open);
    reanchor();
    app_status_set(S(S0143),
                   (int)strlen(g_msgs[g_think_open].think));
}

/* 点按钮：开/关**最新**那一条 —— 用户绝大多数时候关心的就是刚回的那句。 */
static void think_toggle_latest(void)
{
    int i;

    if (!any_think()) {
        app_status_set(S(S0141));
        return;
    }
    if (g_think_open >= 0) {
        g_think_open = -1;
        app_status_set(S(S0142));
        return;
    }
    if (think_find(g_count - 1, -1, &i)) {
        g_think_open = i;
        text_changed(i);
        reanchor();
        app_status_set(S(S0144),
                       (int)strlen(g_msgs[i].think));
    }
}

/* 把对话存下来（关掉再开还在）。
 * 只存最新的 CHATLOG_MAX_TURNS 条：模块那边也有同一道上限兜底，
 * 但"哪几条算最新的"只有这里知道。 */
/* 恢复历史用的回调：chatlog_load 逐条喂过来，直接塞进消息列表 ——
 * 不进任何中间数组（40 条 × 1KB 不值得为它再开一份）。 */
static void hist_cb(bool mine, const char *text, void *user)
{
    (void)user;
    /* ★ **不要**在这里 `if (g_count >= CHAT_MAX) return;`。
     *   原来就是这么写的，而 chatlog_load 是**从头往下**喂的：
     *   文件里有 40 条、内存只放 24 条时，留下来的是**最旧的 24 条**，
     *   最近说的那些反而被挡在门外 —— 用户重启后看到满屏老对话，
     *   会以为"新的丢了"。push_msg 本来就有"满了挤掉最旧"的逻辑，
     *   交给它处理，留下的自然是最后 24 条。 */
    push_msg(mine ? MSG_ME : MSG_AI, text, true);
}

void chat_reload_project(void)
{
    char slot[16];

    proj_slot(slot, sizeof slot);
    chatlog_set_slot(slot);

    /* 消息表清空重建。不清的话新任务的历史会**接**在旧任务的后面，
     * 而那正是用户抱怨的现象（"换项目了但上屏没变"）。 */
    g_count       = 0;
    g_scroll      = 0;
    g_think_open  = -1;
    g_pending     = false;
    g_pending_slot = -1;
    g_input[0]    = '\0';
    g_conv_id[0]  = '\0';

    int restored = chatlog_load(hist_cb, NULL);
    if (restored > 0)
        load_conv_id();
    reanchor();
    app_note("chat: project switch -> slot=%s restored=%d",
             slot[0] ? slot : "(default)", restored);
}

/* 删掉一个任务时，把**它那一份**历史一起收走（2026-10-24）。
 *
 * 由 sc_dock.c 的 proj_remove() 在 projects_remove() 成功后调用。
 *
 * 两件事必须一起做：
 *   ① 正文文件（chat_p<k>.json）—— chatlog_drop_slot 负责
 *   ② 会话 id 文件（chat_conv_p<k>.txt）—— 这里负责
 * 只挪正文不挪 id 的后果很隐蔽：项目丙的对话正文对上了，
 * 但 g_conv_id 还是乙的会话 id，于是"接着聊"接到的是乙在服务端的会话。
 *
 * 挪完立刻重载 —— 当前上屏显示的可能就是刚被删掉那个项目的内容。 */
void chat_drop_project(int idx, int total)
{
    char src[96];
    char dst[96];

    chatlog_drop_slot(idx, total);

    snprintf(dst, sizeof dst, "sdmc:/3ds/aiconsole/chat_conv_p%d.txt", idx);
    remove(dst);
    for (int k = idx; k + 1 < total; ++k) {
        snprintf(src, sizeof src, "sdmc:/3ds/aiconsole/chat_conv_p%d.txt", k + 1);
        snprintf(dst, sizeof dst, "sdmc:/3ds/aiconsole/chat_conv_p%d.txt", k);
        if (rename(src, dst) != 0) {
            remove(dst);
            rename(src, dst);
        }
    }

    app_note("chat: drop slot p%d (total was %d)", idx, total);
    chat_reload_project();
}

static void chat_enter(void)
{
    boot_mark("chat_enter", "begin");
    /* 滑轨回到"聊天"格，并把项目表/沙箱文件读一遍 ——
     * 用户可能刚在电脑上往 out/ 里放了东西。 */
    g_tab = 0;
    dockui_init();
    /* ★ 键盘格编辑的**就是**这条 g_input（2026-10-24）。
     *
     *   不绑的后果不是"不好用"，而是"改了看不见的东西"：键盘格有它自己的
     *   g_text，对话屏有 g_input，两边互不相识。于是那里打的字在上屏
     *   看得见、发出去却还是空的，而"送到对话"只能写一句"按 A 进对话继续"。
     *   绑上之后两处是**同一个缓冲**，不存在"同步"这一步，也就不会不同步。
     *
     *   解绑放在 leave（见 chat_leave）：本屏退出后那块内存在别处可能已经
     *   不是输入框了，留着指针就是悬空的。 */
    dockui_bind_text(g_input, sizeof g_input);
    /* ★ 先定"这次是哪一份历史"，再读它（2026-10-24）。
     *   顺序反了就会读到上一个任务的记录 —— 而读出来的东西看起来完全
     *   正常（就是那个任务聊过的话），没有任何报错，只能靠"上屏没换"
     *   这种主观感受发现。 */
    {
        char slot[16];
        proj_slot(slot, sizeof slot);
        chatlog_set_slot(slot);
    }
    g_scroll = 0;
    g_pending = false;
    g_pending_slot = -1;
    g_live_slot = -1;
    g_live_len = 0;
    g_live_count = 0;
    g_live_trace = 0;
    g_live_snap = 0;
    g_qp_last = -1;         /* R 改的是"这次进屏刚填的那条" */

    /* 语音：进屏复位 + 现读配置。
     * micInit 内部有 inited 守卫，重复调用是安全的（不像 swkbd 那样
     * 进一次要退一次）。asr.txt 不在就用编译期默认那家，不是错误。 */
    g_rec = false;
    g_zl_prev = false;
    g_touch_arm = false;
    g_asr_armed = false;
    voice_init();
    asr_load_file(ASR_CFG_PATH);
    /* ★ 2026-10-23：原先这里**无条件** app_status_set(S0104)，
     *   而 S0104 是「麦克风不可用 · 诊断见 status.txt」——
     *   于是每次进对话屏，状态行都会摆着一句"麦克风不可用"，
     *   哪怕麦克风好得很。用户看到这行只会得出"语音坏了"的结论。 */
    if (!voice_available()) {
        app_note("voice: mic unavailable rc=%d", voice_mic_rc());
        app_status_set(S(S0104));   /* 只有**真的**不可用才说这句话 */
    }

    /* 每次进屏重新读一遍短语文件：用户可能刚在电脑上改过它，
     * 而对话屏是它唯一被用到的地方。 */
    phrases_load();

    /* Key 每次进屏都现读：用户随时可能在管理屏切换，或在电脑上改文件。
     * 来源优先级：仓库激活条目 > key.txt（老习惯兼容） > 无。 */
    const KeyEntry *ks = keystore_active();
    if (ks && ks->key[0]) {
        snprintf(g_key, sizeof g_key, "%s", ks->key);
        snprintf(g_provider, sizeof g_provider, "%s", ks->provider);
        snprintf(g_model, sizeof g_model, "%s", ks->model);
        g_have_key = true;
    } else {
        CredFile cf;
        g_have_key = credfile_scan(&cf);
        snprintf(g_key, sizeof g_key, "%s", cf.key);
        snprintf(g_provider, sizeof g_provider, "%s", cf.provider);
        snprintf(g_model, sizeof g_model, "%s", cf.model);
    }

    /* 这里**曾经**有一道守卫：钥匙来源的 provider 与 config.ai.provider 不一致时
     * 就把这把钥匙作废，理由是"用户在服务目录选的那家才算数"。
     *
     * 它确实修好了「服务目录选了豆包却还在用 DeepSeek」——
     * 但那是**用错了方法**。真正的修法在 sc_providers.c 的 do_pick()：
     * 服务目录切换时会同步 Key 仓库（切到有条目��家就切激活，没有就清激活），
     * 所以走 UI 切完两边本来就一致，守卫没有存在的必要。
     *
     * 而它带来的**回归**是实打实的（2026-10-05 全量回归 R/M/N/S 四趟红）：
     * 验收夹具 stage_demo() 正是靠写 keys.json（provider=demo）来切离线演示的，
     * 不动 config —— 于是被守卫作废，对话屏真出网，
     * demo 的内置剧本（含 pkg.install 动作）根本没跑，解析不出动作。
     * 真实用户里"在 Key 屏直接切 provider"这条用法也会被同样打断。
     *
     * 教训：**一个守卫不能只验它要修的那条路通不通，还得验它没堵住别的路。**
     * 当时只验了"豆包能被选中"，没跑全量回归，于是代价是四个趟次一起红。 */

    if (g_count == 0) {
        /* 先看卡上有没有存过对话。有就恢复 —— 关掉再开还能接着聊，
         * 这是"多轮上下文"真正有价值的前提：上下文得能活过重启。 */
        int restored = chatlog_load(hist_cb, NULL);
        int oversize = chatlog_oversize_count();

        if (restored > 0) {
            app_note("chat: restored=%d oversize=%d", restored, oversize);
            /* 正文恢复了，服务端会话 id 也得跟着回来 ——
             * 否则"接着聊"只是看着像，豆包那边其实已经在新会话里失忆了。 */
            load_conv_id();
            app_note("chat: conv_id=%s", g_conv_id[0] ? g_conv_id : "(空，下一句新建)");
            /* 有被截短的条目就直说。不说的话，用户看到的就是一条
             * "话说到一半就没了"的记录，还以为是自己当初就那么说的。
             * 缓冲按这条文案的全角宽度 + 余量给：给小了编译器会
             * 报 -Wformat-truncation（它按最坏情况算 %d 要 11 字符）。 */
            if (oversize > 0) {
                char warn[128];
                snprintf(warn, sizeof warn,
                         S(S0145), oversize);
                push_msg(MSG_SYS, warn, false);
            }
        } else if (chat_offline()) {
                            push_msg(MSG_SYS, S(S0146),
                     false);
        } else if (chat_ready()) {
            push_msg(MSG_SYS, S(S0147),
                     false);
        } else if (chat_provider()->fmt == PROVIDER_FMT_DOUBAO) {
            /* 豆包缺的是**登录态**，不是 Key。提示必须指向"去哪登录"——
             * 照搬下面那句"还没有 API Key"的话，用户会去找一个
             * 豆包根本不存在的 Key，而且怎么找都找不到。 */
                            push_msg(MSG_SYS, S(S0148),
                     false);
        } else {
                            push_msg(MSG_SYS, S(S0149),
                     false);
        }
    }

    /* ★ 动作结果进对话列表（**不进上下文**，in_ctx=false）：
     *
     * 为什么要有这一条：结果原来只出现在确认屏上，而那块屏一闪就没了 ——
     * 过一会儿用户再看对话，屏幕上只有"AI 说要装"和"我好像点了什么"，
     * 到底装没装、被没被拒，一点痕迹都没有。
     * 这也是"各个任务在干什么"最直白的那一层：列表里有一条**专门的**
     * 记录，颜色和徽章都和对话内容不一样。
     *
     * ⚠️ 拿走即清（take + clear）：不然下回进对话屏会再追加一次同样的。 */
    {
        const char *ev = app_act_event_take();

        if (ev) {
            push_msg(MSG_ACTION, ev, false);
            app_act_event_clear();
        }
    }

    /* 状态行要同时交代"用哪家"和"凭据状态"。离线演示时把它明说出来 ——
     * 这是唯一能解释"为什么没 Key 也能发"的地方。
     * 豆包那支一样要分开：它的凭据是登录态，写"缺 Key"是错的信息。 */
    if (chat_offline())
        app_status_set(S(S0150));
    else if (chat_provider()->fmt == PROVIDER_FMT_DOUBAO)
        app_status_set("%s · %s", chat_provider()->name,
                       chat_ready() ? S(S0151) : S(S0152));
    else
        app_status_set("%s · %s", chat_provider()->name,
                       g_have_key ? S(S0153) : S(S0154));
    boot_mark("chat_enter", "done provider=%s key=%d",
              chat_provider()->name, (int)g_have_key);

    /* 弹窗不留到下一次进来：上个会话开着环的弹窗就退出，
     * 再进来时屏幕会被一个不属于当前状态的弹窗盖住。 */
    g_ctx_open = false;
}

/* --------------------------------------------------------------------------
 * 上下文用量环 + 点它的弹窗
 *
 * 环回答一个问题："这一次发出去，会带多少字节上下文、离 4KB 上限多远"。
 * 而且必须在**发送前**就画得出来（用户打字时就想看），所以只能是预估。
 *
 * 预估能和实际对上的原因：前缀走 chatapi 的 build_prefix，历史挑选走
 * collect_turns / ctx_pick_start，system 消息走 build_sysprompt ——
 * 全是 do_real_send / build_request **正在用的同一份代码**，不是照着抄
 * 一遍公式。抄一遍的后果是"预估绿、实际红"而用户看不出来。
 *
 * ⚠️ 已知缺陷（2026-10-18 定位，未修）：AGENT.md 手册整份（约 21.7KB）
 *    会拼进 system_prompt，而 req_body 只有 4KB。于是 used 一开始就超
 *    预算、历史永远 0 条、请求体在 ~4095B 处被静默截断。修好之前，
 *    这个环把超标**如实显示成满环 + 弹窗报警** —— 不粉饰，也不静默。
 *    三个修法方向记在 shared/chatapi.h「关于请求体装不下这件事」。
 * ----------------------------------------------------------------------- */

/* 环可不可画：只有真走 chatapi 请求体的服务商才画。
 * 离线演示（本地剧本）与豆包（自己的 samantha 接口）都不经过
 * build_request —— 对它们而言"上下文用量"这个概念不存在，
 * 画一个恒为 0 的环是在演示用户面前撒谎。 */
static bool ctx_ring_visible(void)
{
    const ProviderDef *p = chat_provider();

    return !chat_offline() && p->fmt != PROVIDER_FMT_DOUBAO;
}

/* 估算节流：150ms 才算一次。估一次要拼 22KB 的 system 消息、再对全部
 * 历史跑一遍挑选 —— 每帧算会把流式上屏的帧率再砍一刀（那条路径本来就
 * 紧张，M6 为此已经专门做了折行缓存）。环是个慢变量：一次发送之内它的
 * 变化本来就看不出来，0.15 秒的滞后换来每帧零开销，划算。 */
#define CTX_RING_REFRESH_MS 150

static ChatCtxEst g_ring_est;
static u64       g_ring_at;      /* 上一次重算的时刻（ms，osGetTime） */

static void ctx_ring_refresh(void)
{
    /* 两个静态区都与发送路径的那两份**分开**：渲染可能在流式回调里发生，
     * 共用一个缓冲的话，"发送途中被界面改掉"这种事就有了入口。 */
    static char sp[CONFIG_MAX_PROMPT + CONFIG_MAX_MEMORY
                   + AGENTDOC_MAX + ACT_RESULT_MAX + 160];
    static ChatTurn turns[CHAT_CTX_TURNS];
    const char *adoc = NULL;
    u64 now = osGetTime();
    int  n, keep, from;

    if (now - g_ring_at < CTX_RING_REFRESH_MS)
        return;
    g_ring_at = now;

    /* consume=false：只读，**不擦**动作结果。
     * 真消费的话，环每 150ms 就把上一轮的动作结果擦掉一次，
     * 等真发送时模型反而收不到 —— 而这种 bug 在屏上完全看不出来。 */
    build_sysprompt(sp, sizeof sp, &adoc, false);

    /* 历史按"当前输入框"估：user_text 取 g_input，对应真发送时那句
     * g_pending_text（翻译模式的前缀在发送前一刻才拼，环用原文已经
     * 够说明问题，且不会因此和实际差出一个量级）。 */
    n = collect_turns(g_count, turns);

    /* 上下文轮数上限：与 do_real_send 同一个字段、同一种裁法 */
    keep = app_cfg()->ai.context_turns;
    from = (n > keep) ? (n - keep) : 0;

    chat_ctx_estimate(g_model[0] ? g_model : NULL,
                      reply_max_tokens(),
                      sp, turns + from, n - from,
                      g_input, &g_ring_est);
}

/* 环形填充，**不用三角函数逐像素算**（每环只算一次 cosf/sinf）。
 *
 * 推导（屏幕坐标：x 右、y 下；12 点方向 = (0,-1)，顺时针扫）：
 *   已用区 = 从 12 点顺时针转 2πf 的扇形。设 θ = 2πf，
 *   终点方向 T = (sinθ, -cosθ)，起点 B = (0,-1)，E = cosθ·dx + sinθ·dy。
 *     · f ≤ 0.5（θ ≤ π）：扇形是 B、T 张成的**凸锥**，点 p 在锥内
 *       ⟺ p 在 B 的顺时针侧（dx ≥ 0）且在 T 的逆时针侧（E ≤ 0）。
 *     · f > 0.5：已用区是"大半圆"，其补集（未用）才是小锥，
 *       ⟺ filled ⟺ !(dx ≤ 0 && E ≥ 0)。
 *     · f ≤ 0 / f ≥ 1 必须特判：θ=0 时 T 与 B 重合，锥没有面积，
 *       公式会退化成一条线，把整环分错类。
 * 四种比例（0/0.25/0.5/0.75）已离线逐点验证过；已知的 1px 瑕疵：
 * f > 0.5 时正 12 点方向那个边界像素会被判成"未用"（扇形起点那条射线
 * 上的点叉积为 0，落在 < 一侧）。一个像素，装饰性的，认了。 */
static void ctx_ring_draw(float frac)
{
    const float cx   = CTX_RING_CX;
    const float cy   = CTX_RING_CY;
    const float ro   = CTX_RING_R_OUT;
    const float ri   = CTX_RING_R_IN;
    const float ro2  = ro * ro;
    const float ri2  = ri * ri;
    /* 未用 = 浅；已用 = **更强的那一档**。白天用 ACC_DK（深青）满足
     * "用过的更深"；黑夜底下再谈"更深"是看不见的，那一侧的等价物是
     * 更亮更饱和，所以用 ACCENT。两个主题下"已用"都从背景里跳出来。 */
    const RColor track = w_c(C_SLOT_ACC_LT);
    const RColor used  = (w_theme_current() == W_THEME_DARK)
                         ? w_c(C_SLOT_ACCENT) : w_c(C_SLOT_ACC_DK);
    float theta = 0.0f, ct = 1.0f, st = 0.0f;
    int   x, y;

    if (frac < 0.0f) frac = 0.0f;
    if (frac > 1.0f) frac = 1.0f;

    if (frac > 0.0f && frac < 1.0f) {
        theta = 6.2831853f * frac;
        ct    = cosf(theta);
        st    = sinf(theta);
    }

    for (y = (int)(cy - ro); y <= (int)(cy + ro); ++y) {
        float dy = (float)y + 0.5f - cy;      /* 像素中心，别用角 */
        for (x = (int)(cx - ro); x <= (int)(cx + ro); ++x) {
            float dx = (float)x + 0.5f - cx;
            float d2 = dx * dx + dy * dy;
            float e;
            bool  on;

            if (d2 < ri2 || d2 > ro2)
                continue;                     /* 环带之外，不画 */
            e = ct * dx + st * dy;
            if (frac <= 0.0f)
                on = false;
            else if (frac >= 1.0f)
                on = true;
            else if (frac <= 0.5f)
                on = (dx >= 0.0f) && (e <= 0.0f);
            else
                on = !(dx <= 0.0f && e >= 0.0f);
            r_pixel((float)x + 0.5f, (float)y + 0.5f, on ? used : track);
        }
    }
}

/* 环上的百分比。预算见 chat_ctx_budget()（= REQ_BUDGET，请求体上限）。
 * 返回 0..100 的**整数**，够画环也够弹窗里那句"%d%%"。 */
static int ctx_ring_pct(void)
{
    int budget = chat_ctx_budget();
    int used   = g_ring_est.total;

    if (budget <= 0)
        return 0;
    if (used >= budget)
        return 100;
    return used * 100 / budget;
}

/* 弹窗：两块屏的内容区整块盖住（密码门弹窗同一形状，理由见
 * sc_settings.c 的 draw_konami_top：320×240 上留边的对话框会露出
 * 被盖住内容的半列字，整块盖住才干净）。
 *
 * 内容分两屏：上屏讲"这是什么 + 预估值"，下屏放数字与"上一条实际
 * 发了多少"—— 预估与事实并排给，用户自己看得出对不对得上。 */
static void ctx_popup_top(void)
{
    const Rect box = { 0, 26, R_TOP_W, 172, 0 };
    char        line[64];
    int         pct = ctx_ring_pct();

    w_panel(&box, w_c(C_SLOT_BG), w_c(C_SLOT_ACCENT));

    w_center(200.0f, 44.0f, S(S0901), w_c(C_SLOT_ACC_DK));

    snprintf(line, sizeof line, S(S0902),
             g_ring_est.total, chat_ctx_budget(), pct);
    w_center(200.0f, 70.0f, line, w_c(C_SLOT_FG));

    snprintf(line, sizeof line, S(S0903),
             g_ring_est.sys_bytes, g_ring_est.n_turns, g_ring_est.user_bytes);
    w_center(200.0f, 92.0f, line, w_c(C_SLOT_MUTED));

    /* 触到上限要说出来：那条请求的内容被截断了（见 chatapi.h 的已知缺陷），
     * 而这正是用户点开这个弹窗最该知道的事。 */
    if (chat_last_request_capped())
        w_center(200.0f, 118.0f, S(S0908), w_c(C_SLOT_WARN_DK));

    w_center(200.0f, 146.0f, S(S0906), w_c(C_SLOT_MUTED));
    w_center(200.0f, 166.0f, S(S0907), w_c(C_SLOT_FAINT));
}

static void ctx_popup_bottom(void)
{
    const Rect box = { 0, 26, R_BOT_W, 182, 0 };
    char        line[64];

    w_panel(&box, w_c(C_SLOT_BG), w_c(C_SLOT_ACCENT));

    w_center((float)R_BOT_W / 2.0f, 44.0f, S(S0901), w_c(C_SLOT_ACC_DK));

    /* 上一条**实际**发出去的字节数。0 = 还没发过（演示与豆包不走
     * build_request，永远是这个值，所以那句文案要分开）。 */
    if (chat_last_request_len() > 0) {
        snprintf(line, sizeof line, S(S0904), chat_last_request_len());
        w_center((float)R_BOT_W / 2.0f, 70.0f, line, w_c(C_SLOT_FG));
    } else {
        w_center((float)R_BOT_W / 2.0f, 70.0f, S(S0905), w_c(C_SLOT_MUTED));
    }

    w_wrap(42.0f, 96.0f, 236.0f, S(S0906), w_c(C_SLOT_MUTED), TEXT_LINE_H);

    w_button(&CTX_POP_BTN, S(S0233),
             W_BTN_PRIMARY | (app_touch_on(&CTX_POP_BTN) ? W_BTN_PRESSED : 0));

    w_center((float)R_BOT_W / 2.0f, 192.0f, S(S0234), w_c(C_SLOT_FAINT));
}

/* 「思考强度」旋钮：四档循环（关→低→中→高→关）。
 * 从设置屏搬过来的那个控件（R_ROW_THINK 已删），行为照抄它：
 *   · 改完**立刻落盘**：这一项决定下次请求往接口里塞什么参数，
 *     不落盘的话用户退出再进来又回到默认，会以为没生效。
 *   · 不做"这家不支持就拒绝改"：档位存在 config 里，切到支持思考的
 *     服务商自然就生效（按钮在不支持时不画，但值不被锁）。
 * 按钮可见性见 CHAT_BTN_TLVL 上方注释（provider_think_capable()）。 */
static void cycle_think_level(int delta)
{
    AppConfig *c = app_cfg();
    int want = c->ai.thinking + (delta > 0 ? 1 : -1);

    if (want < CONFIG_THINK_OFF)  want = CONFIG_THINK_HIGH;
    if (want > CONFIG_THINK_HIGH) want = CONFIG_THINK_OFF;
    c->ai.thinking = want;
    app_cfg_save_now();
    app_status_set(S(S0581), thinking_name(want));
}

static void render_top(void)
{
    const float x = (float)W_TOP_MARGIN;
    const float w = (float)W_TOP_USABLE;
    float y = (float)W_TOP_CONTENT_Y;
    char right[40];
    const ProviderDef *p = chat_provider();

    /* 右上角写服务商名。离线演示时不写 "demo" —— 那是内部标识，用户看不懂，
     * 直接写明"离线演示"。这个状态必须一眼可见，否则用户会以为真连上了。 */
    snprintf(right, sizeof right, "%s", chat_offline() ? S(S0155) : p->name);
    w_top_header(g_translate ? S(S0893) : S(S0156), right);

    float bottom = (float)W_TOP_CONTENT_B - (float)TEXT_LINE_H;

    /* ★ 拼前缀用的缓冲必须跟 AI 槽位一样大 —— 曾经留成旧的 150 字节，
     * 长回复在这被截断、还腰斩 UTF-8 字符，屏上就是个断尾巴的半字。
     * 现在拼串的事整体挪进了 msg_layout()，那份缓冲也就跟着搬过去了。 */
    for (int i = g_scroll; i < g_count && y <= bottom; ++i)
        msg_layout(i, x, y, w, true, &y);

    /* 只有真的有更早消息被卷出屏幕顶时才提示 —— 底部锚定后
     * g_scroll==0 就是全部可见，旧条件（g_scroll+1<g_count）会平白撒谎 */
    if (g_scroll > 0)
        w_line(x, (float)W_TOP_CONTENT_B - 16, w, S(S0157), w_c(C_SLOT_FAINT));

    /* 上屏最下端 = 状态行（成功/失败/正在请求都在这里）。
     * 第二个参数曾经是「上屏读对话 · 下屏写你要说的」，已删 ——
     * 那是给开发者看的结构说明，不是状态，每帧还占着位置。
     * 现在放的是**发现的提示**：环可见时才写"点环看上下文用量"。
     * 不写这一句，用户永远不知道那个环可以点；而离线演示与豆包
     * 不画环（ctx_ring_visible），那句对它们也不出现 ——
     * 提示一个不存在的交互比不提示更糟。 */
    w_top_footer(app_status(), ctx_ring_visible() ? S(S0907) : "");

    /* 弹窗开着：上屏内容整块盖住（面板 {0,26,400,172}，形状理由见
     * ctx_popup_top 上方注释）。正常内容**照画** —— 盖住它是弹窗的
     * 事，而"盖不住"会立刻看出来；反过来"漏画了正常内容"要等关掉
     * 弹窗才暴露，那种错更难抓。 */
    if (g_ctx_open)
        ctx_popup_top();
}

/* ------------------------------------------------------------------ 下屏滑轨
 *
 * ★ 为什么滑轨在**对话页**而不是独立屏：
 *   聊天时想换模型、看产出文件、打中文，都不该先退出对话再进一个"工作台"。
 *   所以滑轨常驻对话页下屏，6 格：选项 / 键盘 / 项目 / 文件 / 浏览 / 状态。
 *
 *   ★ 2026-10-25 加第 6 格「状态」：帧率、主循环占用、内存的实时图。
 *     这些数字平时藏起来，但"为什么卡"这个问题必须有个地方能当场看。
 *     ⚠️ 这几格标签是**硬编码中文**（历史遗留，见下方注释）——
 *        44px 的格子放不下英文单词，改成 S() 会在英文界面下溢出。
 *   第 0 格是聊天本身（输入框与发送按钮），1~4 交给 dock_ui 组件。
 *
 * 几何（与 dock_ui.h 的约定对齐，别自己另算一套）：
 *   按钮行  y 124..160
 *   滑轨    y 168..198   （5 格 × 56 + 4 间隙 × 4 = 296）
 *   提示    y 202
 *   页脚    y 214..240
 */
#define CHAT_RAIL_N    6
#define CHAT_RAIL_Y   24.0f
#define CHAT_RAIL_H   18.0f
#define CHAT_RAIL_W   44.0f
#define CHAT_RAIL_GAP  2.0f

/* ★ 2026-10-23 用户要求：「把聊天那一页改成选项，加入一些可以及时控制的选项」。
 *
 *   第 0 格原来的作用只是"收起面板"（回到纯输入框视图），
 *   而那块地皮正是对话时最想快速改设置的地方 —— 现在给它内容：
 *   思考强度 / 上下文轮数 / 回复长度 / 回答语言，改完立刻生效。
 *
 *   ⚠️ 输入与发送现在只走「键盘」格。第 0 格不再画输入框，因为
 *      4 行选项 + 输入框 + 发送按钮在 320×240 上挤不下（算过：
 *      滑轨占到 42，剩下 198px 只够 4×26 + 一行提示）。 */
static const char *const CHAT_RAIL_LABEL[CHAT_RAIL_N] = {
    "选项", "键盘", "项目", "文件", "浏览", "状态",
};

static Rect chat_rail_rect(int i)
{
    Rect r;
    /* 右对齐到可用右边界（x=310）：5 格 ×44 + 4 间隙 ×2 = 228 */
    r.x = (float)(R_BOT_W - W_BOT_MARGIN)
          - (CHAT_RAIL_N * CHAT_RAIL_W + (CHAT_RAIL_N - 1) * CHAT_RAIL_GAP)
          + (float)i * (CHAT_RAIL_W + CHAT_RAIL_GAP);
    r.y = CHAT_RAIL_Y;
    r.w = CHAT_RAIL_W;
    r.h = CHAT_RAIL_H;
    return r;
}

static void chat_rail_draw(void)
{
    for (int i = 0; i < CHAT_RAIL_N; ++i) {
        Rect r = chat_rail_rect(i);
        w_button(&r, CHAT_RAIL_LABEL[i],
                 (i == g_tab ? W_BTN_SELECTED : W_BTN_NORMAL)
                 | (app_touch_on(&r) ? W_BTN_PRESSED : 0));
    }
}

/* 语音开关的绘制。**只在键盘格调用**（见 render_bottom 的 g_tab == 1）。
 *
 * 位置沿用页眉右侧那块（CHAT_BTN_MIC 就是按这个位置算的）——
 * 键盘格页眉写的是「键盘」，标题占 x10..40，右侧 236..310 本来就空着。
 * 换成键盘格以后它离"拼音/英文键盘/清空/送到对话"这四个输入相关的键
 * 更近，语义上是**同一组**：都是"怎么把字弄进输入框"。 */
static void chat_voice_button_draw(void)
{
    char mic_lbl[24];
    int  mic_flags = W_BTN_NORMAL;

    if (!voice_available()) {
        snprintf(mic_lbl, sizeof mic_lbl, S(S0163));
        mic_flags = W_BTN_DISABLED;
    } else if (g_asr_armed) {
        snprintf(mic_lbl, sizeof mic_lbl, "%s", S(S0164));
        mic_flags = W_BTN_PRIMARY;
    } else if (g_rec) {
        int ms = voice_ms();
        /* 按钮上只给整秒。旧格式「录 %d.%ds」实测 69px，而按钮
         * 可用宽只有 70-6=64 —— 把标签推过界的正是十分位那一对数字。
         * 0.1 秒的精度没有丢：状态行 S0166 仍是 %d.%d 格式。 */
        snprintf(mic_lbl, sizeof mic_lbl, S(S0165), ms / 1000);
        mic_flags = W_BTN_PRIMARY;
        /* 每帧刷新：状态行那句（含音量条）是"麦克风到底有没有进声"的
         * 即时回显。时长涨不等于有声音 —— 全静音的时候它照样在涨。 */
        app_status_set(S(S0166), voice_bar(voice_rms()), ms / 10, ms % 10);
    } else {
        snprintf(mic_lbl, sizeof mic_lbl, "%s", S(S0167));
    }
    if (app_touch_on(&CHAT_BTN_MIC))
        mic_flags |= W_BTN_PRESSED;
    w_button(&CHAT_BTN_MIC, mic_lbl, mic_flags);
}

/* 换下一条常用短语，填进输入框。
 *
 * ★ 2026-10-24 抽成函数并在**每个格子**都接上（见 chat_handle）。
 *   原先它只挂在第 0 格那条路上，而发送挪到键盘格之后，用户想"用短语发一条"
 *   就得：键盘格 → 左回选项格 → SELECT → 右去键盘格 → Y。
 *   绕三下才能发一句现成的话，比手打还慢 —— 那就等于把短语功能废了。
 *   短语填的是**同一条**输入框（键盘格也在编辑它），所以在哪一格按都一样。
 *   唯一让位的是项目格：那里的 SELECT 是"删掉这个任务"，不能抢。 */
static void chat_fill_phrase(void)
{
    int n = phrases_count();

    if (n <= 0) {
        app_status_set(S(S0173));
        return;
    }
    /* 列表长度可能变小（用户按 R 删掉了一条），所以每次都夹一次 ——
     * 只靠 %n 也能防越界，但那样删到只剩一条时会停在"永远是第 0 条"，
     * 再按 SELECT 换不动，看着像卡了。夹一次就回到轮换的语义。 */
    if (g_qp < 0 || g_qp >= n)
        g_qp = 0;

    g_qp_last = g_qp;
    snprintf(g_input, CHAT_TEXT, "%s", phrases_at(g_qp_last));
    g_qp = (g_qp_last + 1) % n;
    app_status_set(S(S0174));
}

/* ★ 最下行操作指引（2026-10-23 用户要求：「最下行给出操作指引」）。
 *
 *   为什么抽成函数：这段逻辑**两处要用** —— 输入框视图（g_tab==0 的老路）
 *   与面板格（g_tab>0）。抽之前它只挂在输入框那条路上，而那条路现在
 *   走不到了（第 0 格由选项面板接管、其余格由面板接管），结果面板格的
 *   页脚**根本没人画**：看到的是上一帧残留的像素。
 *
 *   这种"看着有内容、其实没人画"的现象在截图里完全正常 —— 它一直是
 *   旧的那一行（S0170，265px），直到有人去改它才会暴露。所以验收里
 *   "页脚会变"这条判据必须**在面板格上测**，光看第 0 格会永远通过。
 *
 *   原来的底部栏是**一句固定文案**（[A] 发送 [Y] 键盘 [SELECT] 短语
 *   [ZL] 说话），而那时键位是另一套；现在十字键接管了"下屏"、摇杆接管了
 *   "上屏滚动"，再挂着一行不完整的旧键位只会误导。所以改成**轮播**：
 *   一次说一条，几秒自动换下一条，循环。
 *
 *   为什么必须轮播而不是排成一行：底屏可用宽 300px = 18 个全角字，
 *   而"十字键切格 / 上下选行 / 摇杆滚上屏 / A 发送 / Y 键盘 / ZL 语音 /
 *   SELECT 短语 / B 返回 / X 配 Key"这一整套，最短写法也要 325px
 *   —— 硬排一行的结果是右边缘裁掉半个字，而裁掉的恰好是最后一句话的
 *   结尾，看起来像程序没写完整。
 *
 *   顺序是**按用到的频率**排的：先讲方向键与摇杆（每次都要用），
 *   再讲发送/键盘/语音，最后才是 B 与 X 这类偶尔用的。
 *
 *   ★★ 2026-10-24：**发送这一条按格子分开讲**。
 *   用户定的分工是「A 在选项格=改值、发送放在键盘格」，于是
 *   "A 发送"这句话在哪个格都不对了：
 *     · 键盘格：A = 改文字，Y = 发送
 *     · 选项格：A = 改值（它自己的页脚 S0963 已经这么写了）
 *     · 项目/文件/浏览：A 是"打开/应用/进入"，Y 是刷新/新建
 *   所以键盘格单独一套（含发送那条），其余格用一套**处处成立**的。
 *   把发送那条写进公共那套，就等于在四个格里有三个说谎 ——
 *   而"屏上写着但按不出来"比不写更坏。
 *
 *   没配 Key 时不提发送 —— 按了也发不出去，先把人引到配 Key 上。
 *   豆包那支也不提 Key：它的凭据是登录态，写"配 API Key"会让人去找
 *   一个豆包根本没有的东西。这两条与轮播是**并列**的替换关系。
 */
static void chat_footer_hint(void)
{
    /* 状态页没有"行"可选，那些键位提示在这里全是错的
     * （[↑↓] 选这一格的行 —— 这一格根本没有行）。
     * 宁可空着，也不要指一条走不通的路。 */
    if (g_tab == 5)
        return;

    if (chat_ready()) {
        /* ★ 为什么是一张**函数内静态**表 + 懒加载，而不是文件级 const 数组：
         *   S() 展开成 uistr(id)（运行时查表），不是编译期常量，
         *   放不进静态初始化器。而每次渲染都重建一遍数组又太浪费
         *   （w_bot_footer_roll 靠**指针身份**判断"换没换组"，数组
         *   每帧换个地址会让轮播永远停在第 0 条）。
         *   所以这里只初始化一次，地址恒定。
         *
         * ⚠️ 两张表的**地址必须不同**（本就是两个数组），否则轮播会把
         *   "换了一组"误判成"同一组"，序号不归零。 */
        static const char *HINT_KBD[7];     /* 键盘格：有发送这一条 */
        static const char *HINT_GEN[6];     /* 其余格：处处成立的六条 */
        static bool hint_init;
        if (!hint_init) {
            hint_init = true;
            /* 键盘格：顺序按**用到的频率**，发送放第一条 ——
             * 进了这一格，最想干的通常就是"把这段字发出去"。 */
            HINT_KBD[0] = S(S0958);     /* Y 发送 · A 改文字 */
            HINT_KBD[1] = S(S0956);     /* ←→ 切格 · ↑↓ 选行 */
            HINT_KBD[2] = S(S0957);     /* 摇杆滚上屏 */
            HINT_KBD[3] = S(S0959);     /* ZL 说话 */
            HINT_KBD[4] = S(S0960);     /* SELECT 换短语 */
            HINT_KBD[5] = S(S0961);     /* B 返回 · X 配 Key */
            HINT_KBD[6] = S(S0962);     /* L/R 也能切格 */

            /* 其余格：不提 A/Y —— 它们每一格的语义都不一样，
             * 由各自的格子自己讲（选项格有自己的 S0963）。 */
            HINT_GEN[0] = S(S0956);
            HINT_GEN[1] = S(S0957);
            HINT_GEN[2] = S(S0959);
            HINT_GEN[3] = S(S0960);
            HINT_GEN[4] = S(S0961);
            HINT_GEN[5] = S(S0962);
        }
        if (g_tab == 1)
            w_bot_footer_roll(HINT_KBD, 7, 3200);
        else
            w_bot_footer_roll(HINT_GEN, 6, 3200);
    } else {
        /* 没配 Key（或豆包）：只给一条，不轮播 —— 这时候"怎么聊"不是问题，
         * "怎么连上"才是，多讲键位反而把那一句关键指引淹掉。 */
        w_bot_footer(chat_provider()->fmt == PROVIDER_FMT_DOUBAO
                         ? S(S0171) : S(S0172));
    }
}

static void render_bottom(void)
{
    /* 拼音键盘展开 → 整屏让给它 */
    if (dockui_ime_active()) {
        dockui_render_ime();
        return;
    }
    /* ★ 第 0 格现在是「选项」：画四项即时控制 + 滑轨。
     *   这条路必须在"g_tab > 0"之前判，否则 g_tab==0 会掉到下面的
     *   输入框视图去 —— 而输入框视图已经不再承担第 0 格的内容了。 */
    if (g_tab == 0) {
        w_bot_header(S(S0948));      /* 「选项」 */
        dockui_render_opt();
        chat_rail_draw();            /* 滑轨本身最后画，压在最上面 */
        return;
    }

    /* 选的是滑轨上某一格 → 页眉 + 面板 + 滑轨 */
    if (g_tab > 0) {
        /* ★★ 面板格也要顶上那条页眉（2026-10-24 用户要求：
         *   「键盘/项目/文件 那些格子，顶上都要有一个蓝色条子，和选项一样」）。
         *
         *   原来只有第 0 格调了 w_bot_header，切到面板格时那条**当场消失**，
         *   下屏顶部变成一块裸背景 —— 看起来像"界面少画了一块"，
         *   而且面板内容自己不写名字（键盘格第一个字就是输入框），
         *   用户只能靠滑轨上那个小标签判断自己在哪一格。
         *
         *   位置：0..24（W_BOT_HEADER_H），滑轨 24..42，面板内容 54 起 ——
         *   正好把原来空着的那 24px 填上，不会挤到任何已有元素。
         *   键盘格右上角的语音按钮就画在 y=3、高 19，本来也是压在页眉这一条上的，
         *   和输入框视图的表现一致。 */
        w_bot_header(dockui_name(g_tab - 1));
        dockui_render(g_tab - 1);
        /* ★ 语音开关**只在键盘格出现**（g_tab == 1 → 面板 0 = 键盘）。
         *   用户原话："键盘页面才会出现语音开关，剩下页面都不出现"。
         *   理由也说得通：语音是**输入手段**，跟打字是一类的；
         *   项目/文件/浏览那几格是"看内容/改设置"，把麦克风放上去
         *   就是一块跟上下文无关的按钮，只会分散注意力。
         *   画在这里（而不是 dock 内部）是因为语音状态全在本文件的
         *   static 里，跨文件导出要连带把 g_rec / g_asr_armed 一起暴露出去。 */
        if (g_tab == 1)
            chat_voice_button_draw();
        chat_rail_draw();
        chat_footer_hint();
        return;
    }

    const float x = (float)W_BOT_MARGIN;
    const float w = (float)W_BOT_USABLE;
    /* 环占着输入区右上角（x281..311），折行宽收到 268 —— 少 32px
     * ≈ 两个全角字，换来环不压字。环不画时（离线演示/豆包）用满宽。 */
    const float in_w = ctx_ring_visible() ? CHAT_INPUT_W : w;
    const ProviderDef *p = chat_provider();
    /* 内容从 48 起：24..42 那一条是滑轨标签 */
    float y = 48.0f;

    /* 环的弹窗开着：这一屏整块归它（模态，见 g_ctx_open 的注释）。
     * 底部栏那句提示也**不画** —— 弹窗开着时按 A 是"返回"不是"发送"，
     * 留着"[A] 发送"就是指着一条走不通的路。 */
    if (g_ctx_open) {
        ctx_popup_bottom();
        return;
    }

    /* 标题用短名（S0156「对话」）：页眉右侧现在排着三个钮
     * （思考强度 / 思考 / 语音），坐标见各自上方的注释。
     * 旧标题 S0158「你要说的」是给整行输入框起的名字，如今这个页眉
     * 已经不只是输入框的标题了。 */
    w_bot_header(S(S0156));
    chat_rail_draw();

    if (g_input[0]) {
        y = w_wrap(x, y, in_w, g_input, w_c(C_SLOT_FG), TEXT_LINE_H);
    } else {
        y = w_wrap(x, y, in_w, S(S0159), w_c(C_SLOT_MUTED), TEXT_LINE_H);
    }

    y += 10;
    w_hr(x, y, in_w, w_c(C_SLOT_LINE));

    w_button(&CHAT_BTN_SEND, S(S0160),
             (chat_ready() ? W_BTN_PRIMARY : W_BTN_NORMAL) |
             (app_touch_on(&CHAT_BTN_SEND) ? W_BTN_PRESSED : 0));
    w_button(&CHAT_BTN_KBD, S(S0161),
             W_BTN_NORMAL | (app_touch_on(&CHAT_BTN_KBD) ? W_BTN_PRESSED : 0));

    /* 「思考强度」按钮（从设置屏搬来的那个旋钮，R_ROW_THINK 已删）。
     * 档位不是 0 时用主动色 —— "这一次会带思考内容"要在一屏里就看出来，
     * 不然用户得逐个点开才知道当前是哪一档。
     * 这家没有可调档位时整个不画（p->think == NONE），取舍见
     * CHAT_BTN_TLVL 上方的注释：值不锁，只是不画一个白点的键。 */
    if (provider_think_capable(p)) {
        char lbl[24];
        int  lvl = app_cfg()->ai.thinking;

        snprintf(lbl, sizeof lbl, S(S0900), thinking_name(lvl));
        w_button(&CHAT_BTN_TLVL, lbl,
                 (lvl != CONFIG_THINK_OFF ? W_BTN_PRIMARY : W_BTN_NORMAL) |
                 (app_touch_on(&CHAT_BTN_TLVL) ? W_BTN_PRESSED : 0));
    }

    /* 「思考」按钮：**没有思考内容时不画**。
     * 画一个点了没反应的键，比没有这个键更坏 —— 用户会以为程序坏了。
     * 箭头是 ▼/▶ 而不是 ▾/▸：后两个码位不在字库里，画出来是豆腐块
     * （旧版这里一直是个空心方框，还因此占着全宽），见 CHAT_BTN_THINK
     * 上方注释里的实测口径。 */
    if (any_think()) {
        char lbl[24];
        snprintf(lbl, sizeof lbl, S(S0162), g_think_open >= 0 ? " ▼" : " ▶");
        w_button(&CHAT_BTN_THINK, lbl,
                 (g_think_open >= 0 ? W_BTN_PRIMARY : W_BTN_NORMAL) |
                 (app_touch_on(&CHAT_BTN_THINK) ? W_BTN_PRESSED : 0));
    }

    /* 语音按钮：见 CHAT_BTN_MIC 的注释（为什么不在主按钮行里）。
     * 录着的时候把时长写在按钮上 —— "到底还在录吗"这种疑问，
     * 只有"已经 4 秒了"能回答，一句"正在录音"回答不了。
     * 十分位的精度在状态行（S0166，和音量条一起每帧刷）——
     * 按钮装不下"录 12.3s"（实测 69px > 可用 64），而按钮要回答的
     * 只是"还在录吗、多久了"，整秒够。
     *
     * ★ 2026-10-22：语音按钮**从聊天格移走**，只画在键盘格
     *   （用户要求"键盘页面才会出现语音开关，剩下页面都不出现"）。
     *   见 chat_voice_button_draw()。聊天格页眉右侧现在空着 ——
     *   思考强度/思考两个钮仍在标题右边，不会因此挤在一起。 */

    /* 上下文用量环：输入区右上角那块空位（x281..311 / y29..55）。
     * 刷新挂在渲染里做（内部按 150ms 节流，见 ctx_ring_refresh）——
     * 渲染是这个环唯一的心跳；把刷新放到别处，就意味着别处也得记着
     * 调它，而"忘了调"的表现是环停在旧值上—— 看不出来。 */
    if (ctx_ring_visible()) {
        ctx_ring_refresh();
        ctx_ring_draw(ctx_ring_pct() / 100.0f);
    }

    /* 次要键单独一行。它们不能挤进底部栏 —— 那一行已经写满主操作
     * （发送/键盘/语音），再塞两个键必然超宽，而底部栏只有 26px 放不下第二行。
     * 这块空位本来就是留给它的。 */
    /* 有思考内容时补一句"[←/→] 思考" ——
     * 光有一行小字还不够，键位**写在屏上**才算告诉了人。
     * 没思考时这一句也不该出现（同按钮：不说做不到的事）。 */
    w_center((float)R_BOT_W / 2.0f, 202,
             any_think() ? S(S0168)
                         : S(S0169),
             w_c(C_SLOT_FAINT));

    chat_footer_hint();
}

/* ★ 摇杆 → 上屏滚动（2026-10-23 用户定的分工）。
 *
 *   「十字键控制下屏内容，摇杆上下控制上屏文字上下滑动」
 *
 *   为什么摇杆不用"每帧一格"：摇杆是连续量，按住不动时它每帧都给读数。
 *   按帧走格的话，稍微推一下就窜十几格，用户根本追不上看到哪。
 *   所以按"累积位移"走格：推得浅（|dy| 刚到死区外）几帧才一格，
 *   推到底则每帧一格，快慢由手决定 —— 这比加快捷键更符合直觉。
 *
 *   为什么要**清余数**：轻推一下、松手，余数若留着，下一次轻推就会
 *   多跳一格。表现是"滚动有时候会自己多跑一步"，只在慢推时出现，
 *   截图看不出来，只能靠这段注释记住。
 *
 *   为什么只在第 0 格（选项）之外生效：下屏面板（键盘/项目/文件/浏览）
 *   本身有上下选行的键位，摇杆若也抢着用，会变成"推摇杆改的是上屏、
 *   光标却在下屏乱动"。滚动只跟"看对话"有关，就该只在看对话时活着。
 *   —— 反过来说，用户在下屏翻文件时上屏滚走也是莫名其妙的行为。 */
static void chat_stick_scroll(void)
{
    int  dy    = app_circle_y();
    bool moved = (dy > STICK_DEAD) || (dy < -STICK_DEAD);

    if (!moved) {
        /* 松手（或回中）：余数清零，见上面那段"为什么清余数" */
        g_stick_acc = 0;
        g_stick_was = false;
        return;
    }

    /* 推得越深走得越快：|dy| 从死区到满推（~156）线性映射到 min..max 两步长。
     * 死区边缘 STICK_STEP 走一格需要十几帧（慢慢挪），推到底 4 帧一格 */
    int mag  = (dy > 0 ? dy : -dy);
    int step = STICK_ACC + (mag - STICK_DEAD) / 8;
    if (step < STICK_ACC) step = STICK_ACC;

    g_stick_acc += step;
    g_stick_was  = true;

    if (g_stick_acc < STICK_STEP)
        return;
    g_stick_acc = 0;

    if (dy < 0) {                        /* 上推 = 往回看更早的 */
        if (g_scroll > 0) --g_scroll;
    } else {                             /* 下推 = 往新的方向 */
        if (g_scroll + 1 < g_count) ++g_scroll;
    }
}

static void chat_handle(u32 down, u32 held)
{
    (void)held;

    /* ★★ 2026-10-23 真机反馈：「只要选进去，就按 B 退不回主页面」。
     *
     *   查下来 B 在这条链上有三个吃法，用户得按 2~3 次才出得去：
     *     ① 键盘展开  → 清空输入串（看起来只是光标没了）
     *     ② 键盘/面板展开 → g_tab 归零，"收起面板"而不是退屏
     *     ③ 都没有时才 app_pop()
     *   每一次都有视觉变化，但**变化都不像"退出"**，于是主观感受就是
     *   "按 B 没反应"。
     *
     *   现在改成一条路：**B 就是"离开这一屏"**。
     *   唯一的例外是"输入框里有字母" —— 那时先清输入，因为用户
     *   敲错拼音时最想按的就是 B，直接退屏会把整句话丢掉。
     *   清完再按一次就退，最多两次，而且第二次一定退。
     *
     *   放在函数最前面：不能有任何一个分支抢在它之前（以前就抢了）。 */
    if (down & KEY_B) {
        if (dockui_ime_active()) {
            if (ime_input()[0]) {
                ime_reset();            /* 敲错了：先清，不退 */
                return;
            }
            dockui_ime_close();         /* 键盘空着：收起并继续往下走 */
        }
        /* 录音中离开要收尾，否则采样会一路涨到 5 秒上限 */
        if (g_rec || g_asr_armed) {
            g_rec       = false;
            g_asr_armed = false;
            voice_stop();
            app_note("voice: cancelled by leaving screen");
        }
        app_pop();
        return;
    }

    /* 滑轨常驻，**最先判**：它是这一屏的"底盘"，
     * 排在输入逻辑后面的话，点滑轨会先被输入区吃掉。
     *
     * ★ 但弹窗开着时必须让位：上下文环的弹窗是**模态**的
     *   （见 g_ctx_open 的注释），这一屏整块归它。
     *   不加 !g_ctx_open 的话，弹窗期间点滑轨会切走面板 ——
     *   而屏幕上还盖着弹窗，用户会以为"点哪都没反应，然后莫名换了页面"。 */
    if (!g_ctx_open) {
        int tx, ty;
        if (app_touch_down(&tx, &ty)) {
            for (int i = 0; i < CHAT_RAIL_N; ++i) {
                Rect r = chat_rail_rect(i);
                if (w_hit(&r, tx, ty)) {
                    if (i != g_tab) {
                        g_tab = i;
                        if (i > 0)
                            dockui_init();      /* 切进来时刷新项目/文件 */
                        app_note("chat: rail -> %d", i);
                    }
                    return;
                }
            }
        }
    }

    /* ★★ 2026-10-23 真机反馈：「对话的那个 ZL 没反应」。
     *
     *   查下来 ZL 的处理原本排在**整条链的末尾**（触摸 → 键盘 → 弹窗 →
     *   短语 → 导出…之后），而中间任何一个 return 都会把它吃掉。
     *   最容易踩的就是**键盘展开**那一段：`dockui_ime_active()` 为真时
     *   整帧归 IME，ZL 根本走不到。
     *
     *   现在提到 B 的旁边 —— 语音是"按住说话"，它不该依赖任何别的状态。
     *   只认**边沿**：按住期间反复进 voice_begin 会把录到一半的缓冲 reset 掉。
     *   `held` 也要算进去：ZL 按住不放时 down 只有第一帧有。 */
    if ((down | held) & KEY_ZL) {
        bool zl_now  = true;
        bool zl_edge = !g_zl_prev;

        g_zl_prev = zl_now;
        if (zl_edge)
            voice_begin();          /* 返回 void；成功/失败它自己设状态行 */
        /* ★★ 按住期间**必须自己搬采样**（2026-10-24 修）。
         *
         *   这一句 return 挡住了函数末尾那段 voice_pump()，于是按住 ZL
         *   期间一个采样都没搬进来：松手时 voice_ms()=0、bytes=0，
         *   被判成"太短，请再说一遍"，而用户明明按了整整一秒多。
         *   症状极具误导性 —— 屏上只有"太短"两个字，看起来像麦克风没收到声音；
         *   而 micprobe（自己单开一轮采集）一切正常，于是很容易得出
         *   "模拟器麦克风不行"的错误结论。两条路互不替证，正是这个道理。
         *
         *   这个 bug 是 2026-10-23「把 ZL 提到最前面」时带出来的：
         *   原来只在**边沿那一帧** return，按住期间会落到函数末尾的
         *   pump 那里；改成"按住期间每帧都 return"之后，pump 就永远轮不到。
         *   教训：为了让某个键"不被别人抢"而提前 return，就得把它
         *   自己那份每帧工作一并搬进来 —— 否则挡掉的不只是别的键。 */
        if (g_rec) {
            voice_pump();
            if (voice_full())
                voice_finish();
        }
        return;                     /* 按住期间每帧都停在这里，别往下走 */
    } else if (g_zl_prev) {
        g_zl_prev = false;
        voice_finish();             /* 松手：收尾并发识别 */
        return;
    }

    /* ★ 摇杆 = 上屏滚动（见 chat_stick_scroll 上方那段）。
     *   放在这里（ZL 之后、各面板之前）的理由：
     *     ① ZL 是语音，按住期间每帧都 return —— 摇杆不该抢它的帧；
     *     ② 面板格（键盘/项目/文件/浏览）要自己吃上下键，摇杆在那些格
     *        里不该动上屏，所以判在 g_tab == 0 之前但**只对第 0 格与
     *        键盘格生效**（那两格看对话最多）。
     *   键盘展开时不生效：那时整屏是候选字，滚动上屏没有意义。 */
    if (!dockui_ime_active() && g_tab <= 1)
        chat_stick_scroll();

    /* ★★ 上一帧按了发送：占位消息已经画出来了，现在才真的发请求。
     *
     *   ★ 2026-10-24：这一段**从"输入框视图"尾部搬到这里**（第 0 格与
     *   面板格的分支之前）。原先它在函数很靠后的位置，而那两个分支都以
     *   `return` 结尾 —— 于是从键盘格发出的消息，g_pending 永远等不到
     *   处理：占位气泡画在屏上、请求一个字节都没出去，而且**没有任何报错**。
     *   现象是"按了发送，屏上多了一条'正在想…'，然后就一直想下去"。
     *
     *   这个 bug 只在"发送入口不在第 0 格"之后才可能出现 —— 所以它不是
     *   老代码的错，是搬动键位时带出来的。教训：凡是 `if (x) { ...; return; }`
     *   的分支，后面挂的收尾逻辑都要重新问一遍"还有没有别的路能到这里"。
     */
    if (g_pending) {
        g_pending = false;
        do_real_send();
        /* 回复已经画出来了。这条回复里如果带着动作，现在才推确认屏 ——
         * 放在这里的理由：do_real_send 是阻塞的，在里面 push 会把刚画好的
         * 回复盖过去，用户还没看清 AI 说了什么就被要求做决定。 */
        if (g_action_ready) {
            g_action_ready = false;
            app_push(SCREEN_CONFIRM);
        }
        return;
    }

    /* ★ 第 0 格（选项）：点某一行 = 切一格。
     *   放在滑轨判定**之后** —— 滑轨压在选项行上方，点滑轨要优先切格。 */
    if (g_tab == 0) {
        int tx, ty;
        if (app_touch_down(&tx, &ty)) {
            dockui_touch_opt(tx, ty);
            return;
        }
        /* ★★ **LEFT/RIGHT 不能交给 dockui_keys_opt**（2026-10-24 修）。
         *
         *   dockui_keys_opt 里 LEFT/RIGHT 的语义是"把当前行的值减/加一档"，
         *   而用户定的分工是"左右切滑轨那五格"（见下方 KEY_LEFT/KEY_RIGHT
         *   那段的注释）。这两件事撞在一起时，**后者必须赢** ——
         *   因为紧接着的 return 挡在切格代码前面：左右被吃掉，人却看到
         *   画面变了（某一行的值少了一档），看着像"切了格"，其实一直
         *   停在第 0 格。
         *
         *   这个坑的代价在验收里非常具体：趟 7 的"←→ 切格"判据**假通过**
         *   ——它只比对"底屏变了"，而改值也会让底屏变。于是一路绿灯，
         *   直到有人去量"页脚为什么从不轮播"才发现人根本没离开过第 0 格。
         *
         *   顺带把文案也说通了：这一格自己的页脚 S0963 写的就是
         *   「[←→] 切格  [↑↓] 选行  [A] 动作  [B] 返回」——
         *   屏上说的和按键做的本来就该是同一件事。
         *
         *   A 仍归选项面板（"动作"= 把这一行的值换下一档）。
         *   UP/DOWN 也仍归它（选行）。 */
        if (down & (KEY_UP | KEY_DOWN)) {
            dockui_keys_opt(down);
            return;
        }
        if (down & KEY_A) {
            dockui_keys_opt(down);
            return;
        }
    }

    /* ★★ 2026-10-27：在「键盘」格里，IME 的上屏目标必须是**对话输入框**。
     *
     *   键盘格的「拼音」按钮只是把 g_ime 置真、**不重新绑定** ——
     *   所以绑定取决于最后一次 ime_bind。而本屏在 IME 关闭时会 bind(NULL,0)，
     *   于是：点「拼音」→ 打出来的字进了**工作台的 g_text**，
     *   对话输入框仍然是空的（按发送会说 Empty input）。
     *   实测就是这么翻车的：脚本填好了句子，发出去却是空输入。
     *
     *   修法：只要停在键盘格，每帧把绑定指回 g_input。每帧一次是想让
     *   "从别的屏切回来"也自动纠正，不必在每条路径上都记得补一句。 */
    if (g_tab == 1)
        dockui_ime_bind(g_input, CHAT_TEXT);

    /* 拼音键盘展开时，一切按键归它 */
    if (dockui_ime_active()) {
        int tx, ty;
        if (app_touch_down(&tx, &ty))
            dockui_ime_touch(tx, ty);
        else
            dockui_ime_keys(down);
        /* ★ 这一帧把它关掉了 → 立刻解绑。
         *   不解绑的话，若本屏随后被 leave（用户按 B 一路退出去），
         *   g_ime_dst 仍指着这个屏的静态 g_input。虽然它也是静态的、
         *   地址不会失效，但一旦别处也用 ime 就会写错地方。
         *   绑定只该在"用它的这一屏"期间存在。 */
        if (!dockui_ime_active()) {
            dockui_ime_bind(NULL, 0);
            /* 上屏内容变了，让本帧重绘输入框。app_status_set 已经
             * 提示过，这里只补一条落盘记录便于真机排查。 */
            app_note("chat: ime closed, input=%d bytes",
                     (int)strlen(g_input));
        }
        return;
    }

    /* 面板格：交给组件；B 回聊天格 */
    if (g_tab > 0) {
        int tx, ty;
        if (app_touch_down(&tx, &ty)) {
            /* ★ 语音开关只在键盘格（g_tab==1）。
             *   先判它，再交给面板 —— 它是**画在页眉上**的，而页眉
             *   不属于面板内容区，dockui_touch 不会认它。 */
            if (g_tab == 1 && w_hit(&CHAT_BTN_MIC, tx, ty)) {
                if (g_rec) { g_touch_arm = false; voice_finish(); }
                else       { voice_begin(); g_touch_arm = true; }
                return;
            }
            dockui_touch(g_tab - 1, tx, ty);
            /* 触摸「送到对话」同样只登记、由本屏发（见按键那段的说明）。
             * ⚠️ 这一句不能省：点了按钮却要再按一下 Y 才发，
             *   用户会认定"这个按钮是坏的"。 */
            if (dockui_take_send())
                do_send();
            return;
        }
        /* ★ B 不再在这里处理 —— 它已经在 chat_handle 开头统一退屏。
         *   原先这里 `g_tab = 0; return;` 只收起面板不退屏，
         *   是"按 B 出不去"的第二个原因。 */
        /* ★★ ←/→ 不交给面板（2026-10-24 修）：它们是"切滑轨那五格"。
         *
         *   这条 return 原先挡在切格代码前面，于是**进了键盘格就再也
         *   按不回来** —— 左右被面板吃掉，屏幕没有任何反应（面板里
         *   本来就不用左右）。表现是"十字键只能往一个方向走"，
         *   而第 0 格那边（见上）同样吞左右，两头一堵，人就锁在原地。
         *
         *   唯一的例外是项目页的"挑模型"子状态（g_pick >= 0）：
         *   那里 ←/→ 是"换一家的下一把"，是个模态。这时候让位给切格，
         *   挑到一半的界面会直接消失，用户会以为自己点错了什么。
         *   所以用 dockui_wants_lr() 问一句，而不是在这里猜。 */
        /* ★ SELECT 在**任何格子**都换短语（见 chat_fill_phrase 的注释）。
         *   必须问一句面板要不要它：项目格的 SELECT 是"删掉这个任务"，
         *   抢过来就会变成"想删任务却把输入框填满了"，而且删不掉。 */
        if ((down & KEY_SELECT) && !dockui_wants_select()) {
            chat_fill_phrase();
            return;
        }
        if (!(down & (KEY_LEFT | KEY_RIGHT)) || dockui_wants_lr()) {
            dockui_keys(g_tab - 1, down);
            /* ★ 面板可能登记了"发送"（键盘格按 Y / 点「送到对话」）。
             *   必须在这一帧就取走并发出去：面板自己不知道怎么发 ——
             *   它没有 g_pending、不会画占位、也不认识 chat_ready()。
             *   放在这里而不是面板里回调，是为了让"发送只有一条路"
             *   （do_send），否则"两条发送路径行为不一致"迟早会出问题。 */
            if (dockui_take_send())
                do_send();
            return;
        }
        /* 落到下面那条统一的 ←/→ 切格处理 */
    }

    /* 语音：**先于一切**处理。
     * 录完的下一帧要发识别（阻塞几秒），所以这两条必须在最前面 ——
     * 否则用户这一帧按 A 会被当成"对着空输入框发送"。 */
    if (g_asr_armed) {
        g_asr_armed = false;
        do_asr();
        return;
    }
    if (g_rec) {
        /* 每帧把新增的采样搬进来。这一步**不能跨帧攒** ——
         * 攒着不采，用户会看到"录了 2 秒只出来 0.5 秒"，而这在内层
         * 完全看不出来（voice_pump 老老实实返回 0）。 */
        voice_pump();
        /* 只认「录满 5 秒硬上限」兜底；正常停录由 ZL 释放边沿触发
         * （见下方 zl_edge 分支）。不再用静音检测（voice_silent_long）提前切，
         * 否则说话中途一停顿就被误停、要重录。 */
        if (voice_full())
            voice_finish();
    }

    /* ── 环弹窗的模态吞键 ────────────────────────────────────────
     * 开着的时候，除了「返回」钮和 B，这个屏谁都不认。
     *
     * 为什么必须吞：用户开着弹窗按 SELECT，短语被换掉了，而弹窗
     * 整块盖着输入框 —— 一个"我明明没动它却变了"的状态。
     * 这种事的排查成本比写这十几行高得多。
     *
     * 为什么放在 g_pending 之后：上一帧按了发送的话，这一帧必须
     * 让请求发出去。吞在它前面会开出"点了发送、又点了环，请求就
     * 永远不发"的窗口（弹窗不会自己关）。
     *
     * 录音的两条路（ZL 边沿、MIC 触摸）不走弹窗：它们会被吞掉，
     * 于是"停录"的那一下没了，录音一路涨到 5 秒硬上限，而弹窗
     * 盖着屏、用户看不见按钮上那个计时。所以环在录着的时候
     * **不接客**（下方触摸分支的条件）—— 从源头去掉这个组合，
     * 比在模态里给语音开特例简单。 */
    if (g_ctx_open) {
        int mx, my;

        if (app_touch_down(&mx, &my) && w_hit(&CTX_POP_BTN, mx, my)) {
            g_ctx_open = false;
            return;
        }
        if (down & KEY_B) {
            g_ctx_open = false;
            return;
        }
        return;                     /* 其余全吞 */
    }

    /* 触摸：模拟器里鼠标点底屏 = 触摸，真机手指同理。
     * 触摸只有按下沿那一帧，先查它，再看按键。 */
    int tx, ty;
    if (app_touch_down(&tx, &ty)) {
        /* 环：点它开用量弹窗。录着的时候不接客（见上方模态注释：
         * 弹窗会吞掉停录的那一下，而录音被盖着看不见）。 */
        if (!g_rec && !g_asr_armed && w_hit(&CTX_RING_HIT, tx, ty)) {
            g_ctx_open = true;
            return;
        }
        /* 思考强度：这家有可调档位时才画这个钮（render_bottom 同一
         * 条件），命中判断必须和画它的条件一致，否则会出现"看得到
         * 点不着"或"点着了却没画"—— 后者更糟：改了个看不见的档位。 */
        if (provider_think_capable(chat_provider()) &&
            w_hit(&CHAT_BTN_TLVL, tx, ty)) {
            cycle_think_level(+1);
            return;
        }
        if (w_hit(&CHAT_BTN_SEND, tx, ty)) { do_send(); return; }
        /* ★ 键盘按钮 → 我们自己的拼音 IME，不再开系统 swkbd。
         *   3DS 的系统键盘**没有中文拼音页**（SWKBD_TYPE_WESTERN 在日版
         *   机器上还会关掉假名输入），中文根本打不出来 —— 这就是用户
         *   反馈的"中文输入法有问题"。上屏目标绑到本屏的 g_input，
         *   cap 传 CHAT_TEXT（字节），写不下由 ime_commit_idx 整字拒绝。 */
        if (w_hit(&CHAT_BTN_KBD,  tx, ty)) {
            dockui_ime_bind(g_input, CHAT_TEXT);
            dockui_ime_open();
            app_status_set("拼音：打字母，点候选上屏");
            return;
        }
        if (any_think() && w_hit(&CHAT_BTN_THINK, tx, ty)) {
            think_toggle_latest();
            return;
        }
        /* ★ 语音按钮在聊天格**不判**了 —— 它现在画在键盘格里
         *   （见 render_bottom 的 g_tab == 1）。这里留着判定会造成
         *   "看不见但点得着"：页眉右侧那块位置空着，用户点一下
         *   莫名开始录音，而屏上没有任何按钮提示。 */
    }

    if (down & KEY_SELECT) {
        chat_fill_phrase();
        return;
    }
    /* ★★ 2026-10-23 用户要求：「上面那个滑轨改成 L2 键，可以移动，移动光标」。
     *
     *   也就是 L / R 两个肩键用来**切滑轨那五格**（选项/键盘/项目/文件/浏览），
     *   不必再伸手指去点屏幕。
     *
     *   代价：这两个键原来绑着「R = 改当前短语」「L = 导出全部对话」。
     *   **功能没删，只是暂时没有快捷键入口**（函数都还在，见下方注释）。
     *   下一轮要补的话，最合适的位置是「更多」菜单里加两项 ——
     *   它们是低频动作，不该占着最顺手的肩键。
     *
     *   注意用 g_tab 的**环形**移动：滑轨是 5 格，从第 0 格按 L 要绕到第 4 格。 */
    if (down & KEY_L) {
        g_tab = (g_tab + CHAT_RAIL_N - 1) % CHAT_RAIL_N;
        if (g_tab > 0)
            dockui_init();
        app_note("chat: rail(L) -> %d", g_tab);
        return;
    }
    if (down & KEY_R) {
        g_tab = (g_tab + 1) % CHAT_RAIL_N;
        if (g_tab > 0)
            dockui_init();
        app_note("chat: rail(R) -> %d", g_tab);
        return;
    }
    if (down & KEY_Y) {
        /* ★ 同键盘按钮：走我们自己的拼音 IME（系统键盘没有中文拼音页） */
        dockui_ime_bind(g_input, CHAT_TEXT);
        dockui_ime_open();
        app_status_set("拼音：打字母，点候选上屏");
        return;
    }
    if (down & KEY_X) {
        /* 豆包没有 Key 可配 —— X 直接去扫码登录。
         * 两处提示（底屏文案、进屏那句）写的都是"扫码登录"，
         * 这里要是还送去 Key 教程，就等于让用户按着提示走却到错的地方。 */
        app_push(chat_provider()->fmt == PROVIDER_FMT_DOUBAO
                     ? SCREEN_DOUBAO : SCREEN_APIKEY);
        return;
    }
    if (down & KEY_A) {
        do_send();
        return;
    }
    /* ★★ 2026-10-23 用户定的分工：「十字键控制下屏内容，摇杆上下控制上屏文字」。
     *
     *   **左/右** = 切下屏滑轨（5 格环形）。原来这两个键是"翻哪一条的思考"，
     *   现在整条滑轨都交给十字键，L/R 就能还回它们原本抢走的那两个活
     *   （改短语 / 导出对话，见 BACKLOG）。
     *
     *   **上/下** = 下屏当前格里的行光标。第 0 格（选项）已有自己的
     *   dockui_keys_opt；面板格交给 dockui_keys 的 KEY_UP/DOWN 分支 ——
     *   它跟触摸点行是**同一份** g_sel，所以"推十字键"和"点屏幕"永远一致。
     *
     *   ⚠️ 上屏滚动的快捷键**删掉**了：现在只有摇杆能滚（见 chat_stick_scroll）。
     *      留着快捷键的话，同一件事有两个入口，而用户刚被要求"分工"，
     *      再留一条暗路就等于没分工。
     */
    if (down & KEY_LEFT) {
        g_tab = (g_tab + CHAT_RAIL_N - 1) % CHAT_RAIL_N;
        if (g_tab == 2)
            dockui_init();          /* 项目格：重读任务表，和 L 键那条路一致 */
        /* 记一行：验收要能证明"十字键真的切了格"。
         * 只看底屏变了是不够的 —— 在第 0 格里改某一行的值，底屏也会变，
         * 而人还停在第 0 格（2026-10-24 趟 7 就是这样假通过的）。 */
        app_note("chat: dpad(L) -> %d", g_tab);
        return;
    }
    if (down & KEY_RIGHT) {
        g_tab = (g_tab + 1) % CHAT_RAIL_N;
        if (g_tab == 2)
            dockui_init();
        app_note("chat: dpad(R) -> %d", g_tab);
        return;
    }
    /* 上下交给下屏的当前格。第 0 格与面板格各有各的处理，这里统一转交 ——
     * 好处是"十字键改的是下屏"这件事只有这一处，不会再有人另开一条链。 */
    if (down & (KEY_UP | KEY_DOWN)) {
        if (g_tab == 0)
            dockui_keys_opt(down);
        else
            dockui_keys(g_tab - 1, down);
        return;
    }
    /* B 已上移到函数开头统一处理（见那里的长注释）。 */
}

/* 退出对话屏：把键盘格绑着的输入框解掉。
 * 不解绑 = 面板还指着一个"可能已经不是输入框"的地址，
 * 下次谁在别的屏里开键盘格，字就写进了对话屏的静态缓冲里，
 * 而那个屏自己什么都看不见。 */
static void chat_leave(void)
{
    dockui_bind_text(NULL, 0);
}

const Screen g_screen_chat = {
    .id            = SCREEN_CHAT,
    .name          = "SCREEN_CHAT",
    .enter         = chat_enter,
    .leave         = chat_leave,
    .render_top    = render_top,
    .render_bottom = render_bottom,
    .handle        = chat_handle,
};
