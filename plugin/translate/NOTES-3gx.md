# 3GX 插件底层约束（真机验证过的，别凭直觉改）

> 这个文件只记"不知道就会做错、做错还查不出来"的东西。
> 全部结论都来自 2026-11-13 ~ 11-14 的真机调试，参数来自 Luma3DS 与
> CTRComposer 的**源码原文**，不是推测。

---

## 一、入口：Luma 的调用契约

Luma 的 `sysmodules/rosalina/source/plugin/pluginLoader.s` 原文：

```asm
gamePatchFunc:
    stmfd sp!, {r0-r12}          ; 存 r0-r12
    mrs   r0, cpsr
    stmfd sp!, {r0}              ; 存 CPSR
    ; 把游戏入口 0x00100000 的前两条指令原样写回，然后刷缓存
startplugin:
    push  {r0}
    ldr   r5, =0x07000100
    blx   r5                     ; ★ 调插件，期待插件返回
    add   sp, sp, #4             ; 弹参数 —— 要求 SP 已原样还原
exit:
    ldmfd sp!, {r0}
    msr   cpsr, r0
    ldmfd sp!, {r0-r12}          ; 从【当前 SP】还原寄存器
    ldr   lr, =0x00100000
    mov   pc, lr                 ; 跳去游戏真正的入口
```

**三条硬要求：**

1. **插件必须返回**。不返回 → 加载器线程回不去 → 游戏永远停在开机的
   Nintendo 3DS logo，且没有任何日志。
2. **SP 必须原样还原**。加载器是从"当前 SP"弹栈还原寄存器的。
3. `r0-r12` / `CPSR` 可以随便用（加载器自己存还原过）。

### 正确写法（`boot.s` 就是这么写的）

```asm
_start:
    stmfd sp!, {r0-r12, lr}      ; 在【加载器的栈】上存现场
    mrs   r0, cpsr
    stmfd sp!, {r0}
    ; 清 BSS
    bl    main
    ldmfd sp!, {r0}
    msr   cpsr, r0
    ldmfd sp!, {r0-r12, pc}      ; 还原 + 返回；SP 自动回到加载器原值
```

### 三个真机踩过的坑

| 错误写法 | 现象 | 真机转储特征 |
|---|---|---|
| `bl main` 后 `b .` 原地打转 | 卡在开机 3DS logo | 无转储、无日志 |
| 拿 `lr` 当返回地址 | 死循环（`bl` 会覆盖 `lr`） | `PC` 停在 `_start` 里同一条 |
| 换了自己的栈却没精确还原 | 进游戏即崩 | `data abort`，`PC=0`，`SP` 掉到 BSS 以下 |

**★ 不要"换到自己的栈再换回来"**：`main` 跑在自己的栈上，一旦用栈超过预设，
"保存区指针"本身就会被压栈冲掉，还原全错。让 `main` 跑在加载器的栈上、
只做轻量初始化，重活全部丢给工作线程。

### `main` 必须轻量

```
main()  = srvInit() + fsInit() + svcCreateThread(工作线程) + return
工作线程 = 一切（UI、网络、绘制）
```

工作线程用 **裸 `svcCreateThread`** 起（栈 ≥16KB，本项目 32KB），
线程开头必须自己补 TLS：

```c
u32 *tv = getThreadLocalStorage();     // mrc p15,0,..,c13,c0,3
tv[0] = 0x21545624;      // THREADVARS_MAGIC，__syscall_getreent 会校验
tv[2] = (u32)&reent;     // TP+8 = struct _reent*
tv[4] = 0;               // fs_magic = 0 → 用全局 fs 会话
```

少任何一项，线程里第一次 `snprintf`/`malloc`/`fopen` 就会在
`__syscall_getreent` 里 `svcBreak`，而且崩点看起来跟这些毫无关系。

---

## 二、显示：**绝不能碰 GSP**

真机实测：插件里调 `gspInit()` **永久阻塞**（不是慢）。
`gsp::Gpu` 是单会话独占服务，游戏进程已经握着，同进程再申请 → srv 无限等待。

⇒ `gfxInitDefault()` / `gfxGetFramebuffer()` / `gfxFlushBuffers()` /
`gfxSwapBuffers()` / `gspWaitForVBlank()` **在插件里全部作废**。

### 正确做法：直接读 LCD 寄存器（走物理镜像）

```
上屏基址 0x10400400，下屏基址 0x10400500
  +0x68 FBA1      帧缓冲 A1 的物理地址
  +0x6C FBA2      帧缓冲 A2 的物理地址
  +0x70 FORMAT    0=RGBA8(4B) 1=RGB8(3B) 3=RGB565(2B) 4=RGBA4444(2B)
  +0x78 SELECT    bit0 = 当前扫描输出哪个缓冲
  +0x90 STRIDE    每列字节数
```

**★★ 读寄存器也必须 `| 1<<31`。** 直接 `*(volatile u32*)0x10400578` 会
`data abort`（`FAR=10400578`）—— `0x10400000` 段是内核态 MMIO，普通进程没映射；
bit31 置 1 是"按物理地址直取"，这条路通。出处 CTRComposer `Includes/common.h`：

```c
#define PA_PTR(addr)  (void *)((u32)(addr) | 1 << 31)
#define REG32(addr)   (*(vu32 *)(PA_PTR(addr)))
#define HID_PAD       (REG32(0x10146000) ^ 0xFFF)
```

### 像素写入

```
off = x*stride + (h-1-y)*bpp;      ptr = (u8*)((fb + off) | (1u<<31));
字节序 BGR（px[0]=b, px[1]=g, px[2]=r）
```

**三件事错一件就花屏或崩机：列主序（转置）+ Y 翻转 + 物理镜像。**

### 怎么贴才不闪

- 物理镜像很慢（全屏约 18ms）→ **先在 RAM 里合成**（row-major RGB888），
  最后一次 blit。
- 游戏每帧都会重画自己的帧缓冲，浮层会被盖掉 → **只重刷可见缓冲，间隔 4ms**
  （20ms 会明显闪）。
- **只贴"这一帧真的画过的那块"**（脏矩形）。合成缓冲不备份游戏画面，
  整屏贴会把上一帧残留糊到游戏上。
- 若走"画隐藏缓冲 + 翻 SELECT"那条路，交还时必须把 SELECT 还原回原值，
  否则 LCD 一直扫描我们那帧（游戏在跑但屏冻住）。

---

## 三、输入：别碰 hidInit

- **绝不能调 `hidInit()`**：New 3DS 上会拉 `irrst`，把游戏冻住。
  也不要 `hidScanInput`/`hidKeysDown`/`hidTouchRead`。
- 按键直接读 `HID_PAD = REG32(0x10146000) ^ 0xFFF`，低 12 位是**取反**的按下状态：
  `A=0 B=1 SELECT=2 START=3 →=4 ←=5 ↑=6 ↓=7 R=8 L=9 X=10 Y=11`。
  边沿自己算：`down = now & ~prev`。
- **`ZL`/`ZR` 不在这个寄存器里**（靠 `ir:rst`，插件 exheader 未必有权限）
  → 触发键只能用 `SELECT`。
- **触摸**要 `hid:USER` 的共享内存（`HIDUSER_GetHandles` + `svcMapMemoryBlock`），
  libctru 把映射地址放在公开全局 `hidSharedMem` 里，触摸区在 `+0xA8`。
  但 `HIDUSER_GetHandles` 用的是 `hidInit` 建立的静态句柄，所以**必须自己
  发一次 IPC 拿 memhandle**，不能走 libctru 的封装。当前版本没做，见"待办"。

---

## 四、打包与体积

- `3gxtool -s <elf> <plgInfo> <out.3gx>`；**吃 ELF 本体**，不是 objcopy 的裸 bin。
- 三段必须首尾相接（`readelf -lW` 看 `VirtAddr + FileSiz` 是否等于下一段 `VirtAddr`）。
  **`.data` 为空时 `.bss` 会按 8 字节对齐**，留 4 字节空隙 → 3gxtool 报
  `Non-contiguous segments!`。链接脚本里给 `.bss` 也加 `ALIGN(4) SUBALIGN(4)`。
- `translate.plgInfo` 的 `MemorySize` 取 5MiB 够用（合成缓冲 288KB + 字库 235KB）。
- 入口必须正好在 `0x07000100`（Luma 硬编码），`.text.boot` 放最前。

## 五、构建必须可信

`Makefile` 一定要挂头文件依赖（`-MMD -MP` 或 `$(wildcard *.h)`）。
踩过一次：改了 `lcd.h` 但 `lcd.o` 没重编译，打出来的包里跑的还是旧代码，
真机现象跟"改动没生效"一模一样，白花一整趟真机测试。
**验收前先看构建输出里有没有 `CC` 那一行。**

---

## 六、待办

| 事项 | 现状 | 方案 |
|---|---|---|
| **触摸** | 停用（←/→ 选择 + A 确认代替） | 自建最小 `hid:USER` 会话：`srvGetServiceHandle("hid:USER")` → 发 IPC 拿 memhandle → `svcMapMemoryBlock(handle, 0, MEMPERM_READ, ...)`（地址必须传 0）→ 触摸区 `+0xA8`，**全程不碰 `ir:rst`** |
| **ZL 触发** | 只能用 SELECT | 需要 `ir:rst`，先确认游戏 exheader 有没有给权限 |
| **长按/短按判定** | 按帧数算（`TRIGGER_LONG_FRAMES=45`） | 主循环现在按 4ms 睡，帧率不等于 60fps，**应改成 `osGetTime()` 毫秒判定** |
| **面板重贴频率** | 每轮循环贴一次脏矩形 | 面板越大越慢；必要时改为只看可见缓冲的局部重贴 |

---

## 七、快捷键与面板显隐（2026-11-14 改）

**面板改成"按需显隐"**：平时**一个像素都不画**（不占帧缓冲、不花 4ms 重贴），
按下快捷键才出来，再按一次或按 B 收起。

组合表在 `main.c` 的 `k_trig[]`，配置里只存**下标**（`trigger=N`）：

| 下标 | 组合 | 说明 |
|---|---|---|
| 0 | `L+R+SELECT` | 默认 |
| 1 | `L+R+START` | |
| 2 | `L+R+X` | |
| 3 | `L+R+Y` | |
| 4 | `L+R+A` | |
| 5 | `L+R+B` | |
| 6 | `L+R+上` | |
| 7 | `SELECT` | 单键，最省事但最容易被游戏抢 |

- 判定：`(pad & mask) == mask` **全部按住**，再配按下沿检测（按住不放只切一次）。
- ⚠️ **绝不能用 `L + ↓ + SELECT`** —— 那是 Rosalina 菜单，会被系统先截走。
- ⚠️ 表**只能往后追加**：配置存下标，插在中间会让老配置错位。
- 老配置 `key=zl` / `key=select` 会在读取时自动迁到新下标。

### ★★ 打桩一个恒 `false` 的函数会静默裁掉整块功能

把 `touch_pressed()` 临时改成恒返回 `false` 之后，GCC 做了常量传播：
唯一调用「翻译/测试」的那段变成死代码 → `--gc-sections` 把 `net.c`
连同 **curl + mbedtls 一起裁掉**。产物从 1.28MB 掉到 **374KB**，
而源码、编译输出、结构校验**全都正常**，一点警告都没有。

**判据**：`arm-none-eabi-nm translate.elf | grep -cE "curl|net_"` 应该是几十个，
变成 0 就是被裁了。**体积突变一定要查**（这次是 1.28MB→374KB）。
**根因**：功能通路不能只有一条，触摸停用时必须有键盘通路替代。
