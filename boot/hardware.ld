ENTRY(_start)
SECTIONS {
    . = 0x100000;
    __kernel_start = .;
    .text : { *(.text.entry) *(.text*) }
    .rodata : { *(.rodata*) }
    .data : { *(.data*) }
    .bss : { __bss_start = .; *(COMMON) *(.bss*) __bss_end = .; }
    ASSERT(__bss_end < 0x1000000, "Hardware kernel overlaps live root")
    /DISCARD/ : { *(.comment) *(.eh_frame) *(.note*) }
}
