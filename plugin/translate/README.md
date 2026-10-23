# AI Console 翻译插件（3gx）

> 状态：**🚧 骨架可构建，加载链未真机验证；翻译逻辑未实现**
> 2026-10-18：工程能编出 `translate.3gx`（97091 B），结构校验器全过；
> **但插件里还没有翻译功能**，只画了一块面板。

---

## 这是什么

Luma3DS 的 **.3gx 插件**：在**别的游戏正在运行时**被 Rosalina 加载，
可以在游戏画面上弹出一块自己的 UI。这是"边玩游戏边查／边翻译"唯一可行的形态
——主程序（.3dsx/.cia）在游戏运行时根本不在 CPU 上。

## 怎么装

1. 把 `translate.3gx` 放到 SD 卡：
   - `sd:/luma/plugins/<TitleID>/translate.3gx` —— 只对某一个游戏生效（**优先**）
   - `sd:/luma/plugins/default.3gx` —— 对所有游戏生效
2. 开机后按 **L + 下 + SELECT** 打开 Rosalina → Plugin Loader → 设为 **Enabled**
3. 进游戏，按 **SELECT** 开关面板（骨架版的行为）

> ⚠️ TitleID 是大写 16 位十六进制（如 `0004000000030800`）。
> 不确定的话先放 `default.3gx`，最简单。

## 构建

```bash
cd plugin/translate
make DEVKITPRO='E:/devkitPro' DEVKITARM='E:/devkitPro/devkitARM'
python ../../tools/plugin/verify_3gx.py translate.3gx
```

依赖：`devkitPro`（devkitARM + libctru）+ **`3gxtool`**。
`3gxtool` 不在官方 devkitPro 包里，装法见 `tools/plugin/README-3gxtool.md`。

---

## 工程里每一个"为什么"（改之前先读）

### 链接地址必须正好 0x07000100
Luma 的 `pluginLoader.s` 里是**硬编码** `ldr r5,=0x07000100; blx r5`，
它**不读** ELF 的 `e_entry`。链接脚本把 `.` 定在 `0x07000100`，
并且 `.text.boot` 排在最前 —— 两处对上才跳得进来。

### 必须正好 3 个 PT_LOAD，且顺序固定
3gxtool 的硬性要求：`flags=5(R+X) → flags=4(R) → flags=6(RW)`，
各段首尾相接，只有最后一段允许 `memsz > filesz`（bss）。
所以：
- 用 `PHDRS` 显式绑定三段；
- **不能用 `-specs=3dsx.specs`** —— 它会再塞一个 `-T 3dsx.ld`，
  两套脚本同时生效会链出两倍的段，3gxtool 报 `Too many segments!`；
- 每个输出节加 `ALIGN(4) SUBALIGN(4)` —— 默认 8 字节对齐会在段间留 4 字节空隙，
  3gxtool 报 `Non-contiguous segments!`。

### 要自己补 crt0 提供的符号
拿掉 crt0（`-nostartfiles`）之后，这些得自己定义，否则链不过：
- `__service_ptr`、`__system_runflags`、`__apt_appid`（`main.c` 顶部）
- `__tdata_align` / `__tdata_lma` / `__tdata_lma_end` /
  `__tls_start` / `__tls_end` / `__end__`（`translate.ld` 里照着 3dsx.ld 抄）

### BSS 要自己清
加载器只拷 code/rodata/data 的**文件内容**，`.bss` 不会被清零。
不清的话全局变量初值是上一个进程的垃圾，而且不同游戏表现不同，极难查。
→ `boot.s` 的 `_start` 里先清 BSS 再调 `main`。

### 每行代码都在游戏进程里跑
- 可以直接读写游戏内存（RWX 全开），**所以任何越界写都会把游戏搞崩**。
- 别调 `hidInit()`（社区报告在 New 3DS 上会拉起 irrst 冻结游戏）。
- 插件没有"退出"，`main` 返回会跳飞 → `boot.s` 末尾死循环。

### plgInfo 的 YAML 结构是固定的
`Version` 必须是 **map**（`Major/Minor/Revision`），写 `0.1.0` 会报
`operator[] call on a scalar (key: "Major")`。

---

## 下一步（按依赖顺序，不能跳）

| # | 任务 | 为什么必须在这一步 |
|---|---|---|
| 1 | **真机确认骨架能加载并画出面板** | 加载/绘制/按键任一坏了，后面全白搭。不验就写翻译 = 不知道 bug 在哪层 |
| 2 | **中文字形渲染** | 插件没有主程序那套 2MB 字库。要么走系统共享字库（`APT:U` GetSharedFont → 解 BCFNT CMAP，简体字可能缺），要么自带点阵子集（~3500 常用字 ≈ 100–200 KB，可控） |
| 3 | **文本输入** | 3DS 系统软键盘在插件里能否调用**未验证**；退路是"从 SD 读待译文本"或"读游戏内存里选中的文本" |
| 4 | **联网 + TLS** | `socInit` 在插件里可行（ArticBaseServer 有先例，需 0x1000 对齐的 4MiB 缓冲）；但 **HTTPS 是硬骨头** —— `ssl:C` 在游戏进程里能否用**无先例**。退路：只支持自建 HTTP 网关，或干脆让**主程序**代发请求（插件写文件 → 主程序译 → 插件读结果） |
| 5 | 翻译 UI 与交互 | 最后才做 |

### 关于第 4 条的诚实判断

这是整个项目**唯一可能让方案推倒重来**的点：
- 若 `ssl:C` 在插件里可用 → 直连 AI API，方案成立。
- 若不可用 → 只能退成"插件 ↔ 本地/PC 网关"或"插件落盘 → 主程序译"，
  后者已经不是"游戏内直接翻译"的体验了。

**所以第 1 步和第 4 步的可行性，必须在写任何翻译代码之前各自单独验一次。**

---

## 已知风险

- 本工程**从未在真机跑过**。Azahar 不加载 .3gx，模拟器里看不到任何东西。
- `gfxInitDefault()` 在插件里的行为取决于游戏是否已 init 过 gsp —— 未验证。
- `socInit` 需要 0x1000 对齐缓冲；若游戏自己已 init 过 soc，插件再 init 可能失败（无源码证据）。
- 插件内存区 5MiB；10MiB 在部分游戏上可能导致地址空间不足。
