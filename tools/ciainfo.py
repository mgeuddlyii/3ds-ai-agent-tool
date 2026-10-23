#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""读 .cia 的身份：Title ID / 版本 / ProductCode / content / 内嵌 SMDH。

============================ 为什么写这个工具 ============================
2026-10-10 真机故障：用户 HOME 菜单上 FBI 的格子被我们自己的程序占了。
真实原因不是"图标顺序"，是 **Title ID 撞了**：

    app/AIConsole_cia.rsf 的 UniqueId = 0xF8001
    → Title ID = 0x000400000F800100
    → 这正是 FBI 官方 build 的 Title ID
      （GodMode9 PR #618 的实测记录：装完 FBI.cia 后 SD 上出现
        title/00040000/0f800100，title.db 里是 000400000F800100）

同 ID 安装 = 覆盖/更新，不是新增。UI 上看着像"占了别人的位置"，
但光看 UI 查不出来，只能读二进制。所以要有这个工具，并且要有
`--selftest` 证明它真的能判红。

============================ 布局（重要） ============================
    header(0x2020) | cert_chain | ticket | tmd | content... | [meta]

⚠️ 下面这些偏移是**实测出来的**（2026-10-10，makerom 0.19 的产物），
  不是查文档抄的。踩过一次：按"标准文档"的 0x18C 读 TMD title id，
  读出来是全 0；实际位置在 0x1DC —— makerom 的布局比通用规范整体
  ​多了一截头部。而且这里的 title id 是**大端**存的（NCCH 里才是小端）。

    ticket:  title_id BE u64 @+0x1FC      version BE u16 @+0x206
    tmd:     title_id BE u64 @+0x1DC      version BE u16 @+0x22C
                                          nchunk  BE u16 @+0x22E
    NCCH:    programId LE u64 @+0x118     productCode ascii @+0x150
             magic "NCCH" @+0x100（用它反推 NCCH 起点）

其中 tmd 的 version 偏移与 tools/build.py 里已有的构建期版本断言
（注释里写的 0x2F9C = tmd+0x22C）吻合，可以互证。

**三处来源必须一致**：ticket / tmd / NCCH 各自独立取出 title id，
只要有一处对不上就报错。这样即使将来 makerom 换了布局，工具会
吵而不是悄悄给出错的数字 —— 这才是能用的判据。
"""
import binascii
import os
import struct
import sys

TICKET_TID_OFF, TICKET_VER_OFF = 0x1FC, 0x206
TMD_TID_OFF, TMD_VER_OFF, TMD_NCHUNK_OFF = 0x1DC, 0x22C, 0x22E
NCCH_MAGIC_OFF, NCCH_PROGRAMID_OFF = 0x100, 0x118
NCCH_PRODUCTCODE_OFF = 0x150

# 已知的、绝不可以使用的 Title ID。
RESERVED = {
    0x000400000F800100: "FBI（官方 build）",
    0x000400000D921E00: "hblauncher_loader（3DS H&S 注入版）",
}

# 3dbrew Titles：Unique ID 区段划分
UID_APP_MIN, UID_APP_MAX = 0x300, 0xF7FFF
UID_EVAL_MIN, UID_EVAL_MAX = 0xF8000, 0xFEFFF


def be16(b, o):
    return struct.unpack_from(">H", b, o)[0]


def be64(b, o):
    return struct.unpack_from(">Q", b, o)[0]


def le64(b, o):
    return struct.unpack_from("<Q", b, o)[0]


def parse(path):
    info = {"path": path, "problems": [], "notes": [], "ok": False}
    d = open(path, "rb").read()
    info["size"] = len(d)
    if len(d) < 0x2020:
        info["problems"].append("文件比 CIA header 还短（%d 字节）" % len(d))
        return info
    hdrsize = struct.unpack_from("<I", d, 0)[0]
    if hdrsize != 0x2020:
        info["problems"].append("header size 是 0x%X，不是 0x2020" % hdrsize)
        return info
    certsz, ticketsz, tmdsz, metasz = struct.unpack_from("<IIII", d, 8)
    ot, om = hdrsize + certsz, hdrsize + certsz + ticketsz
    oc = om + tmdsz
    info["notes"].append("cert=%d ticket=%d tmd=%d meta=%d | ticket@0x%X tmd@0x%X "
                         "content@0x%X" % (certsz, ticketsz, tmdsz, metasz,
                                           ot, om, oc))
    if ticketsz < 0x210 or tmdsz < 0x240:
        info["problems"].append(
            "ticket/tmd 尺寸异常（%d/%d），偏移表可能不适用" % (ticketsz, tmdsz))
        return info

    # ---- 三处独立读取，最后交叉比对 ----
    if d[ot + 0x160:ot + 0x170].startswith(b"Root-CA"):
        pass        # ticket issuer，用来确认 ticket 起点没算错
    else:
        info["notes"].append("ticket 起点的 issuer 不像 Root-CA，起点可能算错")
    tid_ticket = be64(d, ot + TICKET_TID_OFF)
    tid_tmd = be64(d, om + TMD_TID_OFF)

    # NCCH：在 content 区找 magic
    ncch = None
    scan_from = oc
    scan_to = min(len(d), oc + 0x4000) if metasz else len(d)
    i = d.find(b"NCCH", scan_from, scan_to)
    if i >= 0 and i - NCCH_MAGIC_OFF >= oc:
        ncch = i - NCCH_MAGIC_OFF
    info["ncch_off"] = ncch
    tid_ncch = le64(d, ncch + NCCH_PROGRAMID_OFF) if ncch is not None else None

    info["tid_ticket"], info["tid_tmd"], info["tid_ncch"] = (
        tid_ticket, tid_tmd, tid_ncch)

    votes = {}
    for tid in (tid_ticket, tid_tmd, tid_ncch):
        votes[tid] = votes.get(tid, 0) + 1
    if len(votes) != 1:
        info["problems"].append(
            "★ 三处来源的 title id 不一致 —— 偏移表对这份文件不适用，"
            "本工具给出的 ID 不可信，请勿据此下结论：ticket=%s tmd=%s ncch=%s"
            % (hx(tid_ticket), hx(tid_tmd), hx(tid_ncch)))
        info["tid"] = None
    else:
        info["tid"] = tid_ticket

    info["ver_ticket"] = be16(d, ot + TICKET_VER_OFF)
    info["ver_tmd"] = be16(d, om + TMD_VER_OFF)
    if info["ver_ticket"] != info["ver_tmd"]:
        info["problems"].append(
            "ticket(%d) 与 tmd(%d) 里的版本不一致" % (info["ver_ticket"],
                                                 info["ver_tmd"]))
    info["version"] = info["ver_tmd"]
    info["nchunk"] = be16(d, om + TMD_NCHUNK_OFF)

    if ncch is not None:
        pc = d[ncch + NCCH_PRODUCTCODE_OFF:ncch + NCCH_PRODUCTCODE_OFF + 0x10]
        info["product_code"] = pc.split(b"\x00")[0].decode("ascii", "replace")
    else:
        info["product_code"] = "?"

    if metasz:
        # meta 在 content **之后**，不是紧接着 tmd。content 大小得从 chunk
        # 表算，那个表的偏移没实测过 —— 直接搜 magic 更稳，也不引入假偏移。
        i = d.find(b"SMDH", oc)
        ms = d[i:i + metasz] if i >= 0 else b""
        info["meta_title"] = (_smdh_titles(ms) if ms else "（没找到 SMDH 段，桌面图标会不显示）")
    else:
        info["meta_title"] = None
    info["ok"] = True
    return info


def hx(v):
    return "%016X" % v if v is not None else "（读不到）"


def _smdh_titles(s):
    out = []
    for lang in (1, 6):      # 1=English, 6=简体中文
        b = 0x08 + lang * 0x200
        t = s[b:b + 0x80].decode("utf-16-le", "replace").rstrip("\x00")
        out.append("lang%d=%r" % (lang, t))
    return "  ".join(out)


def audit(info):
    reasons, bad = [], False
    tid = info.get("tid")
    if tid is None:
        return "NO", ["title id 三处来源不一致，无法审计身份"]
    if tid in RESERVED:
        bad = True
        reasons.append("★ Title ID %s 已被【%s】占用 —— 装它会覆盖人家"
                       % (hx(tid), RESERVED[tid]))
    uid = (tid >> 8) & 0xFFFFFF
    if UID_EVAL_MIN <= uid <= UID_EVAL_MAX:
        bad = True
        reasons.append("UniqueId 0x%X 落在 Evaluation（评测版）段，"
                       "Application 段的合法上界是 0x%X" % (uid, UID_APP_MAX))
    elif not (UID_APP_MIN <= uid <= UID_APP_MAX):
        bad = True
        reasons.append("UniqueId 0x%X 落在 Application 段之外" % uid)
    if info.get("version") == 0:
        reasons.append("title version = 0 —— 真机 AM 会报 "
                       "0xD8E08027 Invalid or outdated title version")
    return ("BAD" if bad else "OK"), reasons


# ---------------------------------------------------------------- selftest
CSZ, TKSZ, TMDSZ = 0xA00, 0x350, 0xB34     # 与真产物同源的分区大小
NCCH_SZ = 0x600                            # 夹具里最小的 NCCH 头


def _mk(tid, ver, nchunk=1, ncch_tid=None, tmpdir=None):
    """构造一份最小的、布局与真产物同源的 CIA 夹具，写入临时文件并返回路径。

    ⚠️ 夹具不是"随便塞几个字节"。它按 cert/ticket/tmd/content 真实分区铺开，
    四个区互不重叠。早先为了省事把 ticket 和 tmd 叠在同一处，结果 title id
    (u64 @+0x1DC) 和 content 数(u16 @+0x22E) 压在不同的工作人员的字节上，
    夹具自己是错的 —— 用错夹具验出来的"通过"毫无意义（项目铁律 6b）。
    """
    tmpdir = tmpdir or os.environ.get("TEMP", ".")
    path = os.path.join(tmpdir, "_ciainfo_%08X_%d_%s.bin"
                        % (tid & 0xFFFFFFFF, ver,
                           "mix" if ncch_tid is not None else "std"))
    total = 0x2020 + CSZ + TKSZ + TMDSZ + NCCH_SZ
    b = bytearray(total)
    struct.pack_into("<I", b, 0, 0x2020)
    struct.pack_into("<IIII", b, 8, CSZ, TKSZ, TMDSZ, 0)
    ot = 0x2020 + CSZ
    om = ot + TKSZ
    oc = om + TMDSZ
    ncch = oc
    struct.pack_into(">Q", b, ot + TICKET_TID_OFF, tid)
    struct.pack_into(">H", b, ot + TICKET_VER_OFF, ver)
    struct.pack_into(">Q", b, om + TMD_TID_OFF, tid)
    struct.pack_into(">H", b, om + TMD_VER_OFF, ver)
    struct.pack_into(">H", b, om + TMD_NCHUNK_OFF, nchunk)
    struct.pack_into("4s", b, ncch + NCCH_MAGIC_OFF, b"NCCH")
    struct.pack_into("<Q", b, ncch + NCCH_PROGRAMID_OFF,
                     tid if ncch_tid is None else ncch_tid)
    struct.pack_into("10s", b, ncch + NCCH_PRODUCTCODE_OFF, b"CTR-P-AICN")
    open(path, "wb").write(bytes(b))
    return path


def selftest():
    print("=" * 74)
    print("selftest: 判据必须能红也能绿 —— 只会绿的守卫比没有更坏")
    print("=" * 74)
    bad, made = [], []

    def check(name, want, tid, ver=4096, ncch_tid=None, want_warn=None):
        pth = _mk(tid, ver, ncch_tid=ncch_tid)
        made.append(pth)
        r = parse(pth)
        # ★ 夹具本身也要被审：三处来源对不上说明夹具或偏移错了，直接算失败
        if any("不一致" in x for x in r["problems"]) and ncch_tid is None:
            print("  [FAIL] %-34s -> 干净夹具却报不一致，偏移表有问题" % name)
            bad.append(name)
            return
        actual, why = audit(r)
        flag = "OK " if actual == want else "FAIL"
        print("  [%s] %-34s -> %s（期望 %s）" % (flag, name, actual, want))
        for x in why:
            print("         %s" % x)
        if actual != want:
            bad.append(name)
        if want_warn and not any(want_warn in x for x in why):
            bad.append(name + " 缺少告警：%s" % want_warn)
            print("  [FAIL] 缺少预期告警: %s" % want_warn)

    check("干净 ID + 版本号非 0", "OK", 0x000400000A1C0100)
    check("撞 FBI 的 ID", "BAD", 0x000400000F800100)
    check("UniqueId 落在 Evaluation 段", "BAD", 0x00040000FF000100)
    check("version = 0（AM 会拒装）", "OK", 0x000400000A1C0100, ver=0,
          want_warn="title version = 0")

    # 反例：三处来源不一致必须被抓 —— 这条是"偏移表自己坏掉"时的最后防线
    pth = _mk(0x000400000A1C0100, 4096, ncch_tid=0x000400000BAD0100)
    made.append(pth)
    r = parse(pth)
    caught = any("不一致" in x for x in r["problems"])
    print("  [%s] NCCH 与 ticket/tmd 的 ID 不一致 -> 抓到=%s"
          % ("OK " if caught else "FAIL", caught))
    if r["problems"]:
        print("        报法: %s" % r["problems"][0][:150])
    if not caught:
        bad.append("三处不一致没被抓到")

    for fp in made:
        if os.path.exists(fp):
            os.remove(fp)

    print("=" * 74)
    if bad:
        print("selftest FAILED: " + "; ".join(bad))
        return 1
    print("selftest PASSED")
    return 0


def main():
    args = sys.argv[1:]
    if not args:
        print(__doc__)
        return 0
    if args[0] == "--selftest":
        return selftest()
    worst = 0
    for path in args:
        print("=" * 74)
        print("文件: %s" % path)
        if not os.path.exists(path):
            print("   不存在")
            worst = 1
            continue
        info = parse(path)
        for n in info["notes"]:
            print("   note        : %s" % n)
        if not info["ok"]:
            print("   解析失败: %s" % "; ".join(info["problems"]))
            worst = 1
            continue
        print("   大小        : %d 字节 (crc32 %08X)"
              % (info["size"], binascii.crc32(open(path, "rb").read())))
        print("   Title ID    : %s" % hx(info["tid"]))
        print("       三处来源: ticket=%s / tmd=%s / ncch=%s"
              % (hx(info["tid_ticket"]), hx(info["tid_tmd"]),
                 hx(info["tid_ncch"])))
        print("   version     : %d  (ticket %d / tmd %d)"
              % (info["version"], info["ver_ticket"], info["ver_tmd"]))
        print("   ProductCode : %s" % info["product_code"])
        print("   content 段数: %d" % info["nchunk"])
        print("   meta(SMDH)  : %s" % (info["meta_title"] or "（无）"))
        verdict, reasons = audit(info)
        print("   ---- 身份审计: %s ----" % verdict)
        for r in reasons:
            print("     " + r)
        if verdict != "OK":
            worst = 1
    return worst


if __name__ == "__main__":
    sys.exit(main())
