# 真机验证：让 AI 装程序（CIA 路线）

> 这份文档解决的问题：**CIA 安装能力无法在模拟器里验证。**
>
> Azahar 的 am 服务不检查调用者的 exheader，`AM_StartCiaInstall` 对谁都
> 返回成功 —— 那是假阳性，跟 httpc 那条"模拟器里 HTTPS 必然成功"同类。
> 而自动验收（`tools/azahar_test.py`）全程跑 3dsx 形态，**结构上就够不到
> 这条路**。所以这里的每一步都得在真机上人工做一遍。
>
> ⚠️ 本文档写于 2026-10-03。**代码侧已全部就绪并通过 3dsx 侧全量回归，
> 但 CIA 这条路本身没有任何一步是作者亲手在真机上验过的** —— 依据是 Luma3DS
> 与 libctru 的源码，以及大量二手资料。你验完之后，把实际结果回填到本文末尾的
> 「实测记录」。**在你回填之前，本文里所有"有 am:u""能装"的说法都应视为推论，不是实测。**

---

## 0. 先搞清一件事：为什么 3dsx 版装不了 CIA

这不是我们的代码写得不对，而是 Luma3DS 的设计。

Luma 加载一个程序时，判据只有一个（`sysmodules/loader/source/loader.c`）：

```c
if (hbldrIs3dsxTitle(exheaderInfo->aci.local_caps.title_id))
    hbldrPatchExHeaderInfo(exheaderInfo);   // ← 3dsx 走这条
else
    { ...用这个 title 自己的 exheader... }   // ← 我们的 .cia 走这条
```

而 `hbldrIs3dsxTitle`（`hbldr.h`）比的是一个**可配置的**值：

```c
static inline bool hbldrIs3dsxTitle(u64 tid)
{
    if (!Luma_SharedConfig->use_hbldr) return false;
    u64 hbldrTid = Luma_SharedConfig->hbldr_3dsx_tid;   // 默认 = hblauncher
    return ((tid ^ hbldrTid) & ~0xF0000000ull) == 0;
}
```

走上面那条分支时（`hbldr.c`），Luma 会把服务表**先清零再覆盖**成一张
写死的 34 项表：

```c
memset(localcaps0->service_access, 0, sizeof(localcaps0->service_access));
memcpy(localcaps0->service_access, serviceList, 8 * numServices);
```

那张表里有 `am:net`，**没有 `am:u`**。所以：

| 形态 | 服务表来源 | 有 am:u 吗 |
|---|---|---|
| `.3dsx` | Luma 硬编码的 34 项表 | **没有**（RSF 里写什么都没用） |
| 我们的 `.cia` | **它自己 exheader 里的声明**（= `AIConsole_cia.rsf`） | **有** |

**FBI 能装 CIA 不是什么魔法 exheader，它只是同样走了"用自己 exheader"这条路。**

---

## 1. 构建

```
python tools/build.py cia
```

产物：

| 文件 | 大小 | 用途 |
|---|---|---|
| `app/AIConsole.3dsx` | ~1.15 MB | 开发与自动验收（**装不了 CIA**） |
| `app/AIConsole.cia` | ~0.68 MB | **装到机器上，用它来装别的 CIA** |

已验证：`AIConsole.cia` 的 exheader 里 `ServiceAccessControl` 读出来是

```
cfg:u  fs:USER  gsp::Gpu  hid:USER  ndm:u
soc:U  ssl:C    http:C    APT:U     ptm:u
am:u   ← 第 11 个，也是最后一个非空的
```

（用 `python tools/verify_cia_services.py` 可复现这个检查。）

---

## 2. 把程序装到真机

1. 卡插到电脑上（或把 `AIConsole.cia` 拷到 `sdmc:/`）。
2. 用机器上已有的 **FBI** 装 `AIConsole.cia`：
   - FBI → `<SD>` → 找到 `AIConsole.cia` → Install
   - ⚠️ 如果 FBI 报 **"This CIA is invalid"**，多半是签名问题：
     我们用的是 `-target t`（零售目标）+ 自签，**Luma 已经移除签名检查**，
     正常情况下能装。若真报错，把错误码抄下来回填本文档。
3. 回到主菜单，应该能看到 **"3DS AI Assistant"** 的图标。

---

## 3. 验证 am:u 到底有没有（**这一步最关键**）

进程序 → 主界面 → 按 **B** 进 M1 验收页 → 触发 amprobe。

或者更直接：把探针趟跑一遍。探针会往
`sdmc:/3ds/aiconsole/status.txt` 里写结论，README 里有 `boot.log`。

**要看的输出：**

```
amprobe: amInit rc=0（成功）              ← 拿到了 am 服务句柄
amprobe: AM_GetTitleCount(SD) rc=0 ...    ← 只读接口通
amprobe: AM_StartCiaInstall rc=0 ...      ← 安装接口也通
amprobe: title_id=0004000F........        ← 形态判定
amprobe: 形态=自制 title（很可能是 CIA 形态，这个码可用于判断权限）
```

**判据**：
- `title_id` 的高位是 `0004000F` → 这是 `.cia` 形态 ✅ 权限判断有效
- `amInit rc=0` **且** `AM_StartCiaInstall rc=0` → **权限是有的**
- `amInit` 失败（`0xE0E0802B` 之类）→ 权限没批下来，见下方排查

### 排查：装了 CIA 还是拿不到 am:u

按可能性排序：

1. **装上去的其实是 3dsx** —— 检查主菜单图标是不是 HBL 里那种。
2. **ID 撞了**：我们的 `UniqueId: 0xF8001` 如果恰好等于
   `hbldr_3dsx_tid`（有人把收件 title 改成了它），就会被当成 3dsx。
   看一下 Rosalina 菜单里 **Miscellaneous options → 当前 hb. title**。
   撞了就把 RSF 里的 `UniqueId` 换一个（比如 `0xF8012`）再重打包。
3. **Luma 版本/配置**：确认是 Luma3DS 且开了 `use_hbldr`。
4. **FBI 装的时候降权了** —— 极少见，但可以对比：用 FBI 装的别的
   homebrew（比如某个也声明 `am:u` 的）能不能装 CIA。能 → 我们的 RSF 有问题。

---

## 4. 真的装一个 CIA 试试

准备一个**小的、确定的** CIA 放进去：

```
sdmc:/3ds/aiconsole/pkgs/<名字>.cia
```

建议先用自己打的包 —— **不要一上来就装真游戏**，那东西几百 MB，
出问题排查代价大得多。

**这个包已经给你准备好了**（在虚拟 SD 上，真机只需照抄）：

```
sdmc:/3ds/aiconsole/pkgs/selftest.cia
大小    697,280 B
sha256  e84b20fb6ba22e65...（完整值见 app/AIConsole.cia 的 sha256，两者逐字节相同）
```

> 它就是 `app/AIConsole.cia` 的一份改名拷贝 —— 用自己装自己，风险最低、
> 出问题也最容易解释。想核对是不是同一份：
> ```
> python tools/stage_cia.py app/AIConsole.cia --name selftest
> ```
> 真机上是把 `app/AIConsole.cia` 拷进卡里、改名 `selftest.cia`，没有别的玄机。

然后：

1. 程序主界面 → **A** 进对话 → 用演示 provider（不配 Key，`base_url` 空）
2. 发一条带安装触发词的短语，比如「装个游戏」
3. 等到确认屏，核对四行：
   - 要装的包 = `selftest.cia`
   - 包的大小 = 和你丢进去的文件一致
   - **会装到 = 主菜单（会出现在首页）** ← 这一行是 `.cia` 和 `.3dsx` 的分界
   - 校验摘要 = 给的摘要（或"模型没给，装前不核对"）
4. 右方向键选「允许」→ **A**

**成功的表现**：状态行说「已装好 N 字节的包。回到主菜单就能看到它了」，
回主菜单能看到那个新图标。

**失败时的表现**（按 msg 区分）：

| msg 里的话 | 含义 | 下一步 |
|---|---|---|
| "拿不到 am 服务" | 这个形态没有 am:u | 回第 3 步 |
| "系统拒绝了安装请求" | 有 am:u 但 am 拒了 | 抄下 rc 回填；可能是包格式问题 |
| "摘要对不上" | 包的 sha256 与给的不符 | **正常工作**，这是护栏在拦 |
| "写到第 N 字节时失败" | 写一半断了 | 看 N 与包大小的比例：卡满 / 卡劣质 |

### ⚠️ 失败之后必须检查残留

如果程序打印了「撤回也失败了」，那个 title 会以**装到一半的状态**留在
`import.db` 里，占着主菜单位置、点开报错。用 FBI 的
**Pending Titles / 未完成标题** 列表删掉它。

---

## 5. 自动验收在这里失效，为什么

`tools/azahar_test.py` 的 A–S 十九趟全靠往虚拟 SD 放 `autorun.txt`
脚本、注入按键回放。**这套机制对 CIA 形态无效**：装到主菜单之后，
程序是从主菜单启动的，没有能注入按键的入口。

（脚本确实有个 `--keep` 参数——不杀模拟器、自己看完再关。但它解决的是
"想多看一眼截图"，**解决不了**"装到真主菜单之后怎么自动驱动"。
两者别混为一谈。）

所以：

- **3dsx 形态继续用全自动验收**（开发主战场，一天几十轮）
- **CIA 形态只能手动验**（第 3、4 步）

这不是缺陷，是两条路各自的性质决定的。要保留自动化的速度，
就必须保留 3dsx 形态 —— 所以 RSF 是两份、产物是两个，**不是偷懒**。

---

## 附：把包准备好（可选，省几步手工）

`tools/stage_cia.py` 会把 `.cia` 拷进虚拟 SD 的 pkgs/ 并算出 sha256：

```
python tools/stage_cia.py app/AIConsole.cia --name selftest
```

输出会给你：
- 目标路径 `sdmc:/3ds/aiconsole/pkgs/selftest.cia`
- 真实的 sha256（想测"摘要护栏"就故意填错一个字符）

真机上这一步就是**把文件拷进卡里**，没有别的玄机。

---

## 实测记录（待用户回填）

> 下面这几行是留给你的。填了它，这份文档就从"读源码推的"变成"验过的"。

```
日期：2026-__-__
机器：____（型号）  Luma 版本：____
安装 AIConsole.cia：成功 / 失败（错误码 ____）

第 3 步 amprobe 输出：
  amInit rc =
  AM_StartCiaInstall rc =
  title_id =

第 4 步装 selftest.cia：
  结果 =
  若失败，程序给的 msg =

备注：
```
