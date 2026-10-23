/*
 * extract.c — 抽取式回答：从命中段落里**选句子**，不让模型造句
 *
 * ══ 为什么要有这个文件（2026-10-04 实测得出的）════════════════════
 *
 * 原来的 `gnn_generate` 让模型逐 token 自由生成。实测：
 *     「火焰纹章觉醒 转职怎么转」
 *     -> 「上的飞行的飞行的飞行。敌人的飞行属性。龙的飞行。」
 *
 * 教师强迫 top-1 正确率只有 **6.1%**（19/312）——
 * 连**训练集原文**都续不下去（喂「…常见的基础职业有剑士、佣兵和战士。」，
 * top-8 全是 `用的。打压火敌人水道具` 这类虚词）。
 *
 * 而且答案 token **就在候选集里**（转职/转职之证/等级十级 全在 134 个
 * 候选 token 中），所以**不是检索没喂对，是模型给它们的概率不够高**。
 *
 * ══ 也试过"让模型给句子打分"（条件对数似然），**结果更差**══════════
 *
 * 8 条用例：
 *     打分 = 模型条件似然（PMI）        top1 **3/8**   top3 7/8
 *     打分 = 关键词覆盖（**不含模型**）   top1 **6/8**   top3 8/8
 *
 * 加上模型分数反而变差。原因很直白：PMI 高 = "这句接在前文后面很顺"，
 * 而**短而通用的句子恰好最顺**（「外传关卡的经验值很高」1.398
 * 压过「转职需要转职之证」）。这与 6.1% 的续写能力是同一件事的两面：
 * **它学的是语感，不是内容相关性**。
 *
 * ══ 所以这个文件不假装模型有用 ════════════════════════════════════
 *
 * 按实测把各部分派给擅长的那一层：
 *   · **事实层**（这句话说的是不是真的）  -> 语料原文，逐字可信
 *   · **筛选层**（哪句在回答这个问题）    -> 关键词覆盖，实测 6/8
 *   · **语言层**（怎么把它说得像攻略）    -> **暂时不用模型**
 *
 * 最后那层是欠着的。要补上它就得重训 —— 那是唯一一条真正能提升
 * 模型的路，不是调参。
 *
 * ⚠️ 别把"覆盖分 6/8"当成"抽取式 6/8"。这 8 条用例的答案词
 *   （转职/钩索箭/弱点/沙之杖/吸尘器/漂移/循环/大师之剑）都是
 *   **问句里原样出现的词**。换个不含实体词的问题（"这游戏好玩吗"）
 *   覆盖分全是 0，会退化成"取检索前三句"。那仍然是可用输出，
 *   但它不是"答对了"，只是"没答错"。
 */
#include "extract.h"

#include <string.h>

/* ---- 调试追踪：只在 -DEXTRACT_TRACE 时启用，正常构建零开销 ------------
 *
 * 用途：**段错误发生在函数内部时，stderr 的最后一行就是崩点**。
 * 段错误杀进程、Python 侧捕不到异常，所以只能靠"崩前最后一行打印"定位；
 * 而且 stderr 默认无缓冲，不像 stdout 那样会随进程一起丢掉。
 *
 * 判据脚本 tools/guia/extract_parity.py 的 build_so() 会带上这个宏。
 */
#ifdef EXTRACT_TRACE
#include <stdio.h>
#define ET(...) fprintf(stderr, "[ET] " __VA_ARGS__)
#else
#define ET(...) do { } while (0)
#endif

/* 诊断输出钩子（由 exttest 装上）。没装时为 NULL，调用点都判过了。
 * ⚠️ 原来这里还有一个 g_ext_dbg_stub()，但它从来没被调用过 ——
 *   真正的 weak 定义在文件下方（`__attribute__((weak)) void (*g_ext_dbg)(...) = 0;`），
 *   两处定义并存只会产生 "defined but not used" 警告。已删。 */
void (*g_ext_dbg)(const char *s);

/*
 * UTF-8 下"这个字符占几个字节"：按首字节判断。
 * 0xxxxxxx = 1（ASCII） 110xxxxx = 2 1110xxxx = 3 11110xxx = 4
 * 这不是完整的 UTF-8 校验 —— 但语料是自家生成的，没有非法序列，
 * 而完整校验要多一张表（3DS 上没有地方放）。因此这里**只做长度推断**，
 * 不做合法性判断。
 */
static int u8_len(unsigned char c)
{
    if (c < 0x80) return 1;
    if ((c & 0xE0) == 0xC0) return 2;
    if ((c & 0xF0) == 0xE0) return 3;
    return 4;
}

/*
 * 取一个子串。out 必须有 n+1 字节（补 '\0'）。
 * ⚠️ 一律按显式长度拷贝 —— kb->text 里的段落末尾没有 NUL，
 * 用 strncpy/strcpy 会读到别人的数据。
 */
static void sub_str(char *out, int out_cap,
                    const char *src, int src_len, int off, int n)
{
    int w = 0;
    if (off < 0) off = 0;
    if (off > src_len) off = src_len;
    if (off + n > src_len) n = src_len - off;
    if (n > out_cap - 1) n = out_cap - 1;
    for (int i = 0; i < n; i++)
        out[w++] = src[off + i];
    out[w] = '\0';
}

/* ---- 实体词窗口 -----------------------------------------------------
 *
 * ★ 与 PC 侧 local_chat.py 的 entities_of() **必须同规则**。
 * 那边踩过一个坑：逐个 replace 删停用词会把相邻词切碎
 * （「火焰纹章觉醒 转职怎么转」删掉「怎么」后剩下「转职转」，
 *   「转」在虚词表里 → 「转职」被砍掉一截）。
 * 所以两边都改成"把停用词当分隔符、切出片段、再从片段取窗口"。
 *
 * 窗口要列全（一个实体的 2~5 字所有子串）：实测「大师之剑」
 * 若只留最长窗口会得到「大师」（被虚词「之」切开），
 * 而句子里的形式是四个字连着 —— 漏掉正确形态整题就答错。
 */
#define ENT_MAX 24          /* 一个实体的窗口数上限（5+4+3+2 的和） */
/* 实体词表总容量。★ 2026-10-04 从 8 提到 48：
 * 原来只有 8，而 extract_entities 一次最多产出几十个窗口
 * （5+4+3+2 的组合，一个 6 字片段就有 10 个）——
 * 后加的**兜底词会被前面的挤掉**，于是"沙之杖"这类
 * 含单字停用词的名字又拿回来了。48 是"片段词 + 兜底词"都能装下的量级。
 * 内存代价：48 ×16 字节 = 768 字节，可接受。 */
#define ENT_CAP  48

/* 单个窗口的字节上限。UTF-8 汉字最多 3 字节（4 的是 emoji，
 * 攻略文里没有），5 个字 = 15 字节，16 刚好。 */
#define ENT_BYTES 16

/* 查一个子串是否出现在 hay 里（按显式长度，无 NUL） */
static int has_sub(const char *hay, int hlen, const char *ned, int nl)
{
    if (nl <= 0 || nl > hlen) return 0;
    for (int i = 0; i + nl <= hlen; i++) {
        int k = 0;
        while (k < nl && hay[i + k] == ned[k]) k++;
        if (k == nl) return 1;
    }
    return 0;
}

/*
 * 从 query 里取实体词窗口，写进 out（每项 = 一个 NUL 结尾的字符串，
 * 返回项数）。停用词表与 gkb_query_tokens 用的是同一份
 * （guide_kb.c 里的 g_stopsub / g_stopex），但**必须自己再声明一遍**
 * —— 那些是 static，别的文件看不见。
 *
 * ⚠️ 这里不复用 gkb_query_tokens 的 BPE 结果：BPE 判的是
 * "这个 token 在语料里值不值得查"，而这里要的是
 * "这个词在**字符串**里能不能被句子匹配到"。两者判据不同，
 * 混用会让"之"这种高频虚词进入实体词表。
 */
static int extract_entities(const char *query, char out[][ENT_BYTES],
                            int max_ent)
{
    /* 停用词：与 guide_kb.c 的 g_stopsub/g_stopex 保持一致 */
    /* ⚠️⚠️ 这两份表是 guide_kb.c 里 g_stopsub / g_stopex 的**拷贝**。
     * 那边是 static，别的文件看不见，只能各写一份 ——
     * 而「各写各的」正是本项目反复踩的坑：
     * 2026-10-04 发现 C 侧 g_stopex 只有 12 个（PC 侧 28 个），
     * 缺的正好是「之/这/那/上/中/会/有/要」这些高频虚词，
     * 于是 gkb_query_tokens 不滤它们、这里的覆盖判定也失配。
     * → **改任何一边都必须改另一边**，改完跑 X2 趟验。
     */
    static const char *const SUB[] = {
         "怎么", "如何", "哪里", "在哪", "什么", "为什么", "为何", "多少", "要点",
         "用处", "教程", "方法", "介绍", "意思", "能否", "应该", "什么用", "有啥",
         "咋",
    };
    static const char *const EX[] = {
         "的", "了", "吗", "呢", "吧", "啊", "和", "与", "在", "是", "我",
         "会", "要", "个", "这", "那", "上", "下", "中", "之", "其", "有",
         "以及", "还有", "可以", "需要", "没有", "有没有", "哪",
    };
    enum { NSUB = (int)(sizeof(SUB) / sizeof(SUB[0])),
           NEX  = (int)(sizeof(EX)  / sizeof(EX[0])) };

    char buf[192];
    int bl, n_ent = 0, w = 0;

    if (!query) return 0;
    bl = (int)strlen(query);
    if (bl > (int)sizeof(buf) - 1) bl = (int)sizeof(buf) - 1;

    /* ① 先把停用词替换成 0x01 当分隔符（不是删掉 —— 见上面的坑） */
    for (int i = 0; i < bl; i++) {
        int skip = 0;
        for (int k = 0; k < NSUB; k++) {
            int l = (int)strlen(SUB[k]);
            if (l <= bl - i && !memcmp(query + i, SUB[k], l)) { skip = 1; i += l - 1; break; }
        }
        if (skip) { buf[w++] = '\x01'; continue; }
        /* 单字停用词要按**整段 UTF-8 字节**比，不能只比首字节。
         *
         * ⚠️ 第一版写的是  ——
         * 两个错：① 只比**首字节**，而「之」是 3 字节（E4 B9 8B），
         *   任何以 E4 开头的中文都会命中；②  几乎永假
         *   （问句后面还有内容），于是这个判断几乎从不生效。
         * 后果是「时之笛 钩索箭哪里拿」里的「之」没被剔除，
         * 实体词窗口里混进了碎片，覆盖判定失配。
         */
        for (int k = 0; k < NEX; k++) {
            int el = (int)strlen(EX[k]);
            if (el > 0 && i + el <= bl && !memcmp(query + i, EX[k], (size_t)el)) {
                skip = 1; i += el - 1; break;
            }
        }
        buf[w++] = skip ? '\x01' : query[i];
    }
    buf[w] = '\0';

    /* ② 切片段，对每个片段取 2~5 字窗口 */
    int start = 0;
    for (int i = 0; i <= w; i++) {
        if (i == w || buf[i] == '\x01') {
            int flen = i - start;
            /* 跳过片段首尾的空白 */
            while (start < i && (buf[start] == ' ')) start++;
            while (i > start && (buf[i - 1] == ' ')) i--;
            flen = i - start;
            if (flen >= 2 && n_ent < max_ent) {
                for (int L = 5; L >= 2; L--) {
                    if (flen < L) continue;
                    for (int j = 0; j + L <= flen; j++) {
                        if (n_ent >= max_ent) break;
                        /* 按字节取 L 个**汉字**：先量出 L 个字的字节数 */
                        int b0 = start + j, cnt = 0;
                        while (b0 < i && cnt < L) {
                            int l = u8_len((unsigned char)buf[b0]);
                            if (b0 + l > i) break;
                            b0 += l; cnt++;
                        }
                        if (cnt != L) continue;
                        int nb = b0 - (start + j);
                        if (nb >= ENT_BYTES) continue;
                        memcpy(out[n_ent], buf + start + j, (size_t)nb);
                        out[n_ent][nb] = '\0';
                        n_ent++;
                    }
                }
            }
            start = i + 1;
        }
    }

    /* ★ 兜底：把**原始问句**的 2~5 字 n-gram 也列进实体词表。
     *
     * 为什么必需（2026-10-04 在 PC 侧复刻时查出来的真缺陷）：
     * 上面① 把停用词当分隔符，而**单字停用词会切碎实体词** ——
     * 「之」「有」「中」都在 EX 表里，于是
     *     「沙之杖有什么效果」->  片段 ['沙','杖','效果']
     * 「沙之杖」整个消失，只剩通用的「效果」，
     * 而 #1085/#1086 段里**确实写着「沙之杖」**——
     * 句子是对的，实体词表里没有它，覆盖判定于是失效，答了别的段落。
     *
     * 受影响的都是**含单字停用词的名字**，实测中招：
     *   沙之杖 / 大师之剑 / 时之笛 / 心之碎片 / 黄金叶 / 动力之剑 / 风之权杖
     * 它们恰恰是游戏里最核心的道具名，不是边角情况。
     *
     * 为什么这样安全：覆盖判定是「句中有任一实体词窗口」，
     * 多给候选只会更容易命中，而**这些候选全部来自用户输入**，
     * 不是凭空造的词 —— 与 PMI 那种引入噪声的做法相反。
     */
    if (query && *query) {
        /* 源串 = 原始 query去掉空白（空白会把n-gram 切断） */
        char raw[192];
        int rw = 0;
        for (int i = 0; query[i] && rw < (int)sizeof(raw) - 1; i++) {
            unsigned char c0 = (unsigned char)query[i];
            if (c0 == ' ' || c0 == '\t' || c0 == '\n' || c0 == '\r') continue;
            raw[rw++] = query[i];
        }
        raw[rw] = '\0';

        /* 汉字起始下标表。★ 只收**完整**的汉字：末尾若只剩半个码点
         *   就不进表，否则 ns 会比真实汉字数大 1，
         *   下标 st[a + L] 会越界（PC 侧复刻时实测 IndexError）。 */
        int st[64], ns = 0;
        for (int i = 0; i < rw && ns < 64; ) {
            int l = u8_len((unsigned char)raw[i]);
            if (i + l > rw) break;
            st[ns++] = i;
            i += l;
        }

        for (int a = 0; a < ns; a++) {
            for (int L = 5; L >= 2; L--) {
                /* ★ 必须是 `>=` 而不是 `>`：窗口是 st[a] .. st[a + L]，
                 *   下标 a + L 本身就要合法，所以 a + L == ns 已经越界。
                 *   写成 `>` 时 a=0 / L=5 / ns=5 会漏过判据然后越界
                 *   （PC 侧复刻时实测整个函数对 5 字问句全挂）。 */
                if (a + L >= ns) continue;
                int b0 = st[a], b1 = st[a + L];   /* 窗口 = [b0, b1) */
                int nb = b1 - b0;
                if (nb >= ENT_BYTES) continue;
                int dup = 0;
                for (int z = 0; z < n_ent; z++) {
                    if ((int)strlen(out[z]) == nb &&
                        !memcmp(out[z], raw + b0, (size_t)nb)) { dup = 1; break; }
                }
                if (dup || n_ent >= max_ent) continue;
                memcpy(out[n_ent], raw + b0, (size_t)nb);
                out[n_ent][nb] = '\0';
                n_ent++;
            }
        }
    }

    return n_ent;
}

/* ---- 标题型句子过滤 -------------------------------------------------
 *
 * build_kb 时把「## 章节标题。」拼在段首（text = title + "。" + body），
 * 所以切出来的第一句常常是「主要道具与用途。」这种**元信息**。
 * 它不含实词、又特别短，被选中会得到"完全没回答"的答案。
 */
static int is_title_like(const char *s, int len)
{
    static const char *const P[] = {
        "主要", "基本", "常见", "进阶", "隐藏", "前期", "中期", "后期",
        "道具", "玩法", "系统", "要点", "技巧", "总结", "简介", "一览",
        "说明", "注意", "警告", "提示"
    };
    enum { NP = (int)(sizeof(P) / sizeof(P[0])) };

    /* 太短的句子（<10 字节 ≈ 3 个汉字）一定不是答案 */
    if (len < 10) return 1;

    /* 以元信息词开头，且整句很短 */
    for (int k = 0; k < NP; k++) {
        int l = (int)strlen(P[k]);
        if (l <= len && !memcmp(s, P[k], l) && len < 16 * 3)
            return 1;
    }
    return 0;
}

/* ---- 主函数 --------------------------------------------------------- */
__attribute__((weak)) void (*g_ext_dbg)(const char*) = 0;

/*
 * 数 UTF-8 **字符**个数（不是字节）。
 *
 * ★ 为什么要它：候选句的**长度门槛必须两边同单位**。
 * PC 参照 `local_chat.py` 的 `sentences(min_len=10)` 判的是 `len(p)`，
 * 而 Python 的 str.len() 数的是**字符**；C 这边 `tlen = strlen(tmp)` 是**字节**。
 * 一句 9 个汉字 = 27 字节 → PC 判 9<10 丢掉、C 判 27>=10 保留，
 * 于是**候选集就不同 → top-3 选取整体错位**（实测 8/8 全不一致的主因之一）。
 * 而且 10 字节只等于 3 个汉字，作「句子太短就丢」的门槛明显太松。
 * → 以 PC 为参照，C 侧统一按**字符数**判。
 */
static int u8_count(const char *s, int blen)
{
    int i = 0, n = 0;
    while (i < blen) {
        int k = u8_len((unsigned char)s[i]);
        i += (k > 0) ? k : 1;   /* u8_len 返 0 时必须手动 +1，否则死循环 */
        n++;
    }
    return n;
}

int gkb_extract(const Gkb *kb, const GkbHit *hits, int n_hit,
                const char *query, char *out, int out_cap)
{
    /* 容量必须是 ENT_CAP，与 extract_entities 的 max_ent 一致 ——
     * 传 8 会让兜底词全被挤掉（实测 max_ent=8 时「沙之杖」拿不回来）。 */
    char ents[ENT_CAP][ENT_BYTES];
    char (*cand)[EXT_SENT_MAX];
    int  *cand_hit;
    int   n_ent, n_cand = 0, w = 0;
    static char sbuf[EXT_CAND_MAX][EXT_SENT_MAX];
    static int  s_hit[EXT_CAND_MAX];

    if (out_cap < 2) return 0;
    out[0] = '\0';
    if (!kb || !hits || n_hit <= 0) return 0;

    ET("enter n_hit=%d out_cap=%d text_len=%d q=[%s]\n",
       n_hit, out_cap, (int)kb->text_len, query ? query : "(null)");

    cand     = (char (*)[EXT_SENT_MAX])sbuf;
    cand_hit = s_hit;

    n_ent = extract_entities(query, ents, ENT_CAP);
    ET("entities n_ent=%d\n", n_ent);

    /* ① 逐段切句 */
    for (int h = 0; h < n_hit && n_cand < EXT_CAND_MAX; h++) {
        const char *p = kb->text + hits[h].tok_off;
        int plen = hits[h].tok_len;
        int start = 0;

        ET("hit[%d] off=%d len=%d first8=%02x %02x %02x %02x\n",
           h, (int)hits[h].tok_off, (int)plen,
           (unsigned char)p[0], (unsigned char)p[1],
           (unsigned char)p[2], (unsigned char)p[3]);

        /* ⚠️ 空段落必须跳过（2026-10-04 在 PC 侧复刻时发现的真洞）。
         * 下面的循环靠 `i++` 和"标点后按字节长度推进"两种方式前进，
         * 而 `at_end` 分支既不推进 i 也不改 start（start = i = plen）——
         * plen == 0 时循环条件 `i <= plen` 恒真、恒不推进 -> **死循环挂死**。
         * 索引里出现空段落并不需要损坏：一个全被停用词吃掉的短查询，
         * 或将来build_kb.py 改了切分规则，都可能产出 plen=0。 */
        if (plen <= 0) continue;

        for (int i = 0; i <= plen; i++) {
            int at_end = (i == plen);
            /* 句末标点。⚠️ **不能写成 '。'** ——
             * C 的多字节字符常量是 int（multi-character constant），
             * gcc 会报 warning 且**比较恒为假**（它变成 0x3002 之类，
             * 而 char 只有 8 位）。症状是"切不出句子，n_cand 恒为 0"，
             * 而编译器只是 warning、不报错 —— 这种坑必须写成字节比较。
             *   。U+3002 -> E3 80 82
             *   ！U+FF01 -> EF BC 81
             *   ？U+FF1F -> EF BC 9F
             * ⚠️ 也不能只比首字节：0xEF 开头的不止全角标点
             * （ā、ǎ 之类拉丁扩展字母也是 EF 开头的 3 字节序列），
             * 只看首字节会在正常词中间切开。
             * 所以这里**比完整的三字节序列**。 */
            int is_sep = 0;
            unsigned char c = at_end ? 0 : (unsigned char)p[i];
            if (!at_end) {
                if (c == '\n' || c == '\r') {
                    is_sep = 1;
                } else if (c == 0xE3 && i + 2 < plen &&
                           (unsigned char)p[i + 1] == 0x80 &&
                           (unsigned char)p[i + 2] == 0x82) {
                    is_sep = 1;                      /* 。 */
                } else if (c == 0xEF && i + 2 < plen &&
                           (unsigned char)p[i + 1] == 0xBC &&
                           ((unsigned char)p[i + 2] == 0x81 ||
                            (unsigned char)p[i + 2] == 0x9F)) {
                    is_sep = 1;                      /* ！ ？ */
                }
            }
            if (!is_sep && !at_end) continue;

            /* 收一句 [start, i)（含句号） */
            int slen = i - start;
            if (slen > 0) {
                char tmp[EXT_SENT_MAX];
                sub_str(tmp, EXT_SENT_MAX, p, plen, start, slen);
                int tlen = (int)strlen(tmp);
                /* ⚠️ 门槛按**字符数**判，不按字节 —— 与 PC 参照同单位。
                 * 原来写 `tlen >= 10`（tlen 是 strlen = 字节），
                 * 10 字节只等于 3 个汉字，松得离谱。详见 u8_count 的注释。 */
                if (u8_count(tmp, tlen) >= 10 && !is_title_like(tmp, tlen)) {
                    /* 去重：同一句可能出现在多个命中段里 */
                    int dup = 0;
                    for (int k = 0; k < n_cand; k++) {
                        if (!strcmp(cand[k], tmp)) { dup = 1; break; }
                    }
                    if (!dup && n_cand < EXT_CAND_MAX) {
                        ET("cand[%d] tlen=%d (cap=%d)\n",
                           n_cand, tlen, (int)EXT_SENT_MAX);
                        memcpy(cand[n_cand], tmp, (size_t)tlen + 1);
                        n_cand++;
                        if (n_cand <= 3 && g_ext_dbg) {
                            static char m[64];
                            int q = 0;
                            m[q++]='C'; m[q++]='a'; m[q++]='n'; m[q++]='d';
                            m[q++]='='; m[q++]=(char)('0'+n_cand);
                            m[q++]=' ';
                            m[q++]='h'; m[q++]='e'; m[q++]='a'; m[q++]='d'; m[q++]='=';
                            for (int z = 0; z < 4 && z < tlen; z++) {
                                unsigned u = (unsigned char)tmp[z];
                                m[q++]=(char)('0'+(u>>4));
                                m[q++]=(char)('0'+(u&15));
                            }
                            m[q++]=' ';
                            m[q++]='t'; m[q++]='a'; m[q++]='i'; m[q++]='l'; m[q++]='=';
                            for (int z = tlen > 4 ? tlen-4 : 0; z < tlen; z++) {
                                unsigned u = (unsigned char)tmp[z];
                                m[q++]=(char)('0'+(u>>4));
                                m[q++]=(char)('0'+(u&15));
                            }
                            m[q] = 0;
                            g_ext_dbg(m);
                        }
                    }
                }
            }
            /* 跳过整块标点（**不是 1 字节**）。
             *
             * ⚠️⚠️ 这里曾写成「只把 i 推进 1 位」——
             * 而「。」是 **3 字节**（E3 80 82），
             * 于是 E3 被留在上一句尾部，下一句从 80 82 开始：
             *     Cand=1 head=8082...     ← 缺首字节的孤立句号
             * 每句话开头都是坏的，而**整段话的字节数只差 1**
             * （E3 被上一句吃掉了，所以总长看着"差不多"）。
             *
             * 症状特别难认：输出看起来是中文、能看出意思，
             * 只有 UTF-8 校验才抓得到（每句都带一个孤立续字节）。
             * → 所以切分必须用**字节长度**推进，不能用字符位置。
             */
            if (is_sep && !at_end) {
                /* i 指向标点首字节；u8_len 给出它的字节数 */
                i += (c == '\n' || c == '\r') ? 1 : 3;
                /* 标点后可能紧跟引号/括号，略过。
                 *
                 * ⚠️ 0xEF 必须按**完整序列长度**推进，不能 `i++`。
                 * 0xEF 是 3 字节序列的**首字节**：全角标点（！？）之外，
                 * 「ā」「ǎ」这类拉丁扩展字母也是它开头。
                 * 按 1 字节推进会落进续字节中间，而续字节不是任何标点 ->
                 * `is_sep` 恒假 -> 只能靠 `i++` 一个个爬。
                 * 若后面又跟可跳字节就可能在原地打转（PC 侧复刻时
                 * 实测真的挂死了，见 tools/guia/c_side_sim.py 的同名注释）。
                 */
                while (i < plen) {
                    unsigned char d = (unsigned char)p[i];
                    if (d == 0xEF)            i += 3;   /* 整个 3 字节序列 */
                    else if (d == '"' || d == '\'' || d == '\n'
                             || d == '\r' || d == ' ') i += 1;
                    else break;
                }
            }
            start = i;
            /* 收完最后一句就退出。`at_end` 分支既不推进 i 也不改 start，
             * 靠 `i <= plen` 退出在 plen>0 时成立，但多一句显式 break
             * 可以让"循环一定终止"这件事不依赖外部条件。 */
            if (at_end) break;
        }
        ET("hit[%d] split done n_cand=%d\n", h, n_cand);
    }
    if (n_cand == 0) return 0;
    ET("stage1 total n_cand=%d\n", n_cand);

    /* ② 覆盖判定：句中含任一实体词窗口 */
    for (int k = 0; k < n_cand; k++) {
        cand_hit[k] = 0;
        for (int e = 0; e < n_ent && !cand_hit[k]; e++) {
            if (has_sub(cand[k], (int)strlen(cand[k]), ents[e],
                        (int)strlen(ents[e])))
                cand_hit[k] = 1;
        }
    }

    /* ③ 取前 EXT_PICK 句命中；一句没命中就退回检索原序的前几句。
     *
     * ⚠️ 保持**原序**（不改排序）—— BM25 已经把最相关的段落排好，
     * 打乱顺序会让答案读起来像随机抽取。
     * 而"退回"那一条不是答对，只是没答错（见文件头注）。 */
    {
        int picked = 0;
        ET("stage3 assemble n_cand=%d n_ent=%d\n", n_cand, n_ent);
        for (int pass = 1; pass >= 0 && picked < EXT_PICK; pass--) {
            /* pass=1 只要命中的；pass=0 才补没命中的 */
            for (int k = 0; k < n_cand && picked < EXT_PICK; k++) {
                if (pass == 1 && !cand_hit[k]) continue;
                if (pass == 0 && cand_hit[k]) continue;   /* 已取过 */
                int l = (int)strlen(cand[k]);
                {
                    /* 先算需要多少字节，再一次性写完。
                     *
                     * ⚠️ 原来是"边判边写"（分三次 out[w++] 写句号），
                     * 结果输出里出现**缺首字节的句号**（80 82 而不是
                     * E3 80 82），UTF-8 校验 5/8 条 FAIL。
                     *
                     * 「先算后写」还顺带修掉另一个隐患：原来那句
                     * `if (w + 4 >= out_cap) return w;` 里，
                     * 返回时 out[w]='\0' **已经写了**而 w 没推进 ——
                     * 等于"写了个终止符却当作什么都没写"，
                     * 空间刚好不够时会产生**半截输出**。
                     */
                    /* 是否补句号：前面已有内容，且当前句没以句号结尾。
                     * 判「以句号结尾」看最后一个字节是 0x82（。的第三字节）
                     * —— 句子是从原文切下来的，真句子不会以 0x82 单独结尾。 */
                    /* ★ 判断「要不要补句号」要看**当前这句**，
                     * 不是看**已写出的 out**！
                     *
                     * 我第一版判的是 out[w-1]，而 out[w-1] 是**上一句**的尾字节
                     * —— 于是只要上一句以句号结尾就永远不补，
                     * 而上一句不带句号时会补两次。
                     * 实测输出 ：
                     * 前者是补的（正确），后者是**句子自带的 0x82**
                     * 被当成缺首字节 —— 因为本句的句号**已经在句内**。
                     *
                     * 正确判据：当前句自己是否以 0x82（。的第三字节）结尾。
                     * 切句时 [start,i) 理论上不含标点，但**语料里
                     * 「。」可能出现在其它位置**（比如换行前的残留），
                     * 所以句子自带句号是可能的，必须判。
                     */
                    int cur_ends_stop = (l >= 3) &&
                        ((unsigned char)cand[k][l-1] == 0x82);
                    /* ★★ 这里原来还有 `(w > 0) &&`，而句号是写在**本句之前**
                     * （见下方 if (add_stop) 的位置），等于「把上一句缺的句号
                     * 补在下一句开头」。后果有两个：
                     *   ① 首句永远没人补 —— 因为 w==0 时 add_stop 被判假；
                     *   ② 末句永远没有句号 —— 因为它后面没有「下一句」可 prepend。
                     * 实测 8/8 用例的 C 输出都比 PC 少一个结尾句号。
                     * PC 侧 local_chat.py 的 sentences() 用 lookbehind 切分，
                     * **每句自带句号**。所以正确做法是：拷贝完**本句之后**再补，
                     * 且不限制 w>0（首句也要补）。
                     */
                    int add_stop = !cur_ends_stop;
                    int need = l + (add_stop ? 3 : 0);
                    if (w + need >= out_cap) { out[w] = '\0'; return w; }

                    /* 诊断：把这一步的三个数打出来 */
                    {
                        static char dbg[160];
                        int q = 0;
                        dbg[q++]='L'; dbg[q++]='='; dbg[q++]=(char)('0'+l/100%10);
                        dbg[q++]=(char)('0'+l/10%10); dbg[q++]=(char)('0'+l%10);
                        dbg[q++]=' '; dbg[q++]='a'; dbg[q++]='=';
                        dbg[q++]=(char)('0'+add_stop);
                        dbg[q++]=' '; dbg[q++]='w'; dbg[q++]='=';
                        dbg[q++]=(char)('0'+w/100%10);
                        dbg[q++]=(char)('0'+w/10%10);
                        dbg[q++]=(char)('0'+w%10);
                        dbg[q++]=' '; dbg[q++]='L'; dbg[q++]='a'; dbg[q++]='s'; dbg[q++]='t'; dbg[q++]='=';
                        dbg[q++]=(char)('0'+(l>0?l-1:0)/100%10);
                        dbg[q++]=(char)('0'+(l>0?l-1:0)/10%10);
                        dbg[q++]=(char)('0'+(l>0?l-1:0)%10);
                        dbg[q++]=' ';
                        /* ⚠️ 这里原来写成
                         *     '0'+(unsigned char)out[w-1]>>4
                         * 而 C 的 `+` 优先级**高于** `>>` ——
                         * 实际算的是 ('0'+byte)>>4，不是 byte>>4。
                         * 只差一个 0x30，诊断里十六进制高位一直是错的
                         * （而这个钩子恰恰是用来定位坏字节的）。
                         * → 加括号。 */
                        if (w>0) dbg[q++]=(char)('0'+(((unsigned char)out[w-1])>>4));
                        if (w>0) dbg[q++]=(char)('0'+((unsigned char)out[w-1]&15));
                        dbg[q]=0;
                        /* ★ 必须判空。g_ext_dbg 是 weak 函数指针、
                         * 默认 0（见文件下方 337 行的 weak 定义）。
                         * 432/452 那两处调用都有 && g_ext_dbg 守卫，
                         * 只有这里漏了 → 独立编 .so 时（判据脚本 build_so()
                         * 不会给它赋值）就是一记 **NULL 调用 → SIGSEGV**。
                         * 3DS 主线里 app 会赋值（exttest.c:109）所以一直不崩，
                         * 属于**被主线掩盖的潜伏 bug**：app 哪天没赋值就同样炸。 */
                        if (g_ext_dbg) g_ext_dbg(dbg);
                    }
                    for (int q = 0; q < l; q++) out[w++] = cand[k][q];
                    /* ★ 句号补在**本句之后**。原来这段在拷贝之前，
                     * 于是补出来的是「上一句的句号」而不是本句的 ——
                     * 首句没有、末句也没有（详见上面 add_stop 处的注释）。 */
                    if (add_stop) {
                        /* 。 = E3 80 82。⚠️ 不能写成 '。' ——
                         * C 的多字节字符常量是 int（0x3002），
                         * gcc 只报 -Wmultichar **warning**（不是错误），
                         * 赋给 char 只剩低 8 位 = 0x82 →
                         * 输出里多一个孤立续字节（屏上是乱码）。 */
                        out[w++] = (char)0xE3;
                        out[w++] = (char)0x80;
                        out[w++] = (char)0x82;
                    }
                }
                picked++;
            }
        }
    }
    out[w] = '\0';
    return w;
}
