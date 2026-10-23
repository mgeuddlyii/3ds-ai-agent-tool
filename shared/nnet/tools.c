/*
 * tools.c — 工具层（编排层 M6）
 *
 * 本轮交付：**算式求值 + 日期时间**。单位换算见文末 TODO，本轮**不做**
 * —— 它要处理词边界与类别判定，是这批里最容易出错的部分，而刚需是算式。
 * 宁可少做，也不要留半成品（占位桩比没有更糟）。
 *
 * ⚠️ 三条硬纪律，写在这免得以后被"优化"掉：
 *
 * 1. **全程 float，禁止 double。**
 *    3DS 的 VVP 只有 f32。写 double 会让 gcc 生成 vadd.f64 软浮点
 *    （项目规矩：objdump grep vadd.f64 必须为 0）。单 token 算力翻好几倍，
 *    而这里根本不需要双精度 —— 1+1 用 float 精确无误。
 *    → 所以**不许用 atof / pow / strtod / %f**，它们都是 double 版。
 *
 * 2. **不用 %f / %g 打印浮点。**
 *    newlib 的浮点格式化会把 dtoa 拖进运行期路径，Azahar 的小栈上炸过
 *    （见 sc_guide.c do_search 里的同类注释）。fmt_num() 全用整数运算
 *    手工拼字符串，绕开 dtoa。
 *
 * 3. **判不准就返回 TOOL_NONE。** 宁可漏答不可抢答：抢答会把真正的攻略
 *    问题吃掉，答成一句算术比不答更糟。
 */
#include <string.h>
#include <stdio.h>
#include <time.h>

#include "tools.h"

#define EXPR_MAX  64   /* 算式长度上限；递归深度另受 DEPTH_MAX 限制 */
#define DEPTH_MAX 16
#define POW_MAX   64   /* 指数上限，防 2^999999 把 CPU 打死 */

/* ------------------------------------------------------------------ UTF-8 */

static int u8_next(const char *s, int *adv)
{
    unsigned char c = (unsigned char)s[0];
    if (c == 0)              { *adv = 0; return 0; }
    if (c < 0x80)            { *adv = 1; return c; }
    if ((c & 0xE0) == 0xC0 && s[1]) {
        *adv = 2;
        return ((c & 0x1F) << 6) | ((unsigned char)s[1] & 0x3F);
    }
    if ((c & 0xF0) == 0xE0 && s[1] && s[2]) {
        *adv = 3;
        return ((c & 0x0F) << 12) |
               (((unsigned char)s[1] & 0x3F) << 6) |
               ((unsigned char)s[2] & 0x3F);
    }
    *adv = 1;
    return -1;
}

/* 汉字运算符 -> ASCII。乘以/除以 的「以」在 norm_expr 里单独吃掉。 */
static int cn_op(int cp)
{
    if (cp == 0x52A0) return '+';   /* 加 */
    if (cp == 0x51CF) return '-';   /* 减 */
    if (cp == 0x4E58) return '*';   /* 乘 */
    if (cp == 0x9664) return '/';   /* 除 */
    return 0;
}
#define CP_YI 0x4EE5   /* 以 */

/*
 * 归一化成 ASCII 算式，返回写入的字节数。
 *
 * 遇到映射不了的字符（汉字、字母）就**停下并返回已写出的前缀**，
 * 而不是整个失败 —— 因为「1+1等于几」这种问法很常见，答案在汉字之前。
 * 但前缀是否可用交给 expr_charset_ok 再判（它要求前缀里有运算符），
 * 所以「3ds有什么游戏」会因为前缀"3"没有运算符而被拒。
 */
static int norm_expr(const char *in, char *out, int outsz)
{
    int n = 0, i = 0, L = (int)strlen(in);

    while (i < L) {
        int adv, cp = u8_next(in + i, &adv);
        int c;

        if (adv <= 0) break;

        if (cp < 0) break;                                 /* 非法字节：截断 */

        if (cp >= 0xFF01 && cp <= 0xFF5E)            c = cp - 0xFEE0;   /* 全角ASCII */
        else if (cp == 0x3000)                       c = ' ';           /* 全角空格 */
        else if (cp == 0x00D7)                       c = '*';           /* × */
        else if (cp == 0x00F7)                       c = '/';           /* ÷ */
        else if (cp == 0x2212)                       c = '-';           /* − */
        else if (cp > 0x7F) {
            int op = cn_op(cp);
            if (!op) break;                                        /* 别的汉字：截断 */
            c = op;
            if (op == '*' || op == '/') {            /* 吃掉「乘以/除以」的「以」 */
                int a2, cp2 = u8_next(in + i + adv, &a2);
                if (cp2 == CP_YI) i += adv;
            }
        }
        else c = cp;

        if (c >= '0' && c <= '9') { /* 数字，续写 */ }
        else if (c=='.'||c=='+'||c=='-'||c=='*'||c=='/'||
                 c=='('||c==')'||c=='^'||c=='%'||c==' ') { }
        else break;                                  /* 白名单外的 ASCII：截断 */

        if (n >= outsz - 1) break;
        out[n++] = (char)c;
        i += adv;
    }
    out[n] = '\0';
    return n;
}

/*
 * 算式白名单。"判不准就不答"的第一道闸。
 * **必须同时含数字和运算符** —— 光有数字（「3ds」「2周目」）不是算式。
 */
static int expr_charset_ok(const char *s)
{
    int i, has_digit = 0, has_op = 0;
    if (!s || !*s) return 0;
    for (i = 0; s[i]; i++) {
        char c = s[i];
        if (c >= '0' && c <= '9') { has_digit = 1; continue; }
        if (c=='+'||c=='-'||c=='*'||c=='/'||c=='^'||c=='%') { has_op = 1; continue; }
        if (c=='.'||c=='('||c==')'||c==' ') continue;
        return 0;
    }
    return (has_digit && has_op);
}

/* ---------------------------------------------------------------- 数字输出 */

/* 手工格式化 float：完全不碰 dtoa / double。 */
static void fmt_num(float v, char *out, int outsz)
{
    long iv, ip, fp, t;
    int neg = 0, i, k = 0, w = 0, last;
    float av;
    char tmp[24];

    if (outsz <= 0) return;
    if (!(v == v)) { snprintf(out, (size_t)outsz, "无法计算"); return; }   /* NaN */
    if (v < 0) { neg = 1; v = -v; }
    av = v;
    if (av < 0.00005f) { snprintf(out, (size_t)outsz, "0"); return; }
    if (av > 2.0e9f)   { snprintf(out, (size_t)outsz, "数值太大"); return; }

    iv = (long)(av + 0.5f);
    if ((float)iv == av) {                                   /* 整数 */
        if (iv == 0) tmp[k++] = '0';
        t = iv;
        while (t > 0) { tmp[k++] = (char)('0' + (int)(t % 10)); t /= 10; }
        if (neg && w < outsz - 1) out[w++] = '-';
        while (k > 0 && w < outsz - 1) out[w++] = tmp[--k];
        out[w] = '\0';
        return;
    }

    iv = (long)(av * 10000.0f + 0.5f);                        /* 4 位小数 */
    ip = iv / 10000;
    fp = iv % 10000;
    k = 0;
    if (ip == 0) {
        tmp[k++] = '0';
    } else {
        t = ip;
        while (t > 0) { tmp[k++] = (char)('0' + (int)(t % 10)); t /= 10; }
    }
    for (i = 0; i < k / 2; i++) {                            /* 翻转 */
        char c = tmp[i]; tmp[i] = tmp[k - 1 - i]; tmp[k - 1 - i] = c;
    }
    if (neg && w < outsz - 1) out[w++] = '-';
    for (i = 0; i < k && w < outsz - 1; i++) out[w++] = tmp[i];
    if (w < outsz - 1) out[w++] = '.';
    {                                                       /* 小数补足四位 */
        char d[4];
        for (i = 3; i >= 0; i--) { d[i] = (char)('0' + (int)(fp % 10)); fp /= 10; }
        last = -1;
        for (i = 0; i < 4; i++) if (d[i] != '0') last = i;
        for (i = 0; i <= last && w < outsz - 1; i++) out[w++] = d[i];
    }
    out[w] = '\0';
}

/* ---------------------------------------------------------------- 求值器 */

typedef struct { const char *p; int err; int depth; } P;

static void p_ws(P *s) { while (*s->p == ' ') s->p++; }

static float p_expr(P *s);

static float p_prim(P *s)
{
    float v = 0.f, scale;

    p_ws(s);
    if (*s->p == '(') {
        s->p++;
        if (++s->depth > DEPTH_MAX) { s->err = 1; return 0.f; }
        v = p_expr(s);
        s->depth--;
        p_ws(s);
        if (*s->p != ')') { s->err = 1; return 0.f; }
        s->p++;
        return v;
    }
    if (*s->p == '-') { s->p++; return -p_prim(s); }
    if (*s->p == '+') { s->p++; return  p_prim(s); }

    if (*s->p >= '0' && *s->p <= '9') {
        while (*s->p >= '0' && *s->p <= '9') {
            v = v * 10.f + (float)(*s->p - '0');
            s->p++;
        }
        if (*s->p == '.') {
            s->p++;
            scale = 0.1f;
            while (*s->p >= '0' && *s->p <= '9') {
                v += (float)(*s->p - '0') * scale;
                scale *= 0.1f;
                s->p++;
            }
        }
        return v;
    }
    s->err = 1;
    return 0.f;
}

static float p_pow(P *s)
{
    float base = p_prim(s), r = 1.f;
    long n, i;

    p_ws(s);
    if (*s->p != '^') return base;
    s->p++;
    n = (long)p_pow(s);                 /* 右结合 */
    if (n < 0) return 0.f;
    if (n > POW_MAX) n = POW_MAX;
    for (i = 0; i < n; i++) r *= base;
    return r;
}

static float p_term(P *s)
{
    float v = p_pow(s);
    for (;;) {
        char op;
        float r;
        p_ws(s);
        op = *s->p;
        if (op != '*' && op != '/' && op != '%') return v;
        s->p++;
        r = p_pow(s);
        if (op == '*') {
            v *= r;
        } else if (op == '/') {
            if (r == 0.f) { s->err = 2; return 0.f; }        /* 除零 */
            v /= r;
        } else {
            if (r == 0.f) { s->err = 2; return 0.f; }
            v = (float)((long)v % (long)r);
        }
        if (s->err) return 0.f;
    }
}

static float p_expr(P *s)
{
    float v = p_term(s);
    for (;;) {
        char op;
        float r;
        p_ws(s);
        op = *s->p;
        if (op != '+' && op != '-') return v;
        s->p++;
        r = p_term(s);
        if (op == '+') v += r; else v -= r;
        if (s->err) return 0.f;
    }
}

int tools_eval(const char *expr, float *out_val)
{
    char buf[EXPR_MAX];
    P s;

    if (!expr || !out_val) return 0;
    /* norm_expr 现在只会返回 >=0（不合格时截断成前缀），合法性全交给
     * expr_charset_ok 判。这里不再重复判一次 <=0，那是没有意义的守卫。 */
    (void)norm_expr(expr, buf, (int)sizeof buf);
    if (!expr_charset_ok(buf)) return 0;

    s.p = buf; s.err = 0; s.depth = 0;
    *out_val = p_expr(&s);
    if (s.err) return -1;

    p_ws(&s);
    if (*s.p != '\0') return 0;          /* 没消费完 -> 不是合法算式 */
    return 1;
}

/* ---------------------------------------------------------------- 日期时间 */

static ToolKind try_time(const char *q, char *out, int outsz)
{
    time_t t;
    struct tm *v;
    int want_date = 0, want_clock = 0, want_wday = 0;

    if (strstr(q, "几号") || strstr(q, "多少号") ||
        strstr(q, "日期") || strstr(q, "今天几"))
        want_date = 1;
    if (strstr(q, "几点") || strstr(q, "现在几点") || strstr(q, "什么时间"))
        want_clock = 1;
    if (strstr(q, "星期") || strstr(q, "周几"))
        want_wday = 1;
    if (!want_date && !want_clock && !want_wday) return TOOL_NONE;

    t = time(NULL);
    if (t == (time_t)-1) { snprintf(out, (size_t)outsz, "设备时间还没设置"); return TOOL_TIME; }
    v = localtime(&t);
    /* 年份 < 100 说明没对上真实时间（RTC 停在 1970 附近） */
    if (!v || v->tm_year < 100) { snprintf(out, (size_t)outsz, "设备时间还没设置"); return TOOL_TIME; }

    if (want_clock)      snprintf(out, (size_t)outsz, "现在是 %d点%02d分", v->tm_hour, v->tm_min);
    else if (want_wday)  snprintf(out, (size_t)outsz, "今天是星期%s",
                                  (const char *[]){"日","一","二","三","四","五","六"}[v->tm_wday % 7]);
    else                 snprintf(out, (size_t)outsz, "今天是 %d年%d月%d日",
                                  v->tm_year + 1900, v->tm_mon + 1, v->tm_mday);
    return TOOL_TIME;
}

/* ---------------------------------------------------------------- 单位换算 */

/* 单位表。
 *   cat  同一类才能互换；基准单位：长度=米、质量=千克、容量=升
 *   to_base = 1 个该单位等于多少基准单位
 *
 * ⚠️ 温度**不在这张表里** —— 它是仿射变换（有零点偏移），
 *    乘一个系数算不出来，必须单独走 cat==CAT_TEMP 的分支。
 *
 * ⚠️ 匹配必须**最长优先**：
 *    "公里"里含"里"、"千米"里含"米"、"厘米"里含"米"。
 *    按表顺序一遇到就停会匹配错，所以下面 match_unit 会挑最长的那个。 */
enum { CAT_LEN = 1, CAT_MASS, CAT_VOL, CAT_TEMP };

typedef struct {
    const char *name;
    int         cat;
    float       to_base;
} UnitDef;

static const UnitDef UNITS[] = {
    /* ---- 长度（基准：米）---- */
    { "公里",     CAT_LEN, 1000.0f },
    { "千米",     CAT_LEN, 1000.0f },
    { "厘米",     CAT_LEN,    0.01f },
    { "毫米",     CAT_LEN,    0.001f },
    { "英里",     CAT_LEN, 1609.344f },
    { "英尺",     CAT_LEN,    0.3048f },
    { "英寸",     CAT_LEN,    0.0254f },
    { "米",       CAT_LEN,    1.0f },
    { "里",       CAT_LEN,  500.0f },
    { "码",       CAT_LEN,    0.9144f },
    { "km",       CAT_LEN, 1000.0f },
    { "cm",       CAT_LEN,    0.01f },
    { "mm",       CAT_LEN,    0.001f },
    { "mi",       CAT_LEN, 1609.344f },
    { "ft",       CAT_LEN,    0.3048f },
    { "yd",       CAT_LEN,    0.9144f },
    { "m",        CAT_LEN,    1.0f },

    /* ---- 质量（基准：千克）---- */
    { "公斤",     CAT_MASS,   1.0f },
    { "千克",     CAT_MASS,   1.0f },
    { "克",       CAT_MASS,   0.001f },
    { "斤",       CAT_MASS,   0.5f },
    { "吨",       CAT_MASS, 1000.0f },
    { "磅",       CAT_MASS,   0.45359237f },
    { "盎司",     CAT_MASS,   0.028349523125f },
    { "kg",       CAT_MASS,   1.0f },
    { "lb",       CAT_MASS,   0.45359237f },
    { "oz",       CAT_MASS,   0.028349523125f },
    { "g",        CAT_MASS,   0.001f },

    /* ---- 容量（基准：升）---- */
    { "毫升",     CAT_VOL,    0.001f },
    { "升",       CAT_VOL,    1.0f },
    { "加仑",     CAT_VOL,    3.785411784f },
    { "ml",       CAT_VOL,    0.001f },
    { "l",        CAT_VOL,    1.0f },

    /* ---- 温度（仿射，to_base 不用）---- */
    { "摄氏度",   CAT_TEMP,   0.0f },
    { "华氏度",   CAT_TEMP,   0.0f },
    { "开尔文",   CAT_TEMP,   0.0f },
    /* 裸「度」按中文习惯当摄氏。加它是因为"0度等于多少华氏度"是最自然的
     * 问法，不认会显得很蠢；误伤面很小 —— 必须**同时**出现两个温度单位
     * 才会答，像"30度角"这种只有一个单位，仍然返回 NONE。 */
    { "度",       CAT_TEMP,   0.0f },
    { "℃",        CAT_TEMP,   0.0f },
    { "℉",        CAT_TEMP,   0.0f },
};

#define UNIT_N (int)(sizeof UNITS / sizeof UNITS[0])

static int is_alnum_ascii(char c)
{
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

/* 前一个字符只禁止**字母**，不禁数字 ——
 * "30cm" 里 "cm" 前面就是数字，禁掉的话最简单的写法反而不认。
 * 禁字母是为了挡住 "km" 里的 "m"、"min" 里的 "in" 这类误匹配。 */
static int is_alpha_ascii(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

/* 从 s[from] 开始找第一个能匹配上的单位；找到返回 1，并给出
 * 起始下标 *pos、结束下标 *end、以及单位定义。
 * **同一位置上取最长的那个**（"公里" 优先于 "里"）。 */
static int match_unit(const char *s, int from, int *pos, int *end, const UnitDef **def)
{
    int i, k;
    int blen = -1;
    const UnitDef *bd = NULL;
    int bpos = -1;

    for (i = from; s[i]; ++i) {
        for (k = 0; k < UNIT_N; ++k) {
            size_t L = strlen(UNITS[k].name);
            if ((int)L <= blen)
                continue;                   /* 已经有更长的匹配了 */
            if (strncmp(s + i, UNITS[k].name, L) != 0)
                continue;
            /* ASCII 别名要做词边界：否则 "m" 会匹配到 "km" 的中间、
             * "in"(英寸) 会匹配到 "min" 里 —— 那种误判会让答案离谱。 */
            if (L <= 2 && is_alnum_ascii(UNITS[k].name[0])) {
                if (i > 0 && is_alpha_ascii(s[i - 1]))
                    continue;
                if (is_alnum_ascii(s[i + (int)L]))
                    continue;
            }
            blen = (int)L;
            bd   = &UNITS[k];
            bpos = i;
        }
        if (bd)
            break;                          /* 这个起点已经找到，取它 */
    }
    if (!bd)
        return 0;
    *pos = bpos;
    *end = bpos + blen;
    *def = bd;
    return 1;
}

/* 从 s[from] 开始找一个数（支持小数）。返回 1 表示找到。 */
static int match_number(const char *s, int from, int *pos, int *end, float *val)
{
    int i = from, st;
    float v = 0.0f, frac = 0.1f;
    int seen = 0, dot = 0;

    while (s[i] && !(s[i] >= '0' && s[i] <= '9'))
        ++i;
    if (!s[i])
        return 0;
    st = i;
    while (s[i]) {
        if (s[i] >= '0' && s[i] <= '9') {
            if (!dot) v = v * 10.0f + (float)(s[i] - '0');
            else { v += (float)(s[i] - '0') * frac; frac *= 0.1f; }
            seen = 1;
            ++i;
        } else if (s[i] == '.' && !dot) {
            dot = 1;
            ++i;
        } else {
            break;
        }
    }
    if (!seen)
        return 0;
    *pos = st;
    *end = i;
    *val = v;
    return 1;
}

static float temp_to_c(float v, const UnitDef *u)
{
    const char *n = u->name;
    if (strstr(n, "华氏") || strcmp(n, "℉") == 0) return (v - 32.0f) * 5.0f / 9.0f;
    if (strstr(n, "开尔文"))                       return v - 273.15f;
    return v;                                       /* 摄氏 */
}

static float c_to_temp(float c, const UnitDef *u)
{
    const char *n = u->name;
    if (strstr(n, "华氏") || strcmp(n, "℉") == 0) return c * 9.0f / 5.0f + 32.0f;
    if (strstr(n, "开尔文"))                       return c + 273.15f;
    return c;
}

static ToolKind try_unit(const char *q, char *out, int outsz)
{
    int np, ne, u1p, u1e, u2p, u2e;
    float num = 0.0f, res;
    const UnitDef *u1 = NULL, *u2 = NULL;
    char val[32];

    /* 1) 数值 + 第一个单位 */
    if (!match_number(q, 0, &np, &ne, &num))
        return TOOL_NONE;
    if (!match_unit(q, ne, &u1p, &u1e, &u1))
        return TOOL_NONE;

    /* 2) 第二个单位。中间允许夹"等于/是多少/换算成/转成/= /? /多少 /几" */
    if (!match_unit(q, u1e, &u2p, &u2e, &u2))
        return TOOL_NONE;

    /* 3) 同类才能换算。不同类时**不抢答** —— 宁可漏答，
     *    也不要把"5 公里 3 公斤"这种句子答成一句换算。 */
    if (u1->cat != u2->cat)
        return TOOL_NONE;
    if (u1e == u2p)
        return TOOL_NONE;                   /* 同一个单位，没意义 */

    if (u1->cat == CAT_TEMP)
        res = c_to_temp(temp_to_c(num, u1), u2);
    else
        res = num * u1->to_base / u2->to_base;

    fmt_num(num, val, (int)sizeof val);
    snprintf(out, (size_t)outsz, "%s%s = ", val, u1->name);
    fmt_num(res, val, (int)sizeof val);
    {
        size_t used = strlen(out);
        snprintf(out + used, (size_t)(outsz - (int)used), "%s%s", val, u2->name);
    }
    return TOOL_UNIT;
}

/* ---------------------------------------------------------------- 入口 */

ToolKind tools_try(const char *q, char *out, int outsz)
{
    float v;
    char shown[EXPR_MAX];
    char res[32];
    int rc, n;

    if (!q || !out || outsz <= 0) return TOOL_NONE;
    out[0] = '\0';

    /* 1) 算式。绝大多数攻略问句在白名单这一关就出局了。 */
    rc = tools_eval(q, &v);
    if (rc == 1) {
        fmt_num(v, res, (int)sizeof res);
        n = norm_expr(q, shown, (int)sizeof shown);
        if (n > 0) {
            if (n > 34) { memcpy(shown + 31, "...", 4); }
            snprintf(out, (size_t)outsz, "%s = %s", shown, res);
        } else {
            snprintf(out, (size_t)outsz, "等于 %s", res);
        }
        return TOOL_CALC;
    }
    if (rc == -1) {
        snprintf(out, (size_t)outsz, "这个算式算不出来（检查有没有除零或括号不配对）");
        return TOOL_CALC;
    }

    /* 2) 日期时间 */
    if (try_time(q, out, outsz) != TOOL_NONE) return TOOL_TIME;

    /* 3) 单位换算（长度 / 质量 / 容量 / 温度）。
     * 放在算式之后：像 "5*3" 这种没有单位，会被上面吃掉；
     * 而 "5公里等于多少里" 在算式那关就出局（含非算式字符），落到这里。
     * 同类才换算；不同类返回 NONE —— 宁可漏答，不可抢答。 */
    if (try_unit(q, out, outsz) != TOOL_NONE) return TOOL_UNIT;

    return TOOL_NONE;
}
