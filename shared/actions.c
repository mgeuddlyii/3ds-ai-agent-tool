/*
 * actions.c -- 见 actions.h（安全约束写在那儿，改这里前先读一遍）
 */

#include "actions.h"

#include "config.h"
#include "locale.h"
#include "pkg.h"
#include "provider.h"
#include "util.h"
#include "uistr.h"

#include <stdio.h>
#include <stdlib.h>   /* strtol —— 严格解析整数（自己先逐位验过字符） */
#include <string.h>
#include <strings.h>   /* strcasecmp —— 判 .cia 后缀用（大小写不敏感） */

#define ACT_DIR "sdmc:/3ds/aiconsole/out"
#define ACT_LOG "sdmc:/3ds/aiconsole/action.log"

#define MARK_OPEN  "[[ACT:"
#define MARK_END   "]]"
#define MARK_CLOSE "[[/ACT]]"

/* 待确认的动作 */
static ActionReq g_pending;
static bool      g_has_pending;

const char *actions_dir(void) { return ACT_DIR; }
const char *actions_log(void) { return ACT_LOG; }

/* --------------------------------------------------------------------------
 * 编译期常量的人话
 * ----------------------------------------------------------------------- */

const char *actions_verb(ActionId id)
{
    switch (id) {
        case ACT_ASK:          return S(S0984);   /* 提问 */
        case ACT_FILE_WRITE:   return S(S0828);
        case ACT_PKG_INSTALL:  return S(S0829);
        case ACT_LOCALE_SET:   return S(S0830);
        case ACT_LOCALE_CLEAR: return S(S0831);
        /* 设置类的标题必须**说清改什么**，不能只写"改一项设置" ——
         * 确认屏上这是用户唯一能核对的信息。 */
        case ACT_UI_THEME:     return S(S0832);
        case ACT_UI_FONTSIZE:  return S(S0833);
        case ACT_UI_DEPTH:     return S(S0834);
        case ACT_SEC_LEVEL:    return S(S0835);
        case ACT_CFG_SET:      return S(S0836);
        case ACT_SYS_INFO:     return S(S0837);
        case ACT_FILE_LIST:    return S(S0838);
        case ACT_FILE_READ:    return S(S0839);
        case ACT_FONT_INSTALL: return S(S0840);
        case ACT_MODEL_INSTALL: return S(S0841);
        case ACT_FILE_DELETE:  return S(S0842);
        case ACT_FILE_RENAME: return S(S0843);
        case ACT_KEY_REMOVE:  return S(S0844);
        case ACT_KEY_SWITCH:  return S(S0845);
        case ACT_TRASH_PURGE: return S(S0846);
        case ACT_SVC_SET:    return S(S0847);
        case ACT_UNKNOWN:      return "一个不认识的动作";
        default:               return "什么都不做";
    }
}

const char *actions_detail(ActionId id)
{
    switch (id) {
        case ACT_FILE_WRITE:
            return "会在 SD 卡的 3ds/aiconsole/out/ 里写一个文件。"
                   "同名文件会被覆盖，卡上别的东西它碰不到。";
        case ACT_PKG_INSTALL:
            /* 这一条必须把**能力边界**写在用户眼前，而且它已经改过两次，
             * 两次都是因为先前写了没验证过的断言：
             *
             *   第一版："装 CIA 要 am:u，自制程序拿不到" —— 推测当结论。
             *   第二版："Azahar 里 StartCiaInstall 返回 0，所以可能可行" ——
             *            把假阳性当证据（Azahar 的 am 服务不查调用者 exheader）。
             *
             * 现在的说法基于 Luma3DS 源码，且明确区分两种形态：
             *   · 3dsx（本程序的开发形态）：服务表被 hbldr 覆盖，拿不到 am:u；
             *   · cia （app/AIConsole.cia）：用自己 exheader 的声明，有 am:u。
             *
             * 文案不说"做不到"，也不说"什么都能装"——两句都是谎。 */
            return "会把 pkgs/ 里的一个包装到机器上。"
                   ".3dsx 放进 3ds/（Homebrew Launcher 里能看到）；"
                   "装了 .cia 的话它会出现在主菜单里。"
                   "但装 .cia 需要以 CIA 形态运行本程序 —— "
                   "3dsx 形态下系统不给这个权限，那时会明确告诉你是这个原因。";
        case ACT_UNKNOWN:
            /* 这句话本身就是给用户看的风险提示：不认识 = 不会执行，
             * 但"它想干点什么"这件事本身值得知道。 */
            return "这个动作不在白名单里，不会被执行。记录后请按 B 拒绝。";
        case ACT_LOCALE_SET:
            /* 这条动作只写 SD 卡上的一个文本文件，删掉即恢复 ——
             * 与 pkg.install 和（将来的）字库切换比，它是**温和**的那个。
             * 文案要如实说出这个差别，否则用户会把"改区域"和"改系统文件"
             * 当成同一档风险，把两件事都变成不敢碰的。 */
            return "会在 SD 卡的 luma/titles/ 下给这个游戏建一个 locale.txt，"
                   "写上你要的区域和语言。它只影响这一个游戏，"
                   "不碰系统文件；想还原就把那行设置撤掉即可。";
        case ACT_LOCALE_CLEAR:
            return "会删掉这个游戏的 locale.txt，让它回到系统默认的区域语言。"
                   "这个操作没有副作用，删错了重新设一次就行。";

        /* ---- 设置类：三条硬要求 ----
         *   1. 说清**改完会怎样**（用户据此判断要不要改）
         *   2. 说清**能不能改回来**（这是敢不敢点的关键）
         *   3. 不承诺"永久"—— 设置存在卡上，换��会变
         * 这些文案是编译期常量，模型碰不到（actions.h 顶部的第 3 道闸）。 */
        case ACT_UI_THEME:
            return "会把界面配色在白天与黑夜之间切换，立刻生效。"
                   "只改本机设置，不动任何文件；换回来再执行一次即可。";
        case ACT_UI_FONTSIZE:
            return "会改变界面的字号。当前内置字库只有一档字形，"
                   "此项只记录偏好，对现有文字外观没有影响。";
        case ACT_UI_DEPTH:
            return "会改变界面呈现的立体深度。当前版本尚未启用立体渲染，"
                   "调整此项不会看到画面变化。";
        case ACT_SEC_LEVEL:
            return "会改变 AI 能动手的范围：L0 只聊天，L1 可写文件，"
                   "L2 可动系统设置，L3 底层，L4 完整系统，L5 最高。"
                   "**调高会放宽限制**，调回低一级即可恢复；"
                   "此项会被记住，下次启动仍然生效。"
                   "注意：级别再高也不会取消确认步骤 —— "
                   "每一档都要你亲自点允许。";
        case ACT_CFG_SET:
            return "会修改本机的一项设置（护栏、提醒或诊断开关），"
                   "只改配置文件，不动其它文件。此项可改回原值。";
        case ACT_SYS_INFO:
            /* 只读动作的措辞必须让用户看出"它改不了任何东西" ——
             * 确认屏上出现一个不能按"允许"的动作会让人以为程序坏了。 */
            return "只读取这台机器与本软件的状态（机型、启动次数、"
                   "当前服务商、卡上有没有登录态等），不做任何修改。"
                   "读到的内容会在下一轮对话里告诉你。";
        case ACT_FILE_LIST:
            return "只列出沙箱目录里有哪些文件，不打开也不修改。"
                   "文件名会出现在下一轮对话里。";
        case ACT_FILE_READ:
            return "只读取沙箱目录里的一个文本文件（最多几百字节），"
                   "不修改也不删除。读到的内容会出现在下一轮对话里。";
        case ACT_FONT_INSTALL:
            /* 字库是"装了会立刻改变你看到的一切"的那一类 ——
             * 所以必须把三件事说清：只从 fonts/ 里挑、装之前自动备份、
             * 出问题能换回去。少说一句用户就不敢点。 */
            return "会换上你指定的那套字库，界面上的所有文字立刻改变。"
                   "字库只能从 SD 卡的 fonts/ 目录里选（不能指定路径）。"
                   "装之前会自动备份当前这套，换回去随时可以。";
        case ACT_MODEL_INSTALL:
            return "会安装或回滚一份离线内容（模型 / 攻略库 / 硬件知识库）。"
                   "只处理内容更新包清单里列出的那几份，不碰别的文件。"
                   "会先备份再覆盖，出问题能回滚。";
        case ACT_FILE_DELETE:
            /* 措辞必须让用户知道**它去哪了**。说"删除"而实际是移进回收站，
             * 用户会以为找不回来；说"移走"而实际是真删，他就没去回收站找。
             * 两者都要避免，所以直接写成"移到回收站"。 */
            return "会把那个文件移到沙箱的 .trash/ 回收站目录，"
                   "**不会直接消失** —— 你可以在电脑上把它挪回来。"
                   "只处理沙箱 out/ 里的文件，别的目录碰不到。";
        case ACT_FILE_RENAME:
            return "会把沙箱 out/ 里的一个文件改名。内容不会变，"
                   "只是名字换一下。改错了再改回来就行。";
        case ACT_KEY_REMOVE:
            return "会从 Key 仓库里删掉那一条记录（连同它保存的 Key）。"
                   "删掉之后就找不回来了，只能你重新导入。"
                   "最后一把不会被删掉 —— 那会让你彻底没法对话。";
        case ACT_KEY_SWITCH:
            return "会把当前使用的 Key 换成仓库里的另一把。"
                   "之后发消息就用新的那把。原来的还留着，随时能切回去。";
        case ACT_SVC_SET:
            /* 切服务商会连带换模型、切 Key 仓库的激活项 —— 三件事一起发生，
             * 所以文案必须一次说完，否则用户以为"只换了家、别的没动"。 */
            return "会把对话切到另一家服务商，并同时换成它默认的模型。"
                   "如果 Key 仓库里有那家的凭据，会一并切过去；"
                   "没有的话请在「Key 管理」里配置。";
        case ACT_TRASH_PURGE:
            /* 这一句是整个动作里最重要的字。它和 file.delete 的区别全在这里：
             * 那一个移到回收站（还能找回来），这一个**真的没了**。 */
            return "会**永久删除**回收站里的所有文件。\n"
                   "这一次没有任何地方还留着它们 —— 找不回来了。\n"
                   "确定要这么做的话，需要连按两次确认。";
        default:
            return "";
    }
}

/* --------------------------------------------------------------------------
 * 文件名校验
 *
 * 用黑名单而不是白名单：白名单会把中文名和 emoji 一起挡掉，而这里要防的
 * 只是**路径穿越**，那几类字符足够。
 * 注意这层校验与"拼路径的人不是模型"是**两道独立的防线** ——
 * 就算哪天这层被绕过，模型也仍然只能待在沙箱目录里。
 * ----------------------------------------------------------------------- */
static bool name_ok(const char *n)
{
    size_t len;

    if (!n || !*n)
        return false;

    len = strlen(n);
    if (len > ACT_NAME_MAX)
        return false;

    for (const char *p = n; *p; ++p) {
        unsigned char c = (unsigned char)*p;

        if (c < 0x20)                       /* 控制字符 */
            return false;
        switch (c) {
            case '/': case '\\': case ':':  /* 分隔符与盘符 */
            case '*': case '?': case '"':   /* 通配与引号 */
            case '<': case '>': case '|':
                return false;
            default:
                break;
        }
    }

    /* "." 和 ".." 不含分隔符，上面拦不住，单独拒 */
    if (!strcmp(n, ".") || !strcmp(n, ".."))
        return false;

    return true;
}

/* --------------------------------------------------------------------------
 * 解析
 * ----------------------------------------------------------------------- */

/* 动作名匹配：必须**整段相等**，后面要么结束、要么跟一个空格。
 *
 * 为什么不用 strncmp(hdr, name, strlen(name))：那样 "file.writeEvil"
 * 会被认成 file.write。动作名是编译期白名单的入口，宽一格 = 白名单宽一格，
 * 而多出来的那几个字符正是"有人在试探协议"的直接证据。
 * 这种"前缀当全等"的写法在别处也踩过（见 chatapi.c 的 provider 判定）。 */
static bool hdr_match(const char *hdr, const char *name)
{
    size_t n = strlen(name);

    if (strncmp(hdr, name, n) != 0)
        return false;
    return hdr[n] == '\0' || hdr[n] == ' ' || hdr[n] == '\t';
}

/* 去掉首尾空白（就地）。正文里模型常爱多打一个换行。 */
static void trim(char *s)
{
    size_t n = strlen(s);

    while (n > 0 && (s[n - 1] == '\n' || s[n - 1] == '\r' ||
                     s[n - 1] == ' '  || s[n - 1] == '\t'))
        s[--n] = '\0';

    size_t i = 0;
    while (s[i] == '\n' || s[i] == '\r' || s[i] == ' ' || s[i] == '\t')
        ++i;
    if (i)
        memmove(s, s + i, n - i + 1);
}

/* 从 hdr 里取一个 key=value 参数。value 到空白为止。
 *
 * 抽出来是因为后面一下子多了好几个参数（tid/region/lang），
 * 每个都手写一遍 strstr + strcspn + memcpy 的话，
 * "忘了查长度"这类错一定会出现一次 —— 而这里忘了查长度就是缓冲区溢出。
 * 返回值：true = 找到了并且填进 out（超长则**拒收**，不截断）。
 *
 * 为什么超长要拒而不是截断：截断会造出一个"看起来合法"的值。
 * 比如 tid 需要 16 位，模型给了 20 位，截成 16 位后恰好是个**别的游戏**的 ID
 * —— 于是我们安安静静地改错了游戏。宁可当成没给。 */
/* 从动作头里取一个 key=value 的值。
 *
 * ★ 关键约定：**键不存在时，一个字节都不许碰 out**。
 *
 * 早先这里是 `out[0] = '\0';` 写在查找之前，于是
 * "取不到就清空"。而动作头的参数是分几次取的：
 *     arg_get(hdr, "value=", out->value, ...);
 *     arg_get(hdr, "sub=",   out->key,   ...);   ← 这一步把上面那个擦了
 * 后一步对 `ui.theme value=dark` 这种没有 `sub=` 的动作照样执行，
 * 于是 key/value 变成空串，所有设置类动作全部收不到参数。
 *
 * 它为什么能潜伏到现在：那些"非法值必须被拒"的用例**也**因为参数空了
 * 而被拒 → 判据全绿。**是"合法值必须通过"那几条把它抓出来的。**
 * 教训：判据里必须同时有"该过的过"与"该拒的拒"，只后者会被"以错拒对"喂饱。
 *
 * 因此改成：先提取到临时缓冲，**全部条件过了才写回 out**。 */
static bool arg_get(const char *hdr, const char *key, char *out, size_t cap)
{
    const char *p;
    size_t      n;
    char        tmp[ACT_NAME_MAX + 8];

    if (!out || cap == 0)
        return false;
    p = strstr(hdr, key);
    if (!p)
        return false;               /* 键不存在 —— out 原样不动 */

    p += strlen(key);
    n = strcspn(p, " \t\r\n");
    if (n == 0 || n >= sizeof tmp || n >= cap)
        return false;               /* 空值或太长 —— 也一个字都不写 */

    memcpy(tmp, p, n);
    tmp[n] = '\0';
    memcpy(out, tmp, n + 1);
    return true;
}

ActionId actions_parse(const char *reply, char *clean, size_t clean_cap,
                       ActionReq *out)
{
    const char *p, *hdr_end, *body, *tail, *after;
    char        hdr[128];
    size_t      hlen, bn, pre, rest;

    if (clean && clean_cap)
        clean[0] = '\0';
    if (out)
        memset(out, 0, sizeof *out);

    if (!reply)
        return ACT_NONE;

    p = strstr(reply, MARK_OPEN);
    if (!p)
        goto none;

    hdr_end = strstr(p + strlen(MARK_OPEN), MARK_END);
    if (!hdr_end)
        goto none;

    body = hdr_end + strlen(MARK_END);
    tail = strstr(body, MARK_CLOSE);
    if (!tail)
        goto none;

    after = tail + strlen(MARK_CLOSE);

    /* ---- 头部：动作名 + 参数 ---- */
    hlen = (size_t)(hdr_end - (p + strlen(MARK_OPEN)));
    if (hlen == 0 || hlen >= sizeof hdr)
        goto none;               /* 头太长 = 不可能是我们认得的动作 */

    memcpy(hdr, p + strlen(MARK_OPEN), hlen);
    hdr[hlen] = '\0';

    if (out) {
        /* 动作名必须是**完整的一段**（后面跟空格或结束），不能只是前缀。
         * 早先用 strncmp(hdr,"file.write",10)，于是 "file.writeX" 也会被
         * 认成 file.write —— 而"多出来的那几个字符"正是协议被试探的痕迹。
         * 动作名在这里是**编译期白名单**的入口，匹配宽一格就等于白名单宽一格。 */
        if (hdr_match(hdr, "file.write"))
            out->id = ACT_FILE_WRITE;
        else if (hdr_match(hdr, "pkg.install"))
            out->id = ACT_PKG_INSTALL;
        else if (hdr_match(hdr, "locale.set"))
            out->id = ACT_LOCALE_SET;
        else if (hdr_match(hdr, "locale.clear"))
            out->id = ACT_LOCALE_CLEAR;
        else if (hdr_match(hdr, "ui.theme"))
            out->id = ACT_UI_THEME;
        else if (hdr_match(hdr, "ui.fontsize"))
            out->id = ACT_UI_FONTSIZE;
        else if (hdr_match(hdr, "ui.depth"))
            out->id = ACT_UI_DEPTH;
        else if (hdr_match(hdr, "sec.level"))
            out->id = ACT_SEC_LEVEL;
        else if (hdr_match(hdr, "cfg.set"))
            out->id = ACT_CFG_SET;
        else if (hdr_match(hdr, "sys.info"))
            out->id = ACT_SYS_INFO;
        else if (hdr_match(hdr, "file.list"))
            out->id = ACT_FILE_LIST;
        else if (hdr_match(hdr, "file.read"))
            out->id = ACT_FILE_READ;
        else if (hdr_match(hdr, "font.install"))
            out->id = ACT_FONT_INSTALL;
        else if (hdr_match(hdr, "model.install"))
            out->id = ACT_MODEL_INSTALL;
        else if (hdr_match(hdr, "file.delete"))
            out->id = ACT_FILE_DELETE;
        else if (hdr_match(hdr, "file.rename"))
            out->id = ACT_FILE_RENAME;
        else if (hdr_match(hdr, "key.remove"))
            out->id = ACT_KEY_REMOVE;
        else if (hdr_match(hdr, "key.switch"))
            out->id = ACT_KEY_SWITCH;
        else if (hdr_match(hdr, "trash.purge"))
            out->id = ACT_TRASH_PURGE;
        else if (hdr_match(hdr, "svc.set"))
            out->id = ACT_SVC_SET;
        else if (hdr_match(hdr, "ask"))
            out->id = ACT_ASK;          /* AI 主动提问：不走确认屏，切下屏选项页 */
        else
            /* **不认识的动作也要走到确认那一屏** —— 静默忽略等于把一次
             * 越权尝试咽进肚子里，而那正是最该被看见的东西。
             * 这里不执行任何东西，只是让上层弹一句"我不认识它"。 */
            out->id = ACT_UNKNOWN;

        /* ---- 参数：name=xxx ---- */
        arg_get(hdr, "name=", out->name, sizeof out->name);

        /* ---- 参数：sha256=<64 位十六进制>（只有 pkg.install 用）----
         * 长度不是 64 就当没给 —— 半截的摘要比没有摘要更坏，
         * 因为它看起来像是校验过了。
         * arg_get 只保证"到空白为止、放得下"，长度语义是这里的事。 */
        {
            char tmp[80];

            if (arg_get(hdr, "sha256=", tmp, sizeof tmp) && strlen(tmp) == 64)
                memcpy(out->sha256, tmp, 65);   /* 含结尾符 */
        }

        /* ---- 参数：locale.* 用的三个值 ----
         * 这里只做"取出来"，**不做合法性判断** —— 判断统一放在执行前
         * （actions_run 调 locale.c 的那一步）。理由：解析阶段要把动作
         * 原样呈现给用户看，包括那个不合法的值。"模型试图设一个不存在的区域"
         * 这件事用户应该看见，而不是被我们悄悄修好或悄悄丢掉。 */
        arg_get(hdr, "tid=",    out->tid,    sizeof out->tid);
        arg_get(hdr, "region=", out->region, sizeof out->region);
        arg_get(hdr, "lang=",   out->lang,   sizeof out->lang);

        /* ---- 参数：设置类动作用 key= 与 value= ----
         *
         * 同样只取不判：非法值要原样显示在确认屏上，
         * 用户看得见"模型想写一个不存在的字号"才可能拒绝它。
         * 合法性在 actions_run 里查白名单。 */
        arg_get(hdr, "key=",   out->key,   sizeof out->key);
        arg_get(hdr, "value=", out->value, sizeof out->value);
        /* 只读沙箱用：sub= 子目录名，from= 起始字节 */
        arg_get(hdr, "sub=",   out->key,   sizeof out->key);
        arg_get(hdr, "from=",  out->value, sizeof out->value);
        /* file.rename 用 to= 新名 */
        if (out->id == ACT_FILE_RENAME)
            arg_get(hdr, "to=", out->to, sizeof out->to);

        /* ask 用：question= 问句，options= 用 | 分隔的选项。
         * 为什么选项用 | 分隔而不是数组：这套协议是**一行文本**里的键值对，
         * 数组要引入引号与转义 —— 模型写错一个引号，整个动作就解析失败，
         * 而"少一个选项"用户再问一句就行。宁可少功能，不要脆协议。 */
        if (out->id == ACT_ASK) {
            arg_get(hdr, "question=", out->question, sizeof out->question);
            arg_get(hdr, "options=",  out->options,  sizeof out->options);
        }

        /* model.install 用：kind= 类别，idx= 第几条（都进 key/value）*/
        if (out->id == ACT_MODEL_INSTALL) {
            arg_get(hdr, "kind=", out->key,   sizeof out->key);
            arg_get(hdr, "idx=",  out->value, sizeof out->value);
        }

        /* ui.theme / ui.fontsize / ui.depth / sec.level 只带 value，
         * 不带 key —— 键由动作名本身决定，这里补上，
         * 好让下游（审计、确认屏、执行）只认 key 一个来源。 */
        if (out->id == ACT_UI_THEME)
            snprintf(out->key, sizeof out->key, "theme");
        else if (out->id == ACT_UI_FONTSIZE)
            snprintf(out->key, sizeof out->key, "fontsize");
        else if (out->id == ACT_UI_DEPTH)
            snprintf(out->key, sizeof out->key, "depth");
        else if (out->id == ACT_SEC_LEVEL)
            snprintf(out->key, sizeof out->key, "level");

        /* ---- 正文：[[ACT:...]] 与 [[/ACT]] 之间 ---- */
        bn = (size_t)(tail - body);
        if (bn > ACT_TEXT_MAX)
            bn = ACT_TEXT_MAX;       /* 超了就截断：内容太长人没法核对，
                                  * 而且真写下去也不是模型承诺的那份 */
        memcpy(out->text, body, bn);
        out->text[bn] = '\0';
    }

    /* ---- 正文里把标记整段剥掉 ----
     *
     * 这里曾经是"放不下就整个不拷"，那是个**静默丢正文**的 bug：
     * 前段已经在 pre 处截断了，后段又被整块跳过，于是用户看到半句话，
     * 还不知道自己少看了什么。正确做法是**按剩余空间截断着拷** ——
     * 宁可让用户看见"后面被切了"，也不能让他以为那就是全部。 */
    if (clean && clean_cap) {
        size_t pos = 0;

        pre = (size_t)(p - reply);
        if (pre > clean_cap - 1)
            pre = clean_cap - 1;
        memcpy(clean, reply, pre);
        pos = pre;

        rest = strlen(after);
        if (rest > clean_cap - 1 - pos)
            rest = clean_cap - 1 - pos;      /* 装得下多少拷多少 */
        memcpy(clean + pos, after, rest);
        pos += rest;
        clean[pos] = '\0';

        trim(clean);
    }

    return out ? out->id : ACT_NONE;

none:
    if (out)
        out->id = ACT_NONE;
    if (clean && clean_cap)
        snprintf(clean, clean_cap, "%s", reply);
    return ACT_NONE;
}

/* --------------------------------------------------------------------------
 * 待确认
 * ----------------------------------------------------------------------- */

void actions_set_pending(const ActionReq *req)
{
    if (!req) {
        g_has_pending = false;
        return;
    }
    g_pending     = *req;
    g_has_pending = true;
}

const ActionReq *actions_pending(void)
{
    return g_has_pending ? &g_pending : NULL;
}

void actions_clear_pending(void)
{
    g_has_pending = false;
    memset(&g_pending, 0, sizeof g_pending);
}

/* --------------------------------------------------------------------------
 * 审计
 * ----------------------------------------------------------------------- */

void actions_audit(const char *verdict, const ActionReq *req, const char *detail)
{
    char  stamp[32];
    FILE *f;
    /* 审计行里那个 key=value 要写**这次动作真正的对象**。
     * 早先这里硬写 "name=%s"，而 locale 动作根本没有 name —— 于是审计里
     * 会出现一行 "name=-"，而真正的游戏 ID 反倒不在里面。
     * 审计的价值全在"事后能查出是谁改了什么"，一行看不出对象的记录等于没记。
     *
     * 缓冲区要按**最长的那种**开：locale 那条是
     * "tid=16位 + 空格 + region(最长3) + 空格 + lang(最长2)" = 4+16+1+3+1+2 = 27，
     * 但 region/lang 是模型给的、未经校验的串（ActionReq 里各留了 8 字节），
     * 所以上界是 4+16+1+8+1+8 = 38。给 64 足够，而且不会被 -Wformat-truncation
     * 抱怨 —— 那个警告在这里是真话：写死的小缓冲迟早会截断。 */
    char  subj[96];

    util_mkdirs(ACT_DIR);
    util_stamp_full(stamp, sizeof stamp);

    if (req && (req->id == ACT_LOCALE_SET || req->id == ACT_LOCALE_CLEAR)) {
        if (req->id == ACT_LOCALE_SET)
            snprintf(subj, sizeof subj, "tid=%s %s %s",
                     req->tid[0] ? req->tid : "-",
                     req->region[0] ? req->region : "?",
                     req->lang[0] ? req->lang : "?");
        else
            snprintf(subj, sizeof subj, "tid=%s",
                     req->tid[0] ? req->tid : "-");
    } else if (req && actions_is_setting(req->id)) {
        /* 设置类要记的是"哪个键改成了什么"。
         * 键最长 28 字节（nand_backup.remind_on_startup），
         * 值最长 5（8192/off），加上分隔与取值兜底，96 字节够用。 */
        snprintf(subj, sizeof subj, "%s=%s",
                 req->key[0] ? req->key : "-",
                 req->value[0] ? req->value : "?");
    } else {
        snprintf(subj, sizeof subj, "name=%s",
                 (req && req->name[0]) ? req->name : "-");
    }

    f = fopen(ACT_LOG, "ab");
    if (!f)
        return;

    fprintf(f, "%s | %-5s | %-14s | %s | %s\n",
            stamp,
            (verdict && *verdict) ? verdict : "?",
            req ? actions_verb(req->id) : "-",
            subj,
            detail ? detail : "");

    fclose(f);
}

/* --------------------------------------------------------------------------
 * 执行
 * ----------------------------------------------------------------------- */

/* --------------------------------------------------------------------------
 * 设置类动作：键白名单与值校验
 *
 * 白名单里**故意没有** guard.confirm_overwrite —— 那一项是设计上的硬约束
 * （保护用户唯一无法撤销的操作），设置屏上它也是锁着的。
 * 把它排除在这里，才不会出现"屏上锁着、AI 却能改"的分裂。
 * ----------------------------------------------------------------------- */
typedef struct {
    const char *key;
    bool        is_bool;   /* true = on/off；false = 整数 */
    int         vmax;      /* 整数上界（inclusive） */
} CfgKey;

static const CfgKey CFG_KEYS[] = {
    { "guard.confirm_delete",         true,  0 },
    { "guard.confirm_bulk",           true,  0 },
    { "guard.max_write_kb",           false, 8192 },
    { "guard.max_batch_files",        false, 999  },
    { "nand_backup.remind_on_startup", true,  0 },
    { "ota.enabled",                  true,  0 },
    { "ota.auto_check",               true,  0 },
    { "diag.show_layout_overflow",    true,  0 },
    { "diag.show_notes",              true,  0 },
    { "diag.verbose_status",          true,  0 },
};
#define CFG_KEYS_N ((int)(sizeof CFG_KEYS / sizeof CFG_KEYS[0]))

const char *actions_cfg_key_at(int i)
{
    if (i < 0 || i >= CFG_KEYS_N)
        return NULL;
    return CFG_KEYS[i].key;
}

bool actions_cfg_key_is_bool(int i)
{
    if (i < 0 || i >= CFG_KEYS_N)
        return false;
    return CFG_KEYS[i].is_bool;
}

/* 严格十进制解析。定义在文件更靠后（与设置类那套共用），
 * 但沙箱只读与安装类的校验都要用 —— 在这里给一个前向声明，
 * 免得"为了编译过"去复制第二份实现：两份解析会各自漂移，
 * 然后同一种输入在两个动作里得到不同的结论。 */
static bool val_int(const char *v, long *out);

bool actions_is_setting(ActionId id)
{
    return id == ACT_UI_THEME || id == ACT_UI_FONTSIZE ||
           id == ACT_UI_DEPTH  || id == ACT_SEC_LEVEL ||
           id == ACT_CFG_SET;
}

bool actions_is_readonly(ActionId id)
{
    return id == ACT_SYS_INFO || id == ACT_FILE_LIST || id == ACT_FILE_READ;
}

bool actions_is_fileop(ActionId id)
{
    return id == ACT_FILE_DELETE || id == ACT_FILE_RENAME ||
           id == ACT_KEY_REMOVE || id == ACT_KEY_SWITCH ||
           id == ACT_TRASH_PURGE || id == ACT_SVC_SET;
}

bool actions_check_svc(const ActionReq *req, char *msg, size_t msg_cap)
{
    if (!req || !msg || msg_cap == 0)
        return false;
    msg[0] = '\0';

    if (req->id != ACT_SVC_SET) {
        snprintf(msg, msg_cap, "这个动作不是切换服务商");
        return false;
    }
    /* 键名统一走 name=（模型习惯写 name），也接受 key= 兜底。 */
    {
        const char *n = req->name[0] ? req->name : req->key;
        if (!n[0]) {
            snprintf(msg, msg_cap, "要写明切到哪家（name= 服务商名）");
            return false;
        }
        /* ★ 必须比名字，不能判空：provider_find() 找不到时返回**默认那家**
         * （见 provider.h 的说明），判空等于没校验 —— 那样
         * `name=nope` 这种根本不存在的一家会畅通无阻。
         * 2026-10-06 实测踩过。 */
        {
            const ProviderDef *p = provider_find(n);
            if (!p || strcmp(p->name, n) != 0) {
            /* 理由里列出可选的几家 —— 模型看不到界面，
             * 只报"没有这家"它就只能瞎猜名字。 */
            snprintf(msg, msg_cap, "没有名为 %s 的服务商。可选：", n);
            for (int i = 0; i < provider_count(); ++i) {
                const ProviderDef *q = provider_at(i);
                if (q)
                    snprintf(msg + strlen(msg), msg_cap - strlen(msg),
                             "%s%s", (i ? " / " : ""), q->name);
            }
            return false;
            }
        }
    }
    return true;
}

bool actions_needs_double_confirm(ActionId id)
{
    /* 只有清空回收站要两次。它是全套动作里唯一一个"做错了就真的没了"的 ——
     * 连 file.delete 都能从回收站捞回来。 */
    return id == ACT_TRASH_PURGE;
}

bool actions_check_fileop(const ActionReq *req, char *msg, size_t msg_cap)
{
    if (!req || !msg || msg_cap == 0)
        return false;
    msg[0] = '\0';

    if (req->id == ACT_FILE_DELETE) {
        if (!req->name[0]) {
            snprintf(msg, msg_cap, "要写明删哪个文件（name= 文件名）");
            return false;
        }
        if (!name_ok(req->name)) {
            snprintf(msg, msg_cap,
                     "文件名不合法，已拦下：%.40s（不能带 / \\ : 或 ..）",
                     req->name);
            return false;
        }
        /* 回收站本身不许被删：它被清空之后，前面删掉的东西就真的找不回来了，
         * 而用户在这条消息里并不会知道这一点。 */
        if (req->name[0] == '.') {
            snprintf(msg, msg_cap,
                     "%.20s 以点开头，那是系统目录，不是你要删的文件", req->name);
            return false;
        }
        return true;
    }

    if (req->id == ACT_FILE_RENAME) {
        if (!req->name[0] || !req->to[0]) {
            snprintf(msg, msg_cap, "要写明原名与新名（name= 原名 to= 新名）");
            return false;
        }
        if (!name_ok(req->name) || !name_ok(req->to)) {
            snprintf(msg, msg_cap,
                     "文件名不合法，已拦下：%.30s -> %.30s", req->name, req->to);
            return false;
        }
        if (!strcmp(req->name, req->to)) {
            snprintf(msg, msg_cap, "新旧名字一样，没有要改的东西");
            return false;
        }
        if (req->to[0] == '.') {
            snprintf(msg, msg_cap, "新名不能以点开头（那是系统目录）");
            return false;
        }
        return true;
    }

    if (req->id == ACT_KEY_REMOVE || req->id == ACT_KEY_SWITCH) {
        if (!req->name[0]) {
            snprintf(msg, msg_cap, "要写明是哪一把 Key（name= 条目名）");
            return false;
        }
        /* 条目名走白名单式校验：只允许字母数字与 - _ .，
         * 不允许任何分隔符。它会被写进 creds.enc 的索引里，
         * 允许斜杠就等于允许改写到别处。 */
        for (const char *q = req->name; *q; ++q) {
            if ((*q >= 'a' && *q <= 'z') || (*q >= 'A' && *q <= 'Z') ||
                (*q >= '0' && *q <= '9') || *q == '-' || *q == '_' || *q == '.')
                continue;
            snprintf(msg, msg_cap,
                     "Key 条目名只能是字母、数字、减号、下划线与点，"
                     "收到的是 %.32s", req->name);
            return false;
        }
        return true;
    }

    if (req->id == ACT_TRASH_PURGE)
        return true;               /* 无参数 */

    snprintf(msg, msg_cap, "这个动作不是文件或凭据类");
    return false;
}

bool actions_is_install(ActionId id)
{
    return id == ACT_FONT_INSTALL || id == ACT_MODEL_INSTALL;
}

/* model.install 的类别表。
 *
 * 这一张表**只此一份**：手册里教模型的取值从它来，
 * 校验时用的也是它，两处不可能对不上。
 * 名字要与 cupd.h 的 CuKind 语义一致（model / guide / hw），
 * 那边拿不到就查 cu_kind_name 报出来的是什么。 */
static const char *const MODEL_KINDS[] = { "model", "kb", "hw" };
#define MODEL_KINDS_N ((int)(sizeof MODEL_KINDS / sizeof MODEL_KINDS[0]))

const char *actions_model_kind(int i)
{
    if (i < 0 || i >= MODEL_KINDS_N)
        return NULL;
    return MODEL_KINDS[i];
}

bool actions_check_install(const ActionReq *req, char *msg, size_t msg_cap)
{
    int i;

    if (!req || !msg || msg_cap == 0)
        return false;
    msg[0] = '\0';

    if (req->id == ACT_FONT_INSTALL) {
        /* 字库名要过 name_ok：虽然最后拼的是 fonts/<name>，
         * 但字体文件名会带下划线与连字符（两种都合法），
         * 而 name_ok 放行它们的同时挡掉 / \ : 与 .. —— 正好是我们要的。
         * 落地前还会核对名字是否真的在 fonts/ 里（见 app 层 shfont_list），
         * 白名单管"形状"，在不在目录里由那一步管。 */
        if (!req->name[0]) {
            snprintf(msg, msg_cap, "要写明用哪套字库（name= 字体文件名）");
            return false;
        }
        if (!name_ok(req->name)) {
            snprintf(msg, msg_cap,
                     "字库文件名不合法，已拦下：%.40s（不能带 / \\ : 或 ..）",
                     req->name);
            return false;
        }
        return true;
    }

    if (req->id == ACT_MODEL_INSTALL) {
        long n;
        if (!req->key[0]) {
            snprintf(msg, msg_cap,
                     "要写明装哪一类（kind= model / kb / hw）");
            return false;
        }
        for (i = 0; i < MODEL_KINDS_N; ++i)
            if (!strcmp(MODEL_KINDS[i], req->key))
                break;
        if (i == MODEL_KINDS_N) {
            snprintf(msg, msg_cap,
                     "没有名为 %s 的类别。只有 model / kb / hw 三类。",
                     req->key);
            return false;
        }
        /* idx 可以不给（默认第 0 条）—— 清单通常只有一条，
         * 多一条时模型必须明确说第几条，那要靠 idx。 */
        if (req->value[0] && (!val_int(req->value, &n) || n < 0 || n > 63)) {
            snprintf(msg, msg_cap,
                     "idx 只能是一个序号（0 到 63），收到的是 %.16s", req->value);
            return false;
        }
        return true;
    }

    snprintf(msg, msg_cap, "这个动作不是安装类");
    return false;
}

/* ---- 沙箱子目录白名单 ---- */

/*
 * 为什么是白名单而不是"让模型给个路径"：file.list / file.read 一旦能收
 * 任意路径，`../../` 就能一路读到 config.json、creds.enc（里面是加密的
 * Key 仓库）、甚至 sdmc: 之外。它是只读动作不改变这一点 ——
 * 读走凭据和写坏文件一样是事故。
 *
 * 用"省略号"这一项表示沙箱根目录本身：模型不需要知道路径，
 * ���"根"这个概念映射到 actions_dir() 是编译期常量。 */
static const char *const SANDBOX_SUBDIRS[] = {
    "...", "out", "pkgs", "doubao", "update", ".trash"
};
#define SANDBOX_SUBDIRS_N ((int)(sizeof SANDBOX_SUBDIRS / sizeof SANDBOX_SUBDIRS[0]))

const char *actions_sandbox_subdir(int i)
{
    if (i < 0 || i >= SANDBOX_SUBDIRS_N)
        return NULL;
    return SANDBOX_SUBDIRS[i];
}

bool actions_check_readonly(const ActionReq *req, char *msg, size_t msg_cap)
{
    int i;

    if (!req || !msg || msg_cap == 0)
        return false;
    msg[0] = '\0';

    if (req->id == ACT_SYS_INFO)
        return true;               /* 无参数 */

    if (req->id == ACT_FILE_LIST) {
        /* 没给 sub 就当根目录 —— 模型不写参数是最常见的情形，
         * 不该为这个报"参数不合法"。 */
        if (!req->key[0])
            return true;
        for (i = 0; i < SANDBOX_SUBDIRS_N; ++i)
            if (!strcmp(SANDBOX_SUBDIRS[i], req->key))
                return true;
        snprintf(msg, msg_cap,
                 "没有名为 %s 的子目录。能列的有：根目录（不写 sub）、"
                 "out、pkgs、doubao、update、.trash（回收站）", req->key);
        return false;
    }

    if (req->id == ACT_FILE_READ) {
        if (!name_ok(req->name)) {
            /* 拦截理由要写清是**路径穿越**，而不是"文件名格式不对" ——
             * 模型拼出 ../ 时需要知道它错在哪一类问题上。 */
            snprintf(msg, msg_cap,
                     "文件名不合法，已拦下：%.40s"
                     "（只允许纯文件名，不能带 / \\ : 或 ..）", req->name);
            return false;
        }
        if (req->value[0]) {
            long n;
            if (!val_int(req->value, &n) || n < 0 || n > (1 << 20)) {
                snprintf(msg, msg_cap,
                         "from 只能是一个字节偏移（0 到 1048576），收到的是 %.16s",
                         req->value);
                return false;
            }
        }
        return true;
    }

    snprintf(msg, msg_cap, "这个动作不是只读类");
    return false;
}

/* 把 "on"/"off"/数字 规范化成布尔与整数两条路。 */
static bool val_bool(const char *v, bool *out)
{
    if (!v || !*v)
        return false;
    if (!strcasecmp(v, "on") || !strcasecmp(v, "true") ||
        !strcasecmp(v, "yes") || !strcmp(v, "1")) {
        *out = true;
        return true;
    }
    if (!strcasecmp(v, "off") || !strcasecmp(v, "false") ||
        !strcasecmp(v, "no") || !strcmp(v, "0")) {
        *out = false;
        return true;
    }
    return false;
}

/* 严格十进制：只允许数字与一个可选负号，且必须有至少一位数字。
 * 用 strtol 的话 "12abc" 会被当 12 —— 那等于替模型把错值修好了，
 * 而"模型写了个非法值"这件事用户该看见。 */
static bool val_int(const char *v, long *out)
{
    const char *p = v;
    char       *end = NULL;
    long        n;

    if (!v || !*v)
        return false;
    if (*p == '-' || *p == '+')
        ++p;
    if (!*p)
        return false;
    for (; *p; ++p)
        if (*p < '0' || *p > '9')
            return false;

    n = strtol(v, &end, 10);
    if (!end || *end)
        return false;
    *out = n;
    return true;
}

/* 原地小写化。设置项的值最终要写进 config.json，
 * 而那里全是小写 —— 让 "DARK" 和 "dark" 变成两行不同的配置是纯负担。 */
static void lower(char *s)
{
    for (; *s; ++s)
        if (*s >= 'A' && *s <= 'Z')
            *s = (char)(*s - 'A' + 'a');
}

bool actions_check_setting(const ActionReq *req, char *out_value, size_t cap,
                           char *msg, size_t msg_cap)
{
    const char *v;

    if (!req || !out_value || cap == 0 || !msg || msg_cap == 0)
        return false;
    out_value[0] = '\0';
    msg[0] = '\0';

    v = (req->value[0]) ? req->value : NULL;

    switch (req->id) {
        case ACT_UI_THEME:
            if (!v || (strcasecmp(v, "light") && strcasecmp(v, "dark"))) {
                snprintf(msg, msg_cap, "主题只能是 light 或 dark，收到的是 %.16s",
                         v ? v : "(空)");
                return false;
            }
            snprintf(out_value, cap, "%s", strcasecmp(v, "dark") ? "light" : "dark");
            return true;

        case ACT_UI_FONTSIZE: {
            char norm[16];
            if (!v || !*v || strlen(v) >= sizeof norm) {
                snprintf(msg, msg_cap, "字号只能是 small / mid / big，收到的是 %.16s",
                         v ? v : "(空)");
                return false;
            }
            snprintf(norm, sizeof norm, "%s", v);
            lower(norm);
            if (strcmp(norm, "small") && strcmp(norm, "mid") && strcmp(norm, "big")) {
                snprintf(msg, msg_cap, "字号只能是 small / mid / big，收到的是 %.16s", v);
                return false;
            }
            snprintf(out_value, cap, "%s", norm);
            return true;
        }

        case ACT_UI_DEPTH: {
            long n;
            if (!val_int(v, &n)) {
                snprintf(msg, msg_cap, "立体深度要一个整数（0 到 100），收到的是 %.16s",
                         v ? v : "(空)");
                return false;
            }
            if (n < 0 || n > 100) {
                snprintf(msg, msg_cap, "立体深度要在 0 到 100 之间，收到 %ld", n);
                return false;
            }
            snprintf(out_value, cap, "%ld", n);
            return true;
        }

        case ACT_SEC_LEVEL: {
            long n;
            /* 接受 L0/L1 与 0/1 两种写法 —— 模型两种都会写，
             * 而在这里为写法之争拒绝一次毫无意义。 */
            const char *p = v;
            if (p && (p[0] == 'L' || p[0] == 'l'))
                ++p;
            if (!val_int(p, &n)) {
                snprintf(msg, msg_cap,
                         "安全等级只能是 L0 到 L%d，收到的是 %.16s", SEC_LEVEL_MAX,
                         v ? v : "(空)");
                return false;
            }
            if (n < 0 || n > SEC_LEVEL_MAX) {
                snprintf(msg, msg_cap, "安全等级要在 L0 到 L%d 之间，收到 %ld",
                         SEC_LEVEL_MAX, n);
                return false;
            }
            snprintf(out_value, cap, "L%ld", n);
            return true;
        }

        case ACT_CFG_SET: {
            int i;
            for (i = 0; i < CFG_KEYS_N; ++i)
                if (!strcmp(CFG_KEYS[i].key, req->key))
                    break;
            if (i == CFG_KEYS_N) {
                snprintf(msg, msg_cap,
                         "没有名为 %s 的设置项。可改的有："
                         "guard.confirm_delete / guard.confirm_bulk / "
                         "guard.max_write_kb / guard.max_batch_files / "
                         "nand_backup.remind_on_startup / ota.enabled / "
                         "ota.auto_check / diag.show_layout_overflow / "
                         "diag.show_notes / diag.verbose_status",
                         req->key[0] ? req->key : "(空)");
                return false;
            }
            if (CFG_KEYS[i].is_bool) {
                bool b;
                if (!val_bool(v, &b)) {
                    snprintf(msg, msg_cap, "%s 只能取 on 或 off，收到的是 %.16s",
                             CFG_KEYS[i].key, v ? v : "(空)");
                    return false;
                }
                snprintf(out_value, cap, "%s", b ? "on" : "off");
            } else {
                long n;
                if (!val_int(v, &n)) {
                    snprintf(msg, msg_cap, "%s 要一个整数（0 到 %d），收到的是 %.16s",
                             CFG_KEYS[i].key, CFG_KEYS[i].vmax, v ? v : "(空)");
                    return false;
                }
                if (n < 0 || n > CFG_KEYS[i].vmax) {
                    snprintf(msg, msg_cap, "%s 要在 0 到 %d 之间，收到 %ld",
                             CFG_KEYS[i].key, CFG_KEYS[i].vmax, n);
                    return false;
                }
                snprintf(out_value, cap, "%ld", n);
            }
            return true;
        }

        default:
            snprintf(msg, msg_cap, "这个动作不是设置类");
            return false;
    }
}

bool actions_run(const ActionReq *req, char *msg, size_t cap)
{
    char   path[192];
    FILE  *f;
    size_t want, got;
    int    n;

    if (!msg || cap == 0)
        return false;
    msg[0] = '\0';

    if (!req) {
        snprintf(msg, cap, "没有待执行的动作。");
        return false;
    }

    /* ---- 安装一个程序 ----
     * 分两条**完全不同**的路，按包名后缀分流：
     *
     *   .3dsx → pkg_install()      ：搬文件到 3ds/。只要 SD 写权限。
     *   .cia  → pkg_install_cia()  ：交给系统的 am 服务装到主菜单。
     *                                需要 am:u，也就是**必须是 CIA 形态**。
     *
     * 为什么要在动作层就分开、而不是让 AI 用两个不同的动作名：
     * 对用户和模型来说，"装这个包"是同一件事，区别只在包本身。
     * 多一个动作名只会多一处需要同步维护的白名单。
     * 而分流判据（后缀）来自**包名**，包名是要过校验的，
     * 不是模型可以拿来乱指的路径 —— 第 4 道闸没有因此变松。
     *
     * 分开写而不是塞进同一个函数，是因为两者的失败理由与后果量级
     * 都不是一回事：装 .3dsx 失败最多是"没装上"；装 .cia 失败可能
     * 留下半个 title（pkg_cia.c 里有专门的 Cancel 收拾逻辑）。 */
    if (req->id == ACT_PKG_INSTALL) {
        PkgState ps;
        bool     is_cia;

        if (!pkg_name_ok(req->name)) {
            snprintf(msg, cap, "包名不合法，已拦下：%.40s", req->name);
            return false;
        }

        /* 后缀判定用**结尾匹配**，不是 strstr —— "a.cia.b" 不是 CIA 包。
         * 大小写不敏感：用户从网上下的包名什么写法都有。
         * 这里刻意不用 pkg_name_ok 之类做"猜后缀"——后缀就是后缀，
         * 一个独立的、只看最后 4 个字节的判断，改起来不会牵动别处。 */
        {
            size_t nl = strlen(req->name);
            is_cia = (nl > 4) && (strcasecmp(req->name + nl - 4, ".cia") == 0);
        }

        if (is_cia)
            ps = pkg_install_cia(req->name,
                                 req->sha256[0] ? req->sha256 : NULL,
                                 msg, cap);
        else
            ps = pkg_install(req->name,
                             req->sha256[0] ? req->sha256 : NULL,
                             msg, cap);
        return ps == PKG_OK;
    }

    /* ---- 设 / 撤销游戏的区域语言 ----
     *
     * 这两条是**温和**的动作：只往 luma/titles/ 写（或删）一个几字节的
     * 文本文件。不碰系统文件、不用特权服务、删掉即恢复。
     *
     * 但"温和"不等于"不用校验" —— 反过来说，正因为它是写文件，
     * **游戏 ID 会被拼进路径**，所以它是本模块里路径穿越风险最直接的一条。
     * 防线是 loc_tid_ok()：恰好 16 位、每位都必须是十六进制数字。
     * 这不是黑名单式的"挡掉几个坏字符"，而是穷举式的"只放行一种形态"——
     * 一个 '../' 里至少含 '.' 和 '/'，两个字符都过不了。
     *
     * 校验在**这里**做（执行前），不在解析时做：解析阶段要把动作原样
     * 呈现给用户，包括那个不合法的值。"模型想设一个不存在的区域"
     * 这件事用户该看见，而不是被我们悄悄丢掉。 */
    if (req->id == ACT_LOCALE_SET || req->id == ACT_LOCALE_CLEAR) {
        LocState ls;

        if (!loc_tid_ok(req->tid)) {
            snprintf(msg, cap,
                     "游戏 ID 不合法，已拦下：%.40s"
                     "（必须是 16 位十六进制，例如 0004000000055D00）",
                     req->tid[0] ? req->tid : "(空)");
            return false;
        }

        if (req->id == ACT_LOCALE_SET)
            ls = loc_set(req->tid, req->region, req->lang, msg, cap);
        else
            ls = loc_clear(req->tid, msg, cap);

        return ls == LOC_OK;
    }

    if (actions_is_setting(req->id)) {
        /* 设置类**不在这里执行** —— 改配置要调 app_cfg() / w_theme()，
         * 那是 app 层的东西，shared 不该依赖。
         * 正常路径由 sc_confirm.c 先 actions_check_setting() 校验、
         * 再由它自己落盘。走到这个分支说明**接线漏了**，
         * 所以文案要说清是内部问题，而不是"值不对"——
         * 否则用户会去改自己的输入，而那不是他的问题。 */
        snprintf(msg, cap, "设置类动作应由界面层执行（内部接线问题）");
        return false;
    }

    if (req->id != ACT_FILE_WRITE) {
        snprintf(msg, cap, "这个动作不在白名单里，没有执行。");
        return false;
    }

    if (!name_ok(req->name)) {
        /* 拦住之后要**说清楚拦的是什么** —— 用户看到的是
         * "AI 想写一个名字不对劲的文件，被拦下了"，
         * 而不是一个不知从哪来的失败。 */
        snprintf(msg, cap, "文件名不合法，已拦下：%.40s", req->name);
        return false;
    }

    util_mkdirs(ACT_DIR);

    /* 路径在这里拼 —— 这是第 4 道闸的落点。
     * req->name 已经过校验（无分隔符、无盘符），所以拼出来必然在沙箱里。
     *
     * 拼完必须**核对结果**：snprintf 在放不下时会静默截断，而截断出来的
     * 是**另一个路径**。今天算下来只要 95 字节（目录 33 + 斜杠 1 + 名字 60 + 结束符），
     * 192 够用得很 —— 但"够用"是个会随常量变化而失效的假设，
     * 而这里的失效方式恰好是"写到别的文件去"。宁可显式拒绝。 */
    n = snprintf(path, sizeof path, "%s/%s", ACT_DIR, req->name);
    if (n < 0 || n >= (int)sizeof path) {
        snprintf(msg, cap, "路径太长，已拦下");
        return false;
    }

    /* 再核一遍前缀：万一哪天 name_ok 被人改松了（比如允许 ':'），
     * 这里能兜住 —— 两道独立的防线，不靠同一处判断。 */
    if (strncmp(path, ACT_DIR "/", strlen(ACT_DIR) + 1) != 0) {
        snprintf(msg, cap, "路径跑出沙箱了，已拦下");
        return false;
    }

    f = fopen(path, "wb");
    if (!f) {
        snprintf(msg, cap, "写不进去 —— 卡满了还是写保护？");
        return false;
    }

    want = strlen(req->text);
    got  = fwrite(req->text, 1, want, f);
    fclose(f);

    if (got != want) {
        snprintf(msg, cap, "只写进去 %d/%d 字节，可能卡满了",
                 (int)got, (int)want);
        return false;
    }

    snprintf(msg, cap, "已创建 3ds/aiconsole/out/%s（%d 字节）",
             req->name, (int)got);
    return true;
}
