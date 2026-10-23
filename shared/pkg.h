/*
 * pkg.h -- 安装软件包（把 .3dsx 装到卡上）
 *
 * ── 为什么单独一个模块，而不是并进 ota.c ──
 * ota.c 管的是"**我自己**要不要升级"：它比对 version_code、拒绝降级包、
 * 覆盖 sdmc:/3ds/AIConsole.3dsx。那套逻辑只对一个程序有意义。
 *
 * 这里管的是"**装一个别的**程序"：它不关心版本号大小（用户想装什么就装什么，
 * 包括降级），落点是按包名算出来的另一个路径，而且要能被 AI 以动作的形式请求。
 * 两者的判据、护栏、失败理由都不一样，混在一起会让"更新自己"这条已经很
 * 要紧的链路变脆。
 *
 * ── 3DS 上"安装"到底能做到哪一步 ──
 * 三种包，能力差别巨大：
 *
 *   1. .3dsx  自制程序。放进 sdmc:/3ds/ 就能被 Homebrew Launcher 看见。
 *             **只需要 SD 卡写权限** —— 本程序现在就有。✅ 这条路通，
 *             实现是 pkg_install()（搬文件 + 核摘要）。
 *   2. .cia   正式感的应用（会出现在主菜单）。要调 AM_StartCiaInstall。
 *             ✅ 这条路现在也通了，实现是 pkg_install_cia()。
 *             它能通的原因值得写清楚，因为先前这里有**两版错误说法**：
 *
 *               · 第一版说"自制程序拿不到 am:u，exheader 编译期烤死"——
 *                 没实测过，是推测。
 *               · 第二版说"Azahar 里 AM_StartCiaInstall 返回 0，说明有权限"——
 *                 这是**假阳性**：Azahar 的 am.cpp 不检查调用者 exheader，
 *                 对谁都返 0（与 httpc 证书那条是同一类坑）。
 *
 *             真正的原因在 Luma3DS 的 loader 里，判据只有一个：
 *                 if (hbldrIs3dsxTitle(title_id)) hbldrPatchExHeaderInfo(...);
 *                 else { ...用这个 title 自己的 exheader... }
 *             而 hbldrIs3dsxTitle 比的是**可配置的** hbldr_tid（默认
 *             hblauncher）。3dsx 走前者 → 服务表被 Luma 的 34 项硬编码
 *             表覆盖（无 am:u）；我们自己打的 .cia，UniqueId 0xA1C01
 *             不等于 hbldr_tid → 走后者 → **用 AIConsole_cia.rsf 里的声明**。
 *
 *             所以 FBI 能装 CIA 不是什么魔法 exheader，它只是同样走了
 *             "用自己 exheader 的声明"这条路。我们不需要抄它。
 *             依据：LumaTeam/Luma3DS master 的 hbldr.c / hbldr.h / loader.c。
 *             ⚠️ 源码阅读所得，真机未实测 —— 用户有破解机，待实测。
 *   3. 装完之后"自己启动它"：要 APT:U 的 chainload，而 chainload 目标
 *             同样受 exheader 约束。所以只能写一个直达图标 + 告诉用户
 *             路径，让他去 Launcher 里点。
 *
 * 于是本模块做第 1 条与第 2 条，并把第 3 条的**真实限制如实说出来** ——
 * 界面和审计里都不许出现"已启动它"这种话，因为那还没实现。
 *
 * 【教训】这里先后写过两版错话，一版是推测当结论，一版是把假阳性当证据。
 * 凡是"能不能"的问题，要么实测、要么标明是推测 —— 写在注释里也一样，
 * 因为下一个人会当结论用。
 *
 * ── 四道闸在这里的落点（与 actions.c 同源，一条不少）──
 *   ① AI 只能给"包名"，给不了路径；
 *   ② 包名要过编译期的字符校验（无分隔符、无盘符、无 ..）；
 *   ③ 真正写盘前必须过物理确认（界面的事，见 sc_confirm.c）；
 *   ④ 目标路径由这里拼，并且**拼完核对前缀** —— 永远出不了 PKG_DIR。
 *
 * ── 为什么装之前一定要核 sha256 ──
 * "安装"是这台机器上**副作用最大**的一个动作：装进去的东西下次会被执行。
 * 所以摘要不符时必须拒绝，而不是"警告一下继续"。这一条与 ota_verify_one
 * 是同一个判据、同一份实现（sha256_file），不另起一套。
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>

/* 安装落点。Homebrew Launcher 扫的是 sdmc:/3ds/ 下的 *.3dsx。 */
#define PKG_INSTALL_DIR "sdmc:/3ds"

/* 包名（纯名字）与显示名的上限 */
#define PKG_NAME_MAX 60
#define PKG_DESC_MAX 96

/* 单包大小上限。装的是可执行文件，几 MB 是正常的；
 * 但它同时也是一道闸 —— 免得一个 2GB 的文件把卡写满才发现。 */
#define PKG_MAX_BYTES (32 * 1024 * 1024)

typedef enum {
    PKG_OK = 0,
    PKG_ERR_NO_FILE,        /* 源文件不在 */
    PKG_ERR_NAME,           /* 包名不合法（穿越 / 空 / 太长） */
    PKG_ERR_TOO_BIG,        /* 超过上限 */
    PKG_ERR_EMPTY,          /* 0 字节 —— 不像一个程序 */
    PKG_ERR_HASH,           /* 摘要对不上（**最要紧的一条**） */
    PKG_ERR_IO,             /* 读写失败 */
    PKG_ERR_ALREADY,        /* 目标已存在且调用方要求不覆盖 */
    PKG_ERR_NO_PERM,        /* 没有权限。对 .cia 来说最常见的原因是
                             * **当前跑的是 3dsx 形态**，拿不到 am:u ——
                             * 这与"包坏了"是完全不同的两件事，界面上
                             * 必须分得开，否则用户会去查一个没坏的包。 */
} PkgState;

const char *pkg_state_text(PkgState s);

/* 源文件放哪：sdmc:/3ds/aiconsole/pkgs/<name>.3dsx
 * 用户把下载好的 .3dsx 丢进去，本程序负责"装"（= 搬进 3ds/ 并留痕）。 */
const char *pkg_src_dir(void);
const char *pkg_install_dir(void);

/* 把包名算成源路径。返回值是 bool：false = 名字不合法，dst 已被清空。
 * 单独暴露出来，是为了让 UI 能在"确认"之前就显示"它会读哪个文件" ——
 * 用户在按允许之前看得见来源，是确认这件事有意义的前提。 */
bool pkg_src_path(const char *name, char *dst, size_t cap);
/* 同上，算安装目标路径。 */
bool pkg_dst_path(const char *name, char *dst, size_t cap);

/* 校验包名。对外暴露是因为 actions.c 要在**解析阶段**就拒掉坏名字，
 * 而不是等真去写盘才发现 —— 那时候确认屏已经弹过了。 */
bool pkg_name_ok(const char *name);

/* 查一个包有多大（不读内容）。不存在返回 false。 */
bool pkg_size(const char *name, int *out_bytes);

/* 核一个包的 sha256（hex 小写，65 字节含结尾）。
 * want 非空时表示"期望值"，返回 false 表示对不上；
 * want 为空只算不比对（用于 UI 展示"这个包的指纹是…"）。 */
bool pkg_verify(const char *name, const char *want_hex, char *got_hex,
                size_t got_cap);

/* --------------------------------------------------------------------------
 * 安装
 *
 * 这是本模块唯一会产生副作用的地方。它不弹确认 —— 确认由调用方
 * （sc_confirm.c）在**调它之前**完成，因为"谁来确认"是界面的事，
 * 而"确认过了要做什么"是这里的事。
 *
 * 流程：源在不在 → 名字合不合法 → 大小合理吗 → sha256 对得上吗
 *       → 写 .tmp → rename 到目标（先写临时再改名，断电不会留半个程序）
 *
 * msg 填一句人话（会进对话流和审计），失败时必须说得出理由。
 * ----------------------------------------------------------------------- */
PkgState pkg_install(const char *name, const char *want_sha256,
                     char *msg, size_t cap);

/* 列出 pkgs/ 目录里有哪些 .3dsx（给 UI 和 AI 看"卡上有什么可装的"）。
 * 返回写入条数；每条写进 names[i]（cap 大小）。 */
int pkg_list(char names[][PKG_NAME_MAX + 1], int max, int *count_out);

/* ==========================================================================
 * .cia 路线：装到主菜单
 *
 * 与 pkg_install() 的**根本区别**：那条是搬文件（只要 SD 写权限），
 * 这条要跟系统的 am 服务打交道，装完由系统接管、出现在主菜单。
 *
 * 能力前提：**只有 .cia 形态的本程序才拿得到 am:u**。3dsx 形态下
 * Luma 会把服务表覆盖掉，这个函数会在 amInit 那一步失败并如实报错。
 * 所以它自己会先探一次，探不到就明确说"当前形态不行，要用 CIA 形态"，
 * 而不是含糊地说"失败了"。
 *
 * 四道闸与 pkg_install() 完全同源，一条不少，尤其 sha256：
 * 装进主菜单的东西下次会被执行，摘要不符必须拒绝，不是"警告一下继续"。
 * ======================================================================= */

/* 源放哪：sdmc:/3ds/aiconsole/pkgs/<name>.cia
 * 与 .3dsx 同一个目录，靠后缀区分 —— 用户和 AI 都只需记住"丢进 pkgs/"。 */
bool pkg_cia_src_path(const char *name, char *dst, size_t cap);

/* 包名校验（.cia 版）。与 pkg_name_ok 同判据，同样**故意各留一份**。 */
bool pkg_cia_name_ok(const char *name);

/* 查一个 .cia 包多大（不读内容）。不存在返回 false。
 *
 * 为什么要单独一个而不是让 pkg_size 自己猜：两个查大小函数查的是
 * **两个不同目录下的两个不同后缀**。让一个函数"根据名字猜后缀再选目录"
 * 看似省事，但它把"名字决定去向"这件事埋进了函数内部 ——
 * 而名字是模型给的。宁可两处各写 5 行，也不给模型一条能影响
 * "读哪个文件"的隐含通道。 */
bool pkg_cia_size(const char *name, int *out_bytes);

/* 装一个 .cia 到主菜单。
 *
 * 流程：名字 → 源在不在 → 大小 → sha256（对得上才继续）
 *       → amInit → AM_StartCiaInstall → 分块写 → AM_FinishCiaInstall
 * 任一步失败都走 AM_CancelCIAInstall 收拾干净，**不留半个 title**。
 *
 * msg 必须说得出：装了什么、装到哪、以及**下一步要用户自己做什么**。
 * 装完不会自动启动 —— 那需要 chainload，本模块不做（见上面第 3 条）。
 */
PkgState pkg_install_cia(const char *name, const char *want_sha256,
                         char *msg, size_t cap);

/* 当前形态到底有没有 am:u。给 UI 用：没有就**别显示** CIA 安装入口，
 * 或者显示成灰的并说明原因 —— 让用户点了才失败是最差的交互。 */
bool pkg_cia_available(void);
