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

    /* ★★ 2026-10-28 用户要求：「卡上这个 API Key 文件要非常明确的格式，
     *   每个字段（网址、Key、名称）都要用易懂的形式写清楚，像一份指令。」
     *
     *   旧模板的问题是**只给例子、不给规则**：用户在电脑上打开它，
     *   看到一堆 # 注释和几个示例，仍然不知道"我该删哪行、填哪行"。
     *   现在整份文件按这个顺序写：
     *     ① 这是什么、放哪儿   ② 三个字段各是什么、怎么填
     *     ③ 三条规矩           ④ 一行不动就能用的填写区
     *   三件事都做到：**读得懂、照着填就行、填错了也不会静默失败**。
     *
     *   ⚠️ 下面 key= 故意留空：留空时 credfile_scan 会判 "文件是空的 / 没填 Key"，
     *      而不是把 `provider=deepseek` 那一行当成 Key 收下 ——
     *      不能让"模板还没填"看起来像"已经配好了"。 */
    fputs("# ============================================================\n", f);
    fputs("#  AIConsole 的 API 设置                                              \n", f);
    fputs("#                                                                    \n", f);
    fputs("#  这个文件要放在 SD 卡的**这个位置**（一个字母都不要差）：           \n", f);
    fputs("#      SD:/3ds/aiconsole/key.txt                                     \n", f);
    fputs("#                                                                    \n", f);
    fputs("#  怎么用：用电脑的记事本打开本文件 → 填最下面那三行 → 存盘            \n", f);
    fputs("#          → 回到 3DS，进「更多 → 添加 API」→ 按「从卡上导入」         \n", f);
    fputs("# ============================================================\n", f);
    fputs("#\n", f);
    fputs("# ---- 三个字段，分别是什么 ----------------------------------\n", f);
    fputs("#\n", f);
    fputs("#  名称  provider   服务商的名字。填这几个之一（小写）：\n", f);
    fputs("#                   deepseek / zhipu / ark / moonshot / dashscope /\n", f);
    fputs("#                   siliconflow / hunyuan / minimax / openai /\n", f);
    fputs("#                   gemini / grok / mistral / openrouter / custom\n", f);
    fputs("#                   连自己的服务器就写 custom（这时网址必须填）。\n", f);
    fputs("#                   不确定就**留空**，导入时在机器上再选也行。\n", f);
    fputs("#\n", f);
    fputs("#  网址  base_url   接口地址，形如 https://api.deepseek.com/v1\n", f);
    fputs("#                   用内置服务商时**留空**即可（程序自带默认地址）；\n", f);
    fputs("#                   只有 custom 或自建中转才需要填。\n", f);
    fputs("#\n", f);
    fputs("#  Key   key        你申请到的那串密钥，通常以 sk- 开头。\n", f);
    fputs("#                   **这一项必须有**，没有它就什么也连不上。\n", f);
    fputs("#\n", f);
    fputs("#  模型  model（可选）想指定具体模型时填，例如 deepseek-chat。\n", f);
    fputs("#                   留空就用该服务商的默认模型。\n", f);
    fputs("#\n", f);
    fputs("# ---- 三条规矩 ----------------------------------------------\n", f);
    fputs("#  1) 以 # 开头的整行都是说明，程序不读 —— 不放心可以全删掉。\n", f);
    fputs("#  2) 格式是「名字=值」，等号两边可以有空格，行尾空格无所谓。\n", f);
    fputs("#  3) 懒得写名字也行：直接把 Key 单独粘一行，程序认第一个像 Key 的行。\n", f);
    fputs("#     （所以最省事的用法是：删光本文件，只粘一行 Key。）\n", f);
    fputs("#\n", f);
    fputs("# ---- 往下填这三行 ------------------------------------------\n", f);
    fputs("provider=deepseek\n", f);
    fputs("base_url=\n", f);
    fputs("key=\n", f);
    fputs("#\n", f);
    fputs("# 想换个服务商？只改 provider 那一行为 deepseek 或 zhipu 等即可，\n", f);
    fputs("# 例如智谱：provider=zhipu （网址仍留空）\n", f);
    fputs("# 自建 / 中转：provider=custom 并且 base_url 必须写成完整地址。\n", f);
    fputs("#\n", f);
    fputs("# 填完存盘后，这个文件里的 Key 会被**加密搬进**程序自己的库；\n", f);
    fputs("# 想把明文从卡上抹掉，导入成功后可以把这个 key.txt 删掉。\n", f);

    fclose(f);
    return true;
}
