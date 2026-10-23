/*
 * ime.c -- 拼音输入法引擎（格式与策略见 ime.h）
 *
 * 表格式（gen_pinyin_table.py 生成，全部小端）：
 *     u16 n_syl
 *     n_syl × { char name[8]; u32 off; u16 n; }   // 14 字节/条
 *     pool: UTF-8 汉字连续排列，每个 3 字节
 *
 * ★ 全程**零动态分配**：3DS 上没有 MMU，堆碎片是长期跑之后的隐患，
 *   而这张表是只读常量，静态缓冲足够。
 */

#include <stdio.h>
#include <string.h>

#include "ime.h"
#include "pinyin_tbl_bin.h"      /* build.py/bin2s 生成：pinyin_tbl_bin[] / _size */

#define ENTRY_SZ   14
#define NAME_SZ     8
#define MAX_CAND   16            /* 与生成脚本的 MAX_PER_SYL 对齐 */
#define CAND_SZ    16            /* 4 个汉字 × 3 字节 + NUL，够用 */

static char     s_in[IME_INPUT_MAX + 1];
static int      s_len;
static int      s_n_cand;
static char     s_cand[MAX_CAND][CAND_SZ];
static char     s_tmp[CAND_SZ];

/* ---- 表访问 ---- */

static int syl_n(void)
{
    if (pinyin_tbl_bin_size < 2)
        return 0;
    return (int)(pinyin_tbl_bin[0] | (pinyin_tbl_bin[1] << 8));
}

static const unsigned char *pool_base(void)
{
    return pinyin_tbl_bin + 2 + (size_t)syl_n() * ENTRY_SZ;
}

static const unsigned char *entry_at(int i)
{
    return pinyin_tbl_bin + 2 + (size_t)i * ENTRY_SZ;
}

/* 在 s 的前 len 个字节精确匹配一条音节名。返回下标，没有返回 -1。 */
static int find_syl(const char *s, int len)
{
    int n = syl_n(), i;

    for (i = 0; i < n; ++i) {
        const unsigned char *e = entry_at(i);
        /* 名是 NUL 补齐的定长字段，先比长度再看字节 */
        int l = 0;
        while (l < NAME_SZ && e[l] != 0)
            ++l;
        if (l != len)
            continue;
        if (memcmp(e, s, (size_t)len) == 0)
            return i;
    }
    return -1;
}

static const unsigned char *syl_chars(int idx, int *nchars)
{
    const unsigned char *e = entry_at(idx);
    unsigned int off;

    memcpy(&off, e + NAME_SZ, 4);            /* ★ 不直接转指针：表是字节数组，
                                              * 某些 ARM 配置下未对齐读会异常 */
    *nchars = (int)(e[NAME_SZ + 4] | (e[NAME_SZ + 5] << 8));
    return pool_base() + off;
}

bool ime_ready(void)
{
    return syl_n() > 0 && pinyin_tbl_bin_size > 2;
}

/* ---- 输入缓冲 ---- */

void ime_reset(void)
{
    s_in[0] = '\0';
    s_len = 0;
    s_n_cand = 0;
}

bool ime_push(char c)
{
    if (!ime_ready())
        return false;
    if (c >= 'A' && c <= 'Z')
        c = (char)(c - 'A' + 'a');
    if (c < 'a' || c > 'z')
        return false;
    if (s_len >= IME_INPUT_MAX)
        return false;

    s_in[s_len++] = c;
    s_in[s_len] = '\0';
    return true;
}

void ime_backspace(void)
{
    if (s_len > 0)
        s_in[--s_len] = '\0';
}

const char *ime_input(void)
{
    return s_in;
}

/* ---- 切分 + 候选 ---- */

/* 返回切出来的音节数；任一段匹配不上就返回 0（整体无候选）。
 * 贪婪：每次从最长可能的长度往下试。 */
static int split(const char *s, int len, int *idx, int max_seg)
{
    int p = 0, n = 0;

    while (p < len) {
        int try_len, hit = -1;

        if (n >= max_seg)
            return 0;

        for (try_len = (len - p > 6 ? 6 : len - p); try_len >= 1; --try_len) {
            int k = find_syl(s + p, try_len);
            if (k >= 0) {
                hit = k;
                p += try_len;
                break;
            }
        }
        if (hit < 0)
            return 0;
        idx[n++] = hit;
    }
    return n;
}

static void build_cands(void)
{
    int idx[IME_SEG_MAX];
    int nseg, i;

    s_n_cand = 0;
    if (s_len == 0 || !ime_ready())
        return;

    nseg = split(s_in, s_len, idx, IME_SEG_MAX);
    if (nseg <= 0)
        return;

    if (nseg == 1) {
        int nc = 0;
        const unsigned char *ch = syl_chars(idx[0], &nc);
        for (i = 0; i < nc && s_n_cand < MAX_CAND; ++i) {
            memcpy(s_cand[s_n_cand], ch + (size_t)i * 3, 3);
            s_cand[s_n_cand][3] = '\0';
            ++s_n_cand;
        }
        return;
    }

    /* ---- 双音节：按"排名和"枚举 ----------------------------------------
     * ★ 早先这里只取各段首选字再换末字，实测**不够用**：
     *     diannao → 点脑（"电"在 dian 里排第 4，永远当不上前缀）
     *     youxi   → 有系（"游"同理）
     *     zhongwen→ 重稳
     *   也就是说"只改末字"这个策略，在首字不是最高频时会给出一个没人用的词。
     *
     * 现在枚举 (i, j) 并按 i+j 从小到大取 —— 两段都靠前的组合先出现，
     * "电"(4)+"脑"(1) 的和是 5，仍能挤进前 16 个，而"点"(0)+"垫"(1) 自然更靠前。
     * 只做双音节：三音节以上的组合数增长太快，而中文输入绝大多数是 1~2 字。 */
    if (nseg == 2) {
        int nc0 = 0, nc1 = 0;
        const unsigned char *c0 = syl_chars(idx[0], &nc0);
        const unsigned char *c1 = syl_chars(idx[1], &nc1);

        for (int sum = 0; sum <= 8 && s_n_cand < MAX_CAND; ++sum) {
            for (int i2 = 0; i2 <= sum && s_n_cand < MAX_CAND; ++i2) {
                int j2 = sum - i2;
                if (i2 >= nc0 || j2 >= nc1)
                    continue;
                snprintf(s_cand[s_n_cand], CAND_SZ, "%.3s%.3s",
                         (const char *)(c0 + (size_t)i2 * 3),
                         (const char *)(c1 + (size_t)j2 * 3));
                ++s_n_cand;
            }
        }
        return;
    }

    /* ---- 三音节以上：前缀取各段首选字，末段逐个换字 ---------------------
     * 组合数太大，这里只做"首字定死、改末字"。用户真要在三字词里改中间那个，
     * 逐音节输入即可（提交一个字后缓冲会清空）。 */
    {
        char prefix[CAND_SZ];
        int plen = 0;
        int nc = 0;
        const unsigned char *ch;

        for (i = 0; i < nseg - 1; ++i) {
            int k;
            const unsigned char *pc = syl_chars(idx[i], &k);
            if (plen + 3 >= (int)sizeof prefix)
                break;
            memcpy(prefix + plen, pc, 3);        /* 该音节首选字 */
            plen += 3;
        }
        prefix[plen] = '\0';

        ch = syl_chars(idx[nseg - 1], &nc);
        for (i = 0; i < nc && s_n_cand < MAX_CAND; ++i) {
            snprintf(s_cand[s_n_cand], CAND_SZ, "%s%.3s",
                     prefix, (const char *)(ch + (size_t)i * 3));
            ++s_n_cand;
        }
    }
}

int ime_cand_count(void)
{
    build_cands();
    return s_n_cand;
}

const char *ime_cand(int i)
{
    if (i < 0 || i >= s_n_cand)
        return "";
    return s_cand[i];
}

bool ime_commit(int i, char *out, size_t cap)
{
    if (i < 0 || i >= s_n_cand || !out || cap == 0)
        return false;

    snprintf(out, cap, "%s", s_cand[i]);
    ime_reset();
    return true;
}
