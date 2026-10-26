/*
 * widgets.h -- 绘制小工具与主题
 *
 * 目的：把"页眉 22px、页脚 40px、内容区 26..200"这类版面常数，
 * 以及配色，收到一个地方。屏幕代码只描述**内容**，不重复算坐标。
 *
 * 顶屏 400x240，底屏 320x240。两块屏的页眉高度不同（底屏不需要标题栏那么高），
 * 所以常数成对出现。
 */

#pragma once

#include <stdbool.h>

#include "render.h"
#include "text.h"

/* --------------------------------------------------------------------------
 * 配色：白底 + 青(teal) 主色。状态色按用途分 ——
 * 警告用暖黄（不是红：这不是错误，是提醒），错误才用红。
 *
 * M3 起改成**运行时可切换主题**。屏幕代码一律用 w_c(C_XXX) 取色，
 * 不要再直接用 C_XXX 宏（宏仍是编译期默认值，只作为 light 主题的字面量）。
 * 这样切主题不需要重建界面代码。
 * ----------------------------------------------------------------------- */

#define C_BG       R_RGB(0xFF, 0xFF, 0xFF)
#define C_FG       R_RGB(0x1A, 0x1A, 0x1A)
#define C_MUTED    R_RGB(0x75, 0x75, 0x75)
#define C_FAINT    R_RGB(0xA8, 0xA8, 0xA8)
#define C_ACCENT   R_RGB(0x00, 0x96, 0x88)
#define C_ACC_DK   R_RGB(0x00, 0x69, 0x5C)
#define C_ACC_LT   R_RGB(0xE2, 0xF3, 0xF1)
#define C_LINE     R_RGB(0xDD, 0xDD, 0xDD)
#define C_PANEL    R_RGB(0xF5, 0xF7, 0xF7)
#define C_OK       R_RGB(0x2E, 0x7D, 0x32)
#define C_WARN     R_RGB(0xD3, 0x2F, 0x2F)
#define C_WARN_BG  R_RGB(0xFF, 0xF6, 0xD9)
#define C_WARN_LN  R_RGB(0xE8, 0xC4, 0x4A)
#define C_WARN_DK  R_RGB(0x8A, 0x64, 0x00)
#define C_DIS_FG   R_RGB(0xA8, 0xA8, 0xA8)
#define C_DIS_BG   R_RGB(0xF4, 0xF4, 0xF4)
#define C_DIS_LN   R_RGB(0xE2, 0xE2, 0xE2)

/* 主题槽位（顺序即 themes[] 的下标） */
enum {
    C_SLOT_BG = 0, C_SLOT_FG, C_SLOT_MUTED, C_SLOT_FAINT,
    C_SLOT_ACCENT, C_SLOT_ACC_DK, C_SLOT_ACC_LT, C_SLOT_LINE,
    C_SLOT_PANEL, C_SLOT_OK, C_SLOT_WARN,
    C_SLOT_WARN_BG, C_SLOT_WARN_LN, C_SLOT_WARN_DK,
    C_SLOT_DIS_FG, C_SLOT_DIS_BG, C_SLOT_DIS_LN,
    /* GEN = 模型生成的文本。**只能加在末尾** —— THEMES[][] 是按
     * 槽位顺序展开的定长数组，插在中间会把后面所有颜色错位一格，
     * 而且编译器一声不吭。 */
    C_SLOT_GEN,
    C_SLOT_COUNT
};

/* 界面主题 */
enum {
    W_THEME_LIGHT = 0,
    W_THEME_DARK  = 1,
};

/* 当前主题取色。屏幕代码用这个，不要直接用 C_XXX 宏。 */
RColor w_c(int slot);

/* 切换主题；返回切换后的主题号。传 -1 只读当前值。 */
int  w_theme(int id);
int  w_theme_current(void);
const char *w_theme_name(int id);

/* --------------------------------------------------------------------------
 * 主题色（强调色）
 *
 * 与 W_THEME_LIGHT/DARK **正交**：亮暗决定底色和前景色，
 * 主题色只换"品牌那一族"（ACCENT / ACC_DK / ACC_LT / GEN）。
 * 所以同一个蓝色在亮色下是深蓝、在暗色下是亮蓝，对比度各自成立。
 *
 * 为什么不是"再多几个完整主题"：完整主题 = 18 个槽位全换一遍，
 * 每加一个颜色就要手挑 18 个值并逐个验对比度，成本高且容易配出看不清的组合。
 * 只换强调色族的话，底色/前色/警告色都还是原来验过的那套。
 * ----------------------------------------------------------------------- */

int    w_accent_count(void);          /* 可选个数 */
int    w_accent_current(void);
void   w_accent_set(int idx);         /* 越界忽略 */
/* 第 idx 个主题色的"色块"颜色（画选择器用，与当前亮暗无关地给出饱和色）。 */
RColor w_accent_swatch(int idx);
/* 配置里存的名字（"teal"/"blue"…）<-> 下标。找不到返回 0。 */
int    w_accent_find(const char *name);
const char *w_accent_name(int idx);


/* --------------------------------------------------------------------------
 * 版面
 * ----------------------------------------------------------------------- */

#define W_TOP_HEADER_H   22
#define W_FOOTER_H       40
#define W_TOP_CONTENT_Y  26
#define W_TOP_CONTENT_B  (R_TOP_H - W_FOOTER_H)      /* 200 */
#define W_TOP_MARGIN     8
#define W_TOP_USABLE     (R_TOP_W - W_TOP_MARGIN * 2) /* 384 */

#define W_BOT_HEADER_H   24
#define W_BOT_FOOTER_H   26
#define W_BOT_CONTENT_Y  30
#define W_BOT_CONTENT_B  (R_BOT_H - W_BOT_FOOTER_H)  /* 214 */
#define W_BOT_MARGIN     10
#define W_BOT_USABLE     (R_BOT_W - W_BOT_MARGIN * 2) /* 300 */

/* --------------------------------------------------------------------------
 * 框架
 * ----------------------------------------------------------------------- */

void w_top_header(const char *title, const char *right);
void w_top_footer(const char *status, const char *hint);
void w_bot_header(const char *title);
void w_bot_footer(const char *hint);

/* 轮播式操作指引：每 ms 毫秒换一条，循环。
 * 为什么需要它见 widgets.c 里 w_bot_footer_roll 上方那段
 * （一句话：300px 只放得下 18 个全角字，整套键位一行讲不完）。
 * ms<=0 或 n==1 时退化为静态显示第一条。 */
void w_bot_footer_roll(const char *const *items, int n, int ms);

/* --------------------------------------------------------------------------
 * 内容
 * ----------------------------------------------------------------------- */

/* 单行，不折行，超出宽度裁掉。返回下一行的 y。 */
float w_line(float x, float y, float w, const char *s, RColor c);

/* 自动折行。line_h 传 0 用 TEXT_LINE_H（18）。返回画完之后的 y。 */
float w_wrap(float x, float y, float w, const char *s, RColor c, int line_h);

/* 项目符号行：左边一个小方块，文字自动折行并悬挂缩进。 */
float w_bullet(float x, float y, float w, const char *s, RColor c, int line_h);

/* 水平分隔线 */
void w_hr(float x, float y, float w, RColor c);

/* 圆角感面板（本项目只有直角，用 1px 边框 + 浅底代替） */
void w_panel(const Rect *r, RColor fill, RColor border);

/* 键值对：左列 key 用灰，右列 val 用指定色。返回下一行 y。 */
float w_kv(float x, float y, float kw, const char *key, const char *val,
           RColor kc, RColor vc, int line_h);

/* --------------------------------------------------------------------------
 * 控件
 * ----------------------------------------------------------------------- */

#define W_BTN_NORMAL    0   /* 白底 + 主色边 + 深青字 */
#define W_BTN_SELECTED  1   /* 主色底 + 白字（有焦点） */
#define W_BTN_PRIMARY   2   /* 主色底 + 白字（无焦点，但仍是主动作） */
#define W_BTN_DISABLED  3   /* 灰底灰字，"还没做"要用它，别用能点的样子 */
#define W_BTN_DANGER    4   /* 红底白字 —— **只给"删了回不来"的动作**。
                             * 项目里到处都是主色按钮，如果删除也用主色，
                             * 用户靠手感分不出"确认"和"删掉"，
                             * 而这两件事的后果差一个量级。 */

/* 按下的视觉反馈。三者是**正交**的：状态决定"这是什么按钮"，
 * 按下位决定"手指正压在上面"。所以不能把 pressed 塞进 state 枚举 ——
 * 否则"按下的主动作"和"按下的普通按钮"就得各占一个枚举值，组合爆炸。
 *
 * 按下时整块下移 1px + 边框换成深色。3DS 的屏很小，单纯变色不够明显；
 * 位移 1px 配合变色在任何底色上都看得出来。 */
#define W_BTN_PRESSED   0x100

void w_button(const Rect *r, const char *label, int state);

/* ---- 按钮按下动画（2026-10-18）----
 * 每帧调一次 w_anim_frame() 推进所有按钮的按下/松开过渡动画。
 * 必须在渲染之前调 —— main 循环里放在 app_handle 之后、app_render 之前。
 * 不调用也不会崩，只是按钮动画不会推进（停在半途）。 */
void w_anim_frame(void);
void w_anim_reset_all(void);   /* 换屏或主题切换时可清，非必需 */

/* 触摸命中测试：点 (x,y) 是否落在矩形里。render 与 handle 必须共用
 * 同一份矩形常量，别在两处手写坐标 —— 迟早漂移。 */
bool w_hit(const Rect *r, int x, int y);

/* 居中画一行（cx 是中心 x） */
void w_center(float cx, float y, const char *s, RColor c);

/* 分页圆点 */
void w_dots(float cx, float y, int n, int cur, RColor on, RColor off);

/* 分段式进度条 —— 经典 Windows（Win95/98/XP）那个观感：
 * 底槽是浅灰，条被切成一段一段，段与段之间留一条缝。
 *
 * 为什么画成分段的而不是一整块：那一段段的缝就是"老 Windows 味"的来源。
 * 连续的圆角进度条第一眼看着更现代，但用户要的正是那个年代的东西。
 *
 * 配色**刻意不跟主题**（固定经典蓝）：主题会随白天/黑夜变，
 * 而这个控件的辨识度全靠那套固定配色。跟了主题就不像了。
 * pct 传 0..100；传 NULL 的 progress 会被当成"无限动画"（不画完成色）。 */
void w_progress(float x, float y, float w, float h, int pct, const char *label);

/* 黄色提醒横幅。返回其高度。 */
float w_banner(float x, float y, float w, const char *text);

/* --------------------------------------------------------------------------
 * 版面溢出计数（回归护栏）
 *
 * 界面上大量字符串是编译期常量，字体是 16px 全角/比例半角，
 * 所以"这行会不会超出宽度"是**可计算**的 —— 但肉眼在截图里很难发现
 * 最后一个字被裁掉（就是被裁的那个字看不到，才叫难发现）。
 *
 * 于是让单行绘制自己量一遍：超出可用宽度就计数，并记下第一处冒犯者。
 * 这个数字进 status.txt，验收脚本断言它为 0。开发期改文案时，
 * 忘了量宽度会立刻被抓出来。
 *
 * 注意：w_wrap 折行绘制的不算（它本来就会折行）。
 * ----------------------------------------------------------------------- */

int         w_overflow_count(void);
void        w_overflow_reset(void);
const char *w_overflow_last(void);   /* 第一处溢出的一句话描述；没有则为 "" */

/* --------------------------------------------------------------------------
 * 纵向溢出（元素画到了内容区下界以下 = 画了但看不见）
 *
 * 为什么要与横向那条分开：横向溢出是"最后几个字被裁掉"，
 * 纵向溢出是"整个元素沉在页脚底下"。后者**横向完全合规**，
 * 所以上面那套判据一条都抓不到；而触摸验收按坐标点，
 * 只点**能看见的位置**，于是这个 bug 可以一路全绿。
 *
 * 实际咬过两次（都是内容更新屏）：
 *   五行 × 34px 画到 236，下界 200 → 最后一个条目永远点不到；
 *   改成 30px 之后余 8px，又放了一行 18px → 落在 194..212 与页脚叠字。
 *
 * 用法：画完一个块之后调 w_vbounds((int)(y + h), W_TOP_CONTENT_B)，
 * 上屏用 W_TOP_CONTENT_B、下屏用 W_BOT_CONTENT_B。
 * 它只计数不拦截 —— 屏已经画完了，此时能做的只有"下次别再犯"。
 * ----------------------------------------------------------------------- */

void w_vbounds(int bottom, int limit);
int         w_voverflow_count(void);
const char *w_voverflow_last(void);
