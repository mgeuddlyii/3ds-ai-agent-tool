# -*- coding: utf-8 -*-
"""把 extract_parity 需要的全部文件打成一个自包含 zip，方便丢到 AI Studio。

用法：cd <仓库根> && python tools/guia/make_parity_bundle.py

产物：E:/Gudyi/WORK5/_parity_bundle.zip
里面保持仓库原有的相对目录结构 —— 因为 extract_parity.py 内部写的是
`../../shared/nnet/extract.c` 这种相对路径，结构一乱就找不到文件。
"""
import os
import zipfile
import sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
OUT_DIR = os.path.abspath(os.path.join(ROOT, ".."))
ZIP_PATH = os.path.join(OUT_DIR, "_parity_bundle.zip")

# (仓库相对路径, 是否必需)
FILES = [
    ("shared/nnet/extract.c",          True),
    ("shared/nnet/extract.h",          True),
    ("shared/nnet/guide_kb.h",         True),
    ("tools/guia/extract_parity.py",   True),
    ("tools/guia/guide_search.py",     True),
    ("tools/guia/local_chat.py",       True),
    ("tools/guia/test_kb.py",          True),
    ("tools/guia/train_bpe.py",        True),   # guide_search.py 要 from train_bpe import BPE
    ("tools/guia/forward_parity.py",   True),   # local_chat 会 import 它（上一版漏了，用户手工补的）
    ("tools/guia/ckpt/guide_kb.bin",   True),   # 3.0MB
    ("tools/guia/ckpt/bpe.pkl",        True),   # 110KB
    ("app/data/guide_model.bin",       True),   # 1.25MB
    ("app/data/guide_vocab.txt",       False),  # 备用，有些分支会读
]

RUN_SH = r"""#!/bin/bash
# extract_parity 一键跑。先自检环境，缺什么就明说。
set -e
echo "=== 1/3 检查 gcc ==="
if ! command -v gcc >/dev/null 2>&1; then
  echo "❌ 没有 gcc。这个判据必须用宿主 gcc 把 extract.c 编成 .so。"
  echo "   AI Studio 的 Linux 环境应该自带 gcc；没有就 apt-get install gcc"
  exit 1
fi
gcc --version | head -1

echo ""
echo "=== 2/3 检查 numpy ==="
python -c "import numpy; print('numpy', numpy.__version__)" 2>/dev/null \
  || { echo "❌ 缺 numpy，请先: pip install numpy"; exit 1; }

echo ""
echo "=== 3/3 跑判据 ==="
cd "$(dirname "$0")"
python tools/guia/extract_parity.py
"""

README = """# extract_parity 判据包 — 操作步骤

## 这个包是干什么的
把 3DS 端 C 代码 `shared/nnet/extract.c`（抽取式造句）编成 .so，
和 PC 参照实现 `local_chat.py` 的输出**逐条比对**，
验证「卡上跑的 C 代码」和「PC 上验证过的逻辑」行为一致。

## 为什么必须跑
`tools/guia/c_side_sim.py` 里有一份 C 代码的 **Python 复刻**，
`verify_all_offline` 的「C 侧链路复刻 8/8」验的是**那份复刻，不是 C 本身**。
如果两边早就分叉了，接线到卡上后会静默出错。

## 怎么跑（Linux / AI Studio）

### 步骤 1 — 上传
把 `_parity_bundle.zip` 上传到 AI Studio，在 Notebook 里解压：
```python
import zipfile
zipfile.ZipFile("_parity_bundle.zip").extractall(".")
```

### 步骤 2 — 确认环境
```bash
gcc --version
pip install numpy        # 如果已装会提示 satisfied，可忽略
```

### 步骤 3 — 一键跑
```bash
bash run.sh
```

## 怎么读结果

| 输出 | 含义 |
|---|---|
| `已构建 _extract_test.so` | 编译通过 |
| 表格里 一致列 = `是` | 这条 C 与 PC 参照一致 |
| 表格里 一致列 = `★否` | **不一致** |
| 最后一行 `一致 N/M` | **M 必须等于 N** |

编译阶段可能出现 `multi-character constant` 的 warning —— **正常，不用管**
（extract.c 里中文标点被当多字符常量，只 warning 不报错）。

## 结果怎么给我
把 `run.sh` 的**完整输出**贴回来给我，尤其是：
- 最后那行 `一致 N/M`
- 所有标了 `★否` 的行

## 注意
- 别改目录结构：脚本内部用的是相对路径 `../../shared/nnet/extract.c`
- 缺包就照报错装；缺文件就 `ls -R` 看看有没有漏
"""


def main():
    missing = []
    for rel, need in FILES:
        p = os.path.join(ROOT, rel)
        if os.path.exists(p):
            continue
        if need:
            missing.append(rel)
        else:
            print("⚠️  跳过（不存在，列为可选）: %s" % rel)

    if missing:
        print("❌ 缺以下必需文件，打包中止：")
        for m in missing:
            print("   " + m)
        return 1

    n = 0
    with zipfile.ZipFile(ZIP_PATH, "w", zipfile.ZIP_DEFLATED) as z:
        for rel, need in FILES:
            p = os.path.join(ROOT, rel)
            if not os.path.exists(p):
                continue
            z.write(p, rel)
            n += 1
            print("  + %-40s %8d B" % (rel, os.path.getsize(p)))
        z.writestr("run.sh", RUN_SH)
        z.writestr("操作步骤.md", README)
        n += 2

    print("\n✅ 已打包 %d 项 -> %s (%.1f MB)" % (n, ZIP_PATH, os.path.getsize(ZIP_PATH) / 1048576.0))
    print("   上传这一个 zip 就行，目录结构已保持原样。")
    return 0


if __name__ == "__main__":
    sys.exit(main())
