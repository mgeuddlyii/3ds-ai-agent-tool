/* 临时占位：把网络层摘掉，只留界面，用来判断"体积是不是加载失败的原因" */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "net.h"

bool net_translate(const uint8_t *bgr, int w, int h, char *out, size_t cap)
{
    (void)bgr; (void)w; (void)h;
    if (out && cap > 12) { const char *s = "net removed"; for (int i=0;i<12;i++) out[i]=s[i]; out[12]=0; }
    return false;
}
bool net_init(void) { return false; }
bool net_ping(char *out, size_t cap)
{
    if (out && cap > 12) { const char *s = "net removed"; for (int i=0;i<12;i++) out[i]=s[i]; out[12]=0; }
    return false;
}
