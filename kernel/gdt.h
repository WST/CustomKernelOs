#ifndef GDT_H
#define GDT_H
#include "types.h"

/* Селекторы сегментов. Младшие 2 бита — уровень привилегий (RPL). */
#define KERNEL_CS 0x08
#define KERNEL_DS 0x10
#define USER_CS   (0x18 | 3)
#define USER_DS   (0x20 | 3)
#define TSS_SEL   0x28

void gdt_init(void);
void tss_set_kernel_stack(u32 esp0);   /* куда переключать стек при прерывании из ring 3 */

#endif
