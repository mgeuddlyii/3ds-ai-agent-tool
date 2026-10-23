/*
 * actprobe.c -- 见 actprobe.h
 *
 * 验的是「设置类动作这条链路」：解析 → 白名单校验 → 落盘 → 读回。
 *
 * 为什么每条都写成"期望 id / 期望结果"两栏，而不是跑一遍看现象：
 * 这一层的失败模式全是**静默的** —— 值写错了、键表漏了、白名单放过了界，
 * 都不会崩、不会报错，只是配置里存了一个不该有的值。
 * 没有对照就看不出"绿灯到底绿得对不对"。
 *
 * ⚠️ 落盘之后**必须读回**：只验 act_apply_setting 返回 true 是不够的 ——
 * 它完全可以"返回成功但什么都没写"。读回 config 才闭环。
 *
 * 探针会改真实的 config.json。跑完由调用方负责还原（验收脚本已经这么做了）。
 */

#include "actprobe.h"

#include "actions.h"
#include "actapply.h"
#include "sysinfo.h"
#include "sandfile.h"
#include "cupd.h"
#include "actfile.h"
#include "konami.h"
#include "actsvc.h"
#include "provider.h"
#include "xfer.h"
#include "app.h"
#include "config.h"

#include <stdio.h>
#include <string.h>

static int g_ok;
static int g_fail;

/* 走一遍完整链路：把动作头塞进一段假回复，让 actions_parse 抠出来，
 * 再校验、（合法时）落盘。返回 true 表示"整条链路都该通"。 */
/* ★ must_contain 是这一版新增的判据维度。
 *
 * 2026-10-05 23:53 实测踩出来的：那时 arg_get 有 bug 会把参数清空，
 * 于是"非法值必须被拒"那批**全绿** —— 但它们是被"参数为空"拒的，
 * 不是被"值非法"拒的。这叫**假通过**：判据被以错拒对喂饱了。
 *
 * 加上 must_contain 之后，拒绝的理由里必须出现那个具体的错值
 * （例如期望理由含 "blue"）。参数空时理由是 "(空)"，判据立刻红。 */
static void step_ex(const char *tag, const char *hdr,
                    ActionId want_id, bool want_pass, const char *must_contain)
{
    char     reply[256];
    char     clean[256];
    char     value[32];
    char     msg[192];
    ActionReq req;
    ActionId got;
    bool     pass;

    snprintf(reply, sizeof reply,
             "好的，我这就处理。[[ACT:%s]][[/ACT]]", hdr);

    got = actions_parse(reply, clean, sizeof clean, &req);

    if (got != want_id) {
        app_note("actprobe: %s FAIL 解析得到 %d，期望 %d（hdr=%s）",
                 tag, (int)got, (int)want_id, hdr);
        ++g_fail;
        return;
    }

    if (!actions_is_setting(got)) {
        /* 走到这里说明动作名被认出来了但没被归类成设置类 ——
         * 那会让确认屏拒绝放行，也是个真 bug。 */
        app_note("actprobe: %s FAIL 动作被认出但没被归为设置类", tag);
        ++g_fail;
        return;
    }

    pass = actions_check_setting(&req, value, sizeof value, msg, sizeof msg);
    if (pass != want_pass) {
        app_note("actprobe: %s FAIL 校验=%s 期望=%s（%s）",
                 tag, pass ? "过" : "拒", want_pass ? "过" : "拒", msg);
        ++g_fail;
        return;
    }

    if (!pass) {
        if (!msg[0]) {
            app_note("actprobe: %s FAIL 拒了但没给理由", tag);
            ++g_fail;
            return;
        }
        /* 理由里必须出现那个具体的错值，否则就是"以错拒对"的假通过 */
        if (must_contain && *must_contain && !strstr(msg, must_contain)) {
            app_note("actprobe: %s FAIL 拒了但理由不含 %s（疑似因参数为空而拒）· %s",
                     tag, must_contain, msg);
            ++g_fail;
            return;
        }
        ++g_ok;                     /* 拒得对且理由到位 —— 不逐条输出 */
        return;
    }

    /* 合法 → 落盘，并读回确认。 */
    if (!act_apply_setting(req.key, value, msg, sizeof msg)) {
        app_note("actprobe: %s FAIL 落盘失败 · %s", tag, msg);
        ++g_fail;
        return;
    }

    /* 读回：这是闭环的那一步。app_cfg() 拿到的是内存里的结构，
     * 真正证明"写进去了"的，是它与刚传入的值一致。 */
    {
        const AppConfig *c = app_cfg();
        const char *got_val = NULL;

        if (!strcmp(req.key, "theme"))
            got_val = c->ui.theme;
        else if (!strcmp(req.key, "fontsize"))
            got_val = c->ui.font_size;
        else if (!strcmp(req.key, "depth"))
            got_val = value;          /* 浮点，比较走规范化后的值 */
        else if (!strcmp(req.key, "level")) {
            static char lv[4];
            snprintf(lv, sizeof lv, "L%d", c->security.current_level);
            got_val = lv;
        }

        if (got_val && strcmp(got_val, value) != 0) {
            app_note("actprobe: %s FAIL 读回是 %s，写进去的却是 %s",
                     tag, got_val, value);
            ++g_fail;
            return;
        }
    }

    /* 成功**不逐条输出**。86 个用例全写进 status.txt 会溢出 notes 区
     * （上限 6144 字节，溢出后 app_note 直接丢弃 —— 连 RESULT 行都会被挤掉，
     *  而那行才是唯一要看的东西）。失败的才写，成功的最后汇总成一行。 */
    ++g_ok;
}

static void step(const char *tag, const char *hdr,
                 ActionId want_id, bool want_pass)
{
    step_ex(tag, hdr, want_id, want_pass, NULL);
}

void actprobe_run(void)
{
    g_ok = g_fail = 0;
    app_note("actprobe: 开始（设置类动作；会改 config.json）");

    /* ---- 1. 解析：动作名必须完整匹配 ----
     * "ui.themeX" 要被判成"不认识"，否则白名单就能被前缀绕过 ——
     * 那正是 actions_parse 早年用 strncmp 时踩过的坑。 */
    {
        char     clean[128];
        ActionReq req;
        ActionId id = actions_parse("[[ACT:ui.themeX value=dark]][[/ACT]]",
                                    clean, sizeof clean, &req);
        if (id == ACT_UNKNOWN) {
            app_note("actprobe: 拒绝前缀冒充 PASS（ui.themeX -> UNKNOWN）");
            ++g_ok;
        } else {
            app_note("actprobe: 拒绝前缀冒充 FAIL（ui.themeX -> %d）", (int)id);
            ++g_fail;
        }
    }

    /* ---- 2. 合法值应当通过并落盘 ---- */
    step("主题改黑夜",   "ui.theme value=dark",        ACT_UI_THEME,    true);
    step("主题改白天",   "ui.theme value=light",       ACT_UI_THEME,    true);
    step("字号大写",     "ui.fontsize value=BIG",      ACT_UI_FONTSIZE, true);
    step("立体深度",     "ui.depth value=60",          ACT_UI_DEPTH,    true);
    step("安全等级",     "sec.level value=L1",         ACT_SEC_LEVEL,   true);
    step("等级无L前缀",  "sec.level value=2",          ACT_SEC_LEVEL,   true);
    step("关自动更新",   "cfg.set key=ota.auto_check value=off",  ACT_CFG_SET, true);
    step("写入上限",     "cfg.set key=guard.max_write_kb value=1024", ACT_CFG_SET, true);

    /* ---- 3. 非法值必须被拒，且理由要说清 ---- */
    /* 下面这批都带 must_contain —— 拒绝理由里**必须出现那个具体的错值**。
     * 少了这一维，"被正确地拒"和"因为参数空所以被拒"长得一模一样：
     * 2026-10-05 23:53 那次 arg_get 的 bug 就是这么藏住的
     * （10 条非法值用例全绿，而它们全是因为 value 变空才被拒的）。 */
    step_ex("主题写错值",   "ui.theme value=blue",        ACT_UI_THEME,    false, "blue");
    step_ex("主题没给值",   "ui.theme",                   ACT_UI_THEME,    false, "(空)");
    step_ex("字号写错值",   "ui.fontsize value=huge",     ACT_UI_FONTSIZE, false, "huge");
    step_ex("深度超范围",   "ui.depth value=180",         ACT_UI_DEPTH,    false, "180");
    step_ex("深度非数字",   "ui.depth value=abc",         ACT_UI_DEPTH,    false, "abc");
    step_ex("深度带尾巴",   "ui.depth value=60abc",       ACT_UI_DEPTH,    false, "60abc");
    step_ex("等级越界",     "sec.level value=L9",         ACT_SEC_LEVEL,   false, "9");
    step_ex("未知设置键",   "cfg.set key=ui.bogus value=1",      ACT_CFG_SET, false, "ui.bogus");
    step_ex("布尔写成数字", "cfg.set key=ota.enabled value=maybe", ACT_CFG_SET, false, "maybe");
    step_ex("整数越界",     "cfg.set key=guard.max_batch_files value=99999", ACT_CFG_SET, false, "99999");

    /* ---- 3b. 只读动作 sys.info ----
     * 它不走 actions_check_setting（那是设置类专用的），
     * 所以这里单独验：能认出来、归类对、采集到东西、并且**真的进了回灌缓冲**。
     * 最后一条最要紧：结果没进缓冲，模型就永远拿不到，
     * 整个只读动作机制就是空转的。 */
    {
        char     reply[128], clean[128], info[ACT_RESULT_MAX];
        ActionReq req;
        ActionId id = actions_parse("[[ACT:sys.info]][[/ACT]]",
                                    clean, sizeof clean, &req);
        if (id != ACT_SYS_INFO) {
            app_note("actprobe: sys.info FAIL 解析得到 %d，期望 %d",
                     (int)id, (int)ACT_SYS_INFO);
            ++g_fail;
        } else if (!actions_is_readonly(id)) {
            app_note("actprobe: sys.info FAIL 认出来了但没归为只读类");
            ++g_fail;
        } else if (actions_is_setting(id)) {
            app_note("actprobe: sys.info FAIL 被错归为设置类（会走白名单校验）");
            ++g_fail;
        } else {
            sysinfo_collect(info, sizeof info);
            if (!info[0]) {
                app_note("actprobe: sys.info FAIL 采集到空");
                ++g_fail;
            } else if (!strstr(info, "机型")) {
                app_note("actprobe: sys.info FAIL 采集结果里没有机型那一条");
                ++g_fail;
            } else {
                /* 顺带看一眼有没有把凭据漏出去 —— sys.info 的结果
                 * 下一轮就发到云端了，登录态绝不能出现在里面。 */
                if (strstr(info, "sessionid=") || strstr(info, "ttwid=")) {
                    app_note("actprobe: sys.info FAIL 结果里混进了登录态凭据");
                    ++g_fail;
                } else {
                    app_act_result_store(info);
                    if (app_act_result_get() &&
                        strstr(app_act_result_get(), "机型")) {
                        app_note("actprobe: sys.info PASS 采集到 %d 字节并已进回灌缓冲",
                                 (int)strlen(info));
                        ++g_ok;
                    } else {
                        app_note("actprobe: sys.info FAIL 采集到了但没进回灌缓冲");
                        ++g_fail;
                    }
                    app_act_result_clear();
                }
            }
        }
    }

    /* ---- 3c. 沙箱只读：file.list / file.read ----
     *
     * ⚠️ 这一组**尚未在真机或模拟器上实测过**（积分约束，见记忆），
     * 所以这里除了"参数校验"，也把 dirent 能不能用这件事**如实报出来**：
     * opendir 失败时 sandfile_list 会写一句人话并返回 false，
     * 探针把它当"环境不支持"记下来，而不是当失败 ——
     * 分不清这两者的话，下次真机一跑就会误判成代码坏了。 */
    {
        char     clean[128];
        ActionReq req;
        char     info[ACT_RESULT_MAX];
        char     msg[192];

        /* 参数校验（离线可判，不依赖 dirent） */
        struct { const char *hdr; bool want; const char *why; const char *must; } CASES[] = {
            { "file.list",                    true,  "不给 sub = 根目录", NULL },
            { "file.list sub=out",            true,  "白名单子目录" },
            { "file.list sub=pkgs",           true,  "白名单子目录" },
            { "file.list sub=..",             false, "路径穿越子目录" },
            { "file.list sub=/",              false, "绝对路径" },
            { "file.list sub=config",         false, "白名单外" },
            { "file.read name=notes.txt",     true,  "纯文件名" },
            { "file.read name=..\\x",        false, "路径穿越" },
            { "file.read name=a/b",            false, "带斜杠" },
            { "file.read name=x from=0",       true,  "合法偏移" },
            { "file.read name=x from=abc",     false, "偏移非数字" },
            { "file.read name=x from=99999999", false, "偏移越界" },
        };

        for (unsigned i = 0; i < sizeof CASES / sizeof CASES[0]; ++i) {
            char reply[160];
            snprintf(reply, sizeof reply, "[[ACT:%s]][[/ACT]]", CASES[i].hdr);
            msg[0] = '\0';
            {
                ActionId id = actions_parse(reply, clean, sizeof clean, &req);
                bool got;
                if (id != ACT_FILE_LIST && id != ACT_FILE_READ) {
                    app_note("actprobe: %s FAIL 解析得到 %d",
                             CASES[i].why, (int)id);
                    ++g_fail;
                    continue;
                }
                got = actions_check_readonly(&req, msg, sizeof msg);
                if (got != CASES[i].want) {
                    app_note("actprobe: %s FAIL 校验=%s 期望=%s（%s）",
                             CASES[i].why, got ? "过" : "拒",
                             CASES[i].want ? "过" : "拒", msg);
                    ++g_fail;
                } else {
                    app_note("actprobe: %s PASS %s", CASES[i].why,
                             got ? "已放行" : msg);
                    ++g_ok;
                }
            }
        }

        /* dirent 能不能用：如实报，不假装通过 */
        {
            bool ok = sandfile_list("out", info, sizeof info);
            if (ok) {
                app_note("actprobe: file.list 采集到 %d 字节（dirent 可用）",
                         (int)strlen(info));
                ++g_ok;
            } else {
                app_note("actprobe: file.list 未能列目录（%s）"
                         " —— **尚未实测**，真机上这一项才能定论", info);
                ++g_fail;
            }
        }
        {
            /* 读一个必然不存在的文件：降级路径必须给出人话而不是空串 */
            sandfile_read("__actprobe_none__.txt", 0, info, sizeof info);
            if (info[0]) {
                app_note("actprobe: file.read 降级路径 PASS · %s", info);
                ++g_ok;
            } else {
                app_note("actprobe: file.read 降级路径 FAIL "
                         "（读不到时必须写清原因，不能留空）");
                ++g_fail;
            }
        }
    }

    /* ---- 3d. 安装类：font.install / model.install ----
     *
     * ⚠️ 同样**未经实测**。参数校验离线可判，先把这一层钉住；
     * 真要验"装上去之后界面变了"必须等模拟器/真机。 */
    {
        char     clean[128];
        ActionReq req;
        char     msg[192];

        struct { const char *hdr; bool want; const char *why; } CASES[] = {
            { "font.install name=my_font.ttf",   true,  "字库名合法（下划线合法）" },
            { "font.install name=my-font.bin",   true,  "字库名合法（连字符合法）" },
            { "font.install name=..\\x",        false, "字库名路径穿越" },
            { "font.install name=/abs/x",        false, "字库名绝对路径" },
            { "font.install",                    false, "没给字库名" },
            { "model.install kind=model",        true,  "类别 model" },
            { "model.install kind=kb",           true,  "类别 kb" },
            { "model.install kind=hw",           true,  "类别 hw" },
            { "model.install kind=model idx=0",  true,  "带序号" },
            { "model.install kind=guide",        false, "旧写法 guide（应为 kb）" },
            { "model.install kind=bogus",        false, "未知类别" },
            { "model.install",                   false, "没给类别" },
            { "model.install kind=model idx=99", false, "序号越界（校验层）" },
        };

        for (unsigned i = 0; i < sizeof CASES / sizeof CASES[0]; ++i) {
            char reply[160];
            snprintf(reply, sizeof reply, "[[ACT:%s]][[/ACT]]", CASES[i].hdr);
            msg[0] = '\0';
            {
                ActionId id = actions_parse(reply, clean, sizeof clean, &req);
                bool got;
                if (!actions_is_install(id)) {
                    app_note("actprobe: %s FAIL 解析得到 %d，不是安装类",
                             CASES[i].why, (int)id);
                    ++g_fail;
                    continue;
                }
                got = actions_check_install(&req, msg, sizeof msg);
                if (got != CASES[i].want) {
                    app_note("actprobe: %s FAIL 校验=%s 期望=%s（%s）",
                             CASES[i].why, got ? "过" : "拒",
                             CASES[i].want ? "过" : "拒", msg);
                    ++g_fail;
                } else {
                    app_note("actprobe: %s PASS %s", CASES[i].why,
                             got ? "已放行" : msg);
                    ++g_ok;
                }
            }
        }

        /* 类别表：手册里教模型的取值必须与代码认的完全一致。
         * 对不上时模型会一直用一个被拒的词，然后以为功能坏了。 */
        {
            const char *km = actions_model_kind(0);
            if (km && !strcmp(km, "model")) {
                app_note("actprobe: 类别表 PASS model/kb/hw 与代码一致");
                ++g_ok;
            } else {
                app_note("actprobe: 类别表 FAIL 第 0 类是 %s", km ? km : "(空)");
                ++g_fail;
            }
        }
    }

    /* ---- 3e. 文件与凭据类：这一组带不可逆后果，校验要最严 ---- */
    {
        char     clean[128];
        ActionReq req;
        char     msg[192];

        struct { const char *hdr; bool want; const char *why; } CASES[] = {
            { "file.delete name=old.txt",        true,  "删普通文件" },
            { "file.delete name=../../etc/x",    false, "路径穿越" },
            { "file.delete name=.trash",         false, "不许删回收站本身" },
            { "file.delete",                     false, "没给文件名" },
            { "file.rename name=a.txt to=b.txt", true,  "改名" },
            { "file.rename name=a.txt to=a.txt", false, "新旧同名（没东西可改）" },
            { "file.rename name=a.txt",          false, "没给新名" },
            { "file.rename name=a.txt to=../b",  false, "新名路径穿越" },
            { "file.rename name=a.txt to=.hidden", false, "新名以点开头" },
            { "key.remove name=old-key",         true,  "Key 条目名合法" },
            { "key.remove name=a/b",             false, "条目名带斜杠" },
            { "key.remove name=../x",            false, "条目名路径穿越" },
            { "key.switch name=my_key-2",        true,  "下划线与连字符合法" },
            { "key.remove",                      false, "没给条目名" },
        };

        for (unsigned i = 0; i < sizeof CASES / sizeof CASES[0]; ++i) {
            char reply[160];
            snprintf(reply, sizeof reply, "[[ACT:%s]][[/ACT]]", CASES[i].hdr);
            msg[0] = '\0';
            {
                ActionId id = actions_parse(reply, clean, sizeof clean, &req);
                bool got;
                if (!actions_is_fileop(id)) {
                    app_note("actprobe: %s FAIL 解析得到 %d，不是文件/凭据类",
                             CASES[i].why, (int)id);
                    ++g_fail;
                    continue;
                }
                got = actions_check_fileop(&req, msg, sizeof msg);
                if (got != CASES[i].want) {
                    app_note("actprobe: %s FAIL 校验=%s 期望=%s（%s）",
                             CASES[i].why, got ? "过" : "拒",
                             CASES[i].want ? "过" : "拒", msg);
                    ++g_fail;
                } else {
                    app_note("actprobe: %s PASS %s", CASES[i].why,
                             got ? "已放行" : msg);
                    ++g_ok;
                }
            }
        }

        /* ★ 二次确认登记：清空回收站必须被登记为"要按两次"，
         * 而 file.delete / key.remove 不该是（它们都能恢复或还有备份）。
         * 登记漏了的后果是永久删除只要按一次 A —— 而那是不可逆的。 */
        {
            ActionReq r2;
            char c2[128];
            ActionId a = actions_parse("[[ACT:trash.purge]][[/ACT]]", c2, sizeof c2, &r2);
            ActionId b = actions_parse("[[ACT:file.delete name=x]][[/ACT]]", c2, sizeof c2, &r2);
            if (a == ACT_TRASH_PURGE && actions_needs_double_confirm(a) &&
                !actions_needs_double_confirm(b)) {
                app_note("actprobe: 二次确认登记 PASS（只有 trash.purge 要两次）");
                ++g_ok;
            } else {
                app_note("actprobe: 二次确认登记 FAIL purge=%d purge需两次=%d delete需两次=%d",
                         (int)a, (int)actions_needs_double_confirm(a),
                         (int)actions_needs_double_confirm(b));
                ++g_fail;
            }
        }

        /* 回收站清单：即使空的也必须给出一句话而不是空串 */
        {
            char tl[224];
            if (actfile_trash_list(tl, sizeof tl) && tl[0]) {
                app_note("actprobe: 回收站清单 PASS · %s", tl);
                ++g_ok;
            } else {
                app_note("actprobe: 回收站清单 FAIL（空回收站也该有一句话）");
                ++g_fail;
            }
        }

        /* ★ 逆向安全：模型**不能写凭据**。整个动作白名单里不许有
         * 任何"写入一把新 Key"的入口 —— 模型无法验证 Key 有没有效，
         * 而它很可能编一个看起来像的写进去。 */
        {
            ActionId id = actions_parse("[[ACT:key.set name=x key=sk-fake]][[/ACT]]",
                                        clean, sizeof clean, &req);
            if (id == ACT_UNKNOWN) {
                app_note("actprobe: 拒绝模型写凭据 PASS（key.set -> UNKNOWN）");
                ++g_ok;
            } else {
                app_note("actprobe: 拒绝模型写凭据 FAIL（key.set 被认成了 %d）",
                         (int)id);
                ++g_fail;
            }
        }
    }

    /* ---- 4. 硬锁项：任何动作都改不了它 ----
     * 这一条最要紧：guard.confirm_overwrite 保护的是用户唯一无法撤销的操作。
     * 它不在白名单里，所以模型连"试一下"的机会都没有。 */
    step("改硬锁项",     "cfg.set key=guard.confirm_overwrite value=off",
        ACT_CFG_SET, false);

    /* ---- 3f. 密码门（L4/L5 的前提）----
     *
     * 这一组必须能**离线验**：密码机是纯逻辑（按键 → 状态），
     * 唯一的时间依赖是连点的时间窗，而探针里连点是同一个 tick 喂的，
     * 所以窗口判定恒成立 —— 正好让"输对能过、输错不能过"变成可测的。 */
    {
        static const u32 CODE[] = {
            KEY_UP, KEY_UP, KEY_DOWN, KEY_DOWN,
            KEY_LEFT, KEY_RIGHT, KEY_LEFT, KEY_RIGHT,
            KEY_B, KEY_A
        };
        int n = (int)(sizeof CODE / sizeof CODE[0]);
        int i;

        /* ★ 常量自证：把 10 个键的低 8 位与**期望序列**逐个比。
         *
         * 第一版写的是"10 个键互不相同" —— 那是错的：
         * KONAMI 码本来就有重复键（↑↑↓↓←→←→），这条判据必然红。
         * 教训：判据写"应该是什么样"，不要写"看起来应该怎样" ——
         * 后者会把实现的特点（重复）当成缺陷。
         *
         * 这条自证真正的价值：锁住"低 8 位的实际取值"。
         * 那个"下键被丢"的 bug 之所以难查，就是因为现象是
         * "输对了却不过"，指向密码逻辑，而真因是常量在 int 下为负。 */
        {
            static const u8 WANT[] = {
                0x40, 0x40, 0x80, 0x80, 0x20, 0x10, 0x20, 0x10, 0x02, 0x01
            };
            bool same = true;
            for (i = 0; i < n && i < (int)(sizeof WANT / sizeof WANT[0]); ++i)
                if ((CODE[i] & 0xFFu) != (u32)WANT[i])
                    same = false;
            if (same) {
                app_note("actprobe: KONAMI 常量自证 PASS（低 8 位与期望一致）");
                ++g_ok;
            } else {
                app_note("actprobe: KONAMI 常量自证 FAIL（第 %d 键是 %02x，期望 %02x）",
                         i, (unsigned)(CODE[i] & 0xFFu), (unsigned)WANT[i]);
                ++g_fail;
            }
        }

        /* 1. 连点六下 A → 提示出现 */
        konami_test_reset_all();
        for (i = 0; i < 6; ++i) konami_feed(KEY_A);
        if (konami_prompt_active()) {
            app_note("actprobe: 连点六下 PASS（提示已出现）");
            ++g_ok;
        } else {
            app_note("actprobe: 连点六下 FAIL（提示没出现）");
            ++g_fail;
        }

        /* 2. 输对密码 → 通过。
         *    注意这里是"连点六下之后紧接着输"，中间不留间隔 ——
         *    因为提示态的**第一个键就是密码的第一个键**（见 konami.c）。
         *    早先提示态会吞掉第一个键，于是这个用例 FAIL，
         *    而真机上是同一个毛病：用户看���提示后按第一个方向键，
         *    提示消失、密码错位，"明明输对了却不通过"。 */
        for (i = 0; i < n; ++i) konami_feed(CODE[i]);
        if (konami_unlocked()) {
            app_note("actprobe: KONAMI 码 PASS（已解锁）");
            ++g_ok;
        } else {
            app_note("actprobe: KONAMI 码 FAIL（输对了却没过）");
            ++g_fail;
        }

        /* 2b. 提示态里按 START 要能取消 —— 不然用户输到一半想退出
         *     就没有办法了（那一段密码会一直挂着）。 */
        konami_test_reset_all();
        for (i = 0; i < 6; ++i) konami_feed(KEY_A);
        konami_feed(KEY_UP);
        konami_feed(KEY_UP);
        konami_feed(KEY_START);
        if (!konami_unlocked() && !konami_prompt_active()) {
            app_note("actprobe: START 取消 PASS（提示已关、未解锁）");
            ++g_ok;
        } else {
            app_note("actprobe: START 取消 FAIL（unlocked=%d prompt=%d）",
                     (int)konami_unlocked(), (int)konami_prompt_active());
            ++g_fail;
        }

        /* 3. 输错 → 不得通过。
         *    这是最要紧的一条：密码门最危险的失败是"乱按也过"。 */
        konami_test_reset_all();
        for (i = 0; i < 6; ++i) konami_feed(KEY_A);
        for (i = 0; i < n; ++i)
            konami_feed(CODE[i] == KEY_DOWN ? KEY_UP : KEY_DOWN);
        if (!konami_unlocked()) {
            app_note("actprobe: 错误密码被拒 PASS");
            ++g_ok;
        } else {
            app_note("actprobe: 错误密码被拒 FAIL（乱按竟然过了）");
            ++g_fail;
        }

        /* 4. 不连点直接输密码 → 不得通过（提示没出现就不该接受） */
        konami_test_reset_all();
        for (i = 0; i < n; ++i) konami_feed(CODE[i]);
        if (!konami_unlocked()) {
            app_note("actprobe: 未开门就输码被拒 PASS");
            ++g_ok;
        } else {
            app_note("actprobe: 未开门就输码被拒 FAIL（没连点也过了）");
            ++g_fail;
        }
        konami_test_reset_all();
    }

    /* ---- 3g. 切服务商：白名单 + 连带改动 ---- */
    {
        char     clean[128], msg[192];
        ActionReq req;

        /* 数组名**不要用单字母 C** —— 它与 3ds.h/newlib 里的某个符号撞了，
         * 编译器会把它解析成指针类型，报 "'(struct <anonymous> *)&C' is a
         * pointer" 这种看不懂的错。上面三张用例表因此都叫 XXX_CASES。 */
        struct { const char *hdr; bool want; const char *why; const char *must; } SVC_CASES[] = {
            { "svc.set name=doubao",  true,  "切到豆包（白名单内）", NULL },
            { "svc.set",              false, "没给服务商名", "要写明切到哪家" },
            { "svc.set name=nope",    false, "白名单外", "nope" },
            { "svc.set name=../x",    false, "路径穿越式名字", "../x" },
        };
        for (unsigned i = 0; i < sizeof SVC_CASES / sizeof SVC_CASES[0]; ++i) {
            char reply[160];
            snprintf(reply, sizeof reply, "[[ACT:%s]][[/ACT]]", SVC_CASES[i].hdr);
            msg[0] = '\0';
            {
                ActionId id = actions_parse(reply, clean, sizeof clean, &req);
                bool got;
                if (id != ACT_SVC_SET) {
                    app_note("actprobe: %s FAIL 解析得到 %d", SVC_CASES[i].why, (int)id);
                    ++g_fail;
                    continue;
                }
                got = actions_check_svc(&req, msg, sizeof msg);
                if (got != SVC_CASES[i].want) {
                    app_note("actprobe: %s FAIL 校验=%s 期望=%s（%s）",
                             SVC_CASES[i].why, got ? "过" : "拒",
                             SVC_CASES[i].want ? "过" : "拒", msg);
                    ++g_fail;
                } else if (!got && SVC_CASES[i].must && !strstr(msg, SVC_CASES[i].must)) {
                    app_note("actprobe: %s FAIL 理由不含 %s（疑似假通过）· %s",
                             SVC_CASES[i].why, SVC_CASES[i].must, msg);
                    ++g_fail;
                } else {
                    ++g_ok;
                }
            }
        }
        /* 切一次真的，读回 provider 与 model —— **两样都要读**。
         * 只读 provider 抓不到"model 没跟着换"那个老 bug：
         * 症状是切到豆包却发 glm-4-flash，而 provider 看起来完全正确。 */
        {
            const char *first = provider_count() > 0 ? provider_at(0)->name : NULL;
            if (first && act_svc_set(first, msg, sizeof msg)) {
                const AppConfig *c = app_cfg();
                const ProviderDef *pd = provider_find(first);
                const char *want_model = (pd && pd->default_model[0])
                                       ? pd->default_model : first;
                if (!strcmp(c->ai.provider, first) &&
                    !strcmp(c->ai.model, want_model)) {
                    app_note("actprobe: svc.set 落盘读回 PASS（%s / %s）",
                             c->ai.provider, c->ai.model);
                    ++g_ok;
                } else {
                    app_note("actprobe: svc.set 落盘读回 FAIL（provider=%s model=%s，"
                             "期望 %s / %s）", c->ai.provider, c->ai.model,
                             first, want_model);
                    ++g_fail;
                }
            } else {
                app_note("actprobe: svc.set 落盘读回 FAIL · %s", msg);
                ++g_fail;
            }
        }
    }

    /* ---- 3h. 传输模块：离线能判的部分 ----
     *
     * ★ **真正传一个文件需要电脑上有个 HTTP 服务**，所以这里只能验
     *   ① 型号档位 ② 参数拒绝 ③ 降级路径（URL 不合法/连不上时怎么收场）。
     *   实测速度**尚未测过** —— 探针里已留 xferprobe 的位置，
     *   等有 PC 端服务时再补一条真的传。
     */
    {
        XferProfile prof;
        char msg[192];

        xfer_profile(&prof);
        if (prof.model && prof.is_new && prof.buf_size >= 256 * 1024) {
            app_note("actprobe: 传输档位 PASS（%s · %ld KB 缓冲 · 约 %ld KB/s）",
                     prof.model, prof.buf_size / 1024, prof.speed_hint);
            ++g_ok;
        } else if (prof.model && !prof.is_new && prof.buf_size <= 64 * 1024) {
            app_note("actprobe: 传输档位 PASS（旧机 · %ld KB 缓冲 · 约 %ld KB/s）",
                     prof.buf_size / 1024, prof.speed_hint);
            ++g_ok;
        } else {
            app_note("actprobe: 传输档位 FAIL（%s new=%d buf=%ld）",
                     prof.model ? prof.model : "(空)",
                     (int)prof.is_new, prof.buf_size);
            ++g_fail;
        }

        /* 参数拒绝：这些**必须**在联网之前就被挡住，
         * 否则一个非法 URL 会真的去连 —— 那既是安全问题也是白等。 */
        struct { const char *url; const char *name; bool want; const char *why; } XC[] = {
            { "file:///sdmc:/config.json", "x.cia",   false, "file:// 被拒" },
            { "ftp://x/y",                  "x.cia",   false, "ftp 被拒" },
            { "http://x/y",                 "a/b.cia", false, "名字带斜杠被拒" },
            { "http://x/y",                 "../x",    false, "名字带 .. 被拒" },
            { NULL,                          "x.cia",   false, "URL 为空被拒" },
        };
        for (unsigned i = 0; i < sizeof XC / sizeof XC[0]; ++i) {
            msg[0] = '\0';
            {
                /* 只验"在联网之前就拒"，所以用一个必然连不上的地址：
                 * 如果它居然去连了（耗时长），说明白名单没生效。 */
                bool got = xfer_pull(XC[i].url, "sdmc:/3ds/aiconsole/pkgs",
                                     XC[i].name, 0, NULL, NULL, NULL,
                                     msg, sizeof msg);
                if (got != XC[i].want) {
                    app_note("actprobe: %s FAIL 放行了=%d（%s）",
                             XC[i].why, (int)got, msg);
                    ++g_fail;
                } else {
                    ++g_ok;
                }
            }
        }
    }

    app_note("actprobe: RESULT %s ok=%d fail=%d",
             g_fail == 0 ? "PASS" : "FAIL", g_ok, g_fail);
    app_note("actprobe: 结束");
}
