#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""
retrain.py — 重训启动器（后台跑，零积分消耗）

══ 为什么用这个而不是直接跑 train_gpt.py ════════════════════════════

**积分消耗全在"是否联网"上**：
  · 训练本身是**纯 numpy**（train_gpt.py 不 import torch）→ 本地 CPU，0 积分
  · 之前那两项算力优化也是纯本地 → 0 积分
  · 只有**云端 API 调用**（豆包/智谱/…）才消耗积分
所以重训**天然不花积分**，本脚本要做的是**保证它一条网络请求都不发**：
  · 不 import 任何 http/requests/urllib
  · 不调用 pc_chat_test.py 那条路
  · 训练日志实时写盘（断电/中断也不丢）

⚠️ 顺便把「积分」这件事说清楚，免得误会：
  这个项目里"云端对话"（shared/chatapi.c）才消耗用户充值的 API 积分；
  "离线攻略 AI"（shared/nnet/）**完全不联网**，两者互不相干。
  用户看到"训练"默认会以为在烧 API —— 不是。

══ 用法 ══════════════════════════════════════════════════════════

    python tools/guia/retrain.py                # 前台（会一直占用终端）
    python tools/guia/retrain.py --bg           # 后台（推荐，关掉终端也能跑）
    python tools/guia/retrain.py --status       # 看进度
    python tools/guia/retrain.py --stop         # 停（可 --resume 续）

默认配置（2026-10-04 实测定，不是猜的）：
    steps=4000  batch=32
    依据：上次 950 步用了 2867s（约 3s/步）→ 4000 步约 3.3 小时。
    且上次是**被中断**的（950/4000），lr 一直停在峰值的 94.6%，
    从没退过火 —— 所以"步数够不够"这件事这次要一次给对。
"""
import os
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
PY = r"C:\Users\Kevin\.workbuddy\binaries\python\envs\default\Scripts\python.exe"
TRAIN = os.path.join(HERE, "train_gpt.py")
CKPT = os.path.join(HERE, "ckpt", "arch288")
LOG = os.path.join(CKPT, "retrain.log")
PIDF = os.path.join(CKPT, "retrain.pid")
LOCK = os.path.join(CKPT, "retrain.lock")

STEPS = 4000
BATCH = 32


def read_pid():
    try:
        with open(PIDF) as f:
            return int(f.read().strip())
    except Exception:
        return None


def alive(pid):
    """进程还在吗。

    ⚠️ `tasklist` 在中文 Windows 上输出是**本地代码页**（GBK/936），
    不是 UTF-8。capture_output 拿到的是 bytes，text=True 会按
    UTF-8 解码 → UnicodeDecodeError，而 out.stdout 直接变 None。
    症状是「--status 崩了」，而训练其实好好地在跑 ——
    看起来像训练挂了，其实是查状态的脚本报编码错。
    → 所以这里手动按 gbksim 试，errors 兜底。
    """
    if not pid:
        return False
    r = subprocess.run(["tasklist", "/FI", "PID eq %d" % pid, "/NH"],
                       capture_output=True)
    for enc in ("gbk", "mbcs", "utf-8"):
        try:
            s = r.stdout.decode(enc, errors="replace")
            break
        except Exception:
            continue
    else:
        s = r.stdout.decode("utf-8", errors="replace")
    return str(pid) in s


def tail(n=14):
    try:
        with open(LOG, encoding="utf-8", errors="replace") as f:
            return f.read().splitlines()[-n:]
    except Exception:
        return ["（还没有日志）"]


def cmd_bg():
    if alive(read_pid()):
        print("已经在跑了（pid %d）。要先停：--stop" % read_pid())
        return 1
    os.makedirs(CKPT, exist_ok=True)
    # 关键：**不继承任何代理环境变量**。训练不需要网络，
    # 留着 proxy 只会让"万一发了请求"变得不可见。
    env = dict(os.environ)
    for k in ("http_proxy", "https_proxy", "HTTP_PROXY", "HTTPS_PROXY",
              "ALL_PROXY", "all_proxy"):
        env.pop(k, None)
    env["PYTHONUNBUFFERED"] = "1"        # 日志实时写盘
    cmd = [PY, TRAIN, str(STEPS), str(BATCH)]
    with open(LOG, "w", encoding="utf-8") as lf:
        p = subprocess.Popen(cmd, stdout=lf, stderr=subprocess.STDOUT,
                             cwd=ROOT, env=env)
    with open(PIDF, "w") as f:
        f.write(str(p.pid))
    print("已在后台启动")
    print("  pid  %d" % p.pid)
    print("  日志  %s" % LOG)
    print("  步数  %d  batch %d  预计约 %.1f 小时"
          % (STEPS, BATCH, STEPS * 3.0 / 3600.0))
    print()
    print("看进度：python tools/guia/retrain.py --status")
    return 0


def cmd_status():
    pid = read_pid()
    if not alive(pid):
        print("没在跑（pid %s）" % pid)
    else:
        print("在跑，pid %d" % pid)
    for l in tail():
        print("  " + l)
    return 0


def cmd_stop():
    pid = read_pid()
    if alive(pid):
        subprocess.run(["taskkill", "/F", "/PID", str(pid)],
                       capture_output=True)
        print("已停（pid %d）。续训：--start --resume" % pid)
    else:
        print("本来就没在跑")
    return 0


def cmd_start_again(resume):
    if alive(read_pid()):
        print("已经在跑了")
        return 1
    os.makedirs(CKPT, exist_ok=True)
    env = dict(os.environ)
    for k in ("http_proxy", "https_proxy", "HTTP_PROXY", "HTTPS_PROXY"):
        env.pop(k, None)
    env["PYTHONUNBUFFERED"] = "1"
    cmd = [PY, TRAIN, str(STEPS), str(BATCH)] + (["--resume"] if resume else [])
    with open(LOG, "a", encoding="utf-8") as lf:
        lf.write("\n===== 续训 %s =====\n" % time.strftime("%Y-%m-%d %H:%M:%S"))
        p = subprocess.Popen(cmd, stdout=lf, stderr=subprocess.STDOUT,
                             cwd=ROOT, env=env)
    with open(PIDF, "w") as f:
        f.write(str(p.pid))
    print("已续训，pid %d" % p.pid)
    return 0


def main():
    a = sys.argv[1:]
    if "--status" in a:
        return cmd_status()
    if "--stop" in a:
        return cmd_stop()
    if "--bg" in a:
        return cmd_bg()
    if "--start" in a:
        return cmd_start_again("--resume" in a)
    # 前台
    env = dict(os.environ)
    for k in ("http_proxy", "https_proxy", "HTTP_PROXY", "HTTPS_PROXY"):
        env.pop(k, None)
    return subprocess.call([PY, TRAIN, str(STEPS), str(BATCH)],
                           cwd=ROOT, env=env)


if __name__ == "__main__":
    sys.exit(main())
