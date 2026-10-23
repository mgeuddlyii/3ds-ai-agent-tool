/*
 * input.c -- 见 input.h
 */

#include "input.h"

#include <3ds.h>

#include <stdio.h>
#include <string.h>
#include "uistr.h"

/* swkbd 自己往这块缓冲里写，所以它必须是静态的：
 * 3DS 上栈只有很小一截，256 字节的局部数组在深调用里是有风险的；
 * 而这个函数是**模态**的（调完才返回），不存在重入。 */
static char g_buf[256];

static char *trim(char *s)
{
    char *p = s;
    size_t n;

    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')
        ++p;
    n = strlen(p);
    while (n > 0) {
        char c = p[n - 1];
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n')
            p[--n] = '\0';
        else
            break;
    }
    return p;
}

bool ui_input_text(const char *hint, const char *initial, int max_chars,
                   char *out, size_t cap)
{
    SwkbdState swkbd;
    SwkbdButton btn;

    if (!out || cap == 0)
        return false;

    if (max_chars < 1)
        max_chars = 1;
    /* ★ 按 3 字节/字符收紧：swkbd 的长度参数是**字符**，
     *   而这台机器自带中文输入法 —— 如果拿 cap（字节）当字符上限，
     *   输 90 个汉字就是 270 字节，直接写穿缓冲。
     *   宁可少给几个字符的位置，也不能越界。 */
    if ((size_t)max_chars * 3 + 1 > sizeof g_buf)
        max_chars = (int)((sizeof g_buf - 1) / 3);

    swkbdInit(&swkbd, SWKBD_TYPE_WESTERN, 2, max_chars);
    swkbdSetHintText(&swkbd, (hint && hint[0]) ? hint : S(S0040));
    if (initial && initial[0])
        swkbdSetInitialText(&swkbd, initial);

    g_buf[0] = '\0';
    btn = swkbdInputText(&swkbd, g_buf, sizeof g_buf);

    if (btn != SWKBD_BUTTON_CONFIRM)
        return false;

    snprintf(out, cap, "%s", trim(g_buf));
    return true;
}
