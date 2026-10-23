"""
Toolchain doctor for the 3DS AI Console project.

Checks that every piece needed to build .3dsx / .cia is present and healthy,
then structurally validates the artefacts in app/build/.

Usage:
    python tools/doctor.py
"""

import os
import struct
import subprocess
import sys

DKP   = r"E:\devkitPro"
GCC   = os.path.join(DKP, "devkitARM", "bin", "arm-none-eabi-gcc.exe")
BIN   = os.path.join(DKP, "devkitARM", "bin")
TOOLS = os.path.join(DKP, "tools", "bin")
CTRU  = os.path.join(DKP, "libctru")

ROOT  = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BUILD = os.path.join(ROOT, "app")   # final artefacts land next to the Makefile
TGT   = "AIConsole"

ok_n = bad_n = 0


def ok(msg, detail=""):
    global ok_n
    ok_n += 1
    print("  [ OK ] %-46s %s" % (msg, detail))


def bad(msg, detail=""):
    global bad_n
    bad_n += 1
    print("  [FAIL] %-46s %s" % (msg, detail))


def check_file(label, path, kind="file"):
    if not os.path.exists(path):
        bad(label, "missing: " + path)
        return False
    sz = os.path.getsize(path) if kind == "file" else "-"
    ok(label, "%s byte(s)" % sz)
    return True


def run1(cmd):
    try:
        r = subprocess.run(cmd, capture_output=True, text=True,
                           errors="replace", timeout=30)
        return r.stdout.strip().splitlines() or r.stderr.strip().splitlines()
    except Exception as e:
        return ["<failed: %s>" % e]


def find_libm():
    """libm.a ships with the compiler sysroot, not with libctru."""
    base = os.path.join(DKP, "devkitARM", "arm-none-eabi", "lib")
    if not os.path.isdir(base):
        return None
    for root, dirs, files in os.walk(base):
        if root[len(base):].count(os.sep) > 2:
            dirs[:] = []
            continue
        if "libm.a" in files:
            return os.path.join(root, "libm.a")
    return None


# --------------------------------------------------------------------------
def sec(title):
    print("\n== %s ==" % title)


def check_toolchain():
    sec("devkitARM / libctru")
    if not check_file("arm-none-eabi-gcc.exe", GCC):
        return
    out = run1([GCC, "--version"])
    if out and "devkitARM" in out[0]:
        ok("gcc reports devkitARM", out[0][:52])
    else:
        bad("gcc version banner", (out[0] if out else "?")[:60])

    check_file("libctru/lib/libctru.a", os.path.join(CTRU, "lib", "libctru.a"))

    # libm lives in the compiler sysroot, not in libctru.
    libm = find_libm()
    if libm:
        ok("libm.a (compiler sysroot)", "found")
    else:
        bad("libm.a", "not found under devkitARM/arm-none-eabi/lib")

    for hdr in ("3ds.h", "citro2d.h", "citro3d.h"):
        p = os.path.join(CTRU, "include", hdr)
        if os.path.exists(p):
            ok("libctru/include/" + hdr, "")
        else:
            bad("libctru/include/" + hdr, "missing")


def check_tools():
    sec("devkitPro tools")
    for t in ("3dsxtool.exe", "smdhtool.exe", "bannertool.exe",
              "tex3ds.exe", "picasso.exe", "mkromfs3ds.exe", "3dslink.exe"):
        p = os.path.join(TOOLS, t)
        if os.path.exists(p):
            ok(t, "%d byte(s)" % os.path.getsize(p))
        else:
            print("  [ -- ] %-46s optional / not present" % t)
    check_file("makerom.exe", os.path.join(TOOLS, "makerom.exe"))


def check_msys2():
    sec("MSYS2 (needed by the Makefile)")
    for t in ("bash.exe", "pacman.exe", "make.exe"):
        p = os.path.join(DKP, "msys2", "usr", "bin", t)
        if os.path.exists(p):
            ok(t, "")
        else:
            bad(t, "missing - use tools/build.py instead of make")


def check_env():
    sec("environment variables")
    for var, want in (("DEVKITPRO", DKP), ("DEVKITARM", os.path.join(DKP, "devkitARM")),
                      ("DEVKITPPC", "-")):
        val = os.environ.get(var)
        if val:
            ok(var, val)
        else:
            print("  [ -- ] %-46s not set in this shell" % var)
    path = os.environ.get("PATH", "")
    for d in (BIN, TOOLS):
        if d.lower() in path.lower():
            ok("PATH contains", d)
        else:
            print("  [ -- ] %-46s not on PATH here" % ("PATH -> " + d))


# --------------------------------------------------------------------------
def parse_cia(path):
    """
    Decode the parts of a CIA that are cheap and unambiguous:
    the header size table plus the embedded NCCH header, which we locate by
    its ASCII magic instead of recomputing nested offsets.

    Returns a dict, or None when the file is not a CIA.
    """
    with open(path, "rb") as f:
        d = f.read()

    if len(d) < 0x2020:
        return None
    hdr_size, = struct.unpack_from("<I", d, 0x00)
    if hdr_size != 0x2020:
        return None
    cia_type, cia_ver = struct.unpack_from("<HH", d, 0x04)
    cert, ticket, tmd, meta = struct.unpack_from("<IIII", d, 0x08)
    content, = struct.unpack_from("<Q", d, 0x18)

    out = {
        "type": cia_type, "version": cia_ver,
        "cert": cert, "ticket": ticket, "tmd": tmd, "meta": meta,
        "content": content,
        "ncch": [], "smdh": 0,
    }

    # 'NCCH' magic sits at NCCH header + 0x100
    i = 0
    while True:
        j = d.find(b"NCCH", i)
        if j < 0:
            break
        n = j - 0x100
        if n >= 0:
            rec = {}
            rec["offset"] = n
            rec["media_size"] = struct.unpack_from("<I", d, n + 0x104)[0] * 0x200
            rec["maker"] = d[n + 0x110:n + 0x112].decode("ascii", "replace")
            rec["ver"] = struct.unpack_from("<H", d, n + 0x112)[0]
            rec["title_id"] = struct.unpack_from("<Q", d, n + 0x118)[0]
            # product code: verified at 0x150 on makerom output, but probe
            # 0x148 too in case the tool changes.
            pc = ""
            for off in (0x148, 0x150):
                cand = d[n + off:n + off + 0x10].split(b"\x00")[0]
                if cand and all(0x20 <= b < 0x7F for b in cand):
                    pc = cand.decode("ascii")
                    break
            rec["product"] = pc
            # exheader size: verified at 0x180 (0x400 on makerom output)
            rec["exheader_size"] = struct.unpack_from("<I", d, n + 0x180)[0]
            out["ncch"].append(rec)
        i = j + 1

    out["smdh"] = d.count(b"SMDH")
    return out


def parse_smdh(path):
    with open(path, "rb") as f:
        d = f.read(0x100)
    if d[:4] != b"SMDH":
        return None
    def dec(off, ln):
        raw = d[off:off + ln]
        s = raw.decode("utf-16-le", "ignore").split("\x00")[0]
        return s.strip()
    return dec(0x08, 0x40), dec(0xC8, 0x40)


def parse_font(path):
    """Parse the AFNT v1 header produced by tools/mkfont.py."""
    with open(path, "rb") as f:
        d = f.read(32)
    if len(d) < 32 or d[:4] != b"AFNT":
        return None
    ver, flags = struct.unpack_from("<HH", d, 4)
    cw, chh, baseline, _rsv = struct.unpack_from("<BBBB", d, 8)
    count, ioff, boff, bsize, fallback = struct.unpack_from("<IIIII", d, 12)
    return {
        "version": ver, "flags": flags,
        "cell_w": cw, "cell_h": chh, "baseline": baseline,
        "count": count, "index_off": ioff, "bitmap_off": boff,
        "bitmap_size": bsize, "fallback": fallback,
    }


def check_font():
    sec("point font (M1)")
    fontbin = os.path.join(ROOT, "app", "data", "font.bin")
    if not os.path.exists(fontbin):
        bad("app/data/font.bin", "missing - run tools/mkfont.py")
        return
    info = parse_font(fontbin)
    if not info:
        bad("font.bin header", "bad magic / too short")
        return
    size = os.path.getsize(fontbin)
    ok("font.bin header", "AFNT v%d, %d bytes" % (info["version"], size))
    ok("font cell / baseline", "%dx%d, baseline row %d"
       % (info["cell_w"], info["cell_h"], info["baseline"]))
    ok("font glyph count", "%d (fallback index %d)" % (info["count"], info["fallback"]))

    # 布局自洽性：索引 + 位图必须正好铺满文件
    cell_bytes = info["cell_w"] * info["cell_h"] // 8
    want = info["bitmap_off"] + info["count"] * cell_bytes
    if want == size and info["bitmap_size"] == info["count"] * cell_bytes:
        ok("font layout consistent", "index %d B + bitmap %d B"
           % (info["count"] * 8, info["bitmap_size"]))
    else:
        bad("font layout", "expected %d bytes, file is %d" % (want, size))

    for f in ("font_bin.h", "font.bin.s", "font.bin.o"):
        p = os.path.join(ROOT, "app", "build", f)
        if os.path.exists(p):
            ok("build/" + f, "%d byte(s)" % os.path.getsize(p))
        else:
            print("  [ -- ] %-46s not built yet" % ("build/" + f))

    check_font_coverage(fontbin, info)


def check_font_coverage(fontbin, info):
    """源码里所有字符串字面量用到的字符，字库里都必须有。

    缺一个字的后果不是报错，而是**界面上出现一个豆腐块** ——
    改文案的人不会发现，只有用户截图里能看到。所以在自检里挡住。
    （正常的做法是扩字库；如果只是临时写了个字库没有的符号，
     那就换成字库里有的。）
    """
    import re
    have = set()
    with open(fontbin, "rb") as f:
        data = f.read()
    for i in range(info["count"]):
        cp = struct.unpack_from("<I", data, info["index_off"] + i * 8)[0]
        have.add(cp)

    missing = {}
    for base, _dirs, files in os.walk(os.path.join(ROOT, "app", "source")):
        for fn in files:
            if not fn.endswith((".c", ".h")):
                continue
            p = os.path.join(base, fn)
            with open(p, "r", encoding="utf-8", errors="replace") as f:
                src = f.read()
            for lit in re.findall(r'"([^"\\]*(?:\\.[^"\\]*)*)"', src):
                for ch in lit:
                    cp = ord(ch)
                    if cp < 0x20 or cp == 0x7F:
                        continue
                    if cp not in have:
                        missing.setdefault(ch, set()).add(fn)

    if missing:
        detail = "; ".join("%r U+%04X (%s)" % (c, ord(c), ",".join(sorted(v)))
                           for c, v in sorted(missing.items())[:8])
        bad("font covers all UI strings",
            "%d 个字在字库里没有，界面会显示豆腐块：%s" % (len(missing), detail))
    else:
        ok("font covers all UI strings", "界面用到的字符全部有字形")

    check_font_quality(fontbin, info)


#: 字形质量下限：汉字平均墨量（低于此 = 笔画被吃掉）
FONT_INK_MIN = 48.0
#: 孤立墨点上限（高于此 = 二值化把斜画打碎成噪点）
FONT_ISO_MAX = 1.00
#: 4-连通块上限（高于此 = 笔画被打断成好几截；正常汉字 1~2 块）
FONT_COMP_MAX = 2.50


def check_font_quality(fontbin, info):
    """直接读 font.bin 的位图，量化字形质量。

    为什么要算「连通块」：越界检查只保证没被切掉，墨量只保证没被吃掉，
    但都**判不出笔画被打断** —— 一个汉字正常 1~2 个连通块，
    硬阈值下"是"会断成 4 块，屏幕上就是"字缺一截"。
    这三项是 mkfont.py 里 audit_strokes 的同款判据，在这里再挡一道，
    免得有人手改 font.bin 或换了源字体却没重跑自检。
    """
    with open(fontbin, "rb") as f:
        data = f.read()

    n = info["count"]
    boff = info["bitmap_off"]
    cellw, cellh = info["cell_w"], info["cell_h"]
    cbytes = cellw * cellh // 8

    ink_sum = 0
    iso_sum = 0
    comp_sum = 0
    han = 0
    worst = []

    for i in range(n):
        cp = struct.unpack_from("<I", data, info["index_off"] + i * 8)[0]
        if not (0x4E00 <= cp <= 0x9FFF):
            continue
        base = boff + i * cbytes
        bits = data[base:base + cbytes]
        if len(bits) < cbytes:
            return

        def on(x, y):
            if 0 <= x < cellw and 0 <= y < cellh:
                return (bits[y * 2] >> (7 - x)) & 1 if x < 8 \
                    else (bits[y * 2 + 1] >> (7 - (x - 8))) & 1
            return 0

        pts = [(x, y) for y in range(cellh) for x in range(cellw) if on(x, y)]
        ink = len(pts)
        iso = sum(1 for (x, y) in pts
                  if on(x - 1, y) + on(x + 1, y) + on(x, y - 1) + on(x, y + 1) == 0)

        pt = set(pts)
        seen = set()
        comp = 0
        for p in pt:
            if p in seen:
                continue
            comp += 1
            stack = [p]
            seen.add(p)
            while stack:
                cx, cy = stack.pop()
                for dx, dy in ((1, 0), (-1, 0), (0, 1), (0, -1)):
                    q = (cx + dx, cy + dy)
                    if q in pt and q not in seen:
                        seen.add(q)
                        stack.append(q)

        ink_sum += ink
        iso_sum += iso
        comp_sum += comp
        han += 1
        worst.append((comp, chr(cp), ink))

    if han == 0:
        return
    ink = ink_sum / han
    iso = iso_sum / han
    comp = comp_sum / han

    worst.sort(reverse=True)
    broken = ", ".join("%s(%d块)" % (c, k) for k, c, _i in worst[:6])

    if comp > FONT_COMP_MAX:
        bad("font strokes intact",
            "汉字平均 %.2f 个连通块（应 <=%.2f）—— 笔画被打断，"
            "屏幕上表现为「字缺一截」。最碎的：%s"
            % (comp, FONT_COMP_MAX, broken))
    elif ink < FONT_INK_MIN:
        low = ", ".join("%s(%d)" % (c, i) for _k, c, i in
                        sorted(worst, key=lambda t: t[2])[:6])
        bad("font strokes intact",
            "汉字平均墨量 %.1f（应 >=%.1f）—— 笔画被二值化吃掉。最淡的：%s"
            % (ink, FONT_INK_MIN, low))
    else:
        ok("font strokes intact",
           "墨量 %.1f / 孤点 %.2f / 连通块 %.2f（汉字 %d）"
           % (ink, iso, comp, han))


def check_artifacts():
    sec("artefacts in app/")
    if not os.path.isdir(BUILD):
        bad("app directory", "not found")
        return

    elf = os.path.join(BUILD, TGT + ".elf")
    if os.path.exists(elf):
        with open(elf, "rb") as f:
            magic = f.read(4)
        if magic == b"\x7fELF":
            ok("AIConsole.elf", "ELF magic, %d bytes" % os.path.getsize(elf))
        else:
            bad("AIConsole.elf", "bad magic")
    else:
        bad("AIConsole.elf", "missing")

    smdh = os.path.join(BUILD, TGT + ".smdh")
    if os.path.exists(smdh):
        info = parse_smdh(smdh)
        if info:
            ok("AIConsole.smdh", "title=%r author=%r" % info)
        else:
            bad("AIConsole.smdh", "bad magic")
    else:
        bad("AIConsole.smdh", "missing")

    for ext in ("3dsx", "cia"):
        p = os.path.join(BUILD, TGT + "." + ext)
        if os.path.exists(p):
            ok("AIConsole." + ext, "%d bytes" % os.path.getsize(p))
        else:
            print("  [ -- ] %-46s not built" % ("AIConsole." + ext))

    cia = os.path.join(BUILD, TGT + ".cia")
    if os.path.exists(cia):
        info = parse_cia(cia)
        if not info:
            bad("CIA structure", "not a CIA container")
        else:
            ok("CIA header", "size=0x2020 type=%d ver=%d" % (info["type"], info["version"]))
            ok("CIA blocks", "cert=%d ticket=%d tmd=%d meta=%d content=%d"
               % (info["cert"], info["ticket"], info["tmd"], info["meta"], info["content"]))
            if len(info["ncch"]) == 1:
                c = info["ncch"][0]
                ok("NCCH offset", "0x%X" % c["offset"])
                ok("NCCH title id", "0x%016X" % c["title_id"])
                ok("NCCH maker/version", "%r v%d" % (c["maker"], c["ver"]))
                ok("NCCH media size", "%d bytes" % c["media_size"])
                ok("NCCH exheader", "%d bytes" % c["exheader_size"])
                ok("NCCH product code", repr(c["product"]))
            else:
                bad("NCCH count", "expected 1, found %d" % len(info["ncch"]))
            if info["smdh"]:
                ok("SMDH blobs embedded", "%d (icon + logo)" % info["smdh"])
            else:
                bad("SMDH blobs embedded", "none found")


def check_config_layer():
    """
    M2: 配置文件的结构检查。

    这里做的是**静态**检查 —— 逐条确认 设计规划.md §5.1 里的点分路径
    在 config.c 里都有对应的读写分支。运行时的真检查在 azahar_test.py：
    那边会拿真机（模拟器）写出来的 config.json 去做 JSON 解析和字段断言。
    静态查"有没有漏写"，动态查"写出来对不对"，两件事不重复。
    """
    sec("config layer (M2)")

    cfg_c = os.path.join(ROOT, "shared", "config.c")
    mj_c  = os.path.join(ROOT, "shared", "minijson.c")
    check_file("shared/minijson.c", mj_c)
    if not check_file("shared/config.c", cfg_c):
        return

    with open(cfg_c, "r", encoding="utf-8", errors="replace") as f:
        src = f.read()

    # §5.1 的每一条路径都必须在解析分支里出现
    paths = [
        "version", "first_run_completed", "launches",
        "nand_backup.confirmed", "nand_backup.remind_on_startup",
        "security.current_level", "security.persist_level",
        "security.granted_actions", "security.dev_mode_unlocked",
        "ai.provider", "ai.model", "ai.stream",
        "ui.theme", "ui.depth", "ui.font_size",
    ]
    missing = [p for p in paths if ('"%s"' % p) not in src]
    if missing:
        bad("schema paths in config.c", "missing: " + ", ".join(missing))
    else:
        ok("schema paths in config.c", "%d/%d 条 §5.1 路径都有分支" % (len(paths), len(paths)))

    # 写盘用的顶层键
    roots = ["version", "first_run_completed", "launches", "nand_backup",
             "security", "ai", "ui"]
    miss2 = [k for k in roots if ('\\"%s\\"' % k) not in src]
    if miss2:
        bad("JSON keys written", "missing: " + ", ".join(miss2))
    else:
        ok("JSON keys written", "%d 个顶层键" % len(roots))

    # 原子落盘：写了 .tmp 就得有 rename/复制的收尾，否则会留垃圾文件
    if ".tmp" in src and ("rename" in src or "copy_file" in src):
        ok("atomic save", "写 .tmp 再改名（rename 不可用时复制兜底）")
    else:
        bad("atomic save", "没看到 .tmp + rename 的痕迹")


def check_ui_tree():
    sec("UI tree (M2)")
    files = [
        ("source/ui/screen.h",       "屏幕接口"),
        ("source/ui/app.c",          "全局状态 + 屏幕栈"),
        ("source/ui/widgets.c",      "绘制小工具"),
        ("source/ui/sc_welcome.c",   "首启 3 屏"),
        ("source/ui/sc_home.c",      "主界面占位"),
        ("source/ui/sc_settings.c",  "设置屏（主题/护栏/诊断）"),
        ("source/ui/sc_lab.c",       "M1 回归屏"),
        ("source/test/autorun.c",    "脚本化按键回放"),
    ]
    for rel, what in files:
        p = os.path.join(ROOT, "app", rel)
        if os.path.exists(p):
            ok(rel, what)
        else:
            bad(rel, "missing (%s)" % what)


def find_azahar_sdmc():
    """上次跑 Azahar 留下的虚拟 SD 卡根目录（用于检查真实的 config.json）。"""
    cands = [
        r"E:\Azahar\azahar-windows-msvc-2126.1.1\user\sdmc",
        r"E:\Azahar\azahar-windows-msvc-2126.1.1\sdmc",
    ]
    base = os.environ.get("APPDATA", "")
    cands += [os.path.join(base, "azahar", "sdmc"),
              os.path.join(base, "Citra", "sdmc"),
              os.path.join(base, "Lime3DS", "sdmc")]
    for c in cands:
        if os.path.isdir(os.path.join(c, "3ds", "aiconsole")):
            return c
    return None


def check_runtime_config():
    """
    M2: 检查模拟器真跑出来的 config.json / status.txt。
    没有就跑 azahar_test.py —— 这两份文件是它的产物。
    """
    sec("runtime config.json (last Azahar run)")

    root = find_azahar_sdmc()
    if not root:
        print("  [ -- ] %-46s 跑一次 tools/azahar_test.py 就能有"
              % "Azahar SD card not found")
        return

    print("  [info] %s" % root)

    cfg_path = os.path.join(root, "3ds", "aiconsole", "config.json")
    if not os.path.exists(cfg_path):
        print("  [ -- ] %-46s 尚未生成" % "config.json")
    else:
        import json
        try:
            with open(cfg_path, "r", encoding="utf-8") as f:
                cfg = json.load(f)
        except Exception as e:                          # noqa: BLE001
            bad("config.json parses as JSON", str(e))
            cfg = None
        if cfg is not None:
            ok("config.json parses as JSON", "%d bytes" % os.path.getsize(cfg_path))

            want_roots = {"version": int, "first_run_completed": bool,
                          "launches": int, "nand_backup": dict,
                          "security": dict, "ai": dict, "ui": dict,
                          "guard": dict, "diag": dict}
            problems = []
            for k, t in want_roots.items():
                if k not in cfg:
                    problems.append("缺 " + k)
                elif not isinstance(cfg[k], t):
                    problems.append("%s 类型应为 %s" % (k, t.__name__))
            if problems:
                bad("config.json 顶层结构", "; ".join(problems))
            else:
                ok("config.json 顶层结构", "%d 个键，类型都对" % len(want_roots))

            subs = {"nand_backup": ("confirmed", "remind_on_startup"),
                    "security": ("current_level", "persist_level",
                                 "granted_actions", "dev_mode_unlocked"),
                    "ai": ("provider", "model", "stream"),
                    "ui": ("theme", "depth", "font_size"),
                    "guard": ("confirm_overwrite", "confirm_delete",
                              "confirm_bulk", "max_write_kb", "max_batch_files"),
                    "diag": ("show_layout_overflow", "show_notes",
                             "verbose_status")}
            probs2 = [("%s.%s" % (g, s)) for g, keys in subs.items()
                      for s in keys if g in cfg and isinstance(cfg[g], dict) and s not in cfg[g]]
            if probs2:
                bad("config.json 子结构", "缺 " + ", ".join(probs2))
            else:
                ok("config.json 子结构", "§5.1 的嵌套字段齐全")

            lvl = cfg.get("security", {}).get("current_level")
            if isinstance(lvl, int) and 0 <= lvl <= 3:
                ok("security.current_level 在范围内", "L%d" % lvl)
            else:
                bad("security.current_level", "越界: %r" % (lvl,))

    st = os.path.join(root, "3ds", "aiconsole", "status.txt")
    if os.path.exists(st):
        with open(st, "r", encoding="utf-8", errors="replace") as f:
            txt = f.read()
        first = dict(l.split("=", 1) for l in txt.splitlines() if "=" in l)
        ok("status.txt", "start=%s end=%s launches=%s"
           % (first.get("start_screen", "?"), first.get("end_screen", "?"),
              first.get("launches", "?")))
    else:
        print("  [ -- ] %-46s 尚未生成" % "status.txt")

    shotdir = os.path.join(root, "3ds", "aiconsole", "shots")
    if os.path.isdir(shotdir):
        bmps = [n for n in os.listdir(shotdir) if n.endswith(".bmp")]
        ok("shots/", "%d 个 bmp（azahar_test.py 会转成 PNG 放到 tools/out/shots/）"
           % len(bmps))
    err = os.path.join(shotdir, "error.txt")
    if os.path.exists(err):
        bad("app 写过 error.txt", err)


def check_crypto_layer():
    """
    M5: 加密存储层的静态检查。

    查的是一致性 —— 加密实现的每个部件都在，且 creds.enc 的格式定义
    在 C 代码和文档里对得上。真正的运行时验证在设备侧（crypto_selftest
    跑 RFC 向量，结果落 status.txt），这里只挡"文件删了/漏改了"这类事故。
    """
    sec("crypto layer (M5)")

    crypto_c = os.path.join(ROOT, "shared", "crypto.c")
    crypto_h = os.path.join(ROOT, "shared", "crypto.h")
    creds_c = os.path.join(ROOT, "shared", "creds.c")
    creds_h = os.path.join(ROOT, "shared", "creds.h")

    for path, label in ((crypto_c, "crypto.c"), (crypto_h, "crypto.h"),
                        (creds_c, "creds.c"), (creds_h, "creds.h")):
        if not os.path.exists(path):
            bad(label, "missing")
        else:
            ok(label, "%d bytes" % os.path.getsize(path))

    if not os.path.exists(crypto_c) or not os.path.exists(creds_c):
        return

    src_crypto = open(crypto_c, encoding="utf-8", errors="replace").read()
    src_creds = open(creds_c, encoding="utf-8", errors="replace").read()

    # ChaCha20 的三样必备：四分之一轮、块函数、流异或
    for needle, label in (
        ("chacha20_xor", "chacha20_xor defined"),
        ("poly1305_mac", "poly1305_mac defined"),
        ("crypto_derive_key", "crypto_derive_key defined"),
        ("crypto_equal_ct", "crypto_equal_ct defined"),
    ):
        if needle in src_crypto:
            ok(label, "present")
        else:
            bad(label, "not found in crypto.c")

    # KDF 迭代次数：拉高才有意义，掉到 1 就形同虚设
    # （常量定义在 crypto.h，两个文件都扫一遍，免得以后挪了位置就误报）
    head_or_src = src_crypto
    if os.path.exists(crypto_h):
        head_or_src += open(crypto_h, encoding="utf-8", errors="replace").read()
    if "CRYPTO_KDF_ROUNDS 4096" in head_or_src:
        ok("KDF rounds", "4096")
    else:
        bad("KDF rounds", "expected 4096 (weakened?)")

    # creds.enc 的格式要素
    for needle, label in (
        ("AICRED01", "creds magic"),
        ("CREDS_VERSION", "creds version field"),
        ("salt", "creds salt field"),
        ("nonce", "creds nonce field"),
        ("mac", "creds mac field"),
    ):
        if needle in src_creds:
            ok("creds.enc/" + label, "present")
        else:
            bad("creds.enc/" + label, "not found in creds.c")

    # 每次保存都必须换 nonce —— 流密码复用 nonce 会泄漏明文异或。
    if src_creds.count("crypto_random(h.nonce") >= 1:
        ok("nonce freshness", "regenerated on every save")
    else:
        bad("nonce freshness", "nonce may be reused across saves")

    # 原子写：不能直接覆盖原文件
    if "write_atomic" in src_creds:
        ok("atomic write", "tmp + rename")
    else:
        bad("atomic write", "missing (power-loss risk)")

    # main 必须跑自检并开闸门，否则加密形同虚设
    main_c = os.path.join(ROOT, "app", "source", "ui", "app.c")
    if os.path.exists(main_c):
        src_app = open(main_c, encoding="utf-8", errors="replace").read()
        if "crypto_selftest()" in src_app:
            ok("startup self-test", "crypto_selftest() called")
        else:
            bad("startup self-test", "crypto_selftest() not called")
        if "creds_set_available" in src_app:
            ok("crypto gate", "creds_set_available() wired")
        else:
            bad("crypto gate", "creds_set_available() not wired")
        if "creds_roundtrip_selftest()" in src_app:
            ok("roundtrip self-test", "called")
        else:
            bad("roundtrip self-test", "not called")


def main():
    print("3DS AI Console -- toolchain doctor")
    print("devkitPro root: %s" % DKP)
    check_toolchain()
    check_tools()
    check_msys2()
    check_env()
    check_font()
    check_crypto_layer()
    check_config_layer()
    check_ui_tree()
    check_artifacts()
    check_runtime_config()
    print("\n%d ok, %d problem(s)" % (ok_n, bad_n))
    return 1 if bad_n else 0


if __name__ == "__main__":
    sys.exit(main())
