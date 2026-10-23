/*
 * keystore.c -- 见 keystore.h
 *
 * 解析走 minijson 的 walk（它对数组元素带 index，修复过对象数组的问题），
 * 写盘走 config.c 同款套路：先写 .tmp 再换名，写一半断电不留残废文件。
 */

#include "keystore.h"

#include "creds.h"
#include "minijson.h"

#include <3ds.h>
#include <stdio.h>
#include <string.h>

#define KS_PATH "sdmc:/3ds/aiconsole/keys.json"
#define KS_TMP  "sdmc:/3ds/aiconsole/keys.json.tmp"

static KeyEntry g_entries[KEYSTORE_MAX];
static int      g_count;
static char     g_active[KEYSTORE_NAME_MAX];
static bool     g_loaded;

/* ---- 解析 ---- */

typedef struct {
    int  idx;                       /* 正在填第几个条目（数组下标） */
    bool saw_active;
} WalkCtx;

static bool on_value(const MiniJsonValue *v, void *user)
{
    WalkCtx *w = (WalkCtx *)user;

    if (v->type != MJ_STRING)
        return true;

    if (!strcmp(v->path, "active")) {
        snprintf(g_active, sizeof g_active, "%s", v->str);
        w->saw_active = true;
        return true;
    }

    /* keys 数组：路径形如 keys.name / keys.provider / ...，下标在 v->index。
     * 字段按文件顺序来（name, provider, model, key），遇见 name 就开启新条目。 */
    if (v->index < 0 || v->index >= KEYSTORE_MAX)
        return true;

    /* ★ 计数在**这里**推进，不在 name 分支里（B-10）。
     *   早先只有 keys.name 会推进 g_count，于是字段顺序异常
     *   （name 不在首位、或某条缺 name）时，provider/model/key 会写进
     *   g_entries[index] 但 g_count 不前进 —— keystore_at 判越界、看不见，
     *   下一次 render_json 整条消失。**静默丢凭据**，比报错难查得多。 */
    if (v->index >= g_count)
        g_count = v->index + 1;

    if (!strcmp(v->path, "keys.name")) {
        snprintf(g_entries[v->index].name, KEYSTORE_NAME_MAX, "%s", v->str);
    } else if (!strcmp(v->path, "keys.provider")) {
        snprintf(g_entries[v->index].provider, 24, "%s", v->str);
    } else if (!strcmp(v->path, "keys.model")) {
        snprintf(g_entries[v->index].model, 48, "%s", v->str);
    } else if (!strcmp(v->path, "keys.key")) {
        snprintf(g_entries[v->index].key, sizeof g_entries[0].key, "%s", v->str);
    }
    return true;
}

/* 把一段 JSON 解析进仓库。buf 要保证以 '\0' 结尾（walk 用长度，但保险）。 */
static void parse_json(const char *buf, size_t n)
{
    WalkCtx w;
    memset(&w, 0, sizeof w);
    minijson_walk(buf, n, on_value, &w);
}

/* active 指到不存在的名字就当没激活。
 * 这里必须内联查找，**不能调 keystore_active()**：它开头 ensure_loaded，
 * 而 g_loaded 要到载入末尾才置位 —— load -> active -> ensure -> load
 * 无限递归。进对话屏（chat_enter 现读 Key）就卡死：boot.log 停在
 * "chat_enter begin"，J 趟 240s 零输出，2026-10-01 实锤。 */
static bool finish_active_check(void)
{
    if (g_active[0]) {
        bool found = false;
        for (int i = 0; i < g_count; ++i) {
            if (!strcmp(g_entries[i].name, g_active)) {
                found = true;
                break;
            }
        }
        if (!found)
            g_active[0] = '\0';
    }
    return g_count > 0;
}

/* --------------------------------------------------------------------------
 * 载入：加密优先
 *
 * creds.enc 在 → 解密后用（这是 M5 之后的主路径）；
 * 不在或解不开 → 退回明文 keys.json（老路径，也兼容用户手工放的明文）。
 *
 * 解不开时**不要**删 creds.enc：多半是换了机器，用户可能想把卡插回原机。
 * 但也不能装作没这回事 —— 状态留给 UI 显示。
 * ----------------------------------------------------------------------- */
static bool load_from_creds(void)
{
    static char plain[CREDS_MAX_PLAIN + 1];
    size_t n = 0;

    CredsStatus st = creds_load(plain, sizeof plain - 1, &n);
    if (st != CREDS_OK)
        return false;

    plain[n] = '\0';
    parse_json(plain, n);
    return g_count > 0;
}

bool keystore_load(void)
{
    FILE *f;
    static char buf[8192];
    size_t n;

    memset(g_entries, 0, sizeof g_entries);
    g_count = 0;
    g_active[0] = '\0';

    /* 1) 加密仓库 */
    if (creds_exists() && load_from_creds()) {
        g_loaded = true;
        return finish_active_check();
    }

    /* 2) 明文仓库（兼容旧文件 / 自检不可用时的兜底） */
    f = fopen(KS_PATH, "rb");
    if (!f) {
        g_loaded = true;
        return false;
    }

    n = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    buf[n] = '\0';

    parse_json(buf, n);

    g_loaded = true;
    return finish_active_check();
}

static void ensure_loaded(void)
{
    if (!g_loaded)
        keystore_load();
}

int keystore_count(void)
{
    ensure_loaded();
    return g_count;
}

bool keystore_empty(void)
{
    return keystore_count() == 0;
}

const KeyEntry *keystore_at(int i)
{
    ensure_loaded();
    if (i < 0 || i >= g_count)
        return NULL;
    return &g_entries[i];
}

const KeyEntry *keystore_active(void)
{
    int i;

    ensure_loaded();
    if (!g_active[0])
        return NULL;

    for (i = 0; i < g_count; ++i) {
        if (!strcmp(g_entries[i].name, g_active))
            return &g_entries[i];
    }
    return NULL;
}

const char *keystore_active_name(void)
{
    ensure_loaded();
    return g_active;
}

/* ---- 写盘 ---- */

/* 仓库当前存在哪：加密（creds.enc）还是明文（keys.json）。
 * 两个都在说明"加密写过但明文没删干净"（正常路径不会），以加密为准。 */
bool keystore_is_encrypted(void)
{
    return creds_exists();
}

/* 把一段 JSON 写进文件（明文路径用）。 */
static bool write_all(FILE *f, const char *buf, size_t n)
{
    return fwrite(buf, 1, n, f) == n;
}

/* 明文路径：先写 .tmp 再换名，换不动就复制。 */
static bool save_plain(const char *json, size_t n)
{
    FILE *f;

    f = fopen(KS_TMP, "wb");
    if (f) {
        bool ok = write_all(f, json, n);
        fflush(f);
        fclose(f);

        if (!ok) {
            remove(KS_TMP);
            return false;
        }

        remove(KS_PATH);
        if (rename(KS_TMP, KS_PATH) == 0)
            return true;
        /* rename 失败（跨设备之类）：退回复制 */
        remove(KS_PATH);
        f = fopen(KS_PATH, "wb");
        if (f) {
            ok = write_all(f, json, n);
            fclose(f);
        } else {
            ok = false;
        }
        remove(KS_TMP);
        return ok;
    }

    /* .tmp 都写不了就直接写正式文件 */
    f = fopen(KS_PATH, "wb");
    if (!f)
        return false;
    bool ok = write_all(f, json, n);
    fclose(f);
    return ok;
}

/* --------------------------------------------------------------------------
 * 加密优先的落盘（M5）
 *
 * 策略：先渲染成内存里的 JSON，然后
 *   1) 尝试写 creds.enc（加密）—— 成功就删掉明文的 keys.json；
 *   2) 加密不可用（自检没过 / 写失败）时退回 keys.json（明文）。
 *
 * 为什么"加密成功就删明文"：留一份 keys.json 相当于加密白做 ——
 * 拔卡的人照样能读到。删明文这一步是这个功能的意义所在。
 * 但**只在确定 creds.enc 写成功之后**才删，绝不先删后写。
 * ----------------------------------------------------------------------- */

/* 把仓库渲染进内存缓冲。返回写入长度，0 表示超了缓冲。 */
static size_t render_json(char *buf, size_t cap)
{
    size_t n = 0;
    int i;

    /* snprintf 返回值是"本该写入的长度"，加满就说明截断了 —— 用 n >= cap 判。 */
#define KS_APPEND(...)                                                       \
    do {                                                                     \
        if (n >= cap)                                                        \
            return 0;                                                        \
        int _r = snprintf(buf + n, cap - n, __VA_ARGS__);                    \
        if (_r < 0 || (size_t)_r >= cap - n)                                 \
            return 0;                                                        \
        n += (size_t)_r;                                                     \
    } while (0)

#define KS_APPEND_ESC(s)                                                     \
    do {                                                                     \
        for (const char *_p = (s); *_p; ++_p) {                              \
            unsigned char _c = (unsigned char)*_p;                           \
            if (n + 8 >= cap)                                                \
                return 0;                                                    \
            if (_c == '"' || _c == '\\') {                                   \
                buf[n++] = '\\';                                             \
                buf[n++] = (char)_c;                                         \
            } else if (_c == '\n') {                                         \
                buf[n++] = '\\';                                             \
                buf[n++] = 'n';                                              \
            } else if (_c < 0x20) {                                          \
                n += (size_t)snprintf(buf + n, cap - n, "\\u%04x", _c);      \
            } else {                                                         \
                buf[n++] = (char)_c;                                         \
            }                                                                \
        }                                                                    \
    } while (0)

    KS_APPEND("{\"version\":1,\"active\":");
    KS_APPEND("\"");
    KS_APPEND_ESC(g_active);
    KS_APPEND("\",\"keys\":[");

    for (i = 0; i < g_count; ++i) {
        if (i)
            KS_APPEND(",");
        KS_APPEND("\n{\"name\":\"");
        KS_APPEND_ESC(g_entries[i].name);
        KS_APPEND("\",\"provider\":\"");
        KS_APPEND_ESC(g_entries[i].provider);
        KS_APPEND("\",\"model\":\"");
        KS_APPEND_ESC(g_entries[i].model);
        KS_APPEND("\",\"key\":\"");
        KS_APPEND_ESC(g_entries[i].key);
        KS_APPEND("\"}");
    }
    KS_APPEND("]}\n");

#undef KS_APPEND
#undef KS_APPEND_ESC
    return n;
}

static bool save(void)
{
    static char json[8192];
    size_t n = render_json(json, sizeof json);

    if (n == 0) {
        /* 渲染失败（异常地长）—— 退回老路径，至少不静默丢数据。
         * 这里没有可写的 JSON，只能放弃并报失败。 */
        return false;
    }

    /* 1) 加密存储优先 */
    if (creds_save(json, n) == CREDS_OK) {
        /* 加密成功 → 明文的 keys.json 没有留的理由，删掉。
         * 删失败不算致命（加密文件已经写好了），但别报成功骗人。 */
        remove(KS_PATH);
        remove(KS_TMP);
        return true;
    }

    /* 2) 加密不可用 → 明文兜底 */
    return save_plain(json, n);
}

bool keystore_set_active(const char *name)
{
    int i;

    ensure_loaded();
    if (!name || !*name)
        return false;
    for (i = 0; i < g_count; ++i) {
        if (!strcmp(g_entries[i].name, name)) {
            snprintf(g_active, sizeof g_active, "%s", name);
            return save();
        }
    }
    return false;
}

/* 取消激活。理由见 keystore.h 里那段注释 —— 切到豆包这类
 * "没有 Key 条目"的服务商时，必须有一手能把旧激活清掉，
 * 否则对话屏读 keystore_active() 拿到的仍是别家。 */
bool keystore_clear_active(void)
{
    ensure_loaded();
    if (!g_active[0])
        return true;          /* 本来就没有激活，不用白落一次盘 */
    g_active[0] = '\0';
    return save();
}

bool keystore_remove(const char *name)
{
    int i, j;

    ensure_loaded();
    for (i = 0; i < g_count; ++i) {
        if (strcmp(g_entries[i].name, name))
            continue;

        for (j = i; j < g_count - 1; ++j)
            g_entries[j] = g_entries[j + 1];
        --g_count;
        memset(&g_entries[g_count], 0, sizeof(KeyEntry));

        if (!strcmp(g_active, name)) {
            /* 删的是激活中的：换头一个，没有就空着 */
            snprintf(g_active, sizeof g_active, "%s",
                     g_count > 0 ? g_entries[0].name : "");
        }
        return save();
    }
    return false;
}

/* --------------------------------------------------------------------------
 * 名字生成（导入 与 直录 共用一份）
 *
 * 为什么必须共用：这个名字是**用户唯一能看到的标识**（屏上列表、
 * 激活名都显示它）。两条路各写一份的话，迟早分叉成
 * "从文件导入的叫 zhipu-1、手敲进去的叫 zhipu1"，而用户完全无从理解
 * 为什么两把 Key 长得不一样。
 * ----------------------------------------------------------------------- */

static bool assign_unique_name(char *out, size_t cap, const char *provider)
{
    size_t pn;

    if (cap < 4)
        return false;
    memset(out, 0, cap);

    pn = provider ? strlen(provider) : 0;
    if (pn == 0) {
        memcpy(out, "key", 3);
        pn = 3;
    } else {
        if (pn > 12)
            pn = 12;
        memcpy(out, provider, pn);
    }
    if (pn + 2 >= cap)
        return false;
    out[pn++] = '-';
    out[pn++] = (char)('0' + (g_count + 1) % 10);
    out[pn]   = '\0';

    /* 名字去重：尾部数字递增，最多几次（8 把上限，够用）。
     * ⚠️ 递增时必须**自己补回结尾的 '\0'** —— 上一版靠"静态数组本来就是 0"
     *    侥幸成立，一旦谁把这块缓冲改成非零初值（或复用一段脏内存）
     *    就会读越界。这种"靠全局变量的初值兜着"的写法不能留。 */
    for (;;) {
        bool dup = false;
        int  i;

        for (i = 0; i < g_count; ++i) {
            if (!strcmp(g_entries[i].name, out)) {
                dup = true;
                break;
            }
        }
        if (!dup)
            return true;

        {
            size_t n = strlen(out);

            if (n > 0 && out[n - 1] >= '0' && out[n - 1] <= '8') {
                out[n - 1]++;
            } else if (n + 1 < cap) {
                out[n]     = '1';
                out[n + 1] = '\0';
            } else {
                return false;
            }
        }
    }
}

bool keystore_import_from_credfile(char *out_base_url, size_t out_cap)
{
    CredFile cf;
    KeyEntry *e;

    if (out_base_url && out_cap)
        out_base_url[0] = '\0';

    ensure_loaded();
    if (g_count >= KEYSTORE_MAX)
        return false;
    if (!credfile_scan(&cf) || cf.key[0] == '\0')
        return false;

    /* ★ provider / model 留空时**不再留空**：早先直接照抄（空串），
     *   结果仓库里躺着一个 provider 为空的条目 —— 界面上显示成空名字，
     *   请求也发不出去。现在留空就用 active 那把已知的，都没有则用
     *   keystore 里第一条的，最后兜底 "custom"（至少界面上不会空白）。
     *   ⚠️ 这层拿不到 cfg（shared 不许依赖 app/），所以只能靠仓库自身。 */
    if (!cf.provider[0]) {
        const KeyEntry *a = keystore_active();
        snprintf(cf.provider, sizeof cf.provider, "%s",
                 a ? a->provider : (g_count > 0 ? g_entries[0].provider : "custom"));
    }
    if (!cf.model[0]) {
        const KeyEntry *a = keystore_active();
        snprintf(cf.model, sizeof cf.model, "%s",
                 a ? a->model : (g_count > 0 ? g_entries[0].model : ""));
    }

    e = &g_entries[g_count];
    memset(e, 0, sizeof *e);
    if (!assign_unique_name(e->name, sizeof e->name, cf.provider))
        return false;

    snprintf(e->provider, sizeof e->provider, "%s", cf.provider);
    snprintf(e->model, sizeof e->model, "%s", cf.model);
    snprintf(e->key, sizeof e->key, "%s", cf.key);
    ++g_count;

    /* 第一把进来就激活，别让用户导入了还得再切一下 */
    if (g_count == 1)
        snprintf(g_active, sizeof g_active, "%s", e->name);

    /* ★ base_url 带出去给**调用方**。只有 provider=custom 且文件里真给了
     *   网址时才带 —— 内置几家的地址是编译期常量，混进配置只会误导人。 */
    if (out_base_url && out_cap && !strcmp(cf.provider, "custom") && cf.base_url[0])
        snprintf(out_base_url, out_cap, "%s", cf.base_url);

    return save();
}

bool keystore_add(const char *provider, const char *model, const char *key,
                  char *out_name, size_t out_cap)
{
    KeyEntry *e;

    if (out_name && out_cap)
        out_name[0] = '\0';

    ensure_loaded();
    if (g_count >= KEYSTORE_MAX)
        return false;
    /* 空 Key 不收：收了只会在服务目录里显示成"已配 Key"，
     * 而请求发出去必然 401 —— 那比"没配"更难查。 */
    if (!key || !key[0])
        return false;

    e = &g_entries[g_count];
    memset(e, 0, sizeof *e);
    if (!assign_unique_name(e->name, sizeof e->name, provider))
        return false;

    snprintf(e->provider, sizeof e->provider, "%s", provider ? provider : "");
    snprintf(e->model, sizeof e->model, "%s", model ? model : "");
    snprintf(e->key, sizeof e->key, "%s", key);
    ++g_count;

    /* 与导入同一条规矩：第一把自动激活。**之后的激活由调用方显式做** ——
     * 只有它知道"用户这次到底想用哪一把"（见 keystore.h 的说明）。 */
    if (g_count == 1)
        snprintf(g_active, sizeof g_active, "%s", e->name);

    if (out_name && out_cap)
        snprintf(out_name, out_cap, "%s", e->name);

    return save();
}
