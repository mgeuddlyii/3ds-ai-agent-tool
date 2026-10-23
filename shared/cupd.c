/*
 * cupd.c — 见 cupd.h
 *
 * 与 ota.c 的关系：复用 sha256 与 minijson，**不复用它的版本模型**。
 * 内容是三类独立数据、各有各的版本号，而 ota 的
 * `version_code > cur_code` 那个单值模型表达不了「模型旧、知识库新」
 * 这种状态。具体分歧见 cupd.h 头部。
 */

#include "cupd.h"

#include "minijson.h"
#include "sha256.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define DIR_PATH  "sdmc:/3ds/aiconsole"
#define CU_PACK   DIR_PATH "/content"      /* 内容更新包目录 */
#define CU_MANIFEST_NAME "manifest.json"

/*
 * 三类内容的**文件名**（不含目录）。落盘目录由 g_root 决定，
 * 这样一个 root 就切换了全部三类内容的位置 —— 自测需要这个。
 */
static const char *const CU_FILE[CU_KIND_COUNT] = {
    "guide_model.bin",     /* CU_MODEL     */
    "guide_kb.bin",        /* CU_KB        */
    "hw_knowledge.txt",    /* CU_HARDWARE  */
};

/* 与 CU_FILE 一一对应的显示名，供 UI 用 */
static const char *const CU_KIND_NAME[CU_KIND_COUNT] = {
    "推理模型", "攻略库", "3DS 硬件知识库"
};

/*
 * 各内容当前版本的记录文件名。
 *
 * 为什么单独存版本而不是「读文件头里的魔数当版本」：
 * 攻略库和硬件知识库都是纯文本/自定义容器，格式里没有版本位；
 * 而模型文件的 GM1 头里**也没有版本字段**（只有 8 个维度）。
 * 与其为了记版本去改所有文件格式，不如在更新成功后写一个小文件。
 *
 * 信任问题：SD 卡上的版本记录用户可以随便改，改大了就「没有更新」，
 * 改小了就会收到降级拒绝 —— 但这些都是用户在自己卡上做的事，
 * 且真要防的话该防的是「安装包本身」，那是 sha256 的职责。
 * 版本记录不是安全边界，只是「避免重复安装同一个包」。
 */
static const char *const CU_VERFILE[CU_KIND_COUNT] = {
    "ver_model.txt", "ver_kb.txt", "ver_hw.txt"
};

/* 可切换的根目录。NULL = 用默认的 sdmc:/3ds/aiconsole。 */
static const char *g_root = NULL;

/* 拼出某个常量文件名在当前根下的完整路径。
 * root_len 为 0 时表示"路径就是文件名本身"（自测用相对路径）。 */
static const char *root_join(char *buf, size_t cap,
                             const char *root, const char *name)
{
    if (!root || !*root)
        snprintf(buf, cap, "%s", name);
    else
        snprintf(buf, cap, "%s/%s", root, name);
    return buf;
}

void cu_set_root(const char *root)
{
    g_root = (root && *root) ? root : NULL;
}

const char *cu_dir(void)
{
    static char buf[320];
    if (!g_root)
        return CU_PACK;
    snprintf(buf, sizeof buf, "%s/content", g_root);
    return buf;
}

const char *cu_target(CuKind k)
{
    static char buf[320];
    /*
     * ⚠️ 这里原来写的是 `if ((int)k < 0 || (int)k >= CU_KIND_COUNT)`，
     * gcc 报 "comparison is always false due to limited range of
     * data type" —— 因为 CuKind 是**无符号枚举**，k<0 恒假。
     * 越界防御的前半段从来没生效过。
     *
     * 另一半（>= COUNT）是真的在防的：调用方传一个越界枚举值过来，
     * 数组下标就会越界读。所以保留那一半，去掉恒假的那一半。
     */
    if ((unsigned)k >= (unsigned)CU_KIND_COUNT)
        return "";
    return root_join(buf, sizeof buf, g_root, CU_FILE[k]);
}

/* 版本记录文件的完整路径（内部用） */
static const char *ver_path(char *buf, size_t cap, CuKind k)
{
    return root_join(buf, cap, g_root, CU_VERFILE[k]);
}

/* 备份路径：正式名 + ".bak"。
 * cu_has_backup 定义在文件前面（它是对外接口），所以这里要前向声明。
 * ⚠️ 声明与定义必须**完全一致**（static void + 同样的参数类型）。
 * 少写 static 会报 "static declaration follows non-static declaration"，
 * 位置放错（用在声明之前）会报 implicit declaration —— 两个错误的
 * 指的都是同一件事，但 gcc 不会告诉你"你只是把声明放晚了"。 */
static void bak_path(char *dst, size_t cap, CuKind k);

/* 包目录下的某个产物 */
static const char *pack_path(char *buf, size_t cap, const char *file)
{
    char pack[320];
    if (!g_root) {
        snprintf(pack, sizeof pack, "%s", CU_PACK);
    } else {
        snprintf(pack, sizeof pack, "%s/content", g_root);
    }
    snprintf(buf, cap, "%s/%s", pack, file);
    return buf;
}

const char *cu_kind_name(CuKind k)
{
    if ((unsigned)k >= (unsigned)CU_KIND_COUNT)
        return "?";
    return CU_KIND_NAME[k];
}

const char *cu_state_text(CuState s)
{
    switch (s) {
        case CU_NONE:           return "还没检查";
        case CU_NO_PACKAGE:     return "卡上没有内容更新包";
        case CU_UP_TO_DATE:     return "内容都是最新的";
        case CU_AVAILABLE:      return "有可用更新";
        case CU_VERIFY_OK:      return "校验通过，可安装";
        case CU_DONE:           return "已安装";
        case CU_ERR_PARSE:      return "manifest 读不出来";
        case CU_ERR_TOO_OLD:    return "这是降级包，已拒绝";
        case CU_ERR_MISMATCH:   return "大小或摘要对不上";
        case CU_ERR_IO:         return "读写失败";
        default:                return "?";
    }
}

/* --------------------------------------------------------------------------
 * 小工具
 * ----------------------------------------------------------------------- */

static void copy_str(char *dst, size_t cap, const char *src)
{
    if (cap == 0)
        return;
    strncpy(dst, src ? src : "", cap - 1);
    dst[cap - 1] = '\0';
}

static bool file_size(const char *path, int *out)
{
    struct stat st;
    if (stat(path, &st) != 0)
        return false;
    /*
     * ⚠️ out 允许为 NULL —— 调用方只想问「这个文件在不在」。
     * 原实现无条件写 *out，于是 cu_rollback 里那句
     * `file_size(bak, NULL)` 会空指针解引用，而且是在
     * 「回滚」这条用户主动操作的路径上崩。
     * 这种崩法尤其难查：它只在「用户想撤销一次坏更新」时出现，
     * 而那恰恰是最需要它别崩的时刻。
     */
    if (out)
        *out = (int)st.st_size;
    return true;
}

static bool read_whole(const char *path, char **buf, int *len)
{
    FILE *f;
    long sz;
    char *b;

    *buf = NULL;
    *len = 0;

    f = fopen(path, "rb");
    if (!f)
        return false;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return false; }
    sz = ftell(f);
    /* manifest 是 JSON，不会超过 4KB。给个硬上限是防止
     * 拿到一个 8MB 的坏文件时按 sz 去 malloc —— 那正是
     * 「被一个坏文件搞到 OOM」这类问题的标准成因。 */
    if (sz <= 0 || sz > 8192) { fclose(f); return false; }
    rewind(f);

    b = (char *)malloc((size_t)sz + 1);
    if (!b) { fclose(f); return false; }
    if (fread(b, 1, (size_t)sz, f) != (size_t)sz) {
        free(b);
        fclose(f);
        return false;
    }
    b[sz] = '\0';
    fclose(f);
    *buf = b;
    *len = (int)sz;
    return true;
}

static int cur_version(CuKind k)
{
    char path[320];
    FILE *f;
    int v = 0;

    f = fopen(ver_path(path, sizeof path, k), "rb");
    if (!f)
        return 0;                      /* 没记录 = 版本 0 */
    if (fscanf(f, "%d", &v) != 1)
        v = 0;
    fclose(f);
    /* 版本必须是正数。负数或 0 都当损坏，重新当 0 处理 ——
     * 「读到垃圾就信」会让一个坏记录永久挡住所有更新。 */
    if (v < 0)
        v = 0;
    return v;
}

int cu_local_version(CuKind k)
{
    if ((unsigned)k >= (unsigned)CU_KIND_COUNT)
        return 0;
    return cur_version(k);
}

bool cu_has_backup(CuKind k)
{
    char bak[344];
    int  sz = 0;

    if ((unsigned)k >= (unsigned)CU_KIND_COUNT)
        return false;
    /* bak_path 定义在文件更下面（安装/回滚那一段），这里用前要先声明。
     * 顺手提一句：它返回 void 不是 int —— 第一次写的时候按 int 用了，
     * gcc 报 "invalid use of void expression"，而这个错误信息
     * 不会提醒你"你记错了返回类型"，得自己回去看定义。 */
    bak_path(bak, sizeof bak, k);
    /* file_size 允许 out 传 NULL —— 这里只想问"在不在、有多大"。
     * sz > 0 这个附加判据是必要的：安装流程里 .old → rename → .bak
     * 做过一次改名，理论上不该留下 0 字节的 .bak；但万一断了，
     * 一个 0 字节的备份会让回滚"成功"地装上一份空文件 ——
     * 那比"回滚失败"糟得多，因为它看起来成功了。 */
    return file_size(bak, &sz) && sz > 0;
}

static bool write_version(CuKind k, int v)
{
    char path[320], tmp[336];
    FILE *f;

    ver_path(path, sizeof path, k);
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    f = fopen(tmp, "wb");
    if (!f)
        return false;
    fprintf(f, "%d\n", v);
    fclose(f);
    /* 同样是 .tmp + rename：写到一半断电不能留下半个版本号，
     * 而半个版本号（空文件）会被读成 0，等于「所有更新重来」。 */
    return rename(tmp, path) == 0;
}

static void ensure_dir(const char *path)
{
    char tmp[160];
    char *q;
    char *scan;

    if (strlen(path) >= sizeof tmp)
        return;
    strcpy(tmp, path);
    scan = (tmp[0] && tmp[1] == ':') ? tmp + 3 : tmp;
    for (q = scan; *q; ++q) {
        if (*q == '/') {
            *q = '\0';
            mkdir(tmp, 0777);
            *q = '/';
        }
    }
    mkdir(tmp, 0777);
}

/* --------------------------------------------------------------------------
 * 解析
 * ----------------------------------------------------------------------- */

typedef struct {
    CuManifest *m;
} ParseCtx;

/* kind 字符串 -> 枚举。**只认这三个**，别的名字一律忽略。
 *
 * 为什么按名字匹配而不是按数组下标：
 * manifest 里三类内容可以只带其中一两个（这次只更知识库），
 * 下标就不稳定了。而且下标一旦与 CU_KIND_COUNT 的顺序脱节，
 * 就会静默把「知识库」的文件装到「模型」的位置上。
 *
 * 短名（model/kb/hardware）而不是枚举名：包小、拼错的机会少。 */
static int kind_from_name(const char *s)
{
    if (!s || !*s)
        return -1;
    if (!strcmp(s, "model"))    return CU_MODEL;
    if (!strcmp(s, "kb"))       return CU_KB;
    if (!strcmp(s, "hardware")) return CU_HARDWARE;
    return -1;
}

static bool on_value(const MiniJsonValue *v, void *user)
{
    ParseCtx *pc = (ParseCtx *)user;
    CuManifest *m = pc->m;
    const char *p = v->path;
    int idx;
    CuArtifact *a;

    if (v->type == MJ_OBJECT_START || v->type == MJ_OBJECT_END ||
        v->type == MJ_ARRAY_START  || v->type == MJ_ARRAY_END)
        return true;

    if (strncmp(p, "artifacts.", 10) == 0) {
        /* minijson 对数组元素不拼 [i]，下标在 v->index 里。
         * （与 ota.c 同一个坑，注释留在那里避免再踩。） */
        idx = v->index;
        const char *field = p + 10;

        if (idx < 0 || idx >= CU_KIND_COUNT)
            return true;

        if (idx >= m->count)
            m->count = idx + 1;
        a = &m->art[idx];

        if (v->type == MJ_STRING) {
            if (!strcmp(field, "kn"))      copy_str(a->kn, sizeof a->kn, v->str);
            else if (!strcmp(field, "file")) copy_str(a->file, sizeof a->file, v->str);
            else if (!strcmp(field, "sha256")) copy_str(a->sha256, sizeof a->sha256, v->str);
        } else if (v->type == MJ_NUMBER) {
            if (!strcmp(field, "size"))      a->size = (int)v->num;
            else if (!strcmp(field, "version")) a->version = (int)v->num;
        }
        return true;
    }

    if (v->type == MJ_NUMBER && !strcmp(p, "schema"))
        m->schema = (int)v->num;

    return true;
}

bool cu_parse(const char *text, size_t n, CuManifest *m)
{
    ParseCtx ctx;

    if (!text || !m)
        return false;

    memset(m, 0, sizeof *m);
    ctx.m = m;

    if (!minijson_walk(text, n, on_value, &ctx))
        return false;

    if (m->schema != 1)
        return false;

    /* ---- 结构自检：每一条都必须有 kind / file / size / sha256 / version ----
     *
     * 少校验一项的后果不是「这一条被跳过」，而是
     * **那条空产物会被装到 CU_TARGET[0]（模型的位置）上** ——
     * 因为落盘位置是按 kind 查表填的，而 kind 没填就没法查，
     * 剩下的字段全是空串，一个空文件照样能通过 size 校验（size 0）。
     * 所以这里逐条把关，而不是等 install 时再说。 */
    {
        int i;
        for (i = 0; i < m->count; i++) {
            CuArtifact *a = &m->art[i];
            int k;

            /* kn 是种类短名（model/kb/hardware），file 是包内文件名。
             * 这两个字段各管一件事 —— 曾经复用同一个字段装两种含义，
             * 结果 file[0] 有时是"种类名"有时是"文件名"，
             * 而 file 有时是包内路径、有时是正式路径。
             * 一个字段有两种含义时，任何一次读取都得先问「现在是什么」，
             * 而问错一次就是「把知识库装到模型的位置」。 */
            k = kind_from_name(a->kn);
            if (k < 0)
                return false;
            a->kind = (CuKind)k;

            if (!a->file[0] || !a->sha256[0])
                return false;
            /* 摘要必须是 64 个 hex。少这条，一个空串摘要
             * 会在安装时"和文件对照"失败，但**失败得晚**——
             * 在写完 .new、rename 之前。而这里挡掉的是零成本。 */
            if (strlen(a->sha256) != 64)
                return false;
            if (a->size <= 0 || a->version <= 0)
                return false;
        }
    }
    return true;
}

/* --------------------------------------------------------------------------
 * 校验
 * ----------------------------------------------------------------------- */

int cu_scan_local(CuManifest *m, CuState *state, char *detail, size_t cap)
{
    char *text = NULL;
    int   n = 0, i, n_new = 0, n_old = 0, n_checked = 0;

    if (detail && cap)
        detail[0] = '\0';

    char mpath[360];
    pack_path(mpath, sizeof mpath, CU_MANIFEST_NAME);

    if (!read_whole(mpath, &text, &n)) {
        if (state) *state = CU_NO_PACKAGE;
        if (detail && cap)
            snprintf(detail, cap, "没找到 %s", mpath);
        return 0;
    }

    if (!cu_parse(text, (size_t)n, m)) {
        free(text);
        if (state) *state = CU_ERR_PARSE;
        if (detail && cap)
            snprintf(detail, cap, "manifest 结构不对（schema 必须为 1，"
                                 "且每条都要有 kind/file/size/sha256/version）");
        return 0;
    }
    free(text);

    if (m->count <= 0) {
        if (state) *state = CU_NO_PACKAGE;
        if (detail && cap) snprintf(detail, cap, "包里有 0 条内容");
        return 0;
    }

    /* ---- 先比版本（G3）：降级的直接拒，不浪费一次 sha256 ---- */
    for (i = 0; i < m->count; i++) {
        int cur = cur_version(m->art[i].kind);
        if (m->art[i].version < cur)
            n_old++;
        else if (m->art[i].version > cur)
            n_new++;
    }

    if (n_new == 0) {
        if (state) *state = CU_UP_TO_DATE;
        if (detail && cap) {
            if (n_old > 0)
                snprintf(detail, cap, "%d 条是降级包，已拒绝（当前更新）", n_old);
            else
                snprintf(detail, cap, "%d 条内容都已是最新", m->count);
        }
        return 0;
    }

    /* ---- 再验大小与摘要（G4）----
     *
     * 只验**要装的那几条**。降级的那几条不验：反正不装，
     * 验它只是白白读一遍几百 KB。 */
    for (i = 0; i < m->count; i++) {
        char path[256], hex[65];
        int  sz = 0, cur;

        if (m->art[i].version <= cur_version(m->art[i].kind))
            continue;                       /* 不更新，也不降级 */

        n_checked++;
        pack_path(path, sizeof path, m->art[i].file);
        if (!file_size(path, &sz)) {
            if (state) *state = CU_ERR_IO;
            if (detail && cap)
                snprintf(detail, cap, "包里缺 %s", m->art[i].file);
            return 0;
        }
        cur = (int)sz;
        if (cur != m->art[i].size) {
            if (state) *state = CU_ERR_MISMATCH;
            if (detail && cap)
                snprintf(detail, cap, "%s 大小不符：包写 %d，实际 %d",
                         m->art[i].file, m->art[i].size, cur);
            return 0;
        }
        if (!sha256_file(path, hex) || strcmp(hex, m->art[i].sha256) != 0) {
            if (state) *state = CU_ERR_MISMATCH;
            if (detail && cap)
                snprintf(detail, cap, "%s 摘要不符", m->art[i].file);
            return 0;
        }
    }

    if (state) *state = CU_VERIFY_OK;
    if (detail && cap)
        snprintf(detail, cap, "%d 条可更新（已校验 %d 条，%d 条已最新，%d 条降级已拒）",
                 n_new, n_checked, m->count - n_new - n_old, n_old);
    return n_new;
}

/* --------------------------------------------------------------------------
 * 安装 / 回滚
 * ----------------------------------------------------------------------- */

/* 备份路径：正式名 + ".bak" */
static void bak_path(char *dst, size_t cap, CuKind k)
{
    snprintf(dst, cap, "%s.bak", cu_target(k));
}

/*
 * 装一个产物。顺序是 **先备份、再写新、最后写版本号**。
 *
 * 顺序为什么是这个：
 *   备份在写之前 —— 写完再备份就备份到新文件了，等于没备份；
 *   版本号在最后 —— 如果中途失败，版本没更新，下次进来还会再试一次。
 *   「版本号先写」会造成最坏情况：文件是旧的，版本号是新的，
 *   于是这个包**永远装不上**（版本比较会跳过它）。
 */
int cu_index_of_kind(const CuManifest *m, CuKind kind)
{
    if (!m || kind < 0 || kind >= CU_KIND_COUNT)
        return -1;
    for (int i = 0; i < m->count; ++i) {
        /* ★ 比的是 a->kind（解析时由 kn 映射来的枚举），
         *   不是 kn 字符串本身 —— kn 的写法由生成端决定（"hardware"/"hw"），
         *   拿它做字符串比较等于把两边的拼写绑死。 */
        if (m->art[i].kind == kind)
            return i;
    }
    return -1;
}

bool cu_install(const CuManifest *m, int idx, char *msg, size_t cap)
{
    /* 目标路径最长是 DIR_PATH(19) + 最长文件名 + ".new"。
     * 留 320 而不是 256：gcc 的 -Wformat-truncation 会因为
     * 「理论上 src 可能填满 256 再加 19」而报警。
     *
     * ⚠️ 这条警告不能靠"实际不会那么长"来忽略：
     * snprintf 截断后返回的是**已写入的字符数**，不是" would've fitted"，
     * 于是调用方检查返回值也发现不了被截断。路径被截断的后果是
     * 写到「半个路径」上 —— mkdir 失败、rename 失败，
     * 表现为一堆无意义的 IO 错误。宁可给够。 */
    char src[320], dst[320], tmp[336], bak[336], oldbak[344], hex[65];
    FILE *fi, *fo;

    if (!m || idx < 0 || idx >= m->count) {
        if (msg && cap) snprintf(msg, cap, "产物下标不对");
        return false;
    }

    /* 目录不存在就建。g_root 可能指向多级路径，逐级建。 */
    ensure_dir(g_root ? g_root : DIR_PATH);

    pack_path(src, sizeof src, m->art[idx].file);
    snprintf(dst, sizeof dst, "%s", cu_target(m->art[idx].kind));
    snprintf(tmp, sizeof tmp, "%s.new", dst);
    bak_path(bak, sizeof bak, m->art[idx].kind);
    snprintf(oldbak, sizeof oldbak, "%s.old", bak);

    /* ---- ① 备份现有的 ----
     * 没有旧文件（首次安装）不是错误，跳过备份即可。
     * 旧文件已经损坏也一样：备份一个坏文件没有价值，
     * 但也不能让它挡住安装。 */
    fi = fopen(dst, "rb");
    if (fi) {
        fo = fopen(oldbak, "wb");
        if (!fo) {
            fclose(fi);
            if (msg && cap) snprintf(msg, cap, "备份写不进去");
            return false;
        }
        {
            static unsigned char blk[8192];
            size_t got;
            while ((got = fread(blk, 1, sizeof blk, fi)) > 0) {
                if (fwrite(blk, 1, got, fo) != got) {
                    fclose(fi); fclose(fo);
                    remove(oldbak);
                    if (msg && cap) snprintf(msg, cap, "备份写到一半失败");
                    return false;
                }
            }
        }
        fclose(fi);
        if (fclose(fo) != 0) {
            remove(oldbak);
            if (msg && cap) snprintf(msg, cap, "备份关闭时出错");
            return false;
        }
        /* 备份本身也要原子：先写 .old 再改名成 .bak。
         * 否则中途断电会留下一个**半份备份**，
         * 而回滚时无法区分「备份不完整」和「这就是全部」——
         * 它会安静地用一个残缺文件覆盖掉能用的那份。 */
        if (rename(oldbak, bak) != 0) {
            remove(oldbak);
            if (msg && cap) snprintf(msg, cap, "备份改名失败");
            return false;
        }
    }

    /* ---- ② 写新文件到 .new，中途绝不碰正式名 ---- */
    fi = fopen(src, "rb");
    if (!fi) {
        if (msg && cap) snprintf(msg, cap, "读不了包里的 %s", m->art[idx].file);
        return false;
    }
    fo = fopen(tmp, "wb");
    if (!fo) {
        fclose(fi);
        if (msg && cap) snprintf(msg, cap, "写不进 %s", tmp);
        return false;
    }
    {
        /* 8KB 一块，别 read 整个文件进内存 —— 模型 1.25MB */
        static unsigned char blk[8192];
        size_t got;
        while ((got = fread(blk, 1, sizeof blk, fi)) > 0) {
            if (fwrite(blk, 1, got, fo) != got) {
                fclose(fi); fclose(fo);
                remove(tmp);
                if (msg && cap) snprintf(msg, cap, "写到 %s 时出错", tmp);
                return false;
            }
        }
    }
    fclose(fi);
    if (fclose(fo) != 0) {
        remove(tmp);
        if (msg && cap) snprintf(msg, cap, "关闭 %s 时出错", tmp);
        return false;
    }

    /* ---- ③ **先验新文件，再改名** ----
     *
     * 这一步是本模块与 ota.c 最大的行为差异。
     * ota.c 是「先改名，再由用户去 FBI 装」，出问题只能靠装不上发现。
     * 这里是「rename 之前就 sha256 一遍」：摘要不对就删掉 .new，
     * 正式名原封不动 —— 用户手上那份还能用。
     *
     * 不做这步的后果：SD 卡上的文件在传输中损坏 → rename 成功 →
     * 模型加载失败 → 而 guide_nn 的失败策略是「回退到检索直出」，
     * 于是屏上只是「没有生成」，**完全没有异常迹象**。
     * 这种静默降级比崩溃难查一个数量级。 */
    if (!sha256_file(tmp, hex) || strcmp(hex, m->art[idx].sha256) != 0) {
        remove(tmp);
        if (msg && cap)
            snprintf(msg, cap, "写入后校验不符，已丢弃，原文件未动");
        return false;
    }

    if (rename(tmp, dst) != 0) {
        remove(tmp);
        if (msg && cap) snprintf(msg, cap, "改不了 %s", dst);
        return false;
    }

    /* ---- ④ 版本号最后写 ---- */
    if (!write_version(m->art[idx].kind, m->art[idx].version)) {
        /* 文件已经装好了，只是版本号没写成。下次进来会再装一遍，
         * 那不是错的（幂等），但要告诉用户，否则会以为没装上。 */
        if (msg && cap)
            snprintf(msg, cap, "%s 已更新，但版本记录写入失败（下次会再装一次）",
                     cu_kind_name(m->art[idx].kind));
        return true;
    }

    if (msg && cap)
        snprintf(msg, cap, "%s → v%d（已备份旧文件为 .bak，可用 cu_rollback 回退）",
                 cu_kind_name(m->art[idx].kind), m->art[idx].version);
    return true;
}

bool cu_rollback(const CuManifest *m, int kind, char *msg, size_t cap)
{
    char dst[256], bak[256];

    (void)m;    /* ★ 回滚**不需要**更新包：它靠的是本机 .bak。
                 *   早先这里用 (CuKind)kind 拼路径，于是"卡上没有更新包"
                 *   时即使本机有备份也回滚不了 —— 这是不合理的耦合。
                 *   现在参数直接是 CuKind，m 只是为保持调用签名而留着。 */

    if (kind < 0 || kind >= CU_KIND_COUNT) {
        if (msg && cap) snprintf(msg, cap, "产物种类不对");
        return false;
    }

    snprintf(dst, sizeof dst, "%s", cu_target((CuKind)kind));
    bak_path(bak, sizeof bak, (CuKind)kind);

    if (!file_size(bak, NULL) ) {
        if (msg && cap) snprintf(msg, cap, "没有 %s 的备份（首次安装不会留备份）", bak);
        return false;
    }

    if (rename(bak, dst) != 0) {
        if (msg && cap) snprintf(msg, cap, "回退失败：改不了 %s", dst);
        return false;
    }

    /* 版本号要跟着回退，否则回退完「已经是最新」，
     * 用户再也不会收到这个内容的修复版本。
     * 备份里记的版本号 —— 我们没记，改成 0：
     * 0 表示「不知道」，下次任何版本都会被认为比它新，重新装。
     * 这在「回退是为了应急」的场景下是正确的选择。 */
    write_version((CuKind)kind, 0);

    if (msg && cap)
        snprintf(msg, cap, "%s 已回退到上一份（版本号归零，下次会重新收到更新）",
                 cu_kind_name((CuKind)kind));
    return true;
}
