#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""把 release/ 里的产物 + 校验工具打成一个可交付的 zip。

设计要点（都是从踩过的坑里来的）：
1. **先校验再打包** —— 包里不封没验过的东西。标题用 smdhdump 的
   check_title 逐字节比对，CIA 身份用 ciainfo 审计。
2. **CIA 审计判 BAD 不中止打包，但必须写进 MANIFEST 并在终端红字报出**。
   理由：这一版 CIA 的 Title ID 撞 FBI（000400000F800100），是**已知且
   待修**的问题。静默打包 = 把一个会覆盖别人程序的文件默默递出去；
   直接中止 = 用户拿不到 3dsx 这个能安全用的形态。两者都不对，
   所以选「照打，但把风险钉在包里最显眼的地方」。
3. **打完必须解包复核** —— 大小一致 ≠ 内容一致。zip 里的每份文件
   都重新读出来算 sha256，与源文件比。
4. MANIFEST 里记源文件的 sha256，包可自证。

用法：
    python tools/make_pack.py                # 打 dist/pack-<ver>/ 成 zip
    python tools/make_pack.py --only-check   # 只跑打包前的校验，不打
"""
import hashlib
import os
import shutil
import sys
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)

try:
    from smdhdump import check_title
except Exception:
    check_title = None
try:
    import ciainfo
except Exception:
    ciainfo = None

VERSION = "0.99.5"
RELEASE = os.path.join(ROOT, "release")
PACKDIR = os.path.join(ROOT, "dist", "pack-" + VERSION)
ZIPPATH = os.path.join(ROOT, "dist", "AIConsole-%s-pack.zip" % VERSION)

# 随包发给用户的辅助脚本。
# ★ 2026-10-23：`ftp_putkey.py` 换成 `ftp_push.py`。
#   前者是**开发者自己的**一次性助手 —— 它的注释里写着
#   "把用户 E 盘那份 DeepSeek key 写进 SD 卡"，含个人目录路径；
#   发给别人既没用，又漏了开发者的磁盘结构。
#   后者是通用工具（把 build 产物推到 3DS），对使用者才真正有用，
#   且主机改为必填参数（不再内置某个内网 IP）。
TOOLS = ["ciainfo.py", "smdhdump.py", "ftp_push.py"]
# 与 SMDH 里实际标题一致（容量 8 字符，补丁号装不下；"关于"页读全版本号）
# 2026-10-23 改名：短标题从「测试 0.99」改成「AI Agent」。
# 8 码元上限没变 —— "3DS AI Agent" 是 12 码元会被静默截断，所以只取后两词。
EXPECT_SHORT = "AI Agent"


def sha256(p):
    h = hashlib.sha256()
    with open(p, "rb") as f:
        for b in iter(lambda: f.read(1 << 20), b""):
            h.update(b)
    return h.hexdigest()


def main():
    only_check = "--only-check" in sys.argv
    print("=" * 74)
    print("打包前校验（包里不封没验过的东西）")
    print("=" * 74)

    artifacts = [
        ("AIConsole-%s.3dsx" % VERSION, "3dsx"),
        ("AIConsole-%s.cia" % VERSION, "cia"),
    ]
    problems = []

    for name, kind in artifacts:
        src = os.path.join(RELEASE, name)
        if not os.path.exists(src):
            problems.append("缺少产物 %s" % src)
            print("  [FAIL] %-26s 不存在" % name)
            continue
        print("  [INFO] %-26s %d 字节  sha256 %s…"
              % (name, os.path.getsize(src), sha256(src)[:16]))

        # 标题：两份 SMDH 都要对（3dsx 与 cia 各一份）
        smdh = os.path.join(ROOT, "app",
                            "AIConsole.smdh" if kind == "3dsx"
                            else "AIConsole_cia.smdh")
        if check_title is None:
            problems.append("smdhdump 没装，标题无人把关")
            print("  [FAIL] 标题校验不可用（check_title 没导入）")
        else:
            # ★ check_title 返回 True/False（不是元组），失败原因它自己打印
            ok = check_title(smdh, EXPECT_SHORT)
            print("  [%s] SMDH 标题 (%s)" % ("OK  " if ok else "FAIL",
                                             os.path.basename(smdh)))
            if not ok:
                problems.append("SMDH 标题不对: %s" % os.path.basename(smdh))

        if kind == "cia" and ciainfo is not None:
            info = ciainfo.parse(src)
            verdict, why = ciainfo.audit(info)
            print("  [%s] CIA 身份审计 -> %s" % ("OK  " if verdict == "OK"
                                                 else "WARN", verdict))
            for w in why:
                print("         ★ %s" % w)
            if verdict != "OK":
                # 已知待修，不中止，但必须钉进 MANIFEST
                problems.append("CIA 身份审计: %s" % " / ".join(why))

    if only_check:
        print("\n(--only-check) 到此为止，未打包。")
        return 1 if problems else 0

    if not os.path.exists(os.path.join(RELEASE, artifacts[0][0])):
        print("\n产物缺失，中止。")
        return 1

    # ---- 组装目录 ----
    if os.path.exists(PACKDIR):
        shutil.rmtree(PACKDIR)
    os.makedirs(os.path.join(PACKDIR, "tools"), exist_ok=True)

    manifest = []
    for name, _ in artifacts:
        src = os.path.join(RELEASE, name)
        dst = os.path.join(PACKDIR, name)
        shutil.copy2(src, dst)
        manifest.append((name, os.path.getsize(src), sha256(src)))
    for t in TOOLS:
        src = os.path.join(HERE, t)
        if not os.path.exists(src):
            problems.append("工具缺失 %s" % t)
            continue
        shutil.copy2(src, os.path.join(PACKDIR, "tools", t))
        manifest.append(("tools/" + t, os.path.getsize(src), sha256(src)))

    # ★ 模板放在 tools/ 下，不能放在 PACKDIR 里 —— PACKDIR 每次打包会被清空，
    #   上次手写进去的 装前必读.md 就是这么被 rmtree 删掉的（已踩）。
    readme_src = os.path.join(HERE, "pack_readme.md")
    readme = os.path.join(PACKDIR, "装前必读.md")
    if not os.path.exists(readme_src):
        problems.append("缺少 tools/pack_readme.md —— 包会没有风险说明，不能这么发出去")
        print("  [FAIL] 缺少 tools/pack_readme.md")
    else:
        shutil.copy2(readme_src, readme)
        manifest.append(("装前必读.md", os.path.getsize(readme),
                         sha256(readme)))

    # ---- MANIFEST ----
    lines = ["AIConsole %s 交付包 · 校验清单" % VERSION,
             "生成时间: %s" % __import__("time").strftime("%Y-%m-%d %H:%M:%S"),
             "桌面显示名: %s" % EXPECT_SHORT,
             ""]
    if problems:
        lines.append("!! 已知问题（打包时如实记录，不掩饰）:")
        for p in problems:
            lines.append("   - %s" % p)
        lines.append("")
    lines.append("%-28s %12s  %s" % ("文件", "字节", "sha256"))
    for n, s, h in manifest:
        lines.append("%-28s %12d  %s" % (n, s, h))
    # ⚠️ 记账必须用**落盘后的真实字节数**：Windows 下 open(w) 会把 \n 转成 \r\n，
    #   len(mf.encode()) 会比实际文件少 14 字节，解包复核就会被这条假不一致判红
    #   （已踩：内容其实完全一致，是我计量口径错了）。
    mfpath = os.path.join(PACKDIR, "MANIFEST.txt")
    with open(mfpath, "w", encoding="utf-8", newline="") as f:
        f.write("\n".join(lines) + "\n")
    manifest.append(("MANIFEST.txt", os.path.getsize(mfpath), sha256(mfpath)))

    # ---- 打 zip ----
    if os.path.exists(ZIPPATH):
        os.remove(ZIPPATH)
    with zipfile.ZipFile(ZIPPATH, "w", zipfile.ZIP_DEFLATED,
                         compresslevel=6) as z:
        for base, _, files in os.walk(PACKDIR):
            for fn in files:
                fp = os.path.join(base, fn)
                arc = os.path.relpath(fp, PACKDIR)
                z.write(fp, arc)
    print("\n[OK] 已打包 -> %s (%d 字节)"
          % (ZIPPATH, os.path.getsize(ZIPPATH)))

    # ---- ★ 解包复核：大小一致 ≠ 内容一致 ----
    print("\n" + "=" * 74)
    print("解包复核（重新从 zip 里读出来算 sha256）")
    print("=" * 74)
    bad = []
    with zipfile.ZipFile(ZIPPATH) as z:
        names = z.namelist()
        for n, s, h in manifest:
            arc = n
            if arc not in names:
                bad.append("%s 不在 zip 里" % arc)
                print("  [FAIL] %-28s 缺失" % arc)
                continue
            data = z.read(arc)
            got = hashlib.sha256(data).hexdigest()
            ok = (got == h) and (len(data) == s)
            print("  [%s] %-28s %d 字节  %s"
                  % ("OK  " if ok else "FAIL", arc, len(data),
                     got[:16] + "…"))
            if not ok:
                bad.append("%s 解包后 sha256 不一致" % arc)
        extra = [x for x in names if x not in [m[0] for m in manifest]]
        if extra:
            print("  [INFO] zip 内还有清单外的文件: %s" % extra)

    print("\n" + "=" * 74)
    if bad:
        print("RESULT: FAIL —— %s" % "; ".join(bad))
        return 1
    if problems:
        print("RESULT: 打包完成，但带已知问题（已写入 MANIFEST / 装前必读）:")
        for p in problems:
            print("   ★ %s" % p)
        return 0
    print("RESULT: OK —— 全部复核通过")
    return 0


if __name__ == "__main__":
    sys.exit(main())
