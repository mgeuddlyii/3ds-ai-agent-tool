/*
 * html.h -- 极小的 HTML 子集解析（不是浏览器）
 *
 * 边界见 设计规划.md §4.9：没有 JS、没有 CSS、没有 DOM。
 * 能做的是把一份静态文档变成"一串带层级的文本行"，
 * 排版交给已有的 text.c 折行 —— 所以这一层完全不关心像素。
 *
 * 支持的标签就这些：h1 h2 h3 p li ul ol br blockquote div
 * 其余一律剥掉；script / style 的**内容**直接丢弃（不然会看到一坨 JS 源码）。
 *
 * 能力上限也要说清楚：它打不开任何 SPA。豆包那种连页面框架都是前端
 * 画出来的站点，在这里就是一张白纸。它的用途是更新说明和内置文档。
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>

/* 一页文档的容量。128 行 x 200 字节 ≈ 25KB，够放一份更新说明；
 * 真不够的话，应该拆文档，而不是把这个数往上调 —— 3DS 内存不宽裕。 */
#define HTML_MAX_LINES 128
#define HTML_TEXT_MAX  200

typedef enum {
    HL_TEXT = 0,
    HL_H1,
    HL_H2,
    HL_H3,
    HL_ITEM,     /* 列表项 */
    HL_QUOTE,
} HtmlLineKind;

typedef struct {
    HtmlLineKind kind;
    int          indent;   /* 缩进级别，列表嵌套用 */
    char         text[HTML_TEXT_MAX];
} HtmlLine;

typedef struct {
    int      count;
    HtmlLine line[HTML_MAX_LINES];
} HtmlDoc;

/* 解析整份文本。返回行数；0 = 空文档；-1 = 参数不对。 */
int html_parse(const char *src, size_t n, HtmlDoc *doc);

/* 读文件再解析。文件打不开返回 false。 */
bool html_parse_file(const char *path, HtmlDoc *doc);
