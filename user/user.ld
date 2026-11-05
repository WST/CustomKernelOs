/* Раскладка программы MyOS: код с адреса 0x40000000 (начало памяти программ),
 * данные — с новой страницы, чтобы у кода и данных были разные права. */
ENTRY(_start)

PHDRS {
    text PT_LOAD FLAGS(5);      /* R-X: только чтение и выполнение */
    data PT_LOAD FLAGS(6);      /* RW-: чтение и запись */
}

SECTIONS {
    . = 0x40000000;
    .text   : { *(.text.start) *(.text*) } :text
    .rodata : { *(.rodata*) }              :text

    . = ALIGN(4096);
    .data   : { *(.data*) }                :data
    .bss    : { *(.bss*) *(COMMON) }       :data

    /DISCARD/ : { *(.comment) *(.eh_frame*) *(.note*) }
}
