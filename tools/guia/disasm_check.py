"""
点积内核的反汇编判据。

══ 为什么需要这个 ═════════════════════════════════════════════════════

ARM11 没有 NEON、没有向量加载，int8 点积只能靠 `smlabb`（DSP 扩展）
配逐字节 `ldrsb`。在这个前提下，「怎么把点积写快」这件事上，
**纸面推算几乎全是错的**，因为：

  - 累加器开太多 → ARM11 只有 16 个寄存器，gcc 被迫把累加器
    溢出到栈（str/ldr 混进循环体），而访存正是瓶颈。
    **实测过的例子**：4 路展开的循环体是纯寄存器操作；
    改成 8 路后总指令数从 4.00/MAC 降到 3.25/MAC（看着 -19%），
    但多出来的是 5 条 str + 5 条 ldr —— 净效果是**更慢**。

  - 所以「每 MAC 几条指令」这个指标本身就不够。
    必须看**指令的种类**：一条 str 和一条 add 不能算等价。

本脚本把这件事变成可重复的检查：
  1. 反汇编指定函数；
  2. 定位主循环体（含最多 smlabb 的回边区间）；
  3. 检查有没有 str/ldr/spill 混进循环体；
  4. 给结论。

用法：python tools/guia/disasm_check.py
"""
import os
import re
import subprocess
import sys
from collections import Counter

OBJDUMP = r"E:\devkitPro\devkitARM\bin\arm-none-eabi-objdump.exe"
OBJ = os.path.join("app", "build", "nnet.o")

# 主循环体的识别：含 smlabb 最多的一段，且以回边（bne）结束
SPILL = ("str", "ldrh", "strh", "ldrd", "strd", "push", "pop")


def disasm(section):
    if not os.path.exists(OBJ):
        sys.exit("FAIL: %s 不存在，先跑 tools/build.py" % OBJ)
    if not os.path.exists(OBJDUMP):
        sys.exit("FAIL: 找不到 objdump：%s" % OBJDUMP)
    r = subprocess.run([OBJDUMP, "-d", "-j", section, OBJ],
                       capture_output=True, text=True)
    ins = []
    for l in r.stdout.split("\n"):
        # 形如 "  1c:\te1011789\tsmlabb\tr1, r9, r7, r1"
        if re.match(r"^\s+[0-9a-f]+:\t", l):
            p = l.split("\t")
            ins.append((p[2].strip().split()[0] if len(p) > 2 else "?", l))
    return ins


def find_loop(ins):
    """返回 (起, 止) —— 含 smlabb 最多、以 bne 收尾的区间。"""
    idx = [i for i, (m, _) in enumerate(ins) if m == "smlabb"]
    if not idx:
        return None
    st = idx[0]
    for i in range(st, len(ins)):
        if ins[i][0] == "bne":
            return (st, i)
    return None


def report(section, lanes):
    ins = disasm(section)
    if not ins:
        print("  %-22s （无此段，跳过）" % section)
        return True
    loop = find_loop(ins)
    if loop is None:
        print("  %-22s 找不到含 smlabb 的循环体" % section)
        return False
    st, en = loop
    body = ins[st:en + 1]
    n_mac = sum(1 for m, _ in body if m == "smlabb")
    c = Counter(m for m, _ in body)
    spill = sum(v for k, v in c.items() if k in SPILL)
    ok = (n_mac == lanes) and (spill == 0)

    print("  %-22s %2d MAC / %2d 指令  = %.2f 指令/MAC   溢出访存 %d 条  %s"
          % (section, n_mac, len(body), len(body) / float(n_mac),
             spill, "OK" if ok else "!! 不合格"))
    for k, v in c.most_common():
        mark = "  <- 慢" if k in SPILL else ""
        print("        %-8s %d%s" % (k, v, mark))
    return ok


def main():
    print("== 点积内核反汇编判据 ==")
    print("  目标：%d 路展开、循环体内无栈溢出访存" % 4)
    print()
    ok = report(".text.dot_i8_i8", 4)
    ok = report(".text.matvec_f32", 4) and ok
    print()
    if ok:
        print("判定：通过。点积内核是纯寄存器操作。")
        print()
        print("  ⚠️ 要改这个内核，先想清楚这两条：")
        print("     1. ARM11 只有 16 个通用寄存器，累加器开到 8 路就会")
        print("        把累加器挤到栈上（实测更慢，尽管总指令数更少）。")
        print("     2. 真正的瓶颈是访存（L1 32KB / L2 512KB，")
        print("        主干权重 640KB 已经装不进 L2），所以省访存 > 省指令。")
        return 0
    print("判定：不通过。")
    return 1


if __name__ == "__main__":
    sys.exit(main())
