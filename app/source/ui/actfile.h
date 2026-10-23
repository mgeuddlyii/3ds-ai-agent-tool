/*
 * actfile.h -- file.delete / file.rename / key.remove / key.switch 的执行
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>

/* 移到沙箱回收站（out/.trash/），不真删。 */
bool actfile_delete(const char *name, char *msg, size_t cap);

/* 沙箱内改名。目标已存在时拒绝（不覆盖）。 */
bool actfile_rename(const char *from, const char *to, char *msg, size_t cap);

/* 删掉一把 Key。仓库里只剩一把时拒绝。 */
bool actkey_remove(const char *name, char *msg, size_t cap);

/* 列出回收站内容（永久删除前要让用户看清要没的是什么）。 */
bool actfile_trash_list(char *out, size_t cap);

/* 清空回收站 —— **真删，不可恢复**。调用方必须先做两次确认。 */
bool actfile_trash_purge(char *msg, size_t cap);

/* 切到某一把 Key。 */
bool actkey_switch(const char *name, char *msg, size_t cap);
