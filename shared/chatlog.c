/*
 * chatlog.c -- 见 chatlog.h
 *
 * 两种格式：
 *   JSONL（持久化）        {"mine":1,"text":"..."}  一行一条
 *   纯文本（导出）         我：...
 *                        AI：...
 *
 * 文本格式刻意不用 Markdown：它要能在 3DS 的记事本、Windows 的写字板里
 * 都直接读，而这两个东西对 Markdown 的态度是"原样显示星号"。
 */

#include "chatlog.h"

#include "minijson.h"
#include "util.h"

#include <3ds.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHATLOG_DIR_PATH "sdmc:/3ds/aiconsole"
#define CHATLOG_JSON     CHATLOG_DIR_PATH "/chat.json"

static FILE *g_fp;
static int   g_items;
static bool  g_human;        /* 这次写的是给人看的文本还是持久化 JSONL */
static char  g_last_path[160];

/* --------------------------------------------------------------------------
 * JSON 字符串输出（带转义）
 *
 * 非 ASCII 原样输出 —— UTF-8 字节都 ≥ 0x80，转成 \uXXXX 反而会把中文
 * 变成别的东西（对端解出来是另一串字符）。
 * ----------------------------------------------------------------------- */
static void put_str(FILE *f, const char *s)
{
    fputc('"', f);
    for (const unsigned char *p = (const unsigned char *)s; *p; ++p) {
        switch (*p) {
            case '"':  fputs("\\\"", f); break;
            case '\\': fputs("\\\\", f); break;
            case '\n': fputs("\\n",  f); break;
            case '\r': fputs("\\r",  f); break;
            case '\t': fputs("\\t",  f); break;
            default:
                if (*p < 0x20) fprintf(f, "\\u%04x", (unsigned)*p);
                else           fputc((int)*p, f);
        }
    }
    fputc('"', f);
}

/* 目录创建与时间戳都在 util.c —— 动作审计（actions.c）用同一份，
 * 免得两边各有一套日历换算、哪天修了一边另一边还是旧的。 */

/* --------------------------------------------------------------------------
 * 查询
 * ----------------------------------------------------------------------- */

const char *chatlog_dir(void)  { return CHATLOG_DIR_PATH; }
const char *chatlog_path(void) { return CHATLOG_JSON; }

bool chatlog_exists(void)
{
    FILE *f = fopen(CHATLOG_JSON, "rb");
    if (!f)
        return false;
    fclose(f);
    return true;
}

/* --------------------------------------------------------------------------
 * 写
 * ----------------------------------------------------------------------- */

bool chatlog_open(bool human, char *out_path, size_t cap)
{
    char name[64];

    if (g_fp)
        return false;                    /* 上一次没 close：拒绝重入 */

    util_mkdirs(CHATLOG_DIR_PATH);
    g_items = 0;
    g_human = human;
    g_last_path[0] = '\0';

    if (human) {
        util_stamp(name, sizeof name);
        snprintf(g_last_path, sizeof g_last_path,
                 CHATLOG_DIR_PATH "/chat_%s.txt", name);
        g_fp = fopen(g_last_path, "wb");
        if (g_fp) {
            fputs("3DS AI Console -- 对话记录\n", g_fp);
            fputs("（导出时间 ", g_fp);
            fputs(name, g_fp);
            fputs("，本地时区）\n\n", g_fp);
        }
    } else {
        snprintf(g_last_path, sizeof g_last_path, "%s", CHATLOG_JSON);
        g_fp = fopen(g_last_path, "wb");
    }

    if (!g_fp) {
        g_last_path[0] = '\0';
        return false;
    }

    if (out_path && cap) {
        snprintf(out_path, cap, "%s", g_last_path);
    }
    return true;
}

void chatlog_item(bool mine, const char *text)
{
    if (!g_fp || !text)
        return;

    /* 条数上限只在**持久化**时兜底 —— 那是"最多留多少条历史"的产品决定。
     *
     * 导出（human=true）**不能**跟着这条上限走：导出是"把我聊过的导出去"，
     * 超过 40 条的部分凭空消失，而屏幕上还写着"已导出 N 条"——
     * 用户没有任何办法知道少了什么。这里改成不设限：
     * 导出文件写多大由用户自己点的那一下决定，不该由持久化的窗口大小决定。 */
    if (!g_human && g_items >= CHATLOG_MAX_TURNS)
        return;

    if (g_human) {
        /* 纯文本：一行一条，中文标签 */
        fputs(mine ? "我：" : "AI：", g_fp);
        fputs(text, g_fp);
        fputs("\n\n", g_fp);
    } else {
        /* JSONL：一行一条，读的时候可以逐行解析 */
        fputs("{\"mine\":", g_fp);
        fputs(mine ? "1" : "0", g_fp);
        fputs(",\"text\":", g_fp);
        put_str(g_fp, text);
        fputs("}\n", g_fp);
    }

    ++g_items;
}

/* 文件超过这个大小就压一次：保留最近 CHATLOG_KEEP_LINES 行。
 * ★ 为什么需要它：追加写是只增不减的，几百轮之后 chat.json 会越读越慢
 *   （3DS 的 sdmc 读大文件本来就慢）。压的时候**按行保留**，
 *   不做解析 —— JSONL 一条一行，按行截断天然不会切坏一条记录。 */
#define CHATLOG_COMPACT_BYTES (128 * 1024)
#define CHATLOG_KEEP_LINES    80

/* 把文件压缩成"最后 keep 行"。返回 true 表示压过了。
 *
 * ★ 走 .tmp + rename：压缩本身也不能冒着把历史弄丢的风险。
 * ★ 按行切，不解析 JSON —— JSONL 一条一行，按行截断天然不会切坏一条记录；
 *   而"解析后重写"要多一份解析器，那才是会出错的地方。
 */
static bool compact_keep_last(int keep)
{
    FILE *in, *out;
    char *buf;
    char tmp[160];
    long size, rd_start, len, from = 0;
    int n = 0;
    size_t got;

    if (keep <= 0)
        return false;

    in = fopen(CHATLOG_JSON, "rb");
    if (!in)
        return false;
    if (fseek(in, 0, SEEK_END) != 0) { fclose(in); return false; }
    size = ftell(in);
    if (size <= 0) { fclose(in); return false; }

    /* 只往回读最多 96 KB：压缩的目的是控制体积，不需要把整份读进内存 */
    rd_start = size > 96 * 1024 ? size - 96 * 1024 : 0;
    len      = size - rd_start;
    if (fseek(in, rd_start, SEEK_SET) != 0) { fclose(in); return false; }

    buf = (char *)malloc((size_t)len + 1);
    if (!buf) { fclose(in); return false; }
    got = fread(buf, 1, (size_t)len, in);
    fclose(in);
    buf[got] = '\0';
    len = (long)got;

    /* 从尾向前数 keep 个换行，得到保留段的起点 from（buf 内偏移） */
    for (long i = len - 1; i >= 0; --i) {
        if (buf[i] == '\n' && ++n > keep) { from = i + 1; break; }
    }
    if (n <= keep) { free(buf); return false; }        /* 行数不够，不用压 */

    /* 从文件中间开始读时，buf 的第一行可能是半截 ——
     * 真正要写的起点就落在半截行上时，往后挪到下一个换行之后。 */
    if (rd_start > 0 && from == 0) {
        for (long i = 0; i < len; ++i) {
            if (buf[i] == '\n') { from = i + 1; break; }
        }
    }
    if (from >= len) { free(buf); return false; }

    snprintf(tmp, sizeof tmp, "%s.tmp", CHATLOG_JSON);
    out = fopen(tmp, "wb");
    if (!out) { free(buf); return false; }

    if (fwrite(buf + from, 1, (size_t)(len - from), out) != (size_t)(len - from)) {
        free(buf);
        fclose(out);
        remove(tmp);
        return false;
    }
    free(buf);

    if (fclose(out) != 0) { remove(tmp); return false; }
    if (rename(tmp, CHATLOG_JSON) != 0) { remove(tmp); return false; }
    return true;
}

bool chatlog_append(bool mine, const char *text)
{
    FILE *f;
    long size = 0;

    if (!text || !text[0])
        return false;               /* 空内容不该在历史里留一行空壳 */

    util_mkdirs(CHATLOG_DIR_PATH);

    /* 先看多大，超限就先压一次 */
    f = fopen(CHATLOG_JSON, "rb");
    if (f) {
        if (fseek(f, 0, SEEK_END) == 0)
            size = ftell(f);
        fclose(f);
    }
    if (size > CHATLOG_COMPACT_BYTES)
        compact_keep_last(CHATLOG_KEEP_LINES);

    /* ★ "ab" 而不是 "wb"：**这是整个修复的关键**。
     *   "wb" 会在打开瞬间清空文件，中途出任何事都是全丢。 */
    f = fopen(CHATLOG_JSON, "ab");
    if (!f)
        return false;

    fputs("{\"mine\":", f);
    fputs(mine ? "1" : "0", f);
    fputs(",\"text\":", f);
    put_str(f, text);
    fputs("}\n", f);

    /* 写失败要报出来：fclose 会把缓冲刷下去，它失败才是真的没写进去 */
    if (fflush(f) != 0 || ferror(f)) {
        fclose(f);
        return false;
    }
    return fclose(f) == 0;
}

bool chatlog_close(void)
{
    if (!g_fp)
        return false;
    fclose(g_fp);
    g_fp = NULL;
    return true;
}

/* --------------------------------------------------------------------------
 * 读
 *
 * ⚠️ 这里曾经是 `char g_line[CHATLOG_TEXT_MAX + 256]`（1280 字节）+ fgets，
 * 看着够（1024 字节正文 + 结构开销），实则**不够** —— 因为写盘时转义会膨胀：
 * `\n` `\t` `\r` 各占 2 字节，`\uXXXX` 占 6 字节。一条 1024 字节的正文
 * 若全是换行，写出来是 2068 字节。
 *
 * 后果不是崩溃，而是**静默丢历史**：fgets 把 1279 字节读走后截断，
 * 剩下那截在下一次调用里被当成新的一行读进来；它不以 '{' 开头，
 * 于是被"跳过非 JSON 行"的规则吃掉。用户看到的是"聊过的某条不见了"。
 * 差一点就撞上：O 趟的演示正文正好有 9 个换行。
 *
 * 修法不是"把缓冲调大" —— 转义膨胀的上限是 6×，那要 6KB，
 * 等于拿空间换一个"还没解决"。改成**按需增长的行读入**：
 * 读满一段没遇到换行，就把缓冲翻倍接着读。于是无论行长多少都不会丢，
 * 而普通情况下（一行几百字节）占用的内存和从前一样。
 * ----------------------------------------------------------------------- */

/* 行长上限。理论最坏 6×1024，留到 8KB 足够，且是个硬闸 ——
 * 万一文件被别的东西写坏了（比如一整行没有换行符），
 * 也不能让它把内存吃光。到这个上限就放弃那一行并跳到下一个换行。 */
#define CHATLOG_LINE_MAX 8192

static char     *g_line;          /* 按需 realloc，初值 NULL */
static size_t    g_line_cap;
static char      g_text[CHATLOG_TEXT_MAX];
static bool      g_line_mine;
static bool      g_text_oversize;  /* 本条 text 超 CHATLOG_TEXT_MAX，被截过 */
static ChatlogFn g_fn;
static void     *g_user;
static int       g_read;
static int       g_oversize;       /* 本次读盘里被截短了几条 */

/* 读一整行（含 '\n'），自动扩容。返回行长，EOF 且无内容时返回 -1。
 * 与 fgets 的区别：**绝不因为行太长而截断成两行**。 */
static int read_line(FILE *f)
{
    size_t len = 0;

    if (!g_line) {
        g_line_cap = 512;
        g_line = malloc(g_line_cap);
        if (!g_line)
            return -1;
    }

    for (;;) {
        if (len + 1 >= g_line_cap) {
            size_t ncap;
            char  *nl;

            if (g_line_cap >= CHATLOG_LINE_MAX)
                break;                    /* 到硬闸：放弃这行剩下的部分 */

            ncap = g_line_cap * 2;
            if (ncap > CHATLOG_LINE_MAX)
                ncap = CHATLOG_LINE_MAX;

            nl = realloc(g_line, ncap);
            if (!nl)
                break;                    /* 内存不够：用已有的部分，别丢整行 */
            g_line     = nl;
            g_line_cap = ncap;
        }

        if (!fgets(g_line + len, (int)(g_line_cap - len), f)) {
            if (len == 0)
                return -1;                /* 干净 EOF */
            break;                        /* 最后一行没有换行符 */
        }

        len += strlen(g_line + len);

        /* 读到的片段以 '\n' 结尾 → 这一行完整了 */
        if (len > 0 && g_line[len - 1] == '\n')
            break;
        /* 否则是缓冲读满了，扩容后接着读同一行的后半段 */
    }

    return (int)len;
}

static bool on_walk(const MiniJsonValue *v, void *user)
{
    (void)user;

    if (v->type == MJ_NUMBER && !strcmp(v->path, "mine")) {
        g_line_mine = (v->num != 0.0);
        return true;
    }
    if (v->type == MJ_STRING && !strcmp(v->path, "text")) {
        size_t n = strlen(v->str);

        /* 超长要**说出来**，不能悄悄按 1KB 截掉。
         *
         * 这一条本来是不会有的事：写盘时 chatlog_item 收的就是界面给的
         * 1KB 上限内的文本。会超，只有两种可能 —— 文件是外面手改的，
         * 或者是**别的版本**的程序写的（上限以后可能会放宽）。
         * 两种情况下用户都不该看到一条被腰斩的对话还以为是原文。 */
        if (n >= sizeof g_text) {
            g_text_oversize = true;
            n = sizeof g_text - 1;
        }

        memcpy(g_text, v->str, n);
        g_text[n] = '\0';
        return true;
    }
    return true;
}
int chatlog_load(ChatlogFn fn, void *user)
{
    FILE *f;

    if (!fn)
        return 0;

    f = fopen(CHATLOG_JSON, "rb");
    if (!f)
        return 0;

    g_fn       = fn;
    g_user     = user;
    g_read     = 0;
    g_oversize = 0;

    while (read_line(f) >= 0) {
        bool got_text = false;

        if (g_line[0] != '{')
            continue;                    /* 空行、注释、被截断的尾巴都跳过 */

        g_line_mine     = false;
        g_text[0]       = '\0';
        g_text_oversize = false;

        minijson_walk(g_line, strlen(g_line), on_walk, NULL);
        got_text = (g_text[0] != '\0');

        if (got_text) {
            fn(g_line_mine, g_text, user);
            ++g_read;
            if (g_text_oversize)
                ++g_oversize;
        }
    }

    fclose(f);
    return g_read;
}

int chatlog_oversize_count(void)
{
    return g_oversize;
}

/* 缓冲是**一次分配、长期复用**的：读盘发生在进对话屏时，
 * 而那块内存此后一直闲着。留着比每次 free 再 malloc 省事，
 * 也避免"关了屏又开、反复申请"的碎片。进程退出时由系统回收。 */
void chatlog_reader_free(void)
{
    free(g_line);
    g_line     = NULL;
    g_line_cap = 0;
}
