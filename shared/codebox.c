/*
 * codebox.c -- 见 codebox.h
 */

#include "codebox.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "actions.h"        /* actions_dir()：沙箱里唯一允许写的地方 */

#define CODEBOX_MAX_FILES 8
#define CODEBOX_MAX_BODY  (24 * 1024)   /* 单文件上限：够放一个 .c，超了截断 */

/* 围栏语言 → 扩展名。只认常见的几种；认不出就用 txt。
 * ⚠️ 别写成"把语言名直接当扩展名"：模型会写 `cpp`、`C++`、`console`、
 *    甚至 `bash session`，那些名字要么非法（含 +），要么 3DS 上没人认。 */
static const char *ext_of(const char *lang, size_t n)
{
    static const struct { const char *k; const char *v; } kMap[] = {
        { "c",        "c"   }, { "h",       "h"   },
        { "cpp",      "cpp" }, { "c++",     "cpp" }, { "cc",   "cpp" },
        { "cs",       "cs"  }, { "csharp",  "cs"  },
        { "python",   "py"  }, { "py",      "py"  },
        { "js",       "js"  }, { "javascript", "js" }, { "ts", "ts" },
        { "json",     "json"}, { "lua",     "lua" },
        { "makefile", "mk"  }, { "make",    "mk"  }, { "sh",   "sh" },
        { "bash",     "sh"  }, { "shell",   "sh"  }, { "bat",  "bat" },
        { "html",     "html"}, { "css",     "css" }, { "xml",  "xml" },
        { "yml",      "yml" }, { "yaml",    "yml" },
        { "asm",      "s"   }, { "s",       "s"   },
    };
    char buf[16];
    size_t i;

    if (!lang || n == 0)
        return "txt";
    if (n >= sizeof buf)
        n = sizeof buf - 1;
    for (i = 0; i < n; ++i) {
        char c = lang[i];
        if (c >= 'A' && c <= 'Z')
            c = (char)(c - 'A' + 'a');
        buf[i] = c;
    }
    buf[n] = '\0';

    for (i = 0; i < sizeof kMap / sizeof kMap[0]; ++i) {
        if (!strcmp(buf, kMap[i].k))
            return kMap[i].v;
    }
    return "txt";
}

/* 找一个没被占用的名字：code001.c / code002.c …
 * 用"试到不冲突"而不是自增计数器：计数器放内存里，重启就归零，
 * 会覆盖用户上次存的文件 —— 那些文件他可能还没看。 */
static bool fresh_path(char *out, size_t cap, const char *ext)
{
    int n;

    for (n = 1; n <= 999; ++n) {
        struct stat st;

        snprintf(out, cap, "%s/code%03d.%s", actions_dir(), n, ext);
        if (stat(out, &st) != 0)
            return true;
    }
    return false;
}

int codebox_extract(const char *text, char *summary, size_t cap)
{
    const char *p;
    int         n = 0;
    int         truncated_any = 0;

    if (summary && cap)
        summary[0] = '\0';
    if (!text)
        return 0;

    p = text;
    while (n < CODEBOX_MAX_FILES) {
        const char *open = strstr(p, "```");
        const char *lang_s, *lang_e, *body, *end;
        size_t      blen;

        if (!open)
            break;

        lang_s = open + 3;
        lang_e = lang_s;
        while (*lang_e && *lang_e != '\n' && *lang_e != '\r')
            ++lang_e;
        if (!*lang_e)
            break;                       /* 只有开围栏没有内容：当普通文本 */

        body = lang_e;
        while (*body == '\n' || *body == '\r')
            ++body;

        end = strstr(body, "```");
        if (!end)
            end = body + strlen(body);   /* 忘了收尾围栏：就取到结尾 */
        blen = (size_t)(end - body);

        {
            char        path[192];
            const char *ext = ext_of(lang_s, (size_t)(lang_e - lang_s));
            FILE       *f;

            if (fresh_path(path, sizeof path, ext) &&
                (f = fopen(path, "wb")) != NULL) {
                const char *base = strrchr(path, '/');
                size_t wlen = blen;

                if (wlen > CODEBOX_MAX_BODY) {
                    wlen = CODEBOX_MAX_BODY;
                    truncated_any = 1;
                }
                if (wlen)
                    fwrite(body, 1, wlen, f);
                if (truncated_any && wlen == CODEBOX_MAX_BODY)
                    fwrite("\n/* truncated by AIConsole */\n", 1, 29, f);
                fclose(f);
                ++n;

                /* 把文件名记进 summary（只记名字，句子由 UI 层拼 ——
                 * 这个文件在 shared/ 层，不该出现中文字面量）。 */
                if (summary && cap && base) {
                    size_t used = strlen(summary);

                    if (used && used + 1 < cap)
                        snprintf(summary + used, cap - used, " ");
                    used = strlen(summary);
                    if (used + 1 < cap)
                        snprintf(summary + used, cap - used, "%s", base + 1);
                }
            }
        }

        p = (*end) ? end + 3 : end;
    }

    if (n == 0 && summary && cap)
        summary[0] = '\0';
    return n;
}

/* 见 codebox.h。走一条最朴素的扫描：找第一个 ```，跳过该行的语言标记，
 * 抄到下一个 ``` 为止。**不做语法判断** —— 围栏里面是什么语言与"复制"
 * 这件事无关，多解析一层只会多一种出错方式。 */
size_t codebox_first_block(const char *text, char *out, size_t cap)
{
    const char *p, *body, *end;
    size_t      n;

    if (!text || !out || cap < 2)
        return 0;
    out[0] = '\0';

    p = strstr(text, "```");
    if (!p)
        return 0;

    p += 3;
    /* 跳过语言标记（同一行剩余部分） */
    while (*p && *p != '\n')
        ++p;
    if (*p == '\n')
        ++p;
    body = p;

    end = strstr(body, "```");
    if (!end)
        return 0;

    /* 逐行抄，**剥掉装饰竖条**（"| "）。
     * 不剥的话，复制出来的代码每行都带着一条竖线 —— 粘到别处就是错的。 */
    {
        const char *q = body;
        size_t      w = 0;

        while (q < end && w + 1 < cap) {
            if (q[0] == '|' && q[1] == ' ') {
                q += 2;
                continue;
            }
            out[w++] = *q++;
        }
        out[w] = '\0';
        return w;
    }
}

/* 见 codebox.h。逐行扫：在围栏之间、且不是围栏行本身 → 前面加 "▍ "。
 * 用临时缓冲再整体回写：原地插入会让后面的内容搬两次，容易写错。 */
void codebox_decorate(char *buf, size_t cap)
{
    static char tmp[4096];
    const char *p;
    size_t      o = 0;
    int         in_code = 0;

    if (!buf || cap < 8 || strlen(buf) >= sizeof tmp)
        return;

    p = buf;
    while (*p) {
        const char *nl = strchr(p, '\n');
        size_t      n  = nl ? (size_t)(nl - p) : strlen(p);
        int         fence = (n >= 3 && p[0] == '`' && p[1] == '`' && p[2] == '`');

        if (fence) {
            in_code = !in_code;
            if (o + n + 1 < sizeof tmp) {
                memcpy(tmp + o, p, n);
                o += n;
                tmp[o++] = '\n';
            }
        } else if (in_code && n > 0) {
            /* ⚠️ 原打算用 U+258D「▍」，但本机字库**没有这个字形** ——
             *   屏上显示成豆腐块（实拍确认）。改用 ASCII '| '：
             *   字库里必然有，而且本来就是"代码行"的通用记号。 */
            static const char BAR[] = "| ";
            if (o + 4 + n + 1 < sizeof tmp) {
                memcpy(tmp + o, BAR, 4);
                o += 4;
                memcpy(tmp + o, p, n);
                o += n;
                tmp[o++] = '\n';
            }
        } else if (o + n + 1 < sizeof tmp) {
            memcpy(tmp + o, p, n);
            o += n;
            tmp[o++] = '\n';
        }

        if (!nl)
            break;
        p = nl + 1;
    }
    tmp[o] = '\0';

    if (o < cap)
        memcpy(buf, tmp, o + 1);
}
