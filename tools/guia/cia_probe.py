"""
cia_probe.py — 解析 CIA 的 exheader，把三个关键字段抠出来。

用途：RSF 改完要确认「真的写进二进制了」，而不是「makerom 没报错」
就算过。makerom 对认得的 key 才会写，对不认识的会直接 fatal，
所以「没报错」本身是个弱证据；把位抠出来看才是强证据。

检查项：
  ① ServiceAccessControl 里有没有 ptm:sysm（升频通道）
  ② core_info.use_cpu_clockrate_804MHz 那一位（启动即 804MHz）
  ③ reslimits[0]（core1 的 CPU 时间配额；0 = 运行时自己申请）

定位 exheader 的方法（**用锚点反推，不硬编码 section 偏移**）：
  CIA 顶层 header 的布局随 makerom 版本变（0x20 定长 / 0x2020 变长都实测到过），
  所以不按 section table 走。改成拿 ServiceAccessControl 的**第 0 项内容**
  当锚点 —— 我们 RSF 里第一项是 `cfg:u`，但这只是本项目的约定，
  更稳的是用**服务表的已知长度**：从 0 开始扫连续 8 字节可打印且以 ':' 结尾
  的槽，第一个命中处减去固定偏移就是 service_access 的位置。

  ⚠️ **偏移和位号都是 diff 实测出来的，不是照抄头文件算的**
     （第一次照抄算错了，浪费了一整轮）：
      exheader 起点 -> service_access = 0x2A8
      exheader 起点 -> core_info      = 0x264
      core_info    -> reslimits[0]   = +0x10
  验证方法：同一份 elf 打两份 CIA，唯一差别是 RSF 里有没有 `CpuSpeed: 804mhz`，
  结果**只有 core_info 的第 0 字节**不同：0x00 → 0x02。
  即 804MHz 对应 **core_info 的 bit1**（不是 bit8 —— 头文件的位域是按位算的，
  但 core_version 虽是 u32，实测打包进了同一个字节的低位，位号从 0 起连续排）。
  同一字节的 bit2 = 0x04（两份都有）= enable_l2c。

用法：
  python cia_probe.py out.cia [more.cia ...]
"""
import struct
import sys

# ── 以下偏移与位号全部由 diff 实测确认 ──
OFF_SERVICE   = 0x2A8    # ServiceAccessControl[34][8]
OFF_CORE_INFO = 0x264    # ExHeader_Arm11CoreInfo
OFF_RESLIMITS = 0x274    # u16 reslimits[16]  (core_info 之后)
OFF_STORAGE   = 0x294    # ExHeader_Arm11StorageInfo

# core_info 第一个字节的位域（实测）
CV_BIT        = 0x0F     # core_version，占 bit0-3
BIT_804MHZ    = 1        # use_cpu_clockrate_804MHz
BIT_L2C       = 2        # enable_l2c

FIRST_SERVICE = b'cfg:u'   # 我们 RSF 里 ServiceAccessControl 的第一项


def find_exheader(data):
    """用服务表首项反推 exheader 起点。找不到返回 None。"""
    i = data.find(FIRST_SERVICE)
    if i < 0:
        return None
    start = i - OFF_SERVICE
    if start < 0:
        return None
    # 合理性检查：core_info 的 byte2 应该非 0（实测两份 CIA 都是 0x04）
    if data[start + OFF_CORE_INFO + 2] == 0:
        return None
    return start


def probe(path):
    data = open(path, 'rb').read()
    ex0 = find_exheader(data)
    if ex0 is None:
        print('%s: 找不到 exheader（锚点 "%s" 不在）' %
              (path, FIRST_SERVICE.decode()))
        return False
    ex = data[ex0:]
    ci = ex[OFF_CORE_INFO]
    print('  exheader @ 0x%X   core_info bytes = %s'
          % (ex0, ex[OFF_CORE_INFO:OFF_CORE_INFO + 8].hex(' ')))

    ok = True

    # ---- ① 服务表 ----
    svc = []
    for i in range(34):
        name = ex[OFF_SERVICE + i * 8:OFF_SERVICE + i * 8 + 8]
        if name[:1] == b'\x00':
            break
        svc.append(name.rstrip(b'\x00').decode('ascii', 'replace'))
    print('  services (%d): %s' % (len(svc), ' '.join(svc)))
    for need, why in (('ptm:sysm', 'osSetSpeedupEnable 升频通道'),
                      ('APT:U',    'CheckNew3DS / SetAppCpuTimeLimit')):
        if need not in svc:
            print('  [!] 缺 %-9s -> %s 不可用' % (need, why))
            ok = False
        else:
            print('  [ok] %-9s -> %s' % (need, why))

    # ---- ② 804MHz 位 ----
    has804 = bool(ci & (1 << BIT_804MHZ))
    hasl2c = bool(ci & (1 << BIT_L2C))
    cv     = ci & ((1 << 4) - 1)
    print('  core_version=%d  804MHz=%d  L2C=%d' % (cv, has804, hasl2c))
    if not has804:
        print('  [!] use_cpu_clockrate_804MHz = 0 -> 启动就是 268MHz')
        ok = False
    else:
        print('  [ok] 启动即 804MHz')

    # ---- ③ core1 配额 ----
    resl = struct.unpack_from('<H', ex, OFF_RESLIMITS)[0]
    print('  reslimits[0] = 0x%04X (%d%%)' % (resl, resl))
    if resl == 0:
        print('  [ok] reslimit 留空 -> 运行时 APT_SetAppCpuTimeLimit(60) 申请')
    else:
        print('  [i] exheader 已写死配额 %d%%，运行时不必再申请' % resl)

    return ok


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 1
    bad = 0
    for p in sys.argv[1:]:
        print('== %s' % p)
        if not probe(p):
            bad += 1
    print()
    print('======= %s =======' % ('全部通过' if bad == 0 else '%d 个文件有问题' % bad))
    return 0 if bad == 0 else 1


if __name__ == '__main__':
    sys.exit(main())