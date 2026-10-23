#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""OTA 更新包生成器。

把 app/ 下的 build 产物打包成一个可发布目录：

    dist/update/
    ├── manifest.json      ← 索引 + 版本 + SHA256，3DS 只看这个
    ├── notes.html         ← 更新说明，给内置查看器用
    ├── AIConsole.3dsx
    └── AIConsole.cia

用法
----
    python tools/make_update.py --code 3 --name 0.3.0 --notes "换了新 Artifact"

    # 从文件读更新说明
    python tools/make_update.py --code 3 --name 0.3.0 --notes-file CHANGELOG.md

    # 校验已生成的 manifest 与磁盘上的文件是否还对得上
    python tools/make_update.py --check

设计要点（与 app 端一一对应，别单独改一边）
------------------------------------------
1. **manifest 里不带 http(s) 地址。** 只有文件名。
   基址是 3DS 端代码里的编译期常量（app/source/ota/ota.c 的 kUpdateBase）。
   这样就算有人改了你 SD 卡上的 config.json，也改不掉更新来源 ——
   否则配置 + manifest 一起被污染，就等于给了中间人一个远程代码执行的口子。

2. **每个产物都带 size + sha256。** 3DS 下载完必须算完摘要再落盘，
   不匹配就删掉重写前的文件，绝不让半个文件进安装环节。

3. **version_code 单调递增。** 3DS 端拒绝 code <= 当前版本的包，
   防止降级攻击（故意推旧版本去绕掉后来修掉的洞）。

4. **install 字段区分两条路：**
     "replace" = .3dsx，可以直接覆盖 SD 卡上的同名文件
     "fbi"     = .cia，只能写到卡上让用户去 FBI 里装，程序自己装不了
   这不是偷懒：3DS 上 .cia 的安装走 AM 服务，自制程序拿不到那个权限。
"""

import argparse
import hashlib
import json
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
APP = os.path.join(ROOT, "app")
DIST = os.path.join(ROOT, "dist", "update")

MANIFEST_SCHEMA = 1

# artefact -> 3DS 端该怎么装它
ARTIFACTS = [
    ("AIConsole.3dsx", "3dsx", "replace"),
    ("AIConsole.cia", "cia", "fbi"),
]


def sha256_of(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def build_manifest(code, name, channel, notes, min_supported, base_url=""):
    artifacts = []
    missing = []

    for fname, kind, install in ARTIFACTS:
        p = os.path.join(APP, fname)
        if not os.path.isfile(p):
            missing.append(fname)
            continue
        artifacts.append({
            "kind": kind,
            "file": fname,
            "size": os.path.getsize(p),
            "sha256": sha256_of(p),
            "install": install,
        })

    m = {
        "schema": MANIFEST_SCHEMA,
        "version_code": code,
        "version_name": name,
        "channel": channel,
        "min_supported": min_supported,
        "notes": notes,
        "artifacts": artifacts,
    }
    if base_url:
        m["_note"] = ("仅生成时参考；3DS 端忽略此字段，"
                      "实际基址是代码里的常量: %s" % base_url)
    return m, missing


def write_notes_html(path, name, notes):
    """给内置查看器看的更新说明。

    刻意只用最朴素的标签：内置查看器不支持 CSS/JS，
    写复杂了在 3DS 上就是一堆乱码。 heading / p / ul 够用。
    """
    esc = (notes.replace("&", "&amp;").replace("<", "&lt;")
                .replace(">", "&gt;"))
    blocks = []
    for para in esc.split("\n\n"):
        para = para.strip()
        if not para:
            continue
        if para.startswith("- "):
            items = [x.strip()[2:] for x in para.split("\n")
                     if x.strip().startswith("- ")]
            blocks.append("<ul>" + "".join(
                "<li>%s</li>" % i for i in items) + "</ul>")
        else:
            blocks.append("<p>%s</p>" % para.replace("\n", "<br>"))

    html = ("<html><body><h1>%s</h1>%s</body></html>"
            % (name.replace("<", "&lt;"), "".join(blocks)))
    with open(path, "w", encoding="utf-8") as f:
        f.write(html)


def cmd_build(args):
    m, missing = build_manifest(args.code, args.name, args.channel,
                                args.notes or "", args.min_supported,
                                args.base)

    if not m["artifacts"]:
        print("!! app/ 下一个产物都没有。先跑 python tools/build.py cia")
        return 1

    os.makedirs(DIST, exist_ok=True)

    mp = os.path.join(DIST, "manifest.json")
    with open(mp, "w", encoding="utf-8") as f:
        json.dump(m, f, ensure_ascii=False, indent=2)
        f.write("\n")

    write_notes_html(os.path.join(DIST, "notes.html"), args.name,
                     m["notes"])

    # 产物 copy 过来（覆盖），这样 dist/update 是一个可以直接整目录上传的发布包
    for fname, _, _ in ARTIFACTS:
        src = os.path.join(APP, fname)
        if os.path.isfile(src):
            with open(src, "rb") as a, \
                    open(os.path.join(DIST, fname), "wb") as b:
                b.write(a.read())

    print("=" * 60)
    print("OTA 包已生成：%s" % DIST)
    print("=" * 60)
    print("version_code : %d" % m["version_code"])
    print("version_name : %s" % m["version_name"])
    print("channel      : %s" % m["channel"])
    for a in m["artifacts"]:
        print("  %-14s %8d bytes  %s  [%s]"
              % (a["file"], a["size"], a["sha256"][:16] + "...", a["install"]))
    if missing:
        print("\n缺少（未纳入本次更新）：%s" % ", ".join(missing))
        print("  提示：.cia 要跑 'build.py cia' 才有")
    return 0


def cmd_check(args):
    mp = os.path.join(DIST, "manifest.json")
    if not os.path.isfile(mp):
        print("!! 没有 manifest：%s" % mp)
        return 1

    with open(mp, encoding="utf-8") as f:
        m = json.load(f)

    bad = 0
    print("=" * 60)
    print("校验 %s" % DIST)
    print("=" * 60)

    for a in m["artifacts"]:
        p = os.path.join(DIST, a["file"])
        if not os.path.isfile(p):
            print("  [缺] %s" % a["file"])
            bad += 1
            continue
        size = os.path.getsize(p)
        digest = sha256_of(p)
        if size != a["size"]:
            print("  [大小不符] %s：manifest %d，磁盘 %d"
                  % (a["file"], a["size"], size))
            bad += 1
        elif digest != a["sha256"]:
            print("  [摘要不符] %s" % a["file"])
            bad += 1
        else:
            print("  [OK] %-14s %8d bytes" % (a["file"], size))

    if bad:
        print("\n%d 个有问题 —— 别发布，重新 make_update.py" % bad)
        return 1
    print("\n全部一致，可以发布。")
    print("发布方式：把 dist/update/ 整个目录传到 %s 下面即可。"
          % (args.base or "<你的更新基址>"))
    return 0


def main(argv=None):
    ap = argparse.ArgumentParser(description="生成 / 校验 OTA 更新包")
    ap.add_argument("--check", action="store_true",
                    help="校验已有的 manifest 与磁盘文件是否一致")
    ap.add_argument("--code", type=int, default=1, help="内部版本号，必须递增")
    ap.add_argument("--name", default="0.1.0", help="展示版本号")
    ap.add_argument("--channel", default="stable",
                    choices=["stable", "beta"])
    ap.add_argument("--notes", default="", help="更新说明")
    ap.add_argument("--notes-file", help="从文件读更新说明")
    ap.add_argument("--min-supported", type=int, default=1,
                    help="能直接升上来的最低旧版本号")
    ap.add_argument("--base", default="",
                    help="更新基址（只写进注释，不参与决定逻辑）")
    args = ap.parse_args(argv)

    if args.notes_file:
        with open(args.notes_file, encoding="utf-8") as f:
            args.notes = f.read().strip()

    if args.check:
        return cmd_check(args)
    return cmd_build(args)


if __name__ == "__main__":
    sys.exit(main())
