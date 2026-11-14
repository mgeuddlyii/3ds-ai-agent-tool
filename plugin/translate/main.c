/*
 * main.c —— AIConsole 翻译插件
 *
 * 界面：面板顶上三个按钮「翻译 / 设置 / 说明」，都能点（触摸）。
 *       短按触发键 = 翻译，长按 ≈0.75 秒 = 设置。
 *
 * ── 为什么全部自绘 ──
 *   libctru 的 PrintConsole 里 windowX/Y/Width/Height 标着 **not implemented**，
 *   只能整屏；而"面板能随便摆、按钮能点"正是这个插件要的东西。
 *
 * ── 三条硬约束（骨架版就记着，别改）──
 *   1. 自己定义 __service_ptr / __system_runflags / __apt_appid（crt0 被拿掉）
 *   2. 别调 hidInit()，New 3DS 上会拉入 irrst 把游戏冻住
 *   3. BSS 由 boot.s 自己清
 *
 * ⚠️ 未在真机验证：Azahar 不加载 .3gx，模拟器里什么都看不到。
 */

#include <3ds.h>
#include <reent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cfg.h"
#include "help.h"
#include "hud.h"
#include "lcd.h"     /* HID_PAD：按键走寄存器，不调 hidInit */
#include "net.h"

void *__service_ptr = NULL;
u32   __system_runflags = 0;
u32   __apt_appid = 0;

/* --------------------------------------------------------------------------
 * ★★ 插件必须自己做的三件"crt0 的活"（2026-11-13 真机崩溃后补）
 *
 * 现场：一进游戏就崩，PC=svcBreak、LR=__syscall_getreent。
 * 反汇编 libctru 的 __syscall_getreent 才看清它要什么：
 *
 *     mrc p15,0,r3,cr13,cr0,3     @ 读线程指针 TP
 *     ldr r1,[r3]                 @ 读 TP[0]
 *     cmp r1,#0x21545624          @ 必须等于这个魔数
 *     bne -> svcBreak             @ 不等就崩（我们就是死在这）
 *     ldr r0,[r3,#8]              @ 返回 TP[8] 当 struct _reent*
 *
 * 也就是说：**TP 必须指向一块我们自己的内存，头 4 字节是魔数，
 * 第 8 字节处放 reent 指针**。平时这些是 crt0 + initThreadVars() 干的，
 * 而 3gx 插件拿掉了 crt0 —— 于是第一次调 fopen/snprintf 就崩，
 * 且崩在"看起来跟这些毫无关系"的地方。
 *
 * srvInit/fsInit 同理：没有它们，服务句柄拿不到，fopen 与 gfx 都会失败。
 * 顺序不能错：先 TLS（不然连 printf 都活不过），再服务，最后 gfx。
 * ----------------------------------------------------------------------- */
static struct _reent g_reent_store;
static uint32_t      g_tls_block[16];     /* 32 字节头 + 余量，4 字节对齐 */

/* 启动痕迹：覆盖写一行到卡上，**全程不碰 newlib**。
 *
 * 为什么不用 fopen/fprintf：这正是我们要排查的那条路 ——
 * TLS/reent 没弄好时，第一个 fopen 就会 svcBreak，于是"崩溃前想留个痕迹"
 * 这件事本身也崩了，什么都留不下（前两次真机就是这样）。
 * 所以这里直接用 libctru 的 FSUSER/FSFILE，绕开 newlib。
 * 覆盖写而不是追加：文件里剩下的那一行就是最后到达的步骤。 */
static void mark(const char *what)
{
    Handle    fh;
    FS_Path   p  = fsMakePath(PATH_ASCII, "/3ds/aiconsole/plugin_boot.txt");
    u32       nw = 0;

    if (R_FAILED(FSUSER_OpenFileDirectly(&fh, ARCHIVE_SDMC,
                                         fsMakePath(PATH_EMPTY, ""), p,
                                         FS_OPEN_WRITE | FS_OPEN_CREATE, 0)))
        return;
    FSFILE_Write(fh, &nw, 0, what, (u32)strlen(what), FS_WRITE_FLUSH);
    FSFILE_Close(fh);
}

static void thread_tls(void)
{
    uint32_t *tp = NULL;

    /* ★★ 2026-11-13 第二次真机崩溃修正：
     *   原来直接 `mcr p15,0,tp,c13,c0,3` 想把 TP 指到自己的块 —— **不管用**。
     *   真机转储里 R3(TP)=0x1FFB1000，根本不是我们的缓冲：ARM11 上
     *   TPIDRURO 对用户态是**只读**的，写不进去。
     *
     *   所以改成：**先读出当前 TP，把魔数和 reent 指针填进它指向的内存**。
     *   那块内存是加载器/游戏给这个线程的 TLS 区，本来就是该放这些的地方。
     *   只有读出来是 0 或明显非法时，才退回到自己写 TP 的老路。 */
    __asm__ volatile ("mrc p15, 0, %0, c13, c0, 3" : "=r"(tp));

    if (!tp || (uintptr_t)tp < 0x00100000u) {
        tp = g_tls_block;
        __asm__ volatile ("mcr p15, 0, %0, c13, c0, 3" :: "r"(tp) : "memory");
    }

    tp[0] = 0x21545624u;                  /* libctru 的 TP 魔数 */
    tp[1] = 0u;
    tp[2] = (uint32_t)&g_reent_store;     /* TP[8] = struct _reent* */
    tp[3] = 0xFFFFFFF8u;                  /* 与 initThreadVars 里写的值一致 */
    ((unsigned char *)tp)[24] = 0;

    tp[4] = 0u;                           /* fs_magic=0 → 走全局 fs 会话 */

    _REENT_INIT_PTR(&g_reent_store);      /* 让 newlib 的 stdio/errno 可用 */

    /* ★ 服务初始化不在这里做：srvInit()/fsInit() 已在 main() 里调过，
     *   它们是进程级的全局状态，工作线程直接用就行。 */
}

#define MODE_GAME     0
#define MODE_SETTINGS 1
#define MODE_HELP     2
#define MODE_LAYOUT   3

/* 长按阈值：毫秒。别用帧数 —— 主循环是 4ms 一轮，帧率不等于 60fps。 */
#define TRIGGER_LONG_MS 750

static PluginCfg g_cfg;
static int       g_mode = MODE_GAME;
static int       g_sel;
static char      g_text[512] = "点「翻译」，或短按触发键";
static int       g_boot_marked;
static bool      g_panel_on;     /* 面板是否显示：平时**完全不画**，按快捷键才出来 */
static int       g_trig_prev;    /* 上一步是不是按着组合键（做按下沿检测） */

/* 一次触摸只消费一次：按住不放不该反复触发 */
static bool      g_touching, g_tap_used;

static void do_translate(void);
static void do_test(void);
static const char *trigger_name(void);   /* 定义在下面，设置页要先声明 */
static int         trig_idx(void);

/* ---------------- 设置页 ---------------- */

#define SET_ROWS 8

static const char *row_label(int i)
{
    switch (i) {
    case 0: return "显示在哪块屏";
    case 1: return "翻译屏位置";
    case 2: return "宽度（字符）";
    case 3: return "高度（字符）";
    case 4: return "背板深浅";
    case 5: return "触发键";
    case 6: return "暂停游戏(Beta)";
    default: return "摆放位置";
    }
}

static void row_value(int i, char *out, size_t n)
{
    switch (i) {
    case 0: snprintf(out, n, "%s", g_cfg.screen == SCR_TOP ? "上屏" : "下屏"); break;
    case 1: {
        static const char *k[] = { "左上", "上中", "右上", "左中", "中央",
                                   "右中", "左下", "下中", "右下" };
        snprintf(out, n, "%s", k[(int)g_cfg.anchor]);
        break;
    }
    case 2: snprintf(out, n, "%d", g_cfg.cols); break;
    case 3: snprintf(out, n, "%d", g_cfg.rows); break;
    case 4: {
        static const char *k[] = { "最浅", "浅", "中", "最深" };
        snprintf(out, n, "%s", k[g_cfg.opacity & 3]);
        break;
    }
    case 5: snprintf(out, n, "%s", trigger_name()); break;
    case 6:
        /* 明确把风险写在值里，不藏在说明页 */
        snprintf(out, n, "%s", g_cfg.beta_pause ? "开(可能卡死)" : "关");
        break;
    default:
        snprintf(out, n, "%s", g_cfg.free_pos ? "已拖动" : "按 A 拖动");
        break;
    }
}

static void row_step(int i, int dir)
{
    switch (i) {
    case 0: g_cfg.screen = g_cfg.screen == SCR_TOP ? SCR_BOTTOM : SCR_TOP; break;
    case 1: {
        int a = (int)g_cfg.anchor + dir;

        if (a < 0) a = 8;
        if (a > 8) a = 0;
        g_cfg.anchor = (CfgAnchor)a;
        break;
    }
    case 2:
        g_cfg.cols += dir * 4;
        if (g_cfg.cols < 10) g_cfg.cols = 10;
        if (g_cfg.cols > 50) g_cfg.cols = 50;
        break;
    case 3:
        g_cfg.rows += dir;
        if (g_cfg.rows < 2) g_cfg.rows = 2;
        if (g_cfg.rows > 16) g_cfg.rows = 16;
        break;
    case 4: g_cfg.opacity = (g_cfg.opacity + dir + 4) & 3; break;
    case 5:
        g_cfg.trigger = trig_idx() + (dir > 0 ? 1 : CFG_TRIGGER_MAX - 1);
        if (g_cfg.trigger >= CFG_TRIGGER_MAX) g_cfg.trigger = 0;
        if (g_cfg.trigger < 0) g_cfg.trigger = CFG_TRIGGER_MAX - 1;
        break;
    case 6: g_cfg.beta_pause = !g_cfg.beta_pause; break;
    default: break;                 /* 摆放行不响应左右键 */

    }
}

/* 行矩形：绘制与触摸共用一份，两处各算一遍必定错开 */
static void row_rect(int i, int w, int *rx, int *ry, int *rw, int *rh)
{
    *rx = 6;
    *ry = 42 + i * 26 - 5;
    *rw = w - 12;
    *rh = 24;
}

static void draw_settings(void)
{
    int w = 0, h = 0;

    if (!hud_begin(GFX_BOTTOM, &w, &h))
        return;

    hud_rect(0, 0, w, h, 10, 12, 16);
    hud_text(10, 8, w - 10, "翻译插件 · 设置", 120, 220, 210);
    hud_rect(10, 30, w - 20, 1, 40, 70, 80);

    for (int i = 0; i < SET_ROWS; ++i) {
        int  rx, ry, rw, rh;
        char val[32];

        row_value(i, val, sizeof val);
        row_rect(i, w, &rx, &ry, &rw, &rh);

        if (i == g_sel)
            hud_rect(rx, ry, rw, rh, 26, 62, 72);

        hud_text(14, ry + 5, w - 130, row_label(i),
                 i == g_sel ? 240 : 190,
                 i == g_sel ? 245 : 200,
                 i == g_sel ? 250 : 210);
        {
            int vw = hud_text_w(val);

            hud_text(w - 14 - vw, ry + 5, w - 6, val, 150, 220, 190);
        }
    }

    hud_rect(10, h - 44, w - 20, 1, 40, 70, 80);
    hud_text(10, h - 40, w - 10, "点一行选中，再点一次改值", 120, 150, 160);
    hud_text(10, h - 20, w - 10, "A 保存    B 返回", 120, 150, 160);
    hud_end();
}

/* ---------------- 触摸 ---------------- */

/* ⚠️ 触摸暂时停用（2026-11-14）。
 *   读触摸屏要 `hid:USER` 的共享内存（HIDUSER_GetIPCHandles + svcMapMemoryBlock），
 *   而 `hidInit()` 在 New 3DS 上会拉起 irrst 把游戏冻住 —— 这两件事不能简单解决。
 *   当前版本：面板三个按钮用 ←/→ 选择、A 确认；全局触发键仍可用。
 *   触摸的恢复方案写在 README 的待办里（自建最小 hid:USER 会话，只映射共享内存）。 */
static bool touch_pressed(touchPosition *tp)
{
    if (tp) {
        tp->px = 0;
        tp->py = 0;
    }
    g_touching = false;
    g_tap_used = false;
    return false;
}

/* ---- 唤出面板的快捷键 ------------------------------------------------
 * 只能从 HID 寄存器 0x10146000 的 12 个键里组合（ZL/ZR 拿不到，见 NOTES-3gx.md）。
 * 设计原则：
 *   ① 三键组合、必须"同时按下"才认，跟游戏抢键的概率最低；
 *   ② **绝不用 L + ↓ + SELECT** —— 那是 Rosalina 菜单的组合，会被系统先截走；
 *   ③ 这张表只能往后追加 —— 配置里存的是下标，插在中间会让老配置错位。
 */
static const struct { u32 mask; const char *name; } k_trig[CFG_TRIGGER_MAX] = {
    { KEY_L | KEY_R | KEY_SELECT, "L+R+SELECT" },   /* 0 ← 默认 */
    { KEY_L | KEY_R | KEY_START,  "L+R+START"  },
    { KEY_L | KEY_R | KEY_X,      "L+R+X"      },
    { KEY_L | KEY_R | KEY_Y,      "L+R+Y"      },
    { KEY_L | KEY_R | KEY_A,      "L+R+A"      },
    { KEY_L | KEY_R | KEY_B,      "L+R+B"      },
    { KEY_L | KEY_R | KEY_DUP,    "L+R+上"     },
    { KEY_SELECT,                 "SELECT"     },   /* 7 单键：最省事也最容易被游戏抢 */
};

static int trig_idx(void)
{
    int i = g_cfg.trigger;

    if (i < 0 || i >= CFG_TRIGGER_MAX)
        i = 0;
    return i;
}

static u32 trigger_mask(void)
{
    return k_trig[trig_idx()].mask;
}

static const char *trigger_name(void)
{
    return k_trig[trig_idx()].name;
}

/* ---- Beta：暂停游戏 ----------------------------------------------------
 * `svcControlProcess(SCHEDULE_THREADS)` 会让**谓词返回 true 的线程被挂起**。
 * 游戏线程一停就不重画帧缓冲，我们的面板自然不会被擦掉，也就不闪了。
 *
 * ★ 谓词里那两条判断都是保命的：
 *   ① `0xFFFF9000` 是当前内核线程指针 —— 调用者自己绝不能暂停，
 *      否则当场自锁（连恢复的代码都跑不到）。
 *   ② 带我们魔数 `0x21545624` 的线程（UI 线程）要放过，
 *      否则按完快捷键就再也反应不过来了。
 *
 * ⚠️ `KThread->tls` 的偏移 `0x94` 来自 CTRComposer，**未在本机内核复核**。
 *    万一偏移不对，最坏结果是"该暂停的没暂停"或"游戏卡死只能长按电源"，
 *    所以这个功能默认关闭，界面上标成 Beta。
 */
#define CUR_PROCESS_HANDLE_ 0xFFFF8001u

/* libctru **没有导出** `svcControlProcess`（头文件里只有 `svcControlProcessMemory`
 * 并在注释里 `@sa svcControlProcess`）。内核里它是 SVC 编号 0x3C，
 * 紧邻已导出的 0x3D，所以这里自己发一条。参数按 AAPCS 放 r0-r3。 */
static inline Result svc_control_process(Handle process, u32 op, u32 arg, void *addr)
{
    register u32 r0 __asm__("r0") = (u32)process;
    register u32 r1 __asm__("r1") = op;
    register u32 r2 __asm__("r2") = arg;
    register u32 r3 __asm__("r3") = (u32)(uintptr_t)addr;

    __asm__ volatile ("svc 0x3C"
                      : "+r"(r0)
                      : "r"(r1), "r"(r2), "r"(r3)
                      : "memory");
    return (Result)r0;
}

#define PROCESSOP_SCHEDULE_THREADS_ 5u

static bool pause_pred(void *th)
{
    u32 tls_va;

    if (*(void **)0xFFFF9000u == th)
        return false;                       /* 别暂停调用者自己 */

    tls_va = *(u32 *)((u8 *)th + 0x94);     /* KThread->tls */
    if (tls_va && *(u32 *)tls_va == 0x21545624u)
        return false;                       /* 别暂停我们自己的线程 */

    return true;                            /* 其余（游戏线程）挂起 */
}

static int g_paused;

static void game_pause(int on)
{
    Result rc;

    if (on == g_paused)
        return;

    rc = svc_control_process(CUR_PROCESS_HANDLE_, PROCESSOP_SCHEDULE_THREADS_,
                             on ? 1u : 0u, on ? (void *)pause_pred : NULL);
    if (R_FAILED(rc)) {
        /* 失败就本会话内关掉它，别每 4ms 重试一遍 */
        mark("5 beta pause FAILED, disabled");
        g_cfg.beta_pause = false;
        g_paused = 0;
        return;
    }
    g_paused = on;
}


/* ---------------- 拖动摆放页 ----------------
 *
 * 触摸只在下屏有效，而目标屏可能是上屏 —— 所以这里画的是**缩略画布**：
 * 下屏那块区域按比例代表目标屏，拖的是画布里的窗口。
 * 直接"在上屏拖"是做不到的（上屏没有触摸层），这条限制绕不过去。
 */

#define LAY_TITLE_H 30
#define LAY_BTN_H   40

static int   g_drag;                 /* 正在拖 */
static int   g_drag_dx, g_drag_dy;   /* 触摸点相对窗口左上角的偏移（屏像素） */
static int   g_lay_x, g_lay_y;       /* 布局页里的自由坐标（目标屏像素） */

static void lay_canvas(int *cx, int *cy, int *cw, int *ch, float *scale)
{
    int scr_w = g_cfg.screen == SCR_TOP ? 400 : 320;
    int scr_h = 240;
    int avail_w = 320;
    int avail_h = 240 - LAY_TITLE_H - LAY_BTN_H;

    *scale = (float)avail_w / scr_w;
    if ((float)avail_h / scr_h < *scale)
        *scale = (float)avail_h / scr_h;

    *cw = (int)(scr_w * *scale);
    *ch = (int)(scr_h * *scale);
    *cx = (320 - *cw) / 2;
    *cy = LAY_TITLE_H + (avail_h - *ch) / 2;
}

static void draw_layout(void)
{
    int   cx, cy, cw, ch;
    float sc;
    int   px, py, pw, ph;

    if (!hud_begin(GFX_BOTTOM, NULL, NULL))
        return;

    hud_rect(0, 0, 320, 240, 10, 12, 16);
    hud_text(10, 6, 310, g_cfg.screen == SCR_TOP ? "拖动摆放 · 上屏" : "拖动摆放 · 下屏",
             120, 220, 210);

    lay_canvas(&cx, &cy, &cw, &ch, &sc);

    /* 画布 = 目标屏的缩影 */
    hud_rect(cx, cy, cw, ch, 22, 26, 34);
    hud_frame(cx, cy, cw, ch, 60, 90, 100);

    {
        int pw0 = g_cfg.cols * 8, ph0 = g_cfg.rows * 16;

        if (pw0 > (g_cfg.screen == SCR_TOP ? 400 : 320)) pw0 = (g_cfg.screen == SCR_TOP ? 400 : 320);
        if (ph0 > 240) ph0 = 240;

        pw = (int)(pw0 * sc);
        ph = (int)(ph0 * sc);
        px = cx + (int)(g_lay_x * sc);
        py = cy + (int)(g_lay_y * sc);

        hud_rect(px, py, pw, ph, 40, 120, 130);
        hud_frame(px, py, pw, ph, 120, 220, 210);
        if (pw > 40 && ph > 20)
            hud_text(px + 4, py + 4, px + pw - 4, "拖我", 235, 245, 250);
    }

    /* 底部三个按钮：换屏 / 确认 / 取消 */
    {
        static const char *kLbl[3] = { "换屏", "确认", "取消" };
        int bw = (320 - 8 * 2 - 6 * 2) / 3;

        for (int i = 0; i < 3; ++i) {
            int bx = 8 + i * (bw + 6);
            int by = 240 - LAY_BTN_H + 8;

            hud_rect(bx, by, bw, LAY_BTN_H - 16, 34, 42, 52);
            hud_frame(bx, by, bw, LAY_BTN_H - 16, 70, 120, 130);
            {
                int tw = hud_text_w(kLbl[i]);

                hud_text(bx + (bw - tw) / 2, by + 4, bx + bw, kLbl[i], 220, 235, 240);
            }
        }
    }

    hud_end();
}

/* 布局页的触摸：返回 0=没动作 1=确认 2=取消 3=换屏 */
static int layout_touch(int pressed, int tx, int ty)
{
    int   cx, cy, cw, ch;
    float sc;
    int   pw = (int)(g_cfg.cols * 8 * 0.0f);   /* 占位，下面重算 */

    lay_canvas(&cx, &cy, &cw, &ch, &sc);

    /* 底部按钮 */
    {
        int bw = (320 - 8 * 2 - 6 * 2) / 3;
        int by = 240 - LAY_BTN_H + 8;

        if (ty >= by && ty < by + LAY_BTN_H - 16) {
            for (int i = 0; i < 3; ++i) {
                int bx = 8 + i * (bw + 6);

                if (tx >= bx && tx < bx + bw)
                    return i == 0 ? 3 : (i == 1 ? 1 : 2);
            }
        }
    }

    {
        int pw0 = g_cfg.cols * 8, ph0 = g_cfg.rows * 16;
        int maxw = g_cfg.screen == SCR_TOP ? 400 : 320;

        if (pw0 > maxw) pw0 = maxw;
        if (ph0 > 240) ph0 = 240;
        pw = (int)(pw0 * sc);

        {
            int ph  = (int)(ph0 * sc);
            int px  = cx + (int)(g_lay_x * sc);
            int py  = cy + (int)(g_lay_y * sc);

            if (pressed) {
                if (tx >= px && tx < px + pw && ty >= py && ty < py + ph) {
                    g_drag    = 1;
                    g_drag_dx = (int)((tx - px) / sc);
                    g_drag_dy = (int)((ty - py) / sc);
                }
            } else if (g_drag) {
                /* 拖动中：把画布坐标换算回目标屏像素 */
                int nx = (int)((tx - cx) / sc) - g_drag_dx;
                int ny = (int)((ty - cy) / sc) - g_drag_dy;
                int maxx = maxw - pw0;
                int maxy = 240 - ph0;

                if (nx < 0) nx = 0;
                if (ny < 0) ny = 0;
                if (nx > maxx) nx = maxx;
                if (ny > maxy) ny = maxy;
                g_lay_x = nx;
                g_lay_y = ny;
            }
        }
    }
    return 0;
}

/* ---------------- 主循环 ---------------- */

/* ---------------- 线程与输入 ---------------- */

/* ★ 2026-11-14：插件里**不能**调 hidInit()/hidScanInput()
 *   （N3DS 上会拉 irrst 把游戏冻住，而且插件也拿不到 hid 的共享内存）。
 *   按键直接读 HID 寄存器：0x10146000 低 12 位是**取反**的按下状态，
 *   lcd.h 里的 HID_PAD 已经 ^0xFFF 并且走了物理镜像。
 *   边沿（"刚按下"）自己算：本帧按下 & ~上帧按下。 */
static u32 g_pad_prev;

static u32 pad_now(void)
{
    return HID_PAD;
}

static u32 pad_down(void)
{
    u32 now = pad_now();
    u32 d   = now & ~g_pad_prev;

    g_pad_prev = now;
    return d;
}

static u32     g_ui_stack[8192];    /* 32KB 工作线程栈 */
static Handle  g_ui_thread;

static void ui_thread(void *arg);

/* main 只做三件事：拿服务、起 UI 线程、**立刻返回**。
 * 返回是硬要求：Luma 的 pluginLoader.s 里 `blx r5` 之后还要
 * `add sp,#4` 并从栈上还原寄存器、再跳去游戏真正的入口 0x00100000。
 * 不返回 = 游戏永远停在开机 3DS logo（这一步真机验过两次）。 */
int main(void)
{
    srvInit();
    fsInit();
    mark("0 entry srv+fs ok");

    if (R_FAILED(svcCreateThread(&g_ui_thread, (ThreadFunc)ui_thread, 0,
                                 g_ui_stack + sizeof(g_ui_stack) / 4,
                                 0x30, -1))) {
        mark("1 thread FAILED");
        return 0;
    }

    mark("1 thread ok, returning");
    return 0;
}

static void ui_thread(void *arg)
{
    u32 down = 0, held = 0;
    touchPosition tp = { 0, 0 };

    (void)arg;

    /* ★ 裸 svcCreateThread 起的线程没有 libctru 的 ThreadVars，
     *   第一次 snprintf/malloc/curl 都会在 __syscall_getreent 里 svcBreak。
     *   所以这里必须先补 TLS，且必须排在任何 newlib 调用之前。 */
    thread_tls();
    mark("2 worker tls ok");

    cfg_load(&g_cfg);
    mark("3 cfg_load ok");

    for (;;) {
        down = pad_down();
        held = pad_now();

        if (g_mode == MODE_HELP) {
            int w = 0, h = 0;

            if (down & KEY_UP)    help_scroll(-1);
            if (down & KEY_DOWN)  help_scroll(+1);
            if (down & KEY_LEFT)  help_page(-1);
            if (down & KEY_RIGHT) help_page(+1);
            if (down & (KEY_B | KEY_SELECT))
                g_mode = MODE_GAME;

            if (touch_pressed(&tp)) {
                /* 点最下面 24px = 返回；其余按左右半屏翻页 —— 翻页比滚动更常用 */
                if (tp.py > 216)
                    g_mode = MODE_GAME;
                else
                    help_page(tp.px < 160 ? -1 : +1);
            }

            if (g_mode == MODE_HELP) {
                if (hud_begin(GFX_BOTTOM, &w, &h))
                    help_draw(w, h);
                hud_end();
            }
        } else if (g_mode == MODE_SETTINGS) {
            int w = 0, h = 0;

            if (down & KEY_UP)   { if (--g_sel < 0) g_sel = SET_ROWS - 1; }
            if (down & KEY_DOWN) { if (++g_sel >= SET_ROWS) g_sel = 0; }
            if (down & KEY_LEFT)   row_step(g_sel, -1);
            if (down & KEY_RIGHT)  row_step(g_sel, +1);

            if (touch_pressed(&tp) && hud_begin(GFX_BOTTOM, &w, &h)) {
                for (int i = 0; i < SET_ROWS; ++i) {
                    int rx, ry, rw, rh;

                    row_rect(i, w, &rx, &ry, &rw, &rh);
                    if (tp.px >= rx && tp.px < rx + rw &&
                        tp.py >= ry && tp.py < ry + rh) {
                        /* 触摸屏没有左右键，所以"再点一次"就是它的加减 */
                        if (g_sel == i)
                            row_step(i, +1);
                        g_sel = i;
                        break;
                    }
                }
            }

            if (down & KEY_A) {
                if (g_sel == SET_ROWS - 1) {
                    /* 进拖动页：把当前位置换算成自由坐标作为起点 */
                    int sx, sy, sw2, sh2;

                    cfg_panel_rect(&g_cfg, g_cfg.screen == SCR_TOP ? 400 : 320, 240,
                                   &sx, &sy, &sw2, &sh2);
                    g_lay_x = sx;
                    g_lay_y = sy;
                    g_drag  = 0;
                    g_mode  = MODE_LAYOUT;
                } else {
                    cfg_save(&g_cfg);
                    g_mode = MODE_GAME;
                }
            }
            if (down & (KEY_B | KEY_SELECT))
                g_mode = MODE_GAME;

            if (g_mode == MODE_SETTINGS)
                draw_settings();
        } else if (g_mode == MODE_LAYOUT) {
            int pressed = (down & KEY_TOUCH) ? 1 : 0;
            int heldt   = (held & KEY_TOUCH) ? 1 : 0;

            if (down & KEY_LEFT)  { if ((g_lay_x -= 4) < 0) g_lay_x = 0; }
            if (down & KEY_RIGHT) { g_lay_x += 4; }
            if (down & KEY_UP)    { if ((g_lay_y -= 4) < 0) g_lay_y = 0; }
            if (down & KEY_DOWN)  { g_lay_y += 4; }

            if (pressed || heldt) {
                /* ⚠️ 触摸暂停期间这个分支永远不可达：KEY_TOUCH = BIT(20)，
                 *   而 HID_PAD 只给我们 0x10146000 的低 12 位。
                 *   这里**绝不能**再调 hidTouchRead()（它依赖 hidInit 的共享内存，
                 *   而 hidInit 在 N3DS 上会拉 irrst 把游戏冻住）。
                 *   拖动改由 D-pad 完成（见上面四行），确认/取消走下面的 A / B。 */
                touchPosition t2 = { 0, 0 };

                {
                    int act = layout_touch(pressed, t2.px, t2.py);

                    if (act == 1) {              /* 确认 */
                        g_cfg.free_pos = true;
                        g_cfg.pos_x    = g_lay_x;
                        g_cfg.pos_y    = g_lay_y;
                        cfg_save(&g_cfg);
                        g_drag = 0;
                        g_mode = MODE_GAME;
                    } else if (act == 2) {       /* 取消 */
                        g_drag = 0;
                        g_mode = MODE_SETTINGS;
                    } else if (act == 3) {       /* 换屏 */
                        g_cfg.screen = g_cfg.screen == SCR_TOP ? SCR_BOTTOM : SCR_TOP;
                        g_lay_x = g_lay_y = 8;
                        g_drag  = 0;
                    }
                }
            }
            if (down & KEY_A) {                  /* 键盘确认 */
                g_cfg.free_pos = true;
                g_cfg.pos_x    = g_lay_x;
                g_cfg.pos_y    = g_lay_y;
                cfg_save(&g_cfg);
                g_mode = MODE_GAME;
            }
            if (down & (KEY_B | KEY_SELECT))
                g_mode = MODE_SETTINGS;

            if (g_mode == MODE_LAYOUT)
                draw_layout();
        } else {
            /* ---- 游戏态：面板常驻，三个按钮 ---- */
            /* ---- 快捷键：按下沿切换面板 ----
             * 三键组合用"全部按住"判定，再配边沿检测，
             * 这样按住不放只切一次，不会来回跳。 */
            {
                u32 mask  = trigger_mask();
                int combo = ((held & mask) == mask);

                if (combo && !g_trig_prev)
                    g_panel_on = !g_panel_on;
                g_trig_prev = combo;
            }

            /* 面板开着时 B 先用来关面板，不然会跟"从子页返回"打架 */
            if (g_panel_on && (down & KEY_B))
                g_panel_on = false;

            /* ---- Beta：只要我们在画东西（面板或子页），就暂停游戏 ----
             * 游戏线程一停就不再重画帧缓冲，面板不会被擦掉，也就不闪。
             * ★ 恢复那条必须无条件走到：少了它，面板一关游戏还停在原地。 */
            game_pause(g_cfg.beta_pause &&
                       (g_mode != MODE_GAME || g_panel_on) ? 1 : 0);

            /* ★ 键盘操作面板按钮：←/→ 选，A 确认。
             *   触摸停用期间这是**唯一**通路。没有它，"翻译/测试"这几个
             *   只有触摸能到的分支会被 GCC 判成死代码、再被 --gc-sections 裁掉
             *   （实测产物从 1.28MB 掉到 374KB，curl/mbedtls 全没了）。 */
            if (g_panel_on) {
                if (down & KEY_LEFT)  { if (--g_sel < 0) g_sel = BTN_COUNT - 1; }
                if (down & KEY_RIGHT) { if (++g_sel >= BTN_COUNT) g_sel = 0; }

                if (down & KEY_A) {
                    int b = g_sel;

                    if (b == BTN_TRANSLATE)
                        do_translate();
                    else if (b == BTN_SETTINGS) { g_sel = 0; g_mode = MODE_SETTINGS; }
                    else if (b == BTN_HELP)     { help_init(9); g_mode = MODE_HELP; }
                    else if (b == BTN_TEST)     do_test();
                }
            }

            if (touch_pressed(&tp)) {
                int b = hud_button_at(&g_cfg, tp.px, tp.py);

                if (b == BTN_TRANSLATE) {
                    do_translate();
                } else if (b == BTN_SETTINGS) {
                    g_sel = 0;
                    g_mode = MODE_SETTINGS;
                } else if (b == BTN_HELP) {
                    help_init(9);
                    g_mode = MODE_HELP;
                } else if (b == BTN_TEST) {
                    do_test();
                }
            }

            /* 只有面板开着才画。关着的时候**一个像素都不碰**：
             * 既不花 4ms 重贴，也不会跟游戏画面抢帧缓冲。 */
            if (g_mode == MODE_GAME && g_panel_on) {
                hud_set_sel(g_sel);      /* 让用户看见 ←/→ 选中了哪个按钮 */
                if (!g_boot_marked) { hud_draw(&g_cfg, HUD_TEXT, g_text); mark("4 first draw ok"); g_boot_marked = 1; }
                else hud_draw(&g_cfg, HUD_TEXT, g_text);
            }
        }

        /* ★ 4ms 重刷（CTRComposer 实测值）：游戏每帧都会重画自己的帧缓冲，
         *   我们的浮层会被盖掉；20ms 重贴会明显闪，4ms 就基本稳定。
         *   不再有 gfxSwapBuffers/gspWaitForVBlank —— 那些都要 GSP，插件里拿不到。 */
        svcSleepThread(4000000LL);
    }
}

/* ---------------- 翻译 ---------------- */

static void do_translate(void)
{
    gfxScreen_t scr = g_cfg.screen == SCR_TOP ? GFX_TOP : GFX_BOTTOM;
    u8 *px = NULL;
    int w = 0, h = 0;

    snprintf(g_text, sizeof g_text, "截图中…");
    hud_draw(&g_cfg, HUD_CAPTURING, NULL);

    if (!hud_grab(scr, &px, &w, &h)) {
        snprintf(g_text, sizeof g_text, "抓屏失败\n拿不到帧缓冲");
        hud_draw(&g_cfg, HUD_ERROR, g_text);
        return;
    }

    snprintf(g_text, sizeof g_text, "等 AI 回复…");
    hud_draw(&g_cfg, HUD_WAITING, NULL);

    if (net_translate(px, w, h, g_text, sizeof g_text))
        hud_draw(&g_cfg, HUD_TEXT, g_text);
    else
        hud_draw(&g_cfg, HUD_ERROR, g_text);

    free(px);
}

/* 测试连接：只发一句纯文本。真机上第一次跑，先点这个 ——
 * 它能把"网络/TLS/Key/余额"四种失败分开报出来，而翻译按钮只会给一个笼统的错。 */
static void do_test(void)
{
    snprintf(g_text, sizeof g_text, "测试中…");
    hud_draw(&g_cfg, HUD_WAITING, NULL);

    if (net_ping(g_text, sizeof g_text))
        hud_draw(&g_cfg, HUD_TEXT, g_text);
    else
        hud_draw(&g_cfg, HUD_ERROR, g_text);
}
