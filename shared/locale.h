/*
 * locale.h -- 给游戏设区域与语言（Luma3DS 的 locale emulation）
 *
 * ── 这个模块做什么 ──
 * 往 SD 卡的  sdmc:/luma/titles/<游戏ID>/locale.txt  写一行两个词，比如
 *       USA EN
 * 下一次启动那个游戏时，Luma3DS 会读到它，把"这台机器是哪个区、说什么语言"
 * 按这两个词告诉游戏。用来解决"日版游戏在你美版机上因为找不到日文字库而崩"
 * 这类问题。
 *
 * ── 为什么这件事安全（与字库切换正相反） ──
 * 它**只在 SD 卡上写一个几字节的文本文件**：
 *   · 不碰 NAND、不碰系统文件、不调任何特权服务；
 *   · 不需要 am:u，也不需要任何 3DS 特有的权限 —— 就是普通文件写；
 *   · 删掉这个文件（或按 SELECT 那种"还原"）就完全恢复原状，无残留；
 *   · Luma 自己实现了它，我们只是往 Luma 约定的位置放文件。
 * 所以它能在 3dsx 形态下正常使用，也能在模拟器里把"写对了没有"验清楚
 * （唯一验不了的只有"Luma 认不认"——那是真机上的事，见文档）。
 *
 * ── 与字库切换的分界（这是本模块存在的理由之一）──
 * 用户常把这两件事当成一类"区域相关的汉化工具"，但它们的代价差了一个数量级：
 *   · 区域切换 = 写一个文本文件，随时可删       ← 本模块，随便用；
 *   · 字库切换 = 内存改写系统共享字库区         ← 见 shfont.h，有崩溃风险。
 * 界面上必须把这两件事分开说，不能让用户以为"都是改改设置"。
 *
 * ── 四道闸在这里的落点 ──
 *   ① AI 只能给「游戏 ID + 区域 + 语言」三个**值**，给不了路径；
 *   ② 三者都必须过编译期的白名单校验（游戏 ID 必须是 16 位十六进制）；
 *   ③ 写盘前过物理确认（sc_confirm.c）；
 *   ④ 路径由这里拼，拼完核对前缀 —— 永远出不了 LO_DIR。
 *      **这一条尤其要紧**：游戏 ID 会被拼进路径，如果它是模型随手给的
 *      一串 "../../.."，那就成了路径穿越。所以 ID 用"只允许 [0-9A-F] 且
 *      恰好 16 位"来校验 —— 比黑名单更严，因为这里的输入形态是可穷举的。
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>

/* 游戏 ID 是 16 位十六进制，例如 0004000000055D00（口袋妖怪 X）。 */
#define LOC_TID_HEX 16
#define LOC_TID_MAX (LOC_TID_HEX + 1)   /* 含结尾符 */

/* locale.txt 会写进 luma/titles/<TID>/ 下。Luma 约定的位置，不要改。 */
#define LOC_DIR "sdmc:/luma/titles"

typedef enum {
    LOC_OK = 0,
    LOC_ERR_TID,        /* 游戏 ID 不是 16 位十六进制 */
    LOC_ERR_REGION,     /* 区域不在白名单里 */
    LOC_ERR_LANG,       /* 语言不在白名单里 */
    LOC_ERR_IO,         /* 建目录 / 写文件失败 */
    LOC_ERR_VERIFY,     /* 写完了读回来对不上 —— 这个必须报，不能当成功 */
    LOC_ERR_NONE,       /* 想撤销，但本来就没设过 */
} LocState;

const char *loc_state_text(LocState s);

/* 校验。三个都用编译期表比对，不做任何"看起来像"的猜测。
 * tid_ok 只要求"16 位且都是十六进制数字"（大小写都收，内部统一转大写）。 */
bool loc_tid_ok(const char *tid);
bool loc_region_ok(const char *region);
bool loc_lang_ok(const char *lang);

/* 把区域字符串规范成 Luma 认的大写形式（JPN/USA/EUR/AUS/CHN/KOR/TWN）。
 * 合法则返回 true 并把大写形式写进 out。 */
bool loc_region_norm(const char *in, char *out, size_t cap);
bool loc_lang_norm(const char *in, char *out, size_t cap);

/* 算出这个游戏的 locale.txt 完整路径：sdmc:/luma/titles/<TID>/locale.txt
 * 返回 false = 游戏 ID 不合法（dst 已清空）。
 * 单独暴露是为了让确认屏能在**按允许之前**显示"它会写哪个文件"。 */
bool loc_path(const char *tid, char *dst, size_t cap);

/* 读一个游戏当前的设置。没设过返回 false。
 * 设过则把两个词写进 region/lang（各自 cap 字节）。 */
bool loc_read(const char *tid, char *region, size_t rcap,
              char *lang, size_t lcap);

/* --------------------------------------------------------------------------
 * 设置 / 撤销（本模块唯一的副作用点）
 *
 * 与 pkg.c 一样，这里**不弹确认** —— 确认由调用方先做完，因为
 * "谁来确认"是界面的事，"确认过了要做什么"是这里的事。
 *
 * 设置：建目录 → 写 "REGION LANG\n" → **读回来核对**
 * 撤销：删掉 locale.txt → 连带把空掉的目录也删掉（不留一堆空目录）
 *
 * msg 填一句人话，失败时必须说得出理由。
 * ----------------------------------------------------------------------- */
LocState loc_set(const char *tid, const char *region, const char *lang,
                 char *msg, size_t cap);

LocState loc_clear(const char *tid, char *msg, size_t cap);

/* --------------------------------------------------------------------------
 * 列出已经设过 locale 的游戏（给"已设区域"界面和 AI 看）
 *
 * 扫 sdmc:/luma/titles 下的每个游戏目录，找里面的 locale.txt。返回条数。
 * 每条把游戏 ID 写进 tids[i]，紧跟着把读到的那行写进 vals[i]
 * （形如 "USA EN"，给用户核对）。
 * ----------------------------------------------------------------------- */
#define LOC_LIST_MAX 32
int loc_list(char tids[][LOC_TID_MAX], char vals[][24], int max,
             int *count_out);

/* --------------------------------------------------------------------------
 * Luma 的 "Enable game patching" 开没开
 *
 * 这是 Luma3DS 的一个配置项，**关着的话 locale.txt 会被完全忽略** ——
 * 文件安安静静躺在那儿，游戏却毫无变化，用户只会觉得"这工具没用"。
 * 它存在 sdmc:/luma/config.ini 里。我们能读到那个文件，
 * 所以这里做的是：读一次，给出"已开 / 已关 / 读不到"三态。
 *
 * ⚠️ 这是**辅助判断**，不是权威：不同 Luma 版本的键名/写法可能变，
 *    读不到时返回 LOC_PATCH_UNKNOWN，界面照常让用户继续，
 *    只是多一句"如果没生效，去 Luma 配置里确认这项是开的"。
 *    绝不因为"读不到"就拦下操作 —— 那是把我们的不确定变成用户的障碍。
 * ----------------------------------------------------------------------- */
typedef enum {
    LOC_PATCH_UNKNOWN = 0,   /* 没读到 config.ini 或找不到这一项 */
    LOC_PATCH_ON,            /* 明确读到开着 */
    LOC_PATCH_OFF,           /* 明确读到关着 —— 这时候要特别提醒 */
} LocPatchState;

LocPatchState loc_game_patching(void);
