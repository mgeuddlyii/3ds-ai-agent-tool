/*
 * sc_keyedit.c -- 在机器上直接录入凭据（Key / 服务商 / 模型 / 网址 / 思考强度）
 *
 * ── 这一屏补的是什么洞 ──
 * 在这之前，"给这个 app 配一把 Key"只有一条路：**把 SD 卡拔下来插电脑**，
 * 用记事本编辑 key.txt，再插回来。3DS 上没有粘贴板，所以那条路确实更省事 ——
 * 但它不是**唯一**该有的路：卡在读卡器里、手边没电脑、或者只是想把
 * 网址改一个字符的时候，用户被卡死在"请去找一台电脑"上。
 *
 * ── 为什么是"服务商 + 模型 + 网址 + Key"挤在一屏，而不是四个向导页 ──
 * 这四项是**同一个动作的四个字段**：用户脑子里是"我要连上豆包/我要连上
 * 我自己的服务器"。拆成向导的话，改一个网址得把另外三项也重走一遍，
 * 而重走就意味着"不小心把 Key 也改了"这类事故。
 * 一屏一栏、按 A 编辑、最后一行保存 —— 一次只动你要动的那一项。
 *
 * ── 与既有两条路的关系（不要重复造） ──
 *   · 教程页（sc_apikey.c）：教"怎么申请、放到哪个文件"，那条路仍在，
 *     它是**离线**可读的说明书；这一屏是**动手做**。
 *   · Key 管理屏（sc_keys.c）：管**已有的**多把 Key（切换/删除/导入）。
 *     这一屏是新增，存进去之后在那边就能看到。
 *   · 三者共用同一个仓库（keystore），不是三套存储。
 *
 * ── 网址为什么只有"自定义"能改 ──
 * 内置那几家的地址是**编译期常量**（provider.h 开头写了理由：那等于让
 * 配置文件决定把你的 Key 发到哪去）。所以这里不去"放宽"它，而是
 * 把话说明白，并指向唯一的那条合法路径：把服务商选成 custom。
 * 静默失败（按了没反应）是最坏的：用户会以为程序坏了。
 */

#include "screen.h"

#include "app.h"
#include "credfile.h"
#include "dock_ui.h"      /* 行内编辑器：不弹系统键盘 */
#include "input.h"
#include "keystore.h"
#include "provider.h"
#include "httpx.h"   /* 测试连接：只发一个 GET /models */
#include "widgets.h"

#include <stdio.h>
#include <string.h>
#include "uistr.h"

/* ★ 行高从 24 收到 21、起点保持 26：加了两行之后（6 → 8 行），
 * 8×21 = 168，26+168 = 194，**刚好落在底屏保存按钮（156）与
 * 页脚之上**。24 的话是 26+192=218，会越过 W_TOP 的可视区，
 * 最后两行压到页脚上 —— 而那种"行挤在一起"在截图里看着像
 * 渲染错位，真因只是行高没跟着行数调。 */
#define ROW_H   21
#define ROW_Y0  26.0f

/* 行的顺序按**用户脑子里配对的东西**排，不按字段的存储顺序：
 *   · 服务商 / 模型 / 网址 / 人设 / 上下文轮数 —— 都是"这台 AI 是哪一家、
 *     叫什么、连哪儿、什么性格、记得多久"，一个动作五件事；
 *   · API 密钥 —— 唯一的秘密，单独一档、放中间偏下，离"保存"远一点；
 *   · 思考强度 / 保存返回。
 * 早先 key 紧挨着网址，是因为那时只有 6 行；加了两行之后仍把它
 * 摆在网址后面，"改网址"和"改 Key"就成了相邻行，误按 A 直接弹密钥
 * 键盘的概率变高 —— 密钥那一步是**不可逆**的（敲了就加密落盘）。 */
enum {
    R_SVC = 0,
    R_MODEL,
    R_URL,
    R_SYS,        /* 助手人设（system_prompt） */
    R_CTX,        /* 上下文轮数（context_turns） */
    R_KEY,
    R_THINK,
    R_SAVE,
    R_COUNT
};

/* ★★ 2026-10-23 用户要求：「改回以前那样列出知名厂商 + 填模型名，
 *   剩下都不需要动手了」。
 *
 *   所以这一屏现在只露四行（自定义再多一行网址）：
 *     服务商 / API 密钥 / 模型名 / （自定义）网址 / 保存
 *
 *   被藏起来的三行是：助手人设、上下文轮数、思考强度。
 *   它们**不是删了** —— do_save() 仍然从 cfg 里读原值写回，
 *   改它们的地方在「设置」屏（那里空间够、还带说明）。
 *   隐藏而不是删除的理由：这三项的值必须继续参与保存，
 *   从枚举里摘掉会引起一大片 `case R_SYS:` 编译错误，
 *   而这轮要的是"界面别露出来"，不是重构数据结构。
 *
 *   ★ 不要在这里加回任何一行"顺手也能填"的东西 —— 用户原话是
 *     「剩下都不需要做动手了」，加回去就是又一次把简单的事弄复杂。 */
static bool svc_is_custom(void);   /* 定义在后面，row_visible 要用 */

static bool row_visible(int r)
{
    switch (r) {
    case R_SVC:
    case R_KEY:
    case R_MODEL:
    case R_SAVE:
        return true;
    case R_URL:
        return svc_is_custom();     /* 内置厂商的网址是编译期常量，不让填 */
    default:
        return false;               /* R_SYS / R_CTX / R_THINK：挪去设置屏 */
    }
}

/* 可见行数（"3/5"那个分母）。 */
static int visible_count(void)
{
    int n = 0;

    for (int i = 0; i < R_COUNT; ++i)
        if (row_visible(i))
            ++n;
    return n;
}

/* 可见序号（1-based，"3/5"那个分子）。 */
static int visible_index(int r)
{
    int k = 0;

    for (int i = 0; i <= r && i < R_COUNT; ++i)
        if (row_visible(i))
            ++k;
    return k;
}

/* 上下移动时跳过隐藏行。环形 —— 和原来的 % R_COUNT 行为一致。 */
static int step_row(int from, int delta)
{
    int r = from;

    for (int guard = 0; guard < R_COUNT; ++guard) {
        r += delta;
        if (r < 0)
            r = R_COUNT - 1;
        if (r >= R_COUNT)
            r = 0;
        if (row_visible(r))
            return r;
    }
    return from;
}

/* ★ 存编号不存字符串：静态初始化里 S() 放不下。
 * 早先 R_KEY 这一位写 0、由 row_label() 兜底返回字面量 "API Key"，
 * 结果是全屏唯一一行**英文**（其余都是中文）——用户报"这一行看着
 * 不对劲"。现在它也拿一个真正的中文标签（S0909「API 密钥」），
 * row_label() 不再有兜底分支。 */
static const short ROW_LABEL[R_COUNT] = {
    S0065, S0380, S0381, S0533, S0859, S0909, S0382, S0383,
};

/* 取第 i 行的标签。取字符串这件事收在这里，
 * 所以"忘了包一层 S()"的漏法只可能出现在这一处。 */
static const char *row_label(int i)
{
    return S(ROW_LABEL[i]);
}

static int  g_row;
static char g_svc[24];
static char g_model[48];
static char g_url[96];
static char g_key[CREDFILE_KEY_MAX + 1];
static int  g_think;
/* ★ 这两项原先没有。用户报"加 Key 那里缺格子：上下文长度、人设都没有" ——
 * 它们是 AiCfg 里的一等字段（ai.context_turns / ai.system_prompt），
 * 别处（设置屏、对话页）都在用，录入凭据屏却没给入口，等于逼用户
 * 只能靠"拔卡改 config.json"这一条路，而这屏存在的意义正是补那条路。 */
static int  g_ctx;                              /* 上下文轮数 */
static char g_sys[CONFIG_MAX_PROMPT + 1];       /* 助手人设 */
static char g_msg[192];

/* ★★ 2026-10-27：本屏四个可编辑字段**改用自绘编辑器**（dockui_edit_*），
 *   不再走 ui_input_text → system swkbd。
 *
 *   原因有两条，都不是"更好看"：
 *   ① 模拟器里 swkbd 弹的是 Qt 窗口，**验收脚本够不着** ——
 *      "添加 API"是整机最需要可复现的一条流程，却偏偏没法自动化；
 *   ② 系统键盘在 3DS 上没有拼音页，中文人设根本打不出来。
 *   自绘键盘现在支持拼音 + 数字 + 符号 + 空格，两件事一起解决。 */
static char g_ebuf[CREDFILE_KEY_MAX + 1];   /* 编辑中的临时缓冲 */
static int  g_edit_row = -1;                /* 正在编辑哪一行，-1 = 没在编辑 */

static void edit_begin(int row);            /* 前向声明（下面才实现） */
static void edit_apply(void);

/* 保存按钮的矩形。**render 与 handle 共用这一份** ——
 * 两处各写一遍坐标的话，改版面时必然漂移成"看到的位置点不到"，
 * 而那种 bug 在截图里完全看不出来（见 widgets.h 的 w_hit 说明）。 */
/* ★ 2026-10-23：保存按钮从通栏 300 缩成一半，右边让给「测试连接」。
 *   为什么测试要和保存并排：填完 Key 之后最想知道的永远是"这个 Key 能用吗"，
 *   而原来唯一的验证方式是**保存 → 回主页 → 进对话 → 发一句话**，
 *   失败了还得再走一遍。把验证放在保存旁边，改一个字段就能再测一次。
 *   测试**不落盘**，与「按 B 返回不保存」那句承诺一致。 */
static const Rect SAVE_BTN = { 10, 156, 146, 36, 0 };
static const Rect TEST_BTN = { 164, 156, 146, 36, 0 };

/* ★ 2026-10-22 新增：从卡上 key.txt 导入整个配置。
 *   用户要求「API 除了新手教程之外，在更多里也可以通过多种方法加进来 ——
 *   可以写文件、也可以直接手输」。写文件那条路本来只在 Key 管理屏的 Y 键里，
 *   而现在这一屏是「更多 → 添加 API」的落地页，两条路都该在这里可达 ——
 *   否则用户从"添加 API"进来，发现只能一个个字符敲，等于少了一半功能。
 *   放在保存按钮**下方**，因为它不是"本次编辑的结果"，而是**替换开头**。 */
static const Rect IMPORT_BTN = { 10, 196, 300, 26, 0 };

static bool editing_key(void) { return g_key[0] != '\0'; }

/* 值列靠右对齐，最多能占 ~370px。超长的值（典型是自定义网址和用户自己敲的
 * 模型名）**必须先截断**：
 *   · 整条画出去 → x 变成负数，文字跑到屏幕左边界之外，用户看到的是网址的
 *     **尾巴**，还以为显示错位了；
 *   · 为了不裁而按文字宽度给足宽 → 等于把版面护栏关掉，以后谁把这一栏
 *     改窄都不会有人报错。
 * 所以：截成「头…尾」（网址最有信息量的就是两头），再按实际宽度右对齐。
 *
 * ⚠️ 截断点必须落在 **UTF-8 起始字节**上：按字节硬切会把一个汉字劈成
 *    半个，渲染出来是一个方块或者干脆乱码（中文 UTF-8 在 C 里踩过四次，
 *    见项目 memory）。这里两个方向都做了边界回退。 */
#define VALUE_MAX_BYTES 34

static void value_fit(const char *in, char *out, size_t cap)
{
    size_t n = strlen(in);
    size_t head, tail;

    if (n <= VALUE_MAX_BYTES) {
        snprintf(out, cap, "%s", in);
        return;
    }

    head = VALUE_MAX_BYTES / 2;
    tail = VALUE_MAX_BYTES - head - 1;

    /* 头部：切点不能落在续字节 (10xxxxxx) 上 → 往回退 */
    while (head > 0 && ((unsigned char)in[head] & 0xC0) == 0x80)
        --head;
    /* 尾部：起点同理 → 往前退（即尾巴短一点） */
    while (tail > 0 && ((unsigned char)in[n - tail] & 0xC0) == 0x80)
        --tail;

    snprintf(out, cap, "%.*s…%.*s",
             (int)head, in, (int)tail, in + (n - tail));
}

/* 当前选中的服务商定义。**比名字判存在性** —— provider_find 找不到时
 * 返回的是默认那一家，不报错（provider.h 里写明了这个陷阱）。 */
static const ProviderDef *cur_def(void)
{
    const ProviderDef *p = provider_find(g_svc);

    return (p && strcmp(p->name, g_svc) == 0) ? p : NULL;
}

static bool svc_is_custom(void) { return strcmp(g_svc, "custom") == 0; }

/* --------------------------------------------------------------------------
 * 值
 * ----------------------------------------------------------------------- */

static void row_value(int row, char *out, size_t n)
{
    switch (row) {
        case R_SVC:
            snprintf(out, n, "%s", g_svc);
            break;

        case R_MODEL:
            /* 空不是"错"，是"用这家的默认模型"。界面上要说清楚，
             * 否则用户会以为漏填了。 */
            snprintf(out, n, "%s", g_model[0] ? g_model : S(S0384));
            break;

        case R_URL:
            if (!svc_is_custom())
                snprintf(out, n, S(S0385));
            else
                snprintf(out, n, "%s", g_url[0] ? g_url : S(S0386));
            break;

        case R_SYS:
            /* 和设置屏同一个口径：人设最长 240 字节，全摊出来会把这一行
             * 撑爆，所以只报"设没设 + 多少字"，全文按 A 进去看。 */
            if (g_sys[0])
                snprintf(out, n, S(S0550), (int)strlen(g_sys));
            else
                snprintf(out, n, "%s", S(S0551));
            break;

        case R_CTX:
            /* 值格式与设置屏同一条（S0864「%d 轮」）—— 同一个字段在
             * 两个屏上必须长得一样，否则用户会以为改的是两回事。 */
            snprintf(out, n, S(S0864), g_ctx);
            break;

        case R_KEY:
            if (editing_key())
                snprintf(out, n, S(S0387), (int)strlen(g_key));
            else
                snprintf(out, n, "%s", S(S0388));
            break;

        case R_THINK: {
            /* 和设置屏同一个口径：这家没有这个旋钮就直说，
             * 别让用户对着一个"选了没反应"的档位发呆。 */
            const ProviderDef *p = cur_def();
            if (p && p->think == PROVIDER_THINK_NONE)
                snprintf(out, n, S(S0389), thinking_name(g_think));
            else
                snprintf(out, n, "%s", thinking_name(g_think));
            break;
        }

        default:
            /* 最后一行是动作：值列只说"会发生什么"，不重复标签 */
            snprintf(out, n, "%s", editing_key() ? S(S0390)
                                                 : S(S0391));
            break;
    }
}

/* 当前行"这一项是干嘛的"。下屏承担解释 —— 上屏只放事实，
 * 与 sc_apikey.c 的分工一致（§4.7）。 */
static const char *row_help(int row)
{
    switch (row) {
        case R_SVC:
            return S(S0790);
        case R_MODEL:
            return S(S0791);
        case R_URL:
            if (!svc_is_custom())
                return S(S0792);
            return S(S0793);
        case R_SYS:
            return S(S0911);
        case R_CTX:
            return S(S0910);
        case R_KEY:
            return S(S0794);
        case R_THINK:
            return S(S0795);
        default:
            return S(S0398);
    }
}

/* --------------------------------------------------------------------------
 * 编辑动作
 * ----------------------------------------------------------------------- */

static void cycle_svc(int delta)
{
    int n = provider_count();
    int i, cur = -1;

    if (n <= 0)
        return;

    for (i = 0; i < n; ++i) {
        const ProviderDef *p = provider_at(i);
        if (p && !strcmp(p->name, g_svc)) {
            cur = i;
            break;
        }
    }
    if (cur < 0)
        cur = 0;
    else
        cur = (cur + delta % n + n) % n;

    {
        const ProviderDef *p = provider_at(cur);
        if (!p)
            return;
        /* ★★ 2026-10-23 真机 bug：换了服务商，模型名还留着上一家的。
         *
         *   用户从 DeepSeek 切到智谱，模型那一栏仍然写着 `deepseek-chat` ——
         *   而那是**DeepSeek 专属的 id**，拿去问智谱只会 400。
         *   更糟的是界面上看着"填好了"，用户不会怀疑是这里的问题。
         *
         *   `save` 里的逻辑是 `g_model[0] ? g_model : p->default_model`，
         *   也就是说：**只要 g_model 非空就信任它**。所以切换服务商时必须
         *   主动清空，让它回落到新服务商的 default_model。
         *
         *   清空后值列会显示 S0384（"留空用默认"那一类提示），
         *   用户一眼能看出"这里要重新选"。 */
        if (strcmp(p->name, g_svc) != 0)
            g_model[0] = '\0';
        snprintf(g_svc, sizeof g_svc, "%s", p->name);
    }
    g_msg[0] = '\0';
}

static void cycle_think(int delta)
{
    int want = g_think + delta;

    if (want < 0)
        want = CONFIG_THINK_HIGH;
    if (want > CONFIG_THINK_HIGH)
        want = 0;
    g_think = want;
    app_status_set(S(S0399), thinking_name(want));
}

/* 上下文轮数：和设置屏**同一条口径**（4/8/12 循环，不做 +1 递增 ——
 * 用户要的是"少带点 / 多带点"，4 和 5 轮的实际差别看不出来）。
 * 设置屏那边直接写进配置并落盘；这里**不写** —— 本屏是"改完一起保存"
 * 的模型，中途落盘会让"按 B 返回不保存"这句承诺变成假话。 */
static void cycle_ctx(int delta)
{
    if (delta < 0)
        g_ctx = (g_ctx <= 4) ? 12 : (g_ctx <= 8) ? 4 : 8;
    else
        g_ctx = (g_ctx <= 4) ? 8 : (g_ctx <= 8) ? 12 : 4;

    app_status_set(S(S0864), g_ctx);
}

/* 助手人设：自由文本，上限 CONFIG_MAX_PROMPT（240 字节）。
 * ★ 用 ui_input_text 而不是直调 swkbd：它已经处理了
 *   "swkbd 的长度参数是**字符**、字段上限是**字节**"这条坑
 *   （见 input.c 里那段 3 字节/字符的收紧），并且会 trim 首尾空白。
 *   30 个字符 = 最多 90 字节，稳在 240 以内。 */
/* ---- 行内编辑（2026-10-27 取代原来的四个 edit_xxx / ui_input_text）------ */

static void edit_begin(int row)
{
    const char *title = "";

    if (row == R_URL && !svc_is_custom()) {
        /* 非自定义服务商的网址是**编译期常量**，不给改。
         * 这里必须把"为什么不能改"说出来 —— 弹了键盘再拒绝、
         * 或者按了没反应，用户都会当成 bug。 */
        snprintf(g_msg, sizeof g_msg, "%s", S(S0796));
        app_status_set(S(S0405));
        return;
    }

    switch (row) {
        case R_MODEL: title = S(S0400); snprintf(g_ebuf, sizeof g_ebuf, "%s", g_model); break;
        case R_URL:   title = S(S0406); snprintf(g_ebuf, sizeof g_ebuf, "%s", g_url);   break;
        case R_SYS:   title = S(S0577); snprintf(g_ebuf, sizeof g_ebuf, "%s", g_sys);   break;
        case R_KEY:   title = S(S0409); g_ebuf[0] = '\0';  break;   /* Key 不预填：屏幕上不该出现旧值 */
        default:      return;
    }

    if (!dockui_edit_open(g_ebuf, sizeof g_ebuf, title)) {
        app_status_set(S(S0403));
        return;
    }
    g_edit_row = row;
    app_note("keyedit: edit row=%d start", row);
}

/* 把编辑结果写回字段。**只有这里会改内存字段** —— 与"改完一起保存"的契约一致。 */
static void edit_apply(void)
{
    int row = g_edit_row;

    g_edit_row = -1;
    dockui_edit_close();

    switch (row) {
        case R_MODEL:
            snprintf(g_model, sizeof g_model, "%s", g_ebuf);
            app_status_set(S(S0401), g_model[0] ? g_model : S(S0402));
            break;
        case R_URL:
            snprintf(g_url, sizeof g_url, "%s", g_ebuf);
            g_msg[0] = '\0';
            app_status_set(S(S0407), g_url[0] ? g_url : S(S0044));
            break;
        case R_SYS:
            snprintf(g_sys, sizeof g_sys, "%s", g_ebuf);
            g_msg[0] = '\0';
            if (g_sys[0])
                app_status_set(S(S0578), (int)strlen(g_sys));
            else
                app_status_set(S(S0579));
            app_note("keyedit: system_prompt=%d bytes (not saved yet)", (int)strlen(g_sys));
            break;
        case R_KEY:
            if (!g_ebuf[0]) {                 /* 空 Key 不接受：那不是"清除"，是误触 */
                app_status_set(S(S0410));
                break;
            }
            snprintf(g_key, sizeof g_key, "%s", g_ebuf);
            /* 日志**只报长度**，一个字符都不落盘 —— status.txt 会被脚本回读。 */
            app_note("keyedit: key captured len=%d (not written to status)",
                     (int)strlen(g_key));
            app_status_set(S(S0411), (int)strlen(g_key));
            break;
        default:
            break;
    }
    /* 编辑缓冲用完就抹掉：里面可能是 Key。 */
    memset(g_ebuf, 0, sizeof g_ebuf);
}

/* --------------------------------------------------------------------------
 * 从卡上文件导入
 * ----------------------------------------------------------------------- */

/* 读 key.txt，把四项搬进本屏的内存字段。
 * ★ 刻意**不直接落盘**：这一屏的契约是"改完一起保存"，中途保存会让
 *   "按 B 返回不保存"变成假话（cycle_ctx 那里也是同一条理由）。
 * ★ 搬进 g_key 的明文 Key 只在内存里活到离屏（keyedit_leave 会抹掉）。
 * ★ 文件里没写的字段**保留屏上现有值**，不当成"要清空" —— 用户可能只想
 *   用文件换 Key，别的字段一动就被清掉是最气人的那种设计。 */
static void do_import(void)
{
    CredFile cf;

    if (!credfile_scan(&cf)) {
        snprintf(g_msg, sizeof g_msg, "%s", credfile_status_text(cf.status));
        app_status_set(S(S0916));
        app_note("keyedit: import failed status=%d", (int)cf.status);
        return;
    }

    if (cf.provider[0])
        snprintf(g_svc, sizeof g_svc, "%s", cf.provider);
    if (cf.model[0])
        snprintf(g_model, sizeof g_model, "%s", cf.model);
    if (cf.base_url[0] && svc_is_custom())
        snprintf(g_url, sizeof g_url, "%s", cf.base_url);
    if (cf.key[0])
        snprintf(g_key, sizeof g_key, "%s", cf.key);

    g_msg[0] = '\0';
    app_status_set(S(S0917), (int)strlen(g_key));
    /* 日志只报长度和"哪几项被文件填了"，Key 一个字符都不落盘 ——
     * status.txt 会被验收脚本回读。 */
    app_note("keyedit: import ok provider=%d model=%d url=%d key_len=%d",
             cf.provider[0] ? 1 : 0, cf.model[0] ? 1 : 0,
             cf.base_url[0] ? 1 : 0, (int)strlen(g_key));
}

/* --------------------------------------------------------------------------
 * 测试连接
 * ----------------------------------------------------------------------- */

/* 用**当前屏幕上填的**参数发一个最小请求，看能不能通。
 *
 * 为什么打 /models 而不是发一条真的对话：
 *   · /models 不消耗 token，用户会反复点（改个字段就想再测一次）
 *   · 它同时验证了三件事：DNS/网络通不通、Key 认不认、base_url 对不对
 *   · 缺点是有些服务商不实现它 —— 那时会拿到 404，
 *     所以 404 的文案要写"服务商可能不支持这个探测"，别一口咬定 Key 错
 *
 * 为什么不落盘：这一屏的承诺是「按 B 返回不保存」。测试也照这条走 ——
 * 用户测完发现不对，直接退出去什么都没改。
 *
 * ⚠️ 它会**阻塞**（hx_perform 是同步的），最长 8 秒。所以：
 *   调用前先把状态栏改成"正在测试…"，让用户知道这是程序在等，
 *   不是他按漏了。 */
static void do_test(void)
{
    const ProviderDef *p = cur_def();
    char  url[192];
    char  auth[CREDFILE_KEY_MAX + 32];
    char  err[96];
    const char *hdrs[2];
    HxReq req;
    long  code;
    const char *base;
    size_t bl;

    if (!g_key[0]) {
        app_status_set(S(S0925));     /* 先填 Key */
        return;
    }

    /* 优先用用户填的网址（自定义服务商只有这一条），没填才回落到服务商默认 */
    base = g_url[0] ? g_url : (p ? p->base_url : "");
    if (!base || !base[0]) {
        app_status_set(S(S0926));     /* 没有网址可测 */
        return;
    }
    /* 去掉末尾斜杠再拼 —— 否则会出现 //models，有些服务端会 404 */
    bl = strlen(base);
    while (bl > 0 && base[bl - 1] == '/')
        --bl;
    snprintf(url, sizeof url, "%.*s/models", (int)bl, base);

    snprintf(auth, sizeof auth, "Authorization: Bearer %s", g_key);
    hdrs[0] = auth;
    hdrs[1] = NULL;

    memset(&req, 0, sizeof req);
    req.url         = url;
    req.headers     = hdrs;
    req.timeout_s   = 8;
    req.follow      = true;
    req.add_cookies = false;

    app_status_set(S(S0927));         /* 正在测试… */
    app_note("keyedit: test %s", url);

    err[0] = '\0';
    code = hx_perform(&req, NULL, NULL, err, sizeof err);

    if (code > 0) {
        if (code == 200) {
            app_status_set(S(S0928), (int)code);
            app_note("keyedit: test ok http=%ld", code);
        } else if (code == 401 || code == 403) {
            app_status_set(S(S0929), (int)code);
            app_note("keyedit: test auth http=%ld", code);
        } else if (code == 404) {
            app_status_set(S(S0930), (int)code);
            app_note("keyedit: test 404 (provider may lack /models)");
        } else {
            app_status_set(S(S0931), (int)code);
            app_note("keyedit: test http=%ld", code);
        }
    } else {
        app_status_set(S(S0932), err[0] ? err : S(S0933));
        app_note("keyedit: test fail %ld %s", code, err);
    }
}

/* --------------------------------------------------------------------------
 * 保存
 * ----------------------------------------------------------------------- */

static void do_save(void)
{
    AppConfig    *c = app_cfg();
    const ProviderDef *p = cur_def();
    char          nm[KEYSTORE_NAME_MAX];
    bool          stored = false;

    /* 1) 服务商必须认得 —— 见 cur_def() 的说明 */
    if (!p) {
        snprintf(g_msg, sizeof g_msg, S(S0413), g_svc);
        app_status_set(S(S0414));
        return;
    }

    /* 2) 自定义必须填网址。不拦的话请求连不上，而界面上一切正常 ——
     *    这正是 provider.h 里那个"空网址"陷阱换了个入口再来一遍。 */
    if (svc_is_custom() && !g_url[0]) {
        snprintf(g_msg, sizeof g_msg, S(S0415));
        app_status_set(S(S0416));
        return;
    }

    /* 3) 配置。**网址只有 custom 才写**：内置几家的 base_url 是编译期常量，
     *    留一份用户看得见却不起作用的"地址"在配置里只会误导人。
     *    ★ 人设与上下文轮数照常写 —— 它们和网址不同，不是"分服务商"的，
     *      而是整台的全局设定。 */
    snprintf(c->ai.provider, sizeof c->ai.provider, "%s", g_svc);
    snprintf(c->ai.model, sizeof c->ai.model, "%s",
             g_model[0] ? g_model : p->default_model);
    snprintf(c->ai.base_url, sizeof c->ai.base_url, "%s",
             svc_is_custom() ? g_url : "");
    c->ai.thinking = g_think;

    /* 上下文轮数：夹到合法区间再写。config.c 的 load 侧也会夹，
     * 但**这里也夹一次** —— 否则界面上显示的是一个越界值，
     * 存下去却被改成别的，用户会以为保存没生效。 */
    {
        int ctx = g_ctx;
        if (ctx < CONFIG_CTX_MIN) ctx = CONFIG_CTX_MIN;
        if (ctx > CONFIG_CTX_MAX) ctx = CONFIG_CTX_MAX;
        c->ai.context_turns = ctx;
    }

    /* 人设：整段照搬。空串是合法值（= 用模型默认性格），不是"漏填"。
     * ★ 用 memcpy 而不是 snprintf：g_sys 与字段等长，snprintf 会因
     *   "源可能有 CONFIG_MAX_PROMPT 字符 + 结尾"报 -Wformat-truncation
     *   （编译告警，且真被截断时用户看不见）。这里要的正是"照搬、不越界"，
     *   来源已经过 ui_input_text 的字节上限把关，拷贝完自己补终止符即可。
     *   —— 与设置屏 edit_prompt() 同一条做法。 */
    memcpy(c->ai.system_prompt, g_sys, sizeof c->ai.system_prompt);
    c->ai.system_prompt[sizeof c->ai.system_prompt - 1] = '\0';

    /* 4) Key。留空 = 只改配置，这是合法用法（改个网址不必重敲 Key）。 */
    if (editing_key()) {
        stored = keystore_add(g_svc, c->ai.model, g_key, nm, sizeof nm);
        if (stored) {
            /* 激活要显式做：只有这里知道"用户这次就是想把这一把用起来"。
             * （keystore_add 自己只在仓库原本为空时自动激活。） */
            keystore_set_active(nm);
        } else {
            /* 仓库满 / 空 Key。**配置已经写了**，所以不能报"全都没成" ——
             * 说清楚哪一半成了一半没成，用户才知道该怎么办。 */
            app_cfg_dirty();
            app_cfg_save_now();
            snprintf(g_msg, sizeof g_msg, S(S0797), KEYSTORE_MAX);
            app_status_set(S(S0418));
            app_note("keyedit: cfg saved, keystore_add failed (count=%d)",
                     keystore_count());
            return;
        }
    }

    app_cfg_dirty();
    bool ok = app_cfg_save_now();

    app_note("keyedit: save svc=%s model=%s url_len=%d think=%d "
             "ctx=%d prompt_len=%d key_stored=%d cfg_saved=%d",
             c->ai.provider, c->ai.model, (int)strlen(c->ai.base_url),
             c->ai.thinking, c->ai.context_turns,
             (int)strlen(c->ai.system_prompt), (int)stored, (int)ok);

    app_status_set(stored ? S(S0419) : S(S0420),
                   keystore_count());
    app_pop();
}

/* --------------------------------------------------------------------------
 * 渲染
 * ----------------------------------------------------------------------- */

static void render_top(void)
{
    const float x = (float)W_TOP_MARGIN;
    float       y = ROW_Y0;
    char        pos[24];

    snprintf(pos, sizeof pos, "%d/%d", visible_index(g_row), visible_count());
    w_top_header(S(S0421), pos);

    for (int i = 0; i < R_COUNT; ++i) {
        if (!row_visible(i))
            continue;               /* 人设/上下文/思考强度已挪去设置屏 */

        /* 缓冲要装得下**最长的那一项**（自定义网址 95 字节），
         * 给 80 的时候编译器就报了 -Wformat-truncation —— 而那条警告
         * 在这类"值列"上特别容易被忽略，真被截断时用户看到的是一个
         * 少了一截的网址，照样能存进去。 */
        char val[112];
        char shown[VALUE_MAX_BYTES * 2 + 8];
        const bool cur = (i == g_row);
        RColor fg;

        row_value(i, val, sizeof val);
        value_fit(val, shown, sizeof shown);

        if (cur)
            r_rect(&(Rect){ 0, y - 3, R_TOP_W, ROW_H - 2, 0 }, w_c(C_SLOT_ACC_LT));

        fg = cur ? w_c(C_SLOT_ACC_DK) : w_c(C_SLOT_FG);
        /* 最后一行是动作不是字段，颜色区分开（和 sc_settings 里
         * "版本号"那一行同一套做法） */
        if (i == R_SAVE)
            fg = cur ? w_c(C_SLOT_ACC_DK) : w_c(C_SLOT_ACCENT);
        w_line(x + 4, y, 140, row_label(i), fg);

        {
            /* 和 sc_settings 的值列同一套做法：按实测宽度右对齐，
             * 直接 r_text（不经 w_line 的溢出计数 —— 它按 vw 给宽，
             * 计数恒不触发，反而会把护栏变成摆设）。 */
            int vw = text_width(shown);
            r_text(&(Rect){ (float)(R_TOP_W - W_TOP_MARGIN - vw), y,
                            (float)(vw + 2), 16, 0 },
                   shown, cur ? w_c(C_SLOT_ACC_DK) : w_c(C_SLOT_MUTED));
        }

        y += (float)ROW_H;
    }

    w_top_footer(app_status(), S(S0422));
}

static void render_bottom(void)
{
    const float x = (float)W_BOT_MARGIN;

    /* 编辑中：整屏交给键盘（它自带回显与候选）。 */
    if (dockui_edit_active()) {
        dockui_render_ime();
        return;
    }
    const float w = (float)W_BOT_USABLE;
    float       y = (float)W_BOT_CONTENT_Y;

    w_bot_header(row_label(g_row));

    y = w_wrap(x, y, w, row_help(g_row), w_c(C_SLOT_FG), TEXT_LINE_H);

    y += 8;
    if (g_msg[0])
        y = w_wrap(x, y, w, g_msg, w_c(C_SLOT_WARN), TEXT_LINE_H);
    else
        y += TEXT_LINE_H;

    /* 保存按钮：**下屏唯一可触摸的东西**。上屏的列表点不到
     * （3DS 只有下屏能触摸，见 sc_settings.c 里那段版本号的教训），
     * 所以"可点的动作"必须落在下屏。 */
    w_button(&SAVE_BTN, S(S0383),
             (g_row == R_SAVE) ? W_BTN_SELECTED : W_BTN_PRIMARY);

    /* 测试连接：只读当前屏幕上的值，不落盘、不写 key.txt。 */
    w_button(&TEST_BTN, S(S0924),
             W_BTN_NORMAL | (app_touch_on(&TEST_BTN) ? W_BTN_PRESSED : 0));

    /* 导入按钮：读 sdmc:/3ds/aiconsole/key.txt，把里面的
     * provider/model/base_url/key 一次性搬进本屏的字段（**不落盘**，
     * 仍要点保存才生效 —— 与"按 B 返回不保存"这句承诺保持一致）。 */
    w_button(&IMPORT_BTN, S(S0915), W_BTN_NORMAL);

    w_center((float)R_BOT_W / 2.0f, 226,
             S(S0423), w_c(C_SLOT_FAINT));

    w_bot_footer(S(S0424));
}

/* --------------------------------------------------------------------------
 * 行为
 * ----------------------------------------------------------------------- */

static void keyedit_enter(void)
{
    const AppConfig *c = app_cfg();

    /* ★ 2026-10-28：进这一屏也顺手把卡上的 key.txt 模板建好（已存在则不动）。
     *   原来只有「怎么申请 Key」那个教程屏会建 —— 而用户真正会点进来的
     *   是「添加 API」这一屏，于是"卡上根本没有模板文件"。
     *   模板里把**名称 / 网址 / Key** 三个字段逐条讲清楚了。 */
    if (credfile_write_template())
        app_note("keyedit: key.txt 模板已生成（%s）", credfile_path_display());

    g_row   = 0;
    g_msg[0] = '\0';
    g_key[0] = '\0';

    /* 从当前配置起步，而不是空表：多数人是来"改一项"的，
     * 让他们先把已有的值重新敲一遍是最讨厌的那种设计。 */
    snprintf(g_svc, sizeof g_svc, "%s", c->ai.provider);
    snprintf(g_model, sizeof g_model, "%s", c->ai.model);
    snprintf(g_url, sizeof g_url, "%s", c->ai.base_url);
    g_think = c->ai.thinking;
    /* ★ 这两项必须载入 —— 否则用户只是进来改个网址、顺手保存，
     *   人设和轮数会被"空值"覆盖回默认，那是**静默丢数据**。 */
    g_ctx   = c->ai.context_turns;
    snprintf(g_sys, sizeof g_sys, "%s", c->ai.system_prompt);

    /* 配置里那家的名字如果连表都不认（被手改坏了），退回默认那家 ——
     * 否则这一屏一进来就是"不认识的服务器"，用户不知道从哪修。 */
    if (!cur_def()) {
        const ProviderDef *d = provider_default();
        snprintf(g_svc, sizeof g_svc, "%s", d ? d->name : "deepseek");
        snprintf(g_msg, sizeof g_msg, "%s", S(S0425));
    }

    keystore_load();
    app_status_set(S(S0426), keystore_count());
}

static void keyedit_leave(void)
{
    /* 明文 Key 只活在内存里，离屏即抹 —— 和 sc_apikey.c 的 leave
     * 同一条规矩。它已经加密存了一份，内存里没有再留的理由。 */
    memset(g_key, 0, sizeof g_key);
    memset(g_url, 0, sizeof g_url);
}

static void keyedit_handle(u32 down, u32 held)
{
    (void)held;

    /* ---- 编辑中：键盘优先，别的键一个都不往下走 ----
     * **SELECT = 完成**（写回字段）、B = 取消（原值不动）、A = 上屏候选。
     *
     * ⚠️ 千万别用 START 当"完成"：`main.c` 里 START 是**全局退出热键**
     *    （`if (down & KEY_START) break;`，在 app_handle 之前就拦了）。
     *    2026-10-27 第一版就踩了这个：脚本按 START 想提交，结果整个程序退出，
     *    后面 4 张截图一张没出 —— 而屏上看不出任何异常，特别难查。 */
    if (dockui_edit_active()) {
        if (down & KEY_SELECT) {
            edit_apply();
            return;
        }
        if (down & KEY_B) {
            g_edit_row = -1;
            dockui_edit_close();
            app_status_set(S(S0134));
            return;
        }
        dockui_ime_keys(down);
        return;
    }

    if (down & KEY_B) {
        app_pop();
        return;
    }

    if (down & KEY_UP) {
        g_row = step_row(g_row, -1);
        g_msg[0] = '\0';
        return;
    }
    if (down & KEY_DOWN) {
        g_row = step_row(g_row, +1);
        g_msg[0] = '\0';
        return;
    }

    if (down & KEY_LEFT) {
        if (g_row == R_SVC)   cycle_svc(-1);
        if (g_row == R_THINK) cycle_think(-1);
        if (g_row == R_CTX)   cycle_ctx(-1);
        return;
    }
    if (down & KEY_RIGHT) {
        if (g_row == R_SVC)   cycle_svc(+1);
        if (g_row == R_THINK) cycle_think(+1);
        if (g_row == R_CTX)   cycle_ctx(+1);
        return;
    }

    /* 编辑中：触摸先给键盘（不然点键盘会被当成点"保存"按钮）。 */
    if (dockui_edit_active()) {
        int tx, ty;
        if (app_touch_down(&tx, &ty))
            dockui_ime_touch(tx, ty);
        return;
    }

    /* 触摸：只有下屏那两个按钮是可点的东西 */
    {
        int tx, ty;
        if (app_touch_down(&tx, &ty)) {
            if (w_hit(&SAVE_BTN, tx, ty))   { do_save();   return; }
            if (w_hit(&TEST_BTN, tx, ty))   { do_test();   return; }
            if (w_hit(&IMPORT_BTN, tx, ty)) { do_import(); return; }
        }
    }

    if (!(down & KEY_A))
        return;

    switch (g_row) {
        case R_SVC:   cycle_svc(+1); break;
        case R_MODEL: edit_begin(R_MODEL); break;
        case R_URL:   edit_begin(R_URL);   break;
        case R_SYS:   edit_begin(R_SYS);   break;
        case R_CTX:   cycle_ctx(+1); break;
        case R_KEY:   edit_begin(R_KEY);   break;
        case R_THINK: cycle_think(+1); break;
        default:      do_save();     break;
    }
}

const Screen g_screen_keyedit = {
    .id            = SCREEN_KEYEDIT,
    .name          = "SCREEN_KEYEDIT",
    .enter         = keyedit_enter,
    .leave         = keyedit_leave,
    .render_top    = render_top,
    .render_bottom = render_bottom,
    .handle        = keyedit_handle,
};
