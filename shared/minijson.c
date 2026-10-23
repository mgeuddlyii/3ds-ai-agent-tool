/*
 * minijson.c -- 见 minijson.h
 *
 * 递归下降。每个值解析前把"父路径"传下去，对象成员拼 ".key"，
 * 数组元素拼 "[i]"。路径缓冲是栈上的，深度上限 12 层，
 * 每层 200 字节 -> 最坏 2.4 KB 栈，3DS 主线程栈够用。
 */

#include "minijson.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MJ_MAX_DEPTH  12
#define MJ_PATH_MAX   200
#define MJ_KEY_MAX    80

/* 解码后的字符串缓冲。
 *
 * ⚠️ 这里曾经是栈上的 `char scratch[2048]`。看着够用，实则不够 ——
 * 因为 JSON 的转义是**会缩短**的：`"\\n"` 两个字节解出来是一个换行。
 * 于是"写进去 2068 字节、读出来 1024 字节"是常态，而
 * "写进去 3070 字节、读出来 3007 字节"就溢出了 2048。
 *
 * 溢出的后果不是崩溃，而是 **整条记录静默消失**：
 * parse_string 返回 -1 → parse_value 失败 → minijson_walk 返回 false，
 * 调用方（chatlog_load）只看到 g_text[0] == '\0'，判为"这行没有 text"，
 * 悄悄跳过。用户看到的是"聊过的某条不见了"，没有任何报错。
 * 2026-10-03 由 P 趟（长消息回落）实测抓到：chat.json 三条齐全，
 * 但 app 只 restored=2。
 *
 * 修法与行缓冲同理：**按需增长**，不再对"一条字符串能有多长"下注。
 * 结构体本身也因此改成堆分配（scratch 从栈上搬走）—— 一个 2KB 的
 * 大数组压在递归下降的栈帧里，本来就不该有。
 */
#define MJ_STR_MIN    256              /* 起步大小，常见配置项一两个字就够 */

/* 硬闸：解码后超过这个长度就判文件坏了。取 64KB ——
 * 比任何合法字段都大得多（最长的是对话正文，上界 1KB 量级），
 * 又小到不会因为一个坏文件把内存吃光。 */
#define MJ_STR_MAX    65536

typedef struct {
    const char *p;
    const char *end;
    char       *scratch;               /* 堆上，按需 realloc */
    size_t      scratch_cap;
    MiniJsonFn  fn;
    void       *user;
    int         unknown;               /* 供调用方统计（这里不发 MJ_ERROR） */
} Ctx;

static bool emit(Ctx *c, MiniJsonValue *v)
{
    return c->fn ? c->fn(v, c->user) : true;
}

static bool is_ws(char ch)
{
    return ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r';
}

static void skip_ws(Ctx *c)
{
    while (c->p < c->end && is_ws(*c->p))
        ++c->p;
}

static int hexval(char ch)
{
    if (ch >= '0' && ch <= '9') return ch - '0';
    if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
    if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
    return -1;
}

/* 确保 c->scratch 至少能放下 need 个字节（不含结尾 '\0'）。
 * 与行缓冲同理：读满就翻倍，绝不用"一条字符串不该这么长"当作失败的理由。
 * 唯一的硬闸是 MJ_STR_MAX，到顶就返回失败 ——
 * 那时说明文件确实坏了，而不是我们的假设不成立。 */
static bool ensure(Ctx *c, size_t need)
{
    size_t want;

    if (c->scratch_cap > need)              /* 还够（要给结尾 '\0' 留一位） */
        return true;
    if (need >= MJ_STR_MAX)
        return false;                       /* 硬闸：到这个长度就是文件坏了 */

    want = c->scratch_cap ? c->scratch_cap : MJ_STR_MIN;
    while (want <= need)
        want = want * 2 > MJ_STR_MAX ? MJ_STR_MAX : want * 2;

    char *nl = realloc(c->scratch, want);
    if (!nl)
        return false;
    c->scratch     = nl;
    c->scratch_cap = want;
    return true;
}

/* 从 '"' 开始解析一个字符串到 out[cap]。
 * 返回写入字节数；放不下（或转义非法）返回 -1。
 *
 * 这是**定长**版本：它不会自己长大，调用方负责给够空间。
 * 变长的场景走 parse_string_grow（在外面套一层"装不下就翻倍重试"）。 */
static int parse_string_buf(Ctx *c, char *out, size_t cap)
{
    size_t n = 0;

    if (c->p >= c->end || *c->p != '"')
        return -1;
    ++c->p;

    while (c->p < c->end) {
        unsigned char ch = (unsigned char)*c->p++;

        if (ch == '"') {
            out[n] = '\0';
            return (int)n;
        }

        if (ch == '\\') {
            if (c->p >= c->end)
                return -1;
            char esc = *c->p++;
            switch (esc) {
                case '"':  ch = '"';  break;
                case '\\': ch = '\\'; break;
                case '/':  ch = '/';  break;
                case 'b':  ch = '\b'; break;
                case 'f':  ch = '\f'; break;
                case 'n':  ch = '\n'; break;
                case 'r':  ch = '\r'; break;
                case 't':  ch = '\t'; break;
                case 'u': {
                    if (c->end - c->p < 4)
                        return -1;
                    int v = 0;
                    for (int i = 0; i < 4; ++i) {
                        int h = hexval(c->p[i]);
                        if (h < 0)
                            return -1;
                        v = (v << 4) | h;
                    }
                    c->p += 4;
                    /* 我们的 schema 里没有代理对，只处理 BMP 基本平面。 */
                    if (v < 0x80) {
                        if (n + 1 >= cap) return -1;
                        out[n++] = (char)v;
                    } else if (v < 0x800) {
                        if (n + 2 >= cap) return -1;
                        out[n++] = (char)(0xC0 | (v >> 6));
                        out[n++] = (char)(0x80 | (v & 0x3F));
                    } else {
                        if (n + 3 >= cap) return -1;
                        out[n++] = (char)(0xE0 | (v >> 12));
                        out[n++] = (char)(0x80 | ((v >> 6) & 0x3F));
                        out[n++] = (char)(0x80 | (v & 0x3F));
                    }
                    continue;
                }
                default:
                    return -1;
            }
        }

        if (n + 1 >= cap)
            return -1;
        out[n++] = (char)ch;
    }
    return -1;
}

/* 定长字段（key）：超出就失败 —— key 是 schema 的一部分，长了说明文件坏了。 */
static int parse_string(Ctx *c, char *out, size_t cap)
{
    return parse_string_buf(c, out, cap);
}

/* 变长字符串：落在 c->scratch 里。
 *
 * parse_string_buf 是"给我一个上界、我在里面写"的形状，本身不知道
 * 什么时候该长大。所以这里在外面重试：装不下就翻倍再来一遍。
 * 每轮都要从**字符串起点**重新解析（c->p 已经被上一轮推走了），
 * 所以先把起点记下来。重试次数是 O(log n)，代价可以忽略 ——
 * 而且以"猜小了"为常态，反而比一开始就分配一只大的更省内存。 */
static int parse_string_grow(Ctx *c)
{
    const char *start = c->p;
    int         n;

    /* 首次使用就分配起步大小。注意 ensure 的语义是"至少能放下 need 个"，
     * 传 0 就是"保证有一块非空缓冲"。 */
    if (!ensure(c, 0))
        return -1;

    for (;;) {
        c->p = start;
        n    = parse_string_buf(c, c->scratch, c->scratch_cap);
        if (n >= 0)
            return n;

        /* 失败可能是"装不下"，也可能是"这个字符串本身就是坏的"。
         * 前者翻倍能好；后者翻到硬闸还是失败，由下面两行收场。
         * 这里**不能**把失败直接当格式错误返回 —— 那正是旧实现
         * 静默丢记录的那条路（见文件头的说明）。 */
        if (c->scratch_cap >= MJ_STR_MAX)
            return -1;
        if (!ensure(c, c->scratch_cap))     /* 翻倍；拿到硬闸也失败即放弃 */
            return -1;
    }
}

static bool parse_value(Ctx *c, const char *path, int index, int depth);

/* index：本对象若位于数组内，为其下标；否则 -1。
 * 必须一路传下去 —— 对象里的字段要沿用同一个下标，否则
 * "artifacts":[{...},{...}] 这种结构就分不清哪个字段属于哪个元素。
 * 早先这里没有 index 参数，字段被硬编码成 -1，对象数组等于不可用。 */
static bool parse_object(Ctx *c, const char *path, int index, int depth)
{
    ++c->p;                        /* 吃掉 '{' */
    skip_ws(c);

    if (c->p < c->end && *c->p == '}') {
        ++c->p;
        return true;
    }

    for (;;) {
        char key[MJ_KEY_MAX];
        char sub[MJ_PATH_MAX];

        skip_ws(c);
        if (parse_string(c, key, sizeof key) < 0)
            return false;

        skip_ws(c);
        if (c->p >= c->end || *c->p != ':')
            return false;
        ++c->p;

        if (path[0] == '\0')
            snprintf(sub, sizeof sub, "%s", key);
        else
            snprintf(sub, sizeof sub, "%s.%s", path, key);

        if (!parse_value(c, sub, index, depth + 1))
            return false;

        skip_ws(c);
        if (c->p < c->end && *c->p == ',') {
            ++c->p;
            continue;
        }
        if (c->p < c->end && *c->p == '}') {
            ++c->p;
            return true;
        }
        return false;              /* 既不是 ',' 也不是 '}' */
    }
}

static bool parse_array(Ctx *c, const char *path, int depth)
{
    int idx = 0;

    ++c->p;                        /* 吃掉 '[' */
    skip_ws(c);

    if (c->p < c->end && *c->p == ']') {
        ++c->p;
        return true;
    }

    for (;;) {
        if (!parse_value(c, path, idx, depth + 1))
            return false;
        ++idx;

        /* 元素自己会带 [i]，所以父路径不拼接，只把 index 传下去。 */

        skip_ws(c);
        if (c->p < c->end && *c->p == ',') {
            ++c->p;
            skip_ws(c);
            continue;
        }
        if (c->p < c->end && *c->p == ']') {
            ++c->p;
            return true;
        }
        return false;
    }
}

static bool parse_value(Ctx *c, const char *path, int index, int depth)
{
    MiniJsonValue v;

    if (depth > MJ_MAX_DEPTH)
        return false;

    skip_ws(c);
    if (c->p >= c->end)
        return false;

    memset(&v, 0, sizeof v);
    v.path  = path;
    v.index = index;

    switch (*c->p) {
        case '{':
            v.type = MJ_OBJECT_START;
            if (!emit(c, &v)) return false;
            if (!parse_object(c, path, index, depth)) return false;
            v.type = MJ_OBJECT_END;
            return emit(c, &v);

        case '[':
            v.type = MJ_ARRAY_START;
            if (!emit(c, &v)) return false;
            if (!parse_array(c, path, depth)) return false;
            v.type = MJ_ARRAY_END;
            return emit(c, &v);

        case '"': {
            /* 走可增长路径：这里的长度由**文件内容**决定（对话正文可能是
             * 用户打的几千字），不能拿一个定长栈数组去接。 */
            int n = parse_string_grow(c);
            if (n < 0)
                return false;
            v.type = MJ_STRING;
            v.str  = c->scratch;
            return emit(c, &v);
        }

        default:
            break;
    }

    if (c->end - c->p >= 4 && !strncmp(c->p, "true", 4)) {
        c->p += 4;
        v.type = MJ_BOOL; v.boolean = true; v.num = 1.0;
        return emit(c, &v);
    }
    if (c->end - c->p >= 5 && !strncmp(c->p, "false", 5)) {
        c->p += 5;
        v.type = MJ_BOOL; v.boolean = false; v.num = 0.0;
        return emit(c, &v);
    }
    if (c->end - c->p >= 4 && !strncmp(c->p, "null", 4)) {
        c->p += 4;
        v.type = MJ_NULL;
        return emit(c, &v);
    }

    {
        /* 手工十进制解析，刻意不用 strtod：newlib 的 strtod 背后拖着 dtoa
         * 一大家子（__hexdig / Bigint），单次调用要吃 2-3KB 栈；3DS homebrew
         * 的主栈很小，曾把栈底打穿（unmapped Read32 @ 0x07FFFFF8 -> PC=0）。
         * 我们的 schema 只有版本号、计数和 0~1 的小数，用不上通用浮点解析。
         * 有效数字截到 17 位，指数限幅 ±300，恶意长数字也不会爆。 */
        bool neg = false;
        double d = 0.0;
        int    exp10 = 0;
        int    ndig = 0;

        if (c->p < c->end && *c->p == '-') {
            neg = true;
            ++c->p;
        }
        while (c->p < c->end && *c->p >= '0' && *c->p <= '9') {
            if (ndig < 17)
                d = d * 10.0 + (double)(*c->p - '0');
            ++c->p;
            ++ndig;
        }
        if (c->p < c->end && *c->p == '.') {
            ++c->p;
            while (c->p < c->end && *c->p >= '0' && *c->p <= '9') {
                if (ndig < 17) {
                    d = d * 10.0 + (double)(*c->p - '0');
                    --exp10;
                }
                ++c->p;
                ++ndig;
            }
        }
        if (ndig == 0)
            return false;              /* 一个数字都没有，不是 number */

        if (c->p < c->end && (*c->p == 'e' || *c->p == 'E')) {
            const char *save = c->p;
            int e = 0, esign = 1;

            ++c->p;
            if (c->p < c->end && (*c->p == '+' || *c->p == '-')) {
                if (*c->p == '-')
                    esign = -1;
                ++c->p;
            }
            if (c->p < c->end && *c->p >= '0' && *c->p <= '9') {
                while (c->p < c->end && *c->p >= '0' && *c->p <= '9') {
                    if (e < 300)
                        e = e * 10 + (*c->p - '0');
                    ++c->p;
                }
                exp10 += esign * e;
            } else {
                c->p = save;           /* e 后面没数字，当没写过 */
            }
        }

        while (exp10 > 0) { d *= 10.0; --exp10; }
        while (exp10 < 0) { d *= 0.1;  ++exp10; }
        if (neg)
            d = -d;

        v.type = MJ_NUMBER;
        v.num  = d;
        return emit(c, &v);
    }
}

bool minijson_walk(const char *text, size_t len, MiniJsonFn fn, void *user)
{
    Ctx c;

    memset(&c, 0, sizeof c);
    c.p    = text;
    c.end  = text + len;
    c.fn   = fn;
    c.user = user;

    /* UTF-8 BOM：Windows 上手动编辑过的文件很容易带上 */
    if (len >= 3 && (unsigned char)text[0] == 0xEF
                 && (unsigned char)text[1] == 0xBB
                 && (unsigned char)text[2] == 0xBF)
        c.p += 3;

    /* 字符串缓冲改到堆上之后，出口就必须自己收尾。
     * 回调里拿到的 v->str 指向这块内存 —— 所以**只能在 walk 返回之后**释放，
     * 这也是为什么不在每个 MJ_STRING 处 free：那样回调拿到的就是悬垂指针。 */
    bool ok = parse_value(&c, "", -1, 0);

    if (ok) {
        skip_ws(&c);
        ok = (c.p == c.end);
    }

    free(c.scratch);
    return ok;
}

const char *minijson_type_name(MiniJsonType t)
{
    switch (t) {
        case MJ_NULL:         return "null";
        case MJ_BOOL:         return "bool";
        case MJ_NUMBER:       return "number";
        case MJ_STRING:       return "string";
        case MJ_ARRAY_START:  return "[";
        case MJ_ARRAY_END:    return "]";
        case MJ_OBJECT_START: return "{";
        case MJ_OBJECT_END:   return "}";
        default:              return "error";
    }
}
