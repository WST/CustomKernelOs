/* Таблица дескрипторов прерываний (IDT) и контроллер прерываний 8259 PIC. */
#include "idt.h"
#include "vga.h"
#include "task.h"
#include "syscall.h"
#include "paging.h"

struct idt_entry {
    u16 base_lo;
    u16 sel;
    u8  zero;
    u8  flags;
    u16 base_hi;
} __attribute__((packed));

struct idt_ptr {
    u16 limit;
    u32 base;
} __attribute__((packed));

static struct idt_entry idt[256];
static struct idt_ptr   idtr;
static irq_handler_t    irq_handlers[16];

extern void idt_load(struct idt_ptr *p);

/* заглушки из isr.asm */
extern void isr0(void),  isr1(void),  isr2(void),  isr3(void),  isr4(void),
            isr5(void),  isr6(void),  isr7(void),  isr8(void),  isr9(void),
            isr10(void), isr11(void), isr12(void), isr13(void), isr14(void),
            isr15(void), isr16(void), isr17(void), isr18(void), isr19(void),
            isr20(void), isr21(void), isr22(void), isr23(void), isr24(void),
            isr25(void), isr26(void), isr27(void), isr28(void), isr29(void),
            isr30(void), isr31(void);
extern void irq0(void), irq1(void), irq2(void),  irq3(void),  irq4(void),
            irq5(void), irq6(void), irq7(void),  irq8(void),  irq9(void),
            irq10(void), irq11(void), irq12(void), irq13(void), irq14(void),
            irq15(void);
extern void isr129(void), isr128(void), isr130(void), isr131(void), isr255(void);

static void (*const isr_stubs[32])(void) = {
    isr0, isr1, isr2, isr3, isr4, isr5, isr6, isr7, isr8, isr9, isr10,
    isr11, isr12, isr13, isr14, isr15, isr16, isr17, isr18, isr19, isr20,
    isr21, isr22, isr23, isr24, isr25, isr26, isr27, isr28, isr29, isr30, isr31
};
static void (*const irq_stubs[16])(void) = {
    irq0, irq1, irq2, irq3, irq4, irq5, irq6, irq7,
    irq8, irq9, irq10, irq11, irq12, irq13, irq14, irq15
};

static const char *exception_names[32] = {
    "Division by zero", "Debug", "NMI", "Breakpoint", "Overflow",
    "Bound range exceeded", "Invalid opcode", "Device not available",
    "Double fault", "Coprocessor segment overrun", "Invalid TSS",
    "Segment not present", "Stack-segment fault", "General protection fault",
    "Page fault", "Reserved", "x87 FPU error", "Alignment check",
    "Machine check", "SIMD FP exception", "Virtualization", "Control protection",
    "Reserved", "Reserved", "Reserved", "Reserved", "Reserved", "Reserved",
    "Hypervisor injection", "VMM communication", "Security", "Reserved"
};

static void idt_set_gate(int n, void (*handler)(void), u8 flags);
static void idt_set(int n, void (*handler)(void)) { idt_set_gate(n, handler, 0x8E); }

static void idt_set_gate(int n, void (*handler)(void), u8 flags) {
    u32 base = (u32)handler;
    idt[n].base_lo = base & 0xFFFF;
    idt[n].base_hi = base >> 16;
    idt[n].sel     = 0x08;     /* селектор кода из GDT загрузчика */
    idt[n].zero    = 0;
    idt[n].flags   = flags;    /* 0x8E: present, DPL 0, 32-bit interrupt gate; 0xEE: DPL 3 */
}

/* Переназначаем IRQ 0..15 на векторы 32..47, чтобы не пересекаться
 * с исключениями процессора (0..31). */
static void pic_remap(void) {
    outb(0x20, 0x11); io_wait();   /* ICW1: инициализация */
    outb(0xA0, 0x11); io_wait();
    outb(0x21, 0x20); io_wait();   /* ICW2: смещение master = 32 */
    outb(0xA1, 0x28); io_wait();   /*        смещение slave  = 40 */
    outb(0x21, 0x04); io_wait();   /* ICW3: slave на IRQ2 */
    outb(0xA1, 0x02); io_wait();
    outb(0x21, 0x01); io_wait();   /* ICW4: режим 8086 */
    outb(0xA1, 0x01); io_wait();
    outb(0x21, 0x00);              /* размаскировать все IRQ */
    outb(0xA1, 0x00);
}

void idt_init(void) {
    for (int i = 0; i < 32; i++) idt_set(i, isr_stubs[i]);
    pic_remap();
    for (int i = 0; i < 16; i++) idt_set(32 + i, irq_stubs[i]);
    idt_set(0x82, isr130);
    idt_set(0x83, isr131);
    idt_set(0xff, isr255);
    idt_set(0x81, isr129);                  /* task_yield: только для ядра */
    idt_set_gate(0x80, isr128, 0xEE);       /* системные вызовы: DPL 3, программам можно */
    idtr.limit = sizeof(idt) - 1;
    idtr.base  = (u32)&idt;
    idt_load(&idtr);
}

void idt_reload(void) { idt_load(&idtr); }

void irq_install(int irq, irq_handler_t handler) { irq_handlers[irq] = handler; }

static void panic_header(void) {
    __asm__ volatile ("cli");
    vga_set_color(WHITE, RED);
}

static void panic(struct regs *r) {
    panic_header();
    kprintf("\n*** KERNEL PANIC: %s (vector %u, error 0x%x) ***\n",
            exception_names[r->int_no], r->int_no, r->err_code);
    if (r->int_no == 14) {
        /* CR2 = адрес, обращение к которому вызвало page fault */
        u32 cr2;
        __asm__ volatile ("mov %%cr2, %0" : "=r"(cr2));
        kprintf("CR2=%p: %s, %s%s\n", cr2,
                (r->err_code & 1) ? "protection violation" : "page not present",
                (r->err_code & 2) ? "write" : "read",
                cr2 < 0x1000 ? "  <- NULL pointer dereference!" : "");
    }
    kprintf("EIP=%p  CS=%x  EFLAGS=%p\n", r->eip, r->cs, r->eflags);
    kprintf("EAX=%p EBX=%p ECX=%p EDX=%p\n", r->eax, r->ebx, r->ecx, r->edx);
    kprintf("ESI=%p EDI=%p EBP=%p\n", r->esi, r->edi, r->ebp);
    kprintf("System halted.");
    for (;;) __asm__ volatile ("cli; hlt");
}

void kpanic(const char *msg) {
    panic_header();
    kprintf("\n*** KERNEL PANIC: %s ***\nSystem halted.", msg);
    for (;;) __asm__ volatile ("cli; hlt");
}

/* Вызывается из isr_common для любого прерывания */
/* Вызывается из isr_common для любого прерывания.
 * Возвращает regs, к которым надо вернуться: свои же или другой задачи. */
struct regs *isr_handler(struct regs *r) {
    if (r->int_no < 32) {
        if (r->int_no == 14 && (r->err_code & 2)) {     /* запись в общую страницу fork */
            u32 cr2;
            __asm__ volatile ("mov %%cr2, %0" : "=r"(cr2));
            if (vmm_cow_fault(cr2)) return r;
        }
        if (task_fault(r, exception_names[r->int_no])) return schedule(r);  /* упал поток */
        panic(r);                                                           /* упало ядро */
    } else if (r->int_no < 48) {
        int irq = r->int_no - 32;
        irq_nesting++;
        if (irq_handlers[irq]) irq_handlers[irq](r);
        irq_nesting--;
        /* EOI: сообщаем PIC, что прерывание обработано */
        if (irq >= 8) outb(0xA0, 0x20);
        outb(0x20, 0x20);
        if (irq == 0) { smp_tick(); return sched_tick(r); }      /* таймер: возможно, пора сменить задачу */
        return sched_maybe(r);                   /* например, клавиатура разбудила оболочку */
    } else if (r->int_no == 0x82) {
        smp_eoi();
        return sched_tick(r);
    } else if (r->int_no == 0x83) {
        smp_eoi();
        return sched_maybe(r); /* wakeup IPI does not charge a timer tick */
    } else if (r->int_no == 0xff) {
        return r; /* LAPIC spurious interrupt: no EOI */
    } else if (r->int_no == 0x81) {
        return schedule(r);                      /* task_yield() */
    } else if (r->int_no == 0x80) {
        syscall_dispatch(r);                     /* системный вызов из программы */
        return sched_maybe(r);                   /* например, процесс вызвал exit */
    }
    return r;
}
