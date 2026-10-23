#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""
guide_probe.py — 离线攻略模块的 Azahar 实测（读 status.txt 里的 app_note）

app_note 写的 g_notes 落在 sdmc:/3ds/aiconsole/status.txt
（app.c 里 fopen("sdmc:/3ds/aiconsole/status.txt","wb")）。
sc_guide.c 埋了 7 个探针，所以这个页面一进去就会自报家门。

要验的事，和每条对应的证据：
  1. 词表能加载      "guide: vocab ok v=... merges=..."
  2. 知识库能绑定    "guide: bound n=836"     n > 0
  3. 屏幕能进入      "guide: screen=guide end"
  4. 检索真能命中    "guide: q=... hits=N top=M"   hits > 0

铁律（每条都付过通宵代价）：
  - 读完立刻快照。status.txt 会被后续趟覆盖，断言必须在读的瞬间做。
  - 读app 写的文件要等它不再变化：fopen("w") 一调用就截断且 mtime 变新，
    按 mtime 判断会读到空文件。这里轮询到"内容含 guide: 且长度稳定"。
"""
import os
import re
import sys
import glob
import time
import shutil
import subprocess

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
AZAHAR_DIR = r"E:\Gudyi\_tools\azahar"


def find_azahar():
    c = glob.glob(os.path.join(AZAHAR_DIR, "**", "azahar.exe"), recursive=True)
    return c[0] if c else None


def find_sdmc():
    a = find_azahar()
    if not a:
        return None
    d = os.path.dirname(a)
    while d and d.upper() not in ("C:\\", "D:\\", "E:\\"):
        cand = os.path.join(d, "user", "sdmc")
        if os.path.isdir(cand):
            return cand
        d = os.path.dirname(d)
    return None


def read_stable(path, want, timeout=60):
    """
    轮询到内容里含 want 且长度连续两次相同，才返回。
    这就是"等它不再变化"，不然会读到 fopen("w") 截断出来的空文件。
    """
    t0 = time.time()
    last, lastn = None, -1
    stable = 0
    while time.time() - t0 < timeout:
        try:
            s = open(path, encoding="utf-8", errors="replace").read()
        except OSError:
            s = None
        if s and want in s and len(s) == lastn:
            stable += 1
            if stable >= 2:
                return s
        else:
            stable = 0
        last, lastn = s, len(s or "")
        time.sleep(2)
    # 超时也返回最后一次读到的，好让调用方看到"到底写到了哪一步"
    return last


def main():
    az = find_azahar()
    sdmc = find_sdmc()
    print("Azahar: %s" % (az or "未找到"))
    print("sdmc  : %s" % (sdmc or "未找到"))
    if not az or not sdmc:
        return 1

    work = os.path.join(sdmc, "3ds", "aiconsole")
    os.makedirs(work, exist_ok=True)
    status = os.path.join(work, "status.txt")
    # 挪走旧状态，不 os.remove
    if os.path.exists(status):
        os.replace(status, status + ".prev")

    cia = os.path.join(ROOT, "app", "AIConsole.cia")
    print("CIA: %d 字节" % os.path.getsize(cia))
    print("启动 Azahar ...")

    env = dict(os.environ)
    p = subprocess.Popen([az, cia], env=env,
                         stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        # 先等 app 起来并写出第一版 status
        s = read_stable(status, "guide:", timeout=75)
    finally:
        time.sleep(2)
        p.terminate()
        try:
            p.wait(timeout=10)
        except Exception:
            p.kill()

    if not s:
        print("\n结果：status.txt 没写出含 guide: 的内容")
        if os.path.exists(status):
            print("--- 实际内容 ---")
            print(open(status, encoding="utf-8", errors="replace").read()[:1500])
        return 2

    # ---- 当场快照，后面只对着这份字符串做断言 ----
    snap = s
    print("\n=== status.txt 中的 guide 探针 ===")
    for line in snap.split("\n"):
        if "guide" in line.lower() or "error" in line.lower():
            print("  " + line.strip()[:150])

    checks = []
    m = re.search(r"guide: vocab ok v=(\d+) merges=(\d+)", snap)
    checks.append(("词表加载", bool(m),
                   ("v=%s merges=%s" % m.groups()) if m else "无 vocab ok"))
    m = re.search(r"guide: bound n=(\d+)", snap)
    n = int(m.group(1)) if m else 0
    checks.append(("知识库绑定", n > 0, "n=%d" % n))
    m = re.search(r"guide: screen=guide end", snap)
    checks.append(("进入攻略屏", bool(m), "有 screen=guide end" if m else "无"))
    hits = [int(x) for x in re.findall(r"guide: q=.*?hits=(\d+)", snap)]
    checks.append(("检索命中", bool(hits) and max(hits) > 0,
                   "hits=%s" % (hits or "无探针")))

    print("\n=== 判定 ===")
    ok = True
    for name, passed, detail in checks:
        print("  [%s] %-12s %s" % ("PASS" if passed else "FAIL", name, detail))
        ok = ok and passed
    print("\n总结: %s" % ("全部通过" if ok else "有未通过项"))
    return 0 if ok else 3


if __name__ == "__main__":
    sys.exit(main())