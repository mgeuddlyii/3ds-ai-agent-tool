/*
 * credfile.c -- 见 credfile.h
 *
 * 解析刻意做得宽容：
 *   - 空行随便写
 *   - '#' 开头是注释
 *   - provider=xxx / model=xxx / key=xxx 认得 key/value 形式
 *   - 但如果用户不写 key=，只裸贴一行 —— 第一行裸文本也算 Key
 *
 * 最后那条是特意留的：要求用户去学我们的格式，是没道理的。
 * 大多数人会直接把 Key 粘进一个空文件，那就该能直接用。
 */

#include "credfile.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#define DIR_PATH  "sdmc:/3ds/aiconsole"
#define FILE_PATH DIR_PATH "/key.txt"

const char *credfile_path(void) { return FILE_PATH; }
const char *credfile_path_display(void) { return "SD:/3ds/aiconsole/key.txt"; }

const char *credfile_status_text(CredFileStatus s)
{
    switch (s) {
        case CREDFILE_NONE:     return "还没找到 key.txt";
        case CREDFILE_EMPTY:    return "文件是空的";
        case CREDFILE_OK:       return "读到了 API Key";
        case CREDFILE_TOO_LONG: return "Key 太长了，多半粘错东西";
        default:                return "?";
    }
}

/* 去掉首尾空白与行尾换行。原地修改。 */
static char *trim(char *s)
{
    while (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n')
        ++s;

    size_t n = strlen(s);
    while (n > 0 && (s[n - 1] == ' ' || s[n - 1] == '\t' ||
                     s[n - 1] == '\r' || s[n - 1] == '\n'))
        s[--n] = '\0';

    return s;
}

static void copy_str(char *dst, size_t cap, const char *src)
{
    if (cap == 0)
        return;
    strncpy(dst, src ? src : "", cap - 1);
    dst[cap - 1] = '\0';
}

bool credfile_scan(CredFile *out)
{
    FILE *f;
    char line[320];
    bool got_key = false;

    if (!out)
        return false;

    memset(out, 0, sizeof *out);
    out->status = CREDFILE_NONE;

    f = fopen(FILE_PATH, "rb");
    if (!f)
        return false;

    out->status = CREDFILE_EMPTY;

    while (fgets(line, sizeof line, f)) {
        char *s = trim(line);
        char *eq;

        if (*s == '\0')
            continue;
        if (*s == '#')
            continue;              /* 注释 */

        ++out->lines;

        /* key / provider / model 的 k=v 形式 */
        eq = strchr(s, '=');
        if (eq) {
            *eq = '\0';
            char *k = trim(s);
            char *v = trim(eq + 1);

            if (strcmp(k, "key") == 0) {
                if (strlen(v) > CREDFILE_KEY_MAX) {
                    out->status = CREDFILE_TOO_LONG;
                    fclose(f);
                    return false;
                }
                copy_str(out->key, sizeof out->key, v);
                got_key = (*v != '\0');
            } else if (strcmp(k, "provider") == 0) {
                copy_str(out->provider, sizeof out->provider, v);
            } else if (strcmp(k, "model") == 0) {
                copy_str(out->model, sizeof out->model, v);
            } else if (strcmp(k, "base_url") == 0) {
                /* 别名 "url=xxx" 也认：用户看模板时未必记得写全名，
                 * 而"网址"这一项写成 url 是最自然的缩写。
                 * ★ 两者写同一个字段，只是入口名字不同。 */
                copy_str(out->base_url, sizeof out->base_url, v);
            } else if (strcmp(k, "url") == 0) {
                if (out->base_url[0] == '\0')
                    copy_str(out->base_url, sizeof out->base_url, v);
            }
            continue;
        }

        /* 没写过 key= 的话，第一行裸文本就当成 Key */
        if (!got_key && out->key[0] == '\0') {
            if (strlen(s) > CREDFILE_KEY_MAX) {
                out->status = CREDFILE_TOO_LONG;
                fclose(f);
                return false;
            }
            copy_str(out->key, sizeof out->key, s);
            got_key = (*s != '\0');
        }
    }

    fclose(f);

    if (got_key) {
        out->key_len = (int)strlen(out->key);
        out->status = CREDFILE_OK;
        return true;
    }

    return false;
}

void credfile_mask(const char *key, char *out, size_t cap)
{
    size_t n;

    if (!out || cap == 0)
        return;
    if (!key || *key == '\0') {
        copy_str(out, cap, "(空)");
        return;
    }

    n = strlen(key);
    if (n <= 8) {
        /* 太短的话全部打星，别把完整的短 Key 暴露出来 */
        size_t i = 0;
        while (i < n && i < cap - 1) {
            out[i++] = '*';
        }
        out[i] = '\0';
        return;
    }

    snprintf(out, cap, "%.4s****%s", key, key + n - 4);
}

/* 逐级 mkdir，失败忽略（已存在也算成功）。与 config.c 里那套同思路。 */
static void ensure_dir(const char *path)
{
    char tmp[160];
    const char *scan;
    char *q;

    if (strlen(path) >= sizeof tmp)
        return;

    strcpy(tmp, path);
    scan = (tmp[0] && tmp[1] == ':') ? tmp + 3 : tmp;

    for (q = (char *)scan; *q; ++q) {
        if (*q == '/') {
            *q = '\0';
            mkdir(tmp, 0777);
            *q = '/';
        }
    }
    mkdir(tmp, 0777);
}

bool credfile_write_template(void)
{
    FILE *f;

    ensure_dir(DIR_PATH);

    /* 别覆盖用户已有的文件 —— 那里面可能已经有他的 Key 了 */
    f = fopen(FILE_PATH, "rb");
    if (f) {
        fclose(f);
        return false;
    }

    f = fopen(FILE_PATH, "wb");
    if (!f)
        return false;

    fputs("# 把你的 API Key 粘贴在下面，删掉这一行的 # 开头，直接贴即可\n", f);
    fputs("# 例：sk-xxxxxxxxxxxxxxxxxxxxxxxx\n", f);
    fputs("#\n", f);
    fputs("# 推荐（都有免费额度）：\n", f);
    fputs("#   DeepSeek   platform.deepseek.com\n", f);
    fputs("#   火山方舟    console.volcengine.com/ark\n", f);
    fputs("#   智谱       open.bigmodel.cn\n", f);
    fputs("#\n", f);
    fputs("# 想指定服务商和模型就取消下面两行的注释：\n", f);
    fputs("# provider=deepseek\n", f);
    fputs("# model=deepseek-chat\n", f);
    fputs("#\n", f);
    fputs("# 连自己的服务器（或不在内置表里的服务商）：\n", f);
    fputs("# provider=custom\n", f);
    fputs("# base_url=https://your-server.example.com/v1\n", f);
    fputs("# model=your-model-name\n", f);

    fclose(f);
    return true;
}
