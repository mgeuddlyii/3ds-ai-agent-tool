# 3gxtool 怎么装（本机已装好，此文档供重装/换机用）

> 2026-10-18 已装到 `E:\devkitPro\tools\bin\3gxtool.exe`（1840640 B，v1.3）。

## 为什么需要它

Luma3DS 加载的是 `.3gx` 容器，不是普通 ELF。
`3gxtool` 把 `arm-none-eabi-ld` 产出的 ELF 包成 `.3gx`（v2 格式，magic `3GX$0002`）。

**它不是 devkitPro 官方组件**（`3ds-dev` 组里没有），所以要单独装。

## 装法（推荐：官方预编译包，无需宿主编译器）

```bash
cd /tmp && mkdir g3x && cd g3x
curl -sSL -o pkg.tar.zst \
  "https://thepixellizeross.gitlab.io/packages/x86_64/win/3gxtool-1.3-1-x86_64.pkg.tar.zst"
# 解压（Windows 自带的 bsdtar 支持 zstd；Git Bash 的 GNU tar 不行）
/c/Windows/System32/tar.exe -xf pkg.tar.zst
cp opt/devkitpro/tools/bin/3gxtool.exe /e/devkitPro/tools/bin/
/e/devkitPro/tools/bin/3gxtool.exe --help     # 能出版本说明就成
```

包大小 462812 B，包内 exe 1840640 B。

## 三个坑

1. **别用 2018 年 GitHub Release 那个（Nanquitas 版 v1.0）**。
   它写的是 `3GX$0001`，现行 Luma 会判 `Outdated plugin file`。
   现行版要 **ThePixellizerOSS v1.3**（写 `0002`，吃 ELF 本体）。
2. **不吃 `objcopy -O binary` 出来的裸 bin**。老教程那句是 v1 的写法。
3. 解压要 zstd。Git Bash 的 `tar` 是 GNU tar，会报不支持；
   用 `C:\Windows\System32\tar.exe`（bsdtar 3.8+，内置 libzstd）。

## 用法

```
3gxtool [OPTION...] <input.elf> <settings.plgInfo> <output.3gx>
  -s, --silent            安静模式
  -d, --discard-symbols   丢弃符号表（Luma 不读符号，可减体积）
  -e, --enclib            内嵌解包函数（默认已内嵌一个 ARM 桩）
```

`settings.plgInfo` 是 YAML，**结构固定**：

```yaml
Title: xxx
Author: xxx
Description: xxx
Version:            # ★ 必须是 map，写 "0.1.0" 会报 operator[] on a scalar
  Major: 0
  Minor: 1
  Revision: 0
Compatibility: Any  # Console / Citra / Any；不写会警告并默认 Any
MemorySize: 5MiB    # 2MiB / 5MiB / 10MiB；不写会警告并默认 5MiB
```

## 校验产物

```bash
python tools/plugin/verify_3gx.py <file.3gx>
```
