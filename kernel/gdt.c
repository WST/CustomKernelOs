/* GDT ядра и TSS.
 *
 * Загрузчик дал нам минимальную GDT (только сегменты ядра). Для программ нужны
 * ещё сегменты кольца 3 и TSS. TSS в нашей ОС нужен ради одного поля — esp0:
 * когда прерывание (или системный вызов) приходит из ring 3, процессор берёт
 * из TSS адрес стека ядра, переключается на него и только там сохраняет
 * регистры программы. Свой стек программе доверять нельзя. */
#include "gdt.h"
#include "string.h"

struct gdt_entry {
    u16 limit_lo;
    u16 base_lo;
    u8  base_mid;
    u8  access;
    u8  flags_limit_hi;
    u8  base_hi;
} __attribute__((packed));

struct gdt_ptr {
    u16 limit;
    u32 base;
} __attribute__((packed));

struct tss {
    u32 prev, esp0, ss0, esp1, ss1, esp2, ss2;
    u32 cr3, eip, eflags, eax, ecx, edx, ebx, esp, ebp, esi, edi;
    u32 es, cs, ss, ds, fs, gs, ldt;
    u16 trap, iomap_base;
} __attribute__((packed));

static struct gdt_entry gdt[6];
static struct gdt_ptr   gdtr;
static struct tss       tss;

static void set_entry(int i, u32 base, u32 limit, u8 access, u8 flags) {
    gdt[i].limit_lo       = limit & 0xFFFF;
    gdt[i].base_lo        = base & 0xFFFF;
    gdt[i].base_mid       = (base >> 16) & 0xFF;
    gdt[i].access         = access;
    gdt[i].flags_limit_hi = (flags << 4) | ((limit >> 16) & 0x0F);
    gdt[i].base_hi        = base >> 24;
}

void gdt_init(void) {
    /*            base  limit    access     flags                         */
    set_entry(0, 0, 0,       0,         0);    /* обязательный нулевой     */
    set_entry(1, 0, 0xFFFFF, 0x9A,      0xC);  /* 0x08 код ядра,    DPL 0  */
    set_entry(2, 0, 0xFFFFF, 0x92,      0xC);  /* 0x10 данные ядра, DPL 0  */
    set_entry(3, 0, 0xFFFFF, 0xFA,      0xC);  /* 0x18 код программ, DPL 3 */
    set_entry(4, 0, 0xFFFFF, 0xF2,      0xC);  /* 0x20 данные программ     */

    memset(&tss, 0, sizeof(tss));
    tss.ss0 = KERNEL_DS;
    tss.iomap_base = sizeof(tss);              /* нет карты портов: in/out из ring 3 запрещены */
    set_entry(5, (u32)&tss, sizeof(tss) - 1, 0x89, 0x0);   /* 0x28 TSS (32-bit available) */

    gdtr.limit = sizeof(gdt) - 1;
    gdtr.base  = (u32)&gdt;
    __asm__ volatile (
        "lgdt %0\n\t"
        "ljmp $0x08, $1f\n"                    /* перезагрузить CS */
        "1:\n\t"
        "mov $0x10, %%ax\n\t"
        "mov %%ax, %%ds\n\t"
        "mov %%ax, %%es\n\t"
        "mov %%ax, %%fs\n\t"
        "mov %%ax, %%gs\n\t"
        "mov %%ax, %%ss\n\t"
        "mov $0x28, %%ax\n\t"
        "ltr %%ax\n\t"                         /* загрузить TSS */
        : : "m"(gdtr) : "eax", "memory");
}

void tss_set_kernel_stack(u32 esp0) { tss.esp0 = esp0; }
