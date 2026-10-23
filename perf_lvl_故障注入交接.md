# perf_lvl 故障注入验收 · 技术交接

> 自包含文档。读完这一份就能接手「3DS 上跑 AI 插件的性能层」这件事，
> 不需要回头翻聊天记录。
>
> 对应代码：
> - `shared/nnet/perf_lvl.c` / `.h` —— 性能档位实现（升频 + 双核 + worker）
> - `app/source/test/perffault.c` / `.h` —— 故障注入验收（8 场景）
> - `app/source/test/autorun.c` —— `perffault <场景名>` 这个 op
> - `tools/azahar_test.py` —— T 趟宿主脚本（脚本 + 断言）
>
> 最后更新：2026-10-03。全量 **A–T 二十趟通过**。

---

## 0. 这件事要解决什么问题

把 AIConsole 做成**游戏运行中就能打开的插件**（Luma `.3gx`）。
于是核心约束不是"跑得快"，而是 **绝不把宿主游戏带崩**。

这个约束推导出三条设计原则，也是整个实现的骨架：

1. **失败静默降级**：任何一步失败只体现为"档位低一档"，
   没有一处 abort / 死循环 / 无限等待 / 往 stderr 喷东西
   （喷了也会被游戏看到，白底黑字，很难看）。
2. **带硬超时**：宁可放弃，也不能被拖死 —— 插件卡住 = 整个游戏卡住。
3. **绝不 free 一个还在跑的线程**：这是本项目最容易写出必炸代码的地方。

---

## 1. 三档性能

| 档位 | 含义 | 达成条件 |
|---|---|---|
| `L1_BASE` | 1 核 268MHz | 非 New 3DS |
| `L2_BOOST` | 1 核 804MHz | 升频成功，双核失败 |
| `L3_DUAL` | 2 核 804MHz | 两者都成功 |

**升频** = `osSetSpeedupEnable(true)`，内部走
`ptmSysmInit → PTMSYSM_ConfigureNew3DSCPU(3) → ptmSysmExit`。
value 3 = 804MHz + L2C-310。

- 它返回 **void**，拿不到 Result → `boost_rc==0` **≠** 升频成功。
  真的升频只能靠"跑起来是否变快"间接判断。
- **绝不调 `osSetSpeedupEnable(false)`**：降频影响整个进程，
  而进程可能还带着别人（链式加载），一旦降下去接下来几个 token 全是 3 倍慢。

**双核** = `threadCreate(..., core_id, ...)`，内部直接 `svcCreateThread`。

- core0 = application core，总能建。
- core1 = system core，**必须先调 `APT_SetAppCpuTimeLimit(60)`** 才有时���片。
  取 60 而非 Luma 默认的 89，理由：**插件的第一服务对象是游戏，不是我们**。
- core2 = New 3DS 独占，需 exheader kernel flag 0x2000（Luma 已给）。
- 探测顺序 core2 → core1 → 单核。
- **插件形态（3dsx/3gx）两条路同样通**：要的都是运行时授权，不依赖我们自己的 exheader。
- `.cia` 另有 RSF `CpuSpeed: 804mhz`（插件形态无效），且 RSF 里 `ptm:sysm` 也必须加。

实测（Azahar，`new3ds=1`）：`lv=3 rc=0 new3ds=1 dual=0 limit=60`，
档位名 `L3 dual(2core/804MHz)`。

---

## 2. ★★ 本环境没有"带超时的等待"（最重要的一节）

这一节的每条都是**实测 + 反汇编**得出的，不是读文档推断的。

### 2.1 tick 是 268 MHz

```
sleep 1ms   = 1,000,000 ns  ->    269,414 tick
sleep 10ms  =10,000,000 ns  ->  2,682,421 tick
sleep 100ms =100,000,000 ns -> 26,812,488 tick
```

三档收敛到 **268 tick/微秒 = 268 MHz**。
另一条独立路径交叉验证：`join(600ms)` 对死循环线程实测 160,896,234 tick
→ 268,160 tick/ms，与上面差 0.013%。

**我错了两次**：
- 第一次以为 60Hz → 差 4400 倍；
- 第二次改成 268000 并注释"268 kHz" → **还是差 1000 倍**（268 是 tick/**微秒**）。

### 2.2 只有 `svcGetSystemTick` 返回 tick，其余时间参数都是纳秒

`svc.h` 的注释写得很清楚：

```
svcSleepThread:              "@param ns The minimum number of nanoseconds"
threadJoin:                 "@param timeout_ns Timeout in nanoseconds"
svcWaitSynchronization:     "@param nanoseconds Maximum nanoseconds to wait for"
```

**两个量纲混用时编译器不报错、运行时不崩，只是超时全错** —— 最难发现的一类 bug。

### 2.3 `threadJoin` 的 timeout 根本没被传下去

`libctru.a` 的 `thread.o` 反汇编：

```asm
<threadJoin>:
   0:  cmp  r0, #0
   4:  beq  14
   8:  ldrb r1, [r0, #17]   ; ← 顺手占了 r1
   c:  cmp  r1, #0
  10:  beq  1c
  14:  mov  r0, #0
  18:  bx   lr
  1c:  ldr  r0, [r0]        ; r0 = handle
  20:  b    svcWaitSynchronization
```

`threadJoin(Thread, u64 timeout_ns)` 的 timeout 在栈上（第二个参数），
而 `svcWaitSynchronization(Handle, s64 timeout_ns)` 需要 `r1:r2=timeout`。
这段代码只重载了 `r0`，**从没往 r1:r2 写 timeout**。

**实测**：对一个**确认在跑**（`ran=1`）的死循环线程，`threadJoin(th, 1ms)`
返回 **rc=0（成功）**，耗时 4409 tick。一个永不返回的线程"成功"结束了。

### 2.4 `svcWaitSynchronization` 的 timeout 同样不可信

直接对活线程调 `svcWaitSynchronization(h, 1ms)` → 程序**卡死不返回**
（`status.txt` 空、`boot.txt` 停在 `f=74`）。

**注意这不是"svc 坏了"** —— 更可能是它**根本没在 timeout 上超时**，
而是一直等到线程真的结束。对活线程来说那就是永远。
**它不是不能用，是它的 timeout 不可信**，和 `threadJoin` 一个毛病。

### 2.5 结论：只能用 volatile 标志轮询

两条证据指向同一句话：**这一版 libctru + Azahar 上，没有任何 wait/join
能给你"等不及就撒手"**。

所以 `perf_wait_worker()` 只用两个被实测证明可靠的原语：

```c
static int perf_wait_worker(uint32_t timeout_ms)
{
    const uint64_t deadline = MS2TICKS(timeout_ms);
    const uint64_t step     = MS2TICKS(PERF_WAIT_STEP_MS);   /* 20ms */
    uint64_t waited = 0;

    while (!g_worker_done) {
        if (waited >= deadline)
            return -1;               /* 超时。**不 free** */
        svcSleepThread((s64)(PERF_WAIT_STEP_MS * 1000000ull));  /* 纳秒 */
        waited += step;
    }
    return 0;
}
```

- `svcGetSystemTick` —— 纯读寄存器，不进内核。
- `svcSleepThread` —— 纳秒，ruler A 段实测三档全部线性。
- 加一个 `volatile int`。**不依赖任何 wait/join svc 的语义**，
  也不依赖"建出来的线程一定会被调度"—— 它要是永远不来，我们只是超时，
  **而超时正是我们要的行为**。

为什么反而更安全：唯一能让我们卡死的因素变成了
「`svcGetSystemTick` 或 `svcSleepThread` 本身坏了」，而这两个是最简单、
最不可能有语义分歧的调用。

### 2.6 ★ 反过来：只想要"等到结束"时，`svcWaitSynchronization(h, U64_MAX)` 是安全的

`app/source/main.c` 里主线程等 app 线程结束就用的它，**刻意不绕开**。

判据很简单：

| 你想要什么 | 用什么 |
|---|---|
| **等到线程结束** | `svcWaitSynchronization(h, U64_MAX)` ✅ |
| **等不及就放弃** | `threadJoin` ❌ / `svcWaitSynchronization` ❌ → **volatile 轮询** ✅ |

---

## 3. ★ 为什么常数错了却测不出来

`MS2TICKS` 只出现在**比值**里：

```
deadline / step = MS2TICKS(600) / MS2TICKS(20) = 30 步 = 30 × 20ms
```

任何线性常数都会约掉。所以 join(600ms) 实测**恰好** 600.359ms 返回 -2 ——
超时行为是对的，常数只在"tick 换算成人话"的地方露馅。

> **教训：只验证相对量时，常数错了也测不出来。必须另配一条「绝对量」判据。**

`ruler` A 段（量时基）就是这条绝对量判据，也正是它把
268MHz / 268kHz 的差别钉出来的。

---

## 4. ★ 两个真 bug（都是"极难归因"那一类）

### 4.1 `perf_pick_core` 的 use-after-free

原实现每次 `perf_worker_start` 都「建探针线程 → join → free」。
而 join 的 timeout 是坏的（§2.3），**探针万一没被调度
（这正是它要探测的那种情况！）**，`threadFree(th)` 释放的就是一个
**还在跑**的线程的控制块。之后系统调度它，
trampoline（`_thread_begin`：`ldr r3,[r0,#4]` / `ldr r2,[r0,#8]`）
从**已释放内存**里读入口函数指针并 `blx` 它。

**在插件场景下这是必崩，而且崩在宿主游戏里。**

修法：
1. `perf_verify_core` **握手成功才 free**（`g_worker_ready` 由 worker 自己写）；
2. 探测只在 `perf_init` 做一次，结果缓存进 `g_verified_core`；
3. `perf_pick_core()` **只读缓存**，热路径一次探测都不做。

### 4.2 `perf_worker_join` 超时不置空句柄 → 一次卡死 = 插件永久瘫痪

原写法 `return -2;` 什么都没干，`g_worker` 一直非空。后果是链式的：

```
某次 worker 卡死 → join 返回 -2，但 g_worker 仍非空
  → 下次 start 看到 g_worker 非空 → perf_wait_worker(3000) → 又 3 秒死等
  → start 返回 0（推理失败）
  → 再下次，同样
```

也就是**一次卡死就让 AI 永久停在"每次推理白等 3 秒然后失败"**，
而游戏一切正常 —— **最难被用户归因的一种故障**，
因为"游戏没崩"反而让人以为插件没事。

修法：超时就**放弃**（置空句柄、**绝不 free**）。

> **取舍判据：按「一次性 vs 每次」判。**
> 泄漏一个 16KB 栈是**一次性**代价；卡住句柄是**每次调用**付 3 秒。

---

## 5. 故障注入验收（T 趟）

```bash
python tools/azahar_test.py --passes T --wait 200 --no-png
```

### 5.1 三条设计原则

1. **只注入「无法在真机上制造」的故障。**
   内存不足、线程卡死、重复调用都能真造，就不打桩 ——
   **桩只能证明桩自己是对的**。

   依据：`threadCreate` 失败时**返回 NULL，不 abort**（已反汇编确认：
   `memalign` 失败 → `mov r4,#0`；`svcCreateThread < 0` → `free` 后同样返回 0）。
   所以「申请 768MB 栈」就是**真**故障。

2. **判据必须是「还活着」，不是「结果正确」。**
   每个场景跑完必须能继续执行下一个 `app_note()`。
   崩了的话 `status.txt` 里后面的行就全都不在了 —— 这本身就是判据。

3. **每个场景都要能单独触发**（`perffault <名>`），崩一个不该连累别的。

### 5.2 八个场景

| 场景 | 内容 | 判据要点 |
|---|---|---|
| `ruler` | 时基标定 + **死循环线程的 join 行为** | 268MHz；join 必须返回 -2 而非谎报成功 |
| `alloc` | 768MB 栈（真故障） | `start=0`、`handle=0x0`、失败后重试 `start=1` |
| `hang` | worker 进死循环 | `join=-2`、耗时 ≈600ms、**句柄被置空** |
| `coexist` | 死循环线程还活着时正常起停 3 轮 | UAF 未发生；轮间做 `snprintf` 触发 malloc |
| `repeat` | 连续 start 两次 | 参数不串用（1111 / 2222） |
| `edges` | 空 fn / 无 worker join / join(UINT32_MAX) / 1 字节栈 | 边界输入 |
| `churn` | 8 轮起停（第 4 轮故意 128MB 失败） | `ok=7 fail=1`，无累积泄漏 |
| `idem` | `perf_init` 连调 5 次 | 幂等，不重复发 IPC |

**顺序有讲究**：`hang` 必须在 `coexist` 之前（后者依赖前者留下的活线程），
也必须在 `repeat`/`edges` 之前（它会占住一个核，放最后会给前面引入噪音）。

### 5.3 ★ 唯一能区分"谎报成功"的办法

**对一个已结束的线程测超时是测不出什么的** —— 成功是唯一合理结果。

唯一能区分「超时机制可信」和「谎报成功（然后去 free 一个活线程）」的办法，
就是拿一个**永远不会结束**的线程去试。这条写进 `ruler` D 段。

### 5.4 实测结果（2026-10-03）

```
tick = 268 MHz（A 段 268124 tick/ms 与 D 段 268160 tick/ms 交叉验证）
ruler D   join(600ms) 对死循环线程 = -2，耗时 600.384ms，句柄置空
ruler C   join(0xFFFFFFFF) = 20ms   <- 判据是完成标志，不是时间
alloc     768MB 栈 = start=0 + handle=0x0，失败后重试正常
hang      join=-2 / 600.359ms / handle 置空
coexist   死循环线程还活着时 3 轮起停全过
churn     ok=7 fail=1
idem      一致=1, lv=3, rc=0, new3ds=1, dual=0, limit=60
全量 A–T 二十趟 PASS，184 张截图，8m16s
```

---

## 6. ★★ 验收基础设施的两个坑（比性能层本身更容易浪费时间）

### 6.1 `status.txt` 的 0 字节窗口 → 被读成"程序崩了"

`app_write_status` 原来直接 `fopen("...status.txt","wb")`，
而 **`"w"` 一调用就把文件截断、mtime 也跟着变新**。
宿主脚本那个「mtime 比启动时刻新就 break」的等待逻辑，
会在内容还没落盘的那一瞬间读到 **0 字节**。

现象是「所有字段都没了」，断言报出来的是：

> status 里没有 layout_overflow 字段（护栏未生效，旧二进制？）

**一句把人和 1.5 秒的运气都误导了的话。**

我为此加过一个 `read_status_stable`（连读两次一致才算稳），但那只治了**读侧**，
**产生 0 字节窗口的写侧没治**。实测抓到的时序：

```
t=12.0s  status=0B      <- fopen("w") 刚截断
t=13.5s  status=1241B   <- 才写完（只差 1.5 秒，而等待窗口正好 12.0s）
```

**修法（写侧治根）**：先写 `status.tmp`，`fflush` + `fclose` 之后再 `rename`。
同目录内 `rename` 是原子的 → 读者只会看到**完整的旧文件**或**完整的新文件**。
实测验证（0.25s 采样）：`status.txt` 从「不存在」直接变「1241B 完整」，
**全程再无 0 字节**。

配套：
- 脚本等待窗口 12s → 30s（实测落盘约 13.5s）。
- `clear_pass_artifacts` 里清 `status.tmp`
  （否则它可能比 `status.txt` 更新，被 `newest()` 选中）。

> **教训：竞态要在产生它的那一侧修。读侧重试只是把概率压低，不是消除。**

### 6.2 缓冲区满的现象和程序崩的现象一模一样

`app_note` 在 `room<=8` 时**直接 return，不报错也不留标记**。
T 趟全部记录约 **3707 字节**，而 `g_notes` 当时只有 **2048** ——
只够前 3 个场景，后面 4 个场景的记录**凭空消失**，
脚本读不到「场景 X DONE」，判它是**崩溃（use-after-free）**。7 条断言被误判。

修法：
1. `g_notes` 2048 → **6144**（按实测字节数算，不是试出来的）。
2. 加 `g_notes_overflow` 计数并写进 `status.txt`
   （`notes_overflow=` / `notes_bytes=`），**让溢出变成可观测事实**。
3. 脚本第一条判据改成「**先确认证据完整，再看结论**」。

> **凡是用"某行不存在"当崩溃判据的，必须同时提供一个"证据是否完整"的字段。**

### 6.3 另外两个"静默失败"

- `kv()` 返回**字符串**。写 `notes_overflow == 0` 恒为假（`"0" != 0`），
  而且失败信息长得特别像真出事（"notes_overflow=0" 明明是 0）。
- 改了输出文案忘了同步 `pick()` 的子串 → `"join=-1"` 在
  `join(UINT32_MAX)=-1` 里当然找不到 → **静默失败**。

### 6.4 `pf_note` 的缓冲也要够

一行 40 个汉字就是 120 字节（UTF-8 每字 3 字节）。
曾用 192 字节导致最长的那行被截断，表现为
「记录里有 `join=-2` 但没有耗时字段」——
宿主脚本据此判"没超时"，**把一次成功的超时误判成失败**，
比没有这条记录更坏，因为它看起来像有结论。现在 256，并显式打 `[截断!]` 标记。

---

## 7. 故障注入测试的价值（为什么值得写这 470 行）

T 趟**第一次跑通就抓到 3 个致命 bug**，第二次又抓到 4 个。
其中 5 个是"写在注释里等于成立"但实际是假的：

- `threadJoin` 的 timeout 失效 → 整个「卡住就放弃」的设计是假的；
- tick 频率错 4400 倍 → 所有超时变成几毫秒；
- svc 时间单位是纳秒不是 tick → 把"死循环"写成了"忙等"；
- `perf_pick_core` 的 UAF；
- `perf_worker_join` 超时不置空 → 永久降级。

**关键判断：这些都是「读代码读不出来」的。**
读代码只能发现你想到的那一个，而时基、svc 单位、join 语义、
缓冲区容量这些问题**只能靠实测钉死**。

`ruler` 场景存在的全部理由就是这个 ——
它量的是**这台机器的事实**，而不是我们以为的事实。

---

## 8. 还没做的

- **真机 804MHz 验证**：Azahar 上 `dual_rc=0` / `boost_rc=0` 都拿到了，
  但真机上的实际频率与 matvec 加速比还没量过。
- **`svcWaitSynchronization` 在真机上是否可信**：本环境不可信，
  真机大概率可用，但**没测过就不该赌**（现在也不用它赌了）。
- **探针只探 core2 → core1**：没试过"探测时建不出线程、后来内存腾出来再探"的恢复路径
  （现在 `g_verified_core` 只在 `perf_init` 写一次）。
- **泄漏线程的回收**：被放弃的线程永不回收。当前每次泄漏 16KB，
  长期反复触发会累积 —— 需要一个"安全回收"的时机（目前没有）。

---

## 9. 快速上手

```bash
# 只跑故障注入（20 秒）
python tools/azahar_test.py --passes T --wait 200 --no-png

# 全量回归（8 分钟）
python tools/azahar_test.py --wait 200 --no-png

# 只看某场景（崩一个不该连累别的）
#   autorun 脚本里写：perffault hang

# 构建
python tools/build.py cia          # 零告警是硬要求
```

排查线索（SD 卡 `sdmc:/3ds/aiconsole/`）：

| 文件 | 用途 |
|---|---|
| `status.txt` | 程序自述（含 `notes_overflow` 判断证据是否完整） |
| `boot.txt` | 最后到达的屏与帧号 —— **崩了先看这个** |
| `boot.log` | 完整启动轨迹（含每次 autorun 的步数） |
| `autorun` | 上一趟跑的脚本 |

**注意**：验收用的 sdmc 是 `E:\Azahar\...\user\sdmc`，
**不是** `E:\Gudyi\_tools\azahar\...`（那里只有旧的 tls12.txt）。
