/*
 * app.h -- 全局状态 + 屏幕栈 + 事件分发
 *
 * M2 落的是**骨架**：栈只有 4 层、没有转场动画、没有异步消息。
 * 之所以现在就抽出来，是因为首启引导本身就需要"三页走完切到主界面"，
 * 顺手把 M3 要用的形状定下来比后面返工便宜。
 */

#pragma once

#include <3ds.h>
#include <stdbool.h>

#include "config.h"
#include "render.h"
#include "screen.h"

#define APP_STACK_MAX 4

/* 初始化：读配置 → 决定起始屏（首启进引导，否则直接进主界面）。
 * 返回 false 表示配置层出问题（目前不会致命，仅记录）。 */
bool app_init(void);

/* 收尾：把 dirty 的配置落盘，写 status.txt。 */
void app_shutdown(void);

/* ---- 导航 ---- */
ScreenId app_current(void);
ScreenId app_previous(void);              /* 栈里下一层；栈只有一层时返回 SCREEN_NONE */
void     app_push(ScreenId id);
void     app_pop(void);
void     app_switch(ScreenId id);         /* 替换栈顶，不增长栈 */
int      app_depth(void);

/* ---- 配置 ---- */
AppConfig *app_cfg(void);

/* 字号档位（small/mid/large）-> 渲染缩放百分比（100/125/150）。
 * 阅读区（对话正文、文档浏览）用它，按钮与表格不受影响。 */
int app_text_scale(void);
void       app_cfg_dirty(void);           /* 标记"有改动"，shutdown 时写 */
bool       app_cfg_save_now(void);        /* 立刻落盘（引导跑完就用它） */
bool       app_cfg_was_saved(void);
const ConfigLoadInfo *app_cfg_info(void);

/* creds.enc 在不在（M4/M5 之前恒为 false） */
bool app_ai_configured(void);

/* ---- M5 加密（ChaCha20 + Poly1305）----
 * 自检在 app_init 里跑，结果进 status.txt；这里给界面查询用。 */
bool app_crypto_ok(void);              /* 自检全过且加解密往返自洽 */
int  app_crypto_selftest_passed(void); /* 通过项数（对照 CRYPTO_SELFTEST_TOTAL） */
bool app_creds_used(void);             /* creds.enc 存在且自检通过 = 加密存储在用 */

/* ---- 每帧 ---- */
void app_render(void);
void app_handle(u32 down, u32 held);

/* ---- 触摸（底屏 320x240 显示序坐标） ----
 * main 每帧喂一次：KEY_TOUCH 按住期间传 (x,y)，松开传 -1。
 * 屏幕层在 handle() 里用下面两个查询做按钮命中测试：
 *   app_touch_down()  —— 只在这一帧是**按下沿**时为真（用来触发动作）
 *   app_touch_held()  —— 只要手指还按着就为真（用来画"按下"的样子）
 * 分开是必要的：动作只能触发一次，但"按下"的外观要持续整个按压过程。 */
void app_touch_frame(int x, int y, bool pressed);
bool app_touch_down(int *x, int *y);
bool app_touch_held(int *x, int *y);

/* 便捷：手指这帧是否按在 r 上（画按下态用）。 */
bool app_touch_on(const Rect *r);

/* ---- 摇杆（圆摇）----
 * main 每帧喂一次原始值（-156..156，已钳制）。屏幕层自己决定死区
 * 与用法 —— 采集层不替它判断"多大才算动"，因为不同屏要的灵敏度不一样。
 * 没喂过时恒为 0（= 摇杆居中），所以"没读数"与"没动"看起来一样，
 * 这是有意的：屏幕不该因为采集层没接上就画出异常状态。 */
void app_circle_frame(int x, int y);
int  app_circle_x(void);
int  app_circle_y(void);

/* ---- 状态行（页脚第一行） ---- */
void        app_status_set(const char *fmt, ...);
const char *app_status(void);

/* ---- 首启引导完成 ---- */
void app_finish_onboarding(void);

/* ---- 内置查看器 ----
 * 屏幕之间不直接互相调用（见 screen.h），所以"带着一个文件路径进查看器"
 * 也走这里：先把路径存到 app 层，再由查看器 enter 时取走。 */
void        app_open_viewer(const char *path);
const char *app_viewer_path(void);

/* ---- 诊断：写 sdmc:/3ds/aiconsole/status.txt ---- */
void app_write_status(int frames);
    void app_note(const char *fmt, ...);      /* 往 status.txt 的备注区追加一行 */
/* ---- 语音识别探针（与流式同款形状）-------------------------------
 * 值：1 = 识别成功（文字进了输入框）；2 = 识别失败（原因在 notes 里）。
 * 一次都没用过时是 -1。放在 app 层是因为 status.txt 在 app_write_status
 * 里拼装 —— 验收脚本只能读文件，这个事实必须能从文件里看出来。 */
void app_voice_report(int used);
int  ai_voice_used(void);
int  ai_voice_calls(void);

/* ---- M6 流式输出的"实际走了哪条路"探针 --------------------------------
 * 对话屏在每次发送后调 app_stream_report()。放 app 层而不是 sc_chat 的
 * 私有 static，是因为 status.txt 在别处（app_write_status）拼装 ——
 * 验收脚本只能读文件，所以这个事实必须能从文件里看出来。
 * 值：0 = 这次用的是非流式；1 = 流式成功；2 = 流式失败后退回非流式。
 * 没发过请求时是 -1，避免和"非流式"混为一谈。 */
void app_stream_report(int used);
int  ai_stream_used(void);
int  ai_stream_calls(void);

/* ---- 动作结果的回灌缓冲（对话屏用）---- */

/* 动作结果缓冲的上限。480 是这么定的：它要装得下列一个目录的若干条文件名
 * 或一个文件的前几百字节，而再多对模型这一轮也没有额外价值 ——
 * 真要读长内容，模型会再发一次动作分段读。
 * 刻意不用 2 KB：那会让 system 消息比手册还长，而手册才是必须整份送达的部分。 */
#define ACT_RESULT_MAX 480

/* 记下一次动作的执行结果，等下一轮拼进 system 消息。
 * 整体覆盖（不追加）：连续两个动作的结果里，后者才是当下的事实，
 * 留着旧的只会让模型拿到一份过期的当前状态。 */
void app_act_result_store(const char *text);

/* 读 / 清。做成函数而不是把缓冲 extern 出去：
 * 那样"读了顺手清掉"这个约定就没有地方落实，而它重复一次就会让模型
 * 拿到两份一模一样的结果，误以为出了两次不同的结果。 */
const char *app_act_result_get(void);
void        app_act_result_clear(void);

/* ---- 同一件事的**另一句话**：给界面（对话列表）看 ----
 *
 * 存的是"动作后来怎么样了"这句人话，等回到对话屏时被取走显示一次。
 * 与 app_act_result_store 的区别见 app.c 的实现注释 —— 两句话的
 * **读者不同**（一个给模型、一个给人），共用一份必然露馅。
 *
 * take() = 读且不改（调用方显示完自己 clear）；
 * 之所以不自动清：显示那一步可能因为屏幕不在对话屏而被跳过，
 * 自动清会让那条记录凭空消失。 */
void        app_act_event_set(const char *text);
const char *app_act_event_take(void);
void        app_act_event_clear(void);
