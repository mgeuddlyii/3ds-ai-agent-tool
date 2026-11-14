# 交接文档 · 3DS 游戏内翻译插件

> 用途：新会话读这一份就能接着干，不需要翻历史对话。
> 最后更新：2026-11-14 10:35

---

## 一、这是什么

Luma3DS 的 `.3gx` 常驻插件，装在游戏进程里。按快捷键唤出浮层 → 截当前画面 →
发给 AI → 译文显示在一块可摆放的面板上。

代码在 `plugin/translate/`。产物 `translate.3gx` 约 1.30 MB。

## 二、当前状态

### ✅ 真机已验证

| 能力 | 证据 |
|---|---|
| 插件被加载、入口返回、工作线程、TLS 嫁接 | 痕迹文件走到 `B4 drawing`，游戏不崩 |
| 直接读 LCD 寄存器 + 写像素 | 下屏出现洋红色带 |
| DeepSeek 纯文本请求 | HTTP 200（Key `sk-fc9…81bc`，实际走 `deepseek-flash`） |
| 豆包上传四步（prepare→ApplyImageUpload→TOS→Commit） | 2026-11-14 复跑成功，拿到 ImageUri |

### 🚧 已实现但**未上机**

- 面板按快捷键显隐（默认 `L+R+SELECT`，设置页可换 8 种组合）
- 面板按钮键盘操作（←/→ + A），选中高亮
- Key 走 `keys.json` 仓库（复用主程序 shared/keystore）
- 「测试」按钮带 1×1 图验视觉能力 ⚠️ **这个设计已证伪，见下**
- `beta_pause` 暂停游戏（默认关，标 Beta）

### ❌ 已知没做完

1. **豆包发图**：上传通，但第 5 步模型看不到图（详见第五节）
2. **触摸**：停用中。要 `hid:USER` 共享内存，不能调 `hidInit`（N3DS 上拉 irrst 冻游戏）
3. **翻译屏多位置**：现在只有 9 宫格锚点 + 1 个拖放位置，没有位置存档位
4. **「测试」的图必须换**：1×1 的 PNG 被服务端判为无效图，会误报"模型不读图"

---

## 三、五条硬约束（违反的后果都是卡死/崩溃且无日志）

### 1. 入口必须返回，且还原现场

Luma 的 `pluginLoader.s` 是 `push {r0}; blx r5` 调进来的，返回后它要
`add sp,#4` 并从**当前 SP** 弹栈还原寄存器，最后跳去 `0x00100000`。

⇒ `boot.s` 照抄 CTRComposer：`stmfd sp!,{r0-r12,lr}` + 存 CPSR →
清 BSS → `bl main` → `ldmfd sp!,{r0-r12,pc}`。**全程不换 SP**。
`main()` 只做 TLS + `srvInit/fsInit` + 起线程 + `return`。

### 2. 绝不能碰 GSP

`gsp::Gpu` 单会话独占，游戏已握着 → 插件里 `gspInit()` **永久阻塞**。
所以 `gfxInitDefault` / `gfxGetFramebuffer` / `gfxSwapBuffers` / `gspWaitForVBlank` 全部作废。

画像素走 LCD 寄存器（上屏 `0x10400400`、下屏 `0x10400500`）：
`+0x68/+0x6C` 双缓冲物理地址、`+0x70` 格式、`+0x78` bit0 当前缓冲、`+0x90` 行宽。
像素 `off = x*stride + (h-1-y)*bpp`，指针 **`| (1u<<31)`**，字节序 **BGR**。

**★ 读寄存器也必须 `|1<<31`**，否则 data abort（`FAR=10400578`）。

### 3. 绝不调 hidInit

N3DS 上会拉 `irrst` 冻游戏。按键读 `HID_PAD = REG32(0x10146000) ^ 0xFFF`
（低 12 位是取反的按下状态）；`ZL/ZR` 不在这个寄存器里，所以触发键只能 SELECT 系列。

### 4. 工作线程必须自己补 TLS

裸 `svcCreateThread` 没有 ThreadVars，线程开头要写：
`tv[0]=0x21545624`、`tv[2]=&reent`、`tv[4]=0`（fs_magic）。栈 ≥16KB（用 32KB）。

### 5. 构建必须可信

- Makefile 要挂头文件依赖（曾因没挂，改了 `.h` 没重编，白跑一趟真机）
- `.data` 为空时 `.bss` 按 8 字节对齐会留空隙 → 3gxtool 报
  `Non-contiguous segments!` → 给 `.bss` 加 `ALIGN(4) SUBALIGN(4)`
- **体积突变必须查**：`nm translate.elf | grep -cE "curl|net_"` 应是几十，
  为 0 说明整块被 `--gc-sections` 裁了（打桩恒 false 的函数会引起）

---

## 四、Key 与模型（重要）

- **不要**在插件里自己解析 `key.txt`。已添加的 Key 住在 `sdmc:/3ds/aiconsole/keys.json`，
  由 `shared/keystore.c` 管理。优先级：`keystore 激活条目 > key.txt > config`
- 插件已链接 shared 的 9 个模块（keystore/creds/crypto/minijson/sha256/
  provider/uistr/uistr_table/locale），见 Makefile 的 `SHARED_SRC`
- `KeyEntry` 里**没有网址**，`base_url` 只在 `provider.c` 的表里；
  `provider_find()` 找不到时返回默认家而非 NULL，**必须比名字**才算校验
- 豆包的 auth 是 `"cookie"`（扫码登录），**不是 Key**；所以有 `keystore_clear_active()`

### 视觉能力判据（2026-11-14 实测修正）

- ❌ **不要按模型名白名单预判**。用户可能一把 Key 都没填过，预判等于替用户下结论
- ❌ **不要用 1×1 的小图做探测**。实测：Moonshot `kimi-k3`（`supports_image_in:true`）
  收到 1×1 返回 400 `failed to decode image`，收到 96×64 真实图返回 200
- ✅ **优先读声明**：部分厂家的 `GET /v1/models` 直接给 `supports_image_in`
- ✅ 兜底才发图试探，且必须用**真实尺寸**的图（建议 320×240，和截图同尺寸）

---

## 五、豆包发图：卡在哪

`豆包通道技术交接.md` §2.4 有完整五步；`tools/doubao_imgprobe.py` 是参考实现
（`tools/doubao_session.json` 里的 session 到 2026-11-14 仍有效）。

**实测结果：**

```
[1] prepare_upload      → service_id + AWS 临时凭证(ttl 1h)   ✅
[2] ApplyImageUpload    → StoreUri/Auth/SessionKey (AWS4 签名) ✅
[3] POST TOS            → "Success"                            ✅
[4] CommitImageUpload   → ImageUri                             ✅
[5] chat 带 attachments → http=200，但答「没有看到图呢」        ❌
```

变体对照（给探针加 `DBIMG_VARIANT=A/B/C/D/E`）：

| 变体 | 结果 |
|---|---|
| A 原样 | 200，看不到图 |
| B 去 `md5`+`size` | **400**（字段必需） |
| C 去 `option` | **400** |
| D `key`→`uri` | **400** |
| E `state`=1 | 200，仍看不到图 |

⇒ schema 是对的，**缺的是 Commit 之后的一步**。最可能是审核/解析状态轮询
（`file_review_state` / `file_parse_state` 现在是硬编码 3，真实值应从服务端查回）。

**下一步**：抓一次真实 webapp 的带图聊天请求（F12 → Network →
`samantha/chat/completion` 的请求体），对比出缺的那一步。拿到就能照着填。

AWS4 签名规格（可直接移植 C）：
- region=`cn-north-1`、service=`imagex`；GET 时 payload hash = `sha256("")`
- 签名头含 `host` / `x-amz-date` / `x-amz-security-token`
- 只需 SHA256 + HMAC-SHA256（`shared/sha256.c` + mbedtls 都有）

---

## 六、常用命令

```bash
# 构建（make 不在 PATH 里，要用 devkitPro 自带的）
E:/devkitPro/msys2/usr/bin/make.exe DEVKITPRO=E:/devkitPro DEVKITARM=E:/devkitPro/devkitARM
E:/devkitPro/msys2/usr/bin/make.exe ... stage1     # 架构探针
E:/devkitPro/msys2/usr/bin/make.exe ... stage2     # 架构+绘制探针
E:/devkitPro/msys2/usr/bin/make.exe ... diag       # 分步痕迹探针
E:/devkitPro/msys2/usr/bin/make.exe ... root       # 根目录版（不碰 gfx）

# 结构校验（发版/上机前必跑）
python tools/plugin/verify_3gx.py plugin/translate/translate.3gx

# 部署到真机（FTPD：192.168.100.109:5000，anonymous，不支持 MLSD）
curl -T plugin/translate/translate.3gx \
  "ftp://192.168.100.109:5000/luma/plugins/0004000000186700/default.3gx" --user anonymous:

# 读启动痕迹（插件唯一的日志）
curl "ftp://192.168.100.109:5000/3ds/aiconsole/plugin_boot.txt" --user anonymous:

# 豆包探针
python tools/doubao_imgprobe.py [--verbose]        # DBIMG_VARIANT=B/C/D/E 换形状
```

**目标游戏的 Title ID 是 `0004000000186700`**（不是 00187000，早期看错过一次）。

## 七、痕迹文件路标

`plugin_boot.txt` 覆盖写，最后一行 = 最后活着走到的步骤：

```
0 entry srv+fs ok → 1 thread ok, returning → 2 worker tls ok → 3 cfg_load ok
→ 4 first draw ok → 5 beta pause FAILED, disabled
```

## 八、文档索引

- `plugin/translate/NOTES-3gx.md` — 底层约束（入口/GSP/LCD/输入/构建）
- `豆包通道技术交接.md` — 豆包全链路（登录/聊天/历史/上传）
- `tools/plugin/README-3gxtool.md` — 打包工具说明
