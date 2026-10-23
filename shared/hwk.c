/*
 * hwk.c — 见 hwk.h
 *
 * 数据源两条，与 guide_kb 同构：SD 卡优先、失败静默回退编译期内嵌。
 * 差别在于**这份是纯文本**（TF 问答），不压缩也不解包，
 * 所以没有"两种封装"的问题，只有一个文本缓冲。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hwk.h"

/*
 * 编译期内嵌的那份：app/data/hw_knowledge.txt 走 bin2s 进 build/。
 *
 * ⚠️ **必须 include 那个头，不能手写 extern。**
 * bin2s 把长度做成 `static const size_t xxx_size`（不是符号），
 * 手写 `extern const unsigned int hw_knowledge_txt_size;` 编译得过，
 * 链接时才报 undefined reference —— 而报错信息里**不会**提到
 * "你该 include 头文件"，只说符号不存在，很容易往错方向查。
 * 参照 shared/nnet/nnet_text.c 对 guide_vocab_bin.h 的做法。
 *
 * 符号名的坑：头文件叫 hw_knowledge_bin.h（来自 -H 参数），
 * 但数组叫 **hw_knowledge_txt**（来自源文件名，含扩展名）。
 * 照着头文件名去 extern 数组，一样链接失败。
 */
#include "hw_knowledge_bin.h"   /* hw_knowledge_txt[] / hw_knowledge_txt_size */

#define HWK_SD_PATH "sdmc:/3ds/aiconsole/hw_knowledge.txt"

/* ---------------- 状态 ----------------
 *
 * 静态数组而不是动态链表：条目定长上限 512，答案最长 768，
 * 最坏 512*(96+768) ≈ 440KB 放堆上太大，而**实际用到的只有 25 条**。
 * 折中：问句和答案都指向同一个文本缓冲（不拷贝），条目数组只存
 * 偏移和长度 —— 那是 512*8 = 4KB 静态区，完全可接受。
 */
typedef struct {
    int  ask_off, ask_len;    /* 问句在 buf 里的字节范围 */
    int  ans_off, ans_len;    /* 同上 */
} HwkEntry;

static char           *g_buf;        /* 整份文本（已把 CRLF 归一为 LF） */
static int             g_buf_len;
static HwkEntry        g_ent[HWK_MAX_ENTRY];
static int             g_n;
static bool            g_ready;
static int             g_from_sd;

/* 解析。
 *
 * 行格式：`问<TAB>答`，答案里允许有换行（用空白行分隔条目），
 * 但**单行内不能换行** —— 这条约束让解析可以极简：
 * 找行首非 '#' 且含 TAB 的行，答案是同行 TAB 之后的全部。
 *
 * 跳过的行：空行、以 '#' 开头的注释行、TAB 前为空的行。
 * 最后一行的行尾没有 '\n'，所以循环条件是「还有字符」，不是「还有换行」。
 */
static int hwk_parse(char *text, int len)
{
    int i = 0, n = 0;

    while (i < len && n < HWK_MAX_ENTRY) {
        int ls, le, tab;

        /* 取一行的 [ls, le)（不含行尾的 '\n'） */
        ls = i;
        while (i < len && text[i] != '\n')
            i++;
        le = i;
        if (i < len) i++;                    /* 吃掉 '\n' */

        /* 兼容 CRLF：Windows 上编辑的文本可能有 \r。
         * 不处理的话答案末尾会多一个 0x0D，屏上显示成一个空方块，
         * 而且 sha256 校验会把它算进去（打包侧与本侧口径必须一致）。 */
        if (le > ls && text[le - 1] == '\r')
            le--;

        if (le <= ls) continue;              /* 空行 */
        if (text[ls] == '#') continue;        /* 注释 */

        tab = -1;
        {
            int k;
            for (k = ls; k < le; k++) {
                if (text[k] == '\t') { tab = k; break; }
            }
        }
        /* 没有 TAB 的行不是条目。但也不能当错误：
         * 文件头部的说明文字就是这种行。 */
        if (tab < 0 || tab == ls) continue;

        /* 问句/答案末尾的空白去掉。留着的话覆盖率会算上无意义的空格。 */
        {
            int ae = le;
            while (ae > tab + 1 && (text[ae - 1] == ' ' || text[ae - 1] == '\t'))
                ae--;
            if (ae <= tab + 1) continue;     /* 答案为空 */
            if (tab - ls >= HWK_MAX_ASK) continue;  /* 问句超长，畸形 */

            g_ent[n].ask_off = ls;
            g_ent[n].ask_len = tab - ls;
            g_ent[n].ans_off = tab + 1;
            g_ent[n].ans_len = ae - (tab + 1);
            n++;
        }
    }
    return n;
}

/* 读一份完整的文件。成功返回 malloc 的缓冲，*out_len 为长度。 */
static char *hwk_read_file(const char *path, int *out_len)
{
    FILE *f = fopen(path, "rb");
    long sz;
    char *buf;
    size_t got;

    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    sz = ftell(f);
    /* 上限 256KB：这份东西是人写的知识条目，不是数据文件。
     * 万一 SD 卡上放了个别的东西进来，也不该让 app 吃一整块堆。 */
    if (sz <= 0 || sz > (256L << 10)) { fclose(f); return NULL; }
    rewind(f);

    buf = (char *)malloc((size_t)sz + 1);
    if (!buf) { fclose(f); return NULL; }
    got = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    if (got != (size_t)sz) { free(buf); return NULL; }

    buf[sz] = '\0';
    *out_len = (int)sz;
    return buf;
}

void hwk_free(void)
{
    if (g_buf) { free(g_buf); g_buf = NULL; }
    g_buf_len = 0;
    g_n = 0;
    g_ready = false;
    g_from_sd = 0;
}

int hwk_load(void)
{
    char *text;
    int   len = 0;
    int   n;

    /* 幂等：先把上一次的放掉。不放的话第二次调用会泄漏，
     * 而"进攻略页→退→再进"在用户手里是很常见的动作。 */
    hwk_free();

    text = hwk_read_file(HWK_SD_PATH, &len);
    if (text) {
        g_from_sd = 1;
    } else {
        /* 回退内嵌。这份内嵌是编译期常量，不需要也不能释放，
         * 所以要拷一份出来 —— 解析阶段不修改内容，拷只是为了
         * 让两条来源走完全相同的代码路径（否则 CRLF 归一那步
         * 会在其中一条路上编译都没发现地不存在）。 */
        if (hw_knowledge_txt_size == 0 ||
            hw_knowledge_txt_size > (256u << 10))
            return -1;
        len = (int)hw_knowledge_txt_size;
        text = (char *)malloc((size_t)len + 1);
        if (!text) return -1;
        memcpy(text, hw_knowledge_txt, (size_t)len);
        text[len] = '\0';
        g_from_sd = 0;
    }

    n = hwk_parse(text, len);
    if (n <= 0) {
        /* 一条也解析不出来 = 文件不是这个格式。**必须把缓冲放掉**，
         * 留着它就等于把一个"读到了但没用"的东西留在堆上，
         * 而调用方看到 -2 之后往往还会再试别的路径。 */
        free(text);
        g_from_sd = 0;
        return -2;
    }

    g_buf = text;
    g_buf_len = len;
    g_n = n;
    g_ready = true;
    return n;
}

bool hwk_ready(void)     { return g_ready; }
int  hwk_from_sd(void)   { return g_from_sd; }
const char *hwk_sd_path(void) { return HWK_SD_PATH; }

/* ---------------- bigram 覆盖率打分 ----------------
 *
 * 判据必须建立在**覆盖率**（命中比例）而不是绝对命中数上：
 * 用户问「3ds能超频吗」6 个 bigram，命中 3 个就该算 50%；
 * 而另一条 30 个 bigram 的长条目命中 3 个只有 10%。
 * 用绝对数的话，短而精准的条目永远输给长而泛的条目。
 */

/* 取第 i 个 UTF-8 字符的字节长度。
 *
 * ⚠️ 不能只看首字节 < 0x80 就当 ASCII —— 0xC0~0xDF 是 2 字节、
 * 0xE0~0xEF 是 3 字节、0xF0~0xF7 是 4 字节，都 >= 0x80。
 * 而硬件知识里混着 `804MHz`、`ptm:sysm`、`ldrsb` 这类拉丁串，
 * 里面一定有多字节字符，算错长度会让后面的比较全部错位。
 */
static int u8_len(unsigned char c)
{
    if (c < 0x80) return 1;
    if (c < 0xC0) return 1;   /* 落单的续字节：当成 1 字节推进，不解死循环 */
    if (c < 0xE0) return 2;
    if (c < 0xF0) return 3;
    return 4;
}

/*
 * 取「第 i 个字符开始、跨两个字符」的那一段（返回长度）。
 * i 每次只推进**一个**字符 —— 那才是 bigram（相邻重叠）。
 *
 * ⚠️ 这里原来写成 `i += a + b`，即每次跳**两个**字符。
 * 后果不是"慢一点"或"少算一点"，而是**判据整个失效**：
 * 那样取到的是「大师 / 之剑 / 哪里 / 拿」这种**不重叠**的切分，
 * 问句被腰斩成互相独立的片段，于是**一个**片段命中就等于
 * 1/4 的覆盖率 —— 而 1/4 = 25% > 20% 的门槛，
 * 于是「大师之剑在哪里拿」这种纯游戏问句，
 * 因为「哪里」撞上了「3ds的sd卡在**哪**个**位置**」就被判命中。
 *
 * 错在这里时**所有正向用例都还是绿的**（问句与条目措辞一致时，
 * 两种切法都能对上）—— 只有反向用例能抓住它。
 * 这也是为什么反向用例不是可选项。
 */
static int bigram_at(const char *s, int len, int i)
{
    int a = u8_len((unsigned char)s[i]);
    int b, w;

    if (i + a >= len) return 0;          /* 第二个字符不完整 */
    b = u8_len((unsigned char)s[i + a]);
    w = a + b;
    if (i + w > len) return 0;
    return w;
}

/*
 * 统计 needle 里有多少个 bigram 出现在 text 中。
 *
 * 直接嵌套比较（每对 bigram 扫一遍 text）是 O(m*n)，但
 * m、n 都很小（问句 ≤ 96 字节 ≈ 32 bigram），一次检索 25 条，
 * 最坏 32*32*25 = 25600 次字节比较 —— 在 268MHz 上是微秒级。
 * 建倒排哈希反而要维护一张表，而这张表的生命周期只有这一次调用，
 * 分配/释放的 malloc 开销比省下的那点比较还大。
 *
 * 传长度而不是靠 '\0'：buf 是连续 blob，段末没有 NUL。
 */
static int bigram_hit(const char *needle, int nlen,
                      const char *text,  int tlen)
{
    int i, hit = 0, total = 0;

    for (i = 0; i + 1 < nlen; ) {
        int w = bigram_at(needle, nlen, i);
        int k;

        if (w == 0) break;

        /* 在 text 的**字符边界**上找这 w 字节。逐字节找会误命中
         * 半个 UTF-8 字符（那三个字节恰好也在别处出现），
         * 而那意味着"命中了一个不存在的词"。 */
        for (k = 0; k + w <= tlen; k += u8_len((unsigned char)text[k])) {
            if (!memcmp(needle + i, text + k, (size_t)w)) {
                hit++;
                break;
            }
        }
        total++;
        i += u8_len((unsigned char)needle[i]);   /* 推进**一个**字符 */
    }

    return (total > 0) ? hit : 0;
}

/* bigram 的个数。判据是**数量**，不是字节数：
 * 4 字节的 ASCII 问句只有 3 个 bigram，而 12 字节的中文有 11 个 ——
 * 拿字节数当"够不够"会让短英文问句被误判成"信息太少"。 */
static int bigram_total(const char *needle, int nlen)
{
    int i, total = 0;
    for (i = 0; i + 1 < nlen; ) {
        int w = bigram_at(needle, nlen, i);
        if (w == 0) break;
        i += u8_len((unsigned char)needle[i]);
        total++;
    }
    return total;
}


/* 问句里是否**完整包含**另一串。
 *
 * ⚠️ 必须**逐字符推进**，不能逐字节。
 * 逐字节找会在 UTF-8 序列的中间命中：那 2~3 个字节恰好与别处
 * 连续相同是完全可能的（中文 UTF-8 的首字节与续接字节范围不重叠，
 * 但**续接字节之间**可能凑巧相同）。命中一次就等于
 * 「两段话在这几个字节上重合」，而那不构成"完整包含"。
 *
 * 起点也不用额外判边界：needle 自身是合法 UTF-8 序列，
 * 它的首字节不可能是续接字节（10xxxxxx），所以一旦逐字节比上了，
 * 起点必然落在字符边界上。
 */
static int contains(const char *hay, int hlen, const char *ned, int nlen)
{
    int i;
    if (nlen <= 0 || nlen > hlen) return 0;
    for (i = 0; i + nlen <= hlen; i += u8_len((unsigned char)hay[i])) {
        if (hay[i] == ned[0] && !memcmp(hay + i, ned, (size_t)nlen))
            return 1;
    }
    return 0;
}

/* 至少命中 2 个 bigram。
 *
 * 1 个 bigram 命中不构成证据。实测的反例：
 *   问「道具怎么用」-> 「3ds怎么读自己的文件」23%
 * 唯一命中的 bigram 是「怎么」—— 一个**疑问代词**，
 * 它在每一条问句里都有。用它当证据，就等于
 * 「凡是带『怎么』的输入都算命中硬件知识」。
 * 而命中率 1/4 = 25% 恰好高于 20% 的门槛，于是这条误命中稳稳当当通过。
 *
 * 反过来说，2 个 bigram 一致命中基本不可能是巧合：
 * 两个不同的 bigram 同时出现在同一条问句里，那条问句就是在讲这件事。
 */
#define HWK_MIN_HIT 2

/* 问句里有没有中日韩字符。
 *
 * 没有就别检索。这条是为了「3ds」这种**纯平台名**的输入：
 * 它有 2 个 bigram（「3d」「ds」），而这两个片段在半数条目里都有，
 * 于是覆盖率 100% 并被 contains 判成满分 —— 随机命中一条。
 * 而用户打「3ds」时想要的是"关于 3DS 的一切"，不是随机那一条。
 *
 * 中文问句至少有一个汉字，而汉字 bigram 才有区分度。
 * 这一条与「3ds 的 cpu 是几核」不冲突：那个问句里有汉字，
 * 而「3d」「ds」这两个 ASCII bigram 帮不上忙也无害。
 */
static int has_cjk(const char *s)
{
    /* UTF-8 里 CJK 是 3 字节（E4..E9 起头）或更多。
     * 判据只看**首字节**且跳过续接字节，这样不依赖 strlen 之外的信息。 */
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if ((c & 0xC0u) != 0x80u) {          /* 不是一个续接字节 */
            if (c >= 0xE4u) return 1;         /* 三字节及以上 = CJK/全角 */
        }
    }
    return 0;
}

void hwk_search(const char *query, HwkResult *out)
{
    int qlen, qtot, i, best = -1, best_score = 0, over = 0;

    if (!out) return;
    memset(out, 0, sizeof *out);
    out->hit = -1;
    out->n = g_n;

    if (!g_ready || !g_buf || !query) return;

    qlen = (int)strlen(query);
    if (qlen <= 0) return;

    qtot = bigram_total(query, qlen);

    /*
     * 至少要 2 个 bigram（即 3 个字符）才检索。
     *
     * 1 个 bigram 时覆盖率要么 0 要么 100%，而 100% 意味着
     * 「只要那条问句里有这两个字就算命中」—— 答非所问比答不出来糟得多。
     */
    if (qtot < 2) return;

    /* 纯 ASCII 问句（平台名/缩写）不检索。理由见 has_cjk 的注释。 */
    if (!has_cjk(query)) return;

    for (i = 0; i < g_n; i++) {
        const char *ask = g_buf + g_ent[i].ask_off;
        int  alen = g_ent[i].ask_len;
        int  hit, score;

        hit = bigram_hit(query, qlen, ask, alen);
        /* 1 个 bigram 命中不算证据（见 HWK_MIN_HIT 的注释）。
         * 注意这一条**不能省**：「道具怎么用」只命中「怎么」一个，
         * 而那个恰好在半数条目里都有。 */
        if (hit < HWK_MIN_HIT) continue;

        /* 覆盖率 ×100。整数运算：score 一律用放大 100 倍的整数，
         * 因为 %.0f 会把 newlib 的 dtoa 拖进运行期路径
         * （见 sc_guide.c 里同类的注记）。 */
        score = hit * 100 / qtot;

        /* 长度惩罚：问句长不代表答得好。候选问句越短越精确，
         * 满分基础上最多打 7 折（alen<=8 拿满，alen 每多 8 字节多一折）。 */
        if (alen > 8)
            score -= (alen - 8) * 100 / 96 / 8;
        if (score < 1) score = 1;

        /* 完整包含 → **饱和到 100**，而不是加分。
         *
         * 原来写的是 `score += 40`，于是问句与条目一字不差时
         * 打出 140%。那个数不是"匹配度 140%"，它只是"排第一"的手段，
         * 而 140% 会被当成一个可解读的量 —— 探针日志里那几行
         * 「覆盖 140%」看着就不像话。
         * 饱和的语义清楚得多：**100 = 完整对应**。
         * 而且饱和不影响排序 —— 包含关系的分数本来就该最高。 */
        if (contains(query, qlen, ask, alen) ||
            contains(ask, alen, query, qlen))
            score = 100;

        if (score > best_score) {
            best_score = score;
            best = i;
            over = 0;
        } else if (best >= 0 && score == best_score) {
            over++;
        }
    }

    /*
     * 门槛：覆盖率低于 20% 视为没找到。
     *
     * 为什么要门槛而不是"取最高分就行"：bigram 天然会给所有条目
     * 打一点分（"3ds"这个词在每条问句里都有），不设门槛的话
     * 问什么都会有"最佳命中"，而那条其实跟问题无关。
     * 用户看到的应该是"没找到"而不是一条不相关的硬件知识 ——
     * 后者更糟，因为它看起来像个答案。
     */
    if (best < 0 || best_score < 20)
        return;

    out->hit = best;
    out->score = best_score;
    out->over = over;
    {
        int n = g_ent[best].ans_len;
        if (n > HWK_MAX_ANSWER) n = HWK_MAX_ANSWER;   /* 不截断成半个 UTF-8 */
        memcpy(out->best, g_buf + g_ent[best].ans_off, (size_t)n);
        out->best[n] = '\0';
        n = g_ent[best].ask_len;
        if (n > HWK_MAX_ASK - 1) n = HWK_MAX_ASK - 1;
        memcpy(out->ask, g_buf + g_ent[best].ask_off, (size_t)n);
        out->ask[n] = '\0';
    }
}
