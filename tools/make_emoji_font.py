# -*- coding: utf-8 -*-
"""把 NotoEmoji[wght].ttf 实例化成静态字重并子集化，产出 mkfont.py 用的
小体积 emoji 字体（tools/fonts/NotoEmoji-subset.ttf）。

为什么不在 mkfont.py 里做：mkfont 只依赖 Pillow，不该被 fontTools 绑死。
这里做一次性离线处理，产物进仓库，构建可复现。

用法：
    python tools/make_emoji_font.py <源ttf> [--wght 400] [--out tools/fonts/NotoEmoji-subset.ttf]
"""
import argparse
import os
import sys

from fontTools.ttLib import TTFont
from fontTools.varLib.instancer import instantiateVariableFont
from fontTools.subset import Subsetter, Options

# ★ 区段表**从 mkfont.py 导入**，不在这里复制一份。
# 铁律 6b：复刻脚本与被测对象同源 —— 复制一份就会出现"子集收了 A、
# mkfont 想要 B"，而且两边不一致时**不报错**，只是静默少一批 emoji。
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from mkfont import EMOJI_RANGES, ZERO_WIDTH_CPS  # noqa: E402


def wanted():
    s = set()
    for lo, hi in EMOJI_RANGES:
        s.update(range(lo, hi + 1))
    s.update(ZERO_WIDTH_CPS)
    return sorted(s)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("src")
    ap.add_argument("--wght", type=float, default=400.0)
    ap.add_argument("--out", default=os.path.join(
        os.path.dirname(os.path.abspath(__file__)), "fonts",
        "NotoEmoji-subset.ttf"))
    args = ap.parse_args()

    cps = wanted()
    print("目标码位 %d 个" % len(cps))

    f = TTFont(args.src)
    if "fvar" in f:
        loc = {"wght": args.wght}
        print("实例化 wght=%s ..." % args.wght)
        instantiateVariableFont(f, loc, inplace=True)

    opt = Options()
    opt.layout_features = []
    opt.name_IDs = ["*"]
    opt.notdef_outline = True
    opt.recalc_bounds = True
    opt.drop_tables += ["GSUB", "GPOS", "GDEF", "BASE", "JSTF", "DSIG"]
    sub = Subsetter(options=opt)
    sub.populate(unicodes=cps)
    sub.subset(f)

    # 只保留真有的字形，缺的列出来（mkfont 侧还会再过滤一次）
    have = set()
    for t in f["cmap"].tables:
        have.update(t.cmap.keys())
    missing = [cp for cp in cps if cp not in have]
    if missing:
        print("字体本身缺 %d 个码位（子集里自然也没有）：%s"
              % (len(missing), " ".join("U+%04X" % c for c in missing[:40])))

    out = os.path.abspath(args.out)
    os.makedirs(os.path.dirname(out), exist_ok=True)
    f.save(out)
    print("产物 %s  %.1f KB" % (out, os.path.getsize(out) / 1024.0))


if __name__ == "__main__":
    main()
