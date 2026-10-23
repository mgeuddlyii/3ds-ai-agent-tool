/*
 * nnet_text.c — BPE 编码 / 解码（3DS 端）
 *
 * 词表以 GVB1 纯文本**内嵌进 .cia**（app/data/guide_vocab.txt 由
 * tools/guia/export_vocab.py 导出，tools/build.py 经 bin2s 编成
 * guide_vocab_bin.o）。加载后常驻内存，检索时不再碰文件系统。
 *
 * 内存账（vocab=4096, 实测 token 字节总量 30521 字节）：
 *   token 描述符  4096 * 16B   =  64 KB
 *   token 字节堆   30521 B     =  30 KB
 *   merge 优先级  3840 * 8B    =  30 KB
 *   合计约 124 KB（gkb_vocab_bytes 按 4096/4096 保守估到 224 KB）。
 * Old 3DS 的 64MB 里可以忽略，但工作区必须由调用方给（见 gkb_vocab_bytes）。
 */
#include "nnet_text.h"
#include "guide_vocab_bin.h"   /* 由 bin2s 生成到 build/：guide_vocab_txt[] */

#include <string.h>
#include <stdlib.h>
#include <stdio.h>

static GkbVocab g_vocab;
static int      g_loaded;

/* ---- 基础工具 --------------------------------------------------------
 * 这两个都只在**本文件内**用，所以一律 static。
 *
 * 之前它们是全局符号，而 app/source/gfx/text.c 里也有一个同名
 * utf8_decode —— C 没有重载，两个不同签名的定义在链接期是同一个符号，
 * 必然撞名。撞上之后链接器二选一，取到的那个签名与调用点不符，
 * 在 ARM 上表现为读栈上的垃圾当指针。
 */

/* 十六进制字符 -> 值，非法返回 -1 */
static int hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/*
 * UTF-8 解码。返回消耗的字节数，非法序列返回 0。
 * 3DS 上没有 locale，也不需要完整 Unicode 表：
 * 只需要知道「这个字符占几个字节」，因为归一化只关心全角区（U+FF01-U+FF5E）。
 */
static int utf8_decode(const unsigned char *p, uint32_t *cp)
{
    if (p[0] < 0x80) { *cp = p[0]; return 1; }
    if ((p[0] & 0xE0) == 0xC0 && (p[1] & 0xC0) == 0x80) {
        *cp = (uint32_t)((p[0] & 0x1F) << 6) | (p[1] & 0x3F);
        return 2;
    }
    if ((p[0] & 0xF0) == 0xE0 && (p[1] & 0xC0) == 0x80 && (p[2] & 0xC0) == 0x80) {
        *cp = (uint32_t)((p[0] & 0x0F) << 12) | (uint32_t)((p[1] & 0x3F) << 6)
            | (p[2] & 0x3F);
        return 3;
    }
    if ((p[0] & 0xF8) == 0xF0 && (p[1] & 0xC0) == 0x80 &&
        (p[2] & 0xC0) == 0x80 && (p[3] & 0xC0) == 0x80) {
        *cp = (uint32_t)((p[0] & 0x07) << 18) | (uint32_t)((p[1] & 0x3F) << 12)
            | (uint32_t)((p[2] & 0x3F) << 6) | (p[3] & 0x3F);
        return 4;
    }
    *cp = 0xFFFD;
    return 0;
}

int gkb_vocab_bytes(int32_t vocab, int32_t merges)
{
    return (int)sizeof(GkbVocab)
         + vocab * (int)sizeof(GkbTok)          /* token 描述符 */
         + vocab * 24                            /* token 字节内容 */
         + merges * (int)sizeof(GkbMerge);
}

int nnet_vocab_size(void) { return g_loaded ? g_vocab.vocab : 0; }
int nnet_bos_id(void)     { return g_loaded ? g_vocab.bos : 1; }
int nnet_eos_id(void)     { return g_loaded ? g_vocab.eos : 2; }
int nnet_sep_id(void)     { return g_loaded ? g_vocab.sep : 3; }

/* --------------------------------------------------------------------------
 * GVB1 文本解析（游标式，不用 stdio）
 *
 * 格式：
 *   GVB1
 *   <vocab> <n_merge> <bos> <eos> <sep>
 *   每行一个 token：<字节数> <十六进制字节，两位一组，**无分隔无 \x 前缀**>
 *   然后 n_merge 行：<a_id> <b_id>
 *
 * 为什么不用 fscanf/fgetc 逐段读（早先的写法）：
 *   1. 导出端写的是 bytes.hex()，也就是**裸 hex**，不带反斜杠。
 *      而老代码每字节期望 "\\" + "x" + 两个 hex 位，一读到 '3' 就当成
 *      可打印 ASCII 存进去 —— 整个词表的内容全是错的，而且不报错。
 *   2. 长度上限写死 23，而实测最长 token 有 144 字节（未合并的整句片段），
 *      加载会在中途 return -3。
 *   3. fscanf 的 %d 会吃掉空白与换行，CRLF 文件上虽然碰巧能work，
 *      但混在 fgetc 的字节读取里就没有一致性可言了。
 *
 * 现在统一按"游标 + 跳过空白 + 自己认 token"走，三种问题一起消失。
 * ----------------------------------------------------------------------- */
typedef struct {
    const char *p;
    const char *end;
} Cur;

static void cur_init(Cur *c, const char *p, int len)
{
    c->p = p;
    c->end = p + len;
}

/* 跳过空白（含 CRLF）。到头返回 0。 */
static void cur_ws(Cur *c)
{
    while (c->p < c->end) {
        char ch = *c->p;
        if (ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n')
            ++c->p;
        else
            break;
    }
}

/* 读一个十进制非负整数。到头或遇非数字返回 -1。 */
static int cur_int(Cur *c)
{
    int v = 0, any = 0;

    cur_ws(c);
    while (c->p < c->end && *c->p >= '0' && *c->p <= '9') {
        /* 溢出保护：解析失败时返回 -1 而不是绕回负数，
         * 否则"文件损坏"会表现成"词表大小 = 某个离谱的值"。 */
        if (v > 0x00FFFFFF) return -1;
        v = v * 10 + (*c->p - '0');
        ++c->p;
        any = 1;
    }
    return any ? v : -1;
}

int gkb_vocab_load_mem(const char *src, int src_len,
                       GkbVocab *dst, void *work, int work_sz)
{
    Cur c;
    char magic[4];
    int32_t hdr[5];
    int32_t vocab, n_merge;

    if (!src || !dst || !work || src_len < 8) return -1;
    if (work_sz < gkb_vocab_bytes(4096, 4096)) return -1;

    cur_init(&c, src, src_len);

    /* magic：跳过前导空白后应当正好是 "GVB1" */
    cur_ws(&c);
    if (c.end - c.p < 4) return -3;
    memcpy(magic, c.p, 4);
    if (memcmp(magic, "GVB1", 4) != 0) return -3;
    c.p += 4;

    for (int i = 0; i < 5; ++i) {
        hdr[i] = cur_int(&c);
        if (hdr[i] < 0) return -3;
    }
    vocab   = hdr[0];
    n_merge = hdr[1];
    if (vocab <= 0 || vocab > 65536) return -3;
    /* merge 下标是 int16，范围外的直接拒掉（否则存进去会静默变号） */
    if (n_merge < 0 || n_merge > 32767) return -3;

    dst->vocab   = vocab;
    dst->n_merge = n_merge;
    dst->bos     = hdr[2];
    dst->eos     = hdr[3];
    dst->sep     = hdr[4];

    /* 分配：描述符 / 字节堆 / merge 表，全部落在 work 里 */
    char *q = (char *)work;
    q += (4 - ((uintptr_t)q & 3)) & 3;
    GkbTok *toks = (GkbTok *)q;
    q += (size_t)vocab * sizeof(GkbTok);

    /* 字节堆紧跟在描述符后面。多留的量取决于导出端给了多少 token 字节，
     * 所以这里把「剩余工作区」全部划给堆，而不是按 vocab*24 猜 ——
     * 猜小了会截断，截断的 token 会让 BPE 编出错误的 id。 */
    const char *mg_at = q;
    q += (size_t)n_merge * sizeof(GkbMerge);
    size_t heap_cap = (size_t)(work + work_sz) - (size_t)q;
    uint8_t *heap = (uint8_t *)q;

    size_t hpos = 0;
    for (int32_t i = 0; i < vocab; i++) {
        int len = cur_int(&c);
        if (len < 0) return -3;
        /* GkbTok.len 是 uint8_t，长度上限就是 255；实测最长 144。 */
        if (len > 255) return -3;
        if (hpos + (size_t)len > heap_cap) return -4;   /* 堆不够 */

        /* 长度与 hex 之间有一个分隔空格，而 hex 本身是连续的
         * （"3 e883bd"，不是"3 e8 83 bd"）。所以这里跳过空白后
         * 一次读到底 —— 漏掉这个跳空白，第一个 token 就会把那个
         * 空格当成 hex 位读，解析结果全错且不报错。 */
        cur_ws(&c);
        for (int k = 0; k < len; k++) {
            int hi = hexval(c.p[0]);
            int lo = hexval(c.p[1]);
            if (hi < 0 || lo < 0) return -3;
            heap[hpos + (size_t)k] = (uint8_t)((hi << 4) | lo);
            c.p += 2;
        }
        toks[i].off = (uint16_t)hpos;
        toks[i].len = (uint8_t)len;
        hpos += (size_t)len;
    }

    GkbMerge *mg = (GkbMerge *)mg_at;
    for (int32_t i = 0; i < n_merge; i++) {
        int a = cur_int(&c);
        int b = cur_int(&c);
        if (a < 0 || b < 0) return -3;
        if (a > 32767 || b > 32767) return -3;
        mg[i].a = (int16_t)a;
        mg[i].b = (int16_t)b;
    }

    dst->toks   = toks;
    dst->heap   = heap;
    dst->merges = (n_merge > 0) ? mg : NULL;
    g_vocab = *dst;
    g_loaded = 1;
    return 0;
}

/* 从 SD 卡加载：读进内存再走同一个解析器。
 * 不去逐段 fscanf 是为了只维护**一份**格式实现 —— 早先这两条路径各写
 * 一遍，格式一改就只改一半，另一半静默解析错。 */
int gkb_vocab_load(const char *path, GkbVocab *dst, void *work, int work_sz)
{
    FILE   *fp;
    long    sz;
    char   *buf;
    int     rc;

    if (!path || !dst || !work) return -1;
    if (work_sz < gkb_vocab_bytes(4096, 4096)) return -1;

    fp = fopen(path, "rb");
    if (!fp) return -2;

    if (fseek(fp, 0, SEEK_END) != 0) { fclose(fp); return -2; }
    sz = ftell(fp);
    /* 词表正常是 100KB 量级，给 8MB 上限：读到 absurd 大小的文件时
     * 直接拒绝，而不是拿 malloc 去赌 SD 卡里是不是有个坏块。 */
    if (sz <= 8 || sz > (8 << 20)) { fclose(fp); return -2; }
    rewind(fp);

    buf = (char *)malloc((size_t)sz);
    if (!buf) { fclose(fp); return -1; }

    rc = (int)fread(buf, 1, (size_t)sz, fp);
    fclose(fp);
    if (rc != (int)sz) { free(buf); return -3; }

    rc = gkb_vocab_load_mem(buf, (int)sz, dst, work, work_sz);
    free(buf);
    return rc;
}

/* 内嵌的那一份（app/data/guide_vocab.txt → guide_vocab_bin.o）。 */
int gkb_vocab_load_embedded(GkbVocab *dst, void *work, int work_sz)
{
    return gkb_vocab_load_mem((const char *)guide_vocab_txt,
                              (int)guide_vocab_txt_size,
                              dst, work, work_sz);
}

/* UTF-8 归一化：全角转半角（与训练端 norm_text 保持一致） */
static int norm_char(const char **pp, char *out)
{
    const unsigned char *p = (const unsigned char *)*pp;
    uint32_t cp;
    int n = utf8_decode(p, &cp);
    if (n <= 0) { *pp = (const char *)(p + 1); out[0] = '?'; return 1; }
    *pp += n;

    if (cp == 0x3000) { out[0] = ' '; return 1; }              /* 全角空格 */
    if (cp >= 0xFF01 && cp <= 0xFF5E) { out[0] = (char)(cp - 0xFEE0); return 1; }

    memcpy(out, p - n, (size_t)n);
    return n;
}

/*
 * 对一段「词」（按空格切出的块）做 BPE merge。
 * 与训练端 train_bpe.py 的 _bpe_word 完全同构：
 *   初始每个字节一个 id，按 merge rank 从小到大贪心合并。
 */
static int bpe_word(const uint8_t *w, int n, int32_t *out, int max_out)
{
    if (n == 0) return 0;
    if (n == 1) { out[0] = w[0]; return 1; }

    int32_t ids[64];
    if (n > 63) n = 63;              /* 超长词直接截断（语料里不会出现） */
    for (int i = 0; i < n; i++) ids[i] = w[i];

    int cnt = n;
    while (cnt > 1) {
        int best = -1, pos = -1;
        for (int i = 0; i < cnt - 1; i++) {
            for (int32_t m = 0; m < g_vocab.n_merge; m++) {
                if (g_vocab.merges[m].a == ids[i] &&
                    g_vocab.merges[m].b == ids[i + 1]) {
                    if (best < 0 || m < best) { best = m; pos = i; }
                    break;
                }
            }
        }
        if (best < 0) break;
        /* 合并 ids[pos], ids[pos+1] -> 新 id */
        int32_t merged = -1;
        for (int32_t i = 0; i < g_vocab.vocab; i++) {
            GkbTok *t = &g_vocab.toks[i];
            if (t->len != g_vocab.toks[ids[pos]].len + g_vocab.toks[ids[pos + 1]].len)
                continue;
            if (memcmp(g_vocab.heap + t->off,
                       g_vocab.heap + g_vocab.toks[ids[pos]].off,
                       g_vocab.toks[ids[pos]].len) != 0) continue;
            if (memcmp(g_vocab.heap + t->off + g_vocab.toks[ids[pos]].len,
                       g_vocab.heap + g_vocab.toks[ids[pos + 1]].off,
                       g_vocab.toks[ids[pos + 1]].len) != 0) continue;
            merged = i; break;
        }
        if (merged < 0) break;
        ids[pos] = merged;
        for (int i = pos + 1; i < cnt - 1; i++) ids[i] = ids[i + 1];
        cnt--;
    }
    if (cnt > max_out) cnt = max_out;
    memcpy(out, ids, (size_t)cnt * sizeof(int32_t));
    return cnt;
}

int nnet_text_encode(const char *utf8, int32_t *out, int max_out)
{
    if (!g_loaded) return -1;
    const char *p = utf8;
    int n = 0;
    while (*p && n < max_out) {
        /* 取一个空格分隔的块 */
        char buf[256];
        int len = 0;
        /* ★ 守卫必须留够 **4 字节**余量，不能只写 len < 255。
         * norm_char 一次可写 1~4 字节（UTF-8 四字节序列，标点/emoji 会中）：
         * 当 len==253 遇 3 字节汉字 → 写 buf[253..255]，len 变 256；
         * 当 len==254 遇 4 字节 → 写 buf[254..257]，**越界 2 字节**。
         * 中文本就不空格，所以用户问一句长中文（>253 字节）就能触发。
         * 3DS 无 MMU、无栈保护，这种越界是静默踩栈，不会当场崩。
         * 判据写成 len + 4 <= sizeof buf 才是"这一次写一定放得下"。 */
        while (*p && *p != ' ' && len + 4 <= (int)sizeof buf) {
            len += norm_char(&p, buf + len);
        }
        if (*p == ' ') p++;
        if (len == 0) continue;
        if (n < max_out) n += bpe_word((const uint8_t *)buf, len, out + n, max_out - n);
    }
    return n;
}

/* 取某个 token id 的 UTF-8 文本。返回长度，id 越界返回 0。
 * guide_kb.c 的停用词判定需要它（要在 token 层面判，
 * 因为 BPE 会把「在哪里」粘成一个 token，按空格切词看不到）。 */
int nnet_vocab_piece(int32_t id, const uint8_t **out)
{
    if (!g_loaded || id < 0 || id >= g_vocab.vocab) return 0;
    GkbTok *t = &g_vocab.toks[id];
    if (out) *out = g_vocab.heap + t->off;
    return t->len;
}

int nnet_text_decode(const int32_t *ids, int n, char *out, int out_sz)
{
    if (!g_loaded) return -1;
    int w = 0;
    for (int i = 0; i < n; i++) {
        int32_t id = ids[i];
        if (id < 0 || id >= g_vocab.vocab) continue;
        GkbTok *t = &g_vocab.toks[id];
        if (w + t->len >= out_sz) break;
        memcpy(out + w, g_vocab.heap + t->off, t->len);
        w += t->len;
    }
    out[w] = 0;
    return w;
}
