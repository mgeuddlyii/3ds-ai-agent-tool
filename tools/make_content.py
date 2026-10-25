#!/usr/bin/env python3
"""
打包内容更新包。

用法：
    python tools/make_content.py --kb tools/guia/ckpt/guide_kb.bin
    python tools/make_content.py --kb ... --model app/data/guide_model.bin
    python tools/make_content.py --all

产物落在 build/content/，把**整个目录**拷到 SD 卡的
    3ds/aiconsole/content/
下面就行。3DS 那边走 sc_content 的「检查更新」。

══ 为什么内容更新要和程序更新分成两个包 ═══════════════════════════════

ota.c 的包装的是 3dsx/cia，cupd.c 的包装的是模型/攻略库/知识库。
两者刻意不合并，理由不是"整洁"，是**失败后果不同**：

  程序更新装坏了 = 软件打不开，只能重装
  内容更新装坏了 = 少一层生成能力，检索还在（guide_nn 的失败策略）

更重要的是内容更新**有回滚**（.bak），程序更新没有 ——
因为攻略库和模型是手工/训练产出的，卡上那份可能就是唯一一份，
而下坏了的程序总能重下一遍。

⚠️ 版本号各自独立（G3）：
    模型月级、知识库周级、硬件知识年级级更新。
    共用一个 version_code 会导致「更新了知识库、界面却说模型也是新的」。

══ 三类内容各自的"更新真正生效"条件 ══════════════════════════════════

这一节是 2026-10-04 补的，因为**打包成功不等于更新生效**。
当时三类内容里只有一类真的有读者：

  model    从 SD 读（nnet_load_file）        -> 装上就生效
  kb       曾是**编译期内嵌**（gkb_data[]）  -> 装上**不生效**（已修：gkb_bind
                                               现在先找 SD，找不到才回退内嵌）
  hardware 曾**没有任何代码读它**            -> 装上**不生效**（已修：新增
                                               shared/hwk.c，SD 优先 + 内嵌兜底）

→ 所以「往卡上装一份」这件事本身是必要的但不充分：
**每类内容都得有一个读者去读它装到的那份**。
新增一种可更新内容时，第一件事不是写打包规则，是写它的读者。

⚠️ hardware 的源头是 app/data/hw_knowledge.txt，**同一个文件**
既被 bin2s 内嵌进 .cia、又被打进更新包。这是刻意的：
内嵌那份保证「SD 拔了也能用」，包里的那份保证「能改内容不用重编固件」。
代价是改这份文件之后**要重编一次 .cia**，内嵌的那份才会跟着变。
"""
import argparse
import hashlib
import json
import os
import shutil
import sys

OUT_DIR = os.path.join("build", "content")

# 与 shared/cupd.c 的 CU_TARGET / 解析短名一一对应。
# 顺序即 manifest 里的数组下标，改这里必须同步改 cupd.h 的枚举。
KINDS = [
    # (kind 短名, 包内文件名, 正式落盘文件名, 版本文件)
    ("model",    "guide_model.bin",  "guide_model.bin"),
    ("kb",       "guide_kb.bin",     "guide_kb.bin"),
    ("hardware", "hw_knowledge.txt", "hw_knowledge.txt"),
]


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        while True:
            b = f.read(1 << 16)
            if not b:
                break
            h.update(b)
    return h.hexdigest()


def find_default(kind):
    """按 kind 猜一个合理的源文件位置。"""
    cands = {
        "model": [os.path.join("app", "data", "guide_model.bin"),
                  os.path.join("tools", "guia", "ckpt", "guide_model.bin")],
        "kb":    [os.path.join("tools", "guia", "ckpt", "guide_kb.bin"),
                  os.path.join("tools", "guia", "ckpt", "guide_kb_zlib.bin"),
                  os.path.join("app", "data", "guide_kb.bin")],
        "hardware": [os.path.join("app", "data", "hw_knowledge.txt")],
    }[kind]
    for c in cands:
        if os.path.exists(c):
            return c
    return None


def read_version(verfile, kind):
    """读 3DS 上记录的版本号。没有就是 0（= 全新安装）。"""
    if not os.path.exists(verfile):
        return 0
    try:
        with open(verfile) as f:
            v = int(f.read().strip())
        return v if v > 0 else 0
    except (ValueError, OSError):
        # 版本文件坏了就当 0 —— 不是"当成最新"。
        # 当成最新会让这个内容**永远收不到更新**，而用户看不出为什么。
        return 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--all", action="store_true", help="三类内容全打")
    ap.add_argument("--kb", help="攻略库文件")
    ap.add_argument("--model", help="模型文件")
    ap.add_argument("--hw", help="硬件知识库文件")
    ap.add_argument("--out", default=OUT_DIR)
    ap.add_argument("--sd", action="store_true",
                    help="同时拷到 Azahar 的 sdmc（找得到的话）")
    ap.add_argument("--bump", type=int, default=1,
                    help="相对当前版本升几级（默认 1）")
    args = ap.parse_args()

    given = {"kb": args.kb, "model": args.model, "hardware": args.hw}
    if args.all:
        given = {k: (given[k] or find_default(k)) for k in given}

    if not any(given.values()):
        sys.exit("没指定任何内容。用 --all 或 --kb/--model/--hw 指定。")

    os.makedirs(args.out, exist_ok=True)

    arts = []
    for kind, packname, realname in KINDS:
        src = given.get(kind)
        if not src:
            print("  跳过 %-9s（没给文件）" % kind)
            continue
        if not os.path.exists(src):
            sys.exit("找不到 %s：%s" % (kind, src))

        # 版本号相对 SD 卡上记录的那个升。
        # 注意这里读的是**模拟器/真机上的记录**，不是 git 版本 ——
        # 版本号的唯一用途是「3DS 那边该不该装」，判据必须在设备侧。
        verfile = os.path.join(args.out, "ver_%s.txt" % kind)
        cur = read_version(verfile, kind)
        newv = cur + args.bump

        dst = os.path.join(args.out, packname)
        shutil.copyfile(src, dst)
        size = os.path.getsize(dst)
        digest = sha256(dst)

        # 顺手把版本号写进包里，方便下次打包时读
        with open(verfile, "w") as f:
            f.write("%d\n" % newv)

        arts.append({
            "kn": kind,
            "file": packname,
            "size": size,
            "sha256": digest,
            "version": newv,
        })
        print("  %-9s %8d B  v%d → v%d  %s"
              % (kind, size, cur, newv, digest[:16]))

    if not arts:
        sys.exit("一条内容都没打出来。")

    mf = {"schema": 1, "artifacts": arts}
    # ★ 2026-10-25：统一下载目录后不能和软件的 manifest.json 撞名 → content.json
    mpath = os.path.join(args.out, "content.json")
    with open(mpath, "w", encoding="utf-8") as f:
        json.dump(mf, f, indent=2, ensure_ascii=False)

    print("\nmanifest -> %s（%d 条）" % (mpath, len(arts)))

    if args.sd:
        for root in find_sdmc_candidates():
            dst = os.path.join(root, "3ds", "aiconsole", "content")
            os.makedirs(dst, exist_ok=True)
            for a in arts:
                shutil.copyfile(os.path.join(args.out, a["file"]),
                                os.path.join(dst, a["file"]))
            shutil.copyfile(mpath, os.path.join(dst, "content.json"))
            print("已拷到 %s" % dst)
            return
        print("没找到 sdmc，跳过拷贝（手动拷 build/content 即可）")


def find_sdmc_candidates():
    """Azahar 的 sdmc 位置历史上换过，不写死一个路径。"""
    out = []
    for base in (r"E:\Azahar", r"E:\Gudyi\_tools\azahar",
                 os.path.expanduser("~")):
        if not os.path.isdir(base):
            continue
        for dirpath, dirnames, _ in os.walk(base):
            if dirpath.endswith(os.path.join("user", "sdmc")):
                out.append(dirpath)
                dirnames[:] = []      # 不往下走了
        if out:
            break
    return out


if __name__ == "__main__":
    main()
