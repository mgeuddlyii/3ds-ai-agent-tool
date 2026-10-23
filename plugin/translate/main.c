/*
 * main.c —— 「AI Console 翻译插件」的**骨架版**（先证明链路，再接翻译）
 *
 * ── 这一版做到哪 ──
 * 让插件能被 Luma 加载、在下屏画出一块面板、按键能开关。
 * **翻译逻辑还没接**（见 README 的「下一步」）。
 *
 * ── 为什么先做骨架，而不是直接写翻译 ──
 * 插件有四个彼此独立、都可能单独失败的前置条件：
 *   ① 能加载（.3gx 格式 / 链接地址 / 入口）
 *   ② 能画屏（framebuffer / gfx 初始化）
 *   ③ 能读按键（hid）
 *   ④ 能联网（socInit + TLS）
 * 一次性全写进去，失败时**分不清是哪一条挂了** —— 而插件崩溃会连带游戏崩，
 * 现场全黑，没有任何日志。
 * 所以先让 ①②③ 成立、④单独验，每步都能独立判断。
 *
 * ── 三条来自社区的硬约束（无官方文档，但踩过的人一致）──
 * 1. **必须自己定义 `__service_ptr`**，否则 libctru 里 `srvGetServiceHandle`
 *    链接不过（它是个由应用定义的全局）。
 * 2. **别调 `hidInit()`** 在 New 3DS 上会拉入 irrst 把游戏冻住；
 *    直接 `hidScanInput` 读共享内存的按键状态即可。
 * 3. **BSS 由 boot.s 自己清**（加载器不清），见 boot.s 注释。
 *
 * ⚠️ 本文件**未在真机验证**。插件的一切都只能在真机上确认：
 *    Azahar 不加载 .3gx，模拟器里什么都看不到。
 */

#include <3ds.h>
#include <stdio.h>
#include <string.h>

/* ★ libctru 需要应用侧提供这几个符号（见文件头第 1 条）。
 * 平时它们是 `3dsx_crt0.o` 定义的；插件拿掉了 crt0（要自己当入口），
 * 就得自己补，否则 apt.o 里会报 undefined reference。 */
void *__service_ptr = NULL;
u32   __system_runflags = 0;
u32   __apt_appid = 0;

/* 面板是否显示。用 SELECT 切 —— 和 Rosalina 的菜单键（L+Down+SELECT）不冲突。 */
static int g_show = 1;
static int g_frames;

static void draw_panel(void)
{
    /* consoleInit 会自己拿 gfx 的 framebuffer 并接管下屏的文本输出。
     * 用"控制台"而不是自绘，是为了让骨架版**不依赖任何字体资源** ——
     * 中文点阵是下一步的事，先把"能画出来"这条单独验掉。 */
    printf("\x1b[2J");                       /* 清屏 */
    printf("\x1b[1;1H");
    printf("AI Console Translate Plugin\n");
    printf("---------------------------\n");
    printf("status : skeleton, no network yet\n");
    printf("frames : %d\n", g_frames);
    printf("\n");
    printf("SELECT : toggle this panel\n");
    printf("\n");
    printf("This build only proves the plugin\n");
    printf("loads and draws. Translation is the\n");
    printf("next step (see README).\n");
}

int main(void)
{
    /* gfxInitDefault 在插件里能不能用，取决于游戏是否已经 init 过 gsp。
     * 大多数游戏都 init 了，libctru 会复用；个别游戏可能失败。
     * ⚠️ 未真机验证 —— 若面板不出现，先怀疑这一条。 */
    gfxInitDefault();
    consoleInit(GFX_BOTTOM, NULL);

    draw_panel();

    for (;;) {
        hidScanInput();

        u32 down = hidKeysDown();
        if (down & KEY_SELECT) {
            g_show = !g_show;
            if (g_show)
                draw_panel();
            else
                printf("\x1b[2J");           /* 收起面板：清掉 */
        }

        ++g_frames;
        if (g_show && (g_frames % 60) == 0)
            draw_panel();                    /* 让帧计数动起来，证明没死 */

        gfxFlushBuffers();
        gfxSwapBuffers();
        gspWaitForVBlank();
    }
}
