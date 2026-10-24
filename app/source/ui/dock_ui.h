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

#define DOCKUI_PANEL_N   5       /* 键盘 / 项目 / 文件 / 浏览 / 状态 */
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

/* 面板是否正处在**必须独占 ←/→** 的子状态（目前只有项目页的"挑模型"：
 * 那里 ←/→ 是"换一家的下一把"）。
 *
 * 宿主把 ←/→ 交给"切格"之前必须问一句 —— 否则挑模型挑到一半按一下左右，
 * 整块面板连人带状态一起消失，用户会以为自己点错了什么。
 * 除这个模态之外，面板里没有别的地方用 ←/→。 */
bool dockui_wants_lr(void);

/* 面板是否要用 SELECT。目前只有项目格的 SELECT 有语义（删掉当前任务）。
 *
 * 宿主把 SELECT 当作"换快捷短语"之前必须问这一句 —— 否则在项目格按
 * SELECT，输入框被短语填满而任务没删掉，用户会以为删除功能坏了。 */
bool dockui_wants_select(void);

/* ---- 键盘格与宿主输入框的绑定 ----
 *
 * 键盘格要编辑的**就是宿主那条输入框**，不是它自己的缓冲：
 * 不绑的话"送到对话"没有东西可送（历史上它是个占位实现）。
 * buf 传 NULL 解绑，恢复用内部缓冲（独立工作台用法）。 */
void dockui_bind_text(char *buf, size_t cap);

/* 键盘格按了「发送」/点了「送到对话」→ 返回 true（取走即清）。
 * 宿主拿到 true 就真的去发 —— 发送动作必须由宿主做，它才知道
 * 有没有 Key、怎么画占位、失败了怎么说。 */
bool dockui_take_send(void);

/* ---- 拼音键盘（键盘格里的"拼音"按钮会把它打开）----
 * 打开时它要占满整个下屏，宿主必须**先问 dockui_ime_active()**：
 * 为真时把整屏交给 dockui_render_ime()，别画自己的东西。 */
bool dockui_ime_active(void);
void dockui_render_ime(void);
bool dockui_ime_touch(int x, int y);
void dockui_ime_keys(u32 down);

/* 给宿主屏直接开/关（对话页的「键盘」按钮用它代替系统 swkbd）。
 * 打开前会 ime_reset()，所以上屏目标由宿主自己决定 —— 见 dockui_ime_bind()。 */
/* 对话屏第 0 格「选项」：四项即时控制（思考强度 / 上下文轮数 / 回复长度 / 回答语言）。
 * 改完立刻 config_save()，不走"退出时统一保存"那套。 */
/* 选 API 页（SCREEN_PICKAPI）与项目面板之间的桥。
 * 选页只管"第 k 套"这件事，不碰 projects/keystore 的细节。 */
int  dockui_pick_target(void);          /* 正在为哪个任务挑；<0 = 没有 */
bool dockui_pick_apply(int k);          /* 把第 k 套可用 API 绑给那个任务 */
void dockui_pick_cancel(void);          /* 退出挑选，保持默认 */

void dockui_render_opt(void);
bool dockui_touch_opt(int x, int y);
void dockui_keys_opt(u32 down);

void dockui_ime_open(void);
void dockui_ime_close(void);

/* ★ 绑定上屏缓冲：之后选中的候选字会**追加**到这个缓冲末尾。
 *   传 NULL 解绑（回到"写到工作台面板的 g_text"的老行为）。
 *   这是为了让对话页的输入框（g_input）能被拼音键盘直接写。
 *   宿主必须在离开该屏时解绑，否则会写到已经失效的栈内存上。 */
void dockui_ime_bind(char *buf, size_t cap);

/* 供宿主复用：滑轨第 i 格的矩形（与组件内部同一份计算）。 */
void dockui_rail_rect(int i, float *x, float *y, float *w, float *h);
