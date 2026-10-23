# -*- coding: utf-8 -*-
"""font.bin 结构自检：头 / 索引升序 / 二分可找 / 豆腐块哨兵 / emoji 步进。"""
import struct
import sys

p = sys.argv[1] if len(sys.argv) > 1 else "app/data/font.bin"
d = open(p, "rb").read()

magic, ver, flags, cw, ch, base, rsv = struct.unpack("<4sHHBBBB", d[:12])
count, ioff, boff, bsize, fb = struct.unpack("<IIIII", d[12:32])
print("magic=%r ver=%d flags=0x%02x cell=%dx%d baseline=%d" % (magic, ver, flags, cw, ch, base))
print("count=%d index_off=%d bitmap_off=%d bitmap_size=%d fallback=%d"
      % (count, ioff, boff, bsize, fb))
print("file=%d  expect=%d  %s" % (len(d), boff + bsize,
                                  "OK" if len(d) == boff + bsize else "MISMATCH"))

cps = [struct.unpack("<I", d[ioff + i * 8: ioff + i * 8 + 4])[0] for i in range(count)]
print("索引升序:", "OK" if cps == sorted(cps) else "FAIL")
print("索引去重:", "OK" if len(set(cps)) == count else "FAIL(有重复码位)")

# 二分抽查（与 C 端 font_find 同算法）
def find(cp):
    lo, hi = 0, count - 1
    while lo <= hi:
        mid = (lo + hi) // 2
        c = struct.unpack("<I", d[ioff + mid * 8: ioff + mid * 8 + 4])[0]
        if c == cp:
            return mid
        if c < cp:
            lo = mid + 1
        else:
            hi = mid - 1
    return -1

probes = {
    0x4E2D: "中", 0x41: "A", 0x20: "空格", 0x2014: "—", 0x00B7: "·",
    0x25B2: "▲", 0x25B6: "▶", 0x2764: "❤", 0x1F60A: "😊", 0x2615: "☕",
    0x1F4C8: "📈", 0x1F7E2: "🟢", 0x200D: "ZWJ", 0xFE0F: "VS16",
    0x1F3FB: "肤色1", 0x10FFFF: "豆腐哨兵", 0x1F600: "😀",
}
for cp, name in probes.items():
    i = find(cp)
    if i < 0:
        print("  U+%04X %-8s 缺失" % (cp, name))
        continue
    adv, fl = struct.unpack("<BB", d[ioff + i * 8 + 4: ioff + i * 8 + 6])
    print("  U+%04X %-8s idx=%4d advance=%d wide=%d" % (cp, name, i, adv, fl & 1))

# 豆腐块本身必须有墨（不然"缺字"和"空格"分不出来）
def alpha(cp, x, y):
    i = find(cp)
    if i < 0:
        return -1
    off = boff + i * (cw * ch)
    return d[off + y * cw + x]

print("豆腐哨兵墨量(中心 3x3 >=1):",
      any(alpha(0x10FFFF, x, y) for x in range(7, 9) for y in range(7, 9)))
print("零宽符 advance==0:",
      [struct.unpack("<B", d[ioff + find(c) * 8 + 4: ioff + find(c) * 8 + 5])[0]
       for c in (0x200D, 0xFE0F, 0x1F3FB) if find(c) >= 0])
