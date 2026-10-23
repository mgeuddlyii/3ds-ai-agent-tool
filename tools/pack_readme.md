# AIConsole 0.99 交付包 · 装前必读

> 桌面显示名：**测试 0.99**
> 详细构建时间、各文件 sha256 见同目录 `MANIFEST.txt`

---

## 🔴 最重要的一件事：包里的 CIA **会覆盖 FBI**，先别装

不是"占了位置"，是 **Title ID 撞车**：

| 项 | Title ID |
|---|---|
| 本包 `AIConsole-0.99.cia` | `000400000F800100` |
| **FBI 官方 build** | `000400000F800100` |

同一个 ID 安装 = **覆盖 / 更新**，不是新增。装完 FBI 那个格子里的内容就变成我们的程序了。

证据来自包内 CIA 的三处独立来源（用 `tools/ciainfo.py` 读）：

```
ticket @+0x1FC (大端) = 000400000F800100
tmd    @+0x1DC (大端) = 000400000F800100
NCCH   @+0x118 (小端) = 000400000F800100     ← 三处一致
version = 4096   ProductCode = CTR-P-AICN   content 1 段
```

顺带还有个底色问题：`UniqueId 0xF8001` 落在 **Evaluation（评测版）段 `0xF8000–0xFEFFF`**，
而 Application 段合法上界是 **`0xF7FFF`** —— 这 ID 当初选得就不合规。

### 本包的推荐用法

| 形态 | 会不会覆盖 FBI | 建议 |
|---|---|---|
| **`.3dsx`** | ❌ 不会（走 Homebrew Launcher，不进系统） | ✅ **先用这个测** |
| **`.cia`** | ✅ 会 | ⏸ **先别装**，等换掉 UniqueId 重新构建 |

`ciainfo.py` 对包内 CIA 的审计结论是 **BAD**（退出码 1）。这不是包坏了，
是它如实报告了 ID 撞车 —— **这个结果也被钉进了 `MANIFEST.txt`**。

---

## 包里有什么

```
AIConsole-0.99.3dsx          5,052,340 B   桌面显示「测试 0.99」
AIConsole-0.99.cia           3,744,704 B   ⚠️ 见上方警告
装前必读.md                  ← 你正在看的这份
MANIFEST.txt                 校验清单（含已知问题记录）
tools/
├── ciainfo.py               CIA 身份审计（带 --selftest）
├── smdhdump.py              SMDH 标题字节级校验（带 --selftest）
└── ftp_putkey.py            给 3DS SD 卡写 DeepSeek key（写完回读校验）
```

### 已通过的校验

| 检查 | 结果 |
|---|---|
| 两份 SMDH 标题（16 语言槽逐字节） | ✅ 全部 == `测试 0.99` 的 UTF-16LE |
| `.3dsx` 内嵌 SMDH（1 处） | ✅ 标题正确 |
| `.cia` 内嵌 SMDH（2 处） | ✅ 标题正确 |
| icon24 / icon48 | ✅ 非零字节 1143 / 4582，唯一值 135 / 147（不是空块） |
| 解包复核（每份文件重算 sha256） | ✅ 与源文件一致 |
| `verify_all_offline.py` | ✅ 8/8 |

⚠️ **未经真机/模拟器验证**：字节层面全对，但 HOME 菜单实际渲染成什么样没截图确认。

---

## 怎么装

### 3dsx（推荐先用这个）

1. 把 `AIConsole-0.99.3dsx` 拷到 SD 卡 `/3ds/` 下
2. 打开 Homebrew Launcher 就能看到「测试 0.99」

### CIA（等换 ID 之后再说）

用 FBI 装。当前 ID 会覆盖 FBI，所以顺序上**必须先修 ID、重建、再装**。

---

## 顺带记下的另外两件事

### 1. DeepSeek API key 已经写进 SD 卡了

- 位置：`SD:/3ds/aiconsole/key.txt`（`shared/credfile.c:21` 写死的约定路径）
- 来源：`E:/Gudyi/密码/deepseek api.txt` 里**标注用途为 `3ds`** 的那把
  （同文件里还有一把标 `桌面` 的，不是给 3DS 的，别混）
- 原来 SD 卡上那份 key.txt 是 383 字节的模板（全是 `#` 注释，**没有真 key**），已整份覆盖
- 配套确认过：SD 卡 `config.json` 里 `provider=deepseek` / `model=deepseek-chat`，与 key 匹配

⚠️ 一个坑：优先级是 **keystore 激活条目 > key.txt > config**。
如果程序里已经激活了别的 key 条目，改 key.txt **不生效**。

### 2. 开机弹 Homebrew —— 能关，但别删文件

这是 Luma3DS 的 **`Hbmenu autoboot`** 开着。三选一关掉：

1. 关机 → **按住 SELECT 开机** → Luma 配置菜单 → `Hbmenu autoboot` → Off → START 保存
2. Rosalina（`L + ↓ + Select`）→ Miscellaneous options → `Hbmenu autoboot` → Off
3. 改 `sd:/luma/config.ini` → `autoboot_mode=off`

🔴 **千万别删 SD 卡根目录的 `boot.3dsx`** —— 那是 hbmenu 本体，删了就进不去 HBL。
要删的是"每次自动跑"这个**行为**，不是那个文件。

---

## 下一步（还没做，等你拍板）

1. **换 `UniqueId`** —— 两份 RSF 都要改（`AIConsole.rsf` + `AIConsole_cia.rsf`），不能只改一个。
   候选 `0xA1C01` → `000400000A1C0100`（落在合法 Application 段，不在已知占用表里）。
   换之前先在 FBI / Rosalina 的 Titles 列表确认这 ID 没被别的东西占着。
2. **把 `ciainfo.py` 的身份审计接进 `build.py`** —— 让撞 ID 这种事以后在打包阶段就被拦下，
   而不是等真机装完才发现。
3. 换 ID 后清掉旧 `000400000F800100` 残留，FBI 需要重装 `.cia` 恢复。
