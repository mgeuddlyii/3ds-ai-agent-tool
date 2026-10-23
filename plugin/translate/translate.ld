/*
 * translate.ld —— 3GX 插件的链接脚本
 *
 * ── 为什么不能照抄 3dsx 的链接脚本 ──
 * Luma 的插件加载器把插件**装到固定地址**：`0x07000000` 是
 * `PluginHeader`（正好 0x100 字节），代码从 **`0x07000100`** 开始。
 * 而且 `pluginLoader.s` 里是**硬编码** `ldr r5,=0x07000100; blx r5`，
 * 它不看 ELF 的 e_entry —— 所以 `.` 必须正好是 0x07000100，
 * 差一个字节就跳飞。
 *
 * ── 为什么正好三个 PT_LOAD、顺序也不能换 ──
 * 3gxtool 的 ElfConvert 硬性要求（不满足直接报错退出）：
 *   PT_LOAD ≤ 3，按顺序必须是 flags=5(R+X) → flags=4(R) → flags=6(RW)
 *   各段 vaddr 连续；只有 flags=6 的段允许 memsz > filesz（即只有 data 能带 bss）
 * 所以这里用 PHDRS 显式把 .text/.rodata/.data(+bss) 绑到三个段上。
 * ⚠️ 这也意味着**不能用 `-specs=3dsx.specs`** —— 那套 specs 会再塞一个
 *    `-T 3dsx.ld`，两套脚本同时生效会链接出两倍的 PT_LOAD，
 *    3gxtool 报 "Too many segments!"。
 *
 * ── 为什么要抄 3dsx.ld 的 TLS 那段 ──
 * libctru 的 `threadCreate` / `initThreadVars` 引用
 * `__tdata_align` / `__tdata_lma` / `__tdata_lma_end` /
 * `__tls_start` / `__tls_end`，`_sbrk_r` 引用 `__end__`。
 * 这些平时由 3dsx.ld 提供；我们自己写脚本就得自己给，
 * 否则链接期一堆 undefined reference（而且报错全指向 libctru，
 * 看起来像"库缺东西"，真因在链接脚本）。
 */

ENTRY(_start)

PHDRS
{
    code   PT_LOAD FLAGS(5);   /* R + X */
    rodata PT_LOAD FLAGS(4);   /* R     */
    data   PT_LOAD FLAGS(6);   /* R + W */
}

SECTIONS
{
    /* ★ 必须正好是 0x07000100，理由见文件头 */
    . = 0x07000100;

    .text : ALIGN(4) SUBALIGN(4) {
        KEEP(*(.text.boot))    /* _start 必须在最前面：入口是硬编码地址 */
        *(.text*)
        *(.glue_7) *(.glue_7t) *(.vfp11_veneer) *(.v4_bx)
    } : code

    .rodata : ALIGN(4) SUBALIGN(4) {
        *(.rodata*)
        /* 官方脚本把 .tdata 的对齐值以一个字面量存在 .rodata 末尾，
         * libctru 的 initThreadVars 按这个值对齐 TLS 区。 */
        . = ALIGN(4);
        __tdata_align = .;
        LONG (ALIGNOF(.tdata));
        . = ALIGN(4);
    } : rodata

    .ARM.extab : { *(.ARM.extab* .gnu.linkonce.armextab.*) } : rodata
    __exidx_start = .;
    .ARM.exidx : { *(.ARM.exidx* .gnu.linkonce.armexidx.*) } : rodata
    __exidx_end = .;

    .data : ALIGN(4) SUBALIGN(4) {
        *(.data*)
        *(.gnu.linkonce.d*)
        . = ALIGN(4);
    } : data

    .tdata : ALIGN(4) {
        __tdata_lma = .;
        *(.tdata)
        *(.tdata.*)
        *(.gnu.linkonce.td.*)
        . = ALIGN(4);
        __tdata_lma_end = .;
    } : data

    .tbss : ALIGN(4) {
        *(.tbss)
        *(.tbss.*)
        *(.gnu.linkonce.tb.*)
        *(.tcommon)
        . = ALIGN(4);
    } : data

    /* NOLOAD：在 data 段里占地址，但不进文件（memsz > filesz 就是 bss） */
    __bss_start = .;
    .bss (NOLOAD) : {
        *(.dynbss)
        *(.bss*)
        *(.gnu.linkonce.b*)
        *(COMMON)
        . = ALIGN(4);

        /* 给主线程预留 TLS 区（8 字节是 ARM TLS 头，不用但要占位对齐）。
         * 不预留的话，第一个 TLS 变量就会踩进堆里。 */
        . = 8 + ABSOLUTE(ALIGN(ABSOLUTE(. - 8),
                              MAX(ALIGNOF(.tdata), ALIGNOF(.tbss))));
        __tls_start = .;
        . += SIZEOF(.tdata);
        . = ALIGN(ALIGNOF(.tbss));
        . += SIZEOF(.tbss);
        __tls_end = .;
    } : data
    __bss_end = .;

    __end__ = ABSOLUTE(.);

    /DISCARD/ : {
        *(.comment)
        *(.note*)
        *(.ARM.attributes)
        *(.eh_frame*)
        *(.preinit_array) *(.init_array) *(.fini_array)
    }
}
