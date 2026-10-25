/*
 * screen.h -- 屏幕接口
 *
 * 每屏是一组静态函数，通过 Screen 结构注册。屏幕之间不直接调用，
 * 一律经 app_push / app_pop / app_switch（见 app.h）—— 这样返回栈
 * 始终是唯一真相，"按 B 返回哪里"不会各屏各说各话。
 *
 * 屏幕实例是单例：状态放各 .c 的 static 变量，enter() 负责复位。
 * 3DS 上不值得为了"可重入"引入堆分配。
 *
 * 注意：M2 只落地"首启 3 屏 + 主界面占位"，完整的屏幕栈与软键盘在 M3。
 * 接口形状是按 M3 的需求定的，M3 只加屏、不改接口。
 */

#pragma once

#include <3ds.h>

typedef enum {
    SCREEN_NONE = 0,
    SCREEN_WELCOME,      /* 首启引导（内部 3 页：欢迎 / NAND / 服务） */
    SCREEN_HOME,         /* 主界面（M3 换成真菜单） */
    SCREEN_CHAT,         /* 对话：上屏对话记录，下屏输入与发送 */
    SCREEN_APIKEY,       /* 新手教程：怎么弄到 API Key、放进哪个文件、自动读取 */
    SCREEN_KEYS,         /* Key 管理：多把 Key 存仓库，屏上切换/删除/导入 */
    SCREEN_UPDATE,       /* 软件更新：上屏全部事实，下屏只有「允许」「拒绝」 */
    SCREEN_CONFIRM,      /* 动作确认：AI 想做一件事，由用户决定执不执行 */
    SCREEN_VIEWER,       /* 内置查看器：HTML 子集，无 JS 无 CSS（见 §4.9） */
    SCREEN_SETTINGS,     /* 设置：外观 / 提醒 / 安全闸门 / 诊断开关 */
    SCREEN_LAB,          /* M1 验收页：字样 / 帧缓冲 / 字库巡检（回归用） */
    SCREEN_GUIDE,        /* 离线攻略：内嵌索引检索，命中段落原文直出 */
    SCREEN_LOCALES,      /* 已设区域：列出设过 locale 的游戏，可逐个撤销 */
    SCREEN_DOUBAO,       /* 豆包登录：扫码把机器连到账号上（不用 API Key） */
    SCREEN_PROVIDERS,    /* 服务目录：列全部服务商，切换 + 按需配凭据 */
    SCREEN_KEYEDIT,      /* 录入凭据：在机器上直接敲 Key / 模型 / 网址 / 思考强度 */
    SCREEN_XFER,         /* 从电脑取文件：清单 + 分段进度（经典 Win 进度条） */
    SCREEN_PLUGINS,      /* 插件：luma/plugins 下的 3gx，能看能开关（不能删） */
    SCREEN_MORE,         /* 二级菜单：不常用的入口（区域/内容更新/传文件/插件） */
    SCREEN_PICKAPI,      /* 选 API：新建任务后进来，列已添加且有 Key 的条目 */
    SCREEN_DOCK,         /* 滑轨工作台：下屏四格（键盘/项目/文件/浏览），切面板不切屏 */
    SCREEN_COUNT,
} ScreenId;

typedef struct Screen {
    ScreenId    id;
    const char *name;                                   /* 诊断用，写进 status.txt */

    void (*enter)(void);                                /* 进入时调一次 */
    void (*leave)(void);                                /* 离开时调一次 */
    void (*render_top)(void);                           /* 当前 target = 顶屏 */
    void (*render_bottom)(void);                        /* 当前 target = 底屏 */
    void (*handle)(u32 down, u32 held);                 /* down = 本帧新按下 */
} Screen;

const Screen *screen_get(ScreenId id);

/* 用的时候才知道对不对：返回像样的名字，日志里能直接读。 */
const char *screen_name(ScreenId id);
