/*
 * config.h -- 持久化配置（真实 JSON）
 *
 * 落盘位置：sdmc:/3ds/aiconsole/config.json
 * 结构见 设计规划.md §5.1。刻意与 §5.1 逐字段对齐 —— 那是权威定义，
 * 改这里必须同步改文档。
 *
 * 关于"为什么不叫 config.cfg"：这是给人看、给人拷的文件。真 JSON 意味着
 * 用户可以用电脑上的编辑器直接改，也能被别的工具读。早期版本写的是
 * "假 JSON"（key=value 存成 .json），已废弃。
 */

#pragma once

#include <stdbool.h>

/* 安全等级（闸门），见 设计规划.md §1 / SAFETY_DESIGN.md §2。
 * 注意：这与 T1–T3（能力域分类）是**正交**的两维，不要混。 */
typedef enum {
    SEC_LEVEL_CHAT     = 0,  /* L0 对话级 */
    SEC_LEVEL_FILE     = 1,  /* L1 文件级 */
    SEC_LEVEL_SYSTEM   = 2,  /* L2 系统级 */
    SEC_LEVEL_LOWLEVEL = 3,      /* L4 完整系统级：能读写 NAND 与系统文件。
     * 走到这里要过密码门（见 konami.c），因为这一档的后果不再能靠
     * "从 SD 卡挪回来"解决。 */
    SEC_LEVEL_FULL = 4,
    /* L5 最高级：AI 可以请求任何一类动作，闸门仍然在 ——
     * **L5 不等于"没有闸门"**，它只表示"AI 被允许请求的种类更全"，
     * 而白名单、执行前确认、路径不来自 AI 这三条对 L5 一样有效。
     * 进这一档必须输密码。 */
    SEC_LEVEL_MAXSEC = 5,
} SecurityLevel;

#define SEC_LEVEL_MAX 5

/* granted_actions 的容量。M2 只是把它原样读回/写回，等到 M7 才会真的用上；
 * 先定一个够用的上限，避免为了一个未来字段引入动态分配。 */
#define CONFIG_MAX_GRANTED 8

typedef struct {
    bool confirmed;
    bool remind_on_startup;
} NandBackupCfg;

typedef struct {
    int  current_level;                    /* SecurityLevel */
    bool persist_level;
    bool dev_mode_unlocked;
    int  granted_count;
    char granted[CONFIG_MAX_GRANTED][32];
} SecurityCfg;

/* 系统提示词（人设）的长度上限。够写一两句，又不至于把 4KB 的请求体吃掉
 * 一大块 —— 上下文预算里 system 是**优先保留**的，太长就会把历史挤光。 */
#define CONFIG_MAX_PROMPT 240

/* 思考强度的档位上限。0=关 1=低 2=中 3=高。
 * 放头文件是因为**界面、配置夹取、请求翻译**三处都要用它 ——
 * 各自写一个 3 的话，改档位时必漏一处。 */
#define CONFIG_THINK_OFF   0
#define CONFIG_THINK_HIGH  3

/* AI 记忆的长度上限。比人设短：它是"补充事实"，不是第二份人设；
 * 太长会把上下文预算吃掉（system 是优先保留的，历史会被挤光）。 */
#define CONFIG_MAX_MEMORY 200

/* 回复长度档位 → max_tokens。放这里是因为**界面、配置夹取、请求**三处都要用，
 * 各写一份数字，改档位时必漏一处（与 CONFIG_THINK_* 同一条纪律）。 */
#define CONFIG_REPLY_SHORT 0
#define CONFIG_REPLY_LONG  2

/* 上下文轮数。下限 2（至少能接上一句），上限 24（再多请求体就顶到边了）。 */
#define CONFIG_CTX_MIN  2
#define CONFIG_CTX_MAX  24

typedef struct {
    char provider[24];
    char model[48];
    /* 凭据类型。"bearer" = 往 Authorization 头塞 API Key；
     * "cookie"  = 往 Cookie 头塞网页会话（豆包那条路预留）。
     * provider 抽象因此与鉴权方式解耦 —— 详见 设计规划.md §5.4。 */
    char auth_type[12];
    bool stream;
    /* 系统提示词（人设）。空串 = 不发 system 消息，就是"没有设定"。
     * 放在配置里而不是写死在代码，是因为这是**用户自己的助手**，
     * 该由他决定它是什么脾气、该不该回避某些话题。 */
    char system_prompt[CONFIG_MAX_PROMPT];

    /* ★★★ 自定义服务商的地址（provider == "custom" 时才用）。
     *
     * 这是本项目里**唯一一处让用户决定"请求发到哪台主机"**的地方，
     * 所以边界必须说清楚：
     *   · 只有**用户**能填 —— 界面（服务目录/引导页）和电脑端写进来的。
     *   · **AI 碰不到** —— actions.c 里没有任何一条改 ai.* 的路径，
     *     所以"模型只给数据、不能自己选主机"那条闸门仍然成立。
     *     以后要加"能改配置"的动作时，**必须**把这两项排除在外。
     *   · 长度 96 够放下常见的 ".../v1"、".../compatible-mode/v1" 这种结尾。
     *     超了会被 config_clamp 截断（而不是让请求拼出一个畸形 URL）。 */
    char base_url[96];

    /* 思考强度：0 = 关，1 = 低，2 = 中，3 = 高。
     * 名字叫"强度"而不是"开关"，是因为各家接口的表达方式不一样：
     * 有的收 low/medium/high，有的收 token 预算，有的只有开/关。
     * 翻译成各家参数是 chatapi 的事，这里只存一个档位。
     * 默认 0（关）：多花 token 是用户自己的选择，不能替他想。 */
    int  thinking;

    /* AI 记忆：一段"关于用户"的长期事实。每次请求拼在 system 之后。
     * 与 system_prompt 分开存是刻意的 —— 前者是"助手是什么脾气"，
     * 后者是"关于你的事实"，两者的编辑时机与用途都不同。 */
    char memory[CONFIG_MAX_MEMORY];

    /* 回复长度：0 短 / 1 中 / 2 长 → 映射到请求的 max_tokens。 */
    int  reply_len;

    /* 带几轮历史。0 = 用默认（8）。 */
    int  context_turns;

    /* 回答语言：0 = 跟随界面语言，1 = 强制中文，2 = 强制英文。 */
    int  reply_lang;
} AiCfg;

typedef struct {
    char  theme[12];
    float depth;          /* 0.0 - 1.0，立体深度 */
    char  font_size[12];       /* small / mid / large，见 config_text_scale() */

    /* ★ 界面语言（uistr.c 读它选表）。默认 "zh"，所以**老用户升级后
     * 看到的还是中文** —— 语言偏好不该因为升级被重置。
     *
     * 存短码而不是数字：config.json 是给人看、给人手改的文件，
     * 写 "lang": "en" 一眼就知道是什么，而写 "lang": 1 得去猜枚举序。
     * 同时它天然给了"以后加第三种语言"的余地 —— 加语言不必改这个字段的语义。 */
    char  lang[8];

    /* 主题色（强调色）名字："teal"/"blue"/…，见 widgets.c 的 ACCENTS。
     * 存名字不存下标：**下标会随颜色表增删而错位**，而配置文件要跨版本读。
     * 认不出的名字一律按 teal 处理（widgets 的 w_accent_find 返回 0）。 */
    char  accent[12];
} UiCfg;

/* 界面护栏（分层设置，见 设计规划.md §6）。
 * 这些不是"功能开关"，而是**限制程序自己能做什么**的闸门 ——
 * 默认全开（最保守），用户可以逐条放松，但放松的每一项都要能单独看见。
 * 放 config.json 里而不是藏进代码：用户有权知道有哪些限制在约束他。 */
typedef struct {
    bool confirm_overwrite;   /* 写文件前一律确认（不能关，留字段是为了显示"不可关闭"） */
    bool confirm_delete;      /* 删除前确认 */
    bool confirm_bulk;        /* 批量操作（>1 个文件）额外确认 */
    int  max_write_kb;        /* 单次写入上限（KB），<=0 表示不限 */
    int  max_batch_files;     /* 单次批量操作的文件数上限，<=0 表示不限 */
} GuardCfg;

/* 诊断/开发用的显示开关。它们只影响 status.txt 与界面提示，
 * 不改变任何实际行为 —— 所以可以安全地让用户随便开。 */
typedef struct {
    bool show_layout_overflow;  /* 界面上直接提示"这行超宽了"（回归用） */
    bool show_notes;            /* 界面上展开 status.txt 的备注区 */
    bool verbose_status;        /* status.txt 写更多字段 */
} DiagCfg;

/* OTA 远程升级。注意这里**不存更新源 URL** ——
 * 更新源是编译期常量（见 app/source/ota/ota.c 的 kUpdateBase[]），
 * 放进用户可编辑的 JSON 等于给中间人留了个后门。 */
typedef struct {
    bool enabled;         /* 总开关，用户可关 */
    char channel[12];     /* "stable" / "beta" */
    bool auto_check;      /* 启动时自动检查（只提示，永不自动装） */
    int  last_check;      /* unix 秒；0 = 从未检查过 */
    int  skipped_version; /* 用户主动跳过的版本号，别再烦他 */
} OtaCfg;

typedef struct {
    int version;
    bool first_run_completed;
    bool tutorial_completed;   /* 新手教程（首启流程里那几屏） */
    int  launches;

    NandBackupCfg nand_backup;
    SecurityCfg   security;
    AiCfg         ai;
    UiCfg         ui;
    GuardCfg      guard;
    DiagCfg       diag;
    OtaCfg        ota;
} AppConfig;

/* 上一次 config_load 的结果。UI 与自检都要用它说人话 ——
 * "读不到文件"和"文件坏了"是完全不同的两件事。 */
typedef struct {
    bool file_present;
    bool parse_ok;
    int  version_seen;    /* 文件里声明的 version；缺省 -1 */
    int  unknown_keys;    /* 不认识的键数：通常意味着文件比程序新 */
    int  bad_values;      /* 解析成功但取值越界的键数 */
} ConfigLoadInfo;

const char *security_level_name(int level);

/* 思考强度的显示名（"关" / "低" / "中" / "高"）。
 * 和 security_level_name 放在一起：都是"把配置里的数字翻成人话"，
 * 散到各自界面去写就会出现两个屏说的不一样。越界一律返回"关"。 */
const char *thinking_name(int level);

/* 路径（都是常量字符串，别 free） */
const char *config_dir(void);
const char *config_path(void);
const char *config_creds_path(void);   /* sdmc:/3ds/aiconsole/creds.enc，M5 才写 */

/* 填默认值。DeepSeek 是 设计规划.md §9 建议的默认服务商。 */
void config_defaults(AppConfig *cfg);

/* 读盘。文件不存在或解析失败都会填默认值并返回 false；
 * 具体原因看 config_last_load()。 */
bool config_load(AppConfig *cfg);

/* 写盘（先建目录，再原子落盘：写 .tmp 再 rename，避免断电写坏半个文件）。 */
bool config_save(const AppConfig *cfg);

bool               config_exists(void);
const ConfigLoadInfo *config_last_load(void);

/* 是否已经配好 API Key —— 判据是 creds.enc 在不在，
 * 而不是在 config.json 里再存一个布尔，免得两处状态会不一致。 */
bool config_ai_configured(void);

/* 把 security.current_level 夹到合法范围（读盘后调用）。 */
void config_clamp(AppConfig *cfg);

/* 字号档位（small / mid / large）→ 渲染缩放百分比（100 / 125 / 150）。
 * 只影响阅读区（对话正文、文档浏览），不动按钮与表格。 */
int  config_text_scale(void);
