/*
 * help.c -- 见 help.h
 */

#include "help.h"
#include "hud.h"

#include <stdio.h>
#include <string.h>

/* 说明正文。一行一句，短句为主 —— 小屏幕上长句会折成两行，
 * 读起来像在啃说明书（本来就是说明书，但也别写成产品手册）。 */
static const char kHelp[] =
    "这是什么\n"
    "游戏运行时按一个键，把当前画面截下来发给 AI，"
    "译文显示在一块可以自己摆位置的翻译屏上。\n"
    "\n"
    "按键\n"
    "短按 SELECT：截屏并翻译\n"
    "长按 SELECT 约一秒：打开设置\n"
    "设置里 ↑↓ 选行，←→ 改值\n"
    "A 保存并退出，B 不保存退出\n"
    "\n"
    "设置项\n"
    "显示在哪块屏：上屏画面大，下屏不挡手\n"
    "翻译屏位置：九宫格，贴着哪个角\n"
    "宽/高：以字符为单位，1 字符 = 8×16 像素\n"
    "背板深浅：3DS 没有半透明，只能调底色\n"
    "触发键：SELECT 或 ZL（ZL 只有新三有）\n"
    "\n"
    "译文放不下怎么办\n"
    "先调高、再调宽。宽到 40 字符就顶满下屏了，"
    "再宽会被屏幕裁掉。\n"
    "\n"
    "现在还没做好的\n"
    "截屏已经能用了，面板上会显示抓到的分辨率。"
    "把画面传给 AI 这一步还没接 —— 所以你现在按下去，"
    "看到的是“抓屏成功”，不是译文。\n"
    "\n"
    "真机出问题先看这里\n"
    "面板不出现：多半是游戏自己管着显示服务，"
    "换个游戏试试，能出来的话就是这个原因。\n"
    "按键没反应：确认 Luma 的 Plugin Loader 开着。\n"
    "截出来的图是黑的：说明抓到的是插件自己的空画面，"
    "这条要靠改抓屏方式解决，不是设置问题。\n"
    "游戏直接崩：插件占的内存太多，"
    "这个要改插件打包时的内存设置。\n"
    "\n"
    "设置文件\n"
    "存在 sdmc:/3ds/aiconsole/plugin.ini，"
    "也能用电脑记事本直接改，下次进游戏生效。\n"
    "\n"
    "版本\n"
    "翻译插件 0.2.0　作者 AI Console\n";

#define MAX_LINES 96
#define MAX_LINE  18          /* = (320-20)/16：一行最多 18 个 16px 汉字 */

static char  g_lines[MAX_LINES][MAX_LINE * 4];
static int   g_count;
static int   g_top;              /* 第一行的序号 */
static int   g_per_page = 9;

/* UTF-8 里取一个字符的字节数（只看首字节，够用）。 */
static int ch_len(unsigned char c)
{
    if (c < 0x80) return 1;
    if ((c & 0xE0) == 0xC0) return 2;
    if ((c & 0xF0) == 0xE0) return 3;
    return 4;
}

/* 按字符数折行。中文没有词边界，直接按宽度断；
 * 但断点落在 ASCII 单词中间时回退到上一个空格 ——
 * 屏幕上出现半个英文单词比多一行难看。 */
static void wrap_into_lines(void)
{
    const char *p = kHelp;

    g_count = 0;
    while (*p && g_count < MAX_LINES) {
        char       *dst    = g_lines[g_count];
        const char *q      = p;
        int         nbytes = 0;
        int         nchars = 0;

        if (*q == '\n') {                 /* 空行 */
            dst[0] = '\0';
            ++g_count;
            p = q + 1;
            continue;
        }

        while (*q && *q != '\n') {
            int l = ch_len((unsigned char)*q);

            if (nchars >= MAX_LINE) {
                int k = nbytes;            /* 往回找空格，别撕开单词 */

                while (k > 0 && dst[k - 1] != ' ')
                    --k;
                if (k > 0 && nbytes - k < 12)
                    nbytes = k;
                break;
            }
            memcpy(dst + nbytes, q, l);
            nbytes += l;
            ++nchars;
            q += l;
        }
        dst[nbytes] = '\0';

        if (*q == ' ')
            ++q;                          /* 断在空格上：吃掉它，别让下一行开头空一格 */
        p = (*q == '\n') ? q + 1 : q;
        ++g_count;
    }
}

void help_init(int lines_per_page)
{
    if (lines_per_page > 0)
        g_per_page = lines_per_page;
    wrap_into_lines();
    g_top = 0;
}

bool help_scroll(int lines)
{
    int t = g_top + lines;

    if (t < 0) t = 0;
    if (t > g_count - 1) t = g_count - 1;
    if (t == g_top)
        return false;
    g_top = t;
    return true;
}

bool help_page(int dir)
{
    return help_scroll(dir * g_per_page);
}

void help_pos(int *page, int *pages)
{
    int p = (g_count + g_per_page - 1) / g_per_page;

    if (p < 1) p = 1;
    if (page)  *page  = g_top / g_per_page + 1;
    if (pages) *pages = p;
}

void help_draw(int w, int h)
{
    int y = 8;

    hud_rect(0, 0, w, h, 10, 12, 16);
    hud_text(10, 6, w - 10, "使用说明", 120, 220, 210);
    hud_rect(10, 28, w - 20, 1, 40, 70, 80);

    y = 34;
    for (int i = 0; i < g_per_page; ++i) {
        int idx = g_top + i;

        if (idx >= g_count)
            break;
        if (y + 16 > h - 24)
            break;

        /* 小标题（行首不是空格、且下一行是空行的）用亮一点的颜色。
         * 这条判断很土，但比给正文加标记语言省事得多。 */
        {
            int is_head = (idx + 1 < g_count && g_lines[idx + 1][0] == '\0'
                           && g_lines[idx][0] != '\0');
            hud_text(10, y, w - 10, g_lines[idx],
                     is_head ? 235 : 195,
                     is_head ? 240 : 205,
                     is_head ? 245 : 215);
        }
        y += 18;
    }

    {
        char foot[96];   /* 47~66 字节，48 会截断（编译器报过） */
        int  page = 0, pages = 0;

        help_pos(&page, &pages);
        snprintf(foot, sizeof foot, "↑↓ 滚动   ←→ 翻页   %d/%d   B 返回",
                 page, pages);
        hud_rect(10, h - 22, w - 20, 1, 40, 70, 80);
        hud_text(10, h - 18, w - 10, foot, 120, 150, 160);
    }
}
