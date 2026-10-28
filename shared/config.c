/*
 * config.c -- 见 config.h
 *
 * 写盘是手写 fprintf（结构固定，不需要通用序列化器）；
 * 读盘走 minijson 的点分路径回调。
 *
 * 落盘用"写 .tmp 再改名"：3DS 上最常见的意外是直接拔卡/断电，
 * 半个 JSON 比没有 JSON 更糟 —— 解析失败会让用户丢掉全部设置。
 * rename 在 libctru 的 fs devoptab 上不一定可用，所以备了一条复制兜底。
 */

#include "config.h"
#include "minijson.h"
#include "uistr.h"        /* uistr_lang_known：挡掉手改坏的 ui.lang */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define CONFIG_DIR    "sdmc:/3ds/aiconsole"
#define CONFIG_PATH   CONFIG_DIR "/config.json"
#define CONFIG_TMP    CONFIG_PATH ".tmp"
#define CREDS_PATH    CONFIG_DIR "/creds.enc"

/* schema 版本：1 = M2 初版；2 = 加 ai.auth_type / tutorial_completed / ota 段。
 *
 * 迁移策略是「读时补默认值」：config_load 先填 defaults 再覆盖读到的字段，
 * 所以老文件缺的那几个键自动落到默认值，不需要单独的升级函数。
 * clamp 会把读到的 version 抬到当前值，于是下一次写盘就顺带升级格式了。 */
#define CONFIG_VERSION 2

/* 读整份文件的缓冲上限。当前文件不到 400 字节，留足余量。 */
#define CONFIG_MAX_BYTES 4096

static ConfigLoadInfo g_info;

/* --------------------------------------------------------------------------
 * 小工具
 * ----------------------------------------------------------------------- */

const char *config_dir(void)   { return CONFIG_DIR; }
const char *config_path(void)  { return CONFIG_PATH; }
const char *config_creds_path(void) { return CREDS_PATH; }

const ConfigLoadInfo *config_last_load(void) { return &g_info; }

static void copy_str(char *dst, size_t cap, const char *src)
{
    if (cap == 0)
        return;
    strncpy(dst, src ? src : "", cap - 1);
    dst[cap - 1] = '\0';
}

const char *security_level_name(int level)
{
    switch (level) {
        case SEC_LEVEL_CHAT:     return S(S0819);
        case SEC_LEVEL_FILE:     return S(S0820);
        case SEC_LEVEL_SYSTEM:   return S(S0821);
        case SEC_LEVEL_LOWLEVEL: return S(S0822);
        case SEC_LEVEL_FULL:     return S(S0823);
        case SEC_LEVEL_MAXSEC:   return S(S0824);
        default:                 return "?";
    }
}

/* 思考强度的显示名。
 * ⚠️ 越界一律返回"关"，不返回 "?" —— 这一项会被拼进状态行给用户看，
 *    而"?"对着一个旋钮是没有意义的（用户不知道要怎么办）。 */
const char *thinking_name(int level)
{
    switch (level) {
        case 1:  return S(S0825);
        case 2:  return S(S0826);
        case 3:  return S(S0827);
        case 0:
        default: return S(S0350);
    }
}

/* 逐级创建 "sdmc:/a/b/c"。mkd 失败一律忽略 —— 目录已存在也算成功。 */
static void ensure_dir(const char *path)
{
    char tmp[160];
    const char *scan;

    if (strlen(path) >= sizeof tmp)
        return;

    strcpy(tmp, path);

    /* 跳过 "sdmc:" 这样的设备前缀，别去 mkdir 它 */
    scan = (tmp[0] && tmp[1] == ':') ? tmp + 3 : tmp;

    for (char *q = (char *)scan; *q; ++q) {
        if (*q == '/') {
            *q = '\0';
            mkdir(tmp, 0777);
            *q = '/';
        }
    }
    mkdir(tmp, 0777);
}

/* --------------------------------------------------------------------------
 * 默认值与规范化
 * ----------------------------------------------------------------------- */

void config_defaults(AppConfig *cfg)
{
    memset(cfg, 0, sizeof *cfg);

    cfg->version             = CONFIG_VERSION;
    cfg->first_run_completed = false;
    cfg->tutorial_completed  = false;
    cfg->launches            = 0;

    cfg->nand_backup.confirmed          = false;
    cfg->nand_backup.remind_on_startup  = true;

    /* 最小权限原则：默认停在最低等级，且退出后不保留。 */
    cfg->security.current_level   = SEC_LEVEL_CHAT;
    cfg->security.persist_level   = false;
    cfg->security.dev_mode_unlocked = false;
    cfg->security.granted_count   = 0;

    /* 设计规划.md §9 建议的默认服务商。auth_type 默认走 API Key，
     * Cookie 通道要用户显式切换才会用 —— 那是实验性路径，不做默认。 */
    copy_str(cfg->ai.provider,  sizeof cfg->ai.provider,  "deepseek");
    copy_str(cfg->ai.model,     sizeof cfg->ai.model,     "deepseek-chat");
    copy_str(cfg->ai.auth_type, sizeof cfg->ai.auth_type, "bearer");
    cfg->ai.stream = true;
    /* 人设默认**空**：不替用户拿主意。空串表示不发 system 消息。 */
    cfg->ai.system_prompt[0] = '\0';
    cfg->ai.memory[0]        = '\0';
    cfg->ai.reply_len        = 1;      /* 中：短会显得敷衍，长会拖慢 */
    cfg->ai.context_turns    = 8;      /* 够接上几句，又不至于把请求撑爆 */
    cfg->ai.reply_lang       = 0;      /* 跟随界面：多数人希望两者一致 */
    /* 自定义地址默认空 —— 只有用户主动选「自定义」并填了才用得上。
     * 空着是安全的：provider 不是 "custom" 时这一项根本不参与拼 URL。 */
    cfg->ai.base_url[0] = '\0';
    /* 思考强度默认**关**。多花 token 是用户自己的选择，不能替他想。 */
    cfg->ai.thinking = CONFIG_THINK_OFF;

    copy_str(cfg->ui.theme,     sizeof cfg->ui.theme,     "light");
    copy_str(cfg->ui.font_size, sizeof cfg->ui.font_size, "mid");
    copy_str(cfg->ui.accent,    sizeof cfg->ui.accent,    "teal");
    copy_str(cfg->ui.lang,      sizeof cfg->ui.lang,      "zh");
    cfg->ui.depth = 0.5f;

    /* 护栏：默认最保守。confirm_overwrite 置 true 且界面上不让关 ——
     * 覆盖用户的文件是唯一"做错了就没法回头"的操作，值得永久留一道闸。
     * 其余三项用户可以按自己的习惯放松。 */
    cfg->guard.confirm_overwrite = true;
    cfg->guard.confirm_delete    = true;
    cfg->guard.confirm_bulk      = true;
    cfg->guard.max_write_kb      = 512;
    cfg->guard.max_batch_files   = 20;

    /* 诊断：默认全关。这些是给验收脚本看的，正常使用不该被塞满屏幕。 */
    cfg->diag.show_layout_overflow = false;
    cfg->diag.show_notes           = false;
    cfg->diag.verbose_status       = false;

    /* OTA：自动检查默认开（只是提示），但**安装永远要人按允许**。
     * 这两件事必须分开，否则 patch 之夜就变成了远程执行 Θ。 */
    cfg->ota.enabled         = true;
    copy_str(cfg->ota.channel, sizeof cfg->ota.channel, "stable");
    cfg->ota.auto_check      = true;
    cfg->ota.last_check      = 0;
    cfg->ota.skipped_version = 0;
}

void config_clamp(AppConfig *cfg)
{
    if (cfg->security.current_level < 0 || cfg->security.current_level > SEC_LEVEL_MAX)
        cfg->security.current_level = SEC_LEVEL_CHAT;

    if (cfg->security.granted_count < 0)
        cfg->security.granted_count = 0;
    if (cfg->security.granted_count > CONFIG_MAX_GRANTED)
        cfg->security.granted_count = CONFIG_MAX_GRANTED;

    if (cfg->ui.depth < 0.0f) cfg->ui.depth = 0.0f;
    if (cfg->ui.depth > 1.0f) cfg->ui.depth = 1.0f;

    /* 思考强度夹到 0..3。**必须夹**：这个值会被翻译成请求体里的参数，
     * 越界的话要么发出个没人认的档位，要么把别人的接口参数写坏。 */
    if (cfg->ai.thinking < CONFIG_THINK_OFF) cfg->ai.thinking = CONFIG_THINK_OFF;
    if (cfg->ai.thinking > CONFIG_THINK_HIGH) cfg->ai.thinking = CONFIG_THINK_HIGH;
    /* 新字段的夹取。**必须在这里**（而不是读 JSON 时就地夹）：
     * 卡上的文件是人手可改的，脏数据要在唯一的入口被挡一次。 */
    if (cfg->ai.reply_len < CONFIG_REPLY_SHORT) cfg->ai.reply_len = CONFIG_REPLY_SHORT;
    if (cfg->ai.reply_len > CONFIG_REPLY_LONG)  cfg->ai.reply_len = CONFIG_REPLY_LONG;
    if (cfg->ai.reply_lang < 0 || cfg->ai.reply_lang > 2) cfg->ai.reply_lang = 0;
    if (cfg->ai.context_turns <= 0) cfg->ai.context_turns = 8;
    if (cfg->ai.context_turns < CONFIG_CTX_MIN) cfg->ai.context_turns = CONFIG_CTX_MIN;
    if (cfg->ai.context_turns > CONFIG_CTX_MAX) cfg->ai.context_turns = CONFIG_CTX_MAX;

    /* 护栏夹取。confirm_overwrite 是**只读式**的：无论文件里写什么，
     * 都强制回到 true —— 这是设计上的硬约束，不是可配项。 */
    cfg->guard.confirm_overwrite = true;
    if (cfg->guard.max_write_kb < 0)    cfg->guard.max_write_kb = 0;
    if (cfg->guard.max_batch_files < 0) cfg->guard.max_batch_files = 0;
    /* 上限存在的意义就是"别一次动太多"，给它一个天花板免得被写成 99999 */
    if (cfg->guard.max_batch_files > 200) cfg->guard.max_batch_files = 200;

    /* 读到旧 schema（v1）不算错，直接抬到当前版本；
     * g_info.version_seen 仍保留文件里写的旧值，供诊断说人话。 */
    if (cfg->version < CONFIG_VERSION)
        cfg->version = CONFIG_VERSION;
    if (cfg->launches < 0)
        cfg->launches = 0;

    if (cfg->ota.skipped_version < 0)
        cfg->ota.skipped_version = 0;
    if (cfg->ota.last_check < 0)
        cfg->ota.last_check = 0;
}

/* --------------------------------------------------------------------------
 * 读
 * ----------------------------------------------------------------------- */

typedef struct {
    AppConfig      *cfg;
    ConfigLoadInfo *info;
} LoadCtx;

/* 把一个标量路径分派到结构体字段。返回 false 表示"这个路径不认识"。 */
static bool apply_scalar(LoadCtx *lc, const MiniJsonValue *v)
{
    AppConfig *c = lc->cfg;
    const char *p = v->path;

    /* ---- 数组元素：granted_actions ---- */
    if (v->index >= 0) {
        if (strcmp(p, "security.granted_actions") == 0) {
            if (v->type == MJ_STRING && c->security.granted_count < CONFIG_MAX_GRANTED) {
                copy_str(c->security.granted[c->security.granted_count],
                         sizeof c->security.granted[0], v->str);
                c->security.granted_count++;
            } else if (v->type != MJ_STRING) {
                lc->info->bad_values++;
            }
            return true;
        }
        return false;
    }

    /* ---- 根 ---- */
    if (!strcmp(p, "version")) {
        if (v->type == MJ_NUMBER) { c->version = (int)v->num; lc->info->version_seen = (int)v->num; }
        else lc->info->bad_values++;
        return true;
    }
    if (!strcmp(p, "first_run_completed")) {
        if (v->type == MJ_BOOL) c->first_run_completed = v->boolean;
        else lc->info->bad_values++;
        return true;
    }
    if (!strcmp(p, "tutorial_completed")) {
        if (v->type == MJ_BOOL) c->tutorial_completed = v->boolean;
        else lc->info->bad_values++;
        return true;
    }
    if (!strcmp(p, "launches")) {
        if (v->type == MJ_NUMBER) c->launches = (int)v->num;
        else lc->info->bad_values++;
        return true;
    }

    /* ---- nand_backup ---- */
    if (!strcmp(p, "nand_backup.confirmed")) {
        if (v->type == MJ_BOOL) c->nand_backup.confirmed = v->boolean;
        else lc->info->bad_values++;
        return true;
    }
    if (!strcmp(p, "nand_backup.remind_on_startup")) {
        if (v->type == MJ_BOOL) c->nand_backup.remind_on_startup = v->boolean;
        else lc->info->bad_values++;
        return true;
    }

    /* ---- security ---- */
    if (!strcmp(p, "security.current_level")) {
        if (v->type == MJ_NUMBER) {
            int lv = (int)v->num;
            if (lv < 0 || lv > SEC_LEVEL_MAX)
                lc->info->bad_values++;
            c->security.current_level = lv;
        } else {
            lc->info->bad_values++;
        }
        return true;
    }
    if (!strcmp(p, "security.persist_level")) {
        if (v->type == MJ_BOOL) c->security.persist_level = v->boolean;
        else lc->info->bad_values++;
        return true;
    }
    if (!strcmp(p, "security.dev_mode_unlocked")) {
        if (v->type == MJ_BOOL) c->security.dev_mode_unlocked = v->boolean;
        else lc->info->bad_values++;
        return true;
    }

    /* ---- ai ---- */
    if (!strcmp(p, "ai.provider")) {
        if (v->type == MJ_STRING) copy_str(c->ai.provider, sizeof c->ai.provider, v->str);
        else lc->info->bad_values++;
        return true;
    }
    if (!strcmp(p, "ai.model")) {
        if (v->type == MJ_STRING) copy_str(c->ai.model, sizeof c->ai.model, v->str);
        else lc->info->bad_values++;
        return true;
    }
    if (!strcmp(p, "ai.stream")) {
        if (v->type == MJ_BOOL) c->ai.stream = v->boolean;
        else lc->info->bad_values++;
        return true;
    }
    if (!strcmp(p, "ai.auth_type")) {
        if (v->type == MJ_STRING) copy_str(c->ai.auth_type, sizeof c->ai.auth_type, v->str);
        else lc->info->bad_values++;
        return true;
    }
    if (!strcmp(p, "ai.system_prompt")) {
        if (v->type == MJ_STRING) copy_str(c->ai.system_prompt, sizeof c->ai.system_prompt, v->str);
        else lc->info->bad_values++;
        return true;
    }
    /* 自定义服务商地址。copy_str 会在超长时安全截断 ——
     * 与 config_clamp 里"不截断 URL"的说法不冲突：这里截的是**文件里的脏数据**，
     * 宁可要一个短一点但完整的地址段，也不要一个越界的拼接。 */
    if (!strcmp(p, "ai.base_url")) {
        if (v->type == MJ_STRING) copy_str(c->ai.base_url, sizeof c->ai.base_url, v->str);
        else lc->info->bad_values++;
        return true;
    }
    if (!strcmp(p, "ai.thinking")) {
        if (v->type == MJ_NUMBER) c->ai.thinking = (int)v->num;
        else lc->info->bad_values++;
        return true;
    }
    if (!strcmp(p, "ai.memory")) {
        if (v->type == MJ_STRING) copy_str(c->ai.memory, sizeof c->ai.memory, v->str);
        else lc->info->bad_values++;
        return true;
    }
    if (!strcmp(p, "ai.reply_len")) {
        if (v->type == MJ_NUMBER) c->ai.reply_len = (int)v->num;
        else lc->info->bad_values++;
        return true;
    }
    if (!strcmp(p, "ai.context_turns")) {
        if (v->type == MJ_NUMBER) c->ai.context_turns = (int)v->num;
        else lc->info->bad_values++;
        return true;
    }
    if (!strcmp(p, "ai.reply_lang")) {
        if (v->type == MJ_NUMBER) c->ai.reply_lang = (int)v->num;
        else lc->info->bad_values++;
        return true;
    }

    /* ---- ui ---- */
    if (!strcmp(p, "ui.theme")) {
        if (v->type == MJ_STRING) copy_str(c->ui.theme, sizeof c->ui.theme, v->str);
        else lc->info->bad_values++;
        return true;
    }
    if (!strcmp(p, "ui.depth")) {
        if (v->type == MJ_NUMBER) c->ui.depth = (float)v->num;
        else lc->info->bad_values++;
        return true;
    }
    if (!strcmp(p, "ui.accent")) {
        if (v->type == MJ_STRING) copy_str(c->ui.accent, sizeof c->ui.accent, v->str);
        else lc->info->bad_values++;
        return true;
    }

    if (!strcmp(p, "ui.font_size")) {
        if (v->type == MJ_STRING) copy_str(c->ui.font_size, sizeof c->ui.font_size, v->str);
        else lc->info->bad_values++;
        return true;
    }
    if (!strcmp(p, "ui.lang")) {
        if (v->type == MJ_STRING) {
            /* 只认认识的短码。写错（比如 "english"、"e"、"EN "带空格）
             * 静默退回中文**而不是**把界面切成半中半英：
             * 认不出的值说明文件是手改坏的，宁可保守。 */
            if (uistr_lang_known(v->str))
                copy_str(c->ui.lang, sizeof c->ui.lang, v->str);
            else
                lc->info->bad_values++;
        } else {
            lc->info->bad_values++;
        }
        return true;
    }

    /* ---- guard（界面护栏） ---- */
    if (!strcmp(p, "guard.confirm_overwrite")) {
        /* 刻意不读取值：这一项是硬约束，文件里写 false 也不生效，
         * 但**要计数**，好让用户从 status.txt 看出他的修改被忽略了。 */
        if (v->type != MJ_BOOL || !v->boolean)
            lc->info->bad_values++;
        c->guard.confirm_overwrite = true;
        return true;
    }
    if (!strcmp(p, "guard.confirm_delete")) {
        if (v->type == MJ_BOOL) c->guard.confirm_delete = v->boolean;
        else lc->info->bad_values++;
        return true;
    }
    if (!strcmp(p, "guard.confirm_bulk")) {
        if (v->type == MJ_BOOL) c->guard.confirm_bulk = v->boolean;
        else lc->info->bad_values++;
        return true;
    }
    if (!strcmp(p, "guard.max_write_kb")) {
        if (v->type == MJ_NUMBER) c->guard.max_write_kb = (int)v->num;
        else lc->info->bad_values++;
        return true;
    }
    if (!strcmp(p, "guard.max_batch_files")) {
        if (v->type == MJ_NUMBER) c->guard.max_batch_files = (int)v->num;
        else lc->info->bad_values++;
        return true;
    }

    /* ---- diag ---- */
    if (!strcmp(p, "diag.show_layout_overflow")) {
        if (v->type == MJ_BOOL) c->diag.show_layout_overflow = v->boolean;
        else lc->info->bad_values++;
        return true;
    }
    if (!strcmp(p, "diag.show_notes")) {
        if (v->type == MJ_BOOL) c->diag.show_notes = v->boolean;
        else lc->info->bad_values++;
        return true;
    }
    if (!strcmp(p, "diag.verbose_status")) {
        if (v->type == MJ_BOOL) c->diag.verbose_status = v->boolean;
        else lc->info->bad_values++;
        return true;
    }

    /* ---- ota ---- */
    if (!strcmp(p, "ota.enabled")) {
        if (v->type == MJ_BOOL) c->ota.enabled = v->boolean;
        else lc->info->bad_values++;
        return true;
    }
    if (!strcmp(p, "ota.channel")) {
        if (v->type == MJ_STRING) copy_str(c->ota.channel, sizeof c->ota.channel, v->str);
        else lc->info->bad_values++;
        return true;
    }
    if (!strcmp(p, "ota.auto_check")) {
        if (v->type == MJ_BOOL) c->ota.auto_check = v->boolean;
        else lc->info->bad_values++;
        return true;
    }
    if (!strcmp(p, "ota.last_check")) {
        if (v->type == MJ_NUMBER) c->ota.last_check = (int)v->num;
        else lc->info->bad_values++;
        return true;
    }
    if (!strcmp(p, "ota.skipped_version")) {
        if (v->type == MJ_NUMBER) c->ota.skipped_version = (int)v->num;
        else lc->info->bad_values++;
        return true;
    }

    return false;
}

static bool on_scalar(const MiniJsonValue *v, void *user)
{
    LoadCtx *lc = (LoadCtx *)user;

    /* 结构节点不参与取值，但仍需被"认识"。 */
    if (v->type == MJ_OBJECT_START || v->type == MJ_OBJECT_END ||
        v->type == MJ_ARRAY_START  || v->type == MJ_ARRAY_END)
        return true;

    if (!apply_scalar(lc, v))
        lc->info->unknown_keys++;

    return true;
}

bool config_load(AppConfig *cfg)
{
    FILE *f;
    static char buf[CONFIG_MAX_BYTES];
    size_t n;

    memset(&g_info, 0, sizeof g_info);
    g_info.version_seen = -1;

    config_defaults(cfg);

    f = fopen(CONFIG_PATH, "rb");
    if (!f) {
        /* 文件不存在是**正常**的（首启），不算解析失败。 */
        return false;
    }
    g_info.file_present = true;

    n = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    buf[n] = '\0';

    if (n == 0) {
        g_info.parse_ok = false;
        return false;
    }

    if (!minijson_walk(buf, n, on_scalar, &(LoadCtx){ cfg, &g_info })) {
        g_info.parse_ok = false;
        /* 解析到一半已经写进去的字段不可信 —— 整体回退到默认值。 */
        config_defaults(cfg);
        return false;
    }

    g_info.parse_ok = true;
    config_clamp(cfg);
    return true;
}

bool config_exists(void)
{
    FILE *f = fopen(CONFIG_PATH, "rb");
    if (!f)
        return false;
    fclose(f);
    return true;
}

bool config_ai_configured(void)
{
    FILE *f = fopen(CREDS_PATH, "rb");
    if (!f)
        return false;
    fclose(f);
    return true;
}

/* --------------------------------------------------------------------------
 * 写
 * ----------------------------------------------------------------------- */

static const char *jbool(bool b) { return b ? "true" : "false"; }

/* 我们的取值都来自固定集合，但仍然转义一遍：
 * 用户手改过的 provider/model 可能带引号，写回去不能把文件弄坏。 */
static void jstr(FILE *f, const char *s)
{
    fputc('"', f);
    for (; s && *s; ++s) {
        unsigned char ch = (unsigned char)*s;
        switch (ch) {
            case '"':  fputs("\\\"", f); break;
            case '\\': fputs("\\\\", f); break;
            case '\n': fputs("\\n", f);  break;
            case '\r': fputs("\\r", f);  break;
            case '\t': fputs("\\t", f);  break;
            default:
                if (ch < 0x20)
                    fprintf(f, "\\u%04x", ch);
                else
                    fputc((char)ch, f);
                break;
        }
    }
    fputc('"', f);
}

static void write_json(FILE *f, const AppConfig *c)
{
    fprintf(f, "{\n");
    fprintf(f, "  \"version\": %d,\n", c->version);
    fprintf(f, "  \"first_run_completed\": %s,\n", jbool(c->first_run_completed));
    fprintf(f, "  \"tutorial_completed\": %s,\n", jbool(c->tutorial_completed));
    fprintf(f, "  \"launches\": %d,\n", c->launches);

    fprintf(f, "  \"nand_backup\": {\n");
    fprintf(f, "    \"confirmed\": %s,\n", jbool(c->nand_backup.confirmed));
    fprintf(f, "    \"remind_on_startup\": %s\n", jbool(c->nand_backup.remind_on_startup));
    fprintf(f, "  },\n");

    fprintf(f, "  \"security\": {\n");
    fprintf(f, "    \"current_level\": %d,\n", c->security.current_level);
    fprintf(f, "    \"persist_level\": %s,\n", jbool(c->security.persist_level));
    fprintf(f, "    \"granted_actions\": [");
    for (int i = 0; i < c->security.granted_count; ++i) {
        if (i) fputs(", ", f);
        jstr(f, c->security.granted[i]);
    }
    fprintf(f, "],\n");
    fprintf(f, "    \"dev_mode_unlocked\": %s\n", jbool(c->security.dev_mode_unlocked));
    fprintf(f, "  },\n");

    fprintf(f, "  \"ai\": {\n");
    fprintf(f, "    \"provider\": ");
    jstr(f, c->ai.provider);
    fprintf(f, ",\n    \"model\": ");
    jstr(f, c->ai.model);
    fprintf(f, ",\n    \"auth_type\": ");
    jstr(f, c->ai.auth_type);
    fprintf(f, ",\n    \"stream\": %s,\n    \"system_prompt\": ", jbool(c->ai.stream));
    jstr(f, c->ai.system_prompt);
    fprintf(f, ",\n    \"base_url\": ");
    jstr(f, c->ai.base_url);
    fprintf(f, ",\n    \"thinking\": %d,\n    \"memory\": ", c->ai.thinking);
    jstr(f, c->ai.memory);
    fprintf(f, ",\n    \"reply_len\": %d,\n    \"context_turns\": %d,\n    \"reply_lang\": %d\n",
            c->ai.reply_len, c->ai.context_turns, c->ai.reply_lang);
    fprintf(f, "  },\n");
    fprintf(f, "  \"ui\": {\n");
    fprintf(f, "    \"theme\": ");
    jstr(f, c->ui.theme);
    /* 手工拼两位小数：%f 会把 newlib dtoa（2-3KB 栈）拖进落盘路径 */
    {
        int pct = (int)(c->ui.depth * 100.0f + 0.5f);
        fprintf(f, ",\n    \"depth\": %d.%02d,\n", pct / 100, pct % 100);
    }
    fprintf(f, "    \"font_size\": ");
    jstr(f, c->ui.font_size);
    /* ★ 每个字段都是"键 + 值 + 逗号"三段，jstr 只负责值。
     *   这里曾把 accent 写成紧随其后的一句 jstr(f, ...)（没有键、没有逗号），
     *   产物就成了 "font_size": "mid""teal" —— 整个 config.json 失效，
     *   而症状是"验收一启动就 JSON 解析失败"，看起来和主题色毫无关系。 */
    fprintf(f, ",\n    \"accent\": ");
    jstr(f, c->ui.accent);
    fprintf(f, ",\n    \"lang\": ");
    jstr(f, c->ui.lang);
    fprintf(f, "\n  },\n");

    fprintf(f, "  \"guard\": {\n");
    fprintf(f, "    \"confirm_overwrite\": %s,\n", jbool(c->guard.confirm_overwrite));
    fprintf(f, "    \"confirm_delete\": %s,\n", jbool(c->guard.confirm_delete));
    fprintf(f, "    \"confirm_bulk\": %s,\n", jbool(c->guard.confirm_bulk));
    fprintf(f, "    \"max_write_kb\": %d,\n", c->guard.max_write_kb);
    fprintf(f, "    \"max_batch_files\": %d\n", c->guard.max_batch_files);
    fprintf(f, "  },\n");

    fprintf(f, "  \"diag\": {\n");
    fprintf(f, "    \"show_layout_overflow\": %s,\n", jbool(c->diag.show_layout_overflow));
    fprintf(f, "    \"show_notes\": %s,\n", jbool(c->diag.show_notes));
    fprintf(f, "    \"verbose_status\": %s\n", jbool(c->diag.verbose_status));
    fprintf(f, "  },\n");

    /* OTA 段故意不含 url —— 更新源只能来自代码里的常量，不能来自被改过的文件。 */
    fprintf(f, "  \"ota\": {\n");
    fprintf(f, "    \"enabled\": %s,\n", jbool(c->ota.enabled));
    fprintf(f, "    \"channel\": ");
    jstr(f, c->ota.channel);
    fprintf(f, ",\n    \"auto_check\": %s,\n", jbool(c->ota.auto_check));
    fprintf(f, "    \"last_check\": %d,\n", c->ota.last_check);
    fprintf(f, "    \"skipped_version\": %d\n", c->ota.skipped_version);
    fprintf(f, "  }\n");

    fprintf(f, "}\n");
}

/* 兜底：把 src 整份复制到 dst。rename 不可用时用。 */
static bool copy_file(const char *src, const char *dst)
{
    FILE *a = fopen(src, "rb");
    FILE *b;

    if (!a)
        return false;

    remove(dst);
    b = fopen(dst, "wb");
    if (!b) {
        fclose(a);
        return false;
    }

    char chunk[512];
    size_t got;
    bool ok = true;

    while ((got = fread(chunk, 1, sizeof chunk, a)) > 0) {
        if (fwrite(chunk, 1, got, b) != got) {
            ok = false;
            break;
        }
    }

    fclose(a);
    fclose(b);
    return ok;
}

bool config_save(const AppConfig *cfg)
{
    FILE *f;

    ensure_dir(CONFIG_DIR);

    f = fopen(CONFIG_TMP, "wb");
    if (!f) {
        /* .tmp 写不了（比如只剩读权限）就直接写正式文件，总比什么都不写好 */
        f = fopen(CONFIG_PATH, "wb");
        if (!f)
            return false;
        write_json(f, cfg);
        fclose(f);
        return true;
    }

    write_json(f, cfg);
    fflush(f);
    fclose(f);

    if (remove(CONFIG_PATH) == 0 || !config_exists()) {
        if (rename(CONFIG_TMP, CONFIG_PATH) == 0)
            return true;
    }

    /* rename 不可用（或目标已存在且不允许覆盖）→ 复制 */
    if (copy_file(CONFIG_TMP, CONFIG_PATH)) {
        remove(CONFIG_TMP);
        return true;
    }

    return false;
}
