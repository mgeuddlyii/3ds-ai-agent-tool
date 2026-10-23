@
@ boot.s —— 3GX 插件入口
@
@ ── 为什么入口要做这三件事 ──
@ 1. **清 BSS**：Luma 的加载器只把 code/rodata/data 的文件内容拷进内存，
@    `.bss` 段（memsz > filesz 的那部分）**不会被清零**，里面是上一个
@    进程残留的垃圾。不清就会出现"全局变量初值是随机数"，
@    而且这种 bug 在不同游戏里表现不一样，极难查。
@ 2. **调 main**：C 代码从这里开始。
@ 3. **原地打转**：插件没有"退出"这回事 —— 返回会让 CPU 跳到垃圾地址。
@    要停只能由用户换出/关掉插件，或写死循环。
@
@ ★ 地址 0x07000100 是 Luma 硬编码的跳转目标（pluginLoader.s 里
@   `ldr r5,=0x07000100; blx r5`），不读 ELF 的 e_entry。
@   链接脚本已经把 .text.boot 放在最前，两处对得上才跳得进来。
@

    .arm
    .section .text.boot, "ax", %progbits
    .global _start
    .type   _start, %function

_start:
    @ ---- 清 BSS ----
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
    bl      main

    @ ---- 回来就原地打转（插件没有退出）----
3:
    b       3b

    .size _start, . - _start
    .pool
