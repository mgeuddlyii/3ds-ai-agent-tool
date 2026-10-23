/*
 * exttest.c — 抽取式回答的真机验收
 *
 * ══ 为什么必须在真机/模拟器上验，而不能只靠 PC 侧 ═════════════════
 *
 * 我在 C 端犯过两个**只有实跑才暴露**的错，而 gcc 只报 warning：
 *
 *   ① `if (c == '。')` —— C 的多字节字符常量是 int（0x3002），
 *      和 char（8 位）比**恒为假** → 切不出任何句子，n_cand 恒 0。
 *      gcc 报 `-Wmultichar` 但**不是错误**。
 *   ② `out[w++] = '。'` —— 0x3002 赋给 char 只剩 0x82，
 *      输出里多一个**孤立续字节**（屏上是乱码）。
 *
 * 这两个都是「编译通过、逻辑全错」。所以判据必须是**输出文本**，
 * 不是 rc、不是能不能编过。
 *
 * 本趟验三件事：
 *   ① 切得出句子（n_sent > 0）
 *   ② 每条用例的输出**含答案关键词**（这才是"能用"）
 *   ③ 输出是**合法 UTF-8**（接上一个孤立的 0x82 就会在这里露出来）
 */
#include "exttest.h"

#include "app.h"
#include "extract.h"
#include "guide_kb.h"
#include "guide_nn.h"
#include "nnet_text.h"

extern void (*g_ext_dbg)(const char *s);

#include <3ds.h>
#include <string.h>

#define MODEL_PATH "sdmc:/3ds/aiconsole/guide_model.bin"

static uint8_t g_vocab_work[224 * 1024];

/* 诊断缓冲：collect.c 的拼接每步调一次 */
#define EXT_DBG_MAX 64
static char g_ext_dbg_buf[EXT_DBG_MAX][40];
static int  ext_dbg_n;
static void ext_dbg_fn(const char *s)
{
    if (ext_dbg_n < EXT_DBG_MAX) {
        int i = 0;
        while (s[i] && i < 39) { g_ext_dbg_buf[ext_dbg_n][i] = s[i]; i++; }
        g_ext_dbg_buf[ext_dbg_n][i] = 0;
        ext_dbg_n++;
    }
}

typedef struct {
    const char *q;
    const char *kw;        /* 答案里应该出现的词；NULL = 不判内容 */
    int         exp_game;  /* 期望的游戏；-1 = 不限定 */
} Case;

static const Case CASES[] = {
    { "火焰纹章觉醒 转职怎么转",   "转职",   2 },
    { "时之笛 钩索箭哪里拿",       "钩索箭", 30 },
    { "怪物猎人4G 弱点是什么",     "弱点",   11 },
    { "路易吉洋馆2 吸尘器怎么用", "吸尘器", 7 },
    { "马力欧卡丁车7 漂移技巧",   "漂移",   13 },
    { "大师之剑在哪里拿",         "大师之剑", -1 },
    { "沙之杖怎么用",             "沙之杖", -1 },
    { "梅祖拉的假面 三天循环怎么过", "循环", 29 },
};
#define N_CASE ((int)(sizeof(CASES) / sizeof(CASES[0])))

/*
 * UTF-8 合法性检查。判据只查两件最容易出错的事：
 *   ① 续字节（10xxxxxx）不能出现在**首位**
 *   ② 3 字节序列必须恰好 3 字节长（不截断在中间）
 * 完整校验要查码点范围，而这里要抓的是"孤立的续字节"那种错 ——
 * 那是 ①② 的必然结果。
 */
static int utf8_ok(const char *s, int len)
{
    int i = 0;
    while (i < len) {
        unsigned char c = (unsigned char)s[i];
        int need;
        if (c < 0x80)            { i += 1; continue; }
        else if ((c & 0xE0) == 0xC0) need = 1;
        else if ((c & 0xF0) == 0xE0) need = 2;
        else return 0;                       /* 0x80..0xBF 在首位 = 续字节落单 */

        for (int k = 1; k <= need; k++) {
            if (i + k >= len) return 0;       /* 序列被截断 */
            unsigned char d = (unsigned char)s[i + k];
            if ((d & 0xC0) != 0x80) return 0; /* 不是续字节 */
        }
        i += need + 1;
    }
    return 1;
}

void exttest_run(void)
{
    GkbVocab vocab;
    Gkb     kb;
    GkbHit  hits[GKB_HIT];
    char    rest[128];
    char    out[768];
    int     n_ok = 0, n_bad = 0, n_skip = 0, rc;

    app_note("exttest: ===== 开始 =====");
    g_ext_dbg = ext_dbg_fn;

    /* 词表必须先加载 —— 检索走 BPE，而没有词表编码不出任何 token，
     * 症状是"检索 0 命中"（不是错误，只是检索不到）。 */
    rc = gkb_vocab_load_embedded(&vocab, g_vocab_work,
                                 (int)sizeof g_vocab_work);
    if (rc != 0) {
        app_note("exttest: 词表加载失败 rc=%d（检索必然 0 命中）", rc);
        app_note("exttest: RESULT=SKIP");
        app_note("exttest: ===== 结束 =====");
        return;
    }
    app_note("exttest: 词表 ok v=%d merges=%d",
             (int)vocab.vocab, (int)vocab.n_merge);

    rc = gkb_bind(&kb);
    if (rc <= 0) {
        app_note("exttest: 知识库绑定失败 rc=%d", rc);
        app_note("exttest: RESULT=FAIL");
        app_note("exttest: ===== 结束 =====");
        return;
    }
    app_note("exttest: 索引 ok 段落=%d 游戏=%d", (int)kb.n_passage,
             (int)kb.n_game);

    for (int i = 0; i < N_CASE; i++) {
        int gid, n_hit, w;
        int split_gid;

        /* ⚠️ 顺序不能反，而且**必须检查返回值**。
         *
         * gkb_split_game 的契约：返回 game_id；**返回 -1 时 out 原封不动
         * 就是 query 本身**（它只在认出游戏时才做剔除）。
         * 我第一版写成
         *     gid = exp>=0 ? exp : gkb_split_game(q, rest, cap);
         * 于是 exp<0 的用例拿到 gid=-1 且 rest 仍是 query ——
         * 看起来对，但 exp>=0 的用例走 else 分支，rest 才被填。
         * 结果：**3 条"不限定游戏"的用例 rest 是未初始化栈垃圾**，
         * 症状是「检索 0 命中」，而原因在测试代码里。
         *
         * 正确写法：先无条件拆一次拿 rest（不管认不认得出），
         * 再决定用哪个 gid。
         */
        split_gid = gkb_split_game(CASES[i].q, rest, (int)sizeof rest);
        if (split_gid < 0) {
            /* 没认出游戏：rest 就是原问句，按"全部游戏"检索 */
            gid = -1;
        } else {
            gid = CASES[i].exp_game >= 0 ? CASES[i].exp_game : split_gid;
        }
        if (rest[0] == '\0') {
            app_note("exttest: [%d] %s  FAIL 问句剔除后变成空串", i,
                     CASES[i].q);
            n_bad++;
            continue;
        }

        n_hit = gkb_search(&kb, rest, gid, hits, GKB_HIT);
        if (n_hit <= 0) {
            app_note("exttest: [%d] %s  检索 0 命中  gid=%d rest=[%s]",
                     i, CASES[i].q, gid, rest);
            n_skip++;
            continue;
        }

        ext_dbg_n = 0;
        out[0] = '\0';
        w = gkb_extract(&kb, hits, n_hit, rest, out, (int)sizeof out);

        if (w <= 0) {
            app_note("exttest: [%d] %s  FAIL 切不出句子（n_hit=%d）",
                     i, CASES[i].q, n_hit);
            app_note("exttest:   症状多半是 multi-character constant："
                     "'。' 在 C 里是 0x3002，与 char 比恒为假");
            n_bad++;
            continue;
        }
        if (!utf8_ok(out, w)) {
            app_note("exttest: [%d] %s  FAIL 输出不是合法 UTF-8（%d 字节）",
                     i, CASES[i].q, w);
            for (int z = 0; z < ext_dbg_n && z < 8; z++)
                app_note("exttest:   step%d %s", z, g_ext_dbg_buf[z]);
            {   /* 定位：打印第一个坏字节的位置与附近的字节
                 * 只在诊断时用，正式版可留（它便宜且只在失败时跑） */
                int bad = -1;
                for (int b = 0; b < w; b++) {
                    unsigned char u = (unsigned char)out[b];
                    if (u < 0x80) continue;
                    if ((u & 0xE0) == 0xC0) { if (b+1>=w || ((unsigned char)out[b+1]&0xC0)!=0x80) { bad=b; break; } b+=1; }
                    else if ((u & 0xF0) == 0xE0) { if (b+2>=w || ((unsigned char)out[b+1]&0xC0)!=0x80 || ((unsigned char)out[b+2]&0xC0)!=0x80) { bad=b; break; } b+=2; }
                    else { bad=b; break; }
                }
                app_note("exttest:   坏字节在 %d，前 4 字节 %02x %02x %02x %02x",
                         bad,
                         bad>=1 ? (unsigned char)out[bad-1] : 0,
                         bad>=0 ? (unsigned char)out[bad] : 0,
                         bad+1<w ? (unsigned char)out[bad+1] : 0,
                         bad+2<w ? (unsigned char)out[bad+2] : 0);
                {
                    char hexs[160];
                    int  q3 = 0;
                    for (int z = 0; z < 48 && z < w; z++) {
                        unsigned u3 = (unsigned char)out[z];
                        hexs[q3++] = "0123456789abcdef"[u3 >> 4];
                        hexs[q3++] = "0123456789abcdef"[u3 & 15];
                        hexs[q3++] = ' ';
                    }
                    hexs[q3] = 0;
                    app_note("exttest:   hex[0..48]: %s", hexs);
                }
            }
            n_bad++;
            continue;
        }

        if (CASES[i].kw && !strstr(out, CASES[i].kw)) {
            app_note("exttest: [%d] %s  FAIL 答案里没有「%s」", i,
                     CASES[i].q, CASES[i].kw);
            app_note("exttest:   实得：%s", out);
            n_bad++;
            continue;
        }

        app_note("exttest: [%d] %s  OK  %d 字节  %s", i, CASES[i].q, w, out);
        n_ok++;
    }

    app_note("exttest: 汇总 OK=%d FAIL=%d 跳过=%d / 共 %d", n_ok, n_bad,
             n_skip, N_CASE);
    app_note("exttest: RESULT=%s", n_bad ? "FAIL" : "OK");
    app_note("exttest: ===== 结束 =====");
}
