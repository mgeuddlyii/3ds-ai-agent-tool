#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
make_manifest.py —— 生成新版本推送用的 manifest.json

── 它解决的问题 ──
app 侧（`shared/ota.c`）只认一个东西：SD 卡上
`3ds/aiconsole/update/manifest.json` + 同目录的产物文件。
而**发布侧**过去是手写的 —— 手写 sha256 一定会错，错了的表现是
"包明明在卡上，app 却说摘要不符"，排查方向完全是反的。

这个脚本从**真实产物**算 size 和 sha256，所以不可能写错。

── 用法 ──
    # 用构建出来的产物 + shared/ota.h 里的版本号
    python tools/make_manifest.py

    # 指定产物和输出（发布目录）
    python tools/make_manifest.py --out dist/manifest.json \
        --art dist/AIConsole-1.0.3dsx --art dist/AIConsole-1.0.cia

    # 换通道 / 自定义说明
    python tools/make_manifest.py --channel beta --notes-file NOTES.md

    # 只预览不写
    python tools/make_manifest.py --dry-run

★ 它**不**做的事（刻意）：
  · 不签名。自制软件没有代码签名体系，安全边界只有"https + sha256"。
    别在这份文件里承诺"已签名"。
  · 不上传。上传是发布者的事（对象存储 / GitHub Release / 自己的服务器），
    脚本只保证"这份 manifest 与这些产物**逐字节对应**"。
  · 不猜 min_supported。它决定"多老的版本必须先升中间版"，
    是产品决定，必须人给 —— 给不出就默认当前版本（等于不限制）。
"""

import argparse
import hashlib
import io
import json
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OTA_H = os.path.join(ROOT, "shared", "ota.h")


def read_version():
    """从 shared/ota.h 读版本号。

    ★ 唯一真源在这里，不是手抄 —— 手抄的版本号迟早和程序对不上，
    而"app 认为自己是 v99、manifest 说 v99"这种错**不会报任何错**，
    只会让用户永远收不到更新（或者永远被判定为最新）。
    """
    src = io.open(OTA_H, encoding="utf-8").read()
    code = re.search(r"#define\s+APP_VERSION_CODE\s+(\d+)", src)
    name = re.search(r'#define\s+APP_VERSION_NAME\s+"([^"]*)"', src)
    if not code:
        raise SystemExit("!! 在 %s 里找不到 APP_VERSION_CODE" % OTA_H)
    return int(code.group(1)), (name.group(1) if name else "0.0")


def sha256_of(path):
    h = hashlib.sha256()
    with io.open(path, "rb") as fh:
        for chunk in iter(lambda: fh.read(65536), b""):
            h.update(chunk)
    return h.hexdigest()


def kind_of(path):
    ext = os.path.splitext(path)[1].lower()
    if ext == ".3dsx":
        return "3dsx"
    if ext == ".cia":
        return "cia"
    return None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--art", action="append", default=None,
                    help="产物文件（可重复）。默认取 app/ 下构建好的两个")
    ap.add_argument("--out", default=os.path.join(ROOT, "dist", "manifest.json"))
    ap.add_argument("--channel", default="stable", help="stable / beta")
    ap.add_argument("--notes", default="", help="一句话说明")
    ap.add_argument("--notes-file", default="", help="从文件读说明（优先于 --notes）")
    ap.add_argument("--min-supported", type=int, default=0,
                    help="低于这个 version_code 的旧版必须先升中间版；0 = 当前版本")
    ap.add_argument("--version-code", type=int, default=0,
                    help="覆盖版本号（默认读 shared/ota.h）；仅用于预演")
    ap.add_argument("--dry-run", action="store_true")
    args = ap.parse_args()

    cur_code, cur_name = read_version()
    code = args.version_code or cur_code

    arts = args.art or [
        os.path.join(ROOT, "app", "AIConsole.3dsx"),
        os.path.join(ROOT, "app", "AIConsole.cia"),
    ]

    items = []
    for p in arts:
        if not os.path.isfile(p):
            print("   （跳过，不存在）%s" % p)
            continue
        k = kind_of(p)
        if not k:
            print("!! 不认识的后缀，跳过：%s" % p)
            continue
        size = os.path.getsize(p)
        digest = sha256_of(p)
        items.append({
            "kind": k,
            "file": os.path.basename(p),
            # install: replace = 直接覆盖当前程序（仅 3dsx 可行）；
            #          copy    = 只放进 cias/，由用户去 FBI 里手动装
            "install": "replace" if k == "3dsx" else "copy",
            "size": size,
            "sha256": digest,
        })
        print("   %-10s %8d B  %s…  %s" % (k, size, digest[:16],
                                          os.path.basename(p)))

    if not items:
        print("!! 一个产物都没有，拒绝生成 manifest")
        return 1

    notes = args.notes
    if args.notes_file and os.path.isfile(args.notes_file):
        notes = io.open(args.notes_file, encoding="utf-8").read().strip()
    notes = notes[:190]          # OtaManifest.notes 是 192 字节

    # min_supported 的语义：**低于它的版本必须先升到它**（防跨版本破坏）。
    # 默认取当前版本 = 不限制（谁都能直接从任何旧版升上来）。
    min_sup = args.min_supported or cur_code

    man = {
        "schema": 1,
        "version_code": code,
        "version_name": cur_name,
        "channel": args.channel,
        "min_supported": min_sup,
        "notes": notes,
        "artifacts": items,
    }

    text = json.dumps(man, ensure_ascii=False, indent=2)
    print("\n--- manifest ---")
    print(text)

    # 自检：字段必须与 app 侧的解析器对得上。少一个 app 就整包拒收，
    # 而拒收的理由只会显示成一句"manifest 读不出来"—— 那是最难查的一类错。
    need = {"schema", "version_code", "version_name", "channel",
            "min_supported", "notes", "artifacts"}
    miss = need - set(man)
    if miss:
        print("!! 缺字段：%s" % ", ".join(sorted(miss)))
        return 1
    if len(items) > 4:                                    # OTA_MAX_ARTIFACTS
        print("!! 产物超过 4 个，app 只读前 4 个（OTA_MAX_ARTIFACTS）")
        return 1
    # ★ 只在**显式覆盖**版本号时才检查大小关系。
    #   正常发布时 manifest 的版本号就该等于 shared/ota.h 里的版本号
    #   （新版本的程序自然是新版本号），拿它跟"当前源码版本"比大小
    #   等于自己跟自己比，永远相等 —— 早先那条判断会把正常发布拦下来。
    #   真正要防的是：用 --version-code 手填一个更小的值去做"假更新"，
    #   那会被 app 的降级保护拒绝（这是**对的**，但也说明包白打了）。
    if args.version_code and args.version_code <= cur_code:
        print("!! --version-code(%d) 不大于 shared/ota.h 里的 %d："
              "这个包发出去会被判为「已是最新」或「降级包」"
              % (args.version_code, cur_code))
        return 1

    if args.dry_run:
        print("\n（--dry-run，没有写文件）")
        return 0

    os.makedirs(os.path.dirname(args.out), exist_ok=True)
    io.open(args.out, "w", encoding="utf-8").write(text + "\n")
    print("\n已写入 %s" % args.out)
    print("发布时把 manifest.json 和上面这些产物**放在同一个目录**，"
          "整体丢进 SD:/3ds/aiconsole/update/ 即可被识别。")
    return 0


if __name__ == "__main__":
    sys.exit(main())