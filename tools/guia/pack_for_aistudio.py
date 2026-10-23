#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""
pack_for_aistudio.py — 把训练所需的东西打成一个自包含的目录 + zip

══ 为什么要有这个 ═══════════════════════════════════════════════════
要把训练搬到百度 AI Studio，需要传的是**一棵目录树**，不是几个散文件：

    <root>/train_gpt.py        训练脚本（HERE = 脚本所在目录）
    <root>/train_bpe.py        BPE 实现（build_dataset 里 import 它）
    <root>/ckpt/bpe.pkl        词表（load_bpe 读这里）
    <root>/corpus/guide_*.txt  语料（build_dataset 读这里）

路径全是**相对脚本位置**算的（`CKPT = HERE/ckpt`），所以目录名可以随便起，
但这三层结构不能少、不能改。少一层就是「ModuleNotFoundError: bpe.pkl」
或者「语料 0 篇」——后者更阴：build_dataset 不报错，只是训练集是空的。

⚠️ 语料是**快照**。另一个会话还在扩语料（实测 20 分钟内 116 → 211 款），
所以每次打包都要重跑本脚本，别用上一份 zip。

用法：
    python pack_for_aistudio.py                 # 打包 + 打 zip
    python pack_for_aistudio.py --out D:\\tmp   # 指定输出目录
"""
from __future__ import print_function

import glob
import os
import shutil
import sys
import time
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
DEFAULT_OUT = os.path.join(HERE, "tmp")

# 必须带的脚本（少一个就 train 不起来）
SCRIPTS = ("train_gpt.py", "train_bpe.py")
# 词表：build_dataset -> load_bpe 读 ckpt/bpe.pkl
VOCAB = os.path.join("ckpt", "bpe.pkl")

README = """# 3DS 离线攻略微模型 · 百度 AI Studio 训练包

生成时间：{stamp}
语料：{n_games} 款 / {n_chars:,} 汉字

## 怎么跑（AI Studio Notebook，Python 3 环境，CPU 就够）

**第 1 步**：把本目录整个解压到工作区。解压后必须是这样（缺一层就报错）：

```
aistudio_train/
├── train_gpt.py
├── train_bpe.py
├── ckpt/
│   └── bpe.pkl
└── corpus/
    └── guide_*.txt        （{n_games} 个）
```

**第 2 步**：新建 Notebook（**Python 基础镜像即可，不需要 GPU**），
在单元格里**逐行**执行（先别急着 cd —— 不同镜像的工作目录不一样，
先看清楚自己在哪）：

```bash
pwd && ls                      # 先看清当前目录和已有什么

unzip -q aistudio_train.zip -d .     # 如果提示找不到 zip，把 aistudio_train.zip 换成实际路径
cd aistudio_train

ls -l train_gpt.py train_bpe.py ckpt/bpe.pkl   # 三个文件都在才继续
ls corpus/guide_*.txt | wc -l                   # 应该是 {n_games}
python -c "import numpy; print(numpy.__version__)"   # 需要 numpy

python train_gpt.py 4000 32          # 步数 batch
```

**第 3 步**：等它跑完（约 1.3 小时，CPU 版；线上 CPU 通常比本机快）。
训练过程中的进度会**实时刷**出来（脚本是 flush=True 写的，别用 `| tail`
把输出全攒在管道里 —— 上次就是因为这样 22 分钟看不到一个字，误判成卡死）。

**第 4 步**：结果在 `ckpt/` 下，按重要性取：

| 文件 | 作用 | 要不要传回本机 |
|---|---|---|
| `model_best.npz` | 留出集最优的模型参数 | ✅ **必须** |
| `train.log` | 完整训练曲线 | ✅ 建议（要分析收敛） |
| `model_fp32.npz` | 最终（不一定最优）的参数 | ⭕ 可选 |
| `state.npz` | 断点续训用的动量状态 | ❌ 14MB，不值当 |

传回本机后放回 `3ds-ai/tools/guia/ckpt/`，然后：

```bash
python tools/guia/export_model.py        # fp32 -> int8，给 3DS 用
```

## 常见问题

**Q: `ModuleNotFoundError: No module named 'train_bpe'`**
目录结构错了 —— `train_bpe.py` 必须和 `train_gpt.py` **同一层**。

**Q: 日志里「序列 0 条」**
`corpus/` 没解压对，或解压到了 `aistudio_train/aistudio_train/corpus/`。
检查 `ls corpus/guide_*.txt | wc -l` 是不是 {n_games}。

**Q: 「训练步数」和 cosine 退火对不上**
步数是**本次要跑的总步数**，别虚报（脚本注释里写了原因：lr 走不到底 = 没退火）。

**Q: 想在 GPU 上跑**
这个包是**纯 numpy**，跑不了 GPU。要用 V100 得先迁 PyTorch ——
这个包的作用是先把「上传→跑→下载」这条链路走通，验证平台可用。
"""


def main():
    out_dir = DEFAULT_OUT
    if "--out" in sys.argv:
        out_dir = sys.argv[sys.argv.index("--out") + 1]
    os.makedirs(out_dir, exist_ok=True)

    stamp = time.strftime("%Y-%m-%d %H:%M:%S")
    root = os.path.join(out_dir, "aistudio_train")
    if os.path.isdir(root):
        shutil.rmtree(root)
    os.makedirs(os.path.join(root, "ckpt"))
    os.makedirs(os.path.join(root, "corpus"))

    # ---- 脚本 ----
    for fn in SCRIPTS:
        src = os.path.join(HERE, fn)
        if not os.path.exists(src):
            print("FAIL —— 缺 %s" % src)
            return 1
        shutil.copyfile(src, os.path.join(root, fn))
        print("带 %-16s %8d B" % (fn, os.path.getsize(src)))

    # ---- 词表 ----
    vp = os.path.join(HERE, VOCAB)
    if not os.path.exists(vp):
        print("FAIL —— 缺 %s" % vp)
        return 1
    shutil.copyfile(vp, os.path.join(root, VOCAB))
    print("带 ckpt/bpe.pkl  %8d B" % os.path.getsize(vp))

    # ---- 语料 ----
    games = sorted(glob.glob(os.path.join(HERE, "corpus", "guide_*.txt")))
    if not games:
        print("FAIL —— corpus/guide_*.txt 一个都没有")
        return 1
    n_chars = 0
    for g in games:
        dst = os.path.join(root, "corpus", os.path.basename(g))
        shutil.copyfile(g, dst)
        n_chars += os.path.getsize(g)
    print("带 corpus/*.txt   %8d 个 / %s 字节"
          % (len(games), format(n_chars, ",")))

    # ---- 说明 ----
    with open(os.path.join(root, "README.md"), "w", encoding="utf-8") as f:
        f.write(README.format(stamp=stamp, n_games=len(games),
                              n_chars=n_chars))
    print("带 README.md")

    # ---- 自检：目录结构对不对 ----
    errs = []
    for fn in SCRIPTS:
        if not os.path.exists(os.path.join(root, fn)):
            errs.append("缺 " + fn)
    if not os.path.exists(os.path.join(root, VOCAB)):
        errs.append("缺 " + VOCAB)
    n = len(glob.glob(os.path.join(root, "corpus", "guide_*.txt")))
    if n != len(games):
        errs.append("语料数不对：拷了 %d 实际 %d" % (len(games), n))
    if errs:
        print("FAIL —— " + "; ".join(errs))
        return 1

    # ---- zip ----
    zp = os.path.join(out_dir, "aistudio_train.zip")
    if os.path.exists(zp):
        os.remove(zp)
    with zipfile.ZipFile(zp, "w", zipfile.ZIP_DEFLATED) as z:
        for base, _dirs, files in os.walk(root):
            for fn in files:
                p = os.path.join(base, fn)
                z.write(p, os.path.relpath(p, out_dir))
    print()
    print("目录: %s" % root)
    print("压缩: %s  (%.2f MB)"
          % (zp, os.path.getsize(zp) / 1048576))
    print()
    print("自检 OK —— 解压后 train_gpt.py / train_bpe.py / ckpt/bpe.pkl /")
    print("         corpus/*.txt 四样齐全，语料 %d 款。" % len(games))
    return 0


if __name__ == "__main__":
    sys.exit(main())
