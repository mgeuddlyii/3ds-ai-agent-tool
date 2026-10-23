/*
 * cupdtest.c — 见 cupdtest.h
 *
 * 全部用例都在 sdmc:/3ds/aiconsole/_cupdtest/ 下造文件，
 * 不碰真实内容（真实路径由 cu_set_root 换成测试根）。
 */

#include "cupdtest.h"

#include "cupd.h"
#include "app.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>   /* rmdir */

#define TROOT "sdmc:/3ds/aiconsole/_cupdtest"

/* 结果统计。最后一条 app_note 汇总，验收脚本读它。 */
static int g_pass, g_fail;

static void ck(int ok, const char *what)
{
    if (ok) {
        g_pass++;
        app_note("cupdtest: PASS %s", what);
    } else {
        g_fail++;
        app_note("cupdtest: FAIL %s", what);
    }
}

static void rm_r(const char *path)
{
    /* 自测目录是我们自己造的，内容有限，逐个删即可。
     * 不递归 rm -rf：万一路径拼错，那一下删的是别的东西。 */
    static const char *names[] = {
        "/guide_model.bin", "/guide_kb.bin", "/hw_knowledge.txt",
        "/guide_model.bin.bak", "/guide_kb.bin.bak", "/hw_knowledge.txt.bak",
        "/ver_model.txt", "/ver_kb.txt", "/ver_hw.txt",
        "/ver_model.txt.tmp", "/ver_kb.txt.tmp", "/ver_hw.txt.tmp",
        "/content/guide_model.bin", "/content/guide_kb.bin",
        "/content/hw_knowledge.txt",
        "/content/manifest.json",
    };
    size_t i;
    char p[192];
    for (i = 0; i < sizeof names / sizeof names[0]; i++) {
        snprintf(p, sizeof p, "%s%s", path, names[i]);
        remove(p);
    }
}

static bool write_file(const char *path, const char *text)
{
    FILE *f = fopen(path, "wb");
    if (!f)
        return false;
    fputs(text, f);
    fclose(f);
    return true;
}

static bool file_len(const char *path, int *len)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return false;
    fseek(f, 0, SEEK_END);
    *len = (int)ftell(f);
    fclose(f);
    return true;
}

static int read_int(const char *path)
{
    FILE *f = fopen(path, "rb");
    int v = -999;
    if (!f)
        return v;
    if (fscanf(f, "%d", &v) != 1)
        v = -999;
    fclose(f);
    return v;
}

/* 64 个 'a'，构造**结构上合法但内容对不上**的摘要用。
 * 「合法但对不上」比「长度不对」更能考验流程：解析会放行，
 * 然后必须由 sha256 那一道拦住。 */
#define SHA_A "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"

/*
 * 下面三个是**真实** sha256，由 tools 侧用 python -c
 * "hashlib.sha256(...).hexdigest()" 算出来：
 *   sha256("hwhw")       = 327b8fcf...1ba8   长度 4
 *   sha256("NEWER-BODY") = 68b4717b...f2f8   长度 10
 *   sha256("CORRUPT")    = c7791eae...cabf   长度 7
 *
 * ⚠️ 写死的哈希必须与写死的正文一一对应。改正文不改哈希，
 * 测试会「失败」在看起来毫不相关的地方（"cu_install 成功" 挂了，
 * 而 install 根本没参与这个失败）。所以两者放在同一处定义。
 */
#define SHA_HWHW    "327b8fcf2423c63948c732aeeb86b4a8b03f8a93b525cf81be98440278bd1ba8"
#define SHA_NEWER  "68b4717b534474c2796d10b88f2522d38dd4121803f8b8c078999ac936e8f2f8"
#define SHA_CORRUPT "c7791eeaf1260ae1da924053a5c5a0f6d51abe1f8aae76a899376268c5aecabf"

/*
 * 故意**错的**摘要（结构合法 64 位 hex，但内容对不上任何文件）。
 *
 * ⚠️ 这一条最初误用了 SHA_CORRUPT —— 也就是 sha256("CORRUPT") 的
 * 真值。那样包里的文件其实是**对的**，于是整条链路顺利通过，
 * 用例却期待 MISMATCH，于是挂在「产品有 bug」的错误方向上。
 *
 * 教训：构造「校验应该失败」的用例时，要确认**失败原因正是校验**，
 * 而不是别的地方恰好也不对。所以这个宏是纯字面量、与任何文件无关，
 * 一眼就能看出它不可能匹配上什么。
 */
#define SHA_WRONG   "deadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeef"

static int parse_case(const char *txt, int *count)
{
    CuManifest m;
    int ok = cu_parse(txt, strlen(txt), &m) ? 1 : 0;
    if (count)
        *count = m.count;
    return ok;
}

void cupdtest_run(void)
{
    static char pf[192], mf[192];
    char msg[192];
    CuManifest m;
    CuState st;
    int n;

    g_pass = g_fail = 0;
    app_note("cupdtest: begin");

    cu_set_root(TROOT);
    mkdir(TROOT, 0777);
    mkdir(TROOT "/content", 0777);
    rm_r(TROOT);

    /* ================= 解析 ================= */

    ck(parse_case("{\"schema\":1,\"artifacts\":[{\"kn\":\"model\","
                  "\"file\":\"m.bin\",\"size\":10,\"sha256\":\"" SHA_A "\","
                  "\"version\":1}]}", &n) == 1, "最小合法 manifest 被接受");
    ck(n == 1, "最小合法 manifest 只有 1 条");

    ck(parse_case("{\"schema\":1,\"artifacts\":["
                  "{\"kn\":\"model\",\"file\":\"m.bin\",\"size\":10,"
                  "\"sha256\":\"" SHA_A "\",\"version\":1},"
                  "{\"kn\":\"kb\",\"file\":\"k.bin\",\"size\":10,"
                  "\"sha256\":\"" SHA_A "\",\"version\":2},"
                  "{\"kn\":\"hardware\",\"file\":\"h.txt\",\"size\":10,"
                  "\"sha256\":\"" SHA_A "\",\"version\":3}]}", &n) == 1
       && n == 3, "三类齐全被接受");

    /* ---- 必须拒绝的 ---- */
    ck(parse_case("{\"schema\":2,\"artifacts\":[]}", 0) == 0,
       "schema=2 被拒");
    ck(parse_case("{\"artifacts\":[]}", 0) == 0,
       "缺 schema 被拒");
    ck(parse_case("", 0) == 0, "空串被拒");
    ck(parse_case("hello", 0) == 0, "非 JSON 被拒");
    ck(parse_case("{\"schema\":1,\"artifacts\":[{\"kn\":\"mod", 0) == 0,
       "截断的 JSON 被拒");

    ck(parse_case("{\"schema\":1,\"artifacts\":[{\"kn\":\"evil\","
                  "\"file\":\"m.bin\",\"size\":10,\"sha256\":\"" SHA_A "\""
                  ",\"version\":1}]}", 0) == 0, "未知 kind 被拒");
    ck(parse_case("{\"schema\":1,\"artifacts\":[{\"kn\":\"models\","
                  "\"file\":\"m.bin\",\"size\":10,\"sha256\":\"" SHA_A "\","
                  "\"version\":1}]}", 0) == 0, "kind 拼错(models)被拒");
    ck(parse_case("{\"schema\":1,\"artifacts\":[{\"kn\":\"\","
                  "\"file\":\"m.bin\",\"size\":10,\"sha256\":\"" SHA_A "\","
                  "\"version\":1}]}", 0) == 0, "空 kind 被拒");

    ck(parse_case("{\"schema\":1,\"artifacts\":[{\"kn\":\"model\","
                  "\"file\":\"m.bin\",\"size\":10,"
                  "\"sha256\":\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\","
                  "\"version\":1}]}", 0) == 0, "sha256 只有 63 位被拒");
    ck(parse_case("{\"schema\":1,\"artifacts\":[{\"kn\":\"model\","
                  "\"file\":\"m.bin\",\"size\":10,\"version\":1}]}", 0) == 0,
       "缺 sha256 被拒");
    ck(parse_case("{\"schema\":1,\"artifacts\":[{\"kn\":\"model\","
                  "\"file\":\"m.bin\",\"size\":0,\"sha256\":\"" SHA_A "\""
                  ",\"version\":1}]}", 0) == 0, "size=0 被拒");
    ck(parse_case("{\"schema\":1,\"artifacts\":[{\"kn\":\"model\","
                  "\"file\":\"m.bin\",\"size\":10,\"sha256\":\"" SHA_A "\","
                  "\"version\":0}]}", 0) == 0, "version=0 被拒");
    ck(parse_case("{\"schema\":1,\"artifacts\":[{\"kn\":\"model\","
                  "\"size\":10,\"sha256\":\"" SHA_A "\",\"version\":1}]}", 0) == 0,
       "缺 file 被拒");

    /* 三条里第二条坏 —— 整包拒，不是只跳过坏的。
     * 「只跳过坏的」更"友好"，但用户会以为整个包都装了，
     * 结果另外两条永远停在旧版本且没有任何提示。 */
    ck(parse_case("{\"schema\":1,\"artifacts\":["
                  "{\"kn\":\"model\",\"file\":\"m.bin\",\"size\":10,"
                  "\"sha256\":\"" SHA_A "\",\"version\":1},"
                  "{\"kn\":\"evil\",\"file\":\"k.bin\",\"size\":10,"
                  "\"sha256\":\"" SHA_A "\",\"version\":2},"
                  "{\"kn\":\"hardware\",\"file\":\"h.txt\",\"size\":10,"
                  "\"sha256\":\"" SHA_A "\",\"version\":3}]}", 0) == 0,
       "三条里一条坏 → 整包拒");

    /* ---- 越界防御 ----
     * 这条单独强调：越界的 kind 如果没被挡住，
     * CU_TARGET[99] 会越界读一个任意地址当字符串返回。 */
    ck(cu_target((CuKind)99)[0] == '\0', "cu_target 越界返回空串");
    ck(cu_kind_name((CuKind)99)[0] != '\0', "cu_kind_name 越界不崩");
    ck(cu_target(CU_MODEL)[0] != '\0', "cu_target 正常值非空");

    /* ================= 无包 ================= */
    {
        int r = cu_scan_local(&m, &st, msg, sizeof msg);
        ck(r == 0 && st == CU_NO_PACKAGE, "卡上没包时报 NO_PACKAGE");
    }

    /* ================= 完整闭环 =================
     *
     * 造一个包：模型 v1，内容 "AAA"。
     * 真实 sha256 算不出来（在测试里现算太重），
     * 所以第一轮用**故意错的摘要**验证「正式文件不被动」；
     * 真正的成功路径靠 manifest 里写对摘要 —— 而写对需要
     * 先知道内容哈希。所以这里用固定内容 + 外部算好的哈希。
     *
     * "hwhw" 的 sha256：
     *   见下方 k_hash_of_hwhw 常量（由 tools 侧验证过）
     */
    {
        static const char BODY[] = "hwhw";
        char mf_txt[512];

        write_file(TROOT "/content/guide_model.bin", BODY);
        snprintf(mf_txt, sizeof mf_txt,
                 "{\"schema\":1,\"artifacts\":[{\"kn\":\"model\","
                 "\"file\":\"guide_model.bin\",\"size\":%d,"
                 "\"sha256\":\"%s\",\"version\":1}]}",
                 (int)sizeof BODY - 1, SHA_HWHW);

        snprintf(pf, sizeof pf, "%s/content/guide_model.bin", TROOT);
        snprintf(mf, sizeof mf, "%s/content/manifest.json", TROOT);
        write_file(mf, mf_txt);

        /* ---- ① 摘要正确时应扫出「1 条可更新」 ----
         *
         * ⚠️ 这里的判据踩过一个坑：一开始写的是 `r == 0 &&`，
         * 因为脑子里的模型是「成功返回 0」。但 cu_scan_local 的
         * 返回值是**「有几条可更新」**（头文件里就写着），不是状态码。
         * 于是 27/39 通过、3 条挂在这一个误解上。
         *
         * 两处错其实同一个：把「返回值」当「错误码」，
         * 而 status 才是状态。断言要盯对的那一个。 */
        {
            int r = cu_scan_local(&m, &st, msg, sizeof msg);
            ck(r == 1 && st == CU_VERIFY_OK,
               "摘要正确时扫出「1 条可更新」");
        }

        /* ---- ② 装：应该成功，并留下 .bak ---- */
        snprintf(mf, sizeof mf, "%s/guide_model.bin", TROOT);
        write_file(mf, "OLD-CONTENT");
        ck(read_int(TROOT "/ver_model.txt") == -999,
           "装之前没有版本记录");

        {
            char imsg[192];
            bool ok = cu_install(&m, 0, imsg, sizeof imsg);
            int n2 = 0;
            ck(ok, "cu_install 成功");
            ck(file_len(TROOT "/guide_model.bin", &n2) &&
               n2 == (int)sizeof BODY - 1, "装完内容长度正确");
            ck(file_len(TROOT "/guide_model.bin.bak", &n2) &&
               n2 == (int)(sizeof "OLD-CONTENT" - 1), "旧文件已备份成 .bak");
            ck(read_int(TROOT "/ver_model.txt") == 1, "版本记录写成 1");
            /* 装完不能再有 .new 残留 */
            ck(!file_len(TROOT "/guide_model.bin.new", &n2),
               "没有 .new 残留");
        }

        /* ---- ③ 再扫一次：应该是 UP_TO_DATE ---- */
        {
            int r = cu_scan_local(&m, &st, msg, sizeof msg);
            ck(r == 0 && st == CU_UP_TO_DATE, "装完再扫是 UP_TO_DATE");
        }

        /* ---- ④ 降级包：已装 v1，拿到 v0 的包 ----
         *
         * ⚠️ 第一版这里写的是「version:0 的包应当被识别为降级」。
         * 实际结果报的是「manifest 结构不对」—— 因为 version=0
         * 在 cu_parse 的结构自检里就**根本不是合法 manifest**。
         *
         * 这不是产品 bug，是我的用例挑了一个不成立的场景：
         * 版本号 0 表示「从来没装过」，打包工具不会发出这种包
         * （make_content.py 从 1 起编）。真正的降级场景是
         * 「已装 v3，收到了 v2 的包」，所以下面先把版本推到 3。
         *
         * 顺带说明为什么「结构不对」这个反馈是对的：
         * 它比「这是降级包」更准确 —— 用户看到结构不对，
         * 会去检查打包流程，而不是以为自己系统出问题了。 */
        {
            char txt[512];
            int  r;
            write_file(TROOT "/content/guide_model.bin", "NEWER-BODY");
            snprintf(txt, sizeof txt,
                     "{\"schema\":1,\"artifacts\":[{\"kn\":\"model\","
                     "\"file\":\"guide_model.bin\",\"size\":10,"
                     "\"sha256\":\"%s\",\"version\":3}]}", SHA_NEWER);
            snprintf(mf, sizeof mf, "%s/content/manifest.json", TROOT);
            write_file(mf, txt);

            /* 先把版本记录推到 2，好让 v3 能装上去 */
            write_file(TROOT "/ver_model.txt", "2\n");
            r = cu_scan_local(&m, &st, msg, sizeof msg);
            ck(r == 1, "v3 比已装的 v2 新 → 可更新");
            ck(cu_install(&m, 0, msg, sizeof msg), "装上 v3");
            ck(read_int(TROOT "/ver_model.txt") == 3, "版本记录推进到 3");

            /* 现在拿 v1 的包 —— 真降级 */
            snprintf(txt, sizeof txt,
                     "{\"schema\":1,\"artifacts\":[{\"kn\":\"model\","
                     "\"file\":\"guide_model.bin\",\"size\":4,"
                     "\"sha256\":\"%s\",\"version\":1}]}", SHA_HWHW);
            write_file(mf, txt);
            write_file(TROOT "/content/guide_model.bin", "hwhw");

            r = cu_scan_local(&m, &st, msg, sizeof msg);
            ck(r == 0, "v1 包对 v3 已装版本是降级 → 不触发更新");
            ck(read_int(TROOT "/ver_model.txt") == 3,
               "扫降级包不动版本记录");
            /* 降级包的文件内容不该被装进去 —— 这里包里的正文是
             * "hwhw"（4 字节），而正式文件应该还是 v3 的 "NEWER-BODY" */
            {
                int n3 = 0;
                ck(file_len(TROOT "/guide_model.bin", &n3) &&
                   n3 == (int)(sizeof "NEWER-BODY" - 1),
                   "降级包没有被装进去（正式文件仍是 v3 的内容）");
            }
        }

        /* ---- ④b version=0 的包应被当成结构不对，而不是降级 ---- */
        {
            char txt[512];
            int  r;
            snprintf(txt, sizeof txt,
                     "{\"schema\":1,\"artifacts\":[{\"kn\":\"model\","
                     "\"file\":\"guide_model.bin\",\"size\":4,"
                     "\"sha256\":\"%s\",\"version\":0}]}", SHA_HWHW);
            snprintf(mf, sizeof mf, "%s/content/manifest.json", TROOT);
            write_file(mf, txt);
            r = cu_scan_local(&m, &st, msg, sizeof msg);
            ck(r == 0 && st == CU_ERR_PARSE,
               "version=0 的包报「结构不对」而不是「降级」");
            ck(read_int(TROOT "/ver_model.txt") == 3,
               "结构不对的包不动版本记录");
        }

        /* ---- ⑤ 回滚 ----
         *
         * .bak 里现在是**上一份**（v2 之前那次装的是 v1 的正文
         * "hwhw"；再往上装 v3 时备份的正是它），不是最初的
         * OLD-CONTENT —— 因为每次装都把当前那份顶进 .bak，
         * 而 .bak 只保留**一层**。这是有意的：留多层要占
         * 1.25MB（模型那份）一份的空间，在 SD 卡上不值当。
         *
         * 所以这里的期望是"回到 v1 的内容"，不是"回到最初"。
         * 写成 OLD-CONTENT 会得到一条看起来像产品 bug 的失败 ——
         * 实际上"只留一层"是设计决定，不是缺陷。 */
        {
            char rmsg[192];
            int  n2 = 0;
            bool ok = cu_rollback(&m, 0, rmsg, sizeof rmsg);
            ck(ok, "cu_rollback 成功");
            ck(file_len(TROOT "/guide_model.bin", &n2) &&
               n2 == (int)(sizeof "hwhw" - 1),
               "回滚后内容是上一份（v1 的 hwhw）");
            ck(read_int(TROOT "/ver_model.txt") == 0,
               "回滚后版本归零（否则永远收不到修复版本）");
            ck(!file_len(TROOT "/guide_model.bin.bak", &n2),
               "回滚后 .bak 已被消耗");
            ck(!cu_rollback(&m, 0, rmsg, sizeof rmsg),
               "没有 .bak 时回滚必须失败而不是崩");
        }

        /* ---- ⑥ 正式文件存在时装一个摘要错的包：
         *       **正式文件必须原封不动**。这是本模块最重要的一条。 */
        {
            char txt[512];
            int  n2 = 0;
            write_file(TROOT "/guide_model.bin", "PRECIOUS");
            write_file(TROOT "/content/guide_model.bin", "CORRUPT");
            snprintf(txt, sizeof txt,
                     "{\"schema\":1,\"artifacts\":[{\"kn\":\"model\","
                     "\"file\":\"guide_model.bin\",\"size\":7,"
                     "\"sha256\":\"%s\",\"version\":9}]}", SHA_WRONG);
            snprintf(mf, sizeof mf, "%s/content/manifest.json", TROOT);
            write_file(mf, txt);

            /* 扫的时候就会因为摘要不符被挡住，根本进不到 install */
            {
                int r = cu_scan_local(&m, &st, msg, sizeof msg);
                ck(r == 0 && st == CU_ERR_MISMATCH,
                   "包内文件摘要不符时报 MISMATCH");
            }
            ck(file_len(TROOT "/guide_model.bin", &n2) &&
               n2 == (int)(sizeof "PRECIOUS" - 1),
               "被拒绝的更新没有动正式文件");
        }
    }

    /* 清理：把测试目录整个拆掉 */
    rm_r(TROOT);
    rmdir(TROOT "/content");
    rmdir(TROOT);
    cu_set_root(NULL);

    app_note("cupdtest: end pass=%d fail=%d", g_pass, g_fail);
    app_note("cupdtest: RESULT %s", g_fail == 0 ? "PASS" : "FAIL");
}
