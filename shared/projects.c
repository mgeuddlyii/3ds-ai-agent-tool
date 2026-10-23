/*
 * projects.c -- 项目表的读写（格式与职责边界见 projects.h）
 *
 * 落点：sdmc:/3ds/aiconsole/projects.txt
 * 第一行 active=N，后面每行 "name|provider|model"。
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "projects.h"

#define PROJ_PATH "sdmc:/3ds/aiconsole/projects.txt"

static Project s_proj[PROJ_MAX];
static int     s_n;
static int     s_active;
static bool    s_loaded;

static void safe_copy(char *dst, size_t cap, const char *src)
{
    size_t n;
    if (!dst || cap == 0) return;
    if (!src) { dst[0] = '\0'; return; }
    n = strlen(src);
    if (n >= cap) n = cap - 1;
    memcpy(dst, src, n);
    dst[n] = '\0';
}

/* 把一行 "name|provider|model" 拆进项目里。
 * 不用 strtok（它会改原串），自己找 '|' —— 出错时还能把原行打出来。 */
static bool parse_line(const char *line, Project *p)
{
    const char *b1, *b2;
    size_t n, len;

    b1 = strchr(line, '|');
    if (!b1) return false;
    b2 = strchr(b1 + 1, '|');
    if (!b2) return false;

    n = (size_t)(b1 - line);
    if (n == 0 || n > PROJ_NAME_MAX) return false;
    memcpy(p->name, line, n);
    p->name[n] = '\0';

    n = (size_t)(b2 - b1 - 1);
    if (n == 0 || n >= sizeof p->provider) return false;
    memcpy(p->provider, b1 + 1, n);
    p->provider[n] = '\0';

    len = strlen(b2 + 1);
    while (len > 0 && (b2[1 + len - 1] == '\n' || b2[1 + len - 1] == '\r'))
        --len;
    if (len > PROJ_MODEL_MAX) len = PROJ_MODEL_MAX;
    memcpy(p->model, b2 + 1, len);
    p->model[len] = '\0';

    return p->name[0] != '\0' && p->provider[0] != '\0';
}

static bool save(void)
{
    FILE *f = fopen(PROJ_PATH, "wb");
    int i;

    if (!f)
        return false;

    /* 第一行 active，后面每行一条。
     * 用 key=value 开头是为了将来加字段不用换格式版本。 */
    fprintf(f, "active=%d\n", s_active);
    for (i = 0; i < s_n; ++i)
        fprintf(f, "%s|%s|%s\n", s_proj[i].name,
                s_proj[i].provider, s_proj[i].model);

    return fclose(f) == 0;
}

int projects_load(const char *def_provider, const char *def_model)
{
    FILE *f;
    char line[256];

    if (s_loaded)
        return s_n;

    s_n = 0;
    s_active = 0;

    f = fopen(PROJ_PATH, "rb");
    if (f) {
        while (fgets(line, sizeof line, f)) {
            if (strncmp(line, "active=", 7) == 0) {
                s_active = atoi(line + 7);
                continue;
            }
            if (line[0] == '\n' || line[0] == '\r' || line[0] == '\0')
                continue;
            if (line[0] == '#')
                continue;
            if (s_n >= PROJ_MAX)
                break;
            /* 坏行**跳过而不是整份放弃**：用户手工改错一行，
             * 不该让他丢掉全部项目。 */
            if (parse_line(line, &s_proj[s_n]))
                ++s_n;
        }
        fclose(f);
    }

    /* 一条都没有 → 建一条默认项，指向调用方给的当前配置。
     * 面板里永远有东西可选，不会出现"空列表 + 无从下手"。 */
    if (s_n == 0) {
        safe_copy(s_proj[0].name, sizeof s_proj[0].name, "默认");
        safe_copy(s_proj[0].provider, sizeof s_proj[0].provider,
                  def_provider && def_provider[0] ? def_provider : "deepseek");
        safe_copy(s_proj[0].model, sizeof s_proj[0].model,
                  def_model ? def_model : "");
        s_n = 1;
        s_active = 0;
        save();
    }

    if (s_active < 0 || s_active >= s_n)
        s_active = 0;

    s_loaded = true;
    return s_n;
}

int projects_count(void)          { return s_n; }

const Project *projects_at(int i)
{
    if (i < 0 || i >= s_n) return NULL;
    return &s_proj[i];
}

int projects_active(void)         { return s_active; }

bool projects_select(int i)
{
    if (i < 0 || i >= s_n) return false;
    s_active = i;
    return save();
}

int projects_add(const char *name, const char *provider, const char *model)
{
    int i;

    if (s_n >= PROJ_MAX || !name || !name[0])
        return -1;

    i = s_n;
    safe_copy(s_proj[i].name, sizeof s_proj[i].name, name);
    safe_copy(s_proj[i].provider, sizeof s_proj[i].provider,
              provider && provider[0] ? provider : "deepseek");
    safe_copy(s_proj[i].model, sizeof s_proj[i].model, model ? model : "");
    ++s_n;
    save();
    return i;
}

bool projects_set_ai(int i, const char *provider, const char *model)
{
    if (i < 0 || i >= s_n)
        return false;
    if (provider && provider[0])
        safe_copy(s_proj[i].provider, sizeof s_proj[i].provider, provider);
    if (model)
        safe_copy(s_proj[i].model, sizeof s_proj[i].model, model);
    return save();
}

bool projects_rename(int i, const char *name)
{
    if (i < 0 || i >= s_n || !name || !name[0])
        return false;
    safe_copy(s_proj[i].name, sizeof s_proj[i].name, name);
    return save();
}

bool projects_remove(int i)
{
    if (s_n <= 1 || i < 0 || i >= s_n)
        return false;               /* 不许删到 0 条：面板会变成空壳 */

    memmove(&s_proj[i], &s_proj[i + 1],
            (size_t)(s_n - i - 1) * sizeof s_proj[0]);
    --s_n;

    if (s_active >= s_n)
        s_active = s_n - 1;

    save();
    return true;
}
