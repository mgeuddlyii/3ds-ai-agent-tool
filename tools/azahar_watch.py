#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
azahar_watch.py -- 给用户拉起可操作的 Azahar 窗口，同时当"黑匣子看守"。

    * 启动前：杀残留实例、清 boot.txt / autorun / status.txt（手动玩不该有脚本按键）
    * 运行中：0.5s 轮询盯进程
    * 退出时：立刻把 存活时长 + 退出代码（定性）+ boot.txt 快照 + stderr 尾部
      写进 logs/watch_result.txt —— 闪退那一刻的第一现场

用法：
    python tools/azahar_watch.py                 # 一直看守到 Azahar 死
    python tools/azahar_watch.py --watch 120     # 只看守 120s，之后报"还活着"并退出
                                                 # （窗口留给用户，不会被杀）
"""

import argparse
import datetime
import os
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)
import azahar_test as at          # noqa: E402  复用 SD 根发现 / 清场逻辑

LOGS = os.path.join(ROOT, "logs")

DEFAULT_AZAHAR = r"E:\Azahar\azahar-windows-msvc-2126.1.1\azahar.exe"
DEFAULT_ROM    = os.path.join(ROOT, "app", "AIConsole.3dsx")

#: Windows 退出代码 -> 人话。python poll() 返回的是有符号数。
RC_NAMES = {
    0:            "0 = 干净退出（有人调了 exit）",
    3:            "3 = abort()（CRT 主动放弃，通常是断言/堆损坏）",
    -1073741819:  "0xC0000005 = 访问违例（野指针/空指针）",
    -1073740727:  "0xC0000409 = fastfail（栈cookie/abort 快速失败）",
    -1073741571:  "0xC00000FD = 栈溢出",
    -1073741510:  "0xC000013A = 被 Ctrl+C / 关闭信号打断",
    -1073741511:  "0xC0000139 = DLL 入口点未找到",
    -1073741683:  "0xC0000094 = 整数除零",
}


def rc_name(rc):
    if rc in RC_NAMES:
        return RC_NAMES[rc]
    return "0x%08X (%d) = 未知" % (rc & 0xFFFFFFFF, rc)


def read_text(path, limit=4000):
    try:
        with open(path, "r", encoding="utf-8", errors="replace") as fh:
            return fh.read(limit)
    except OSError:
        return ""


def tail_text(path, lines=40):
    try:
        with open(path, "r", encoding="utf-8", errors="replace") as fh:
            return "".join(fh.readlines()[-lines:])
    except OSError:
        return ""


def clear_manual_artifacts(roots):
    """手动游玩模式：boot/autorun/status 都是上一轮的旧证据，清掉。
    autorun 尤其必须删 —— 留着的话 app 一启动就自己按键，用户没法玩。"""
    for r in roots:
        for rel in ("3ds/aiconsole/boot.txt",
                    "3ds/aiconsole/autorun",
                    "3ds/aiconsole/status.txt",
                    "3ds/aiconsole/shots/error.txt"):
            try:
                os.remove(os.path.join(r, *rel.split("/")))
            except OSError:
                pass


def main():
    try:
        sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    except Exception:                                        # noqa: BLE001
        pass

    ap = argparse.ArgumentParser()
    ap.add_argument("--azahar", default=DEFAULT_AZAHAR)
    ap.add_argument("--rom", default=DEFAULT_ROM)
    ap.add_argument("--watch", type=float, default=0.0,
                    help=">0 时只看守 N 秒，然后报'还活着'退出（不杀窗口）")
    args = ap.parse_args()

    if not os.path.isfile(args.azahar):
        print("找不到 Azahar: %s" % args.azahar)
        return 1
    if not os.path.isfile(args.rom):
        print("找不到 ROM: %s" % args.rom)
        return 1

    exe_dir = os.path.dirname(args.azahar)
    roots = at.writable_roots(at.sdmc_roots(exe_dir))
    if not roots:
        print("没有可写的 SD 根，放弃")
        return 1

    os.makedirs(LOGS, exist_ok=True)
    result_path = os.path.join(LOGS, "watch_result.txt")
    stderr_path = os.path.join(LOGS, "azahar_stderr_watch.txt")

    print("清场（杀残留 azahar）...")
    at.kill_stale_azahar()
    clear_manual_artifacts(roots)

    t0 = time.time()
    stamp0 = datetime.datetime.now().strftime("%H:%M:%S")
    print("启动 Azahar（黑匣子看守中）: %s" % args.rom)

    with open(stderr_path, "w", encoding="utf-8", errors="replace") as ef:
        proc = subprocess.Popen(
            [args.azahar, args.rom], cwd=exe_dir,
            stdout=subprocess.DEVNULL, stderr=ef,
            creationflags=subprocess.DETACHED_PROCESS
                          | subprocess.CREATE_NEW_PROCESS_GROUP)

    while True:
        rc = proc.poll()
        if rc is not None:
            break
        if args.watch > 0 and time.time() - t0 > args.watch:
            alive = ("[WATCH] Azahar 还活着（已 %d 秒）—— 看守退出，窗口留给你玩。\n"
                     "复现步骤：点一下画面拿焦点 → 按住要按的键约 2 秒（模拟器内只有 ~1fps，"
                     "短按会被吃掉）。\n死亡后重跑本脚本（不带 --watch）即可抓退出代码。"
                     % int(time.time() - t0))
            print(alive)
            with open(result_path, "w", encoding="utf-8") as fh:
                fh.write("status=alive\nstarted=%s\nalive_seconds=%d\nrc=none\n"
                         % (stamp0, int(time.time() - t0)))
            return 0
        time.sleep(0.5)

    elapsed = time.time() - t0
    boot = ""
    for r in roots:
        p = os.path.join(r, "3ds", "aiconsole", "boot.txt")
        if os.path.isfile(p):
            boot = read_text(p)
            break

    report = []
    report.append("status=died")
    report.append("started=%s  died=%s  elapsed=%.1fs"
                  % (stamp0, datetime.datetime.now().strftime("%H:%M:%S"), elapsed))
    report.append("rc=%s" % rc_name(rc))
    report.append("")
    report.append("---- boot.txt 最后现场 ----")
    report.append(boot or "(boot.txt 不存在 —— 死在 app 起跑前)")
    report.append("")
    report.append("---- stderr 尾部 ----")
    report.append(tail_text(stderr_path, 40).strip() or "(空)")
    report.append("")
    report.append("下一步：日志 %s"
                  % os.path.join(exe_dir, "user", "log", "azahar_log.txt"))
    text = "\n".join(report)

    with open(result_path, "w", encoding="utf-8") as fh:
        fh.write(text + "\n")

    print(text)
    print("\n[WATCH] 已写入 %s" % result_path)
    return 0


if __name__ == "__main__":
    sys.exit(main())
