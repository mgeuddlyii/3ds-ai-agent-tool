/*
 * html.c -- 见 html.h
 *
 * 单遍扫描，没有回溯，也没有 DOM。
 *
 * 三个要点：
 *  1. script / style 的内容必须整个跳过 —— 否则用户会看到一坨 JS/CSS 源码
 *     被当成正文渲染出来，那比"不支持"难看得多。
 *  2. 标签名一律转小写比较：HTML 不区分大小写，写 <P> 和 <p> 是同一件事。
 *  3. 实体只认最常用的六个。其余原样保留 —— 把一个不认识的实体吞掉，
 *     比留着让用户看见更糟（他会以为内容丢了）。
 */

#include "html.h"

#include <stdio.h>
#include <string.h>

typedef struct {
    HtmlDoc *doc;
    char     buf[HTML_TEXT_MAX];
    size_t   len;
    HtmlLineKind kind;
    int      indent;
    int      list_depth;
} Ctx;

/* 把累积的一行存进文档。空行不存，但会结束当前行。 */
static void flush(Ctx *c)
{
    if (c->len == 0)
        return;
    if (c->doc->count >= HTML_MAX_LINES)
        return;

    HtmlLine *l = &c->doc->line[c->doc->count];
    l->kind   = c->kind;
    l->indent = c->indent;
    memcpy(l->text, c->buf, c->len);
    l->text[c->len] = '\0';

    c->doc->count++;
    c->len = 0;
}

static void put(Ctx *c, char ch)
{
    if (c->len + 1 < HTML_TEXT_MAX)
        c->buf[c->len++] = ch;
}

static void put_utf8(Ctx *c, unsigned cp)
{
    /* 只处理 BMP 里的常用区，够用了 */
    if (cp < 0x80) {
        put(c, (char)cp);
    } else if (cp < 0x800) {
        put(c, (char)(0xC0 | (cp >> 6)));
        put(c, (char)(0x80 | (cp & 0x3F)));
    } else {
        put(c, (char)(0xE0 | (cp >> 12)));
        put(c, (char)(0x80 | ((cp >> 6) & 0x3F)));
        put(c, (char)(0x80 | (cp & 0x3F)));
    }
}

/* 读一个实体，从 '&' 之后开始。返回消耗的字节数；不认识则返回 0（原样输出 '&'）。 */
static size_t entity(const char *s, size_t avail, unsigned *out)
{
    size_t i = 1;                       /* s[0] == '&' */

    if (i >= avail)
        return 0;

    if (s[i] == '#') {
        unsigned cp = 0;
        bool hex = false;
        ++i;
        if (i < avail && (s[i] == 'x' || s[i] == 'X')) { hex = true; ++i; }
        if (i >= avail) return 0;
        for (; i < avail && s[i] != ';'; ++i) {
            int d;
            if (s[i] >= '0' && s[i] <= '9')      d = s[i] - '0';
            else if (hex && s[i] >= 'a' && s[i] <= 'f') d = s[i] - 'a' + 10;
            else if (hex && s[i] >= 'A' && s[i] <= 'F') d = s[i] - 'A' + 10;
            else return 0;
            cp = cp * (hex ? 16u : 10u) + (unsigned)d;
            if (cp > 0x10FFFF) return 0;
        }
        if (i >= avail || s[i] != ';') return 0;
        /* ★ 空数字实体（`&#;` / `&#x;`）必须在这里挡掉，不能靠上面的循环。
         * 这两种情况下 for 一次都不执行，而 `s[i] == ';'` 又让上一行的
         * 检查通过 —— 于是 cp 还是初值 0，却被当作"解析成功"返回。
         * 调用方会把 U+0000 写进渲染缓冲，整行在 NUL 处被截断
         * （后面的字全丢，看起来像"更新说明被吃掉一半"）。
         * 合法的 `&#0;` 也是 NUL，同样不该往文本里塞。
         * → 判据：cp == 0 一律算不认识的实体，原样输出 '&'。 */
        if (cp == 0) return 0;
        *out = cp;
        return i + 1;
    }

    static const struct { const char *name; unsigned cp; } TAB[] = {
        { "lt", '<' }, { "gt", '>' }, { "amp", '&' },
        { "quot", '"' }, { "apos", '\'' }, { "nbsp", 0xA0 },
    };

    for (; i < avail && s[i] != ';'; ++i)
        ;
    if (i >= avail || s[i] != ';')
        return 0;

    size_t nlen = i - 1;                /* '&' 与 ';' 之间的名字长度 */
    for (size_t k = 0; k < sizeof TAB / sizeof TAB[0]; ++k) {
        if (strlen(TAB[k].name) == nlen &&
            !strncmp(s + 1, TAB[k].name, nlen)) {
            *out = TAB[k].cp;
            return i + 1;
        }
    }
    return 0;
}

/* 读标签名。返回名字长度；'/' 开头表示结束标签（*closing = true）。 */
static size_t tag_name(const char *s, size_t avail, char *out, size_t cap,
                       bool *closing)
{
    size_t i = 1;                       /* s[0] == '<' */
    size_t n = 0;

    *closing = false;
    if (i < avail && s[i] == '/') { *closing = true; ++i; }

    while (i < avail && n + 1 < cap) {
        char ch = s[i];
        if (ch == '>' || ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n')
            break;
        out[n++] = (char)((ch >= 'A' && ch <= 'Z') ? ch - 'A' + 'a' : ch);
        ++i;
    }
    out[n] = '\0';
    return n;
}

/* 跳过到指定结束标签之后（用于 script / style）。 */
static size_t skip_to_close(const char *s, size_t avail, const char *name)
{
    size_t i = 0;
    while (i + 1 < avail) {
        if (s[i] == '<' && (s[i + 1] == '/')) {
            /* 可能是我们要找的结束标签 */
            size_t j = i + 2;
            size_t k = 0;
            while (j < avail && k < strlen(name) &&
                   ((s[j] >= 'A' && s[j] <= 'Z') ? s[j] - 'A' + 'a' : s[j])
                       == name[k]) {
                ++j; ++k;
            }
            if (k == strlen(name) && j < avail && s[j] == '>')
                return j + 1;
        }
        ++i;
    }
    return avail;
}

static void apply_tag(Ctx *c, const char *name, bool closing)
{
    if (closing) {
        if (!strcmp(name, "ul") || !strcmp(name, "ol")) {
            if (c->list_depth > 0) --c->list_depth;
        }
        /* 块级结束标签：结束当前行 */
        if (!strcmp(name, "h1") || !strcmp(name, "h2") || !strcmp(name, "h3") ||
            !strcmp(name, "p")  || !strcmp(name, "li") || !strcmp(name, "div") ||
            !strcmp(name, "blockquote"))
            flush(c);
        return;
    }

    /* 开始标签：先结束上一行 */
    if (!strcmp(name, "br")) {
        flush(c);
        return;
    }
    if (!strcmp(name, "h1"))      { flush(c); c->kind = HL_H1;   c->indent = 0; return; }
    if (!strcmp(name, "h2"))      { flush(c); c->kind = HL_H2;   c->indent = 0; return; }
    if (!strcmp(name, "h3"))      { flush(c); c->kind = HL_H3;   c->indent = 0; return; }
    if (!strcmp(name, "p") ||
        !strcmp(name, "div"))     { flush(c); c->kind = HL_TEXT; c->indent = 0; return; }
    if (!strcmp(name, "li"))      { flush(c); c->kind = HL_ITEM; c->indent = c->list_depth; return; }
    if (!strcmp(name, "blockquote")) { flush(c); c->kind = HL_QUOTE; c->indent = 1; return; }
    if (!strcmp(name, "ul") ||
        !strcmp(name, "ol"))      { flush(c); ++c->list_depth; return; }

    /* 其余（含 b / i / a / span）：不换行，标签本身也不显示 */
}

int html_parse(const char *src, size_t n, HtmlDoc *doc)
{
    Ctx c;

    if (!src || !doc)
        return -1;

    memset(doc, 0, sizeof *doc);
    memset(&c, 0, sizeof c);
    c.doc = doc;
    c.kind = HL_TEXT;

    for (size_t i = 0; i < n;) {
        char ch = src[i];

        if (ch == '<') {
            char name[24];
            bool closing;
            size_t nl = tag_name(src + i, n - i, name, sizeof name, &closing);

            if (nl == 0) {              /* 不是标签，当普通字符 */
                put(&c, ch);
                ++i;
                continue;
            }

            /* script / style：整段跳过，别把代码当正文渲染 */
            if (!closing && (!strcmp(name, "script") || !strcmp(name, "style"))) {
                i += skip_to_close(src + i, n - i, name);
                continue;
            }

            apply_tag(&c, name, closing);

            /* 跳到 '>' 之后 */
            size_t j = i + 1;
            if (closing) ++j;
            j += nl;
            while (j < n && src[j] != '>')
                ++j;
            i = (j < n) ? j + 1 : n;
            continue;
        }

        if (ch == '&') {
            unsigned cp = 0;
            size_t used = entity(src + i, n - i, &cp);
            if (used > 0) {
                if (cp == 0xA0)
                    put(&c, ' ');       /* 不间断空格当普通空格 */
                else
                    put_utf8(&c, cp);
                i += used;
                continue;
            }
            put(&c, ch);
            ++i;
            continue;
        }

        if (ch == '\n' || ch == '\r' || ch == '\t') {
            /* 空白折叠成一个空格。HTML 本来就是这样 */
            if (c.len > 0 && c.buf[c.len - 1] != ' ')
                put(&c, ' ');
            ++i;
            continue;
        }

        put(&c, ch);
        ++i;
    }

    flush(&c);
    return doc->count;
}

bool html_parse_file(const char *path, HtmlDoc *doc)
{
    FILE *f;
    /* 64KB 上限：更新说明不可能更大，超了就截 —— 不会因此崩 */
    static char buf[65536];
    size_t n;

    if (!path || !doc)
        return false;

    f = fopen(path, "rb");
    if (!f)
        return false;

    n = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    buf[n] = '\0';

    html_parse(buf, n, doc);
    return true;
}
