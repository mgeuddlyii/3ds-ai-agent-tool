"""
热点量化：这一层每 token 的算力花在哪。

══ 为什么先数再改 ═════════════════════════════════════════════════════

3DS 是 ARM11 @ 268/804MHz，**没有 NEON、没有硬件超越函数**。
int8×int8 点积走 `smulbb`/`smlabb`（ARM11 DSP 扩展，gcc 会自己用），
所以「MAC 次数」在这台机器上近似正比于算力，先数清楚再决定优化谁。

══ ⚠️ 上一版这个脚本给出的结论是错的 ═══════════════════════════════

第一版按「输出层 = 全词表 4096」算，得出「wte 占 66.7%，是主要矛盾」。
错在**假设了输出层走全词表**，而实际 `nnet.c:541` 只遍历候选集
（`cand_probe.py` 对 12 条实测用例量的中位数是 169，最大 248）。
改正之后输出层从 524288 MAC 掉到约 2 万，直接从第一名变成无名小卒。

→ 这条本身就是本项目的一条通用教训：**测热点必须把「实际走的那条
  路径」喂进去，不能用最坏情况或最简单情况的假设代替**。

══ 只数 MAC 会漏掉访存 ═══════════════════════════════════════════════

关键量是**这一段要读多少字节**。矩阵的 MAC 数 = 权重字节数（1:1），
所以 MAC 本身隐含了访存量；但两者对缓存的**影响完全不同**：

  - 单个矩阵 ≤ L2 → 留在缓存里，逐 token 复用，几乎免费
  - 输出层候选集：只读 169 行 × 128B = 21KB，**反而是 L1/L2 友好的**
  - 但如果**按随机顺序**访问候选行，就是 169 次缓存未命中

所以本脚本对输出层同时给出两种访存假设，取更悲观的。

L2 512KB（ARM11 MPCore），主 RAM 约 64MB。整模型 768KB > L2，
所以「每 token 全模型重新从 RAM 读一遍」是**已知的下限**，
优化只能在常数上做，不存在「消除这次访存」的空间。
"""
import os
import struct
import sys

MODEL_BIN = os.path.join("app", "data", "guide_model.bin")

# 实测候选集大小（tools/guia/cand_probe.py，12 条用例）
CAND_MEDIAN = 169
CAND_MAX = 248

# 3DS (ARM11 MPCore) 缓存
L1_D_BYTES = 32 * 1024
L2_BYTES = 512 * 1024


def read_dims(path=MODEL_BIN):
    """维度只在文件头里。硬编码过一次（D_MODEL=192），报 broadcast 错误。"""
    with open(path, "rb") as f:
        d = f.read(36)
    if d[:4] != b"GM1\0":
        sys.exit("FAIL: %s 不是 GM1 模型文件" % path)
    v = struct.unpack("<8i", d[4:36])
    names = ("vocab d_model n_layer n_head d_ff max_ctx d_head n_rope").split()
    dim = dict(zip(names, v))
    if dim["n_head"] * dim["d_head"] != dim["d_model"]:
        sys.exit("FAIL: n_head*d_head(%d) != d_model(%d)"
                 % (dim["n_head"] * dim["d_head"], dim["d_model"]))
    return dim


def main():
    D = read_dims()
    V, DM, L, H, F, C, DH = (D["vocab"], D["d_model"], D["n_layer"],
                             D["n_head"], D["d_ff"], D["max_ctx"], D["d_head"])

    print("== 维度（从文件头读）==")
    print("  V=%d D=%d L=%d H=%d F=%d C=%d dh=%d" % (V, DM, L, H, F, C, DH))
    print("  缓存 L1=%dKB  L2=%dKB" % (L1_D_BYTES // 1024, L2_BYTES // 1024))
    print()

    # ---- 每层每 token 的矩阵段 ----
    # (名字, 单层 MAC, 单层权重字节)
    mats = [
        ("attn.wq", DM * DM, DM * DM),
        ("attn.wk", DM * DM, DM * DM),
        ("attn.wv", DM * DM, DM * DM),
        ("attn.wo", DM * DM, DM * DM),
        ("ffn.w1",  DM * F,  DM * F),
        ("ffn.w2",  F * DM,  F * DM),
    ]

    # ---- 注意力打分（与权重无关，随 pos 线性增长）----
    avg_pos = C // 2          # 平均上下文长度取一半作为典型
    att_scores = L * avg_pos * DM     # Q·K
    att_av     = L * avg_pos * DM     # ·V
    att_total  = att_scores + att_av

    # ---- 输出层（候选集约束下）----
    out_macs_c = CAND_MEDIAN * DM
    out_bytes_c = CAND_MEDIAN * DM
    out_macs_full = V * DM
    out_bytes_full = V * DM

    print("== 每 token 算力分解 ==")
    total_macs = 0
    total_bytes = 0
    rows = []
    for name, mac, wbytes in mats:
        m, b = mac * L, wbytes * L
        total_macs += m
        total_bytes += b
        rows.append((name + " ×4层", m, b))

    # 输出层：候选集（实际）vs 全表（对照）
    total_macs_c = total_macs + out_macs_c + att_total
    total_bytes_c = total_bytes + out_bytes_c
    rows.append(("out.wte（候选集）", out_macs_c, out_bytes_c))
    rows.append(("attention QK+AV", att_total, 0))
    total_macs_full = total_macs + out_macs_full + att_total
    total_bytes_full = total_bytes + out_bytes_full

    def show(title, tm, tb):
        print("  %s" % title)
        print("  %-22s %10s %10s %8s" % ("段", "MAC", "字节", "占比"))
        print("  " + "-" * 54)
        for name, mac, wbytes in sorted(rows, key=lambda r: -r[1]):
            pct = 100.0 * mac / tm if tm else 0
            print("  %-22s %10d %9.1fK %7.1f%%"
                  % (name, mac, wbytes / 1024.0, pct))
        print("  %-22s %10d %9.1fK" % ("合计", tm, tb / 1024.0))
        print()

    show("A. 实际路径（候选集中位 %d）" % CAND_MEDIAN,
         total_macs_c, total_bytes_c)
    print("  对照 B. 若输出层走全词表 %d：%d MAC（比 A 多 %.1f 倍）"
          % (V, total_macs_full, total_macs_full / float(total_macs_c)))
    print()

    # ---- 访存结论 ----
    print("== 访存结论 ==")
    # mats 里是**单层**的字节，×L 才是主干
    trunk_bytes = sum(w for _, _, w in mats) * L
    per_layer_bytes = trunk_bytes / float(L)
    # embedding（tied wte）另有 512KB，只有输出层候选集那几行会被读
    emb_bytes = V * DM
    print("  主干权重（×%d 层）%.0f KB   单层 %.0f KB"
          % (L, trunk_bytes / 1024.0, per_layer_bytes / 1024.0))
    print("  vs  L2 %d KB →  **超出 %.0f KB**"
          % (L2_BYTES // 1024, (trunk_bytes - L2_BYTES) / 1024.0))
    print("  ⇒ 每 token 前向都要从 RAM 重读一遍主干，这是**物理下限**，")
    print("    常数优化消不掉它。能做的只是「让这次读更便宜」。")
    print()
    print("  单层 FFN w1=%.0fKB w2=%.0fKB；attn 四段各 %.0fKB"
          % (DM * F / 1024.0, F * DM / 1024.0, DM * DM / 1024.0))
    print("  ⇒ FFN 占主干字节 %.0f%%，是访存主体。"
          % (100.0 * 2 * DM * F * L / trunk_bytes))
    print()
    print("  换权重分辨率的收益上限：主干 int8→int4 = %.0fKB，**装得进 L2**；"
          % (trunk_bytes / 2048.0))
    print("    这是**唯一能消除每 token 重读**的优化，但它要求重新量化训练。")
    print("    （embedding %.0fKB 是 tied 的，只读候选行，不在热路径上。）"
          % (emb_bytes / 1024.0))
    print()

    # ---- 可操作项 ----
    print("== 按收益/风险排序的可操作项 ==")
    items = [
        ("wq/wk/wv 合并成一次大 matvec",
         "三个 [128,128] 用同一个 h1。合并成 [384,128] 单次遍历："
         "3 次函数调用→1 次，3 段 16KB 顺序流→1 段 48KB 顺序流。"
         "访存总量不变，但对预取器友好。", "低"),
        ("quant_dyn 开销确认",
         "每层调 7 次，每次 O(n) 两趟扫描；n=128~384。"
         "相比同层 7 段 matvec 的 ~90 万 MAC，量级差 3 个数量级，"
         "**应当可忽略，但要用实测确认而不是这么断言**。", "低"),
        ("滑窗注意力",
         "attention 段随 pos 线性增长，pos=%d 时占 %.1f%%。"
         "只看最近 64 个可砍一半，但会丢长程指代。"
         % (C, 100.0 * att_total / total_macs_c),
         "中（损质量）"),
        ("FFN 双核流水",
         "层与层串行，但同层内 w1 与 w2 串行、attn 四段互不依赖，"
         "可拆到 core1。**通信成本可能吃掉全部收益**，"
         "必须先量再决定，不能凭直觉上。", "高"),
        ("权重量化到 int4",
         "主干 %.0fKB → %.0fKB，**刚好装进 L2**。这是唯一能消除"
         "「每 token 重读」的优化。但需要重新量化训练，"
         "且 int4 在 d_model=128 时精度损失未知。"
         % (trunk_bytes / 1024.0, trunk_bytes / 2048.0),
         "高（需重训）"),
    ]
    for i, (name, why, risk) in enumerate(items, 1):
        print("  %d. [%s] %s" % (i, risk, name))
        print("     %s" % why)
    print()
    print("  **建议顺序：1 → 2 → 3（若质量可接受）**。4/5 都要先拿数据再决定。")
    print("  判据统一用「每 token 耗时」这个单调量，不要用帧号。")


if __name__ == "__main__":
    main()
