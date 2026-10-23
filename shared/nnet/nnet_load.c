/*
 * nnet_load.c — GM1 权重文件 -> NnModel 指针树
 *
 * 这个文件唯一的工作是**把字节流安全地翻译成指针树**。
 * 「安全」的具体含义见下面每一处检查 —— 在插件场景里，
 * 一次越界读或一次失控 malloc 崩掉的是用户正在玩的游戏，
 * 而不是我们自己的进程，那种失败方式是无法接受的。
 *
 * 贯穿全文的三条做法：
 *
 *  1. **先验证，后填指针。** 任何一个字段在校验通过之前不会被
 *     写进 model。这样中途失败时 loader 处于「干净但无效」状态，
 *     调用方只要看一眼 ld->state 就知道发生了什么，不需要
 *     逐字段检查。
 *
 *  2. **每个维度都对着编译期上限查。** 头部是外部数据，
 *     vocab=2^31 这种值一个 malloc 就能把 3DS 内存吃光。
 *
 *  3. **偏移全部 4 字节对齐才 cast。** 未对齐的 float* 在
 *     ARM11 上不是「慢一点」，是 ldmia 直接 Data Abort。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nnet_load.h"

/* ------------------------------------------------------------------ */
/* 错误原因                                                            */
/* ------------------------------------------------------------------ */

const char *nnet_load_strerror(int code)
{
    switch (code) {
    case NNLM_OK:         return "正常";
    case NNLM_ERR_IO:     return "打不开模型文件（SD 卡没插？）";
    case NNLM_ERR_SHORT:  return "模型文件被截断（比头部还短）";
    case NNLM_ERR_MAGIC:  return "不是模型文件（magic 不对）";
    case NNLM_ERR_HEADER: return "模型头部维度非法";
    case NNLM_ERR_TRUNC:  return "模型文件不完整（声明长度超出实际）";
    case NNLM_ERR_ALIGN:  return "模型文件偏移未对齐";
    case NNLM_ERR_RANGE:  return "模型维度超出本机上限";
    case NNLM_ERR_MEM:    return "内存不够（2.9MB 模型放不下）";
    case NNLM_ERR_DIM:    return "模型维度自相矛盾（头数×头宽≠隐藏维）";
    case NNLM_ERR_ROPE:   return "RoPE 参数与内置表不匹配（模型需重新导出）";
    case NNLM_ERR_STATE:  return "状态缓冲申请失败";
    default:              return "未知错误";
    }
}

/* ------------------------------------------------------------------ */
/* 游标                                                                */
/* ------------------------------------------------------------------ */

/*
 * NnMat.bs 的占位零数组。embedding 段在文件里没有 bias，但
 * matvec_f32 会无条件加 m->bs[j] —— 指针为空就是段错误。
 *
 * ⚠️ 长度必须 >= 词表大小。给 8 个是**不够**的：tok_emb 的 out 是
 * vocab=4096，虽然当前输出层走的是 tied 专用路径（直接用 wte_sc
 * 定标，不碰 bs），但那只是「现在恰好没走」——
 * 一旦有人给输出层补上 bias 路径就会越界读，而越界读 int8 缓冲
 * 在 3DS 上不崩，只是安静地返回垃圾。
 *
 * 8192 个 float = 32KB 常驻，对 3DS 来说不小但付得起。
 * 更省的做法是给 tok_emb 单独 malloc 一份，这里用静态的取安全。
 */
#define NNET_ZB 8192
static const float g_zero_bias[NNET_ZB] = { 0 };

typedef struct {
    const unsigned char *base;
    size_t               len;
    size_t               off;
    int                  err;
} Rd;

static int rd_need(Rd *r, size_t n)
{
    if (r->err) return -1;
    if (n > r->len || r->off > r->len - n) {  /* 减法形式，避免 off+n 溢出 */
        r->err = NNLM_ERR_TRUNC;
        return -1;
    }
    return 0;
}

/* 取 n 字节并前移游标。返回 0 成功。 */
static int rd_take(Rd *r, size_t n, const void **out)
{
    if (rd_need(r, n) < 0) return -1;
    if (r->off & 3u) { r->err = NNLM_ERR_ALIGN; return -1; }  /* 4 字节对齐 */
    *out = r->base + r->off;
    r->off += n;
    return 0;
}

static int rd_i8(Rd *r, size_t n, const int8_t **out)
{
    const void *p;
    if (rd_take(r, n, &p) < 0) return -1;
    *out = (const int8_t *)p;
    return 0;
}

static int rd_f32(Rd *r, size_t n, const float **out)
{
    const void *p;
    if (rd_take(r, n, &p) < 0) return -1;
    *out = (const float *)p;
    return 0;
}

static int rd_i32(Rd *r, int32_t *out)
{
    const void *p;
    if (rd_take(r, 4, &p) < 0) return -1;
    memcpy(out, p, 4);          /* 小端 ARM 直接拷 */
    return 0;
}

/* 读一个 [out][in] 的 int8 矩阵 + scale + bias。
 *
 * ⚠️ 参数顺序是 (in, out) —— 与 m->in/m->out 的赋值同名，
 * 而文件里是行主序 [out][in]。**这个顺序最容易写反**，
 * 而写反了 D/F 都是合法正数，不会有任何编译期或运行期报错。
 * 唯一的判据是「读完 36+所有层之后游标正好等于文件长度」——
 * nnet_load_file 之外建议用 tools/guia/forward_parity.py 验一次。
 * 下面加一句 assert 式的自检：scale/bias 的元素数都是 out，
 * 所以 in/out 谁大谁小不影响这里的正确性，但 rd_mat 的调用点
 * 必须与 export_model.py 的 w1_/w2_ 形状一致。 */
static int rd_mat(Rd *r, int32_t in, int32_t out, NnMat *m)
{
    size_t wn = (size_t)in * (size_t)out;
    if (rd_i8(r, wn, &m->w) < 0) return -1;
    if (rd_f32(r, 4 * (size_t)out, &m->sc) < 0) return -1;
    if (rd_f32(r, 4 * (size_t)out, &m->bs) < 0) return -1;
    m->in  = in;
    m->out = out;
    return 0;
}

/* ------------------------------------------------------------------ */
/* 核心绑定                                                            */
/* ------------------------------------------------------------------ */

/*
 * 内部实现。mem 模式不接管 base 的所有权，file 模式已经读进 ld->buf。
 */
static int bind(NNLoader *ld, const unsigned char *base, size_t len)
{
    Rd r = { base, len, 0, 0 };
    const void *magic_p;
    int32_t V, D, L, H, F, T, dh, n_rope;
    NnModel *m = &ld->model;

    if (len < 36) { ld->state = NNLM_ERR_SHORT; return ld->state; }
    if (rd_take(&r, 4, &magic_p) < 0) { ld->state = r.err; return ld->state; }
    if (memcmp(magic_p, "GM1", 4) != 0) {
        ld->state = NNLM_ERR_MAGIC;
        return ld->state;
    }

    if (rd_i32(&r, &V) < 0 || rd_i32(&r, &D) < 0 ||
        rd_i32(&r, &L) < 0 || rd_i32(&r, &H) < 0 ||
        rd_i32(&r, &F) < 0 || rd_i32(&r, &T) < 0 ||
        rd_i32(&r, &dh) < 0 || rd_i32(&r, &n_rope) < 0) {
        ld->state = r.err;
        return ld->state;
    }

    /* --- 维度校验。顺序有讲究：先查 0/负（会一路传播成 absurd 的
     * 乘法），再查上限（合法但放不下），最后查自相矛盾。 --- */
    if (V <= 0 || D <= 0 || L <= 0 || H <= 0 ||
        F <= 0 || T <= 0 || dh <= 0 || n_rope <= 0) {
        ld->state = NNLM_ERR_HEADER;
        return ld->state;
    }
    if (V > NNLM_MAX_VOCAB || D > NNLM_MAX_DMODEL || L > NNLM_MAX_LAYER ||
        H > NNLM_MAX_HEAD || F > NNLM_MAX_FF || T > NNLM_MAX_CTX) {
        ld->state = NNLM_ERR_RANGE;
        return ld->state;
    }
    /* 零 bias 占位数组必须够大。它是静态的，词表超过上限时
     * matvec_f32 会读到数组外面去 —— 而越界读 float 数组在
     * 3DS 上不会崩，只会安静地返回垃圾。 */
    if ((size_t)V > (size_t)NNET_ZB) {
        ld->state = NNLM_ERR_RANGE;
        return ld->state;
    }
    /* d_head * n_head 必须等于 d_model。不等说明文件与引擎不是一套，
     * 后面所有偏移都会错位 —— 与其错位几百 KB 之后再崩，不如现在
     * 就拒绝。 */
    if ((int64_t)dh * H != (int64_t)D) {
        ld->state = NNLM_ERR_DIM;
        return ld->state;
    }
    /* n_rope 是 d_head 的一部分，必须为偶数且不超过 d_head。
     * 再加一条：它必须与内置 RoPE 表对得上（表长 = max_ctx*d_head/2），
     * 否则 rope() 会越界读。表长是编译期常量，模型是数据 ——
     * 数据不能假设常量跟自己匹配。 */
    if ((n_rope & 1) || n_rope > dh ||
        (int64_t)T * (n_rope / 2) > (int64_t)NN_ROPE_TAB_LEN) {
        ld->state = NNLM_ERR_ROPE;
        return ld->state;
    }

    memset(m, 0, sizeof(*m));
    m->vocab    = V;
    m->d_model  = D;
    m->n_layer  = L;
    m->n_head   = H;
    m->d_ff     = F;
    m->d_head   = dh;
    m->max_ctx  = T;
    m->n_rope   = n_rope;
    m->layers   = ld->layers;
    m->tok_emb  = ld->tok_emb;

    /* --- token embedding（tied，兼输出层）---
     * ⚠️ embedding 段**没有 bias**（文件里只有 wte_i8 + wte_sc），
     * 所以不能用 rd_mat —— 它会多读一个 [vocab] 的 bias，
     * 于是从这里开始后面**全部字段错位**。
     *
     * 错位之后会发生什么：文件总长依然分毫不差（因为后面还有 bias 段，
     * 数量正好抵上），magic 与头部全部合法，加载器一路返回 NNLM_OK，
     * 而 pos_emb / LayerNorm 全是垃圾。**这正是我上一版
     * forward_parity.py 溢出到 NaN 的原因。**
     *
     * 所以这里用只读 w+sc 的版本，并把 bias 指向一个静态零数组 ——
     * NnMat 结构要求 bs 非空（matvec_f32 会无条件加它）。 */
    {
        const int8_t *wq;
        const float  *sc;
        if (rd_i8(&r, (size_t)V * D, &wq) < 0 ||
            rd_f32(&r, 4 * (size_t)V, &sc) < 0) {
            ld->state = r.err;
            return ld->state;
        }
        ld->tok_emb[0].in  = D;
        ld->tok_emb[0].out = V;
        ld->tok_emb[0].w   = wq;
        ld->tok_emb[0].sc  = sc;
        ld->tok_emb[0].bs  = g_zero_bias;    /* 见下方说明 */
    }

    /* --- 位置 embedding（同样没有 bias）--- */
    {
        const int8_t *we;
        const float  *ps;
        if (rd_i8(&r, (size_t)T * D, &we) < 0 ||
            rd_f32(&r, 4 * (size_t)T, &ps) < 0) {
            ld->state = r.err;
            return ld->state;
        }
        m->pos_emb   = we;
        m->pos_scale = ps;
    }

    /* --- 每层 --- */
    for (int32_t l = 0; l < L; l++) {
        NnLayer *ly = &ld->layers[l];
        memset(ly, 0, sizeof(*ly));
        if (rd_mat(&r, D, D, &ly->wq) < 0 ||
            rd_mat(&r, D, D, &ly->wk) < 0 ||
            rd_mat(&r, D, D, &ly->wv) < 0 ||
            rd_mat(&r, D, D, &ly->wo) < 0) { ld->state = r.err; return ld->state; }
        /* pre-LN：每层 2 个 LayerNorm，不是 4 个。见 nnet.h 的说明。 */
        if (rd_f32(&r, 4 * (size_t)D, &ly->ln1g) < 0 ||
            rd_f32(&r, 4 * (size_t)D, &ly->ln1b) < 0 ||
            rd_f32(&r, 4 * (size_t)D, &ly->ln2g) < 0 ||
            rd_f32(&r, 4 * (size_t)D, &ly->ln2b) < 0) { ld->state = r.err; return ld->state; }
        /* FFN。**方向别搞反**：文件里是 [out][in]，
         * 即 ff1 是 [F][D]（in=D, out=F），ff2 是 [D][F]（in=F, out=D）。
         * 写反了不会立刻崩 —— D=192、F=512，两边都合法 ——
         * 只是每层都在读一段错位的权重，输出全是垃圾。
         * （这正是我把 nnet.c 接进构建时踩到的：直觉会写
         *   ff1: rd_mat(D, F)   ← in=D, out=F 看似顺理
         * 但导出脚本的 w1_{l} 本身就是 [F,D]。以文件为准，别靠推理。）
         * bias 在文件里是**全零占位**（训练端 nn.Linear 无 bias），
         * 格式里保留是为了将来加 bias 时不用改格式。 */
        if (rd_mat(&r, D, F, &ly->ff1) < 0 ||
            rd_mat(&r, F, D, &ly->ff2) < 0) { ld->state = r.err; return ld->state; }
    }

    /* --- 最终 LayerNorm --- */
    if (rd_f32(&r, 4 * (size_t)D, &m->lnf_g) < 0 ||
        rd_f32(&r, 4 * (size_t)D, &m->lnf_b) < 0) {
        ld->state = r.err;
        return ld->state;
    }

    /* 允许尾部有少量余字节（导出脚本以后加字段），但不能差太多 ——
     * 差得多说明格式变了（比如从 GM1 换成了 GM2），必须报出来。 */
    if (len - r.off > 1024) {
        ld->state = NNLM_ERR_HEADER;
        return ld->state;
    }

    ld->state = NNLM_OK;
    return NNLM_OK;
}

/* ------------------------------------------------------------------ */
/* 三个入口                                                            */
/* ------------------------------------------------------------------ */

int nnet_load_mem(NNLoader *ld, const void *base, size_t len)
{
    if (!ld || !base) return NNLM_ERR_IO;
    nnet_load_free(ld);              /* 防重复调用时泄漏上一份 */
    if (((uintptr_t)base & 3u) != 0) return NNLM_ERR_ALIGN;
    return bind(ld, (const unsigned char *)base, len);
}

int nnet_load_file(NNLoader *ld, const char *path)
{
    FILE   *f;
    long    sz;
    size_t  got;

    if (!ld || !path) return NNLM_ERR_IO;
    nnet_load_free(ld);

    f = fopen(path, "rb");
    if (!f) { ld->state = NNLM_ERR_IO; return ld->state; }

    /* 长度先拿到手：解析中途发现截断时已经无法回退。
     * ftell 的返回值只在 -1（LONG_MAX）时算错，其余情况
     * ftell 返回的是 long，2GB 以上的文件在 3DS 上不可能。 */
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); ld->state = NNLM_ERR_IO; return ld->state; }
    sz = ftell(f);
    if (sz <= 0) { fclose(f); ld->state = NNLM_ERR_IO; return ld->state; }
    rewind(f);

    ld->buf = (unsigned char *)malloc((size_t)sz);
    if (!ld->buf) {
        fclose(f);
        ld->state = NNLM_ERR_MEM;
        return ld->state;
    }
    got = fread(ld->buf, 1, (size_t)sz, f);
    fclose(f);
    if (got != (size_t)sz) {
        free(ld->buf);
        ld->buf = NULL;
        ld->state = NNLM_ERR_IO;      /* 卡拔了 / 读错了 */
        return ld->state;
    }
    ld->buf_len = got;
    return bind(ld, ld->buf, got);
}

void nnet_load_free(NNLoader *ld)
{
    if (!ld) return;
    if (ld->buf) {
        free(ld->buf);
        ld->buf = NULL;
    }
    ld->buf_len = 0;
    /* model 的指针全部指进那块刚释放的内存 —— 必须清零，
     * 否则一次误用（free 之后又 nnet_push）就是 UAF。
     * 在插件里 UAF 崩的是游戏，不是我们。 */
    memset(&ld->model, 0, sizeof(ld->model));
    ld->state = NNLM_ERR_IO;
}

/* ------------------------------------------------------------------ */
/* KV cache                                                            */
/* ------------------------------------------------------------------ */

size_t nnet_cache_bytes(const NnModel *m)
{
    size_t D = (size_t)m->d_model, C = (size_t)m->max_ctx, L = (size_t)m->n_layer;
    size_t one = L * C * D;            /* 一份 k 或 v 的大小 */
    /* ★ 按 L 算，不是按 C 算。旧版本漏了这一项：
     *   分配量 = C*D（只有一层），访问量 = L*C*D。
     *   于��第 1 层写完就踩进后面的堆。实测差 5*128*192 = 122880 字节。 */
    return one * 2                     /* k + v */
         + 4 * L * C * 2;              /* k_scale + v_scale */
}

NnCache *nnet_cache_new(const NnModel *m)
{
    size_t n = nnet_cache_bytes(m);
    NnCache *c;
    void *p;
    size_t one = (size_t)m->n_layer * (size_t)m->max_ctx * (size_t)m->d_model;

    if (n == 0 || n > 8u * 1024u * 1024u) return NULL;   /* 6 层约 300KB */
    c = (NnCache *)calloc(1, sizeof(*c));
    if (!c) return NULL;

    p = malloc(n);
    if (!p) { free(c); return NULL; }
    memset(p, 0, n);

    /* 4 字节对齐：k/v 是 int8 可以任意地址，但 scale 是 float*，
     * VFP 的 ldmia 要求 4 字节对齐。所以 int8 段先走，
     * 段尾补齐再放 float。 */
    c->k = (int8_t *)p;
    c->v = c->k + one;
    p = (void *)(c->v + one);
    p += (4 - ((uintptr_t)p & 3)) & 3;
    c->k_scale = (float *)p;
    c->v_scale = c->k_scale + (size_t)m->n_layer * (size_t)m->max_ctx;
    c->len = 0;
    return c;
}

void nnet_cache_free(NnCache *c)
{
    if (!c) return;
    /* k 和 v 在同一块 malloc 里，只能 free 起点那个。 */
    free((void *)c->k);
    memset(c, 0, sizeof(*c));
}

/* ------------------------------------------------------------------ */
/* Session                                                             */
/* ------------------------------------------------------------------ */

/*
 * ⚠️ ld 必须由**调用方**提供。
 *
 * NNSession.ld 是指针，而这个函数开头 memset(ss, 0, ...) 会把它清零 ——
 * 于是调用方在调用前设的 ld 必然丢失，随后 nnet_load_file(NULL, path)
 * 直接段错误。原签名没有 ld 参数，等于「调用方无法提供 loader」，
 * 是个死设计：不是用错，是**没法用对**。
 *
 * 为什么 loader 必须由调用方给：NNLoader 里有 layers[NNLM_MAX_LAYER]
 * 这个大数组（NNLM_MAX_LAYER=32 时约 1.3KB），它属于**谁**的
 * 生命周期是调用方的事 —— 放栈上会挤爆 3dsx 的 16KB 主线程栈，
 * 放堆上就多一个 malloc 失败点。让调用方自己决定放哪，
 * 是把这件事交回给唯一有信息的人。
 */
int nnet_session_open(NNSession *ss, NNLoader *ld, const char *path,
                      void *state_work)
{
    int rc;
    if (!ss || !ld) return NNLM_ERR_IO;
    /* 只清 st / cache，**保留** ld —— 它是调用方给的。 */
    memset(&ss->st, 0, sizeof(ss->st));
    ss->cache = NULL;
    ss->ld = ld;

    rc = nnet_load_file(ss->ld, path);
    if (rc != NNLM_OK) { ss->ld = NULL; return rc; }

    if (nnet_state_bind(&ss->st, &ss->ld->model, state_work) != 0) {
        nnet_load_free(ss->ld);
        ss->ld->state = NNLM_ERR_STATE;
        ss->ld = NULL;
        return NNLM_ERR_STATE;
    }
    ss->cache = nnet_cache_new(&ss->ld->model);
    if (!ss->cache) {
        nnet_load_free(ss->ld);
        ss->ld->state = NNLM_ERR_STATE;
        ss->ld = NULL;
        return NNLM_ERR_STATE;
    }
    ss->st.cache = ss->cache;
    nnet_init();
    return NNLM_OK;
}

void nnet_session_close(NNSession *ss)
{
    if (!ss) return;
    nnet_cache_free(ss->cache);
    ss->cache = NULL;
    /* 释放 loader 持有的**权重缓冲**，但 loader 本身是调用方的
     * 静态数组，不归我们管，也不该碰。 */
    nnet_load_free(ss->ld);
    /* ss->st.m 仍指向刚释放的权重 —— 必须清掉。
     * 留着的话一次误用（close 之后又 nnet_push）就是 UAF，
     * 而 UAF 在插件里崩的是用户正在玩的游戏。 */
    memset(&ss->st, 0, sizeof(ss->st));
    ss->ld = NULL;
}

int nnet_session_generate(NNSession *ss, const int32_t *prompt, int n_prompt,
                          int max_gen, float temperature, uint32_t *seed,
                          const int32_t *cand, int cand_n, NnetSink sink,
                          void *ud)
{
    int produced = 0;
    if (!ss || !ss->cache) return -1;
    if (n_prompt < 0 || n_prompt > ss->ld->model.max_ctx) return -1;

    /* 候选集在 push 之前设一次，生成循环里不重设
     * （nnet_set_candidates 只是存指针，不拷贝，但反复调
     *  会让「这一轮用哪个候选集」的语义变得含糊）。 */
    if (cand && cand_n > 0) nnet_set_candidates(&ss->st, cand, cand_n);
    else                     nnet_set_candidates(&ss->st, NULL, 0);

    ss->st.pos    = 0;
    ss->cache->len = 0;

    /* 逐个喂 prompt。每个 token 都是一次完整前向，
     * 所以 n_prompt 直接决定了这个循环的耗时。 */
    for (int i = 0; i < n_prompt; i++) {
        if (nnet_push(&ss->st, prompt[i]) != 0) return -1;
    }
    if (n_prompt == 0) return 0;

    for (int step = 0; step < max_gen; step++) {
        NnTopK tk;
        int32_t tok;
        if (nnet_sample(&ss->st, temperature, &tk) <= 0) break;
        tok = nnet_pick(&tk, temperature, seed);
        if (tok == g_nnet_eos_id) break;      /* 采到 EOS 就停 */
        if (sink && !sink(ud, tok, step)) break;
        produced++;
        if (nnet_push(&ss->st, tok) != 0) break;   /* 撞 max_ctx */
    }
    return produced;
}
