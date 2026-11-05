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

    /DISCARD/ : { *(.comment) *(.eh_frame) *(.note*) }
}
