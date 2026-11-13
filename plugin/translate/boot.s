@
@ boot.s —— 3GX 插件入口
@
@ ── 权威依据（2026-11-13 22:28 对照两份源码得出）──
@
@ ① Luma 的 sysmodules/rosalina/source/plugin/pluginLoader.s：
@       gamePatchFunc 钩在游戏入口 0x00100000 上，先存 r0-r12 + CPSR，
@       然后 `push {r0}; ldr r5,=0x07000100; blx r5` 调插件；
@       插件返回后 `add sp,#4`，再 `ldmfd sp!,...` 从【当前 SP】还原寄存器，
@       最后 `ldr lr,=0x00100000; mov pc,lr` 跳去游戏真正的入口。
@    ⇒ 契约：插件**必须返回**，且 **SP 必须原样还原**；
@      r0-r12 / CPSR 可以随便用（加载器自己存还原过）。
@
@ ② CTRComposer（跑通的成品）的 Sources/bootloader.s 正是按这个契约写的：
@       全程**不换 SP**，直接在加载器的栈上存/取现场。
@    本文件照抄它的结构，只把 BSS 符号换成我们自己链接脚本里的名字。
@
@ ── 为什么不再"换到自己 32KB 栈再换回来"──
@ 22:24 真机崩溃：`data abort / PC=00000000 / SP=0700C048`（SP 已掉到 BSS
@ 起点 0700F168 下面 12KB）。那套写法的失败模式是：main 跑在自己的栈上，
@ 一旦用栈超过预设，"保存区指针"本身就会被压栈冲掉，于是还原全错。
@ CTRComposer 的写法根本没有这个失败模式 —— main 就跑在加载器的栈上，
@ 只做轻量初始化，重活全丢给工作线程（线程自带栈）。
@
@ ── 历史三坑（保留备忘，别回头）──
@ 坑 1：`bl main` 之后写 `b .` 原地打转 → 焊死加载器线程 → 卡开机 logo。
@ 坑 2：`bl` 会覆盖 lr；拿 lr 当返回地址会跳回自己 → 死循环。
@ 坑 3：换了 SP 却不精确还原 → 加载器从错误 SP 弹栈 → 跳到 0。
@
@ ★ 0x07000100 是 Luma 硬编码的跳转目标，不读 ELF 的 e_entry；
@   链接脚本把 .text.boot 放在最前，两处对得上才跳得进来。
@

    .arm
    .align  4
    .section .text.boot, "ax", %progbits
    .global _start
    .type   _start, %function

_start:
    @ ---- 在【加载器的栈】上保存现场（此刻 sp 还是加载器的，安全）----
    stmfd   sp!, {r0-r12, lr}
    mrs     r0, cpsr
    stmfd   sp!, {r0}

    @ ---- 清 BSS ----
    @ Luma 只拷 code/rodata/data 的文件内容，`.bss`（memsz > filesz 那部分）
    @ 不清零，里面是上个进程的残留。不清 = 全局变量初值随机，极难查。
    ldr     r0, =__bss_start
    ldr     r1, =__bss_end
    mov     r2, #0
1:
    cmp     r0, r1
    bge     2f
    str     r2, [r0], #4
    b       1b

2:
    @ ---- 进 C ----
    @ 跑在加载器的栈上，所以 main 必须轻量：只做 TLS/服务初始化 + 起工作线程。
    bl      main

    @ ---- 原样还原现场并返回 ----
    @ `ldmfd sp!, {r0-r12, pc}` 一次弹完 r0-r12 并跳到弹出的 pc（= 存下的 lr，
    @ 也就是加载器 `blx` 的返回地址）。SP 同时回到加载器给我们的原值，
    @ 正好满足它 `add sp,#4` + `ldmfd sp!,...` 的还原契约。
    ldmfd   sp!, {r0}
    msr     cpsr, r0
    ldmfd   sp!, {r0-r12, pc}

    .size _start, . - _start
