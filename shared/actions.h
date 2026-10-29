/*
 * actions.h -- AI 能请求执行的动作
 *
 * ── 这个模块是干什么的 ──
 * 它是「会说话的助手」变成「agent」的那一步：从说，到做。
 * 同时也是整个项目里**唯一**会让模型的意图变成现实副作用的地方 ——
 * 所以所有的安全约束都集中在这里，一条都不能松。
 *
 * ── 三道闸（设计规划.md §7，顺序不可调）──
 *   1. **模型只能给动作 ID 和参数，不能给代码。**
 *      它写的一切都是**数据**：不是要执行的字符串，不是路径，不是命令。
 *      只要开一个"照着它说的做"的口子，前面所有的安全设计都白搭。
 *
 *   2. **动作必须在编译期白名单里。**
 *      没登记的一律拒绝 —— 而且**拒绝也要留痕**：
 *      "模型试图做一件它做不到的事"是重要的安全信号，不是噪音。
 *
 *   3. **执行前必须过物理确认。** 确认框的标题和按钮文案是编译期常量，
 *      绝不使用模型给的文本 —— 否则模型能把确认框伪装成别的样子，
 *      让用户以为自己在批准另一件事。
 *
 * ── 第四条是本模块自己的 ──
 *   4. **路径不来自 AI。** 模型只给一段"纯名字"，最终路径由这里拼到
 *      沙箱目录下。它永远够不着目录之外，也就永远碰不到 NAND、
 *      碰不到系统文件 —— 这条不靠"模型守规矩"，靠的是拼路径的人不是它。
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "locale.h"     /* LOC_TID_MAX —— ActionReq 要按它开字段 */

/* 文件名与内容的上限。内容 512 字节是刻意的：
 * 一次动作写太多，人就没法在确认框里核对它到底写了什么。 */
#define ACT_NAME_MAX 60
#define ACT_TEXT_MAX 512

typedef enum {
    ACT_NONE = 0,        /* 这条回复里没有任何动作 */
    ACT_UNKNOWN,         /* 有动作，但不在白名单里 —— 照样要弹确认并留痕 */
    ACT_FILE_WRITE,      /* 在沙箱目录里创建/覆盖一个文本文件 */
    ACT_PKG_INSTALL,     /* 把一个已下载到 pkgs/ 的包装到机器上 */
    ACT_LOCALE_SET,      /* 给某个游戏设区域/语言（写 luma/titles/<TID>/locale.txt） */
    ACT_LOCALE_CLEAR,    /* 撤销某个游戏的区域设置（删掉那个文件） */

    /* ---- 设置类（2026-10-05）----
     *
     * 为什么把"改设置"也做成动作：设置**不该有页面**。
     * 用户跟 AI 说"换成黑夜模式"和自己在设置屏里翻到那一项是同一件事，
     * 后者却要多记"设置项在第几行、叫什么名字"。
     * 这一组做完，SCREEN_SETTINGS 就可以整个撤掉。
     *
     * 全部只改 config.json 里的**值**，不动任何文件：
     * 越权面比 file.write 小得多，但**仍然要过确认屏** ——
     * 安全等级那一条尤其要（它决定 AI 能动到哪一层）。 */
    ACT_UI_THEME,        /* 界面主题：light / dark */
    ACT_UI_FONTSIZE,     /* 字号：small / mid / big */
    ACT_UI_DEPTH,        /* 立体深度：0–100 */
    ACT_SEC_LEVEL,       /* 安全等级：L0 / L1 / L2 / L3 */
    ACT_CFG_SET,         /* 其余设置项：白名单键 + 值 */

    /* ---- 只读类（2026-10-05）----
     *
     * 它们**不改任何东西**，但没有它们这套动作就是残的：用户问
     * "我卡上有什么"、"我是新3DS还是旧3DS"，AI 只能靠猜。
     *
     * 结果怎么回到模型：执行结果会被存进 app.c 的回灌缓冲，
     * 下一轮作为 system 消息的一部分发回去（见 sc_chat.c）。
     * 这条通路原先不存在，是先补上才加的这些动作。 */
    ACT_SYS_INFO,        /* 这台机器与本软件的状态（无参数） */

    /* ---- 沙箱只读（2026-10-05）----
     *
     * 与 sys.info 同属只读，但要看东西：列沙箱里有什么、读一个文本文件回来。
     * 两者都**只够得着沙箱目录**（out/ 与它的固定子目录），
     * 路径由这里的白名单拼，模型给不出目录之外的东西。
     *
     * ⚠️ dirent 在 sdmc: 上的可用性**尚未在真机验证过**（Azahar 上也没验），
     * 所以 opendir 失败必须是一条正常的"读不到"，不是崩溃。 */
    ACT_FILE_LIST,       /* 列一个沙箱子目录；sub= 取白名单里的子目录名 */
    ACT_FILE_READ,       /* 读一个沙箱里的文本文件；name= 纯文件名 from= 起始字节 */

    /* ---- 安装类（2026-10-05）----
     *
     * 这一组是"AI 帮用户装东西"，取代三个独立页面：
     * 内容更新屏（装模型/攻略库/硬件库）、更新屏的字库那一项。
     *
     * 它们与 pkg.install 的区别：pkg.install 装的是**程序**（进 3ds/ 或主菜单），
     * 这一组装的是**数据**（字库、模型、词表），落到程序自己的数据目录。
     * 分成两个动作而不是一个：落点与回滚方式完全不同，
     * 混在一个动作里会让确认屏没法把风险说清楚。 */
    ACT_FONT_INSTALL,    /* 换字库：fonts/ 里挑一个装上 */
    ACT_MODEL_INSTALL,   /* 装/回滚离线内容：kind= 类别 idx= 第几条 */

    /* ---- 文件与凭据（2026-10-05）----
     *
     * 这一组是**唯一带不可逆后果**的动作，所以三条额外规矩：
     *
     * 1. 删除一律**移进回收站**（out/.trash/），不真删。
     *    真删在这个平台上救不回来，而 AI 判断"这个文件该删"出错的概率
     *    远高于用户点"允许"出错的概率。
     * 2. **没有 key.set** —— 模型不能写凭据。
     *    模型无法验证一个 Key 有没有效，而它很可能**编一个看起来像的**写进去；
     *    用户看到"已配置"却始终连不上，排查方向会被带偏。
     *    写入凭据必须由用户在 Key 屏亲自导入文件。
     *    只给"删"与"切"，这两个方向不需要模型提供新的秘密。
     * 3. 不许把当前唯一一把 Key 删掉 —— 那会让用户彻底没法对话，
     *    而他当初要 AI 删多半只是想去掉一把多余的。 */
    ACT_FILE_DELETE,     /* 移进回收站：name= 纯文件名 */
    ACT_FILE_RENAME,     /* 沙箱内重命名：name= 旧名 to= 新名 */
    ACT_KEY_REMOVE,      /* 删掉一把 Key：name= 条目名 */
    ACT_KEY_SWITCH,      /* 切到某一把：name= 条目名 */
    ACT_TRASH_PURGE,     /* 清空回收站 —— **真删，不可恢复** */
    ACT_SVC_SET,         /* 切服务商：name= 服务商名 */

    /* ★ 2026-10-29：AI 主动提问。
     * 它**不是**要动系统的动作，所以不走确认屏 —— 收到就直接把下屏切成
     * 选项页，用户点一项，那一项会填进输入框（还要用户自己按发送）。
     * 为什么不自动发：AI 会问、也可能问歪，自动发等于模型自己跟自己说话。 */
    ACT_ASK,

    ACT_COUNT,
} ActionId;

typedef struct {
    ActionId id;
    char     name[ACT_NAME_MAX + 1];   /* 纯文件名，不含任何路径成分 */
    char     text[ACT_TEXT_MAX + 1];   /* 要写进去的内容（仅 file.write 用） */
    char     sha256[65];               /* 期望摘要（仅 pkg.install 用，可空） */

    /* ---- 仅 locale.* 用 ----
     * 三个字段都是**值**，不是路径：游戏 ID、区域码、语言码。
     * 路径由 locale.c 自己拼，模型碰不到它 —— 这是第 4 道闸（路径不来自 AI）
     * 在这条动作上的落点。tid 恰好 16 位十六进制，region/lang 各是
     * 2-3 个字母的白名单值，三者的合法形态都是可穷举的，
     * 所以校验用的是白名单而不是黑名单（见 locale.c 的说明）。 */
    char     tid[LOC_TID_MAX];
    char     region[8];
    char     lang[8];

    /* ---- 设置类动作用 ----
     *
     * 统一是 key + value 两个**值**，不是路径：
     * 键要么是编译期白名单里的常量名（cfg.set），
     * 要么由动作名本身决定（ui.theme 的 key 恒为 "theme"）。
     * 落盘路径永远由 config.c 自己拼，模型碰不到它 ——
     * 与 locale.* 同属第 4 道闸（路径不来自 AI）。 */
    char     key[40];
    char     value[32];
    char     to[ACT_NAME_MAX + 1];   /* 仅 file.rename 的新名 */
    char     question[160];          /* 仅 ask：它想问什么（给人看） */
    char     options[128];           /* 仅 ask：用 | 分隔的选项，最多 4 项 */
} ActionReq;

const char *actions_dir(void);     /* sdmc:/3ds/aiconsole/out —— 文件只能落这儿 */
const char *actions_log(void);     /* sdmc:/3ds/aiconsole/action.log */

/* --------------------------------------------------------------------------
 * 设置类动作：**只校验，不落盘**
 *
 * 为什么拆成两步（校验在 shared、落盘在 app）：
 *   改配置要调 app_cfg() 与 w_theme()，那两个在 app 层（app.h / widgets.h）。
 *   shared 不该反过来依赖 app —— 那样 shared 里的东西就再也不能被别的
 *   前端复用了。所以这里只回答"这个键、这个值合不合法"，并把**规范化后**
 *   的值回填给调用方；真正写 config、切主题由 app 层做。
 *
 * 校验用白名单而不是黑名单：cfg.set 只认表里那十个键，
 * 表外的键（含模型自己编的）一律拒绝 —— 与 locale.* 同一个思路，
 * 穷举"只放行这一种形态"比"挡住几种坏形态"可靠。
 * ----------------------------------------------------------------------- */

/* 这个 id 是不是设置类动作（ui.* / sec.level / cfg.set）。*/
bool actions_is_setting(ActionId id);

/* 只读类（不改任何东西）。它们仍然要过确认屏：用户有权知道
 * "AI 正在读我的机器信息"，哪怕它读了什么并不改变。 */
bool actions_is_readonly(ActionId id);

/* 只读动作的**参数**校验（与设置类各走各的白名单）。
 * 沙箱路径由这里的子目录白名单拼出来 —— 模型只能挑一个目录名，
 * 拼不出目录之外的东西（第 4 道闸在只读侧同样成立）。 */
bool actions_check_readonly(const ActionReq *req, char *msg, size_t msg_cap);

/* 文件与凭据类（会动文件，或动凭据）。与前四类分开是因为
 * 确认屏必须为它单独措辞 —— "移进回收站"和"立即消失"是一回事吗？不是。 */
bool actions_is_fileop(ActionId id);

/* svc.set 的**参数校验**（服务商名必须在白名单里）。
 * 它单独一个函数而不是塞进 cfg.set —— 服务商的切换要连带改
 * model 与 Key 仓库的激活项，那是一整套联动，混进"改一项设置"里
 * 会被当成普通字段写盘，从而丢掉联动。 */
bool actions_check_svc(const ActionReq *req, char *msg, size_t msg_cap);

/* ★ 需要**两次**确认的动作（真删，无可挽回）。
 * 单独列成一个函数而不是在确认屏里 if 某几个 id ——
 * 以后每加一个不可逆动作，都必须在这里显式登记；
 * 漏登记的后果是它只被确认一次，而用户未必看清自己按了什么。 */
bool actions_needs_double_confirm(ActionId id);;

/* 文件与凭据类的参数校验。 */
bool actions_check_fileop(const ActionReq *req, char *msg, size_t msg_cap);

/* 安装类动作的**归类**：它们既不是设置也不是只读。
 * 单独一个谓词是为了让确认屏能对上"这会改动系统"这三类文案，
 * 而不是靠 else 兜底 —— 兜底的后果是新加一类动作时它悄悄走了错的那支。 */
bool actions_is_install(ActionId id);

/* model.install 能装哪几类内容（NULL = 到头了）。导出给 app 层，
 * 好让"手册里教模型的取值"和"代码里认的取值"是同一张表。 */
const char *actions_model_kind(int i);

/* 安装类动作的参数校验。与设置类各走各的白名单。 */
bool actions_check_install(const ActionReq *req, char *msg, size_t msg_cap);


/* file.list 能列哪几个子目录（NULL = 到头了）。导出给 app 层，
 * 好让"校验用哪张表"和"实际打开哪张表"永远是同一张。 */
const char *actions_sandbox_subdir(int i);

/* 校验并规范化。合法返回 true，out_value 拿到可以直接写进 config 的值；
 * 非法返回 false，msg 里说清"哪个键/值不合法、合法的是什么"。
 * 注意：它**不会**改任何配置。 */
bool actions_check_setting(const ActionReq *req, char *out_value, size_t cap,
                           char *msg, size_t msg_cap);

/* cfg.set 的键白名单（NULL 结尾）。导出来是为了让 app 层知道
 * "这个键能不能映射到 config 里的哪个字段"，避免两处各写一张表。 */
const char *actions_cfg_key_at(int i);       /* 键名，NULL 表示到头了 */
bool        actions_cfg_key_is_bool(int i);  /* 该键取 on/off 还是整数 */

/* --------------------------------------------------------------------------
 *
 *   标记格式：  [[ACT:file.write name=hello.txt]]正文[[/ACT]]
 *               [[ACT:pkg.install name=myapp sha256=<64位hex>]][[/ACT]]
 *               [[ACT:locale.set tid=<16位hex> region=USA lang=EN]][[/ACT]]
 *               [[ACT:locale.clear tid=<16位hex>]][[/ACT]]
 *               [[ACT:ui.theme value=dark]][[/ACT]]
 *               [[ACT:ui.fontsize value=big]][[/ACT]]
 *               [[ACT:ui.depth value=60]][[/ACT]]
 *               [[ACT:sec.level value=L1]][[/ACT]]
 *               [[ACT:cfg.set key=guard.confirm_delete value=off]][[/ACT]]
 *               [[ACT:sys.info]][[/ACT]]
 *               [[ACT:file.list sub=out]][[/ACT]]
 *               [[ACT:file.read name=notes.txt from=0]][[/ACT]]
 *               [[ACT:font.install name=myfont.ttf]][[/ACT]]
 *               [[ACT:model.install kind=model idx=0]][[/ACT]]
 *               [[ACT:file.delete name=old.txt]][[/ACT]]
 *               [[ACT:file.rename name=a.txt to=b.txt]][[/ACT]]
 *               [[ACT:key.remove name=old-key]][[/ACT]]
 *               [[ACT:key.switch name=my-key]][[/ACT]]
 *               [[ACT:trash.purge]][[/ACT]]
 *               [[ACT:svc.set name=doubao]][[/ACT]]
 *
 * 没找到动作 → 返回 ACT_NONE，clean 就是原文（照常显示）。
 * 找到了     → clean 是不含标记的正文，动作填进 out。
 *
 * 剥离是必须的：用户不该看见这串内部协议，而正文本身要完整保留 ——
 * 模型常常会先解释一句再给动作。
 *
 * pkg.install 的 sha256 是可选的：**有就一定要核**（见 pkg.c），
 * 没有则只做名字与大小校验，并在确认屏上把"这个包没带摘要"说出来。
 * 之所以允许它缺席，是因为用户自己放进去的包未必有人给他算摘要；
 * 但"没摘要"这件事必须被显示，不能默认成"那就是安全的"。
 * ----------------------------------------------------------------------- */
ActionId actions_parse(const char *reply, char *clean, size_t clean_cap,
                       ActionReq *out);

/* --------------------------------------------------------------------------
 * 编译期常量的人话
 *
 * 这两个函数**只**能返回源码里写死的字符串。界面上所有解释"这次要做什么"
 * 的文字都从这儿取 —— 哪天有人把它们改成"返回模型给的摘要"，
 * 第 3 道闸就废了，而且界面看上去还是对的。
 * ----------------------------------------------------------------------- */
const char *actions_verb(ActionId id);     /* "创建一个文件" */
const char *actions_detail(ActionId id);   /* 它到底会动什么，一句话 */

/* --------------------------------------------------------------------------
 * 待确认的动作
 *
 * 屏接口没有参数通道（见 screen.h），而同一时刻只可能有一个待确认的动作
 * （确认本身是阻塞式的），所以放这儿而不是硬塞进屏幕的参数里。
 * ----------------------------------------------------------------------- */
void actions_set_pending(const ActionReq *req);
const ActionReq *actions_pending(void);
void actions_clear_pending(void);

/* --------------------------------------------------------------------------
 * 执行
 *
 * 成功返回 true，msg 是给人看的结果（会进对话流/状态行）。
 * 失败也必须给出**说得出口的理由**，不能只是静默 false。
 * ----------------------------------------------------------------------- */
bool actions_run(const ActionReq *req, char *msg, size_t cap);

/* --------------------------------------------------------------------------
 * 审计
 *
 * verdict 用 "ALLOW" / "DENY" / "FAIL"。
 * 写的是**追加**日志 —— 被覆盖掉的审计日志等于没有审计日志。
 * 允许、拒绝、失败三种都要记：只记成功的话，"模型反复试探被拦下的动作"
 * 这件事就永远看不见。
 * ----------------------------------------------------------------------- */
void actions_audit(const char *verdict, const ActionReq *req, const char *detail);
