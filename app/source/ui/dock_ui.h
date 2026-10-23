/*
 * dock_ui.h -- 下屏滑轨（Dock）的可复用组件
 *
 * ── 为什么要抽成组件 ──
 * 滑轨本来是一个独立屏（SCREEN_DOCK，从「更多」进），但用户明确要的是
 * **它在对话页的下屏**：聊天时想换模型、看文件、打中文，都不该先退出对话。
 * 所以把"画面板内容 / 命中判定 / 按键"三件事暴露出来，
 * 由宿主屏（sc_chat）自己画滑轨条并决定当前是哪一格。
 *
 * ── 几何是**约定死的**，宿主与组件共用 ──
 *   面板内容区  y 24 .. 166
 *   滑轨条      y 168 .. 198   （4 格，每格 72 宽）
 *   提示/页脚   y 202 / 214
 * 宿主不要自己另算一套：算两套就会出现"画在一处、点在另一处"。
 *
 * ── 面板编号 ──
 *   0 键盘 / 1 项目 / 2 文件 / 3 浏览
 *   （宿主通常还多一格"聊天"，那是宿主自己的事，不在这个编号里）
 */

#pragma once

#include <3ds.h>
#include <stdbool.h>

#define DOCKUI_PANEL_N   4
#define DOCKUI_RAIL_Y  168.0f
#define DOCKUI_RAIL_H   30.0f
#define DOCKUI_RAIL_GAP  4.0f
#define DOCKUI_RAIL_W   72.0f
#define DOCKUI_BODY_Y0  24.0f
#define DOCKUI_BODY_BOT 166.0f

/* 一次性准备：读项目表 + 扫沙箱目录。重复调用是安全的（幂等）。 */
void dockui_init(void);

/* 面板名（宿主画滑轨条时用）。越界返回 "?"。 */
const char *dockui_name(int panel);

/* 画某一格的面板内容（**不含**滑轨条）。 */
void dockui_render(int panel);

/* 命中判定：返回 true 表示这个点被面板吃掉了。
 * 宿主应当**先判滑轨条**，再调这个。 */
bool dockui_touch(int panel, int x, int y);

/* 面板内的按键（A/X/Y/SELECT/上下）。滑轨条的左右切换由宿主处理。 */
void dockui_keys(int panel, u32 down);

/* ---- 拼音键盘（键盘格里的"拼音"按钮会把它打开）----
 * 打开时它要占满整个下屏，宿主必须**先问 dockui_ime_active()**：
 * 为真时把整屏交给 dockui_render_ime()，别画自己的东西。 */
bool dockui_ime_active(void);
void dockui_render_ime(void);
bool dockui_ime_touch(int x, int y);
void dockui_ime_keys(u32 down);

/* 给宿主屏直接开/关（对话页的「键盘」按钮用它代替系统 swkbd）。
 * 打开前会 ime_reset()，所以上屏目标由宿主自己决定 —— 见 dockui_ime_bind()。 */
void dockui_ime_open(void);
void dockui_ime_close(void);

/* ★ 绑定上屏缓冲：之后选中的候选字会**追加**到这个缓冲末尾。
 *   传 NULL 解绑（回到"写到工作台面板的 g_text"的老行为）。
 *   这是为了让对话页的输入框（g_input）能被拼音键盘直接写。
 *   宿主必须在离开该屏时解绑，否则会写到已经失效的栈内存上。 */
void dockui_ime_bind(char *buf, size_t cap);

/* 供宿主复用：滑轨第 i 格的矩形（与组件内部同一份计算）。 */
void dockui_rail_rect(int i, float *x, float *y, float *w, float *h);
