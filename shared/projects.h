/*
 * projects.h -- 「项目」：把一套工作上下文绑到一套 AI 上
 *
 * ── 它解决什么 ──
 * 同一个 AI 控制台，用户可能同时干几件不同的事：
 *   写攻略用便宜快的模型、改程序代码用强模型、翻译用另一家的中文模型。
 * 每换一次事就去设置里改服务商和模型，是**最容易忘**的那种操作 ——
 * 忘了就变成"拿翻译用的模型去改代码"，结果差但不报错。
 * 所以把「项目 = 名字 + 服务商 + 模型」存成一组，一键切换。
 *
 * ── 为什么用 .txt 而不是 .json ──
 * config.json 的解析是手写的，够用但只服务那一个结构；
 * 再手写一份 JSON 解析来装四五个字段，等于多一处会漂的代码。
 * 这里的数据形状是**定长表格**，一行一条，用 '|' 分隔，
 * 任何文本编辑器都能改，出错了也一眼能看出是哪一行坏了。
 *
 * ── 职责边界（重要）──
 * 这一层**只管表格**：读、写、增、删、选。
 * 它**不碰** AppConfig —— 配置实例活在 app 层（app_cfg()），
 * shared 依赖 app 会把分层搞反，也会让这批代码没法单独测。
 * 所以"切换项目 = 把 provider/model 写进 cfg"这一步在屏里做，
 * 见 sc_dock.c 的 apply_project()。
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>

#define PROJ_MAX        8
#define PROJ_NAME_MAX   23    /* 中文 7 字左右，够认出来 */
#define PROJ_MODEL_MAX  47

typedef struct {
    char name[PROJ_NAME_MAX + 1];
    char provider[24];
    char model[PROJ_MODEL_MAX + 1];
} Project;

/* 从卡上读回项目表。def_provider/def_model 只在"一条都没有"时用来建默认项。
 * 返回条数（保证 ≥1）。 */
int  projects_load(const char *def_provider, const char *def_model);

int  projects_count(void);
const Project *projects_at(int i);

/* 当前选中的项目下标（永远合法：0..count-1）。 */
int  projects_active(void);

/* 选中某条并落盘。**不写 cfg**（见文件头）。返回 false = 下标越界或写盘失败。 */
bool projects_select(int i);

/* 新建一条。返回新下标，失败 -1。 */
int  projects_add(const char *name, const char *provider, const char *model);

/* 改某条的服务商/模型并落盘。 */
bool projects_set_ai(int i, const char *provider, const char *model);

/* 改某条的名字并落盘。空名或越界返回 false。
 *
 * ★ 为什么不"删了再加一条"：那会打乱 active 下标与顺序 ——
 *   用户在任务列表里改个名，回头发现当前任务是另一个，看起来像 bug。 */
bool projects_rename(int i, const char *name);

/* 删一条（不能删到 0 条）。 */
bool projects_remove(int i);
