/*
 * uistr.c —— 文案表的运行时（选语言、越界回落）
 */

#include <string.h>

#include "uistr.h"

/* uistr_table.c 生成的（那张表由 tools/i18n_table.py 自动产出）。
 * 数组本体在那边是 static，只有这几个符号能出去。 */
extern const char *const *uistr_tbl(int lang);
extern int uistr_count(void);

#define UISTR_LANG_ZH 0
#define UISTR_LANG_EN 1

static int g_lang = UISTR_LANG_ZH;

const char *uistr(int id)
{
    const char *const *tbl = uistr_tbl(g_lang);

    /* 越界回落：返回第一条而不是崩溃或空串。理由见 uistr.h ——
     * 编号写错是开发期笔误，而"因为笔误让用户看到乱码"比
     * "看到一句没翻的英文"糟糕得多。
     *
     * （正常编译下这一段永远不成立：编号是枚举常量，越界编译器就报错了。
     *   留着是因为 uistr() 也可能被传一个运行期算出来的数。） */
    if (id < 1 || id > UISTR_LAST)
        return tbl[0];

    return tbl[id - 1];
}

int uistr_lang(void) { return g_lang; }

bool uistr_lang_known(const char *code)
{
    if (!code) return false;
    return !strcmp(code, "zh") || !strcmp(code, "en");
}

const char *uistr_lang_name(int lang)
{
    return lang == UISTR_LANG_EN ? "English" : "中文";
}

int uistr_set_lang(int lang)
{
    /* 忽略非法值：界面上算出个奇怪下标时，宁可什么都不改，
     * 也不要让语言进入"既不是中文也不是英文"的未定义态 ——
     * 那种态下 uistr_tbl() 会读越界。 */
    if (lang != UISTR_LANG_ZH && lang != UISTR_LANG_EN)
        return g_lang;
    g_lang = lang;
    return g_lang;
}

void uistr_init_from_config(void)
{
    /* 刻意留空：语言**不在这里**从配置读。
     * 原因是时序 —— 首启引导第一屏就要显示文案，而那一刻配置未必
     * 已经落盘。让调用方（app_init）读出 cfg->ui.lang 再调 uistr_apply()，
     * 依赖是显式的，也不会出现"两处各读一次、读到不同时刻的值"。
     * 保留这个函数是为了让"文案系统已就绪"有一个明确入口。 */
}

void uistr_apply(const char *code)
{
    if (!uistr_lang_known(code)) {
        g_lang = UISTR_LANG_ZH;
        return;
    }
    g_lang = !strcmp(code, "en") ? UISTR_LANG_EN : UISTR_LANG_ZH;
}