#ifndef IDT_H
#define IDT_H
#include "types.h"

/* Порядок полей совпадает с тем, что кладёт в стек isr_common (isr.asm). */
struct regs {
    u32 gs, fs, es, ds;
    u32 edi, esi, ebp, esp_dummy, ebx, edx, ecx, eax;
    u32 int_no, err_code;
    u32 eip, cs, eflags;
    u32 useresp, ss;                /* есть в кадре, только если прервали ring 3 */
};

typedef void (*irq_handler_t)(struct regs *r);

void idt_init(void);
void idt_reload(void);
void irq_install(int irq, irq_handler_t handler);
bool irq_try_install(int irq, irq_handler_t handler);
void kpanic(const char *msg) __attribute__((noreturn));

#endif
