/*
 * nnet.c — 3DS 离线微模型推理引擎（内核）
 *
 * 性能预算（实测口径见 tools/guia/bench_host.py，宿主 PC 只是量级参考）
 * ------------------------------------------------------------------
 * ARM11 @804MHz 估有效吞吐 ~150 MMAC/s。3.25M 参数 -> 每 token 2*3.25M MAC
 * = 6.5M MAC -> 约 23 tok/s（New 3DS）。Old 3DS 268MHz -> 约 7.6 tok/s。
 *
 * 所以内核里所有能省 MAC 的地方都要省：
 *   1. int8 点积替代 fp32 —— ARM11 的乘法器本来就是 32 位，
 *      用 int8 一次能累 4 个乘积再进位，等效吞吐翻 4 倍。
 *   2. RoPE 只对 d_head 的前 n_rope 维做旋转，省掉 cos/sin 的重复计算。
 *   3. KV cache 存 int8，注意力点积也在 int8 域做。
 *   4. 权重按行 16 字节对齐，让 LDRD 一次能搬两个 int8x8 块。
 *
 * 关于 4 次展开：ARM11 是双发射 in-order，MLA 依赖前一条 MLA 的累加器，
 * 单靠编译器很难填满流水。这里手工写 4 路累加，靠寄存器旁路撑 ILP。
 */
#include "nnet.h"
#include "nnet_math.h"   /* 定点expf/tanhf/rsqrtf，见该文件头的ARM11 分析 */
#include <string.h>
#include <math.h>

/* 必须在 m 之前声明：.rodata 里的小表 */
extern const float g_nnet_rope_cos[];
extern const float g_nnet_rope_sin[];
extern const int8_t  g_nnet_start_byte[256];
extern const int32_t g_nnet_bos_id;
extern const int32_t g_nnet_eos_id;

#define NNET_MAX_CTX 256

/* ------------------------------------------------------------------ */
/* int8 dot product                                                     */
/* ------------------------------------------------------------------ */

/*
 * acc += sum(w[i] * x[i])，w/x 都是 int8。
 * 返回值是 int32 累加和（不缩放）。
 *
 * 4 路累加的关键：ARM11 的 MLA 读的是前一条指令的累加器结果，
 * 所以 4 条 MLA 之间存在 4 拍的依赖。手工交错成 4 个独立累加器
 * （a0..a3），编译器就会用满 4 个独立累加器流水。
 *
 * ══ 反汇编实测（objdump -j .text.matvec_f32 / .text.dot_i8_i8）══════
 *
 * int8 在 ARM11 上**没有向量加载**，只能逐字节 `ldrsb`，
 * 而 `smlabb` 要的是半字。所以喂满一条 MLA 需要 2 次 ldrsb
 * （w 一个字节、x 一个字节）。gcc 已经做对了该做的：
 * 用了 DSP 扩展、用了独立累加器、没有退化成 int32 乘法。
 *
 * ⚠️⚠️ **试过 8 路展开，更慢，已改回 4 路。**
 *
 * 纸面推算很漂亮：指针递增和 cmp/bne 都能摊薄一半，
 * 「每 4 MAC 16 条 → 每 8 MAC 28 条，约 12%」。
 * **实测反汇编打脸：8 路版本的循环体里出现了 5 条 str + 5 条 ldr** ——
 * ARM11 只有 16 个寄存器，8 个累加器 + 2 个指针 + w/x 基址
 * 把它挤爆了，gcc 只好把 5 个累加器溢出到栈上再读回来。
 * 那是**访存**，而访存正是这里的瓶颈。净指令数从
 * 4.00/MAC 变成 3.25/MAC，看着少了，但多出来的全是慢指令。
 *
 * 这就是「数字好看不等于更快」的一个具体例子：只数总条数会
 * 把一条 `str`（写内存）和一条 `add`（寄存器）当成等价的东西，
 * 而在这台机器上前者可能比后者贵一个数量级。
 *
 * → **判据必须是「有没有 str/ldr 混进循环体」，不能只是总条数。**
 *
 * ⚠️ 也不要试图「一次算 2 个输出通道」来摊薄 x 的加载：
 * 那样要同时读 w 的两行，而两行相隔 `in` 字节（128 或 384），
 * 等于开两条流。ARM11 的 L1 只有 32KB，双流会让预取命中率下降，
 * 而访存正是瓶颈。纸上好看、实测大概率变慢，无数据不要写。
 */
#ifdef NNET_DOT_VIA_LDR
/*
 * 实现见下方「同一个点积，但用 ldr + sxtb」那段。
 * 两版并存用宏切换，是为了让**对比是同一份代码里的 A/B**，
 * 而不是"改一份、量一次、再改回来、量一次" ——
 * 后者两次构建的其它差异（对齐、链接顺序）会混进结果里。
 */
static inline int32_t dot_i8_i8(const int8_t *w, const int8_t *x, int n);
#else
static inline int32_t dot_i8_i8(const int8_t *w, const int8_t *x, int n)
{
    int32_t a0 = 0, a1 = 0, a2 = 0, a3 = 0;
    int i = 0;
    for (; i + 4 <= n; i += 4) {
        a0 += (int32_t)w[i + 0] * (int32_t)x[i + 0];
        a1 += (int32_t)w[i + 1] * (int32_t)x[i + 1];
        a2 += (int32_t)w[i + 2] * (int32_t)x[i + 2];
        a3 += (int32_t)w[i + 3] * (int32_t)x[i + 3];
    }
    int32_t s = a0 + a1 + a2 + a3;
    for (; i < n; i++)
        s += (int32_t)w[i] * (int32_t)x[i];
    return s;
}
#endif

/*
 * 同一个点积，但用 **ldr + sxtb** 一次取 4 个字节。
 *
 * ══ 动机：ldrsb 一次只搬 1 字节 ═══════════════════════════════════
 *
 * 上面那版每 4 个 MAC 要 4 条 ldrsb，也就是 **4 次访存**。
 * 而 ARM11 的一条 ldr 能搬 4 字节 —— 访存次数可以降到 1/4。
 * 访存正是这里的瓶颈（perf_count.py 测出 L2 装不下 640KB 主干，
 * 每 token 都要从 RAM 重读），所以少搬字节比少几条指令更值钱。
 *
 * 拆字节用 sxtb Rd, Rm, #rot（ARMv6K 的带移位字节符号扩展，单周期）：
 *   ldr  t, [w, #0]     ; 取 4 字节
 *   sxtb b0, t, #24     ; 高字节（有符号）
 *   sxtb b1, t, #16
 *   sxtb b2, t, #8
 *   sxtb b3, t, #0
 *
 * → 指令数从 4 条 ldrsb + 4 条乘法 变成 1 条 ldr + 4 条 sxtb + 4 条乘法。
 * **指令数多了 1 条，访存少了 3 次。** 这个交换在访存受限时是赚的。
 *
 * ⚠️ 前提是 **w 每 4 字节对齐**：
 *   in=128（wq/wk/wv/wo）与 in=384（ff1/ff2）都是 4 的倍数，
 *   而行起点是 w + j*in，所以只要 m->w 本身 4 字节对齐就成立。
 *   不满足时 ldr 会触发 alignment fault —— 那是**崩**，不是变慢。
 *   下面的 assert 挡的就是这个。
 *
 * ⚠️ 这里也解释了为什么 8 路展开会输：它输在**寄存器不够**导致 spill
 * （str/ldr 混进循环体）。而这一版完全不增加寄存器压力 ——
 * 4 个累加器不变，只多一个临时的 32 位容器（sxtb 完立刻可用）。
 * 也就是说它**踩的是 8 路那版的坑的反面**：用访存换指令，
 * 而不是用寄存器换指令。
 *
 * ══ 实测结论（2026-10-04）：**默认不要开它**，收益为 0 ══════════════
 *
 * 完整 A/B（同一问句、同一随机种子、同一台机器、只切这一个宏）：
 *
 *   版本                          tick        输出字节   相对基线
 *   基线（上面那版 ldrsb）        431459452   122        -
 *   本版（ldr + sxtb）            432045724   122        **+0.14%（更慢）**
 *
 * w 侧访存次数确实从 4 降到 1（反汇编可核），而**整段生成时间毫无改善**。
 *
 * 为什么访存降 4 倍却不省时间 —— 这否定了"每条访存指令都贵"这个假设：
 *   真正的开销是**缓存未命中**，不是访存指令本身。
 *   权重的一行 16KB，w 的 4 个相邻字节本来就在**同一条 cache line** 里，
 *   第 1 条 ldrsb 把这行拉进 cache 之后，后 3 条就是**cache 命中** ——
 *   它们本来就便宜。换成 1 条 ldr + 4 条 sxtb，省掉的是 3 次
 *   本来就便宜的 cache 命中，换来 3 条额外指令。**净亏。**
 *
 * 顺带修正一条此前的认知。perf_count.py 说「访存是瓶颈不是乘法」，
 * 那个结论对 **L2 未命中的主干重读**是成立的（每 token 从 RAM 搬 640KB），
 * 但**不能推广成「点积内部每条 ldrsb 都贵」**。这两层必须分开看：
 *   · RAM ← L2 的搬运：每 token 一次，量大      ← 真瓶颈
 *   · L1/L2 内部的字节提取：每 MAC 一次，命中    ← 便宜，不值得优化
 *
 * ⚠️ 保留本函数不是为了将来启用它，而是为了让**下一个改这里的人
 * 不用重新推导一遍**。真要提速，方向是减少**未命中**
 * （int4 量化把主干 640KB 压到 320KB，刚好装进 512KB 的 L2），那要重训。
 *
 * ⚠️⚠️ 顺带记一个差点上当的坑。第一版我把字节序写反了：
 * 3DS 是**小端**，w[i+0] 是最低字节，我当成了高字节，
 * 于是点积的下标错位一格 —— 程序不崩不报错，数字量级也正常，
 * 但模型提前采到 EOS，**输出从 122 字节缩到 19 字节**，
 * 耗时「下降 34%」。那不是优化，是**模型坏了所以少干活**。
 * 详见 app/source/test/perfprobe.c：RAW 那一行为什么必须同时记字节数。
 */
#ifdef NNET_DOT_VIA_LDR
static inline int32_t dot_i8_i8(const int8_t *w, const int8_t *x, int n)
{
    int32_t a0 = 0, a1 = 0, a2 = 0, a3 = 0;
    int i = 0;

    /* n 是 4 的倍数（128 / 384），但**不假设**它 ——
     * 尾数单独处理。假设比不假设危险：n 一旦变成 127，
     * 按 4 倍数走会读越界，而越界读 int8 往往读得到合法值，
     * 于是结果只是"微微错一点"，完全没有症状。 */
    for (; i + 4 <= n; i += 4) {
        uint32_t t;
        int32_t b0, b1, b2, b3;

        memcpy(&t, w + i, 4);          /* 编译成一条 ldr */
        /* ⚠️⚠️ 字节序：3DS 是**小端**，w[i] 是 t 的**最低**字节。
         *
         * 所以必须：
         *     b0 = 低字节  = w[i+0]
         *     b1 = 次低     = w[i+1]
         *     b2 = 次高     = w[i+2]
         *     b3 = 高字节  = w[i+3]
         *
         * 我第一版写反了（b0 取 t>>24），而 dot product 恰好是
         * **对下标配对极其敏感**的运算：b0=x[i+0] 实际配的是 w[i+3]·x[i+0]，
         * 全都错位一格。
         *
         * 后果的形态极其恶劣：
         *   · 程序跑得好好的，不崩不报错；
         *   · 数字仍然在合理量级（只是错位，不是乱码）；
         *   · **输出会变短** —— 模型提前采到 EOS，19 字节 vs 122 字节；
         *   · 于是"耗时下降 34%"看起来像个**大成功**，
         *     而它其实只是**少干了活**。
         * 一度差点拿这个数字去汇报优化成果。
         *
         * → 教训：**性能数字必须与输出一致**才可信。
         * 任何"变快了"的结论，都要先确认"活也没少干"。
         * 判据：输出字节数/token 数应与基线一致。
         * 这一条已固化到 perfprobe 的 RAW 那一行里（它同时记字节数）。
         */
        b0 = (int8_t)(t      );
        b1 = (int8_t)(t >>  8);
        b2 = (int8_t)(t >> 16);
        b3 = (int8_t)(t >> 24);

        a0 += b0 * (int32_t)x[i + 0];
        a1 += b1 * (int32_t)x[i + 1];
        a2 += b2 * (int32_t)x[i + 2];
        a3 += b3 * (int32_t)x[i + 3];
    }
    {
        int32_t s = a0 + a1 + a2 + a3;
        for (; i < n; i++)
            s += (int32_t)w[i] * (int32_t)x[i];
        return s;
    }
}
#endif

/*
 * 动态 per-tensor 量化：out[i] ≈ x[i] / scale，返回 scale。
 *
 * ⚠️⚠️ 本函数存在的理由是 nnet.c 里曾经写着一句**错的**假设，
 * 代价是整个模型静默失效：
 *
 *     "激活的隐含 scale 是 1：LayerNorm 输出是 N(0,1)，量级 ±5，
 *      直接当 int8 整数值用，误差 ±0.5/127 ≈ 0.4%。"
 *
 * 前半句对（LayerNorm 输出确实 N(0,1)），**结论错**。
 * 「scale 1」只对 LayerNorm 的输出成立，而量化发生在**投影之后**：
 *
 *     张量            真实 absmax    直接 int8 后非零率
 *     h1 = LN(x)      2.5            35.9%   （勉强能用）
 *     q  = h1@wq.T    2.6e-4          0.0%   ← 全零
 *     k  = h1@wk.T    3.1e-4          0.0%   ← 全零
 *     v  = h1@wv.T    1.6e-1          0.0%   ← 全零
 *     ff1 = gelu(..)  1.4e-1          0.0%   ← 全零
 *
 * 原因在 checkpoint 本身：wq/wk 的 absmax 只有 1.9e-5（训练时权重
 * 衰减把 q/k 压得很小，这是训练结果不是 bug），而 wv/wo/w1/w2 是
 * 1e-2 量级。差三个数量级，于是投影输出远小于 1，四舍五入全变 0。
 *
 * 后果：q=k=0 -> 注意力 score 全 0 -> softmax 均匀 -> 位置信息全丢；
 * v=0、ff1=0 -> 注意力输出与 FFN 输出全 0。**整个前向退化成
 * 只剩 embedding 和 LayerNorm。** 而程序照样跑、照样出字、零告警
 * —— 「零告警 / 不崩溃 / 能出字」三条判据全部通过。
 *
 * 修法：每个张量在量化前扫一遍求 absmax，scale = absmax/127，
 * 反量化时乘回。K/V 因为要进 cache，scale 存 NnCache.k_scale/v_scale
 * （这两个字段一直都在，只是此前没人写它们）。
 *
 * 代价：每个张量多一趟 O(n) 的 max 扫描。相对同张量的 matvec
 * （D*out MAC）不到 1%，而换来的是用满 int8 的全部码位
 * （0.79% 步进而不是 100%）。
 */
static float quant_dyn(const float *x, int n, int8_t *out)
{
    float amax = 0.0f;
    for (int i = 0; i < n; i++) {
        float a = (x[i] < 0.0f) ? -x[i] : x[i];
        if (a > amax) amax = a;     /* NaN 比较恒假 -> amax 保持 0，安全 */
    }
    /* amax==0（全零张量）或 amax 极小时给 1.0：
     * 不能给 0，那会让 inv=Inf 然后 0*Inf=NaN。 */
    float s = amax * (1.0f / 127.0f);
    if (!(s > 1e-20f)) s = 1.0f;
    float inv = 1.0f / s;
    for (int i = 0; i < n; i++) {
        float t = x[i] * inv;
        int iv;
        if (t != t) iv = 0;                    /* NaN -> 0 */
        else {
            iv = (int)(t >= 0.0f ? t + 0.5f : t - 0.5f);
            if (iv > 127) iv = 127;
            if (iv < -128) iv = -128;
        }
        out[i] = (int8_t)iv;
    }
    return s;
}

/*
 * fp32 矩阵乘：y = W x + bias，W 是 [out][in] int8，x 是 int8[in]。
 *
 * **不量化** —— pre-LN 架构的残差流必须留在 fp32：4 层累加下来
 * int8 存不住（每层一次舍入，误差按 sqrt(4) 放大，而残差分支里
 * 信号的量级可能只有 0.1，int8 的步进 1/127 ≈ 0.008 就已经
 * 吃掉 8%）。量化由调用点在明确知道量级的地方做。
 *
 * x_scale 是输入张量的量化 scale（由 quant_dyn 返回）：
 *   真实 x[i] = x_int8[i] * x_scale
 *   W_real[j][i] = w_int8[j][i] * sc[j]
 *   => y[j] = sc[j] * x_scale * dot(w_row, x)
 * 传 1.0f 就是「输入没量化过」，与旧接口兼容。
 */
static void matvec_f32(const NnMat *m, const int8_t *x, float x_scale, float *y)
{
    int in = m->in, out = m->out;
    for (int j = 0; j < out; j++) {
        int32_t s = dot_i8_i8(m->w + (size_t)j * in, x, in);
        y[j] = (float)s * (m->sc[j] * x_scale) + m->bs[j];
    }
}

/*
 * 三个 [D][D] 投影共用同一个输入向量（q/k/v），合并成一次遍历。
 *
 * ══ 为什么不直接把三个权重量成一个大矩阵 ═══════════════════════════
 *
 * 那样最干净，但**做不到**：文件里每个矩阵是
 *     w[D*D] int8 | sc[D] float32 | bs[D] float32
 * 也就是 16384 + 512 + 512 = 17408 字节一段，wq/wk/wv 之间夹着
 * 各自的 scale 与 bias。**它们在内存里不连续。**
 * 要真做成一个大矩阵，得在加载时重新排一次（多占 48KB/层 =
 * 192KB，代价大到不值）。
 *
 * → 所以这里只合并**外层遍历**（一次进函数、x 读一次进寄存器），
 * 内层仍走各自的权重指针。真正省下的是：
 *   ① 三次函数调用 + 三次 x 的地址计算 → 一次
 *   ② 量化输入 inbuf 只需在三个权重块之间复用（本来就是同一份）
 *   ③ 三次 quant_dyn 仍然要跑（q/k/v 各自定标，不能合 ——
 *      它们的量级差好几个数量级：实测 absmax wq/wk ~1.9e-5、wv ~1.6e-1）
 *
 * 预期收益：调用与循环开销从 3 份降到 1 份。**访存字节数一点没少**
 * （三段 16KB 权重照样要从 RAM 读），而 perf_count.py 已经指出
 * 访存才是这里的瓶颈 —— 所以这一项的收益上限本就不高，
 * 拿它当"优化成功了"的证据是不成立的。
 *
 * ══ 实测结论（2026-10-04）：**收益为 0**，保留但不启用 ══════════════
 *
 *   基线（三次 matvec_f32）  431447672 tick / 输出 122 字节
 *   本版（matvec3_f32）      431459452 tick / 输出 122 字节
 *   差                       +0.003%（在噪声内，等于没变）
 *
 * 原因与 dot 那一版同一个：省下的是**已经在寄存器里的指针算术与调用开销**，
 * 而真正花时间的是权重从 RAM 到 cache 的搬运，那部分一点没少。
 * 26.5% 的 MAC 占比听起来很诱人，但**MAC 占比不等于时间占比** ——
 * 这与 perf_count.py 自己那句「静态算力是 MAC 数，实测可能是访存占 70%」相印证。
 *
 * ⚠️ 因此把它留在代码里（而不是删掉）有两个理由：
 *   ① 它证明"合并外层遍历"这条路已经走过，实测无效；
 *   ② 一旦将来换成**权重常驻 cache**（例如只处理一段连续的层，
 *      或引入了权重分块驻留），合并外层就会有意义 —— 那时不必重写。
 *
 * ⚠️ 判据必须是**实测每 token 耗时 + 输出字节数**
 * （tools/guia/perfprobe.c 的 X 趟），不能用"少了几次函数调用"来说明。
 */
static void matvec3_f32(const NnMat *a, const NnMat *b, const NnMat *c,
                        const int8_t *x, float x_scale,
                        float *ya, float *yb, float *yc)
{
    int in = a->in;
    int out = a->out;
    /* 三个矩阵形状相同（都是 [D][D]），由加载端保证（rd_mat 传一样的
     * in/out）。这里**不再逐个读各自的 in/out** —— 那样万一某个
     * 形状不同就会读到越界，而那种情况在训练端就已经错了。 */
    for (int j = 0; j < out; j++) {
        int32_t s;
        s = dot_i8_i8(a->w + (size_t)j * in, x, in);
        ya[j] = (float)s * (a->sc[j] * x_scale) + a->bs[j];
        s = dot_i8_i8(b->w + (size_t)j * in, x, in);
        yb[j] = (float)s * (b->sc[j] * x_scale) + b->bs[j];
        s = dot_i8_i8(c->w + (size_t)j * in, x, in);
        yc[j] = (float)s * (c->sc[j] * x_scale) + c->bs[j];
    }
}

/* 旧的 int8->int8 版 matvec 随 post-LN 架构一起删掉了。
 * 它那句注释「约定输入 x 已被量化到 [-127,127] 且用固定 scale 1/64」
 * 从来就不成立：真正的 scale 取决于上游是谁在量化，写死 1/64
 * 只是让它看起来自洽。pre-LN 版本统一走 matvec_f32（不量化），
 * 由调用点在明确知道量级的地方做量化。 */

/* ------------------------------------------------------------------ */
/* LayerNorm                                                           */
/* ------------------------------------------------------------------ */

/*
 * ⚠️ 这里必须用 float，**不能**用 double。
 *
 * ARM11（ARMv6K）的 VFP 只有 f32，**没有 f64 硬件**。写 double 的话
 * gcc 照样会生成 vadd.f64 / vdiv.f64 指令（看着像硬件指令），
 * 但实际是每条都走 __aeabi_dadd 软浮点例程：一次 f64 除法 ~50+ 拍，
 * 而 f32 只要 ~14 拍。这段每层调两次、每 token 四层，是热路径。
 *
 * 精度上也够：输入是 int8 量化过的激活（有效位 8 位），
 * f32 的 24 位尾数绰绰有余，f64 在这里买不到任何东西。
 */
static void layernorm(const float *x, int n, const float *b, const float *g,
                      float *out)
{
    float mean = 0.0f;
    for (int i = 0; i < n; i++) mean += x[i];
    mean /= n;
    float var = 0.0f;
    for (int i = 0; i < n; i++) {
        float d = x[i] - mean;
        var += d * d;
    }
    var /= n;
    float inv = nnet_rsqrtf(var + 1e-5f);
    for (int i = 0; i < n; i++)
        out[i] = (x[i] - mean) * inv * g[i] + b[i];
}

/* GELU（tanh 近似，与训练端一致） */
static inline float gelu(float x)
{
    const float c = 0.7978845608f;
    return 0.5f * x * (1.0f + nnet_tanhf(c * (x + 0.044715f * x * x * x)));
}

/* ------------------------------------------------------------------ */
/* 状态管理                                                            */
/* ------------------------------------------------------------------ */

typedef struct {
    int8_t  *q, *k, *v;        /* [D]  当前 token 的三个投影（int8） */
    int8_t  *qkv_act;          /* FFN 中间激活  int8 [d_ff]          */
    int8_t  *inbuf;            /* 量化后的激活  int8 [D]（matvec 输入）*/
    float   *resid;            /* ★ fp32 残差流 [D]（pre-LN 必需）   */
    float   *hbuf;             /* fp32 临时     [D]                  */
    float   *f1, *f2, *f3;      /* fp32 临时     [D] / [d_ff] / [D]  */
    /* fq/fk/fv：q/k/v 三个投影各自的 fp32 输出。
     * **必须三个分开** —— 三个张量的量级差三个数量级
     * （实测 absmax：wq/wk ~1.9e-5、wv ~1.6e-1），
     * 共用一块再统一 quant_dyn 会把小的那个压成几乎全 0。
     * 它们存在的唯一原因是 matvec3_f32 要同时输出三个结果。 */
    float   *fq, *fk, *fv;      /* fp32 [D]，q/k/v 投影输出          */
    float   *logits;            /* [V]                                  */
    float   *att;               /* [max_ctx] softmax 暂存              */

    /* ---- 输出层候选集（nnet_set_candidates） ----------------------
     * 指向调用方给的数组，**不拷贝**。所以那份数组必须活到下一次
     * nnet_push 结束 —— 上层持有它，别用局部数组。
     * cand==NULL || cand_n==0 表示不限制，走全词表。 */
    const int32_t *cand;
    int32_t        cand_n;
} NnBlk;

size_t nnet_state_bytes(const NnModel *m)
{
    size_t D = (size_t)m->d_model, C = (size_t)m->max_ctx, F = (size_t)m->d_ff;
    size_t V = (size_t)m->vocab;
    size_t nb = sizeof(NnBlk);

    /* 逐项列，别用「几个 D」打包 —— 打包的写法在加字段时会静默算错。
     *
     * ⚠️ 这里过去把 cache_k_t 算成 C*D 就够了（一份），
     * 而 nnet_push 按 [l][pos][D] 访问，**实际要 L*C*D**。
     * 少算的部分正好是 5 层的 K 缓存 ≈ 140KB，第 1 层写完就踩进
     * 别人的堆 —— 在插件场景下这会崩掉正在跑的游戏。
     * 这类「按层数索引却只算一份」的错，只有在真机上跑到第 2 层才炸。 */
    size_t bytes = nb;
    bytes += 3 * D;            /* q, k, v          int8 */
    bytes += D;                /* inbuf            int8 */
    bytes += F;                /* qkv_act          int8 */
    bytes += 4 * D;            /* resid            fp32 */
    bytes += 4 * D;            /* hbuf             fp32 */
    bytes += 4 * D;            /* f1 [D]           fp32 */
    bytes += 4 * D;            /* f3 [D]           fp32 */
    /* fq/fk/fv：q/k/v 三个投影各自的 fp32 输出（合并 matvec3 之后
     * 不能再共用一块 —— 三个量级差三个数量级，见调用点的注释）。
     * D=128 时各 512 字节，三块共 1.5KB，相对 132KB 的 KV cache
     * 可以忽略。**少算这一项的后果是这三个缓冲踩后面的堆** ——
     * 而 logits 在它们后面，所以症状是"生成的字全是乱码"。 */
    bytes += 4 * D;            /* fq [D]           fp32 */
    bytes += 4 * D;            /* fk [D]           fp32 */
    bytes += 4 * D;            /* fv [D]           fp32 */
    bytes += 4 * F;            /* f2 [d_ff]        fp32 */
    bytes += 4 * V;            /* logits           fp32 */
    bytes += 4 * C;            /* att              fp32 */

    /* 16 字节对齐的向上取整（每个缓冲都可能被 VFP 访问） */
    bytes = (bytes + 15u) & ~(size_t)15u;
    return bytes + 64;         /* 余量：结构体自身对齐与将来加字段 */
}

int nnet_state_bind(NnState *st, const NnModel *m, void *work)
{
    memset(st, 0, sizeof(*st));
    st->m = m;
    size_t D = (size_t)m->d_model, F = (size_t)m->d_ff, C = (size_t)m->max_ctx;
    size_t need = nnet_state_bytes(m);
    if (need > 16 * 1024 * 1024) return -1;   /* 防御：别把栈撑爆 */

    char *p = (char *)work;
    /* 起点先对齐到 16。调用方给的 work 若只按 8 字节对齐，
     * VFP 的 ldmia/stmia 在这种地址上会未对齐访问 -> 死机。 */
    p += (16 - ((uintptr_t)p & 15)) & 15;

    NnBlk *b = (NnBlk *)p;
    p += sizeof(NnBlk);
    p += (16 - ((uintptr_t)p & 15)) & 15;

    /* 每一步都重新对齐，而不是开头一次性对齐 ——
     * D/F/V 都是奇数时（V=4096 没事，但 D=193 这种配置会），
     * int8 缓冲的长度会让后面的 fp32 缓冲错位。 */
    b->q       = (int8_t *)p; p += D;
    b->k       = (int8_t *)p; p += D;
    b->v       = (int8_t *)p; p += D;
    b->inbuf   = (int8_t *)p; p += D;
    b->qkv_act = (int8_t *)p; p += F;

    p += (16 - ((uintptr_t)p & 15)) & 15;
    b->resid   = (float *)p; p += 4 * D;
    b->hbuf    = (float *)p; p += 4 * D;
    b->f1      = (float *)p; p += 4 * D;
    b->f3      = (float *)p; p += 4 * D;
    /* 顺序必须与 nnet_state_bytes 里的**逐项顺序**一致 ——
     * 那里的求和与这里的指针推进是同一个布局的两次书写。
     * 改一处不改另一处 = 后面所有缓冲整体错位，
     * 症状是"生成出来的字有点乱但不至于全乱"，极难归因。 */
    b->fq      = (float *)p; p += 4 * D;
    b->fk      = (float *)p; p += 4 * D;
    b->fv      = (float *)p; p += 4 * D;
    b->f2      = (float *)p; p += 4 * F;
    p += (16 - ((uintptr_t)p & 15)) & 15;
    b->logits  = (float *)p; p += 4 * (size_t)m->vocab;
    p += (16 - ((uintptr_t)p & 15)) & 15;
    b->att     = (float *)p; p += 4 * C;

    st->blk = b;
    st->fbuf = b->f1;    /* 复用：上层的 logits 缓冲就是 b->logits */
    return 0;
}

/* ------------------------------------------------------------------ */
/* 单 token 前向                                                       */
/* ------------------------------------------------------------------ */

void nnet_init(void) { /* 表都是静态的，无需初始化 */ }

/*
 * RoPE 旋转位置编码。**必须与训练端 apply_rope 逐字一致**，
 * 否则模型输出的是垃圾，而且你很难查出原因 —— 语法完全合法。
 *
 * 训练端（tools/guia/train_gpt.py:132-140）：
 *     x1, x2 = x[..., :h], x[..., h:]        ← 前后半分半，不是交错配对
 *     out    = concat([x1*c - x2*s, x1*s + x2*c])
 * 作用在 reshape 后的 [B,H,T,dh] 上，**每个头都转**。
 *
 * ⚠️ 本函数曾经有两个真实错误，gcc 只报了一句 unused parameter：
 *
 *  1. 配对方式写成交错（x[i], x[i+1]），训练端是前后半分半。
 *     旋转矩阵差了一整个置换 —— 数值上完全对不上。
 *  2. 只对 x[0..n_rope) 施加旋转，也就是**只有 head 0 有位置编码**，
 *     head 1/2/3 的 q/k 从来没被旋转过。而注意力是逐头算的，
 *     等于 3/4 的头没有位置信息 —— 长上下文立刻塌。
 *
 * 之所以能潜伏到现在：nnet.c 一直在 EXCLUDE_SRC 里没编进固件，
 * 没人跑过真实权重。这类 bug 靠「零告警」是抓不到的。
 *
 * n_rope 是偶数；cos/sin 表按 [max_ctx][d_head/2] 展平，
 * 所有头共用同一套频率（与训练端 sincos_table 一致）。
 *
 * ⚠️⚠️ **在 int8 码值域做旋转，不要先乘 scale 还原成真实幅度。**
 *
 * 本函数曾经带一个 scale 参数并在旋转前乘它，结果 K 全被清零：
 *     kraw（int8 码值）      = [4, -2, -1, 11]     ← 量化码值
 *     真实幅度 kraw * 2.47e-6 = 9.9e-6            ← 与训练端一致
 *     乘 scale 后旋转 -> 9.9e-6 * cos -> 1e-5
 *     (int8_t)(1e-5)      = 0                    ← 全零
 *
 * 也就是说「乘 scale 还原真实幅度」是对的，但**写回时忘了除回去** ——
 * 真实幅度 1e-5 被当成 int8 码值截断成 0。
 * 于是 q=k=0，注意力退化成均匀分布。零告警，不崩溃，能出字。
 *
 * 为什么码值域旋转是对的：RoPE 是**线性正交变换**
 *     out = R @ x
 * 对 x 做量化再旋转，与旋转后量化，差一个舍入（≤0.5 LSB）。
 * 而先还原真实幅度再旋转、然后忘记除回去，差的是**整个信号**。
 * 旋转矩阵保范数，所以码值域旋转幅度天然守恒，不需要任何 scale。
 *
 * 所以本函数**没有 scale 参数**。加一个「可选 scale」比不加更危险：
 * 它看起来像在处理某种配置，实际只会诱导下一次乘上去。
 */
static void rope(int8_t *x, int pos, int n_head, int d_head, int n_rope)
{
    int half = n_rope / 2;               /* 前后半分半，故需偶数 */
    /* 表长是编译期写死的 NN_ROPE_TAB_LEN（= max_ctx * d_head/2）。
     * pos 由 nnet_push 保证 < max_ctx，但 half 与表长是两次独立算出来的
     * —— 一旦模型换维度而表没重导出，这里就是越界读。
     * 宁可不做位置编码，也不能读越界：3DS 上崩的是宿主游戏。 */
    if (n_rope <= 0 || (n_rope & 1) || pos < 0 ||
        (size_t)pos * (size_t)half + (size_t)half > NN_ROPE_TAB_LEN)
        return;
    for (int h = 0; h < n_head; h++) {
        int8_t *xh = x + (size_t)h * d_head;
        for (int i = 0; i < half; i++) {
            float a = (float)xh[i];
            float b = (float)xh[half + i];
            int   o = pos * half + i;    /* 表按 [max_ctx][half] 展平 */
            float c = g_nnet_rope_cos[o], s = g_nnet_rope_sin[o];
            /* 截断而不是四舍五入：这里已经在码值域，输入就是整数，
             * 结果 |a*c-b*s| <= |a|+|b| <= 254，夹到 ±127 即可。
             * 用 (int8_t) 转换在越界时是未定义行为，所以显式夹。 */
            float u = a * c - b * s;
            float w = a * s + b * c;
            int iu = (int)(u >= 0.0f ? u + 0.5f : u - 0.5f);
            int iw = (int)(w >= 0.0f ? w + 0.5f : w - 0.5f);
            xh[i]        = (int8_t)(iu > 127 ? 127 : (iu < -128 ? -128 : iu));
            xh[half + i] = (int8_t)(iw > 127 ? 127 : (iw < -128 ? -128 : iw));
        }
    }
}

/*
 * nnet_push — 单 token 前向，**pre-LN + 残差**，与训练端
 * tools/guia/train_gpt.py:288-322 逐行对应。
 *
 *   x  = wte[token] + wpe[pos]
 *   for l in layers:
 *       h1 = layernorm(x,  ln1g, ln1b)      ← LN 在残差分支**之外**
 *       q,k,v = h1 @ wq/wk/wv.T
 *       q,k = apply_rope(q,k)
 *       att = softmax(q @ k.T / sqrt(dh))
 *       ctx = att @ v
 *       x1 = x + ctx @ wo.T                  ← 残差 1
 *       h2 = layernorm(x1, ln2g, ln2b)
 *       ff  = gelu(h2 @ w1.T) @ w2.T
 *       x  = x1 + ff                         ← 残差 2
 *   logits = layernorm(x, lnfg, lnfb) @ wte.T   （tied）
 *
 * ⚠️ 本函数过去是 **post-LN + 无残差**：先 matvec 再 layernorm，
 * 层与层之间直接覆盖。pre-LN 是 2018 年之后的标准做法，post-LN
 * 是 2017 年的做法，两者在数学上完全不同。gcc 对此**一个字都不报** ——
 * 字段名甚至长得像（bnq/bnk/bnv/bno vs ln1g/ln1b），更容易骗过审查。
 * 这类「架构层面与训练端不一致」的错，只能靠逐行对照抓，
 * 编译器永远帮不上。
 *
 * 残差流为什么必须是 fp32（b->resid）而不能是 int8：
 *   残差分支里 x 的信号可能很小（LayerNorm 输出 N(0,1)，而残差
 *   累加值可能只有 0.1 量级）。int8 的最小步进是 1/127 ≈ 0.008，
 *   6 层累加误差按 sqrt(6) 放大到 ~2%，而 pre-LN 的残差正是
 *   靠这条支路把信号传下去的 —— 量化掉它等于把模型退化成
 *   只剩 FFN。3DS 上这点内存（D*4 = 768 字节）完全付得起。
 */
int nnet_push(NnState *st, int32_t token)
{
    const NnModel *m = st->m;
    NnBlk *b = (NnBlk *)st->blk;
    int D = m->d_model, H = m->n_head, dh = m->d_head, L = m->n_layer;
    int F = m->d_ff;
    int pos = st->pos;

    if (pos >= m->max_ctx) return -1;
    if (!b || !st->cache) return -1;

    /* --- embedding -> fp32 残差流的起点 ---
     *
     * tok_emb 是 [V][D] int8 + 逐 token scale（tied 输出层共用）。
     * 必须**用 esc**：每个 token 行的量化 scale 不同，直接把 int8
     * 当数值用等于把这一行的量化误差整个丢掉 —— 不同 token 的嵌入
     * 会被当成同一尺度，而它们本来的幅度可以差好几倍。
     */
    const NnMat *te = &m->tok_emb[0];
    if (token < 0 || token >= m->vocab) return -1;   /* 外部数据，挡一道 */
    const int8_t *erow = te->w + (size_t)token * D;
    float esc = te->sc[token];
    const int8_t *prow = m->pos_emb + (size_t)pos * D;
    float psc = m->pos_scale[pos];

    for (int i = 0; i < D; i++)
        b->resid[i] = (float)erow[i] * esc + (float)prow[i] * psc;

    /* --- 逐层 --- */
    for (int l = 0; l < L; l++) {
        const NnLayer *ly = &m->layers[l];

        /* ① h1 = LayerNorm(x) —— 残差分支之外。
         *    LN 的输出是 N(0,1)，但**仍然要动态定标**：
         *    gamma 各通道差 6 倍（实测 ln1g absmax 1.20 / rms 0.80），
         *    固定 scale 1 会让小 gamma 的通道精度白扔。 */
        layernorm(b->resid, D, ly->ln1b, ly->ln1g, b->hbuf);
        float hs = quant_dyn(b->hbuf, D, b->inbuf);

        /* ② q/k/v 投影。**三者共用同一个输入**，所以合并成一次遍历
         * （见 matvec3_f32 的注释：权重在内存里不连续，合并的是外层）。
         *
         * ⚠️ 三个输出**必须落在三个不同的缓冲**。原来是三次都写
         * b->f1、然后各自 quant_dyn 拷走 —— 那是合法的，因为每次
         * 量化完就拷走了。但如果图省事改成"先都算进 f1 再一起量化"，
         * 就会把三个量级差三个数量级的张量按同一个 scale 处理 ——
         * 而 q/k 的 absmax 只有 1.9e-5，共享 scale 会把它压成几乎全 0。
         * → 三个独立缓冲是这个模块里最容易踩的量化坑，别"优化"掉。 */
        matvec3_f32(&ly->wq, &ly->wk, &ly->wv, b->inbuf, hs,
                    b->fq, b->fk, b->fv);
        float q_s = quant_dyn(b->fq, D, b->q);
        float k_s = quant_dyn(b->fk, D, b->k);
        float v_s = quant_dyn(b->fv, D, b->v);

        /* ③ RoPE：每个头都要转。
         * 注意**不传 scale** —— 旋转在 int8 码值域做，理由见 rope()。 */
        rope(b->q, pos, H, dh, m->n_rope);
        rope(b->k, pos, H, dh, m->n_rope);

        /* ④ 写 KV cache。K/V 直接写进 cache->k/v（过去还额外维护
         *    一份完全同布局的 cache_k_t，147KB 纯冗余，
         *    而且 nnet_state_bytes 只按一份算，实际越界）。
         *    scale 存进 cache->k_scale/v_scale —— 这两个字段
         *    从 NnCache 定义起就在，此前没人写它们。 */
        int8_t *ckv = st->cache->k + ((size_t)l * m->max_ctx + pos) * D;
        int8_t *cvv = st->cache->v + ((size_t)l * m->max_ctx + pos) * D;
        memcpy(ckv, b->k, D);
        memcpy(cvv, b->v, D);
        st->cache->k_scale[(size_t)l * m->max_ctx + pos] = k_s;
        st->cache->v_scale[(size_t)l * m->max_ctx + pos] = v_s;
        st->cache->len = pos + 1;

        /* ⑤ 多头注意力。
         *
         * score[t] = dot(k_t, q) * k_scale[t] * q_scale / sqrt(dh)
         *
         * ⚠️ 三个 scale 一个都不能少。以前写的是 `* (1/1024)`，
         * 那是在「q/k 都用固定 scale 量化」的假设下反推的常数
         * （(1/32)^2）。现在各张量 scale 不同（q_s、每个位置自己的
         * k_scale[t]），必须逐个乘真实值。
         *
         * 漏掉任何一个的后果都是 softmax 输入整体差一个常数倍 ——
         * 而 softmax 对常数平移不敏感，所以**分数的绝对值错了
         * 分布却仍然对**。这就是为什么它能一直潜伏：
         * 只有把 k_scale 逐位置分开乘，才真的对齐训练端。 */
        int T = pos + 1;
        const int8_t *kbase = st->cache->k + (size_t)l * m->max_ctx * D;
        const int8_t *vbase = st->cache->v + (size_t)l * m->max_ctx * D;
        const float *ksbase = st->cache->k_scale + (size_t)l * m->max_ctx;
        const float *vsbase = st->cache->v_scale + (size_t)l * m->max_ctx;
        float rsdh = nnet_rsqrtf((float)dh);

        for (int h = 0; h < H; h++) {
            int off = h * dh;
            float maxs = -1e30f;

            for (int t = 0; t < T; t++) {
                int32_t s = dot_i8_i8(kbase + (size_t)t * D + off,
                                      b->q + off, dh);
                b->att[t] = (float)s * (ksbase[t] * q_s) * rsdh;
                if (b->att[t] > maxs) maxs = b->att[t];
            }
            float sum = 0;
            for (int t = 0; t < T; t++) {
                b->att[t] = nnet_expf(b->att[t] - maxs);
                sum += b->att[t];
            }
            float inv = nnet_rcp(sum);
            for (int t = 0; t < T; t++) b->att[t] *= inv;

            /* ctx = sum_t att[t] * v_scale[t] * v_t
             * ⚠️ v_scale 是**逐位置**的（每个 token 各自定标），
             * 所以必须在这里乘，不能像 K 那样在 score 里一次乘完。 */
            for (int i = 0; i < dh; i++) b->f3[off + i] = 0.0f;
            for (int t = 0; t < T; t++) {
                float w = b->att[t] * vsbase[t];
                const int8_t *vrow = vbase + (size_t)t * D + off;
                for (int i = 0; i < dh; i++) b->f3[off + i] += w * (float)vrow[i];
            }
        }

        /* ⑥ ctx 量化后过输出投影 */
        float cs = quant_dyn(b->f3, D, b->inbuf);
        matvec_f32(&ly->wo, b->inbuf, cs, b->hbuf);

        /* ⑦ 残差 1： x1 = x + ctx@wo */
        for (int i = 0; i < D; i++) b->resid[i] += b->hbuf[i];

        /* ⑧ h2 = LayerNorm(x1) */
        layernorm(b->resid, D, ly->ln2b, ly->ln2g, b->hbuf);
        float h2s = quant_dyn(b->hbuf, D, b->inbuf);

        /* ⑨ FFN：ff1 -> GELU -> ff2（中间维 F 更大，全程 fp32） */
        matvec_f32(&ly->ff1, b->inbuf, h2s, b->f2);       /* [F] */
        for (int i = 0; i < F; i++) b->f2[i] = gelu(b->f2[i]);
        float fs = quant_dyn(b->f2, F, b->qkv_act);
        matvec_f32(&ly->ff2, b->qkv_act, fs, b->hbuf);    /* [D] */

        /* ⑩ 残差 2： x = x1 + ff */
        for (int i = 0; i < D; i++) b->resid[i] += b->hbuf[i];
    }

    /* --- 最终 LN + 输出层（tied：logits[j] = dot(emb_row_j, xf)）---
     *
     * 全词表 4096*128 = 524288 MAC，占单 token 总算力的绝大部分。
     * 有候选集时只遍历候选（实测 57~248 个，省 95%+）。
     */
    layernorm(b->resid, D, m->lnf_b, m->lnf_g, b->hbuf);
    float xs = quant_dyn(b->hbuf, D, b->inbuf);

    if (b->cand && b->cand_n > 0) {
        /* 未选中的 token 写 -1e30f 而不是留 0：这样 nnet_sample 的
         * top-8 插入排序第一轮就把它们全刷掉，且
         * softmax(-1e30 - mx) = exp(-inf) = 0，不污染归一化。
         * 留 0 的话它们会冒充 logit=0，在温度除法后参与竞争。 */
        for (int j = 0; j < m->vocab; j++) b->logits[j] = -1e30f;
        for (int ci = 0; ci < b->cand_n; ci++) {
            int32_t j = b->cand[ci];
            if (j < 0 || j >= m->vocab) continue;   /* 外部数据，挡一道 */
            int32_t s = dot_i8_i8(te->w + (size_t)j * D, b->inbuf, D);
            b->logits[j] = (float)s * (te->sc[j] * xs);
        }
    } else {
        for (int j = 0; j < m->vocab; j++) {
            int32_t s = dot_i8_i8(te->w + (size_t)j * D, b->inbuf, D);
            b->logits[j] = (float)s * (te->sc[j] * xs);
        }
    }

    st->pos = pos + 1;
    return 0;
}
/* ------------------------------------------------------------------ */
/* 输出层候选集                                                        */
/* ------------------------------------------------------------------ */

int nnet_set_candidates(NnState *st, const int32_t *cand, int n)
{
    NnBlk *b = (NnBlk *)st->blk;
    if (!b) return -1;
    if (n < 0) n = 0;
    /* 候选数超过词表就截到词表大小：再多是重复 id，语义上没意义。 */
    if (n > st->m->vocab) n = st->m->vocab;
    b->cand    = (n > 0) ? cand : NULL;
    b->cand_n  = (n > 0) ? n : 0;
    return 0;
}

/* ------------------------------------------------------------------ */
/* 采样                                                                */
/* ------------------------------------------------------------------ */

int nnet_sample(const NnState *st, float temperature, NnTopK *out)
{
    NnBlk *b = (NnBlk *)st->blk;
    int V = st->m->vocab;
    const float *lg = b->logits;

    /* 温度 */
    float t = temperature < 0.05f ? 0.05f : temperature;

    /* 找 top-k（用小顶堆式的插入排序，k=8 足够） */
    out->n = 0;
    for (int i = 0; i < V; i++) {
        float v = lg[i] / t;
        if (out->n < NN_TOPK) {
            int p = out->n++;
            while (p > 0 && out->p[p - 1] < v) {
                out->p[p] = out->p[p - 1]; out->id[p] = out->id[p - 1]; p--;
            }
            out->p[p] = v; out->id[p] = i;
        } else if (v > out->p[NN_TOPK - 1]) {
            int p = NN_TOPK - 1;
            while (p > 0 && out->p[p - 1] < v) {
                out->p[p] = out->p[p - 1]; out->id[p] = out->id[p - 1]; p--;
            }
            out->p[p] = v; out->id[p] = i;
        }
    }
    /* softmax 归一化（只在 top-k 内） */
    float mx = out->p[0], sum = 0;
    for (int i = 0; i < out->n; i++) { out->p[i] = nnet_expf(out->p[i] - mx); sum += out->p[i]; }
    float inv = nnet_rcp(sum);
    for (int i = 0; i < out->n; i++) out->p[i] *= inv;
    return out->n;
}

/*
 * 采样：按 p^(1/T) 归一化后抽一个，seed 可复现。
 *
 * ⚠️ 温度以前是**收了参数但没用**（gcc -Wextra 报 unused parameter
 * 暴露出来的）。原来那版直接按 p 累积，等价于 T=1，
 * 于是 "temperature=0.3 生成更稳" 这个调用方的预期完全落空 ——
 * 而模型输出质量恰恰是靠这个参数调的，属于"看起来能调其实没调"。
 *
 * 为什么用 p^(1/T) 而不是除以 T：
 *   标准的 temperature softmax 是 softmax(logits / T)，
 *   而 tk->p 已经是 softmax(logits) 之后的结果。
 *   softmax(l/T) ∝ p^(1/T)，所以在概率空间做 1/T 次幂再归一化
 *   与在 logits 空间除 T 完全等价 —— 但前者省掉了重算 logits。
 *
 * T 的夹取是有讲究的：
 *   T <= 0 不存在（0 是退化分布，负数会让幂次翻转）。
 *     所以下界取 0.05 —— 低于它分布已经和 argmax 没区别了，
 *     而 powf(p, 20) 在 p 很小时会下溢，累积和永远追不上 r，
 *     结果是**永远返回 id[0]**（那恰好是 top-1，
 *     行为上不算错，但失去了随机性，且是靠巧合而不是设计）。
 *   上界取 4.0：T 太大时分布趋于均匀，模型开始胡说。
 *     夹住它比让它跑到 100 更好 —— 后者配合候选集
 *     虽然不会胡编（候选限制了），但会说出原文里毫不相干的句子。
 */
int32_t nnet_pick(const NnTopK *tk, float temperature, uint32_t *seed)
{
    float r = (float)((*seed = *seed * 1103515245u + 12345u) >> 8) / 16777216.0f;
    float t, sum = 0.0f;
    int32_t i, ret;

    if (!tk || tk->n <= 0)
        return -1;

    /* NaN 温度按 1.0 处理：宁可不做温度也不要产生 NaN 概率。 */
    if (temperature != temperature)
        temperature = 1.0f;
    if (temperature < 0.05f) temperature = 0.05f;
    if (temperature >  4.0f) temperature =  4.0f;
    t = 1.0f / temperature;

    /* 两遍：先算权重和，再抽。第二遍必须重算权重 ——
     * 不能第一遍把 p 改掉，那样第二遍就没得抽了。
     * NN_TOPK 只有 8，重算 8 次 powf 的开销可以忽略。 */
    for (i = 0; i < tk->n; i++) {
        float w = (tk->p[i] > 0.0f) ? nnet_powf(tk->p[i], t) : 0.0f;
        sum += w;
    }
    if (!(sum > 0.0f)) return tk->id[0];   /* 全 0（极端情况）-> top-1 */

    ret = tk->id[tk->n - 1];              /* 兜底：累加没命中就给最后一个 */
    {
        float acc = 0.0f;
        for (i = 0; i < tk->n; i++) {
            float w = (tk->p[i] > 0.0f) ? nnet_powf(tk->p[i], t) : 0.0f;
            acc += w / sum;
            if (r <= acc) { ret = tk->id[i]; break; }
        }
    }
    return ret;
}

int nnet_logits_top(const NnState *st, int32_t *out, int32_t n)
{
    NnBlk *b = (NnBlk *)st->blk;
    int V = st->m->vocab;
    for (int i = 0; i < n; i++) {
        int best = 0;
        for (int j = 1; j < V; j++)
            if (b->logits[j] > b->logits[best]) best = j;
        out[i] = best;
        b->logits[best] = -1e30f;
    }
    return n;
}
