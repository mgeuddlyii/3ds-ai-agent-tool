"""把崩溃转储里的 8 位十六进制地址解析成 ELF 符号+偏移。

用法:
  python tools/symresolve.py                  # 扫 logs/new_crashblock.txt，按出现次数列 code 区地址
  python tools/symresolve.py 0011C960 124434  # 解析指定地址
"""
import re, bisect, sys, os

BASE = r"E:/Gudyi/WORK5/3ds-ai"
SYM = BASE + "/tools/_elfsym.txt"

funcs = []
pat = re.compile(r"^([0-9a-fA-F]{8})\s+\S+\s+F\s+(\S+)\s+([0-9a-fA-F]+)\s+(\S+)", re.M)
for m in pat.finditer(open(SYM, errors="replace").read()):
    funcs.append((int(m.group(1), 16), int(m.group(3), 16), m.group(4)))
funcs.sort()
addrs = [f[0] for f in funcs]

def resolve(a):
    i = bisect.bisect_right(addrs, a) - 1
    if i >= 0:
        addr, size, name = funcs[i]
        if addr <= a < addr + size:
            return "%s+0x%x" % (name, a - addr)
    return None

if len(sys.argv) > 1:
    for tok in sys.argv[1:]:
        a = int(tok, 16)
        print("%s -> %s" % (tok, resolve(a)))
else:
    text = open(BASE + "/logs/new_crashblock.txt", errors="replace").read(200000)
    hits = {}
    for m in re.finditer(r"\b([0-9A-Fa-f]{8})\b", text):
        v = int(m.group(1), 16)
        if 0x00100000 <= v < 0x00200000:
            hits[v] = hits.get(v, 0) + 1
    print("code-region 8-hex values (top 30 by count):")
    for v, c in sorted(hits.items(), key=lambda kv: -kv[1])[:30]:
        print("  0x%08X x%-4d %s" % (v, c, resolve(v) or "?"))
