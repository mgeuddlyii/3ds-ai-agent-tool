/*
 * cupd.h — 内容更新通道（模型 / 攻略库 / 硬件知识库）
 *
 * ══ 为什么要有这个模块，而不是把 ota.c 的产物类型扩一扩 ════════
 *
 * ota.c 与本模块的**根本差别不是文件类型，是失败后果**：
 *
 *   ota.c   装的是程序本体（3dsx / cia）。装坏了 = 软件打不开。
 *           护栏必须最硬：version_code 严格递增、装 CIA 只能落到卡上
 *           让人去 FBI 点、更新永远要人按允许。
 *
 *   本模块 装的是**数据**。模型装坏了 = 检索还在、界面还在、
 *           只是「没有生成」这一层消失 —— 而那一层本来就是
 *           **失败时回退到检索直出**的（见 guide_nn.c 的失败策略）。
 *           也就是说：一次坏更新最坏的结果是「少了个锦上添花的功能」，
 *           而不是「软件变砖」。这是它能比 ota.c 宽松一点的**唯一理由**，
 *           不是因为它不重要。
 *
 * ══ 六条护栏原样继承（设计规划.md §4.8）══════════════════════════
 *
 *   G1 更新源编译期常量      本模块不需要网络，只认卡上的包目录
 *   G2 manifest 无 http 地址  同上 —— 文件名由编译期白名单决定
 *   G3 version 严格递增       每个内容**各自**有版本号，不是共用
 *                             APP_VERSION_CODE。三类内容更新节奏完全
 *                             不同（模型月级、知识库周级、硬件知识年级级），
 *                             共用一个版本号会导致「更新了知识库结果
 *                             提示模型也是新的」这种误导。
 *   G4 size + sha256 双重校验 一条不少。数据文件损坏的后果是
 *                             **静默答错**（BM25 索引烂掉但仍返回
 *                             命中 0 条，看起来像「库里没有」）。
 *   G5 写盘 .tmp + rename     同上，且额外要求**校验通过才 rename**。
 *   G6 安装要人按允许          UI 层面，本模块只提供「装不装」的判断，
 *                             不自动装。
 *
 * ══ 与 ota.c 最实质的一处不同：可回滚 ═════════════════════════════
 *
 * ota.c 直接覆盖目标，**没有备份** —— 因为程序装坏了能用 FBI 重装，
 * 卡上那份坏了就再下一遍。内容文件没有这个退路：
 * 攻略库 408KB，模型 1.25MB，都是手工/训练产出的东西，
 * 卡上那份可能正是唯一一份。
 *
 * 所以本模块的安装是**两段式**：
 *     备份（.bak）→ 写新文件（.tmp → rename）
 * 新文件校验失败或用户事后发现生成质量下降，
 * 把 .bak 改回正式名即可恢复，**不需要重下一遍**。
 *
 * 这一条是「内容更新」与「程序更新」在需求上的真正分歧点。
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>

/* 内容种类。**顺序即 kind 字符串的匹配顺序**，别随便改。 */
typedef enum {
    CU_MODEL = 0,      /* guide_model.bin  —— 推理权重 */
    CU_KB,             /* guide_kb.bin     —— BM25 攻略库 */
    CU_HARDWARE,       /* hw_knowledge.txt —— 3DS 硬件知识库 */
    CU_KIND_COUNT
} CuKind;

/* 单个内容产物 */
typedef struct {
    CuKind  kind;
    char    kn[12];       /* 种类短名：model / kb / hardware（**只作解析用**） */
    char    file[48];     /* 包里该产物的文件名（不含路径） */
    int     size;
    char    sha256[65];
    int     version;      /* 该内容自己的版本号，G3 用它比 */
} CuArtifact;

typedef struct {
    int  schema;
    int  count;
    CuArtifact art[CU_KIND_COUNT];
} CuManifest;

typedef enum {
    CU_NONE = 0,
    CU_NO_PACKAGE,      /* 卡上没有内容更新包     */
    CU_UP_TO_DATE,      /* 全部不比自己新         */
    CU_AVAILABLE,       /* 有新的，尚未校验       */
    CU_VERIFY_OK,       /* 全部校验通过，可装     */
    CU_DONE,            /* 已安装                 */
    CU_ERR_PARSE,       /* manifest 读不出来      */
    CU_ERR_TOO_OLD,     /* 降级包 —— 拒绝          */
    CU_ERR_MISMATCH,    /* 大小或摘要不符          */
    CU_ERR_IO,          /* 读写失败                */
} CuState;

const char *cu_state_text(CuState s);
const char *cu_kind_name(CuKind k);

/* 更新包目录（与 ota_dir 同级但分开，避免两种包混在一起） */
const char *cu_dir(void);

/* 某个种类的正式落盘路径，如 cu_target(CU_MODEL) */
const char *cu_target(CuKind k);

/*
 * 把三类内容的落盘目录改到别处（**只给自测用**，传 NULL 恢复默认）。
 *
 * 为什么需要：cupdtest 要验证「安装 → 校验 → 回滚」的完整闭环，
 * 而这个闭环会写文件、rename、备份 —— 真实路径下没有 .bak 就没法测回滚，
 * 有 .bak 又会污染用户真正的模型和攻略库。
 *
 * ⚠️ 这是个危险口子：它能把内容装到任意目录。只在自测里用，
 * 且只在函数入口设一次、出口清掉。
 * 之所以不把路径全部参数化（给每个函数加一个 ctx）：
 * 生产代码里只有一个调用点，参数化只会让"忘记传对"从编译期
 * 可查的事变成运行期才暴露的事。两害相权取其轻。
 */
void cu_set_root(const char *root);

/* 解析 manifest 文本。结构不对返回 false。 */
bool cu_parse(const char *text, size_t n, CuManifest *m);

/*
 * 与当前各内容版本比较，**返回需要更新的种类个数**（0 = 无更新）。
 *
 * ⚠️ 这里刻意不复用 ota_compare 那个「1/0/-1」的返回式：
 * 内容是三类独立的，各自独立地新旧不同。「有没有更新」这个问题
 * 在内容场景下的答案是**一个集合**，不是布尔。压成布尔就必须挑一个
 * 语义（取新？取旧？），而无论挑哪个都会在 UI 上显示错的东西。
 */
int cu_scan_local(CuManifest *m, CuState *state, char *detail, size_t cap);

/* 装第 idx 个产物。先备份旧文件，再写新文件（校验通过才 rename）。 */
/* 按**种类**查它在 art[] 里的下标；没有这一类返回 -1。
 *
 * ★ 必须用这个，不能拿 CuKind 直接当数组下标（B-08）：
 *   art[] 是按 manifest 里 artifacts 数组的**出现顺序**填的，
 *   只有"三种齐全且顺序正好是 model/kb/hardware"时才碰巧与 CuKind 对齐。
 *   子集（比如只发了一个 kb）或乱序时，
 *   `art[CU_MODEL]` 读到的是别的产物（版本号对不上、按 A 装错文件），
 *   而行数不够的那些行会被判"不能装"—— 用户看到"装完没反应 / 单行装不了"。 */
int  cu_index_of_kind(const CuManifest *m, CuKind kind);

bool cu_install(const CuManifest *m, int idx, char *msg, size_t cap);

/* 回滚第 idx 个产物：用 .bak 覆盖回正式名。没有 .bak 返回 false。 */
/* 第二个参数是 **CuKind**，不是数组下标（回滚只用本机 .bak，不需要 manifest） */
bool cu_rollback(const CuManifest *m, int kind, char *msg, size_t cap);

/*
 * 卡上那份当前的版本号（0 = 从没装过 / 记录损坏）。
 *
 * 单独暴露是因为 UI 必须能说清「现在是什么版本，能升到哪个」——
 * 只给一个"有更新/没更新"的布尔，用户在按下安装之前无法判断
 * **这次更新值不值得**。对内容更新来说这个信息是决定性的：
 * 知识库从 v1 升到 v2 值得，v2 升到 v2 就不该出现在列表里。
 */
int cu_local_version(CuKind k);

/*
 * 有没有可回滚的备份。
 *
 * 回滚按钮**必须按这个来禁用**，不能只靠 cu_rollback 失败来兜底：
 * 按了没反应是最差的交互 —— 用户会以为是自己按错了，
 * 然后反复按。按钮灰掉本身就是答案。
 */
bool cu_has_backup(CuKind k);
