/* Ядро грузится загрузчиком по физическому адресу 0x10000. */
ENTRY(_start)

SECTIONS
{
    . = 0x10000;

    .text : {
        *(.text.entry)      /* _start обязательно первым */
        *(.text*)
    }
    .rodata : { *(.rodata*) }
    .data   : { *(.data*) }

    .bss : {
        __bss_start = .;
        *(COMMON)
        *(.bss*)
        __bss_end = .;
    }

    ASSERT(__bss_end <= 0x9b000, "kernel data overlaps the 16 KiB boot stack: reduce static buffers")

    /DISCARD/ : { *(.comment) *(.eh_frame) *(.note*) }
}
